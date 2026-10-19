#ifndef PC_UI_PEN_STATE_H
#define PC_UI_PEN_STATE_H
#include "base/json.h"
typedef struct pc_ui_pen_knot {
    double points[3][2];
    bool unlinked;
} pc_ui_pen_knot;
typedef struct pc_ui_pen_sample {
    double point[2];
    bool straight;
} pc_ui_pen_sample;
typedef struct pc_ui_pen {
    pc_ui_pen_knot *knots;
    size_t count, capacity;
    bool curvature, dragging, adjusting_last, has_moving;
    size_t moving;
    /* Runtime-only capture, absent from workspace JSON. */
    bool freeform, has_layer, mask_target, has_hover, alt;
    uint64_t document, revision, layer, generation;
    float zoom;
    double hover[2];
    pc_ui_pen_sample *trace;
    size_t ntrace, trace_capacity;
} pc_ui_pen;
void pc_ui_pen_fini(pc_ui_pen *pen);
pc_status pc_ui_pen_to_json(const pc_ui_pen *pen, yyjson_mut_doc *doc, yyjson_mut_val **out, pc_error *err);
pc_status pc_ui_pen_from_json(pc_ui_pen *pen, yyjson_val *object, pc_error *err);
#endif
