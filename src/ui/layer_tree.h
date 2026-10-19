/* Borrowed rows in top-to-bottom Layers panel order. No GUI or document mutation. */
#ifndef PC_UI_LAYER_TREE_H
#define PC_UI_LAYER_TREE_H
#include "doc/doc.h"
typedef struct pc_ui_layer_row {
    const pc_layer *layer;
    unsigned depth;
    bool ancestor_hidden;
} pc_ui_layer_row;
typedef struct pc_ui_layer_rows {
    pc_ui_layer_row *items;
    size_t count, capacity;
} pc_ui_layer_rows;
void pc_ui_layer_rows_fini(pc_ui_layer_rows *rows);
const char *pc_ui_layer_filter_kind(pc_layer_kind kind);
/* An empty filter lists all kinds. Active filters search inside closed groups. `omit` is the
 * transient working Type Mask layer, or NULL. Replaces the output only after a complete walk. */
pc_status pc_ui_layer_rows_build(const pc_document *doc, const char *const *filter, size_t count,
                                 const pc_layer_id *omit, pc_ui_layer_rows *out, pc_error *err);
#endif
