/* Destructive tone and colour adjustments that need more than a per-pixel formula: Shadows /
 * Highlights, Replace Color, Match Color and HDR Toning (port of crates/algo/src/tone.rs).
 *
 * All work on straight RGBA pixels in 0..=1, row-major w x h (`n` must equal w * h, else
 * PC_ERR_INVALID; the reference panics on a short buffer). Formulas are documented
 * approximations of Photoshop's behaviour. Per-pixel stages run in parallel on the global pool;
 * results do not depend on the thread count. */
#ifndef PC_ALGO_TONE_H
#define PC_ALGO_TONE_H

#include <stdbool.h>
#include <stddef.h>

#include "base/status.h"

/* Image > Adjustments > Shadows/Highlights (percentages 0..=100), as measured on Photoshop 25.4
 * (tone_sh.c): an edge-aware neighbourhood brightness picks the tones, Amount sets a screening
 * curve, only the new luma is kept and the colour differences are re-added scaled by Color. */
typedef struct pc_algo_shadows_highlights {
    float shadow_amount, shadow_tone, shadow_radius;
    float highlight_amount, highlight_tone, highlight_radius;
    float color;                  /* -100..=100 */
    float midtone;                /* -100..=100 */
    float black_clip, white_clip; /* percent of pixels clipped, 0..=50 */
} pc_algo_shadows_highlights_params;

pc_algo_shadows_highlights_params pc_algo_shadows_highlights_default(void);
pc_status pc_algo_shadows_highlights(float (*px)[4], size_t n, size_t w, size_t h,
                                     const pc_algo_shadows_highlights_params *p, pc_error *err);
/* Its pieces, exposed for tests: the tonal mask for Tone (0..=100) at the 256 neighbourhood
 * levels, the curve strength for Amount (0..=100), and the curve of strength s at v. */
void pc_algo_sh_tonal_mask(float tone, float mask[256]);
float pc_algo_sh_strength(float amount);
float pc_algo_sh_lift(float s, float v);

/* Replace Color: Color Range selection (fuzziness 0..=200) around `color`, then a hue (+-180),
 * saturation and lightness (+-100) shift weighted by it. Writes the mask (n values) when mask is
 * non-NULL. */
void pc_algo_replace_color(float (*px)[4], size_t n, const float color[3], float fuzziness, float hue,
                           float saturation, float lightness, float *mask);

/* Lab mean and standard deviation of the opaque pixels. */
typedef struct pc_algo_lab_stats {
    float mean[3];
    float std[3];
} pc_algo_lab_stats;

/* Statistics weighted by alpha x mask (mask may be NULL; mask_len values, missing ones 0).
 * False when no pixel has weight. */
bool pc_algo_lab_stats_compute(const float (*px)[4], size_t n, const float *mask, size_t mask_len,
                               pc_algo_lab_stats *out);

typedef struct pc_algo_match_color {
    float luminance; /* 1..=200 */
    float intensity; /* 1..=200 */
    float fade;      /* 0..=100 */
    bool neutralize;
} pc_algo_match_color;

pc_algo_match_color pc_algo_match_color_default(void);
/* Reinhard-style colour transfer in Lab toward `source` (NULL: the image's own statistics). */
void pc_algo_match_color_apply(float (*px)[4], size_t n, const pc_algo_lab_stats *target,
                               const pc_algo_lab_stats *source, const pc_algo_match_color *o);

/* HDR Toning (Local Adaptation). `curve` is (input, output) pairs; NULL / 0 = identity. */
typedef struct pc_algo_hdr_toning {
    float radius, strength, gamma, exposure, detail, shadow, highlight, vibrance, saturation;
    const float (*curve)[2];
    size_t curve_len;
} pc_algo_hdr_toning_params;

pc_algo_hdr_toning_params pc_algo_hdr_toning_default(void);
pc_status pc_algo_hdr_toning(float (*px)[4], size_t n, size_t w, size_t h, const pc_algo_hdr_toning_params *p,
                             pc_error *err);

#endif
