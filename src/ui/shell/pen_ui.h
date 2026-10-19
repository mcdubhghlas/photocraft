#ifndef PC_UI_SHELL_PEN_UI_H
#define PC_UI_SHELL_PEN_UI_H
#include "shell_internal.h"
#include "vector_preview.h"
bool pc_ui_shell_pen_canvas(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, const SDL_Event *event,
                            float factor);
bool pc_ui_shell_pen_input(pc_ui_shell_app *app, const SDL_Event *event);
pc_status pc_ui_shell_pen_draw(pc_ui_shell_app *app, pc_ui_vector_preview *cache,
                               const pc_ui_canvas_xform *xf, pc_error *err);
void pc_ui_shell_paths_panel(pc_ui_shell_app *app);
#endif
