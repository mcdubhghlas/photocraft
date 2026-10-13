/* Filter > Camera Raw Filter: a float develop pipeline over RGB pixels (port of
 * crates/algo/src/camera_raw.rs).
 *
 * Stages, in order (each skipped when its controls are neutral, so defaults are an identity):
 * white balance and exposure in linear light; Dehaze (dark channel prior, He/Sun/Tang 2009);
 * tone on luminance (Highlights / Shadows weighted by a guided-filter base layer, Whites, Blacks,
 * Contrast, Clarity, Texture); Vibrance / Saturation; tone curve (parametric regions, then
 * monotone cubic point curves); Color Mixer (eight hue bands); Color Grading wheels; Detail
 * (sharpening, luminance and colour noise reduction); post-crop vignette and grain.
 *
 * Settings: pc_algo_camera_raw holds Adobe Camera Raw's slider units. Point curves are owned
 * vectors and vignette_style an owned string (NULL means the default "highlightPriority"), so a
 * settings struct is released with pc_algo_camera_raw_fini. JSON follows the reference's serde
 * (camelCase, every field defaulted).
 *
 * Thread safety: functions are reentrant; develop runs its per-row work on the global pool and
 * its result does not depend on the thread count. */
#ifndef PC_ALGO_CAMERA_RAW_H
#define PC_ALGO_CAMERA_RAW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <yyjson.h>

#include "algo/params.h"
#include "base/status.h"

/* Most points a Camera Raw point curve accepts from commands and the control channel. */
#define PC_ALGO_CR_MAX_CURVE_POINTS 16

/* One colour grading wheel. */
typedef struct pc_algo_cr_wheel {
    float hue; /* degrees 0..360 */
    float sat; /* 0..=100 */
    float lum; /* -100..=100 */
} pc_algo_cr_wheel;

typedef struct pc_algo_camera_raw {
    /* Basic */
    float temperature, tint, exposure, contrast, highlights, shadows, whites, blacks, texture, clarity,
        dehaze, vibrance, saturation;
    /* Tone curve */
    float curve_highlights, curve_lights, curve_darks, curve_shadows;
    float curve_splits[3]; /* percent */
    /* [input, output] in 0..=255 (empty = linear) */
    pc_algo_points point_curve, point_curve_red, point_curve_green, point_curve_blue;
    /* Color mixer: red, orange, yellow, green, aqua, blue, purple, magenta */
    float hsl_hue[8], hsl_sat[8], hsl_lum[8];
    /* Color grading */
    pc_algo_cr_wheel grade_shadows, grade_midtones, grade_highlights, grade_global;
    float grade_blending, grade_balance;
    /* Detail */
    float sharpen_amount, sharpen_radius, sharpen_detail, sharpen_masking;
    float noise_luminance, noise_luminance_detail, noise_color, noise_color_detail;
    /* Effects */
    float grain_amount, grain_size, grain_roughness;
    float vignette_amount, vignette_midpoint, vignette_roundness, vignette_feather, vignette_highlights;
    /* "highlightPriority" | "colorPriority" | "paintOverlay" (anything else acts as the first);
     * owned, NULL = "highlightPriority" */
    char *vignette_style;
    uint32_t seed;
    /* Pixel-size multiplier for previews on downsampled proxies (radii in px scale by it). */
    float pixel_scale;
    /* The document's transfer curve: 0 for sRGB's, else the gamma of a power-law space (1.8 for
     * ProPhoto). Set from the document's profile; Highlights/Shadows/Whites/Blacks act on light. */
    float encoding_gamma;
} pc_algo_camera_raw;

/* Hue band centres of the Color Mixer (degrees). */
extern const float PC_ALGO_CR_HSL_BANDS[8];

/* CameraRaw::default() (allocates nothing). */
void pc_algo_camera_raw_default(pc_algo_camera_raw *p);
/* Releases the curves and the style string; leaves the defaults. NULL-safe. */
void pc_algo_camera_raw_fini(pc_algo_camera_raw *p);
/* Deep copy; dst is written on success. */
pc_status pc_algo_camera_raw_copy(pc_algo_camera_raw *dst, const pc_algo_camera_raw *src, pc_error *err);
/* PartialEq (NaN fields are unequal, as in the reference). */
bool pc_algo_camera_raw_eq(const pc_algo_camera_raw *a, const pc_algo_camera_raw *b);
/* The style string ("highlightPriority" for NULL). */
const char *pc_algo_camera_raw_style(const pc_algo_camera_raw *p);

/* serde: from a JSON object (missing fields keep their defaults); `out` written on success. */
pc_status pc_algo_camera_raw_from_json(yyjson_val *v, pc_algo_camera_raw *out, pc_error *err);
/* serde: every field in declaration order; NULL on out-of-memory. */
yyjson_mut_val *pc_algo_camera_raw_to_json(yyjson_mut_doc *d, const pc_algo_camera_raw *p);

/* Checks a point curve from untrusted input: empty (linear) or 2..=16 finite points in 0..=255
 * with inputs increasing by at least one level. PC_ERR_INVALID with the reference's message. */
pc_status pc_algo_cr_validate_curve(const float (*pts)[2], size_t n, pc_error *err);
/* Every curve validated, messages prefixed with the field ("pointCurve: ..."). */
pc_status pc_algo_camera_raw_validate(const pc_algo_camera_raw *p, pc_error *err);
/* validate() for an edit of `stored` settings: a curve left as stored is accepted. */
pc_status pc_algo_camera_raw_validate_changes(const pc_algo_camera_raw *p, const pc_algo_camera_raw *stored,
                                              pc_error *err);
/* True when every control is neutral (the filter is an identity). */
bool pc_algo_camera_raw_is_identity(const pc_algo_camera_raw *p);

/* Monotone cubic (Fritsch-Carlson) through pts (0..=255 units) evaluated into out[0..n). */
pc_status pc_algo_cr_curve_lut(const float (*pts)[2], size_t npts, float *out, size_t n, pc_error *err);
/* Weights of the eight HSL bands at hue h (hat functions between neighbouring centres). */
void pc_algo_cr_band_weights(float h, float out[8]);
/* White-balance channel gains for Temperature / Tint (-100..100). */
void pc_algo_cr_white_balance_gains(float temperature, float tint, float out[3]);

/* Runs the pipeline on straight RGBA pixels (n == w * h, display-encoded RGB) in place. `float_doc`
 * keeps values above 1 (32-bit documents). PC_ERR_INVALID when n != w * h or for a sharpening
 * Detail whose clamp bounds cross (the reference panics); PC_ERR_LIMIT for blur radii past
 * PC_ALGO_MAX_BOX_RADIUS. On failure the pixels may be partly developed. */
pc_status pc_algo_camera_raw_develop(float (*px)[4], size_t n, size_t w, size_t h,
                                     const pc_algo_camera_raw *p, bool float_doc, pc_error *err);

#endif
