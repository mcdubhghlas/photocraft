/* Edit > Content-Aware Fill (port of crates/algo/src/content_aware.rs): patch-based completion
 * (inpaint.h) with a restricted sampling area, geometric adaptation and colour adaptation.
 *
 *  - Sampling area: pixels outside the allowed `source` mask are handed to the completion as
 *    unknown, so it never copies from them; only the real hole keeps the synthesised values.
 *  - Rotation / scale / mirror adaptation: the sampling window is extended with rotated, rescaled
 *    and mirrored copies laid out beside it (separated by unknown gaps wider than a patch), a small
 *    enumerated version of Generalized PatchMatch (Barnes et al., ECCV 2010).
 *  - Colour adaptation: the fill's low frequencies are pulled towards the membrane interpolation
 *    of the hole boundary (Perez et al. 2003).
 *
 * Buffers are interleaved w x h x ch normalised floats (any model/depth); deterministic; output
 * newly allocated (free()). Limits as poisson.h; the mosaic of adaptations must also fit them. */
#ifndef PC_ALGO_CONTENT_AWARE_H
#define PC_ALGO_CONTENT_AWARE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "raster/interrupt.h"

/* Most extra source rotations one fill accepts (each adds a full-size copy to the mosaic). */
#define PC_ALGO_CA_MAX_ROTATIONS ((size_t)64)

/* FillOptions. `rotations` is borrowed (rotation_count degrees). */
typedef struct pc_algo_ca_fill_options {
    float color_adaptation; /* 0..=1 (none 0, default 0.35, high 0.65, very high 0.9) */
    const float *rotations; /* extra source rotations in degrees */
    size_t rotation_count;
    bool scale;  /* add 0.8x and 1.25x source copies */
    bool mirror; /* add a horizontally mirrored source copy */
    uint64_t seed;
} pc_algo_ca_fill_options;

/* {0.35, none, false, false, seed 1}. */
pc_algo_ca_fill_options pc_algo_ca_fill_options_default(void);

/* Rotation set for Photoshop's adaptation levels ("none", "low", "medium", "high", "full";
 * anything else is none): writes up to 3 degrees into out and returns the count. */
size_t pc_algo_ca_rotation_level(const char *level, float out[3]);
/* Colour adaptation strength for "none", "default", "high", "veryHigh" (anything else: default). */
float pc_algo_ca_color_level(const char *level);

/* Fill `hole` in img (len floats = w * h * ch), copying only from pixels where `source` is true and
 * not in the hole; falls back to the membrane fill when nothing can be sampled. *out is the full
 * buffer, unchanged outside the hole. ctl (may be NULL) cancels (PC_ERR_CANCELLED) and receives
 * progress up to 1. */
pc_status pc_algo_ca_fill(size_t w, size_t h, size_t ch, const float *img, size_t len, const bool *hole,
                          const bool *source, size_t mask_len, const pc_algo_ca_fill_options *opts,
                          const pc_interrupt *ctl, float **out, pc_error *err);

/* A transformed source copy (rotation in degrees and scale about the centre, optional mirror):
 * *ow x *oh pixels in *img (ch floats each) and usability in *ok (both free()). Exposed for tests. */
pc_status pc_algo_ca_variant(size_t w, size_t h, size_t ch, const float *img, const bool *ok, float deg,
                             float scale, bool mirror, size_t *ow, size_t *oh, float **out_img, bool **out_ok,
                             pc_error *err);

#endif
