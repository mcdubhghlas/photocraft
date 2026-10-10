/* Edge refinement and colour decontamination (port of crates/algo/src/matting.rs). */
#include "algo/matting.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

#define TILE 256
/* Largest Gaussian radius (taps per side); the reference would allocate gigabytes beyond it. */
#define MAX_GAUSS_R ((int64_t)1 << 24)
/* Largest refine halo; beyond it every tile reads an absurd area (the reference overflows). */
#define MAX_HALO ((int64_t)1 << 20)

const pc_algo_jfield pc_algo_refine_params_fields[] = {
    PC_ALGO_JFD("radius", pc_algo_refine_params, radius, PC_ALGO_JF_F32),
    PC_ALGO_JFD("smartRadius", pc_algo_refine_params, smart_radius, PC_ALGO_JF_BOOL),
    PC_ALGO_JFD("smooth", pc_algo_refine_params, smooth, PC_ALGO_JF_F32),
    PC_ALGO_JFD("feather", pc_algo_refine_params, feather, PC_ALGO_JF_F32),
    PC_ALGO_JFD("contrast", pc_algo_refine_params, contrast, PC_ALGO_JF_F32),
    PC_ALGO_JFD("shiftEdge", pc_algo_refine_params, shift_edge, PC_ALGO_JF_F32),
};
const size_t pc_algo_refine_params_nfields =
    sizeof pc_algo_refine_params_fields / sizeof pc_algo_refine_params_fields[0];

pc_algo_refine_params pc_algo_refine_params_default(void)
{
    pc_algo_refine_params p;
    memset(&p, 0, sizeof p);
    return p;
}

static size_t gf_radius(const pc_algo_refine_params *p)
{
    return pc_f32_to_size(pc_clampf(ceilf(p->radius), 1.0f, 64.0f));
}
static float smooth_sigma(const pc_algo_refine_params *p)
{
    return pc_clampf(p->smooth, 0.0f, 100.0f) / 100.0f * 4.0f;
}
static float feather_sigma(const pc_algo_refine_params *p)
{
    return pc_rs_maxf_k(p->feather, 0.0f) / 2.0f;
}
static int32_t shift_px(const pc_algo_refine_params *p)
{
    float v = roundf(pc_clampf(p->shift_edge, -100.0f, 100.0f) / 100.0f * pc_rs_maxf_k(p->radius, 4.0f));
    return pc_f32_to_i32(pc_clampf(v, -64.0f, 64.0f));
}
static bool uses_image(const pc_algo_refine_params *p)
{
    return p->radius > 0.0f;
}

/* The halo in 64 bits (no wrapping), for the limit check. */
static int64_t halo64(const pc_algo_refine_params *p)
{
    int64_t edge = 0, s = shift_px(p);
    if (uses_image(p)) {
        int64_t g = (int64_t)gf_radius(p), a = pc_f32_to_i32(ceilf(p->radius)),
                b = 2 * g + (p->smart_radius ? g + 1 : 0);
        edge = a > b ? a : b;
    }
    return edge + pc_f32_to_i32(ceilf(3.0f * smooth_sigma(p))) +
           pc_f32_to_i32(ceilf(3.0f * feather_sigma(p))) + (s < 0 ? -s : s) + 2;
}

int32_t pc_algo_refine_halo(const pc_algo_refine_params *p)
{
    int32_t edge = 0, s = shift_px(p), h;
    if (uses_image(p)) {
        int32_t g = (int32_t)gf_radius(p), a = pc_f32_to_i32(ceilf(p->radius)),
                b = 2 * g + (p->smart_radius ? g + 1 : 0);
        edge = a > b ? a : b;
    }
    /* i32 arithmetic wraps like the reference's release build. */
    h = pc_i32_wrapping_add(edge, pc_f32_to_i32(ceilf(3.0f * smooth_sigma(p))));
    h = pc_i32_wrapping_add(h, pc_f32_to_i32(ceilf(3.0f * feather_sigma(p))));
    h = pc_i32_wrapping_add(h, s < 0 ? -s : s);
    return pc_i32_wrapping_add(h, 2);
}

int32_t pc_algo_refine_spread(const pc_algo_refine_params *p)
{
    int32_t h = uses_image(p) ? pc_f32_to_i32(ceilf(p->radius)) : 0, s = shift_px(p);
    h = pc_i32_wrapping_add(h, pc_f32_to_i32(ceilf(3.0f * smooth_sigma(p))));
    h = pc_i32_wrapping_add(h, pc_f32_to_i32(ceilf(3.0f * feather_sigma(p))));
    h = pc_i32_wrapping_add(h, s > 0 ? s : 0);
    return pc_i32_wrapping_add(h, 1);
}

static pc_status count2(size_t w, size_t h, size_t elem, size_t *n, pc_error *err)
{
    size_t c;
    if (!pc_size_mul(w, h, &c) || (elem > 0 && c > PC_MAX_ALLOC / elem)) {
        return pc_fail(err, PC_ERR_LIMIT, "mask too large");
    }
    *n = c;
    return PC_OK;
}

static float *alloc_f(size_t n)
{
    return pc_calloc_array(n ? n : 1, sizeof(float));
}

/* ---- box mean, guided filters ---- */

pc_status pc_algo_matting_box_mean(const float *src, size_t w, size_t h, size_t r, float **out_p,
                                   pc_error *err)
{
    size_t n, x, y, cn;
    float *tmp, *out;
    double *pre, *col;
    pc_status st = count2(w, h, sizeof(double), &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (!pc_size_mul(h + 1, w, &cn) || cn > PC_MAX_ALLOC / sizeof(double)) {
        return pc_fail(err, PC_ERR_LIMIT, "mask too large");
    }
    tmp = alloc_f(n);
    out = alloc_f(n);
    pre = pc_calloc_array((w > h ? w : h) + 1, sizeof(double));
    col = pc_calloc_array(cn ? cn : 1, sizeof(double));
    if (tmp == NULL || out == NULL || pre == NULL || col == NULL) {
        free(tmp);
        free(out);
        free(pre);
        free(col);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a box mean");
    }
    for (y = 0; y < h; y++) {
        const float *row = src + y * w;
        for (x = 0; x < w; x++) {
            pre[x + 1] = pre[x] + (double)row[x];
        }
        for (x = 0; x < w; x++) {
            size_t lo = x > r ? x - r : 0, hi = r < w - x ? x + r + 1 : w;
            if (hi > w) {
                hi = w;
            }
            tmp[y * w + x] = pc_f64_to_f32((pre[hi] - pre[lo]) / (double)(hi - lo));
        }
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            col[(y + 1) * w + x] = col[y * w + x] + (double)tmp[y * w + x];
        }
    }
    for (y = 0; y < h; y++) {
        size_t lo = y > r ? y - r : 0, hi = r < h - y ? y + r + 1 : h;
        double nn;
        if (hi > h) {
            hi = h;
        }
        nn = (double)(hi - lo);
        for (x = 0; x < w; x++) {
            out[y * w + x] = pc_f64_to_f32((col[hi * w + x] - col[lo * w + x]) / nn);
        }
    }
    free(tmp);
    free(pre);
    free(col);
    *out_p = out;
    return PC_OK;
}

pc_status pc_algo_matting_guided_filter_gray(const float *guide, const float *p, size_t w, size_t h, size_t r,
                                             float eps, float **out_p, pc_error *err)
{
    size_t n, i;
    float *mi = NULL, *mp = NULL, *ip = NULL, *ii = NULL, *mip = NULL, *mii = NULL, *a = NULL, *b = NULL;
    float *ma = NULL, *mb = NULL, *out = NULL;
    pc_status st = count2(w, h, sizeof(double), &n, err);
    if (st != PC_OK) {
        return st;
    }
    ip = alloc_f(n);
    ii = alloc_f(n);
    a = alloc_f(n);
    b = alloc_f(n);
    if (ip == NULL || ii == NULL || a == NULL || b == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for a guided filter");
        goto done;
    }
    for (i = 0; i < n; i++) {
        ip[i] = guide[i] * p[i];
        ii[i] = guide[i] * guide[i];
    }
    if ((st = pc_algo_matting_box_mean(guide, w, h, r, &mi, err)) != PC_OK ||
        (st = pc_algo_matting_box_mean(p, w, h, r, &mp, err)) != PC_OK ||
        (st = pc_algo_matting_box_mean(ip, w, h, r, &mip, err)) != PC_OK ||
        (st = pc_algo_matting_box_mean(ii, w, h, r, &mii, err)) != PC_OK) {
        goto done;
    }
    for (i = 0; i < n; i++) {
        float var = mii[i] - mi[i] * mi[i];
        float cov = mip[i] - mi[i] * mp[i];
        a[i] = cov / (var + eps);
        b[i] = mp[i] - a[i] * mi[i];
    }
    if ((st = pc_algo_matting_box_mean(a, w, h, r, &ma, err)) != PC_OK ||
        (st = pc_algo_matting_box_mean(b, w, h, r, &mb, err)) != PC_OK) {
        goto done;
    }
    out = alloc_f(n);
    if (out == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for a guided filter");
        goto done;
    }
    for (i = 0; i < n; i++) {
        out[i] = ma[i] * guide[i] + mb[i];
    }
    *out_p = out;
done:
    free(mi);
    free(mp);
    free(ip);
    free(ii);
    free(mip);
    free(mii);
    free(a);
    free(b);
    free(ma);
    free(mb);
    return st;
}

pc_status pc_algo_matting_guided_filter_color(const pc_algo_rgb_image *guide, const float *p, size_t r,
                                              float eps, float **out_p, pc_error *err)
{
    static const int PAIRS[6][2] = {{0, 0}, {0, 1}, {0, 2}, {1, 1}, {1, 2}, {2, 2}};
    size_t w = guide->w, h = guide->h, n, i;
    float *chans[3] = {NULL, NULL, NULL}, *m[3] = {NULL, NULL, NULL}, *mp = NULL,
          *mip[3] = {NULL, NULL, NULL};
    float *mii[6] = {NULL, NULL, NULL, NULL, NULL, NULL}, *av[3] = {NULL, NULL, NULL}, *bv = NULL;
    float *ma[3] = {NULL, NULL, NULL}, *mb = NULL, *tmp = NULL, *out = NULL;
    int c, k;
    pc_status st = count2(w, h, sizeof(double), &n, err);
    if (st != PC_OK) {
        return st;
    }
    tmp = alloc_f(n);
    bv = alloc_f(n);
    for (c = 0; c < 3; c++) {
        chans[c] = alloc_f(n);
        av[c] = alloc_f(n);
    }
    if (tmp == NULL || bv == NULL || chans[0] == NULL || chans[1] == NULL || chans[2] == NULL ||
        av[0] == NULL || av[1] == NULL || av[2] == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for a guided filter");
        goto done;
    }
    for (i = 0; i < n; i++) {
        for (c = 0; c < 3; c++) {
            chans[c][i] = guide->px[i][c];
        }
    }
    for (c = 0; c < 3; c++) {
        if ((st = pc_algo_matting_box_mean(chans[c], w, h, r, &m[c], err)) != PC_OK) {
            goto done;
        }
    }
    if ((st = pc_algo_matting_box_mean(p, w, h, r, &mp, err)) != PC_OK) {
        goto done;
    }
    for (c = 0; c < 3; c++) {
        for (i = 0; i < n; i++) {
            tmp[i] = chans[c][i] * p[i];
        }
        if ((st = pc_algo_matting_box_mean(tmp, w, h, r, &mip[c], err)) != PC_OK) {
            goto done;
        }
    }
    for (k = 0; k < 6; k++) {
        for (i = 0; i < n; i++) {
            tmp[i] = chans[PAIRS[k][0]][i] * chans[PAIRS[k][1]][i];
        }
        if ((st = pc_algo_matting_box_mean(tmp, w, h, r, &mii[k], err)) != PC_OK) {
            goto done;
        }
    }
    for (i = 0; i < n; i++) {
        double mu[3] = {(double)m[0][i], (double)m[1][i], (double)m[2][i]}, e = (double)eps;
        double rr = (double)mii[0][i] - mu[0] * mu[0] + e, rg = (double)mii[1][i] - mu[0] * mu[1];
        double rb = (double)mii[2][i] - mu[0] * mu[2], gg = (double)mii[3][i] - mu[1] * mu[1] + e;
        double gb = (double)mii[4][i] - mu[1] * mu[2], bb = (double)mii[5][i] - mu[2] * mu[2] + e;
        double mpi = (double)mp[i];
        double cov[3] = {(double)mip[0][i] - mu[0] * mpi, (double)mip[1][i] - mu[1] * mpi,
                         (double)mip[2][i] - mu[2] * mpi};
        /* Inverse of the symmetric 3x3 matrix. */
        double i00 = gg * bb - gb * gb, i01 = gb * rb - rg * bb, i02 = rg * gb - gg * rb;
        double i11 = rr * bb - rb * rb, i12 = rb * rg - rr * gb, i22 = rr * gg - rg * rg;
        double det = rr * i00 + rg * i01 + rb * i02, a0 = 0.0, a1 = 0.0, a2 = 0.0;
        if (!(fabs(det) < 1e-30)) {
            a0 = (i00 * cov[0] + i01 * cov[1] + i02 * cov[2]) / det;
            a1 = (i01 * cov[0] + i11 * cov[1] + i12 * cov[2]) / det;
            a2 = (i02 * cov[0] + i12 * cov[1] + i22 * cov[2]) / det;
        }
        av[0][i] = pc_f64_to_f32(a0);
        av[1][i] = pc_f64_to_f32(a1);
        av[2][i] = pc_f64_to_f32(a2);
        bv[i] = pc_f64_to_f32(mpi - a0 * mu[0] - a1 * mu[1] - a2 * mu[2]);
    }
    for (c = 0; c < 3; c++) {
        if ((st = pc_algo_matting_box_mean(av[c], w, h, r, &ma[c], err)) != PC_OK) {
            goto done;
        }
    }
    if ((st = pc_algo_matting_box_mean(bv, w, h, r, &mb, err)) != PC_OK) {
        goto done;
    }
    out = alloc_f(n);
    if (out == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for a guided filter");
        goto done;
    }
    for (i = 0; i < n; i++) {
        out[i] = ma[0][i] * chans[0][i] + ma[1][i] * chans[1][i] + ma[2][i] * chans[2][i] + mb[i];
    }
    *out_p = out;
done:
    for (c = 0; c < 3; c++) {
        free(chans[c]);
        free(m[c]);
        free(mip[c]);
        free(av[c]);
        free(ma[c]);
    }
    for (k = 0; k < 6; k++) {
        free(mii[k]);
    }
    free(mp);
    free(bv);
    free(mb);
    free(tmp);
    return st;
}

pc_status pc_algo_matting_gaussian_blur(const float *src, size_t w, size_t h, float sigma, float **out_p,
                                        pc_error *err)
{
    size_t n, x, y, nk, i;
    int64_t r, j;
    float *k, *tmp, *out, s = -0.0f;
    pc_status st = count2(w, h, sizeof(float), &n, err);
    if (st != PC_OK) {
        return st;
    }
    out = alloc_f(n);
    if (out == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a blur");
    }
    if (sigma < 0.1f) {
        if (n > 0) {
            memcpy(out, src, n * sizeof(float));
        }
        *out_p = out;
        return PC_OK;
    }
    r = pc_f32_to_i64(ceilf(sigma * 3.0f));
    if (r > MAX_GAUSS_R) {
        free(out);
        return pc_fail(err, PC_ERR_LIMIT, "blur radius too large");
    }
    nk = (size_t)(2 * r + 1);
    k = pc_malloc_array(nk, sizeof(float));
    tmp = alloc_f(n);
    if (k == NULL || tmp == NULL) {
        free(k);
        free(tmp);
        free(out);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a blur");
    }
    for (j = -r; j <= r; j++) {
        k[j + r] = expf((float)(-(j * j)) / (2.0f * sigma * sigma));
    }
    for (i = 0; i < nk; i++) {
        s += k[i];
    }
    for (i = 0; i < nk; i++) {
        k[i] = k[i] / s;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float a = 0.0f;
            for (i = 0; i < nk; i++) {
                int64_t xx = pc_clamp_i64((int64_t)x + (int64_t)i - r, 0, (int64_t)w - 1);
                a += src[y * w + (size_t)xx] * k[i];
            }
            tmp[y * w + x] = a;
        }
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float a = 0.0f;
            for (i = 0; i < nk; i++) {
                int64_t yy = pc_clamp_i64((int64_t)y + (int64_t)i - r, 0, (int64_t)h - 1);
                a += tmp[(size_t)yy * w + x] * k[i];
            }
            out[y * w + x] = a;
        }
    }
    free(k);
    free(tmp);
    *out_p = out;
    return PC_OK;
}

pc_status pc_algo_matting_morph(const float *src, size_t w, size_t h, size_t r, bool grow, float **out_p,
                                pc_error *err)
{
    size_t n, x, y, k, cap;
    float *cur, *nxt;
    pc_status st = count2(w, h, sizeof(float), &n, err);
    if (st != PC_OK) {
        return st;
    }
    cur = alloc_f(n);
    nxt = alloc_f(n);
    if (cur == NULL || nxt == NULL) {
        free(cur);
        free(nxt);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a morphology pass");
    }
    if (n > 0) {
        memcpy(cur, src, n * sizeof(float));
        memcpy(nxt, src, n * sizeof(float));
    }
    /* After 2(w + h) steps every pixel holds the image's extremum, so more steps change nothing
     * (the reference would just keep looping). */
    cap = 2 * (w + h) + 2;
    if (r > cap) {
        r = cap + (r - cap) % 2;
    }
    for (k = 0; k < r; k++) {
        bool square = k % 2 == 0;
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                float v = cur[y * w + x];
                int dy, dx;
                for (dy = -1; dy <= 1; dy++) {
                    for (dx = -1; dx <= 1; dx++) {
                        int64_t nx = (int64_t)x + dx, ny = (int64_t)y + dy;
                        if ((dx != 0 && dy != 0 && !square) || (dx == 0 && dy == 0)) {
                            continue;
                        }
                        if (nx >= 0 && ny >= 0 && (size_t)nx < w && (size_t)ny < h) {
                            float o = cur[(size_t)ny * w + (size_t)nx];
                            v = grow ? pc_rs_maxf(v, o) : pc_rs_minf(v, o);
                        }
                    }
                }
                nxt[y * w + x] = v;
            }
        }
        {
            float *t = cur;
            cur = nxt;
            nxt = t;
        }
    }
    free(nxt);
    *out_p = cur;
    return PC_OK;
}

/* Running max (or min) over a window of radius r, van Herk / Gil-Werman. g and hb hold m. */
static void running_extreme(const float *src, size_t n, float *out, size_t r, bool max, float *g, float *hb,
                            size_t m)
{
    size_t k = 2 * r + 1, j, i;
    float pad = max ? -INFINITY : INFINITY;
#define F(a, b) (max ? pc_rs_maxf((a), (b)) : pc_rs_minf((a), (b)))
#define P(jj) ((jj) >= r && (jj) < r + n ? src[(jj) - r] : pad)
    for (j = 0; j < m; j++) {
        g[j] = j % k == 0 ? P(j) : F(g[j - 1], P(j));
    }
    for (j = m; j-- > 0;) {
        hb[j] = (j % k == k - 1 || j == m - 1) ? P(j) : F(hb[j + 1], P(j));
    }
    for (i = 0; i < n; i++) {
        out[i] = F(hb[i], g[i + 2 * r]);
    }
#undef F
#undef P
}

pc_status pc_algo_matting_max_square(const float *src, size_t w, size_t h, size_t r, bool max, float **out_p,
                                     pc_error *err)
{
    size_t n, k, mw, mh, mm, x, y;
    float *tmp, *out, *g, *hb, *col, *res;
    pc_status st = count2(w, h, sizeof(float), &n, err);
    if (st != PC_OK) {
        return st;
    }
    /* Windows wider than the line clip to the whole line. */
    if (r > w + h) {
        r = w + h;
    }
    k = 2 * r + 1;
    mw = (w + 2 * r + k - 1) / k * k;
    mh = (h + 2 * r + k - 1) / k * k;
    mm = mw > mh ? mw : mh;
    tmp = alloc_f(n);
    out = alloc_f(n);
    g = alloc_f(mm);
    hb = alloc_f(mm);
    col = alloc_f(h);
    res = alloc_f(h);
    if (tmp == NULL || out == NULL || g == NULL || hb == NULL || col == NULL || res == NULL) {
        free(tmp);
        free(out);
        free(g);
        free(hb);
        free(col);
        free(res);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a running extremum");
    }
    for (y = 0; y < h; y++) {
        running_extreme(src + y * w, w, tmp + y * w, r, max, g, hb, mw);
    }
    for (x = 0; x < w; x++) {
        for (y = 0; y < h; y++) {
            col[y] = tmp[y * w + x];
        }
        running_extreme(col, h, res, r, max, g, hb, mh);
        for (y = 0; y < h; y++) {
            out[y * w + x] = res[y];
        }
    }
    free(tmp);
    free(g);
    free(hb);
    free(col);
    free(res);
    *out_p = out;
    return PC_OK;
}

pc_status pc_algo_matting_edge_width(const pc_algo_rgb_image *img, size_t r, float **out_p, pc_error *err)
{
    size_t w = img->w, h = img->h, n, x, y;
    float *yl = NULL, *g = NULL, *gm = NULL, *hi = NULL, *lo = NULL;
    pc_status st = count2(w, h, sizeof(float), &n, err);
    if (st != PC_OK) {
        return st;
    }
    yl = alloc_f(n);
    g = alloc_f(n);
    if (yl == NULL || g == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory measuring edges");
        goto done;
    }
    for (x = 0; x < n; x++) {
        yl[x] = 0.299f * img->px[x][0] + 0.587f * img->px[x][1] + 0.114f * img->px[x][2];
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float gx = (yl[y * w + (x + 1 < w ? x + 1 : w - 1)] - yl[y * w + (x > 0 ? x - 1 : 0)]) / 2.0f;
            float gy = (yl[(y + 1 < h ? y + 1 : h - 1) * w + x] - yl[(y > 0 ? y - 1 : 0) * w + x]) / 2.0f;
            g[y * w + x] = sqrtf(gx * gx + gy * gy);
        }
    }
    if ((st = pc_algo_matting_max_square(g, w, h, r, true, &gm, err)) != PC_OK ||
        (st = pc_algo_matting_max_square(yl, w, h, r, true, &hi, err)) != PC_OK ||
        (st = pc_algo_matting_max_square(yl, w, h, r, false, &lo, err)) != PC_OK) {
        goto done;
    }
    for (x = 0; x < n; x++) {
        hi[x] = (hi[x] - lo[x]) / pc_rs_maxf_k(gm[x], 1e-4f);
    }
    *out_p = hi;
    hi = NULL;
done:
    free(yl);
    free(g);
    free(gm);
    free(hi);
    free(lo);
    return st;
}

/* ---- refine ---- */

pc_status pc_algo_matting_refine_buffer(const pc_algo_rgb_image *img, const float *m, size_t w, size_t h,
                                        const pc_algo_refine_params *p, float **out_p, pc_error *err)
{
    size_t n, i, x, y;
    float *a, *t = NULL, ss;
    int32_t d;
    pc_status st = count2(w, h, sizeof(double), &n, err);
    if (st != PC_OK) {
        return st;
    }
    a = alloc_f(n);
    if (a == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory refining a mask");
    }
    if (n > 0) {
        memcpy(a, m, n * sizeof(float));
    }
    if (uses_image(p) && img != NULL) {
        bool *bin = NULL, *boundary = NULL, any = false;
        float *dist = NULL, *q = NULL, *widths = NULL;
        size_t r = gf_radius(p);
        if (img->w != w || img->h != h) {
            free(a);
            return pc_fail(err, PC_ERR_INVALID, "guide image %zux%zu for a %zux%zu mask", img->w, img->h, w,
                           h);
        }
        bin = pc_malloc_array(n ? n : 1, sizeof(bool));
        boundary = pc_calloc_array(n ? n : 1, sizeof(bool));
        if (bin == NULL || boundary == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory refining a mask");
            goto img_done;
        }
        for (i = 0; i < n; i++) {
            bin[i] = m[i] >= 0.5f;
        }
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                bool v;
                i = y * w + x;
                v = bin[i];
                boundary[i] = (x > 0 && bin[i - 1] != v) || (x + 1 < w && bin[i + 1] != v) ||
                              (y > 0 && bin[i - w] != v) || (y + 1 < h && bin[i + w] != v);
                any |= boundary[i];
            }
        }
        if (any) {
            if ((st = pc_algo_selection_edt(boundary, w, h, &dist, err)) != PC_OK ||
                (st = pc_algo_matting_guided_filter_color(img, m, r, PC_ALGO_MATTING_EPS, &q, err)) !=
                    PC_OK) {
                goto img_done;
            }
            if (p->smart_radius && (st = pc_algo_matting_edge_width(img, r, &widths, err)) != PC_OK) {
                goto img_done;
            }
            for (i = 0; i < n; i++) {
                float reff =
                    widths != NULL ? pc_clampf(widths[i], 1.5f, pc_rs_maxf_k(p->radius, 1.5f)) : p->radius;
                float wgt = pc_clampf(reff + 1.0f - dist[i], 0.0f, 1.0f);
                if (wgt > 0.0f) {
                    a[i] = m[i] + (pc_clampf(q[i], 0.0f, 1.0f) - m[i]) * wgt;
                }
            }
        }
    img_done:
        free(bin);
        free(boundary);
        free(dist);
        free(q);
        free(widths);
        if (st != PC_OK) {
            free(a);
            return st;
        }
    }
    ss = smooth_sigma(p);
    if (ss >= 0.3f) {
        float gain = 1.0f + ss;
        st = pc_algo_matting_gaussian_blur(a, w, h, ss, &t, err);
        free(a);
        if (st != PC_OK) {
            return st;
        }
        a = t;
        for (i = 0; i < n; i++) {
            a[i] = pc_clampf((a[i] - 0.5f) * gain + 0.5f, 0.0f, 1.0f);
        }
    }
    st = pc_algo_matting_gaussian_blur(a, w, h, feather_sigma(p), &t, err);
    free(a);
    if (st != PC_OK) {
        return st;
    }
    a = t;
    if (p->contrast > 0.0f) {
        float gain = 1.0f / (1.0f - 0.99f * pc_clampf(p->contrast, 0.0f, 100.0f) / 100.0f);
        for (i = 0; i < n; i++) {
            a[i] = pc_clampf((a[i] - 0.5f) * gain + 0.5f, 0.0f, 1.0f);
        }
    }
    d = shift_px(p);
    if (d != 0) {
        st = pc_algo_matting_morph(a, w, h, (size_t)(d < 0 ? -d : d), d > 0, &t, err);
        free(a);
        if (st != PC_OK) {
            return st;
        }
        a = t;
    }
    *out_p = a;
    return PC_OK;
}

pc_status pc_algo_matting_region_reader(const void *ctx, pc_rect r, float *out, size_t n, pc_error *err)
{
    const pc_algo_region *reg = ctx;
    size_t i = 0;
    int32_t x, y;
    (void)err;
    for (y = r.y0; y < r.y1; y++) {
        for (x = r.x0; x < r.x1 && i < n; x++) {
            out[i++] = pc_algo_region_at(reg, x, y);
        }
    }
    return PC_OK;
}

pc_status pc_algo_matting_surface_reader(const void *ctx, pc_rect r, float *out, size_t n, pc_error *err)
{
    float *v = NULL;
    size_t len = 0;
    pc_status st = pc_algo_selection_mask_from_surface(ctx, r, &v, &len, err);
    if (st != PC_OK) {
        return st;
    }
    if (len > 0) {
        memcpy(out, v, (len < n ? len : n) * sizeof(float));
    }
    free(v);
    return PC_OK;
}

/* Tiles of TILE px covering area, row by row. */
static pc_status tiles_of(pc_rect area, pc_rect **out, size_t *n, pc_error *err)
{
    pc_rect *t = NULL;
    size_t cnt = 0, cap = 0;
    int64_t x, y;
    for (y = area.y0; y < area.y1; y += TILE) {
        for (x = area.x0; x < area.x1; x += TILE) {
            if (cnt == cap) {
                void *g = pc_grow_array(t, &cap, cnt + 1, sizeof *t);
                if (g == NULL) {
                    free(t);
                    return pc_fail(err, PC_ERR_NOMEM, "out of memory tiling a mask");
                }
                t = g;
            }
            t[cnt++] =
                pc_rect_make((int32_t)x, (int32_t)y, (int32_t)(x + TILE < area.x1 ? x + TILE : area.x1),
                             (int32_t)(y + TILE < area.y1 ? y + TILE : area.y1));
        }
    }
    *out = t;
    *n = cnt;
    return PC_OK;
}

typedef struct refine_ctx {
    const pc_algo_sampler *s;
    pc_algo_mask_fn mask;
    const void *mask_ctx;
    pc_rect canvas, bbox;
    const pc_algo_refine_params *p;
    int32_t halo;
    const pc_rect *tiles;
    uint8_t *out;
} refine_ctx;

static uint8_t q8(float v)
{
    return pc_f32_to_u8(roundf(pc_clampf(v, 0.0f, 1.0f) * 255.0f));
}

static pc_status refine_tile(refine_ctx *c, pc_rect t)
{
    pc_rect r = pc_rect_intersect(pc_rect_inflate(t, c->halo), c->canvas);
    size_t rw = pc_rect_width(r), rh = pc_rect_height(r), n = rw * rh, i, xx, yy;
    size_t cx = (size_t)(t.x0 - r.x0), cy = (size_t)(t.y0 - r.y0), cw = pc_rect_width(t),
           ch = pc_rect_height(t);
    size_t bw = pc_rect_width(c->bbox);
    float *m = alloc_f(n), *res = NULL, first;
    bool same = true;
    pc_algo_rgb_image img = {0, 0, NULL};
    pc_status st;
    if (m == NULL) {
        return PC_ERR_NOMEM;
    }
    st = c->mask(c->mask_ctx, r, m, n, NULL);
    if (st != PC_OK) {
        free(m);
        return st;
    }
    first = n > 0 ? m[0] : 0.0f;
    for (i = 0; i < n; i++) {
        if (m[i] != first) {
            same = false;
            break;
        }
    }
    if ((first == 0.0f || first == 1.0f) && same) {
        uint8_t v = q8(first);
        for (yy = 0; yy < ch; yy++) {
            memset(c->out + ((size_t)(t.y0 - c->bbox.y0) + yy) * bw + (size_t)(t.x0 - c->bbox.x0), v, cw);
        }
        free(m);
        return PC_OK;
    }
    if (uses_image(c->p)) {
        st = pc_algo_sampler_rgb(c->s, r, &img, NULL);
        if (st != PC_OK) {
            free(m);
            return st;
        }
    }
    st = pc_algo_matting_refine_buffer(uses_image(c->p) ? &img : NULL, m, rw, rh, c->p, &res, NULL);
    pc_algo_rgb_image_fini(&img);
    free(m);
    if (st != PC_OK) {
        return st;
    }
    for (yy = 0; yy < ch; yy++) {
        uint8_t *o = c->out + ((size_t)(t.y0 - c->bbox.y0) + yy) * bw + (size_t)(t.x0 - c->bbox.x0);
        for (xx = 0; xx < cw; xx++) {
            o[xx] = q8(res[(cy + yy) * rw + cx + xx]);
        }
    }
    free(res);
    return PC_OK;
}

static pc_status refine_tiles(void *ctx, size_t chunk, size_t begin, size_t end)
{
    refine_ctx *c = ctx;
    size_t i;
    (void)chunk;
    for (i = begin; i < end; i++) {
        pc_status st = refine_tile(c, c->tiles[i]);
        if (st != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

pc_status pc_algo_matting_refine_mask(const pc_algo_sampler *s, pc_algo_mask_fn mask, const void *mask_ctx,
                                      pc_rect content, pc_rect canvas, const pc_algo_refine_params *p,
                                      pc_algo_region *out, pc_error *err)
{
    refine_ctx c;
    pc_rect *tiles = NULL;
    size_t nt = 0, n;
    pc_algo_region reg;
    int64_t halo = halo64(p);
    pc_status st;
    content = pc_rect_intersect(content, canvas);
    if (pc_rect_is_empty(content)) {
        out->bbox = PC_RECT_EMPTY;
        out->mask = NULL;
        out->len = 0;
        return PC_OK;
    }
    if (halo > MAX_HALO || pc_algo_refine_spread(p) < 0) {
        return pc_fail(err, PC_ERR_LIMIT, "refine edge reach too large");
    }
    c.bbox = pc_rect_intersect(pc_rect_inflate(content, pc_algo_refine_spread(p)), canvas);
    st = count2(pc_rect_width(c.bbox), pc_rect_height(c.bbox), 1, &n, err);
    if (st != PC_OK) {
        return st;
    }
    st = tiles_of(c.bbox, &tiles, &nt, err);
    if (st != PC_OK) {
        return st;
    }
    c.out = pc_calloc_array(n ? n : 1, 1);
    if (c.out == NULL) {
        free(tiles);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory refining a selection");
    }
    c.s = s;
    c.mask = mask;
    c.mask_ctx = mask_ctx;
    c.canvas = canvas;
    c.p = p;
    c.halo = (int32_t)halo;
    c.tiles = tiles;
    st = pc_algo_par_for(nt, 1, refine_tiles, &c);
    free(tiles);
    if (st != PC_OK) {
        free(c.out);
        return pc_fail(err, st, "refining the selection failed");
    }
    reg.bbox = c.bbox;
    reg.mask = c.out;
    reg.len = n;
    return pc_algo_segment_trim_region(&reg, out, err);
}

pc_status pc_algo_matting_region_surface(const pc_algo_region *reg, pc_surface *out, pc_error *err)
{
    pc_status st;
    if (pc_rect_is_empty(reg->bbox)) {
        pc_surface_init(out, PC_PIXEL_FORMAT_GRAY8);
        return PC_OK;
    }
    st = pc_surface_from_interleaved(PC_PIXEL_FORMAT_GRAY8, reg->bbox, reg->mask, reg->len, out, err);
    if (st == PC_OK) {
        pc_surface_prune(out);
    }
    return st;
}

/* ---- decontaminate / masked copy ---- */

typedef struct tile_out {
    pc_rect t;
    float *data; /* NULL: tile skipped */
    size_t len;
} tile_out;

typedef struct decon_ctx {
    const pc_surface *src;
    const pc_algo_region *alpha;
    size_t n, nc, rad;
    bool has_alpha;
    int32_t halo;
    float amount;
    tile_out *res;
} decon_ctx;

static bool soft(float a)
{
    return a > 0.02f && a < 0.98f;
}

static pc_status decon_tile(decon_ctx *c, tile_out *to)
{
    pc_rect t = to->t, r;
    size_t rw, rh, rn, i, ch, xx, yy, cx, cy, cw, chh, n = c->n, nc = c->nc;
    float *px = NULL, *al = NULL, *wts = NULL, *den = NULL, *den2 = NULL, *v = NULL, *out = NULL;
    float **num1 = NULL, **num2 = NULL;
    bool any = false;
    int32_t x, y;
    pc_status st = PC_OK;
    for (y = t.y0; y < t.y1 && !any; y++) {
        for (x = t.x0; x < t.x1; x++) {
            if (soft(pc_algo_region_at(c->alpha, x, y))) {
                any = true;
                break;
            }
        }
    }
    if (!any) {
        return PC_OK;
    }
    r = pc_rect_inflate(t, c->halo);
    rw = pc_rect_width(r);
    rh = pc_rect_height(r);
    if (count2(rw, rh, sizeof(double) * (n > 0 ? n : 1), &rn, NULL) != PC_OK) {
        return PC_ERR_LIMIT;
    }
    st = pc_surface_read_region(c->src, r, &px, &i, NULL);
    if (st != PC_OK) {
        return st;
    }
    al = alloc_f(rn);
    wts = alloc_f(rn);
    v = alloc_f(rn);
    num1 = pc_calloc_array(nc ? nc : 1, sizeof(float *));
    num2 = pc_calloc_array(nc ? nc : 1, sizeof(float *));
    if (al == NULL || wts == NULL || v == NULL || num1 == NULL || num2 == NULL) {
        st = PC_ERR_NOMEM;
        goto done;
    }
    i = 0;
    for (y = r.y0; y < r.y1; y++) {
        for (x = r.x0; x < r.x1; x++) {
            al[i++] = pc_algo_region_at(c->alpha, x, y);
        }
    }
    for (i = 0; i < rn; i++) {
        wts[i] = al[i] >= 0.98f ? (c->has_alpha ? px[i * n + n - 1] : 1.0f) : 0.0f;
    }
    if ((st = pc_algo_matting_box_mean(wts, rw, rh, c->rad, &den, NULL)) != PC_OK ||
        (st = pc_algo_matting_box_mean(wts, rw, rh, c->rad * 4, &den2, NULL)) != PC_OK) {
        goto done;
    }
    for (ch = 0; ch < nc; ch++) {
        for (i = 0; i < rn; i++) {
            v[i] = wts[i] * px[i * n + ch];
        }
        if ((st = pc_algo_matting_box_mean(v, rw, rh, c->rad, &num1[ch], NULL)) != PC_OK ||
            (st = pc_algo_matting_box_mean(v, rw, rh, c->rad * 4, &num2[ch], NULL)) != PC_OK) {
            goto done;
        }
    }
    cx = (size_t)(t.x0 - r.x0);
    cy = (size_t)(t.y0 - r.y0);
    cw = pc_rect_width(t);
    chh = pc_rect_height(t);
    out = pc_malloc_array((cw * chh * n) > 0 ? cw * chh * n : 1, sizeof(float));
    if (out == NULL) {
        st = PC_ERR_NOMEM;
        goto done;
    }
    for (yy = 0; yy < chh; yy++) {
        for (xx = 0; xx < cw; xx++) {
            size_t ii = (cy + yy) * rw + cx + xx, o = (yy * cw + xx) * n;
            float a = al[ii];
            float k = soft(a) ? c->amount * pc_clampf((1.0f - a) * 4.0f, 0.0f, 1.0f) : 0.0f;
            for (ch = 0; ch < n; ch++) {
                float val = px[ii * n + ch], f;
                if (ch >= nc || k == 0.0f) {
                    out[o + ch] = val;
                    continue;
                }
                if (den[ii] > 1e-4f) {
                    f = num1[ch][ii] / den[ii];
                } else if (den2[ii] > 1e-4f) {
                    f = num2[ch][ii] / den2[ii];
                } else {
                    f = val;
                }
                out[o + ch] = val + (f - val) * k;
            }
        }
    }
    to->data = out;
    to->len = cw * chh * n;
    out = NULL;
done:
    if (num1 != NULL) {
        for (ch = 0; ch < nc; ch++) {
            free(num1[ch]);
        }
    }
    if (num2 != NULL) {
        for (ch = 0; ch < nc; ch++) {
            free(num2[ch]);
        }
    }
    free(num1);
    free(num2);
    free(px);
    free(al);
    free(wts);
    free(den);
    free(den2);
    free(v);
    free(out);
    return st;
}

static pc_status decon_tiles(void *ctx, size_t chunk, size_t begin, size_t end)
{
    decon_ctx *c = ctx;
    size_t i;
    (void)chunk;
    for (i = begin; i < end; i++) {
        pc_status st = decon_tile(c, &c->res[i]);
        if (st != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

static void free_results(tile_out *res, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        free(res[i].data);
    }
    free(res);
}

/* Runs fn over the tiles of area, then writes the produced tiles into dst in order. */
static pc_status run_tiles(pc_rect area, pc_pool_fn fn, void *ctx, tile_out **res_slot, pc_surface *dst,
                           pc_error *err)
{
    pc_rect *tiles = NULL;
    size_t nt = 0, i;
    tile_out *res;
    pc_status st = tiles_of(area, &tiles, &nt, err);
    if (st != PC_OK) {
        return st;
    }
    res = pc_calloc_array(nt ? nt : 1, sizeof *res);
    if (res == NULL) {
        free(tiles);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < nt; i++) {
        res[i].t = tiles[i];
    }
    free(tiles);
    *res_slot = res;
    st = pc_algo_par_for(nt, 1, fn, ctx);
    if (st != PC_OK) {
        free_results(res, nt);
        return pc_fail(err, st, "processing the selection's tiles failed");
    }
    for (i = 0; i < nt && st == PC_OK; i++) {
        if (res[i].data != NULL) {
            st = pc_surface_write_region(dst, res[i].t, res[i].data, res[i].len, err);
        }
    }
    free_results(res, nt);
    return st;
}

pc_status pc_algo_matting_decontaminate(const pc_surface *src, const pc_algo_region *alpha, float radius,
                                        float amount, pc_surface *out, pc_error *err)
{
    pc_pixel_format fmt = pc_surface_format(src);
    decon_ctx c;
    pc_surface o;
    float radf = ceilf(pc_rs_maxf_k(radius, 3.0f));
    pc_status st;
    c.src = src;
    c.alpha = alpha;
    c.n = pc_pixel_format_channels(fmt);
    c.has_alpha = fmt.alpha;
    c.nc = c.n - (fmt.alpha ? 1 : 0);
    if (!(radf <= (float)(MAX_HALO / 4))) {
        return pc_fail(err, PC_ERR_LIMIT, "decontamination radius too large");
    }
    c.rad = pc_f32_to_size(radf);
    c.halo = (int32_t)(c.rad * 4);
    c.amount = pc_clampf(amount, 0.0f, 100.0f) / 100.0f;
    c.res = NULL;
    st = pc_surface_clone(src, &o, err);
    if (st != PC_OK) {
        return st;
    }
    st = run_tiles(alpha->bbox, decon_tiles, &c, &c.res, &o, err);
    if (st != PC_OK) {
        pc_surface_fini(&o);
        return st;
    }
    *out = o;
    return PC_OK;
}

typedef struct masked_ctx {
    const pc_surface *src;
    const pc_algo_region *alpha;
    size_t n, m;
    bool has_alpha;
    tile_out *res;
} masked_ctx;

static pc_status masked_tiles(void *ctx, size_t chunk, size_t begin, size_t end)
{
    masked_ctx *c = ctx;
    size_t ti;
    (void)chunk;
    for (ti = begin; ti < end; ti++) {
        pc_rect t = c->res[ti].t;
        float *px = NULL, *out;
        size_t len = 0, cnt = (size_t)pc_rect_width(t) * pc_rect_height(t), i = 0, o = 0;
        bool any = false;
        int32_t x, y;
        pc_status st = pc_surface_read_region(c->src, t, &px, &len, NULL);
        if (st != PC_OK) {
            return st;
        }
        out = pc_malloc_array((cnt * c->m) > 0 ? cnt * c->m : 1, sizeof(float));
        if (out == NULL) {
            free(px);
            return PC_ERR_NOMEM;
        }
        for (y = t.y0; y < t.y1; y++) {
            for (x = t.x0; x < t.x1; x++) {
                float a = pc_algo_region_at(c->alpha, x, y);
                const float *p = px + i * c->n;
                float sa = c->has_alpha ? p[c->n - 1] : 1.0f;
                memcpy(out + o, p, (c->m - 1) * sizeof(float));
                out[o + c->m - 1] = sa * a;
                any |= sa * a > 0.0f;
                o += c->m;
                i++;
            }
        }
        free(px);
        if (any) {
            c->res[ti].data = out;
            c->res[ti].len = cnt * c->m;
        } else {
            free(out);
        }
    }
    return PC_OK;
}

pc_status pc_algo_matting_masked_copy(const pc_surface *src, const pc_algo_region *alpha, pc_surface *out,
                                      pc_error *err)
{
    pc_pixel_format fmt = pc_surface_format(src), ofmt = fmt;
    masked_ctx c;
    pc_surface o;
    pc_status st;
    ofmt.alpha = true;
    c.src = src;
    c.alpha = alpha;
    c.n = pc_pixel_format_channels(fmt);
    c.m = pc_pixel_format_channels(ofmt);
    c.has_alpha = fmt.alpha;
    c.res = NULL;
    pc_surface_init(&o, ofmt);
    st = run_tiles(alpha->bbox, masked_tiles, &c, &c.res, &o, err);
    if (st != PC_OK) {
        pc_surface_fini(&o);
        return st;
    }
    pc_surface_prune(&o);
    *out = o;
    return PC_OK;
}
