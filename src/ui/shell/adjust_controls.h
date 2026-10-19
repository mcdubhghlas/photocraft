#ifndef PC_UI_SHELL_ADJUST_CONTROLS_H
#define PC_UI_SHELL_ADJUST_CONTROLS_H
#include "dialogs.h"
void pc_ui_shell_adjust_gradient(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime, pc_ui_dialog *dialog);
void pc_ui_shell_adjust_controls(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime, pc_ui_dialog *dialog);
bool pc_ui_shell_adjust_owns(const char *command, const char *parameter);
#endif
