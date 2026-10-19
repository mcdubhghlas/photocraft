#ifndef PC_UI_CONTROL_OPTIONS_H
#define PC_UI_CONTROL_OPTIONS_H
#include "ui/tool_options.h"
/* Materialize and validate the tool-option portion of ui.set without applying any changes. */
pc_status pc_ui_control_options_prepare(const pc_ui_tool_options *current, yyjson_val *params,
                                        pc_ui_tool_options *out, pc_error *err);
#endif
