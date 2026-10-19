#ifndef PC_UI_SHELL_LAYER_STYLE_UI_H
#define PC_UI_SHELL_LAYER_STYLE_UI_H
#include "dialogs.h"
bool pc_ui_shell_style_command(const char *command, const char **kind);
pc_status pc_ui_shell_style_open(pc_ui_shell_app *app, const char *kind, pc_json_value *out, pc_error *err);
void pc_ui_shell_style_blend_if(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime, pc_ui_dialog *dialog);
void pc_ui_shell_style_contour(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime, pc_ui_dialog *dialog,
                               yyjson_mut_val *entry, const char *key);
void pc_ui_shell_style_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime, pc_ui_dialog *dialog);
#endif
