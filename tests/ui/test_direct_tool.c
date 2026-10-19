#include "harness.h"
#include "ui/direct_tool.h"
#include <math.h>
static pc_status ex(pc_engine_session *s, const char *id, const char *text)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status status = pc_engine_session_execute_json(s, id, text, &out, NULL);
    pc_json_value_fini(&out);
    return status;
}
static pc_engine_session *fresh(pc_ui_state *ui)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(ex(s, "file.new", "{\"width\":200,\"height\":160}") == PC_OK);
    CHECK(
        ex(s, "path.set",
           "{\"path\":{\"subpaths\":[{\"closed\":true,\"knots\":[[20,20],[100,20],[100,100],[20,100]]}]}}") ==
        PC_OK);
    pc_ui_state_init(ui);
    ui->tool = PC_TOOL_DIRECT_SELECTION;
    return s;
}
static const pc_path *path(pc_engine_session *s)
{
    return &pc_doc_arc_get(pc_engine_session_active(s)->doc)->work_path;
}
static pc_status event(pc_engine_session *s, pc_ui_state *ui, pc_ui_pointer_kind kind, float x, float y,
                       unsigned mods)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error error = {0};
    pc_status status = pc_ui_direct_pointer(s, ui, kind, (pc_ui_tool_point){.x = x, .y = y, .pressure = 1},
                                            mods, 1, &out, &error);
    if (status != PC_OK)
        fprintf(stderr, "direct event: %s\n", error.msg);
    pc_json_value_fini(&out);
    return status;
}
static void click(pc_engine_session *s, pc_ui_state *ui, float x, float y, unsigned mods)
{
    CHECK(event(s, ui, PC_UI_POINTER_DOWN, x, y, mods) == PC_OK);
    CHECK(event(s, ui, PC_UI_POINTER_UP, x, y, mods) == PC_OK);
}
static void test_anchor_drag_preview_cancel_and_history(void)
{
    pc_ui_state ui;
    pc_engine_session *s = fresh(&ui);
    uint64_t revision = pc_engine_session_active(s)->revision;
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 20, 20, 0) == PC_OK);
    CHECK(ui.direct.count == 1);
    CHECK(event(s, &ui, PC_UI_POINTER_MOVE, 35, 27, 0) == PC_OK);
    CHECK(path(s)->subpaths[0].knots[0].anchor.x == 20);
    CHECK(ui.direct.has_preview && ui.direct.preview.subpaths[0].knots[0].anchor.x == 35);
    CHECK(pc_engine_session_active(s)->revision == revision);
    CHECK(event(s, &ui, PC_UI_POINTER_CANCEL, 35, 27, 0) == PC_OK);
    CHECK(!ui.direct.active);
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 20, 20, 0) == PC_OK);
    CHECK(event(s, &ui, PC_UI_POINTER_MOVE, 35, 27, 0) == PC_OK);
    pc_path expected = {0};
    CHECK(pc_path_clone(&ui.direct.preview, &expected, NULL) == PC_OK);
    CHECK(event(s, &ui, PC_UI_POINTER_UP, 35, 27, 0) == PC_OK);
    CHECK(pc_path_eq(path(s), &expected));
    CHECK(pc_engine_session_active(s)->revision == revision + 1);
    CHECK(ex(s, "edit.undo", "{}") == PC_OK);
    CHECK(path(s)->subpaths[0].knots[0].anchor.x == 20);
    pc_path_fini(&expected);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
static void test_selection_marquee_subpath_and_shift_drag(void)
{
    pc_ui_state ui;
    pc_engine_session *s = fresh(&ui);
    click(s, &ui, 20, 20, 0);
    click(s, &ui, 100, 20, PC_UI_MOD_SHIFT);
    CHECK(ui.direct.count == 2);
    click(s, &ui, 20, 20, PC_UI_MOD_SHIFT);
    CHECK(ui.direct.count == 1);
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 5, 5, 0) == PC_OK);
    CHECK(event(s, &ui, PC_UI_POINTER_UP, 110, 30, 0) == PC_OK);
    CHECK(ui.direct.count == 2);
    ui.tool = PC_TOOL_PATH_SELECTION;
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 20, 100, 0) == PC_OK);
    CHECK(ui.direct.count == 4);
    CHECK(event(s, &ui, PC_UI_POINTER_UP, 25, 120, PC_UI_MOD_SHIFT) == PC_OK);
    CHECK(path(s)->subpaths[0].knots[0].anchor.x == 20);
    CHECK(path(s)->subpaths[0].knots[0].anchor.y == 40);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
static void test_convert_handle_and_add_delete(void)
{
    pc_ui_state ui;
    pc_engine_session *s = fresh(&ui);
    ui.tool = PC_TOOL_CONVERT_POINT;
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 20, 20, 0) == PC_OK);
    CHECK(event(s, &ui, PC_UI_POINTER_MOVE, 35, 25, 0) == PC_OK);
    CHECK(ui.direct.preview.subpaths[0].knots[0].out_ctrl.x == 35);
    CHECK(event(s, &ui, PC_UI_POINTER_UP, 35, 25, 0) == PC_OK);
    CHECK(path(s)->subpaths[0].knots[0].smooth);
    ui.tool = PC_TOOL_DIRECT_SELECTION;
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 35, 25, 0) == PC_OK);
    CHECK(ui.direct.edit == 1);
    CHECK(event(s, &ui, PC_UI_POINTER_UP, 40, 30, PC_UI_MOD_ALT) == PC_OK);
    CHECK(path(s)->subpaths[0].knots[0].out_ctrl.x == 40);
    ui.tool = PC_TOOL_CONVERT_POINT;
    click(s, &ui, 20, 20, 0);
    CHECK(path(s)->subpaths[0].knots[0].out_ctrl.x == 20);
    ui.tool = PC_TOOL_ADD_ANCHOR_POINT;
    click(s, &ui, 60, 20, 0);
    CHECK(path(s)->subpaths[0].nknots == 5);
    ui.tool = PC_TOOL_DELETE_ANCHOR_POINT;
    click(s, &ui, 60, 20, 0);
    CHECK(path(s)->subpaths[0].nknots == 4);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
static void test_stale_and_hostile_state(void)
{
    pc_ui_state ui;
    pc_engine_session *s = fresh(&ui);
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 20, 20, 0) == PC_OK);
    CHECK(ex(s, "path.transform", "{\"matrix\":[1,0,0,1,1,0]}") == PC_OK);
    CHECK(event(s, &ui, PC_UI_POINTER_UP, 30, 30, 0) != PC_OK);
    CHECK(!ui.direct.active);
    CHECK(event(s, &ui, PC_UI_POINTER_DOWN, NAN, 30, 0) != PC_OK);
    click(s, &ui, 21, 20, 0);
    pc_json_value encoded = PC_JSON_VALUE_INIT;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *o = NULL;
    CHECK(pc_ui_direct_to_json(&ui.direct, d, &o, NULL) == PC_OK);
    CHECK(pc_json_value_from_mut(o, &encoded, NULL) == PC_OK);
    pc_buf text = PC_BUF_INIT;
    CHECK(pc_json_write(pc_json_value_root(&encoded), 0, &text, NULL) == PC_OK);
    yyjson_doc *read = NULL;
    CHECK(pc_json_parse(pc_buf_cstr(&text), text.len, &read, NULL) == PC_OK);
    pc_ui_direct decoded = {0};
    CHECK(pc_ui_direct_from_json(&decoded, yyjson_doc_get_root(read), NULL) == PC_OK);
    CHECK(decoded.count == 1 && !decoded.active);
    yyjson_doc_free(read);
    read = NULL;
    CHECK(pc_json_parse("{\"anchors\":[[9999999,0]]}", sizeof "{\"anchors\":[[9999999,0]]}" - 1, &read,
                        NULL) == PC_OK);
    CHECK(pc_ui_direct_from_json(&decoded, yyjson_doc_get_root(read), NULL) != PC_OK);
    CHECK(decoded.count == 1);
    yyjson_doc_free(read);
    pc_buf_fini(&text);
    pc_json_value_fini(&encoded);
    yyjson_mut_doc_free(d);
    pc_ui_direct_fini(&decoded);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
int main(int argc, char **argv)
{
    RUN(test_anchor_drag_preview_cancel_and_history);
    RUN(test_selection_marquee_subpath_and_shift_drag);
    RUN(test_convert_handle_and_add_delete);
    RUN(test_stale_and_hostile_state);
    return pc_t_finish();
}
