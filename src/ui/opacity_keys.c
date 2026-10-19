#include "ui/opacity_keys.h"

pc_ui_opacity_change pc_ui_opacity_press(pc_ui_opacity_pending *p, pc_ui_tool tool, unsigned digit,
                                         bool shift, bool repeat, uint64_t now)
{
    pc_ui_opacity_change out = {0};
    if (!p || digit > 9 || repeat)
        return out;
    unsigned percent;
    if (p->pending && p->shift == shift && now >= p->time && now - p->time < 800) {
        percent = p->digit * 10 + digit;
        p->pending = false;
    } else {
        *p = (pc_ui_opacity_pending){.pending = true, .shift = shift, .digit = digit, .time = now};
        percent = digit ? digit * 10 : 100;
    }
    if (tool == PC_TOOL_BRUSH || tool == PC_TOOL_ERASER || tool == PC_TOOL_PENCIL) {
        if (!shift)
            out.target = PC_UI_OPACITY_BRUSH;
        else if (tool != PC_TOOL_PENCIL)
            out.target = PC_UI_OPACITY_FLOW;
    } else if (tool == PC_TOOL_GRADIENT || tool == PC_TOOL_PAINT_BUCKET) {
        if (!shift)
            out.target = PC_UI_OPACITY_TOOL;
    } else if (!pc_ui_tool_is_brushlike(tool) && tool != PC_TOOL_MAGIC_ERASER) {
        out.target = shift ? PC_UI_OPACITY_FILL : PC_UI_OPACITY_LAYER;
    }
    out.value = (float)percent / 100;
    if (out.target == PC_UI_OPACITY_FLOW && percent == 0)
        out.value = .01f;
    return out;
}
