/* Corners, steered BRIEF, matching and RANSAC (port of crates/algo/src/features.rs). */
#include "algo/features.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/features_ransac.h"
#include "algo/fxutil.h"
#include "algo/photo_util.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

#define PR PC_ALGO_FEATURES_PATCH_RADIUS

static pc_status check_plane(size_t w, size_t h, size_t *n, pc_error *err)
{
    if (!pc_size_mul(w, h, n) || *n > PC_MAX_ALLOC / sizeof(float) / 4) {
        return pc_fail(err, PC_ERR_LIMIT, "image of %zux%zu is too large for feature detection", w, h);
    }
    return PC_OK;
}

/* f32::total_cmp as an integer key. */
static int32_t total_key(float f)
{
    uint32_t u;
    int32_t i;
    memcpy(&u, &f, sizeof u);
    i = pc_u32_as_i32(u);
    return i ^ pc_u32_as_i32((uint32_t)(i >> 31) >> 1);
}

/* One box pass (radius r, edge-clamped) along rows or columns. */
static void blur_pass(size_t w, size_t h, const float *src, float *dst, size_t r, bool horizontal)
{
    size_t n = horizontal ? h : w, m = horizontal ? w : h, line, k;
    int64_t ri = (int64_t)r, mi = (int64_t)m;
    float div = (float)(2 * r + 1);
#define AT(kk)                                                                                               \
    (horizontal ? src[line * w + (size_t)pc_clamp_i64((kk), 0, mi - 1)]                                      \
                : src[(size_t)pc_clamp_i64((kk), 0, mi - 1) * w + line])
    for (line = 0; line < n; line++) {
        float acc = -0.0f;
        int64_t d;
        for (d = -ri; d <= ri; d++) {
            acc += AT(d);
        }
        for (k = 0; k < m; k++) {
            size_t i = horizontal ? line * w + k : k * w + line;
            int64_t ki = (int64_t)k;
            dst[i] = acc / div;
            acc += AT(ki + ri + 1) - AT(ki - ri);
        }
    }
#undef AT
}

/* blur(): horizontal then vertical box pass; `out` and `tmp` hold w * h (w, h > 0). */
static void box_blur(size_t w, size_t h, const float *v, float *tmp, float *out, size_t r)
{
    blur_pass(w, h, v, tmp, r, true);
    blur_pass(w, h, tmp, out, r, false);
}

static int cmp_corner(const void *pa, const void *pb)
{
    const pc_algo_corner *a = pa, *b = pb;
    int32_t ka = total_key(a->score), kb = total_key(b->score);
    if (ka != kb) {
        return ka > kb ? -1 : 1; /* strongest first */
    }
    ka = total_key(a->y), kb = total_key(b->y);
    if (ka != kb) {
        return ka < kb ? -1 : 1;
    }
    ka = total_key(a->x), kb = total_key(b->x);
    return ka < kb ? -1 : (ka > kb ? 1 : 0);
}

pc_status pc_algo_features_harris(size_t w, size_t h, const float *img, size_t max, float min_dist,
                                  const bool *valid, pc_algo_corner **out, size_t *count, pc_error *err)
{
    size_t n, x, y, i, ncand = 0, capcand = 0, nout = 0, gw, gh, m = PR + 1;
    float *xx = NULL, *yy = NULL, *xy = NULL, *tmp = NULL, *bx = NULL, *rr = NULL, peak, cell;
    pc_algo_corner *cand = NULL, *res = NULL;
    size_t *head = NULL, *next = NULL;
    pc_status st;
    if (w < 2 * PR + 3 || h < 2 * PR + 3) {
        *out = NULL;
        *count = 0;
        return PC_OK;
    }
    st = check_plane(w, h, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (img == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no image for corner detection");
    }
    xx = pc_calloc_array(n, sizeof(float));
    yy = pc_calloc_array(n, sizeof(float));
    xy = pc_calloc_array(n, sizeof(float));
    tmp = pc_malloc_array(n, sizeof(float));
    bx = pc_malloc_array(n, sizeof(float));
    rr = pc_malloc_array(n, sizeof(float));
    if (xx == NULL || yy == NULL || xy == NULL || tmp == NULL || bx == NULL || rr == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for corner detection");
        goto cleanup;
    }
    for (y = 1; y < h - 1; y++) {
        for (x = 1; x < w - 1; x++) {
#define P(dx, dy) img[(size_t)((int64_t)y + (dy)) * w + (size_t)((int64_t)x + (dx))]
            float gx = (P(1, -1) + 2.0f * P(1, 0) + P(1, 1)) - (P(-1, -1) + 2.0f * P(-1, 0) + P(-1, 1));
            float gy = (P(-1, 1) + 2.0f * P(0, 1) + P(1, 1)) - (P(-1, -1) + 2.0f * P(0, -1) + P(1, -1));
#undef P
            i = y * w + x;
            xx[i] = gx * gx;
            yy[i] = gy * gy;
            xy[i] = gx * gy;
        }
    }
    /* Two box passes approximate the Gaussian window (blur(blur(v, 1), 1) per tensor term). */
    box_blur(w, h, xx, tmp, bx, 1);
    box_blur(w, h, bx, tmp, xx, 1);
    box_blur(w, h, yy, tmp, bx, 1);
    box_blur(w, h, bx, tmp, yy, 1);
    box_blur(w, h, xy, tmp, bx, 1);
    box_blur(w, h, bx, tmp, xy, 1);
    peak = 0.0f;
    for (i = 0; i < n; i++) {
        float s = xx[i] + yy[i];
        rr[i] = xx[i] * yy[i] - xy[i] * xy[i] - 0.04f * (s * s);
        peak = pc_rs_maxf(peak, rr[i]);
    }
    if (!(peak > 0.0f)) {
        *out = NULL;
        *count = 0;
        st = PC_OK;
        goto cleanup;
    }
    for (y = m; y < h - m; y++) {
        for (x = m; x < w - m; x++) {
            float v = rr[y * w + x];
            bool is_max = true;
            int dy, dx;
            if (v < peak * 0.01f) {
                continue;
            }
            for (dy = -1; dy <= 1 && is_max; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    if ((dx != 0 || dy != 0) &&
                        !(rr[(size_t)((int64_t)y + dy) * w + (size_t)((int64_t)x + dx)] <= v)) {
                        is_max = false;
                        break;
                    }
                }
            }
            if (!is_max) {
                continue;
            }
            if (valid != NULL) {
                bool all = true;
                size_t py, px;
                for (py = y - PR; py <= y + PR && all; py += 5) {
                    for (px = x - PR; px <= x + PR; px += 5) {
                        if (!valid[py * w + px]) {
                            all = false;
                            break;
                        }
                    }
                }
                if (!all) {
                    continue;
                }
            }
            if (ncand + 1 > capcand) {
                pc_algo_corner *c2 = pc_grow_array(cand, &capcand, ncand + 1, sizeof *cand);
                if (c2 == NULL) {
                    st = pc_fail(err, PC_ERR_NOMEM, "out of memory for corner candidates");
                    goto cleanup;
                }
                cand = c2;
            }
            cand[ncand].x = (float)x;
            cand[ncand].y = (float)y;
            cand[ncand].score = v;
            ncand++;
        }
    }
    if (ncand > 1) {
        /* Keys are unique (distinct positions), so qsort matches the stable sort. */
        qsort(cand, ncand, sizeof *cand, cmp_corner);
    }
    /* Minimum spacing via a coarse occupancy grid (per-cell linked lists of accepted corners). */
    cell = pc_rs_maxf_k(min_dist, 1.0f);
    gw = pc_f32_to_size(ceilf((float)w / cell)) + 1;
    gh = pc_f32_to_size(ceilf((float)h / cell)) + 1;
    {
        size_t ncell;
        if (!pc_size_mul(gw, gh, &ncell) || ncell > PC_MAX_ALLOC / sizeof(size_t)) {
            st = pc_fail(err, PC_ERR_LIMIT, "corner grid too large");
            goto cleanup;
        }
        head = pc_malloc_array(ncell, sizeof(size_t));
        next = pc_malloc_array(ncand > 0 ? ncand : 1, sizeof(size_t));
        res = pc_malloc_array(ncand > 0 ? ncand : 1, sizeof *res);
        if (head == NULL || next == NULL || res == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory for the corner grid");
            goto cleanup;
        }
        for (i = 0; i < ncell; i++) {
            head[i] = SIZE_MAX;
        }
    }
    for (i = 0; i < ncand; i++) {
        pc_algo_corner c = cand[i];
        size_t gx = pc_f32_to_size(c.x / cell), gy = pc_f32_to_size(c.y / cell), cy, cx;
        size_t y0, y1, x0, x1;
        bool near = false;
        gx = gx < gw - 1 ? gx : gw - 1;
        gy = gy < gh - 1 ? gy : gh - 1;
        y0 = gy > 0 ? gy - 1 : 0;
        y1 = gy + 1 < gh - 1 ? gy + 1 : gh - 1;
        x0 = gx > 0 ? gx - 1 : 0;
        x1 = gx + 1 < gw - 1 ? gx + 1 : gw - 1;
        for (cy = y0; cy <= y1 && !near; cy++) {
            for (cx = x0; cx <= x1 && !near; cx++) {
                size_t k;
                for (k = head[cy * gw + cx]; k != SIZE_MAX; k = next[k]) {
                    if (hypotf(res[k].x - c.x, res[k].y - c.y) < min_dist) {
                        near = true;
                        break;
                    }
                }
            }
        }
        if (near) {
            continue;
        }
        res[nout] = c;
        next[nout] = head[gy * gw + gx];
        head[gy * gw + gx] = nout;
        nout++;
        if (nout >= max) {
            break;
        }
    }
    if (nout == 0) {
        free(res);
        res = NULL;
    }
    *out = res;
    *count = nout;
    res = NULL;
    st = PC_OK;
cleanup:
    free(xx), free(yy), free(xy), free(tmp), free(bx), free(rr);
    free(cand), free(head), free(next), free(res);
    return st;
}

/* The 256 BRIEF test pairs: Box-Muller Gaussians around the centre, sigma 31/5, clipped to the
 * patch, from the fixed stream 0x0B21EF00. Computed per call (1,024 ln/cos: negligible). */
static void brief_pairs(float p[256][4])
{
    pc_algo_photo_rng rng = {0x0B21EF00u};
    size_t i, k;
    for (i = 0; i < 256; i++) {
        for (k = 0; k < 4; k++) {
            double u = pc_rs_max_k(pc_algo_photo_rng_unit(&rng), 1e-12);
            double v = pc_algo_photo_rng_unit(&rng);
            double g = sqrt(-2.0 * log(u)) * cos(6.283185307179586 * v) * 31.0 / 5.0;
            p[i][k] = (float)pc_clampd(g, -13.0, 13.0);
        }
    }
}

pc_status pc_algo_features_describe(size_t w, size_t h, const float *img, const pc_algo_corner *corners,
                                    size_t n, bool upright, pc_algo_descriptor **out, pc_error *err)
{
    size_t np, i;
    float *smooth, *tmp, (*pairs)[4];
    pc_algo_descriptor *d;
    pc_status st;
    if (n == 0) {
        *out = NULL;
        return PC_OK;
    }
    if (w == 0 || h == 0) {
        return pc_fail(err, PC_ERR_INVALID, "cannot describe corners of an empty image");
    }
    st = check_plane(w, h, &np, err);
    if (st != PC_OK) {
        return st;
    }
    smooth = pc_malloc_array(np, sizeof(float));
    tmp = pc_malloc_array(np, sizeof(float));
    pairs = pc_malloc_array(256, sizeof *pairs);
    d = pc_calloc_array(n, sizeof *d);
    if (smooth == NULL || tmp == NULL || pairs == NULL || d == NULL) {
        free(smooth), free(tmp), free(pairs), free(d);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for descriptors");
    }
    box_blur(w, h, img, tmp, smooth, 2);
    brief_pairs(pairs);
#define AT(px, py)                                                                                           \
    smooth[(size_t)pc_clamp_i64(pc_f32_to_i64(roundf(py)), 0, (int64_t)h - 1) * w +                          \
           (size_t)pc_clamp_i64(pc_f32_to_i64(roundf(px)), 0, (int64_t)w - 1)]
    for (i = 0; i < n; i++) {
        pc_algo_corner c = corners[i];
        float s = 0.0f, co = 1.0f;
        size_t k;
        if (!upright) {
            float m10 = 0.0f, m01 = 0.0f;
            int64_t r = PR, dy, dx;
            for (dy = -r; dy <= r; dy++) {
                for (dx = -r; dx <= r; dx++) {
                    if (dx * dx + dy * dy <= r * r) {
                        float v = AT(c.x + (float)dx, c.y + (float)dy);
                        m10 += (float)dx * v;
                        m01 += (float)dy * v;
                    }
                }
            }
            {
                float a = atan2f(m01, m10);
                s = sinf(a);
                co = cosf(a);
            }
        }
        for (k = 0; k < 256; k++) {
            const float *p = pairs[k];
            float ax = c.x + p[0] * co - p[1] * s, ay = c.y + p[0] * s + p[1] * co;
            float bx = c.x + p[2] * co - p[3] * s, by = c.y + p[2] * s + p[3] * co;
            if (AT(ax, ay) < AT(bx, by)) {
                d[i].bits[k / 64] |= (uint64_t)1 << (k % 64);
            }
        }
    }
#undef AT
    free(smooth), free(tmp), free(pairs);
    *out = d;
    return PC_OK;
}

static uint32_t popcount64(uint64_t v)
{
    uint32_t c = 0;
    while (v != 0) {
        v &= v - 1;
        c++;
    }
    return c;
}

uint32_t pc_algo_features_hamming(const pc_algo_descriptor *a, const pc_algo_descriptor *b)
{
    return popcount64(a->bits[0] ^ b->bits[0]) + popcount64(a->bits[1] ^ b->bits[1]) +
           popcount64(a->bits[2] ^ b->bits[2]) + popcount64(a->bits[3] ^ b->bits[3]);
}

/* Nearest and second-nearest distances of d in set; false for an empty set. */
static bool best_of(const pc_algo_descriptor *d, const pc_algo_descriptor *set, size_t n, size_t *j,
                    uint32_t *d1, uint32_t *d2)
{
    size_t k, fj = SIZE_MAX;
    uint32_t f1 = UINT32_MAX, second = UINT32_MAX;
    for (k = 0; k < n; k++) {
        uint32_t hd = pc_algo_features_hamming(d, &set[k]);
        if (hd < f1) {
            second = f1;
            fj = k;
            f1 = hd;
        } else if (hd < second) {
            second = hd;
        }
    }
    *j = fj;
    *d1 = f1;
    *d2 = second;
    return fj != SIZE_MAX;
}

typedef struct match_ctx {
    const pc_algo_descriptor *a, *b;
    size_t na, nb;
    float ratio;
    size_t *hit; /* per a: index in b or SIZE_MAX */
} match_ctx;

static pc_status match_chunk(void *vctx, size_t chunk, size_t begin, size_t end)
{
    match_ctx *c = vctx;
    size_t i;
    (void)chunk;
    for (i = begin; i < end; i++) {
        size_t j, k;
        uint32_t d1, d2, e1, e2;
        c->hit[i] = SIZE_MAX;
        if (!best_of(&c->a[i], c->b, c->nb, &j, &d1, &d2)) {
            continue;
        }
        if (d1 > 80 || (d2 != UINT32_MAX && (float)d1 > c->ratio * (float)d2)) {
            continue;
        }
        if (best_of(&c->b[j], c->a, c->na, &k, &e1, &e2) && k == i) {
            c->hit[i] = j;
        }
    }
    return PC_OK;
}

pc_status pc_algo_features_match(const pc_algo_descriptor *a, size_t na, const pc_algo_descriptor *b,
                                 size_t nb, float ratio, pc_algo_match **out, size_t *count, pc_error *err)
{
    match_ctx c;
    pc_algo_match *res;
    size_t i, nres = 0;
    if (na == 0) {
        *out = NULL;
        *count = 0;
        return PC_OK;
    }
    c.a = a, c.b = b, c.na = na, c.nb = nb, c.ratio = ratio;
    c.hit = pc_malloc_array(na, sizeof(size_t));
    res = pc_malloc_array(na, sizeof *res);
    if (c.hit == NULL || res == NULL) {
        free(c.hit), free(res);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for descriptor matching");
    }
    (void)pc_algo_par_for(na, 16, match_chunk, &c);
    for (i = 0; i < na; i++) {
        if (c.hit[i] != SIZE_MAX) {
            res[nres].a = i;
            res[nres].b = c.hit[i];
            nres++;
        }
    }
    free(c.hit);
    if (nres == 0) {
        free(res);
        res = NULL;
    }
    *out = res;
    *count = nres;
    return PC_OK;
}

/* ------------------------------------------------------------------ model fitting */

/* Gaussian elimination with partial pivoting (features' own copy: threshold 1e-12, no zero-factor
 * skip, no finiteness check). a (n * n) and b (n) are overwritten; x gets the solution. */
static bool solve(size_t n, double *a, double *b, double *x)
{
    size_t c, r, k;
    for (c = 0; c < n; c++) {
        size_t p = c;
        double best = fabs(a[c * n + c]);
        uint64_t bb, vb;
        memcpy(&bb, &best, sizeof bb);
        for (r = c + 1; r < n; r++) {
            double v = fabs(a[r * n + c]);
            memcpy(&vb, &v, sizeof vb);
            if (vb >= bb) { /* max_by keeps the last maximum (total order on |v|) */
                bb = vb;
                p = r;
            }
        }
        if (fabs(a[p * n + c]) < 1e-12) {
            return false;
        }
        if (p != c) {
            for (k = 0; k < n; k++) {
                double t = a[c * n + k];
                a[c * n + k] = a[p * n + k];
                a[p * n + k] = t;
            }
            {
                double t = b[c];
                b[c] = b[p];
                b[p] = t;
            }
        }
        for (r = c + 1; r < n; r++) {
            double f = a[r * n + c] / a[c * n + c];
            for (k = c; k < n; k++) {
                a[r * n + k] -= f * a[c * n + k];
            }
            b[r] -= f * b[c];
        }
    }
    for (r = n; r-- > 0;) {
        double s = -0.0;
        for (k = r + 1; k < n; k++) {
            s += a[r * n + k] * x[k];
        }
        x[r] = (b[r] - s) / a[r * n + r];
    }
    return true;
}

/* Normal-equation accumulation of one row (all n entries, zeros included, as the reference). */
static void ls_row(double *ata, double *atb, size_t n, const double *r, double b)
{
    size_t i, j;
    for (i = 0; i < n; i++) {
        atb[i] += r[i] * b;
        for (j = 0; j < n; j++) {
            ata[i * n + j] += r[i] * r[j];
        }
    }
}

/* Hartley normalisation: centroid to the origin, mean distance sqrt 2. */
static pc_algo_homography normaliser(const double (*pts)[2], size_t n)
{
    double nf = (double)(n > 0 ? n : 1), sx = -0.0, sy = -0.0, sd = -0.0, cx, cy, d, s;
    size_t i;
    pc_algo_homography h;
    for (i = 0; i < n; i++) {
        sx += pts[i][0];
    }
    for (i = 0; i < n; i++) {
        sy += pts[i][1];
    }
    cx = sx / nf;
    cy = sy / nf;
    for (i = 0; i < n; i++) {
        sd += hypot(pts[i][0] - cx, pts[i][1] - cy);
    }
    d = sd / nf;
    s = d > 1e-12 ? 1.4142135623730951 / d : 1.0;
    h.m[0] = s, h.m[1] = 0.0, h.m[2] = -s * cx;
    h.m[3] = 0.0, h.m[4] = s, h.m[5] = -s * cy;
    h.m[6] = 0.0, h.m[7] = 0.0, h.m[8] = 1.0;
    return h;
}

bool pc_algo_features_fit(pc_algo_model model, const double (*src)[2], const double (*dst)[2], size_t n,
                          pc_algo_homography *out)
{
    size_t i;
    if (model == PC_ALGO_MODEL_TRANSLATION) {
        double tx = 0.0, ty = 0.0;
        if (n == 0) {
            return false;
        }
        for (i = 0; i < n; i++) {
            tx = tx + dst[i][0] - src[i][0];
            ty = ty + dst[i][1] - src[i][1];
        }
        *out = (pc_algo_homography){{1.0, 0.0, tx / (double)n, 0.0, 1.0, ty / (double)n, 0.0, 0.0, 1.0}};
        return true;
    }
    if (model == PC_ALGO_MODEL_SIMILARITY) {
        double ata[16] = {0}, atb[4] = {0}, v[4];
        if (n < 2) {
            return false;
        }
        /* x' = a x - b y + tx, y' = b x + a y + ty. */
        for (i = 0; i < n; i++) {
            const double *s = src[i], *d = dst[i];
            double r1[4] = {s[0], -s[1], 1.0, 0.0}, r2[4] = {s[1], s[0], 0.0, 1.0};
            ls_row(ata, atb, 4, r1, d[0]);
            ls_row(ata, atb, 4, r2, d[1]);
        }
        if (!solve(4, ata, atb, v)) {
            return false;
        }
        *out = (pc_algo_homography){{v[0], -v[1], v[2], v[1], v[0], v[3], 0.0, 0.0, 1.0}};
        return true;
    }
    {
        double ata[64] = {0}, atb[8] = {0}, hv[8], k;
        pc_algo_homography ts, td, tdi, hn, m;
        if (n < 4) {
            return false;
        }
        ts = normaliser(src, n);
        td = normaliser(dst, n);
        for (i = 0; i < n; i++) {
            double x, y, u, v;
            pc_algo_homography_apply(&ts, src[i][0], src[i][1], &x, &y);
            pc_algo_homography_apply(&td, dst[i][0], dst[i][1], &u, &v);
            {
                double r1[8] = {x, y, 1.0, 0.0, 0.0, 0.0, -u * x, -u * y};
                double r2[8] = {0.0, 0.0, 0.0, x, y, 1.0, -v * x, -v * y};
                ls_row(ata, atb, 8, r1, u);
                ls_row(ata, atb, 8, r2, v);
            }
        }
        if (!solve(8, ata, atb, hv)) {
            return false;
        }
        hn = (pc_algo_homography){{hv[0], hv[1], hv[2], hv[3], hv[4], hv[5], hv[6], hv[7], 1.0}};
        if (!pc_algo_homography_inverse(&td, &tdi)) {
            return false;
        }
        m = pc_algo_homography_mul(&tdi, &hn);
        m = pc_algo_homography_mul(&m, &ts);
        k = m.m[8];
        if (!(fabs(k) > 1e-12)) {
            return false;
        }
        for (i = 0; i < 9; i++) {
            m.m[i] /= k;
        }
        *out = m;
        return true;
    }
}

static size_t min_points(pc_algo_model model)
{
    return model == PC_ALGO_MODEL_TRANSLATION ? 1 : (model == PC_ALGO_MODEL_SIMILARITY ? 2 : 4);
}

/* ---- generic RANSAC (shared with panorama's ransac_motion) ---- */

static size_t inliers_of(const pc_algo_homography *h, const double (*src)[2], const double (*dst)[2],
                         size_t n, double threshold, size_t *idx)
{
    size_t i, c = 0;
    for (i = 0; i < n; i++) {
        double x, y;
        pc_algo_homography_apply(h, src[i][0], src[i][1], &x, &y);
        if (isfinite(x) && hypot(x - dst[i][0], y - dst[i][1]) <= threshold) {
            idx[c++] = i;
        }
    }
    return c;
}

pc_status pc_algo_ransac_generic(pc_algo_fit_fn fit, void *fit_ctx, size_t k, bool draw_when_minimal,
                                 const double (*src)[2], const double (*dst)[2], size_t n, size_t iters,
                                 double threshold, uint64_t stream, bool *found, pc_algo_homography *out,
                                 size_t **inliers, size_t *ninliers, pc_error *err)
{
    pc_algo_photo_rng rng = {stream};
    size_t *cur = NULL, *best = NULL, *sel = NULL, nbest = 0, it, i;
    double (*ss)[2] = NULL, (*sd)[2] = NULL;
    bool have = false;
    pc_algo_homography hbest, h2;
    pc_status st = PC_OK;
    *found = false;
    if (n < k || k == 0) {
        return PC_OK;
    }
    cur = pc_malloc_array(n, sizeof(size_t));
    best = pc_malloc_array(n, sizeof(size_t));
    sel = pc_malloc_array(k, sizeof(size_t));
    ss = pc_malloc_array(n, sizeof *ss);
    sd = pc_malloc_array(n, sizeof *sd);
    if (cur == NULL || best == NULL || sel == NULL || ss == NULL || sd == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for RANSAC");
        goto cleanup;
    }
    for (it = 0; it < (iters > 0 ? iters : 1); it++) {
        size_t nsel = 0, ncur;
        pc_algo_homography h;
        if (n == k && !draw_when_minimal) {
            for (nsel = 0; nsel < k; nsel++) {
                sel[nsel] = nsel;
            }
        }
        while (nsel < k) {
            size_t c = pc_algo_photo_rng_below(&rng, n), q;
            bool dup = false;
            for (q = 0; q < nsel; q++) {
                dup = dup || sel[q] == c;
            }
            if (!dup) {
                sel[nsel++] = c;
            }
            if (n == k) { /* features::ransac: one draw, then the whole set */
                for (nsel = 0; nsel < k; nsel++) {
                    sel[nsel] = nsel;
                }
            }
        }
        for (i = 0; i < k; i++) {
            ss[i][0] = src[sel[i]][0], ss[i][1] = src[sel[i]][1];
            sd[i][0] = dst[sel[i]][0], sd[i][1] = dst[sel[i]][1];
        }
        if (!fit(fit_ctx, (const double (*)[2])ss, (const double (*)[2])sd, k, &h)) {
            continue;
        }
        ncur = inliers_of(&h, src, dst, n, threshold, cur);
        if (!have || ncur > nbest) {
            size_t *t = best;
            best = cur;
            cur = t;
            nbest = ncur;
            hbest = h;
            have = true;
        }
    }
    if (!have || nbest < k) {
        goto cleanup;
    }
    /* Refit to all inliers, then re-collect. */
    for (i = 0; i < nbest; i++) {
        ss[i][0] = src[best[i]][0], ss[i][1] = src[best[i]][1];
        sd[i][0] = dst[best[i]][0], sd[i][1] = dst[best[i]][1];
    }
    if (!fit(fit_ctx, (const double (*)[2])ss, (const double (*)[2])sd, nbest, &h2)) {
        h2 = hbest;
    }
    {
        size_t n2 = inliers_of(&h2, src, dst, n, threshold, cur);
        size_t *res, nres = n2 >= nbest ? n2 : nbest;
        res = pc_malloc_array(nres > 0 ? nres : 1, sizeof(size_t));
        if (res == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory for RANSAC inliers");
            goto cleanup;
        }
        memcpy(res, n2 >= nbest ? cur : best, nres * sizeof(size_t));
        *out = n2 >= nbest ? h2 : hbest;
        *inliers = res;
        *ninliers = nres;
        *found = true;
    }
cleanup:
    free(cur), free(best), free(sel), free(ss), free(sd);
    return st;
}

static bool fit_model(void *ctx, const double (*src)[2], const double (*dst)[2], size_t n,
                      pc_algo_homography *out)
{
    return pc_algo_features_fit(*(const pc_algo_model *)ctx, src, dst, n, out);
}

pc_status pc_algo_features_ransac(pc_algo_model model, const double (*src)[2], const double (*dst)[2],
                                  size_t n, size_t iters, double threshold, uint64_t seed, bool *found,
                                  pc_algo_homography *out, size_t **inliers, size_t *ninliers, pc_error *err)
{
    return pc_algo_ransac_generic(fit_model, &model, min_points(model), true, src, dst, n, iters, threshold,
                                  seed ^ 0xA11C0DE5u, found, out, inliers, ninliers, err);
}

pc_status pc_algo_features_register(size_t w, size_t h, const float *reference, const float *moving,
                                    const bool *valid_ref, const bool *valid_mov, pc_algo_model model,
                                    bool *found, pc_algo_homography *out, size_t *ninliers, pc_error *err)
{
    bool upright = model == PC_ALGO_MODEL_TRANSLATION, ok = false;
    float min_dist = pc_clampf((float)(w > h ? w : h) / 60.0f, 4.0f, 24.0f);
    pc_algo_corner *ca = NULL, *cb = NULL;
    pc_algo_descriptor *da = NULL, *db = NULL;
    pc_algo_match *m = NULL;
    size_t nca = 0, ncb = 0, nm = 0, i, *inl = NULL, ninl = 0;
    double (*src)[2] = NULL, (*dst)[2] = NULL, threshold;
    pc_algo_homography hm;
    pc_status st;
    *found = false;
    st = pc_algo_features_harris(w, h, reference, 600, min_dist, valid_ref, &ca, &nca, err);
    if (st == PC_OK) {
        st = pc_algo_features_harris(w, h, moving, 600, min_dist, valid_mov, &cb, &ncb, err);
    }
    if (st != PC_OK || nca < 4 || ncb < 4) {
        goto cleanup;
    }
    st = pc_algo_features_describe(w, h, reference, ca, nca, upright, &da, err);
    if (st == PC_OK) {
        st = pc_algo_features_describe(w, h, moving, cb, ncb, upright, &db, err);
    }
    if (st == PC_OK) {
        st = pc_algo_features_match(db, ncb, da, nca, 0.85f, &m, &nm, err);
    }
    if (st != PC_OK) {
        goto cleanup;
    }
    src = pc_malloc_array(nm > 0 ? nm : 1, sizeof *src);
    dst = pc_malloc_array(nm > 0 ? nm : 1, sizeof *dst);
    if (src == NULL || dst == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for registration");
        goto cleanup;
    }
    for (i = 0; i < nm; i++) {
        src[i][0] = (double)cb[m[i].a].x, src[i][1] = (double)cb[m[i].a].y;
        dst[i][0] = (double)ca[m[i].b].x, dst[i][1] = (double)ca[m[i].b].y;
    }
    threshold = pc_clampd((double)(w > h ? w : h) / 400.0, 1.5, 4.0);
    st = pc_algo_features_ransac(model, (const double (*)[2])src, (const double (*)[2])dst, nm, 1000,
                                 threshold, 7, &ok, &hm, &inl, &ninl, err);
    if (st == PC_OK && ok) {
        size_t need = min_points(model) > 6 ? min_points(model) : 6;
        if (ninl >= need) {
            *out = hm;
            *ninliers = ninl;
            *found = true;
        }
    }
cleanup:
    free(ca), free(cb), free(da), free(db), free(m), free(src), free(dst), free(inl);
    return st;
}
