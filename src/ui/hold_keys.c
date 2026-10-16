/* Held temporary tools: pure classification (port of hold_keys.rs, input-free half). */
#include "ui/hold_keys.h"

#include <string.h>

#include "engine/prefs.h"

const pc_ui_temporary PC_UI_TEMPORARY_BOUND[3] = {PC_UI_TEMP_HAND, PC_UI_TEMP_ZOOM_IN,
                                                  PC_UI_TEMP_ZOOM_OUT};

const char *pc_ui_temporary_id(pc_ui_temporary t)
{
    switch (t) {
    case PC_UI_TEMP_HAND:
        return "tools.temporary.hand";
    case PC_UI_TEMP_ZOOM_IN:
        return "tools.temporary.zoomIn";
    case PC_UI_TEMP_ZOOM_OUT:
        return "tools.temporary.zoomOut";
    case PC_UI_TEMP_MOVE:
        return "tools.temporary.move";
    }
    return "";
}

pc_ui_tool pc_ui_temporary_tool(pc_ui_temporary t)
{
    switch (t) {
    case PC_UI_TEMP_HAND:
        return PC_TOOL_HAND;
    case PC_UI_TEMP_ZOOM_IN:
    case PC_UI_TEMP_ZOOM_OUT:
        return PC_TOOL_ZOOM;
    case PC_UI_TEMP_MOVE:
        return PC_TOOL_MOVE;
    }
    return PC_TOOL_MOVE;
}

bool pc_ui_is_shape_tool(pc_ui_tool t)
{
    switch (t) {
    case PC_TOOL_RECTANGLE:
    case PC_TOOL_ELLIPSE_SHAPE:
    case PC_TOOL_TRIANGLE:
    case PC_TOOL_POLYGON:
    case PC_TOOL_LINE:
    case PC_TOOL_CUSTOM_SHAPE:
        return true;
    default:
        return false;
    }
}

bool pc_ui_cmd_moves(pc_ui_tool t)
{
    if (pc_ui_tool_is_type(t) || pc_ui_is_shape_tool(t)) {
        return false;
    }
    switch (t) {
    case PC_TOOL_MOVE:
    case PC_TOOL_HAND:
    case PC_TOOL_ROTATE_VIEW:
    case PC_TOOL_ZOOM:
    case PC_TOOL_CROP:
    case PC_TOOL_PERSPECTIVE_CROP:
    case PC_TOOL_SLICE:
    case PC_TOOL_SLICE_SELECT:
    case PC_TOOL_PATH_SELECTION:
    case PC_TOOL_DIRECT_SELECTION:
    case PC_TOOL_PEN:
    case PC_TOOL_FREEFORM_PEN:
    case PC_TOOL_CURVATURE_PEN:
    case PC_TOOL_ADD_ANCHOR_POINT:
    case PC_TOOL_DELETE_ANCHOR_POINT:
    case PC_TOOL_CONVERT_POINT:
    case PC_TOOL_RECT_MARQUEE:
    case PC_TOOL_ELLIPSE_MARQUEE:
    case PC_TOOL_LASSO:
    case PC_TOOL_POLYGON_LASSO:
    case PC_TOOL_MAGIC_WAND:
        return false;
    default:
        return true;
    }
}

bool pc_ui_erases_while_held(pc_ui_tool t)
{
    return t == PC_TOOL_BRUSH || t == PC_TOOL_PENCIL;
}

bool pc_ui_repositions(pc_ui_tool t)
{
    switch (t) {
    case PC_TOOL_RECT_MARQUEE:
    case PC_TOOL_ELLIPSE_MARQUEE:
    case PC_TOOL_LASSO:
    case PC_TOOL_OBJECT_SELECTION:
        return true;
    default:
        return pc_ui_is_shape_tool(t);
    }
}

bool pc_ui_is_temporary(const char *id)
{
    if (id == NULL) {
        return false;
    }
    size_t n = 0;
    const pc_engine_temporary_tool *tt = pc_engine_temporary_tools(&n);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(tt[i].id, id) == 0) {
            return true;
        }
    }
    return false;
}
