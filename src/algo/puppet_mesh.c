/* Edit > Puppet Warp: the mesh (port of build_mesh and its helpers in crates/algo/src/puppet.rs).
 *
 * The layer's coverage (alpha above a threshold) becomes a signed distance field, offset by the
 * expansion to grow or shrink the region. Its outline is traced (marching squares), simplified
 * (corners kept) and split into sides no longer than the density's spacing; a triangular lattice
 * at that spacing fills the inside, at least half a step from the outline. The mesh is the
 * Delaunay triangulation of those points with the outline's sides recovered (split until each is
 * an edge), minus the triangles outside the outline, numbered in reverse Cuthill-McKee order.
 *
 * The reference's hash map of outline links and its edge hash set are dense arrays / a sorted edge
 * list here: only lookups and an ascending key walk are used, so the result is the same. */
#include "algo/puppet.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

#define NONE SIZE_MAX
/* Coverage threshold: alpha above this counts as part of the puppet. */
#define ALPHA_MIN 0.05f
/* The outline field's longest side, in blocks: how finely the mesh edge follows the outline. */
#define FIELD_BLOCKS 1024.0
/* Most outline points the mesh takes; a busier outline is simplified more coarsely. */
#define MAX_OUTLINE_POINTS 20000u

typedef double pt[2];

/* ---- small growable arrays ---- */

typedef struct ptvec {
    pt *v;
    size_t n, cap;
} ptvec;

static bool ptvec_push(ptvec *a, const double p[2])
{
    if (a->n == a->cap) {
        void *nv = pc_grow_array(a->v, &a->cap, a->n + 1, sizeof *a->v);
        if (nv == NULL) {
            return false;
        }
        a->v = nv;
    }
    a->v[a->n][0] = p[0];
    a->v[a->n][1] = p[1];
    a->n++;
    return true;
}

typedef struct szvec {
    size_t *v;
    size_t n, cap;
} szvec;

static bool szvec_push(szvec *a, size_t x)
{
    if (a->n == a->cap) {
        void *nv = pc_grow_array(a->v, &a->cap, a->n + 1, sizeof *a->v);
        if (nv == NULL) {
            return false;
        }
        a->v = nv;
    }
    a->v[a->n++] = x;
    return true;
}

typedef struct seg {
    size_t a, b;
} seg;

typedef struct segvec {
    seg *v;
    size_t n, cap;
} segvec;

static bool segvec_push(segvec *a, size_t x, size_t y)
{
    if (a->n == a->cap) {
        void *nv = pc_grow_array(a->v, &a->cap, a->n + 1, sizeof *a->v);
        if (nv == NULL) {
            return false;
        }
        a->v = nv;
    }
    a->v[a->n].a = x;
    a->v[a->n].b = y;
    a->n++;
    return true;
}

static double dist2(const double *a, const double *b)
{
    double dx = a[0] - b[0], dy = a[1] - b[1];
    return dx * dx + dy * dy;
}

/* f64::total_cmp as an ordering key. */
static int64_t total_key(double a)
{
    int64_t ia;
    memcpy(&ia, &a, 8);
    return ia ^ (int64_t)((uint64_t)(ia >> 63) >> 1);
}

static int total_cmp(double a, double b)
{
    int64_t ia = total_key(a), ib = total_key(b);
    return ia < ib ? -1 : (ia > ib ? 1 : 0);
}

/* ---- the signed distance field ---- */

typedef struct field {
    double x0, y0, g; /* origin and block size (px) */
    size_t bw, bh;
    float *sd; /* signed distance from each block centre to the outline (px, negative inside) */
    double expansion;
} field;

/* One-dimensional squared distance transform of f (Felzenszwalb & Huttenlocher). */
static void edt_1d(const double *f, size_t n, double *out, size_t *v, double *z)
{
    size_t k = 0, q;
    if (n == 0) {
        return;
    }
    v[0] = 0;
    z[0] = -INFINITY;
    z[1] = INFINITY;
    for (q = 1; q < n; q++) {
        double qf = (double)q, pf = (double)v[k];
        double s = ((f[q] + qf * qf) - (f[v[k]] + pf * pf)) / (2.0 * (qf - pf));
        while (k > 0 && s <= z[k]) {
            k--;
            pf = (double)v[k];
            s = ((f[q] + qf * qf) - (f[v[k]] + pf * pf)) / (2.0 * (qf - pf));
        }
        k++;
        v[k] = q;
        z[k] = s;
        z[k + 1] = INFINITY;
    }
    k = 0;
    for (q = 0; q < n; q++) {
        double qf = (double)q, dd;
        size_t p;
        while (z[k + 1] < qf) {
            k++;
        }
        p = v[k];
        dd = qf - (double)p;
        out[q] = dd * dd + f[p];
    }
}

/* Squared Euclidean distance (in blocks) from every block to the nearest block whose mask equals
 * `site`; about 1e20 when there is none. NULL on NOMEM. */
static double *edt(const bool *mask, bool site, size_t w, size_t h)
{
    const double FAR = 1e20;
    size_t m = w > h ? w : h, i, x, y;
    double *d = pc_malloc_array(w * h, sizeof *d), *f = pc_malloc_array(m, sizeof *f);
    double *out = pc_malloc_array(m, sizeof *out), *z = pc_malloc_array(m + 1, sizeof *z);
    size_t *v = pc_malloc_array(m, sizeof *v);
    if (d == NULL || f == NULL || out == NULL || z == NULL || v == NULL) {
        free(d);
        d = NULL;
        goto done;
    }
    for (i = 0; i < w * h; i++) {
        d[i] = mask[i] == site ? 0.0 : FAR;
    }
    for (x = 0; x < w; x++) {
        for (y = 0; y < h; y++) {
            f[y] = d[y * w + x];
        }
        edt_1d(f, h, out, v, z);
        for (y = 0; y < h; y++) {
            d[y * w + x] = out[y];
        }
    }
    for (y = 0; y < h; y++) {
        memcpy(f, d + y * w, w * sizeof *f);
        edt_1d(f, w, d + y * w, v, z);
    }
done:
    free(f);
    free(out);
    free(z);
    free(v);
    return d;
}

typedef struct cov_ctx {
    const pc_surface *src;
    pc_rect area, bounds;
    int32_t g;
    size_t bw, n;
    bool *cov;
    pc_error *errs;
} cov_ctx;

/* One block row of coverage (rows are independent). */
static pc_status cov_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const cov_ctx *c = ctx;
    pc_status first = PC_OK;
    size_t by;
    (void)chunk;
    for (by = begin; by < end; by++) {
        int32_t y0 = pc_i32_wrapping_add(c->area.y0, pc_i32_wrapping_mul((int32_t)by, c->g));
        pc_rect r = pc_rect_intersect(
            pc_rect_make(c->area.x0, y0,
                         pc_i32_wrapping_add(c->area.x0, pc_i32_wrapping_mul((int32_t)c->bw, c->g)),
                         pc_i32_wrapping_add(y0, c->g)),
            c->bounds);
        float *row = NULL;
        size_t len = 0, i, rw, n = c->n;
        pc_status st;
        if (pc_rect_is_empty(r) || !pc_surface_has_tiles_in(c->src, r)) {
            continue;
        }
        st = pc_surface_read_region(c->src, r, &row, &len, &c->errs[by]);
        if (st != PC_OK) {
            first = first == PC_OK ? st : first;
            continue;
        }
        rw = pc_rect_width(r);
        for (i = 0; n > 0 && i * n + n <= len; i++) {
            if (row[i * n + n - 1] > ALPHA_MIN) {
                int32_t x = r.x0 + (int32_t)(i % rw);
                size_t bx = (size_t)((x - c->area.x0) / c->g);
                if (bx < c->bw) {
                    c->cov[by * c->bw + bx] = true;
                }
            }
        }
        free(row);
    }
    return first;
}

static pc_status field_new(const pc_surface *src, pc_rect bounds, double expansion, field *out, pc_error *err)
{
    uint32_t bwd = pc_rect_width(bounds), bhd = pc_rect_height(bounds);
    double lng = (double)(bwd > bhd ? bwd : bhd), grow = ceil(pc_rs_max_k(expansion, 0.0)), gf;
    int32_t g0 = pc_f64_to_i32(pc_rs_max_k(ceil((lng + 2.0 * grow + 8.0) / FIELD_BLOCKS), 1.0));
    int32_t g = g0 > 1 ? g0 : 1;
    pc_rect area =
        pc_rect_inflate(bounds, pc_i32_wrapping_add(pc_f64_to_i32(grow), pc_i32_wrapping_mul(2, g)));
    int32_t bw_i = pc_i32_wrapping_sub(pc_i32_wrapping_add(pc_u32_as_i32(pc_rect_width(area)), g), 1) / g;
    int32_t bh_i = pc_i32_wrapping_sub(pc_i32_wrapping_add(pc_u32_as_i32(pc_rect_height(area)), g), 1) / g;
    size_t bw = (size_t)(bw_i > 1 ? bw_i : 1), bh = (size_t)(bh_i > 1 ? bh_i : 1), i;
    pc_pixel_format fmt = pc_surface_format(src);
    bool *cov;
    double *outside = NULL, *inside = NULL;
    pc_status st = PC_OK;
    memset(out, 0, sizeof *out);
    /* A huge layer has a coarse block, so the grid stays about FIELD_BLOCKS across; widths past
     * i32 wrap and are refused. */
    if (bw > 4 * (size_t)FIELD_BLOCKS || bh > 4 * (size_t)FIELD_BLOCKS) {
        return pc_fail(err, PC_ERR_LIMIT, "puppet warp: the mesh grid over %ux%u px is too large", bwd, bhd);
    }
    cov = pc_calloc_array(bw * bh, sizeof *cov);
    if (cov == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "puppet warp: out of memory");
    }
    if (!fmt.alpha) {
        for (i = 0; i < bw * bh; i++) {
            int32_t x = pc_i32_wrapping_add(area.x0, pc_i32_wrapping_mul((int32_t)(i % bw), g));
            int32_t y = pc_i32_wrapping_add(area.y0, pc_i32_wrapping_mul((int32_t)(i / bw), g));
            pc_rect r = pc_rect_make(x, y, pc_i32_wrapping_add(x, g), pc_i32_wrapping_add(y, g));
            cov[i] = !pc_rect_is_empty(pc_rect_intersect(r, bounds));
        }
    } else {
        cov_ctx c;
        c.src = src;
        c.area = area;
        c.bounds = bounds;
        c.g = g;
        c.bw = bw;
        c.n = pc_surface_channels(src);
        c.cov = cov;
        c.errs = pc_calloc_array(bh, sizeof *c.errs);
        if (c.errs == NULL) {
            free(cov);
            return pc_fail(err, PC_ERR_NOMEM, "puppet warp: out of memory");
        }
        st = pc_algo_par_for(bh, 1, cov_rows, &c);
        if (st != PC_OK) {
            size_t by;
            for (by = 0; by < bh; by++) {
                if (c.errs[by].code != PC_OK) {
                    if (err != NULL) {
                        *err = c.errs[by];
                    }
                    st = c.errs[by].code;
                    break;
                }
            }
        }
        free(c.errs);
        if (st != PC_OK) {
            free(cov);
            return st;
        }
    }
    outside = edt(cov, true, bw, bh);
    inside = edt(cov, false, bw, bh);
    out->sd = pc_malloc_array(bw * bh, sizeof *out->sd);
    if (outside == NULL || inside == NULL || out->sd == NULL) {
        free(cov);
        free(outside);
        free(inside);
        free(out->sd);
        out->sd = NULL;
        return pc_fail(err, PC_ERR_NOMEM, "puppet warp: out of memory");
    }
    gf = (double)g;
    for (i = 0; i < bw * bh; i++) {
        out->sd[i] = cov[i] ? pc_f64_to_f32(-(sqrt(inside[i]) - 0.5) * gf)
                            : pc_f64_to_f32((sqrt(outside[i]) - 0.5) * gf);
    }
    free(cov);
    free(outside);
    free(inside);
    out->x0 = (double)area.x0;
    out->y0 = (double)area.y0;
    out->g = gf;
    out->bw = bw;
    out->bh = bh;
    out->expansion = expansion;
    return PC_OK;
}

/* The field at a block centre (exact, no interpolation). */
static double field_at(const field *f, size_t bx, size_t by)
{
    size_t i = by * f->bw + bx;
    return i < f->bw * f->bh ? (double)f->sd[i] - f->expansion : DBL_MAX;
}

/* The field at p (bilinear between block centres; clamped to the grid, which ends outside). */
static double field_value(const field *f, const double p[2])
{
    double fx = pc_clampd((p[0] - f->x0) / f->g - 0.5, 0.0, (double)(f->bw - 1));
    double fy = pc_clampd((p[1] - f->y0) / f->g - 0.5, 0.0, (double)(f->bh - 1));
    size_t ix = pc_f64_to_size(floor(fx)), iy = pc_f64_to_size(floor(fy));
    size_t ix1 = ix + 1 < f->bw - 1 ? ix + 1 : f->bw - 1, iy1 = iy + 1 < f->bh - 1 ? iy + 1 : f->bh - 1;
    double tx = fx - (double)ix, ty = fy - (double)iy;
    double top = field_at(f, ix, iy) * (1.0 - tx) + field_at(f, ix1, iy) * tx;
    double bottom = field_at(f, ix, iy1) * (1.0 - tx) + field_at(f, ix1, iy1) * tx;
    return top * (1.0 - ty) + bottom * ty;
}

/* A crossing on a block edge: id 2*block for the edge to the right neighbour, 2*block + 1 for the
 * edge to the one below. */
static void crossing_point(const field *f, size_t id, double out[2])
{
    size_t b = id / 2, x = b % f->bw, y = b / f->bw, x2 = id % 2 == 0 ? x + 1 : x,
           y2 = id % 2 == 0 ? y : y + 1;
    double fa = field_at(f, x, y), fb = field_at(f, x2, y2);
    double t = fabs(fa - fb) > 1e-12 ? pc_clampd(fa / (fa - fb), 0.0, 1.0) : 0.5;
    double dx = (double)(x2 - x), dy = (double)(y2 - y);
    out[0] = f->x0 + ((double)x + 0.5 + dx * t) * f->g;
    out[1] = f->y0 + ((double)y + 0.5 + dy * t) * f->g;
}

typedef struct loops {
    ptvec *v;
    size_t n, cap;
} loops;

static void loops_free(loops *l)
{
    size_t i;
    for (i = 0; i < l->n; i++) {
        free(l->v[i].v);
    }
    free(l->v);
    memset(l, 0, sizeof *l);
}

static bool loops_push(loops *l, ptvec p)
{
    if (l->n == l->cap) {
        void *nv = pc_grow_array(l->v, &l->cap, l->n + 1, sizeof *l->v);
        if (nv == NULL) {
            return false;
        }
        l->v = nv;
    }
    l->v[l->n++] = p;
    return true;
}

static void link_one(size_t (*links)[2], size_t p, size_t q)
{
    if (links[p][0] == NONE) {
        links[p][0] = q;
    } else {
        links[p][1] = q;
    }
}

static void link_pair(size_t (*links)[2], size_t a, size_t b)
{
    link_one(links, a, b);
    link_one(links, b, a);
}

/* The outline (where the field crosses zero) as closed loops, traced by marching squares over the
 * block centres. The field's border blocks lie outside, so every loop closes. */
static pc_status field_outline(const field *f, loops *out)
{
    size_t bw = f->bw, bh = f->bh, nid, x, y, start;
    size_t (*links)[2];
    bool *seen;
    pc_status st = PC_OK;
    memset(out, 0, sizeof *out);
    if (bw < 2 || bh < 2) {
        return PC_OK;
    }
    nid = 2 * bw * bh;
    links = pc_malloc_array(nid, sizeof *links);
    seen = pc_calloc_array(nid, sizeof *seen);
    if (links == NULL || seen == NULL) {
        free(links);
        free(seen);
        return PC_ERR_NOMEM;
    }
    memset(links, 0xff, nid * sizeof *links);
#define IN(xx, yy) (field_at(f, (xx), (yy)) < 0.0)
#define ID(xx, yy, down) (2 * ((yy) * bw + (xx)) + (size_t)(down))
    for (y = 0; y + 1 < bh; y++) {
        for (x = 0; x + 1 < bw; x++) {
            bool c[4] = {IN(x, y), IN(x + 1, y), IN(x + 1, y + 1), IN(x, y + 1)};
            size_t edges[4] = {ID(x, y, 0), ID(x + 1, y, 1), ID(x, y + 1, 0), ID(x, y, 1)};
            bool cross[4] = {c[0] != c[1], c[1] != c[2], c[3] != c[2], c[0] != c[3]};
            size_t ks[4], nk = 0, k;
            if (c[0] == c[1] && c[0] == c[2] && c[0] == c[3]) {
                continue;
            }
            for (k = 0; k < 4; k++) {
                if (cross[k]) {
                    ks[nk++] = k;
                }
            }
            if (nk == 2) {
                link_pair(links, edges[ks[0]], edges[ks[1]]);
            } else if (nk == 4) {
                /* A saddle: the centre decides which diagonal pair is joined. */
                bool centre = field_at(f, x, y) + field_at(f, x + 1, y) + field_at(f, x + 1, y + 1) +
                                  field_at(f, x, y + 1) <
                              0.0;
                if (c[0] == centre) {
                    link_pair(links, edges[0], edges[1]);
                    link_pair(links, edges[2], edges[3]);
                } else {
                    link_pair(links, edges[3], edges[0]);
                    link_pair(links, edges[1], edges[2]);
                }
            }
        }
    }
#undef IN
#undef ID
    for (start = 0; start < nid && st == PC_OK; start++) {
        ptvec lp = {0};
        size_t prev = NONE, cur = start;
        if (links[start][0] == NONE || seen[start]) {
            continue;
        }
        for (;;) {
            double p[2];
            size_t next;
            seen[cur] = true;
            crossing_point(f, cur, p);
            if (!ptvec_push(&lp, p)) {
                st = PC_ERR_NOMEM;
                break;
            }
            next = links[cur][0] != prev ? links[cur][0] : links[cur][1];
            if (next == NONE || seen[next]) {
                break;
            }
            prev = cur;
            cur = next;
        }
        if (st == PC_OK && lp.n >= 3) {
            if (!loops_push(out, lp)) {
                free(lp.v);
                st = PC_ERR_NOMEM;
            }
        } else {
            free(lp.v);
        }
    }
    free(links);
    free(seen);
    if (st != PC_OK) {
        loops_free(out);
    }
    return st;
}

/* ---- outline simplification ---- */

/* Distance from p to the segment a-b. */
static double seg_dist(const double p[2], const double a[2], const double b[2])
{
    double dx = b[0] - a[0], dy = b[1] - a[1], l2 = dx * dx + dy * dy, t, q[2];
    t = l2 > 0.0 ? pc_clampd(((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / l2, 0.0, 1.0) : 0.0;
    q[0] = a[0] + dx * t;
    q[1] = a[1] + dy * t;
    return sqrt(dist2(p, q));
}

/* Simplifies a closed loop (Douglas-Peucker, within tol px), which keeps its corners. */
static bool simplify(const ptvec *lp, double tol, ptvec *out)
{
    size_t n = lp->n, far = 1, i, nk = 0;
    bool *keep;
    seg *stack;
    size_t top = 0;
    memset(out, 0, sizeof *out);
    if (n < 4) {
        for (i = 0; i < n; i++) {
            if (!ptvec_push(out, lp->v[i])) {
                return false;
            }
        }
        return true;
    }
    /* max_by keeps the last of equal maxima. */
    for (i = 2; i < n; i++) {
        if (total_cmp(dist2(lp->v[0], lp->v[i]), dist2(lp->v[0], lp->v[far])) >= 0) {
            far = i;
        }
    }
    keep = pc_calloc_array(n, sizeof *keep);
    stack = pc_malloc_array(n + 2, sizeof *stack);
    if (keep == NULL || stack == NULL) {
        free(keep);
        free(stack);
        return false;
    }
    keep[0] = true;
    keep[far] = true;
    /* Spans (a, b) of the loop; b == n stands for point 0. */
    stack[top].a = 0, stack[top].b = far, top++;
    stack[top].a = far, stack[top].b = n, top++;
    while (top > 0) {
        seg s = stack[--top];
        const double *pa = lp->v[s.a], *pb = lp->v[s.b % n];
        double best = 0.0;
        size_t bi = 0;
        for (i = s.a + 1; i < s.b; i++) {
            double d = seg_dist(lp->v[i], pa, pb);
            if (d > best) {
                best = d;
                bi = i;
            }
        }
        if (best > tol) {
            keep[bi] = true;
            /* At most one span per kept point is ever pending beyond the two starting ones. */
            stack[top].a = s.a, stack[top].b = bi, top++;
            stack[top].a = bi, stack[top].b = s.b, top++;
        }
    }
    for (i = 0; i < n; i++) {
        if (keep[i]) {
            nk++;
        }
    }
    free(stack);
    if (nk >= 3) {
        for (i = 0; i < n; i++) {
            if (keep[i] && !ptvec_push(out, lp->v[i])) {
                free(keep);
                return false;
            }
        }
        free(keep);
        return true;
    }
    free(keep);
    /* A speck thinner than the tolerance: keep a few of its points. */
    {
        size_t step = n / 4 > 1 ? n / 4 : 1;
        for (i = 0; i < n; i += step) {
            if (!ptvec_push(out, lp->v[i])) {
                return false;
            }
        }
    }
    return true;
}

static void lerp(const double a[2], const double b[2], double t, double out[2])
{
    out[0] = a[0] + (b[0] - a[0]) * t;
    out[1] = a[1] + (b[1] - a[1]) * t;
}

/* Splits the sides of a closed loop longer than step into equal parts. */
static bool densify(const ptvec *lp, double step, ptvec *out)
{
    size_t k, i;
    memset(out, 0, sizeof *out);
    for (k = 0; k < lp->n; k++) {
        const double *a = lp->v[k], *b = lp->v[(k + 1) % lp->n];
        size_t parts = pc_f64_to_size(pc_clampd(ceil(sqrt(dist2(a, b)) / step), 1.0, 1e4));
        for (i = 0; i < parts; i++) {
            double p[2];
            lerp(a, b, (double)i / (double)parts, p);
            if (!ptvec_push(out, p)) {
                return false;
            }
        }
    }
    return true;
}

static double orient(const double a[2], const double b[2], const double c[2])
{
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}

/* Positive when d lies inside the circumcircle of the positively oriented a, b, c. */
static double in_circle(const double a[2], const double b[2], const double c[2], const double d[2])
{
    double ax = a[0] - d[0], ay = a[1] - d[1], bx = b[0] - d[0], by = b[1] - d[1], cx = c[0] - d[0],
           cy = c[1] - d[1];
    return (ax * ax + ay * ay) * (bx * cy - cx * by) - (bx * bx + by * by) * (ax * cy - cx * ay) +
           (cx * cx + cy * cy) * (ax * by - bx * ay);
}

/* ---- incremental Delaunay triangulation (Bowyer-Watson) ---- */

/* A triangle: vertices (positively oriented) and the neighbour across the edge opposite each. */
typedef struct dtri {
    size_t v[3], n[3];
    bool alive;
} dtri;

typedef struct rim_edge {
    size_t a, b, o;
} rim_edge;

/* Vertices 0..3 are those of a triangle around the whole area. */
typedef struct delaunay {
    ptvec pts;
    dtri *tris;
    uint32_t *stamp;
    size_t ntris, cap, scap;
    uint32_t pass;
    size_t last;
    szvec bad;
    rim_edge *rim;
    size_t nrim, rimcap;
} delaunay;

static void dl_free(delaunay *d)
{
    free(d->pts.v);
    free(d->tris);
    free(d->stamp);
    free(d->bad.v);
    free(d->rim);
    memset(d, 0, sizeof *d);
}

static bool dl_push_tri(delaunay *d, dtri t)
{
    if (d->ntris == d->cap) {
        void *nv = pc_grow_array(d->tris, &d->cap, d->ntris + 1, sizeof *d->tris);
        if (nv == NULL) {
            return false;
        }
        d->tris = nv;
    }
    if (d->ntris == d->scap) {
        void *nv = pc_grow_array(d->stamp, &d->scap, d->ntris + 1, sizeof *d->stamp);
        if (nv == NULL) {
            return false;
        }
        d->stamp = nv;
    }
    d->tris[d->ntris] = t;
    d->stamp[d->ntris] = 0;
    d->ntris++;
    return true;
}

static bool dl_new(delaunay *d, const double lo[2], const double hi[2])
{
    double cx = 0.5 * (lo[0] + hi[0]), cy = 0.5 * (lo[1] + hi[1]);
    double dd = pc_rs_max_k(pc_rs_max(hi[0] - lo[0], hi[1] - lo[1]), 1.0) * 4.0;
    double p[3][2];
    dtri t = {{0, 1, 2}, {NONE, NONE, NONE}, true};
    int i;
    memset(d, 0, sizeof *d);
    p[0][0] = cx - 2.0 * dd, p[0][1] = cy - dd;
    p[1][0] = cx + 2.0 * dd, p[1][1] = cy - dd;
    p[2][0] = cx, p[2][1] = cy + 2.0 * dd;
    if (orient(p[0], p[1], p[2]) < 0.0) {
        double s0 = p[1][0], s1 = p[1][1];
        p[1][0] = p[2][0], p[1][1] = p[2][1];
        p[2][0] = s0, p[2][1] = s1;
    }
    for (i = 0; i < 3; i++) {
        if (!ptvec_push(&d->pts, p[i])) {
            return false;
        }
    }
    return dl_push_tri(d, t);
}

/* The orientation test of a triangle's edge k (opposite vertex k) against p. */
static double edge_orient(const delaunay *d, const dtri *t, size_t k, const double p[2])
{
    return orient(d->pts.v[t->v[(k + 1) % 3]], d->pts.v[t->v[(k + 2) % 3]], p);
}

/* The triangle holding p: a visibility walk from the last one made. NONE when not found. */
static size_t dl_locate(const delaunay *d, const double p[2])
{
    size_t t, it, k, i;
    if (d->last < d->ntris && d->tris[d->last].alive) {
        t = d->last;
    } else {
        for (t = 0; t < d->ntris && !d->tris[t].alive; t++) {
        }
        if (t == d->ntris) {
            return NONE;
        }
    }
    for (it = 0; it < d->ntris + 3; it++) {
        const dtri *tri;
        if (t >= d->ntris) {
            return NONE;
        }
        tri = &d->tris[t];
        for (k = 0; k < 3 && !(edge_orient(d, tri, k, p) < 0.0); k++) {
        }
        if (k == 3) {
            return t;
        }
        if (tri->n[k] == NONE) {
            return NONE;
        }
        t = tri->n[k];
    }
    for (i = 0; i < d->ntris; i++) {
        const dtri *tri = &d->tris[i];
        if (tri->alive && edge_orient(d, tri, 0, p) >= 0.0 && edge_orient(d, tri, 1, p) >= 0.0 &&
            edge_orient(d, tri, 2, p) >= 0.0) {
            return i;
        }
    }
    return NONE;
}

/* Inserts p; *out receives its vertex index (an existing vertex's when p coincides with it), NONE
 * when p lies outside the triangulation. False on NOMEM. */
static bool dl_insert(delaunay *d, const double p[2], size_t *out)
{
    size_t t0 = dl_locate(d, p), i, k, pi, base;
    uint32_t pass;
    *out = NONE;
    if (t0 == NONE) {
        return true;
    }
    for (k = 0; k < 3; k++) {
        size_t v = d->tris[t0].v[k];
        if (dist2(d->pts.v[v], p) < 1e-12) {
            *out = v;
            return true;
        }
    }
    d->pass++;
    pass = d->pass;
    /* The cavity: every triangle whose circumcircle holds p, grown from t0. */
    d->bad.n = 0;
    if (!szvec_push(&d->bad, t0)) {
        return false;
    }
    d->stamp[t0] = pass;
    for (i = 0; i < d->bad.n; i++) {
        size_t t = d->bad.v[i];
        for (k = 0; k < 3; k++) {
            size_t nb = d->tris[t].n[k];
            const dtri *nt;
            if (nb == NONE || d->stamp[nb] == pass) {
                continue;
            }
            nt = &d->tris[nb];
            if (in_circle(d->pts.v[nt->v[0]], d->pts.v[nt->v[1]], d->pts.v[nt->v[2]], p) > 0.0) {
                d->stamp[nb] = pass;
                if (!szvec_push(&d->bad, nb)) {
                    return false;
                }
            }
        }
    }
    d->nrim = 0;
    for (i = 0; i < d->bad.n; i++) {
        size_t t = d->bad.v[i];
        dtri tri = d->tris[t];
        for (k = 0; k < 3; k++) {
            size_t nb = tri.n[k];
            if (nb == NONE || d->stamp[nb] != pass) {
                if (d->nrim == d->rimcap) {
                    void *nv = pc_grow_array(d->rim, &d->rimcap, d->nrim + 1, sizeof *d->rim);
                    if (nv == NULL) {
                        return false;
                    }
                    d->rim = nv;
                }
                d->rim[d->nrim].a = tri.v[(k + 1) % 3];
                d->rim[d->nrim].b = tri.v[(k + 2) % 3];
                d->rim[d->nrim].o = nb;
                d->nrim++;
            }
        }
        d->tris[t].alive = false;
    }
    pi = d->pts.n;
    if (!ptvec_push(&d->pts, p)) {
        return false;
    }
    base = d->ntris;
    for (i = 0; i < d->nrim; i++) {
        size_t a = d->rim[i].a, b = d->rim[i].b, o = d->rim[i].o;
        dtri nt = {{a, b, pi}, {NONE, NONE, o}, true};
        if (!dl_push_tri(d, nt)) {
            return false;
        }
        if (o != NONE && o < d->ntris) {
            dtri *ot = &d->tris[o];
            for (k = 0; k < 3; k++) {
                size_t x = ot->v[(k + 1) % 3], y = ot->v[(k + 2) % 3];
                if ((x == b && y == a) || (x == a && y == b)) {
                    ot->n[k] = base + i;
                }
            }
        }
    }
    for (i = 0; i < d->nrim; i++) {
        size_t a = d->rim[i].a, b = d->rim[i].b, j, across_b = NONE, across_a = NONE;
        for (j = 0; j < d->nrim; j++) {
            if (d->rim[j].a == b) {
                across_b = base + j;
                break;
            }
        }
        for (j = 0; j < d->nrim; j++) {
            if (d->rim[j].b == a) {
                across_a = base + j;
                break;
            }
        }
        d->tris[base + i].n[0] = across_b;
        d->tris[base + i].n[1] = across_a;
    }
    d->last = base;
    *out = pi;
    return true;
}

static int cmp_seg(const void *pa, const void *pb)
{
    const seg *a = pa, *b = pb;
    if (a->a != b->a) {
        return a->a < b->a ? -1 : 1;
    }
    return a->b < b->b ? -1 : (a->b > b->b ? 1 : 0);
}

/* The triangulation's edges as sorted (min, max) pairs (the reference's hash set). */
static bool dl_edges(const delaunay *d, segvec *e)
{
    size_t t, k;
    e->n = 0;
    for (t = 0; t < d->ntris; t++) {
        if (!d->tris[t].alive) {
            continue;
        }
        for (k = 0; k < 3; k++) {
            size_t a = d->tris[t].v[k], b = d->tris[t].v[(k + 1) % 3];
            if (!segvec_push(e, a < b ? a : b, a < b ? b : a)) {
                return false;
            }
        }
    }
    if (e->n > 0) {
        qsort(e->v, e->n, sizeof *e->v, cmp_seg);
    }
    return true;
}

static bool has_edge(const segvec *e, size_t a, size_t b)
{
    seg key;
    key.a = a < b ? a : b;
    key.b = a < b ? b : a;
    return e->n > 0 && bsearch(&key, e->v, e->n, sizeof *e->v, cmp_seg) != NULL;
}

/* ---- even-odd point-in-polygon test against closed outline segments, bucketed by row ---- */

typedef struct inside_test {
    const pt *pts;
    const seg *segs;
    double y0, row;
    szvec *rows;
    size_t nrows;
} inside_test;

static void inside_free(inside_test *in)
{
    size_t i;
    for (i = 0; i < in->nrows; i++) {
        free(in->rows[i].v);
    }
    free(in->rows);
}

static bool inside_new(inside_test *in, const pt *pts, const seg *segs, size_t nsegs, double row)
{
    double y0 = DBL_MAX, y1 = -DBL_MAX;
    size_t i, r;
    memset(in, 0, sizeof *in);
    for (i = 0; i < nsegs; i++) {
        y0 = pc_rs_min(pc_rs_min(y0, pts[segs[i].a][1]), pts[segs[i].b][1]);
        y1 = pc_rs_max(pc_rs_max(y1, pts[segs[i].a][1]), pts[segs[i].b][1]);
    }
    in->pts = pts;
    in->segs = segs;
    in->y0 = y0;
    in->row = row;
    in->nrows = y1 >= y0 ? pc_f64_to_size(pc_clampd(floor((y1 - y0) / row), 0.0, 1e5)) + 1 : 0;
    if (in->nrows > 0) {
        in->rows = pc_calloc_array(in->nrows, sizeof *in->rows);
        if (in->rows == NULL) {
            in->nrows = 0;
            return false;
        }
    }
    for (i = 0; i < nsegs; i++) {
        double ya = pts[segs[i].a][1], yb = pts[segs[i].b][1];
        double lo = pc_rs_min(ya, yb), hi = pc_rs_max(ya, yb);
        size_t r0 = pc_f64_to_size(floor((lo - y0) / row)), r1 = pc_f64_to_size(floor((hi - y0) / row));
        for (r = r0; r <= r1 && r < in->nrows; r++) {
            if (!szvec_push(&in->rows[r], i)) {
                return false;
            }
        }
    }
    return true;
}

static bool inside_contains(const inside_test *in, const double p[2])
{
    size_t r, k;
    bool odd = false;
    const szvec *row;
    if (in->segs == NULL || in->pts == NULL || p[1] < in->y0) {
        return false;
    }
    r = pc_f64_to_size(floor((p[1] - in->y0) / in->row));
    if (r >= in->nrows) {
        return false;
    }
    row = &in->rows[r];
    for (k = 0; k < row->n; k++) {
        const double *a = in->pts[in->segs[row->v[k]].a], *b = in->pts[in->segs[row->v[k]].b];
        if ((a[1] > p[1]) != (b[1] > p[1]) && p[0] < a[0] + (p[1] - a[1]) / (b[1] - a[1]) * (b[0] - a[0])) {
            odd = !odd;
        }
    }
    return odd;
}

/* ---- vertex numbering (reverse Cuthill-McKee) ---- */

typedef struct order_ctx {
    const pt *pos;
    const szvec *adj;
} order_ctx;

/* Stable merge sort of size_t keys by a comparator (the reference's slice::sort_by is stable). */
static void merge_sort(size_t *a, size_t *tmp, size_t n, int (*cmp)(const order_ctx *, size_t, size_t),
                       const order_ctx *c)
{
    size_t width, i;
    for (width = 1; width < n; width *= 2) {
        for (i = 0; i < n; i += 2 * width) {
            size_t lo = i, mid = i + width < n ? i + width : n, hi = i + 2 * width < n ? i + 2 * width : n;
            size_t l = lo, r = mid, o = lo;
            while (l < mid && r < hi) {
                tmp[o++] = cmp(c, a[r], a[l]) < 0 ? a[r++] : a[l++];
            }
            while (l < mid) {
                tmp[o++] = a[l++];
            }
            while (r < hi) {
                tmp[o++] = a[r++];
            }
        }
        memcpy(a, tmp, n * sizeof *a);
    }
}

static int cmp_pos(const order_ctx *c, size_t a, size_t b)
{
    int r = total_cmp(c->pos[a][1], c->pos[b][1]);
    return r != 0 ? r : total_cmp(c->pos[a][0], c->pos[b][0]);
}

static int cmp_degree(const order_ctx *c, size_t a, size_t b)
{
    return c->adj[a].n < c->adj[b].n ? -1 : (c->adj[a].n > c->adj[b].n ? 1 : 0);
}

static int cmp_size(const void *pa, const void *pb)
{
    size_t a = *(const size_t *)pa, b = *(const size_t *)pb;
    return a < b ? -1 : (a > b ? 1 : 0);
}

/* The vertices tris use, in reverse Cuthill-McKee order (breadth first from the topmost vertex of
 * each piece, lower degrees first), which keeps the solver's banded factor small. */
static bool band_order(const pt *pos, size_t npos, const size_t (*tris)[3], size_t ntris, szvec *order)
{
    szvec *adj = pc_calloc_array(npos > 0 ? npos : 1, sizeof *adj);
    size_t *starts = NULL, *tmp = NULL, *next = NULL, nstarts = 0, i, k, s;
    bool *seen = NULL, ok = false;
    order_ctx c;
    memset(order, 0, sizeof *order);
    if (adj == NULL) {
        return false;
    }
    for (i = 0; i < ntris; i++) {
        for (k = 0; k < 3; k++) {
            size_t a = tris[i][k], b = tris[i][(k + 1) % 3];
            if (!szvec_push(&adj[a], b) || !szvec_push(&adj[b], a)) {
                goto done;
            }
        }
    }
    for (i = 0; i < npos; i++) {
        size_t w = 0;
        if (adj[i].n > 1) {
            qsort(adj[i].v, adj[i].n, sizeof *adj[i].v, cmp_size);
        }
        for (k = 0; k < adj[i].n; k++) {
            if (w == 0 || adj[i].v[w - 1] != adj[i].v[k]) {
                adj[i].v[w++] = adj[i].v[k];
            }
        }
        adj[i].n = w;
    }
    starts = pc_malloc_array(npos > 0 ? npos : 1, sizeof *starts);
    tmp = pc_malloc_array(npos > 0 ? npos : 1, sizeof *tmp);
    next = pc_malloc_array(npos > 0 ? npos : 1, sizeof *next);
    seen = pc_calloc_array(npos > 0 ? npos : 1, sizeof *seen);
    if (starts == NULL || tmp == NULL || next == NULL || seen == NULL) {
        goto done;
    }
    for (i = 0; i < npos; i++) {
        if (adj[i].n > 0) {
            starts[nstarts++] = i;
        }
    }
    c.pos = pos;
    c.adj = adj;
    merge_sort(starts, tmp, nstarts, cmp_pos, &c);
    for (s = 0; s < nstarts; s++) {
        size_t st = starts[s], at;
        if (seen[st]) {
            continue;
        }
        seen[st] = true;
        at = order->n;
        if (!szvec_push(order, st)) {
            goto done;
        }
        while (at < order->n) {
            size_t v = order->v[at++], nn = 0;
            for (k = 0; k < adj[v].n; k++) {
                if (!seen[adj[v].v[k]]) {
                    next[nn++] = adj[v].v[k];
                }
            }
            merge_sort(next, tmp, nn, cmp_degree, &c);
            for (k = 0; k < nn; k++) {
                seen[next[k]] = true;
                if (!szvec_push(order, next[k])) {
                    goto done;
                }
            }
        }
    }
    for (i = 0; i < order->n / 2; i++) {
        size_t t = order->v[i];
        order->v[i] = order->v[order->n - 1 - i];
        order->v[order->n - 1 - i] = t;
    }
    ok = true;
done:
    for (i = 0; i < npos; i++) {
        free(adj[i].v);
    }
    free(adj);
    free(starts);
    free(tmp);
    free(next);
    free(seen);
    if (!ok) {
        free(order->v);
        memset(order, 0, sizeof *order);
    }
    return ok;
}

/* ---- the mesh ---- */

static double density_cells(pc_algo_puppet_density d)
{
    return d == PC_ALGO_PUPPET_FEWER ? 12.0 : (d == PC_ALGO_PUPPET_MORE ? 32.0 : 20.0);
}

/* Outline rings at tolerance tol: each loop simplified then densified to spacing. */
static bool make_rings(const loops *lp, double tol, double spacing, loops *rings, size_t *total)
{
    size_t i;
    memset(rings, 0, sizeof *rings);
    *total = 0;
    for (i = 0; i < lp->n; i++) {
        ptvec simple, dense;
        if (!simplify(&lp->v[i], tol, &simple)) {
            loops_free(rings);
            return false;
        }
        if (!densify(&simple, spacing, &dense)) {
            free(simple.v);
            free(dense.v);
            loops_free(rings);
            return false;
        }
        free(simple.v);
        *total += dense.n;
        if (!loops_push(rings, dense)) {
            free(dense.v);
            loops_free(rings);
            return false;
        }
    }
    return true;
}

pc_status pc_algo_puppet_build_mesh(const pc_surface *src, pc_rect bounds, pc_algo_puppet_density density,
                                    double expansion, pc_algo_puppet_mesh *out, pc_error *err)
{
    pc_algo_puppet_mesh mesh;
    double e, lng, spacing, tol, lo[2], hi[2], rise;
    field fd;
    loops lps = {0}, rings = {0};
    delaunay dl;
    segvec segs = {0}, next = {0}, edges = {0};
    size_t (*tris)[3] = NULL, ntris = 0, tcap = 0, total, i, k, rows, cols, j, budget, pass, *remap = NULL;
    szvec idx = {0}, order = {0};
    inside_test in;
    bool have_in = false;
    pc_status st = PC_OK;
    memset(&mesh, 0, sizeof mesh);
    memset(&dl, 0, sizeof dl);
    if (pc_rect_is_empty(bounds)) {
        *out = mesh;
        return PC_OK;
    }
    e = isfinite(expansion) ? pc_clampd(expansion, -200.0, 200.0) : 0.0;
    /* The density counts steps across the region the mesh covers (grown by the expansion). */
    {
        uint32_t w = pc_rect_width(bounds), h = pc_rect_height(bounds);
        lng = (double)(w > h ? w : h) + 2.0 * pc_rs_max_k(e, 0.0);
    }
    spacing = pc_rs_max_k(lng / density_cells(density), 2.0);
    mesh.spacing = spacing;
    st = field_new(src, bounds, e, &fd, err);
    if (st != PC_OK) {
        return st;
    }
    if (field_outline(&fd, &lps) != PC_OK) {
        goto nomem;
    }
    if (lps.n == 0) {
        goto finish;
    }
    tol = pc_rs_max_k(0.5 * fd.g, 0.5);
    for (;;) {
        if (!make_rings(&lps, tol, spacing, &rings, &total)) {
            goto nomem;
        }
        if (total <= MAX_OUTLINE_POINTS || tol > spacing) {
            break;
        }
        loops_free(&rings);
        tol *= 2.0;
    }
    lo[0] = fd.x0;
    lo[1] = fd.y0;
    hi[0] = fd.x0 + (double)fd.bw * fd.g;
    hi[1] = fd.y0 + (double)fd.bh * fd.g;
    if (!dl_new(&dl, lo, hi)) {
        goto nomem;
    }
    for (i = 0; i < rings.n; i++) {
        idx.n = 0;
        for (k = 0; k < rings.v[i].n; k++) {
            size_t v;
            if (!dl_insert(&dl, rings.v[i].v[k], &v)) {
                goto nomem;
            }
            if (v != NONE && !szvec_push(&idx, v)) {
                goto nomem;
            }
        }
        for (k = 0; k < idx.n; k++) {
            size_t a = idx.v[k], b = idx.v[(k + 1) % idx.n];
            if (a != b && !segvec_push(&segs, a, b)) {
                goto nomem;
            }
        }
    }
    /* The inside: a triangular lattice, kept half a step clear of the outline. */
    rise = spacing * sqrt(3.0) / 2.0;
    rows = pc_f64_to_size(ceil((hi[1] - lo[1]) / rise));
    cols = pc_f64_to_size(ceil((hi[0] - lo[0]) / spacing));
    for (j = 0; j <= rows; j++) {
        for (i = 0; i <= cols; i++) {
            double p[2];
            size_t v;
            p[0] = lo[0] + ((double)i + 0.5 * (double)(j % 2)) * spacing;
            p[1] = lo[1] + (double)j * rise;
            if (field_value(&fd, p) < -0.5 * spacing && !dl_insert(&dl, p, &v)) {
                goto nomem;
            }
        }
    }
    /* Recover the outline's sides: split a side the triangulation lacks at its midpoint. */
    budget = segs.n * 2;
    for (pass = 0; pass < 8; pass++) {
        bool split = false;
        segvec tmp;
        if (!dl_edges(&dl, &edges)) {
            goto nomem;
        }
        next.n = 0;
        for (k = 0; k < segs.n; k++) {
            size_t a = segs.v[k].a, b = segs.v[k].b, c;
            double mid[2];
            if (budget == 0 || has_edge(&edges, a, b)) {
                if (!segvec_push(&next, a, b)) {
                    goto nomem;
                }
                continue;
            }
            lerp(dl.pts.v[a], dl.pts.v[b], 0.5, mid);
            if (!dl_insert(&dl, mid, &c)) {
                goto nomem;
            }
            if (c != NONE && c != a && c != b) {
                if (!segvec_push(&next, a, c) || !segvec_push(&next, c, b)) {
                    goto nomem;
                }
                budget--;
                split = true;
            } else if (!segvec_push(&next, a, b)) {
                goto nomem;
            }
        }
        tmp = segs;
        segs = next;
        next = tmp;
        if (!split) {
            break;
        }
    }
    if (!inside_new(&in, (const pt *)dl.pts.v, segs.v, segs.n, spacing)) {
        have_in = true;
        goto nomem;
    }
    have_in = true;
    for (i = 0; i < dl.ntris; i++) {
        const dtri *t = &dl.tris[i];
        const double *a, *b, *c;
        double centroid[2];
        if (!t->alive || t->v[0] < 3 || t->v[1] < 3 || t->v[2] < 3) {
            continue;
        }
        a = dl.pts.v[t->v[0]];
        b = dl.pts.v[t->v[1]];
        c = dl.pts.v[t->v[2]];
        centroid[0] = (a[0] + b[0] + c[0]) / 3.0;
        centroid[1] = (a[1] + b[1] + c[1]) / 3.0;
        if (orient(a, b, c) > 1e-9 && inside_contains(&in, centroid)) {
            if (ntris == tcap) {
                void *nv = pc_grow_array(tris, &tcap, ntris + 1, sizeof *tris);
                if (nv == NULL) {
                    goto nomem;
                }
                tris = nv;
            }
            memcpy(tris[ntris++], t->v, sizeof t->v);
        }
    }
    if (ntris == 0) {
        goto finish;
    }
    /* Keep the vertices in use, numbered for a narrow band in the solver's matrix. */
    if (!band_order((const pt *)dl.pts.v, dl.pts.n, (const size_t (*)[3])tris, ntris, &order)) {
        goto nomem;
    }
    remap = pc_malloc_array(dl.pts.n, sizeof *remap);
    mesh.verts = pc_malloc_array(order.n > 0 ? order.n : 1, sizeof *mesh.verts);
    if (remap == NULL || mesh.verts == NULL) {
        goto nomem;
    }
    memset(remap, 0xff, dl.pts.n * sizeof *remap);
    for (i = 0; i < order.n; i++) {
        remap[order.v[i]] = i;
        mesh.verts[i][0] = dl.pts.v[order.v[i]][0];
        mesh.verts[i][1] = dl.pts.v[order.v[i]][1];
    }
    mesh.nverts = order.n;
    for (i = 0; i < ntris; i++) {
        for (k = 0; k < 3; k++) {
            tris[i][k] = remap[tris[i][k]];
        }
    }
    mesh.tris = tris;
    mesh.ntris = ntris;
    tris = NULL;
    goto finish;
nomem:
    st = pc_fail(err, PC_ERR_NOMEM, "puppet warp: out of memory");
    pc_algo_puppet_mesh_fini(&mesh);
finish:
    free(fd.sd);
    loops_free(&lps);
    loops_free(&rings);
    dl_free(&dl);
    free(segs.v);
    free(next.v);
    free(edges.v);
    free(idx.v);
    free(order.v);
    free(remap);
    free(tris);
    if (have_in) {
        inside_free(&in);
    }
    if (st != PC_OK) {
        return st;
    }
    if (mesh.ntris == 0) {
        /* The empty mesh keeps the spacing. */
        pc_algo_puppet_mesh_fini(&mesh);
        mesh.spacing = spacing;
    }
    *out = mesh;
    return PC_OK;
}
