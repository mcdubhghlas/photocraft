#include "ui/brush_settings.h"
#include "paint/json.h"
#include <string.h>
yyjson_mut_val *pc_ui_brush_settings_fields(yyjson_mut_doc *d, const pc_paint_brush *brush)
{
    if (!d || !brush)
        return NULL;
    pc_paint_brush small = *brush;
    small.tip = (pc_paint_tip){.kind = PC_PAINT_TIP_ROUND};
    small.dual_brush.tip = (pc_paint_tip){.kind = PC_PAINT_TIP_ROUND};
    small.texture.pattern = (pc_paint_pattern){.kind = PC_PAINT_PATTERN_PROCEDURAL};
    yyjson_mut_val *root = pc_paint_json_write(PC_PAINT_JSON_BRUSH, d, &small, 0);
    if (!root)
        return NULL;
    const char *omit[] = {"tip", "color", "background", "seed", "erase", "aliased", "square"};
    for (size_t i = 0; i < sizeof omit / sizeof omit[0]; i++)
        (void)yyjson_mut_obj_remove_key(root, omit[i]);
    (void)yyjson_mut_obj_remove_key(yyjson_mut_obj_get(root, "dualBrush"), "tip");
    (void)yyjson_mut_obj_remove_key(yyjson_mut_obj_get(root, "texture"), "pattern");
    return root;
}
pc_status pc_ui_brush_setting_patch(yyjson_mut_doc *d, const char *path, yyjson_mut_val *value,
                                    yyjson_mut_val **out, pc_error *err)
{
    if (!d || !path || !*path || strlen(path) >= 192 || !value || !out)
        return pc_fail(err, PC_ERR_INVALID, "invalid brush field");
    yyjson_mut_val *root = yyjson_mut_obj(d), *object = root;
    const char *at = path;
    for (size_t depth = 0; depth < 8; depth++) {
        const char *dot = strchr(at, '.');
        size_t length = dot ? (size_t)(dot - at) : strlen(at);
        if (!length || length > 63)
            return pc_fail(err, PC_ERR_INVALID, "invalid brush field path");
        yyjson_mut_val *key = yyjson_mut_strncpy(d, at, length), *child = dot ? yyjson_mut_obj(d) : value;
        if (!object || !key || !child || !yyjson_mut_obj_add(object, key, child))
            return pc_fail(err, PC_ERR_NOMEM, "brush field patch");
        if (!dot) {
            *out = root;
            return PC_OK;
        }
        object = child;
        at = dot + 1;
    }
    return pc_fail(err, PC_ERR_LIMIT, "brush field path too deep");
}
