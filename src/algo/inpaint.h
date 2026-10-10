/* Hole filling for Spot Healing: patch-based completion, proximity match and texture synthesis
 * (port of crates/algo/src/inpaint.rs).
 *
 *  - pc_algo_inpaint_complete, content-aware fill: the multi-scale EM completion of Wexler,
 *    Shechtman, Irani ("Space-Time Completion of Video", TPAMI 2007) with the nearest-neighbour
 *    field from PatchMatch (Barnes et al., SIGGRAPH 2009), coarse-to-fine over a pyramid whose
 *    coarsest level starts from a membrane fill.
 *  - pc_algo_inpaint_best_offset, proximity match: the displacement whose surroundings best match
 *    the ring around the hole (an automatic Healing Brush source).
 *  - pc_algo_inpaint_synthesize, create texture: patch-based texture synthesis after Efros and
 *    Freeman ("Image Quilting", SIGGRAPH 2001), feathered overlaps instead of a min-cut seam.
 *
 * Buffers: interleaved w x h x ch normalised floats (`len` floats, must be w * h * ch) and a w x h
 * hole mask; outputs equal the input outside the hole, newly allocated (free()). Deterministic
 * (fixed-seed SplitMix64, results independent of the thread count). Size limits as poisson.h;
 * patch radii above PC_ALGO_INPAINT_MAX_RADIUS and iteration counts above
 * PC_ALGO_INPAINT_MAX_ITERS are PC_ERR_LIMIT (the reference would run for hours). */
#ifndef PC_ALGO_INPAINT_H
#define PC_ALGO_INPAINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "raster/interrupt.h"

#define PC_ALGO_INPAINT_MAX_RADIUS ((size_t)1024)
#define PC_ALGO_INPAINT_MAX_ITERS ((size_t)1000)

/* CompleteParams. */
typedef struct pc_algo_complete_params {
    size_t patch_radius; /* patches are 2r+1 square */
    size_t em_iters;     /* EM iterations at the coarsest level (finer levels fewer) */
    size_t min_em_iters;
    size_t pm_iters; /* PatchMatch passes per EM iteration */
    /* Levels where the hole is wider than this only refine the upsampled match field. */
    size_t refine_extent;
    uint64_t seed;
} pc_algo_complete_params;

/* {3, 8, 2, 2, 96, 1}. */
pc_algo_complete_params pc_algo_complete_params_default(void);

/* Content-aware completion of `hole`. *out is NULL (with PC_OK) when there is no fully-known
 * patch to copy from (the reference's None; callers fall back to a membrane fill). ctl may be
 * NULL; it is checked per EM iteration, PatchMatch pass and row band (PC_ERR_CANCELLED) and gets
 * progress by pyramid level. */
pc_status pc_algo_inpaint_complete(size_t w, size_t h, size_t ch, const float *img, size_t len,
                                   const bool *hole, size_t hole_len, const pc_algo_complete_params *p,
                                   const pc_interrupt *ctl, float **out, pc_error *err);

/* Proximity match: the displacement (*dx, *dy) within max_radius whose surroundings best match the
 * ring of `ring` pixels around the hole, with the displaced hole fully inside the grid and outside
 * the hole. *found is false (PC_OK) when no displacement fits (the reference's None). ring above
 * 2^24 is PC_ERR_LIMIT. */
pc_status pc_algo_inpaint_best_offset(size_t w, size_t h, size_t ch, const float *img, size_t len,
                                      const bool *hole, size_t hole_len, size_t ring, int32_t max_radius,
                                      bool *found, int32_t *dx, int32_t *dy, pc_error *err);

/* Create texture: fill the hole with blocks (side max(block, 4)) of surrounding texture; falls back
 * to the membrane fill when no candidate block fits. block above 2^24 is PC_ERR_LIMIT. */
pc_status pc_algo_inpaint_synthesize(size_t w, size_t h, size_t ch, const float *img, size_t len,
                                     const bool *hole, size_t hole_len, size_t block, uint64_t seed,
                                     float **out, pc_error *err);

#endif
