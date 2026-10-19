#include "ui/layer_drop.h"
#include <string.h>
#include <math.h>
pc_ui_layer_drop_position pc_ui_layer_drop_zone(bool group, float fraction)
{
    if (!isfinite(fraction))
        return PC_UI_LAYER_ABOVE;
    if (group && fraction >= .3f && fraction < .7f)
        return PC_UI_LAYER_INTO;
    return fraction < .5f ? PC_UI_LAYER_ABOVE : PC_UI_LAYER_BELOW;
}
const char *pc_ui_layer_drop_name(pc_ui_layer_drop_position p)
{
    return p == PC_UI_LAYER_INTO ? "into" : p == PC_UI_LAYER_BELOW ? "below" : "above";
}
bool pc_ui_layer_drop_allowed(const pc_document *doc, const pc_layer_id *layers, size_t count,
                              pc_layer_id target, pc_ui_layer_drop_position p, bool copy)
{
    if (!doc || !layers || !count || count > 4096)
        return false;
    const pc_layer *destination = pc_document_layer(doc, target);
    if (!destination || (p == PC_UI_LAYER_INTO && !pc_layer_is_group(destination)) ||
        (p == PC_UI_LAYER_BELOW && pc_document_is_background(doc, target)))
        return false;
    pc_layer_path target_path = {0};
    if (pc_document_path_of(doc, target, &target_path, NULL) != PC_OK)
        return false;
    bool allowed = true;
    for (size_t i = 0; allowed && i < count; i++) {
        pc_layer_path path = {0};
        if (!copy && pc_document_is_background(doc, layers[i]))
            allowed = false;
        else if (pc_document_path_of(doc, layers[i], &path, NULL) != PC_OK)
            allowed = false;
        else if (!(copy && layers[i] == target && count == 1 && p != PC_UI_LAYER_INTO) && path.len &&
                 path.len <= target_path.len &&
                 !memcmp(path.idx, target_path.idx, path.len * sizeof *path.idx))
            allowed = false;
        pc_layer_path_fini(&path);
    }
    pc_layer_path_fini(&target_path);
    return allowed;
}
