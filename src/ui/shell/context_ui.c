#include "lasso_ui.h"
#include "context_ui.h"
#include "dialogs.h"
#include "jobs_ui.h"
#include "ui/enable_rules.h"
#include "ui/menus.h"
#include "ui/pen_tool.h"
#include "ui/i18n.h"
#include "ui/widgets.h"
#include "ui/color_picker.h"
#include "ui/type_properties.h"
#include "engine/prefs.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
static bool scope(pc_ui_shell_app *app)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    return ui->context_menu.open && !ui->n_dialogs && !pc_ui_shell_jobs_locked(app) && state &&
           pc_doc_arc_get(state->doc)->id == ui->context_menu.document &&
           state->revision == ui->context_menu.revision;
}
static int checked(pc_ui_shell_app *app, const char *id)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (ui->context_menu.kind == PC_UI_CONTEXT_TRANSFORM) {
        const char *modes[] = {"edit.freeTransform", "edit.transform.rotate", "edit.transform.skew",
                               "edit.transform.distort", "edit.transform.perspective"};
        for (size_t i = 0; i < 5; i++)
            if (!strcmp(id, modes[i]))
                return ui->transform.active && ui->transform.mode == (pc_ui_transform_mode)i;
    }
    if (ui->context_menu.kind == PC_UI_CONTEXT_PASTEBOARD && strcmp(id, "pasteboard.selectCustomColor")) {
        pc_json_value pref = PC_JSON_VALUE_INIT;
        int result = 0;
        if (pc_engine_prefs_get(pc_ui_shell_app_session(app), "interface.canvasColor", &pref, NULL) ==
            PC_OK) {
            const char *name = yyjson_mut_get_str(pc_json_value_root(&pref));
            result = name && !strcmp(name, id + 11);
        }
        pc_json_value_fini(&pref);
        return result;
    }
    if (ui->context_menu.kind == PC_UI_CONTEXT_TEXT && ui->has_text_edit &&
        (!strcmp(id, "type.fauxBold") || !strcmp(id, "type.fauxItalic"))) {
        const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
        const pc_layer *layer =
            state ? pc_document_layer(pc_doc_arc_get(state->doc), ui->text_edit.layer) : NULL;
        const pc_char_style *style =
            layer && layer->content.kind == PC_LAYER_TEXT
                ? pc_ui_type_representative(&layer->content.u.text, ui->text_edit.caret, ui->text_edit.anchor)
                : NULL;
        return style ? (!strcmp(id, "type.fauxBold") ? style->faux_bold : style->faux_italic) : 0;
    }
    return -1;
}
static bool enabled(pc_ui_shell_app *app, const char *id)
{
    if (!id || !scope(app))
        return false;
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (ui->context_menu.kind == PC_UI_CONTEXT_PASTEBOARD)
        return true;
    if (ui->context_menu.kind == PC_UI_CONTEXT_VIEW && !strcmp(id, "view.fillScreen"))
        return true;
    if (ui->context_menu.kind == PC_UI_CONTEXT_TEXT) {
        if (!ui->has_text_edit)
            return false;
        if (!strcmp(id, "edit.cut") || !strcmp(id, "edit.copy"))
            return ui->text_edit.anchor != ui->text_edit.caret;
        if (!strcmp(id, "edit.paste"))
            return SDL_HasClipboardText();
        if (!strcmp(id, "select.all") || !strcmp(id, "type.fauxBold") || !strcmp(id, "type.fauxItalic"))
            return true;
    }
    if (ui->context_menu.kind == PC_UI_CONTEXT_TRANSFORM &&
        (!strcmp(id, "edit.puppetWarp") || !strcmp(id, "edit.contentAwareScale")))
        return false;
    if (ui->context_menu.kind == PC_UI_CONTEXT_MASK) {
        const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
        const pc_layer *layer =
            state ? pc_document_layer(pc_doc_arc_get(state->doc), ui->context_menu.layer) : NULL;
        if (!layer || !layer->has_mask)
            return false;
        if (!strcmp(id, "layer.layerMask.enable"))
            return !layer->mask.enabled;
        if (!strcmp(id, "layer.layerMask.disable"))
            return layer->mask.enabled;
        if (!strcmp(id, "layer.layerMask.toSelection"))
            return true;
    }
    if (ui->context_menu.kind == PC_UI_CONTEXT_PEN && !ui->context_menu.path[0] &&
        (!strncmp(id, "path.", 5) || !strcmp(id, "edit.defineCustomShape") ||
         !strcmp(id, "layer.vectorMask.fromPath")))
        return false;
    if (!strcmp(id, "path.transform"))
        return ui->context_menu.path[0] != 0;
    if (pc_ui_command_disabled(pc_ui_shell_app_session(app), ui, id))
        return false;
    const pc_engine_command *command = pc_engine_command_find(id);
    if (command)
        return command->enabled(pc_ui_shell_app_session(app), NULL) == PC_OK;
    return pc_ui_is_live(id);
}
pc_status pc_ui_shell_context_open(pc_ui_shell_app *app, pc_ui_context_kind kind, float x, float y,
                                   const double anchor[2], pc_layer_id layer, pc_error *err)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (!isfinite(x) || !isfinite(y) || fabsf(x) > 1000000 || fabsf(y) > 1000000 || !anchor ||
        !isfinite(anchor[0]) || !isfinite(anchor[1]) || fabs(anchor[0]) > 1000000 ||
        fabs(anchor[1]) > 1000000)
        return pc_fail(err, PC_ERR_INVALID, "invalid context-menu position");
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (!state || ui->n_dialogs || pc_ui_shell_jobs_locked(app))
        return pc_fail(err, PC_ERR_INVALID, "no available context-menu target");
    if (kind == PC_UI_CONTEXT_PEN && ui->has_pen) {
        pc_json_value ignored = PC_JSON_VALUE_INIT;
        pc_status status = pc_ui_pen_commit(pc_ui_shell_app_session(app), ui, false, &ignored, err);
        pc_json_value_fini(&ignored);
        if (status != PC_OK)
            return status;
        state = pc_engine_session_active(pc_ui_shell_app_session(app));
    }
    pc_ui_context_menu menu = {.open = true,
                               .point = {x, y},
                               .anchor = {anchor[0], anchor[1]},
                               .kind = kind,
                               .document = pc_doc_arc_get(state->doc)->id,
                               .revision = state->revision,
                               .layer = layer                     ? layer
                                        : state->has_active_layer ? state->active_layer
                                                                  : 0,
                               .tool = (unsigned)ui->tool};
    const char *name = ui->selected_path                           ? ui->selected_path
                       : ui->direct.name.len                       ? pc_str_c(&ui->direct.name)
                       : pc_doc_arc_get(state->doc)->has_work_path ? "work"
                                                                   : "";
    if (strlen(name) >= sizeof menu.path)
        return pc_fail(err, PC_ERR_LIMIT, "context path name too long");
    memcpy(menu.path, name, strlen(name) + 1);
    ui->context_menu = menu;
    ui->brush_picker.open = false;
    pc_ui_shell_app_changed(app);
    return PC_OK;
}
static pc_status invoke_mut(pc_ui_shell_app *app, const char *id, yyjson_mut_val *params, pc_json_value *out,
                            pc_error *err)
{
    pc_buf text = PC_BUF_INIT;
    yyjson_doc *parsed = NULL;
    pc_status status = pc_json_write(params, 0, &text, err);
    if (status == PC_OK)
        status = pc_json_parse(pc_buf_cstr(&text), text.len, &parsed, err);
    if (status == PC_OK)
        status = pc_ui_shell_app_invoke(app, id, yyjson_doc_get_root(parsed), out, err);
    pc_buf_fini(&text);
    yyjson_doc_free(parsed);
    return status;
}
pc_status pc_ui_shell_context_choose(pc_ui_shell_app *app, const char *id, pc_json_value *out, pc_error *err)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    size_t count = 0;
    const pc_ui_context_row *rows = pc_ui_context_rows(ui->context_menu.kind, &count);
    bool found = false;
    for (size_t i = 0; i < count; i++)
        if (rows[i].command && id && !strcmp(rows[i].command, id))
            found = true;
    if (!found || !enabled(app, id))
        return pc_fail(err, PC_ERR_INVALID, "context action is unavailable");
    pc_ui_context_menu menu = ui->context_menu;
    ui->context_menu.open = false;
    if (menu.kind == PC_UI_CONTEXT_TEXT && ui->has_text_edit) {
        if (!strcmp(id, "edit.cut") || !strcmp(id, "edit.copy") || !strcmp(id, "edit.paste"))
            return pc_ui_shell_type_clipboard(app,
                                              !strcmp(id, "edit.cut")    ? SDLK_X
                                              : !strcmp(id, "edit.copy") ? SDLK_C
                                                                         : SDLK_V,
                                              out, err);
        if (!strcmp(id, "select.all") || !strcmp(id, "edit.undo"))
            return pc_ui_type_key(pc_ui_shell_app_session(app), ui, &pc_ui_shell_type_pointer(app)->layout,
                                  !strcmp(id, "select.all") ? PC_KEY_A : PC_KEY_Z, PC_UI_MOD_COMMAND, out,
                                  err);
        if (!strcmp(id, "type.fauxBold") || !strcmp(id, "type.fauxItalic")) {
            const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
            const pc_layer *layer =
                state ? pc_document_layer(pc_doc_arc_get(state->doc), ui->text_edit.layer) : NULL;
            const pc_char_style *style =
                layer && layer->content.kind == PC_LAYER_TEXT
                    ? pc_ui_type_representative(&layer->content.u.text, ui->text_edit.caret,
                                                ui->text_edit.anchor)
                    : NULL;
            if (!style)
                return pc_fail(err, PC_ERR_INVALID, "type context target changed");
            bool bold = !strcmp(id, "type.fauxBold");
            yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
            yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
            pc_status status = p && yyjson_mut_obj_add_bool(d, p, bold ? "fauxBold" : "fauxItalic",
                                                            bold ? !style->faux_bold : !style->faux_italic)
                                   ? invoke_mut(app, "type.setStyle", p, out, err)
                                   : pc_fail(err, PC_ERR_NOMEM, "type context style");
            yyjson_mut_doc_free(d);
            return status;
        }
    }
    if (menu.kind == PC_UI_CONTEXT_VIEW && (!strcmp(id, "view.zoomIn") || !strcmp(id, "view.zoomOut"))) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL, *anchor = d ? yyjson_mut_arr(d) : NULL;
        pc_status status = p && anchor && yyjson_mut_arr_add_real(d, anchor, menu.anchor[0]) &&
                                   yyjson_mut_arr_add_real(d, anchor, menu.anchor[1]) &&
                                   yyjson_mut_obj_add_val(d, p, "point", anchor)
                               ? invoke_mut(app, id, p, out, err)
                               : pc_fail(err, PC_ERR_NOMEM, "view context anchor");
        yyjson_mut_doc_free(d);
        return status;
    }
    if (menu.kind == PC_UI_CONTEXT_PASTEBOARD && !strcmp(id, "pasteboard.selectCustomColor")) {
        const char *json = "{\"kind\":\"colorPicker\",\"target\":\"pasteboard\"}";
        yyjson_doc *p = NULL;
        pc_status status = pc_json_parse(json, strlen(json), &p, err);
        if (status == PC_OK)
            status = pc_ui_shell_dialog_control(app, "ui.dialog.open", yyjson_doc_get_root(p), out, err);
        yyjson_doc_free(p);
        return status;
    }
    if (menu.kind == PC_UI_CONTEXT_PASTEBOARD) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
        pc_status status = p && yyjson_mut_obj_add_str(d, p, "path", "interface.canvasColor") &&
                                   yyjson_mut_obj_add_strcpy(d, p, "value", id + 11)
                               ? invoke_mut(app, "prefs.set", p, out, err)
                               : pc_fail(err, PC_ERR_NOMEM, "pasteboard preference");
        yyjson_mut_doc_free(d);
        return status;
    }
    if (menu.kind == PC_UI_CONTEXT_MASK &&
        (!strcmp(id, "layer.layerMask.enable") || !strcmp(id, "layer.layerMask.disable") ||
         !strcmp(id, "layer.layerMask.toSelection"))) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
        bool ok = p && yyjson_mut_obj_add_uint(d, p, "layer", menu.layer);
        const char *command = "layer.layerMask.enabled";
        if (!strcmp(id, "layer.layerMask.toSelection")) {
            command = "select.loadSelection";
            ok = ok && yyjson_mut_obj_add_str(d, p, "channel", "mask") &&
                 yyjson_mut_obj_add_str(d, p, "operation", "replace");
        } else
            ok = ok && yyjson_mut_obj_add_bool(d, p, "enabled", !strcmp(id, "layer.layerMask.enable"));
        pc_status status =
            ok ? invoke_mut(app, command, p, out, err) : pc_fail(err, PC_ERR_NOMEM, "mask context action");
        yyjson_mut_doc_free(d);
        return status;
    }
    if (!strcmp(id, "path.transform"))
        return pc_ui_shell_app_invoke(app, "edit.freeTransform", NULL, out, err);
    const char *forms[] = {"select.modify.feather",
                           "select.saveSelection",
                           "select.loadSelection",
                           "select.toWorkPath",
                           "edit.fill",
                           "edit.stroke",
                           "edit.fade",
                           "path.fill",
                           "path.stroke",
                           "path.toSelection",
                           "path.clippingPath.set",
                           "edit.defineCustomShape",
                           "layer.duplicate"};
    for (size_t i = 0; i < sizeof forms / sizeof forms[0]; i++)
        if (!strcmp(id, forms[i])) {
            yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
            yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL, *fields = d ? yyjson_mut_obj(d) : NULL;
            bool ok = p && fields && yyjson_mut_obj_add_str(d, p, "kind", "command") &&
                      yyjson_mut_obj_add_strcpy(d, p, "command", id) &&
                      yyjson_mut_obj_add_val(d, p, "fields", fields);
            if (menu.kind == PC_UI_CONTEXT_PEN && menu.path[0])
                ok = ok && yyjson_mut_obj_add_strcpy(d, fields, "name", menu.path);
            pc_buf text = PC_BUF_INIT;
            yyjson_doc *parsed = NULL;
            pc_status status =
                ok ? pc_json_write(p, 0, &text, err) : pc_fail(err, PC_ERR_NOMEM, "context form");
            if (status == PC_OK)
                status = pc_json_parse(pc_buf_cstr(&text), text.len, &parsed, err);
            if (status == PC_OK)
                status =
                    pc_ui_shell_dialog_control(app, "ui.dialog.open", yyjson_doc_get_root(parsed), out, err);
            pc_buf_fini(&text);
            yyjson_doc_free(parsed);
            yyjson_mut_doc_free(d);
            return status;
        }
    if (menu.kind == PC_UI_CONTEXT_MASK && !strncmp(id, "layer.layerMask.", 16)) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
        pc_status status = p && yyjson_mut_obj_add_uint(d, p, "layer", menu.layer)
                               ? invoke_mut(app, id, p, out, err)
                               : pc_fail(err, PC_ERR_NOMEM, "mask context target");
        yyjson_mut_doc_free(d);
        return status;
    }
    if (menu.kind == PC_UI_CONTEXT_PEN && menu.path[0] && strncmp(id, "layer.", 6)) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
        pc_status status = p && yyjson_mut_obj_add_strcpy(d, p, "name", menu.path)
                               ? invoke_mut(app, id, p, out, err)
                               : pc_fail(err, PC_ERR_NOMEM, "path context action");
        yyjson_mut_doc_free(d);
        return status;
    }
    return pc_ui_shell_app_invoke(app, id, NULL, out, err);
}
pc_status pc_ui_shell_context_json(pc_ui_shell_app *app, yyjson_mut_doc *d, yyjson_mut_val **out,
                                   pc_error *err)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (ui->context_menu.open && !scope(app))
        ui->context_menu.open = false;
    pc_status status = pc_ui_context_to_json(&ui->context_menu, d, out, err);
    if (status != PC_OK || !ui->context_menu.open)
        return status;
    yyjson_mut_val *entries = yyjson_mut_arr(d);
    size_t count = 0;
    const pc_ui_context_row *rows = pc_ui_context_rows(ui->context_menu.kind, &count);
    bool ok = entries != NULL;
    for (size_t i = 0; ok && i < count; i++) {
        if (!rows[i].command) {
            ok = yyjson_mut_arr_add_null(d, entries);
            continue;
        }
        yyjson_mut_val *row = yyjson_mut_obj(d);
        ok = row && yyjson_mut_obj_add_str(d, row, "id", rows[i].command) &&
             yyjson_mut_obj_add_strcpy(d, row, "label", pc_i18n_t(rows[i].label)) &&
             yyjson_mut_obj_add_bool(d, row, "enabled", enabled(app, rows[i].command)) &&
             yyjson_mut_obj_add_val(d, row, "checked",
                                    checked(app, rows[i].command) < 0
                                        ? yyjson_mut_null(d)
                                        : yyjson_mut_bool(d, checked(app, rows[i].command) != 0)) &&
             yyjson_mut_arr_append(entries, row);
    }
    if (!ok || !yyjson_mut_obj_add_val(d, *out, "entries", entries))
        return pc_fail(err, PC_ERR_NOMEM, "context entries");
    return PC_OK;
}
bool pc_ui_shell_context_event(pc_ui_shell_app *app, const SDL_Event *event)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (!ui->context_menu.open)
        return false;
    if (!scope(app)) {
        ui->context_menu.open = false;
        return false;
    }
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST ||
        (event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_ESCAPE)) {
        ui->context_menu.open = false;
        return event->type == SDL_EVENT_KEY_DOWN;
    }
    if (event->type == SDL_EVENT_KEY_DOWN) {
        size_t count = 0;
        const pc_ui_context_row *rows = pc_ui_context_rows(ui->context_menu.kind, &count);
        if (!count)
            return true;
        if (event->key.key == SDLK_DOWN || event->key.key == SDLK_UP) {
            size_t selected = ui->context_menu.selected;
            for (size_t i = 0; i < count; i++) {
                selected =
                    event->key.key == SDLK_DOWN ? (selected + 1) % count : (selected + count - 1) % count;
                if (enabled(app, rows[selected].command)) {
                    ui->context_menu.selected = selected;
                    struct nk_context *ctx = pc_ui_shell_app_nk(app);
                    struct nk_window *window = ctx ? nk_window_find(ctx, "Canvas context menu") : NULL;
                    if (window) {
                        size_t visible = (size_t)fmaxf(1, (window->bounds.h - 20) / 24);
                        window->scrollbar.y =
                            (nk_uint)(selected >= visible ? (selected - visible + 1) * 24 : 0);
                    }
                    break;
                }
            }
        } else if (event->key.key == SDLK_RETURN && !event->key.repeat && ui->context_menu.selected < count &&
                   enabled(app, rows[ui->context_menu.selected].command)) {
            pc_json_value out = PC_JSON_VALUE_INIT;
            pc_error err = {0};
            pc_status status =
                pc_ui_shell_context_choose(app, rows[ui->context_menu.selected].command, &out, &err);
            if (status != PC_OK)
                pc_ui_shell_report(app, "Context Menu", err.msg, true);
            pc_json_value_fini(&out);
        }
        return true;
    }
    if (event->type != SDL_EVENT_MOUSE_BUTTON_DOWN)
        return false;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    struct nk_window *window = ctx ? nk_window_find(ctx, "Canvas context menu") : NULL;
    float scale = pc_ui_shell_app_ui_scale(app);
    SDL_Window *host = pc_ui_shell_app_sdl_window(app);
    float factor = (host ? SDL_GetWindowPixelDensity(host) : 1) / fmaxf(.01f, scale);
    float x = event->button.x * factor, y = event->button.y * factor;
    if (window && (x < window->bounds.x || y < window->bounds.y || x >= window->bounds.x + window->bounds.w ||
                   y >= window->bounds.y + window->bounds.h)) {
        ui->context_menu.open = false;
        return true;
    }
    return false;
}
pc_status pc_ui_shell_secondary_pointer(pc_ui_shell_app *app, pc_ui_pointer_kind event,
                                        pc_ui_tool_point point, unsigned modifiers, pc_json_value *out,
                                        pc_error *err)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_secondary_runtime *runtime = pc_ui_shell_secondary_runtime(app);
    if (event == PC_UI_POINTER_DOWN && ui->lasso.active && (ui->tool != PC_TOOL_LASSO || !ui->lasso.down))
        return pc_ui_lasso_retract(pc_ui_shell_app_session(app), ui, out, err);
    if (event == PC_UI_POINTER_DOWN) {
        runtime->erase = false;
        pc_json_value prefs = PC_JSON_VALUE_INIT;
        if (!ui->transform.active && (ui->tool == PC_TOOL_BRUSH || ui->tool == PC_TOOL_ERASER) &&
            pc_engine_prefs_get(pc_ui_shell_app_session(app), "tools.rightClickWithPaintingTools", &prefs,
                                NULL) == PC_OK) {
            const char *choice = yyjson_mut_get_str(pc_json_value_root(&prefs));
            runtime->erase = choice && !strcmp(choice, "erase");
            runtime->tool = ui->tool;
        }
        pc_json_value_fini(&prefs);
    }
    if (runtime->erase) {
        pc_ui_tool previous = ui->tool;
        if (previous != runtime->tool || ui->n_dialogs)
            runtime->erase = false;
        else {
            ui->tool = PC_TOOL_ERASER;
            pc_status status = pc_ui_shell_app_pointer(app, event, point, modifiers, out, err);
            ui->tool = previous;
            if (event == PC_UI_POINTER_UP || event == PC_UI_POINTER_CANCEL || status != PC_OK)
                runtime->erase = false;
            return status;
        }
    }
    if (event != PC_UI_POINTER_DOWN)
        return PC_OK;
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (!state || ui->n_dialogs || pc_ui_shell_jobs_locked(app))
        return pc_fail(err, PC_ERR_INVALID, "no available context-menu target");
    double anchor[2] = {point.x, point.y};
    float screen[2] = {point.x, point.y};
    pc_ui_canvas_xform xf;
    if (pc_ui_shell_app_canvas_mapping(app, &xf))
        (void)pc_ui_canvas_to_screen(&xf, point.x, point.y, screen);
    const pc_document *doc = pc_doc_arc_get(state->doc);
    pc_ui_context_kind kind;
    bool outside =
        point.x < 0 || point.y < 0 || point.x >= (float)doc->size.width || point.y >= (float)doc->size.height;
    if (ui->transform.active)
        kind = PC_UI_CONTEXT_TRANSFORM;
    else if (pc_ui_tool_is_brushlike(ui->tool) || ui->tool == PC_TOOL_QUICK_SELECTION) {
        ui->brush_picker.open = true;
        ui->brush_picker.point[0] = screen[0];
        ui->brush_picker.point[1] = screen[1];
        return PC_OK;
    } else if (outside && !pc_ui_pen_tool(ui->tool) && !ui->has_text_edit)
        kind = PC_UI_CONTEXT_PASTEBOARD;
    else if (ui->has_text_edit)
        kind = PC_UI_CONTEXT_TEXT;
    else if (ui->tool == PC_TOOL_HAND || ui->tool == PC_TOOL_ZOOM)
        kind = PC_UI_CONTEXT_VIEW;
    else if (pc_ui_pen_tool(ui->tool) || ui->tool == PC_TOOL_PATH_SELECTION ||
             ui->tool == PC_TOOL_DIRECT_SELECTION)
        kind = PC_UI_CONTEXT_PEN;
    else
        kind = doc->has_selection ? PC_UI_CONTEXT_SELECTION : PC_UI_CONTEXT_EMPTY_SELECTION;
    return pc_ui_shell_context_open(app, kind, screen[0], screen[1], anchor, 0, err);
}
bool pc_ui_shell_context_canvas(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, const SDL_Event *event,
                                float factor)
{
    bool motion = event->type == SDL_EVENT_MOUSE_MOTION;
    bool down = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    bool button = down || event->type == SDL_EVENT_MOUSE_BUTTON_UP;
    pc_ui_secondary_runtime *runtime = pc_ui_shell_secondary_runtime(app);
    if ((!button || event->button.button != SDL_BUTTON_RIGHT) && !(motion && runtime->erase))
        return false;
    float x = (motion ? event->motion.x : event->button.x) * factor,
          y = (motion ? event->motion.y : event->button.y) * factor;
    if (!runtime->erase &&
        (!down || x < xf->x || y < xf->y || x >= xf->x + xf->width || y >= xf->y + xf->height))
        return false;
    float point[2];
    if (pc_ui_canvas_to_doc(xf, x, y, point) != PC_OK)
        return true;
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status status =
        pc_ui_shell_secondary_pointer(app,
                                      down     ? PC_UI_POINTER_DOWN
                                      : motion ? PC_UI_POINTER_MOVE
                                               : PC_UI_POINTER_UP,
                                      (pc_ui_tool_point){.x = point[0], .y = point[1], .pressure = 1},
                                      pc_ui_shell_input_modifiers(app), &out, &err);
    if (status != PC_OK)
        pc_ui_shell_report(app, "Context Menu", err.msg, true);
    pc_json_value_fini(&out);
    return true;
}
void pc_ui_shell_context_draw(pc_ui_shell_app *app, float width, float height)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (!ui->context_menu.open)
        return;
    if (!scope(app)) {
        ui->context_menu.open = false;
        return;
    }
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    size_t count = 0;
    const pc_ui_context_row *rows = pc_ui_context_rows(ui->context_menu.kind, &count);
    float h = fminf(height - 20, (float)count * 24 + 12), w = fminf(300, width - 20);
    float x = fminf(fmaxf(0, ui->context_menu.point[0]), width - w),
          y = fminf(fmaxf(0, ui->context_menu.point[1]), height - h);
    nk_window_set_bounds(ctx, "Canvas context menu", nk_rect(x, y, w, h));
    if (nk_begin(ctx, "Canvas context menu", nk_rect(x, y, w, h), NK_WINDOW_BORDER)) {
        for (size_t i = 0; i < count; i++) {
            nk_layout_row_dynamic(ctx, rows[i].command ? 22 : 5, 1);
            if (!rows[i].command) {
                nk_label(ctx, "", NK_TEXT_LEFT);
                continue;
            }
            bool live = enabled(app, rows[i].command);
            if (!live)
                nk_widget_disable_begin(ctx);
            char mapping[160];
            (void)snprintf(mapping, sizeof mapping, "context.%s", rows[i].command);
            pc_ui_shell_widget_mapping(app, mapping, nk_widget_bounds(ctx));
            struct nk_style_item before = ctx->style.button.normal;
            if (i == ui->context_menu.selected)
                ctx->style.button.normal =
                    nk_style_item_color(pc_ui_nk_color(pc_ui_shell_app_tokens(app)->accent_soft));
            if (nk_input_is_mouse_hovering_rect(&ctx->input, nk_widget_bounds(ctx)))
                ui->context_menu.selected = i;
            char label[512];
            (void)snprintf(label, sizeof label, "%s%s", checked(app, rows[i].command) > 0 ? "✓ " : "",
                           pc_i18n_t(rows[i].label));
            bool clicked = nk_button_label(ctx, label) != 0;
            ctx->style.button.normal = before;
            if (clicked) {
                yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
                yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
                if (p && yyjson_mut_obj_add_str(d, p, "id", rows[i].command))
                    pc_ui_shell_queue(app, "ui.context.choose", p);
                yyjson_mut_doc_free(d);
            }
            if (!live)
                nk_widget_disable_end(ctx);
        }
    }
    nk_end(ctx);
    nk_window_set_focus(ctx, "Canvas context menu");
}
pc_ui_color pc_ui_shell_canvas_color(pc_ui_shell_app *app)
{
    pc_ui_color color = pc_ui_shell_app_tokens(app)->canvas;
    pc_json_value prefs = PC_JSON_VALUE_INIT;
    if (pc_engine_prefs_get(pc_ui_shell_app_session(app), "interface", &prefs, NULL) == PC_OK) {
        yyjson_mut_val *root = pc_json_value_root(&prefs);
        const char *name = yyjson_mut_get_str(yyjson_mut_obj_get(root, "canvasColor"));
        int gray = name && !strcmp(name, "black")        ? 0
                   : name && !strcmp(name, "darkGray")   ? 40
                   : name && !strcmp(name, "mediumGray") ? 128
                   : name && !strcmp(name, "lightGray")  ? 192
                                                         : -1;
        if (gray >= 0)
            color = (pc_ui_color){(uint8_t)gray, (uint8_t)gray, (uint8_t)gray, 255};
        else if (name && !strcmp(name, "custom")) {
            const char *hex = yyjson_mut_get_str(yyjson_mut_obj_get(root, "canvasCustomColor"));
            float rgb[3];
            if (hex && pc_ui_picker_parse_hex(hex, strlen(hex), rgb))
                color = (pc_ui_color){(uint8_t)lroundf(rgb[0] * 255), (uint8_t)lroundf(rgb[1] * 255),
                                      (uint8_t)lroundf(rgb[2] * 255), 255};
        }
    }
    pc_json_value_fini(&prefs);
    return color;
}
