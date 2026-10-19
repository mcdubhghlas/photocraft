#ifndef PC_UI_SHELL_ARTBOARD_UI_H
#define PC_UI_SHELL_ARTBOARD_UI_H
#include "shell_internal.h"
#include "ui/artboard.h"
pc_ui_artboard_layout *pc_ui_shell_artboard_layout(pc_ui_shell_app *app);
bool pc_ui_shell_artboard_event(pc_ui_shell_app *app, const SDL_Event *event);
bool pc_ui_shell_artboard_canvas(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, const SDL_Event *event,
                                 float factor);
void pc_ui_shell_artboard_pasteboard(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf);
void pc_ui_shell_artboard_frames(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf);
void pc_ui_shell_artboard_options(pc_ui_shell_app *app);
void pc_ui_shell_artboard_properties(pc_ui_shell_app *app, const pc_layer *layer);
pc_status pc_ui_shell_artboard_command(pc_ui_shell_app *app, const char *id, yyjson_val *params,
                                       pc_json_value *out, pc_error *err);
#endif
