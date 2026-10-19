#ifndef PC_UI_PEN_TOOL_H
#define PC_UI_PEN_TOOL_H
#include "ui/tool_gesture.h"
#include "ui/shortcuts.h"
bool pc_ui_pen_tool(pc_ui_tool tool);
bool pc_ui_pen_drawn_with(const pc_ui_pen *pen, pc_ui_tool tool);
void pc_ui_pen_cancel(pc_ui_state *ui);
pc_status pc_ui_pen_path(const pc_ui_pen *pen, bool closed, pc_path *out, pc_error *err);
pc_status pc_ui_pen_commit(pc_engine_session *session, pc_ui_state *ui, bool closed, pc_json_value *out,
                           pc_error *err);
pc_status pc_ui_pen_pointer(pc_engine_session *session, pc_ui_state *ui, pc_ui_pointer_kind kind,
                            pc_ui_tool_point point, unsigned modifiers, float zoom, pc_json_value *out,
                            pc_error *err);
pc_status pc_ui_pen_key(pc_engine_session *session, pc_ui_state *ui, pc_ui_key key, unsigned modifiers,
                        pc_json_value *out, pc_error *err);
pc_status pc_ui_pen_curvature_toggle(pc_ui_state *ui, const float point[2], float zoom, pc_error *err);
#endif
