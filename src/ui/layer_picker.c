#include "layer_picker.h"
#include "engine/sampling.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static const char *const POINTS[] = {"black", "gray", "white"};
bool pc_ui_layer_picker_armed(const pc_engine_session *session, const pc_ui_state *ui)
{
    if (!session || !ui || !ui->layer_picker.armed || ui->layer_picker.tool != ui->tool ||
        ui->layer_picker.point > 2)
        return false;
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (!state || !state->has_active_layer || state->active_layer != ui->layer_picker.layer)
        return false;
    const pc_document *doc = pc_doc_arc_get(state->doc);
    if (ui->layer_picker.document && ui->layer_picker.document != doc->id)
        return false;
    const pc_layer *layer = pc_document_layer(doc, state->active_layer);
    return layer && layer->content.kind == PC_LAYER_ADJUSTMENT &&
           (layer->content.u.adjustment.kind == PC_ADJ_LEVELS ||
            layer->content.u.adjustment.kind == PC_ADJ_CURVES) &&
           pc_engine_tone_picker_supported(doc->mode, (pc_engine_tone_picker)ui->layer_picker.point);
}
void pc_ui_layer_picker_prune(const pc_engine_session *session, pc_ui_state *ui)
{
    if (ui && ui->layer_picker.armed && !pc_ui_layer_picker_armed(session, ui))
        ui->layer_picker = (pc_ui_layer_picker){0};
}
pc_status pc_ui_layer_picker_arm(const pc_engine_session *session, pc_ui_state *ui,
                                 pc_engine_tone_picker point, pc_error *err)
{
    const pc_engine_doc_state *state = session ? pc_engine_session_active(session) : NULL;
    if (!ui || !state || !state->has_active_layer || (unsigned)point > 2)
        return pc_fail(err, PC_ERR_INVALID, "select a Levels or Curves adjustment layer");
    pc_ui_layer_picker previous = ui->layer_picker;
    ui->layer_picker = (pc_ui_layer_picker){.armed = true,
                                            .layer = state->active_layer,
                                            .document = pc_doc_arc_get(state->doc)->id,
                                            .point = (unsigned)point,
                                            .tool = ui->tool};
    if (!pc_ui_layer_picker_armed(session, ui)) {
        ui->layer_picker = previous;
        return pc_fail(err, PC_ERR_INVALID, "this adjustment or document mode does not support that picker");
    }
    return PC_OK;
}
bool pc_ui_layer_picker_cancel(pc_ui_state *ui)
{
    if (!ui)
        return false;
    bool armed = ui->layer_picker.armed;
    ui->layer_picker = (pc_ui_layer_picker){0};
    return armed;
}
pc_status pc_ui_layer_picker_pointer(pc_engine_session *session, pc_ui_state *ui, pc_ui_pointer_kind kind,
                                     pc_ui_tool_point point, pc_json_value *out, pc_error *err)
{
    if (!session || !ui || !out || (unsigned)kind > PC_UI_POINTER_CANCEL)
        return pc_fail(err, PC_ERR_INVALID, "invalid adjustment picker context");
    pc_ui_layer_picker_prune(session, ui);
    if (!pc_ui_layer_picker_armed(session, ui) || ui->n_dialogs)
        return PC_ERR_NOT_FOUND;
    if (kind == PC_UI_POINTER_CANCEL) {
        ui->layer_picker.down = false;
        return PC_OK;
    }
    if (kind == PC_UI_POINTER_UP) {
        ui->layer_picker.down = false;
        return PC_OK;
    }
    if (kind != PC_UI_POINTER_DOWN)
        return PC_OK;
    ui->layer_picker.down = true;
    if (!isfinite(point.x) || !isfinite(point.y))
        return pc_fail(err, PC_ERR_INVALID, "nonfinite adjustment sample point");
    char text[240];
    (void)snprintf(text, sizeof text,
                   "{\"layer\":%llu,\"eyedropper\":{\"point\":\"%s\",\"at\":[%.9g,%.9g],\"size\":%u}}",
                   (unsigned long long)ui->layer_picker.layer, POINTS[ui->layer_picker.point],
                   (double)point.x, (double)point.y, ui->tool_options.eyedropper_size);
    return pc_engine_session_execute_json(session, "layer.setAdjustment", text, out, err);
}
pc_status pc_ui_layer_picker_to_json(const pc_ui_layer_picker *picker, yyjson_mut_doc *doc,
                                     yyjson_mut_val **out, pc_error *err)
{
    if (!picker || !doc || !out)
        return pc_fail(err, PC_ERR_INVALID, "invalid picker state");
    if (!picker->armed) {
        *out = yyjson_mut_null(doc);
        return *out ? PC_OK : pc_fail(err, PC_ERR_NOMEM, "picker state");
    }
    if (picker->point > 2 || !picker->layer || !pc_ui_tool_serde(picker->tool))
        return pc_fail(err, PC_ERR_INVALID, "invalid armed picker state");
    yyjson_mut_val *value = yyjson_mut_obj(doc);
    if (!value || !yyjson_mut_obj_add_uint(doc, value, "layer", picker->layer) ||
        !yyjson_mut_obj_add_str(doc, value, "point", POINTS[picker->point]) ||
        !yyjson_mut_obj_add_str(doc, value, "tool", pc_ui_tool_serde(picker->tool)))
        return pc_fail(err, PC_ERR_NOMEM, "picker state");
    *out = value;
    return PC_OK;
}
pc_status pc_ui_layer_picker_from_json(yyjson_val *value, pc_ui_layer_picker *out, pc_error *err)
{
    if (!out)
        return pc_fail(err, PC_ERR_INVALID, "missing picker state output");
    if (!value || yyjson_is_null(value)) {
        *out = (pc_ui_layer_picker){0};
        return PC_OK;
    }
    uint64_t layer = 0;
    yyjson_val *p = pc_json_get(value, "point"), *t = pc_json_get(value, "tool");
    const char *point = yyjson_get_str(p), *tool = yyjson_get_str(t);
    pc_ui_tool selected;
    if (!yyjson_is_obj(value) || !pc_json_as_u64(pc_json_get(value, "layer"), &layer) || !layer || !point ||
        yyjson_get_len(p) != strlen(point) || !tool || yyjson_get_len(t) != strlen(tool) ||
        !pc_ui_tool_from_serde(tool, &selected))
        return pc_fail(err, PC_ERR_INVALID, "invalid saved picker state");
    unsigned index = 3;
    for (unsigned i = 0; i < 3; i++)
        if (!strcmp(point, POINTS[i]))
            index = i;
    if (index == 3)
        return pc_fail(err, PC_ERR_INVALID, "invalid saved picker point");
    *out = (pc_ui_layer_picker){.armed = true, .layer = layer, .point = index, .tool = selected};
    return PC_OK;
}
