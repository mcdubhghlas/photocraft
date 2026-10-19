#include "ui/rasterize.h"
#include "engine/channel_display.h"
#include <string.h>
static const pc_layer *editable(const pc_engine_session *s)
{
    const pc_engine_doc_state *state = pc_engine_session_active(s);
    if (!state || !state->has_active_layer)
        return NULL;
    const pc_document *doc = pc_doc_arc_get(state->doc);
    pc_locks locks = pc_document_effective_locks(doc, state->active_layer);
    return locks.all || locks.pixels ? NULL : pc_document_layer(doc, state->active_layer);
}
const pc_ui_rasterize_kind *pc_ui_rasterize_needed(const pc_engine_session *s, const pc_ui_state *ui)
{
    bool paints = pc_ui_tool_is_brushlike(ui->tool) || ui->tool == PC_TOOL_PAINT_BUCKET ||
                  ui->tool == PC_TOOL_MAGIC_ERASER || ui->tool == PC_TOOL_RED_EYE ||
                  (ui->tool == PC_TOOL_GRADIENT && ui->tool_options.gradient_classic);
    if (!paints || ui->mask_target || ui->vector_mask_target || pc_engine_targeted_mask(s) ||
        pc_engine_targeted_channel(s, NULL, NULL) || pc_engine_session_tools(s)->mask_targeted)
        return NULL;
    const pc_layer *layer = editable(s);
    if (!layer)
        return NULL;
    static const pc_ui_rasterize_kind
        type = {"layer.rasterize.type", "This type layer must be rasterized before proceeding. Its text will "
                                        "no longer be editable. Rasterize the type?"},
        shape = {"layer.rasterize.shape", "This shape layer must be rasterized before proceeding. It will no "
                                          "longer be editable as a shape. Rasterize the shape?"},
        smart = {"layer.rasterize.smartObject",
                 "This Smart Object must be rasterized before proceeding. Its contents will no longer be "
                 "editable. Rasterize the Smart Object?"},
        fill = {"layer.rasterize.fillContent",
                "This fill layer must be rasterized before proceeding. Its fill content will no longer be "
                "editable. Rasterize the layer?"};
    switch (layer->content.kind) {
    case PC_LAYER_TEXT:
        return &type;
    case PC_LAYER_SHAPE:
        return &shape;
    case PC_LAYER_SMART:
        return &smart;
    case PC_LAYER_FILL:
        return &fill;
    default:
        return NULL;
    }
}
bool pc_ui_rasterize_filter_needed(const pc_engine_session *s, const char *command, bool *smart)
{
    if (!command || strncmp(command, "filter.", 7))
        return false;
    const pc_layer *layer = editable(s);
    if (!layer || layer->content.kind != PC_LAYER_TEXT)
        return false;
    pc_error reason = {0};
    if (!pc_engine_session_disabled_reason(s, command, &reason) || !strstr(reason.msg, "Type layer") ||
        !strstr(reason.msg, "pixel layer"))
        return false;
    if (smart)
        *smart = !strncmp(reason.msg, "filters need a pixel layer", 26);
    return true;
}
