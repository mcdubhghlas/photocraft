/* Held temporary tools (pure classification) and the View-menu command classifier. Ports of the
 * hold_keys.rs / view_cmds.rs predicates; the input-driven resolution is reimplemented with the
 * app shell (later U-chunks). */
#include "harness.h"

#include "ui/hold_keys.h"
#include "ui/state.h"
#include "ui/view_cmds.h"

#include <string.h>

static void test_temporary(void)
{
    CHECK_EQ_STR(pc_ui_temporary_id(PC_UI_TEMP_HAND), "tools.temporary.hand");
    CHECK_EQ_STR(pc_ui_temporary_id(PC_UI_TEMP_ZOOM_IN), "tools.temporary.zoomIn");
    CHECK_EQ_STR(pc_ui_temporary_id(PC_UI_TEMP_ZOOM_OUT), "tools.temporary.zoomOut");
    CHECK_EQ_STR(pc_ui_temporary_id(PC_UI_TEMP_MOVE), "tools.temporary.move");
    CHECK(pc_ui_temporary_tool(PC_UI_TEMP_HAND) == PC_TOOL_HAND);
    CHECK(pc_ui_temporary_tool(PC_UI_TEMP_ZOOM_IN) == PC_TOOL_ZOOM);
    CHECK(pc_ui_temporary_tool(PC_UI_TEMP_ZOOM_OUT) == PC_TOOL_ZOOM);
    CHECK(pc_ui_temporary_tool(PC_UI_TEMP_MOVE) == PC_TOOL_MOVE);
    CHECK(PC_UI_TEMPORARY_BOUND[0] == PC_UI_TEMP_HAND);
    CHECK(PC_UI_TEMPORARY_BOUND[1] == PC_UI_TEMP_ZOOM_IN);
    CHECK(PC_UI_TEMPORARY_BOUND[2] == PC_UI_TEMP_ZOOM_OUT);

    CHECK(pc_ui_is_temporary("tools.temporary.hand"));
    CHECK(pc_ui_is_temporary("tools.temporary.erase"));
    CHECK(!pc_ui_is_temporary("file.new"));
    CHECK(!pc_ui_is_temporary(NULL));
}

static void test_cmd_moves(void)
{
    /* ⌘ is the Move tool with the painting/retouching/etc. tools. */
    CHECK(pc_ui_cmd_moves(PC_TOOL_BRUSH));
    CHECK(pc_ui_cmd_moves(PC_TOOL_ERASER));
    CHECK(pc_ui_cmd_moves(PC_TOOL_GRADIENT));
    CHECK(pc_ui_cmd_moves(PC_TOOL_EYEDROPPER));
    CHECK(pc_ui_cmd_moves(PC_TOOL_CLONE_STAMP));
    CHECK(pc_ui_cmd_moves(PC_TOOL_PAINT_BUCKET));
    /* …but not these, which keep ⌘ for themselves. */
    CHECK(!pc_ui_cmd_moves(PC_TOOL_MOVE));
    CHECK(!pc_ui_cmd_moves(PC_TOOL_HAND));
    CHECK(!pc_ui_cmd_moves(PC_TOOL_ROTATE_VIEW));
    CHECK(!pc_ui_cmd_moves(PC_TOOL_ZOOM));
    CHECK(!pc_ui_cmd_moves(PC_TOOL_CROP));
    CHECK(!pc_ui_cmd_moves(PC_TOOL_PERSPECTIVE_CROP));
    CHECK(!pc_ui_cmd_moves(PC_TOOL_SLICE));
    CHECK(!pc_ui_cmd_moves(PC_TOOL_SLICE_SELECT));
    CHECK(!pc_ui_cmd_moves(PC_TOOL_PATH_SELECTION));
    CHECK(!pc_ui_cmd_moves(PC_TOOL_DIRECT_SELECTION));
    CHECK(!pc_ui_cmd_moves(PC_TOOL_PEN));
    CHECK(!pc_ui_cmd_moves(PC_TOOL_FREEFORM_PEN));
    CHECK(!pc_ui_cmd_moves(PC_TOOL_CURVATURE_PEN));
    CHECK(!pc_ui_cmd_moves(PC_TOOL_RECT_MARQUEE));
    CHECK(!pc_ui_cmd_moves(PC_TOOL_ELLIPSE_MARQUEE));
    CHECK(!pc_ui_cmd_moves(PC_TOOL_LASSO));
    CHECK(!pc_ui_cmd_moves(PC_TOOL_POLYGON_LASSO));
    CHECK(!pc_ui_cmd_moves(PC_TOOL_MAGIC_WAND));
    CHECK(!pc_ui_cmd_moves(PC_TOOL_TYPE));
    CHECK(!pc_ui_cmd_moves(PC_TOOL_VERTICAL_TYPE));
    CHECK(!pc_ui_cmd_moves(PC_TOOL_RECTANGLE));
    CHECK(!pc_ui_cmd_moves(PC_TOOL_CUSTOM_SHAPE));
}

static void test_repositions_and_erase(void)
{
    CHECK(pc_ui_repositions(PC_TOOL_RECT_MARQUEE));
    CHECK(pc_ui_repositions(PC_TOOL_ELLIPSE_MARQUEE));
    CHECK(pc_ui_repositions(PC_TOOL_LASSO));
    CHECK(pc_ui_repositions(PC_TOOL_OBJECT_SELECTION));
    CHECK(pc_ui_repositions(PC_TOOL_RECTANGLE));
    CHECK(pc_ui_repositions(PC_TOOL_LINE));
    CHECK(!pc_ui_repositions(PC_TOOL_POLYGON_LASSO));
    CHECK(!pc_ui_repositions(PC_TOOL_BRUSH));

    CHECK(pc_ui_erases_while_held(PC_TOOL_BRUSH));
    CHECK(pc_ui_erases_while_held(PC_TOOL_PENCIL));
    CHECK(!pc_ui_erases_while_held(PC_TOOL_ERASER));
    CHECK(!pc_ui_erases_while_held(PC_TOOL_MIXER_BRUSH));

    CHECK(pc_ui_is_shape_tool(PC_TOOL_TRIANGLE));
    CHECK(pc_ui_is_shape_tool(PC_TOOL_POLYGON));
    CHECK(!pc_ui_is_shape_tool(PC_TOOL_PEN));
}

static void test_view_handles(void)
{
    CHECK(pc_ui_view_cmds_handles("view.show.layerEdges"));
    CHECK(pc_ui_view_cmds_handles("view.show.all"));
    CHECK(!pc_ui_view_cmds_handles("view.show.bogus"));
    CHECK(pc_ui_view_cmds_handles("view.snapTo.grid"));
    CHECK(pc_ui_view_cmds_handles("view.snapTo.all"));
    CHECK(pc_ui_view_cmds_handles("view.snapTo.none"));
    CHECK(pc_ui_view_cmds_handles("view.screenMode.fullScreen"));
    CHECK(pc_ui_view_cmds_handles("view.screenMode.cycle"));
    CHECK(pc_ui_view_cmds_handles("view.pixelAspectRatio.square"));
    CHECK(!pc_ui_view_cmds_handles("view.pixelAspectRatio.custom")); /* handled by workspace_ui */
    CHECK(pc_ui_view_cmds_handles("type.fontPreviewSize.huge"));
    CHECK(pc_ui_view_cmds_handles("type.languageOptions.defaultFeatures"));
    CHECK(pc_ui_view_cmds_handles("type.languageOptions.middleEasternAndSouthAsianComposer"));
    CHECK(pc_ui_view_cmds_handles("window.arrange.tile"));
    CHECK(pc_ui_view_cmds_handles("window.arrange.cascade"));
    CHECK(pc_ui_view_cmds_handles("view.flipHorizontal"));
    CHECK(pc_ui_view_cmds_handles("window.panel.layers"));
    CHECK(!pc_ui_view_cmds_handles("file.new"));
    CHECK(!pc_ui_view_cmds_handles(NULL));

    CHECK(pc_ui_view_show_slot_exists("pixelGrid"));
    CHECK(!pc_ui_view_show_slot_exists("all"));
    CHECK(pc_ui_view_snap_slot_exists("documentBounds"));
    CHECK(!pc_ui_view_snap_slot_exists("none"));
}

static void test_resolver(void)
{
    pc_engine_session *session = NULL;
    CHECK(pc_engine_session_new(&session, NULL) == PC_OK);
    bool keys[PC_KEY_EXCLAMATIONMARK + 1] = {false};
    pc_ui_temporary t;
    keys[PC_KEY_SPACE] = true;
    CHECK(pc_ui_temporary_resolve(session, PC_TOOL_BRUSH, keys, 0, false, true, &t));
    CHECK(t == PC_UI_TEMP_HAND);
    CHECK(pc_ui_temporary_resolve(session, PC_TOOL_BRUSH, keys, PC_UI_MOD_COMMAND, false, true, &t));
    CHECK(t == PC_UI_TEMP_ZOOM_IN);
    CHECK(pc_ui_temporary_resolve(session, PC_TOOL_BRUSH, keys, PC_UI_MOD_COMMAND | PC_UI_MOD_ALT,
                                   false, true, &t));
    CHECK(t == PC_UI_TEMP_ZOOM_OUT);
    CHECK(!pc_ui_temporary_resolve(session, PC_TOOL_BRUSH, keys, 0, true, true, &t));
    CHECK(pc_ui_reposition_held(session, keys, false));
    keys[PC_KEY_SPACE] = false;
    CHECK(pc_ui_temporary_resolve(session, PC_TOOL_BRUSH, keys, PC_UI_MOD_COMMAND, false, true, &t));
    CHECK(t == PC_UI_TEMP_MOVE);
    CHECK(!pc_ui_temporary_resolve(session, PC_TOOL_BRUSH, keys, PC_UI_MOD_COMMAND, false, false, &t));
    CHECK(!pc_ui_temporary_resolve(session, PC_TOOL_RECT_MARQUEE, keys, PC_UI_MOD_COMMAND, false, true, &t));
    keys[PC_KEY_BACKTICK] = true;
    CHECK(pc_ui_erase_held(session, PC_TOOL_BRUSH, keys, 0, false));
    CHECK(!pc_ui_erase_held(session, PC_TOOL_CLONE_STAMP, keys, 0, false));
    CHECK(!pc_ui_erase_held(session, PC_TOOL_BRUSH, keys, 0, true));
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(session, "edit.keyboardShortcuts",
          "{\"set\":{\"tools.temporary.hand\":\"Shift+J\",\"tools.temporary.zoomIn\":\"\"}}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    keys[PC_KEY_J] = true;
    CHECK(!pc_ui_temporary_resolve(session, PC_TOOL_BRUSH, keys, 0, false, true, &t));
    CHECK(pc_ui_temporary_resolve(session, PC_TOOL_BRUSH, keys, PC_UI_MOD_SHIFT, false, true, &t));
    CHECK(t == PC_UI_TEMP_HAND);
    CHECK(pc_ui_reposition_held(session, keys, false));
    CHECK(!pc_ui_reposition_held(session, keys, true));
    pc_engine_session_free(session);
}

int main(int argc, char **argv)
{
    RUN(test_temporary);
    RUN(test_resolver);
    RUN(test_cmd_moves);
    RUN(test_repositions_and_erase);
    RUN(test_view_handles);
    return pc_t_finish();
}
