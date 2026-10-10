/* Filter > Other: Custom (5x5 convolution) and HSB/HSL (port of crates/algo/src/other2.rs; its
 * HSL/HSB decoders live in fxutil.c), and Filter > Video: De-Interlace and NTSC Colors (video.rs). */
#include <stdlib.h>
#include <string.h>

#include "algo/fx.h"
#include "algo/fx_defs.h"

/* ---- Custom ---- */

/* sum(kernel * pixel) / scale + offset / 255 per colour channel (alpha kept). Kernels shorter
 * than 25 entries are zero-padded. */
static pc_status custom_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                          const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_custom_params *cp = p;
    size_t n = src->ch, cc = pc_algo_ncol(ctx, n), nk = 0, i, c, j;
    int32_t kx[25], ky[25];
    float kw[25], s = cp->scale == 0.0f ? 1.0f : cp->scale, off = cp->offset / 255.0f;
    (void)prep, (void)err;
    pc_algo_image_crop_into(src, out, dst);
    for (i = 0; i < 25 && i < cp->kernel.n; i++) {
        float v = cp->kernel.v[i];
        if (v != 0.0f) {
            kx[nk] = (int32_t)(i % 5) - 2;
            ky[nk] = (int32_t)(i / 5) - 2;
            kw[nk] = v;
            nk++;
        }
    }
    if (nk == 0) {
        return PC_OK;
    }
    for (i = 0; i < pc_fx_npx(out); i++) {
        int32_t x, y;
        pc_algo_xy(out, i, &x, &y);
        for (c = 0; c < cc; c++) {
            float sum = -0.0f;
            for (j = 0; j < nk; j++) {
                sum += pc_algo_image_get(src, x + kx[j], y + ky[j], c) * kw[j];
            }
            dst[i * n + c] = sum / s + off;
        }
    }
    return PC_OK;
}

/* ---- HSB/HSL ---- */

static float hue(const float c[3], float mx, float d)
{
    float h;
    if (d <= 0.0f) {
        return 0.0f;
    }
    if (mx == c[0]) {
        h = pc_algo_rem_euclidf((c[1] - c[2]) / d, 6.0f);
    } else if (mx == c[1]) {
        h = (c[2] - c[0]) / d + 2.0f;
    } else {
        h = (c[0] - c[1]) / d + 4.0f;
    }
    return h / 6.0f;
}

void pc_algo_other2_rgb_to_hsb(const float c[3], float out[3])
{
    float mx = pc_rs_maxf(pc_rs_maxf(c[0], c[1]), c[2]), mn = pc_rs_minf(pc_rs_minf(c[0], c[1]), c[2]);
    float d = mx - mn;
    out[0] = hue(c, mx, d);
    out[1] = mx > 0.0f ? d / mx : 0.0f;
    out[2] = mx;
}

/* Reinterprets the three colour channels: encodes RGB as H, S, B/L in the R, G, B channels, or
 * decodes such channels back to RGB. */
static pc_status hsb_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                       const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_hsb_params *hp = p;
    size_t n = src->ch, i;
    bool native = ctx->mode == PC_COLOR_MODE_RGB;
    (void)prep, (void)err;
    pc_algo_image_crop_into(src, out, dst);
    if (hp->input == hp->output || n < (native ? 3u : 1u)) {
        return PC_OK;
    }
    for (i = 0; i < pc_fx_npx(out); i++) {
        float *px = dst + i * n, c4[4], c[3], rgb[3], o[3];
        if (native) {
            c4[0] = px[0], c4[1] = px[1], c4[2] = px[2], c4[3] = 1.0f;
        } else {
            pc_algo_rgba(ctx, px, n, c4);
        }
        c[0] = pc_clampf(c4[0], 0.0f, 1.0f);
        c[1] = pc_clampf(c4[1], 0.0f, 1.0f);
        c[2] = pc_clampf(c4[2], 0.0f, 1.0f);
        switch (hp->input) {
        case PC_ALGO_HSB_HSB:
            pc_algo_hsb_to_rgb(c, rgb);
            break;
        case PC_ALGO_HSB_HSL:
            pc_algo_hsl_to_rgb(c, rgb);
            break;
        default:
            memcpy(rgb, c, sizeof rgb);
            break;
        }
        switch (hp->output) {
        case PC_ALGO_HSB_HSB:
            pc_algo_other2_rgb_to_hsb(rgb, o);
            break;
        case PC_ALGO_HSB_HSL:
            pc_algo_rgb_to_hsl(rgb, o);
            break;
        default:
            memcpy(o, rgb, sizeof o);
            break;
        }
        if (native) {
            memcpy(px, o, sizeof o);
        } else {
            const float col[4] = {o[0], o[1], o[2], c4[3]};
            pc_algo_set_rgba(ctx, px, n, col);
        }
    }
    return PC_OK;
}

/* ---- De-Interlace ---- */

/* Removes one field (odd = lines 1, 3, 5... counted from the bounds' top, i.e. even offsets) and
 * rebuilds it from the neighbouring kept lines by duplication or (premultiplied) interpolation. */
static pc_status deinterlace_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                               const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_deinterlace_params *dp = p;
    size_t n = src->ch, i, c, r;
    pc_rect b = ctx->bounds;
    (void)prep, (void)err;
    pc_algo_image_crop_into(src, out, dst);
    if (n == 0 || n > PC_ALGO_MAXC) {
        return PC_OK;
    }
    for (i = 0; i < pc_fx_npx(out); i++) {
        float *px = dst + i * n, acc[PC_ALGO_MAXC] = {0}, k;
        int32_t x, y, rows[2];
        size_t cnt;
        bool odd_line, has_above, has_below;
        pc_algo_xy(out, i, &x, &y);
        if (!pc_rect_contains(b, x, y)) {
            continue;
        }
        /* 1-based line number parity: offset 0 is line 1 (odd). */
        odd_line = pc_i32_wrapping_sub(y, b.y0) % 2 == 0;
        if (odd_line == dp->eliminate_even) {
            continue;
        }
        has_above = y > b.y0;
        has_below = (int64_t)y + 1 < (int64_t)b.y1;
        if (dp->interpolate && has_above && has_below) {
            rows[0] = y - 1, rows[1] = y + 1, cnt = 2;
        } else if (has_above) {
            rows[0] = rows[1] = y - 1, cnt = 1;
        } else if (has_below) {
            rows[0] = rows[1] = y + 1, cnt = 1;
        } else {
            continue;
        }
        k = 1.0f / (float)cnt;
        for (r = 0; r < cnt; r++) {
            float a = ctx->alpha ? pc_algo_image_get(src, x, rows[r], n - 1) : 1.0f;
            for (c = 0; c < n; c++) {
                float s = pc_algo_image_get(src, x, rows[r], c);
                acc[c] += (ctx->alpha && c < n - 1 ? s * a : s) * k;
            }
        }
        pc_algo_unpremul_px(acc, n, ctx->alpha);
        memcpy(px, acc, n * sizeof(float));
    }
    return PC_OK;
}

/* ---- NTSC Colors ---- */

/* Composite-signal limits (fractions of the 7.5-100 IRE luma range): broadcast-safe is
 * -20..120 IRE, i.e. Y +- C within these bounds. */
#define NTSC_MAX ((120.0f - 7.5f) / 92.5f)
#define NTSC_MIN ((-20.0f - 7.5f) / 92.5f)

/* Scales chroma (in YIQ) so the composite signal Y +- C stays within broadcast limits; hue and luma
 * are kept. */
static pc_status ntsc_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                        const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    size_t n = src->ch, i;
    bool native = ctx->mode == PC_COLOR_MODE_RGB;
    (void)p, (void)prep, (void)err;
    pc_algo_image_crop_into(src, out, dst);
    if (n < (native ? 3u : 1u)) {
        return PC_OK;
    }
    for (i = 0; i < pc_fx_npx(out); i++) {
        float *px = dst + i * n, c[4], yy, ii, qq, chroma, k = 1.0f, i2, q2, rgb[3];
        if (native) {
            c[0] = px[0], c[1] = px[1], c[2] = px[2], c[3] = 1.0f;
        } else {
            pc_algo_rgba(ctx, px, n, c);
        }
        yy = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
        ii = 0.595716f * c[0] - 0.274453f * c[1] - 0.321263f * c[2];
        qq = 0.211456f * c[0] - 0.522591f * c[1] + 0.311135f * c[2];
        chroma = sqrtf(ii * ii + qq * qq);
        if (chroma <= 1e-7f) {
            continue;
        }
        if (yy + chroma > NTSC_MAX) {
            k = pc_rs_minf(k, pc_rs_maxf_k((NTSC_MAX - yy) / chroma, 0.0f));
        }
        if (yy - chroma < NTSC_MIN) {
            k = pc_rs_minf(k, pc_rs_maxf_k((yy - NTSC_MIN) / chroma, 0.0f));
        }
        if (k >= 1.0f) {
            continue;
        }
        i2 = ii * k;
        q2 = qq * k;
        rgb[0] = yy + 0.9563f * i2 + 0.6210f * q2;
        rgb[1] = yy - 0.2721f * i2 - 0.6474f * q2;
        rgb[2] = yy - 1.1070f * i2 + 1.7046f * q2;
        if (native) {
            memcpy(px, rgb, sizeof rgb);
        } else {
            const float col[4] = {rgb[0], rgb[1], rgb[2], c[3]};
            pc_algo_set_rgba(ctx, px, n, col);
        }
    }
    return PC_OK;
}

/* ---- halos and defs ---- */

static pc_algo_halo custom_halo(const void *p, const pc_rect *b)
{
    (void)p, (void)b;
    return pc_algo_halo_reach(2.0f);
}

static pc_algo_halo deinterlace_halo(const void *p, const pc_rect *b)
{
    (void)p, (void)b;
    return pc_algo_halo_reach(1.0f);
}

static const pc_algo_jfield CUSTOM_F[] = {
    PC_ALGO_JF("kernel", pc_algo_custom_params, kernel, PC_ALGO_JF_F32_VEC),
    PC_ALGO_JF("scale", pc_algo_custom_params, scale, PC_ALGO_JF_F32),
    PC_ALGO_JF("offset", pc_algo_custom_params, offset, PC_ALGO_JF_F32),
};
static const pc_algo_jfield HSB_F[] = {
    PC_ALGO_JFE("input", pc_algo_hsb_params, input, PC_ALGO_ENUM_HSB_MODEL, false),
    PC_ALGO_JFE("output", pc_algo_hsb_params, output, PC_ALGO_ENUM_HSB_MODEL, false),
};
static const pc_algo_jfield DEINTERLACE_F[] = {
    PC_ALGO_JF("eliminateEven", pc_algo_deinterlace_params, eliminate_even, PC_ALGO_JF_BOOL),
    PC_ALGO_JF("interpolate", pc_algo_deinterlace_params, interpolate, PC_ALGO_JF_BOOL),
};

PC_FX_DEF(pc_algo_def_custom, "custom", pc_algo_custom_params, CUSTOM_F, NULL, custom_halo, custom_k);
PC_FX_DEF(pc_algo_def_hsb_hsl, "hsbHsl", pc_algo_hsb_params, HSB_F, NULL, NULL, hsb_k);
PC_FX_DEF(pc_algo_def_deinterlace, "deInterlace", pc_algo_deinterlace_params, DEINTERLACE_F, NULL,
          deinterlace_halo, deinterlace_k);
PC_FX_UNIT(pc_algo_def_ntsc, "ntscColors", NULL, ntsc_k);
