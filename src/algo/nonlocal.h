/* Non-local patch-based completion, the Remove Tool's fill (port of crates/algo/src/nonlocal.rs):
 * A. Newson, A. Almansa, Y. Gousseau, P. Perez, "Non-Local Patch-Based Image Inpainting", IPOL 7
 * (2017). Coarse to fine, a PatchMatch search for each patch's nearest fully-known patch alternates
 * with a similarity-weighted vote, with:
 *  - texture features (local mean |dx|, |dy| after Liu and Caselles, weighted by lambda) in the
 *    patch distance, filled along with the colour;
 *  - onion-peel initialisation at the coarsest level;
 *  - the match field (not the image) upsampled between levels;
 *  - each final pixel copied from the best patch covering it;
 *  - seams hidden in the gradient domain (a membrane carries the colour mismatch along the hole's
 *    edge into the fill; steps between areas copied from different places are spread by a smooth
 *    correction);
 *  - as many levels as the hole is thick (log2(2 No / N)), not as it is wide.
 *
 * Buffers are interleaved w x h x ch normalised floats with a w x h hole mask; deterministic for a
 * seed whatever the thread count. */
#ifndef PC_ALGO_NONLOCAL_H
#define PC_ALGO_NONLOCAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "raster/interrupt.h"

/* Params. Radii above PC_ALGO_INPAINT_MAX_RADIUS and counts above PC_ALGO_INPAINT_MAX_ITERS are
 * PC_ERR_LIMIT. */
typedef struct pc_algo_nonlocal_params {
    size_t patch_radius; /* patches are 2r+1 square */
    float lambda;        /* weight of the texture features against the colour */
    size_t max_iters;    /* most EM iterations per level */
    size_t pm_passes;    /* PatchMatch passes per EM iteration */
    float tolerance;     /* a level is done once an iteration changes the hole by less on average */
    uint64_t seed;
} pc_algo_nonlocal_params;

/* {3, 50, 10, 2, 0.1 / 255, 1}. */
pc_algo_nonlocal_params pc_algo_nonlocal_params_default(void);

/* Complete `hole` in img (complete_with). *out is NULL (PC_OK) when no fully-known patch exists to
 * copy from; a buffer or mask of the wrong size, ch 0, or no hole gives a copy of img (len floats).
 * ctl may be NULL (PC_ERR_CANCELLED). */
pc_status pc_algo_nonlocal_complete(size_t w, size_t h, size_t ch, const float *img, size_t len, const bool *hole,
                                    size_t hole_len, const pc_algo_nonlocal_params *p, const pc_interrupt *ctl,
                                    float **out, pc_error *err);

/* How many levels the completion uses for this hole and patch radius (level_count; exposed for
 * tests). */
pc_status pc_algo_nonlocal_level_count(size_t w, size_t h, const bool *hole, size_t r, size_t *out, pc_error *err);

#endif
