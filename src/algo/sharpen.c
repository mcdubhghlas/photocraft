/* Unsharp Mask, Smart Sharpen (basic) and High Pass (port of crates/algo/src/sharpen.rs): each
 * compares a pixel with its Gaussian blur (the direct kernel at any radius). */
#include "algo/blur_int.h"

#include <math.h>
#include <stdlib.h>

#include "algo/fxutil.h"
#include "base/mem.h"
#include "base/num.h"

typedef enum sharp_kind { SHARP_UNSHARP, SHARP_SMART, SHARP_HIGH_PASS } sharp_kind;

typedef struct sharp_args {
    sharp_kind kind;
    float k, t, nr;
} sharp_args;

static float apply_one(const sharp_args *s, float o, float b)
{
    float d = o - b;
    switch (s->kind) {
    case SHARP_UNSHARP:
        return fabsf(d) + 1e-6f >= s->t ? o + s->k * d : o;
    case SHARP_SMART: {
        /* Suppress small (noise-level) differences. */
        float keep = 1.0f - s->nr * expf(-(fabsf(d) * 40.0f));
        return o + s->k * d * keep;
    }
    case SHARP_HIGH_PASS:
        return 0.5f + (o - b);
    }
    return o;
}

static pc_status with_blur(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx, float radius,
                           const sharp_args *s, float *dst, pc_error *err)
{
    size_t n = src->ch, cc = pc_algo_ncol(ctx, n), npx = (size_t)pc_rect_width(out) * pc_rect_height(out), nk,
           i, c, len;
    float *k, *blurred;
    pc_status st;
    if (n == 0) {
        return PC_OK;
    }
    st = pc_algo_gaussian_kernel(radius, &k, &nk, err);
    if (st != PC_OK) {
        return st;
    }
    if (!pc_algo_float_count(out, n, &len)) {
        free(k);
        return pc_fail(err, PC_ERR_LIMIT, "sharpen area too large");
    }
    blurred = pc_calloc_array(len > 0 ? len : 1, sizeof(float));
    if (blurred == NULL) {
        free(k);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for sharpening");
    }
    st = pc_algo_conv_sep(src, out, k, nk, k, nk, ctx->alpha, blurred, err);
    free(k);
    if (st != PC_OK) {
        free(blurred);
        return st;
    }
    pc_algo_image_crop_into(src, out, dst);
    for (i = 0; i < npx; i++) {
        float *px = dst + i * n;
        const float *bl = blurred + i * n;
        if (ctx->alpha && px[n - 1] <= 0.0f) {
            continue;
        }
        for (c = 0; c < cc; c++) {
            px[c] = apply_one(s, px[c], bl[c]);
        }
    }
    free(blurred);
    return PC_OK;
}

pc_status pc_algo_unsharp_k(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx,
                            const pc_algo_unsharp_params *p, float *dst, pc_error *err)
{
    sharp_args s = {SHARP_UNSHARP, p->amount / 100.0f, p->threshold / 255.0f, 0.0f};
    return with_blur(src, out, ctx, p->radius, &s, dst, err);
}

pc_status pc_algo_smart_sharpen_k(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx,
                                  const pc_algo_smart_sharpen_params *p, float *dst, pc_error *err)
{
    sharp_args s = {SHARP_SMART, p->amount / 100.0f, 0.0f, pc_clampf(p->reduce_noise / 100.0f, 0.0f, 1.0f)};
    return with_blur(src, out, ctx, p->radius, &s, dst, err);
}

pc_status pc_algo_high_pass_k(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx, float radius,
                              float *dst, pc_error *err)
{
    sharp_args s = {SHARP_HIGH_PASS, 0.0f, 0.0f, 0.0f};
    return with_blur(src, out, ctx, radius, &s, dst, err);
}
