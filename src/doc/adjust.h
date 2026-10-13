/* Adjustment-layer parameters, data only (port of crates/doc/src/adjust.rs). Evaluation lives
 * in src/compose.
 *
 * Conventions for every src/doc header:
 *   - Types are plain structs with public fields, named after the reference (pc_adjustment for
 *     Adjustment). `Vec<T>` is a pointer + count (`T *x; size_t nx;`, NULL when empty);
 *     `Option<T>` is `bool has_x; T x;` (pointers, blobs and pc_json_value use NULL for None);
 *     `String` is pc_str; `Arc<Vec<u8>>` is a pc_blob reference.
 *   - A zero-initialised value is valid to finalise. Types that own memory have
 *     pc_<type>_fini (NULL-safe, leaves the value zeroed), pc_<type>_clone (out is overwritten,
 *     written only on success) and pc_<type>_eq (the reference's PartialEq: floats compare with
 *     ==, so NaN != NaN and -0.0 == 0.0). "moved" parameters are copied bitwise and taken over
 *     on success only; on failure the caller still owns them.
 *   - Tagged unions (`kind` + `u`) mirror Rust enums; only the active member is meaningful.
 * Nothing here is synchronised: one owner at a time; const access from several threads is fine.
 * JSON (serde) forms are in doc/json.h. */
#ifndef PC_DOC_ADJUST_H
#define PC_DOC_ADJUST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/blob.h"
#include "base/status.h"
#include "base/str.h"

typedef struct pc_curve_point {
    float input;
    float output;
} pc_curve_point;

/* Vec<CurvePoint>. */
typedef struct pc_curve {
    pc_curve_point *pts;
    size_t len;
} pc_curve;

void pc_curve_fini(pc_curve *c);
pc_status pc_curve_clone(const pc_curve *c, pc_curve *out, pc_error *err);
bool pc_curve_eq(const pc_curve *a, const pc_curve *b);
/* The straight line (0,0)-(1,1): two points exactly, or fewer than two points. */
pc_status pc_curve_identity(pc_curve *out, pc_error *err); /* the two-point line */
bool pc_curve_is_identity(const pc_curve_point *pts, size_t n);

/* Entries in a pencil-drawn Curves map (one per 8-bit level, as in PSD `curv` maps). */
#define PC_CURVE_MAP_LEN 256
/* A pencil-mode curve from its map: outputs[k] (0..1) is the output at input k / 255. Missing
 * entries repeat the last one (non-finite ones too); extra entries are ignored. *out gets
 * PC_CURVE_MAP_LEN points. */
pc_status pc_curve_map(const float *outputs, size_t n, pc_curve *out, pc_error *err);
/* The PC_CURVE_MAP_LEN outputs (0..1) of pts read as a pencil-mode curve: straight segments
 * between the (finite) points, constant outside the first/last, identity without two points. */
pc_status pc_curve_map_outputs(const pc_curve_point *pts, size_t n, float out[PC_CURVE_MAP_LEN],
                               pc_error *err);

/* Values in the table of a size^3 Color Lookup (size^3 * 3). False for size < 2 or a length
 * that overflows usize (a stored size is untrusted). */
bool pc_lut3d_len(uint32_t size, size_t *out);

typedef struct pc_levels_channel {
    float in_black;
    float in_white;
    float gamma;
    float out_black;
    float out_white;
} pc_levels_channel;

pc_levels_channel pc_levels_channel_default(void); /* 0, 1, 1, 0, 1 */

/* Which channels the per-channel Levels/Curves records address (see the reference's docs). */
typedef enum pc_tone_space { PC_TONE_RGB, PC_TONE_CMYK, PC_TONE_LAB } pc_tone_space;

/* One of Hue/Saturation's six colour ranges. */
typedef struct pc_hue_range {
    float hue;        /* degrees, -180..=180 */
    float saturation; /* percent */
    float lightness;  /* percent */
    float bounds[4];  /* fade-in start, full, fade-out start, end (degrees; may wrap) */
} pc_hue_range;

pc_hue_range pc_hue_range_neutral(size_t i); /* 0 = reds ... 5 = magentas */
void pc_hue_range_defaults(pc_hue_range out[6]);
void pc_hue_range_canonical_bounds(const float b[4], float out[4]);
bool pc_hue_range_is_neutral(const pc_hue_range *r);
float pc_hue_range_weight(const pc_hue_range *r, float deg);

typedef struct pc_gradient_map_stop {
    float pos;
    float rgb[3];
} pc_gradient_map_stop;

typedef enum pc_adjustment_kind {
    PC_ADJ_BRIGHTNESS_CONTRAST,
    PC_ADJ_LEVELS,
    PC_ADJ_CURVES,
    PC_ADJ_EXPOSURE,
    PC_ADJ_VIBRANCE,
    PC_ADJ_HUE_SATURATION,
    PC_ADJ_COLOR_BALANCE,
    PC_ADJ_BLACK_WHITE,
    PC_ADJ_PHOTO_FILTER,
    PC_ADJ_CHANNEL_MIXER,
    PC_ADJ_COLOR_LOOKUP,
    PC_ADJ_INVERT,
    PC_ADJ_POSTERIZE,
    PC_ADJ_THRESHOLD,
    PC_ADJ_GRADIENT_MAP,
    PC_ADJ_SELECTIVE_COLOR,
    PC_ADJ_UNSUPPORTED
} pc_adjustment_kind;
#define PC_ADJ_KIND_COUNT 17

typedef struct pc_adjustment {
    pc_adjustment_kind kind;
    union {
        struct {
            float brightness;
            float contrast;
            bool legacy;
        } brightness_contrast;
        struct {
            pc_levels_channel master;
            pc_levels_channel per_channel[3];
            pc_tone_space space;
            pc_levels_channel black;
        } levels;
        struct {
            pc_curve master;
            pc_curve per_channel[3];
            pc_tone_space space;
            pc_curve black;
            /* Drawn with the pencil (PSD `curv` map form): every curve is a PC_CURVE_MAP_LEN
             * map (pc_curve_map) evaluated piecewise-linearly instead of a spline. */
            bool pencil;
        } curves;
        struct {
            float exposure;
            float offset;
            float gamma;
        } exposure;
        struct {
            float vibrance;
            float saturation;
        } vibrance;
        struct {
            float hue;
            float saturation;
            float lightness;
            bool colorize;
            pc_hue_range ranges[6];
        } hue_saturation;
        struct {
            float shadows[3];
            float midtones[3];
            float highlights[3];
            bool preserve_luminosity;
        } color_balance;
        struct {
            float weights[6];
            bool has_tint;
            float tint[3];
        } black_white;
        struct {
            float color[3];
            float density;
            bool preserve_luminosity;
        } photo_filter;
        struct {
            float matrix[3][4];
            bool monochrome;
        } channel_mixer;
        struct {
            pc_str name;
            pc_blob *lut; /* Arc<Vec<f32>>: floats (len = bytes / 4); NULL = None */
            uint32_t size;
            bool tetrahedral;
            bool dither;
        } color_lookup;
        struct {
            uint32_t levels;
        } posterize;
        struct {
            float level;
        } threshold;
        struct {
            pc_gradient_map_stop *stops;
            size_t nstops;
            bool reverse;
            bool dither;
        } gradient_map;
        struct {
            bool relative;
            float adjustments[9][4];
        } selective_color;
        struct {
            pc_str psd_key;
            pc_blob *raw; /* Vec<u8> (never None: NULL = empty) */
        } unsupported;
    } u;
} pc_adjustment;

void pc_adjustment_fini(pc_adjustment *a);
pc_status pc_adjustment_clone(const pc_adjustment *a, pc_adjustment *out, pc_error *err);
bool pc_adjustment_eq(const pc_adjustment *a, const pc_adjustment *b);
const char *pc_adjustment_label(const pc_adjustment *a); /* "Hue/Saturation", ... */
/* The float table of a Color Lookup (NULL when lut is NULL); *n gets the count. */
const float *pc_adjustment_lut(const pc_adjustment *a, size_t *n);

void pc_adjustment_default_hue_saturation(pc_adjustment *out);
pc_status pc_adjustment_identity_curves(pc_adjustment *out, pc_error *err);
void pc_adjustment_identity_levels(pc_adjustment *out);

#endif
