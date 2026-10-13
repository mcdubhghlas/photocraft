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

#include <stdbool.h>
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

/* ---- focus-stack fusion (pyramid_fuse.c) ----
 *
 * fuse_stack: per Laplacian coefficient the image whose luminance band has the largest region
 * energy wins (Adelson et al. 1984 with the maximum-region-energy rule of Wang & Chang 2011), the
 * residual follows the deviation + entropy rule, and the blending happens in the reconstruction.
 * With halo control (halo > 0) the levels coarser than `guide` are the mean of the images
 * weighed by the guide's region energy to the power `halo`, so a defocused copy of a bright
 * object doesn't glow into the result. Ties keep the earlier image; an image has no say where
 * its coverage is below one half. Output clamped to 0..=1. */
#define PC_ALGO_HALO_MAX 8.0f

typedef struct pc_algo_stack_fusion {
    size_t energy_radius; /* region-energy binomial window radius (0 = per-node |L|), at most 4096 */
    float halo;           /* halo control hardness (0 = off), capped at PC_ALGO_HALO_MAX */
    size_t guide;         /* the guide level (0 = finest band-pass), clamped to the last band */
    bool has_levels;      /* false: pc_algo_stack_levels */
    size_t levels;        /* pyramid levels, the residual included */
} pc_algo_stack_fusion;

/* energy_radius 1, halo 2, guide 2, automatic levels. */
pc_algo_stack_fusion pc_algo_stack_fusion_default(void);
/* As many band-pass levels as keep the residual's short side >= 32 px (at most 10 levels). */
size_t pc_algo_stack_levels(size_t w, size_t h);
/* `count` images (w * h * ch each) with their luminance and coverage planes (w * h each); *out is
 * w * h * ch floats (malloc'd). NOMEM / LIMIT (sizes, energy_radius > 4096). */
pc_status pc_algo_fuse_stack(size_t w, size_t h, size_t ch, const float *const *images,
                             const float *const *luma, const float *const *alpha, size_t count,
                             pc_algo_stack_fusion p, float **out, pc_error *err);
/* Pieces exposed for tests: the halo weight ((re + 1e-8) / 1e-4)^p (exponent clamped to +-60),
 * the binomial window of radius r (row 2r of Pascal's triangle / 4^r), the separable weighted
 * window sum of a plane (clamped borders) and the 5 x 5 local deviation / entropy. */
float pc_algo_halo_weight(float re, float p);
pc_status pc_algo_binomial(size_t r, float **out, size_t *len, pc_error *err);
pc_status pc_algo_window_sum(size_t w, size_t h, const float *src, const float *wt, size_t nwt, float **out,
                             pc_error *err);
pc_status pc_algo_deviation_entropy(size_t w, size_t h, const float *y, float **dev, float **ent,
                                    pc_error *err);

#endif
