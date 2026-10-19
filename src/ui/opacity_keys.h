#ifndef PC_UI_OPACITY_KEYS_H
#define PC_UI_OPACITY_KEYS_H
#include "ui/state.h"
typedef struct pc_ui_opacity_pending {
    bool pending, shift;
    unsigned digit;
    uint64_t time;
} pc_ui_opacity_pending;
typedef enum pc_ui_opacity_target {
    PC_UI_OPACITY_NONE,
    PC_UI_OPACITY_BRUSH,
    PC_UI_OPACITY_FLOW,
    PC_UI_OPACITY_TOOL,
    PC_UI_OPACITY_LAYER,
    PC_UI_OPACITY_FILL
} pc_ui_opacity_target;
typedef struct pc_ui_opacity_change {
    pc_ui_opacity_target target;
    float value; /* normalized, except ToolOptions which stores percent */
} pc_ui_opacity_change;
/* Physical digit key presses; repeat events are consumed without changing the value.
 * Two digits less than 800 ms apart produce an exact percentage. */
pc_ui_opacity_change pc_ui_opacity_press(pc_ui_opacity_pending *pending, pc_ui_tool tool, unsigned digit,
                                         bool shift, bool repeat, uint64_t now);
#endif
