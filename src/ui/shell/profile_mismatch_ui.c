#include "profile_mismatch_ui.h"

#include "shell_internal.h"
#include "ui/profile_mismatch.h"
#include "ui/state.h"
#include "ui/i18n.h"
#include "base/buf.h"
#include "base/json.h"
#include "photocraft_nuklear.h"

#include <stdio.h>
#include <string.h>

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

bool pc_ui_shell_profile_mismatch_owns(yyjson_mut_val *fields)
{
    return fields && yyjson_mut_get_bool(yyjson_mut_obj_get(fields, "__mismatch"));
}

pc_status pc_ui_shell_profile_mismatch_maybe_open(pc_ui_shell_app *app, yyjson_mut_val *report,
                                                  pc_error *err)
{
    if (!pc_ui_profile_mismatch_should_open(report)) {
        return PC_OK;
    }
    pc_ui_profile_mismatch model;
    if (!pc_ui_profile_mismatch_parse(report, &model)) {
        return PC_OK;
    }
    /* Compose the prompt message with the profile names filled in. */
    pc_buf message = PC_BUF_INIT;
    const char *keys[] = {"working", "embedded"};
    const char *vals[] = {model.working[0] ? model.working : "?", model.embedded[0] ? model.embedded : "?"};
    const char *template =
        model.missing
            ? pc_i18n_t("The document does not have an embedded colour profile. Working space: {working}.")
            : pc_i18n_t("The document has an embedded colour profile that does not match the working "
                        "space.\nEmbedded: {embedded}\nWorking: {working}");
    pc_status rc = pc_i18n_fmt(template, keys, vals, 2, &message, err);
    yyjson_mut_doc *d = rc == PC_OK ? yyjson_mut_doc_new(NULL) : NULL;
    yyjson_mut_val *root = d ? yyjson_mut_obj(d) : NULL;
    if (rc == PC_OK && !root) {
        rc = pc_fail(err, PC_ERR_NOMEM, "profile mismatch");
    }
    if (rc == PC_OK) {
        bool ok = yyjson_mut_obj_add_bool(d, root, "__mismatch", true) &&
                  yyjson_mut_obj_add_str(d, root, "__label", "Embedded Profile Mismatch") &&
                  yyjson_mut_obj_add_strncpy(d, root, "message", pc_buf_cstr(&message), message.len) &&
                  yyjson_mut_obj_add_strcpy(d, root, "action", model.initial_action) &&
                  yyjson_mut_obj_add_strcpy(d, root, "applied", model.initial_action) &&
                  yyjson_mut_obj_add_bool(d, root, "missing", model.missing);
        if (!ok) {
            rc = pc_fail(err, PC_ERR_NOMEM, "profile mismatch");
        }
    }
    pc_buf_fini(&message);
    if (rc == PC_OK) {
        pc_json_value out = PC_JSON_VALUE_INIT;
        rc = pc_ui_shell_dialog_store(app, PC_DIALOG_COMMAND, d, root, &out, err);
        pc_json_value_fini(&out);
        d = NULL;
    }
    yyjson_mut_doc_free(d);
    return rc;
}

void pc_ui_shell_profile_mismatch_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime,
                                       pc_ui_dialog *dialog)
{
    (void)runtime;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_doc *doc = dialog->fields;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    const char *message = yyjson_mut_get_str(yyjson_mut_obj_get(root, "message"));
    nk_layout_row_dynamic(ctx, 72, 1);
    nk_label_wrap(ctx, message ? message : "");
    bool missing = yyjson_mut_get_bool(yyjson_mut_obj_get(root, "missing"));
    const char *action = yyjson_mut_get_str(yyjson_mut_obj_get(root, "action"));
    if (!action) {
        action = "preserve";
    }
    const pc_ui_pm_option *opts = NULL;
    size_t n = pc_ui_profile_mismatch_options(missing, &opts);
    for (size_t i = 0; i < n; i++) {
        nk_layout_row_dynamic(ctx, 24, 1);
        if (nk_option_label(ctx, pc_i18n_t(opts[i].label), strcmp(action, opts[i].action) == 0)) {
            if (strcmp(action, opts[i].action) != 0) {
                (void)put(doc, root, "action", yyjson_mut_strcpy(doc, opts[i].action));
                action = yyjson_mut_get_str(yyjson_mut_obj_get(root, "action"));
            }
        }
    }
}

pc_status pc_ui_shell_profile_mismatch_confirm(pc_ui_shell_app *app, uint64_t id, pc_json_value *out,
                                               pc_error *err)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_dialog *dialog = pc_ui_state_dialog_mut(ui, id);
    yyjson_mut_val *root = dialog ? yyjson_mut_doc_get_root(dialog->fields) : NULL;
    if (!pc_ui_shell_profile_mismatch_owns(root)) {
        return pc_fail(err, PC_ERR_INVALID, "not the profile-mismatch prompt");
    }
    const char *action = yyjson_mut_get_str(yyjson_mut_obj_get(root, "action"));
    const char *applied = yyjson_mut_get_str(yyjson_mut_obj_get(root, "applied"));
    if (!action) {
        action = "preserve";
    }
    if (!applied) {
        applied = "preserve";
    }
    pc_status rc = PC_OK;
    if (strcmp(action, applied) == 0) {
        /* The open policy already applied this action; nothing to re-run. */
        yyjson_mut_doc *rd = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *rr = rd ? yyjson_mut_obj(rd) : NULL;
        if (rr && yyjson_mut_obj_add_strcpy(rd, rr, "action", action)) {
            rc = pc_json_value_from_mut(rr, out, err);
        } else {
            rc = pc_fail(err, PC_ERR_NOMEM, "profile mismatch");
        }
        yyjson_mut_doc_free(rd);
    } else {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *params = d ? yyjson_mut_obj(d) : NULL;
        if (!params || !yyjson_mut_obj_add_strcpy(d, params, "action", action)) {
            yyjson_mut_doc_free(d);
            return pc_fail(err, PC_ERR_NOMEM, "profile mismatch");
        }
        rc = run_cmd(app, "color.profileMismatch", params, out, err);
        yyjson_mut_doc_free(d);
    }
    if (rc == PC_OK) {
        pc_ui_state_close_dialog(ui, id);
    }
    return rc;
}
