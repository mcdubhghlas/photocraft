/* Definitions and typed data-set values are drafts until the single atomic command succeeds. */
#include "variables_ui.h"
#include "shell_internal.h"
#include "ui/i18n.h"
#include <stdio.h>
#include <string.h>

bool pc_ui_shell_variables_command(const char *command)
{
    return command &&
           (!strcmp(command, "image.variables.define") || !strcmp(command, "image.variables.dataSets") ||
            !strcmp(command, "image.applyDataSet"));
}
static bool put(yyjson_mut_doc *d, yyjson_mut_val *o, const char *key, yyjson_mut_val *v)
{
    yyjson_mut_val *k = yyjson_mut_strcpy(d, key);
    return k && v && yyjson_mut_obj_put(o, k, v);
}
pc_status pc_ui_shell_variables_open(pc_ui_shell_app *app, const char *command, pc_json_value *out,
                                     pc_error *err)
{
    pc_engine_session *s = pc_ui_shell_app_session(app);
    const pc_engine_doc_state *state = pc_engine_session_active(s);
    if (!state)
        return pc_fail(err, PC_ERR_INVALID, "Variables needs an open document");
    pc_json_value current = PC_JSON_VALUE_INIT;
    pc_status status = pc_engine_session_execute(s, "variables.list", NULL, &current, err);
    yyjson_mut_doc *d = status == PC_OK ? yyjson_mut_doc_new(NULL) : NULL;
    yyjson_mut_val *root = d ? yyjson_mut_val_mut_copy(d, pc_json_value_root(&current)) : NULL;
    bool ok = root && yyjson_mut_obj_add_str(d, root, "__command", "image.variables.edit") &&
              yyjson_mut_obj_add_str(d, root, "__label", "Variables and Data Sets") &&
              yyjson_mut_obj_add_bool(d, root, "__variables", true) &&
              yyjson_mut_obj_add_uint(d, root, "__document", pc_doc_arc_get(state->doc)->id) &&
              yyjson_mut_obj_add_uint(d, root, "__revision", state->revision) &&
              yyjson_mut_obj_add_uint(d, root, "__page", !strcmp(command, "image.variables.define") ? 0 : 1);
    if (root)
        (void)yyjson_mut_obj_remove_str(root, "active");
    pc_json_value_fini(&current);
    if (status != PC_OK || !ok) {
        yyjson_mut_doc_free(d);
        return status != PC_OK ? status : pc_fail(err, PC_ERR_NOMEM, "Variables draft");
    }
    return pc_ui_shell_dialog_store(app, PC_DIALOG_COMMAND, d, root, out, err);
}
static void text_field(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog, yyjson_mut_val *o,
                       const char *key, const char *label, const char *identity)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *v = yyjson_mut_obj_get(o, key);
    const char *value = yyjson_mut_get_str(v);
    nk_layout_row_dynamic(ctx, 26, 2);
    nk_label(ctx, pc_i18n_t(label), NK_TEXT_LEFT);
    if (v && !value) {
        nk_label(ctx, pc_i18n_t("Invalid text value"), NK_TEXT_LEFT);
        return;
    }
    struct nk_rect bounds = nk_widget_bounds(ctx), content = nk_window_get_content_region(ctx);
    if (bounds.y + bounds.h < content.y || bounds.y > content.y + content.h) {
        nk_spacing(ctx, 1);
        return;
    }
    pc_ui_dialog_text *text = pc_ui_dialog_text_buffer(r, dialog->id, identity, value ? value : "");
    if (!text)
        return;
    pc_ui_shell_widget_mapping(app, identity, nk_widget_bounds(ctx));
    nk_flags flags = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, (char *)text->text.data,
                                                    (int)text->text.cap, nk_filter_default);
    text->active = (flags & NK_EDIT_ACTIVE) != 0;
    text->text.len = strlen(pc_buf_cstr(&text->text));
    if ((!value || strcmp(value, pc_buf_cstr(&text->text))) &&
        !put(dialog->fields, o, key, yyjson_mut_strcpy(dialog->fields, pc_buf_cstr(&text->text))))
        (void)snprintf(r->error, sizeof r->error, "Cannot allocate variable text");
}
static void choice(pc_ui_shell_app *app, pc_ui_dialog *dialog, yyjson_mut_val *o, const char *key,
                   const char *label, const char *const *values, int count)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const char *value = yyjson_mut_get_str(yyjson_mut_obj_get(o, key));
    int selected = 0;
    for (int i = 0; value && i < count; i++)
        if (!strcmp(value, values[i]))
            selected = i;
    nk_layout_row_dynamic(ctx, 26, 2);
    nk_label(ctx, pc_i18n_t(label), NK_TEXT_LEFT);
    if (nk_combo_begin_label(ctx, value ? value : values[selected], nk_vec2(260, 260))) {
        for (int i = 0; i < count; i++)
            if (nk_combo_item_label(ctx, pc_i18n_t(values[i]), NK_TEXT_LEFT))
                (void)put(dialog->fields, o, key, yyjson_mut_strcpy(dialog->fields, values[i]));
        nk_combo_end(ctx);
    }
}
static bool action(pc_ui_shell_app *app, const char *label, const char *id)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_ui_shell_widget_mapping(app, id, nk_widget_bounds(ctx));
    return nk_button_label(ctx, pc_i18n_t(label));
}
static size_t select_entry(pc_ui_shell_app *app, pc_ui_dialog *dialog, yyjson_mut_val *array, const char *key)
{
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    size_t count = yyjson_mut_arr_size(array),
           index = (size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(root, key));
    if (index >= count)
        index = count ? count - 1 : 0;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const char *current = yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_arr_get(array, index), "name"));
    nk_layout_row_dynamic(ctx, 28, 1);
    if (nk_combo_begin_label(ctx, current ? current : pc_i18n_t("Choose an entry"), nk_vec2(400, 240))) {
        for (size_t i = 0; i < count; i++) {
            const char *name = yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_arr_get(array, i), "name"));
            if (nk_combo_item_label(ctx, name ? name : "?", NK_TEXT_LEFT))
                index = i;
        }
        nk_combo_end(ctx);
    }
    (void)put(dialog->fields, root, key, yyjson_mut_uint(dialog->fields, index));
    return index;
}
static void define_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog,
                        yyjson_mut_val *defs)
{
    yyjson_mut_doc *d = dialog->fields;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(d);
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    size_t index = select_entry(app, dialog, defs, "__definition");
    nk_layout_row_dynamic(ctx, 28, 2);
    if (action(app, "New Variable", "variables.newDefinition") && yyjson_mut_arr_size(defs) < 4096) {
        yyjson_mut_val *v = yyjson_mut_obj(d);
        char name[64];
        (void)snprintf(name, sizeof name, "Variable %zu", yyjson_mut_arr_size(defs) + 1);
        if (v && yyjson_mut_obj_add_strcpy(d, v, "name", name) &&
            yyjson_mut_obj_add_uint(d, v, "layer",
                                    state && state->has_active_layer ? state->active_layer : 0) &&
            yyjson_mut_obj_add_str(d, v, "type", "visibility") && yyjson_mut_arr_append(defs, v)) {
            index = yyjson_mut_arr_size(defs) - 1;
            (void)put(d, root, "__definition", yyjson_mut_uint(d, index));
        }
    }
    if (action(app, "Delete Variable", "variables.deleteDefinition")) {
        (void)yyjson_mut_arr_remove(defs, index);
        return;
    }
    yyjson_mut_val *v = yyjson_mut_arr_get(defs, index);
    if (!yyjson_mut_is_obj(v))
        return;
    char identity[80];
    (void)snprintf(identity, sizeof identity, "variables.definition.%zu.name", index);
    text_field(app, r, dialog, v, "name", "Name", identity);
    static const char *const types[] = {"visibility", "textReplacement", "pixelReplacement"};
    choice(app, dialog, v, "type", "Type", types, 3);
    const pc_document *doc = state ? pc_doc_arc_get(state->doc) : NULL;
    const pc_layer *layer =
        doc ? pc_document_layer(doc, yyjson_mut_get_uint(yyjson_mut_obj_get(v, "layer"))) : NULL;
    nk_layout_row_dynamic(ctx, 26, 2);
    nk_label(ctx, pc_i18n_t("Layer"), NK_TEXT_LEFT);
    if (nk_combo_begin_label(ctx, layer ? pc_str_c(&layer->name) : "?", nk_vec2(340, 240))) {
        pc_walk walk = {0};
        if (doc && pc_document_walk(doc, &walk, NULL) == PC_OK)
            for (size_t i = walk.len; i-- > 0;)
                if (nk_combo_item_label(ctx, pc_str_c(&walk.items[i].layer->name), NK_TEXT_LEFT))
                    (void)put(d, v, "layer", yyjson_mut_uint(d, walk.items[i].layer->id));
        pc_walk_fini(&walk);
        nk_combo_end(ctx);
    }
    const char *type = yyjson_mut_get_str(yyjson_mut_obj_get(v, "type"));
    if (type && !strcmp(type, "pixelReplacement")) {
        static const char *const methods[] = {"fit", "fill", "asIs", "conform"};
        static const char *const aligns[] = {"center",     "topLeft",      "topCenter",
                                             "topRight",   "centerLeft",   "centerRight",
                                             "bottomLeft", "bottomCenter", "bottomRight"};
        choice(app, dialog, v, "method", "Method", methods, 4);
        choice(app, dialog, v, "align", "Alignment", aligns, 9);
        nk_layout_row_dynamic(ctx, 26, 1);
        nk_bool clip = yyjson_mut_get_bool(yyjson_mut_obj_get(v, "clip"));
        if (nk_checkbox_label(ctx, pc_i18n_t("Clip to original bounds"), &clip))
            (void)put(d, v, "clip", yyjson_mut_bool(d, clip != 0));
    }
}
static void sets_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog,
                      yyjson_mut_val *defs, yyjson_mut_val *sets)
{
    yyjson_mut_doc *d = dialog->fields;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(d);
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    size_t index = select_entry(app, dialog, sets, "__set");
    nk_layout_row_dynamic(ctx, 28, 3);
    if (action(app, "New Data Set", "variables.newDataSet") && yyjson_mut_arr_size(sets) < 4096) {
        yyjson_mut_val *set = yyjson_mut_obj(d), *values = yyjson_mut_arr(d);
        char name[64];
        (void)snprintf(name, sizeof name, "Data Set %zu", yyjson_mut_arr_size(sets) + 1);
        if (set && values && yyjson_mut_obj_add_strcpy(d, set, "name", name) &&
            yyjson_mut_obj_add_val(d, set, "values", values) && yyjson_mut_arr_append(sets, set)) {
            index = yyjson_mut_arr_size(sets) - 1;
            (void)put(d, root, "__set", yyjson_mut_uint(d, index));
        }
    }
    if (action(app, "Delete Data Set", "variables.deleteDataSet")) {
        (void)yyjson_mut_arr_remove(sets, index);
        return;
    }
    if (action(app, "Apply on OK", "variables.apply")) {
        yyjson_mut_val *name = yyjson_mut_obj_get(yyjson_mut_arr_get(sets, index), "name");
        if (yyjson_mut_is_str(name))
            (void)put(d, root, "apply", yyjson_mut_val_mut_copy(d, name));
    }
    yyjson_mut_val *set = yyjson_mut_arr_get(sets, index);
    if (!yyjson_mut_is_obj(set))
        return;
    char identity[100];
    (void)snprintf(identity, sizeof identity, "variables.set.%zu.name", index);
    text_field(app, r, dialog, set, "name", "Name", identity);
    yyjson_mut_val *values = yyjson_mut_obj_get(set, "values");
    if (!yyjson_mut_is_arr(values)) {
        nk_layout_row_dynamic(ctx, 28, 1);
        nk_label(ctx, pc_i18n_t("Invalid data-set values"), NK_TEXT_LEFT);
        return;
    }
    for (size_t i = 0; i < yyjson_mut_arr_size(defs); i++) {
        yyjson_mut_val *def = yyjson_mut_arr_get(defs, i);
        const char *name = yyjson_mut_get_str(yyjson_mut_obj_get(def, "name"));
        const char *type = yyjson_mut_get_str(yyjson_mut_obj_get(def, "type"));
        if (!name || !type)
            continue;
        yyjson_mut_val *entry = NULL;
        for (size_t j = 0; j < yyjson_mut_arr_size(values); j++) {
            yyjson_mut_val *v = yyjson_mut_arr_get(values, j);
            const char *variable = yyjson_mut_get_str(yyjson_mut_obj_get(v, "variable"));
            if (variable && !strcmp(variable, name)) {
                entry = v;
                break;
            }
        }
        nk_layout_row_dynamic(ctx, 26, 2);
        nk_label(ctx, name, NK_TEXT_LEFT);
        if (!entry) {
            (void)snprintf(identity, sizeof identity, "variables.addValue.%zu", i);
            pc_ui_shell_widget_mapping(app, identity, nk_widget_bounds(ctx));
            if (nk_button_label(ctx, pc_i18n_t("Add value"))) {
                entry = yyjson_mut_obj(d);
                bool visible = !strcmp(type, "visibility");
                const char *kind = visible                            ? "visibility"
                                   : !strcmp(type, "textReplacement") ? "text"
                                                                      : "pixels";
                if (entry && yyjson_mut_obj_add_strcpy(d, entry, "variable", name) &&
                    yyjson_mut_obj_add_str(d, entry, "kind", kind) &&
                    yyjson_mut_obj_add_val(d, entry, "value",
                                           visible ? yyjson_mut_bool(d, true) : yyjson_mut_str(d, "")))
                    (void)yyjson_mut_arr_append(values, entry);
            }
            continue;
        }
        if (nk_button_label(ctx, pc_i18n_t("Remove value"))) {
            for (size_t j = 0; j < yyjson_mut_arr_size(values); j++)
                if (yyjson_mut_arr_get(values, j) == entry) {
                    (void)yyjson_mut_arr_remove(values, j);
                    break;
                }
            continue;
        }
        const char *kind = yyjson_mut_get_str(yyjson_mut_obj_get(entry, "kind"));
        if (kind && !strcmp(kind, "visibility")) {
            nk_layout_row_dynamic(ctx, 26, 1);
            (void)snprintf(identity, sizeof identity, "variables.visible.%zu", i);
            pc_ui_shell_widget_mapping(app, identity, nk_widget_bounds(ctx));
            nk_bool shown = yyjson_mut_get_bool(yyjson_mut_obj_get(entry, "value"));
            if (nk_checkbox_label(ctx, pc_i18n_t("Visible"), &shown))
                (void)put(d, entry, "value", yyjson_mut_bool(d, shown != 0));
        } else {
            (void)snprintf(identity, sizeof identity, "variables.set.%zu.value.%zu", index, i);
            text_field(app, r, dialog, entry, "value",
                       kind && !strcmp(kind, "pixels") ? "Image path" : "Text", identity);
        }
    }
}
void pc_ui_shell_variables_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog)
{
    yyjson_mut_doc *d = dialog->fields;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(d), *defs = yyjson_mut_obj_get(root, "defs"),
                   *sets = yyjson_mut_obj_get(root, "dataSets");
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    if (!yyjson_mut_is_arr(defs) || !yyjson_mut_is_arr(sets) || yyjson_mut_arr_size(defs) > 4096 ||
        yyjson_mut_arr_size(sets) > 4096) {
        nk_layout_row_dynamic(ctx, 40, 1);
        nk_label_wrap(
            ctx,
            pc_i18n_t("Invalid Variables draft. Correct the definitions and data sets before confirming."));
        return;
    }
    size_t page = (size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(root, "__page"));
    nk_layout_row_dynamic(ctx, 28, 2);
    if (action(app, "Define", "variables.define"))
        page = 0;
    if (action(app, "Data Sets", "variables.dataSets"))
        page = 1;
    (void)put(d, root, "__page", yyjson_mut_uint(d, page));
    if (page == 0)
        define_body(app, r, dialog, defs);
    else
        sets_body(app, r, dialog, defs, sets);
    const char *apply = yyjson_mut_get_str(yyjson_mut_obj_get(root, "apply"));
    if (apply) {
        nk_layout_row_dynamic(ctx, 26, 2);
        nk_label(ctx, apply, NK_TEXT_LEFT);
        if (action(app, "Do not apply", "variables.clearApply"))
            (void)yyjson_mut_obj_remove_str(root, "apply");
    }
}
