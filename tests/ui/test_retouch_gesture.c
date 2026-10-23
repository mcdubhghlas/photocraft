/* Clone Stamp / Healing Brush / Pattern Stamp canvas gesture dispatch + Alt-set-source (U7).
 * Drives pc_ui_tool_gesture_event headlessly against a real session; checks the dispatched command
 * params and that no input path crashes (REWRITE_PLAN.md rule 9). */
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
static void test_clone_stamp_set_source_and_stroke(void)
{
    pc_engine_session *s = session();
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_CLONE_STAMP;
    pc_ui_tool_gesture g = {0};
    /* Alt-click sets the source and never starts a stroke. */
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 8, 8, PC_UI_MOD_ALT) == PC_OK);
    CHECK(g.has_clone_source);
    CHECK_NEAR(g.clone_source[0], 8, 1e-6);
    /* A stroke now dispatches paint.cloneStamp. */
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 20, 20, 0) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_MOVE, 24, 22, 0) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_UP, 28, 24, 0) == PC_OK);
    const yyjson_mut_val *p = last_journal(s, "paint.cloneStamp");
    CHECK(p != NULL);
    if (p) {
        CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(p, "aligned")) == true);
        CHECK(!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(p, "sampleLayer")), "current"));
        CHECK(yyjson_mut_obj_get(p, "opacity") != NULL);
        CHECK(yyjson_mut_obj_get(p, "flow") != NULL);
        CHECK(yyjson_mut_arr_size(yyjson_mut_obj_get(p, "points")) >= 2);
    }
    pc_ui_tool_gesture_fini(&g);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
static void test_clone_without_source_errors_gracefully(void)
{
    pc_engine_session *s = session();
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_CLONE_STAMP;
    pc_ui_tool_gesture g = {0};
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 10, 10, 0) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_MOVE, 14, 12, 0) == PC_OK);
    /* No source set: the engine refuses on commit, but the gesture never crashes. */
    pc_status up = event(&g, s, &ui, PC_UI_POINTER_UP, 18, 14, 0);
    CHECK(up != PC_OK);
    pc_ui_tool_gesture_fini(&g);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
static void test_healing_brush_stroke(void)
{
    pc_engine_session *s = session();
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_HEALING;
    pc_ui_tool_gesture g = {0};
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 6, 6, PC_UI_MOD_ALT) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 20, 20, 0) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_MOVE, 22, 22, 0) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_UP, 24, 24, 0) == PC_OK);
    const yyjson_mut_val *p = last_journal(s, "paint.healingBrush");
    CHECK(p != NULL);
    if (p) {
        CHECK(!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(p, "sampleLayer")), "current"));
        CHECK(yyjson_mut_obj_get(p, "opacity") == NULL); /* healing sends no opacity/flow */
    }
    pc_ui_tool_gesture_fini(&g);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
static void test_pattern_stamp_stroke(void)
{
    pc_engine_session *s = session();
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_PATTERN_STAMP;
    pc_ui_tool_gesture g = {0};
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 10, 10, 0) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_MOVE, 14, 14, 0) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_UP, 18, 18, 0) == PC_OK);
    const yyjson_mut_val *p = last_journal(s, "paint.patternStamp");
    CHECK(p != NULL);
    if (p) {
        CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(p, "aligned")) == true);
        CHECK(yyjson_mut_obj_get(p, "scale") != NULL);
        CHECK(yyjson_mut_obj_get(p, "angle") != NULL);
    }
    pc_ui_tool_gesture_fini(&g);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
static void test_supported(void)
{
    CHECK(pc_ui_tool_gesture_supported(PC_TOOL_CLONE_STAMP));
    CHECK(pc_ui_tool_gesture_supported(PC_TOOL_HEALING));
    CHECK(pc_ui_tool_gesture_supported(PC_TOOL_PATTERN_STAMP));
}
int main(int argc, char **argv)
{
    RUN(test_clone_stamp_set_source_and_stroke);
    RUN(test_clone_without_source_errors_gracefully);
    RUN(test_healing_brush_stroke);
    RUN(test_pattern_stamp_stroke);
    RUN(test_supported);
    return pc_t_finish();
}
