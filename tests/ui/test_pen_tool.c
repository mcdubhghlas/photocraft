#include "harness.h"
#include "ui/pen_tool.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
static pc_engine_session *fresh(pc_ui_state *ui)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.new", "{\"width\":200,\"height\":160}", &out, NULL) ==
          PC_OK);
    pc_json_value_fini(&out);
    pc_ui_state_init(ui);
    ui->tool = PC_TOOL_PEN;
    return s;
}
static const pc_document *document(pc_engine_session *s)
{
    return pc_doc_arc_get(pc_engine_session_active(s)->doc);
}
static pc_status event(pc_engine_session *s, pc_ui_state *ui, pc_ui_pointer_kind kind, float x, float y,
                       unsigned modifiers)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status status = pc_ui_pen_pointer(s, ui, kind, (pc_ui_tool_point){.x = x, .y = y, .pressure = 1},
                                         modifiers, 1, &out, NULL);
    pc_json_value_fini(&out);
    return status;
}
static void click(pc_engine_session *s, pc_ui_state *ui, float x, float y)
{
    CHECK(event(s, ui, PC_UI_POINTER_DOWN, x, y, 0) == PC_OK);
    CHECK(event(s, ui, PC_UI_POINTER_UP, x, y, 0) == PC_OK);
}
static pc_status key(pc_engine_session *s, pc_ui_state *ui, pc_ui_key value, unsigned modifiers)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status status = pc_ui_pen_key(s, ui, value, modifiers, &out, NULL);
    pc_json_value_fini(&out);
    return status;
}
static void test_pen_handles_close_drag_and_one_history_step(void)
{
    pc_ui_state ui;
    pc_engine_session *s = fresh(&ui);
    uint64_t revision = pc_engine_session_active(s)->revision;
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 20, 20, 0) == PC_OK);
    CHECK(event(s, &ui, PC_UI_POINTER_MOVE, 30, 20, 0) == PC_OK);
    CHECK(event(s, &ui, PC_UI_POINTER_UP, 30, 20, 0) == PC_OK);
    CHECK(ui.has_pen && ui.pen.count == 1 && ui.pen.knots[0].points[1][0] == 10);
    click(s, &ui, 80, 60);
    CHECK(!document(s)->has_work_path && pc_engine_session_active(s)->revision == revision);
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 20, 20, 0) == PC_OK);
    CHECK(event(s, &ui, PC_UI_POINTER_MOVE, 20, 32, 0) == PC_OK);
    CHECK(event(s, &ui, PC_UI_POINTER_UP, 20, 32, 0) == PC_OK);
    CHECK(!ui.has_pen && document(s)->has_work_path && ui.selected_path && !strcmp(ui.selected_path, "work"));
    const pc_subpath *path = &document(s)->work_path.subpaths[0];
    CHECK(path->closed && path->nknots == 2);
    CHECK(path->knots[0].in_ctrl.x == 20 && path->knots[0].in_ctrl.y == 8 && path->knots[0].out_ctrl.y == 32);
    CHECK(pc_engine_session_undo(s));
    CHECK(!document(s)->has_work_path);
    CHECK(pc_engine_session_redo(s));
    CHECK(document(s)->has_work_path);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
static void test_pen_alt_break_undo_and_escape_keep_open(void)
{
    pc_ui_state ui;
    pc_engine_session *s = fresh(&ui);
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 20, 20, 0) == PC_OK);
    CHECK(event(s, &ui, PC_UI_POINTER_MOVE, 30, 20, 0) == PC_OK);
    CHECK(event(s, &ui, PC_UI_POINTER_MOVE, 35, 30, PC_UI_MOD_ALT) == PC_OK);
    CHECK(event(s, &ui, PC_UI_POINTER_UP, 35, 30, PC_UI_MOD_ALT) == PC_OK);
    CHECK(ui.pen.knots[0].points[1][0] == 10 && ui.pen.knots[0].points[1][1] == 20 &&
          ui.pen.knots[0].unlinked);
    click(s, &ui, 70, 50);
    click(s, &ui, 100, 80);
    CHECK(key(s, &ui, PC_KEY_Z, PC_UI_MOD_COMMAND) == PC_OK && ui.pen.count == 2 &&
          !document(s)->has_work_path);
    CHECK(key(s, &ui, PC_KEY_ESCAPE, 0) == PC_OK && !ui.has_pen && document(s)->has_work_path);
    CHECK(!document(s)->work_path.subpaths[0].closed && document(s)->work_path.subpaths[0].nknots == 2);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
static void test_curvature_move_corner_and_close(void)
{
    pc_ui_state ui;
    pc_engine_session *s = fresh(&ui);
    ui.tool = PC_TOOL_CURVATURE_PEN;
    click(s, &ui, 20, 20);
    click(s, &ui, 70, 60);
    click(s, &ui, 110, 20);
    CHECK(ui.pen.curvature && ui.pen.count == 3 && ui.pen.knots[1].points[1][0] != 70);
    float point[2] = {70, 60};
    CHECK(pc_ui_pen_curvature_toggle(&ui, point, 1, NULL) == PC_OK);
    CHECK(ui.pen.knots[1].unlinked && ui.pen.knots[1].points[1][0] == 70 &&
          ui.pen.knots[1].points[2][1] == 60);
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 70, 60, 0) == PC_OK);
    CHECK(event(s, &ui, PC_UI_POINTER_MOVE, 70, 80, 0) == PC_OK);
    CHECK(event(s, &ui, PC_UI_POINTER_UP, 70, 80, 0) == PC_OK);
    CHECK(ui.pen.knots[1].points[0][1] == 80);
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 20, 20, 0) == PC_OK);
    CHECK(event(s, &ui, PC_UI_POINTER_UP, 20, 20, 0) == PC_OK);
    CHECK(!ui.has_pen && document(s)->has_work_path && document(s)->work_path.subpaths[0].closed);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
static void test_freeform_fit_and_straight_bridge(void)
{
    pc_ui_state ui;
    pc_engine_session *s = fresh(&ui);
    ui.tool = PC_TOOL_FREEFORM_PEN;
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 10, 10, 0) == PC_OK);
    CHECK(event(s, &ui, PC_UI_POINTER_MOVE, 20, 20, 0) == PC_OK);
    CHECK(event(s, &ui, PC_UI_POINTER_UP, 20, 20, PC_UI_MOD_ALT) == PC_OK);
    CHECK(!document(s)->has_work_path && ui.has_pen);
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 50, 20, PC_UI_MOD_ALT) == PC_OK);
    CHECK(event(s, &ui, PC_UI_POINTER_UP, 50, 20, PC_UI_MOD_ALT) == PC_OK);
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 70, 50, 0) == PC_OK);
    CHECK(event(s, &ui, PC_UI_POINTER_MOVE, 90, 80, 0) == PC_OK);
    CHECK(event(s, &ui, PC_UI_POINTER_UP, 90, 80, 0) == PC_OK);
    CHECK(!ui.has_pen && document(s)->has_work_path);
    const pc_subpath *sub = &document(s)->work_path.subpaths[0];
    bool bridge = false;
    for (size_t i = 1; i < sub->nknots; i++) {
        const pc_knot *a = &sub->knots[i - 1], *b = &sub->knots[i];
        if (a->anchor.x == 20 && a->anchor.y == 20 && b->anchor.x == 50 && b->anchor.y == 20) {
            bridge = true;
            CHECK(a->out_ctrl.x == 20 && b->in_ctrl.x == 50);
        }
    }
    CHECK(bridge);
    CHECK(pc_engine_session_undo(s));
    CHECK(!document(s)->has_work_path);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
static void test_restored_pen_close_grows_and_guards_foreign_edits(void)
{
    pc_ui_state ui;
    pc_engine_session *s = fresh(&ui);
    yyjson_doc *json = NULL;
    const char *saved = "{\"knots\":[[[10,10],[10,10],[10,10]],[[60,30],[60,30],[60,30]]],\"unlinked\":[]}";
    CHECK(pc_json_parse(saved, strlen(saved), &json, NULL) == PC_OK);
    CHECK(pc_ui_pen_from_json(&ui.pen, yyjson_doc_get_root(json), NULL) == PC_OK);
    yyjson_doc_free(json);
    ui.has_pen = true;
    CHECK(ui.pen.capacity == 2);
    click(s, &ui, 10, 10);
    CHECK(!ui.has_pen && document(s)->has_work_path);
    CHECK(document(s)->work_path.subpaths[0].nknots == 2);
    click(s, &ui, 20, 70);
    click(s, &ui, 60, 100);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "layer.new.layer", "{}", &out, NULL) == PC_OK);
    uint64_t revision = pc_engine_session_active(s)->revision;
    CHECK(key(s, &ui, PC_KEY_ENTER, 0) == PC_ERR_INVALID && ui.has_pen &&
          pc_engine_session_active(s)->revision == revision);
    CHECK(event(s, &ui, PC_UI_POINTER_CANCEL, 0, 0, 0) == PC_OK && !ui.has_pen);
    pc_json_value_fini(&out);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
static void test_pen_state_round_trip_and_failed_read_preserves_knots(void)
{
    pc_ui_state ui;
    pc_engine_session *s = fresh(&ui);
    click(s, &ui, 10, 20);
    click(s, &ui, 80, 90);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = NULL;
    CHECK(pc_ui_state_to_json(&ui, doc, &root, NULL) == PC_OK);
    pc_buf json = PC_BUF_INIT;
    CHECK(pc_json_write(root, 0, &json, NULL) == PC_OK);
    yyjson_mut_doc_free(doc);
    yyjson_doc *read = NULL;
    CHECK(pc_json_parse(pc_buf_cstr(&json), json.len, &read, NULL) == PC_OK);
    pc_ui_state restored;
    pc_ui_state_init(&restored);
    CHECK(pc_ui_state_from_json(&restored, yyjson_doc_get_root(read), NULL) == PC_OK);
    yyjson_doc_free(read);
    pc_buf_fini(&json);
    CHECK(restored.has_pen && restored.pen.count == 2 && !restored.pen.dragging && !restored.pen.document);
    const char *bad[] = {"null", "{\"knots\":1}", "{\"knots\":[[[0,0]]]} ",
                         "{\"knots\":[[[0,0],[0,0],[0,1e100]]]}",
                         "{\"knots\":[[[0,0],[0,0],[0,0]]],\"unlinked\":[1]}"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        read = NULL;
        CHECK(pc_json_parse(bad[i], strlen(bad[i]), &read, NULL) == PC_OK);
        CHECK(pc_ui_pen_from_json(&restored.pen, yyjson_doc_get_root(read), NULL) == PC_ERR_INVALID);
        yyjson_doc_free(read);
        CHECK(restored.pen.count == 2 && restored.pen.knots[1].points[0][0] == 80);
    }
    pc_ui_state_fini(&restored);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
int main(int argc, char **argv)
{
    RUN(test_pen_state_round_trip_and_failed_read_preserves_knots);
    RUN(test_pen_handles_close_drag_and_one_history_step);
    RUN(test_pen_alt_break_undo_and_escape_keep_open);
    RUN(test_curvature_move_corner_and_close);
    RUN(test_freeform_fit_and_straight_bridge);
    RUN(test_restored_pen_close_grows_and_guards_foreign_edits);
    return pc_t_finish();
}
