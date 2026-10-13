/* Hole filling for Content-Aware Fill and Spot Healing: patch-based completion, proximity match and
 * texture synthesis (port of crates/algo/src/inpaint.rs).
 *
 *  - pc_algo_inpaint_complete*, content-aware fill: the multi-scale EM completion of Wexler,
 *    Shechtman, Irani ("Space-Time Completion of Video", TPAMI 2007) with the nearest-neighbour
 *    field from PatchMatch (Barnes et al., SIGGRAPH 2009), coarse to fine over a pyramid down to
 *    where the hole is about four patches across. The coarsest level is peeled from the hole's
 *    edge inwards (each pixel from the source patch that best matches what is known around it);
 *    finer levels start from the coarser matches scaled up and voted with their own pixels. With
 *    colour adaptation (pc_algo_adapt) each copied patch takes a per-channel gain and bias within
 *    limits (Darabi et al., "Image Melding", SIGGRAPH 2012).
 *  - pc_algo_inpaint_best_offset, proximity match: the displacement whose surroundings best match
 *    the ring around the hole (an automatic Healing Brush source).
 *  - pc_algo_inpaint_synthesize, create texture: image quilting after Efros and Freeman
 *    (SIGGRAPH 2001): blocks picked by overlap error, joined along minimum-error boundary cuts.
 *
 * Buffers: interleaved w x h x ch normalised floats (`len` floats, must be w * h * ch) and a w x h
 * mask; outputs equal the input outside the hole, newly allocated (free()). Deterministic
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

/* How far a copied patch may be adjusted to its target per channel: gain * source + bias, the gain
 * within [gain_lo, gain_hi] and the bias within +-bias (Adapt). gain_lo > gain_hi, a negative
 * bias or NaN limits are PC_ERR_INVALID where they are used (the reference panics in clamp). */
typedef struct pc_algo_adapt {
    float gain_lo, gain_hi, bias;
} pc_algo_adapt;

/* Copies taken as they are (Adapt::NONE). */
#define PC_ALGO_ADAPT_NONE ((pc_algo_adapt){1.0f, 1.0f, 0.0f})
bool pc_algo_adapt_is_none(pc_algo_adapt a);
/* PC_OK when the limits are usable (gain_lo <= gain_hi, bias >= 0, none NaN). */
pc_status pc_algo_adapt_check(pc_algo_adapt a, pc_error *err);

/* Gain and bias of one channel for target moments (st = sum t, stt = sum t^2) and source moments
 * (ss, sss) over n samples, within the limits (inpaint::fit). */
void pc_algo_adapt_fit(pc_algo_adapt a, float n, float st, float stt, float ss, float sss, float *gain,
                       float *bias);

/* CompleteParams. */
typedef struct pc_algo_complete_params {
    size_t patch_radius; /* patches are 2r+1 square */
    size_t em_iters;     /* EM iterations at the coarsest level (finer levels fewer) */
    size_t min_em_iters;
    size_t pm_iters; /* PatchMatch passes per EM iteration */
    /* Levels where the hole is wider than this only refine the upsampled match field. */
    size_t refine_extent;
    pc_algo_adapt adapt; /* colour adaptation of the copies */
    uint64_t seed;
} pc_algo_complete_params;

/* {3, 10, 3, 2, 192, NONE, 1}. */
pc_algo_complete_params pc_algo_complete_params_default(void);

/* What a pixel is to the masked completion (Px), one byte per pixel. */
enum {
    PC_ALGO_PX_VOID = 0,   /* not part of the image: ignored */
    PC_ALGO_PX_KNOWN = 1,  /* known context the fill must agree with, never copied from */
    PC_ALGO_PX_SOURCE = 2, /* known and may be copied from */
    PC_ALGO_PX_HOLE = 3    /* to be filled */
};

/* Content-aware completion of `hole` (copying from any pixel outside it). *out is NULL (with PC_OK)
 * when there is no fully-known patch to copy from (the reference's None; callers fall back to a
 * membrane fill). ctl may be NULL; it is checked per EM iteration, PatchMatch pass and row band
 * (PC_ERR_CANCELLED) and gets progress by pyramid level. */
pc_status pc_algo_inpaint_complete(size_t w, size_t h, size_t ch, const float *img, size_t len,
                                   const bool *hole, size_t hole_len, const pc_algo_complete_params *p,
                                   const pc_interrupt *ctl, float **out, pc_error *err);

/* The completion with every pixel's role given (px: w * h PC_ALGO_PX_* bytes; other values are
 * PC_ERR_INVALID). *out NULL when nothing may be copied from (complete_masked). */
pc_status pc_algo_inpaint_complete_masked(size_t w, size_t h, size_t ch, const float *img, size_t len,
                                          const uint8_t *px, size_t px_len, const pc_algo_complete_params *p,
                                          const pc_interrupt *ctl, float **out, pc_error *err);

/* complete_masked_owned: fills img itself (only hole pixels change). *filled is false, img
 * untouched, when nothing may be copied from. On an error (cancelled) hole pixels may hold partial
 * values; nothing else changes. */
pc_status pc_algo_inpaint_complete_masked_inplace(size_t w, size_t h, size_t ch, float *img, size_t len,
                                                  const uint8_t *px, size_t px_len,
                                                  const pc_algo_complete_params *p, const pc_interrupt *ctl,
                                                  bool *filled, pc_error *err);

/* Proximity match: the displacement (*dx, *dy) within max_radius whose surroundings best match the
 * ring of `ring` pixels around the hole, with the displaced hole fully inside the grid and outside
 * the hole. *found is false (PC_OK) when no displacement fits (the reference's None). The radius
 * and ring are clamped to the image's extent. */
pc_status pc_algo_inpaint_best_offset(size_t w, size_t h, size_t ch, const float *img, size_t len,
                                      const bool *hole, size_t hole_len, size_t ring, int32_t max_radius,
                                      bool *found, int32_t *dx, int32_t *dy, pc_error *err);

/* Create texture: fill the hole with blocks (side max(block, 4)) of surrounding texture joined by
 * minimum-error cuts; falls back to the membrane fill when no candidate block fits. block above
 * 2^24 is PC_ERR_LIMIT. */
pc_status pc_algo_inpaint_synthesize(size_t w, size_t h, size_t ch, const float *img, size_t len,
                                     const bool *hole, size_t hole_len, size_t block, uint64_t seed,
                                     float **out, pc_error *err);

#endif
