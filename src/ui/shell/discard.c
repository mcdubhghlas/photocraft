#include "discard.h"
#include "shell_internal.h"
#include "engine/file_ui.h"
#include <stdio.h>
#include <string.h>
static int reach(const char *id)
{
    if (!strcmp(id, "file.close") || !strcmp(id, "file.revert")) {
        return 1;
    }
    if (!strcmp(id, "file.closeOthers")) {
        return 2;
    }
    if (!strcmp(id, "file.closeAll") || !strcmp(id, "file.exit")) {
        return 3;
    }
    return 0;
}
bool pc_ui_shell_is_discard(yyjson_mut_val *fields)
{
    return yyjson_mut_get_bool(yyjson_mut_obj_get(fields, "__discard"));
}
static bool index_of(pc_engine_session *session, pc_doc_id id, size_t *index)
{
    size_t count = 0;
    const pc_engine_doc_state *docs = pc_engine_session_documents(session, &count);
    for (size_t i = 0; i < count; i++) {
        if (pc_doc_arc_get(docs[i].doc)->id == id) {
            *index = i;
            return true;
        }
    }
    return false;
}
static pc_status result(uint64_t id, pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return pc_fail(err, PC_ERR_NOMEM, "discard response");
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, root, "dialog", id);
    pc_status status = pc_json_value_from_mut(root, out, err);
    yyjson_mut_doc_free(doc);
    return status;
}
pc_status pc_ui_shell_discard_intercept(pc_ui_shell_app *app, const char *command, yyjson_val *params,
                                        pc_json_value *out, pc_error *err)
{
    int scope = reach(command);
    if (!scope) {
        return PC_ERR_NOT_FOUND;
    }
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_engine_session *session = pc_ui_shell_app_session(app);
    uint64_t replace = 0;
    for (size_t i = 0; i < ui->n_dialogs; i++) {
        yyjson_mut_val *fields = yyjson_mut_doc_get_root(ui->dialogs[i].fields);
        if (pc_ui_shell_is_discard(fields)) {
            const char *id = yyjson_mut_get_str(yyjson_mut_obj_get(fields, "__request"));
            if (id && !strcmp(command, id)) {
                return result(ui->dialogs[i].id, out, err);
            }
            if (!strcmp(command, "file.exit")) {
                replace = ui->dialogs[i].id;
                break;
            }
            return pc_fail(err, PC_ERR_INVALID, "answer the unsaved-changes prompt first");
        }
    }
    size_t count = 0, target = SIZE_MAX;
    const pc_engine_doc_state *docs = pc_engine_session_documents(session, &count);
    (void)pc_engine_session_active_index(session, &target);
    yyjson_val *requested = pc_json_get(params, "document");
    uint64_t value = 0;
    if (requested) {
        if (!pc_json_as_u64(requested, &value) || value >= count) {
            return pc_fail(err, PC_ERR_INVALID, "invalid document index");
        }
        target = (size_t)value;
    }
    if (count > 4096) {
        return pc_fail(err, PC_ERR_LIMIT, "too many documents for a close prompt");
    }
    size_t dirty = 0;
    for (size_t i = 0; i < count; i++) {
        if ((scope == 3 || (scope == 1 && i == target) || (scope == 2 && i != target)) &&
            pc_engine_doc_state_is_dirty(&docs[i])) {
            dirty++;
        }
    }
    if (!dirty) {
        return PC_ERR_NOT_FOUND;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return pc_fail(err, PC_ERR_NOMEM, "discard prompt");
    }
    yyjson_mut_val *fields = yyjson_mut_obj(doc), *pending = yyjson_mut_arr(doc);
    bool ok =
        fields && pending && yyjson_mut_obj_add_bool(doc, fields, "__discard", true) &&
        yyjson_mut_obj_add_strcpy(doc, fields, "__request", command) &&
        yyjson_mut_obj_add_str(doc, fields, "__label", "Unsaved Changes") &&
        yyjson_mut_obj_add_uint(doc, fields, "__target",
                                target < count ? pc_doc_arc_get(docs[target].doc)->id : 0) &&
        yyjson_mut_obj_add_str(doc, fields, "choice", !strcmp(command, "file.revert") ? "discard" : "save") &&
        yyjson_mut_obj_add_val(doc, fields, "__pending", pending);
    for (size_t i = 0; ok && i < count; i++) {
        if ((scope == 3 || (scope == 1 && i == target) || (scope == 2 && i != target)) &&
            pc_engine_doc_state_is_dirty(&docs[i])) {
            ok = yyjson_mut_arr_add_uint(doc, pending, pc_doc_arc_get(docs[i].doc)->id);
        }
    }
    yyjson_doc *read = ok ? yyjson_mut_val_imut_copy(fields, NULL) : NULL;
    uint64_t id = read ? pc_ui_state_open_dialog(ui, PC_DIALOG_COMMAND, yyjson_doc_get_root(read)) : 0;
    yyjson_doc_free(read);
    yyjson_mut_doc_free(doc);
    if (!id) {
        return pc_fail(err, PC_ERR_NOMEM, "discard prompt");
    }
    if (replace) {
        for (size_t i = ui->n_dialogs; i-- > 0;) {
            yyjson_mut_val *root = yyjson_mut_doc_get_root(ui->dialogs[i].fields);
            if (ui->dialogs[i].id == replace ||
                yyjson_mut_get_uint(yyjson_mut_obj_get(root, "__dialog")) == replace) {
                pc_ui_state_close_dialog(ui, ui->dialogs[i].id);
            }
        }
    }
    pc_ui_shell_modal_changed(app);
    pc_ui_dialog *dialog = pc_ui_state_dialog_mut(ui, id);
    pending = yyjson_mut_obj_get(yyjson_mut_doc_get_root(dialog->fields), "__pending");
    size_t first = 0;
    if (index_of(session, yyjson_mut_get_uint(yyjson_mut_arr_get_first(pending)), &first)) {
        (void)pc_engine_session_set_active(session, first);
    }
    return result(id, out, err);
}
static pc_status advance(pc_ui_shell_app *app, uint64_t id, pc_json_value *out, pc_error *err)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_engine_session *session = pc_ui_shell_app_session(app);
    pc_ui_dialog *dialog = pc_ui_state_dialog_mut(ui, id);
    if (!dialog) {
        return pc_fail(err, PC_ERR_NOT_FOUND, "close prompt is gone");
    }
    yyjson_mut_val *fields = yyjson_mut_doc_get_root(dialog->fields),
                   *pending = yyjson_mut_obj_get(fields, "__pending");
    const char *request = yyjson_mut_get_str(yyjson_mut_obj_get(fields, "__request"));
    if (!request || !reach(request) ||
        (!yyjson_mut_is_arr(pending) || !yyjson_mut_arr_size(pending) ||
         yyjson_mut_arr_size(pending) > 4096)) {
        return pc_fail(err, PC_ERR_INVALID, "invalid close prompt");
    }
    pc_doc_id answered = yyjson_mut_get_uint(yyjson_mut_arr_get_first(pending));
    size_t index = 0;
    /* Multi-document closes commit each answer separately. Cancel preserves the unanswered work. */
    if (reach(request) > 1 && index_of(session, answered, &index)) {
        (void)pc_engine_session_close(session, index);
    }
    if (yyjson_mut_arr_size(pending)) {
        (void)yyjson_mut_arr_remove(pending, 0);
    }
    while (yyjson_mut_arr_size(pending) &&
           !index_of(session, yyjson_mut_get_uint(yyjson_mut_arr_get_first(pending)), &index)) {
        (void)yyjson_mut_arr_remove(pending, 0);
    }
    if (yyjson_mut_arr_size(pending)) {
        (void)pc_engine_session_set_active(session, index);
        yyjson_mut_val *choice = yyjson_mut_obj_get(fields, "choice");
        if (choice) {
            (void)yyjson_mut_set_str(choice, "save");
        }
        return result(id, out, err);
    }
    if (!strcmp(request, "file.exit")) {
        pc_ui_state_close_dialog(ui, id);
        pc_ui_shell_request_exit(app);
        return result(0, out, err);
    }
    pc_doc_id target = yyjson_mut_get_uint(yyjson_mut_obj_get(fields, "__target"));
    bool found = index_of(session, target, &index);
    if (!found && reach(request) != 3) {
        pc_ui_state_close_dialog(ui, id);
        return result(0, out, err);
    }
    size_t remaining = 0;
    (void)pc_engine_session_documents(session, &remaining);
    if (!remaining && reach(request) == 3) {
        pc_ui_state_close_dialog(ui, id);
        return result(0, out, err);
    }
    char params[96];
    if (found) {
        (void)snprintf(params, sizeof params, "{\"document\":%zu}", index);
    } else {
        (void)snprintf(params, sizeof params, "{}");
    }
    if (found && reach(request) == 1) {
        (void)pc_engine_session_set_active(session, index);
    }
    pc_status status = pc_engine_session_execute_json(session, request, params, out, err);
    if (status == PC_OK) {
        pc_ui_state_close_dialog(ui, id);
        status = pc_ui_shell_app_sync_views(app, err);
    }
    return status;
}
pc_status pc_ui_shell_discard_confirm(pc_ui_shell_app *app, uint64_t id, pc_json_value *out, pc_error *err)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_engine_session *session = pc_ui_shell_app_session(app);
    pc_ui_dialog *dialog = pc_ui_state_dialog_mut(ui, id);
    if (!dialog) {
        return pc_fail(err, PC_ERR_NOT_FOUND, "close prompt is gone");
    }
    if (ui->dialogs[ui->n_dialogs - 1].id != id) {
        return pc_fail(err, PC_ERR_INVALID, "answer the save dialog first");
    }
    yyjson_mut_val *fields = yyjson_mut_doc_get_root(dialog->fields);
    const char *choice = yyjson_mut_get_str(yyjson_mut_obj_get(fields, "choice"));
    if (!choice || (strcmp(choice, "save") && strcmp(choice, "discard"))) {
        return pc_fail(err, PC_ERR_INVALID, "choose Save or Don't Save");
    }
    pc_doc_id document =
        yyjson_mut_get_uint(yyjson_mut_arr_get_first(yyjson_mut_obj_get(fields, "__pending")));
    size_t index = 0;
    if (!index_of(session, document, &index) || !strcmp(choice, "discard")) {
        return advance(app, id, out, err);
    }
    (void)pc_engine_session_set_active(session, index);
    const pc_engine_doc_state *active = pc_engine_session_active(session);
    if (!pc_engine_doc_state_is_dirty(active)) {
        return advance(app, id, out, err);
    }
    const char *path = yyjson_mut_get_str(yyjson_mut_obj_get(fields, "path"));
    if (path && *path) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        if (!doc) {
            return pc_fail(err, PC_ERR_NOMEM, "save request");
        }
        yyjson_mut_val *p = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, p, "path", path);
        yyjson_doc *read = yyjson_mut_val_imut_copy(p, NULL);
        pc_status status =
            read ? pc_ui_shell_file_execute(app, "file.saveAs", yyjson_doc_get_root(read), out, err)
                 : pc_fail(err, PC_ERR_NOMEM, "save request");
        yyjson_doc_free(read);
        yyjson_mut_doc_free(doc);
        if (status != PC_OK) {
            return status;
        }
    } else if (!active->path || !pc_engine_file_can_write_back(active->path) || active->source_read_only) {
        yyjson_mut_val *waiting = yyjson_mut_bool(dialog->fields, true);
        if (!waiting || !yyjson_mut_obj_put(fields, yyjson_mut_str(dialog->fields, "__waiting"), waiting)) {
            return pc_fail(err, PC_ERR_NOMEM, "save prompt");
        }
        pc_status status = pc_ui_shell_dialog_file(app, "file.saveAs", out, err);
        if (status == PC_OK && ui->n_dialogs) {
            pc_ui_dialog *child = &ui->dialogs[ui->n_dialogs - 1];
            yyjson_mut_val *root = yyjson_mut_doc_get_root(child->fields);
            (void)yyjson_mut_obj_add_uint(child->fields, root, "__dialog", id);
        }
        return status;
    } else {
        pc_status status = pc_ui_shell_file_execute(app, "file.save", NULL, out, err);
        if (status != PC_OK) {
            return status;
        }
    }
    if (pc_engine_doc_state_is_dirty(pc_engine_session_active(session))) {
        return pc_fail(
            err, PC_ERR_INVALID,
            "save the editable document before closing; a flattened copy keeps its unsaved changes");
    }
    pc_json_value_fini(out);
    return advance(app, id, out, err);
}
void pc_ui_shell_discard_poll(pc_ui_shell_app *app)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (!ui->n_dialogs) {
        return;
    }
    pc_ui_dialog *dialog = &ui->dialogs[ui->n_dialogs - 1];
    yyjson_mut_val *fields = yyjson_mut_doc_get_root(dialog->fields);
    if (!pc_ui_shell_is_discard(fields) || !yyjson_mut_get_bool(yyjson_mut_obj_get(fields, "__waiting"))) {
        return;
    }
    yyjson_mut_val *waiting = yyjson_mut_obj_get(fields, "__waiting");
    (void)yyjson_mut_set_bool(waiting, false);
    pc_doc_id document =
        yyjson_mut_get_uint(yyjson_mut_arr_get_first(yyjson_mut_obj_get(fields, "__pending")));
    size_t index = 0;
    pc_engine_session *session = pc_ui_shell_app_session(app);
    size_t count = 0;
    const pc_engine_doc_state *docs = pc_engine_session_documents(session, &count);
    if (!index_of(session, document, &index) || !pc_engine_doc_state_is_dirty(&docs[index])) {
        pc_json_value out = PC_JSON_VALUE_INIT;
        (void)advance(app, dialog->id, &out, NULL);
        pc_json_value_fini(&out);
    }
}
