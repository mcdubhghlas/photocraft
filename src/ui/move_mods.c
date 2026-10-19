#include "move_mods.h"
#include <math.h>
void pc_ui_move_constrain(const double delta[2], const double *previous, unsigned directions, double out[2])
{
    double x = delta[0], y = delta[1];
    out[0] = x;
    out[1] = y;
    double len = hypot(x, y);
    if (!directions || !isfinite(len) || len == 0)
        return;
    double step = 6.2831853071795864769 / directions, angle = atan2(y, x), k = round(angle / step);
    if (previous && isfinite(previous[0]) && isfinite(previous[1]) && hypot(previous[0], previous[1]) > 0) {
        double prior = round(atan2(previous[1], previous[0]) / step);
        if (fabs(remainder(angle - prior * step, 6.2831853071795864769)) < step / 2 + .13962634015954636)
            k = prior;
    }
    double c = cos(k * step), s = sin(k * step), t = fmax(0, x * c + y * s);
    out[0] = fabs(t * c) < 1e-9 ? 0 : t * c;
    out[1] = fabs(t * s) < 1e-9 ? 0 : t * s;
}

#include "ui/shortcuts.h"
#include "ui/tool_cursor.h"
#include "engine/floating.h"
#include <stdio.h>
pc_status pc_ui_move_key(pc_engine_session *session, const pc_ui_state *ui, pc_ui_key key, unsigned modifiers,
                         pc_json_value *out, pc_error *err)
{
    if (!ui || ui->n_dialogs || ui->has_text_edit || ui->transform.active ||
        !pc_engine_session_active(session))
        return PC_ERR_NOT_FOUND;
    bool temporary = (modifiers & (PC_UI_MOD_COMMAND | PC_UI_MOD_CTRL)) != 0;
    bool move = ui->tool == PC_TOOL_MOVE || temporary;
    if ((move || pc_ui_is_selection_tool(ui->tool)) && pc_engine_floating_offset(session, NULL) &&
        (key == PC_KEY_ENTER || key == PC_KEY_ESCAPE))
        return pc_engine_session_execute_json(session, key == PC_KEY_ENTER ? "select.drop" : "edit.undo",
                                              "{}", out, err);
    bool arrow = key == PC_KEY_ARROWLEFT || key == PC_KEY_ARROWRIGHT || key == PC_KEY_ARROWUP ||
                 key == PC_KEY_ARROWDOWN;
    if (!arrow)
        return PC_ERR_NOT_FOUND;
    bool alt = (modifiers & PC_UI_MOD_ALT) != 0, shift = (modifiers & PC_UI_MOD_SHIFT) != 0;
    if (!temporary && !alt && pc_ui_tool_is_brushlike(ui->tool) &&
        (key == PC_KEY_ARROWLEFT || key == PC_KEY_ARROWRIGHT)) {
        char text[60];
        (void)snprintf(text, sizeof text, "{\"degrees\":%d}",
                       (key == PC_KEY_ARROWLEFT ? 1 : -1) * (shift ? 15 : 1));
        return pc_engine_session_execute_json(session, "tools.rotateBrush", text, out, err);
    }
    bool outline = !move && pc_ui_is_selection_tool(ui->tool) &&
                   pc_doc_arc_get(pc_engine_session_active(session)->doc)->has_selection;
    if (!move && !outline)
        return PC_ERR_NOT_FOUND;
    int step = shift ? 10 : 1,
        dx = key == PC_KEY_ARROWLEFT    ? -step
             : key == PC_KEY_ARROWRIGHT ? step
                                        : 0,
        dy = key == PC_KEY_ARROWUP     ? -step
             : key == PC_KEY_ARROWDOWN ? step
                                       : 0;
    const char *command = outline ? "select.transformSelection" : "layer.translate";
    bool floating = move && pc_engine_floating_can_move(session);
    if (floating) {
        if (alt && pc_engine_floating_offset(session, NULL)) {
            pc_status status = pc_engine_session_execute_json(session, "select.drop", "{}", out, err);
            if (status != PC_OK)
                return status;
        }
        command = "select.float";
    }
    char text[180];
    (void)snprintf(text, sizeof text,
                   "{\"dx\":%d,\"dy\":%d,\"copy\":%s,\"target\":\"%s\",\"interpolation\":\"nearest\"}", dx,
                   dy, alt ? "true" : "false", ui->mask_target ? "mask" : "pixels");
    return pc_engine_session_execute_json(session, command, text, out, err);
}

unsigned pc_ui_selection_move_kind(const pc_engine_session *session, const pc_ui_state *ui, double x,
                                   double y, unsigned modifiers)
{
    if (!session || !ui || ui->lasso.active || ui->transform.active || ui->n_dialogs || !isfinite(x) ||
        !isfinite(y))
        return 0;
    pc_ui_tool tool = ui->tool;
    bool clicky = tool == PC_TOOL_POLYGON_LASSO || tool == PC_TOOL_MAGIC_WAND;
    if (!clicky && tool != PC_TOOL_RECT_MARQUEE && tool != PC_TOOL_ELLIPSE_MARQUEE && tool != PC_TOOL_LASSO)
        return 0;
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (!state)
        return 0;
    const pc_document *doc = pc_doc_arc_get(state->doc);
    bool command = (modifiers & (PC_UI_MOD_COMMAND | PC_UI_MOD_CTRL)) != 0;
    bool shift = (modifiers & PC_UI_MOD_SHIFT) != 0, alt = (modifiers & PC_UI_MOD_ALT) != 0;
    int32_t offset[2] = {0, 0};
    bool floating = pc_engine_floating_offset(session, offset);
    double px = floor(x - offset[0]), py = floor(y - offset[1]);
    bool inside = doc->has_selection && px >= INT32_MIN && px <= INT32_MAX && py >= INT32_MIN &&
                  py <= INT32_MAX &&
                  pc_surface_sample_channel(&doc->selection, (int32_t)px, (int32_t)py, 0) > 0;
    if (floating)
        return inside && !shift && (!alt || command) && (command || !clicky) ? 2 : 0;
    if (!inside)
        return command && !shift ? 3 : 0;
    if (command && !shift)
        return 2;
    return !clicky && !command && !shift && !alt && ui->selection_mode == 0 ? 1 : 0;
}
