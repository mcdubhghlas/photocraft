#include "rasterize_ui.h"
#include "shell_internal.h"
#include "ui/rasterize.h"
#include "ui/i18n.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static pc_status open_prompt(pc_ui_shell_app *app, const pc_ui_rasterize_kind *kind,
                             const pc_ui_tool_point *point, const char *filter, bool smart,
                             yyjson_val *params, pc_json_value *out, pc_error *err)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (ui->n_dialogs)
        return pc_fail(err, PC_ERR_INVALID, "confirm or cancel the open dialog first");
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (!state || !state->has_active_layer)
        return pc_fail(err, PC_ERR_INVALID, "rasterizing needs an active layer");
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = d ? yyjson_mut_obj(d) : NULL;
    bool ok = root && yyjson_mut_obj_add_bool(d, root, "__rasterize", true) &&
              yyjson_mut_obj_add_str(d, root, "__label", "PhotoCraft") &&
              yyjson_mut_obj_add_strcpy(d, root, "__message", kind->message) &&
              yyjson_mut_obj_add_strcpy(d, root, "__command", kind->command) &&
              yyjson_mut_obj_add_uint(d, root, "__document", pc_doc_arc_get(state->doc)->id) &&
              yyjson_mut_obj_add_uint(d, root, "__revision", state->revision) &&
              yyjson_mut_obj_add_uint(d, root, "__layer", state->active_layer) &&
              yyjson_mut_obj_add_str(d, root, "__tool", pc_ui_tool_serde(ui->tool));
    if (point)
        ok = ok && yyjson_mut_obj_add_real(d, root, "__x", point->x) &&
             yyjson_mut_obj_add_real(d, root, "__y", point->y) &&
             yyjson_mut_obj_add_real(d, root, "__pressure", point->pressure);
    if (filter)
        ok = ok && yyjson_mut_obj_add_strcpy(d, root, "__afterFilter", filter) &&
             yyjson_mut_obj_add_bool(d, root, "__smart", smart) &&
             yyjson_mut_obj_add_str(d, root, "choice", "rasterize") &&
             yyjson_mut_obj_add_val(d, root, "__params",
                                    params ? yyjson_val_mut_copy(d, params) : yyjson_mut_null(d));
    if (!ok) {
        yyjson_mut_doc_free(d);
        return pc_fail(err, PC_ERR_NOMEM, "rasterize prompt");
    }
    return pc_ui_shell_dialog_store(app, PC_DIALOG_COMMAND, d, root, out, err);
}
pc_status pc_ui_shell_rasterize_pointer(pc_ui_shell_app *app, pc_ui_tool_point point, pc_json_value *out,
                                        pc_error *err)
{
    const pc_ui_rasterize_kind *kind =
        pc_ui_rasterize_needed(pc_ui_shell_app_session(app), pc_ui_shell_app_state(app));
    if (!kind)
        return PC_ERR_NOT_FOUND;
    if (!isfinite(point.x) || !isfinite(point.y) || !isfinite(point.pressure))
        return pc_fail(err, PC_ERR_INVALID, "invalid paint point");
    return open_prompt(app, kind, &point, NULL, false, NULL, out, err);
}
pc_status pc_ui_shell_rasterize_filter(pc_ui_shell_app *app, const char *command, yyjson_val *params,
                                       pc_json_value *out, pc_error *err)
{
    bool smart = false;
    if (!pc_ui_rasterize_filter_needed(pc_ui_shell_app_session(app), command, &smart))
        return PC_ERR_NOT_FOUND;
    const pc_ui_rasterize_kind kind = {"layer.rasterize.type",
                                       "This type layer must be rasterized before proceeding. Its text will "
                                       "no longer be editable. Rasterize the type?"};
    return open_prompt(app, &kind, NULL, command, smart, params, out, err);
}
pc_status pc_ui_shell_rasterize_confirm(pc_ui_shell_app *app, uint64_t id, pc_json_value *out, pc_error *err)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_dialog *dialog = pc_ui_state_dialog_mut(ui, id);
    yyjson_mut_val *root = dialog ? yyjson_mut_doc_get_root(dialog->fields) : NULL;
    pc_engine_session *s = pc_ui_shell_app_session(app);
    const pc_engine_doc_state *state = pc_engine_session_active(s);
    if (!root || !state ||
        pc_doc_arc_get(state->doc)->id != yyjson_mut_get_uint(yyjson_mut_obj_get(root, "__document")) ||
        state->revision != yyjson_mut_get_uint(yyjson_mut_obj_get(root, "__revision")) ||
        !state->has_active_layer ||
        state->active_layer != yyjson_mut_get_uint(yyjson_mut_obj_get(root, "__layer")))
        return pc_fail(err, PC_ERR_INVALID, "the rasterize target changed while the prompt was open");
    const char *choice = yyjson_mut_get_str(yyjson_mut_obj_get(root, "choice"));
    bool smart = choice && !strcmp(choice, "smartObject");
    if (choice && strcmp(choice, "rasterize") && !smart)
        return pc_fail(err, PC_ERR_INVALID, "invalid rasterize choice");
    if (smart && !yyjson_mut_get_bool(yyjson_mut_obj_get(root, "__smart")))
        return pc_fail(err, PC_ERR_INVALID, "this filter cannot run as a smart filter");
    const char *command = smart ? "layer.smartObjects.convertToSmartObject"
                                : yyjson_mut_get_str(yyjson_mut_obj_get(root, "__command"));
    char json[96];
    (void)snprintf(json, sizeof json, "{\"layer\":%llu}", (unsigned long long)state->active_layer);
    pc_ui_tool tool = ui->tool;
    (void)pc_ui_tool_from_serde(yyjson_mut_get_str(yyjson_mut_obj_get(root, "__tool")), &tool);
    pc_ui_tool_point point = {.x = (float)yyjson_mut_get_num(yyjson_mut_obj_get(root, "__x")),
                              .y = (float)yyjson_mut_get_num(yyjson_mut_obj_get(root, "__y")),
                              .pressure = (float)yyjson_mut_get_num(yyjson_mut_obj_get(root, "__pressure"))};
    const char *filter = yyjson_mut_get_str(yyjson_mut_obj_get(root, "__afterFilter"));
    pc_status status = pc_engine_session_execute_json(s, command, json, out, err);
    if (status != PC_OK)
        return status;
    if (filter) {
        yyjson_mut_val *params = yyjson_mut_obj_get(root, "__params");
        pc_ui_shell_queue(app, filter, yyjson_mut_is_null(params) ? NULL : params);
        pc_ui_state_close_dialog(ui, id);
        return PC_OK;
    }
    pc_ui_state_close_dialog(ui, id);
    ui->tool = tool;
    status = pc_ui_shell_app_pointer(app, PC_UI_POINTER_DOWN, point, 0, out, err);
    if (status == PC_OK)
        status = pc_ui_shell_app_pointer(app, PC_UI_POINTER_UP, point, 0, out, err);
    return status;
}
void pc_ui_shell_rasterize_body(pc_ui_shell_app *app, pc_ui_dialog *dialog)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    const char *message = yyjson_mut_get_str(yyjson_mut_obj_get(root, "__message"));
    nk_layout_row_dynamic(ctx, 90, 1);
    nk_label_wrap(ctx, pc_i18n_t(message ? message : "Rasterize this layer?"));
    if (yyjson_mut_get_bool(yyjson_mut_obj_get(root, "__smart"))) {
        nk_layout_row_dynamic(ctx, 26, 1);
        const char *choice = yyjson_mut_get_str(yyjson_mut_obj_get(root, "choice"));
        nk_bool smart = choice && !strcmp(choice, "smartObject");
        pc_ui_shell_widget_mapping(app, "rasterize.smartObject", nk_widget_bounds(ctx));
        if (nk_checkbox_label(ctx, pc_i18n_t("Convert to Smart Object"), &smart)) {
            (void)yyjson_mut_obj_remove_str(root, "choice");
            (void)yyjson_mut_obj_add_str(dialog->fields, root, "choice", smart ? "smartObject" : "rasterize");
        }
    }
}
