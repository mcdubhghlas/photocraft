/* Layer-effect maps, part 1 (port of effects.rs): the Map type, distance transforms (exact
 * Euclidean after Felzenszwalb & Huttenlocher, and the 5 x 5 chamfer Photoshop's strokes follow),
 * the two-box-pass tent blur, contours, noise, and the shadow / glow / satin maps.
 *
 * compose may not use src/algo (layering), so the blur and distance transforms live here, as they
 * do in effects.rs. Numerics follow the reference operation for operation (f32 vs f64, Rust's
 * min / max / clamp, wrapping integer arithmetic where the reference wraps). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "base/rmath.h"
#include "compose/fx_internal.h"

#define PI_F32 3.14159265358979323846f

static pc_status nomem(pc_error *err)
{
    return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
}

static float rad_f32(float deg)
{
    const float k = PI_F32 / 180.0f;
    return deg * k;
}

/* ---- Map ---- */

pc_status pc_fxm_new(size_t w, size_t h, float fill, pc_fxm *out, pc_error *err)
{
    size_t n;
    size_t i;
    float *v = NULL;
    if (!pc_size_mul(w, h, &n) || n > PC_COMPOSE_MAX_PIXELS) {
        return pc_fail(err, PC_ERR_LIMIT, "compose: effect map too large");
    }
    /* Never NULL from here (one spare value for empty maps), so loops need no extra guard. */
    v = pc_malloc_array(n > 0 ? n : 1, sizeof *v);
    if (v == NULL) {
        return nomem(err);
    }
    for (i = 0; i < n; i++) {
        v[i] = fill;
    }
    out->w = w;
    out->h = h;
    out->v = v;
    return PC_OK;
}

pc_status pc_fxm_clone(const pc_fxm *m, pc_fxm *out, pc_error *err)
{
    size_t n = pc_fxm_len(m);
    float *v = NULL;
    if (n > 0) {
        v = pc_malloc_array(n, sizeof *v);
        if (v == NULL) {
            return nomem(err);
        }
        memcpy(v, m->v, n * sizeof *v);
    }
    out->w = m->w;
    out->h = m->h;
    out->v = v;
    return PC_OK;
}

pc_fxm pc_fxm_wrap(size_t w, size_t h, float *v)
{
    pc_fxm m;
    m.w = w;
    m.h = h;
    m.v = v;
    return m;
}

void pc_fxm_free(pc_fxm *m)
{
    if (m != NULL) {
        free(m->v);
        m->v = NULL;
        m->w = 0;
        m->h = 0;
    }
}

float pc_fxm_get(const pc_fxm *m, int64_t x, int64_t y)
{
    if (x < 0 || y < 0 || (uint64_t)x >= m->w || (uint64_t)y >= m->h) {
        return 0.0f;
    }
    return m->v[(size_t)y * m->w + (size_t)x];
}

static float get_or(const pc_fxm *m, int64_t x, int64_t y, float outside)
{
    if (x < 0 || y < 0 || (uint64_t)x >= m->w || (uint64_t)y >= m->h) {
        return outside;
    }
    return m->v[(size_t)y * m->w + (size_t)x];
}

/* reach: a hostile distance (+-1e30, inf, NaN) would overflow the i64 offsets below; no shift
 * beyond 2^30 px can land inside a map, so clamp (non-finite shifts read as none). */
static float reach(float d)
{
    return isfinite(d) ? pc_clampf(d, -1.0e9f, 1.0e9f) : 0.0f;
}

/* Map::shifted: a bilinear shifted copy; reads outside the map give `outside`. */
static pc_status shifted(const pc_fxm *m, float dx, float dy, float outside, pc_fxm *out, pc_error *err)
{
    float cdx = reach(dx);
    float cdy = reach(dy);
    float fx = floorf(cdx);
    float fy = floorf(cdy);
    float ax = cdx - fx;
    float ay = cdy - fy;
    int64_t ifx = pc_f32_to_i64(fx);
    int64_t ify = pc_f32_to_i64(fy);
    pc_fxm o;
    size_t x;
    size_t y;
    pc_status st = pc_fxm_new(m->w, m->h, 0.0f, &o, err);
    if (st != PC_OK) {
        return st;
    }
    for (y = 0; y < m->h; y++) {
        for (x = 0; x < m->w; x++) {
            int64_t sx = (int64_t)x - ifx;
            int64_t sy = (int64_t)y - ify;
            float v00 = get_or(m, sx, sy, outside);
            float v10 = get_or(m, sx - 1, sy, outside);
            float v01 = get_or(m, sx, sy - 1, outside);
            float v11 = get_or(m, sx - 1, sy - 1, outside);
            float top = v00 * (1.0f - ax) + v10 * ax;
            float bot = v01 * (1.0f - ax) + v11 * ax;
            o.v[y * m->w + x] = top * (1.0f - ay) + bot * ay;
        }
    }
    *out = o;
    return PC_OK;
}

static void map_one_minus(pc_fxm *m)
{
    size_t i;
    size_t n = pc_fxm_len(m);
    for (i = 0; i < n; i++) {
        m->v[i] = 1.0f - m->v[i];
    }
}

static void mul_by(pc_fxm *m, const pc_fxm *shape)
{
    size_t i;
    size_t n = pc_fxm_len(m);
    for (i = 0; i < n; i++) {
        m->v[i] *= shape->v[i];
    }
}

/* ---- distance transforms ---- */

typedef struct dt_scratch {
    float *f;
    float *o;
    size_t *v;
    float *z;
    size_t *nr;
} dt_scratch;

/* 1D squared distance transform (Felzenszwalb & Huttenlocher) of f[0..n) into s->o, with the
 * nearest sample in s->nr. */
static void dt1(dt_scratch *s, size_t n)
{
    const float *f = s->f;
    size_t *v = s->v;
    float *z = s->z;
    size_t k = 0;
    size_t q;
    if (n == 0) {
        return;
    }
    v[0] = 0;
    z[0] = -(float)INFINITY;
    z[1] = (float)INFINITY;
    for (q = 1; q < n; q++) {
        for (;;) {
            size_t p = v[k];
            float s1 = (f[q] + (float)(q * q)) - (f[p] + (float)(p * p));
            float sv = s1 / (2.0f * (float)q - 2.0f * (float)p);
            if (sv <= z[k] && k > 0) {
                k--;
                continue;
            }
            if (sv <= z[k]) {
                /* k == 0 and the parabola dominates: replace. */
                v[0] = q;
                z[0] = -(float)INFINITY;
                z[1] = (float)INFINITY;
                break;
            }
            k++;
            v[k] = q;
            z[k] = sv;
            z[k + 1] = (float)INFINITY;
            break;
        }
    }
    k = 0;
    for (q = 0; q < n; q++) {
        size_t p;
        float d;
        while (k + 1 < n && z[k + 1] < (float)q) {
            k++;
        }
        p = v[k];
        d = (float)q - (float)p;
        s->o[q] = d * d + f[p];
        s->nr[q] = p;
    }
}

pc_status pc_fx_edt_nearest(const bool *inside, size_t w, size_t h, float **dist, size_t **nearest,
                            pc_error *err)
{
    const float inf = 1e20f;
    size_t len = w * h;
    size_t n = w > h ? w : h;
    float *g = pc_malloc_array(len > 0 ? len : 1, sizeof *g);
    size_t *near_row = pc_malloc_array(len > 0 ? len : 1, sizeof *near_row);
    size_t *nr_out = pc_malloc_array(len > 0 ? len : 1, sizeof *nr_out);
    dt_scratch s;
    size_t x;
    size_t y;
    size_t i;
    s.f = pc_malloc_array(n + 1, sizeof *s.f);
    s.o = pc_malloc_array(n + 1, sizeof *s.o);
    s.v = pc_malloc_array(n + 1, sizeof *s.v);
    s.z = pc_malloc_array(n + 2, sizeof *s.z);
    s.nr = pc_malloc_array(n + 1, sizeof *s.nr);
    if (g == NULL || near_row == NULL || nr_out == NULL || s.f == NULL || s.o == NULL || s.v == NULL ||
        s.z == NULL || s.nr == NULL) {
        free(g);
        free(near_row);
        free(nr_out);
        free(s.f);
        free(s.o);
        free(s.v);
        free(s.z);
        free(s.nr);
        return nomem(err);
    }
    for (i = 0; i < len; i++) {
        g[i] = inside[i] ? 0.0f : inf;
        near_row[i] = 0;
        nr_out[i] = 0;
    }
    for (x = 0; x < w; x++) {
        for (y = 0; y < h; y++) {
            s.f[y] = g[y * w + x];
        }
        dt1(&s, h);
        for (y = 0; y < h; y++) {
            g[y * w + x] = s.o[y];
            near_row[y * w + x] = s.nr[y];
        }
    }
    for (y = 0; y < h; y++) {
        memcpy(s.f, g + y * w, w * sizeof *s.f);
        dt1(&s, w);
        for (x = 0; x < w; x++) {
            size_t px = s.nr[x];
            g[y * w + x] = sqrtf(s.o[x]);
            nr_out[y * w + x] = near_row[y * w + px] * w + px;
        }
    }
    free(near_row);
    free(s.f);
    free(s.o);
    free(s.v);
    free(s.z);
    free(s.nr);
    *dist = g;
    *nearest = nr_out;
    return PC_OK;
}

/* Chamfer distance transform with a 5 x 5 mask of exact Euclidean step lengths (1, sqrt 2,
 * sqrt 5) from per-pixel start values d (PC_FX_CHAMFER_INF = not a seed), in place: the minimum
 * over seeds of start value + path length. Photoshop's effect distances follow this metric: its
 * stroke corners measure sqrt 5 + 1 at offset (1, 3), where the exact distance is 3.162. */
static void chamfer_from(float *d, size_t *near, size_t w, size_t h)
{
    const float s2 = 1.41421356237309504880f;
    const float s5 = 2.236068f;
    /* Forward-pass neighbours (already visited in raster order); the backward pass mirrors them. */
    static const int8_t fwd[8][2] = {{-1, 0},  {-1, -1}, {0, -1},  {1, -1},
                                     {-1, -2}, {1, -2},  {-2, -1}, {2, -1}};
    const float wt[8] = {1.0f, s2, 1.0f, s2, s5, s5, s5, s5};
    int64_t wi = (int64_t)w;
    int64_t hi = (int64_t)h;
    int64_t x;
    int64_t y;
    int pass;
    size_t i;
    for (i = 0; i < w * h; i++) {
        near[i] = i;
    }
    for (pass = 0; pass < 2; pass++) {
        int64_t sign = pass == 0 ? 1 : -1;
        int64_t yy;
        for (yy = 0; yy < hi; yy++) {
            int64_t xx;
            y = pass == 0 ? yy : hi - 1 - yy;
            for (xx = 0; xx < wi; xx++) {
                size_t idx;
                float best;
                size_t bn;
                int k;
                x = pass == 0 ? xx : wi - 1 - xx;
                idx = (size_t)(y * wi + x);
                best = d[idx];
                bn = near[idx];
                for (k = 0; k < 8; k++) {
                    int64_t nx = x + fwd[k][0] * sign;
                    int64_t ny = y + fwd[k][1] * sign;
                    size_t j;
                    float c;
                    if (nx < 0 || ny < 0 || nx >= wi || ny >= hi) {
                        continue;
                    }
                    j = (size_t)(ny * wi + nx);
                    c = d[j] + wt[k];
                    if (c < best) {
                        best = c;
                        bn = near[j];
                    }
                }
                d[idx] = best;
                near[idx] = bn;
            }
        }
    }
}

static pc_status chamfer_run(float *d, size_t w, size_t h, pc_error *err)
{
    size_t *near = pc_malloc_array(w * h > 0 ? w * h : 1, sizeof *near);
    if (near == NULL) {
        return nomem(err);
    }
    chamfer_from(d, near, w, h);
    free(near);
    return PC_OK;
}

pc_status pc_fx_local_coverage(const pc_fxm *s, pc_fxm *out, pc_error *err)
{
    pc_fxm o;
    int64_t x;
    int64_t y;
    pc_status st = pc_fxm_clone(s, &o, err);
    if (st != PC_OK) {
        return st;
    }
    for (y = 0; y < (int64_t)s->h; y++) {
        for (x = 0; x < (int64_t)s->w; x++) {
            float a = pc_fxm_get(s, x, y);
            float mx = a;
            int dy;
            int dx;
            if (a <= PC_FX_INSIDE_EPS) {
                continue;
            }
            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    mx = pc_rs_maxf(mx, pc_fxm_get(s, x + dx, y + dy));
                }
            }
            o.v[(size_t)y * s->w + (size_t)x] = pc_rs_minf_k(a / mx, 1.0f);
        }
    }
    *out = o;
    return PC_OK;
}

pc_status pc_fx_dist_outside(const pc_fxm *s, pc_fx_metric metric, float **out, pc_error *err)
{
    size_t n = pc_fxm_len(s);
    size_t i;
    float *d;
    if (metric == PC_FX_CHAMFER) {
        pc_status st;
        d = pc_malloc_array(n > 0 ? n : 1, sizeof *d);
        if (d == NULL) {
            return nomem(err);
        }
        /* Seeds start at their sub-pixel edge offset (1 - coverage). */
        for (i = 0; i < n; i++) {
            float a = s->v[i];
            d[i] = a > PC_FX_INSIDE_EPS ? 1.0f - pc_rs_minf_k(a, 1.0f) : PC_FX_CHAMFER_INF;
        }
        st = chamfer_run(d, s->w, s->h, err);
        if (st != PC_OK) {
            free(d);
            return st;
        }
        for (i = 0; i < n; i++) {
            d[i] = s->v[i] > PC_FX_INSIDE_EPS ? -0.5f : d[i] - 0.5f;
        }
    } else {
        bool *inside = pc_malloc_array(n > 0 ? n : 1, sizeof *inside);
        size_t *near = NULL;
        pc_status st;
        if (inside == NULL) {
            return nomem(err);
        }
        for (i = 0; i < n; i++) {
            inside[i] = s->v[i] > PC_FX_INSIDE_EPS;
        }
        st = pc_fx_edt_nearest(inside, s->w, s->h, &d, &near, err);
        if (st != PC_OK) {
            free(inside);
            return st;
        }
        for (i = 0; i < n; i++) {
            d[i] = inside[i] ? -0.5f : d[i] - 0.5f + (1.0f - pc_rs_minf_k(s->v[near[i]], 1.0f));
        }
        free(inside);
        free(near);
    }
    *out = d;
    return PC_OK;
}

pc_status pc_fx_dist_inside(const pc_fxm *s, pc_fx_metric metric, float **out, pc_error *err)
{
    size_t n = pc_fxm_len(s);
    size_t i;
    float *d;
    pc_status st;
    if (metric == PC_FX_CHAMFER) {
        /* Partly covered edge pixels (alpha below their neighbourhood's) are seeds starting at
         * their coverage, mirroring dist_outside: a 25 % edge column puts the edge 0.25 px in. */
        pc_fxm cov;
        st = pc_fx_local_coverage(s, &cov, err);
        if (st != PC_OK) {
            return st;
        }
        d = pc_malloc_array(n > 0 ? n : 1, sizeof *d);
        if (d == NULL) {
            pc_fxm_free(&cov);
            return nomem(err);
        }
        for (i = 0; i < n; i++) {
            float a = s->v[i];
            float c = cov.v[i];
            if (a <= PC_FX_INSIDE_EPS) {
                d[i] = 0.0f;
            } else if (c < 1.0f - PC_FX_INSIDE_EPS) {
                d[i] = c;
            } else {
                d[i] = PC_FX_CHAMFER_INF;
            }
        }
        pc_fxm_free(&cov);
        st = chamfer_run(d, s->w, s->h, err);
        if (st != PC_OK) {
            free(d);
            return st;
        }
        for (i = 0; i < n; i++) {
            d[i] = s->v[i] <= PC_FX_INSIDE_EPS ? -0.5f : d[i] - 0.5f;
        }
    } else {
        bool *outside = pc_malloc_array(n > 0 ? n : 1, sizeof *outside);
        size_t *near = NULL;
        if (outside == NULL) {
            return nomem(err);
        }
        for (i = 0; i < n; i++) {
            outside[i] = s->v[i] <= PC_FX_INSIDE_EPS;
        }
        st = pc_fx_edt_nearest(outside, s->w, s->h, &d, &near, err);
        if (st != PC_OK) {
            free(outside);
            return st;
        }
        for (i = 0; i < n; i++) {
            d[i] = outside[i] ? -0.5f : d[i] - 0.5f;
        }
        free(outside);
        free(near);
    }
    *out = d;
    return PC_OK;
}

pc_status pc_compose_fx_distance_field(pc_compose_fx_field_kind kind, const float *alpha, size_t w, size_t h,
                                       float reach, float **out, pc_error *err)
{
    size_t n;
    pc_fxm s;
    pc_fxm t = {0, 0, NULL};
    float *d = NULL;
    pc_status st;
    if (!pc_size_mul(w, h, &n) || n > PC_COMPOSE_MAX_PIXELS) {
        return pc_fail(err, PC_ERR_LIMIT, "compose: distance field too large");
    }
    if (n > 0 && alpha == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "compose: distance field without alpha");
    }
    s = pc_fxm_wrap(w, h, (float *)(uintptr_t)alpha); /* only read below */
    switch (kind) {
    case PC_COMPOSE_FIELD_OUTSIDE:
        st = pc_fx_dist_outside(&s, PC_FX_EUCLIDEAN, &d, err);
        break;
    case PC_COMPOSE_FIELD_INSIDE:
        st = pc_fx_dist_inside(&s, PC_FX_EUCLIDEAN, &d, err);
        break;
    case PC_COMPOSE_FIELD_BEVEL_OUTSIDE:
    case PC_COMPOSE_FIELD_BEVEL_INSIDE: {
        pc_fx_sharp_edges sharp;
        st = pc_fx_sharp_edges_find(&s, &sharp, err);
        if (st == PC_OK) {
            st = kind == PC_COMPOSE_FIELD_BEVEL_INSIDE ? pc_fx_bevel_dist_inside(&s, &sharp, reach, &d, err)
                                                       : pc_fx_bevel_dist_outside(&s, &sharp, reach, &d, err);
            pc_fx_sharp_edges_fini(&sharp);
        }
        break;
    }
    case PC_COMPOSE_FIELD_STROKE_OUTSIDE:
        st = pc_fx_dist_outside(&s, PC_FX_CHAMFER, &d, err);
        break;
    case PC_COMPOSE_FIELD_CHOKE_INSIDE:
        st = pc_fxm_clone(&s, &t, err);
        if (st == PC_OK) {
            map_one_minus(&t);
            st = pc_fx_dist_outside(&t, PC_FX_CHAMFER, &d, err);
        }
        break;
    case PC_COMPOSE_FIELD_STROKE_INSIDE:
        st = pc_fx_dist_inside(&s, PC_FX_CHAMFER, &d, err);
        break;
    case PC_COMPOSE_FIELD_STROKE_OUTSIDE_VECTOR:
        st = pc_fx_local_coverage(&s, &t, err);
        if (st == PC_OK) {
            st = pc_fx_dist_outside(&t, PC_FX_CHAMFER, &d, err);
        }
        break;
    default:
        st = pc_fail(err, PC_ERR_INVALID, "compose: unknown distance field kind");
        break;
    }
    pc_fxm_free(&t);
    if (st != PC_OK) {
        return st;
    }
    if (n == 0) {
        free(d);
        d = NULL;
    }
    *out = d;
    return PC_OK;
}

/* ---- tent blur ---- */

/* A box width as the blurs use it: 1..MAX_REACH, 1 for NaN (#1543: an unbounded width sizes the
 * kernel and its buffers). */
static float box_width(float w)
{
    return isnan(w) ? 1.0f : pc_clampf(w, 1.0f, PC_COMPOSE_FX_MAX_REACH);
}

/* Box geometry for a (fractional) width: r, end-tap weight f and 1 / width. */
static void box_geom(float bw_in, int64_t *r, double *f, double *norm)
{
    float bw = box_width(bw_in);
    float half = bw / 2.0f;
    int64_t rr = pc_f32_to_i64(pc_rs_maxf_k(ceilf(half - 0.5f), 0.0f));
    *r = rr;
    *f = (double)pc_clampf(half - ((float)rr - 0.5f), 0.0f, 1.0f);
    *norm = 1.0 / (double)bw;
}

pc_status pc_compose_fx_tent_kernel(float w, int32_t *radius, float **weights, size_t *n, pc_error *err)
{
    float bwid = box_width(w);
    float half = bwid / 2.0f;
    int64_t r = pc_f32_to_i64(pc_rs_maxf_k(ceilf(half - 0.5f), 0.0f));
    size_t nb = (size_t)(2 * r + 1);
    size_t nt = nb * 2 - 1;
    float *b = pc_malloc_array(nb, sizeof *b);
    float *t = pc_calloc_array(nt, sizeof *t);
    float sum = -0.0f;
    size_t i;
    size_t j;
    if (b == NULL || t == NULL) {
        free(b);
        free(t);
        return nomem(err);
    }
    for (i = 0; i < nb; i++) {
        float fi = (float)((int64_t)i - r);
        b[i] = pc_rs_maxf_k(pc_rs_minf(fi + 0.5f, half) - pc_rs_maxf(fi - 0.5f, -half), 0.0f);
        sum += b[i];
    }
    for (i = 0; i < nb; i++) {
        b[i] = b[i] / sum;
    }
    for (i = 0; i < nb; i++) {
        for (j = 0; j < nb; j++) {
            t[i + j] += b[i] * b[j];
        }
    }
    free(b);
    *radius = (int32_t)((nt - 1) / 2);
    *weights = t;
    *n = nt;
    return PC_OK;
}

/* One box pass over src[0..n) (zero outside) evaluated at x0 .. x0 + m, as a running sum. */
static void box_line(const double *src, size_t n, int64_t x0, double *dst, size_t m, int64_t r, double f,
                     double norm)
{
    int64_t ni = (int64_t)n;
    size_t k;
    double inner = -0.0;
    int64_t i;
#define AT(ix) ((ix) >= 0 && (ix) < ni ? src[(size_t)(ix)] : 0.0)
    if (r == 0) {
        for (k = 0; k < m; k++) {
            dst[k] = AT(x0 + (int64_t)k);
        }
        return;
    }
    for (i = x0 - (r - 1); i <= x0 + (r - 1); i++) {
        inner += AT(i);
    }
    for (k = 0; k < m; k++) {
        int64_t x = x0 + (int64_t)k;
        dst[k] = (inner + f * (AT(x - r) + AT(x + r))) * norm;
        inner += AT(x + r) - AT(x - r + 1);
    }
#undef AT
}

typedef struct tent_job {
    const float *src;
    float *dst;
    size_t w; /* row length */
    float bw;
    int64_t r;
    double f;
    double norm;
} tent_job;

/* Two box passes along each row of a chunk: exactly the convolution with the tent kernel. */
static pc_status tent_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    tent_job *j = ctx;
    size_t n = j->w;
    size_t mlen = n + 2 * (size_t)j->r;
    double *s = pc_malloc_array(n + mlen + n, sizeof *s);
    double *mid;
    double *o;
    size_t row;
    (void)chunk;
    if (s == NULL) {
        return PC_ERR_NOMEM;
    }
    mid = s + n;
    o = mid + mlen;
    for (row = begin; row < end; row++) {
        const float *src = j->src + row * n;
        float *dst = j->dst + row * n;
        size_t k;
        for (k = 0; k < n; k++) {
            s[k] = (double)src[k];
        }
        box_line(s, n, -j->r, mid, mlen, j->r, j->f, j->norm);
        /* mid[k] holds position k - r: output position p reads index p + r. */
        box_line(mid, mlen, j->r, o, n, j->r, j->f, j->norm);
        for (k = 0; k < n; k++) {
            dst[k] = (float)o[k];
        }
    }
    free(s);
    return PC_OK;
}

/* Rows of a w-wide buffer through tent_rows, in parallel on large maps. A map built inside a
 * render tile runs serially (pc_pool_for nested in a chunk does), so nothing waits on a pool
 * the caller is part of. */
static pc_status rows_par(tent_job *j, size_t rows, pc_error *err)
{
    pc_status st;
    size_t grain;
    if (j->w == 0 || rows == 0) {
        return PC_OK;
    }
    grain = ((size_t)1 << 14) / j->w + 1;
    if (rows * j->w < ((size_t)1 << 16)) {
        st = tent_rows(j, 0, 0, rows);
    } else {
        st = pc_pool_for(pc_pool_global(), rows, grain, tent_rows, j);
    }
    if (st != PC_OK) {
        return nomem(err);
    }
    return PC_OK;
}

static void transpose(const float *v, float *out, size_t w, size_t h)
{
    const size_t blk = 32;
    size_t by;
    size_t bx;
    for (by = 0; by < h; by += blk) {
        for (bx = 0; bx < w; bx += blk) {
            size_t ye = by + blk < h ? by + blk : h;
            size_t xe = bx + blk < w ? bx + blk : w;
            size_t y;
            size_t x;
            for (y = by; y < ye; y++) {
                for (x = bx; x < xe; x++) {
                    out[x * h + y] = v[y * w + x];
                }
            }
        }
    }
}

pc_status pc_fxm_tent(pc_fxm *m, float w, pc_error *err)
{
    size_t n = pc_fxm_len(m);
    tent_job j;
    float *a;
    float *t;
    pc_status st;
    box_geom(w, &j.r, &j.f, &j.norm);
    if (j.r == 0 || n == 0) {
        return PC_OK;
    }
    j.bw = w;
    a = pc_malloc_array(n, sizeof *a);
    t = pc_malloc_array(n, sizeof *t);
    if (a == NULL || t == NULL) {
        free(a);
        free(t);
        return nomem(err);
    }
    j.src = m->v;
    j.dst = a;
    j.w = m->w;
    st = rows_par(&j, m->h, err);
    if (st == PC_OK) {
        transpose(a, t, m->w, m->h);
        j.src = t;
        j.dst = a;
        j.w = m->h;
        st = rows_par(&j, m->w, err);
    }
    if (st == PC_OK) {
        transpose(a, m->v, m->h, m->w);
    }
    free(a);
    free(t);
    return st;
}

/* The soft falloff of an effect of `size` pixels: two box passes of width size (a tent). */
static pc_status blur(pc_fxm *m, float size, pc_error *err)
{
    if (isfinite(size)) {
        return pc_fxm_tent(m, size, err);
    }
    return PC_OK;
}

/* Grows the shape by r pixels (anti-aliased) with the chamfer metric, keeping the soft edge. */
pc_status pc_fx_dilate(const pc_fxm *s, float r, pc_fxm *out, pc_error *err)
{
    float *d = NULL;
    pc_fxm o;
    size_t i;
    size_t n = pc_fxm_len(s);
    pc_status st;
    if (r <= 0.0f) {
        return pc_fxm_clone(s, out, err);
    }
    st = pc_fx_dist_outside(s, PC_FX_CHAMFER, &d, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_fxm_clone(s, &o, err);
    if (st != PC_OK) {
        free(d);
        return st;
    }
    for (i = 0; i < n; i++) {
        float v = o.v[i];
        /* Partly covered pixels (distance sentinel < 0) grow by r from their own coverage, so a 2 %
         * spread doesn't make every anti-aliased edge pixel opaque. */
        o.v[i] =
            d[i] < 0.0f ? pc_rs_minf_k(v + r, 1.0f) : pc_rs_maxf(v, pc_clampf(r + 0.5f - d[i], 0.0f, 1.0f));
    }
    free(d);
    *out = o;
    return PC_OK;
}

/* ---- contours ---- */

pc_status pc_compose_fx_contour_lut(const pc_contour *c, float **out, pc_error *err)
{
    float *t;
    pc_status st;
    if (!c->custom) {
        *out = NULL;
        return PC_OK;
    }
    t = pc_malloc_array(PC_COMPOSE_LUT_SIZE, sizeof *t);
    if (t == NULL) {
        return nomem(err);
    }
    /* Corner points bend the curve sharply (each run between corners is its own spline). */
    st = pc_compose_contour_curve_lut(c->points.pts, c->points.len, c->corners, c->ncorners, t, err);
    if (st != PC_OK) {
        free(t);
        return st;
    }
    *out = t;
    return PC_OK;
}

pc_status pc_compose_fx_satin_lut(const pc_contour *c, float **out, pc_error *err)
{
    float *l = NULL;
    float *t;
    float end;
    size_t k;
    const size_t n = PC_COMPOSE_FX_LUT_SIZE;
    pc_status st = pc_compose_fx_contour_lut(c, &l, err);
    if (st != PC_OK || l == NULL) {
        if (st == PC_OK) {
            *out = NULL;
        }
        return st;
    }
    t = pc_malloc_array(n, sizeof *t);
    if (t == NULL) {
        free(l);
        return nomem(err);
    }
    end = l[n - 1];
    for (k = 0; k < n; k++) {
        t[k] = fabsf(l[n - 1 - k] - end);
    }
    free(l);
    *out = t;
    return PC_OK;
}

pc_status pc_compose_fx_bevel_contour_lut(const pc_bevel_contour *c, float **out, pc_error *err)
{
    float *l = NULL;
    size_t k;
    pc_status st = pc_compose_fx_contour_lut(&c->contour, &l, err);
    if (st != PC_OK) {
        return st;
    }
    if (l == NULL) {
        l = pc_malloc_array(PC_COMPOSE_FX_LUT_SIZE, sizeof *l);
        if (l == NULL) {
            return nomem(err);
        }
        for (k = 0; k < PC_COMPOSE_FX_LUT_SIZE; k++) {
            l[k] = (float)k / 4095.0f;
        }
    }
    *out = l;
    return PC_OK;
}

float pc_compose_fx_lut_at(const float *l, size_t n, float v)
{
    float x;
    size_t i;
    size_t j;
    if (n == 0) {
        return v;
    }
    x = pc_clampf(v, 0.0f, 1.0f) * (float)(n - 1);
    i = pc_f32_to_size(floorf(x));
    if (i > n - 1) {
        i = n - 1;
    }
    j = i + 1 < n - 1 ? i + 1 : n - 1;
    return l[i] + (l[j] - l[i]) * (x - (float)i);
}

pc_status pc_compose_fx_ranged_lut(const pc_contour *c, float range, float **out, pc_error *err)
{
    const size_t n = PC_COMPOSE_FX_LUT_SIZE;
    float *base = NULL;
    float *t;
    float r = isfinite(range) ? pc_clampf(range, 0.01f, 1.0f) : 1.0f;
    size_t k;
    pc_status st = pc_compose_fx_contour_lut(c, &base, err);
    if (st != PC_OK) {
        return st;
    }
    if (r >= 0.999f) {
        *out = base;
        return PC_OK;
    }
    t = pc_malloc_array(n, sizeof *t);
    if (t == NULL) {
        free(base);
        return nomem(err);
    }
    for (k = 0; k < n; k++) {
        float v = pc_rs_minf_k((float)k / (float)(n - 1) / r, 1.0f);
        t[k] = base != NULL ? pc_compose_fx_lut_at(base, n, v) : v;
    }
    free(base);
    *out = t;
    return PC_OK;
}

pc_status pc_compose_fx_glow_lut(const pc_glow *g, float **out, pc_error *err)
{
    return pc_compose_fx_ranged_lut(&g->contour, g->range, out, err);
}

void pc_fxm_apply_lut(pc_fxm *m, const float *lut, size_t n)
{
    size_t i;
    size_t len = pc_fxm_len(m);
    if (lut == NULL) {
        return;
    }
    for (i = 0; i < len; i++) {
        m->v[i] = pc_compose_fx_lut_at(lut, n, m->v[i]);
    }
}

static pc_status apply_contour(pc_fxm *m, const pc_contour *c, pc_error *err)
{
    float *l = NULL;
    pc_status st = pc_compose_fx_contour_lut(c, &l, err);
    if (st != PC_OK) {
        return st;
    }
    pc_fxm_apply_lut(m, l, PC_COMPOSE_FX_LUT_SIZE);
    free(l);
    return PC_OK;
}

/* Satin's coverage transfer (pc_compose_fx_satin_lut) in place. */
static pc_status apply_satin_contour(pc_fxm *m, const pc_contour *c, pc_error *err)
{
    float *l = NULL;
    pc_status st = pc_compose_fx_satin_lut(c, &l, err);
    if (st != PC_OK) {
        return st;
    }
    pc_fxm_apply_lut(m, l, PC_COMPOSE_FX_LUT_SIZE);
    free(l);
    return PC_OK;
}

/* ---- noise, offsets ---- */

float pc_compose_fx_hash_noise(int64_t x, int64_t y)
{
    uint32_t h = ((uint32_t)(uint64_t)x * 0x8da6b343u) ^ ((uint32_t)(uint64_t)y * 0xd8163841u) ^ 0x9e3779b9u;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    return (float)(h & 0xffffu) / 65536.0f;
}

/* Effect noise (Photoshop's Noise slider): monochrome speckle in the coverage after the contour. */
static void noise(pc_fxm *m, float amount, int32_t x0, int32_t y0)
{
    size_t x;
    size_t y;
    if (!isfinite(amount) || amount <= 0.0f || m->v == NULL) {
        return;
    }
    for (y = 0; y < m->h; y++) {
        for (x = 0; x < m->w; x++) {
            float *v = &m->v[y * m->w + x];
            float nv;
            if (*v <= 0.0f) {
                continue;
            }
            nv = pc_compose_fx_hash_noise((int64_t)pc_i32_wrapping_add(x0, (int32_t)x),
                                          (int64_t)pc_i32_wrapping_add(y0, (int32_t)y));
            *v = pc_clampf(*v + (nv - 0.5f) * 2.0f * amount, 0.0f, 1.0f);
        }
    }
}

void pc_fx_offset(float angle, float distance, float *dx, float *dy)
{
    float a = rad_f32(angle);
    *dx = roundf(-cosf(a) * distance);
    *dy = roundf(sinf(a) * distance);
}

void pc_compose_fx_spread_split(float size, float spread, float *radius, float *blur_w)
{
    *radius = roundf(size * spread);
    *blur_w = size * (1.0f - spread);
}

float pc_compose_fx_glow_gradient_gain(float range)
{
    float r = isfinite(range) ? pc_clampf(range, 0.01f, 1.0f) : 1.0f;
    return 1.0f / (r * r);
}

/* ---- shadow, glow, satin ---- */

pc_status pc_fx_shadow_map(const pc_fxm *shape, const pc_shadow *s, const pc_global_light *light, bool inner,
                           int32_t ox, int32_t oy, pc_fxm *out, pc_error *err)
{
    float angle = s->use_global_light ? light->angle : s->angle;
    float dx;
    float dy;
    float r;
    float bw;
    pc_fxm src = {0, 0, NULL};
    pc_fxm m = {0, 0, NULL};
    pc_fxm g = {0, 0, NULL};
    pc_status st;
    pc_fx_offset(angle, s->distance, &dx, &dy);
    st = pc_fxm_clone(shape, &src, err);
    if (st != PC_OK) {
        return st;
    }
    if (inner) {
        map_one_minus(&src);
    }
    st = shifted(&src, dx, dy, inner ? 1.0f : 0.0f, &m, err);
    pc_fxm_free(&src);
    if (st != PC_OK) {
        return st;
    }
    pc_compose_fx_spread_split(s->size, s->spread, &r, &bw);
    st = pc_fx_dilate(&m, r, &g, err);
    pc_fxm_free(&m);
    if (st == PC_OK) {
        st = blur(&g, bw, err);
    }
    if (st == PC_OK) {
        st = apply_contour(&g, &s->contour, err);
    }
    if (st != PC_OK) {
        pc_fxm_free(&g);
        return st;
    }
    noise(&g, s->noise, ox, oy);
    if (inner) {
        mul_by(&g, shape);
    }
    *out = g;
    return PC_OK;
}

pc_status pc_fx_glow_map(const pc_fxm *shape, const pc_glow *g, bool inner, int32_t ox, int32_t oy,
                         pc_fxm *out, pc_error *err)
{
    bool center = inner && g->source == PC_GLOW_SOURCE_CENTER;
    pc_fxm m = {0, 0, NULL};
    float *lut = NULL;
    size_t n = pc_fxm_len(shape);
    size_t i;
    pc_status st;
    /* An inner glow is built from the edge inwards; a centre-source glow is its inverse, taken
     * after the contour and Range (Photoshop 27.11: the centre glow is exactly 1 - the edge glow,
     * for both techniques). */
    if (g->technique == PC_GLOW_PRECISE) {
        /* Coverage falls linearly from 0.5 at the edge to 0 at size + 1 px (centre to centre),
         * solid within the spread / choke: after the default 50 % Range that is Photoshop's
         * 1 - D / (size + 1) beyond the solid part. The distances are 0.5 px short of centre to
         * centre. */
        float *d = NULL;
        float solid = g->size * g->spread;
        float soft = pc_rs_maxf_k(g->size + 1.0f - solid, 1e-3f);
        st = inner ? pc_fx_dist_inside(shape, PC_FX_EUCLIDEAN, &d, err)
                   : pc_fx_dist_outside(shape, PC_FX_EUCLIDEAN, &d, err);
        if (st != PC_OK) {
            return st;
        }
        st = pc_fxm_new(shape->w, shape->h, 0.0f, &m, err);
        if (st != PC_OK) {
            free(d);
            return st;
        }
        for (i = 0; i < n; i++) {
            float dd = d[i] + 0.5f;
            m.v[i] = dd <= solid ? 1.0f : 0.5f * pc_clampf(1.0f - (dd - solid) / soft, 0.0f, 1.0f);
        }
        free(d);
    } else {
        float r;
        float bw;
        pc_fxm src = {0, 0, NULL};
        pc_compose_fx_spread_split(g->size, g->spread, &r, &bw);
        st = pc_fxm_clone(shape, &src, err);
        if (st != PC_OK) {
            return st;
        }
        /* Inner glows grow 1 - alpha. */
        if (inner) {
            map_one_minus(&src);
        }
        st = pc_fx_dilate(&src, r, &m, err);
        pc_fxm_free(&src);
        if (st == PC_OK) {
            st = blur(&m, bw, err);
        }
        if (st != PC_OK) {
            pc_fxm_free(&m);
            return st;
        }
    }
    st = pc_compose_fx_glow_lut(g, &lut, err);
    if (st != PC_OK) {
        pc_fxm_free(&m);
        return st;
    }
    pc_fxm_apply_lut(&m, lut, PC_COMPOSE_FX_LUT_SIZE);
    free(lut);
    if (center) {
        map_one_minus(&m);
    }
    noise(&m, g->noise, ox, oy);
    if (inner) {
        mul_by(&m, shape);
    }
    *out = m;
    return PC_OK;
}

pc_status pc_fx_satin_map(const pc_fxm *shape, const pc_satin *s, pc_fxm *out, pc_error *err)
{
    float dx;
    float dy;
    pc_fxm a = {0, 0, NULL};
    pc_fxm b = {0, 0, NULL};
    size_t i;
    size_t n = pc_fxm_len(shape);
    pc_status st;
    pc_fx_offset(s->angle, pc_compose_fx_satin_distance(s), &dx, &dy);
    st = shifted(shape, dx, dy, 0.0f, &a, err);
    if (st == PC_OK) {
        st = shifted(shape, -dx, -dy, 0.0f, &b, err);
    }
    /* Two box passes of the satin size (fitted: 0.2 % mean error on psd-tools layer_effects). */
    if (st == PC_OK) {
        st = pc_fxm_tent(&a, s->size, err);
    }
    if (st == PC_OK) {
        st = pc_fxm_tent(&b, s->size, err);
    }
    if (st == PC_OK) {
        for (i = 0; i < n; i++) {
            a.v[i] = fabsf(a.v[i] - b.v[i]);
        }
        if (s->invert) {
            map_one_minus(&a);
        }
        st = apply_satin_contour(&a, &s->contour, err);
    }
    pc_fxm_free(&b);
    if (st != PC_OK) {
        pc_fxm_free(&a);
        return st;
    }
    mul_by(&a, shape);
    *out = a;
    return PC_OK;
}
