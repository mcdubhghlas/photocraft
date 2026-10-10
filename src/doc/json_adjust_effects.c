/* serde JSON forms of adjust.rs, advanced.rs, blend_if.rs, effects.rs and mode.rs. See
 * doc/json.h. */
#include "doc/json_util.h"

#include <string.h>

/* ---- adjust.rs ---- */

static const jfield curve_point_f[] = {
    JFV("input", pc_curve_point, input, pc_jt_f32),
    JFV("output", pc_curve_point, output, pc_jt_f32),
};
static const jstruct curve_point_s = {"struct CurvePoint", curve_point_f, JCOUNT(curve_point_f), NULL,
                                      sizeof(pc_curve_point)};
const jtype pc_jt_curve_point = JT_STRUCT(pc_curve_point, curve_point_s);

/* Vec<CurvePoint> held as a pc_curve value (array elements of Curves.per_channel). */
static const jvecpair curve_vp = {offsetof(pc_curve, pts), offsetof(pc_curve, len)};
const jtype pc_jt_curve = {sizeof(pc_curve),  pc_jt_vecval_read,  pc_jt_vecval_write,
                           pc_jt_vecval_fini, &pc_jt_curve_point, 0,
                           &curve_vp};

static const jfield levels_channel_f[] = {
    JFV("in_black", pc_levels_channel, in_black, pc_jt_f32),
    JFV("in_white", pc_levels_channel, in_white, pc_jt_f32),
    JFV("gamma", pc_levels_channel, gamma, pc_jt_f32),
    JFV("out_black", pc_levels_channel, out_black, pc_jt_f32),
    JFV("out_white", pc_levels_channel, out_white, pc_jt_f32),
};
static const jstruct levels_channel_s = {"struct LevelsChannel", levels_channel_f, JCOUNT(levels_channel_f),
                                         NULL, sizeof(pc_levels_channel)};
static const jtype jt_levels_channel = JT_STRUCT(pc_levels_channel, levels_channel_s);

static void levels_channel_default(void *slot)
{
    pc_levels_channel c = {0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    memcpy(slot, &c, sizeof c);
}

static const jvariant tone_space_v[] = {JVU("Rgb"), JVU("Cmyk"), JVU("Lab")};
static const jenum tone_space_e = JENUM_UNITS(pc_tone_space, tone_space_v);
static const jtype jt_tone_space = JT_UNIT_ENUM(pc_tone_space, tone_space_e);

static const jtype f32x3 = JT_ARRAY(float, pc_jt_f32, 3);
static const jtype f32x4 = JT_ARRAY(float, pc_jt_f32, 4);
static const jtype f32x6 = JT_ARRAY(float, pc_jt_f32, 6);

static const jfield hue_range_f[] = {
    JFV("hue", pc_hue_range, hue, pc_jt_f32),
    JFV("saturation", pc_hue_range, saturation, pc_jt_f32),
    JFV("lightness", pc_hue_range, lightness, pc_jt_f32),
    JFV("bounds", pc_hue_range, bounds, f32x4),
};
static const jstruct hue_range_s = {"struct HueRange", hue_range_f, JCOUNT(hue_range_f), NULL,
                                    sizeof(pc_hue_range)};
static const jtype jt_hue_range = JT_STRUCT(pc_hue_range, hue_range_s);

/* default_hue_ranges(): HueRange::neutral(0..6). */
static void hue_ranges_default(void *slot)
{
    pc_hue_range r[6];
    size_t i;
    for (i = 0; i < 6; i++) {
        float c = 60.0f * (float)i;
        r[i].hue = 0.0f;
        r[i].saturation = 0.0f;
        r[i].lightness = 0.0f;
        r[i].bounds[0] = c - 45.0f;
        r[i].bounds[1] = c - 15.0f;
        r[i].bounds[2] = c + 15.0f;
        r[i].bounds[3] = c + 45.0f;
    }
    memcpy(slot, r, sizeof r);
}

static const jtype levels_x3 = JT_ARRAY(pc_levels_channel, jt_levels_channel, 3);
static const jtype curve_x3 = JT_ARRAY(pc_curve, pc_jt_curve, 3);
static const jtype hue_range_x6 = JT_ARRAY(pc_hue_range, jt_hue_range, 6);
static const jtype f32x4x3 = JT_ARRAY(float[4], f32x4, 3);
static const jtype f32x4x9 = JT_ARRAY(float[4], f32x4, 9);

#define A pc_adjustment

static const jfield adj_bc_f[] = {
    JFV("brightness", A, u.brightness_contrast.brightness, pc_jt_f32),
    JFV("contrast", A, u.brightness_contrast.contrast, pc_jt_f32),
    JFV("legacy", A, u.brightness_contrast.legacy, pc_jt_bool),
};
static const jfield adj_levels_f[] = {
    JFV("master", A, u.levels.master, jt_levels_channel),
    JFV("per_channel", A, u.levels.per_channel, levels_x3),
    JFVD("space", A, u.levels.space, jt_tone_space, JD_ZERO),
    JFFN("black", A, u.levels.black, jt_levels_channel, levels_channel_default),
};
static const jfield adj_curves_f[] = {
    JFVEC("master", A, u.curves.master.pts, u.curves.master.len, pc_jt_curve_point),
    JFV("per_channel", A, u.curves.per_channel, curve_x3),
    JFVD("space", A, u.curves.space, jt_tone_space, JD_ZERO),
    JFVECD("black", A, u.curves.black.pts, u.curves.black.len, pc_jt_curve_point, JD_ZERO),
};
static const jfield adj_exposure_f[] = {
    JFV("exposure", A, u.exposure.exposure, pc_jt_f32),
    JFV("offset", A, u.exposure.offset, pc_jt_f32),
    JFV("gamma", A, u.exposure.gamma, pc_jt_f32),
};
static const jfield adj_vibrance_f[] = {
    JFV("vibrance", A, u.vibrance.vibrance, pc_jt_f32),
    JFV("saturation", A, u.vibrance.saturation, pc_jt_f32),
};
static const jfield adj_hs_f[] = {
    JFV("hue", A, u.hue_saturation.hue, pc_jt_f32),
    JFV("saturation", A, u.hue_saturation.saturation, pc_jt_f32),
    JFV("lightness", A, u.hue_saturation.lightness, pc_jt_f32),
    JFV("colorize", A, u.hue_saturation.colorize, pc_jt_bool),
    JFFN("ranges", A, u.hue_saturation.ranges, hue_range_x6, hue_ranges_default),
};
static const jfield adj_cb_f[] = {
    JFV("shadows", A, u.color_balance.shadows, f32x3),
    JFV("midtones", A, u.color_balance.midtones, f32x3),
    JFV("highlights", A, u.color_balance.highlights, f32x3),
    JFV("preserve_luminosity", A, u.color_balance.preserve_luminosity, pc_jt_bool),
};
static const jfield adj_bw_f[] = {
    JFV("weights", A, u.black_white.weights, f32x6),
    JFO("tint", A, u.black_white.tint, u.black_white.has_tint, f32x3, JD_REQ),
};
static const jfield adj_pf_f[] = {
    JFV("color", A, u.photo_filter.color, f32x3),
    JFV("density", A, u.photo_filter.density, pc_jt_f32),
    JFV("preserve_luminosity", A, u.photo_filter.preserve_luminosity, pc_jt_bool),
};
static const jfield adj_cm_f[] = {
    JFV("matrix", A, u.channel_mixer.matrix, f32x4x3),
    JFV("monochrome", A, u.channel_mixer.monochrome, pc_jt_bool),
};
static const jfield adj_cl_f[] = {
    JFV("name", A, u.color_lookup.name, pc_jt_str),
    /* Option<Arc<Vec<f32>>>: a missing field is None. */
    JFVD("lut", A, u.color_lookup.lut, pc_jt_f32s_opt, JD_ZERO),
    JFV("size", A, u.color_lookup.size, pc_jt_u32),
    JFVD("tetrahedral", A, u.color_lookup.tetrahedral, pc_jt_bool, JD_ZERO),
    JFVD("dither", A, u.color_lookup.dither, pc_jt_bool, JD_ZERO),
};
static const jfield adj_post_f[] = {JFV("levels", A, u.posterize.levels, pc_jt_u32)};
static const jfield adj_thr_f[] = {JFV("level", A, u.threshold.level, pc_jt_f32)};

static const jtuple_el gm_stop_el[] = {
    {&pc_jt_f32, offsetof(pc_gradient_map_stop, pos)},
    {&f32x3, offsetof(pc_gradient_map_stop, rgb)},
};
static const jtuple gm_stop_tu = {gm_stop_el, 2};
static const jtype jt_gm_stop = JT_TUPLE(pc_gradient_map_stop, gm_stop_tu);

static const jfield adj_gm_f[] = {
    JFVEC("stops", A, u.gradient_map.stops, u.gradient_map.nstops, jt_gm_stop),
    JFV("reverse", A, u.gradient_map.reverse, pc_jt_bool),
    JFVD("dither", A, u.gradient_map.dither, pc_jt_bool, JD_ZERO),
};
static const jfield adj_sc_f[] = {
    JFV("relative", A, u.selective_color.relative, pc_jt_bool),
    JFV("adjustments", A, u.selective_color.adjustments, f32x4x9),
};
static const jfield adj_uns_f[] = {
    JFV("psd_key", A, u.unsupported.psd_key, pc_jt_str),
    JFV("raw", A, u.unsupported.raw, pc_jt_bytes),
};

#define ADJ_ST(id, f, nm)                                                                                    \
    static const jstruct id = {"struct variant Adjustment::" nm, f, JCOUNT(f), NULL, sizeof(A)}
ADJ_ST(adj_bc_s, adj_bc_f, "BrightnessContrast");
ADJ_ST(adj_levels_s, adj_levels_f, "Levels");
ADJ_ST(adj_curves_s, adj_curves_f, "Curves");
ADJ_ST(adj_exposure_s, adj_exposure_f, "Exposure");
ADJ_ST(adj_vibrance_s, adj_vibrance_f, "Vibrance");
ADJ_ST(adj_hs_s, adj_hs_f, "HueSaturation");
ADJ_ST(adj_cb_s, adj_cb_f, "ColorBalance");
ADJ_ST(adj_bw_s, adj_bw_f, "BlackWhite");
ADJ_ST(adj_pf_s, adj_pf_f, "PhotoFilter");
ADJ_ST(adj_cm_s, adj_cm_f, "ChannelMixer");
ADJ_ST(adj_cl_s, adj_cl_f, "ColorLookup");
ADJ_ST(adj_post_s, adj_post_f, "Posterize");
ADJ_ST(adj_thr_s, adj_thr_f, "Threshold");
ADJ_ST(adj_gm_s, adj_gm_f, "GradientMap");
ADJ_ST(adj_sc_s, adj_sc_f, "SelectiveColor");
ADJ_ST(adj_uns_s, adj_uns_f, "Unsupported");

static const jvariant adj_v[] = {
    JVS("BrightnessContrast", adj_bc_s), JVS("Levels", adj_levels_s),     JVS("Curves", adj_curves_s),
    JVS("Exposure", adj_exposure_s),     JVS("Vibrance", adj_vibrance_s), JVS("HueSaturation", adj_hs_s),
    JVS("ColorBalance", adj_cb_s),       JVS("BlackWhite", adj_bw_s),     JVS("PhotoFilter", adj_pf_s),
    JVS("ChannelMixer", adj_cm_s),       JVS("ColorLookup", adj_cl_s),    JVU("Invert"),
    JVS("Posterize", adj_post_s),        JVS("Threshold", adj_thr_s),     JVS("GradientMap", adj_gm_s),
    JVS("SelectiveColor", adj_sc_s),     JVS("Unsupported", adj_uns_s),
};
static const jenum adj_e = JENUM_TAGGED(A, adj_v, kind);
static const jtype jt_adjustment = JT_TAGGED(A, adj_e);
#undef A

/* ---- advanced.rs, blend_if.rs ---- */

static const jvariant knockout_v[] = {JVU("none"), JVU("shallow"), JVU("deep")};
static const jenum knockout_e = JENUM_UNITS(pc_knockout, knockout_v);
static const jtype jt_knockout = JT_UNIT_ENUM(pc_knockout, knockout_e);

static pc_status advanced_default(void *out, pc_error *err)
{
    pc_advanced_blending a = pc_advanced_blending_default();
    (void)err;
    memcpy(out, &a, sizeof a);
    return PC_OK;
}

#define AB pc_advanced_blending
static const jfield advanced_f[] = {
    JFVD("knockout", AB, knockout, jt_knockout, JD_CONT),
    JFVD("blendInterior", AB, blend_interior, pc_jt_bool, JD_CONT),
    JFVD("blendClipped", AB, blend_clipped, pc_jt_bool, JD_CONT),
    JFVD("transparencyShapes", AB, transparency_shapes, pc_jt_bool, JD_CONT),
    JFVD("layerMaskHidesEffects", AB, layer_mask_hides_effects, pc_jt_bool, JD_CONT),
    JFVD("vectorMaskHidesEffects", AB, vector_mask_hides_effects, pc_jt_bool, JD_CONT),
};
static const jstruct advanced_s = {"struct AdvancedBlending", advanced_f, JCOUNT(advanced_f),
                                   advanced_default, sizeof(AB)};
static const jtype jt_advanced = JT_STRUCT(AB, advanced_s);
#undef AB

static const jtype u8x2 = JT_ARRAY(uint8_t, pc_jt_u8, 2);
static const jfield blend_range_f[] = {
    JFV("black", pc_blend_range, black, u8x2),
    JFV("white", pc_blend_range, white, u8x2),
};
static const jstruct blend_range_s = {"struct BlendRange", blend_range_f, JCOUNT(blend_range_f), NULL,
                                      sizeof(pc_blend_range)};
static const jtype jt_blend_range = JT_STRUCT(pc_blend_range, blend_range_s);
static const jtype blend_range_x2 = JT_ARRAY(pc_blend_range, jt_blend_range, 2);

static const jfield blend_if_f[] = {JFVEC("ranges", pc_blend_if, ranges, nranges, blend_range_x2)};
static const jstruct blend_if_s = {"struct BlendIf", blend_if_f, JCOUNT(blend_if_f), NULL,
                                   sizeof(pc_blend_if)};
static const jtype jt_blend_if = JT_STRUCT(pc_blend_if, blend_if_s);

/* ---- effects.rs ---- */

static const jvariant gradient_style_v[] = {JVU("Linear"), JVU("Radial"), JVU("Angle"), JVU("Reflected"),
                                            JVU("Diamond")};
static const jenum gradient_style_e = JENUM_UNITS(pc_gradient_style, gradient_style_v);
const jtype pc_jt_gradient_style = JT_UNIT_ENUM(pc_gradient_style, gradient_style_e);

static const jtuple_el opacity_stop_el[] = {
    {&pc_jt_f32, offsetof(pc_opacity_stop, pos)},
    {&pc_jt_f32, offsetof(pc_opacity_stop, opacity)},
};
static const jtuple opacity_stop_tu = {opacity_stop_el, 2};
static const jtype jt_opacity_stop = JT_TUPLE(pc_opacity_stop, opacity_stop_tu);

static const jfield gradient_f[] = {
    JFVEC("stops", pc_gradient, stops, nstops, pc_jt_color_stop),
    JFVEC("opacity_stops", pc_gradient, opacity_stops, nopacity_stops, jt_opacity_stop),
    JFV("style", pc_gradient, style, pc_jt_gradient_style),
    JFV("angle", pc_gradient, angle, pc_jt_f32),
    JFV("scale", pc_gradient, scale, pc_jt_f32),
    JFV("reverse", pc_gradient, reverse, pc_jt_bool),
    JFV("align", pc_gradient, align, pc_jt_bool),
    JFV("offset", pc_gradient, offset, pc_jt_f32x2t),
};
static const jstruct gradient_s = {"struct Gradient", gradient_f, JCOUNT(gradient_f), NULL,
                                   sizeof(pc_gradient)};
const jtype pc_jt_gradient = JT_STRUCT(pc_gradient, gradient_s);

/* Contour: Linear, or Custom { name, points } (discriminant: the `custom` flag). */
static const jfield contour_custom_f[] = {
    JFV("name", pc_contour, name, pc_jt_str),
    JFVEC("points", pc_contour, points.pts, points.len, pc_jt_curve_point),
};
static const jstruct contour_custom_s = {"struct variant Contour::Custom", contour_custom_f,
                                         JCOUNT(contour_custom_f), NULL, sizeof(pc_contour)};
static const jvariant contour_v[] = {JVU("Linear"), JVS("Custom", contour_custom_s)};
static const jenum contour_e = JENUM_TAGGED(pc_contour, contour_v, custom);
const jtype pc_jt_contour = JT_TAGGED(pc_contour, contour_e);

static const jfield fx_paint_pattern_f[] = {
    JFV("name", pc_fx_paint, u.pattern.name, pc_jt_str),
    JFV("id", pc_fx_paint, u.pattern.id, pc_jt_str),
    JFV("scale", pc_fx_paint, u.pattern.scale, pc_jt_f32),
};
static const jstruct fx_paint_pattern_s = {"struct variant FxPaint::Pattern", fx_paint_pattern_f,
                                           JCOUNT(fx_paint_pattern_f), NULL, sizeof(pc_fx_paint)};
static const jvariant fx_paint_v[] = {
    JVN("Color", pc_jt_color, pc_fx_paint, u.color),
    JVN("Gradient", pc_jt_gradient, pc_fx_paint, u.gradient),
    JVS("Pattern", fx_paint_pattern_s),
};
static const jenum fx_paint_e = JENUM_TAGGED(pc_fx_paint, fx_paint_v, kind);
static const jtype jt_fx_paint = JT_TAGGED(pc_fx_paint, fx_paint_e);

static const jfield fx_common_f[] = {
    JFV("enabled", pc_fx_common, enabled, pc_jt_bool),
    JFV("blend", pc_fx_common, blend, pc_jt_blend_mode),
    JFV("opacity", pc_fx_common, opacity, pc_jt_f32),
};
static const jstruct fx_common_s = {"struct FxCommon", fx_common_f, JCOUNT(fx_common_f), NULL,
                                    sizeof(pc_fx_common)};
const jtype pc_jt_fx_common = JT_STRUCT(pc_fx_common, fx_common_s);

static const jfield shadow_f[] = {
    JFV("common", pc_shadow, common, pc_jt_fx_common),
    JFV("color", pc_shadow, color, pc_jt_color),
    JFV("angle", pc_shadow, angle, pc_jt_f32),
    JFV("use_global_light", pc_shadow, use_global_light, pc_jt_bool),
    JFV("distance", pc_shadow, distance, pc_jt_f32),
    JFV("spread", pc_shadow, spread, pc_jt_f32),
    JFV("size", pc_shadow, size, pc_jt_f32),
    JFV("contour", pc_shadow, contour, pc_jt_contour),
    JFV("anti_alias", pc_shadow, anti_alias, pc_jt_bool),
    JFV("noise", pc_shadow, noise, pc_jt_f32),
    JFV("knocks_out", pc_shadow, knocks_out, pc_jt_bool),
};
static const jstruct shadow_s = {"struct Shadow", shadow_f, JCOUNT(shadow_f), NULL, sizeof(pc_shadow)};
static const jtype jt_shadow = JT_STRUCT(pc_shadow, shadow_s);

static const jvariant glow_technique_v[] = {JVU("Softer"), JVU("Precise")};
static const jenum glow_technique_e = JENUM_UNITS(pc_glow_technique, glow_technique_v);
static const jtype jt_glow_technique = JT_UNIT_ENUM(pc_glow_technique, glow_technique_e);

static const jvariant glow_source_v[] = {JVU("Center"), JVU("Edge")};
static const jenum glow_source_e = JENUM_UNITS(pc_glow_source, glow_source_v);
static const jtype jt_glow_source = JT_UNIT_ENUM(pc_glow_source, glow_source_e);

static const jfield glow_f[] = {
    JFV("common", pc_glow, common, pc_jt_fx_common),
    JFV("paint", pc_glow, paint, jt_fx_paint),
    JFV("technique", pc_glow, technique, jt_glow_technique),
    JFV("spread", pc_glow, spread, pc_jt_f32),
    JFV("size", pc_glow, size, pc_jt_f32),
    JFV("contour", pc_glow, contour, pc_jt_contour),
    JFV("anti_alias", pc_glow, anti_alias, pc_jt_bool),
    JFV("range", pc_glow, range, pc_jt_f32),
    JFV("jitter", pc_glow, jitter, pc_jt_f32),
    JFV("noise", pc_glow, noise, pc_jt_f32),
    JFV("source", pc_glow, source, jt_glow_source),
};
static const jstruct glow_s = {"struct Glow", glow_f, JCOUNT(glow_f), NULL, sizeof(pc_glow)};
static const jtype jt_glow = JT_STRUCT(pc_glow, glow_s);

static const jvariant stroke_position_v[] = {JVU("Outside"), JVU("Inside"), JVU("Center")};
static const jenum stroke_position_e = JENUM_UNITS(pc_stroke_position, stroke_position_v);
static const jtype jt_stroke_position = JT_UNIT_ENUM(pc_stroke_position, stroke_position_e);

static const jfield stroke_fx_f[] = {
    JFV("common", pc_stroke_fx, common, pc_jt_fx_common),
    JFV("size", pc_stroke_fx, size, pc_jt_f32),
    JFV("position", pc_stroke_fx, position, jt_stroke_position),
    JFV("paint", pc_stroke_fx, paint, jt_fx_paint),
};
static const jstruct stroke_fx_s = {"struct StrokeFx", stroke_fx_f, JCOUNT(stroke_fx_f), NULL,
                                    sizeof(pc_stroke_fx)};
static const jtype jt_stroke_fx = JT_STRUCT(pc_stroke_fx, stroke_fx_s);

static const jfield satin_f[] = {
    JFV("common", pc_satin, common, pc_jt_fx_common),
    JFV("color", pc_satin, color, pc_jt_color),
    JFV("angle", pc_satin, angle, pc_jt_f32),
    JFV("distance", pc_satin, distance, pc_jt_f32),
    JFV("size", pc_satin, size, pc_jt_f32),
    JFV("contour", pc_satin, contour, pc_jt_contour),
    JFV("anti_alias", pc_satin, anti_alias, pc_jt_bool),
    JFV("invert", pc_satin, invert, pc_jt_bool),
};
static const jstruct satin_s = {"struct Satin", satin_f, JCOUNT(satin_f), NULL, sizeof(pc_satin)};
static const jtype jt_satin = JT_STRUCT(pc_satin, satin_s);

static const jvariant bevel_style_v[] = {JVU("OuterBevel"), JVU("InnerBevel"), JVU("Emboss"),
                                         JVU("PillowEmboss"), JVU("StrokeEmboss")};
static const jenum bevel_style_e = JENUM_UNITS(pc_bevel_style, bevel_style_v);
static const jtype jt_bevel_style = JT_UNIT_ENUM(pc_bevel_style, bevel_style_e);

static const jvariant bevel_technique_v[] = {JVU("Smooth"), JVU("ChiselHard"), JVU("ChiselSoft")};
static const jenum bevel_technique_e = JENUM_UNITS(pc_bevel_technique, bevel_technique_v);
static const jtype jt_bevel_technique = JT_UNIT_ENUM(pc_bevel_technique, bevel_technique_e);

static const jfield bevel_contour_f[] = {
    JFV("contour", pc_bevel_contour, contour, pc_jt_contour),
    JFV("range", pc_bevel_contour, range, pc_jt_f32),
    JFV("anti_alias", pc_bevel_contour, anti_alias, pc_jt_bool),
};
static const jstruct bevel_contour_s = {"struct BevelContour", bevel_contour_f, JCOUNT(bevel_contour_f), NULL,
                                        sizeof(pc_bevel_contour)};
static const jtype jt_bevel_contour = JT_STRUCT(pc_bevel_contour, bevel_contour_s);

static const jfield bevel_texture_f[] = {
    JFV("name", pc_bevel_texture, name, pc_jt_str),      JFV("id", pc_bevel_texture, id, pc_jt_str),
    JFV("scale", pc_bevel_texture, scale, pc_jt_f32),    JFV("depth", pc_bevel_texture, depth, pc_jt_f32),
    JFV("invert", pc_bevel_texture, invert, pc_jt_bool), JFV("link", pc_bevel_texture, link, pc_jt_bool),
    JFV("phase", pc_bevel_texture, phase, pc_jt_f32x2t),
};
static const jstruct bevel_texture_s = {"struct BevelTexture", bevel_texture_f, JCOUNT(bevel_texture_f), NULL,
                                        sizeof(pc_bevel_texture)};
static const jtype jt_bevel_texture = JT_STRUCT(pc_bevel_texture, bevel_texture_s);

static const jfield bevel_f[] = {
    JFV("enabled", pc_bevel, enabled, pc_jt_bool),
    JFV("style", pc_bevel, style, jt_bevel_style),
    JFV("technique", pc_bevel, technique, jt_bevel_technique),
    JFV("depth", pc_bevel, depth, pc_jt_f32),
    JFV("up", pc_bevel, up, pc_jt_bool),
    JFV("size", pc_bevel, size, pc_jt_f32),
    JFV("soften", pc_bevel, soften, pc_jt_f32),
    JFV("angle", pc_bevel, angle, pc_jt_f32),
    JFV("altitude", pc_bevel, altitude, pc_jt_f32),
    JFV("use_global_light", pc_bevel, use_global_light, pc_jt_bool),
    JFV("gloss_contour", pc_bevel, gloss_contour, pc_jt_contour),
    JFV("highlight", pc_bevel, highlight, pc_jt_fx_common),
    JFV("highlight_color", pc_bevel, highlight_color, pc_jt_color),
    JFV("shadow", pc_bevel, shadow, pc_jt_fx_common),
    JFV("shadow_color", pc_bevel, shadow_color, pc_jt_color),
    JFO("contour", pc_bevel, contour, has_contour, jt_bevel_contour, JD_ZERO),
    JFO("texture", pc_bevel, texture, has_texture, jt_bevel_texture, JD_ZERO),
};
static const jstruct bevel_s = {"struct Bevel", bevel_f, JCOUNT(bevel_f), NULL, sizeof(pc_bevel)};
static const jtype jt_bevel = JT_STRUCT(pc_bevel, bevel_s);

#define E pc_effect
static const jfield fx_color_overlay_f[] = {
    JFV("common", E, u.color_overlay.common, pc_jt_fx_common),
    JFV("color", E, u.color_overlay.color, pc_jt_color),
};
static const jfield fx_gradient_overlay_f[] = {
    JFV("common", E, u.gradient_overlay.common, pc_jt_fx_common),
    JFV("gradient", E, u.gradient_overlay.gradient, pc_jt_gradient),
    JFV("dither", E, u.gradient_overlay.dither, pc_jt_bool),
};
static const jfield fx_pattern_overlay_f[] = {
    JFV("common", E, u.pattern_overlay.common, pc_jt_fx_common),
    JFV("name", E, u.pattern_overlay.name, pc_jt_str),
    JFV("id", E, u.pattern_overlay.id, pc_jt_str),
    JFV("scale", E, u.pattern_overlay.scale, pc_jt_f32),
    JFVD("angle", E, u.pattern_overlay.angle, pc_jt_f32, JD_ZERO),
    JFVD("link", E, u.pattern_overlay.link, pc_jt_bool, JD_TRUE),
    JFVD("phase", E, u.pattern_overlay.phase, pc_jt_f32x2t, JD_ZERO),
};
static const jstruct fx_color_overlay_s = {"struct variant Effect::ColorOverlay", fx_color_overlay_f,
                                           JCOUNT(fx_color_overlay_f), NULL, sizeof(E)};
static const jstruct fx_gradient_overlay_s = {"struct variant Effect::GradientOverlay", fx_gradient_overlay_f,
                                              JCOUNT(fx_gradient_overlay_f), NULL, sizeof(E)};
static const jstruct fx_pattern_overlay_s = {"struct variant Effect::PatternOverlay", fx_pattern_overlay_f,
                                             JCOUNT(fx_pattern_overlay_f), NULL, sizeof(E)};

static const jvariant effect_v[] = {
    JVN("DropShadow", jt_shadow, E, u.shadow),
    JVN("InnerShadow", jt_shadow, E, u.shadow),
    JVN("OuterGlow", jt_glow, E, u.glow),
    JVN("InnerGlow", jt_glow, E, u.glow),
    JVN("Stroke", jt_stroke_fx, E, u.stroke),
    JVS("ColorOverlay", fx_color_overlay_s),
    JVS("GradientOverlay", fx_gradient_overlay_s),
    JVS("PatternOverlay", fx_pattern_overlay_s),
    JVN("Satin", jt_satin, E, u.satin),
    JVN("BevelEmboss", jt_bevel, E, u.bevel),
};
static const jenum effect_e = JENUM_TAGGED(E, effect_v, kind);
static const jtype jt_effect = JT_TAGGED(E, effect_e);
#undef E

static const jfield global_light_f[] = {
    JFV("angle", pc_global_light, angle, pc_jt_f32),
    JFV("altitude", pc_global_light, altitude, pc_jt_f32),
};
static const jstruct global_light_s = {"struct GlobalLight", global_light_f, JCOUNT(global_light_f), NULL,
                                       sizeof(pc_global_light)};
static const jtype jt_global_light = JT_STRUCT(pc_global_light, global_light_s);

/* ---- mode.rs ---- */

static const jtype u8x3 = JT_ARRAY(uint8_t, pc_jt_u8, 3);
static const jfield color_table_f[] = {
    JFVEC("colors", pc_color_table, colors, ncolors, u8x3),
    JFO("transparent", pc_color_table, transparent, has_transparent, pc_jt_u8, JD_ZERO),
};
static const jstruct color_table_s = {"struct ColorTable", color_table_f, JCOUNT(color_table_f), NULL,
                                      sizeof(pc_color_table)};
static const jtype jt_color_table = JT_STRUCT(pc_color_table, color_table_s);

static const jfield duotone_ink_f[] = {
    JFV("name", pc_duotone_ink, name, pc_jt_str),
    JFV("color", pc_duotone_ink, color, f32x3),
    JFVEC("curve", pc_duotone_ink, curve.pts, curve.len, pc_jt_curve_point),
};
static const jstruct duotone_ink_s = {"struct DuotoneInk", duotone_ink_f, JCOUNT(duotone_ink_f), NULL,
                                      sizeof(pc_duotone_ink)};
static const jtype jt_duotone_ink = JT_STRUCT(pc_duotone_ink, duotone_ink_s);

static const jfield duotone_f[] = {
    JFVEC("inks", pc_duotone, inks, ninks, jt_duotone_ink),
    JFVD("psd_raw", pc_duotone, psd_raw, pc_jt_bytes_opt, JD_ZERO),
};
static const jstruct duotone_s = {"struct Duotone", duotone_f, JCOUNT(duotone_f), NULL, sizeof(pc_duotone)};
static const jtype jt_duotone = JT_STRUCT(pc_duotone, duotone_s);

static const jvariant stack_mode_v[] = {
    JVU("Entropy"),   JVU("Kurtosis"), JVU("Maximum"),
    JVU("Mean"),      JVU("Median"),   JVU("Minimum"),
    JVU("Range"),     JVU("Skewness"), JVU("StandardDeviation"),
    JVU("Summation"), JVU("Variance"),
};
static const jenum stack_mode_e = JENUM_UNITS(pc_stack_mode, stack_mode_v);
static const jtype jt_stack_mode = JT_UNIT_ENUM(pc_stack_mode, stack_mode_e);

PC_JSON_API(pc_curve_point, pc_jt_curve_point)
PC_JSON_API(pc_levels_channel, jt_levels_channel)
PC_JSON_API(pc_tone_space, jt_tone_space)
PC_JSON_API(pc_hue_range, jt_hue_range)
PC_JSON_API(pc_adjustment, jt_adjustment)
PC_JSON_API(pc_knockout, jt_knockout)
PC_JSON_API(pc_advanced_blending, jt_advanced)
PC_JSON_API(pc_blend_range, jt_blend_range)
PC_JSON_API(pc_blend_if, jt_blend_if)
PC_JSON_API(pc_gradient_style, pc_jt_gradient_style)
PC_JSON_API(pc_gradient, pc_jt_gradient)
PC_JSON_API(pc_contour, pc_jt_contour)
PC_JSON_API(pc_fx_paint, jt_fx_paint)
PC_JSON_API(pc_fx_common, pc_jt_fx_common)
PC_JSON_API(pc_shadow, jt_shadow)
PC_JSON_API(pc_glow_technique, jt_glow_technique)
PC_JSON_API(pc_glow_source, jt_glow_source)
PC_JSON_API(pc_glow, jt_glow)
PC_JSON_API(pc_stroke_position, jt_stroke_position)
PC_JSON_API(pc_stroke_fx, jt_stroke_fx)
PC_JSON_API(pc_satin, jt_satin)
PC_JSON_API(pc_bevel_style, jt_bevel_style)
PC_JSON_API(pc_bevel_technique, jt_bevel_technique)
PC_JSON_API(pc_bevel, jt_bevel)
PC_JSON_API(pc_bevel_contour, jt_bevel_contour)
PC_JSON_API(pc_bevel_texture, jt_bevel_texture)
PC_JSON_API(pc_effect, jt_effect)
PC_JSON_API(pc_global_light, jt_global_light)
PC_JSON_API(pc_color_table, jt_color_table)
PC_JSON_API(pc_duotone_ink, jt_duotone_ink)
PC_JSON_API(pc_duotone, jt_duotone)
PC_JSON_API(pc_stack_mode, jt_stack_mode)
