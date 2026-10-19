#include "harness.h"
#include "ui/artboard.h"
#include "doc/comps.h"
#include <math.h>
#include <stdlib.h>
static pc_engine_session *session(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.new", "{\"width\":400,\"height\":300}", &out, NULL) ==
          PC_OK);
    pc_json_value_fini(&out);
    return s;
}
static pc_status event(pc_engine_session *s, pc_ui_state *ui, pc_ui_pointer_kind kind, float x, float y,
                       unsigned mods)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status status =
        pc_ui_artboard_pointer(s, ui, kind, (pc_ui_tool_point){.x = x, .y = y}, mods, 1, &out, &err);
    if (status != PC_OK)
        fprintf(stderr, "artboard: %s\n", err.msg);
    pc_json_value_fini(&out);
    return status;
}
static void drag(pc_engine_session *s, pc_ui_state *ui, float x, float y, float endx, float endy,
                 unsigned mods)
{
    CHECK(event(s, ui, PC_UI_POINTER_DOWN, x, y, mods) == PC_OK);
    CHECK(event(s, ui, PC_UI_POINTER_MOVE, endx, endy, mods) == PC_OK);
    CHECK(event(s, ui, PC_UI_POINTER_UP, endx, endy, mods) == PC_OK);
}
static pc_rect board(pc_engine_session *s)
{
    pc_layer_id id;
    pc_rect rect = {0};
    CHECK(pc_ui_artboard_active(pc_engine_session_active(s), &id, &rect));
    return rect;
}
static void test_artboard_draw_resize_move_plus_and_shift(void)
{
    pc_engine_session *s = session();
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_ARTBOARD;
    drag(s, &ui, 20, 30, 120, 110, 0);
    CHECK(pc_rect_eq(board(s), pc_rect_make(20, 30, 120, 110)));
    size_t history = pc_history_past_len(pc_engine_session_active(s)->history);
    drag(s, &ui, 120, 70, 160, 75, 0);
    CHECK(pc_rect_eq(board(s), pc_rect_make(20, 30, 160, 110)));
    CHECK(pc_history_past_len(pc_engine_session_active(s)->history) == history + 1);
    CHECK(pc_engine_session_undo(s));
    drag(s, &ui, 60, 60, 70, 50, 0);
    CHECK(pc_rect_eq(board(s), pc_rect_make(30, 20, 130, 100)));
    pc_ui_artboard_layout layout = {0};
    CHECK(pc_ui_artboard_layout_update(&layout, pc_engine_session_active(s), NULL) == PC_OK);
    CHECK(layout.count == 1);
    drag(s, &ui, 300, 250, 300, 250, 0);
    CHECK(pc_ui_artboard_layout_update(&layout, pc_engine_session_active(s), NULL) == PC_OK &&
          layout.count == 1);
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 200, 200, 0) == PC_OK);
    CHECK(event(s, &ui, PC_UI_POINTER_CANCEL, 0, 0, 0) == PC_OK && !ui.artboard_tool.active);
    CHECK(pc_engine_session_undo(s)); /* Back to the 100 x 80 board. */
    drag(s, &ui, 120, 110, 160, 115, PC_UI_MOD_SHIFT);
    CHECK(pc_rect_eq(board(s), pc_rect_make(20, 30, 160, 142)));
    double plus[4][2];
    pc_ui_artboard_plus_points(board(s), 1, plus);
    drag(s, &ui, (float)plus[1][0], (float)plus[1][1], (float)plus[1][0], (float)plus[1][1], 0);
    CHECK(pc_ui_artboard_layout_update(&layout, pc_engine_session_active(s), NULL) == PC_OK &&
          layout.count == 2);
    pc_ui_artboard_layout_fini(&layout);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
static void test_artboard_pasteboard_coverage_fit_and_scope(void)
{
    pc_engine_session *s = session();
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_ARTBOARD;
    drag(s, &ui, 20, 30, 120, 110, 0);
    pc_ui_artboard_layout layout = {0};
    CHECK(pc_ui_artboard_layout_update(&layout, pc_engine_session_active(s), NULL) == PC_OK);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    uint64_t area = 0;
    for (size_t i = 0; i < layout.ngaps; i++) {
        area += (uint64_t)pc_rect_width(layout.gaps[i]) * pc_rect_height(layout.gaps[i]);
        CHECK(pc_rect_is_empty(pc_rect_intersect(layout.gaps[i], board(s))));
        for (size_t j = 0; j < i; j++)
            CHECK(pc_rect_is_empty(pc_rect_intersect(layout.gaps[i], layout.gaps[j])));
    }
    CHECK(area == (uint64_t)doc->size.width * doc->size.height - UINT64_C(100) * 80);
    pc_ui_view view;
    pc_ui_view_default(&view);
    pc_layer_id id;
    pc_rect rect;
    CHECK(pc_ui_artboard_fit(&view, pc_engine_session_active(s), 1200, 800, 1, &id, &rect, NULL) == PC_OK);
    CHECK_NEAR(view.center[0], 70, .001);
    CHECK_NEAR(view.center[1], 70, .001);
    CHECK(!view.fit_pending && view.zoom > 1);
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 60, 60, 0) == PC_OK);
    CHECK(pc_engine_session_undo(s));
    CHECK(event(s, &ui, PC_UI_POINTER_UP, 80, 80, 0) == PC_ERR_INVALID && !ui.artboard_tool.active);
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, NAN, 0, 0) == PC_ERR_INVALID);
    pc_ui_artboard_layout_fini(&layout);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
int main(int argc, char **argv)
{
    RUN(test_artboard_draw_resize_move_plus_and_shift);
    RUN(test_artboard_pasteboard_coverage_fit_and_scope);
    return pc_t_finish();
}
