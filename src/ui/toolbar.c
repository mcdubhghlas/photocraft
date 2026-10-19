#include "toolbar.h"
#define T(name) PC_TOOL_##name
static const struct {
    unsigned section, count;
    pc_ui_tool tools[6];
} slots[] = {{0, 2, {T(MOVE), T(ARTBOARD)}},
             {1, 4, {T(RECT_MARQUEE), T(ELLIPSE_MARQUEE), T(SINGLE_ROW_MARQUEE), T(SINGLE_COLUMN_MARQUEE)}},
             {1, 3, {T(LASSO), T(POLYGON_LASSO), T(MAGNETIC_LASSO)}},
             {1, 3, {T(OBJECT_SELECTION), T(QUICK_SELECTION), T(MAGIC_WAND)}},
             {1, 4, {T(CROP), T(PERSPECTIVE_CROP), T(SLICE), T(SLICE_SELECT)}},
             {1, 1, {T(FRAME)}},
             {1, 5, {T(EYEDROPPER), T(COLOR_SAMPLER), T(RULER), T(NOTE), T(COUNT_TOOL)}},
             {2, 6, {T(REMOVE), T(SPOT_HEALING), T(HEALING), T(PATCH), T(CONTENT_AWARE_MOVE), T(RED_EYE)}},
             {2, 4, {T(BRUSH), T(PENCIL), T(MIXER_BRUSH), T(COLOR_REPLACEMENT)}},
             {2, 2, {T(CLONE_STAMP), T(PATTERN_STAMP)}},
             {2, 1, {T(HISTORY_BRUSH)}},
             {2, 3, {T(ERASER), T(BACKGROUND_ERASER), T(MAGIC_ERASER)}},
             {2, 2, {T(GRADIENT), T(PAINT_BUCKET)}},
             {2, 3, {T(BLUR), T(SHARPEN), T(SMUDGE)}},
             {2, 3, {T(DODGE), T(BURN), T(SPONGE)}},
             {3,
              6,
              {T(PEN), T(FREEFORM_PEN), T(CURVATURE_PEN), T(ADD_ANCHOR_POINT), T(DELETE_ANCHOR_POINT),
               T(CONVERT_POINT)}},
             {3, 4, {T(TYPE), T(VERTICAL_TYPE), T(VERTICAL_TYPE_MASK), T(HORIZONTAL_TYPE_MASK)}},
             {3, 2, {T(PATH_SELECTION), T(DIRECT_SELECTION)}},
             {3, 6, {T(RECTANGLE), T(ELLIPSE_SHAPE), T(TRIANGLE), T(POLYGON), T(LINE), T(CUSTOM_SHAPE)}},
             {4, 2, {T(HAND), T(ROTATE_VIEW)}},
             {4, 1, {T(ZOOM)}}};
#undef T
size_t pc_ui_toolbar_slots(void)
{
    return sizeof slots / sizeof slots[0];
}
const pc_ui_tool *pc_ui_toolbar_slot(size_t slot, size_t *count, unsigned *section)
{
    if (count) {
        *count = slot < pc_ui_toolbar_slots() ? slots[slot].count : 0;
    }
    if (section) {
        *section = slot < pc_ui_toolbar_slots() ? slots[slot].section : 0;
    }
    return slot < pc_ui_toolbar_slots() ? slots[slot].tools : NULL;
}
pc_ui_tool pc_ui_toolbar_current(const pc_ui_state *state, size_t slot)
{
    size_t n = 0;
    const pc_ui_tool *tools = pc_ui_toolbar_slot(slot, &n, NULL);
    if (!n) {
        return PC_TOOL_MOVE;
    }
    for (size_t i = 0; i < n; i++) {
        if (tools[i] == state->tool) {
            return state->tool;
        }
    }
    for (size_t r = state->n_group_tools; r > 0; r--) {
        for (size_t i = 0; i < n; i++) {
            if (tools[i] == state->group_tools[r - 1]) {
                return tools[i];
            }
        }
    }
    return tools[0];
}
