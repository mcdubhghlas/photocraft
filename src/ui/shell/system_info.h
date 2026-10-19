#ifndef PC_UI_SHELL_SYSTEM_INFO_H
#define PC_UI_SHELL_SYSTEM_INFO_H
#include "dialogs.h"
pc_status pc_ui_shell_system_info(pc_ui_shell_app *app, pc_json_value *out, pc_error *err);
pc_status pc_ui_shell_system_info_open(pc_ui_shell_app *app, pc_json_value *out, pc_error *err);
void pc_ui_shell_system_info_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime, pc_ui_dialog *dialog);
#endif
