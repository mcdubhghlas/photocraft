/* serde JSON forms of vector.rs, text.rs and text_styles.rs, and the internally tagged enum
 * support (LiveShape here, VarKind in json_misc.c). See doc/json.h. */
#include "doc/json_util.h"

#include <string.h>

/* ---- internally tagged enums (#[serde(tag = "...")]) ----
 *
 * serde buffers the input into its private Content type, takes the tag out (map entry, or the
 * first element of a sequence) as a variant identifier, then deserialises the variant from what
 * is left (JR_CONTENT). Unit variants accept any remainder. */

pc_status pc_jinternal_tag(const jenum *e, yyjson_val *tag, unsigned mode, unsigned *idx, pc_error *err)
{
    /* Nested in another tagged enum the tag comes out of serde's Content buffer, whose
     * identifier deserializer also takes an unsigned integer as the variant index. */
    bool index = yyjson_is_uint(tag) || (yyjson_is_sint(tag) && yyjson_get_sint(tag) >= 0);
    if ((mode & JR_CONTENT) != 0 && index) {
        uint64_t i = yyjson_is_uint(tag) ? yyjson_get_uint(tag) : (uint64_t)yyjson_get_sint(tag);
        if (i >= e->n) {
            return pc_fail(err, PC_ERR_INVALID,
                           "invalid value: integer `%llu`, expected variant index 0 <= i < %zu",
                           (unsigned long long)i, e->n);
        }
        *idx = (unsigned)i;
        return PC_OK;
    }
    if (!yyjson_is_str(tag)) {
        return pc_jfail_type(tag, "variant identifier", err);
    }
    return pc_jenum_find(e, yyjson_get_str(tag), yyjson_get_len(tag), idx, err);
}

pc_status pc_jt_internal_read(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err)
{
    const jinternal *in = t->aux;
    yyjson_val *tag;
    size_t start = 0;
    unsigned idx = 0;
    pc_status st;
    const jvariant *var;
    if (yyjson_is_obj(v)) {
        tag = pc_json_get(v, in->tag);
        if (tag == NULL) {
            return pc_jfail_missing(in->tag, err);
        }
    } else if (yyjson_is_arr(v)) {
        tag = yyjson_arr_get_first(v);
        if (tag == NULL) {
            return pc_jfail_missing(in->tag, err);
        }
        start = 1;
    } else {
        return pc_jfail_type(v, in->expecting, err);
    }
    st = pc_jinternal_tag(in->e, tag, mode, &idx, err);
    if (st != PC_OK) {
        return st;
    }
    var = &in->e->v[idx];
    if (var->kind == JV_STRUCT) {
        /* An object still holds the tag entry; the struct ignores it like any unknown key. */
        st = pc_jstruct_read(var->st, v, out, JR_CONTENT, start, err);
        if (st != PC_OK) {
            return st;
        }
    } else if (start == 1 && yyjson_arr_size(v) > 1) {
        /* A unit variant ignores a map remainder but consumes no sequence elements. */
        return pc_fail(err, PC_ERR_INVALID, "invalid length %zu, expected 0 elements in sequence",
                       yyjson_arr_size(v) - 1);
    }
    pc_jenum_set(in->e, out, idx);
    return PC_OK;
}

yyjson_mut_val *pc_jt_internal_write(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags)
{
    const jinternal *in = t->aux;
    unsigned idx = pc_jenum_get(in->e, v);
    const jvariant *var;
    yyjson_mut_val *obj;
    if (idx >= in->e->n) {
        return yyjson_mut_null(d);
    }
    var = &in->e->v[idx];
    obj = yyjson_mut_obj(d);
    if (obj == NULL || !yyjson_mut_obj_add(obj, yyjson_mut_str(d, in->tag), yyjson_mut_str(d, var->name))) {
        return NULL;
    }
    if (var->kind == JV_STRUCT && !pc_jstruct_write_fields(var->st, d, obj, v, flags)) {
        return NULL;
    }
    return obj;
}

void pc_jt_internal_fini(const jtype *t, void *v)
{
    const jinternal *in = t->aux;
    unsigned idx = pc_jenum_get(in->e, v);
    if (idx < in->e->n && in->e->v[idx].kind == JV_STRUCT) {
        pc_jstruct_fini(in->e->v[idx].st, v);
    }
}

/* ---- vector.rs ---- */

static const jfield knot_f[] = {
    JFV("anchor", pc_knot, anchor, pc_jt_point),
    JFV("in_ctrl", pc_knot, in_ctrl, pc_jt_point),
    JFV("out_ctrl", pc_knot, out_ctrl, pc_jt_point),
    JFVD("smooth", pc_knot, smooth, pc_jt_bool, JD_ZERO),
};
static const jstruct knot_s = {"struct Knot", knot_f, JCOUNT(knot_f), NULL, sizeof(pc_knot)};
static const jtype jt_knot = JT_STRUCT(pc_knot, knot_s);

static const jvariant path_op_v[] = {JVU("Combine"), JVU("Subtract"), JVU("Intersect"), JVU("Exclude"),
                                     JVU("Join")};
static const jenum path_op_e = JENUM_UNITS(pc_path_op, path_op_v);
static const jtype jt_path_op = JT_UNIT_ENUM(pc_path_op, path_op_e);

static const jvariant fill_rule_v[] = {JVU("NonZero"), JVU("EvenOdd")};
static const jenum fill_rule_e = JENUM_UNITS(pc_fill_rule, fill_rule_v);
static const jtype jt_fill_rule = JT_UNIT_ENUM(pc_fill_rule, fill_rule_e);

static const jfield subpath_f[] = {
    JFV("closed", pc_subpath, closed, pc_jt_bool),
    JFVEC("knots", pc_subpath, knots, nknots, jt_knot),
    JFVD("op", pc_subpath, op, jt_path_op, JD_ZERO),
};
static const jstruct subpath_s = {"struct Subpath", subpath_f, JCOUNT(subpath_f), NULL, sizeof(pc_subpath)};
static const jtype jt_subpath = JT_STRUCT(pc_subpath, subpath_s);

static const jfield path_f[] = {
    JFVEC("subpaths", pc_path, subpaths, nsubpaths, jt_subpath),
    JFVD("fill_rule", pc_path, fill_rule, jt_fill_rule, JD_ZERO),
    JFVD("inverted", pc_path, inverted, pc_jt_bool, JD_ZERO),
};
static const jstruct path_s = {"struct Path", path_f, JCOUNT(path_f), NULL, sizeof(pc_path)};
const jtype pc_jt_path = JT_STRUCT(pc_path, path_s);

static const jfield clipping_path_f[] = {
    JFV("name", pc_clipping_path, name, pc_jt_str),
    JFV("flatness", pc_clipping_path, flatness, pc_jt_f32),
};
static const jstruct clipping_path_s = {"struct ClippingPath", clipping_path_f, JCOUNT(clipping_path_f), NULL,
                                        sizeof(pc_clipping_path)};
static const jtype jt_clipping_path = JT_STRUCT(pc_clipping_path, clipping_path_s);

static const jfield vector_mask_f[] = {
    JFV("path", pc_vector_mask, path, pc_jt_path),      JFV("enabled", pc_vector_mask, enabled, pc_jt_bool),
    JFV("linked", pc_vector_mask, linked, pc_jt_bool),  JFV("density", pc_vector_mask, density, pc_jt_f32),
    JFV("feather", pc_vector_mask, feather, pc_jt_f32),
};
static const jstruct vector_mask_s = {"struct VectorMask", vector_mask_f, JCOUNT(vector_mask_f), NULL,
                                      sizeof(pc_vector_mask)};
static const jtype jt_vector_mask = JT_STRUCT(pc_vector_mask, vector_mask_s);

static const jvariant stroke_align_v[] = {JVU("Inside"), JVU("Center"), JVU("Outside")};
static const jenum stroke_align_e = JENUM_UNITS(pc_stroke_align, stroke_align_v);
static const jtype jt_stroke_align = JT_UNIT_ENUM(pc_stroke_align, stroke_align_e);

static const jvariant line_cap_v[] = {JVU("Butt"), JVU("Round"), JVU("Square")};
static const jenum line_cap_e = JENUM_UNITS(pc_line_cap, line_cap_v);
static const jtype jt_line_cap = JT_UNIT_ENUM(pc_line_cap, line_cap_e);

static const jvariant line_join_v[] = {JVU("Miter"), JVU("Round"), JVU("Bevel")};
static const jenum line_join_e = JENUM_UNITS(pc_line_join, line_join_v);
static const jtype jt_line_join = JT_UNIT_ENUM(pc_line_join, line_join_e);

static pc_status shape_stroke_default(void *out, pc_error *err)
{
    (void)err;
    pc_shape_stroke_default(out);
    return PC_OK;
}

#define SS pc_shape_stroke
static const jfield shape_stroke_f[] = {
    JFVD("width", SS, width, pc_jt_f32, JD_CONT),
    JFVD("paint", SS, paint, pc_jt_fill, JD_CONT),
    JFVD("opacity", SS, opacity, pc_jt_f32, JD_CONT),
    JFVD("align", SS, align, jt_stroke_align, JD_CONT),
    JFVD("cap", SS, cap, jt_line_cap, JD_CONT),
    JFVD("join", SS, join, jt_line_join, JD_CONT),
    JFVD("miter_limit", SS, miter_limit, pc_jt_f32, JD_CONT),
    JFVECD("dashes", SS, dashes, ndashes, pc_jt_f32, JD_CONT),
    JFVD("dash_offset", SS, dash_offset, pc_jt_f32, JD_CONT),
};
static const jstruct shape_stroke_s = {"struct ShapeStroke", shape_stroke_f, JCOUNT(shape_stroke_f),
                                       shape_stroke_default, sizeof(SS)};
static const jtype jt_shape_stroke = JT_STRUCT(SS, shape_stroke_s);
#undef SS

static const jtype f64x2 = JT_ARRAY(double, pc_jt_f64, 2);
static const jtype f64x4 = JT_ARRAY(double, pc_jt_f64, 4);

#define LS pc_live_shape
static const jfield live_rect_f[] = {
    JFV("rect", LS, rect, f64x4),
    JFV("radii", LS, radii, f64x4),
};
static const jfield live_ellipse_f[] = {JFV("rect", LS, rect, f64x4)};
static const jfield live_polygon_f[] = {
    JFV("rect", LS, rect, f64x4),
    JFV("sides", LS, sides, pc_jt_u32),
    JFV("star_ratio", LS, star_ratio, pc_jt_f64),
};
static const jfield live_line_f[] = {
    JFV("from", LS, from, f64x2),
    JFV("to", LS, to, f64x2),
    JFV("weight", LS, weight, pc_jt_f64),
};
static const jstruct live_rect_s = {"struct variant LiveShape::Rect", live_rect_f, JCOUNT(live_rect_f), NULL,
                                    sizeof(LS)};
static const jstruct live_ellipse_s = {"struct variant LiveShape::Ellipse", live_ellipse_f,
                                       JCOUNT(live_ellipse_f), NULL, sizeof(LS)};
static const jstruct live_polygon_s = {"struct variant LiveShape::Polygon", live_polygon_f,
                                       JCOUNT(live_polygon_f), NULL, sizeof(LS)};
static const jstruct live_line_s = {"struct variant LiveShape::Line", live_line_f, JCOUNT(live_line_f), NULL,
                                    sizeof(LS)};
static const jvariant live_v[] = {
    JVS("rect", live_rect_s),
    JVS("ellipse", live_ellipse_s),
    JVS("polygon", live_polygon_s),
    JVS("line", live_line_s),
};
static const jenum live_e = JENUM_TAGGED(LS, live_v, kind);
static const jinternal live_in = {"kind", "internally tagged enum LiveShape", &live_e};
static const jtype jt_live_shape = JT_INTERNAL(LS, live_in);
#undef LS

/* ---- text.rs ---- */

static const jvariant kerning_v[] = {JVU("Metrics"), JVU("Optical"), JVU("Off")};
static const jenum kerning_e = JENUM_UNITS(pc_kerning, kerning_v);
static const jtype jt_kerning = JT_UNIT_ENUM(pc_kerning, kerning_e);

static const jvariant caps_v[] = {JVU("Normal"), JVU("SmallCaps"), JVU("AllCaps")};
static const jenum caps_e = JENUM_UNITS(pc_caps, caps_v);
static const jtype jt_caps = JT_UNIT_ENUM(pc_caps, caps_e);

static const jfield font_feature_f[] = {
    JFV("tag", pc_font_feature, tag, pc_jt_str),
    JFV("value", pc_font_feature, value, pc_jt_u16),
};
static const jstruct font_feature_s = {"struct FontFeature", font_feature_f, JCOUNT(font_feature_f), NULL,
                                       sizeof(pc_font_feature)};
static const jtype jt_font_feature = JT_STRUCT(pc_font_feature, font_feature_s);

static const jfield font_variation_f[] = {
    JFV("axis", pc_font_variation, axis, pc_jt_str),
    JFV("value", pc_font_variation, value, pc_jt_f32),
};
static const jstruct font_variation_s = {"struct FontVariation", font_variation_f, JCOUNT(font_variation_f),
                                         NULL, sizeof(pc_font_variation)};
static const jtype jt_font_variation = JT_STRUCT(pc_font_variation, font_variation_s);

static pc_status char_style_default(void *out, pc_error *err)
{
    (void)err;
    pc_char_style_default(out);
    return PC_OK;
}

#define CS pc_char_style
static const jfield char_style_f[] = {
    JFVD("font_family", CS, font_family, pc_jt_str, JD_CONT),
    JFVD("font_style", CS, font_style, pc_jt_str, JD_CONT),
    JFO("postscript_name", CS, postscript_name, has_postscript_name, pc_jt_str, JD_CONT),
    JFVD("weight", CS, weight, pc_jt_u16, JD_CONT),
    JFVD("italic", CS, italic, pc_jt_bool, JD_CONT),
    JFVD("size_pt", CS, size_pt, pc_jt_f32, JD_CONT),
    JFVD("color", CS, color, pc_jt_color, JD_CONT),
    JFVD("tracking", CS, tracking, pc_jt_f32, JD_CONT),
    JFO("leading_pt", CS, leading_pt, has_leading, pc_jt_f32, JD_CONT),
    JFVD("baseline_shift_pt", CS, baseline_shift_pt, pc_jt_f32, JD_CONT),
    JFVD("horizontal_scale", CS, horizontal_scale, pc_jt_f32, JD_CONT),
    JFVD("vertical_scale", CS, vertical_scale, pc_jt_f32, JD_CONT),
    JFVD("underline", CS, underline, pc_jt_bool, JD_CONT),
    JFVD("strikethrough", CS, strikethrough, pc_jt_bool, JD_CONT),
    JFVD("faux_bold", CS, faux_bold, pc_jt_bool, JD_CONT),
    JFVD("faux_italic", CS, faux_italic, pc_jt_bool, JD_CONT),
    JFVD("kerning", CS, kerning, jt_kerning, JD_CONT),
    JFVD("kern", CS, kern, pc_jt_f32, JD_CONT),
    JFVD("caps", CS, caps, jt_caps, JD_CONT),
    JFVD("ligatures", CS, ligatures, pc_jt_bool, JD_CONT),
    JFVD("discretionary_ligatures", CS, discretionary_ligatures, pc_jt_bool, JD_CONT),
    JFVECD("features", CS, features, nfeatures, jt_font_feature, JD_CONT),
    JFVECD("variations", CS, variations, nvariations, jt_font_variation, JD_CONT),
    JFO("language", CS, language, has_language, pc_jt_str, JD_CONT),
    JFOSKIP("style_sheet", CS, style_sheet, has_style_sheet, pc_jt_u32, JD_CONT),
};
static const jstruct char_style_s = {"struct CharStyle", char_style_f, JCOUNT(char_style_f),
                                     char_style_default, sizeof(CS)};
const jtype pc_jt_char_style = JT_STRUCT(CS, char_style_s);
#undef CS

static const jfield text_run_f[] = {
    JFV("len", pc_text_run, len, pc_jt_usize),
    JFV("style", pc_text_run, style, pc_jt_char_style),
};
static const jstruct text_run_s = {"struct TextRun", text_run_f, JCOUNT(text_run_f), NULL,
                                   sizeof(pc_text_run)};
static const jtype jt_text_run = JT_STRUCT(pc_text_run, text_run_s);

static const jvariant text_align_v[] = {
    JVU("Left"),          JVU("Center"),       JVU("Right"),      JVU("JustifyLeft"),
    JVU("JustifyCenter"), JVU("JustifyRight"), JVU("JustifyAll"),
};
static const jenum text_align_e = JENUM_UNITS(pc_text_align, text_align_v);
static const jtype jt_text_align = JT_UNIT_ENUM(pc_text_align, text_align_e);

static const jvariant text_direction_v[] = {JVU("Auto"), JVU("Ltr"), JVU("Rtl")};
static const jenum text_direction_e = JENUM_UNITS(pc_text_direction, text_direction_v);
static const jtype jt_text_direction = JT_UNIT_ENUM(pc_text_direction, text_direction_e);

static pc_status paragraph_style_default(void *out, pc_error *err)
{
    pc_paragraph_style p = pc_paragraph_style_default();
    (void)err;
    memcpy(out, &p, sizeof p);
    return PC_OK;
}

#define PS pc_paragraph_style
static const jfield paragraph_style_f[] = {
    JFVD("align", PS, align, jt_text_align, JD_CONT),
    JFVD("first_line_indent_pt", PS, first_line_indent_pt, pc_jt_f32, JD_CONT),
    JFVD("start_indent_pt", PS, start_indent_pt, pc_jt_f32, JD_CONT),
    JFVD("end_indent_pt", PS, end_indent_pt, pc_jt_f32, JD_CONT),
    JFVD("space_before_pt", PS, space_before_pt, pc_jt_f32, JD_CONT),
    JFVD("space_after_pt", PS, space_after_pt, pc_jt_f32, JD_CONT),
    JFVD("auto_leading", PS, auto_leading, pc_jt_f32, JD_CONT),
    JFVD("direction", PS, direction, jt_text_direction, JD_CONT),
    JFVD("hyphenate", PS, hyphenate, pc_jt_bool, JD_CONT),
    JFOSKIP("style_sheet", PS, style_sheet, has_style_sheet, pc_jt_u32, JD_CONT),
};
static const jstruct paragraph_style_s = {"struct ParagraphStyle", paragraph_style_f,
                                          JCOUNT(paragraph_style_f), paragraph_style_default, sizeof(PS)};
const jtype pc_jt_paragraph_style = JT_STRUCT(PS, paragraph_style_s);
#undef PS

static const jfield paragraph_run_f[] = {
    JFV("len", pc_paragraph_run, len, pc_jt_usize),
    JFV("style", pc_paragraph_run, style, pc_jt_paragraph_style),
};
static const jstruct paragraph_run_s = {"struct ParagraphRun", paragraph_run_f, JCOUNT(paragraph_run_f), NULL,
                                        sizeof(pc_paragraph_run)};
static const jtype jt_paragraph_run = JT_STRUCT(pc_paragraph_run, paragraph_run_s);

/* TextShape: Point, Box { x, y, width, height }, or Path { path, start, flip } (discriminant:
 * kind, whose values are the serde variant order). */
static const jfield text_box_f[] = {
    JFV("x", pc_text_shape, x, pc_jt_f32),
    JFV("y", pc_text_shape, y, pc_jt_f32),
    JFV("width", pc_text_shape, width, pc_jt_f32),
    JFV("height", pc_text_shape, height, pc_jt_f32),
};
static const jstruct text_box_s = {"struct variant TextShape::Box", text_box_f, JCOUNT(text_box_f), NULL,
                                   sizeof(pc_text_shape)};
static const jfield text_path_f[] = {
    JFV("path", pc_text_shape, path, jt_subpath),
    JFV("start", pc_text_shape, start, pc_jt_f32),
    JFV("flip", pc_text_shape, flip, pc_jt_bool),
};
static const jstruct text_path_s = {"struct variant TextShape::Path", text_path_f, JCOUNT(text_path_f), NULL,
                                    sizeof(pc_text_shape)};
static const jvariant text_shape_v[] = {JVU("Point"), JVS("Box", text_box_s), JVS("Path", text_path_s)};
static const jenum text_shape_e = JENUM_TAGGED(pc_text_shape, text_shape_v, kind);
static const jtype jt_text_shape = JT_TAGGED(pc_text_shape, text_shape_e);

static const jvariant orientation_v[] = {JVU("Horizontal"), JVU("Vertical")};
static const jenum orientation_e = JENUM_UNITS(pc_orientation, orientation_v);
static const jtype jt_orientation = JT_UNIT_ENUM(pc_orientation, orientation_e);

static const jvariant antialias_v[] = {
    JVU("None"), JVU("Sharp"), JVU("Crisp"), JVU("Strong"), JVU("Smooth"), JVU("Windows"), JVU("WindowsLcd"),
};
static const jenum antialias_e = JENUM_UNITS(pc_antialias, antialias_v);
static const jtype jt_antialias = JT_UNIT_ENUM(pc_antialias, antialias_e);

static const jfield text_warp_f[] = {
    JFV("style", pc_text_warp, style, pc_jt_str),
    JFV("value", pc_text_warp, value, pc_jt_f32),
    JFV("horizontal_distortion", pc_text_warp, horizontal_distortion, pc_jt_f32),
    JFV("vertical_distortion", pc_text_warp, vertical_distortion, pc_jt_f32),
    JFV("horizontal", pc_text_warp, horizontal, pc_jt_bool),
};
static const jstruct text_warp_s = {"struct TextWarp", text_warp_f, JCOUNT(text_warp_f), NULL,
                                    sizeof(pc_text_warp)};
static const jtype jt_text_warp = JT_STRUCT(pc_text_warp, text_warp_s);

/* ---- text_styles.rs ---- */

static const jfield character_style_def_f[] = {
    JFVD("id", pc_character_style_def, id, pc_jt_u32, JD_ZERO),
    JFVD("name", pc_character_style_def, name, pc_jt_str, JD_ZERO),
    JFVD("attrs", pc_character_style_def, attrs, pc_jt_map, JD_ZERO),
};
static const jstruct character_style_def_s = {"struct CharacterStyleDef", character_style_def_f,
                                              JCOUNT(character_style_def_f), NULL,
                                              sizeof(pc_character_style_def)};
static const jtype jt_character_style_def = JT_STRUCT(pc_character_style_def, character_style_def_s);

static const jfield paragraph_style_def_f[] = {
    JFVD("id", pc_paragraph_style_def, id, pc_jt_u32, JD_ZERO),
    JFVD("name", pc_paragraph_style_def, name, pc_jt_str, JD_ZERO),
    JFVD("para_attrs", pc_paragraph_style_def, para_attrs, pc_jt_map, JD_ZERO),
    JFVD("char_attrs", pc_paragraph_style_def, char_attrs, pc_jt_map, JD_ZERO),
};
static const jstruct paragraph_style_def_s = {"struct ParagraphStyleDef", paragraph_style_def_f,
                                              JCOUNT(paragraph_style_def_f), NULL,
                                              sizeof(pc_paragraph_style_def)};
static const jtype jt_paragraph_style_def = JT_STRUCT(pc_paragraph_style_def, paragraph_style_def_s);

static pc_status text_styles_default(void *out, pc_error *err)
{
    return pc_text_styles_default(out, err);
}

static const jfield text_styles_f[] = {
    JFVECD("character", pc_text_styles, character, ncharacter, jt_character_style_def, JD_ZERO),
    JFVECD("paragraph", pc_text_styles, paragraph, nparagraph, jt_paragraph_style_def, JD_ZERO),
    JFVD("basic", pc_text_styles, basic, jt_paragraph_style_def, JD_CONT),
    JFVD("psd_raw", pc_text_styles, psd_raw, pc_jt_value_opt, JD_ZERO),
};
static const jstruct text_styles_s = {"struct TextStyles", text_styles_f, JCOUNT(text_styles_f),
                                      text_styles_default, sizeof(pc_text_styles)};
static const jtype jt_text_styles = JT_STRUCT(pc_text_styles, text_styles_s);

PC_JSON_API(pc_knot, jt_knot)
PC_JSON_API(pc_path_op, jt_path_op)
PC_JSON_API(pc_fill_rule, jt_fill_rule)
PC_JSON_API(pc_subpath, jt_subpath)
PC_JSON_API(pc_path, pc_jt_path)
PC_JSON_API(pc_clipping_path, jt_clipping_path)
PC_JSON_API(pc_vector_mask, jt_vector_mask)
PC_JSON_API(pc_stroke_align, jt_stroke_align)
PC_JSON_API(pc_line_cap, jt_line_cap)
PC_JSON_API(pc_line_join, jt_line_join)
PC_JSON_API(pc_shape_stroke, jt_shape_stroke)
PC_JSON_API(pc_live_shape, jt_live_shape)
PC_JSON_API(pc_kerning, jt_kerning)
PC_JSON_API(pc_caps, jt_caps)
PC_JSON_API(pc_font_feature, jt_font_feature)
PC_JSON_API(pc_font_variation, jt_font_variation)
PC_JSON_API(pc_char_style, pc_jt_char_style)
PC_JSON_API(pc_text_run, jt_text_run)
PC_JSON_API(pc_text_align, jt_text_align)
PC_JSON_API(pc_text_direction, jt_text_direction)
PC_JSON_API(pc_paragraph_style, pc_jt_paragraph_style)
PC_JSON_API(pc_paragraph_run, jt_paragraph_run)
PC_JSON_API(pc_text_shape, jt_text_shape)
PC_JSON_API(pc_orientation, jt_orientation)
PC_JSON_API(pc_antialias, jt_antialias)
PC_JSON_API(pc_text_warp, jt_text_warp)
PC_JSON_API(pc_character_style_def, jt_character_style_def)
PC_JSON_API(pc_paragraph_style_def, jt_paragraph_style_def)
PC_JSON_API(pc_text_styles, jt_text_styles)
