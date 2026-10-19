#include "harness.h"
#include "ui/slice_tool.h"
#include "engine/slice_ui.h"
#include <math.h>
#include <string.h>
static pc_engine_session *session(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.new", "{\"width\":100,\"height\":80}", &out, NULL) ==
          PC_OK);
    pc_json_value_fini(&out);
    return s;
}
static pc_status event(pc_engine_session *s, pc_ui_state *ui, pc_ui_pointer_kind kind, float x, float y)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status status = pc_ui_slice_pointer(s, ui, kind, (pc_ui_tool_point){.x = x, .y = y}, &out, NULL);
    pc_json_value_fini(&out);
    return status;
}
static const pc_document *document(pc_engine_session *s)
{
    return pc_doc_arc_get(pc_engine_session_active(s)->doc);
}
static void test_slice_draw_select_move_auto_and_locked(void)
{
    pc_engine_session *s = session();
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_SLICE;
    size_t history = pc_history_past_len(pc_engine_session_active(s)->history);
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 10, 10) == PC_OK);
    CHECK(event(s, &ui, PC_UI_POINTER_MOVE, 40, 30) == PC_OK);
    CHECK(ui.slices.dragging && document(s)->slices.len == 0);
    CHECK(event(s, &ui, PC_UI_POINTER_UP, 40, 30) == PC_OK);
    CHECK(document(s)->slices.len == 1 && ui.slices.has_selected);
    CHECK(pc_rect_eq(document(s)->slices.list[0].rect, pc_rect_make(10, 10, 40, 30)));
    CHECK(pc_history_past_len(pc_engine_session_active(s)->history) == history + 1);
    size_t selected = ui.slices.selected;
    ui.tool = PC_TOOL_SLICE_SELECT;
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 20, 20) == PC_OK && ui.slices.selected == selected);
    CHECK(event(s, &ui, PC_UI_POINTER_UP, 25, 25) == PC_OK);
    CHECK(pc_rect_eq(document(s)->slices.list[0].rect, pc_rect_make(15, 15, 45, 35)));
    CHECK(pc_history_past_len(pc_engine_session_active(s)->history) == history + 2);
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 90, 75) == PC_OK);
    CHECK(!ui.slices.dragging && ui.slices.has_selected);
    CHECK(pc_ui_slice_selected(&ui.slices)->origin == PC_SLICE_AUTO);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "view.lockSlices", "{\"on\":true}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_slices_locked(s));
    ui.tool = PC_TOOL_SLICE;
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 1, 1) == PC_OK && !ui.slices.dragging);
    CHECK(event(s, &ui, PC_UI_POINTER_UP, 70, 70) == PC_OK && document(s)->slices.len == 1);
    ui.tool = PC_TOOL_SLICE_SELECT;
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 20, 20) == PC_OK && ui.slices.has_selected &&
          !ui.slices.dragging);
    CHECK(pc_ui_slice_selected(&ui.slices)->origin == PC_SLICE_USER);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
static void test_slice_cancel_stale_invalid_and_topmost(void)
{
    pc_engine_session *s = session();
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_SLICE;
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 50, 50) == PC_OK);
    CHECK(event(s, &ui, PC_UI_POINTER_CANCEL, 0, 0) == PC_OK && !ui.slices.dragging);
    CHECK(document(s)->slices.len == 0);
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 10, 10) == PC_OK);
    CHECK(event(s, &ui, PC_UI_POINTER_UP, NAN, 40) == PC_ERR_INVALID && !ui.slices.dragging);
    CHECK(document(s)->slices.len == 0);
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 10, 10) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "slice.new", "{\"rect\":[5,5,30,30]}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(event(s, &ui, PC_UI_POINTER_UP, 40, 40) == PC_ERR_INVALID && document(s)->slices.len == 1);
    CHECK(pc_engine_session_execute_json(s, "slice.new", "{\"rect\":[10,10,30,30]}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_ui_slice_resolve(&ui.slices, pc_engine_session_active(s), NULL) == PC_OK);
    const pc_resolved_slice *hit = pc_ui_slice_hit(&ui.slices, 20, 20);
    CHECK(hit && hit->has_id && hit->id == document(s)->slices.list[1].id);
    CHECK(pc_ui_slice_hit(&ui.slices, INFINITY, 0) == NULL);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
int main(int argc, char **argv)
{
    RUN(test_slice_draw_select_move_auto_and_locked);
    RUN(test_slice_cancel_stale_invalid_and_topmost);
    return pc_t_finish();
}
