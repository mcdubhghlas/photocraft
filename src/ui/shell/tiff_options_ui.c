#include "tiff_options.h"
#include "shell_internal.h"
#include "engine/prefs.h"
#include "ui/file_paths.h"
#include "ui/i18n.h"
#include "io/tiff_layers.h"
#include <stdio.h>
#include <string.h>

static const char *const PREF_ASK = "fileHandling.askBeforeSavingLayeredTiff";

bool pc_ui_shell_is_tiff_options(yyjson_mut_val *fields)
{
    return yyjson_mut_get_bool(yyjson_mut_obj_get(fields, "__tiffOptions"));
}

bool pc_ui_shell_tiff_options_wants(pc_ui_shell_app *app, const char *path)
{
    if (!path || !pc_ui_is_tiff_path(path)) {
        return false;
    }
    pc_engine_session *session = pc_ui_shell_app_session(app);
    bool ask = true;
    pc_json_value pref = PC_JSON_VALUE_INIT;
    if (pc_engine_prefs_get(session, PREF_ASK, &pref, NULL) == PC_OK) {
        yyjson_mut_val *v = pc_json_value_root(&pref);
        if (yyjson_mut_is_bool(v)) {
            ask = yyjson_mut_get_bool(v);
        }
    }
    pc_json_value_fini(&pref);
    if (!ask) {
        return false;
    }
    const pc_engine_doc_state *st = pc_engine_session_active(session);
    return st && pc_io_would_write_layers(pc_doc_arc_get(st->doc));
}

pc_status pc_ui_shell_tiff_options_open(pc_ui_shell_app *app, const char *path, pc_json_value *out,
                                        pc_error *err)
{
    pc_engine_session *session = pc_ui_shell_app_session(app);
    const pc_engine_doc_state *st = pc_engine_session_active(session);
    if (!st) {
        return pc_fail(err, PC_ERR_INVALID, "no document open");
    }
    if (!path || !*path) {
        return pc_fail(err, PC_ERR_INVALID, "missing save path");
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    bool ok = root && yyjson_mut_obj_add_bool(doc, root, "__tiffOptions", true) &&
              yyjson_mut_obj_add_str(doc, root, "__command", "file.saveAs") &&
              yyjson_mut_obj_add_str(doc, root, "__label", "TIFF Options") &&
              yyjson_mut_obj_add_strcpy(doc, root, "path", path) &&
              yyjson_mut_obj_add_uint(doc, root, "__document", pc_doc_arc_get(st->doc)->id) &&
              yyjson_mut_obj_add_bool(doc, root, "discard", false) &&
              yyjson_mut_obj_add_bool(doc, root, "dontAsk", false);
    if (!ok) {
        yyjson_mut_doc_free(doc);
        return pc_fail(err, PC_ERR_NOMEM, "TIFF Options");
    }
    return pc_ui_shell_dialog_store(app, PC_DIALOG_COMMAND, doc, root, out, err);
}

void pc_ui_shell_tiff_options_body(pc_ui_shell_app *app, pc_ui_dialog *dialog)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    nk_layout_row_dynamic(ctx, 24, 1);
    nk_label_wrap(ctx, pc_i18n_t("Including layers will increase file size."));
    nk_bool discard = yyjson_mut_get_bool(yyjson_mut_obj_get(root, "discard"));
    nk_bool dont = yyjson_mut_get_bool(yyjson_mut_obj_get(root, "dontAsk"));
    nk_layout_row_dynamic(ctx, 26, 1);
    if (nk_checkbox_label(ctx, pc_i18n_t("Discard Layers and Save a Copy"), &discard)) {
        yyjson_mut_val *k = yyjson_mut_strcpy(dialog->fields, "discard");
        if (k) {
            (void)yyjson_mut_obj_put(root, k, yyjson_mut_bool(dialog->fields, discard != 0));
        }
    }
    nk_layout_row_dynamic(ctx, 26, 1);
    if (nk_checkbox_label(ctx, pc_i18n_t("Don't show again"), &dont)) {
        yyjson_mut_val *k = yyjson_mut_strcpy(dialog->fields, "dontAsk");
        if (k) {
            (void)yyjson_mut_obj_put(root, k, yyjson_mut_bool(dialog->fields, dont != 0));
        }
    }
}

/* Make `document` (the doc that requested the save) active again, in case tabs changed. */
static pc_status activate_target(pc_engine_session *session, pc_doc_id document, pc_error *err)
{
    size_t count = 0, index = 0;
    const pc_engine_doc_state *docs = pc_engine_session_documents(session, &count);
    while (index < count && pc_doc_arc_get(docs[index].doc)->id != document) {
        index++;
    }
    if (index == count) {
        return pc_fail(err, PC_ERR_NOT_FOUND, "the document for this save was closed");
    }
    return pc_engine_session_set_active(session, index);
}

pc_status pc_ui_shell_tiff_options_confirm(pc_ui_shell_app *app, uint64_t id, pc_json_value *out,
                                           pc_error *err)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_dialog *dialog = pc_ui_state_dialog_mut(ui, id);
    yyjson_mut_val *fields = dialog ? yyjson_mut_doc_get_root(dialog->fields) : NULL;
    if (!dialog || !pc_ui_shell_is_tiff_options(fields)) {
        return pc_fail(err, PC_ERR_INVALID, "not a TIFF Options dialog");
    }
    const char *path = yyjson_mut_get_str(yyjson_mut_obj_get(fields, "path"));
    if (!path || !*path) {
        return pc_fail(err, PC_ERR_INVALID, "missing save path");
    }
    bool discard = yyjson_mut_get_bool(yyjson_mut_obj_get(fields, "discard"));
    bool dont = yyjson_mut_get_bool(yyjson_mut_obj_get(fields, "dontAsk"));
    pc_doc_id document = yyjson_mut_get_uint(yyjson_mut_obj_get(fields, "__document"));
    pc_engine_session *session = pc_ui_shell_app_session(app);
    pc_status status = activate_target(session, document, err);
    if (status != PC_OK) {
        return status;
    }
    /* file.saveAs {path, tiffLayers: !discard}. */
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *params = d ? yyjson_mut_obj(d) : NULL;
    pc_buf text = PC_BUF_INIT;
    yyjson_doc *read = NULL;
    status = (params && yyjson_mut_obj_add_strcpy(d, params, "path", path) &&
              yyjson_mut_obj_add_bool(d, params, "tiffLayers", !discard))
                 ? PC_OK
                 : pc_fail(err, PC_ERR_NOMEM, "TIFF save");
    if (status == PC_OK) {
        status = pc_json_write(params, 0, &text, err);
    }
    if (status == PC_OK) {
        status = pc_json_parse(pc_buf_cstr(&text), text.len, &read, err);
    }
    if (status == PC_OK) {
        status = pc_ui_shell_file_execute(app, "file.saveAs", yyjson_doc_get_root(read), out, err);
    }
    yyjson_doc_free(read);
    pc_buf_fini(&text);
    yyjson_mut_doc_free(d);
    if (status != PC_OK) {
        return status;
    }
    if (dont) {
        yyjson_mut_doc *pd = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *pp = pd ? yyjson_mut_obj(pd) : NULL;
        pc_buf ptext = PC_BUF_INIT;
        yyjson_doc *pread = NULL;
        pc_json_value pout = PC_JSON_VALUE_INIT;
        if (pp && yyjson_mut_obj_add_str(pd, pp, "path", PREF_ASK) &&
            yyjson_mut_obj_add_bool(pd, pp, "value", false) && pc_json_write(pp, 0, &ptext, NULL) == PC_OK &&
            pc_json_parse(pc_buf_cstr(&ptext), ptext.len, &pread, NULL) == PC_OK) {
            (void)pc_engine_session_execute(session, "prefs.set", yyjson_doc_get_root(pread), &pout, NULL);
        }
        pc_json_value_fini(&pout);
        yyjson_doc_free(pread);
        pc_buf_fini(&ptext);
        yyjson_mut_doc_free(pd);
    }
    pc_ui_state_close_dialog(ui, id);
    return pc_ui_shell_app_sync_views(app, err);
}
