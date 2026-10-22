/* Move-tool modifier logic: arrow-key nudge dispatch, the ⌘-arrow temporary-Move tool guard
 * (#2474), the Shift axis-lock direction constraint and selection-move classification. */
#include "harness.h"
#include "ui/move_mods.h"
#include "ui/state.h"
#include "engine/engine.h"
#include <math.h>
#include <string.h>

static pc_engine_session *session(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value result = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(
              s, "file.new", "{\"width\":48,\"height\":40,\"background\":\"transparent\",\"depth\":8}",
              &result, NULL) == PC_OK);
    pc_json_value_fini(&result);
    return s;
}
static pc_status nudge(pc_engine_session *s, pc_ui_state *ui, pc_ui_key key, unsigned mods)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err;
    pc_status rc = pc_ui_move_key(s, ui, key, mods, &out, &err);
    pc_json_value_fini(&out);
    return rc;
}
/* ⌘ nudges the layer from a cmd_moves tool (Brush) but leaves it alone from Pen/Hand/Zoom/Crop. */
static void test_cmd_arrow_tool_guard(void)
{
    pc_engine_session *s = session();
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    uint64_t rev = pc_engine_session_active(s)->revision;
    /* Brush: ⌘ is the temporary Move tool, so ⌘-arrow nudges the layer. */
    ui.tool = PC_TOOL_BRUSH;
    CHECK(nudge(s, &ui, PC_KEY_ARROWRIGHT, PC_UI_MOD_COMMAND) == PC_OK);
    CHECK(pc_engine_session_active(s)->revision == rev + 1);
    rev = pc_engine_session_active(s)->revision;
    /* Pen, Hand, Zoom, Crop: ⌘ is not Move; ⌘-arrow must do nothing here. */
    const pc_ui_tool left_alone[] = {PC_TOOL_PEN, PC_TOOL_HAND, PC_TOOL_ZOOM, PC_TOOL_CROP,
                                     PC_TOOL_CURVATURE_PEN, PC_TOOL_TYPE};
    for (size_t i = 0; i < sizeof left_alone / sizeof *left_alone; i++) {
        ui.tool = left_alone[i];
        CHECK(nudge(s, &ui, PC_KEY_ARROWDOWN, PC_UI_MOD_COMMAND) == PC_ERR_NOT_FOUND);
        CHECK(pc_engine_session_active(s)->revision == rev);
    }
    /* The Move tool nudges with a plain arrow (no modifier needed). */
    ui.tool = PC_TOOL_MOVE;
    CHECK(nudge(s, &ui, PC_KEY_ARROWUP, 0) == PC_OK);
    CHECK(pc_engine_session_active(s)->revision == rev + 1);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
/* An open dialog, a text edit or an active transform suppresses the nudge entirely. */
static void test_nudge_suppressed_by_modal_state(void)
{
    pc_engine_session *s = session();
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_MOVE;
    ui.transform.active = true;
    CHECK(nudge(s, &ui, PC_KEY_ARROWUP, 0) == PC_ERR_NOT_FOUND);
    ui.transform.active = false;
    ui.has_text_edit = true;
    CHECK(nudge(s, &ui, PC_KEY_ARROWUP, 0) == PC_ERR_NOT_FOUND);
    ui.has_text_edit = false;
    ui.n_dialogs = 1;
    CHECK(nudge(s, &ui, PC_KEY_ARROWUP, 0) == PC_ERR_NOT_FOUND);
    ui.n_dialogs = 0; /* the count was faked for the guard check; drop it before teardown */
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
static void test_constrain_directions_and_hysteresis(void)
{
    double out[2];
    /* Four-direction constraint snaps a shallow diagonal to the nearest axis. */
    pc_ui_move_constrain((double[]){10, 3}, NULL, 4, out);
    CHECK_NEAR(out[0], 10, 1e-9);
    CHECK_NEAR(out[1], 0, 1e-9);
    /* Eight directions reach the 45 degree diagonal. */
    pc_ui_move_constrain((double[]){10, 9}, NULL, 8, out);
    CHECK_NEAR(out[0], out[1], 1e-9);
    CHECK(out[0] > 0);
    /* Hysteresis keeps the previous axis for a small wobble past the boundary. */
    const double previous[2] = {10, 0};
    pc_ui_move_constrain((double[]){10, 3}, previous, 4, out);
    CHECK_NEAR(out[1], 0, 1e-9);
    /* Zero directions pass the delta through untouched. */
    pc_ui_move_constrain((double[]){4, -7}, NULL, 0, out);
    CHECK_NEAR(out[0], 4, 1e-9);
    CHECK_NEAR(out[1], -7, 1e-9);
}
int main(int argc, char **argv)
{
    RUN(test_cmd_arrow_tool_guard);
    RUN(test_nudge_suppressed_by_modal_state);
    RUN(test_constrain_directions_and_hysteresis);
    return pc_t_finish();
}
