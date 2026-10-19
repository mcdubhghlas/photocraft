#ifndef PC_UI_SHELL_CAMERA_RAW_UI_H
#define PC_UI_SHELL_CAMERA_RAW_UI_H
#include "dialogs.h"
bool pc_ui_shell_camera_raw_hidden(pc_ui_dialog *dialog, const char *command, const char *key);
void pc_ui_shell_camera_raw_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime, pc_ui_dialog *dialog);
#endif
