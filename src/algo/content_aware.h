/* Edit > Content-Aware Fill (port of crates/algo/src/content_aware.rs): patch-based completion
 * (inpaint.h) with Photoshop's sampling area, colour adaptation and geometric adaptation.
 *
 *  - Sampling window (pc_algo_ca_sampling_window): the square of side 4 sqrt(max(w,50) max(h,50))
 *    centred on the selection's bounds, slid inside the canvas.
 *  - Auto sampling area (pc_algo_ca_auto_sampling): within the window, only what looks like the
 *    selection's surroundings (local mean colour and contrast clustered on a ring around the hole,
 *    settled by one contrast-sensitive min cut).
 *  - Colour adaptation (pc_algo_adapt): each copied patch may take a gain and a bias towards its
 *    target (Darabi et al., Image Melding, SIGGRAPH 2012), more at each level.
 *  - Rotation / scale / mirror adaptation: the sampling window is extended with transformed copies
 *    laid out beside it (separated by gaps that are no image), a small enumerated version of
 *    Generalized PatchMatch (Barnes et al., ECCV 2010).
 *
 * Buffers are interleaved w x h x ch normalised floats (any model/depth); deterministic; outputs
 * newly allocated (free()). Limits as poisson.h; the mosaic of adaptations must also fit them. */
#ifndef PC_ALGO_CONTENT_AWARE_H
#define PC_ALGO_CONTENT_AWARE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "algo/inpaint.h"
#include "base/status.h"
#include "geom/geom.h"
#include "raster/interrupt.h"

/* Most extra source rotations one fill accepts (each adds a full-size copy to the mosaic). */
#define PC_ALGO_CA_MAX_ROTATIONS ((size_t)64)

/* FillOptions. `rotations` is borrowed (rotation_count degrees). */
typedef struct pc_algo_ca_fill_options {
    pc_algo_adapt color_adaptation; /* pc_algo_ca_color_level */
    const float *rotations;         /* extra source rotations in degrees */
    size_t rotation_count;
    bool scale;  /* add 0.8x and 1.25x source copies */
    bool mirror; /* add a horizontally mirrored source copy */
    uint64_t seed;
} pc_algo_ca_fill_options;

/* {color_level("default"), none, false, false, seed 1}. */
pc_algo_ca_fill_options pc_algo_ca_fill_options_default(void);

/* Rotation set for Photoshop's adaptation levels ("none", "low", "medium", "high", "full";
 * anything else is none): writes up to 3 degrees into out and returns the count. */
size_t pc_algo_ca_rotation_level(const char *level, float out[3]);
/* Colour adaptation for "none", "default", "high", "veryHigh" (anything else: default). */
pc_algo_adapt pc_algo_ca_color_level(const char *level);
/* The Patch / Content-Aware Move tools' Color option (0..10, more is 10) as colour adaptation. */
pc_algo_adapt pc_algo_ca_color_amount(uint8_t color);

/* Photoshop's sampling window for a selection with bounding box `hole` inside `canvas`
 * (PC_RECT_EMPTY when either is empty). */
pc_rect pc_algo_ca_sampling_window(pc_rect hole, pc_rect canvas);

/* Fill `hole` in img (len floats = w * h * ch), copying only from pixels where `source` is true and
 * not in the hole; falls back to the membrane fill when nothing can be sampled. *out is the full
 * buffer, unchanged outside the hole. ctl (may be NULL) cancels (PC_ERR_CANCELLED) and receives
 * progress up to 1. Invalid colour adaptation limits are PC_ERR_INVALID. */
pc_status pc_algo_ca_fill(size_t w, size_t h, size_t ch, const float *img, size_t len, const bool *hole,
                          const bool *source, size_t mask_len, const pc_algo_ca_fill_options *opts,
                          const pc_interrupt *ctl, float **out, pc_error *err);

/* fill_owned: the same, filling img itself (only hole pixels change; after an error they may hold
 * partial values). */
pc_status pc_algo_ca_fill_inplace(size_t w, size_t h, size_t ch, float *img, size_t len, const bool *hole,
                                  const bool *source, size_t mask_len, const pc_algo_ca_fill_options *opts,
                                  const pc_interrupt *ctl, pc_error *err);

/* A transformed source copy (rotation in degrees and scale about the centre, optional mirror):
 * *ow x *oh pixels in *img (ch floats each) and usability in *ok (both free()). ctl may be NULL
 * (PC_ERR_CANCELLED). Exposed for tests. */
pc_status pc_algo_ca_variant(size_t w, size_t h, size_t ch, const float *img, const bool *ok, float deg,
                             float scale, bool mirror, const pc_interrupt *ctl, size_t *ow, size_t *oh,
                             float **out_img, bool **out_ok, pc_error *err);

/* Auto sampling area within a window (the whole w x h buffer): *out (w * h bools, free()) marks
 * the pixels that look like the hole's surroundings (never the hole); everything outside the hole
 * when that area would be too small. ctl may be NULL (PC_ERR_CANCELLED). */
pc_status pc_algo_ca_auto_sampling(size_t w, size_t h, size_t ch, const float *img, size_t len,
                                   const bool *hole, size_t hole_len, const pc_interrupt *ctl, bool **out,
                                   pc_error *err);

/* Gain and bias per channel (out: ch pairs) taking the samples `from` (nfrom floats, ch per
 * sample) to the level and contrast of `to` (nto floats), within a (level_transform). */
pc_status pc_algo_ca_level_transform(size_t ch, const float *from, size_t nfrom, const float *to, size_t nto,
                                     pc_algo_adapt a, float (*out)[2], pc_error *err);

#endif
