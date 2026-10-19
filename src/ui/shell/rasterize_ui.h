#ifndef PC_UI_SHELL_RASTERIZE_UI_H
#define PC_UI_SHELL_RASTERIZE_UI_H
#include "dialogs.h"
#include "ui/tool_gesture.h"
pc_status pc_ui_shell_rasterize_pointer(pc_ui_shell_app *app, pc_ui_tool_point point, pc_json_value *out,
                                        pc_error *err);
pc_status pc_ui_shell_rasterize_filter(pc_ui_shell_app *app, const char *command, yyjson_val *params,
                                       pc_json_value *out, pc_error *err);
pc_status pc_ui_shell_rasterize_confirm(pc_ui_shell_app *app, uint64_t id, pc_json_value *out, pc_error *err);
void pc_ui_shell_rasterize_body(pc_ui_shell_app *app, pc_ui_dialog *dialog);
#endif
