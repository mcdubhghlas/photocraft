#ifndef PC_UI_SHELL_FILTER_UI_H
#define PC_UI_SHELL_FILTER_UI_H
#include "dialogs.h"
pc_status pc_ui_shell_filter_open(pc_ui_shell_app *app, const char *command, pc_json_value *out,
                                  pc_error *err);
void pc_ui_shell_filter_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime, pc_ui_dialog *dialog);
void pc_ui_shell_parameter_label(const char *key, char out[128]);
void pc_ui_shell_filter_fields(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime, pc_ui_dialog *dialog,
                               const char *command);
#endif
