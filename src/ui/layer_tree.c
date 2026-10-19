#include "ui/layer_tree.h"
#include "base/mem.h"
#include <stdlib.h>
#include <string.h>
void pc_ui_layer_rows_fini(pc_ui_layer_rows *rows)
{
    if (rows) {
        free(rows->items);
        *rows = (pc_ui_layer_rows){0};
    }
}
const char *pc_ui_layer_filter_kind(pc_layer_kind kind)
{
    switch (kind) {
    case PC_LAYER_RASTER:
        return "pixel";
    case PC_LAYER_ADJUSTMENT:
        return "adjustment";
    case PC_LAYER_TEXT:
        return "type";
    case PC_LAYER_SHAPE:
        return "shape";
    case PC_LAYER_SMART:
        return "smart";
    case PC_LAYER_GROUP:
        return "group";
    case PC_LAYER_FILL:
        return "fill";
    }
    return "";
}
static pc_status walk(const pc_layer *layers, size_t n, unsigned depth, bool hidden,
                      const char *const *filter, size_t count, const pc_layer_id *omit, pc_ui_layer_rows *out,
                      pc_error *err)
{
    if (depth > PC_MAX_GROUP_DEPTH) {
        return pc_fail(err, PC_ERR_LIMIT, "Layers panel nesting limit");
    }
    if (n && !layers) {
        return pc_fail(err, PC_ERR_INVALID, "missing Layers panel data");
    }
    for (size_t i = n; i-- > 0;) {
        const pc_layer *layer = &layers[i];
        bool shown = count == 0;
        const char *kind = pc_ui_layer_filter_kind(layer->content.kind);
        for (size_t k = 0; k < count; k++) {
            shown = shown || (filter[k] && strcmp(filter[k], kind) == 0);
        }
        if (shown && (!omit || *omit != layer->id)) {
            if (out->count == 100000) {
                return pc_fail(err, PC_ERR_LIMIT, "Layers panel row limit");
            }
            pc_ui_layer_row *grown = pc_grow_array(out->items, &out->capacity, out->count + 1, sizeof *grown);
            if (!grown) {
                return pc_fail(err, PC_ERR_NOMEM, "Layers panel rows");
            }
            out->items = grown;
            out->items[out->count++] = (pc_ui_layer_row){layer, depth, hidden};
        }
        if (pc_layer_is_group(layer) && (layer->content.u.group.expanded || count)) {
            pc_status rc = walk(layer->content.u.group.children, layer->content.u.group.nchildren, depth + 1,
                                hidden || !layer->visible, filter, count, omit, out, err);
            if (rc != PC_OK) {
                return rc;
            }
        }
    }
    return PC_OK;
}
pc_status pc_ui_layer_rows_build(const pc_document *doc, const char *const *filter, size_t count,
                                 const pc_layer_id *omit, pc_ui_layer_rows *out, pc_error *err)
{
    if (!doc || !out || (count && !filter) || count > 64) {
        return pc_fail(err, PC_ERR_INVALID, "invalid Layers panel filter");
    }
    pc_ui_layer_rows next = {0};
    pc_status rc = walk(doc->layers, doc->nlayers, 0, false, filter, count, omit, &next, err);
    if (rc != PC_OK) {
        pc_ui_layer_rows_fini(&next);
        return rc;
    }
    pc_ui_layer_rows_fini(out);
    *out = next;
    return PC_OK;
}
