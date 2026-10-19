#include "workspace_ui.h"
#include "shell_internal.h"
#include "ui/workspace.h"
#include "ui/menus.h"
#include "ui/i18n.h"
#include "engine/prefs.h"
#include "base/utf8.h"
#include "base/mem.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
bool pc_ui_shell_workspace_handles(const char *id)
{
    return id &&
           (!strcmp(id, "window.workspace.newWorkspace") || !strcmp(id, "window.workspace.deleteWorkspace") ||
            !strcmp(id, "window.workspace.lockWorkspace") || !strcmp(id, "window.workspace.select") ||
            !strncmp(id, "window.workspace.saved.", 23) || pc_ui_workspace_name(id, NULL));
}
static pc_status result(yyjson_mut_val *root, pc_json_value *out, pc_error *err)
{
    return pc_json_value_from_mut(root, out, err);
}
static pc_status preferences(pc_ui_shell_app *app, yyjson_mut_doc *d, yyjson_mut_val *values, pc_error *err)
{
    pc_buf json = PC_BUF_INIT;
    pc_json_value out = PC_JSON_VALUE_INIT;
    yyjson_mut_val *root = yyjson_mut_obj(d);
    pc_status status = root && yyjson_mut_obj_add_val(d, root, "values", values) &&
                               yyjson_mut_obj_add_bool(d, root, "replace", true)
                           ? pc_json_write(root, 0, &json, err)
                           : pc_fail(err, PC_ERR_NOMEM, "workspace preferences");
    if (status == PC_OK)
        status = pc_engine_session_execute_json(pc_ui_shell_app_session(app), "prefs.set", pc_buf_cstr(&json),
                                                &out, err);
    pc_buf_fini(&json);
    pc_json_value_fini(&out);
    if (status == PC_OK)
        pc_ui_shell_apply_preferences(app);
    return status;
}
static pc_status dialog(pc_ui_shell_app *app, const char *command, pc_json_value *out, pc_error *err)
{
    pc_json_value saved = PC_JSON_VALUE_INIT;
    pc_status status = pc_engine_prefs_get(pc_ui_shell_app_session(app), "workspaces", &saved, err);
    if (status != PC_OK)
        return status;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = d ? yyjson_mut_obj(d) : NULL;
    bool create = !strcmp(command, "window.workspace.newWorkspace");
    bool ok =
        root && yyjson_mut_obj_add_strcpy(d, root, "__command", command) &&
        yyjson_mut_obj_add_bool(d, root, "__workspace", true) &&
        yyjson_mut_obj_add_str(d, root, "__label", create ? "New Workspace" : "Delete Workspace") &&
        yyjson_mut_obj_add_str(d, root, "name", "") &&
        yyjson_mut_obj_add_bool(d, root, "keyboardShortcuts", false) &&
        yyjson_mut_obj_add_bool(d, root, "menus", false) &&
        yyjson_mut_obj_add_bool(d, root, "toolbar", false) &&
        yyjson_mut_obj_add_val(d, root, "__saved", yyjson_mut_val_mut_copy(d, pc_json_value_root(&saved)));
    pc_json_value_fini(&saved);
    if (!ok) {
        yyjson_mut_doc_free(d);
        return pc_fail(err, PC_ERR_NOMEM, "workspace dialog");
    }
    return pc_ui_shell_dialog_store(app, PC_DIALOG_COMMAND, d, root, out, err);
}
pc_status pc_ui_shell_workspace_command(pc_ui_shell_app *app, const char *id, yyjson_val *p,
                                        pc_json_value *out, pc_error *err)
{
    if (!pc_ui_shell_workspace_handles(id))
        return PC_ERR_NOT_FOUND;
    bool create = !strcmp(id, "window.workspace.newWorkspace"),
         remove = !strcmp(id, "window.workspace.deleteWorkspace"),
         lock = !strcmp(id, "window.workspace.lockWorkspace");
    if ((create || remove) && (!p || (yyjson_is_obj(p) && !yyjson_obj_size(p))))
        return dialog(app, id, out, err);
    if (p && !yyjson_is_obj(p))
        return pc_fail(err, PC_ERR_INVALID, "workspace parameters must be an object");
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_json_value prefs = PC_JSON_VALUE_INIT;
    pc_status status = pc_engine_prefs_get(pc_ui_shell_app_session(app),
                                           lock ? "workspaceLocked" : "workspaces", &prefs, err);
    if (status != PC_OK)
        return status;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = d ? yyjson_mut_obj(d) : NULL, *values = d ? yyjson_mut_obj(d) : NULL,
                   *workspaces = d && !lock ? yyjson_mut_val_mut_copy(d, pc_json_value_root(&prefs)) : NULL;
    if (!root || !values || (!lock && !yyjson_mut_is_obj(workspaces)))
        status = pc_fail(err, PC_ERR_NOMEM, "workspaces");
    if (status == PC_OK && lock) {
        yyjson_val *value = pc_json_get(p, "on");
        if (value && !yyjson_is_bool(value))
            status = pc_fail(err, PC_ERR_INVALID, "workspace lock needs a boolean");
        bool on = value ? yyjson_get_bool(value) : !yyjson_mut_get_bool(pc_json_value_root(&prefs));
        if (status == PC_OK && (!yyjson_mut_obj_add_bool(d, values, "workspaceLocked", on) ||
                                !yyjson_mut_obj_add_bool(d, root, "locked", on)))
            status = pc_fail(err, PC_ERR_NOMEM, "workspace lock");
        if (status == PC_OK)
            status = preferences(app, d, values, err);
    } else if (status == PC_OK) {
        const char *builtin = NULL, *name = NULL;
        yyjson_val *nv = pc_json_get(p, "name");
        if (pc_ui_workspace_name(id, &builtin))
            name = !strcmp(id, "window.workspace.resetWorkspace") ? ui->workspace
                   : builtin && *builtin                          ? builtin
                                                                  : "Essentials";
        else if (!strncmp(id, "window.workspace.saved.", 23))
            name = id + 23;
        else
            name = yyjson_get_str(nv);
        size_t start = 0, length = name ? pc_utf8_trim(name, strlen(name), &start) : 0;
        char *owned = length && length <= 128 ? pc_strndup(name + start, length) : NULL;
        if (!owned || (nv && strlen(name) != yyjson_get_len(nv)))
            status = pc_fail(err, PC_ERR_INVALID, "a workspace needs a name of 1 to 128 bytes");
        yyjson_mut_val *stored = owned ? yyjson_mut_obj_get(workspaces, owned) : NULL;
        if (status == PC_OK && create) {
            if (pc_ui_workspace_builtin(owned))
                status = pc_fail(err, PC_ERR_INVALID, "a built-in workspace cannot be replaced");
            else if (!stored && yyjson_mut_obj_size(workspaces) >= 128)
                status = pc_fail(err, PC_ERR_LIMIT, "at most 128 saved workspaces");
            yyjson_mut_val *snapshot = NULL;
            if (status == PC_OK)
                status = pc_ui_workspace_snapshot(ui, d, &snapshot, err);
            const char *options[] = {"keyboardShortcuts", "menus", "toolbar"},
                       *keys[] = {"shortcuts", "menus", "toolbar"};
            for (size_t i = 0; status == PC_OK && i < 3; i++) {
                yyjson_val *option = pc_json_get(p, options[i]);
                if (option && !yyjson_is_bool(option)) {
                    status = pc_fail(err, PC_ERR_INVALID, "workspace options need booleans");
                    break;
                }
                if (!yyjson_is_true(option))
                    continue;
                pc_json_value value = PC_JSON_VALUE_INIT;
                status = pc_engine_prefs_get(pc_ui_shell_app_session(app), keys[i], &value, err);
                if (status == PC_OK &&
                    !yyjson_mut_obj_add_val(d, snapshot, keys[i],
                                            yyjson_mut_val_mut_copy(d, pc_json_value_root(&value))))
                    status = pc_fail(err, PC_ERR_NOMEM, "workspace options");
                pc_json_value_fini(&value);
            }
            if (status == PC_OK) {
                yyjson_mut_val *k = yyjson_mut_strcpy(d, owned);
                if (!k || !yyjson_mut_obj_put(workspaces, k, snapshot) ||
                    !yyjson_mut_obj_add_val(d, values, "workspaces", workspaces))
                    status = pc_fail(err, PC_ERR_NOMEM, "workspace snapshot");
                if (status == PC_OK)
                    status = preferences(app, d, values, err);
                if (status == PC_OK) {
                    free(ui->workspace);
                    ui->workspace = owned;
                    owned = NULL;
                }
                (void)yyjson_mut_obj_add_bool(d, root, "replaced", stored != NULL);
            }
        } else if (status == PC_OK && remove) {
            if (!stored)
                status = pc_fail(err, PC_ERR_NOT_FOUND, "no saved workspace with this name");
            else if (ui->workspace && !strcmp(ui->workspace, owned))
                status = pc_fail(err, PC_ERR_INVALID, "switch workspaces before deleting the current one");
            else {
                (void)yyjson_mut_obj_remove_str(workspaces, owned);
                if (!yyjson_mut_obj_add_val(d, values, "workspaces", workspaces))
                    status = pc_fail(err, PC_ERR_NOMEM, "workspace delete");
                else
                    status = preferences(app, d, values, err);
            }
        } else if (status == PC_OK) {
            yyjson_doc *read = stored ? yyjson_mut_val_imut_copy(stored, NULL) : NULL;
            if (stored && !read)
                status = pc_fail(err, PC_ERR_NOMEM, "workspace layout");
            if (status == PC_OK && !stored && !pc_ui_workspace_builtin(owned))
                status = pc_fail(err, PC_ERR_NOT_FOUND, "no saved workspace with this name");
            pc_ui_state prepared;
            pc_ui_state_init(&prepared);
            prepared.panels = ui->panels;
            prepared.dock_width = ui->dock_width;
            if (status == PC_OK)
                status =
                    pc_ui_workspace_apply(&prepared, owned, read ? yyjson_doc_get_root(read) : NULL, err);
            if (status == PC_OK && stored) {
                const char *keys[] = {"shortcuts", "menus", "toolbar"};
                for (size_t i = 0; i < 3; i++) {
                    yyjson_mut_val *v = yyjson_mut_obj_get(stored, keys[i]);
                    if (v && !yyjson_mut_obj_add_val(d, values, keys[i], yyjson_mut_val_mut_copy(d, v)))
                        status = pc_fail(err, PC_ERR_NOMEM, "workspace options");
                }
                if (status == PC_OK && yyjson_mut_obj_size(values))
                    status = preferences(app, d, values, err);
            }
            if (status == PC_OK)
                pc_ui_workspace_take(ui, &prepared);
            pc_ui_state_fini(&prepared);
            yyjson_doc_free(read);
        }
        free(owned);
        if (status == PC_OK)
            (void)yyjson_mut_obj_add_strcpy(d, root, "workspace", ui->workspace);
    }
    if (status == PC_OK)
        status = result(root, out, err);
    yyjson_mut_doc_free(d);
    pc_json_value_fini(&prefs);
    return status;
}
void pc_ui_shell_workspace_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    const char *command = yyjson_mut_get_str(yyjson_mut_obj_get(root, "__command")),
               *name = yyjson_mut_get_str(yyjson_mut_obj_get(root, "name"));
    bool create = command && !strcmp(command, "window.workspace.newWorkspace");
    nk_layout_row_dynamic(ctx, 28, 2);
    nk_label(ctx, pc_i18n_t("Name"), NK_TEXT_LEFT);
    if (create) {
        pc_ui_dialog_text *text = pc_ui_dialog_text_buffer(r, dialog->id, "name", name ? name : "");
        if (text) {
            pc_ui_shell_widget_mapping(app, "workspace.name", nk_widget_bounds(ctx));
            nk_flags flags = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, (char *)text->text.data,
                                                            (int)text->text.cap, nk_filter_default);
            text->active = (flags & NK_EDIT_ACTIVE) != 0;
            text->text.len = strlen(pc_buf_cstr(&text->text));
            if (!name || strcmp(name, pc_buf_cstr(&text->text))) {
                yyjson_mut_val *key = yyjson_mut_str(dialog->fields, "name"),
                               *value = yyjson_mut_strcpy(dialog->fields, pc_buf_cstr(&text->text));
                if (key && value)
                    (void)yyjson_mut_obj_put(root, key, value);
            }
        }
    } else {
        yyjson_mut_val *saved = yyjson_mut_obj_get(root, "__saved");
        const char *names[128];
        size_t count = 0;
        int selected = 0;
        yyjson_mut_obj_iter iter = yyjson_mut_obj_iter_with(saved);
        yyjson_mut_val *key;
        while (count < 128 && (key = yyjson_mut_obj_iter_next(&iter))) {
            const char *n = yyjson_mut_get_str(key);
            if (n && strcmp(n, pc_ui_shell_app_state(app)->workspace)) {
                if (name && !strcmp(name, n))
                    selected = (int)count;
                names[count++] = n;
            }
        }
        if (count) {
            int next = nk_combo(ctx, names, (int)count, selected, 24, nk_vec2(260, 240));
            if (next >= 0 && (size_t)next < count && (!name || strcmp(name, names[next]))) {
                yyjson_mut_val *k = yyjson_mut_str(dialog->fields, "name"),
                               *v = yyjson_mut_strcpy(dialog->fields, names[next]);
                if (k && v)
                    (void)yyjson_mut_obj_put(root, k, v);
            }
        } else
            nk_label(ctx, pc_i18n_t("No saved workspaces"), NK_TEXT_LEFT);
    }
    if (create) {
        const char *keys[] = {"keyboardShortcuts", "menus", "toolbar"},
                   *labels[] = {"Keyboard Shortcuts", "Menus", "Toolbar"};
        for (size_t i = 0; i < 3; i++) {
            nk_layout_row_dynamic(ctx, 26, 1);
            nk_bool value = yyjson_mut_get_bool(yyjson_mut_obj_get(root, keys[i]));
            if (nk_checkbox_label(ctx, pc_i18n_t(labels[i]), &value)) {
                yyjson_mut_val *k = yyjson_mut_strcpy(dialog->fields, keys[i]),
                               *v = yyjson_mut_bool(dialog->fields, value != 0);
                if (k && v)
                    (void)yyjson_mut_obj_put(root, k, v);
            }
        }
    }
}
