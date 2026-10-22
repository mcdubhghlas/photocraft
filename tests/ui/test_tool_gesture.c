#include "harness.h"
#include "engine/floating.h"
#include "ui/move_mods.h"
#include "ui/tool_gesture.h"
#include "ui/shortcuts.h"
#include "compose/compose.h"
#include <math.h>
#include <string.h>
static pc_engine_session *session(unsigned depth)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char p[192];
    (void)snprintf(p, sizeof p, "{\"width\":48,\"height\":40,\"background\":\"transparent\",\"depth\":%u}",
                   depth);
    pc_json_value result = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.new", p, &result, NULL) == PC_OK);
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
    if (rc != PC_OK) {
        (void)fprintf(stderr, "gesture: %s\n", err.msg);
    }
    pc_json_value_fini(&result);
    return rc;
}
static pc_status event_p(pc_ui_tool_gesture *g, pc_engine_session *s, pc_ui_state *ui,
                         pc_ui_pointer_kind kind, float x, float y, float pressure, unsigned mods)
{
    pc_json_value result = PC_JSON_VALUE_INIT;
    pc_error err;
    pc_status rc = pc_ui_tool_gesture_event(
        g, s, ui, kind, (pc_ui_tool_point){.x = x, .y = y, .pressure = pressure}, mods, 1, &result, &err);
    if (rc != PC_OK) {
        (void)fprintf(stderr, "gesture: %s\n", err.msg);
    }
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
static void test_shift_constrains_painting_stroke(void)
{
    pc_engine_session *s = session(8);
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_BRUSH;
    pc_ui_tool_gesture g = {0};
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 10, 10, PC_UI_MOD_SHIFT) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_MOVE, 40, 12, PC_UI_MOD_SHIFT) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_MOVE, 70, 25, PC_UI_MOD_SHIFT) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_UP, 80, 30, PC_UI_MOD_SHIFT) == PC_OK);
    const yyjson_mut_val *p = last_journal(s, "paint.stroke");
    CHECK(p != NULL);
    const yyjson_mut_val *pts = yyjson_mut_obj_get(p, "points");
    size_t n = yyjson_mut_arr_size(pts);
    CHECK(n >= 2);
    for (size_t i = 0; i < n; i++) {
        const yyjson_mut_val *pt = yyjson_mut_arr_get(pts, i);
        CHECK_NEAR(yyjson_mut_get_num(yyjson_mut_arr_get(pt, 1)), 10, 1e-6);
    }
    const yyjson_mut_val *last = yyjson_mut_arr_get(pts, n - 1);
    CHECK_NEAR(yyjson_mut_get_num(yyjson_mut_arr_get(last, 0)), 80, 1e-6);
    pc_ui_tool_gesture_fini(&g);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
static void test_shift_click_connects_line_with_pressure(void)
{
    pc_engine_session *s = session(8);
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_BRUSH;
    pc_ui_tool_gesture g = {0};
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    /* A free stroke ending at (20,20) at pen pressure 0.35. */
    CHECK(event_p(&g, s, &ui, PC_UI_POINTER_DOWN, 10, 10, 0.2f, 0) == PC_OK);
    CHECK(event_p(&g, s, &ui, PC_UI_POINTER_MOVE, 20, 20, 0.35f, 0) == PC_OK);
    CHECK(event_p(&g, s, &ui, PC_UI_POINTER_UP, 20, 20, 0.35f, 0) == PC_OK);
    size_t before = pc_history_past_len(st->history);
    /* Shift-click at (100,100): a straight line from (20,20), one undo step. */
    CHECK(event_p(&g, s, &ui, PC_UI_POINTER_DOWN, 100, 100, 0.85f, PC_UI_MOD_SHIFT) == PC_OK);
    CHECK(event_p(&g, s, &ui, PC_UI_POINTER_UP, 100, 100, 0.85f, PC_UI_MOD_SHIFT) == PC_OK);
    CHECK(pc_history_past_len(st->history) == before + 1);
    const yyjson_mut_val *p = last_journal(s, "paint.stroke");
    const yyjson_mut_val *pts = p ? yyjson_mut_obj_get(p, "points") : NULL;
    CHECK(yyjson_mut_arr_size(pts) == 2);
    const yyjson_mut_val *a = yyjson_mut_arr_get(pts, 0);
    const yyjson_mut_val *b = yyjson_mut_arr_get(pts, 1);
    CHECK_NEAR(yyjson_mut_get_num(yyjson_mut_arr_get(a, 0)), 20, 1e-6);
    CHECK_NEAR(yyjson_mut_get_num(yyjson_mut_arr_get(a, 1)), 20, 1e-6);
    CHECK_NEAR(yyjson_mut_get_num(yyjson_mut_arr_get(a, 2)), 0.35, 1e-6);
    CHECK_NEAR(yyjson_mut_get_num(yyjson_mut_arr_get(b, 0)), 100, 1e-6);
    CHECK_NEAR(yyjson_mut_get_num(yyjson_mut_arr_get(b, 2)), 0.85, 1e-6);
    /* A dab with no previous stroke does not connect. */
    pc_engine_session *s2 = session(8);
    pc_ui_tool_gesture g2 = {0};
    CHECK(event(&g2, s2, &ui, PC_UI_POINTER_DOWN, 30, 30, PC_UI_MOD_SHIFT) == PC_OK);
    CHECK(event(&g2, s2, &ui, PC_UI_POINTER_UP, 30, 30, PC_UI_MOD_SHIFT) == PC_OK);
    const yyjson_mut_val *p2 = last_journal(s2, "paint.stroke");
    const yyjson_mut_val *pts2 = p2 ? yyjson_mut_obj_get(p2, "points") : NULL;
    CHECK(yyjson_mut_arr_size(pts2) == 1);
    pc_ui_tool_gesture_fini(&g2);
    pc_ui_tool_gesture_fini(&g);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
    pc_engine_session_free(s2);
}
static void test_gradient_shift_snaps_to_45(void)
{
    pc_engine_session *s = session(8);
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_GRADIENT;
    pc_ui_tool_gesture g = {0};
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 10, 10, PC_UI_MOD_SHIFT) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_UP, 90, 22, PC_UI_MOD_SHIFT) == PC_OK);
    const yyjson_mut_val *p = last_journal(s, "paint.gradient");
    const yyjson_mut_val *to = p ? yyjson_mut_obj_get(p, "to") : NULL;
    CHECK(to != NULL);
    CHECK_NEAR(yyjson_mut_get_num(yyjson_mut_arr_get(to, 0)), 90, 1e-6);
    CHECK_NEAR(yyjson_mut_get_num(yyjson_mut_arr_get(to, 1)), 10, 1e-6); /* horizontal */
    /* Diagonal: equal components. */
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 10, 10, PC_UI_MOD_SHIFT) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_UP, 90, 85, PC_UI_MOD_SHIFT) == PC_OK);
    p = last_journal(s, "paint.gradient");
    to = p ? yyjson_mut_obj_get(p, "to") : NULL;
    double tx = yyjson_mut_get_num(yyjson_mut_arr_get(to, 0));
    double ty = yyjson_mut_get_num(yyjson_mut_arr_get(to, 1));
    CHECK(fabs(tx - ty) < 1e-6 && tx > 80);
    pc_ui_tool_gesture_fini(&g);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
static void test_alt_flips_dodge_and_burn(void)
{
    pc_engine_session *s = session(8);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "edit.fill", "{\"color\":\"#808080\"}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_DODGE;
    pc_ui_tool_gesture g = {0};
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 10, 10, 0) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_UP, 20, 20, 0) == PC_OK);
    CHECK(last_journal(s, "paint.dodge") != NULL);
    CHECK(last_journal(s, "paint.burn") == NULL);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 10, 30, PC_UI_MOD_ALT) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_UP, 20, 30, PC_UI_MOD_ALT) == PC_OK);
    CHECK(last_journal(s, "paint.burn") != NULL); /* Alt flipped Dodge -> Burn */
    pc_ui_tool_gesture_fini(&g);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
static void test_red_eye_and_quick_selection_dispatch(void)
{
    CHECK(pc_ui_tool_gesture_supported(PC_TOOL_RED_EYE));
    CHECK(pc_ui_tool_gesture_supported(PC_TOOL_QUICK_SELECTION));
    pc_engine_session *s = session(8);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "edit.fill", "{\"color\":\"#ff0000\"}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    pc_ui_tool_gesture g = {0};
    /* Red Eye click (over red pixels so the kernel has something to darken). */
    ui.tool = PC_TOOL_RED_EYE;
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 20, 20, 0) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_UP, 20, 20, 0) == PC_OK);
    const yyjson_mut_val *p = last_journal(s, "paint.redEye");
    CHECK(p != NULL);
    CHECK_NEAR(yyjson_mut_get_num(yyjson_mut_obj_get(p, "pupilSize")), 50, 1e-6);
    /* Quick Selection drag, Alt subtracts. */
    ui.tool = PC_TOOL_QUICK_SELECTION;
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 5, 5, 0) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_MOVE, 15, 15, 0) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_UP, 25, 25, 0) == PC_OK);
    p = last_journal(s, "select.quick");
    CHECK(p != NULL);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(p, "mode")), "add");
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 5, 5, PC_UI_MOD_ALT) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_UP, 25, 25, PC_UI_MOD_ALT) == PC_OK);
    p = last_journal(s, "select.quick");
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(p, "mode")), "subtract");
    pc_ui_tool_gesture_fini(&g);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
static void test_color_replacement_records_foreground(void)
{
    pc_engine_session *s = session(8);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "edit.fill", "{\"color\":\"#808080\"}", &out, NULL) == PC_OK);
    CHECK(pc_engine_session_execute_json(s, "tools.setColors", "{\"foreground\":[0.0705882,0.2039216,0.3372549,1]}",
                                         &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_COLOR_REPLACEMENT;
    pc_ui_tool_gesture g = {0};
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 10, 10, 0) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_UP, 20, 20, 0) == PC_OK);
    const yyjson_mut_val *p = last_journal(s, "paint.colorReplacement");
    CHECK(p != NULL);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(p, "color")), "#123456");
    pc_ui_tool_gesture_fini(&g);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
static void test_stroke_oracle_at_all_depths(void)
{
    unsigned depths[] = {8, 16, 32};
    for (size_t i = 0; i < 3; i++) {
        pc_engine_session *s = session(depths[i]), *oracle = session(depths[i]);
        pc_ui_state ui;
        pc_ui_state_init(&ui);
        ui.tool = PC_TOOL_BRUSH;
        pc_ui_tool_gesture gesture = {0};
        uint64_t rev = pc_engine_session_active(s)->revision;
        CHECK(event(&gesture, s, &ui, PC_UI_POINTER_DOWN, 10, 10, 0) == PC_OK);
        CHECK(event(&gesture, s, &ui, PC_UI_POINTER_MOVE, 20, 20, 0) == PC_OK);
        CHECK(pc_engine_session_active(s)->revision == rev);
        CHECK(event(&gesture, s, &ui, PC_UI_POINTER_UP, 30, 20, 0) == PC_OK);
        CHECK(pc_engine_session_active(s)->revision == rev + 1 && !gesture.active);
        pc_json_value result = PC_JSON_VALUE_INIT;
        CHECK(pc_engine_session_execute_json(oracle, "paint.stroke",
                                             "{\"points\":[[10,10,1,0,0,0,0,0],[20,20,1,0,0,0,0,0],[30,20,1,"
                                             "0,0,0,0,0]],\"zoom\":1,\"sampleAllLayers\":false}",
                                             &result, NULL) == PC_OK);
        pc_json_value_fini(&result);
        pc_compose_buffer a = {0}, b = {0};
        const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
        const pc_document *ref = pc_doc_arc_get(pc_engine_session_active(oracle)->doc);
        CHECK(pc_compose_render(doc, pc_rect_from_size(doc->size), &a, NULL) == PC_OK);
        CHECK(pc_compose_render(ref, pc_rect_from_size(ref->size), &b, NULL) == PC_OK);
        CHECK(pc_compose_buffer_eq(&a, &b));
        pc_compose_buffer_fini(&a);
        pc_compose_buffer_fini(&b);
        CHECK(pc_engine_session_execute_json(s, "edit.undo", "{}", &result, NULL) == PC_OK);
        pc_json_value_fini(&result);
        pc_ui_tool_gesture_fini(&gesture);
        pc_ui_state_fini(&ui);
        pc_engine_session_free(s);
        pc_engine_session_free(oracle);
    }
}
static void test_selection_options_and_cancel(void)
{
    pc_engine_session *s = session(16);
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    pc_ui_tool_gesture g = {0};
    ui.tool = PC_TOOL_RECT_MARQUEE;
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 5, 6, 0) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_UP, 15, 16, 0) == PC_OK);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    CHECK(pc_surface_sample_channel(&doc->selection, 8, 8, 0) == 1);
    CHECK(pc_surface_sample_channel(&doc->selection, 20, 20, 0) == 0);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 20, 20, PC_UI_MOD_SHIFT) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_UP, 30, 30, 0) == PC_OK);
    doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    CHECK(pc_surface_sample_channel(&doc->selection, 8, 8, 0) == 1);
    CHECK(pc_surface_sample_channel(&doc->selection, 25, 25, 0) == 1);
    uint64_t revision = pc_engine_session_active(s)->revision;
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 10, 10, 0) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_CANCEL, 0, 0, 0) == PC_OK);
    CHECK(!g.active && pc_engine_session_active(s)->revision == revision);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 10, 10, 0) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "select.deselect", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_UP, 20, 20, 0) == PC_ERR_INVALID);
    CHECK(!g.active);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, NAN, 0, 0) == PC_ERR_INVALID);
    CHECK(!g.active);
    pc_ui_tool_gesture_fini(&g);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
static void test_move_selected_pixels_preview_float_copy_drop_and_cancel(void)
{
    const unsigned depths[] = {8, 16, 32};
    for (size_t depth = 0; depth < 3; depth++)
        for (unsigned copy = 0; copy < 2; copy++) {
            pc_engine_session *s = session(depths[depth]);
            pc_json_value out = PC_JSON_VALUE_INIT;
            CHECK(pc_engine_session_execute_json(s, "edit.fill", "{\"color\":\"#ff0000\"}", &out, NULL) ==
                  PC_OK);
            CHECK(pc_engine_session_execute_json(
                      s, "select.rect", "{\"x\":0,\"y\":0,\"width\":10,\"height\":10}", &out, NULL) == PC_OK);
            pc_ui_state ui;
            pc_ui_state_init(&ui);
            ui.tool = PC_TOOL_MOVE;
            pc_ui_tool_gesture g = {0};
            const pc_engine_doc_state *state = pc_engine_session_active(s);
            pc_layer_id layer = state->active_layer;
            size_t history = pc_history_past_len(state->history);
            const pc_doc_arc *original = state->doc;
            CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 2, 2, copy ? PC_UI_MOD_ALT : 0) == PC_OK);
            CHECK(event(&g, s, &ui, PC_UI_POINTER_MOVE, 7, 2, 0) == PC_OK);
            CHECK(g.move_preview != NULL && state->doc == original);
            if (g.move_preview) {
                const pc_document *doc = pc_doc_arc_get(g.move_preview);
                const pc_surface *surface = pc_layer_surface(pc_document_layer(doc, layer));
                float pixel[4];
                pc_surface_rgba(surface, 2, 2, pixel);
                CHECK(fabsf(pixel[3] - (copy ? 1 : 0)) < .001f);
                pc_surface_rgba(surface, 7, 2, pixel);
                CHECK(pixel[0] > .99f && pixel[3] > .99f);
                CHECK(pc_surface_sample_channel(&doc->selection, 2, 2, 0) == 0);
                CHECK(pc_surface_sample_channel(&doc->selection, 7, 2, 0) == 1);
            }
            CHECK(event(&g, s, &ui, PC_UI_POINTER_UP, 7, 2, 0) == PC_OK);
            CHECK(state->doc == original && pc_history_past_len(state->history) == history);
            int32_t offset[2];
            CHECK(pc_engine_floating_offset(s, offset) && offset[0] == 5 && offset[1] == 0);
            pc_doc_arc *display = NULL, *cached = NULL;
            CHECK(pc_engine_floating_document(s, &display, NULL) == PC_OK && display != NULL);
            CHECK(pc_engine_floating_document(s, &cached, NULL) == PC_OK && cached == display);
            display = pc_doc_arc_retain(display);
            CHECK(pc_engine_session_execute_json(s, "select.drop", "{}", &out, NULL) == PC_OK);
            CHECK(pc_history_past_len(state->history) == history + 1);
            const pc_document *dropped = pc_doc_arc_get(state->doc);
            pc_compose_buffer a = {0}, b = {0};
            CHECK(pc_compose_render(dropped, pc_rect_from_size(dropped->size), &a, NULL) == PC_OK);
            CHECK(pc_compose_render(pc_doc_arc_get(display), pc_rect_from_size(dropped->size), &b, NULL) ==
                  PC_OK);
            CHECK(pc_compose_buffer_eq(&a, &b));
            pc_compose_buffer_fini(&a);
            pc_compose_buffer_fini(&b);
            pc_doc_arc_release(display);
            CHECK(pc_engine_session_execute_json(s, "edit.undo", "{}", &out, NULL) == PC_OK);
            CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 2, 2, 0) == PC_OK);
            CHECK(event(&g, s, &ui, PC_UI_POINTER_UP, 9, 4, 0) == PC_OK);
            CHECK(pc_engine_floating_offset(s, offset));
            CHECK(pc_engine_session_execute_json(s, "edit.undo", "{}", &out, NULL) == PC_OK);
            CHECK(!pc_engine_floating_offset(s, offset));
            CHECK(pc_history_past_len(state->history) == history);
            pc_ui_tool_gesture_fini(&g);
            pc_ui_state_fini(&ui);
            pc_json_value_fini(&out);
            pc_engine_session_free(s);
        }
}
static void test_move_directions_and_hysteresis(void)
{
    double out[2], delta[2] = {10, 3};
    pc_ui_move_constrain(delta, NULL, 4, out);
    CHECK(out[0] == 10 && out[1] == 0);
    const double previous[2] = {20, 0}, nearby[2] = {20, 21}, vertical[2] = {20, 40};
    pc_ui_move_constrain(nearby, previous, 4, out);
    CHECK(out[0] == 20 && out[1] == 0);
    pc_ui_move_constrain(vertical, previous, 4, out);
    CHECK(out[0] == 0 && out[1] == 40);
    const double diagonal[2] = {10, 9};
    pc_ui_move_constrain(diagonal, NULL, 8, out);
    CHECK(fabs(out[0] - out[1]) < 1e-9 && out[0] > 9);
    const double negative[2] = {-2, -9};
    pc_ui_move_constrain(negative, NULL, 4, out);
    CHECK(out[0] == 0 && out[1] == -9);
    const double bad[2] = {NAN, INFINITY};
    pc_ui_move_constrain(bad, NULL, 4, out);
    CHECK(isnan(out[0]) && isinf(out[1]));
}
static void test_move_alt_duplicate_shift_and_single_history(void)
{
    pc_engine_session *s = session(16);
    pc_json_value result = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "paint.pencil",
                                         "{\"points\":[[10,10],[15,10]],\"size\":4,\"color\":\"#c32044\"}",
                                         &result, NULL) == PC_OK);
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_MOVE;
    ui.tool_options.move_auto_select = false;
    pc_ui_tool_gesture g = {0};
    const pc_engine_doc_state *state = pc_engine_session_active(s);
    pc_layer_id original = state->active_layer;
    const pc_doc_arc *before = state->doc;
    size_t n = pc_doc_arc_get(before)->nlayers, history = pc_history_past_len(state->history);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_DOWN, 12, 10, PC_UI_MOD_ALT) == PC_OK);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_MOVE, 18, 12, PC_UI_MOD_SHIFT) == PC_OK);
    CHECK(state->doc == before && g.move_preview && pc_doc_arc_get(g.move_preview)->nlayers == n + 1);
    CHECK(event(&g, s, &ui, PC_UI_POINTER_UP, 22, 13, PC_UI_MOD_SHIFT) == PC_OK);
    CHECK(pc_doc_arc_get(state->doc)->nlayers == n + 1 && state->active_layer != original);
    CHECK(pc_history_past_len(state->history) == history + 1);
    const pc_document *doc = pc_doc_arc_get(state->doc);
    pc_rect original_bounds = pc_surface_content_bounds(pc_layer_surface(pc_document_layer(doc, original)));
    pc_rect moved = pc_surface_content_bounds(pc_layer_surface(pc_document_layer(doc, state->active_layer)));
    CHECK(moved.x0 == original_bounds.x0 + 10 && moved.y0 == original_bounds.y0);
    CHECK(pc_engine_session_execute_json(s, "edit.undo", "{}", &result, NULL) == PC_OK);
    CHECK(pc_doc_arc_get(state->doc)->nlayers == n && state->active_layer == original);
    CHECK(pc_engine_session_execute_json(s, "edit.redo", "{}", &result, NULL) == PC_OK);
    CHECK(pc_doc_arc_get(state->doc)->nlayers == n + 1 && state->active_layer != original);
    pc_ui_tool_gesture_fini(&g);
    pc_ui_state_fini(&ui);
    pc_json_value_fini(&result);
    pc_engine_session_free(s);
}
int main(int argc, char **argv)
{
    RUN(test_move_directions_and_hysteresis);
    RUN(test_move_alt_duplicate_shift_and_single_history);
    RUN(test_move_selected_pixels_preview_float_copy_drop_and_cancel);
    RUN(test_stroke_oracle_at_all_depths);
    RUN(test_selection_options_and_cancel);
    RUN(test_shift_constrains_painting_stroke);
    RUN(test_shift_click_connects_line_with_pressure);
    RUN(test_gradient_shift_snaps_to_45);
    RUN(test_alt_flips_dodge_and_burn);
    RUN(test_red_eye_and_quick_selection_dispatch);
    RUN(test_color_replacement_records_foreground);
    return pc_t_finish();
}
