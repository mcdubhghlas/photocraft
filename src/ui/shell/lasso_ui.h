#ifndef PC_UI_SHELL_LASSO_UI_H
#define PC_UI_SHELL_LASSO_UI_H
#include "shell_internal.h"
#include "ui/lasso_tool.h"
bool pc_ui_shell_lasso_event(pc_ui_shell_app *app, const SDL_Event *event);
bool pc_ui_shell_lasso_canvas(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, const SDL_Event *event,
                              float factor);
void pc_ui_shell_lasso_draw(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf);
void pc_ui_shell_lasso_options(pc_ui_shell_app *app);
pc_status pc_ui_shell_lasso_command(pc_ui_shell_app *app, const char *id, pc_json_value *out, pc_error *err);
#endif
