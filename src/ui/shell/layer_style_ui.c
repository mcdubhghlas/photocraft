#include "ui/i18n.h"
#include "layer_style_ui.h"
#include "shell_internal.h"
#include "ui/filter_dialog.h"
#include "ui/widgets.h"
#include "ui/pen_tool.h"
#include "ui/type_tool.h"
#include "engine/style_editor.h"
#include "color/blend.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *const KINDS[] = {"dropShadow", "innerShadow",  "outerGlow",       "innerGlow",
                                    "stroke",     "colorOverlay", "gradientOverlay", "patternOverlay",
                                    "satin",      "bevelEmboss"};
static const char *const LABELS[] = {"Drop Shadow", "Inner Shadow",  "Outer Glow",       "Inner Glow",
                                     "Stroke",      "Color Overlay", "Gradient Overlay", "Pattern Overlay",
                                     "Satin",       "Bevel & Emboss"};
static const char *label(const char *kind)
{
    for (size_t i = 0; i < 10; i++)
        if (kind && !strcmp(kind, KINDS[i]))
            return LABELS[i];
    return "Layer Effect";
}
bool pc_ui_shell_style_command(const char *command, const char **kind)
{
    if (!command)
        return false;
    if (!strcmp(command, "layer.layerStyle.edit") || !strcmp(command, "layer.layerStyle.blendingOptions")) {
        *kind = NULL;
        return true;
    }
    const char *prefix = "layer.layerStyle.";
    if (strncmp(command, prefix, strlen(prefix)))
        return false;
    for (size_t i = 0; i < 10; i++)
        if (!strcmp(command + strlen(prefix), KINDS[i])) {
            *kind = KINDS[i];
            return true;
        }
    return false;
}
static void put(pc_ui_dialog *dialog, yyjson_mut_val *object, const char *key, yyjson_mut_val *value)
{
    yyjson_mut_val *name = yyjson_mut_strcpy(dialog->fields, key);
    if (name && value)
        (void)yyjson_mut_obj_put(object, name, value);
}
pc_status pc_ui_shell_style_open(pc_ui_shell_app *app, const char *kind, pc_json_value *out, pc_error *err)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (ui->n_dialogs)
        return pc_fail(err, PC_ERR_INVALID, "confirm or cancel the open dialog first");
    pc_json_value committed = PC_JSON_VALUE_INIT;
    pc_status prepared = PC_OK;
    if (ui->has_pen)
        prepared = pc_ui_pen_commit(pc_ui_shell_app_session(app), ui, false, &committed, err);
    if (prepared == PC_OK && ui->has_text_edit)
        prepared = pc_ui_type_commit(pc_ui_shell_app_session(app), ui, 0, &committed, err);
    pc_json_value_fini(&committed);
    if (prepared != PC_OK)
        return prepared;
    pc_ui_direct_cancel(&ui->direct);
    yyjson_mut_doc *doc = NULL;
    pc_status status =
        pc_ui_filter_dialog_fields(pc_ui_shell_app_session(app), "layer.layerStyle.edit", &doc, err);
    if (status != PC_OK)
        return status;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc), *initial = NULL;
    status = pc_engine_style_editor_initial(pc_ui_shell_app_session(app), kind, doc, &initial, err);
    if (status == PC_OK && !yyjson_mut_is_obj(initial))
        status = pc_fail(err, PC_ERR_INVALID, "missing style state");
    if (status == PC_OK) {
        yyjson_mut_obj_iter iter = yyjson_mut_obj_iter_with(initial);
        yyjson_mut_val *key;
        while ((key = yyjson_mut_obj_iter_next(&iter))) {
            yyjson_mut_val *value = yyjson_mut_obj_iter_get_val(key);
            put(&(pc_ui_dialog){.fields = doc}, root, yyjson_mut_get_str(key),
                yyjson_mut_val_mut_copy(doc, value));
        }
    }
    if (status == PC_OK) {
        put(&(pc_ui_dialog){.fields = doc}, root, "__label", yyjson_mut_str(doc, "Layer Style"));
        put(&(pc_ui_dialog){.fields = doc}, root, "__style", yyjson_mut_bool(doc, true));
    }
    if (status != PC_OK) {
        yyjson_mut_doc_free(doc);
        return status;
    }
    return pc_ui_shell_dialog_store(app, PC_DIALOG_COMMAND, doc, root, out, err);
}
static yyjson_mut_val *effective(yyjson_mut_val *params, yyjson_mut_val *base, const char *key)
{
    yyjson_mut_val *v = yyjson_mut_obj_get(params, key);
    return v && !yyjson_mut_is_null(v) ? v : yyjson_mut_obj_get(base, key);
}
static int choose(pc_ui_shell_app *app, const char *id, const char *const *labels, size_t count, int selected)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    if (selected < 0 || (size_t)selected >= count)
        selected = 0;
    pc_ui_shell_widget_mapping(app, id, nk_widget_bounds(ctx));
    int next = selected;
    if (nk_combo_begin_label(ctx, labels[selected], nk_vec2(280, 260))) {
        pc_ui_shell_dialog_runtime(app)->graph_blocked = true;
        nk_layout_row_dynamic(ctx, 24, 1);
        for (size_t i = 0; i < count; i++) {
            char mapping[160];
            (void)snprintf(mapping, sizeof mapping, "%s.%zu", id, i);
            pc_ui_shell_widget_mapping(app, mapping, nk_widget_bounds(ctx));
            if (nk_button_label(ctx, labels[i])) {
                next = (int)i;
                nk_combo_close(ctx);
            }
        }
        nk_combo_end(ctx);
    }
    return next;
}
static void title(const char *key, char out[96])
{
    size_t n = 0;
    for (size_t i = 0; key[i] && n + 2 < 96; i++) {
        char c = key[i];
        if (i && c >= 'A' && c <= 'Z')
            out[n++] = ' ';
        out[n++] = !i && c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
    }
    out[n] = 0;
}
static void blend_widget(pc_ui_shell_app *app, pc_ui_dialog *dialog, yyjson_mut_val *params,
                         yyjson_mut_val *base, const char *key)
{
    const char *names[PC_BLEND_COUNT];
    for (size_t i = 0; i < PC_BLEND_COUNT; i++)
        names[i] = pc_blend_label((pc_blend_mode)i);
    const char *value = yyjson_mut_get_str(effective(params, base, key));
    pc_blend_mode mode = PC_BLEND_NORMAL;
    if (value)
        (void)pc_engine_blend_from_str(value, strlen(value), &mode);
    char mapping[96];
    (void)snprintf(mapping, sizeof mapping, "style.%s", key);
    int next = choose(app, mapping, names, PC_BLEND_COUNT, (int)mode);
    if (next != (int)mode)
        put(dialog, params, key, yyjson_mut_strcpy(dialog->fields, names[next]));
}
static bool number_widget(pc_ui_shell_app *app, const char *key, const char *text, double *value, double min,
                          double max)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    char mapping[96];
    (void)snprintf(mapping, sizeof mapping, "style.%s", key);
    pc_ui_shell_widget_mapping(app, mapping, nk_widget_bounds(ctx));
    double old = *value;
    nk_property_double(ctx, text, min, value, max, 1, .1f);
    return *value != old;
}
static void boolean_widget(pc_ui_shell_app *app, pc_ui_dialog *dialog, yyjson_mut_val *params,
                           yyjson_mut_val *base, const char *key)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    bool old = yyjson_mut_get_bool(effective(params, base, key));
    nk_bool next = old;
    char text[96], mapping[96];
    title(key, text);
    (void)snprintf(mapping, sizeof mapping, "style.%s", key);
    pc_ui_shell_widget_mapping(app, mapping, nk_widget_bounds(ctx));
    nk_checkbox_label(ctx, text, &next);
    if ((next != 0) != old)
        put(dialog, params, key, yyjson_mut_bool(dialog->fields, next != 0));
}
static void color_widget(pc_ui_shell_app *app, pc_ui_dialog *dialog, size_t index, const char *key,
                         yyjson_mut_val *value)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    float rgb[3] = {0};
    const char *hex = yyjson_mut_get_str(value);
    if (hex) {
        unsigned a = 0, b = 0, c = 0;
        if (sscanf(hex, "#%2x%2x%2x", &a, &b, &c) == 3) {
            rgb[0] = (float)a / 255;
            rgb[1] = (float)b / 255;
            rgb[2] = (float)c / 255;
        }
    } else
        for (size_t i = 0; i < 3; i++) {
            double n = yyjson_mut_get_num(yyjson_mut_arr_get(value, i));
            rgb[i] = isfinite(n) ? (float)fmin(1, fmax(0, n)) : 0;
        }
    char text[96], mapping[96];
    title(key, text);
    nk_layout_row_dynamic(ctx, 26, 2);
    nk_label(ctx, text, NK_TEXT_LEFT);
    (void)snprintf(mapping, sizeof mapping, "style.%s.color", key);
    pc_ui_shell_widget_mapping(app, mapping, nk_widget_bounds(ctx));
    if (nk_button_color(ctx, nk_rgb_f(rgb[0], rgb[1], rgb[2]))) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
        if (p && yyjson_mut_obj_add_uint(d, p, "dialog", dialog->id) &&
            yyjson_mut_obj_add_str(d, p, "field", "effects") &&
            yyjson_mut_obj_add_uint(d, p, "effect", index) &&
            yyjson_mut_obj_add_strcpy(d, p, "property", key))
            pc_ui_shell_queue(app, "ui.dialog.colorField", p);
        yyjson_mut_doc_free(d);
    }
}
static void defaults(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime, pc_ui_dialog *dialog,
                     yyjson_mut_val *entry)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const char *kind = yyjson_mut_get_str(yyjson_mut_obj_get(entry, "kind"));
    if (!kind)
        return;
    nk_layout_row_dynamic(ctx, 28, 2);
    pc_ui_shell_widget_mapping(app, "style.default.reset", nk_widget_bounds(ctx));
    bool reset = nk_button_label(ctx, pc_i18n_t("Reset to Default"));
    pc_ui_shell_widget_mapping(app, "style.default.make", nk_widget_bounds(ctx));
    bool make = nk_button_label(ctx, pc_i18n_t("Make Default"));
    if (!reset && !make)
        return;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *request = d ? yyjson_mut_obj(d) : NULL;
    bool ok = request && yyjson_mut_obj_add_strcpy(d, request, "kind", kind);
    if (make && ok) {
        yyjson_mut_val *shown = yyjson_mut_val_mut_copy(d, yyjson_mut_obj_get(entry, "values")),
                       *params = yyjson_mut_obj_get(entry, "params");
        if (!yyjson_mut_is_obj(shown))
            shown = yyjson_mut_obj(d);
        yyjson_mut_obj_iter iter = yyjson_mut_obj_iter_with(params);
        yyjson_mut_val *key;
        while ((key = yyjson_mut_obj_iter_next(&iter))) {
            yyjson_mut_val *value = yyjson_mut_obj_iter_get_val(key), *k = yyjson_mut_val_mut_copy(d, key),
                           *v = yyjson_mut_val_mut_copy(d, value);
            ok = ok && shown && k && v && yyjson_mut_obj_put(shown, k, v);
        }
        ok = ok && shown && yyjson_mut_obj_add_val(d, request, "params", shown);
    }
    pc_buf json = PC_BUF_INIT;
    pc_json_value result = PC_JSON_VALUE_INIT;
    pc_error error = {0};
    pc_status status =
        ok ? pc_json_write(request, 0, &json, &error) : pc_fail(&error, PC_ERR_NOMEM, "Layer Style defaults");
    if (status == PC_OK)
        status = pc_engine_session_execute_json(pc_ui_shell_app_session(app),
                                                make ? "layer.layerStyle.makeDefault"
                                                     : "layer.layerStyle.defaultFor",
                                                pc_buf_cstr(&json), &result, &error);
    if (status == PC_OK && reset) {
        yyjson_mut_val *params = yyjson_mut_obj_get(pc_json_value_root(&result), "params"),
                       *copy = yyjson_mut_val_mut_copy(dialog->fields, params);
        if (copy)
            put(dialog, entry, "params", copy);
    }
    if (status != PC_OK)
        (void)snprintf(runtime->error, sizeof runtime->error, "%s", error.msg);
    pc_buf_fini(&json);
    pc_json_value_fini(&result);
    yyjson_mut_doc_free(d);
}
static uint64_t next_effect_id(yyjson_mut_val *effects)
{
    for (uint64_t id = 1; id <= 257; id++) {
        char text[40];
        (void)snprintf(text, sizeof text, "fx%llu", (unsigned long long)id);
        bool found = false;
        for (size_t i = 0; i < yyjson_mut_arr_size(effects); i++) {
            const char *name = yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_arr_get(effects, i), "id"));
            found = found || (name && !strcmp(name, text));
        }
        if (!found)
            return id;
    }
    return 0;
}
static void instance_controls(pc_ui_shell_app *app, pc_ui_dialog *dialog, size_t *selected)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields),
                   *effects = yyjson_mut_obj_get(root, "effects");
    size_t count = yyjson_mut_arr_size(effects);
    nk_layout_row_dynamic(ctx, 26, 3);
    pc_ui_shell_widget_mapping(app, "style.up", nk_widget_bounds(ctx));
    bool up = nk_button_label(ctx, pc_i18n_t("Up"));
    pc_ui_shell_widget_mapping(app, "style.down", nk_widget_bounds(ctx));
    bool down = nk_button_label(ctx, pc_i18n_t("Down"));
    pc_ui_shell_widget_mapping(app, "style.duplicate", nk_widget_bounds(ctx));
    bool duplicate = nk_button_label(ctx, pc_i18n_t("Duplicate"));
    if (*selected >= count || (!up && !down && !duplicate) || (duplicate && count >= 256))
        return;
    size_t from = *selected, to = up && from > 0 ? from - 1 : down && from + 1 < count ? from + 1 : from;
    if (!duplicate && from == to)
        return;
    yyjson_mut_val *array = yyjson_mut_arr(dialog->fields), *copy = NULL;
    bool ok = array != NULL;
    if (duplicate) {
        copy = yyjson_mut_val_mut_copy(dialog->fields, yyjson_mut_arr_get(effects, from));
        char id[40];
        (void)snprintf(id, sizeof id, "fx%llu", (unsigned long long)next_effect_id(effects));
        if (copy)
            put(dialog, copy, "id", yyjson_mut_strcpy(dialog->fields, id));
        else
            ok = false;
        to = from + 1;
    }
    for (size_t i = 0; ok && i < count; i++) {
        size_t index = duplicate ? i : i == from ? to : i == to ? from : i;
        yyjson_mut_val *value = yyjson_mut_val_mut_copy(dialog->fields, yyjson_mut_arr_get(effects, index));
        ok = value && yyjson_mut_arr_add_val(array, value);
        if (ok && duplicate && i == from)
            ok = yyjson_mut_arr_add_val(array, copy);
    }
    if (ok) {
        yyjson_mut_val *entry = yyjson_mut_arr_get(array, to), *id = yyjson_mut_obj_get(entry, "id");
        put(dialog, root, "effects", array);
        put(dialog, root, "selected", yyjson_mut_val_mut_copy(dialog->fields, id));
        *selected = to;
    }
}
static void properties(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog,
                       yyjson_mut_val *entry, size_t index)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields),
                   *params = yyjson_mut_obj_get(entry, "params"), *base = yyjson_mut_obj_get(entry, "values");
    if (!yyjson_mut_is_obj(params)) {
        params = yyjson_mut_obj(dialog->fields);
        if (!params)
            return;
        put(dialog, entry, "params", params);
    }
    const char *kind = yyjson_mut_get_str(yyjson_mut_obj_get(entry, "kind"));
    char command[128];
    (void)snprintf(command, sizeof command, "layer.layerStyle.%s", kind ? kind : "");
    const pc_engine_command *spec = pc_engine_command_find(command);
    pc_ui_parameter_schema schema = {0};
    if (!spec || pc_ui_parameter_schema_parse(&schema, spec->params, NULL) != PC_OK)
        return;
    nk_layout_row_dynamic(ctx, 26, 1);
    nk_label(ctx, label(kind), NK_TEXT_LEFT);
    for (size_t i = 0; i < schema.count; i++) {
        const pc_ui_param *p = &schema.items[i];
        if (!strcmp(p->key, "add") || !strcmp(p->key, "layer"))
            continue;
        bool supported =
            p->kind != PC_UI_PARAM_STRUCTURED || !strcmp(p->key, "blend") || !strcmp(p->key, "angle") ||
            !strcmp(p->key, "altitude") || !strcmp(p->key, "contour") || !strcmp(p->key, "contourName") ||
            !strcmp(p->key, "glossContour") || !strcmp(p->key, "texture") || !strcmp(p->key, "pattern");
        if (!supported)
            continue;
        yyjson_mut_val *v = effective(params, base, p->key);
        char text[96];
        title(p->key, text);
        if (p->kind == PC_UI_PARAM_COLOR) {
            color_widget(app, dialog, index, p->key, v);
            continue;
        }
        nk_layout_row_dynamic(ctx, 26, 1);
        if (!strcmp(p->key, "blend")) {
            blend_widget(app, dialog, params, base, p->key);
            continue;
        }
        bool contour = !strcmp(p->key, "contourName") || !strcmp(p->key, "glossContour") ||
                       (!strcmp(p->key, "contour") && p->kind != PC_UI_PARAM_BOOL);
        if (contour) {
            const char *names[128];
            names[0] = "Custom";
            size_t count = 1;
            const char *name;
            while (count < 128 && (name = pc_engine_style_contour_name(count - 1)))
                names[count++] = name;
            const char *value = yyjson_mut_get_str(v);
            int selected = 0;
            for (size_t j = 1; j < count; j++)
                if (value && !strcmp(value, names[j]))
                    selected = (int)j;
            char mapping[96];
            (void)snprintf(mapping, sizeof mapping, "style.%s", p->key);
            int next = choose(app, mapping, names, count, selected);
            if (next != selected && next > 0)
                put(dialog, params, p->key, yyjson_mut_strcpy(dialog->fields, names[next]));
            pc_ui_shell_style_contour(app, r, dialog, entry, p->key);
            continue;
        }
        if (p->kind == PC_UI_PARAM_BOOL)
            boolean_widget(app, dialog, params, base, p->key);
        else if (p->kind == PC_UI_PARAM_COLOR) {
            color_widget(app, dialog, index, p->key, v);
        } else if (p->kind == PC_UI_PARAM_CHOICE) {
            const char *current = yyjson_mut_get_str(v);
            int selected = 0;
            for (size_t j = 0; j < p->nchoices; j++)
                if (current && !strcmp(current, p->choices[j]))
                    selected = (int)j;
            char mapping[96];
            (void)snprintf(mapping, sizeof mapping, "style.%s", p->key);
            int next = choose(app, mapping, (const char *const *)p->choices, p->nchoices, selected);
            if (next != selected)
                put(dialog, params, p->key, yyjson_mut_strcpy(dialog->fields, p->choices[next]));
        } else if (p->kind == PC_UI_PARAM_RANGE || p->kind == PC_UI_PARAM_INTEGER ||
                   !strcmp(p->key, "angle") || !strcmp(p->key, "altitude")) {
            double current =
                yyjson_mut_is_num(v) && isfinite(yyjson_mut_get_num(v)) ? yyjson_mut_get_num(v) : p->number;
            double min = p->kind == PC_UI_PARAM_RANGE ? p->min : 0,
                   max = p->kind == PC_UI_PARAM_RANGE ? p->max : 10000;
            if (!strcmp(p->key, "angle")) {
                min = -180;
                max = 180;
            } else if (!strcmp(p->key, "altitude")) {
                min = 0;
                max = 90;
            }
            bool global = (!strcmp(p->key, "angle") || !strcmp(p->key, "altitude")) &&
                          yyjson_mut_get_bool(effective(params, base, "useGlobalLight"));
            yyjson_mut_val *light = yyjson_mut_obj_get(root, "globalLight"),
                           *light_base = yyjson_mut_obj_get(root, "globalLightValues");
            if (global) {
                yyjson_mut_val *value = effective(light, light_base, p->key);
                if (yyjson_mut_is_num(value))
                    current = yyjson_mut_get_num(value);
            }
            if (number_widget(app, p->key, text, &current, min, max))
                put(dialog, global ? light : params, p->key, yyjson_mut_real(dialog->fields, current));
        } else if (p->kind == PC_UI_PARAM_TEXT || !strcmp(p->key, "texture") || !strcmp(p->key, "pattern")) {
            const char *value = yyjson_mut_get_str(v);
            if (!value)
                value = "";
            char key[128];
            (void)snprintf(key, sizeof key, "style.%s.%zu", p->key, index);
            pc_ui_dialog_text *buffer = pc_ui_dialog_text_buffer(r, dialog->id, key, value);
            nk_layout_row_dynamic(ctx, 26, 2);
            nk_label(ctx, text, NK_TEXT_LEFT);
            if (buffer) {
                nk_flags flags = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, (char *)buffer->text.data,
                                                                (int)buffer->text.cap, nk_filter_default);
                buffer->active = (flags & NK_EDIT_ACTIVE) != 0;
                buffer->text.len = strlen(pc_buf_cstr(&buffer->text));
                if (strcmp(value, pc_buf_cstr(&buffer->text)))
                    put(dialog, params, p->key,
                        yyjson_mut_strcpy(dialog->fields, pc_buf_cstr(&buffer->text)));
            }
        }
    }
    pc_ui_parameter_schema_fini(&schema);
    defaults(app, r, dialog, entry);
}
static void blending(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog)
{
    (void)r;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields),
                   *params = yyjson_mut_obj_get(root, "blending"),
                   *base = yyjson_mut_obj_get(root, "blendingValues");
    if (!yyjson_mut_is_obj(params)) {
        params = yyjson_mut_obj(dialog->fields);
        if (!params)
            return;
        put(dialog, root, "blending", params);
    }
    nk_layout_row_dynamic(ctx, 26, 1);
    nk_label(ctx, pc_i18n_t("Blending Options"), NK_TEXT_LEFT);
    blend_widget(app, dialog, params, base, "blend");
    const char *keys[] = {"opacity", "fillOpacity"};
    for (size_t i = 0; i < 2; i++) {
        yyjson_mut_val *v = effective(params, base, keys[i]);
        double number = yyjson_mut_is_num(v) ? yyjson_mut_get_num(v) : 100;
        char text[96];
        title(keys[i], text);
        nk_layout_row_dynamic(ctx, 26, 1);
        if (number_widget(app, keys[i], text, &number, 0, 100))
            put(dialog, params, keys[i], yyjson_mut_real(dialog->fields, number));
    }
    nk_layout_row_dynamic(ctx, 26, 1);
    const char *knockouts[] = {"none", "shallow", "deep"};
    const char *old = yyjson_mut_get_str(effective(params, base, "knockout"));
    int selected = old && !strcmp(old, "shallow") ? 1 : old && !strcmp(old, "deep") ? 2 : 0;
    int next = choose(app, "style.knockout", knockouts, 3, selected);
    if (next != selected)
        put(dialog, params, "knockout", yyjson_mut_strcpy(dialog->fields, knockouts[next]));
    const char *booleans[] = {"blendInteriorEffectsAsGroup", "blendClippedLayersAsGroup",
                              "transparencyShapesLayer", "layerMaskHidesEffects", "vectorMaskHidesEffects"};
    for (size_t i = 0; i < 5; i++) {
        nk_layout_row_dynamic(ctx, 26, 1);
        boolean_widget(app, dialog, params, base, booleans[i]);
    }
    yyjson_mut_val *channels = effective(params, base, "channels");
    size_t count = yyjson_mut_arr_size(channels);
    if (count > 32)
        count = 32;
    for (size_t i = 0; i < count; i++) {
        nk_layout_row_dynamic(ctx, 26, 1);
        char text[64];
        (void)snprintf(text, sizeof text, "Channel %zu", i + 1);
        nk_bool old_value = yyjson_mut_get_bool(yyjson_mut_arr_get(channels, i)), new_value = old_value;
        nk_checkbox_label(ctx, text, &new_value);
        if (old_value != new_value) {
            yyjson_mut_val *copy = yyjson_mut_val_mut_copy(dialog->fields, channels);
            if (copy && yyjson_mut_arr_replace(copy, i, yyjson_mut_bool(dialog->fields, new_value != 0)))
                put(dialog, params, "channels", copy);
        }
    }
    pc_ui_shell_style_blend_if(app, r, dialog);
}
void pc_ui_shell_style_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields),
                   *effects = yyjson_mut_obj_get(root, "effects");
    size_t count = yyjson_mut_arr_size(effects);
    if (!yyjson_mut_is_arr(effects) || count > 256) {
        nk_layout_row_dynamic(ctx, 36, 1);
        nk_label_wrap(ctx, pc_i18n_t("The effect list is invalid."));
        return;
    }
    if (r->style_dialog != dialog->id) {
        r->style_dialog = dialog->id;
        r->style_kind = 0;
        r->style_blend_channel = 0;
    }
    const char *selected = yyjson_mut_get_str(yyjson_mut_obj_get(root, "selected"));
    size_t selected_index = SIZE_MAX;
    nk_layout_row_begin(ctx, NK_DYNAMIC, 460, 2);
    nk_layout_row_push(ctx, .34f);
    if (nk_group_begin(ctx, "style-list", NK_WINDOW_BORDER)) {
        nk_layout_row_dynamic(ctx, 26, 1);
        pc_ui_shell_widget_mapping(app, "style.blending", nk_widget_bounds(ctx));
        if (nk_button_label(ctx, pc_i18n_t("Blending Options")))
            put(dialog, root, "selected", yyjson_mut_str(dialog->fields, "blendingOptions"));
        for (size_t i = 0; i < count; i++) {
            yyjson_mut_val *entry = yyjson_mut_arr_get(effects, i);
            const char *id = yyjson_mut_get_str(yyjson_mut_obj_get(entry, "id")),
                       *kind = yyjson_mut_get_str(yyjson_mut_obj_get(entry, "kind"));
            if (!id || !kind)
                continue;
            if (selected && !strcmp(selected, id))
                selected_index = i;
            nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 2);
            nk_layout_row_push(ctx, .15f);
            nk_bool on = !yyjson_mut_is_false(yyjson_mut_obj_get(entry, "on")), next = on;
            nk_checkbox_label(ctx, pc_i18n_t(""), &next);
            if (next != on)
                put(dialog, entry, "on", yyjson_mut_bool(dialog->fields, next != 0));
            nk_layout_row_push(ctx, .85f);
            char mapping[96];
            (void)snprintf(mapping, sizeof mapping, "style.select.%s", id);
            pc_ui_shell_widget_mapping(app, mapping, nk_widget_bounds(ctx));
            if (nk_button_label(ctx, label(kind))) {
                put(dialog, root, "selected", yyjson_mut_strcpy(dialog->fields, id));
                selected_index = i;
            }
            nk_layout_row_end(ctx);
        }
        nk_layout_row_dynamic(ctx, 26, 1);
        int kind = (int)(r->style_kind < 10 ? r->style_kind : 0);
        int next = choose(app, "style.kind", LABELS, 10, kind);
        r->style_kind = (size_t)next;
        nk_layout_row_dynamic(ctx, 26, 2);
        pc_ui_shell_widget_mapping(app, "style.add", nk_widget_bounds(ctx));
        bool add = nk_button_label(ctx, pc_i18n_t("Add"));
        pc_ui_shell_widget_mapping(app, "style.delete", nk_widget_bounds(ctx));
        bool remove = nk_button_label(ctx, pc_i18n_t("Delete"));
        if (add && count < 256) {
            uint64_t next_id = next_effect_id(effects);
            char id[40];
            (void)snprintf(id, sizeof id, "fx%llu", (unsigned long long)next_id);
            yyjson_mut_val *entry = NULL;
            pc_error error = {0};
            if (pc_engine_style_editor_new_entry(pc_ui_shell_app_session(app), KINDS[r->style_kind], next_id,
                                                 dialog->fields, &entry, &error) == PC_OK &&
                yyjson_mut_arr_add_val(effects, entry)) {
                put(dialog, root, "selected", yyjson_mut_strcpy(dialog->fields, id));
                selected_index = count++;
            } else
                (void)snprintf(r->error, sizeof r->error, "%s", error.msg);
        }
        if (remove && selected_index < count) {
            (void)yyjson_mut_arr_remove(effects, selected_index);
            count--;
            selected_index = SIZE_MAX;
            put(dialog, root, "selected", yyjson_mut_str(dialog->fields, "blendingOptions"));
        }
        instance_controls(app, dialog, &selected_index);
        effects = yyjson_mut_obj_get(root, "effects");
        count = yyjson_mut_arr_size(effects);
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_label(ctx, pc_i18n_t("Canvas Preview"), NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 150, 1);
        struct nk_rect bounds;
        (void)nk_widget(&bounds, ctx);
        struct nk_image image;
        float aspect;
        if (pc_ui_shell_canvas_preview(app, &image, &aspect)) {
            float w = fminf(bounds.w, bounds.h * aspect), h = w / aspect;
            struct nk_rect rect = nk_rect(bounds.x + (bounds.w - w) / 2, bounds.y + (bounds.h - h) / 2, w, h);
            pc_ui_shell_widget_mapping(app, "style.preview", rect);
            nk_draw_image(nk_window_get_canvas(ctx), rect, &image, nk_rgba(255, 255, 255, 255));
        }
        nk_group_end(ctx);
    }
    nk_layout_row_push(ctx, .66f);
    if (nk_group_begin(ctx, "style-properties", NK_WINDOW_BORDER)) {
        if (selected_index < count)
            properties(app, r, dialog, yyjson_mut_arr_get(effects, selected_index), selected_index);
        else
            blending(app, r, dialog);
        nk_group_end(ctx);
    }
    nk_layout_row_end(ctx);
    nk_layout_row_dynamic(ctx, 26, 2);
    nk_bool preview = yyjson_mut_get_bool(yyjson_mut_obj_get(root, "preview")),
            enabled = yyjson_mut_get_bool(yyjson_mut_obj_get(root, "enabled"));
    if (nk_checkbox_label(ctx, pc_i18n_t("Preview"), &preview))
        put(dialog, root, "preview", yyjson_mut_bool(dialog->fields, preview != 0));
    if (nk_checkbox_label(ctx, pc_i18n_t("Layer Effects"), &enabled))
        put(dialog, root, "enabled", yyjson_mut_bool(dialog->fields, enabled != 0));
    nk_layout_row_dynamic(ctx, 24, 1);
    if (pc_ui_filter_preview_busy(&r->preview))
        nk_label(ctx, pc_i18n_t("Updating preview..."), NK_TEXT_LEFT);
    else if (r->preview.has_ready && r->preview.status != PC_OK)
        nk_label_wrap(ctx, r->preview.error.msg);
}
