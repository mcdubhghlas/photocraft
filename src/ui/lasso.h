#ifndef PC_UI_LASSO_H
#define PC_UI_LASSO_H
#include "engine/magnetic_source.h"
typedef struct pc_ui_lasso_points {
    double (*v)[2];
    size_t n, capacity;
} pc_ui_lasso_points;
typedef struct pc_ui_lasso {
    int tool;
    uint64_t document, revision;
    pc_layer_id layer;
    bool active, down, polygonal, initial_alt, alt_released, freehand;
    char mode[16];
    double cursor[2];
    pc_ui_lasso_points path, live, trail;
    size_t *anchors, nanchors, anchor_capacity;
    pc_engine_magnetic_source *source;
} pc_ui_lasso;
void pc_ui_lasso_fini(pc_ui_lasso *lasso);
#endif
