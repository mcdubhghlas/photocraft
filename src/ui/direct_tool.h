#ifndef PC_UI_DIRECT_TOOL_H
#define PC_UI_DIRECT_TOOL_H
#include "ui/tool_gesture.h"
#include "ui/shortcuts.h"
bool pc_ui_direct_tool(pc_ui_tool tool);
bool pc_ui_direct_handles(const pc_ui_state *ui, unsigned modifiers);
const pc_path *pc_ui_direct_path(const pc_ui_direct *direct, const pc_engine_doc_state *state);
pc_status pc_ui_direct_pointer(pc_engine_session *session, pc_ui_state *ui, pc_ui_pointer_kind kind,
                               pc_ui_tool_point point, unsigned modifiers, float zoom, pc_json_value *out,
                               pc_error *err);
pc_status pc_ui_pen_auto(pc_engine_session *session, pc_ui_state *ui, pc_ui_tool_point point, float zoom,
                         pc_json_value *out, pc_error *err);
pc_status pc_ui_direct_key(pc_engine_session *session, pc_ui_state *ui, pc_ui_key key, unsigned modifiers,
                           pc_json_value *out, pc_error *err);
#endif
