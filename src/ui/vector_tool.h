#ifndef PC_UI_VECTOR_TOOL_H
#define PC_UI_VECTOR_TOOL_H
#include "ui/tool_gesture.h"
/* PC_ERR_NOT_FOUND means a subpixel drag, with no document edit. */
pc_status pc_ui_shape_drag_params(pc_ui_tool tool, const pc_ui_tool_options *options,
                                  const float foreground[4], const float background[4], const float start[2],
                                  const float end[2], unsigned modifiers, pc_json_value *out, pc_error *err);
pc_status pc_ui_shape_gesture(pc_ui_tool_gesture *gesture, pc_engine_session *session, bool preview,
                              pc_json_value *out, pc_error *err);
#endif
