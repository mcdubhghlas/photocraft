/* Edge refinement ("Select and Mask" / Refine Edge) and colour decontamination (port of
 * crates/algo/src/matting.rs).
 *
 * Soft edges come from the guided filter (He, Sun & Tang, ECCV 2010 / PAMI 2013) inside a band of
 * `radius` pixels around the selection edge ("smart radius" adapts it to the transition width in
 * the image); then, in Photoshop's order, Smooth, Feather (Gaussian, sigma = feather / 2),
 * Contrast and Shift Edge (grey dilation / erosion). Decontaminate Colors replaces fringe colours
 * by the normalised-convolution average of nearby fully selected pixels.
 *
 * Work is tiled (256 px): only tiles near the selection boundary are processed, on the global
 * pool; results don't depend on the thread count.
 *
 * Buffers are w x h row-major floats. Functions returning `float **out` allocate it (free());
 * inputs must hold w * h values. Failure: PC_ERR_NOMEM, PC_ERR_LIMIT (sizes past PC_MAX_ALLOC,
 * Gaussian kernels past 2^24 taps), or the sampler's / reader's error. */
#ifndef PC_ALGO_MATTING_H
#define PC_ALGO_MATTING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "algo/params.h"
#include "algo/segment.h"
#include "algo/selection.h"
#include "base/status.h"
#include "geom/geom.h"
#include "raster/surface.h"

/* Select and Mask parameters (Photoshop's units). */
typedef struct pc_algo_refine_params {
    float radius;      /* edge-detection band radius in px (0 = no edge detection) */
    bool smart_radius; /* adapt the band to the edge's observed softness */
    float smooth;      /* 0..100 */
    float feather;     /* px */
    float contrast;    /* 0..100 */
    float shift_edge;  /* -100..100 (% of max(radius, 4 px)) */
} pc_algo_refine_params;

/* The serde field table (camelCase, every field #[serde(default)]) for pc_algo_jfields_*. */
extern const pc_algo_jfield pc_algo_refine_params_fields[];
extern const size_t pc_algo_refine_params_nfields;

pc_algo_refine_params pc_algo_refine_params_default(void); /* all zero */
/* Pixels read around an output pixel, and how far the refined mask can extend. */
int32_t pc_algo_refine_halo(const pc_algo_refine_params *p);
int32_t pc_algo_refine_spread(const pc_algo_refine_params *p);

/* Guided-filter regulariser (colour guide, samples in 0..1). */
#define PC_ALGO_MATTING_EPS 1e-4f

/* Box mean with radius r (window clipped to the image, divided by the pixel count). */
pc_status pc_algo_matting_box_mean(const float *src, size_t w, size_t h, size_t r, float **out,
                                   pc_error *err);
pc_status pc_algo_matting_guided_filter_gray(const float *guide, const float *p, size_t w, size_t h, size_t r,
                                             float eps, float **out, pc_error *err);
/* Colour guide (He et al., Eq. 19-21); p holds guide->w * guide->h values. */
pc_status pc_algo_matting_guided_filter_color(const pc_algo_rgb_image *guide, const float *p, size_t r,
                                              float eps, float **out, pc_error *err);
/* Separable Gaussian blur (edge-clamped); sigma below 0.1 copies. */
pc_status pc_algo_matting_gaussian_blur(const float *src, size_t w, size_t h, float sigma, float **out,
                                        pc_error *err);
/* Grey dilation (grow) or erosion by about r pixels (octagonal element). */
pc_status pc_algo_matting_morph(const float *src, size_t w, size_t h, size_t r, bool grow, float **out,
                                pc_error *err);
/* Per-pixel transition width (px) of the guide's luminance over a window of radius r. */
pc_status pc_algo_matting_edge_width(const pc_algo_rgb_image *img, size_t r, float **out, pc_error *err);
/* Running max / min over a square window of radius r (edge-clipped). */
pc_status pc_algo_matting_max_square(const float *src, size_t w, size_t h, size_t r, bool max, float **out,
                                     pc_error *err);

/* Refines a soft mask (w x h) with img as guide (needed when radius > 0; NULL skips that). */
pc_status pc_algo_matting_refine_buffer(const pc_algo_rgb_image *img, const float *m, size_t w, size_t h,
                                        const pc_algo_refine_params *p, float **out, pc_error *err);

/* Fills out[0..n) (n = r.w * r.h, zeroed) with mask coverage over r, row-major. Called
 * concurrently from pool threads. */
typedef pc_status (*pc_algo_mask_fn)(const void *ctx, pc_rect r, float *out, size_t n, pc_error *err);
/* Readers: ctx is a const pc_algo_region * (zero outside its box) / a const pc_surface *. */
pc_status pc_algo_matting_region_reader(const void *ctx, pc_rect r, float *out, size_t n, pc_error *err);
pc_status pc_algo_matting_surface_reader(const void *ctx, pc_rect r, float *out, size_t n, pc_error *err);

/* Refines a selection mask: `content` bounds its non-zero pixels, the result is clipped to
 * canvas; the sampler is read only when radius > 0. None when nothing remains selected. */
pc_status pc_algo_matting_refine_mask(const pc_algo_sampler *s, pc_algo_mask_fn mask, const void *mask_ctx,
                                      pc_rect content, pc_rect canvas, const pc_algo_refine_params *p,
                                      pc_algo_region *out, pc_error *err);
/* GRAY8 surface (default 0) holding a region's coverage. `out` initialised on success. */
pc_status pc_algo_matting_region_surface(const pc_algo_region *reg, pc_surface *out, pc_error *err);
/* Decontaminates colours in the soft fringe of alpha (any model / depth). `out` initialised on
 * success. */
pc_status pc_algo_matting_decontaminate(const pc_surface *src, const pc_algo_region *alpha, float radius,
                                        float amount, pc_surface *out, pc_error *err);
/* src's pixels with alpha multiplied by the region (zero outside its box); the result always has
 * an alpha channel. */
pc_status pc_algo_matting_masked_copy(const pc_surface *src, const pc_algo_region *alpha, pc_surface *out,
                                      pc_error *err);

#endif
