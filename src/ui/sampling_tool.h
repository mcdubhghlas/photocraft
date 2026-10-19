#ifndef PC_UI_SAMPLING_TOOL_H
#define PC_UI_SAMPLING_TOOL_H
#include "ui/tool_gesture.h"
typedef struct pc_ui_sampling_tool {
    bool active, has_sample, background;
    unsigned mode; /* 1 Eyedropper, 2 Color Sampler, 3 latched painting Alt sample */
    pc_ui_tool tool;
    uint64_t document, revision;
    size_t index;
    float point[2], start[2], sample[4], before[4];
} pc_ui_sampling_tool;
void pc_ui_sampling_sync(pc_ui_sampling_tool *runtime, const pc_engine_session *session,
                         const pc_ui_state *ui);
bool pc_ui_alt_samples_color(pc_ui_tool tool, unsigned modifiers);
bool pc_ui_sampling_tool_kind(pc_ui_tool tool);
pc_status pc_ui_sampling_pointer(pc_ui_sampling_tool *runtime, pc_engine_session *session,
                                 const pc_ui_state *ui, pc_ui_pointer_kind kind, pc_ui_tool_point point,
                                 unsigned modifiers, double zoom, pc_json_value *out, pc_error *err);
#endif
