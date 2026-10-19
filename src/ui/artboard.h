#ifndef PC_UI_ARTBOARD_H
#define PC_UI_ARTBOARD_H
#include "ui/tool_gesture.h"
#include "ui/artboard_state.h"
#include "ui/shortcuts.h"
typedef struct pc_ui_artboard_entry {
    pc_layer_id id;
    size_t index;
    pc_rect rect;
} pc_ui_artboard_entry;
typedef struct pc_ui_artboard_layout {
    uint64_t document, revision;
    bool valid;
    pc_size size;
    pc_ui_artboard_entry *boards;
    size_t count;
    pc_rect *gaps;
    size_t ngaps;
} pc_ui_artboard_layout;
void pc_ui_artboard_layout_fini(pc_ui_artboard_layout *layout);
pc_status pc_ui_artboard_layout_update(pc_ui_artboard_layout *layout, const pc_engine_doc_state *state,
                                       pc_error *err);
bool pc_ui_artboard_active(const pc_engine_doc_state *state, pc_layer_id *id, pc_rect *rect);
void pc_ui_artboard_plus_points(pc_rect rect, double zoom, double points[4][2]);
pc_rect pc_ui_artboard_drag_rect(const pc_ui_artboard_tool *tool);
pc_status pc_ui_artboard_pointer(pc_engine_session *session, pc_ui_state *ui, pc_ui_pointer_kind kind,
                                 pc_ui_tool_point point, unsigned mods, double zoom, pc_json_value *out,
                                 pc_error *err);
pc_status pc_ui_artboard_fit(pc_ui_view *view, const pc_engine_doc_state *state, float width, float height,
                             float scale, pc_layer_id *board, pc_rect *rect, pc_error *err);
#endif
