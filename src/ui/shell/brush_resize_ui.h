#ifndef PC_UI_SHELL_BRUSH_RESIZE_H
#define PC_UI_SHELL_BRUSH_RESIZE_H
#include "canvas_view.h"
bool pc_ui_brush_resize_event(pc_ui_shell_app *app, pc_ui_canvas_view *canvas, const pc_ui_canvas_xform *xf,
                              const SDL_Event *event, float factor);
void pc_ui_brush_resize_draw(pc_ui_shell_app *app, pc_ui_canvas_view *canvas);
#endif
