#ifndef PC_UI_TYPE_TOOL_H
#define PC_UI_TYPE_TOOL_H
#include "ui/state.h"
#include "engine/engine.h"
#include "text/engine.h"
#include "ui/shortcuts.h"
#include "ui/tool_gesture.h"
/* One cached layout; never borrows mutable document data across an engine command. */
typedef struct pc_ui_type_layout {
    pc_doc_arc *document;
    pc_layer_id layer;
    uint64_t revision;
    pc_text_layout layout;
} pc_ui_type_layout;
void pc_ui_type_layout_fini(pc_ui_type_layout *cache);
pc_status pc_ui_type_layout_get(pc_ui_type_layout *cache, const pc_engine_doc_state *state, pc_layer_id layer,
                                pc_error *err);
pc_status pc_ui_type_begin(pc_engine_session *session, pc_ui_state *ui, pc_layer_id layer, bool all,
                           pc_error *err);
pc_status pc_ui_type_create(pc_engine_session *session, pc_ui_state *ui, const float start[2],
                            const float end[2], float zoom, pc_json_value *out, pc_error *err);
pc_status pc_ui_type_insert(pc_engine_session *session, pc_ui_state *ui, const char *text, size_t length,
                            pc_json_value *out, pc_error *err);
pc_status pc_ui_type_text_event(pc_engine_session *session, pc_ui_state *ui, const char *text, size_t length,
                                pc_json_value *out, pc_error *err);
pc_status pc_ui_type_preedit(pc_engine_session *session, pc_ui_state *ui, const char *text, size_t length,
                             pc_json_value *out, pc_error *err);
pc_status pc_ui_type_key(pc_engine_session *session, pc_ui_state *ui, pc_ui_type_layout *cache, pc_ui_key key,
                         unsigned modifiers, pc_json_value *out, pc_error *err);
pc_status pc_ui_type_commit(pc_engine_session *session, pc_ui_state *ui, unsigned modifiers,
                            pc_json_value *out, pc_error *err);
pc_status pc_ui_type_cancel(pc_engine_session *session, pc_ui_state *ui, pc_error *err);
pc_status pc_ui_type_edit_command(pc_engine_session *session, pc_ui_state *ui, const char *id,
                                  yyjson_val *params, bool coalesce, bool range, pc_json_value *out,
                                  pc_error *err);
pc_status pc_ui_type_add_scope(pc_engine_session *session, pc_ui_state *ui, yyjson_mut_doc *doc,
                               yyjson_mut_val *params, pc_error *err);
pc_status pc_ui_type_foreground_changed(pc_engine_session *session, pc_ui_state *ui, const float previous[4],
                                        pc_error *err);
typedef struct pc_ui_type_pointer {
    pc_ui_type_layout layout;
    bool pressed, selecting;
    float start[2];
    pc_doc_id document;
    uint64_t revision;
} pc_ui_type_pointer;
void pc_ui_type_pointer_fini(pc_ui_type_pointer *pointer);
pc_status pc_ui_type_pointer_event(pc_ui_type_pointer *pointer, pc_engine_session *session, pc_ui_state *ui,
                                   pc_ui_pointer_kind kind, pc_ui_tool_point point, unsigned modifiers,
                                   float zoom, pc_json_value *out, pc_error *err);
#endif
