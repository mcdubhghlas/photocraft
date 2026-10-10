/* Photographic relight (port of crates/algo/src/relight.rs). See relight.h. */
#include "algo/relight.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* Finite-difference step is sigma, but never more than this many pixels. */
#define STEP_MAX 32.0f
/* Finite differences at the shading scale: a 1 px step vanishes on large documents. */
#define SCALE 2.0f
#define DEG2RAD_F (3.14159265358979323846f / 180.0f)

float pc_algo_relight_sigma(float softness, pc_rect bounds)
{
    uint32_t w = pc_rect_width(bounds), h = pc_rect_height(bounds);
    float side = (float)(w < h ? w : h);
    float s = (pc_clampf(softness, 1.0f, 100.0f) / 100.0f) * 0.08f * pc_rs_maxf_k(side, 1.0f);
    return pc_clampf(s, PC_ALGO_RELIGHT_SIGMA_MIN, PC_ALGO_RELIGHT_SIGMA_MAX);
}

int32_t pc_algo_relight_step(float sigma)
{
    return pc_f32_to_i32(pc_clampf(roundf(sigma), 1.0f, STEP_MAX));
}

int32_t pc_algo_relight_read_radius(float sigma)
{
    int32_t blur = pc_i32_wrapping_add(pc_f32_to_i32(ceilf(pc_rs_maxf_k(sigma, 0.0f) * 3.0f)), 1);
    return pc_i32_sat_add(blur, pc_algo_relight_step(sigma));
}

int32_t pc_algo_relight_halo_radius(float softness, pc_rect bounds)
{
    if (pc_rect_is_empty(bounds)) {
        return pc_algo_relight_halo_radius_max();
    }
    return pc_algo_relight_read_radius(pc_algo_relight_sigma(softness, bounds));
}

int32_t pc_algo_relight_halo_radius_max(void)
{
    return pc_algo_relight_read_radius(PC_ALGO_RELIGHT_SIGMA_MAX);
}

static void norm3(const float v[3], float out[3])
{
    float m = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!isfinite(m) || m < 1e-9f) {
        out[0] = 0.0f, out[1] = 0.0f, out[2] = 1.0f;
    } else {
        out[0] = v[0] / m, out[1] = v[1] / m, out[2] = v[2] / m;
    }
}

/* Infinite-light direction: 0 degrees from the right, 90 from above (image space, y down). */
static void light_dir(float angle, float elevation, float out[3])
{
    float a = angle * DEG2RAD_F, s = sinf(a), co = cosf(a);
    float e = pc_clampf(elevation, 0.0f, 90.0f) * DEG2RAD_F;
    float v[3] = {co * cosf(e), -s * cosf(e), sinf(e)};
    norm3(v, out);
}

static void light_color(float warmth, float out[3])
{
    float w = pc_clampf(warmth, -100.0f, 100.0f) / 100.0f;
    if (w >= 0.0f) {
        out[0] = 1.0f + 0.15f * w, out[1] = 1.0f - 0.05f * w, out[2] = 1.0f - 0.25f * w;
    } else {
        float t = -w;
        out[0] = 1.0f - 0.20f * t, out[1] = 1.0f - 0.10f * t, out[2] = 1.0f + 0.15f * t;
    }
}

static bool finite_all(const float *v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (!isfinite(v[i])) {
            return false;
        }
    }
    return true;
}

static float sample_y(const float *buf, size_t w, size_t h, pc_rect rect, int32_t x, int32_t y)
{
    int32_t xx, yy;
    size_t ox, oy;
    float v;
    if (pc_rect_is_empty(rect) || w == 0 || h == 0) {
        return 0.04f;
    }
    xx = pc_clamp_i32(x, rect.x0, pc_i32_sat_sub(rect.x1, 1));
    yy = pc_clamp_i32(y, rect.y0, pc_i32_sat_sub(rect.y1, 1));
    ox = (size_t)((int64_t)xx - rect.x0);
    oy = (size_t)((int64_t)yy - rect.y0);
    if (ox >= w || oy >= h) {
        return 0.04f;
    }
    v = buf[oy * w + ox];
    return isfinite(v) ? v : 0.04f;
}

/* Approximate Gaussian of a single-channel field (three box passes); non-finite samples are held
 * out through a weight channel so they do not poison their neighbours. */
static pc_status blur_channel(const float *src, size_t w, size_t h, float sigma, float *out, pc_error *err)
{
    size_t n = w * h, i;
    float *packed = pc_calloc_array(n * 2 + 1, sizeof *packed);
    pc_status st;
    if (packed == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for the relight shading");
    }
    for (i = 0; i < n; i++) {
        if (isfinite(src[i])) {
            packed[2 * i] = src[i];
            packed[2 * i + 1] = 1.0f;
        }
    }
    st = pc_algo_gauss_blur_n(packed, w, h, 2, sigma, err);
    if (st == PC_OK) {
        for (i = 0; i < n; i++) {
            float acc = packed[2 * i], wgt = packed[2 * i + 1];
            out[i] = wgt > 1e-6f && isfinite(acc) && isfinite(wgt) ? acc / wgt : 0.5f;
        }
    }
    free(packed);
    return st;
}

static void read_rgba(const pc_algo_image *src, const pc_algo_ctx *ctx, int32_t x, int32_t y, float out[4])
{
    size_t n = src->ch < PC_ALGO_MAXC ? src->ch : PC_ALGO_MAXC, c;
    float tmp[PC_ALGO_MAXC];
    for (c = 0; c < n; c++) {
        tmp[c] = pc_algo_image_get(src, x, y, c);
    }
    pc_algo_rgba(ctx, tmp, n, out);
}

/* Relights `out` in place in res (which holds src's crop of `out`). */
static pc_status relight_rect(const pc_algo_relight *p, const pc_algo_image *src, pc_rect out,
                              const pc_algo_ctx *ctx, float sig, int32_t step, float *res, pc_error *err)
{
    size_t n = src->ch, yw, yh, n_px, i, npx;
    pc_rect y_rect = pc_rect_inflate(out, pc_algo_relight_read_radius(sig));
    float *ybuf = NULL, *sbuf = NULL, ldir[3], lcol[3], amb, inten;
    int32_t x, y;
    pc_status st;
    if (pc_rect_is_empty(y_rect)) {
        return PC_OK;
    }
    yw = pc_rect_width(y_rect);
    yh = pc_rect_height(y_rect);
    if (!pc_size_mul(yw, yh, &n_px) || n_px == 0) {
        return PC_OK;
    }
    if (n_px > PC_MAX_ALLOC / (2 * sizeof(float))) {
        return pc_fail(err, PC_ERR_LIMIT, "relight area too large");
    }
    ybuf = pc_malloc_array(n_px, sizeof *ybuf);
    sbuf = pc_malloc_array(n_px, sizeof *sbuf);
    if (ybuf == NULL || sbuf == NULL) {
        free(ybuf);
        free(sbuf);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for the relight shading");
    }
    for (y = y_rect.y0; y < y_rect.y1; y++) {
        for (x = y_rect.x0; x < y_rect.x1; x++) {
            float *slot = &ybuf[(size_t)((int64_t)y - y_rect.y0) * yw + (size_t)((int64_t)x - y_rect.x0)];
            float c[4], a;
            read_rgba(src, ctx, x, y, c);
            if (!finite_all(c, 4)) {
                *slot = NAN;
                continue;
            }
            a = ctx->alpha ? c[3] : 1.0f;
            *slot = a > 0.0f && isfinite(a) ? 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2] : NAN;
        }
    }
    st = blur_channel(ybuf, yw, yh, sig, sbuf, err);
    free(ybuf);
    if (st != PC_OK) {
        free(sbuf);
        return st;
    }
    light_dir(p->angle, p->elevation, ldir);
    light_color(p->warmth, lcol);
    amb = pc_clampf(isfinite(p->ambient) ? p->ambient : 0.0f, 0.0f, 100.0f) / 100.0f;
    inten = pc_clampf(p->intensity, 0.0f, 100.0f) / 100.0f;
    npx = (size_t)pc_rect_width(out) * pc_rect_height(out);
    for (i = 0; i < npx; i++) {
        float *px = res + i * n, a, c[4], s0, nx, ny, nrm[3], v[3], ndl, shade, oc[4];
        int k;
        pc_algo_xy(out, i, &x, &y);
        if (!finite_all(px, n)) {
            continue;
        }
        a = ctx->alpha ? px[n - 1] : 1.0f;
        if (ctx->alpha && a <= 0.0f) {
            continue;
        }
        pc_algo_rgba(ctx, px, n, c);
        if (!finite_all(c, 4)) {
            continue;
        }
        s0 = pc_rs_maxf_k(sample_y(sbuf, yw, yh, y_rect, x, y), 0.04f);
        nx = (sample_y(sbuf, yw, yh, y_rect, pc_i32_wrapping_sub(x, step), y) -
              sample_y(sbuf, yw, yh, y_rect, pc_i32_wrapping_add(x, step), y)) *
             SCALE;
        ny = (sample_y(sbuf, yw, yh, y_rect, x, pc_i32_wrapping_sub(y, step)) -
              sample_y(sbuf, yw, yh, y_rect, x, pc_i32_wrapping_add(y, step))) *
             SCALE;
        v[0] = nx, v[1] = ny, v[2] = 1.0f;
        norm3(v, nrm);
        ndl = pc_rs_maxf_k(nrm[0] * ldir[0] + nrm[1] * ldir[1] + nrm[2] * ldir[2], 0.0f);
        shade = amb + inten * ndl;
        memcpy(oc, c, sizeof oc);
        for (k = 0; k < 3; k++) {
            oc[k] = (c[k] / s0) * shade * lcol[k];
        }
        if (!finite_all(oc, 4)) {
            continue;
        }
        pc_algo_set_rgba(ctx, px, n, oc);
        if (ctx->alpha) {
            px[n - 1] = a;
        }
    }
    free(sbuf);
    return PC_OK;
}

/* Horizontal strip height so the inflated luma field stays within the budget; 0 = one rect. */
static int32_t luma_band_rows(pc_rect out, int32_t reach)
{
    size_t margin = 2 * (size_t)(reach > 0 ? reach : 0), yw, yh, n, max_yh, inner;
    yw = (size_t)pc_rect_width(out) + margin;
    yh = (size_t)pc_rect_height(out) + margin;
    if (yw == 0) {
        return 0;
    }
    if (!pc_size_mul(yw, yh, &n)) {
        n = SIZE_MAX;
    } else if (n == 0) {
        return 0;
    }
    if (n <= PC_ALGO_RELIGHT_MAX_LUMA_PIXELS) {
        return 0;
    }
    max_yh = PC_ALGO_RELIGHT_MAX_LUMA_PIXELS / yw;
    max_yh = max_yh < 1 ? 1 : max_yh;
    inner = max_yh > margin ? max_yh - margin : 0;
    inner = inner < 1 ? 1 : inner;
    inner = inner > INT32_MAX ? INT32_MAX : inner;
    if ((int64_t)inner >= (int64_t)pc_rect_height(out)) {
        return 0;
    }
    return (int32_t)inner;
}

static pc_status relight_kernel(const void *vp, const void *prep, const pc_algo_image *src, pc_rect out,
                                const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_relight *p = vp;
    size_t n = src->ch, row = (size_t)pc_rect_width(out) * n;
    float sig;
    int32_t step, band_h, y;
    uint32_t guard = 0;
    (void)prep;
    pc_algo_image_crop_into(src, out, dst);
    if (pc_rect_is_empty(out) || n == 0 || !isfinite(p->intensity) || p->intensity == 0.0f) {
        return PC_OK;
    }
    sig = pc_algo_relight_sigma(p->softness, ctx->bounds);
    step = pc_algo_relight_step(sig);
    band_h = luma_band_rows(out, pc_algo_relight_read_radius(sig));
    if (band_h == 0) {
        return relight_rect(p, src, out, ctx, sig, step, dst, err);
    }
    for (y = out.y0; y < out.y1 && guard < 1000000; guard++) {
        int32_t y1 = pc_i32_sat_add(y, band_h);
        pc_rect band;
        pc_status st;
        y1 = y1 < out.y1 ? y1 : out.y1;
        band = pc_rect_make(out.x0, y, out.x1, y1);
        if (pc_rect_is_empty(band)) {
            break;
        }
        st = relight_rect(p, src, band, ctx, sig, step, dst + (size_t)((int64_t)y - out.y0) * row, err);
        if (st != PC_OK) {
            return st;
        }
        y = y1;
    }
    return PC_OK;
}

/* halo(): 0 at identity, else the largest sigma's reach; halo_for(bounds) the actual reach. */
static pc_algo_halo relight_halo(const void *vp, const pc_rect *bounds)
{
    const pc_algo_relight *p = vp;
    if (bounds != NULL && p->intensity != 0.0f) {
        return pc_algo_halo_radius(pc_algo_relight_halo_radius(p->softness, *bounds));
    }
    if (p->intensity == 0.0f) {
        return pc_algo_halo_radius(0);
    }
    return pc_algo_halo_radius(pc_algo_relight_halo_radius_max());
}

static const pc_algo_jfield RELIGHT_F[] = {
    PC_ALGO_JF("angle", pc_algo_relight, angle, PC_ALGO_JF_F32),
    PC_ALGO_JF("elevation", pc_algo_relight, elevation, PC_ALGO_JF_F32),
    PC_ALGO_JF("intensity", pc_algo_relight, intensity, PC_ALGO_JF_F32),
    PC_ALGO_JF("ambient", pc_algo_relight, ambient, PC_ALGO_JF_F32),
    PC_ALGO_JF("warmth", pc_algo_relight, warmth, PC_ALGO_JF_F32),
    PC_ALGO_JF("softness", pc_algo_relight, softness, PC_ALGO_JF_F32),
};

const pc_algo_filter_def PC_ALGO_FILTER_RELIGHT = {
    .tag = "relight",
    .params_size = sizeof(pc_algo_relight),
    .fields = RELIGHT_F,
    .nfields = sizeof RELIGHT_F / sizeof RELIGHT_F[0],
    .halo = relight_halo,
    .kernel = relight_kernel,
};
