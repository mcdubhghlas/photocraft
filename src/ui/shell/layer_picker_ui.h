#ifndef PC_UI_SHELL_LAYER_PICKER_UI_H
#define PC_UI_SHELL_LAYER_PICKER_UI_H
#include "shell_internal.h"
#include "ui/layer_picker.h"
bool pc_ui_shell_layer_picker_event(pc_ui_shell_app *app, const SDL_Event *event);
bool pc_ui_shell_layer_picker_canvas(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf,
                                     const SDL_Event *event, float factor);
void pc_ui_shell_layer_picker_controls(pc_ui_shell_app *app, const pc_layer *layer);
pc_status pc_ui_shell_layer_picker_command(pc_ui_shell_app *app, const char *id, yyjson_val *params,
                                           pc_json_value *out, pc_error *err);
#endif
