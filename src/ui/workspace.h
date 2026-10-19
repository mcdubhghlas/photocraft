#ifndef PC_UI_WORKSPACE_H
#define PC_UI_WORKSPACE_H
#include "ui/state.h"
bool pc_ui_workspace_builtin(const char *name);
pc_status pc_ui_workspace_snapshot(const pc_ui_state *state, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                   pc_error *err);
pc_status pc_ui_workspace_apply(pc_ui_state *state, const char *name, yyjson_val *saved, pc_error *err);
/* Transfer an already validated layout without allocation; other view/tool state is retained. */
void pc_ui_workspace_take(pc_ui_state *target, pc_ui_state *prepared);
#endif
