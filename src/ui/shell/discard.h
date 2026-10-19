#ifndef PC_UI_SHELL_DISCARD_H
#define PC_UI_SHELL_DISCARD_H
#include "dialogs.h"
bool pc_ui_shell_is_discard(yyjson_mut_val *fields);
pc_status pc_ui_shell_discard_intercept(pc_ui_shell_app *app, const char *command, yyjson_val *params,
                                        pc_json_value *out, pc_error *err);
pc_status pc_ui_shell_discard_confirm(pc_ui_shell_app *app, uint64_t dialog, pc_json_value *out,
                                      pc_error *err);
void pc_ui_shell_discard_poll(pc_ui_shell_app *app);
#endif
