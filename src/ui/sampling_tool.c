#include "sampling_tool.h"
#include "ui/shortcuts.h"
#include "engine/sampling.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
void pc_ui_sampling_sync(pc_ui_sampling_tool *r, const pc_engine_session *session, const pc_ui_state *ui)
{
    if (!r || !r->active)
        return;
    const pc_engine_doc_state *state = session ? pc_engine_session_active(session) : NULL;
    if (!state || !ui || ui->tool != r->tool || ui->transform.active || ui->n_dialogs ||
        pc_doc_arc_get(state->doc)->id != r->document || state->revision != r->revision)
        r->active = false;
}
bool pc_ui_sampling_tool_kind(pc_ui_tool tool)
{
    return tool == PC_TOOL_EYEDROPPER || tool == PC_TOOL_COLOR_SAMPLER;
}
bool pc_ui_alt_samples_color(pc_ui_tool tool, unsigned modifiers)
{
    return (modifiers & PC_UI_MOD_ALT) && !(modifiers & PC_UI_MOD_CTRL) &&
           (tool == PC_TOOL_BRUSH || tool == PC_TOOL_PENCIL || tool == PC_TOOL_GRADIENT ||
            tool == PC_TOOL_PAINT_BUCKET);
}
static pc_status sample(pc_ui_sampling_tool *r, pc_engine_session *session, const pc_ui_state *ui,
                        pc_ui_tool_point point, unsigned modifiers, pc_json_value *out, pc_error *err)
{
    pc_engine_sample_layers scope;
    const char *name = pc_str_c(&ui->tool_options.eyedropper_sample);
    if (!pc_engine_sample_layers_parse(name, &scope) ||
        !pc_engine_sample_size_valid(ui->tool_options.eyedropper_size))
        return pc_fail(err, PC_ERR_INVALID, "invalid eyedropper options");
    char text[300];
    (void)snprintf(text, sizeof text, "{\"x\":%.9g,\"y\":%.9g,\"size\":%u,\"sampleLayer\":\"%s\"}",
                   (double)point.x, (double)point.y, ui->tool_options.eyedropper_size, name);
    pc_json_value color = PC_JSON_VALUE_INIT;
    pc_status status = pc_engine_session_execute_json(session, "document.sampleColor", text, &color, err);
    yyjson_mut_val *a = pc_json_value_root(&color);
    r->has_sample = false;
    if (status == PC_OK && (!yyjson_mut_is_arr(a) || yyjson_mut_arr_size(a) != 4))
        status = pc_fail(err, PC_ERR_INVALID, "invalid color sample response");
    if (status == PC_OK) {
        for (size_t c = 0; c < 4; c++)
            r->sample[c] = (float)yyjson_mut_get_num(yyjson_mut_arr_get(a, c));
        if (r->sample[3] > 0) {
            r->background = r->mode == 1 && (modifiers & PC_UI_MOD_ALT);
            r->has_sample = true;
            const char *target = r->background ? "background" : "foreground";
            (void)snprintf(text, sizeof text, "{\"%s\":[%.9g,%.9g,%.9g,1]}", target, (double)r->sample[0],
                           (double)r->sample[1], (double)r->sample[2]);
            status = pc_engine_session_execute_json(session, "tools.setColors", text, out, err);
        }
    }
    pc_json_value_fini(&color);
    return status;
}
pc_status pc_ui_sampling_pointer(pc_ui_sampling_tool *r, pc_engine_session *session, const pc_ui_state *ui,
                                 pc_ui_pointer_kind kind, pc_ui_tool_point point, unsigned modifiers,
                                 double zoom, pc_json_value *out, pc_error *err)
{
    if (!r || !session || !ui || !out || (unsigned)kind > PC_UI_POINTER_CANCEL)
        return pc_fail(err, PC_ERR_INVALID, "invalid sampling context");
    bool applies = pc_ui_sampling_tool_kind(ui->tool) || pc_ui_alt_samples_color(ui->tool, modifiers);
    if (kind == PC_UI_POINTER_CANCEL) {
        bool active = r->active;
        r->active = false;
        return active ? PC_OK : PC_ERR_NOT_FOUND;
    }
    if (kind == PC_UI_POINTER_DOWN && !applies)
        r->active = false;
    if (!r->active && !applies)
        return PC_ERR_NOT_FOUND;
    if (!isfinite(point.x) || !isfinite(point.y) || fabsf(point.x) > 1e8f || fabsf(point.y) > 1e8f ||
        !isfinite(zoom) || zoom <= 0) {
        r->active = false;
        return pc_fail(err, PC_ERR_INVALID, "invalid sample pointer");
    }
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (!state || ui->n_dialogs) {
        r->active = false;
        return pc_fail(err, PC_ERR_INVALID, "no available sampling document");
    }
    const pc_document *doc = pc_doc_arc_get(state->doc);
    if (r->active && (r->tool != ui->tool || r->document != doc->id || r->revision != state->revision)) {
        r->active = false;
        return pc_fail(err, PC_ERR_INVALID, "sampling document or tool changed");
    }
    if (kind == PC_UI_POINTER_DOWN) {
        *r = (pc_ui_sampling_tool){.active = true,
                                   .mode = ui->tool == PC_TOOL_COLOR_SAMPLER ? 2u
                                           : ui->tool == PC_TOOL_EYEDROPPER  ? 1u
                                                                             : 3u,
                                   .tool = ui->tool,
                                   .document = doc->id,
                                   .revision = state->revision,
                                   .index = SIZE_MAX,
                                   .start = {point.x, point.y},
                                   .point = {point.x, point.y}};
        memcpy(r->before, pc_engine_session_tools(session)->foreground, sizeof r->before);
        if (r->mode == 2) {
            size_t hit = SIZE_MAX;
            double distance = 8 / fmax(.01, zoom);
            for (size_t i = 0; i < state->ncolor_samplers; i++) {
                double d =
                    hypot(state->color_samplers[i][0] - point.x, state->color_samplers[i][1] - point.y);
                if (d < distance) {
                    distance = d;
                    hit = i;
                }
            }
            char text[140];
            const char *command = NULL;
            if (hit != SIZE_MAX && (modifiers & PC_UI_MOD_ALT)) {
                command = "view.colorSamplers.delete";
                (void)snprintf(text, sizeof text, "{\"index\":%zu}", hit);
            } else if (hit != SIZE_MAX)
                r->index = hit;
            else if (!(modifiers & PC_UI_MOD_ALT) && point.x >= 0 && point.y >= 0 &&
                     point.x < (float)doc->size.width && point.y < (float)doc->size.height) {
                command = "view.colorSamplers.add";
                (void)snprintf(text, sizeof text, "{\"x\":%.9g,\"y\":%.9g}", (double)point.x,
                               (double)point.y);
            }
            if (command) {
                pc_status status = pc_engine_session_execute_json(session, command, text, out, err);
                if (status != PC_OK)
                    r->active = false;
                else
                    r->revision = pc_engine_session_active(session)->revision;
                return status;
            }
        }
    }
    if (!r->active)
        return PC_OK;
    r->point[0] = point.x;
    r->point[1] = point.y;
    pc_status status = PC_OK;
    if (r->mode == 2 && kind == PC_UI_POINTER_UP && r->index != SIZE_MAX &&
        hypot((double)point.x - r->start[0], (double)point.y - r->start[1]) > .5) {
        char text[160];
        bool inside = point.x >= 0 && point.y >= 0 && point.x < (float)doc->size.width &&
                      point.y < (float)doc->size.height;
        (void)snprintf(text, sizeof text, "{\"index\":%zu,\"x\":%.9g,\"y\":%.9g}", r->index, (double)point.x,
                       (double)point.y);
        status = pc_engine_session_execute_json(
            session, inside ? "view.colorSamplers.move" : "view.colorSamplers.delete", text, out, err);
    } else if (r->mode != 2 && kind != PC_UI_POINTER_UP) {
        status = sample(r, session, ui, point, modifiers, out, err);
        state = pc_engine_session_active(session);
        if (status == PC_OK && state) {
            r->document = pc_doc_arc_get(state->doc)->id;
            r->revision = state->revision;
        }
    }
    if (kind == PC_UI_POINTER_UP || status != PC_OK)
        r->active = false;
    return status;
}
