/* serde JSON forms of the geom and color types the document embeds, and of the lib.rs types
 * (Locks, LabelColor, Fill, SmartFilter, Guides, ColorIndicates). See doc/json.h. */
#include "doc/json_util.h"

/* ---- geom ---- */

static const jfield size_f[] = {
    JFV("width", pc_size, width, pc_jt_u32),
    JFV("height", pc_size, height, pc_jt_u32),
};
static const jstruct size_s = {"struct Size", size_f, JCOUNT(size_f), NULL, sizeof(pc_size)};
const jtype pc_jt_size = JT_STRUCT(pc_size, size_s);

static const jfield rect_f[] = {
    JFV("x0", pc_rect, x0, pc_jt_i32),
    JFV("y0", pc_rect, y0, pc_jt_i32),
    JFV("x1", pc_rect, x1, pc_jt_i32),
    JFV("y1", pc_rect, y1, pc_jt_i32),
};
static const jstruct rect_s = {"struct Rect", rect_f, JCOUNT(rect_f), NULL, sizeof(pc_rect)};
const jtype pc_jt_rect = JT_STRUCT(pc_rect, rect_s);

static const jfield point_f[] = {
    JFV("x", pc_point, x, pc_jt_f64),
    JFV("y", pc_point, y, pc_jt_f64),
};
static const jstruct point_s = {"struct Point", point_f, JCOUNT(point_f), NULL, sizeof(pc_point)};
const jtype pc_jt_point = JT_STRUCT(pc_point, point_s);

static const jtype f64x6 = JT_ARRAY(double, pc_jt_f64, 6);
static const jfield affine_f[] = {JFV("m", pc_affine, m, f64x6)};
static const jstruct affine_s = {"struct Affine", affine_f, JCOUNT(affine_f), NULL, sizeof(pc_affine)};
const jtype pc_jt_affine = JT_STRUCT(pc_affine, affine_s);

/* ---- color ---- */

static const jvariant sample_v[] = {JVU("U8"), JVU("U16"), JVU("F32")};
static const jenum sample_e = JENUM_UNITS(pc_sample_type, sample_v);
const jtype pc_jt_sample_type = JT_UNIT_ENUM(pc_sample_type, sample_e);

static const jvariant mode_v[] = {
    JVU("Bitmap"), JVU("Grayscale"), JVU("Indexed"),      JVU("Rgb"),
    JVU("Cmyk"),   JVU("Lab"),       JVU("Multichannel"), JVU("Duotone"),
};
static const jenum mode_e = JENUM_UNITS(pc_color_mode, mode_v);
const jtype pc_jt_color_mode = JT_UNIT_ENUM(pc_color_mode, mode_e);

static const jfield pixel_format_f[] = {
    JFV("mode", pc_pixel_format, mode, pc_jt_color_mode),
    JFV("sample", pc_pixel_format, sample, pc_jt_sample_type),
    JFV("alpha", pc_pixel_format, alpha, pc_jt_bool),
};
static const jstruct pixel_format_s = {"struct PixelFormat", pixel_format_f, JCOUNT(pixel_format_f), NULL,
                                       sizeof(pc_pixel_format)};
const jtype pc_jt_pixel_format = JT_STRUCT(pc_pixel_format, pixel_format_s);

static const jtype f32x4 = JT_ARRAY(float, pc_jt_f32, 4);
static const jfield color_f[] = {
    JFV("mode", pc_color, mode, pc_jt_color_mode),
    JFV("c", pc_color, c, f32x4),
    JFV("alpha", pc_color, alpha, pc_jt_f32),
};
static const jstruct color_s = {"struct Color", color_f, JCOUNT(color_f), NULL, sizeof(pc_color)};
const jtype pc_jt_color = JT_STRUCT(pc_color, color_s);

static const jvariant blend_v[] = {
    JVU("PassThrough"), JVU("Normal"),      JVU("Dissolve"),     JVU("Darken"),   JVU("Multiply"),
    JVU("ColorBurn"),   JVU("LinearBurn"),  JVU("DarkerColor"),  JVU("Lighten"),  JVU("Screen"),
    JVU("ColorDodge"),  JVU("LinearDodge"), JVU("LighterColor"), JVU("Overlay"),  JVU("SoftLight"),
    JVU("HardLight"),   JVU("VividLight"),  JVU("LinearLight"),  JVU("PinLight"), JVU("HardMix"),
    JVU("Difference"),  JVU("Exclusion"),   JVU("Subtract"),     JVU("Divide"),   JVU("Hue"),
    JVU("Saturation"),  JVU("Color"),       JVU("Luminosity"),
};
static const jenum blend_e = JENUM_UNITS(pc_blend_mode, blend_v);
const jtype pc_jt_blend_mode = JT_UNIT_ENUM(pc_blend_mode, blend_e);

/* ---- lib.rs ---- */

static const jfield locks_f[] = {
    JFV("transparency", pc_locks, transparency, pc_jt_bool),
    JFV("pixels", pc_locks, pixels, pc_jt_bool),
    JFV("position", pc_locks, position, pc_jt_bool),
    JFV("artboard", pc_locks, artboard, pc_jt_bool),
    JFV("all", pc_locks, all, pc_jt_bool),
};
static const jstruct locks_s = {"struct Locks", locks_f, JCOUNT(locks_f), NULL, sizeof(pc_locks)};
static const jtype jt_locks = JT_STRUCT(pc_locks, locks_s);

static const jvariant label_v[] = {
    JVU("None"),   JVU("Red"),  JVU("Orange"),  JVU("Yellow"), JVU("Green"),   JVU("Blue"),
    JVU("Violet"), JVU("Gray"), JVU("Seafoam"), JVU("Indigo"), JVU("Magenta"), JVU("Fuchsia"),
};
static const jenum label_e = JENUM_UNITS(pc_label_color, label_v);
static const jtype jt_label_color = JT_UNIT_ENUM(pc_label_color, label_e);

static const jtuple_el color_stop_el[] = {
    {&pc_jt_f32, offsetof(pc_color_stop, pos)},
    {&pc_jt_color, offsetof(pc_color_stop, color)},
};
static const jtuple color_stop_tu = {color_stop_el, 2};
const jtype pc_jt_color_stop = JT_TUPLE(pc_color_stop, color_stop_tu);

static const jtuple_el opacity_stop_el[] = {
    {&pc_jt_f32, offsetof(pc_opacity_stop, pos)},
    {&pc_jt_f32, offsetof(pc_opacity_stop, opacity)},
};
static const jtuple opacity_stop_tu = {opacity_stop_el, 2};
static const jtype jt_opacity_stop = JT_TUPLE(pc_opacity_stop, opacity_stop_tu);

static const jfield fill_gradient_f[] = {
    JFVEC("stops", pc_fill, u.gradient.stops, u.gradient.nstops, pc_jt_color_stop),
    JFV("angle", pc_fill, u.gradient.angle, pc_jt_f32),
    JFV("scale", pc_fill, u.gradient.scale, pc_jt_f32),
    JFV("style", pc_fill, u.gradient.style, pc_jt_gradient_style),
    JFV("reverse", pc_fill, u.gradient.reverse, pc_jt_bool),
    JFVECD("opacity_stops", pc_fill, u.gradient.opacity_stops, u.gradient.nopacity_stops, jt_opacity_stop,
           JD_ZERO),
    JFVECD("midpoints", pc_fill, u.gradient.midpoints, u.gradient.nmidpoints, pc_jt_f32, JD_ZERO),
    JFVD("offset", pc_fill, u.gradient.offset, pc_jt_f32x2t, JD_ZERO),
    JFVD("dither", pc_fill, u.gradient.dither, pc_jt_bool, JD_ZERO),
    JFVD("align", pc_fill, u.gradient.align, pc_jt_bool, JD_TRUE),
};
static const jstruct fill_gradient_s = {"struct variant Fill::Gradient", fill_gradient_f,
                                        JCOUNT(fill_gradient_f), NULL, sizeof(pc_fill)};

static const jfield fill_pattern_f[] = {
    JFV("name", pc_fill, u.pattern.name, pc_jt_str),
    JFV("scale", pc_fill, u.pattern.scale, pc_jt_f32),
    JFVD("id", pc_fill, u.pattern.id, pc_jt_str, JD_ZERO),
    JFVD("angle", pc_fill, u.pattern.angle, pc_jt_f32, JD_ZERO),
    JFVD("link", pc_fill, u.pattern.link, pc_jt_bool, JD_TRUE),
    JFVD("phase", pc_fill, u.pattern.phase, pc_jt_f32x2t, JD_ZERO),
};
static const jstruct fill_pattern_s = {"struct variant Fill::Pattern", fill_pattern_f, JCOUNT(fill_pattern_f),
                                       NULL, sizeof(pc_fill)};

static const jvariant fill_v[] = {
    JVN("Solid", pc_jt_color, pc_fill, u.solid),
    JVS("Gradient", fill_gradient_s),
    JVS("Pattern", fill_pattern_s),
};
static const jenum fill_e = JENUM_TAGGED(pc_fill, fill_v, kind);
const jtype pc_jt_fill = JT_TAGGED(pc_fill, fill_e);

static const jfield smart_filter_f[] = {
    JFV("command", pc_smart_filter, command, pc_jt_str),
    JFV("params", pc_smart_filter, params, pc_jt_value),
    JFV("blend", pc_smart_filter, blend, pc_jt_blend_mode),
    JFV("opacity", pc_smart_filter, opacity, pc_jt_f32),
    JFV("visible", pc_smart_filter, visible, pc_jt_bool),
};
static const jstruct smart_filter_s = {"struct SmartFilter", smart_filter_f, JCOUNT(smart_filter_f), NULL,
                                       sizeof(pc_smart_filter)};
static const jtype jt_smart_filter = JT_STRUCT(pc_smart_filter, smart_filter_s);

static const jfield guides_f[] = {
    JFVEC("horizontal", pc_guides, horizontal, nhorizontal, pc_jt_f32),
    JFVEC("vertical", pc_guides, vertical, nvertical, pc_jt_f32),
};
static const jstruct guides_s = {"struct Guides", guides_f, JCOUNT(guides_f), NULL, sizeof(pc_guides)};
static const jtype jt_guides = JT_STRUCT(pc_guides, guides_s);

static const jvariant indicates_v[] = {JVU("maskedAreas"), JVU("selectedAreas")};
static const jenum indicates_e = JENUM_UNITS(pc_color_indicates, indicates_v);
static const jtype jt_color_indicates = JT_UNIT_ENUM(pc_color_indicates, indicates_e);

PC_JSON_API(pc_size, pc_jt_size)
PC_JSON_API(pc_rect, pc_jt_rect)
PC_JSON_API(pc_point, pc_jt_point)
PC_JSON_API(pc_affine, pc_jt_affine)
PC_JSON_API(pc_sample_type, pc_jt_sample_type)
PC_JSON_API(pc_color_mode, pc_jt_color_mode)
PC_JSON_API(pc_pixel_format, pc_jt_pixel_format)
PC_JSON_API(pc_color, pc_jt_color)
PC_JSON_API(pc_blend_mode, pc_jt_blend_mode)
PC_JSON_API(pc_locks, jt_locks)
PC_JSON_API(pc_label_color, jt_label_color)
PC_JSON_API(pc_fill, pc_jt_fill)
PC_JSON_API(pc_smart_filter, jt_smart_filter)
PC_JSON_API(pc_guides, jt_guides)
PC_JSON_API(pc_color_indicates, jt_color_indicates)
