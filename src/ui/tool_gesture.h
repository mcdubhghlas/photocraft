/* Shared document-coordinate pointer routing for native input and automation. */
#ifndef PC_UI_TOOL_GESTURE_H
#define PC_UI_TOOL_GESTURE_H
#include "ui/state.h"
#include "engine/engine.h"
#include "engine/live_stroke.h"
typedef enum pc_ui_pointer_kind {
    PC_UI_POINTER_DOWN,
    PC_UI_POINTER_MOVE,
    PC_UI_POINTER_UP,
    PC_UI_POINTER_CANCEL
} pc_ui_pointer_kind;
typedef struct pc_ui_tool_point {
    float x, y, pressure, tilt_x, tilt_y, rotation, time_ms, wheel;
} pc_ui_tool_point;
typedef struct pc_ui_tool_gesture {
    bool active;
    bool has_shape_preview;
    pc_shape_layer shape_preview;
    unsigned modifiers;
    pc_engine_live_stroke *live;
    pc_doc_arc *move_preview;
    pc_layer_id preview_layer;
    bool move_pixels, float_pixels, move_copy, move_has_last;
    unsigned handoff;
    pc_ui_tool handoff_tool;
    double move_last[2];
    uint64_t seed, preview_revision, preview_domain;
    bool has_seed;
    pc_ui_tool tool;
    pc_doc_id document;
    pc_layer_id layer;
    pc_layer_id *selected;
    size_t nselected;
    bool has_layer;
    uint64_t revision;
    unsigned target;
    size_t alpha, color;
    pc_buf brush;
    float colors[2][4];
    unsigned selection_mode;
    float zoom;
    pc_ui_tool_options options;
    pc_ui_tool_point *points;
    size_t count, capacity;
} pc_ui_tool_gesture;
pc_status pc_ui_tool_gesture_selection_event(pc_ui_tool_gesture *gesture, pc_engine_session *session,
                                             const pc_ui_state *ui, unsigned handoff, pc_ui_pointer_kind kind,
                                             pc_ui_tool_point point, unsigned modifiers, float zoom,
                                             pc_json_value *out, pc_error *err);
void pc_ui_tool_gesture_fini(pc_ui_tool_gesture *gesture);
bool pc_ui_tool_gesture_supported(pc_ui_tool tool);
pc_status pc_ui_tool_gesture_event(pc_ui_tool_gesture *gesture, pc_engine_session *session,
                                   const pc_ui_state *ui, pc_ui_pointer_kind kind, pc_ui_tool_point point,
                                   unsigned modifiers, float zoom, pc_json_value *out, pc_error *err);
#endif
