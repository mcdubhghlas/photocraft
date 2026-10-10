/* Blur, sharpen and noise filters (port of crates/algo/src/blur.rs, blur/motion_*.rs, blur2.rs,
 * shape_blur.rs, sharpen.rs, noise.rs and denoise.rs): Gaussian, Box, Motion (with an FFT path
 * for long streaks), Radial, Surface, Smart, Lens and Shape Blur; Unsharp Mask, Smart Sharpen,
 * High Pass; Add Noise, Median, Dust & Scratches and Reduce Noise.
 *
 * Each filter is a pc_algo_filter_def (registered in filters_l5.c, found by tag with
 * pc_algo_filter_def_find) whose parameters are the typed structs below, so callers can build a
 * filter without JSON: pc_algo_filter_make(&PC_ALGO_FILTER_GAUSSIAN_BLUR, &params, &f, err).
 * Parameter units are Photoshop's dialog units (pixels, percent, degrees, levels 0-255), and
 * hostile values (NaN, inf, huge radii) behave as in the reference or return PC_ERR_LIMIT where
 * the reference would run for hours or exhaust memory (REWRITE_PLAN.md section 7).
 *
 * Thread safety: the defs are immutable; kernels are reentrant. */
#ifndef PC_ALGO_BLUR_H
#define PC_ALGO_BLUR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "algo/filter.h"
#include "algo/image.h"
#include "algo/params.h"
#include "base/status.h"
#include "geom/geom.h"

/* GaussianBlur / BoxBlur / HighPass / Median: radius in px. */
typedef struct pc_algo_radius_params {
    float radius;
} pc_algo_radius_params;

typedef struct pc_algo_motion_params {
    float angle, distance;
} pc_algo_motion_params;

/* Amount 1-100; centre as a fraction of the bounds. */
typedef struct pc_algo_radial_params {
    float amount;
    pc_algo_radial_method method;
    pc_algo_radial_quality quality; /* #[serde(default)]: Good */
    float center_x, center_y;
} pc_algo_radial_params;

/* SurfaceBlur and DustAndScratches: radius px, threshold levels. */
typedef struct pc_algo_radius_threshold_params {
    float radius, threshold;
} pc_algo_radius_threshold_params;

typedef struct pc_algo_unsharp_params {
    float amount, radius, threshold;
} pc_algo_unsharp_params;

typedef struct pc_algo_smart_sharpen_params {
    float amount, radius, reduce_noise;
} pc_algo_smart_sharpen_params;

/* Amount 0-400 %. */
typedef struct pc_algo_add_noise_params {
    float amount;
    pc_algo_distribution distribution;
    bool monochromatic;
    uint32_t seed;
} pc_algo_add_noise_params;

/* Strength 0-10, the rest 0-100 %. */
typedef struct pc_algo_reduce_noise_params {
    float strength, preserve_details, reduce_color_noise, sharpen_details;
    bool remove_jpeg_artifact;
} pc_algo_reduce_noise_params;

/* Radius 0.1-100 px, threshold 0.1-100 levels. */
typedef struct pc_algo_smart_blur_params {
    float radius, threshold;
    pc_algo_blur_quality quality;
    pc_algo_smart_blur_mode mode;
} pc_algo_smart_blur_params;

/* Iris radius 0-100 px, blades 3-8, curvature 0-100, rotation degrees; focal distance 0-255;
 * specular brightness 0-100 / threshold 0-255; noise 0-100. depth_map (channel 0, document
 * coordinates) is caller data for PC_ALGO_DEPTH_LAYER_MASK, borrowed: never serialised, copied
 * as a pointer, and it must outlive every filter holding it. */
typedef struct pc_algo_lens_blur_params {
    float radius;
    uint32_t blades;
    float curvature, rotation;
    pc_algo_depth_source depth;
    float focal_distance;
    bool invert_depth;
    float brightness, threshold, noise;
    pc_algo_distribution distribution;
    bool monochromatic;
    uint32_t seed;
    const pc_algo_image *depth_map;
} pc_algo_lens_blur_params;

/* Radius 5-1000 px. */
typedef struct pc_algo_shape_blur_params {
    float radius;
    pc_algo_blur_shape shape;
} pc_algo_shape_blur_params;

extern const pc_algo_filter_def PC_ALGO_FILTER_GAUSSIAN_BLUR, PC_ALGO_FILTER_BOX_BLUR,
    PC_ALGO_FILTER_MOTION_BLUR, PC_ALGO_FILTER_RADIAL_BLUR, PC_ALGO_FILTER_SURFACE_BLUR,
    PC_ALGO_FILTER_UNSHARP_MASK, PC_ALGO_FILTER_SMART_SHARPEN, PC_ALGO_FILTER_HIGH_PASS,
    PC_ALGO_FILTER_ADD_NOISE, PC_ALGO_FILTER_MEDIAN, PC_ALGO_FILTER_DUST_AND_SCRATCHES,
    PC_ALGO_FILTER_REDUCE_NOISE, PC_ALGO_FILTER_SMART_BLUR, PC_ALGO_FILTER_LENS_BLUR,
    PC_ALGO_FILTER_SHAPE_BLUR;

/* blur::gaussian_kernel: the normalised Gaussian of `sigma` (radius ceil(3 sigma); [1] below
 * sigma 0.05), newly allocated (free()). PC_ERR_LIMIT past 2^24 taps. */
pc_status pc_algo_gaussian_kernel(float sigma, float **k, size_t *len, pc_error *err);
/* blur::conv_sep: separable convolution (premultiplied internally) of src for `out` with odd
 * kernels kx, ky; src should cover out grown by their radii (outside reads as 0). dst holds
 * out.w * out.h * src->ch floats. */
pc_status pc_algo_conv_sep(const pc_algo_image *src, pc_rect out, const float *kx, size_t nkx,
                           const float *ky, size_t nky, bool alpha, float *dst, pc_error *err);

#endif
