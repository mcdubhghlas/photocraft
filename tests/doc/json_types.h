/* Test-side dispatch over every serialisable doc type: from_json / to_json / finalise by the
 * reference's type name ("adjustment", "char_style", ...). Shared by tests/doc/test_json*.c. */
#ifndef PC_TESTS_DOC_JSON_TYPES_H
#define PC_TESTS_DOC_JSON_TYPES_H

#include <stdlib.h>
#include <string.h>

#include "doc/doc.h"
#include "doc/json.h"

typedef struct jt_entry {
    const char *name;
    size_t size;
    pc_status (*from)(yyjson_val *v, void *out, pc_error *err);
    yyjson_mut_val *(*to)(yyjson_mut_doc *d, const void *v, unsigned flags);
    void (*fini)(void *v);
} jt_entry;

#define JT_FNS(nm, T, FINI)                                                                                  \
    static pc_status nm##_from_(yyjson_val *v, void *o, pc_error *e)                                         \
    {                                                                                                        \
        return T##_from_json(v, o, e);                                                                       \
    }                                                                                                        \
    static yyjson_mut_val *nm##_to_(yyjson_mut_doc *d, const void *v, unsigned f)                            \
    {                                                                                                        \
        return T##_to_json(d, v, f);                                                                         \
    }                                                                                                        \
    static void nm##_fini_(void *p)                                                                          \
    {                                                                                                        \
        T *x = p;                                                                                            \
        (void)x;                                                                                             \
        FINI;                                                                                                \
    }
#define JT_ROW(nm, T) {#nm, sizeof(T), nm##_from_, nm##_to_, nm##_fini_}

JT_FNS(size, pc_size, (void)0)
JT_FNS(rect, pc_rect, (void)0)
JT_FNS(point, pc_point, (void)0)
JT_FNS(affine, pc_affine, (void)0)
JT_FNS(sample_type, pc_sample_type, (void)0)
JT_FNS(color_mode, pc_color_mode, (void)0)
JT_FNS(pixel_format, pc_pixel_format, (void)0)
JT_FNS(color, pc_color, (void)0)
JT_FNS(blend_mode, pc_blend_mode, (void)0)
JT_FNS(locks, pc_locks, (void)0)
JT_FNS(label_color, pc_label_color, (void)0)
JT_FNS(fill, pc_fill, pc_fill_fini(x))
JT_FNS(smart_filter, pc_smart_filter, pc_smart_filter_fini(x))
JT_FNS(guides, pc_guides, pc_guides_fini(x))
JT_FNS(color_indicates, pc_color_indicates, (void)0)
JT_FNS(curve_point, pc_curve_point, (void)0)
JT_FNS(levels_channel, pc_levels_channel, (void)0)
JT_FNS(tone_space, pc_tone_space, (void)0)
JT_FNS(hue_range, pc_hue_range, (void)0)
JT_FNS(adjustment, pc_adjustment, pc_adjustment_fini(x))
JT_FNS(knockout, pc_knockout, (void)0)
JT_FNS(advanced_blending, pc_advanced_blending, (void)0)
JT_FNS(blend_range, pc_blend_range, (void)0)
JT_FNS(blend_if, pc_blend_if, pc_blend_if_fini(x))
JT_FNS(gradient_style, pc_gradient_style, (void)0)
JT_FNS(gradient, pc_gradient, pc_gradient_fini(x))
JT_FNS(contour, pc_contour, pc_contour_fini(x))
JT_FNS(fx_paint, pc_fx_paint, pc_fx_paint_fini(x))
JT_FNS(fx_common, pc_fx_common, (void)0)
JT_FNS(shadow, pc_shadow, pc_contour_fini(&x->contour))
JT_FNS(glow_technique, pc_glow_technique, (void)0)
JT_FNS(glow_source, pc_glow_source, (void)0)
JT_FNS(glow, pc_glow, (pc_fx_paint_fini(&x->paint), pc_contour_fini(&x->contour)))
JT_FNS(stroke_position, pc_stroke_position, (void)0)
JT_FNS(stroke_fx, pc_stroke_fx, pc_fx_paint_fini(&x->paint))
JT_FNS(satin, pc_satin, pc_contour_fini(&x->contour))
JT_FNS(bevel_style, pc_bevel_style, (void)0)
JT_FNS(bevel_technique, pc_bevel_technique, (void)0)
JT_FNS(bevel, pc_bevel,
       (pc_contour_fini(&x->gloss_contour), pc_contour_fini(&x->contour.contour),
        pc_str_fini(&x->texture.name), pc_str_fini(&x->texture.id)))
JT_FNS(bevel_contour, pc_bevel_contour, pc_contour_fini(&x->contour))
JT_FNS(bevel_texture, pc_bevel_texture, (pc_str_fini(&x->name), pc_str_fini(&x->id)))
JT_FNS(effect, pc_effect, pc_effect_fini(x))
JT_FNS(global_light, pc_global_light, (void)0)
JT_FNS(color_table, pc_color_table, pc_color_table_fini(x))
JT_FNS(duotone_ink, pc_duotone_ink, pc_duotone_ink_fini(x))
JT_FNS(duotone, pc_duotone, pc_duotone_fini(x))
JT_FNS(stack_mode, pc_stack_mode, (void)0)
JT_FNS(knot, pc_knot, (void)0)
JT_FNS(path_op, pc_path_op, (void)0)
JT_FNS(fill_rule, pc_fill_rule, (void)0)
JT_FNS(subpath, pc_subpath, pc_subpath_fini(x))
JT_FNS(path, pc_path, pc_path_fini(x))
JT_FNS(clipping_path, pc_clipping_path, pc_clipping_path_fini(x))
JT_FNS(vector_mask, pc_vector_mask, pc_vector_mask_fini(x))
JT_FNS(stroke_align, pc_stroke_align, (void)0)
JT_FNS(line_cap, pc_line_cap, (void)0)
JT_FNS(line_join, pc_line_join, (void)0)
JT_FNS(shape_stroke, pc_shape_stroke, pc_shape_stroke_fini(x))
JT_FNS(live_shape, pc_live_shape, (void)0)
JT_FNS(kerning, pc_kerning, (void)0)
JT_FNS(caps, pc_caps, (void)0)
JT_FNS(font_feature, pc_font_feature, pc_str_fini(&x->tag))
JT_FNS(font_variation, pc_font_variation, pc_str_fini(&x->axis))
JT_FNS(char_style, pc_char_style, pc_char_style_fini(x))
JT_FNS(text_run, pc_text_run, pc_char_style_fini(&x->style))
JT_FNS(text_align, pc_text_align, (void)0)
JT_FNS(text_direction, pc_text_direction, (void)0)
JT_FNS(paragraph_style, pc_paragraph_style, (void)0)
JT_FNS(paragraph_run, pc_paragraph_run, (void)0)
JT_FNS(text_shape, pc_text_shape, (void)0)
JT_FNS(orientation, pc_orientation, (void)0)
JT_FNS(antialias, pc_antialias, (void)0)
JT_FNS(text_warp, pc_text_warp, pc_text_warp_fini(x))
JT_FNS(character_style_def, pc_character_style_def, pc_character_style_def_fini(x))
JT_FNS(paragraph_style_def, pc_paragraph_style_def, pc_paragraph_style_def_fini(x))
JT_FNS(text_styles, pc_text_styles, pc_text_styles_fini(x))
JT_FNS(artboard_background, pc_artboard_background, (void)0)
JT_FNS(artboard, pc_artboard, pc_artboard_fini(x))
JT_FNS(slice_origin, pc_slice_origin, (void)0)
JT_FNS(slice_kind, pc_slice_kind, (void)0)
JT_FNS(slice, pc_slice, pc_slice_fini(x))
JT_FNS(slices, pc_slices, pc_slices_fini(x))
JT_FNS(variables, pc_variables, pc_variables_fini(x))
JT_FNS(variable_def, pc_variable_def, pc_variable_def_fini(x))
JT_FNS(var_kind, pc_var_kind, (void)0)
JT_FNS(pixel_method, pc_pixel_method, (void)0)
JT_FNS(pixel_align, pc_pixel_align, (void)0)
JT_FNS(data_set, pc_data_set, pc_data_set_fini(x))
JT_FNS(data_value, pc_data_value, pc_data_value_fini(x))
JT_FNS(video_source, pc_video_source, pc_video_source_fini(x))
JT_FNS(timeline, pc_timeline, (void)0)
JT_FNS(measurement_scale, pc_measurement_scale, pc_measurement_scale_fini(x))
JT_FNS(count_group, pc_count_group, pc_count_group_fini(x))
JT_FNS(ruler, pc_ruler, (void)0)
JT_FNS(measurement, pc_measurement, pc_measurement_fini(x))
JT_FNS(note, pc_note, pc_note_fini(x))

static const jt_entry JT_TYPES[] = {
    JT_ROW(size, pc_size),
    JT_ROW(rect, pc_rect),
    JT_ROW(point, pc_point),
    JT_ROW(affine, pc_affine),
    JT_ROW(sample_type, pc_sample_type),
    JT_ROW(color_mode, pc_color_mode),
    JT_ROW(pixel_format, pc_pixel_format),
    JT_ROW(color, pc_color),
    JT_ROW(blend_mode, pc_blend_mode),
    JT_ROW(locks, pc_locks),
    JT_ROW(label_color, pc_label_color),
    JT_ROW(fill, pc_fill),
    JT_ROW(smart_filter, pc_smart_filter),
    JT_ROW(guides, pc_guides),
    JT_ROW(color_indicates, pc_color_indicates),
    JT_ROW(curve_point, pc_curve_point),
    JT_ROW(levels_channel, pc_levels_channel),
    JT_ROW(tone_space, pc_tone_space),
    JT_ROW(hue_range, pc_hue_range),
    JT_ROW(adjustment, pc_adjustment),
    JT_ROW(knockout, pc_knockout),
    JT_ROW(advanced_blending, pc_advanced_blending),
    JT_ROW(blend_range, pc_blend_range),
    JT_ROW(blend_if, pc_blend_if),
    JT_ROW(gradient_style, pc_gradient_style),
    JT_ROW(gradient, pc_gradient),
    JT_ROW(contour, pc_contour),
    JT_ROW(fx_paint, pc_fx_paint),
    JT_ROW(fx_common, pc_fx_common),
    JT_ROW(shadow, pc_shadow),
    JT_ROW(glow_technique, pc_glow_technique),
    JT_ROW(glow_source, pc_glow_source),
    JT_ROW(glow, pc_glow),
    JT_ROW(stroke_position, pc_stroke_position),
    JT_ROW(stroke_fx, pc_stroke_fx),
    JT_ROW(satin, pc_satin),
    JT_ROW(bevel_style, pc_bevel_style),
    JT_ROW(bevel_technique, pc_bevel_technique),
    JT_ROW(bevel, pc_bevel),
    JT_ROW(bevel_contour, pc_bevel_contour),
    JT_ROW(bevel_texture, pc_bevel_texture),
    JT_ROW(effect, pc_effect),
    JT_ROW(global_light, pc_global_light),
    JT_ROW(color_table, pc_color_table),
    JT_ROW(duotone_ink, pc_duotone_ink),
    JT_ROW(duotone, pc_duotone),
    JT_ROW(stack_mode, pc_stack_mode),
    JT_ROW(knot, pc_knot),
    JT_ROW(path_op, pc_path_op),
    JT_ROW(fill_rule, pc_fill_rule),
    JT_ROW(subpath, pc_subpath),
    JT_ROW(path, pc_path),
    JT_ROW(clipping_path, pc_clipping_path),
    JT_ROW(vector_mask, pc_vector_mask),
    JT_ROW(stroke_align, pc_stroke_align),
    JT_ROW(line_cap, pc_line_cap),
    JT_ROW(line_join, pc_line_join),
    JT_ROW(shape_stroke, pc_shape_stroke),
    JT_ROW(live_shape, pc_live_shape),
    JT_ROW(kerning, pc_kerning),
    JT_ROW(caps, pc_caps),
    JT_ROW(font_feature, pc_font_feature),
    JT_ROW(font_variation, pc_font_variation),
    JT_ROW(char_style, pc_char_style),
    JT_ROW(text_run, pc_text_run),
    JT_ROW(text_align, pc_text_align),
    JT_ROW(text_direction, pc_text_direction),
    JT_ROW(paragraph_style, pc_paragraph_style),
    JT_ROW(paragraph_run, pc_paragraph_run),
    JT_ROW(text_shape, pc_text_shape),
    JT_ROW(orientation, pc_orientation),
    JT_ROW(antialias, pc_antialias),
    JT_ROW(text_warp, pc_text_warp),
    JT_ROW(character_style_def, pc_character_style_def),
    JT_ROW(paragraph_style_def, pc_paragraph_style_def),
    JT_ROW(text_styles, pc_text_styles),
    JT_ROW(artboard_background, pc_artboard_background),
    JT_ROW(artboard, pc_artboard),
    JT_ROW(slice_origin, pc_slice_origin),
    JT_ROW(slice_kind, pc_slice_kind),
    JT_ROW(slice, pc_slice),
    JT_ROW(slices, pc_slices),
    JT_ROW(variables, pc_variables),
    JT_ROW(variable_def, pc_variable_def),
    JT_ROW(var_kind, pc_var_kind),
    JT_ROW(pixel_method, pc_pixel_method),
    JT_ROW(pixel_align, pc_pixel_align),
    JT_ROW(data_set, pc_data_set),
    JT_ROW(data_value, pc_data_value),
    JT_ROW(video_source, pc_video_source),
    JT_ROW(timeline, pc_timeline),
    JT_ROW(measurement_scale, pc_measurement_scale),
    JT_ROW(count_group, pc_count_group),
    JT_ROW(ruler, pc_ruler),
    JT_ROW(measurement, pc_measurement),
    JT_ROW(note, pc_note),
};

static const jt_entry *jt_find(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof JT_TYPES / sizeof JT_TYPES[0]; i++) {
        if (strcmp(JT_TYPES[i].name, name) == 0) {
            return &JT_TYPES[i];
        }
    }
    return NULL;
}

#endif
