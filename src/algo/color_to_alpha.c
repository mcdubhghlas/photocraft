/* Filter > Other > Color to Alpha (port of crates/algo/src/color_to_alpha.rs): removes a known
 * colour from the pixels, turning it into transparency while keeping what was mixed with it.
 *
 * A pixel P that is a semi-transparent colour Q composited over a background colour C obeys
 * P = a*Q + (1 - a)*C. Knowing only P and C, the most transparent solution (the smallest a that
 * keeps Q inside 0..1) is per channel a_i = (P_i - C_i) / (1 - C_i) where P_i > C_i,
 * (C_i - P_i) / C_i where P_i < C_i, 0 where equal, and a = max_i a_i; then Q = C + (P - C) / a
 * and the layer's alpha is multiplied by a. The thresholds shape a: at or below `transparency`
 * fully transparent, at or above `opacity` fully opaque, linear in between. The arithmetic runs
 * on the stored (encoded) channel values, the space Normal blending composites in. */
#include "algo/color_to_alpha.h"

#include <math.h>

#include "algo/fx.h"
#include "algo/fx_defs.h"
#include "algo/fxutil.h"
#include "base/rmath.h"

float pc_algo_c2a_opacity(const float *colour, const float *key, size_t n)
{
    float a = 0.0f;
    size_t i;
    for (i = 0; i < n; i++) {
        float p = colour[i], c = key[i], ai;
        if (p > c) {
            /* Past a key at the top of the range (1 - C = 0) no in-range colour reaches P. */
            ai = c < 1.0f ? (p - c) / (1.0f - c) : 1.0f;
        } else if (p < c) {
            ai = c > 0.0f ? (c - p) / c : 1.0f;
        } else {
            ai = 0.0f;
        }
        if (isfinite(ai)) {
            a = pc_rs_maxf(a, ai);
        }
    }
    return pc_clampf(a, 0.0f, 1.0f);
}

float pc_algo_c2a_remap(float a, float transparency, float opacity)
{
    if (a <= transparency) {
        return 0.0f;
    }
    if (a >= opacity) {
        return 1.0f;
    }
    /* Here transparency < a < opacity, so the span is positive. */
    return pc_clampf((a - transparency) / (opacity - transparency), 0.0f, 1.0f);
}

float pc_algo_c2a_unmix(float *colour, const float *key, size_t n, float transparency,
                        float opacity_threshold)
{
    float a = pc_algo_c2a_remap(pc_algo_c2a_opacity(colour, key, n), transparency, opacity_threshold);
    size_t i;
    if (a >= 1.0f) {
        return 1.0f;
    }
    for (i = 0; i < n; i++) {
        float c = key[i];
        colour[i] = a <= 0.0f ? c : pc_clampf(c + (colour[i] - c) / a, 0.0f, 1.0f);
    }
    return pc_rs_maxf_k(a, 0.0f);
}

static pc_status c2a_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                       const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_color_to_alpha_params *cp = p;
    size_t n = src->ch, cc, i, k;
    float key[PC_ALGO_MAXC], c4[4];
    (void)prep, (void)err;
    pc_algo_image_crop_into(src, out, dst);
    if (!ctx->alpha || n < 2) {
        return PC_OK;
    }
    cc = pc_algo_ncol(ctx, n);
    cc = cc < PC_ALGO_MAXC ? cc : PC_ALGO_MAXC;
    c4[0] = cp->color[0], c4[1] = cp->color[1], c4[2] = cp->color[2], c4[3] = 1.0f;
    pc_algo_native(ctx, c4, key);
    /* zip(colour, key): the shorter of the colour channels and the key. */
    k = cc < n - 1 ? cc : n - 1;
    for (i = 0; i < pc_fx_npx(out); i++) {
        float *px = dst + i * n;
        px[n - 1] *= pc_algo_c2a_unmix(px, key, k, cp->transparency_threshold, cp->opacity_threshold);
    }
    return PC_OK;
}

static const pc_algo_jfield C2A_F[] = {
    PC_ALGO_JFA("color", pc_algo_color_to_alpha_params, color, 4, false),
    PC_ALGO_JF("transparencyThreshold", pc_algo_color_to_alpha_params, transparency_threshold,
               PC_ALGO_JF_F32),
    PC_ALGO_JF("opacityThreshold", pc_algo_color_to_alpha_params, opacity_threshold, PC_ALGO_JF_F32),
};

PC_FX_DEF(pc_algo_def_color_to_alpha, "colorToAlpha", pc_algo_color_to_alpha_params, C2A_F, NULL, NULL,
          c2a_k);
