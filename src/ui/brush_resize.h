#ifndef PC_UI_BRUSH_RESIZE_H
#define PC_UI_BRUSH_RESIZE_H
#include "ui/state.h"
bool pc_ui_brush_resize_applies(pc_ui_tool tool);
/* Screen-relative delta: horizontal movement is divided by the displayed document zoom;
 * vertical movement covers the complete hardness/opacity range in 200 UI points. */
void pc_ui_brush_resized(float size, float value, float dx, float dy, float zoom, float out[2]);
#endif
