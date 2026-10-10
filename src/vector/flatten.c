/* Bezier flattening with a distance tolerance (port of crates/vector/src/flatten.rs), plus the
 * small container helpers the other vector files share. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "vector/internal.h"

/* ---- shared helpers ---- */

void pc_vec_stable_sort(void *base, size_t n, size_t size, void *scratch,
                        int (*cmp)(const void *a, const void *b))
{
    unsigned char *a = base;
    unsigned char *tmp = scratch;
    size_t width;
    if (n < 2) {
        return;
    }
    /* Insertion sort for runs of 16, then bottom-up merges: stable, O(n log n). */
    {
        size_t run;
        for (run = 0; run < n; run += 16) {
            size_t end = run + 16 < n ? run + 16 : n;
            size_t i;
            for (i = run + 1; i < end; i++) {
                size_t j = i;
                memcpy(tmp, a + i * size, size);
                while (j > run && cmp(a + (j - 1) * size, tmp) > 0) {
                    j--;
                }
                if (j != i) {
                    memmove(a + (j + 1) * size, a + j * size, (i - j) * size);
                    memcpy(a + j * size, tmp, size);
                }
            }
        }
    }
    for (width = 16; width < n; width *= 2) {
        size_t lo;
        for (lo = 0; lo + width < n; lo += 2 * width) {
            size_t mid = lo + width;
            size_t hi = mid + width < n ? mid + width : n;
            size_t i = lo;
            size_t j = mid;
            size_t k = 0;
            /* Already ordered: nothing to merge. */
            if (cmp(a + (mid - 1) * size, a + mid * size) <= 0) {
                continue;
            }
            while (i < mid && j < hi) {
                /* Take from the right only when strictly smaller: keeps equal keys in order. */
                if (cmp(a + j * size, a + i * size) < 0) {
                    memcpy(tmp + k * size, a + j * size, size);
                    j++;
                } else {
                    memcpy(tmp + k * size, a + i * size, size);
                    i++;
                }
                k++;
            }
            if (i < mid) {
                memcpy(tmp + k * size, a + i * size, (mid - i) * size);
                k += mid - i;
            }
            if (j < hi) {
                memcpy(tmp + k * size, a + j * size, (hi - j) * size);
                k += hi - j;
            }
            memcpy(a + lo * size, tmp, k * size);
        }
        if (width > SIZE_MAX / 4) {
            break;
        }
    }
}

static pc_status polyline_reserve(pc_vec_polyline *p, size_t need, pc_error *err)
{
    size_t cap;
    void *np;
    void *nk;
    if (need <= p->cap) {
        return PC_OK;
    }
    if (need > PC_VEC_MAX_POINTS) {
        return pc_fail(err, PC_ERR_LIMIT, "vector: more than %zu points", (size_t)PC_VEC_MAX_POINTS);
    }
    cap = p->cap;
    np = pc_grow_array(p->pts, &cap, need, sizeof *p->pts);
    if (np == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
    }
    p->pts = np;
    cap = p->cap;
    nk = pc_grow_array(p->knot, &cap, need, sizeof *p->knot);
    if (nk == NULL) {
        /* pts already grew; its capacity is at least the old one, so keep the old count. */
        return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
    }
    p->knot = nk;
    p->cap = cap;
    return PC_OK;
}

pc_status pc_vec_polyline_append(pc_vec_polyline *p, double x, double y, bool knot, pc_error *err)
{
    pc_status st = polyline_reserve(p, p->len + 1, err);
    if (st != PC_OK) {
        return st;
    }
    p->pts[p->len][0] = x;
    p->pts[p->len][1] = y;
    p->knot[p->len] = knot;
    p->len++;
    return PC_OK;
}

pc_status pc_vec_polyline_push(pc_vec_polyline *p, double x, double y, bool knot, pc_error *err)
{
    if (p->len > 0) {
        const double *last = p->pts[p->len - 1];
        if (fabs(last[0] - x) < 1e-12 && fabs(last[1] - y) < 1e-12) {
            if (knot) {
                p->knot[p->len - 1] = true;
            }
            return PC_OK;
        }
    }
    return pc_vec_polyline_append(p, x, y, knot, err);
}

void pc_vec_polyline_fini(pc_vec_polyline *p)
{
    if (p == NULL) {
        return;
    }
    free(p->pts);
    free(p->knot);
    memset(p, 0, sizeof *p);
}

void pc_vec_polylines_free(pc_vec_polyline *v, size_t n)
{
    size_t i;
    if (v == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        pc_vec_polyline_fini(&v[i]);
    }
    free(v);
}

pc_status pc_vec_polyline_clone(const pc_vec_polyline *p, pc_vec_polyline *out, pc_error *err)
{
    pc_vec_polyline c;
    pc_status st;
    memset(&c, 0, sizeof c);
    c.closed = p->closed;
    st = polyline_reserve(&c, p->len, err);
    if (st != PC_OK) {
        pc_vec_polyline_fini(&c);
        return st;
    }
    if (p->len > 0) {
        memcpy(c.pts, p->pts, p->len * sizeof *p->pts);
        memcpy(c.knot, p->knot, p->len * sizeof *p->knot);
    }
    c.len = p->len;
    *out = c;
    return PC_OK;
}

void pc_vec_polys_fini(pc_vec_polys *p)
{
    if (p == NULL) {
        return;
    }
    pc_vec_polys_truncate(p, 0);
    free(p->v);
    memset(p, 0, sizeof *p);
}

void pc_vec_polys_truncate(pc_vec_polys *l, size_t from)
{
    size_t i;
    for (i = from; i < l->len; i++) {
        free(l->v[i].pts);
    }
    if (from < l->len) {
        l->len = from;
    }
}

pc_status pc_vec_polys_push(pc_vec_polys *l, double (*pts)[2], size_t len, pc_error *err)
{
    if (l->len == l->cap) {
        size_t cap = l->cap;
        void *nv = pc_grow_array(l->v, &cap, l->len + 1, sizeof *l->v);
        if (nv == NULL) {
            free(pts);
            return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
        }
        l->v = nv;
        l->cap = cap;
    }
    l->v[l->len].pts = pts;
    l->v[l->len].len = len;
    l->len++;
    return PC_OK;
}

void pc_vec_path_from(pc_subpath *subpaths, size_t n, pc_path *out)
{
    memset(out, 0, sizeof *out);
    out->subpaths = n > 0 ? subpaths : NULL;
    out->nsubpaths = n;
    out->fill_rule = PC_FILL_RULE_NONZERO;
    out->inverted = false;
    if (n == 0) {
        free(subpaths);
    }
}

/* ---- flattening ---- */

/* Straight segment: both control points lie on the chord (within 1e-9 px) between its ends. */
static bool on_chord(const pc_point *p0, double dx, double dy, double len2, const pc_point *c)
{
    double vx = c->x - p0->x;
    double vy = c->y - p0->y;
    double t;
    double cross;
    if (len2 < 1e-24) {
        return fabs(vx) < 1e-9 && fabs(vy) < 1e-9;
    }
    t = (vx * dx + vy * dy) / len2;
    cross = fabs(vx * dy - vy * dx) / sqrt(len2);
    return cross < 1e-9 && t >= -1e-9 && t <= 1.0 + 1e-9;
}

static bool is_line(const pc_point seg[4])
{
    double dx = seg[3].x - seg[0].x;
    double dy = seg[3].y - seg[0].y;
    double len2 = dx * dx + dy * dy;
    return on_chord(&seg[0], dx, dy, len2, &seg[1]) && on_chord(&seg[0], dx, dy, len2, &seg[2]);
}

/* Number of uniform pieces so that the chord deviation of the cubic is at most tol (bound
 * max|B''| / 8n^2 with max|B''| <= 6 * max(|p0 - 2c1 + c2|, |c1 - 2c2 + p1|)). */
static size_t pieces(const pc_point seg[4], double tol)
{
    const pc_point *p0 = &seg[0];
    const pc_point *c1 = &seg[1];
    const pc_point *c2 = &seg[2];
    const pc_point *p1 = &seg[3];
    double ax = p0->x - 2.0 * c1->x + c2->x;
    double ay = p0->y - 2.0 * c1->y + c2->y;
    double bx = c1->x - 2.0 * c2->x + p1->x;
    double by = c1->y - 2.0 * c2->y + p1->y;
    double d1 = sqrt(ax * ax + ay * ay);
    double d2 = sqrt(bx * bx + by * by);
    double m = pc_rs_max(d1, d2);
    /* Small curves get a proportionally finer tolerance so their area stays accurate. */
    double size = hypot(c1->x - p0->x, c1->y - p0->y) + hypot(c2->x - c1->x, c2->y - c1->y) +
                  hypot(p1->x - c2->x, p1->y - c2->y);
    double t = pc_rs_max_k(pc_rs_min(tol, size * 1e-3), 1e-4);
    double n = ceil(sqrt(0.75 * m / t));
    size_t k;
    if (!isfinite(n)) {
        return 1;
    }
    k = pc_f64_to_size(n);
    if (k < 1) {
        k = 1;
    }
    if (k > ((size_t)1 << 16)) {
        k = (size_t)1 << 16;
    }
    return k;
}

static void eval(const pc_point seg[4], double t, double *x, double *y)
{
    double mt = 1.0 - t;
    double a = mt * mt * mt;
    double b = 3.0 * mt * mt * t;
    double c = 3.0 * mt * t * t;
    double d = t * t * t;
    *x = a * seg[0].x + b * seg[1].x + c * seg[2].x + d * seg[3].x;
    *y = a * seg[0].y + b * seg[1].y + c * seg[2].y + d * seg[3].y;
}

pc_status pc_vec_flatten_subpath(const pc_subpath *s, double tol, pc_vec_polyline *out, pc_error *err)
{
    pc_vec_polyline p;
    pc_status st = PC_OK;
    size_t nseg;
    size_t i;
    memset(&p, 0, sizeof p);
    p.closed = s->closed;
    if (s->nknots > 0) {
        st = pc_vec_polyline_push(&p, s->knots[0].anchor.x, s->knots[0].anchor.y, true, err);
    }
    nseg = pc_subpath_segment_count(s);
    for (i = 0; i < nseg && st == PC_OK; i++) {
        pc_point seg[4];
        size_t n;
        size_t j;
        pc_subpath_segment(s, i, seg);
        if (is_line(seg)) {
            st = pc_vec_polyline_push(&p, seg[3].x, seg[3].y, true, err);
            continue;
        }
        n = pieces(seg, tol);
        for (j = 1; j <= n && st == PC_OK; j++) {
            double x;
            double y;
            eval(seg, (double)j / (double)n, &x, &y);
            st = pc_vec_polyline_push(&p, x, y, j == n, err);
        }
    }
    if (st != PC_OK) {
        pc_vec_polyline_fini(&p);
        return st;
    }
    if (s->closed && p.len > 1) {
        const double *a = p.pts[0];
        const double *b = p.pts[p.len - 1];
        if (fabs(a[0] - b[0]) < 1e-12 && fabs(a[1] - b[1]) < 1e-12) {
            p.len--;
            p.knot[0] = true;
        }
    }
    *out = p;
    return PC_OK;
}

pc_status pc_vec_flatten_path(const pc_path *p, double tol, pc_vec_polyline **out, size_t *n, pc_error *err)
{
    pc_vec_polyline *v = NULL;
    size_t i;
    size_t total = 0;
    if (p->nsubpaths > 0) {
        v = pc_calloc_array(p->nsubpaths, sizeof *v);
        if (v == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
        }
    }
    for (i = 0; i < p->nsubpaths; i++) {
        pc_status st = pc_vec_flatten_subpath(&p->subpaths[i], tol, &v[i], err);
        if (st == PC_OK) {
            total += v[i].len;
            if (total > PC_VEC_MAX_POINTS) {
                st = pc_fail(err, PC_ERR_LIMIT, "vector: more than %zu points", (size_t)PC_VEC_MAX_POINTS);
                i++;
            }
        }
        if (st != PC_OK) {
            pc_vec_polylines_free(v, i);
            return st;
        }
    }
    *out = v;
    *n = p->nsubpaths;
    return PC_OK;
}
