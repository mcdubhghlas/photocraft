#ifndef PC_UI_SHELL_CAMERA_RAW_UI_H
#define PC_UI_SHELL_CAMERA_RAW_UI_H
#include "dialogs.h"
bool pc_ui_shell_camera_raw_hidden(pc_ui_dialog *dialog, const char *command, const char *key);
void pc_ui_shell_camera_raw_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime, pc_ui_dialog *dialog);
/* Camera Raw Save/Load Settings (gap B9): apply a chosen .xmp file to, or write one from, the
 * camera-raw dialog's current settings. For a load (saving == false) the applied values are written
 * back into the dialog fields. Never crashes on a malformed file (REWRITE_PLAN.md rule 9). */
pc_status pc_ui_shell_camera_raw_settings_file(pc_ui_shell_app *app, pc_ui_dialog *dialog, const char *path,
                                               bool saving, pc_error *err);
#endif
