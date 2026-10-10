/* Multiresolution blending for Edit > Auto-Blend Layers (port of crates/algo/src/pyramid.rs).
 *
 * Burt & Adelson (ACM TOG 1983): images split into Laplacian pyramids, weights into Gaussian
 * pyramids, each level blended with its own smoothed weights and collapsed. Weights come from
 * pc_algo_stack_weights (focus stacking: the sharpest image wins) or pc_algo_panorama_weights (the
 * image whose opaque area a pixel is deepest inside). Buffers are interleaved w x h x ch
 * normalised floats of any model and depth. Deterministic; rows run in parallel on the global pool.
 *
 * Ownership: levels own their pixels (free with pc_algo_level_fini / pc_algo_pyramid_fini);
 * returned plain buffers are free()d by the caller. Errors: NOMEM, PC_ERR_LIMIT for sizes past
 * PC_MAX_ALLOC, PC_ERR_INVALID for a level with no pixels expanded to a non-empty size (the
 * reference panics there). */
#ifndef PC_ALGO_PYRAMID_H
#define PC_ALGO_PYRAMID_H

#include <stddef.h>

#include "base/status.h"

typedef struct pc_algo_level {
    size_t w, h;
    float *px; /* w * h * ch samples; NULL when empty */
} pc_algo_level;

typedef struct pc_algo_pyramid {
    pc_algo_level *levels;
    size_t n;
} pc_algo_pyramid;

void pc_algo_level_fini(pc_algo_level *l);
void pc_algo_pyramid_fini(pc_algo_pyramid *p);

/* 5-tap binomial filter + decimation by two (REDUCE). */
pc_status pc_algo_pyramid_reduce(const pc_algo_level *l, size_t ch, pc_algo_level *out, pc_error *err);
/* Upsample l to w x h (EXPAND: zero insertion + the same filter, x4 gain); *out has w * h * ch. */
pc_status pc_algo_pyramid_expand(const pc_algo_level *l, size_t ch, size_t w, size_t h, float **out,
                                 pc_error *err);
/* Gaussian pyramid with `levels` levels (level 0 = a copy of px). */
pc_status pc_algo_pyramid_gaussian(size_t w, size_t h, size_t ch, const float *px, size_t levels,
                                   pc_algo_pyramid *out, pc_error *err);
/* Laplacian pyramid: band-pass levels, the last one the low-pass residual. */
pc_status pc_algo_pyramid_laplacian(size_t w, size_t h, size_t ch, const float *px, size_t levels,
                                    pc_algo_pyramid *out, pc_error *err);
/* Rebuild an image from its Laplacian pyramid (*len floats; NULL for an empty pyramid). */
pc_status pc_algo_pyramid_collapse(const pc_algo_pyramid *pyr, size_t ch, float **out, size_t *len,
                                   pc_error *err);
/* Levels so the coarsest is a few pixels across. */
size_t pc_algo_pyramid_auto_levels(size_t w, size_t h);
/* Blend `count` images (w * h * ch each) with per-pixel non-negative weights (w * h each). */
pc_status pc_algo_pyramid_blend(size_t w, size_t h, size_t ch, const float *const *images,
                                const float *const *weights, size_t count, size_t levels, float **out,
                                pc_error *err);

/* Focus-stack weights: one-hot maps (count planes of w * h, allocated into out[k]). */
pc_status pc_algo_stack_weights(size_t w, size_t h, const float *const *luma, const float *const *alpha,
                                size_t count, float **out, pc_error *err);
/* Panorama weights: each pixel goes to the image whose opaque area it lies deepest inside. */
pc_status pc_algo_panorama_weights(size_t w, size_t h, const float *const *alpha, size_t count, float **out,
                                   pc_error *err);

#endif
