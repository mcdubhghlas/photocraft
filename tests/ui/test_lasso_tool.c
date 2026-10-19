#include "harness.h"
#include "ui/lasso_tool.h"
#include "raster/pixel.h"
#include "engine/engine_internal.h"
#include <math.h>
#include <string.h>
static pc_engine_session *new_session(void)
{
    pc_engine_session *session = NULL;
    CHECK(pc_engine_session_new(&session, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(session, "file.new",
                                         "{\"width\":100,\"height\":100,\"background\":\"transparent\"}",
                                         &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    return session;
}
static pc_status event(pc_engine_session *session, pc_ui_state *ui, pc_ui_pointer_kind kind, float x, float y,
                       unsigned mods)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status status = pc_ui_lasso_pointer(
        session, ui, kind, (pc_ui_tool_point){.x = x, .y = y, .pressure = 1}, mods, 1, &out, &err);
    if (status != PC_OK)
        fprintf(stderr, "lasso: %s\n", err.msg);
    pc_json_value_fini(&out);
    return status;
}
static void press(pc_engine_session *session, pc_ui_state *ui, float x, float y, unsigned mods)
{
    CHECK(event(session, ui, PC_UI_POINTER_DOWN, x, y, mods) == PC_OK);
    CHECK(event(session, ui, PC_UI_POINTER_UP, x, y, mods) == PC_OK);
}
static pc_status key(pc_engine_session *session, pc_ui_state *ui, pc_ui_key key_, unsigned mods)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status status = pc_ui_lasso_key(session, ui, key_, mods, 1, &out, NULL);
    pc_json_value_fini(&out);
    return status;
}
static void test_polygon_pending_close_undo_and_stale(void)
{
    pc_engine_session *session = new_session();
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_POLYGON_LASSO;
    size_t history = pc_history_past_len(pc_engine_session_active(session)->history);
    press(session, &ui, 10, 10, 0);
    press(session, &ui, 70, 10, 0);
    press(session, &ui, 70, 70, 0);
    CHECK(ui.lasso.active && ui.lasso.path.n == 3 && pc_ui_lasso_replaces_selection(&ui));
    CHECK(pc_history_past_len(pc_engine_session_active(session)->history) == history);
    CHECK(key(session, &ui, PC_KEY_BACKSPACE, 0) == PC_OK);
    CHECK(ui.lasso.path.n == 2);
    press(session, &ui, 70, 70, 0);
    CHECK(key(session, &ui, PC_KEY_ENTER, 0) == PC_OK && !ui.lasso.active);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(session)->doc);
    CHECK(doc->has_selection &&
          pc_history_past_len(pc_engine_session_active(session)->history) == history + 1);
    pc_rect r = pc_surface_content_bounds(&doc->selection);
    CHECK(r.x0 == 10 && r.y0 == 10 && r.x1 == 70 && r.y1 == 70);
    const pc_doc_arc *before = pc_engine_session_active(session)->doc;
    press(session, &ui, 20, 20, 0);
    press(session, &ui, 90, 20, 0);
    CHECK(key(session, &ui, PC_KEY_ESCAPE, 0) == PC_OK);
    CHECK(pc_engine_session_active(session)->doc == before);
    press(session, &ui, 10, 10, 0);
    ui.tool = PC_TOOL_BRUSH;
    pc_ui_lasso_sync(session, &ui);
    CHECK(!ui.lasso.active);
    ui.tool = PC_TOOL_POLYGON_LASSO;
    press(session, &ui, 10, 10, 0);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(session, "edit.undo", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    pc_ui_lasso_sync(session, &ui);
    CHECK(!ui.lasso.active);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(session);
}
static void test_polygon_latched_modes_shift_and_alt_freehand(void)
{
    pc_engine_session *session = new_session();
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_POLYGON_LASSO;
    press(session, &ui, 10, 10, PC_UI_MOD_ALT);
    CHECK(!strcmp(ui.lasso.mode, "replace"));
    CHECK(event(session, &ui, PC_UI_POINTER_DOWN, 50, 10, 0) == PC_OK);
    CHECK(event(session, &ui, PC_UI_POINTER_MOVE, 60, 20, PC_UI_MOD_ALT) == PC_OK);
    CHECK(event(session, &ui, PC_UI_POINTER_UP, 60, 40, PC_UI_MOD_ALT) == PC_OK);
    CHECK(ui.lasso.path.n >= 4);
    press(session, &ui, 20, 70, PC_UI_MOD_SHIFT);
    CHECK(!strcmp(ui.lasso.mode, "replace"));
    double *last = ui.lasso.path.v[ui.lasso.path.n - 1];
    CHECK_NEAR(fabs(last[0] - 60), fabs(last[1] - 40), 1e-5);
    CHECK(key(session, &ui, PC_KEY_ENTER, 0) == PC_OK);
    press(session, &ui, 10, 10, PC_UI_MOD_ALT);
    CHECK(!strcmp(ui.lasso.mode, "subtract"));
    press(session, &ui, 50, 10, 0);
    press(session, &ui, 50, 50, 0);
    press(session, &ui, 10, 10, 0);
    CHECK(!ui.lasso.active);
    ui.tool = PC_TOOL_LASSO;
    CHECK(event(session, &ui, PC_UI_POINTER_DOWN, 10, 10, 0) == PC_OK);
    CHECK(event(session, &ui, PC_UI_POINTER_MOVE, 50, 10, 0) == PC_OK);
    CHECK(event(session, &ui, PC_UI_POINTER_MOVE, 50, 50, 0) == PC_OK);
    CHECK(event(session, &ui, PC_UI_POINTER_UP, 10, 50, PC_UI_MOD_ALT) == PC_OK);
    CHECK(ui.lasso.active && ui.lasso.polygonal && !ui.lasso.down);
    CHECK(event(session, &ui, PC_UI_POINTER_MOVE, 10, 50, 0) == PC_OK && !ui.lasso.active);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(session);
}
static pc_status square(pc_document *doc, bool *has_active, pc_layer_id *active, void *ctx, pc_error *err)
{
    (void)ctx;
    (void)has_active;
    (void)active;
    pc_surface *surface = pc_layer_surface_mut(&doc->layers[0]);
    const float black[4] = {0, 0, 0, 1}, white[4] = {1, 1, 1, 1};
    float encoded[PC_RASTER_MAX_CHANNELS];
    pc_pixel_format format = pc_document_pixel_format(doc);
    size_t count = pc_from_rgba(format, black, encoded);
    pc_status status = pc_surface_fill_rect(surface, pc_document_bounds(doc), encoded, count, err);
    count = pc_from_rgba(format, white, encoded);
    if (status == PC_OK)
        status = pc_surface_fill_rect(surface, pc_rect_make(20, 20, 80, 80), encoded, count, err);
    return status;
}
static pc_status deny(const char *id, yyjson_val *params, pc_error *err)
{
    (void)id;
    (void)params;
    return pc_fail(err, PC_ERR_INVALID, "blocked");
}
static void test_magnetic_live_fastening_retract_and_source_scope(void)
{
    pc_engine_session *session = new_session();
    CHECK(pc_engine_session_edit(session, "square", square, NULL, NULL) == PC_OK);
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_MAGNETIC_LASSO;
    ui.tool_options.magnetic_frequency = 100;
    size_t history = pc_history_past_len(pc_engine_session_active(session)->history);
    press(session, &ui, 21, 21, 0);
    CHECK(ui.lasso.active && ui.lasso.nanchors == 1);
    CHECK(event(session, &ui, PC_UI_POINTER_MOVE, 78, 21, 0) == PC_OK);
    CHECK(ui.lasso.nanchors > 1 && ui.lasso.live.n > 1);
    CHECK(pc_history_past_len(pc_engine_session_active(session)->history) == history);
    size_t count = ui.lasso.nanchors;
    CHECK(key(session, &ui, PC_KEY_BACKSPACE, 0) == PC_OK && ui.lasso.nanchors == count - 1);
    press(session, &ui, 78, 78, 0);
    press(session, &ui, 21, 78, PC_UI_MOD_ALT);
    CHECK(key(session, &ui, PC_KEY_ENTER, PC_UI_MOD_ALT) == PC_OK);
    CHECK(!ui.lasso.active && pc_history_past_len(pc_engine_session_active(session)->history) == history + 1);
    CHECK(pc_doc_arc_get(pc_engine_session_active(session)->doc)->has_selection);
    session->authorize = deny;
    CHECK(event(session, &ui, PC_UI_POINTER_DOWN, 20, 20, 0) == PC_ERR_INVALID);
    CHECK(!ui.lasso.active && ui.lasso.source == NULL);
    session->authorize = NULL;
    CHECK(key(session, &ui, PC_KEY_CLOSEBRACKET, 0) == PC_OK && ui.tool_options.magnetic_width == 11);
    CHECK(key(session, &ui, PC_KEY_OPENBRACKET, 0) == PC_OK && ui.tool_options.magnetic_width == 10);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(session);
}
static void test_saved_polygon_resume_and_malformed_anchors(void)
{
    pc_engine_session *session = new_session();
    pc_ui_state ui, restored;
    pc_ui_state_init(&ui);
    pc_ui_state_init(&restored);
    ui.tool = PC_TOOL_POLYGON_LASSO;
    press(session, &ui, 10, 10, 0);
    press(session, &ui, 70, 10, 0);
    press(session, &ui, 70, 70, 0);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = NULL;
    CHECK(pc_ui_state_to_json(&ui, doc, &root, NULL) == PC_OK);
    pc_buf text = PC_BUF_INIT;
    yyjson_doc *parsed = NULL;
    CHECK(pc_json_write(root, 0, &text, NULL) == PC_OK);
    CHECK(pc_json_parse(pc_buf_cstr(&text), text.len, &parsed, NULL) == PC_OK);
    CHECK(pc_ui_state_from_json(&restored, yyjson_doc_get_root(parsed), NULL) == PC_OK);
    CHECK(restored.lasso.active && !restored.lasso.down && restored.lasso.path.n == 3);
    CHECK(restored.lasso.path.v != ui.lasso.path.v);
    yyjson_doc_free(parsed);
    pc_buf_fini(&text);
    yyjson_mut_doc_free(doc);
    const char *bad = "{\"active\":true,\"tool\":8,\"document\":1,\"revision\":0,\"layer\":0,\"mode\":"
                      "\"replace\",\"polygonal\":false,\"initialAlt\":false,\"altReleased\":false,\"path\":[["
                      "1,1]],\"live\":[],\"anchors\":[4]}";
    CHECK(pc_json_parse(bad, strlen(bad), &parsed, NULL) == PC_OK);
    CHECK(pc_ui_lasso_from_json(yyjson_doc_get_root(parsed), &restored.lasso, NULL) == PC_ERR_INVALID);
    CHECK(restored.lasso.path.n == 3 && restored.lasso.active);
    yyjson_doc_free(parsed);
    CHECK(key(session, &restored, PC_KEY_ENTER, 0) == PC_OK && !restored.lasso.active);
    pc_ui_lasso_sync(session, &ui);
    CHECK(!ui.lasso.active);
    pc_ui_state_fini(&ui);
    pc_ui_state_fini(&restored);
    pc_engine_session_free(session);
}
static void test_magnetic_all_modes_and_depths(void)
{
    const char *modes[] = {"rgb", "gray", "cmyk", "lab"};
    unsigned depths[] = {8, 16, 32};
    for (size_t mode = 0; mode < 4; mode++)
        for (size_t depth = 0; depth < 3; depth++) {
            pc_engine_session *session = NULL;
            CHECK(pc_engine_session_new(&session, NULL) == PC_OK);
            char params[160];
            (void)snprintf(
                params, sizeof params,
                "{\"width\":32,\"height\":32,\"mode\":\"%s\",\"depth\":%u,\"background\":\"transparent\"}",
                modes[mode], depths[depth]);
            pc_json_value out = PC_JSON_VALUE_INIT;
            CHECK(pc_engine_session_execute_json(session, "file.new", params, &out, NULL) == PC_OK);
            pc_json_value_fini(&out);
            pc_ui_state ui;
            pc_ui_state_init(&ui);
            ui.tool = PC_TOOL_MAGNETIC_LASSO;
            size_t history = pc_history_past_len(pc_engine_session_active(session)->history);
            press(session, &ui, 5, 5, PC_UI_MOD_ALT);
            press(session, &ui, 25, 5, PC_UI_MOD_ALT);
            press(session, &ui, 25, 25, PC_UI_MOD_ALT);
            CHECK(key(session, &ui, PC_KEY_ENTER, PC_UI_MOD_ALT) == PC_OK);
            CHECK(pc_doc_arc_get(pc_engine_session_active(session)->doc)->has_selection);
            CHECK(pc_history_past_len(pc_engine_session_active(session)->history) == history + 1);
            CHECK(pc_engine_session_execute_json(session, "edit.undo", "{}", &out, NULL) == PC_OK);
            pc_json_value_fini(&out);
            CHECK(!pc_doc_arc_get(pc_engine_session_active(session)->doc)->has_selection);
            pc_ui_state_fini(&ui);
            pc_engine_session_free(session);
        }
}
static void test_magnetic_actual_edges_all_formats_keep_source_immutable(void)
{
    const char *modes[] = {"rgb", "gray", "cmyk", "lab"};
    unsigned depths[] = {8, 16, 32};
    for (size_t mode = 0; mode < 4; mode++)
        for (size_t depth = 0; depth < 3; depth++) {
            pc_engine_session *s = NULL;
            CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
            char params[128];
            (void)snprintf(params, sizeof params,
                           "{\"width\":100,\"height\":100,\"mode\":\"%s\",\"depth\":%u}", modes[mode],
                           depths[depth]);
            pc_json_value out = PC_JSON_VALUE_INIT;
            CHECK(pc_engine_session_execute_json(s, "file.new", params, &out, NULL) == PC_OK);
            pc_json_value_fini(&out);
            CHECK(pc_engine_session_edit(s, "square", square, NULL, NULL) == PC_OK);
            const pc_doc_arc *before = pc_engine_session_active(s)->doc;
            pc_ui_state ui;
            pc_ui_state_init(&ui);
            ui.tool = PC_TOOL_MAGNETIC_LASSO;
            CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 21, 40, 0) == PC_OK);
            CHECK(ui.lasso.path.n == 1);
            if (ui.lasso.path.n) {
                CHECK_NEAR(ui.lasso.path.v[0][0], 20, 2);
                CHECK_NEAR(ui.lasso.path.v[0][1], 40, 2);
            }
            CHECK(pc_engine_session_active(s)->doc == before);
            CHECK(key(s, &ui, PC_KEY_ESCAPE, 0) == PC_OK);
            CHECK(pc_engine_session_active(s)->doc == before);
            pc_ui_state_fini(&ui);
            pc_engine_session_free(s);
        }
}
int main(int argc, char **argv)
{
    RUN(test_magnetic_actual_edges_all_formats_keep_source_immutable);
    RUN(test_magnetic_all_modes_and_depths);
    RUN(test_saved_polygon_resume_and_malformed_anchors);
    RUN(test_polygon_pending_close_undo_and_stale);
    RUN(test_polygon_latched_modes_shift_and_alt_freehand);
    RUN(test_magnetic_live_fastening_retract_and_source_scope);
    return pc_t_finish();
}
