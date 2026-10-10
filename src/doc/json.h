/* serde JSON forms of the document model (and of the geom/color types it embeds), matching the
 * reference's `#[derive(Serialize, Deserialize)]` byte for byte when written with pc_json_write
 * (compact or PC_JSON_PRETTY, without PC_JSON_SORT_KEYS).
 *
 * For every serialisable type T (enums included, passed by pointer):
 *
 *   yyjson_mut_val *T_to_json(yyjson_mut_doc *d, const T *v, unsigned flags);
 *       Builds the value in d; NULL on out-of-memory. Field order, enum tagging, rename rules,
 *       skip_serializing_if and f32 formatting follow the reference. With PC_DOC_JSON_VALUE, f32
 *       fields become plain f64 reals (serde_json::to_value semantics, as text_styles needs).
 *       serde_json::Value fields are embedded normalised (keys sorted).
 *
 *   pc_status T_from_json(yyjson_val *v, T *out, pc_error *err);
 *       serde_json::from_value semantics: unknown fields ignored, #[serde(default)] honoured,
 *       missing Option fields are None, integers range-checked, f32 fields accept any number
 *       (converted with Rust's `as f32`), unit variants as "Name" or {"Name": null}. On failure returns
 *       PC_ERR_INVALID with serde's message ("missing field `x`", "invalid type: string \"a\",
 *       expected f32", "unknown variant `X`, expected one of `A`, `B`", "invalid length 2,
 *       expected ...", "invalid value: integer `300`, expected u8"); out is written only on
 *       success. serde_json::from_str adds " at line L column C"; these messages don't (§7).
 *
 * Thread-safe (no shared state). */
#ifndef PC_DOC_JSON_H
#define PC_DOC_JSON_H

#include "base/json.h"
#include "doc/doc.h"

enum { PC_DOC_JSON_VALUE = 1u };

#define PC_DOC_JSON_DECL(T)                                                                                          \
    yyjson_mut_val *T##_to_json(yyjson_mut_doc *d, const T *v, unsigned flags);                                     \
    pc_status T##_from_json(yyjson_val *v, T *out, pc_error *err);

/* geom / color */
PC_DOC_JSON_DECL(pc_size)
PC_DOC_JSON_DECL(pc_rect)
PC_DOC_JSON_DECL(pc_point)
PC_DOC_JSON_DECL(pc_affine)
PC_DOC_JSON_DECL(pc_sample_type)
PC_DOC_JSON_DECL(pc_color_mode)
PC_DOC_JSON_DECL(pc_pixel_format)
PC_DOC_JSON_DECL(pc_color)
PC_DOC_JSON_DECL(pc_blend_mode)

/* lib.rs (LayerId / DocId are plain u64 numbers) */
PC_DOC_JSON_DECL(pc_locks)
PC_DOC_JSON_DECL(pc_label_color)
PC_DOC_JSON_DECL(pc_fill)
PC_DOC_JSON_DECL(pc_smart_filter)
PC_DOC_JSON_DECL(pc_guides)
PC_DOC_JSON_DECL(pc_color_indicates)

/* adjust.rs */
PC_DOC_JSON_DECL(pc_curve_point)
PC_DOC_JSON_DECL(pc_levels_channel)
PC_DOC_JSON_DECL(pc_tone_space)
PC_DOC_JSON_DECL(pc_hue_range)
PC_DOC_JSON_DECL(pc_adjustment)

/* advanced.rs, blend_if.rs */
PC_DOC_JSON_DECL(pc_knockout)
PC_DOC_JSON_DECL(pc_advanced_blending)
PC_DOC_JSON_DECL(pc_blend_range)
PC_DOC_JSON_DECL(pc_blend_if)

/* effects.rs */
PC_DOC_JSON_DECL(pc_gradient_style)
PC_DOC_JSON_DECL(pc_gradient)
PC_DOC_JSON_DECL(pc_contour)
PC_DOC_JSON_DECL(pc_fx_paint)
PC_DOC_JSON_DECL(pc_fx_common)
PC_DOC_JSON_DECL(pc_shadow)
PC_DOC_JSON_DECL(pc_glow_technique)
PC_DOC_JSON_DECL(pc_glow_source)
PC_DOC_JSON_DECL(pc_glow)
PC_DOC_JSON_DECL(pc_stroke_position)
PC_DOC_JSON_DECL(pc_stroke_fx)
PC_DOC_JSON_DECL(pc_satin)
PC_DOC_JSON_DECL(pc_bevel_style)
PC_DOC_JSON_DECL(pc_bevel_technique)
PC_DOC_JSON_DECL(pc_bevel)
PC_DOC_JSON_DECL(pc_bevel_contour)
PC_DOC_JSON_DECL(pc_bevel_texture)
PC_DOC_JSON_DECL(pc_effect)
PC_DOC_JSON_DECL(pc_global_light)

/* mode.rs */
PC_DOC_JSON_DECL(pc_color_table)
PC_DOC_JSON_DECL(pc_duotone_ink)
PC_DOC_JSON_DECL(pc_duotone)
PC_DOC_JSON_DECL(pc_stack_mode)

/* vector.rs */
PC_DOC_JSON_DECL(pc_knot)
PC_DOC_JSON_DECL(pc_path_op)
PC_DOC_JSON_DECL(pc_fill_rule)
PC_DOC_JSON_DECL(pc_subpath)
PC_DOC_JSON_DECL(pc_path)
PC_DOC_JSON_DECL(pc_clipping_path)
PC_DOC_JSON_DECL(pc_vector_mask)
PC_DOC_JSON_DECL(pc_stroke_align)
PC_DOC_JSON_DECL(pc_line_cap)
PC_DOC_JSON_DECL(pc_line_join)
PC_DOC_JSON_DECL(pc_shape_stroke)
PC_DOC_JSON_DECL(pc_live_shape)

/* text.rs */
PC_DOC_JSON_DECL(pc_kerning)
PC_DOC_JSON_DECL(pc_caps)
PC_DOC_JSON_DECL(pc_font_feature)
PC_DOC_JSON_DECL(pc_font_variation)
PC_DOC_JSON_DECL(pc_char_style)
PC_DOC_JSON_DECL(pc_text_run)
PC_DOC_JSON_DECL(pc_text_align)
PC_DOC_JSON_DECL(pc_text_direction)
PC_DOC_JSON_DECL(pc_paragraph_style)
PC_DOC_JSON_DECL(pc_paragraph_run)
PC_DOC_JSON_DECL(pc_text_shape)
PC_DOC_JSON_DECL(pc_orientation)
PC_DOC_JSON_DECL(pc_antialias)
PC_DOC_JSON_DECL(pc_text_warp)

/* text_styles.rs */
PC_DOC_JSON_DECL(pc_character_style_def)
PC_DOC_JSON_DECL(pc_paragraph_style_def)
PC_DOC_JSON_DECL(pc_text_styles)

/* comps.rs */
PC_DOC_JSON_DECL(pc_artboard_background)
PC_DOC_JSON_DECL(pc_artboard)

/* slices.rs */
PC_DOC_JSON_DECL(pc_slice_origin)
PC_DOC_JSON_DECL(pc_slice_kind)
PC_DOC_JSON_DECL(pc_slice)
PC_DOC_JSON_DECL(pc_slices)

/* variables.rs */
PC_DOC_JSON_DECL(pc_variables)
PC_DOC_JSON_DECL(pc_variable_def)
PC_DOC_JSON_DECL(pc_var_kind)
PC_DOC_JSON_DECL(pc_pixel_method)
PC_DOC_JSON_DECL(pc_pixel_align)
PC_DOC_JSON_DECL(pc_data_set)
PC_DOC_JSON_DECL(pc_data_value)

/* video.rs */
PC_DOC_JSON_DECL(pc_video_source)
PC_DOC_JSON_DECL(pc_timeline)

/* analysis.rs */
PC_DOC_JSON_DECL(pc_measurement_scale)
PC_DOC_JSON_DECL(pc_count_group)
PC_DOC_JSON_DECL(pc_ruler)
PC_DOC_JSON_DECL(pc_measurement)
PC_DOC_JSON_DECL(pc_note)

#undef PC_DOC_JSON_DECL

#endif
