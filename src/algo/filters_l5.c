/* Filter registry of chunk L5 (blurs, sharpen, noise, denoise): the pc_algo_filter_defs of
 * blur.h (parameter tables in the reference's serde order, halos of lib.rs FilterParams::halo and
 * halo_ext, kernels and whole-area overrides), listed NULL-terminated for filter.c. */
#include "algo/filter_tables.h"

#include <math.h>
#include <stddef.h>

#include "algo/blur_int.h"
#include "base/num.h"
#include "base/rmath.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))

/* ---- parameter tables ---- */

static const pc_algo_jfield RADIUS_F[] = {
    PC_ALGO_JF("radius", pc_algo_radius_params, radius, PC_ALGO_JF_F32)};
static const pc_algo_jfield MOTION_F[] = {
    PC_ALGO_JF("angle", pc_algo_motion_params, angle, PC_ALGO_JF_F32),
    PC_ALGO_JF("distance", pc_algo_motion_params, distance, PC_ALGO_JF_F32),
};
static const pc_algo_jfield RADIAL_F[] = {
    PC_ALGO_JF("amount", pc_algo_radial_params, amount, PC_ALGO_JF_F32),
    PC_ALGO_JFE("method", pc_algo_radial_params, method, PC_ALGO_ENUM_RADIAL_METHOD, false),
    PC_ALGO_JFE("quality", pc_algo_radial_params, quality, PC_ALGO_ENUM_RADIAL_QUALITY, true),
    PC_ALGO_JF("centerX", pc_algo_radial_params, center_x, PC_ALGO_JF_F32),
    PC_ALGO_JF("centerY", pc_algo_radial_params, center_y, PC_ALGO_JF_F32),
};
static const pc_algo_jfield RADIUS_THRESHOLD_F[] = {
    PC_ALGO_JF("radius", pc_algo_radius_threshold_params, radius, PC_ALGO_JF_F32),
    PC_ALGO_JF("threshold", pc_algo_radius_threshold_params, threshold, PC_ALGO_JF_F32),
};
static const pc_algo_jfield UNSHARP_F[] = {
    PC_ALGO_JF("amount", pc_algo_unsharp_params, amount, PC_ALGO_JF_F32),
    PC_ALGO_JF("radius", pc_algo_unsharp_params, radius, PC_ALGO_JF_F32),
    PC_ALGO_JF("threshold", pc_algo_unsharp_params, threshold, PC_ALGO_JF_F32),
};
static const pc_algo_jfield SMART_SHARPEN_F[] = {
    PC_ALGO_JF("amount", pc_algo_smart_sharpen_params, amount, PC_ALGO_JF_F32),
    PC_ALGO_JF("radius", pc_algo_smart_sharpen_params, radius, PC_ALGO_JF_F32),
    PC_ALGO_JF("reduceNoise", pc_algo_smart_sharpen_params, reduce_noise, PC_ALGO_JF_F32),
};
static const pc_algo_jfield ADD_NOISE_F[] = {
    PC_ALGO_JF("amount", pc_algo_add_noise_params, amount, PC_ALGO_JF_F32),
    PC_ALGO_JFE("distribution", pc_algo_add_noise_params, distribution, PC_ALGO_ENUM_DISTRIBUTION, false),
    PC_ALGO_JF("monochromatic", pc_algo_add_noise_params, monochromatic, PC_ALGO_JF_BOOL),
    PC_ALGO_JF("seed", pc_algo_add_noise_params, seed, PC_ALGO_JF_U32),
};
static const pc_algo_jfield REDUCE_NOISE_F[] = {
    PC_ALGO_JF("strength", pc_algo_reduce_noise_params, strength, PC_ALGO_JF_F32),
    PC_ALGO_JF("preserveDetails", pc_algo_reduce_noise_params, preserve_details, PC_ALGO_JF_F32),
    PC_ALGO_JF("reduceColorNoise", pc_algo_reduce_noise_params, reduce_color_noise, PC_ALGO_JF_F32),
    PC_ALGO_JF("sharpenDetails", pc_algo_reduce_noise_params, sharpen_details, PC_ALGO_JF_F32),
    PC_ALGO_JF("removeJpegArtifact", pc_algo_reduce_noise_params, remove_jpeg_artifact, PC_ALGO_JF_BOOL),
};
static const pc_algo_jfield SMART_BLUR_F[] = {
    PC_ALGO_JF("radius", pc_algo_smart_blur_params, radius, PC_ALGO_JF_F32),
    PC_ALGO_JF("threshold", pc_algo_smart_blur_params, threshold, PC_ALGO_JF_F32),
    PC_ALGO_JFE("quality", pc_algo_smart_blur_params, quality, PC_ALGO_ENUM_BLUR_QUALITY, false),
    PC_ALGO_JFE("mode", pc_algo_smart_blur_params, mode, PC_ALGO_ENUM_SMART_BLUR_MODE, false),
};
static const pc_algo_jfield LENS_BLUR_F[] = {
    PC_ALGO_JF("radius", pc_algo_lens_blur_params, radius, PC_ALGO_JF_F32),
    PC_ALGO_JF("blades", pc_algo_lens_blur_params, blades, PC_ALGO_JF_U32),
    PC_ALGO_JF("curvature", pc_algo_lens_blur_params, curvature, PC_ALGO_JF_F32),
    PC_ALGO_JF("rotation", pc_algo_lens_blur_params, rotation, PC_ALGO_JF_F32),
    PC_ALGO_JFE("depth", pc_algo_lens_blur_params, depth, PC_ALGO_ENUM_DEPTH_SOURCE, false),
    PC_ALGO_JF("focalDistance", pc_algo_lens_blur_params, focal_distance, PC_ALGO_JF_F32),
    PC_ALGO_JF("invertDepth", pc_algo_lens_blur_params, invert_depth, PC_ALGO_JF_BOOL),
    PC_ALGO_JF("brightness", pc_algo_lens_blur_params, brightness, PC_ALGO_JF_F32),
    PC_ALGO_JF("threshold", pc_algo_lens_blur_params, threshold, PC_ALGO_JF_F32),
    PC_ALGO_JF("noise", pc_algo_lens_blur_params, noise, PC_ALGO_JF_F32),
    PC_ALGO_JFE("distribution", pc_algo_lens_blur_params, distribution, PC_ALGO_ENUM_DISTRIBUTION, false),
    PC_ALGO_JF("monochromatic", pc_algo_lens_blur_params, monochromatic, PC_ALGO_JF_BOOL),
    PC_ALGO_JF("seed", pc_algo_lens_blur_params, seed, PC_ALGO_JF_U32),
    PC_ALGO_JFSKIP("depthMap", pc_algo_lens_blur_params, depth_map),
};
static const pc_algo_jfield SHAPE_BLUR_F[] = {
    PC_ALGO_JF("radius", pc_algo_shape_blur_params, radius, PC_ALGO_JF_F32),
    PC_ALGO_JFE("shape", pc_algo_shape_blur_params, shape, PC_ALGO_ENUM_BLUR_SHAPE, false),
};

static void radial_init(void *p)
{
    ((pc_algo_radial_params *)p)->quality = PC_ALGO_RADIAL_GOOD;
}

/* ---- halos ---- */

/* The reference's `g(sigma)`: Radius((sigma.max(0) * 3).ceil() as i32 + 1). */
static pc_algo_halo halo_g(float sigma)
{
    return pc_algo_halo_radius(
        pc_i32_wrapping_add(pc_f32_to_i32(ceilf(pc_rs_maxf_k(sigma, 0.0f) * 3.0f)), 1));
}

static pc_algo_halo h_gauss(const void *p, const pc_rect *b)
{
    (void)b;
    return halo_g(((const pc_algo_radius_params *)p)->radius);
}
static pc_algo_halo h_radius(const void *p, const pc_rect *b)
{
    (void)b;
    return pc_algo_halo_reach(((const pc_algo_radius_params *)p)->radius);
}
static pc_algo_halo h_radius_threshold(const void *p, const pc_rect *b)
{
    (void)b;
    return pc_algo_halo_reach(((const pc_algo_radius_threshold_params *)p)->radius);
}
static pc_algo_halo h_unsharp(const void *p, const pc_rect *b)
{
    (void)b;
    return halo_g(((const pc_algo_unsharp_params *)p)->radius);
}
static pc_algo_halo h_smart_sharpen(const void *p, const pc_rect *b)
{
    (void)b;
    return halo_g(((const pc_algo_smart_sharpen_params *)p)->radius);
}
static pc_algo_halo h_motion(const void *p, const pc_rect *b)
{
    (void)b;
    return pc_algo_halo_radius(pc_i32_wrapping_add(
        pc_f32_to_i32(ceilf(fabsf(((const pc_algo_motion_params *)p)->distance) / 2.0f)), 2));
}
static pc_algo_halo h_bounds(const void *p, const pc_rect *b)
{
    (void)p;
    (void)b;
    return pc_algo_halo_bounds();
}
static pc_algo_halo h_reduce_noise(const void *p, const pc_rect *b)
{
    (void)p;
    (void)b;
    return pc_algo_halo_reach(pc_algo_denoise_reach());
}
static pc_algo_halo h_smart_blur(const void *p, const pc_rect *b)
{
    (void)b;
    return pc_algo_halo_reach(((const pc_algo_smart_blur_params *)p)->radius);
}
static pc_algo_halo h_lens_blur(const void *p, const pc_rect *b)
{
    (void)b;
    return pc_algo_halo_reach(((const pc_algo_lens_blur_params *)p)->radius);
}
static pc_algo_halo h_shape_blur(const void *p, const pc_rect *b)
{
    (void)b;
    return pc_algo_halo_reach(pc_algo_shape_blur_radius(((const pc_algo_shape_blur_params *)p)->radius));
}

/* ---- kernels ---- */

#define KERNEL(name)                                                                                         \
    static pc_status name(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,            \
                          const pc_algo_ctx *ctx, float *dst, pc_error *err)

KERNEL(k_gauss)
{
    (void)prep;
    return pc_algo_gaussian_k(src, out, ctx, ((const pc_algo_radius_params *)p)->radius, dst, err);
}
KERNEL(k_box)
{
    (void)prep;
    return pc_algo_boxed_k(src, out, ctx, ((const pc_algo_radius_params *)p)->radius, dst, err);
}
KERNEL(k_motion)
{
    const pc_algo_motion_params *m = p;
    (void)prep;
    return pc_algo_motion_k(src, out, ctx, m->angle, m->distance, dst, err);
}
KERNEL(k_radial)
{
    (void)prep;
    return pc_algo_radial_k(src, out, ctx, p, dst, err);
}
KERNEL(k_surface)
{
    const pc_algo_radius_threshold_params *s = p;
    (void)prep;
    return pc_algo_surface_blur_k(src, out, ctx, s->radius, s->threshold, dst, err);
}
KERNEL(k_unsharp)
{
    (void)prep;
    return pc_algo_unsharp_k(src, out, ctx, p, dst, err);
}
KERNEL(k_smart_sharpen)
{
    (void)prep;
    return pc_algo_smart_sharpen_k(src, out, ctx, p, dst, err);
}
KERNEL(k_high_pass)
{
    (void)prep;
    return pc_algo_high_pass_k(src, out, ctx, ((const pc_algo_radius_params *)p)->radius, dst, err);
}
KERNEL(k_add_noise)
{
    (void)prep;
    return pc_algo_add_noise_k(src, out, ctx, p, dst, err);
}
KERNEL(k_median)
{
    (void)prep;
    (void)ctx;
    return pc_algo_median_k(src, out, ((const pc_algo_radius_params *)p)->radius, false, 0.0f, dst, err);
}
KERNEL(k_dust)
{
    const pc_algo_radius_threshold_params *s = p;
    (void)prep;
    (void)ctx;
    return pc_algo_median_k(src, out, s->radius, true, s->threshold, dst, err);
}
KERNEL(k_reduce_noise)
{
    (void)prep;
    return pc_algo_reduce_noise_k(src, out, ctx, p, dst, err);
}
KERNEL(k_smart_blur)
{
    (void)prep;
    return pc_algo_smart_blur_k(src, out, ctx, p, dst, err);
}
KERNEL(k_lens_blur)
{
    (void)prep;
    return pc_algo_lens_blur_k(src, out, ctx, p, dst, err);
}
KERNEL(k_shape_blur)
{
    void *own = NULL;
    pc_status st;
    if (prep != NULL) {
        return pc_algo_shape_blur_run(prep, src, out, ctx, dst, err);
    }
    st = pc_algo_shape_blur_prepare(p, &own, err);
    if (st == PC_OK) {
        st = pc_algo_shape_blur_run(own, src, out, ctx, dst, err);
        pc_algo_shape_blur_prep_free(own);
    }
    return st;
}

/* ---- defs ---- */

#define DEF(name, tg, T, F, init, halo, kern, apply, flags)                                                  \
    const pc_algo_filter_def name = {(tg), sizeof(T), (F),  N(F),   (init),  NULL,   (halo),                 \
                                     NULL, NULL,      NULL, (kern), (apply), (flags)}

DEF(PC_ALGO_FILTER_GAUSSIAN_BLUR, "gaussianBlur", pc_algo_radius_params, RADIUS_F, NULL, h_gauss, k_gauss,
    pc_algo_box_blur_apply, 0);
DEF(PC_ALGO_FILTER_BOX_BLUR, "boxBlur", pc_algo_radius_params, RADIUS_F, NULL, h_radius, k_box,
    pc_algo_box_blur_apply, 0);
DEF(PC_ALGO_FILTER_MOTION_BLUR, "motionBlur", pc_algo_motion_params, MOTION_F, NULL, h_motion, k_motion,
    pc_algo_motion_apply, 0);
DEF(PC_ALGO_FILTER_RADIAL_BLUR, "radialBlur", pc_algo_radial_params, RADIAL_F, radial_init, h_bounds,
    k_radial, NULL, 0);
DEF(PC_ALGO_FILTER_SURFACE_BLUR, "surfaceBlur", pc_algo_radius_threshold_params, RADIUS_THRESHOLD_F, NULL,
    h_radius_threshold, k_surface, NULL, 0);
DEF(PC_ALGO_FILTER_UNSHARP_MASK, "unsharpMask", pc_algo_unsharp_params, UNSHARP_F, NULL, h_unsharp, k_unsharp,
    NULL, 0);
DEF(PC_ALGO_FILTER_SMART_SHARPEN, "smartSharpen", pc_algo_smart_sharpen_params, SMART_SHARPEN_F, NULL,
    h_smart_sharpen, k_smart_sharpen, NULL, 0);
DEF(PC_ALGO_FILTER_HIGH_PASS, "highPass", pc_algo_radius_params, RADIUS_F, NULL, h_gauss, k_high_pass, NULL,
    0);
DEF(PC_ALGO_FILTER_ADD_NOISE, "addNoise", pc_algo_add_noise_params, ADD_NOISE_F, NULL, NULL, k_add_noise,
    NULL, 0);
DEF(PC_ALGO_FILTER_MEDIAN, "median", pc_algo_radius_params, RADIUS_F, NULL, h_radius, k_median, NULL, 0);
DEF(PC_ALGO_FILTER_DUST_AND_SCRATCHES, "dustAndScratches", pc_algo_radius_threshold_params,
    RADIUS_THRESHOLD_F, NULL, h_radius_threshold, k_dust, NULL, 0);
DEF(PC_ALGO_FILTER_REDUCE_NOISE, "reduceNoise", pc_algo_reduce_noise_params, REDUCE_NOISE_F, NULL,
    h_reduce_noise, k_reduce_noise, NULL, 0);
DEF(PC_ALGO_FILTER_SMART_BLUR, "smartBlur", pc_algo_smart_blur_params, SMART_BLUR_F, NULL, h_smart_blur,
    k_smart_blur, NULL, 0);
DEF(PC_ALGO_FILTER_LENS_BLUR, "lensBlur", pc_algo_lens_blur_params, LENS_BLUR_F, NULL, h_lens_blur,
    k_lens_blur, NULL, 0);

/* Shape Blur: the rasterised kernel is prepared once per run; the caller's tile size is clamped
 * (its scratch is per tile) and a failed run leaves the input unchanged. */
const pc_algo_filter_def PC_ALGO_FILTER_SHAPE_BLUR = {"shapeBlur",
                                                      sizeof(pc_algo_shape_blur_params),
                                                      SHAPE_BLUR_F,
                                                      N(SHAPE_BLUR_F),
                                                      NULL,
                                                      NULL,
                                                      h_shape_blur,
                                                      NULL,
                                                      pc_algo_shape_blur_prepare,
                                                      pc_algo_shape_blur_prep_free,
                                                      k_shape_blur,
                                                      NULL,
                                                      PC_ALGO_FILTER_CLAMP_TILE |
                                                          PC_ALGO_FILTER_KEEP_ON_FAIL};

const pc_algo_filter_def *const pc_algo_filters_l5[] = {
    &PC_ALGO_FILTER_GAUSSIAN_BLUR,      &PC_ALGO_FILTER_BOX_BLUR,
    &PC_ALGO_FILTER_MOTION_BLUR,        &PC_ALGO_FILTER_RADIAL_BLUR,
    &PC_ALGO_FILTER_SURFACE_BLUR,       &PC_ALGO_FILTER_UNSHARP_MASK,
    &PC_ALGO_FILTER_SMART_SHARPEN,      &PC_ALGO_FILTER_HIGH_PASS,
    &PC_ALGO_FILTER_ADD_NOISE,          &PC_ALGO_FILTER_MEDIAN,
    &PC_ALGO_FILTER_DUST_AND_SCRATCHES, &PC_ALGO_FILTER_REDUCE_NOISE,
    &PC_ALGO_FILTER_SMART_BLUR,         &PC_ALGO_FILTER_LENS_BLUR,
    &PC_ALGO_FILTER_SHAPE_BLUR,         NULL,
};
