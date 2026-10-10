/* serde JSON forms of comps.rs (artboards), slices.rs, variables.rs, video.rs and analysis.rs.
 * See doc/json.h. */
#include "doc/json_util.h"

#include <string.h>

/* ---- comps.rs ---- */

static const jvariant artboard_bg_v[] = {
    JVU("white"),
    JVU("black"),
    JVU("transparent"),
    JVN("custom", pc_jt_color, pc_artboard_background, custom),
};
static const jenum artboard_bg_e = JENUM_TAGGED(pc_artboard_background, artboard_bg_v, kind);
static const jtype jt_artboard_background = JT_TAGGED(pc_artboard_background, artboard_bg_e);

static const jfield artboard_f[] = {
    JFV("rect", pc_artboard, rect, pc_jt_rect),
    JFV("background", pc_artboard, background, jt_artboard_background),
    JFVD("preset", pc_artboard, preset, pc_jt_str, JD_ZERO),
};
static const jstruct artboard_s = {"struct Artboard", artboard_f, JCOUNT(artboard_f), NULL,
                                   sizeof(pc_artboard)};
static const jtype jt_artboard = JT_STRUCT(pc_artboard, artboard_s);

/* ---- slices.rs ---- */

static const jvariant slice_origin_v[] = {JVU("auto"), JVU("layer"), JVU("user")};
static const jenum slice_origin_e = JENUM_UNITS(pc_slice_origin, slice_origin_v);
static const jtype jt_slice_origin = JT_UNIT_ENUM(pc_slice_origin, slice_origin_e);

static const jvariant slice_kind_v[] = {JVU("image"), JVU("noImage"), JVU("table")};
static const jenum slice_kind_e = JENUM_UNITS(pc_slice_kind, slice_kind_v);
static const jtype jt_slice_kind = JT_UNIT_ENUM(pc_slice_kind, slice_kind_e);

static pc_status slice_default(void *out, pc_error *err)
{
    (void)err;
    pc_slice_default(out);
    return PC_OK;
}

static const jtype u8x4 = JT_ARRAY(uint8_t, pc_jt_u8, 4);
static const jtype i32x4 = JT_ARRAY(int32_t, pc_jt_i32, 4);

#define S pc_slice
static const jfield slice_f[] = {
    JFVD("id", S, id, pc_jt_u32, JD_CONT),
    JFVD("groupId", S, group_id, pc_jt_u32, JD_CONT),
    JFVD("origin", S, origin, jt_slice_origin, JD_CONT),
    JFO("layer", S, layer, has_layer, pc_jt_u64, JD_CONT),
    JFVD("name", S, name, pc_jt_str, JD_CONT),
    JFVD("rect", S, rect, pc_jt_rect, JD_CONT),
    JFVD("kind", S, kind, jt_slice_kind, JD_CONT),
    JFVD("url", S, url, pc_jt_str, JD_CONT),
    JFVD("target", S, target, pc_jt_str, JD_CONT),
    JFVD("message", S, message, pc_jt_str, JD_CONT),
    JFVD("alt", S, alt, pc_jt_str, JD_CONT),
    JFVD("cellTextIsHtml", S, cell_text_is_html, pc_jt_bool, JD_CONT),
    JFVD("cellText", S, cell_text, pc_jt_str, JD_CONT),
    JFVD("horizontalAlign", S, horizontal_align, pc_jt_u32, JD_CONT),
    JFVD("verticalAlign", S, vertical_align, pc_jt_u32, JD_CONT),
    JFO("background", S, background, has_background, u8x4, JD_CONT),
    JFVD("outsets", S, outsets, i32x4, JD_CONT),
};
static const jstruct slice_s = {"struct Slice", slice_f, JCOUNT(slice_f), slice_default, sizeof(S)};
static const jtype jt_slice = JT_STRUCT(S, slice_s);
#undef S

static const jfield slices_f[] = {
    JFVD("groupName", pc_slices, group_name, pc_jt_str, JD_ZERO),
    JFVECD("list", pc_slices, list, len, jt_slice, JD_ZERO),
};
static const jstruct slices_s = {"struct Slices", slices_f, JCOUNT(slices_f), NULL, sizeof(pc_slices)};
static const jtype jt_slices = JT_STRUCT(pc_slices, slices_s);

/* ---- variables.rs ---- */

static const jvariant pixel_method_v[] = {JVU("fit"), JVU("fill"), JVU("asIs"), JVU("conform")};
static const jenum pixel_method_e = JENUM_UNITS(pc_pixel_method, pixel_method_v);
static const jtype jt_pixel_method = JT_UNIT_ENUM(pc_pixel_method, pixel_method_e);

static const jvariant pixel_align_v[] = {
    JVU("topLeft"),     JVU("topCenter"),  JVU("topRight"),     JVU("centerLeft"),  JVU("center"),
    JVU("centerRight"), JVU("bottomLeft"), JVU("bottomCenter"), JVU("bottomRight"),
};
static const jenum pixel_align_e = JENUM_UNITS(pc_pixel_align, pixel_align_v);
static const jtype jt_pixel_align = JT_UNIT_ENUM(pc_pixel_align, pixel_align_e);

static void pixel_align_default(void *slot)
{
    pc_pixel_align a = PC_ALIGN_CENTER;
    memcpy(slot, &a, sizeof a);
}

static const jfield var_pixel_f[] = {
    JFVD("method", pc_var_kind, method, jt_pixel_method, JD_ZERO),
    JFFN("align", pc_var_kind, align, jt_pixel_align, pixel_align_default),
    JFVD("clip", pc_var_kind, clip, pc_jt_bool, JD_ZERO),
};
static const jstruct var_pixel_s = {"struct variant VarKind::PixelReplacement", var_pixel_f,
                                    JCOUNT(var_pixel_f), NULL, sizeof(pc_var_kind)};
static const jvariant var_kind_v[] = {
    JVU("visibility"),
    JVU("textReplacement"),
    JVS("pixelReplacement", var_pixel_s),
};
static const jenum var_kind_e = JENUM_TAGGED(pc_var_kind, var_kind_v, type);
static const jinternal var_kind_in = {"type", "internally tagged enum VarKind", &var_kind_e};
static const jtype jt_var_kind = JT_INTERNAL(pc_var_kind, var_kind_in);

static const jfield variable_def_f[] = {
    JFV("name", pc_variable_def, name, pc_jt_str),
    JFV("layer", pc_variable_def, layer, pc_jt_u64),
    JFV("kind", pc_variable_def, kind, jt_var_kind),
};
static const jstruct variable_def_s = {"struct VariableDef", variable_def_f, JCOUNT(variable_def_f), NULL,
                                       sizeof(pc_variable_def)};
static const jtype jt_variable_def = JT_STRUCT(pc_variable_def, variable_def_s);

/* DataValue { variable, #[serde(flatten)] value: Value } where Value is adjacently tagged
 * (tag = "kind", content = "value", camelCase variants):
 * {"variable":"x","kind":"text","value":"Hello"}. serde reads `variable` while walking the map
 * (so its errors come first), then the tag (deserialised like a unit-only enum from serde's
 * Content, so {"text":null} also works), then the payload. */
static const jvariant data_kind_v[] = {JVU("visibility"), JVU("text"), JVU("pixels")};
static const jenum data_kind_e = JENUM_UNITS(pc_data_value_kind, data_kind_v);
static const jtype jt_data_kind = JT_UNIT_ENUM(pc_data_value_kind, data_kind_e);

static pc_status data_value_read(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err)
{
    pc_data_value *dv = out;
    yyjson_val *var;
    yyjson_val *kind;
    yyjson_val *val;
    pc_data_value_kind k = PC_DATA_VISIBILITY;
    pc_status st;
    (void)t;
    (void)mode;
    if (!yyjson_is_obj(v)) {
        return pc_jfail_type(v, "struct DataValue", err);
    }
    var = pc_json_get(v, "variable");
    kind = pc_json_get(v, "kind");
    val = pc_json_get(v, "value");
    if (var == NULL) {
        return pc_jfail_missing("variable", err);
    }
    st = pc_jt_str.read(&pc_jt_str, var, &dv->variable, 0, err);
    if (st != PC_OK) {
        return st;
    }
    if (kind == NULL) {
        st = pc_jfail_missing("kind", err);
    } else if ((st = jt_data_kind.read(&jt_data_kind, kind, &k, JR_CONTENT, err)) == PC_OK) {
        if (val == NULL) {
            st = pc_jfail_missing("value", err);
        } else if (k == PC_DATA_VISIBILITY) {
            st = pc_jt_bool.read(&pc_jt_bool, val, &dv->visibility, JR_CONTENT, err);
        } else {
            st = pc_jt_str.read(&pc_jt_str, val, &dv->text, JR_CONTENT, err);
        }
    }
    if (st != PC_OK) {
        pc_str_fini(&dv->variable);
        return st;
    }
    dv->kind = k;
    return PC_OK;
}

static yyjson_mut_val *data_value_write(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags)
{
    const pc_data_value *dv = v;
    yyjson_mut_val *obj = yyjson_mut_obj(d);
    yyjson_mut_val *payload;
    (void)t;
    (void)flags;
    if (obj == NULL || (unsigned)dv->kind >= JCOUNT(data_kind_v)) {
        return obj == NULL ? NULL : yyjson_mut_null(d);
    }
    payload = dv->kind == PC_DATA_VISIBILITY ? yyjson_mut_bool(d, dv->visibility) : pc_jw_str(d, &dv->text);
    if (!yyjson_mut_obj_add(obj, yyjson_mut_str(d, "variable"), pc_jw_str(d, &dv->variable)) ||
        !yyjson_mut_obj_add(obj, yyjson_mut_str(d, "kind"), yyjson_mut_str(d, data_kind_v[dv->kind].name)) ||
        !yyjson_mut_obj_add(obj, yyjson_mut_str(d, "value"), payload)) {
        return NULL;
    }
    return obj;
}

static void data_value_fini(const jtype *t, void *v)
{
    pc_data_value *dv = v;
    (void)t;
    pc_str_fini(&dv->variable);
    pc_str_fini(&dv->text);
}

static const jtype jt_data_value = {
    sizeof(pc_data_value), data_value_read, data_value_write, data_value_fini, NULL, 0, NULL};

static const jfield data_set_f[] = {
    JFV("name", pc_data_set, name, pc_jt_str),
    JFVECD("values", pc_data_set, values, nvalues, jt_data_value, JD_ZERO),
};
static const jstruct data_set_s = {"struct DataSet", data_set_f, JCOUNT(data_set_f), NULL,
                                   sizeof(pc_data_set)};
static const jtype jt_data_set = JT_STRUCT(pc_data_set, data_set_s);

static const jfield variables_f[] = {
    JFVECD("defs", pc_variables, defs, ndefs, jt_variable_def, JD_ZERO),
    JFVECD("data_sets", pc_variables, data_sets, ndata_sets, jt_data_set, JD_ZERO),
    JFO("active", pc_variables, active, has_active, pc_jt_usize, JD_ZERO),
};
static const jstruct variables_s = {"struct Variables", variables_f, JCOUNT(variables_f), NULL,
                                    sizeof(pc_variables)};
static const jtype jt_variables = JT_STRUCT(pc_variables, variables_s);

/* ---- video.rs ---- */

static const jfield video_file_f[] = {JFV("path", pc_video_source, path, pc_jt_str)};
static const jstruct video_file_s = {"struct variant VideoSource::File", video_file_f, JCOUNT(video_file_f),
                                     NULL, sizeof(pc_video_source)};
static const jvariant video_source_v[] = {JVU("Blank"), JVS("File", video_file_s)};
static const jenum video_source_e = JENUM_TAGGED(pc_video_source, video_source_v, is_file);
static const jtype jt_video_source = JT_TAGGED(pc_video_source, video_source_e);

static const jfield timeline_f[] = {
    JFV("fps", pc_timeline, fps, pc_jt_f32),
    JFV("duration", pc_timeline, duration, pc_jt_usize),
    JFV("current", pc_timeline, current, pc_jt_usize),
    JFVD("work_start", pc_timeline, work_start, pc_jt_usize, JD_ZERO),
    JFV("work_end", pc_timeline, work_end, pc_jt_usize),
};
static const jstruct timeline_s = {"struct Timeline", timeline_f, JCOUNT(timeline_f), NULL,
                                   sizeof(pc_timeline)};
static const jtype jt_timeline = JT_STRUCT(pc_timeline, timeline_s);

/* ---- analysis.rs ---- */

static pc_status measurement_scale_default(void *out, pc_error *err)
{
    return pc_measurement_scale_default(out, err);
}

static const jfield measurement_scale_f[] = {
    JFVD("pixel_length", pc_measurement_scale, pixel_length, pc_jt_f64, JD_CONT),
    JFVD("logical_length", pc_measurement_scale, logical_length, pc_jt_f64, JD_CONT),
    JFVD("units", pc_measurement_scale, units, pc_jt_str, JD_CONT),
};
static const jstruct measurement_scale_s = {"struct MeasurementScale", measurement_scale_f,
                                            JCOUNT(measurement_scale_f), measurement_scale_default,
                                            sizeof(pc_measurement_scale)};
static const jtype jt_measurement_scale = JT_STRUCT(pc_measurement_scale, measurement_scale_s);

static pc_status count_group_default(void *out, pc_error *err)
{
    return pc_count_group_default(out, err);
}

static const jtype f64x2 = JT_ARRAY(double, pc_jt_f64, 2);

static const jfield count_group_f[] = {
    JFVD("name", pc_count_group, name, pc_jt_str, JD_CONT),
    JFVD("color", pc_count_group, color, pc_jt_color, JD_CONT),
    JFVD("marker_size", pc_count_group, marker_size, pc_jt_u32, JD_CONT),
    JFVD("label_size", pc_count_group, label_size, pc_jt_u32, JD_CONT),
    JFVD("visible", pc_count_group, visible, pc_jt_bool, JD_CONT),
    JFVECD("points", pc_count_group, points, npoints, f64x2, JD_CONT),
};
static const jstruct count_group_s = {"struct CountGroup", count_group_f, JCOUNT(count_group_f),
                                      count_group_default, sizeof(pc_count_group)};
static const jtype jt_count_group = JT_STRUCT(pc_count_group, count_group_s);

static const jfield ruler_f[] = {
    JFV("start", pc_ruler, start, f64x2),
    JFV("end", pc_ruler, end, f64x2),
    JFO("protractor", pc_ruler, protractor, has_protractor, f64x2, JD_ZERO),
};
static const jstruct ruler_s = {"struct Ruler", ruler_f, JCOUNT(ruler_f), NULL, sizeof(pc_ruler)};
static const jtype jt_ruler = JT_STRUCT(pc_ruler, ruler_s);

static pc_status measurement_default(void *out, pc_error *err)
{
    return pc_measurement_default(out, err);
}

static const jfield measurement_f[] = {
    JFVD("scale", pc_measurement, scale, jt_measurement_scale, JD_CONT),
    JFVECD("count_groups", pc_measurement, count_groups, ncount_groups, jt_count_group, JD_CONT),
    JFVD("active_count_group", pc_measurement, active_count_group, pc_jt_usize, JD_CONT),
    JFO("ruler", pc_measurement, ruler, has_ruler, jt_ruler, JD_CONT),
};
static const jstruct measurement_s = {"struct Measurement", measurement_f, JCOUNT(measurement_f),
                                      measurement_default, sizeof(pc_measurement)};
static const jtype jt_measurement = JT_STRUCT(pc_measurement, measurement_s);

static pc_status note_default(void *out, pc_error *err)
{
    (void)err;
    pc_note_default(out);
    return PC_OK;
}

static const jtype f64x4 = JT_ARRAY(double, pc_jt_f64, 4);

static const jfield note_f[] = {
    JFVD("author", pc_note, author, pc_jt_str, JD_CONT),
    JFVD("color", pc_note, color, pc_jt_color, JD_CONT),
    JFVD("text", pc_note, text, pc_jt_str, JD_CONT),
    JFVD("position", pc_note, position, f64x2, JD_CONT),
    JFVD("popup", pc_note, popup, f64x4, JD_CONT),
    JFVD("open", pc_note, open, pc_jt_bool, JD_CONT),
    JFVD("modified", pc_note, modified, pc_jt_str, JD_CONT),
};
static const jstruct note_s = {"struct Note", note_f, JCOUNT(note_f), note_default, sizeof(pc_note)};
static const jtype jt_note = JT_STRUCT(pc_note, note_s);

PC_JSON_API(pc_artboard_background, jt_artboard_background)
PC_JSON_API(pc_artboard, jt_artboard)
PC_JSON_API(pc_slice_origin, jt_slice_origin)
PC_JSON_API(pc_slice_kind, jt_slice_kind)
PC_JSON_API(pc_slice, jt_slice)
PC_JSON_API(pc_slices, jt_slices)
PC_JSON_API(pc_variables, jt_variables)
PC_JSON_API(pc_variable_def, jt_variable_def)
PC_JSON_API(pc_var_kind, jt_var_kind)
PC_JSON_API(pc_pixel_method, jt_pixel_method)
PC_JSON_API(pc_pixel_align, jt_pixel_align)
PC_JSON_API(pc_data_set, jt_data_set)
PC_JSON_API(pc_data_value, jt_data_value)
PC_JSON_API(pc_video_source, jt_video_source)
PC_JSON_API(pc_timeline, jt_timeline)
PC_JSON_API(pc_measurement_scale, jt_measurement_scale)
PC_JSON_API(pc_count_group, jt_count_group)
PC_JSON_API(pc_ruler, jt_ruler)
PC_JSON_API(pc_measurement, jt_measurement)
PC_JSON_API(pc_note, jt_note)
