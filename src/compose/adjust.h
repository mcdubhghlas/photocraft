/* Adjustment layers evaluated on a composite buffer (port of crates/compose/src/adjust.rs).
 *
 * Formulas are documented approximations of Photoshop behaviour, fitted against
 * Photoshop-rendered composites (see the reference's comments, repeated in adjust.c).
 * pc_compose_transfer_for_document is implemented in fx_basics.c (the render context needs it
 * before any adjustment runs). Pure functions; thread-safe. Functions writing tables take
 * caller-provided arrays of PC_COMPOSE_LUT_SIZE floats per row. */
#ifndef PC_COMPOSE_ADJUST_H
#define PC_COMPOSE_ADJUST_H

#include <stdbool.h>
#include <stdint.h>

#include "doc/doc.h"

struct pc_compose_buffer;

/* Samples per tone / hue table (LUT_SIZE). */
#define PC_COMPOSE_LUT_SIZE 4096

/* The document tone curve used to linearize values for adjustments that work in linear light
 * (Exposure, Photo Filter, Vibrance). */
typedef enum pc_compose_transfer_kind {
    PC_COMPOSE_TRANSFER_SRGB,
    PC_COMPOSE_TRANSFER_GAMMA
} pc_compose_transfer_kind;

typedef struct pc_compose_transfer {
    pc_compose_transfer_kind kind;
    float gamma; /* GAMMA only */
} pc_compose_transfer;

/* Gamma(1) for F32; Gamma(1.732) for Grayscale / Duotone / Bitmap; else sRGB. */
pc_compose_transfer pc_compose_transfer_for_document(pc_color_mode mode, pc_sample_type depth);
/* The curve Exposure linearises through: sRGB documents use a pure 2.2 power. */
pc_compose_transfer pc_compose_transfer_for_exposure(pc_compose_transfer t);
float pc_compose_transfer_decode(pc_compose_transfer t, float v);
float pc_compose_transfer_encode(pc_compose_transfer t, float v);

/* adjust::apply: assuming an sRGB document, unrounded. */
pc_status pc_compose_adjust_apply(const pc_adjustment *adj, struct pc_compose_buffer *buf, pc_error *err);
/* adjust::apply_with: with the document's tone transfer, unrounded. */
pc_status pc_compose_adjust_apply_with(const pc_adjustment *adj, struct pc_compose_buffer *buf,
                                       pc_compose_transfer transfer, pc_error *err);
/* adjust::apply_depth: applies adj to buf in place with the document's transfer in a document of
 * depth (has_depth false is the reference's `None`: unrounded). Integer depths work on whole
 * levels like Photoshop's, 32-bit documents get its float Levels. Pixels with alpha <= 0 are
 * left alone. Fails only with PC_ERR_NOMEM (tables are allocated); buf is then unchanged or
 * partly adjusted. Color Lookup without a usable table and Unsupported are the identity. */
pc_status pc_compose_adjust_apply_depth(const pc_adjustment *adj, struct pc_compose_buffer *buf,
                                        pc_compose_transfer transfer, bool has_depth, pc_sample_type depth,
                                        pc_error *err);

/* Per-channel LUTs of a Levels or Curves adjustment into out (4 rows of PC_COMPOSE_LUT_SIZE),
 * each channel's record composed with the master: the three per_channel channels then black
 * (identity unless the space is CMYK). In Lab the master is ignored. Other adjustments give
 * identity rows. _q works on samples with q steps per unit (has_q false: continuous); _depth
 * in a document of depth (32-bit RGB Levels use levels_float). PC_ERR_NOMEM only. */
pc_status pc_compose_tone_luts(const pc_adjustment *adj, float *out, pc_error *err);
pc_status pc_compose_tone_luts_q(const pc_adjustment *adj, bool has_q, float q, float *out, pc_error *err);
pc_status pc_compose_tone_luts_depth(const pc_adjustment *adj, bool has_depth, pc_sample_type depth,
                                     float *out, pc_error *err);
/* Hue/Saturation's range edits as three hue-indexed tables into out (3 rows: hue shift in
 * degrees, saturation and lightness as fractions), sampled at hue k / (LUT_SIZE - 1) turns. */
void pc_compose_hue_range_tables(const pc_hue_range ranges[6], float *out);
/* One Hue/Saturation evaluation: hue shift in degrees, s and l in -1..=1. */
void pc_compose_hue_saturation(const float c[3], float hue, float s, float l, bool colorize, float out[3]);
/* Black & White grey value (weights in percent: reds, yellows, greens, cyans, blues, magentas). */
float pc_compose_black_white_gray(const float c[3], const float weights[6]);
/* Ordered-dither offset in -0.5..0.5 (4x4 Bayer matrix) for document pixel (x, y). */
float pc_compose_bayer4(int32_t x, int32_t y);
/* Samples a 3D LUT (n^3 RGB triplets, red fastest; lut_len floats) at c, trilinear or
 * tetrahedral. A table shorter than n^3 * 3 or n < 2 gives c unchanged (the reference panics;
 * its callers check first). */
void pc_compose_lut3d_sample(const float *lut, size_t lut_len, size_t n, const float c[3], bool tetrahedral,
                             float out[3]);
/* How much a colour belongs to each Selective Color range: reds, yellows, greens, cyans, blues,
 * magentas, whites, neutrals, blacks. */
void pc_compose_selective_color_weights(const float c[3], float out[9]);
void pc_compose_selective_color(const float c[3], bool relative, const float adj[9][4], float out[3]);
/* Photo Filter's linear-light RGB matrix (see adjust.c). */
void pc_compose_photo_filter_matrix(const float color[3], float density, bool normalize_y, float out[3][3]);
/* Weights of the grey Vibrance > Saturation desaturates to (R, G; blue has none). */
extern const float PC_COMPOSE_VIBRANCE_GREY[2];
/* Positive Vibrance's saturation boost parameters. */
extern const float PC_COMPOSE_VIBRANCE_BOOST[7];
/* Photoshop's Vibrance on one pixel (v, s: the sliders / 100; hi: the sample ceiling). */
void pc_compose_vibrance_px(const float c[3], float v, float s, pc_compose_transfer transfer, float hi,
                            float out[3]);
/* The Vibrance slider (v in [-1, 1]) on a linear-light pixel. */
void pc_compose_vibrance_lin(const float c[3], float v, float out[3]);
/* Modern Brightness (brightness in [-150, 150]) and Contrast (contrast in [-50, 100]) curves. */
float pc_compose_modern_brightness(float v, float brightness);
float pc_compose_modern_contrast(float v, float contrast);
/* Photoshop posterize: levels equal input bins (clamped to 2..=255). */
float pc_compose_posterize(float v, uint32_t levels);
float pc_compose_levels(const pc_levels_channel *ch, float v);
/* Levels on samples with q steps per unit (has_q false, q < 1 or non-finite: continuous). */
float pc_compose_levels_q(const pc_levels_channel *ch, float v, bool has_q, float q);
/* Levels in a 32-bit document (no clipping; a plain power curve mirrored below black). */
float pc_compose_levels_float(const pc_levels_channel *ch, float v);
/* Photoshop curve: a natural cubic spline through the points sampled into out
 * (PC_COMPOSE_LUT_SIZE values, clamped to 0..=1). PC_ERR_NOMEM only. */
pc_status pc_compose_curve_lut(const pc_curve_point *pts, size_t n, float *out, pc_error *err);
void pc_compose_rgb_to_hsl(const float c[3], float *h, float *s, float *l);
void pc_compose_hsl_to_rgb(float h, float s, float l, float out[3]);

#endif
