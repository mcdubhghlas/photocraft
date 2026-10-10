/* Per-pixel and per-dab kernels for the toning and focus retouching tools: Dodge, Burn, Sponge,
 * Blur, Sharpen (port of crates/algo/src/retouch.rs). The brush engine decides where and how
 * strongly each dab applies; these functions decide what happens to the pixels.
 *
 * Tone curves are our own design, chosen to match the observed behaviour of Photoshop's tools:
 * the selected range is affected most, pure black and white stay put for Midtones, and "Protect
 * Tones" works on luminance (scaling RGB so hue and saturation are kept, with gamut compression
 * instead of per-channel clipping). Pure; thread-safe. */
#ifndef PC_ALGO_RETOUCH_H
#define PC_ALGO_RETOUCH_H

#include <stdbool.h>
#include <stddef.h>

#include "base/status.h"

/* Tonal range targeted by Dodge/Burn (serde camelCase: "shadows", "midtones", "highlights";
 * default midtones). */
typedef enum pc_algo_tone_range {
    PC_ALGO_TONE_SHADOWS,
    PC_ALGO_TONE_MIDTONES,
    PC_ALGO_TONE_HIGHLIGHTS
} pc_algo_tone_range;

const char *pc_algo_tone_range_name(pc_algo_tone_range r); /* NULL for an unknown value */
bool pc_algo_tone_range_from_name(const char *s, size_t len, pc_algo_tone_range *out);

/* Rec. 601 luma of a straight RGB triple. */
float pc_algo_retouch_luma(const float c[3]);
/* The dodge/burn transfer for one value v (0..1) with strength e (0..1). */
float pc_algo_retouch_tone_curve(float v, float e, pc_algo_tone_range range, bool burn);
/* Dodge (burn false) or Burn one straight-RGB colour with strength e (0..1); with protect_tones
 * the curve is applied to luma and the colour scaled to the new luma. `out` may alias c. */
void pc_algo_retouch_dodge_burn(const float c[3], float e, pc_algo_tone_range range, bool burn,
                                bool protect_tones, float out[3]);
/* HSV-style saturation (chroma / max) of an RGB triple. */
float pc_algo_retouch_saturation(const float c[3]);
/* Sponge: move a colour away from (saturate) or toward its luma grey by amount (0..1); vibrance
 * damps the change where it would clip. `out` may alias c. */
void pc_algo_retouch_sponge(const float c[3], float amount, bool saturate, bool vibrance, float out[3]);

/* Gaussian blur of the window [x0, x1) x [y0, y1) of an interleaved w x h x ch buffer (len
 * floats), reading neighbours with edge clamping. alpha < ch names the alpha channel (colour is
 * blurred premultiplied); pass SIZE_MAX for none. *out holds the window's (x1-x0) x (y1-y0) x ch
 * blurred samples (free()). PC_ERR_INVALID for a window outside the buffer or a bad alpha index;
 * blur radii (ceil(3 sigma)) above 4096 are PC_ERR_LIMIT. */
pc_status pc_algo_retouch_local_blur(const float *data, size_t len, size_t w, size_t h, size_t ch,
                                     size_t alpha, const size_t win[4], float sigma, float **out,
                                     pc_error *err);
/* Unsharp-mask sharpen of a window (same layout): v + amount * (v - blur(v)), sigma 1. With
 * protect_detail each result is clamped to its 3x3 neighbourhood's range. Alpha is unchanged. */
pc_status pc_algo_retouch_local_sharpen(const float *data, size_t len, size_t w, size_t h, size_t ch,
                                        size_t alpha, const size_t win[4], float amount, bool protect_detail,
                                        float **out, pc_error *err);

#endif
