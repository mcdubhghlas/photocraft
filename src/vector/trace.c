/* Coverage mask -> path, "Make Work Path" from a selection (port of
 * crates/vector/src/trace.rs).
 *
 * 1. Marching squares at level 0.5 over pixel centres (linear interpolation along cell edges,
 *    saddles resolved with the cell average) gives closed, non-intersecting contours.
 * 2. Corners are found where the direction over a +-1.5 px window turns by more than 60 deg.
 * 3. Each run between corners is fitted with cubic Beziers by least squares with Newton
 *    reparameterisation, splitting at the worst point until within tolerance (P. J. Schneider,
 *    "An Algorithm for Automatically Fitting Digitized Curves", Graphics Gems, 1990).
 *
 * Contours are emitted as subpaths: the first combines, the rest exclude, which is the even-odd
 * area for nested, non-crossing contours regardless of their orientation. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "vector/internal.h"

/* The reference's recursion cap for curve fitting. */
#define FIT_MAX_DEPTH 40u

typedef struct P {
    double x, y;
} P;

/* ---- edge map (the reference's HashMap<(x, y, kind), (x, y, kind)>) ---- */

typedef struct ekey {
    int64_t x, y;
    uint8_t kind; /* 0 = horizontal edge (x,y)-(x+1,y), 1 = vertical (x,y)-(x,y+1) */
} ekey;

typedef struct eslot {
    ekey key;
    ekey val;
    uint8_t state; /* 0 empty, 1 live, 2 removed (tombstone) */
} eslot;

typedef struct emap {
    eslot *slots;
    size_t cap;  /* power of two */
    size_t used; /* live + tombstones */
    size_t live;
} emap;

static bool key_eq(ekey a, ekey b)
{
    return a.x == b.x && a.y == b.y && a.kind == b.kind;
}

static size_t key_hash(ekey k)
{
    uint64_t h = (uint64_t)k.x * 0x9e3779b97f4a7c15u;
    h ^= ((uint64_t)k.y + 0x632be59bd9b4e019u) * 0xc2b2ae3d27d4eb4fu;
    h ^= (uint64_t)k.kind * 0x165667b19e3779f9u;
    h ^= h >> 29;
    h *= 0xbf58476d1ce4e5b9u;
    h ^= h >> 32;
    return (size_t)h;
}

static eslot *emap_find(const emap *m, ekey k)
{
    size_t i;
    if (m->cap == 0) {
        return NULL;
    }
    i = key_hash(k) & (m->cap - 1);
    for (;;) {
        eslot *s = &m->slots[i];
        if (s->state == 0) {
            return NULL;
        }
        if (s->state == 1 && key_eq(s->key, k)) {
            return s;
        }
        i = (i + 1) & (m->cap - 1);
    }
}

static pc_status emap_rehash(emap *m, size_t cap, pc_error *err)
{
    eslot *ns = pc_calloc_array(cap, sizeof *ns);
    size_t i;
    if (ns == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
    }
    for (i = 0; i < m->cap; i++) {
        if (m->slots[i].state == 1) {
            size_t j = key_hash(m->slots[i].key) & (cap - 1);
            while (ns[j].state != 0) {
                j = (j + 1) & (cap - 1);
            }
            ns[j] = m->slots[i];
        }
    }
    free(m->slots);
    m->slots = ns;
    m->cap = cap;
    m->used = m->live;
    return PC_OK;
}

/* Insert or overwrite (last insert wins, like HashMap::insert). */
static pc_status emap_insert(emap *m, ekey k, ekey v, pc_error *err)
{
    eslot *s;
    size_t i;
    if ((m->used + 1) * 2 > m->cap) {
        /* Rehash (dropping tombstones) to a table at most a quarter full. */
        size_t cap = 64;
        pc_status st;
        while ((m->live + 1) * 4 > cap) {
            if (cap > PC_MAX_ALLOC / sizeof(eslot) / 2) {
                return pc_fail(err, PC_ERR_LIMIT, "vector: trace has too many contour edges");
            }
            cap *= 2;
        }
        st = emap_rehash(m, cap, err);
        if (st != PC_OK) {
            return st;
        }
    }
    s = emap_find(m, k);
    if (s != NULL) {
        s->val = v;
        return PC_OK;
    }
    i = key_hash(k) & (m->cap - 1);
    while (m->slots[i].state == 1) {
        i = (i + 1) & (m->cap - 1);
    }
    if (m->slots[i].state == 0) {
        m->used++;
    }
    m->slots[i].key = k;
    m->slots[i].val = v;
    m->slots[i].state = 1;
    m->live++;
    return PC_OK;
}

/* ---- contours ---- */

typedef struct grid {
    const float *values;
    int64_t w, h;
    float outside;
    pc_rect rect;
} grid;

static float gget(const grid *g, int64_t x, int64_t y)
{
    if (x < 0 || y < 0 || x >= g->w || y >= g->h) {
        return g->outside;
    }
    return g->values[(size_t)(y * g->w + x)];
}

static P point_between(const grid *g, int64_t ax0, int64_t ay0, int64_t bx0, int64_t by0)
{
    const float lvl = 0.5f;
    float va = gget(g, ax0, ay0);
    float vb = gget(g, bx0, by0);
    double t = fabsf(vb - va) > 1e-9f ? pc_clampd((double)((lvl - va) / (vb - va)), 0.0, 1.0) : 0.5;
    double ax = (double)ax0 + 0.5;
    double ay = (double)ay0 + 0.5;
    double bx = (double)bx0 + 0.5;
    double by = (double)by0 + 0.5;
    P p;
    p.x = (double)g->rect.x0 + ax + (bx - ax) * t;
    p.y = (double)g->rect.y0 + ay + (by - ay) * t;
    return p;
}

static P edge_point(const grid *g, ekey e)
{
    if (e.kind == 0) {
        return point_between(g, e.x, e.y, e.x + 1, e.y);
    }
    return point_between(g, e.x, e.y, e.x, e.y + 1);
}

static ekey mkkey(int64_t x, int64_t y, uint8_t kind)
{
    ekey k;
    k.x = x;
    k.y = y;
    k.kind = kind;
    return k;
}

static int key_order(const void *pa, const void *pb)
{
    const ekey *a = pa;
    const ekey *b = pb;
    if (a->y != b->y) {
        return a->y < b->y ? -1 : 1;
    }
    if (a->x != b->x) {
        return a->x < b->x ? -1 : 1;
    }
    return (a->kind > b->kind) - (a->kind < b->kind);
}

static pc_status contour_cells(const grid *g, emap *next, pc_error *err)
{
    const float lvl = 0.5f;
    int64_t cy;
    for (cy = -1; cy < g->h; cy++) {
        int64_t cx;
        for (cx = -1; cx < g->w; cx++) {
            float v00 = gget(g, cx, cy);
            float v10 = gget(g, cx + 1, cy);
            float v11 = gget(g, cx + 1, cy + 1);
            float v01 = gget(g, cx, cy + 1);
            unsigned idx = (v00 >= lvl ? 1u : 0u) | (v10 >= lvl ? 2u : 0u) | (v11 >= lvl ? 4u : 0u) |
                           (v01 >= lvl ? 8u : 0u);
            ekey top = mkkey(cx, cy, 0);
            ekey bottom = mkkey(cx, cy + 1, 0);
            ekey left = mkkey(cx, cy, 1);
            ekey right = mkkey(cx + 1, cy, 1);
            ekey sa[2];
            ekey sb[2];
            size_t ns = 0;
            size_t i;
            if (idx == 0 || idx == 15) {
                continue;
            }
            /* Walking along the contour with the inside on the left-hand side. */
            switch (idx) {
            case 1:
                sa[0] = left;
                sb[0] = top;
                ns = 1;
                break;
            case 2:
                sa[0] = top;
                sb[0] = right;
                ns = 1;
                break;
            case 3:
                sa[0] = left;
                sb[0] = right;
                ns = 1;
                break;
            case 4:
                sa[0] = right;
                sb[0] = bottom;
                ns = 1;
                break;
            case 5: {
                float centre = (v00 + v10 + v11 + v01) / 4.0f;
                if (centre >= lvl) {
                    sa[0] = left;
                    sb[0] = bottom;
                    sa[1] = right;
                    sb[1] = top;
                } else {
                    sa[0] = left;
                    sb[0] = top;
                    sa[1] = right;
                    sb[1] = bottom;
                }
                ns = 2;
                break;
            }
            case 6:
                sa[0] = top;
                sb[0] = bottom;
                ns = 1;
                break;
            case 7:
                sa[0] = left;
                sb[0] = bottom;
                ns = 1;
                break;
            case 8:
                sa[0] = bottom;
                sb[0] = left;
                ns = 1;
                break;
            case 9:
                sa[0] = bottom;
                sb[0] = top;
                ns = 1;
                break;
            case 10: {
                float centre = (v00 + v10 + v11 + v01) / 4.0f;
                if (centre >= lvl) {
                    sa[0] = top;
                    sb[0] = left;
                    sa[1] = bottom;
                    sb[1] = right;
                } else {
                    sa[0] = top;
                    sb[0] = right;
                    sa[1] = bottom;
                    sb[1] = left;
                }
                ns = 2;
                break;
            }
            case 11:
                sa[0] = bottom;
                sb[0] = right;
                ns = 1;
                break;
            case 12:
                sa[0] = right;
                sb[0] = left;
                ns = 1;
                break;
            case 13:
                sa[0] = right;
                sb[0] = top;
                ns = 1;
                break;
            case 14:
                sa[0] = top;
                sb[0] = left;
                ns = 1;
                break;
            default:
                break;
            }
            for (i = 0; i < ns; i++) {
                pc_status st = emap_insert(next, sa[i], sb[i], err);
                if (st != PC_OK) {
                    return st;
                }
            }
        }
    }
    return PC_OK;
}

pc_status pc_vec_contours(const float *values, size_t len, pc_rect rect, float outside, pc_vec_polys *out,
                          pc_error *err)
{
    grid g;
    emap next;
    ekey *keys = NULL;
    size_t nkeys = 0;
    size_t i;
    size_t need;
    pc_vec_polys res = {NULL, 0, 0};
    P *lp = NULL;
    size_t lp_cap = 0;
    pc_status st = PC_OK;
    memset(&next, 0, sizeof next);
    g.values = values;
    g.w = (int64_t)pc_rect_width(rect);
    g.h = (int64_t)pc_rect_height(rect);
    g.outside = outside;
    g.rect = rect;
    if (!pc_size_mul((size_t)g.w, (size_t)g.h, &need) || need > len) {
        return pc_fail(err, PC_ERR_INVALID, "vector: trace mask has %zu values, %zux%zu needed", len,
                       (size_t)g.w, (size_t)g.h);
    }
    /* An empty rect reads `outside` everywhere: every cell is uniform, no contour. */
    if (g.w == 0 || g.h == 0) {
        *out = res;
        return PC_OK;
    }
    st = contour_cells(&g, &next, err);
    if (st != PC_OK) {
        goto cleanup;
    }
    if (next.live > 0) {
        keys = pc_malloc_array(next.live, sizeof *keys);
        if (keys == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
            goto cleanup;
        }
        for (i = 0; i < next.cap; i++) {
            if (next.slots[i].state == 1) {
                keys[nkeys++] = next.slots[i].key;
            }
        }
        qsort(keys, nkeys, sizeof *keys, key_order); /* keys are unique */
    }
    for (i = 0; i < nkeys && st == PC_OK; i++) {
        ekey start = keys[i];
        ekey cur = start;
        size_t n = 0;
        size_t k;
        size_t kept;
        eslot *s;
        if (emap_find(&next, start) == NULL) {
            continue;
        }
        while ((s = emap_find(&next, cur)) != NULL) {
            ekey nx = s->val;
            s->state = 2; /* remove */
            next.live--;
            if (n == lp_cap) {
                void *nl = pc_grow_array(lp, &lp_cap, n + 1, sizeof *lp);
                if (nl == NULL) {
                    st = pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
                    break;
                }
                lp = nl;
            }
            lp[n++] = edge_point(&g, cur);
            cur = nx;
            if (key_eq(cur, start)) {
                break;
            }
        }
        if (st != PC_OK) {
            break;
        }
        /* Drop consecutive duplicates (interpolation can land on a corner). */
        kept = 0;
        for (k = 0; k < n; k++) {
            if (kept > 0 && fabs(lp[k].x - lp[kept - 1].x) < 1e-9 && fabs(lp[k].y - lp[kept - 1].y) < 1e-9) {
                continue;
            }
            lp[kept++] = lp[k];
        }
        if (kept >= 3) {
            double (*pts)[2] = pc_malloc_array(kept, sizeof *pts);
            if (pts == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
                break;
            }
            for (k = 0; k < kept; k++) {
                pts[k][0] = lp[k].x;
                pts[k][1] = lp[k].y;
            }
            st = pc_vec_polys_push(&res, pts, kept, err);
        }
    }
cleanup:
    free(keys);
    free(lp);
    free(next.slots);
    if (st != PC_OK) {
        pc_vec_polys_fini(&res);
        return st;
    }
    *out = res;
    return PC_OK;
}

/* ---- curve fitting ---- */

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
static double dot(P a, P b)
{
    return a.x * b.x + a.y * b.y;
}
static double len(P a)
{
    return hypot(a.x, a.y);
}
static P norm(P a)
{
    double l = len(a);
    return l > 1e-12 ? mk(a.x / l, a.y / l) : mk(0.0, 0.0);
}

/* Point `window` px of arc length away from i (forwards or backwards) on a closed loop. */
static P walk(const P *lp, size_t n, size_t i, double window, bool fwd)
{
    double acc = 0.0;
    size_t cur = i;
    size_t s;
    for (s = 0; s < n; s++) {
        size_t nx = fwd ? (cur + 1) % n : (cur + n - 1) % n;
        double d = len(sub(lp[nx], lp[cur]));
        if (acc + d >= window) {
            double t = (window - acc) / pc_rs_max_k(d, 1e-12);
            return add(lp[cur], mul(sub(lp[nx], lp[cur]), t));
        }
        acc += d;
        cur = nx;
    }
    return lp[cur];
}

/* Indices of corner points (*out malloc'd, may be NULL when there are none). */
static pc_status corners(const P *lp, size_t n, size_t **out, size_t *nout, pc_error *err)
{
    const double window = 1.5;
    double cos_thresh = cos(pc_vec_to_radians(60.0));
    double *turn = pc_malloc_array(n, sizeof *turn);
    size_t *cs = pc_malloc_array(n, sizeof *cs);
    size_t ncs = 0;
    size_t i;
    if (turn == NULL || cs == NULL) {
        free(turn);
        free(cs);
        return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
    }
    for (i = 0; i < n; i++) {
        P a = norm(sub(lp[i], walk(lp, n, i, window, false)));
        P b = norm(sub(walk(lp, n, i, window, true), lp[i]));
        turn[i] = dot(a, b);
    }
    for (i = 0; i < n; i++) {
        bool is_min = true;
        double acc = 0.0;
        size_t j = i;
        if (turn[i] >= cos_thresh) {
            continue;
        }
        /* Local minimum of the cosine within +-window (ties: first index). */
        while (acc < window) {
            size_t nx = (j + 1) % n;
            acc += len(sub(lp[nx], lp[j]));
            j = nx;
            if (j == i) {
                break;
            }
            if (turn[j] < turn[i]) {
                is_min = false;
                break;
            }
        }
        acc = 0.0;
        j = i;
        while (is_min && acc < window) {
            size_t pv = (j + n - 1) % n;
            acc += len(sub(lp[j], lp[pv]));
            j = pv;
            if (j == i) {
                break;
            }
            if (turn[j] <= turn[i]) {
                is_min = false;
            }
        }
        if (is_min) {
            cs[ncs++] = i;
        }
    }
    free(turn);
    *out = cs;
    *nout = ncs;
    return PC_OK;
}

typedef struct seg4 {
    P p[4];
} seg4;

typedef struct seg_list {
    seg4 *v;
    bool *corner; /* corner_at_start */
    size_t len;
    size_t cap;
} seg_list;

static pc_status seg_push(seg_list *l, seg4 s, pc_error *err)
{
    if (l->len == l->cap) {
        size_t cap = l->cap;
        size_t cap2;
        void *nv;
        void *nc;
        if (l->len + 1 > PC_VEC_MAX_POINTS) {
            return pc_fail(err, PC_ERR_LIMIT, "vector: trace has more than %zu segments",
                           (size_t)PC_VEC_MAX_POINTS);
        }
        nv = pc_grow_array(l->v, &cap, l->len + 1, sizeof *l->v);
        if (nv == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
        }
        l->v = nv;
        cap2 = l->cap;
        nc = pc_grow_array(l->corner, &cap2, l->len + 1, sizeof *l->corner);
        if (nc == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
        }
        l->corner = nc;
        l->cap = cap < cap2 ? cap : cap2;
    }
    l->v[l->len] = s;
    l->corner[l->len] = false;
    l->len++;
    return PC_OK;
}

static P bezier(const seg4 *b, double t)
{
    double mt = 1.0 - t;
    double a = mt * mt * mt;
    double bb = 3.0 * mt * mt * t;
    double c = 3.0 * mt * t * t;
    double d = t * t * t;
    return mk(a * b->p[0].x + bb * b->p[1].x + c * b->p[2].x + d * b->p[3].x,
              a * b->p[0].y + bb * b->p[1].y + c * b->p[2].y + d * b->p[3].y);
}

static P bezier_d1(const seg4 *b, double t)
{
    double mt = 1.0 - t;
    P q0 = mul(sub(b->p[1], b->p[0]), 3.0);
    P q1 = mul(sub(b->p[2], b->p[1]), 3.0);
    P q2 = mul(sub(b->p[3], b->p[2]), 3.0);
    return add(add(mul(q0, mt * mt), mul(q1, 2.0 * mt * t)), mul(q2, t * t));
}

static P bezier_d2(const seg4 *b, double t)
{
    P r0 = mul(add(sub(b->p[2], mul(b->p[1], 2.0)), b->p[0]), 6.0);
    P r1 = mul(add(sub(b->p[3], mul(b->p[2], 2.0)), b->p[1]), 6.0);
    return add(mul(r0, 1.0 - t), mul(r1, t));
}

static void chord_params(const P *pts, size_t n, double *u)
{
    size_t i;
    double total;
    u[0] = 0.0;
    for (i = 1; i < n; i++) {
        u[i] = u[i - 1] + len(sub(pts[i], pts[i - 1]));
    }
    total = u[n - 1];
    if (total > 0.0) {
        for (i = 0; i < n; i++) {
            u[i] /= total;
        }
    }
}

/* Least-squares control points with fixed end tangents. */
static seg4 generate(const P *pts, size_t n, const double *u, P t1, P t2)
{
    P p0 = pts[0];
    P p3 = pts[n - 1];
    double c00 = 0.0;
    double c01 = 0.0;
    double c10;
    double c11 = 0.0;
    double x0 = 0.0;
    double x1 = 0.0;
    double det;
    double det0;
    double det1;
    double al;
    double ar;
    double seg;
    double eps;
    size_t i;
    seg4 r;
    for (i = 0; i < n; i++) {
        double t = u[i];
        double mt = 1.0 - t;
        double b1 = 3.0 * mt * mt * t;
        double b2 = 3.0 * mt * t * t;
        P a1 = mul(t1, b1);
        P a2 = mul(t2, b2);
        double b0;
        double b3;
        P tmp;
        c00 += dot(a1, a1);
        c01 += dot(a1, a2);
        c11 += dot(a2, a2);
        b0 = mt * mt * mt;
        b3 = t * t * t;
        tmp = sub(pts[i], add(mul(p0, b0 + b1), mul(p3, b2 + b3)));
        x0 += dot(a1, tmp);
        x1 += dot(a2, tmp);
    }
    c10 = c01;
    det = c00 * c11 - c10 * c01;
    det0 = c00 * x1 - c10 * x0;
    det1 = x0 * c11 - x1 * c01;
    if (fabs(det) > 1e-12) {
        al = det1 / det;
        ar = det0 / det;
    } else {
        al = 0.0;
        ar = 0.0;
    }
    seg = len(sub(p3, p0));
    eps = 1e-6 * seg;
    if (al < eps || ar < eps) {
        al = seg / 3.0;
        ar = seg / 3.0;
    }
    r.p[0] = p0;
    r.p[1] = add(p0, mul(t1, al));
    r.p[2] = add(p3, mul(t2, ar));
    r.p[3] = p3;
    return r;
}

static double max_error(const P *pts, size_t n, const seg4 *b, const double *u, size_t *split)
{
    double best = 0.0;
    size_t bi = n / 2;
    size_t i;
    for (i = 1; i + 1 < n; i++) {
        P d = sub(bezier(b, u[i]), pts[i]);
        double e = dot(d, d);
        if (e >= best) {
            best = e;
            bi = i;
        }
    }
    *split = bi;
    return best;
}

static void reparam(const P *pts, size_t n, const double *u, const seg4 *b, double *out)
{
    size_t i;
    for (i = 0; i < n; i++) {
        double t = u[i];
        P q = bezier(b, t);
        P d1 = bezier_d1(b, t);
        P d2 = bezier_d2(b, t);
        double num = dot(sub(q, pts[i]), d1);
        double den = dot(d1, d1) + dot(sub(q, pts[i]), d2);
        out[i] = fabs(den) < 1e-12 ? t : pc_clampd(t - num / den, 0.0, 1.0);
    }
}

/* Recursion depth <= FIT_MAX_DEPTH + 1. */
static pc_status fit_cubic(const P *pts, size_t n, P t1, P t2, double err2, seg_list *out, uint32_t depth,
                           pc_error *err)
{
    double *u;
    double *u2;
    seg4 b;
    double e;
    size_t split;
    P tc;
    pc_status st;
    if (n == 2 || depth > FIT_MAX_DEPTH) {
        double d = len(sub(pts[n - 1], pts[0])) / 3.0;
        seg4 s;
        s.p[0] = pts[0];
        s.p[1] = add(pts[0], mul(t1, d));
        s.p[2] = add(pts[n - 1], mul(t2, d));
        s.p[3] = pts[n - 1];
        return seg_push(out, s, err);
    }
    u = pc_malloc_array(n, sizeof *u);
    u2 = pc_malloc_array(n, sizeof *u2);
    if (u == NULL || u2 == NULL) {
        free(u);
        free(u2);
        return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
    }
    chord_params(pts, n, u);
    b = generate(pts, n, u, t1, t2);
    e = max_error(pts, n, &b, u, &split);
    if (e < err2) {
        free(u);
        free(u2);
        return seg_push(out, b, err);
    }
    if (e < err2 * 16.0) {
        int it;
        for (it = 0; it < 6; it++) {
            seg4 b2;
            double *tmp;
            reparam(pts, n, u, &b, u2);
            b2 = generate(pts, n, u2, t1, t2);
            e = max_error(pts, n, &b2, u2, &split);
            tmp = u;
            u = u2;
            u2 = tmp;
            b = b2;
            if (e < err2) {
                free(u);
                free(u2);
                return seg_push(out, b, err);
            }
        }
    }
    free(u);
    free(u2);
    if (split < 1) {
        split = 1;
    }
    if (split > n - 2) {
        split = n - 2;
    }
    tc = norm(sub(pts[split - 1], pts[split + 1]));
    if (len(tc) < 0.5) {
        tc = norm(sub(pts[split - 1], pts[split]));
    }
    st = fit_cubic(pts, split + 1, t1, tc, err2, out, depth + 1, err);
    if (st != PC_OK) {
        return st;
    }
    return fit_cubic(pts + split, n - split, mul(tc, -1.0), t2, err2, out, depth + 1, err);
}

/* Fits a closed loop (n >= 3) with cubic Beziers. */
static pc_status fit_closed(const P *lp, size_t n, double tol, pc_subpath *out, pc_error *err)
{
    size_t *cs = NULL;
    size_t ncs = 0;
    seg_list segs = {NULL, NULL, 0, 0};
    P *pts = NULL;
    pc_knot *knots = NULL;
    pc_status st = corners(lp, n, &cs, &ncs, err);
    size_t i;
    size_t m;
    if (st != PC_OK) {
        return st;
    }
    pts = pc_malloc_array(n + 1, sizeof *pts);
    if (pts == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
        goto cleanup;
    }
    if (ncs == 0) {
        /* Smooth loop: start at 0 with a shared tangent at the seam. */
        P t = norm(sub(lp[1 % n], lp[n - 1]));
        memcpy(pts, lp, n * sizeof *pts);
        pts[n] = lp[0];
        st = fit_cubic(pts, n + 1, t, mul(t, -1.0), tol * tol, &segs, 0, err);
    } else {
        size_t k;
        for (k = 0; k < ncs && st == PC_OK; k++) {
            size_t c = cs[k];
            size_t end = cs[(k + 1) % ncs];
            size_t np = 0;
            size_t j = c;
            size_t before = segs.len;
            P t1;
            P t2;
            pts[np++] = lp[c];
            for (;;) {
                j = (j + 1) % n;
                pts[np++] = lp[j];
                if (j == end) {
                    break;
                }
            }
            t1 = norm(sub(pts[1 < np - 1 ? 1 : np - 1], pts[0]));
            t2 = norm(sub(pts[np >= 2 ? np - 2 : 0], pts[np - 1]));
            st = fit_cubic(pts, np, t1, t2, tol * tol, &segs, 0, err);
            if (st == PC_OK && segs.len > before) {
                segs.corner[before] = true;
            }
        }
    }
    if (st != PC_OK) {
        goto cleanup;
    }
    m = segs.len;
    knots = pc_malloc_array(m, sizeof *knots);
    if (knots == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
        goto cleanup;
    }
    for (i = 0; i < m; i++) {
        const seg4 *s = &segs.v[i];
        knots[i].anchor = pc_point_make(s->p[0].x, s->p[0].y);
        knots[i].in_ctrl = pc_point_make(s->p[0].x, s->p[0].y);
        knots[i].out_ctrl = pc_point_make(s->p[1].x, s->p[1].y);
        knots[i].smooth = !segs.corner[i];
    }
    for (i = 0; i < m; i++) {
        knots[(i + 1) % m].in_ctrl = pc_point_make(segs.v[i].p[2].x, segs.v[i].p[2].y);
    }
    memset(out, 0, sizeof *out);
    out->closed = true;
    out->knots = knots;
    out->nknots = m;
    out->op = PC_PATH_COMBINE;
    knots = NULL;
cleanup:
    free(cs);
    free(pts);
    free(segs.v);
    free(segs.corner);
    free(knots);
    return st;
}

pc_status pc_vec_trace_mask(const float *values, size_t len, pc_rect rect, float outside, double tolerance,
                            pc_path *out, pc_error *err)
{
    pc_vec_polys loops = {NULL, 0, 0};
    pc_subpath *subs = NULL;
    size_t nsubs = 0;
    size_t i;
    pc_status st = pc_vec_contours(values, len, rect, outside, &loops, err);
    if (st != PC_OK) {
        return st;
    }
    if (loops.len > 0) {
        subs = pc_calloc_array(loops.len, sizeof *subs);
        if (subs == NULL) {
            pc_vec_polys_fini(&loops);
            return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
        }
    }
    for (i = 0; i < loops.len; i++) {
        const pc_vec_poly *lp = &loops.v[i];
        if (lp->len < 3) {
            continue;
        }
        /* Fit to half the tolerance: the worst point may deviate by the full amount only
         * rarely. The points are stored as double[2], the same layout as P. */
        {
            P *pts = pc_malloc_array(lp->len, sizeof *pts);
            size_t k;
            if (pts == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
                break;
            }
            for (k = 0; k < lp->len; k++) {
                pts[k] = mk(lp->pts[k][0], lp->pts[k][1]);
            }
            st = fit_closed(pts, lp->len, pc_rs_max_k(tolerance * 0.5, 0.05), &subs[nsubs], err);
            free(pts);
        }
        if (st != PC_OK) {
            break;
        }
        subs[nsubs].op = i == 0 ? PC_PATH_COMBINE : PC_PATH_EXCLUDE;
        nsubs++;
    }
    pc_vec_polys_fini(&loops);
    if (st != PC_OK) {
        for (i = 0; i < nsubs; i++) {
            pc_subpath_fini(&subs[i]);
        }
        free(subs);
        return st;
    }
    pc_vec_path_from(subs, nsubs, out);
    return PC_OK;
}
