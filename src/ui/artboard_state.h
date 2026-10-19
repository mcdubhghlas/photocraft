#ifndef PC_UI_ARTBOARD_STATE_H
#define PC_UI_ARTBOARD_STATE_H
#include "geom/geom.h"
typedef struct pc_ui_artboard_tool {
    bool active, keep, has_active;
    unsigned kind; /* 0 new, 1 move, 2 resize */
    int hx, hy;
    uint64_t document, revision, layer, active_layer;
    pc_rect rect;
    double start[2], current[2];
} pc_ui_artboard_tool;
#endif
