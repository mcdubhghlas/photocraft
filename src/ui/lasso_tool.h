#ifndef PC_UI_LASSO_TOOL_H
#define PC_UI_LASSO_TOOL_H
#include "ui/tool_gesture.h"
#include "ui/lasso.h"
#include "ui/shortcuts.h"
bool pc_ui_lasso_tool(pc_ui_tool tool);
void pc_ui_lasso_sync(pc_engine_session *session, pc_ui_state *ui);
pc_status pc_ui_lasso_pointer(pc_engine_session *session, pc_ui_state *ui, pc_ui_pointer_kind kind,
                              pc_ui_tool_point point, unsigned mods, double zoom, pc_json_value *out,
                              pc_error *err);
pc_status pc_ui_lasso_key(pc_engine_session *session, pc_ui_state *ui, pc_ui_key key, unsigned mods,
                          double zoom, pc_json_value *out, pc_error *err);
pc_status pc_ui_lasso_finish(pc_engine_session *session, pc_ui_state *ui, bool straight, pc_json_value *out,
                             pc_error *err);
pc_status pc_ui_lasso_retract(pc_engine_session *session, pc_ui_state *ui, pc_json_value *out, pc_error *err);
bool pc_ui_lasso_replaces_selection(const pc_ui_state *ui);
pc_status pc_ui_lasso_to_json(const pc_ui_lasso *lasso, yyjson_mut_doc *doc, yyjson_mut_val **out,
                              pc_error *err);
pc_status pc_ui_lasso_from_json(yyjson_val *value, pc_ui_lasso *out, pc_error *err);
#endif
