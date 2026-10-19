#include "ui/brush_resize.h"
#include "paint/brush.h"
#include <math.h>
bool pc_ui_brush_resize_applies(pc_ui_tool tool)
{
    return pc_ui_tool_is_brushlike(tool) || tool == PC_TOOL_QUICK_SELECTION;
}
void pc_ui_brush_resized(float size, float value, float dx, float dy, float zoom, float out[2])
{
    if (!out)
        return;
    if (!isfinite(size))
        size = 1;
    if (!isfinite(value))
        value = 0;
    if (!isfinite(dx))
        dx = 0;
    if (!isfinite(dy))
        dy = 0;
    if (!isfinite(zoom) || zoom <= 0)
        zoom = 1;
    double diameter = (double)size + 2 * (double)dx / zoom;
    out[0] = (float)fmin(PC_PAINT_MAX_BRUSH_SIZE, fmax(1, round(diameter)));
    out[1] = (float)(round(fmin(1, fmax(0, (double)value + dy / 200.0)) * 100) / 100);
}
