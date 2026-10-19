#include "harness.h"
#include "ui/snap.h"
#include <math.h>
static pc_engine_session *session(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value value = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.new", "{\"width\":400,\"height\":300}", &value, NULL) ==
          PC_OK);
    pc_json_value_fini(&value);
    return s;
}
static void test_snap_capture_threshold_disable_smart_and_failure(void)
{
    pc_engine_session *session_ = session();
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    pc_ui_snap s = {0};
    double start[2] = {20, 30}, out[2], p[2] = {3, 153};
    ui.extras.snap = true;
    CHECK(pc_ui_snap_begin(&s, session_, &ui, start, NULL, NULL, 0, false, false, false, NULL) == PC_OK);
    CHECK(pc_ui_snap_point(&s, p, 4, false, out, NULL) == PC_OK);
    CHECK(out[0] == 0 && out[1] == 150 && s.nlines == 2);
    CHECK(pc_ui_snap_point(&s, p, 4, true, out, NULL) == PC_OK);
    CHECK(out[0] == 3 && out[1] == 153 && s.nlines == 0);
    CHECK(pc_ui_snap_point(&s, p, 2, false, out, NULL) == PC_OK);
    CHECK(out[0] == 3 && out[1] == 153 && s.nlines == 0);
    double invalid[4] = {0, 0, INFINITY, 10};
    CHECK(pc_ui_snap_begin(&s, session_, &ui, start, invalid, NULL, 0, false, false, false, NULL) ==
          PC_ERR_INVALID);
    CHECK(s.active && !s.moving);
    ui.extras.snap = false;
    double rect[4] = {10, 20, 50, 60};
    CHECK(pc_ui_snap_begin(&s, session_, &ui, start, rect, NULL, 0, true, false, false, NULL) == PC_OK);
    p[0] = 188;
    p[1] = 30;
    CHECK(pc_ui_snap_point(&s, p, 4, false, out, NULL) == PC_OK);
    CHECK_NEAR(out[0], 190, 0); /* moved right edge 218; moved centre 198 -> 200 */
    CHECK(s.nlines > 0 && s.lines[0].kind == PC_SNAP_DOCUMENT_CENTER);
    CHECK(pc_ui_snap_begin(&s, session_, &ui, start, rect, NULL, 0, false, false, false, NULL) == PC_OK);
    CHECK(pc_ui_snap_point(&s, p, 4, false, out, NULL) == PC_OK && out[0] == p[0]);
    CHECK(pc_ui_snap_point(&s, p, NAN, false, out, NULL) == PC_ERR_INVALID);
    pc_ui_snap_fini(&s);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(session_);
}
static void test_snap_grid_units_and_visibility(void)
{
    pc_engine_session *session_ = session();
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    for (size_t i = 0; i < PC_UI_SNAP_COUNT; i++)
        ui.view.snap_to[i] = false;
    ui.extras.snap = ui.extras.grid = ui.view.snap_to[1] = true;
    pc_json_value value = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(
              session_, "prefs.set",
              "{\"values\":{\"guidesGridAndSlices.gridlineEvery\":20,\"guidesGridAndSlices.gridUnit\":\"pixels\","
              "\"guidesGridAndSlices.subdivisions\":2}}",
              &value, NULL) == PC_OK);
    pc_json_value_fini(&value);
    pc_ui_snap s = {0};
    double p[2] = {43, 77}, out[2];
    CHECK(pc_ui_snap_begin(&s, session_, &ui, p, NULL, NULL, 0, false, false, false, NULL) == PC_OK);
    CHECK(pc_ui_snap_point(&s, p, 4, false, out, NULL) == PC_OK);
    CHECK(out[0] == 40 && out[1] == 80);
    ui.extras.grid = false;
    CHECK(pc_ui_snap_begin(&s, session_, &ui, p, NULL, NULL, 0, false, true, false, NULL) == PC_OK);
    CHECK(pc_ui_snap_point(&s, p, 4, false, out, NULL) == PC_OK && out[0] == 43 && out[1] == 77);
    pc_ui_snap_fini(&s);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(session_);
}
int main(int argc, char **argv)
{
    RUN(test_snap_capture_threshold_disable_smart_and_failure);
    RUN(test_snap_grid_units_and_visibility);
    return pc_t_finish();
}
