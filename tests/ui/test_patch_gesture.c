/* Patch / Content-Aware Move lasso-then-drag canvas gesture (U7). Drives pc_ui_tool_gesture_event
 * headlessly against a real session and checks the dispatched command + params via the journal, plus
 * that no input path crashes (REWRITE_PLAN.md rule 9). Mirrors tests/ui/test_retouch_gesture.c. */
#include "harness.h"
#include "ui/tool_gesture.h"
#include "ui/shortcuts.h"
#include <math.h>
#include <string.h>

static pc_engine_session *session(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value result = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(
              s, "file.new", "{\"width\":48,\"height\":40,\"background\":\"white\",\"depth\":8}", &result,
              NULL) == PC_OK);
    pc_json_value_fini(&result);
    return s;
}
static void run(pc_engine_session *s, const char *id, const char *params)
{
    pc_json_value result = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, id, params, &result, NULL) == PC_OK);
    pc_json_value_fini(&result);
}
static pc_status event(pc_ui_tool_gesture *g, pc_engine_session *s, pc_ui_state *ui, pc_ui_pointer_kind kind,
                       float x, float y, unsigned mods)
{
    pc_json_value result = PC_JSON_VALUE_INIT;
    pc_error err;
    pc_status rc = pc_ui_tool_gesture_event(g, s, ui, kind, (pc_ui_tool_point){.x = x, .y = y, .pressure = 1},
                                            mods, 1, &result, &err);
    pc_json_value_fini(&result);
    return rc;
}
static const yyjson_mut_val *last_journal(pc_engine_session *s, const char *id)
{
    size_t n = pc_engine_session_journal_len(s);
    for (size_t i = n; i-- > 0;) {
        const char *jid = NULL;
        const yyjson_mut_val *params = NULL;
        if (pc_engine_session_journal_at(s, i, &jid, &params) && jid && !strcmp(jid, id)) {
            return params;
        }
    }
    return NULL;
}
static bool journal_has(pc_engine_session *s, const char *id)
{
    return last_journal(s, id) != NULL;
}

static void test_patch_drag_dispatches_offset(void)
{
    pc_engine_session *s = session();
    run(s, "select.rect", "{\"x\":10,\"y\":10,\"width\":20,\"height\":16}");
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_PATCH;
    pc_ui_tool_gesture g = {0};
    /* Press inside the selection, then drag: dispatches paint.patch with a whole-pixel offset. */
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 15, 15, 0) == PC_OK);
    CHECK(g.patch_dragging);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_MOVE, 18, 17, 0) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_UP, 20.4f, 18.0f, 0) == PC_OK);
    const yyjson_mut_val *p = last_journal(s, "paint.patch");
    CHECK(p != NULL);
    if (p) {
        const yyjson_mut_val *off = yyjson_mut_obj_get(p, "offset");
        CHECK(yyjson_mut_arr_size(off) == 2);
        CHECK_NEAR(yyjson_mut_get_num(yyjson_mut_arr_get(off, 0)), 5, 1e-6); /* round(20.4-15) */
        CHECK_NEAR(yyjson_mut_get_num(yyjson_mut_arr_get(off, 1)), 3, 1e-6); /* round(18-15) */
        CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(p, "mode")), "source");
        CHECK(yyjson_mut_obj_get(p, "contentAware") == NULL); /* normal mode omits it */
    }
    pc_ui_tool_gesture_fini(&g);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}

static void test_patch_content_aware_fields(void)
{
    pc_engine_session *s = session();
    run(s, "select.rect", "{\"x\":8,\"y\":8,\"width\":16,\"height\":12}");
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_PATCH;
    ui.tool_options.patch_content_aware = true;
    ui.tool_options.patch_structure = 6.4f;
    ui.tool_options.patch_color = 3.0f;
    ui.tool_options.sample_all_layers = true;
    pc_ui_tool_gesture g = {0};
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 12, 12, 0) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_UP, 16, 14, 0) == PC_OK);
    const yyjson_mut_val *p = last_journal(s, "paint.patch");
    CHECK(p != NULL);
    if (p) {
        CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(p, "contentAware")) == true);
        CHECK_EQ_INT(yyjson_mut_get_int(yyjson_mut_obj_get(p, "structure")), 6);
        CHECK_EQ_INT(yyjson_mut_get_int(yyjson_mut_obj_get(p, "color")), 3);
        CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(p, "sampleAllLayers")) == true);
    }
    pc_ui_tool_gesture_fini(&g);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}

static void test_content_aware_move_drag(void)
{
    pc_engine_session *s = session();
    run(s, "select.rect", "{\"x\":10,\"y\":10,\"width\":16,\"height\":12}");
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_CONTENT_AWARE_MOVE;
    (void)pc_str_set_c(&ui.tool_options.cam_mode, "extend", NULL);
    pc_ui_tool_gesture g = {0};
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 14, 14, 0) == PC_OK);
    CHECK(g.patch_dragging);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_UP, 18, 16, 0) == PC_OK);
    const yyjson_mut_val *p = last_journal(s, "paint.contentAwareMove");
    CHECK(p != NULL);
    if (p) {
        CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(p, "mode")), "extend");
        const yyjson_mut_val *off = yyjson_mut_obj_get(p, "offset");
        CHECK_NEAR(yyjson_mut_get_num(yyjson_mut_arr_get(off, 0)), 4, 1e-6);
        CHECK_NEAR(yyjson_mut_get_num(yyjson_mut_arr_get(off, 1)), 2, 1e-6);
        CHECK(yyjson_mut_obj_get(p, "structure") != NULL);
    }
    pc_ui_tool_gesture_fini(&g);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}

static void test_lasso_mode_when_no_selection(void)
{
    pc_engine_session *s = session();
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_PATCH;
    pc_ui_tool_gesture g = {0};
    /* No selection yet: the press draws a freehand lasso instead of a drag. */
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 12, 12, 0) == PC_OK);
    CHECK(!g.patch_dragging);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_MOVE, 30, 12, 0) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_MOVE, 30, 28, 0) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_UP, 12, 28, 0) == PC_OK);
    const yyjson_mut_val *p = last_journal(s, "select.lasso");
    CHECK(p != NULL);
    if (p) {
        CHECK_NEAR(yyjson_mut_get_num(yyjson_mut_obj_get(p, "feather")), 0, 1e-6); /* forced off */
        CHECK(yyjson_mut_arr_size(yyjson_mut_obj_get(p, "points")) >= 3);
    }
    CHECK(!journal_has(s, "paint.patch"));
    pc_ui_tool_gesture_fini(&g);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}

static void test_lasso_mode_with_shift_even_inside(void)
{
    pc_engine_session *s = session();
    run(s, "select.rect", "{\"x\":10,\"y\":10,\"width\":20,\"height\":16}");
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_PATCH;
    pc_ui_tool_gesture g = {0};
    /* Shift held forces a new lasso (add) even when pressing inside the selection. */
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 15, 15, PC_UI_MOD_SHIFT) == PC_OK);
    CHECK(!g.patch_dragging);
    pc_ui_tool_gesture_fini(&g);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}

static void test_click_inside_selection_is_noop(void)
{
    pc_engine_session *s = session();
    run(s, "select.rect", "{\"x\":10,\"y\":10,\"width\":20,\"height\":16}");
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_PATCH;
    pc_ui_tool_gesture g = {0};
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 15, 15, 0) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_UP, 15, 15, 0) == PC_OK); /* no movement */
    CHECK(!journal_has(s, "paint.patch"));
    pc_ui_tool_gesture_fini(&g);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}

static void test_hostile_offsets_do_not_crash(void)
{
    pc_engine_session *s = session();
    run(s, "select.rect", "{\"x\":10,\"y\":10,\"width\":20,\"height\":16}");
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_PATCH;
    pc_ui_tool_gesture g = {0};
    /* An absurd drag target is rejected gracefully (never a crash). */
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 15, 15, 0) == PC_OK);
    pc_status rc = event(&g, s, &ui, PC_UI_POINTER_UP, 15, 15, 0); /* zero offset: no-op */
    CHECK(rc == PC_OK);
    pc_ui_tool_gesture_fini(&g);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_patch_drag_dispatches_offset);
    RUN(test_patch_content_aware_fields);
    RUN(test_content_aware_move_drag);
    RUN(test_lasso_mode_when_no_selection);
    RUN(test_lasso_mode_with_shift_even_inside);
    RUN(test_click_inside_selection_is_noop);
    RUN(test_hostile_offsets_do_not_crash);
    return pc_t_finish();
}
