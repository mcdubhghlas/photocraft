/* Polygon stroking (port of crates/vector/src/stroke.rs): a stroke is the union of one quad per
 * segment, a wedge per join and a cap shape per open end. Every piece is emitted with positive
 * orientation, so filling them together with the non-zero rule yields their exact union without
 * computing offset curves. Dashing splits the flattened polylines by arc length first. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "vector/internal.h"

/* The reference's step budget for dashing: past it (tiny dash lengths, file-provided patterns)
 * the polyline gets no dashes at all. Commands reject patterns that exceed it beforehand. */
#define DASH_GUARD 100000u
/* Longest dash pattern dash_polyline accepts. */
#define DASH_MAX_PATTERN 32u

typedef struct P {
    double x, y;
} P;

static P mk(double x, double y)
{
    P p;
    p.x = x;
    p.y = y;
    return p;
}
static P sub(P a, P b)
{
    return mk(a.x - b.x, a.y - b.y);
}
static P add(P a, P b)
{
    return mk(a.x + b.x, a.y + b.y);
}
static P mul(P a, double k)
{
    return mk(a.x * k, a.y * k);
}
static double len(P a)
{
    return hypot(a.x, a.y);
}
static P norm(P a)
{
    double l = len(a);
    return l > 0.0 ? mk(a.x / l, a.y / l) : mk(0.0, 0.0);
}
/* Left normal in y-down coordinates. */
static P perp(P a)
{
    return mk(a.y, -a.x);
}
static double cross(P a, P b)
{
    return a.x * b.y - a.y * b.x;
}
static double dot(P a, P b)
{
    return a.x * b.x + a.y * b.y;
}

void pc_vec_stroke_style_default(pc_vec_stroke_style *out)
{
    memset(out, 0, sizeof *out);
    out->width = 1.0;
    out->cap = PC_LINE_CAP_BUTT;
    out->join = PC_LINE_JOIN_MITER;
    out->miter_limit = 4.0;
}

void pc_vec_stroke_style_fini(pc_vec_stroke_style *s)
{
    if (s == NULL) {
        return;
    }
    free(s->dashes);
    pc_vec_stroke_style_default(s);
}

pc_status pc_vec_stroke_style_from(const pc_shape_stroke *s, pc_vec_stroke_style *out, pc_error *err)
{
    pc_vec_stroke_style st;
    double w = (double)pc_rs_maxf_k(s->width, 0.0f);
    size_t i;
    pc_vec_stroke_style_default(&st);
    st.width = w;
    st.cap = s->cap;
    st.join = s->join;
    st.miter_limit = (double)pc_rs_maxf_k(s->miter_limit, 1.0f);
    if (s->ndashes > 0) {
        st.dashes = pc_malloc_array(s->ndashes, sizeof *st.dashes);
        if (st.dashes == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
        }
        for (i = 0; i < s->ndashes; i++) {
            st.dashes[i] = (double)s->dashes[i] * w;
        }
        st.ndashes = s->ndashes;
    }
    st.dash_offset = (double)s->dash_offset * w;
    *out = st;
    return PC_OK;
}

/* ---- piece emission ---- */

typedef struct sink {
    pc_vec_polys *out;
    size_t points; /* running total, capped at PC_VEC_MAX_POINTS */
} sink;

static double signed_area(const P *p, size_t n)
{
    /* Rust's Iterator::sum starts at -0.0. */
    double s = -0.0;
    size_t i;
    for (i = 0; i < n; i++) {
        s += cross(p[i], p[(i + 1) % n]);
    }
    return s * 0.5;
}

/* Pushes poly (n points, copied) with positive orientation; drops degenerate pieces. */
static pc_status emit(sink *sk, const P *poly, size_t n, pc_error *err)
{
    double a = signed_area(poly, n);
    double (*pts)[2];
    size_t i;
    if (fabs(a) < 1e-12) {
        return PC_OK;
    }
    if (n > PC_VEC_MAX_POINTS - sk->points) {
        return pc_fail(err, PC_ERR_LIMIT, "vector: stroke has more than %zu points",
                       (size_t)PC_VEC_MAX_POINTS);
    }
    pts = pc_malloc_array(n, sizeof *pts);
    if (pts == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
    }
    for (i = 0; i < n; i++) {
        const P *q = a < 0.0 ? &poly[n - 1 - i] : &poly[i];
        pts[i][0] = q->x;
        pts[i][1] = q->y;
    }
    sk->points += n;
    return pc_vec_polys_push(sk->out, pts, n, err);
}

/* Angular step so that arc chords deviate at most tol from a circle of radius r. */
static double arc_step(double r, double tol)
{
    if (r <= tol) {
        return PC_VEC_FRAC_PI_2;
    }
    return pc_clampd(2.0 * acos(pc_clampd(1.0 - tol / r, -1.0, 1.0)), 0.01, PC_VEC_FRAC_PI_2);
}

/* Vertex radius for chords of `step` radians so each chord's pie slice has the area of the
 * circular sector (keeps round caps/joins area-exact at any tessellation). */
static double area_radius(double r, double step)
{
    return step < 1e-9 ? r : r * sqrt(step / sin(step));
}

/* arc_step >= 0.01 (or NaN, which casts to 0), so a circle has at most 629 vertices. */
#define MAX_ARC 700u

static pc_status circle(sink *sk, P c, double r, double tol, pc_error *err)
{
    P poly[MAX_ARC];
    size_t n = pc_f64_to_size(ceil(PC_VEC_TAU / arc_step(r, tol)));
    double rr;
    size_t i;
    if (n < 8) {
        n = 8;
    }
    if (n > MAX_ARC) {
        n = MAX_ARC; /* unreachable: arc_step is clamped to >= 0.01 */
    }
    rr = area_radius(r, PC_VEC_TAU / (double)n);
    for (i = 0; i < n; i++) {
        double a = PC_VEC_TAU * (double)i / (double)n;
        poly[i] = mk(c.x + rr * cos(a), c.y + rr * sin(a));
    }
    return emit(sk, poly, n, err);
}

static pc_status join_piece(sink *sk, P v, P d1, P d2, double hw, pc_line_join join, double miter_limit,
                            double tol, pc_error *err)
{
    double c = cross(d1, d2);
    double d = dot(d1, d2);
    double s;
    P n1;
    P n2;
    P a;
    P b;
    if (fabs(c) < 1e-12 && d > 0.0) {
        return PC_OK;
    }
    /* Outer side: a right turn (c > 0 in y-down) opens the left side. */
    s = c > 0.0 ? 1.0 : -1.0;
    n1 = mul(perp(d1), hw * s);
    n2 = mul(perp(d2), hw * s);
    a = add(v, n1);
    b = add(v, n2);
    if (join == PC_LINE_JOIN_BEVEL) {
        P tri[3];
        tri[0] = v;
        tri[1] = a;
        tri[2] = b;
        return emit(sk, tri, 3, err);
    }
    if (join == PC_LINE_JOIN_MITER) {
        /* Miter length / width = 1 / cos(theta / 2), theta the turning angle. */
        double cos_half = sqrt(pc_rs_max_k((1.0 + d) * 0.5, 0.0));
        if (cos_half < 1e-9 || 1.0 / cos_half > miter_limit) {
            P tri[3];
            tri[0] = v;
            tri[1] = a;
            tri[2] = b;
            return emit(sk, tri, 3, err);
        } else {
            P quad[4];
            P bis = norm(add(n1, n2));
            quad[0] = v;
            quad[1] = a;
            quad[2] = add(v, mul(bis, hw / cos_half));
            quad[3] = b;
            return emit(sk, quad, 4, err);
        }
    }
    {
        /* Round: sweep the short way from n1 to n2 on the outer side. */
        P poly[MAX_ARC + 3];
        double a0 = atan2(n1.y, n1.x);
        double a1 = atan2(n2.y, n2.x);
        double delta = a1 - a0;
        size_t steps;
        size_t np = 0;
        size_t k;
        double rr;
        while (delta > PC_VEC_PI) {
            delta -= PC_VEC_TAU;
        }
        while (delta < -PC_VEC_PI) {
            delta += PC_VEC_TAU;
        }
        if (fabs(c) < 1e-12) {
            /* U-turn: half circle on the side of n1 rotating towards d1. */
            delta = PC_VEC_PI * (cross(n1, d1) > 0.0 ? 1.0 : -1.0);
        }
        a1 = a0 + delta;
        steps = pc_f64_to_size(ceil(fabs(delta) / arc_step(hw, tol)));
        if (steps < 1) {
            steps = 1;
        }
        if (steps > MAX_ARC) {
            steps = MAX_ARC; /* unreachable: |delta| <= pi and arc_step >= 0.01 */
        }
        rr = area_radius(hw, fabs(delta) / (double)steps);
        poly[np++] = v;
        poly[np++] = a;
        for (k = 1; k < steps; k++) {
            double t = a0 + (a1 - a0) * (double)k / (double)steps;
            poly[np++] = mk(v.x + rr * cos(t), v.y + rr * sin(t));
        }
        poly[np++] = b;
        return emit(sk, poly, np, err);
    }
}

static pc_status cap_piece(sink *sk, P p, P dir, double hw, pc_line_cap cap, double tol, pc_error *err)
{
    if (cap == PC_LINE_CAP_ROUND) {
        return circle(sk, p, hw, tol, err);
    }
    if (cap == PC_LINE_CAP_SQUARE) {
        P q[4];
        P n = mul(perp(dir), hw);
        P e = mul(dir, hw);
        q[0] = add(p, n);
        q[1] = add(add(p, n), e);
        q[2] = add(sub(p, n), e);
        q[3] = sub(p, n);
        return emit(sk, q, 4, err);
    }
    return PC_OK;
}

/* ---- one polyline ---- */

typedef struct ribbon {
    P *v;
    size_t len;
    size_t cap;
} ribbon;

static pc_status ribbon_push(ribbon *r, P p, pc_error *err)
{
    if (r->len == r->cap) {
        size_t cap = r->cap;
        void *nv = pc_grow_array(r->v, &cap, r->len + 1, sizeof *r->v);
        if (nv == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
        }
        r->v = nv;
        r->cap = cap;
    }
    r->v[r->len++] = p;
    return PC_OK;
}

typedef struct line_ctx {
    const P *pts;
    const bool *knot;
    size_t n;
    bool closed;
    double hw;
    double cos_smooth;
} line_ctx;

static P dir_at(const line_ctx *L, size_t i)
{
    return norm(sub(L->pts[(i + 1) % L->n], L->pts[i]));
}
static double seg_len(const line_ctx *L, size_t i)
{
    return len(sub(L->pts[(i + 1) % L->n], L->pts[i]));
}

/* A vertex continues a ribbon (no separate join piece) when it is a flattening point with a
 * small turn whose inner offset does not fold back, or a knot where the path is straight. */
static bool smooth_at(const line_ctx *L, size_t j)
{
    size_t i0;
    double d;
    double tan_half;
    if (j == 0 || (!L->closed && j == L->n - 1)) {
        return false;
    }
    i0 = (j + L->n - 1) % L->n;
    d = dot(dir_at(L, i0), dir_at(L, j));
    if (L->knot[j]) {
        return d > 1.0 - 1e-12;
    }
    if (d < L->cos_smooth) {
        return false;
    }
    tan_half = sqrt(pc_rs_max_k((1.0 - d) / (1.0 + d), 0.0));
    return L->hw * tan_half < 0.5 * pc_rs_min(seg_len(L, i0), seg_len(L, j));
}

static pc_status stroke_one(const pc_vec_polyline *pl, const pc_vec_stroke_style *style, double tol, sink *sk,
                            pc_error *err)
{
    double hw = style->width * 0.5;
    P *pts = NULL;
    bool *knot = NULL;
    size_t n = 0;
    size_t i;
    bool closed = pl->closed;
    ribbon left = {NULL, 0, 0};
    ribbon right = {NULL, 0, 0};
    P *poly = NULL;
    pc_status st = PC_OK;
    line_ctx L;
    size_t segs;
    if (pl->len > 0) {
        pts = pc_malloc_array(pl->len, sizeof *pts);
        knot = pc_malloc_array(pl->len, sizeof *knot);
        if (pts == NULL || knot == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
            goto cleanup;
        }
    }
    /* Drop repeated points. */
    for (i = 0; i < pl->len; i++) {
        P p = mk(pl->pts[i][0], pl->pts[i][1]);
        if (n > 0 && len(sub(p, pts[n - 1])) < 1e-9) {
            continue;
        }
        pts[n] = p;
        knot[n] = pl->knot != NULL ? pl->knot[i] : true;
        n++;
    }
    if (closed && n > 1 && len(sub(pts[0], pts[n - 1])) < 1e-9) {
        n--;
    }
    if (n < 3) {
        closed = false;
    }
    if (n == 0) {
        goto cleanup;
    }
    if (n == 1) {
        /* A dot: only round and square caps draw something. */
        P c = pts[0];
        if (style->cap == PC_LINE_CAP_ROUND) {
            st = circle(sk, c, hw, tol, err);
        } else if (style->cap == PC_LINE_CAP_SQUARE) {
            P q[4];
            q[0] = mk(c.x - hw, c.y - hw);
            q[1] = mk(c.x + hw, c.y - hw);
            q[2] = mk(c.x + hw, c.y + hw);
            q[3] = mk(c.x - hw, c.y + hw);
            st = emit(sk, q, 4, err);
        }
        goto cleanup;
    }
    L.pts = pts;
    L.knot = knot;
    L.n = n;
    L.closed = closed;
    L.hw = hw;
    L.cos_smooth = cos(pc_vec_to_radians(25.0));
    segs = closed ? n : n - 1;
    for (i = 0; i < segs && st == PC_OK; i++) {
        P a = pts[i];
        P b = pts[(i + 1) % n];
        P nrm = mul(perp(dir_at(&L, i)), hw);
        size_t j = (i + 1) % n;
        size_t k;
        size_t np;
        if (left.len == 0) {
            st = ribbon_push(&left, add(a, nrm), err);
            if (st == PC_OK) {
                st = ribbon_push(&right, sub(a, nrm), err);
            }
            if (st != PC_OK) {
                break;
            }
        }
        if (i + 1 < segs + (closed ? 0u : 1u) && smooth_at(&L, j)) {
            /* Offset lines of both segments meet at v +- bisector * hw / cos(theta / 2). */
            P n2 = mul(perp(dir_at(&L, j)), hw);
            P bis = norm(add(nrm, n2));
            double cos_half = sqrt(pc_rs_max_k((1.0 + dot(dir_at(&L, i), dir_at(&L, j))) * 0.5, 1e-9));
            P off = mul(bis, hw / cos_half);
            st = ribbon_push(&left, add(b, off), err);
            if (st == PC_OK) {
                st = ribbon_push(&right, sub(b, off), err);
            }
            continue;
        }
        st = ribbon_push(&left, add(b, nrm), err);
        if (st == PC_OK) {
            st = ribbon_push(&right, sub(b, nrm), err);
        }
        if (st != PC_OK) {
            break;
        }
        np = left.len + right.len;
        free(poly);
        poly = pc_malloc_array(np, sizeof *poly);
        if (poly == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
            break;
        }
        memcpy(poly, left.v, left.len * sizeof *poly);
        for (k = 0; k < right.len; k++) {
            poly[left.len + k] = right.v[right.len - 1 - k];
        }
        left.len = 0;
        right.len = 0;
        st = emit(sk, poly, np, err);
        if (st != PC_OK) {
            break;
        }
        if (closed || j != n - 1) {
            P prev = pts[i];
            P v = pts[j];
            P next = pts[(j + 1) % n];
            P d1 = norm(sub(v, prev));
            P d2 = norm(sub(next, v));
            /* Flattening points use miters (the true offset of a smooth curve) unless the
             * requested join is round; bevels only at real corners. */
            pc_line_join join = style->join == PC_LINE_JOIN_ROUND ? PC_LINE_JOIN_ROUND
                                : knot[j]                         ? style->join
                                                                  : PC_LINE_JOIN_MITER;
            st = join_piece(sk, v, d1, d2, hw, join,
                            knot[j] ? pc_rs_min_k(style->miter_limit, 500.0) : INFINITY, tol, err);
        }
    }
    if (st == PC_OK && !closed) {
        P d0 = norm(sub(pts[1], pts[0]));
        P d1 = norm(sub(pts[n - 1], pts[n - 2]));
        st = cap_piece(sk, pts[0], mul(d0, -1.0), hw, style->cap, tol, err);
        if (st == PC_OK) {
            st = cap_piece(sk, pts[n - 1], d1, hw, style->cap, tol, err);
        }
    }
cleanup:
    free(pts);
    free(knot);
    free(left.v);
    free(right.v);
    free(poly);
    return st;
}

static bool polyline_finite(const pc_vec_polyline *pl)
{
    size_t i;
    for (i = 0; i < pl->len; i++) {
        if (!isfinite(pl->pts[i][0]) || !isfinite(pl->pts[i][1])) {
            return false;
        }
    }
    return true;
}

pc_status pc_vec_stroke_polygons(const pc_vec_polyline *lines, size_t n, const pc_vec_stroke_style *style,
                                 double tol, pc_vec_polys *out, pc_error *err)
{
    size_t start = out->len;
    sink sk;
    size_t i;
    size_t k;
    bool dashed = style->ndashes > 0;
    pc_status st = PC_OK;
    if (style->width <= 0.0 || !isfinite(style->width) || !isfinite(style->miter_limit) ||
        style->miter_limit < 1.0) {
        return PC_OK;
    }
    sk.out = out;
    sk.points = 0;
    for (k = 0; k < start; k++) {
        sk.points += out->v[k].len;
    }
    for (i = 0; i < n && st == PC_OK; i++) {
        if (!polyline_finite(&lines[i])) {
            continue;
        }
        if (dashed) {
            pc_vec_polyline *d = NULL;
            size_t nd = 0;
            st = pc_vec_dash_polyline(&lines[i], style->dashes, style->ndashes, style->dash_offset, &d, &nd,
                                      err);
            for (k = 0; k < nd && st == PC_OK; k++) {
                st = stroke_one(&d[k], style, tol, &sk, err);
            }
            pc_vec_polylines_free(d, nd);
        } else {
            st = stroke_one(&lines[i], style, tol, &sk, err);
        }
    }
    if (st != PC_OK) {
        pc_vec_polys_truncate(out, start);
    }
    return st;
}

/* ---- dashes ---- */

typedef struct dash_list {
    pc_vec_polyline *v;
    size_t len;
    size_t cap;
    size_t points;
} dash_list;

static pc_status dash_list_push(dash_list *l, pc_vec_polyline *p, pc_error *err)
{
    if (l->len >= PC_VEC_MAX_DASHES) {
        pc_vec_polyline_fini(p);
        return pc_fail(err, PC_ERR_LIMIT, "vector: more than %zu dashes", (size_t)PC_VEC_MAX_DASHES);
    }
    if (l->points + p->len > PC_VEC_MAX_POINTS) {
        pc_vec_polyline_fini(p);
        return pc_fail(err, PC_ERR_LIMIT, "vector: dashes have more than %zu points",
                       (size_t)PC_VEC_MAX_POINTS);
    }
    if (l->len == l->cap) {
        size_t cap = l->cap;
        void *nv = pc_grow_array(l->v, &cap, l->len + 1, sizeof *l->v);
        if (nv == NULL) {
            pc_vec_polyline_fini(p);
            return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
        }
        l->v = nv;
        l->cap = cap;
    }
    /* Shrink to fit: most dashes hold 2 points, and there may be up to PC_VEC_MAX_DASHES. */
    if (p->len > 0 && p->len < p->cap) {
        void *np = realloc(p->pts, p->len * sizeof *p->pts);
        void *nk;
        if (np != NULL) {
            /* Both arrays stay at least cap long even if the second shrink fails. */
            p->pts = np;
            p->cap = p->len;
            nk = realloc(p->knot, p->len * sizeof *p->knot);
            if (nk != NULL) {
                p->knot = nk;
            }
        }
    }
    l->points += p->len;
    l->v[l->len++] = *p;
    memset(p, 0, sizeof *p);
    return PC_OK;
}

static pc_status dash_start(pc_vec_polyline *cur, P p, pc_error *err)
{
    memset(cur, 0, sizeof *cur);
    return pc_vec_polyline_append(cur, p.x, p.y, true, err);
}

/* A dash that crosses the closing vertex of a closed polyline has a join there, not two caps:
 * stitch the last dash (ending at the seam) onto the first (starting there), keeping the knot
 * flags; a single dash running all the way round becomes closed. */
static pc_status dash_stitch_seam(const pc_vec_polyline *pl, dash_list *res, pc_error *err)
{
    double sx = pl->pts[0][0];
    double sy = pl->pts[0][1];
    pc_vec_polyline *first;
    pc_vec_polyline *tail;
    size_t k;
    if (res->len == 0) {
        return PC_OK;
    }
    first = &res->v[0];
    tail = &res->v[res->len - 1];
    if (first->len == 0 || tail->len == 0 || first->pts[0][0] != sx || first->pts[0][1] != sy ||
        tail->pts[tail->len - 1][0] != sx || tail->pts[tail->len - 1][1] != sy) {
        return PC_OK;
    }
    if (res->len == 1) {
        first->closed = true;
        return PC_OK;
    }
    for (k = 1; k < first->len; k++) {
        pc_status st = pc_vec_polyline_append(tail, first->pts[k][0], first->pts[k][1], first->knot[k], err);
        if (st != PC_OK) {
            return st;
        }
    }
    pc_vec_polyline_fini(first);
    *first = *tail;
    memset(tail, 0, sizeof *tail);
    res->len--;
    return PC_OK;
}

static bool dash_input_ok(const pc_vec_polyline *pl, const double *pattern, size_t npattern, double offset)
{
    size_t i;
    if (npattern > DASH_MAX_PATTERN || !isfinite(offset) || !polyline_finite(pl)) {
        return false;
    }
    for (i = 0; i < npattern; i++) {
        if (!isfinite(pattern[i]) || pattern[i] < 0.0) {
            return false;
        }
    }
    return true;
}

pc_status pc_vec_dash_polyline(const pc_vec_polyline *pl, const double *pattern, size_t npattern,
                               double offset, pc_vec_polyline **out, size_t *nout, pc_error *err)
{
    double pat[2 * DASH_MAX_PATTERN];
    size_t npat = npattern % 2 == 1 ? npattern * 2 : npattern;
    double total = -0.0;
    size_t i;
    P *pts = NULL;
    bool *knots = NULL;
    size_t npts;
    size_t idx = 0;
    double left;
    double skip;
    dash_list res = {NULL, 0, 0, 0};
    pc_vec_polyline cur;
    bool has_cur = false;
    bool empty = false;
    size_t guard = 0;
    pc_status st = PC_OK;
    memset(&cur, 0, sizeof cur);
    if (npattern == 0) {
        pc_vec_polyline *one = pc_malloc(sizeof *one);
        if (one == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
        }
        st = pc_vec_polyline_clone(pl, one, err);
        if (st != PC_OK) {
            free(one);
            return st;
        }
        *out = one;
        *nout = 1;
        return PC_OK;
    }
    if (!dash_input_ok(pl, pattern, npattern, offset)) {
        *out = NULL;
        *nout = 0;
        return PC_OK;
    }
    for (i = 0; i < npat; i++) {
        pat[i] = pattern[i % npattern];
        total += pat[i];
    }
    if (!isfinite(total) || total <= 1e-9 || pl->len < 2) {
        *out = NULL;
        *nout = 0;
        return PC_OK;
    }
    npts = pl->len + (pl->closed ? 1u : 0u);
    pts = pc_malloc_array(npts, sizeof *pts);
    knots = pc_malloc_array(npts, sizeof *knots);
    if (pts == NULL || knots == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
        goto cleanup;
    }
    for (i = 0; i < pl->len; i++) {
        pts[i] = mk(pl->pts[i][0], pl->pts[i][1]);
        knots[i] = pl->knot != NULL ? pl->knot[i] : true;
    }
    if (pl->closed) {
        pts[pl->len] = pts[0];
        knots[pl->len] = true;
    }
    /* Position in the pattern (offset.rem_euclid(total)). */
    left = pat[0];
    skip = fmod(offset, total);
    if (skip < 0.0) {
        skip += fabs(total);
    }
    while (skip > 0.0) {
        if (skip >= left) {
            skip -= left;
            idx = (idx + 1) % npat;
            left = pat[idx];
        } else {
            left -= skip;
            skip = 0.0;
        }
    }
    if (idx % 2 == 0) {
        st = dash_start(&cur, pts[0], err);
        has_cur = st == PC_OK;
        if (st != PC_OK) {
            goto cleanup;
        }
    }
    for (i = 0; i + 1 < npts && st == PC_OK && !empty; i++) {
        P a = pts[i];
        P b = pts[i + 1];
        bool b_knot = knots[i + 1];
        double seg = len(sub(b, a));
        double pos = 0.0;
        while (seg - pos > 1e-12) {
            double step;
            bool at_end;
            P p;
            guard++;
            /* Bounded work and memory, including for file-provided patterns; a step too small
             * to advance pos would never finish. Either way the polyline gets no dashes. */
            step = pc_rs_min(left, seg - pos);
            if (guard > DASH_GUARD || (step > 0.0 && pos + step == pos)) {
                empty = true;
                break;
            }
            pos += step;
            left -= step;
            at_end = seg - pos <= 1e-12;
            p = at_end ? b : add(a, mul(sub(b, a), pos / seg));
            if (has_cur) {
                st = pc_vec_polyline_append(&cur, p.x, p.y, !at_end || b_knot, err);
                if (st != PC_OK) {
                    break;
                }
            }
            if (left <= 1e-12) {
                idx = (idx + 1) % npat;
                left = pat[idx];
                if (idx % 2 == 1) {
                    if (has_cur) {
                        has_cur = false;
                        st = dash_list_push(&res, &cur, err);
                        if (st != PC_OK) {
                            break;
                        }
                    }
                } else {
                    if (has_cur) {
                        pc_vec_polyline_fini(&cur);
                    }
                    st = dash_start(&cur, p, err);
                    has_cur = st == PC_OK;
                    if (st != PC_OK) {
                        break;
                    }
                }
            }
        }
    }
    if (empty) {
        pc_vec_polylines_free(res.v, res.len);
        memset(&res, 0, sizeof res);
        goto cleanup;
    }
    if (st == PC_OK && has_cur && cur.len > 1) {
        has_cur = false;
        st = dash_list_push(&res, &cur, err);
    }
    if (st == PC_OK && pl->closed) {
        st = dash_stitch_seam(pl, &res, err);
    }
cleanup:
    if (has_cur) {
        pc_vec_polyline_fini(&cur);
    }
    free(pts);
    free(knots);
    if (st != PC_OK) {
        pc_vec_polylines_free(res.v, res.len);
        return st;
    }
    *out = res.v;
    *nout = res.len;
    return PC_OK;
}
