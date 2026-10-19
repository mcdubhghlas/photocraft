#ifndef PC_UI_SLICE_TOOL_H
#define PC_UI_SLICE_TOOL_H
#include "ui/slice_state.h"
#include "ui/tool_gesture.h"
bool pc_ui_slice_tool(pc_ui_tool tool);
pc_status pc_ui_slice_resolve(pc_ui_slice_state *slice, const pc_engine_doc_state *state, pc_error *err);
const pc_resolved_slice *pc_ui_slice_selected(pc_ui_slice_state *slice);
const pc_resolved_slice *pc_ui_slice_hit(pc_ui_slice_state *slice, double x, double y);
pc_status pc_ui_slice_pointer(pc_engine_session *session, pc_ui_state *ui, pc_ui_pointer_kind kind,
                              pc_ui_tool_point point, pc_json_value *out, pc_error *err);
pc_rect pc_ui_slice_drag_rect(const pc_ui_slice_state *slice);
#endif
