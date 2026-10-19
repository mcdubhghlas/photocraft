#ifndef PC_UI_SHELL_SAMPLING_UI_H
#define PC_UI_SHELL_SAMPLING_UI_H
#include "shell_internal.h"
#include "ui/sampling_tool.h"
pc_ui_sampling_tool *pc_ui_shell_sampling_runtime(pc_ui_shell_app *app);
bool pc_ui_shell_sampling_event(pc_ui_shell_app *app, const SDL_Event *event);
bool pc_ui_shell_sampling_canvas(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, const SDL_Event *event,
                                 float factor);
void pc_ui_shell_sampling_draw(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf);
void pc_ui_shell_sampling_options(pc_ui_shell_app *app);
#endif
