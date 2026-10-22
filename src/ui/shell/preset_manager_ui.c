#include "preset_manager_ui.h"

#include "shell_internal.h"
#include "ui/preset_manager.h"
#include "ui/state.h"
#include "ui/i18n.h"
#include "base/buf.h"
#include "base/file.h"
#include "base/json.h"
#include "photocraft_nuklear.h"

#include <stdio.h>
#include <string.h>

/* A preset file can carry every brush bitmap, so allow a generous (but bounded) import. */
#define PRESETS_IO_MAX ((size_t)256 * 1024 * 1024)

static bool put(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key, yyjson_mut_val *value)
{
    yyjson_mut_val *k = yyjson_mut_strcpy(doc, key);
    return k && value && yyjson_mut_obj_put(obj, k, value);
}

/* Run an engine command from an owned params value; the reply is written to *reply (owned) when
 * non-NULL, else discarded. */
static pc_status run_cmd(pc_ui_shell_app *app, const char *cmd, yyjson_mut_val *params,
                         pc_json_value *reply, pc_error *err)
{
    pc_buf text = PC_BUF_INIT;
    yyjson_doc *read = NULL;
    pc_json_value local = PC_JSON_VALUE_INIT;
    pc_json_value *sink = reply ? reply : &local;
    pc_status rc = pc_json_write(params, 0, &text, err);
    if (rc == PC_OK) {
        rc = pc_json_parse(pc_buf_cstr(&text), text.len, &read, err);
    }
    if (rc == PC_OK) {
        rc = pc_engine_session_execute(pc_ui_shell_app_session(app), cmd, yyjson_doc_get_root(read), sink,
                                       err);
    }
    if (sink == &local) {
        pc_json_value_fini(&local);
    }
    yyjson_doc_free(read);
    pc_buf_fini(&text);
    return rc;
}

/* =============================================================== Preset Manager */

pc_status pc_ui_shell_preset_manager_open(pc_ui_shell_app *app, pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = d ? yyjson_mut_obj(d) : NULL;
    if (!root) {
        yyjson_mut_doc_free(d);
        return pc_fail(err, PC_ERR_NOMEM, "preset manager");
    }
    bool ok = yyjson_mut_obj_add_str(d, root, "__presetui", "manager") &&
              yyjson_mut_obj_add_str(d, root, "__label", "Preset Manager") &&
              yyjson_mut_obj_add_str(d, root, "kind", "brushes") &&
              yyjson_mut_obj_add_uint(d, root, "selected", 0) &&
              yyjson_mut_obj_add_str(d, root, "newName", "");
    if (!ok) {
        yyjson_mut_doc_free(d);
        return pc_fail(err, PC_ERR_NOMEM, "preset manager");
    }
    return pc_ui_shell_dialog_store(app, PC_DIALOG_COMMAND, d, root, out, err);
}

bool pc_ui_shell_preset_manager_owns(yyjson_mut_val *fields)
{
    const char *kind = fields ? yyjson_mut_get_str(yyjson_mut_obj_get(fields, "__presetui")) : NULL;
    return kind && !strcmp(kind, "manager");
}

/* Dispatch a presetManager mutation (rename/delete/move); the reply is discarded (the body re-lists
 * next frame). */
static void manager_action(pc_ui_shell_app *app, const char *action, const char *kind, uint64_t index,
                           const char *new_name, bool has_to, uint64_t to)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *params = d ? yyjson_mut_obj(d) : NULL;
    if (!params) {
        yyjson_mut_doc_free(d);
        return;
    }
    bool ok = yyjson_mut_obj_add_strcpy(d, params, "action", action) &&
              yyjson_mut_obj_add_strcpy(d, params, "kind", kind) &&
              yyjson_mut_obj_add_uint(d, params, "index", index);
    if (ok && new_name) {
        ok = yyjson_mut_obj_add_strcpy(d, params, "newName", new_name);
    }
    if (ok && has_to) {
        ok = yyjson_mut_obj_add_uint(d, params, "to", to);
    }
    pc_error e = {0};
    if (!ok || run_cmd(app, "edit.presets.presetManager", params, NULL, &e) != PC_OK) {
        pc_ui_shell_report(app, "Preset Manager", ok ? e.msg : "out of memory", true);
    }
    yyjson_mut_doc_free(d);
}

void pc_ui_shell_preset_manager_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime,
                                     pc_ui_dialog *dialog)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_doc *doc = dialog->fields;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    const char *kind = yyjson_mut_get_str(yyjson_mut_obj_get(root, "kind"));
    if (!pc_ui_preset_kind_valid(kind)) {
        kind = "brushes";
        (void)put(doc, root, "kind", yyjson_mut_strcpy(doc, kind));
    }
    const pc_ui_preset_kind *kinds = NULL;
    size_t nkinds = pc_ui_preset_kinds(&kinds);
    /* Kind selector. */
    const char *cur_label = kinds[0].label;
    for (size_t i = 0; i < nkinds; i++) {
        if (!strcmp(kind, kinds[i].id)) {
            cur_label = kinds[i].label;
        }
    }
    nk_layout_row_template_begin(ctx, 26);
    nk_layout_row_template_push_static(ctx, 90);
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_end(ctx);
    nk_label(ctx, pc_i18n_t("Preset Type"), NK_TEXT_LEFT);
    if (nk_combo_begin_label(ctx, pc_i18n_t(cur_label), nk_vec2(220, 160))) {
        nk_layout_row_dynamic(ctx, 24, 1);
        for (size_t i = 0; i < nkinds; i++) {
            if (nk_combo_item_label(ctx, pc_i18n_t(kinds[i].label), NK_TEXT_LEFT)) {
                (void)put(doc, root, "kind", yyjson_mut_strcpy(doc, kinds[i].id));
                (void)put(doc, root, "selected", yyjson_mut_uint(doc, 0));
                (void)put(doc, root, "newName", yyjson_mut_str(doc, ""));
                kind = kinds[i].id;
            }
        }
        nk_combo_end(ctx);
    }
    /* List every library (the `list`-all reply keys each array by a stable kind id) and pick the
     * chosen one; a single-kind query would key the reply by the caller's own string. */
    char **names = NULL;
    size_t n = 0;
    pc_json_value reply = PC_JSON_VALUE_INIT;
    yyjson_mut_doc *pd = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *params = pd ? yyjson_mut_obj(pd) : NULL;
    if (params && run_cmd(app, "edit.presets.presetManager", params, &reply, NULL) == PC_OK) {
        (void)pc_ui_preset_list_parse(pc_json_value_root(&reply), kind, &names, &n, NULL);
    }
    yyjson_mut_doc_free(pd);
    size_t selected = pc_ui_preset_clamp(yyjson_mut_get_uint(yyjson_mut_obj_get(root, "selected")), n);
    float h = fmaxf(120, nk_window_get_content_region(ctx).h - 140);
    nk_layout_row_dynamic(ctx, h, 1);
    if (nk_group_begin(ctx, "preset-list", NK_WINDOW_BORDER)) {
        for (size_t i = 0; i < n; i++) {
            nk_layout_row_dynamic(ctx, 24, 1);
            if (nk_select_label(ctx, names[i], NK_TEXT_LEFT, i == selected)) {
                selected = i;
                (void)put(doc, root, "selected", yyjson_mut_uint(doc, i));
                (void)put(doc, root, "newName", yyjson_mut_strcpy(doc, names[i]));
            }
        }
        if (n == 0) {
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_label(ctx, pc_i18n_t("No presets of this type."), NK_TEXT_LEFT);
        }
        nk_group_end(ctx);
    }
    (void)put(doc, root, "selected", yyjson_mut_uint(doc, selected));
    /* Rename field. */
    const char *stored = yyjson_mut_get_str(yyjson_mut_obj_get(root, "newName"));
    if (!stored) {
        stored = "";
    }
    const char *typed = stored;
    pc_ui_dialog_text *tb = pc_ui_dialog_text_buffer(runtime, dialog->id, "newName", stored);
    nk_layout_row_dynamic(ctx, 26, 1);
    if (tb) {
        nk_flags edited = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, (char *)tb->text.data,
                                                         (int)tb->text.cap, nk_filter_default);
        tb->active = (edited & NK_EDIT_ACTIVE) != 0;
        tb->text.len = strlen(pc_buf_cstr(&tb->text));
        if (strcmp(stored, pc_buf_cstr(&tb->text))) {
            (void)put(doc, root, "newName", yyjson_mut_strncpy(doc, pc_buf_cstr(&tb->text), tb->text.len));
        }
        typed = pc_buf_cstr(&tb->text);
    } else {
        nk_label(ctx, pc_i18n_t("New preset name"), NK_TEXT_LEFT);
    }
    /* Trimmed-empty check for the Rename button. */
    bool name_ok = false;
    for (const char *c = typed; *c; c++) {
        if (*c != ' ' && *c != '\t') {
            name_ok = true;
            break;
        }
    }
    /* Actions. */
    nk_layout_row_dynamic(ctx, 28, 4);
    bool has_sel = selected < n;
    bool can_rename = has_sel && name_ok;
    if (!can_rename) {
        nk_widget_disable_begin(ctx);
    }
    if (nk_button_label(ctx, pc_i18n_t("Rename")) && can_rename) {
        manager_action(app, "rename", kind, selected, typed, false, 0);
    }
    if (!can_rename) {
        nk_widget_disable_end(ctx);
    }
    if (!has_sel) {
        nk_widget_disable_begin(ctx);
    }
    if (nk_button_label(ctx, pc_i18n_t("Delete")) && has_sel) {
        manager_action(app, "delete", kind, selected, NULL, false, 0);
    }
    if (!has_sel) {
        nk_widget_disable_end(ctx);
    }
    bool up = pc_ui_preset_can_move_up(selected, n);
    if (!up) {
        nk_widget_disable_begin(ctx);
    }
    if (nk_button_label(ctx, pc_i18n_t("Move Up")) && up) {
        manager_action(app, "move", kind, selected, NULL, true, selected - 1);
        (void)put(doc, root, "selected", yyjson_mut_uint(doc, selected - 1));
    }
    if (!up) {
        nk_widget_disable_end(ctx);
    }
    bool down = pc_ui_preset_can_move_down(selected, n);
    if (!down) {
        nk_widget_disable_begin(ctx);
    }
    if (nk_button_label(ctx, pc_i18n_t("Move Down")) && down) {
        manager_action(app, "move", kind, selected, NULL, true, selected + 1);
        (void)put(doc, root, "selected", yyjson_mut_uint(doc, selected + 1));
    }
    if (!down) {
        nk_widget_disable_end(ctx);
    }
    pc_ui_preset_names_free(names, n);
    pc_json_value_fini(&reply);
}

/* =============================================================== Export/Import Presets */

pc_status pc_ui_shell_presets_io_open(pc_ui_shell_app *app, pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = d ? yyjson_mut_obj(d) : NULL;
    if (!root) {
        yyjson_mut_doc_free(d);
        return pc_fail(err, PC_ERR_NOMEM, "export/import presets");
    }
    bool ok = yyjson_mut_obj_add_str(d, root, "__presetui", "exportImport") &&
              yyjson_mut_obj_add_str(d, root, "__label", "Export/Import Presets") &&
              yyjson_mut_obj_add_str(d, root, "action", "export") &&
              yyjson_mut_obj_add_bool(d, root, "brushes", true) &&
              yyjson_mut_obj_add_bool(d, root, "customShapes", true);
    if (!ok) {
        yyjson_mut_doc_free(d);
        return pc_fail(err, PC_ERR_NOMEM, "export/import presets");
    }
    return pc_ui_shell_dialog_store(app, PC_DIALOG_COMMAND, d, root, out, err);
}

bool pc_ui_shell_presets_io_owns(yyjson_mut_val *fields)
{
    const char *kind = fields ? yyjson_mut_get_str(yyjson_mut_obj_get(fields, "__presetui")) : NULL;
    return kind && !strcmp(kind, "exportImport");
}

void pc_ui_shell_presets_io_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime,
                                 pc_ui_dialog *dialog)
{
    (void)runtime;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_doc *doc = dialog->fields;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    const char *action = yyjson_mut_get_str(yyjson_mut_obj_get(root, "action"));
    bool importing = action && !strcmp(action, "import");
    /* Direction. */
    nk_layout_row_template_begin(ctx, 26);
    nk_layout_row_template_push_static(ctx, 70);
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_end(ctx);
    nk_label(ctx, pc_i18n_t("Action"), NK_TEXT_LEFT);
    if (nk_combo_begin_label(ctx, pc_i18n_t(importing ? "Import Presets" : "Export Presets"),
                             nk_vec2(220, 90))) {
        nk_layout_row_dynamic(ctx, 24, 1);
        if (nk_combo_item_label(ctx, pc_i18n_t("Export Presets"), NK_TEXT_LEFT)) {
            (void)put(doc, root, "action", yyjson_mut_str(doc, "export"));
        }
        if (nk_combo_item_label(ctx, pc_i18n_t("Import Presets"), NK_TEXT_LEFT)) {
            (void)put(doc, root, "action", yyjson_mut_str(doc, "import"));
        }
        nk_combo_end(ctx);
    }
    /* Which libraries. */
    static const struct {
        const char *key;
        const char *label;
    } rows[] = {{"brushes", "Brushes"}, {"customShapes", "Custom Shapes"}};
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        yyjson_mut_val *v = yyjson_mut_obj_get(root, rows[i].key);
        nk_bool on = v ? yyjson_mut_get_bool(v) : true;
        nk_layout_row_dynamic(ctx, 24, 1);
        if (nk_checkbox_label(ctx, pc_i18n_t(rows[i].label), &on)) {
            (void)put(doc, root, rows[i].key, yyjson_mut_bool(doc, on != 0));
        }
    }
    nk_layout_row_dynamic(ctx, 44, 1);
    nk_label_wrap(ctx, pc_i18n_t(importing
                                     ? "Click OK to choose a preset file to import."
                                     : "Click OK to choose where to save the exported presets."));
}

/* Build the `kinds` array from the checkbox fields (owned by `d`). */
static yyjson_mut_val *io_kinds(yyjson_mut_doc *d, yyjson_mut_val *root)
{
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    if (!arr) {
        return NULL;
    }
    static const char *const keys[] = {"brushes", "customShapes"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        yyjson_mut_val *v = yyjson_mut_obj_get(root, keys[i]);
        bool on = v ? yyjson_mut_get_bool(v) : true;
        if (on) {
            yyjson_mut_arr_append(arr, yyjson_mut_strcpy(d, keys[i]));
        }
    }
    return arr;
}

pc_status pc_ui_shell_presets_io_begin(pc_ui_shell_app *app, pc_ui_dialog *dialog, bool *saving,
                                       pc_error *err)
{
    if (!dialog || !saving) {
        return pc_fail(err, PC_ERR_INVALID, "export/import presets");
    }
    yyjson_mut_doc *doc = dialog->fields;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    const char *action = yyjson_mut_get_str(yyjson_mut_obj_get(root, "action"));
    if (action && !strcmp(action, "import")) {
        *saving = false;
        return PC_OK;
    }
    *saving = true;
    /* Export now; stash the serialised file text for the Save chooser to write. */
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *params = d ? yyjson_mut_obj(d) : NULL;
    yyjson_mut_val *kinds = d ? io_kinds(d, root) : NULL;
    pc_json_value reply = PC_JSON_VALUE_INIT;
    pc_status rc = (params && kinds && yyjson_mut_obj_add_str(d, params, "action", "export") &&
                    yyjson_mut_obj_add_val(d, params, "kinds", kinds))
                       ? PC_OK
                       : pc_fail(err, PC_ERR_NOMEM, "export presets");
    if (rc == PC_OK) {
        rc = run_cmd(app, "edit.presets.exportImportPresets", params, &reply, err);
    }
    pc_buf text = PC_BUF_INIT;
    if (rc == PC_OK) {
        yyjson_mut_val *data = yyjson_mut_obj_get(pc_json_value_root(&reply), "data");
        if (!data) {
            rc = pc_fail(err, PC_ERR_INVALID, "export produced no data");
        } else {
            rc = pc_json_write(data, PC_JSON_PRETTY, &text, err);
        }
    }
    if (rc == PC_OK) {
        if (!put(doc, root, "__exportData", yyjson_mut_strncpy(doc, pc_buf_cstr(&text), text.len))) {
            rc = pc_fail(err, PC_ERR_NOMEM, "export presets");
        }
    }
    pc_buf_fini(&text);
    pc_json_value_fini(&reply);
    yyjson_mut_doc_free(d);
    return rc;
}

pc_status pc_ui_shell_presets_io_write(pc_ui_shell_app *app, pc_ui_dialog *dialog, const char *path,
                                       pc_error *err)
{
    if (!dialog || !path || !*path) {
        return pc_fail(err, PC_ERR_INVALID, "export presets: no path");
    }
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    size_t len = 0;
    const char *text = yyjson_mut_get_str(yyjson_mut_obj_get(root, "__exportData"));
    if (text) {
        len = strlen(text);
    }
    if (!text || len == 0) {
        return pc_fail(err, PC_ERR_INVALID, "no exported data to write");
    }
    pc_status rc = pc_file_write_atomic(path, text, len, err);
    if (rc == PC_OK) {
        char msg[4200];
        (void)snprintf(msg, sizeof msg, "%s %s", pc_i18n_t("Exported presets to"), path);
        pc_ui_shell_report(app, "Export Presets", msg, false);
    }
    return rc;
}

pc_status pc_ui_shell_presets_io_read(pc_ui_shell_app *app, pc_ui_dialog *dialog, const char *path,
                                      pc_error *err)
{
    if (!dialog || !path || !*path) {
        return pc_fail(err, PC_ERR_INVALID, "import presets: no path");
    }
    pc_buf bytes = PC_BUF_INIT;
    pc_status rc = pc_file_read(path, PRESETS_IO_MAX, &bytes, err);
    yyjson_mut_doc *d = rc == PC_OK ? yyjson_mut_doc_new(NULL) : NULL;
    yyjson_mut_val *params = d ? yyjson_mut_obj(d) : NULL;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    yyjson_mut_val *kinds = d ? io_kinds(d, root) : NULL;
    pc_json_value reply = PC_JSON_VALUE_INIT;
    if (rc == PC_OK) {
        bool ok = params && kinds && yyjson_mut_obj_add_str(d, params, "action", "import") &&
                  yyjson_mut_obj_add_val(d, params, "kinds", kinds) &&
                  yyjson_mut_obj_add_strncpy(d, params, "data", pc_buf_cstr(&bytes), bytes.len);
        rc = ok ? run_cmd(app, "edit.presets.exportImportPresets", params, &reply, err)
                : pc_fail(err, PC_ERR_NOMEM, "import presets");
    }
    if (rc == PC_OK) {
        yyjson_mut_val *rr = pc_json_value_root(&reply);
        uint64_t nb = yyjson_mut_get_uint(yyjson_mut_obj_get(rr, "brushes"));
        uint64_t ns = yyjson_mut_get_uint(yyjson_mut_obj_get(rr, "customShapes"));
        char msg[256];
        (void)snprintf(msg, sizeof msg, "%s %llu, %s %llu", pc_i18n_t("Imported brushes:"),
                       (unsigned long long)nb, pc_i18n_t("custom shapes:"), (unsigned long long)ns);
        pc_ui_shell_report(app, "Import Presets", msg, false);
    }
    pc_json_value_fini(&reply);
    yyjson_mut_doc_free(d);
    pc_buf_fini(&bytes);
    return rc;
}
