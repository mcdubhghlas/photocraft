#include "harness.h"
#include "ui/vector_tool.h"
#include "ui/shortcuts.h"
#include "engine/vector_cmds.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
static void execute(pc_engine_session *s, const char *id, const char *params)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, id, params, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
}
static void test_shape_previews_match_commands_at_every_depth_and_mode(void)
{
    const char *modes[] = {"rgb", "grayscale", "cmyk", "lab"};
    const unsigned depths[] = {8, 16, 32};
    const pc_ui_tool tools[] = {PC_TOOL_RECTANGLE, PC_TOOL_ELLIPSE_SHAPE, PC_TOOL_TRIANGLE, PC_TOOL_POLYGON,
                                PC_TOOL_LINE};
    for (size_t mode = 0; mode < 4; mode++) {
        for (size_t depth = 0; depth < 3; depth++) {
            pc_engine_session *session = NULL;
            CHECK(pc_engine_session_new(&session, NULL) == PC_OK);
            char json[160];
            (void)snprintf(
                json, sizeof json,
                "{\"width\":96,\"height\":96,\"mode\":\"%s\",\"depth\":%u,\"background\":\"transparent\"}",
                modes[mode], depths[depth]);
            execute(session, "file.new", json);
            execute(session, "tools.setColors",
                    "{\"foreground\":[0.8,0.2,0.1,0.35],\"background\":[0.1,0.3,0.9,0.6]}");
            pc_ui_state ui;
            pc_ui_state_init(&ui);
            ui.tool_options.stroke_width = 4;
            ui.tool_options.corner_radius = 5;
            ui.tool_options.shape_stroke.dashes = calloc(2, sizeof(float));
            CHECK(ui.tool_options.shape_stroke.dashes != NULL);
            ui.tool_options.shape_stroke.dashes[0] = 2;
            ui.tool_options.shape_stroke.dashes[1] = 1;
            ui.tool_options.shape_stroke.ndashes = 2;
            pc_ui_tool_gesture gesture = {0};
            for (size_t tool = 0; tool < 5; tool++) {
                for (unsigned combo = 0; combo < 4; combo++) {
                    ui.tool = tools[tool];
                    ui.tool_options.shape_stroke.align = (pc_stroke_align)(combo % 3);
                    unsigned mods = (combo & 1 ? PC_UI_MOD_SHIFT : 0) | (combo & 2 ? PC_UI_MOD_ALT : 0);
                    pc_json_value out = PC_JSON_VALUE_INIT;
                    pc_ui_tool_point point = {.x = 40.5f, .y = 35.5f, .pressure = 1};
                    const pc_engine_doc_state *state = pc_engine_session_active(session);
                    uint64_t revision = state->revision;
                    CHECK(pc_ui_tool_gesture_event(&gesture, session, &ui, PC_UI_POINTER_DOWN, point, mods, 1,
                                                   &out, NULL) == PC_OK);
                    point.x = 67.5f;
                    point.y = 55.5f;
                    CHECK(pc_ui_tool_gesture_event(&gesture, session, &ui, PC_UI_POINTER_MOVE, point, mods, 1,
                                                   &out, NULL) == PC_OK);
                    CHECK(gesture.has_shape_preview &&
                          pc_engine_session_active(session)->revision == revision);
                    pc_shape_layer preview = {0};
                    CHECK(pc_shape_layer_clone(&gesture.shape_preview, &preview, NULL) == PC_OK);
                    CHECK(pc_ui_tool_gesture_event(&gesture, session, &ui, PC_UI_POINTER_UP, point, mods, 1,
                                                   &out, NULL) == PC_OK);
                    state = pc_engine_session_active(session);
                    const pc_document *doc = pc_doc_arc_get(state->doc);
                    const pc_layer *layer = pc_document_layer(doc, state->active_layer);
                    CHECK(layer && layer->content.kind == PC_LAYER_SHAPE && doc->nlayers == 2);
                    const pc_shape_layer *shape = &layer->content.u.shape;
                    CHECK(pc_path_eq(&preview.path, &shape->path));
                    CHECK(pc_fill_eq(&preview.fill, &shape->fill));
                    CHECK(pc_shape_stroke_eq(&preview.stroke, &shape->stroke));
                    CHECK(shape->has_live && pc_live_shape_eq(&shape->live, &preview.live));
                    CHECK(!gesture.has_shape_preview && !gesture.active);
                    CHECK(pc_engine_session_undo(session));
                    CHECK(pc_doc_arc_get(pc_engine_session_active(session)->doc)->nlayers == 1);
                    pc_shape_layer_fini(&preview);
                    pc_json_value_fini(&out);
                }
            }
            pc_ui_tool_gesture_fini(&gesture);
            pc_ui_state_fini(&ui);
            pc_engine_session_free(session);
        }
    }
}
static void test_shape_small_drag_cancel_and_validation(void)
{
    pc_engine_session *session = NULL;
    CHECK(pc_engine_session_new(&session, NULL) == PC_OK);
    execute(session, "file.new", "{\"width\":32,\"height\":32}");
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_RECTANGLE;
    pc_ui_tool_gesture gesture = {0};
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_ui_tool_point point = {.x = 10, .y = 10, .pressure = 1};
    CHECK(pc_ui_tool_gesture_event(&gesture, session, &ui, PC_UI_POINTER_DOWN, point, 0, 1, &out, NULL) ==
          PC_OK);
    CHECK(pc_ui_tool_gesture_event(&gesture, session, &ui, PC_UI_POINTER_UP, point, 0, 1, &out, NULL) ==
          PC_OK);
    CHECK(pc_doc_arc_get(pc_engine_session_active(session)->doc)->nlayers == 1 && !gesture.has_shape_preview);
    CHECK(pc_ui_tool_gesture_event(&gesture, session, &ui, PC_UI_POINTER_DOWN, point, 0, 1, &out, NULL) ==
          PC_OK);
    point.x = 20;
    point.y = 25;
    CHECK(pc_ui_tool_gesture_event(&gesture, session, &ui, PC_UI_POINTER_MOVE, point, 0, 1, &out, NULL) ==
              PC_OK &&
          gesture.has_shape_preview);
    CHECK(pc_ui_tool_gesture_event(&gesture, session, &ui, PC_UI_POINTER_CANCEL, point, 0, 1, &out, NULL) ==
          PC_OK);
    CHECK(!gesture.has_shape_preview && pc_doc_arc_get(pc_engine_session_active(session)->doc)->nlayers == 1);
    float start[2] = {0, 0}, end[2] = {NAN, 10}, color[4] = {0, 0, 0, 1};
    CHECK(pc_ui_shape_drag_params(ui.tool, &ui.tool_options, color, color, start, end, 0, &out, NULL) ==
          PC_ERR_INVALID);
    end[0] = 10;
    ui.tool_options.shape_stroke.align = (pc_stroke_align)255;
    ui.tool_options.stroke_width = 2;
    CHECK(pc_ui_shape_drag_params(ui.tool, &ui.tool_options, color, color, start, end, 0, &out, NULL) ==
          PC_ERR_INVALID);
    pc_ui_tool_gesture_fini(&gesture);
    pc_json_value_fini(&out);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(session);
}
int main(int argc, char **argv)
{
    RUN(test_shape_previews_match_commands_at_every_depth_and_mode);
    RUN(test_shape_small_drag_cancel_and_validation);
    return pc_t_finish();
}
