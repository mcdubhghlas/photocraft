#ifndef PC_UI_LAYER_DROP_H
#define PC_UI_LAYER_DROP_H
#include "doc/doc.h"
typedef enum pc_ui_layer_drop_position {
    PC_UI_LAYER_ABOVE,
    PC_UI_LAYER_BELOW,
    PC_UI_LAYER_INTO
} pc_ui_layer_drop_position;
pc_ui_layer_drop_position pc_ui_layer_drop_zone(bool group, float fraction);
const char *pc_ui_layer_drop_name(pc_ui_layer_drop_position position);
bool pc_ui_layer_drop_allowed(const pc_document *doc, const pc_layer_id *layers, size_t count,
                              pc_layer_id target, pc_ui_layer_drop_position position, bool copy);
#endif
