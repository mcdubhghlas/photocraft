/* Headless model tests for the Image > Analysis tools and panels (src/ui/analysis.c): geometry,
 * ruler readout math, hit testing, log-row selection and the Ruler/Count/Note pointer routing.
 * The shell overlays/panels/dialogs are exercised by a GL-gated render check in
 * tests/ui/test_canvas_overlays.c. */
#include "harness.h"
#include "ui/analysis.h"
#include "base/json.h"
#include <math.h>
#include <string.h>

static pc_status command(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, id, json, &out, NULL);
    pc_json_value_fini(&out);
    return rc;
}

static pc_engine_session *session(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(command(s, "file.new", "{\"width\":200,\"height\":100}") == PC_OK);
    return s;
}

static const pc_document *doc_of(pc_engine_session *s)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    return st ? pc_doc_arc_get(st->doc) : NULL;
}

static pc_status event(pc_ui_analysis *a, pc_engine_session *s, const pc_ui_state *ui,
                       pc_ui_pointer_kind kind, double x, double y, unsigned mods)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status rc = pc_ui_analysis_pointer(a, s, ui, kind, (pc_ui_tool_point){.x = (float)x, .y = (float)y},
                                          mods, 1.0, &out, &err);
    if (rc != PC_OK && rc != PC_ERR_NOT_FOUND)
        fprintf(stderr, "analysis pointer: %s\n", err.msg);
    pc_json_value_fini(&out);
    return rc;
}

static void click(pc_ui_analysis *a, pc_engine_session *s, const pc_ui_state *ui, double x, double y,
                  unsigned mods)
{
    CHECK(event(a, s, ui, PC_UI_POINTER_DOWN, x, y, mods) == PC_OK);
    CHECK(event(a, s, ui, PC_UI_POINTER_UP, x, y, mods) == PC_OK);
}

/* ------------------------------------------------------------------ pure math */

static void test_geometry(void)
{
    double a[2] = {0, 0}, b[2] = {3, 4};
    CHECK_NEAR(pc_ui_analysis_dist(a, b), 5.0, 1e-9);

    double from[2] = {0, 0}, out[2];
    pc_ui_analysis_constrain45(from, (double[]){10, 1}, true, out);
    CHECK_NEAR(out[1], 0.0, 1e-6); /* nearly horizontal snaps to the x axis */
    pc_ui_analysis_constrain45(from, (double[]){1, 10}, true, out);
    CHECK_NEAR(out[0], 0.0, 1e-6); /* nearly vertical snaps to the y axis */
    pc_ui_analysis_constrain45(from, (double[]){10, 7}, false, out);
    CHECK_NEAR(out[0], 10.0, 1e-9); /* no shift: unchanged */
    CHECK_NEAR(out[1], 7.0, 1e-9);

    CHECK_NEAR(pc_ui_analysis_tolerance(1.0), 7.0, 1e-9);
    CHECK_NEAR(pc_ui_analysis_tolerance(2.0), 3.5, 1e-9);
    CHECK_NEAR(pc_ui_analysis_tolerance(0.0), 700.0, 1e-9); /* clamped */

    pc_measurement_scale def = {0};
    CHECK(pc_measurement_scale_default(&def, NULL) == PC_OK);
    CHECK_NEAR(pc_ui_analysis_suggested_length(200, &def), 20.0, 1e-9);
    pc_measurement_scale_fini(&def);
}

static void test_ruler_readout(void)
{
    pc_ruler r = {.start = {0, 0}, .end = {30, 40}, .has_protractor = false};
    pc_measurement_scale def = {0};
    CHECK(pc_measurement_scale_default(&def, NULL) == PC_OK);
    pc_ui_ruler_readout out;
    CHECK(pc_ui_analysis_ruler_readout(&r, &def, false, &out));
    CHECK_NEAR(out.w, 30.0, 1e-9);
    CHECK_NEAR(out.h, 40.0, 1e-9);
    CHECK_NEAR(out.l1, 50.0, 1e-6);
    CHECK(isnan(out.l2));
    CHECK_NEAR(out.angle, -53.1301, 1e-3);
    CHECK(!out.has_protractor);
    pc_measurement_scale_fini(&def);

    /* A right-angle protractor arm reads 90 degrees. */
    pc_ruler p = {.start = {0, 0}, .end = {10, 0}, .has_protractor = true, .protractor = {0, 10}};
    CHECK(pc_ui_analysis_ruler_readout(&p, &def, false, &out));
    CHECK_NEAR(out.angle, 90.0, 1e-6);
    CHECK_NEAR(out.l2, 10.0, 1e-6);
    CHECK(out.has_protractor);

    /* use_scale multiplies the lengths by the scale factor, never the angle. */
    pc_measurement_scale sc = {.pixel_length = 10, .logical_length = 5};
    double f = pc_measurement_scale_factor(&sc);
    CHECK(pc_ui_analysis_ruler_readout(&r, &sc, true, &out));
    CHECK_NEAR(out.w, 30.0 * f, 1e-6);
    CHECK_NEAR(out.l1, 50.0 * f, 1e-5);
    CHECK_NEAR(out.angle, -53.1301, 1e-3);
}

static void test_log_selection(void)
{
    pc_ui_analysis a;
    pc_ui_analysis_init(&a);
    CHECK(!pc_ui_analysis_log_contains(&a, 5));
    CHECK(pc_ui_analysis_log_toggle(&a, 5) == PC_OK);
    CHECK(pc_ui_analysis_log_toggle(&a, 9) == PC_OK);
    CHECK(pc_ui_analysis_log_contains(&a, 5) && pc_ui_analysis_log_contains(&a, 9));
    CHECK(a.nlog_selected == 2);
    CHECK(pc_ui_analysis_log_toggle(&a, 5) == PC_OK); /* remove */
    CHECK(!pc_ui_analysis_log_contains(&a, 5) && a.nlog_selected == 1);
    uint64_t ids[3] = {1, 2, 3};
    CHECK(pc_ui_analysis_log_set(&a, ids, 3) == PC_OK);
    CHECK(a.nlog_selected == 3 && pc_ui_analysis_log_contains(&a, 2));
    pc_ui_analysis_log_clear(&a);
    CHECK(a.nlog_selected == 0);
    pc_ui_analysis_fini(&a);
}

/* ------------------------------------------------------------------ hit testing */

static void test_hit_tests(void)
{
    pc_engine_session *s = session();
    CHECK(command(s, "count.add", "{\"x\":20,\"y\":20}") == PC_OK);
    CHECK(command(s, "count.add", "{\"x\":60,\"y\":50}") == PC_OK);
    CHECK(command(s, "notes.add", "{\"x\":30,\"y\":40,\"author\":\"QA\"}") == PC_OK);
    const pc_document *doc = doc_of(s);
    CHECK(doc != NULL);
    size_t g = 99, i = 99;
    CHECK(pc_ui_analysis_nearest_count(&doc->measurement, (double[]){21, 21}, 5, &g, &i));
    CHECK(g == 0 && i == 0);
    CHECK(pc_ui_analysis_nearest_count(&doc->measurement, (double[]){59, 51}, 5, &g, &i));
    CHECK(i == 1);
    CHECK(!pc_ui_analysis_nearest_count(&doc->measurement, (double[]){150, 90}, 5, &g, &i));
    /* Note icon box at zoom 1 is 16x20 document px from the origin. */
    CHECK(pc_ui_analysis_nearest_note(doc->notes, doc->nnotes, (double[]){35, 45}, 1.0) == 0);
    CHECK(pc_ui_analysis_nearest_note(doc->notes, doc->nnotes, (double[]){5, 5}, 1.0) == -1);
    pc_engine_session_free(s);
}

/* ------------------------------------------------------------------ pointer routing */

static void test_pointer_ruler(void)
{
    pc_engine_session *s = session();
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_RULER;
    pc_ui_analysis a;
    pc_ui_analysis_init(&a);

    CHECK(event(&a, s, &ui, PC_UI_POINTER_DOWN, 10, 10, 0) == PC_OK);
    CHECK(event(&a, s, &ui, PC_UI_POINTER_MOVE, 60, 10, 0) == PC_OK);
    CHECK(event(&a, s, &ui, PC_UI_POINTER_UP, 60, 10, 0) == PC_OK);
    CHECK(doc_of(s)->measurement.has_ruler);
    CHECK_NEAR(doc_of(s)->measurement.ruler.end[0], 60.0, 1e-6);

    /* Drag the end handle with Shift from (60,10) toward (12,52): snaps vertical over the start. */
    CHECK(event(&a, s, &ui, PC_UI_POINTER_DOWN, 60, 10, 0) == PC_OK);
    CHECK(event(&a, s, &ui, PC_UI_POINTER_MOVE, 12, 52, PC_UI_MOD_SHIFT) == PC_OK);
    CHECK(event(&a, s, &ui, PC_UI_POINTER_UP, 12, 52, PC_UI_MOD_SHIFT) == PC_OK);
    CHECK_NEAR(doc_of(s)->measurement.ruler.end[0], 10.0, 1e-6);

    /* Alt-drag from the start point starts the protractor arm. */
    CHECK(event(&a, s, &ui, PC_UI_POINTER_DOWN, 10, 10, PC_UI_MOD_ALT) == PC_OK);
    CHECK(event(&a, s, &ui, PC_UI_POINTER_MOVE, 60, 10, 0) == PC_OK);
    CHECK(event(&a, s, &ui, PC_UI_POINTER_UP, 60, 10, 0) == PC_OK);
    CHECK(doc_of(s)->measurement.ruler.has_protractor);

    /* A click far away with no drag starts a new zero-length line and clears it. */
    CHECK(event(&a, s, &ui, PC_UI_POINTER_DOWN, 150, 80, 0) == PC_OK);
    CHECK(event(&a, s, &ui, PC_UI_POINTER_UP, 150, 80, 0) == PC_OK);
    CHECK(!doc_of(s)->measurement.has_ruler);
    /* The ruler records no history. */
    CHECK(pc_history_past_len(pc_engine_session_active(s)->history) == 0);

    pc_ui_analysis_fini(&a);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}

static void test_pointer_count(void)
{
    pc_engine_session *s = session();
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_COUNT_TOOL;
    pc_ui_analysis a;
    pc_ui_analysis_init(&a);

    click(&a, s, &ui, 20, 20, 0);
    click(&a, s, &ui, 50, 50, 0);
    CHECK(doc_of(s)->measurement.ncount_groups == 1);
    CHECK(doc_of(s)->measurement.count_groups[0].npoints == 2);
    /* Drag marker 0 to a new place. */
    CHECK(event(&a, s, &ui, PC_UI_POINTER_DOWN, 20.5, 20.5, 0) == PC_OK);
    CHECK(event(&a, s, &ui, PC_UI_POINTER_UP, 30, 25, 0) == PC_OK);
    CHECK_NEAR(doc_of(s)->measurement.count_groups[0].points[0][0], 30.0, 1e-6);
    /* Alt-click removes a marker. */
    click(&a, s, &ui, 50, 50, PC_UI_MOD_ALT);
    CHECK(doc_of(s)->measurement.count_groups[0].npoints == 1);

    pc_ui_analysis_fini(&a);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}

static void test_pointer_note(void)
{
    pc_engine_session *s = session();
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_NOTE;
    pc_ui_analysis a;
    pc_ui_analysis_init(&a);
    (void)snprintf(a.note_author, sizeof a.note_author, "QA");

    click(&a, s, &ui, 30, 40, 0);
    CHECK(doc_of(s)->nnotes == 1);
    CHECK(a.notes_panel && a.has_note_selected && a.note_selected == 0);
    CHECK(!strcmp(pc_str_c(&doc_of(s)->notes[0].author), "QA"));
    /* Drag the icon by its interior. */
    CHECK(event(&a, s, &ui, PC_UI_POINTER_DOWN, 32, 42, 0) == PC_OK);
    CHECK(event(&a, s, &ui, PC_UI_POINTER_UP, 62, 52, 0) == PC_OK);
    CHECK_NEAR(doc_of(s)->notes[0].position[0], 60.0, 1e-6);
    CHECK_NEAR(doc_of(s)->notes[0].position[1], 50.0, 1e-6);

    pc_ui_analysis_fini(&a);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}

static void test_hostile_and_inert(void)
{
    pc_engine_session *s = session();
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    pc_ui_analysis a;
    pc_ui_analysis_init(&a);

    /* A non-analysis tool is not ours. */
    ui.tool = PC_TOOL_MOVE;
    CHECK(event(&a, s, &ui, PC_UI_POINTER_DOWN, 10, 10, 0) == PC_ERR_NOT_FOUND);

    /* NaN coordinates are rejected, not crashed on. */
    ui.tool = PC_TOOL_RULER;
    CHECK(event(&a, s, &ui, PC_UI_POINTER_DOWN, NAN, 0, 0) == PC_ERR_INVALID);
    CHECK(event(&a, s, &ui, PC_UI_POINTER_DOWN, 0, INFINITY, 0) == PC_ERR_INVALID);

    /* With no document the tools are inert but still owned (no crash, no error). */
    pc_engine_session *empty = NULL;
    CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
    ui.tool = PC_TOOL_COUNT_TOOL;
    CHECK(event(&a, empty, &ui, PC_UI_POINTER_DOWN, 5, 5, 0) == PC_OK);
    pc_engine_session_free(empty);

    pc_ui_analysis_fini(&a);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_geometry);
    RUN(test_ruler_readout);
    RUN(test_log_selection);
    RUN(test_hit_tests);
    RUN(test_pointer_ruler);
    RUN(test_pointer_count);
    RUN(test_pointer_note);
    RUN(test_hostile_and_inert);
    return pc_t_finish();
}
