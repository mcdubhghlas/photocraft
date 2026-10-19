#include "ui/control_options.h"
#include "engine/vector_cmds.h"
#include <string.h>
static bool allowed(const char *key, const char *const *names, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(key, names[i]) == 0) {
            return true;
        }
    }
    return false;
}
static pc_status fields(yyjson_val *object, const char *name, const char *const *names, size_t count,
                        pc_error *err)
{
    if (!yyjson_is_obj(object)) {
        return pc_fail(err, PC_ERR_INVALID, "%s must be an object", name);
    }
    size_t i, max;
    yyjson_val *key, *value;
    yyjson_obj_foreach(object, i, max, key, value)
    {
        (void)value;
        if (!allowed(yyjson_get_str(key), names, count)) {
            return pc_fail(err, PC_ERR_INVALID, "unknown %s field `%s`", name, yyjson_get_str(key));
        }
    }
    return PC_OK;
}
pc_status pc_ui_control_options_prepare(const pc_ui_tool_options *current, yyjson_val *p,
                                        pc_ui_tool_options *out, pc_error *err)
{
    if (!current || !out) {
        return pc_fail(err, PC_ERR_INVALID, "missing tool options");
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return pc_fail(err, PC_ERR_NOMEM, "tool option patch");
    }
    yyjson_val *explicit_options = pc_json_get(p, "toolOptions");
    if (explicit_options && !yyjson_is_obj(explicit_options)) {
        yyjson_mut_doc_free(doc);
        return pc_fail(err, PC_ERR_INVALID, "toolOptions must be an object");
    }
    static const char *const option_names[] = {
#define PC_UI_OPTION(kind, name, value) #name,
#include "ui/tool_options_fields.inc"
#undef PC_UI_OPTION
    };
    if (explicit_options) {
        pc_status status = fields(explicit_options, "toolOptions", option_names,
                                  sizeof option_names / sizeof option_names[0], err);
        if (status != PC_OK) {
            yyjson_mut_doc_free(doc);
            return status;
        }
    }
    yyjson_mut_val *patch =
        explicit_options ? yyjson_val_mut_copy(doc, explicit_options) : yyjson_mut_obj(doc);
    if (!patch) {
        yyjson_mut_doc_free(doc);
        return pc_fail(err, PC_ERR_NOMEM, "tool options");
    }
    static const struct {
        const char *input, *field;
    } map[] = {{"gradientBlendMode", "gradient_blend_mode"},
               {"gradientClassic", "gradient_classic"},
               {"eyedropperSample", "eyedropper_sample"},
               {"eyedropperRing", "eyedropper_ring"},
               {"cropOverlay", "crop_overlay"},
               {"cropOverlayShow", "crop_overlay_show"},
               {"cropOverlayOrientation", "crop_overlay_orientation"},
               {"cropShield", "crop_shield"},
               {"camTransformOnDrop", "cam_transform_on_drop"}};
    pc_status rc = PC_OK;
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) {
        yyjson_val *v = pc_json_get(p, map[i].input);
        if (v && strcmp(map[i].input, "gradientBlendMode") == 0) {
            const char *name = pc_json_as_str(v, NULL);
            pc_blend_mode mode = PC_BLEND_NORMAL;
            if (!name || !pc_engine_blend_from_str(name, strlen(name), &mode)) {
                rc = pc_fail(err, PC_ERR_INVALID, "unknown gradientBlendMode");
                break;
            }
            if (!yyjson_mut_obj_add_str(doc, patch, map[i].field, pc_blend_name(mode))) {
                rc = pc_fail(err, PC_ERR_NOMEM, "gradientBlendMode");
                break;
            }
            continue;
        }
        if (v && !yyjson_mut_obj_add_val(doc, patch, map[i].field, yyjson_val_mut_copy(doc, v))) {
            rc = pc_fail(err, PC_ERR_NOMEM, "tool option patch");
            break;
        }
    }
    yyjson_val *size = pc_json_get(p, "eyedropperSampleSize");
    if (rc == PC_OK && size) {
        uint64_t n = 0;
        const char *text = pc_json_as_str(size, NULL);
        if (yyjson_is_null(size) || (text && strcmp(text, "point") == 0)) {
            n = 1;
        } else if (!pc_json_as_u64(size, &n)) {
            rc =
                pc_fail(err, PC_ERR_INVALID, "eyedropperSampleSize must be point or a supported sample size");
        }
        const uint64_t sizes[] = {1, 3, 5, 11, 31, 51, 101};
        bool found = false;
        for (size_t i = 0; i < 7; i++) {
            found = found || sizes[i] == n;
        }
        if (rc == PC_OK && !found) {
            rc = pc_fail(err, PC_ERR_INVALID, "eyedropperSampleSize must be 1, 3, 5, 11, 31, 51 or 101");
        }
        if (rc == PC_OK) {
            yyjson_mut_obj_add_uint(doc, patch, "eyedropper_size", n);
        }
    }
    yyjson_val *shield = pc_json_get(p, "cropShield");
    if (!shield)
        shield = pc_json_get(explicit_options, "crop_shield");
    if (rc == PC_OK && shield) {
        const char *const names[] = {
            "classic_mode", "auto_center_preview", "show_cropped_area", "enabled",
            "color",        "custom_color",        "opacity",           "auto_adjust"};
        rc = fields(shield, "cropShield", names, sizeof names / sizeof *names, err);
    }
    pc_buf text = PC_BUF_INIT;
    yyjson_doc *parsed = NULL;
    if (rc == PC_OK) {
        rc = pc_json_write(patch, 0, &text, err);
    }
    if (rc == PC_OK) {
        rc = pc_json_parse(pc_buf_cstr(&text), text.len, &parsed, err);
    }
    pc_ui_tool_options next = {0};
    if (rc == PC_OK) {
        rc = pc_ui_tool_options_clone(current, &next, err);
    }
    if (rc == PC_OK) {
        rc = pc_ui_tool_options_patch(&next, yyjson_doc_get_root(parsed), err);
    }
    if (rc == PC_OK &&
        (pc_json_get(p, "gradientBlendMode") || pc_json_get(explicit_options, "gradient_blend_mode")) &&
        (next.gradient_blend_mode < PC_BLEND_NORMAL || next.gradient_blend_mode >= PC_BLEND_PSD_COUNT)) {
        rc = pc_fail(err, PC_ERR_INVALID, "unknown gradient blend mode");
    }
    if (rc == PC_OK &&
        (pc_json_get(p, "eyedropperSample") || pc_json_get(explicit_options, "eyedropper_sample"))) {
        const char *const names[] = {"current", "currentAndBelow", "all", "allNoAdjustments",
                                     "currentAndBelowNoAdjustments"};
        if (!allowed(pc_str_c(&next.eyedropper_sample), names, 5)) {
            rc = pc_fail(err, PC_ERR_INVALID, "unknown eyedropperSample");
        }
    }
    if (rc == PC_OK &&
        (pc_json_get(p, "cropOverlayOrientation") ||
         pc_json_get(explicit_options, "crop_overlay_orientation")) &&
        next.crop_overlay_orientation >= 4) {
        rc = pc_fail(err, PC_ERR_INVALID, "cropOverlayOrientation must be 0, 1, 2 or 3");
    }
    if (rc == PC_OK && shield && (next.crop_shield.opacity < 0 || next.crop_shield.opacity > 100)) {
        rc = pc_fail(err, PC_ERR_INVALID, "cropShield.opacity must be 0..100");
    }
    yyjson_val *stroke = pc_json_get(p, "shapeStroke");
    if (rc == PC_OK && stroke) {
        const char *const names[] = {"width", "opacity",    "align",  "cap",
                                     "join",  "miterLimit", "dashes", "dashOffset"};
        if (!yyjson_is_null(stroke) && !yyjson_is_false(stroke)) {
            rc = fields(stroke, "shapeStroke", names, sizeof names / sizeof *names, err);
        }
        pc_shape_stroke base = next.shape_stroke;
        base.width = next.stroke_width;
        pc_shape_stroke result = {0};
        bool present = false;
        const float black[4] = {0, 0, 0, 1};
        if (rc == PC_OK) {
            rc = pc_engine_parse_shape_stroke(stroke, &base, black, &present, &result, err);
        }
        if (rc == PC_OK) {
            next.stroke_width = present ? result.width : 0;
            if (present) {
                pc_shape_stroke_fini(&next.shape_stroke);
                next.shape_stroke = result;
                result = (pc_shape_stroke){0};
            }
        }
        pc_shape_stroke_fini(&result);
    }
    pc_buf_fini(&text);
    yyjson_doc_free(parsed);
    yyjson_mut_doc_free(doc);
    if (rc != PC_OK) {
        pc_ui_tool_options_fini(&next);
        return rc;
    }
    pc_ui_tool_options_fini(out);
    *out = next;
    return PC_OK;
}
