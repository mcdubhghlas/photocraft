#include "dialogs.h"
#include "shell_internal.h"
#include "ui/color_picker.h"
#include "ui/filter_dialog.h"
#include <math.h>
#include <string.h>
static bool rgb_of(yyjson_mut_val *value, float rgb[3])
{
    const char *hex = yyjson_mut_get_str(value);
    if (hex)
        return pc_ui_picker_parse_hex(hex, strlen(hex), rgb);
    if (yyjson_mut_is_arr(value) && yyjson_mut_arr_size(value) >= 3) {
        for (size_t i = 0; i < 3; i++) {
            yyjson_mut_val *v = yyjson_mut_arr_get(value, i);
            double n = yyjson_mut_get_num(v);
            if (!yyjson_mut_is_num(v) || !isfinite(n) || n < 0 || n > 1)
                return false;
            rgb[i] = (float)n;
        }
        return true;
    }
    return false;
}
static yyjson_mut_val *field_of(yyjson_mut_val *fields, const char *key, bool has_stop, size_t stop)
{
    yyjson_mut_val *v = yyjson_mut_obj_get(fields, key);
    return has_stop ? yyjson_mut_arr_get(yyjson_mut_arr_get(v, stop), 1) : v;
}
static yyjson_mut_val *effect_value(yyjson_mut_val *fields, const char *key, size_t index,
                                    const char *property)
{
    yyjson_mut_val *entry = yyjson_mut_arr_get(yyjson_mut_obj_get(fields, key), index),
                   *p = yyjson_mut_obj_get(entry, "params");
    yyjson_mut_val *value = yyjson_mut_obj_get(p, property);
    return value && !yyjson_mut_is_null(value)
               ? value
               : yyjson_mut_obj_get(yyjson_mut_obj_get(entry, "values"), property);
}
static bool same_value(const yyjson_mut_val *a, const yyjson_mut_val *b)
{
    if (!a || yyjson_mut_is_null(a))
        return !b || yyjson_mut_is_null(b);
    if (!b || yyjson_mut_is_null(b))
        return false;
    pc_buf first = PC_BUF_INIT, second = PC_BUF_INIT;
    bool same = pc_json_write(a, 0, &first, NULL) == PC_OK && pc_json_write(b, 0, &second, NULL) == PC_OK &&
                first.len == second.len && !memcmp(first.data, second.data, first.len);
    pc_buf_fini(&first);
    pc_buf_fini(&second);
    return same;
}
pc_status pc_ui_shell_dialog_color_field(pc_ui_shell_app *app, yyjson_val *params, pc_json_value *out,
                                         pc_error *err)
{
    uint64_t id = 0, index = 0;
    size_t length = 0;
    const char *field = pc_json_as_str(pc_json_get(params, "field"), &length);
    yyjson_val *stop = pc_json_get(params, "stop");
    bool has_stop = stop && !yyjson_is_null(stop);
    if (!pc_json_as_u64(pc_json_get(params, "dialog"), &id) || !field || !length || length > 255 ||
        strlen(field) != length || !strncmp(field, "__", 2) ||
        (has_stop && (!pc_json_as_u64(stop, &index) || index >= 64)))
        return pc_fail(err, PC_ERR_INVALID, "invalid color field");
    yyjson_val *effect = pc_json_get(params, "effect");
    bool has_effect = effect && !yyjson_is_null(effect);
    uint64_t effect_index = 0;
    size_t property_length = 0;
    const char *property = pc_json_as_str(pc_json_get(params, "property"), &property_length);
    if (has_effect && (has_stop || strcmp(field, "effects") || !pc_json_as_u64(effect, &effect_index) ||
                       effect_index >= 256 || !property || !property_length || property_length > 63 ||
                       strlen(property) != property_length || strncmp(property, "__", 2) == 0))
        return pc_fail(err, PC_ERR_INVALID, "invalid effect color field");
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_dialog *parent = pc_ui_state_dialog_mut(ui, id);
    if (!parent || !ui->n_dialogs || ui->dialogs[ui->n_dialogs - 1].id != id)
        return pc_fail(err, PC_ERR_INVALID, "the color's dialog is not active");
    yyjson_mut_val *fields = yyjson_mut_doc_get_root(parent->fields),
                   *original = yyjson_mut_obj_get(fields, field);
    if (!yyjson_mut_get_bool(yyjson_mut_obj_get(fields, "__filter")))
        return pc_fail(err, PC_ERR_INVALID, "the dialog has no editable color fields");
    pc_status status = pc_ui_filter_dialog_guard(pc_ui_shell_app_session(app), fields, err);
    if (status != PC_OK)
        return status;
    if (has_stop && (!yyjson_mut_is_arr(original) || index >= yyjson_mut_arr_size(original) ||
                     !yyjson_mut_is_arr(yyjson_mut_arr_get(original, (size_t)index)) ||
                     yyjson_mut_arr_size(yyjson_mut_arr_get(original, (size_t)index)) < 2))
        return pc_fail(err, PC_ERR_INVALID, "the gradient stop is missing");
    if (has_effect && (!yyjson_mut_is_arr(original) || effect_index >= yyjson_mut_arr_size(original)))
        return pc_fail(err, PC_ERR_INVALID, "the effect is missing");
    float rgb[3] = {0};
    yyjson_mut_val *value = has_effect ? effect_value(fields, field, (size_t)effect_index, property)
                                       : field_of(fields, field, has_stop, (size_t)index);
    if (value && !rgb_of(value, rgb))
        return pc_fail(err, PC_ERR_INVALID, "the field is not a color");
    if (!value && !strcmp(field, "tintColor")) {
        rgb[0] = .882f;
        rgb[1] = .827f;
        rgb[2] = .702f;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? pc_ui_picker_initial(doc, "dialogField", "Select Color", rgb) : NULL;
    bool ok =
        root && yyjson_mut_obj_add_uint(doc, root, "__dialog", id) &&
        yyjson_mut_obj_add_strcpy(doc, root, "__parentField", field) &&
        yyjson_mut_obj_add_bool(doc, root, "__parentHasStop", has_stop) &&
        yyjson_mut_obj_add_uint(doc, root, "__parentStop", index) &&
        yyjson_mut_obj_add_val(doc, root, "__parentValue",
                               original ? yyjson_mut_val_mut_copy(doc, original) : yyjson_mut_null(doc));
    ok = ok && yyjson_mut_obj_add_bool(doc, root, "__parentHasEffect", has_effect) &&
         yyjson_mut_obj_add_uint(doc, root, "__parentEffect", effect_index) &&
         (!has_effect || yyjson_mut_obj_add_strcpy(doc, root, "__parentProperty", property));
    if (!ok) {
        yyjson_mut_doc_free(doc);
        return pc_fail(err, PC_ERR_NOMEM, "color picker");
    }
    return pc_ui_shell_dialog_store(app, PC_DIALOG_COMMAND, doc, root, out, err);
}
static pc_status parent_of(pc_ui_shell_app *app, const yyjson_mut_val *picker, pc_ui_dialog **out,
                           pc_error *err)
{
    uint64_t id = yyjson_mut_get_uint(yyjson_mut_obj_get(picker, "__dialog"));
    pc_ui_dialog *parent = pc_ui_state_dialog_mut(pc_ui_shell_app_state(app), id);
    const char *field = yyjson_mut_get_str(yyjson_mut_obj_get(picker, "__parentField"));
    if (!parent || !field)
        return pc_fail(err, PC_ERR_NOT_FOUND, "the color's dialog was closed");
    yyjson_mut_val *fields = yyjson_mut_doc_get_root(parent->fields);
    if (!same_value(yyjson_mut_obj_get(fields, field), yyjson_mut_obj_get(picker, "__parentValue")))
        return pc_fail(err, PC_ERR_INVALID, "the color field changed while the picker was open");
    pc_status status = pc_ui_filter_dialog_guard(pc_ui_shell_app_session(app), fields, err);
    if (status == PC_OK)
        *out = parent;
    return status;
}
static pc_status write_color(yyjson_mut_doc *doc, yyjson_mut_val *fields, const yyjson_mut_val *picker,
                             pc_error *err)
{
    const char *field = yyjson_mut_get_str(yyjson_mut_obj_get(picker, "__parentField")),
               *hex = yyjson_mut_get_str(yyjson_mut_obj_get(picker, "color"));
    float rgb[3];
    if (!field || !hex || !pc_ui_picker_parse_hex(hex, strlen(hex), rgb))
        return pc_fail(err, PC_ERR_INVALID, "not a color");
    yyjson_mut_val *value = yyjson_mut_strcpy(doc, hex);
    if (!value)
        return pc_fail(err, PC_ERR_NOMEM, "picker color");
    bool has_effect = yyjson_mut_get_bool(yyjson_mut_obj_get(picker, "__parentHasEffect"));
    bool has_stop = yyjson_mut_get_bool(yyjson_mut_obj_get(picker, "__parentHasStop"));
    if (has_effect) {
        size_t index = (size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(picker, "__parentEffect"));
        const char *property = yyjson_mut_get_str(yyjson_mut_obj_get(picker, "__parentProperty"));
        yyjson_mut_val *entry = yyjson_mut_arr_get(yyjson_mut_obj_get(fields, field), index),
                       *params = yyjson_mut_obj_get(entry, "params");
        if (!entry || !property)
            return pc_fail(err, PC_ERR_INVALID, "the effect changed");
        if (!yyjson_mut_is_obj(params)) {
            params = yyjson_mut_obj(doc);
            yyjson_mut_val *name = yyjson_mut_str(doc, "params");
            if (!params || !name || !yyjson_mut_obj_put(entry, name, params))
                return pc_fail(err, PC_ERR_NOMEM, "effect color");
        }
        yyjson_mut_val *name = yyjson_mut_strcpy(doc, property);
        if (!name || !yyjson_mut_obj_put(params, name, value))
            return pc_fail(err, PC_ERR_NOMEM, "effect color");
    } else if (has_stop) {
        size_t index = (size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(picker, "__parentStop"));
        yyjson_mut_val *pair = yyjson_mut_arr_get(yyjson_mut_obj_get(fields, field), index);
        if (!pair || !yyjson_mut_arr_replace(pair, 1, value))
            return pc_fail(err, PC_ERR_INVALID, "the gradient stop changed");
    } else {
        yyjson_mut_val *key = yyjson_mut_strcpy(doc, field);
        if (!key || !yyjson_mut_obj_put(fields, key, value))
            return pc_fail(err, PC_ERR_NOMEM, "picker color");
    }
    return PC_OK;
}
pc_status pc_ui_shell_dialog_color_field_confirm(pc_ui_shell_app *app, uint64_t id, pc_json_value *out,
                                                 pc_error *err)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_dialog *picker = pc_ui_state_dialog_mut(ui, id), *parent = NULL;
    if (!picker)
        return pc_fail(err, PC_ERR_NOT_FOUND, "no color picker");
    yyjson_mut_val *fields = yyjson_mut_doc_get_root(picker->fields);
    pc_status status = parent_of(app, fields, &parent, err);
    if (status == PC_OK)
        status = write_color(parent->fields, yyjson_mut_doc_get_root(parent->fields), fields, err);
    if (status == PC_OK) {
        pc_ui_state_close_dialog(ui, id);
        pc_json_value_fini(out);
        *out = (pc_json_value)PC_JSON_VALUE_INIT;
    }
    return status;
}
pc_status pc_ui_shell_dialog_color_field_preview(pc_ui_shell_app *app, const yyjson_mut_val *picker,
                                                 yyjson_mut_doc **out, pc_error *err)
{
    pc_ui_dialog *parent = NULL;
    pc_status status = parent_of(app, picker, &parent, err);
    if (status != PC_OK)
        return status;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_val_mut_copy(doc, yyjson_mut_doc_get_root(parent->fields)) : NULL;
    if (!root) {
        yyjson_mut_doc_free(doc);
        return pc_fail(err, PC_ERR_NOMEM, "picker preview");
    }
    status = write_color(doc, root, picker, err);
    if (status != PC_OK) {
        yyjson_mut_doc_free(doc);
        return status;
    }
    yyjson_mut_doc_set_root(doc, root);
    *out = doc;
    return PC_OK;
}
