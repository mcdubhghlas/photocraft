/* serde-compatible JSON for the paint types (the reference's derived Serialize/Deserialize).
 *
 * Writing matches serde_json::to_string (fields in declaration order, camelCase names, f32 in
 * f32 shortest form, non-finite floats as null, a preset's empty group omitted, gray tiles as
 * {"width","height","data": base64 of little-endian u16 samples}). With PC_PAINT_JSON_VALUE the
 * output is what serde_json::to_value holds (f32 widened to f64); print it with PC_JSON_SORT_KEYS.
 *
 * Reading matches serde_json::from_value(Value) (what the engine does with command params):
 * object keys are visited in sorted order with the last duplicate winning, unknown keys are
 * ignored, sections with #[serde(default)] fill missing fields from their defaults, structs may
 * also be given as arrays, enums are externally tagged ("round", {"sampled": tile}), and errors
 * carry serde's messages ("invalid type: string \"x\", expected f32", "missing field `name`",
 * "unknown variant `x`, expected one of ...", "duplicate field `pressureSize`" when a field and its
 * snake_case alias are both given). Gray tiles accept a base64 string or an array of 0..1 floats.
 *
 * Ownership: pc_paint_json_read writes *out (uninitialised memory of pc_paint_json_size bytes)
 * only on success; release it with pc_paint_json_fini. Thread-safe (no shared state). */
#ifndef PC_PAINT_JSON_H
#define PC_PAINT_JSON_H

#include <stddef.h>

#include <yyjson.h>

#include "base/buf.h"
#include "base/status.h"

typedef enum pc_paint_json_type {
    PC_PAINT_JSON_POINT,          /* pc_paint_point */
    PC_PAINT_JSON_STROKE,         /* pc_paint_stroke */
    PC_PAINT_JSON_BRUSH,          /* pc_paint_brush */
    PC_PAINT_JSON_PRESET,         /* pc_paint_preset */
    PC_PAINT_JSON_GRAY_TILE,      /* pc_paint_gray_tile */
    PC_PAINT_JSON_DYNAMIC,        /* pc_paint_dynamic */
    PC_PAINT_JSON_CONTROL,        /* pc_paint_control */
    PC_PAINT_JSON_TIP,            /* pc_paint_tip */
    PC_PAINT_JSON_SHAPE_DYNAMICS, /* pc_paint_shape_dynamics */
    PC_PAINT_JSON_SCATTERING,     /* pc_paint_scattering */
    PC_PAINT_JSON_MASK_MODE,      /* pc_paint_mask_mode */
    PC_PAINT_JSON_PATTERN_STYLE,  /* pc_paint_pattern_style */
    PC_PAINT_JSON_PATTERN,        /* pc_paint_pattern */
    PC_PAINT_JSON_TEXTURE,        /* pc_paint_texture */
    PC_PAINT_JSON_DUAL_BRUSH,     /* pc_paint_dual_brush */
    PC_PAINT_JSON_COLOR_DYNAMICS, /* pc_paint_color_dynamics */
    PC_PAINT_JSON_TRANSFER,       /* pc_paint_transfer */
    PC_PAINT_JSON_SECTION_LOCKS,  /* pc_paint_section_locks */
    PC_PAINT_JSON_POSE,           /* pc_paint_pose */
    PC_PAINT_JSON_SMOOTHING,      /* pc_paint_smoothing */
    PC_PAINT_JSON_MIXER,          /* pc_paint_mixer_settings */
    /* chunk L3 (replace.h, bg_erase.h) */
    PC_PAINT_JSON_REPLACE_MODE,      /* pc_paint_replace_mode */
    PC_PAINT_JSON_SAMPLING,          /* pc_paint_sampling */
    PC_PAINT_JSON_LIMITS,            /* pc_paint_limits */
    PC_PAINT_JSON_REPLACE_SETTINGS,  /* pc_paint_replace_settings */
    PC_PAINT_JSON_BG_ERASE_SETTINGS  /* pc_paint_bg_erase_settings */
} pc_paint_json_type;
#define PC_PAINT_JSON_TYPE_COUNT 26

enum { PC_PAINT_JSON_VALUE = 1u };

/* Size of the C value for a type (0 for an unknown type). */
size_t pc_paint_json_size(pc_paint_json_type t);
/* Deserialise v into *out (see above). PC_ERR_INVALID with serde's message on bad input,
 * PC_ERR_NOMEM, PC_ERR_LIMIT for a tile over PC_MAX_ALLOC. */
pc_status pc_paint_json_read(pc_paint_json_type t, yyjson_val *v, void *out, pc_error *err);
/* Serialise *v into d; NULL on out-of-memory. */
yyjson_mut_val *pc_paint_json_write(pc_paint_json_type t, yyjson_mut_doc *d, const void *v, unsigned flags);
/* Release a value read by pc_paint_json_read (or built by the type's own functions). */
void pc_paint_json_fini(pc_paint_json_type t, void *v);

/* Conveniences: compact text (serde_json::to_string) appended to *out, and from_str-through-
 * Value parsing (parse errors keep pc_json_parse's message). */
pc_status pc_paint_json_to_string(pc_paint_json_type t, const void *v, unsigned flags, pc_buf *out,
                                  pc_error *err);
pc_status pc_paint_json_from_string(pc_paint_json_type t, const char *text, size_t len, void *out,
                                    pc_error *err);

#endif
