#ifndef PC_UI_SHELL_COMMAND_FORM_H
#define PC_UI_SHELL_COMMAND_FORM_H
#include "dialogs.h"
bool pc_ui_shell_form_command(const char *command);
pc_status pc_ui_shell_form_open(pc_ui_shell_app *app, const char *command, pc_json_value *out, pc_error *err);
void pc_ui_shell_form_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog);
#endif
