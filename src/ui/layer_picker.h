#ifndef PC_UI_LAYER_PICKER_H
#define PC_UI_LAYER_PICKER_H
#include "ui/tool_gesture.h"
#include "engine/adjust_eyedropper.h"
bool pc_ui_layer_picker_armed(const pc_engine_session *session, const pc_ui_state *ui);
void pc_ui_layer_picker_prune(const pc_engine_session *session, pc_ui_state *ui);
pc_status pc_ui_layer_picker_arm(const pc_engine_session *session, pc_ui_state *ui,
                                 pc_engine_tone_picker point, pc_error *err);
bool pc_ui_layer_picker_cancel(pc_ui_state *ui);
pc_status pc_ui_layer_picker_pointer(pc_engine_session *session, pc_ui_state *ui, pc_ui_pointer_kind kind,
                                     pc_ui_tool_point point, pc_json_value *out, pc_error *err);
pc_status pc_ui_layer_picker_to_json(const pc_ui_layer_picker *picker, yyjson_mut_doc *doc,
                                     yyjson_mut_val **out, pc_error *err);
pc_status pc_ui_layer_picker_from_json(yyjson_val *value, pc_ui_layer_picker *out, pc_error *err);
#endif
