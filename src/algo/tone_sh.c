/* Image > Adjustments > Shadows/Highlights as Photoshop 25.4 does it (port of
 * crates/algo/src/tone/shadows_highlights.rs; measured on Photoshop as a black box).
 *
 * - Neighbourhood brightness: per pixel the mean of its brightest channel and its Rec. 601 luma,
 *   averaged edge-aware (a Gaussian of sigma = 1.1424 * Radius whose weights fall linearly with
 *   the difference from the pixel's own lightly blurred brightness; piecewise-linear bilateral
 *   filter of Durand and Dorsey 2002 on a coarser grid), quantised to 8 bits.
 * - Tonal mask: 1 - I_L(alpha, beta) of a Beta distribution fitted per Tone.
 * - Curve: 1 - (1 - v)^q blended between q = 1, 2, 4, 8 by a strength set by Amount.
 * - Colour: only the new luma is kept; the colour differences are added back scaled by Color and
 *   shortened where they would leave the gamut. Midtone is an S-curve on the luma. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "algo/tone.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

#define SH_SPATIAL 1.1424f /* neighbourhood sigma per pixel of Radius */
#define SH_GUIDE 1.3559f   /* sigma of the blur of the pixel's own brightness */
#define SH_RANGE 1.0152f   /* brightness difference at which a neighbour's weight reaches 0 */
#define SH_LEVELS 17       /* brightness levels of the piecewise-linear average */

static float sh_luma(const float c[3])
{
    return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
}

static size_t sh_grain(size_t w)
{
    size_t g = w > 0 ? 8192 / w : 1;
    return g > 0 ? g : 1;
}

typedef struct nb_ctx {
    const float *avg, *guide;
    float *out;
    size_t w, rw, rh, d, cells;
    float step;
} nb_ctx;

static float nb_at(const nb_ctx *k, size_t lvl, size_t x0, size_t x1, size_t y0, size_t y1, float tx,
                   float ty)
{
    const float *a = k->avg + lvl * k->cells;
    float top = a[y0 * k->rw + x0] + (a[y0 * k->rw + x1] - a[y0 * k->rw + x0]) * tx;
    float bot = a[y1 * k->rw + x0] + (a[y1 * k->rw + x1] - a[y1 * k->rw + x0]) * tx;
    return top + (bot - top) * ty;
}

static pc_status nb_rows(void *vctx, size_t chunk, size_t begin, size_t end)
{
    const nb_ctx *k = vctx;
    size_t y, x;
    float df = (float)k->d;
    (void)chunk;
    for (y = begin; y < end; y++) {
        float fy = pc_clampf(((float)y + 0.5f) / df - 0.5f, 0.0f, (float)(k->rh - 1));
        size_t y0 = pc_f32_to_size(floorf(fy)), y1 = y0 + 1 < k->rh ? y0 + 1 : k->rh - 1;
        float ty = fy - truncf(fy);
        for (x = 0; x < k->w; x++) {
            float fx = pc_clampf(((float)x + 0.5f) / df - 0.5f, 0.0f, (float)(k->rw - 1));
            size_t x0 = pc_f32_to_size(floorf(fx)), x1 = x0 + 1 < k->rw ? x0 + 1 : k->rw - 1, lvl;
            float tx = fx - truncf(fx), t, f;
            t = pc_rs_minf_k(pc_clampf(k->guide[y * k->w + x], 0.0f, 1.0f) / k->step,
                             (float)(SH_LEVELS - 1) - 1e-4f);
            lvl = pc_f32_to_size(floorf(t));
            f = t - (float)lvl;
            k->out[y * k->w + x] = nb_at(k, lvl, x0, x1, y0, y1, tx, ty) * (1.0f - f) +
                                   nb_at(k, lvl + 1, x0, x1, y0, y1, tx, ty) * f;
        }
    }
    return PC_OK;
}

/* Edge-aware neighbourhood brightness of lum (w x h) for Photoshop's Radius; *out malloc'd. */
static pc_status neighbourhood(const float *lum, size_t w, size_t h, float radius, float **out, pc_error *err)
{
    size_t n = w * h, d, rw, rh, cells, k, i, sz;
    float *guide, *avg = NULL, *buf = NULL, *res = NULL, sigma, step;
    pc_status st;
    nb_ctx c;
    guide = pc_malloc_array(n, sizeof *guide);
    if (guide == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for Shadows/Highlights");
    }
    memcpy(guide, lum, n * sizeof *guide);
    st = pc_algo_gauss_blur_n(guide, w, h, 1, SH_GUIDE, err);
    if (st != PC_OK) {
        free(guide);
        return st;
    }
    sigma = SH_SPATIAL * radius;
    if (sigma < 0.3f) {
        *out = guide;
        return PC_OK;
    }
    d = pc_f32_to_size(floorf(sigma / 4.0f));
    d = d > 0 ? d : 1;
    rw = w / d + (w % d != 0);
    rh = h / d + (h % d != 0);
    cells = rw * rh;
    step = 1.0f / (float)(SH_LEVELS - 1);
    if (!pc_size_mul(cells, SH_LEVELS, &sz) || (avg = pc_malloc_array(sz, sizeof *avg)) == NULL ||
        (buf = pc_malloc_array(cells, 2 * sizeof *buf)) == NULL ||
        (res = pc_malloc_array(n, sizeof *res)) == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for Shadows/Highlights");
        goto cleanup;
    }
    for (k = 0; k < SH_LEVELS; k++) {
        float g = (float)k * step, *o = avg + k * cells;
        memset(buf, 0, 2 * cells * sizeof *buf);
        for (i = 0; i < n; i++) {
            float l = lum[i];
            float wgt = pc_rs_maxf_k(1.0f - fabsf(l - g) / SH_RANGE, 0.0f);
            size_t cc = 2 * ((i / w) / d * rw + (i % w) / d);
            buf[cc] += wgt * l;
            buf[cc + 1] += wgt;
        }
        st = pc_algo_gauss_blur_n(buf, rw, rh, 2, sigma / (float)d, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        for (i = 0; i < cells; i++) {
            o[i] = buf[2 * i + 1] > 1e-6f ? buf[2 * i] / buf[2 * i + 1] : g;
        }
    }
    c.avg = avg, c.guide = guide, c.out = res, c.w = w, c.rw = rw, c.rh = rh, c.d = d, c.cells = cells;
    c.step = step;
    st = pc_algo_par_for(h, sh_grain(w), nb_rows, &c);
    if (st != PC_OK) {
        st = pc_fail(err, st, "Shadows/Highlights failed");
        goto cleanup;
    }
    *out = res;
    res = NULL;
cleanup:
    free(guide), free(avg), free(buf), free(res);
    return st;
}

/* Beta(alpha, beta) of the tonal mask for Tone 0, 5, ..., 95. */
static const float MASK_BETA[20][2] = {
    {1.9468f, 22.9366f}, {1.7770f, 18.3471f}, {1.6403f, 14.7559f}, {1.5270f, 11.9346f}, {1.4269f, 9.6467f},
    {1.3470f, 7.8430f},  {1.2807f, 6.4038f},  {1.2240f, 5.2384f},  {1.1772f, 4.2967f},  {1.1373f, 3.5350f},
    {1.1021f, 2.9109f},  {1.0755f, 2.4186f},  {1.0508f, 2.0189f},  {1.0307f, 1.7034f},  {1.0140f, 1.4583f},
    {1.0015f, 1.2758f},  {1.0f, 1.1403f},     {1.0f, 1.0510f},     {1.0f, 1.0002f},     {1.0f, 1.0f},
};

/* The tonal mask for tone (0..=100) at the 256 neighbourhood levels: 1 - I_L(alpha, beta). */
void pc_algo_sh_tonal_mask(float tone, float mask[256])
{
    enum { SUB = 16, NSTEP = 255 * SUB };
    float t = pc_clampf(tone, 0.0f, 95.0f) / 5.0f, f;
    size_t i = pc_f32_to_size(floorf(t)), j, k;
    double a, b, cum[256], acc = 0.0, total;
    i = i < 18 ? i : 18;
    f = t - (float)i;
    a = (double)(MASK_BETA[i][0] + (MASK_BETA[i + 1][0] - MASK_BETA[i][0]) * f);
    b = (double)(MASK_BETA[i][1] + (MASK_BETA[i + 1][1] - MASK_BETA[i][1]) * f);
    memset(cum, 0, sizeof cum);
    for (j = 0; j < NSTEP; j++) {
        double x = ((double)j + 0.5) / (double)NSTEP;
        acc += exp((a - 1.0) * log(x) + (b - 1.0) * log(1.0 - x));
        if ((j + 1) % SUB == 0) {
            cum[(j + 1) / SUB] = acc;
        }
    }
    total = pc_rs_max_k(acc, 2.2250738585072014e-308);
    for (k = 0; k < 256; k++) {
        mask[k] = (float)(1.0 - cum[k] / total);
    }
}

/* Curve strength for Amount a (0..=100): 0 at 0, 1 near 15, 2 at 50, 3 at 100. */
float pc_algo_sh_strength(float a)
{
    float u = powf(pc_clampf(a, 0.0f, 100.0f) / 100.0f, 0.6216f);
    return u <= 0.3f ? u / 0.3f : (1.0f + 20.0f * u) / 7.0f;
}

/* f32::powi with a run-time exponent (compiler-rt's __powisf2: square and multiply). */
static float powi_rt(float a, int e)
{
    float r = 1.0f;
    for (;;) {
        if (e & 1) {
            r *= a;
        }
        e /= 2;
        if (e == 0) {
            break;
        }
        a *= a;
    }
    return r;
}

/* The full-strength curve of strength s: 1 - (1 - v)^q between q = 2^floor(s) and 2^(floor(s)+1). */
float pc_algo_sh_lift(float s, float v)
{
    int32_t n = pc_f32_to_i32(floorf(s));
    float t, c, lo, hi;
    n = n < 0 ? 0 : (n > 2 ? 2 : n);
    t = pc_clampf(s - (float)n, 0.0f, 1.0f);
    c = pc_clampf(1.0f - v, 0.0f, 1.0f);
    lo = powi_rt(c, 1 << n);
    hi = lo * lo;
    return 1.0f - ((1.0f - t) * lo + t * hi);
}

/* Linear interpolation in a table of (x, y) sorted by x. */
static float interp(const float (*table)[2], size_t n, float x)
{
    size_t i;
    if (n == 0) {
        return 0.0f;
    }
    if (x <= table[0][0]) {
        return table[0][1];
    }
    for (i = 0; i + 1 < n; i++) {
        float xa = table[i][0], ya = table[i][1], xb = table[i + 1][0], yb = table[i + 1][1];
        if (x <= xb) {
            return ya + (yb - ya) * (x - xa) / (xb - xa);
        }
    }
    return table[n - 1][1];
}

/* Color: h of the colour-difference gain for positive values (exp(h F)) and negative (1 + h F). */
static const float COLOR_UP[12][2] = {{0.0f, 0.0f},     {1.0f, 0.1566f},  {2.0f, 0.2271f},
                                      {5.0f, 0.3776f},  {10.0f, 0.5653f}, {20.0f, 0.8669f},
                                      {30.0f, 1.1299f}, {40.0f, 1.3748f}, {50.0f, 1.6093f},
                                      {60.0f, 1.8375f}, {75.0f, 2.1721f}, {100.0f, 2.7193f}};
static const float COLOR_DOWN[9][2] = {{0.0f, 0.0f},      {1.0f, -0.0145f},  {5.0f, -0.0709f},
                                       {10.0f, -0.1373f}, {20.0f, -0.2570f}, {30.0f, -0.3612f},
                                       {50.0f, -0.5312f}, {75.0f, -0.6866f}, {100.0f, -0.7968f}};
/* Midtone: (value, k) and (value, p) of k (Y - 1/2) (4Y(1 - Y))^p. */
static const float MID_K[11][2] = {{-100.0f, -0.6955f}, {-75.0f, -0.5873f}, {-50.0f, -0.4429f},
                                   {-25.0f, -0.2514f},  {-10.0f, -0.1088f}, {0.0f, 0.0f},
                                   {10.0f, 0.1206f},    {25.0f, 0.3262f},   {50.0f, 0.7423f},
                                   {75.0f, 1.2635f},    {100.0f, 1.9032f}};
static const float MID_P[11][2] = {{-100.0f, 2.267f}, {-75.0f, 2.362f}, {-50.0f, 2.492f}, {-25.0f, 2.667f},
                                   {-10.0f, 2.803f},  {0.0f, 2.9f},     {10.0f, 3.016f},  {25.0f, 3.216f},
                                   {50.0f, 3.625f},   {75.0f, 4.160f},  {100.0f, 4.850f}};

static float midtone(float m, float y)
{
    float k, p;
    if (m == 0.0f) {
        return y;
    }
    k = interp(MID_K, 11, m);
    p = interp(MID_P, 11, m);
    return pc_clampf(y + k * (y - 0.5f) * powf(pc_rs_maxf_k(4.0f * y * (1.0f - y), 0.0f), p), 0.0f, 1.0f);
}

static size_t level(float v)
{
    return pc_f32_to_size(roundf(pc_clampf(v, 0.0f, 1.0f) * 255.0f));
}

typedef struct sh_ctx {
    float (*px)[4];
    const float *ls, *lh;
    float *y2;
    const float *ms, *mh;
    size_t w;
    float ss, hs, mid, color;
} sh_ctx;

static pc_status luma_rows(void *vctx, size_t chunk, size_t begin, size_t end)
{
    const sh_ctx *k = vctx;
    size_t i;
    (void)chunk;
    for (i = begin * k->w; i < end * k->w; i++) {
        const float *q = k->px[i];
        float adj[3] = {q[0], q[1], q[2]};
        int c;
        if (k->ls != NULL) {
            float m = k->ms[level(k->ls[i])];
            for (c = 0; c < 3; c++) {
                adj[c] += m * (pc_algo_sh_lift(k->ss, adj[c]) - adj[c]);
            }
        }
        if (k->lh != NULL) {
            float m = k->mh[255 - level(k->lh[i])];
            for (c = 0; c < 3; c++) {
                adj[c] -= m * (pc_algo_sh_lift(k->hs, 1.0f - q[c]) - (1.0f - q[c]));
            }
        }
        k->y2[i] = midtone(k->mid, pc_clampf(sh_luma(adj), 0.0f, 1.0f));
    }
    return PC_OK;
}

static float gain(float color, float f)
{
    if (color >= 0.0f) {
        return expf(interp(COLOR_UP, 12, color) * f);
    }
    return 1.0f + interp(COLOR_DOWN, 9, -color) * f;
}

static pc_status colour_rows(void *vctx, size_t chunk, size_t begin, size_t end)
{
    const sh_ctx *k = vctx;
    size_t i;
    (void)chunk;
    for (i = begin * k->w; i < end * k->w; i++) {
        float *q = k->px[i], y1, yn, f, g, d[3], kk = 1.0f;
        int c;
        if (q[3] <= 0.0f) {
            continue;
        }
        y1 = sh_luma(q);
        yn = k->y2[i];
        f = fabsf(yn - y1) / pc_rs_maxf_k(pc_rs_maxf(yn, y1), 1e-6f);
        g = gain(k->color, f);
        for (c = 0; c < 3; c++) {
            d[c] = (q[c] - y1) * g;
        }
        /* Shorten the colour difference where it would leave 0..=1. */
        for (c = 0; c < 3; c++) {
            if (d[c] > 1e-9f) {
                kk = pc_rs_minf(kk, (1.0f - yn) / d[c]);
            } else if (d[c] < -1e-9f) {
                kk = pc_rs_minf(kk, yn / -d[c]);
            }
        }
        kk = pc_clampf(kk, 0.0f, 1.0f);
        for (c = 0; c < 3; c++) {
            q[c] = pc_clampf(yn + kk * d[c], 0.0f, 1.0f);
        }
    }
    return PC_OK;
}

/* Black and White Clip: stretches the new luma so black% of pixels reach 0 and white% reach 1. */
static void clip_levels(float *y2, const float (*px)[4], size_t n, float black, float white)
{
    static const size_t NB = 4096;
    uint32_t *hist, cnt = 0;
    float lo = 0.0f, hi = 1.0f;
    size_t i;
    int which;
    if (black <= 0.0f && white <= 0.0f) {
        return;
    }
    hist = calloc(NB, sizeof *hist);
    if (hist == NULL) { /* 16 KiB; without it the clip is skipped */
        return;
    }
    for (i = 0; i < n; i++) {
        if (px[i][3] > 0.0f) {
            hist[pc_f32_to_size(roundf(pc_clampf(y2[i], 0.0f, 1.0f) * 4095.0f))]++;
            cnt++;
        }
    }
    if (cnt == 0) {
        free(hist);
        return;
    }
    for (which = 0; which < 2; which++) {
        float pct = which == 0 ? black : white, found = which == 0 ? 1.0f : 0.0f;
        uint32_t target, acc = 0;
        size_t kk;
        if (!(pct > 0.0f)) {
            continue;
        }
        target = pc_f32_to_u32(pc_clampf(pct, 0.0f, 50.0f) / 100.0f * (float)cnt);
        for (kk = 0; kk < NB; kk++) {
            size_t idx = which == 1 ? NB - 1 - kk : kk;
            acc += hist[idx];
            if (acc > target) {
                found = (float)idx / 4095.0f;
                break;
            }
        }
        if (which == 0) {
            lo = found;
        } else {
            hi = found;
        }
    }
    free(hist);
    if (hi - lo < 1e-3f) {
        return;
    }
    for (i = 0; i < n; i++) {
        y2[i] = pc_clampf((y2[i] - lo) / (hi - lo), 0.0f, 1.0f);
    }
}

pc_status pc_algo_shadows_highlights(float (*px)[4], size_t n, size_t w, size_t h,
                                     const pc_algo_shadows_highlights_params *p, pc_error *err)
{
    float *lum = NULL, *ls = NULL, *lh = NULL, *y2 = NULL, ms[256], mh[256];
    size_t i, wh;
    sh_ctx k;
    pc_status st = PC_OK;
    if (n == 0) {
        return PC_OK;
    }
    if (!pc_size_mul(w, h, &wh) || wh != n) {
        return pc_fail(err, PC_ERR_INVALID, "%zu pixels given for a %zux%zu image", n, w, h);
    }
    lum = pc_malloc_array(n, sizeof *lum);
    y2 = pc_malloc_array(n, sizeof *y2);
    if (lum == NULL || y2 == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for Shadows/Highlights");
        goto cleanup;
    }
    for (i = 0; i < n; i++) {
        const float *q = px[i];
        lum[i] = (pc_rs_maxf(pc_rs_maxf(q[0], q[1]), q[2]) + sh_luma(q)) / 2.0f;
    }
    if (p->shadow_amount > 0.0f) {
        st = neighbourhood(lum, w, h, p->shadow_radius, &ls, err);
        if (st != PC_OK) {
            goto cleanup;
        }
    }
    if (p->highlight_amount > 0.0f) {
        if (ls != NULL && fabsf(p->highlight_radius - p->shadow_radius) < 1e-3f) {
            lh = ls;
        } else {
            st = neighbourhood(lum, w, h, p->highlight_radius, &lh, err);
            if (st != PC_OK) {
                goto cleanup;
            }
        }
    }
    pc_algo_sh_tonal_mask(p->shadow_tone, ms);
    pc_algo_sh_tonal_mask(p->highlight_tone, mh);
    k.px = px, k.ls = ls, k.lh = lh, k.y2 = y2, k.ms = ms, k.mh = mh, k.w = w;
    k.ss = pc_algo_sh_strength(p->shadow_amount), k.hs = pc_algo_sh_strength(p->highlight_amount);
    k.mid = p->midtone, k.color = p->color;
    st = pc_algo_par_for(h, sh_grain(w), luma_rows, &k);
    if (st == PC_OK) {
        clip_levels(y2, (const float (*)[4])px, n, p->black_clip, p->white_clip);
        st = pc_algo_par_for(h, sh_grain(w), colour_rows, &k);
    }
    if (st != PC_OK) {
        st = pc_fail(err, st, "Shadows/Highlights failed");
    }
cleanup:
    if (lh != ls) {
        free(lh);
    }
    free(ls), free(lum), free(y2);
    return st;
}
