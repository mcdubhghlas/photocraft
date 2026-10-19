#ifndef PC_UI_SHELL_PREFERENCES_H
#define PC_UI_SHELL_PREFERENCES_H
#include "dialogs.h"
bool pc_ui_shell_is_preferences(yyjson_mut_val *fields);
pc_status pc_ui_shell_preferences_open(pc_ui_shell_app *app, const char *section, pc_json_value *out,
                                       pc_error *err);
pc_status pc_ui_shell_preferences_commit(pc_ui_shell_app *app, uint64_t id, bool close, pc_json_value *out,
                                         pc_error *err);
void pc_ui_shell_preferences_draw(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime, pc_ui_dialog *dialog);
#endif
