#ifndef PC_UI_SHELL_CANVAS_INK_H
#define PC_UI_SHELL_CANVAS_INK_H
#include "shell_internal.h"
#include "ui/canvas.h"
void pc_ui_canvas_ink_line(struct nk_command_buffer *canvas, const pc_ui_canvas_xform *xf, float x0, float y0,
                           float x1, float y1, float width, struct nk_color color);
bool pc_ui_canvas_ink_visible(const pc_ui_canvas_xform *xf, float x, float y, float padding);
void pc_ui_canvas_ink_dash(struct nk_command_buffer *canvas, const pc_ui_canvas_xform *xf, float x0, float y0,
                           float x1, float y1, float width, struct nk_color color, double dash, double gap,
                           double phase, size_t *budget);
#endif
