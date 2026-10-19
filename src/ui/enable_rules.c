#include "enable_rules.h"
#include "engine/channel_display.h"
#include "compose/effects.h"
#include <string.h>
#include <stdlib.h>
static bool eq(const char *a, const char *b)
{
    return !strcmp(a, b);
}
static bool starts(const char *a, const char *b)
{
    return !strncmp(a, b, strlen(b));
}
static bool transform(const char *id)
{
    return eq(id, "edit.freeTransform") || (starts(id, "edit.transform.") && !eq(id, "edit.transform.again"));
}
static bool background_blocks(const char *id, bool selection)
{
    if (starts(id, "layer.layerMask.") || starts(id, "layer.vectorMask.") || starts(id, "layer.arrange.") ||
        starts(id, "layer.matting.")) {
        return true;
    }
    if (starts(id, "layer.layerStyle.")) {
        return !eq(id, "layer.layerStyle.globalLight") && !eq(id, "layer.layerStyle.hideAllEffects") &&
               !eq(id, "layer.layerStyle.copyLayerStyle");
    }
    if (!selection && transform(id)) {
        return true;
    }
    static const char *const ids[] = {
        "layer.createClippingMask", "layer.releaseClippingMask", "layer.mergeDown", "layer.lockLayers",
        "layer.maskAllObjects",     "edit.contentAwareScale",    "edit.puppetWarp", "edit.perspectiveWarp"};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        if (eq(id, ids[i])) {
            return true;
        }
    }
    return false;
}
static bool kind_blocks(const pc_layer *layer, const char *id)
{
    pc_layer_kind kind = layer->content.kind;
    if (eq(id, "layer.rasterize.type") || eq(id, "layer.rasterize.rasterizeType")) {
        return kind != PC_LAYER_TEXT;
    }
    if (eq(id, "layer.rasterize.shape") || eq(id, "layer.rasterize.rasterizeShape")) {
        return kind != PC_LAYER_SHAPE;
    }
    if (eq(id, "layer.rasterize.fillContent")) {
        return kind != PC_LAYER_FILL;
    }
    if (eq(id, "layer.rasterize.smartObject")) {
        return kind != PC_LAYER_SMART;
    }
    if (eq(id, "layer.rasterize.vectorMask")) {
        return !layer->has_vector_mask;
    }
    if (eq(id, "layer.rasterize.layer")) {
        return !layer->has_vector_mask && kind != PC_LAYER_TEXT && kind != PC_LAYER_SHAPE &&
               kind != PC_LAYER_FILL && kind != PC_LAYER_SMART;
    }
    if (eq(id, "layer.rasterize.layerStyle")) {
        return !pc_compose_has_effects(layer) || kind == PC_LAYER_GROUP || kind == PC_LAYER_ADJUSTMENT;
    }
    if (eq(id, "layer.layerStyle.copyLayerStyle") || eq(id, "layer.layerStyle.clear")) {
        return !layer->effects.nitems;
    }
    return starts(id, "layer.combineShapes.") && kind != PC_LAYER_SHAPE;
}
bool pc_ui_disabled_for_layer(const pc_document *doc, const pc_layer *layer, const char *id)
{
    if (!doc || !id) {
        return false;
    }
    bool only_background = doc->nlayers == 1 && pc_document_is_background(doc, doc->layers[0].id);
    if (only_background && (eq(id, "layer.mergeVisible") || eq(id, "layer.flattenImage") ||
                            eq(id, "layer.mergeLayers") || eq(id, "layer.rasterize.allLayers"))) {
        return true;
    }
    if (eq(id, "layer.delete.hiddenLayers")) {
        pc_walk walk = {0};
        if (pc_document_walk(doc, &walk, NULL) != PC_OK) {
            return true;
        }
        bool any = false;
        for (size_t i = 0; i < walk.len; i++) {
            if (!walk.items[i].layer->visible) {
                any = true;
                break;
            }
        }
        pc_walk_fini(&walk);
        if (!any) {
            return true;
        }
    }
    if (!layer) {
        return false;
    }
    if (pc_document_is_background(doc, layer->id) && background_blocks(id, doc->has_selection)) {
        return true;
    }
    return kind_blocks(layer, id);
}
static int selection_rule(const char *id)
{
    static const char *const any[] = {
        "layer.rasterize.layer",     "layer.rasterize.layerStyle",       "layer.rasterize.type",
        "layer.rasterize.shape",     "layer.rasterize.fillContent",      "layer.rasterize.smartObject",
        "layer.releaseClippingMask", "layer.layerStyle.pasteLayerStyle", "layer.layerStyle.clear"};
    for (size_t i = 0; i < sizeof any / sizeof any[0]; i++) {
        if (eq(id, any[i])) {
            return 1;
        }
    }
    if (eq(id, "layer.createClippingMask")) {
        return 2;
    }
    if (eq(id, "layer.layerMask.revealAll") || eq(id, "layer.layerMask.hideAll") ||
        eq(id, "layer.layerMask.revealSelection") || eq(id, "layer.layerMask.hideSelection")) {
        return 3;
    }
    return 0;
}
bool pc_ui_disabled_for_selection(const pc_document *doc, const pc_layer_id *selected, size_t count,
                                  const pc_layer *active, const char *id)
{
    int rule = id ? selection_rule(id) : 0;
    if (!rule || count <= 1 || !selected) {
        return pc_ui_disabled_for_layer(doc, active, id);
    }
    bool skipped = false;
    for (size_t i = 0; i < count; i++) {
        const pc_layer *layer = pc_document_layer(doc, selected[i]);
        if (!layer) {
            continue;
        }
        if (rule == 2 && !skipped) {
            skipped = true;
            continue;
        }
        if (!pc_ui_disabled_for_layer(doc, layer, id) && (rule != 3 || !layer->has_mask)) {
            return false;
        }
    }
    return true;
}
bool pc_ui_command_disabled(const pc_engine_session *session, const pc_ui_state *state, const char *id)
{
    const pc_engine_doc_state *active = pc_engine_session_active(session);
    if (!id) {
        return false;
    }
    if (state && (eq(id, "tools.decreaseBrushHardness") || eq(id, "tools.increaseBrushHardness")) &&
        !pc_ui_tool_is_brushlike(state->tool)) {
        return true;
    }
    if (state && state->transform.active && eq(id, "edit.freeTransformCopy"))
        return true;
    if (eq(id, "view.fitArtboardOnScreen")) {
        const pc_document *doc = active ? pc_doc_arc_get(active->doc) : NULL;
        if (doc)
            for (size_t i = 0; i < doc->nlayers; i++)
                if (pc_layer_artboard(&doc->layers[i]))
                    return false;
        return true;
    }
    if (!active) {
        return transform(id) || eq(id, "edit.freeTransformCopy") || eq(id, "type.editText") ||
               eq(id, "select.selectAndMask") || eq(id, "window.newWindowForDocument") ||
               eq(id, "window.nextDocument") || eq(id, "window.previousDocument") ||
               starts(id, "view.zoom") || eq(id, "view.fitOnScreen") || eq(id, "view.actualPixels") ||
               eq(id, "view.rotateView") || eq(id, "view.resetView");
    }
    const pc_document *doc = pc_doc_arc_get(active->doc);
    const pc_layer *layer = active->has_active_layer ? pc_document_layer(doc, active->active_layer) : NULL;
    if (eq(id, "type.editText")) {
        return !layer || layer->content.kind != PC_LAYER_TEXT;
    }
    if (transform(id)) {
        if (pc_engine_targeted_channel(session, NULL, NULL)) {
            return false;
        }
        if (state && (state->tool == PC_TOOL_PATH_SELECTION || state->tool == PC_TOOL_DIRECT_SELECTION)) {
            double bounds[4];
            const pc_path *path = doc->has_work_path ? &doc->work_path : NULL;
            if (state->selected_path && !strcmp(state->selected_path, "layer") && layer) {
                path = layer->has_vector_mask                  ? &layer->vector_mask.path
                       : layer->content.kind == PC_LAYER_SHAPE ? &layer->content.u.shape.path
                                                               : NULL;
            }
            if (path && pc_path_control_bounds(path, bounds)) {
                return false;
            }
        }
    }
    static const char *const type_blocks[] = {"edit.transform.distort",
                                              "edit.transform.perspective",
                                              "edit.transform.splitWarpCrosswise",
                                              "edit.transform.splitWarpHorizontally",
                                              "edit.transform.splitWarpVertically",
                                              "edit.transform.removeWarpSplit"};
    if (layer && layer->content.kind == PC_LAYER_TEXT && !pc_engine_targeted_channel(session, NULL, NULL) &&
        !(pc_engine_targeted_mask(session) && layer->has_mask && !layer->mask.linked)) {
        for (size_t i = 0; i < sizeof type_blocks / sizeof type_blocks[0]; i++) {
            if (eq(id, type_blocks[i])) {
                return true;
            }
        }
    }
    if (selection_rule(id) && active->nselected > 1) {
        pc_layer_id *selected = NULL;
        size_t count = 0;
        if (pc_engine_doc_state_selected_layers(active, &selected, &count, NULL) != PC_OK) {
            return true;
        }
        bool disabled = pc_ui_disabled_for_selection(doc, selected, count, layer, id);
        free(selected);
        return disabled;
    }
    return pc_ui_disabled_for_layer(doc, layer, id);
}
