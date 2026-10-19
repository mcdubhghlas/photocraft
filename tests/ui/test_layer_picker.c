#include "harness.h"
#include "ui/layer_picker.h"
#include "compose/compose.h"
#include <math.h>
#include <string.h>
static pc_status command(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status status = pc_engine_session_execute_json(s, id, json, &out, NULL);
    pc_json_value_fini(&out);
    return status;
}
static pc_status event(pc_engine_session *s, pc_ui_state *ui, pc_ui_pointer_kind kind, float x, float y)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status status =
        pc_ui_layer_picker_pointer(s, ui, kind, (pc_ui_tool_point){.x = x, .y = y}, &out, NULL);
    pc_json_value_fini(&out);
    return status;
}
static void test_layer_picker_samples_only_on_down_one_undo_and_remains_armed(void)
{
    const char *kinds[] = {"levels", "curves"};
    const unsigned depths[] = {8, 16, 32};
    for (size_t k = 0; k < 2; k++)
        for (size_t d = 0; d < 3; d++) {
            pc_engine_session *s = NULL;
            CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
            char json[180], id[120];
            (void)snprintf(json, sizeof json,
                           "{\"width\":16,\"height\":16,\"depth\":%u,\"background\":\"transparent\"}",
                           depths[d]);
            CHECK(command(s, "file.new", json) == PC_OK);
            CHECK(command(s, "edit.fill", "{\"color\":[0.6,0.5,0.4]}") == PC_OK);
            (void)snprintf(id, sizeof id, "layer.newAdjustmentLayer.%s", kinds[k]);
            CHECK(command(s, id, "{}") == PC_OK);
            const pc_engine_doc_state *state = pc_engine_session_active(s);
            size_t history = pc_history_past_len(state->history);
            pc_ui_state ui;
            pc_ui_state_init(&ui);
            ui.tool = PC_TOOL_BRUSH;
            CHECK(pc_ui_layer_picker_arm(s, &ui, PC_TONE_PICK_WHITE, NULL) == PC_OK);
            CHECK(pc_ui_layer_picker_armed(s, &ui));
            CHECK(event(s, &ui, PC_UI_POINTER_DOWN, 4, 4) == PC_OK);
            CHECK(ui.layer_picker.down && pc_history_past_len(state->history) == history + 1);
            CHECK(event(s, &ui, PC_UI_POINTER_MOVE, 7, 7) == PC_OK);
            CHECK(event(s, &ui, PC_UI_POINTER_UP, 7, 7) == PC_OK);
            CHECK(pc_history_past_len(state->history) == history + 1 && !ui.layer_picker.down &&
                  pc_ui_layer_picker_armed(s, &ui));
            pc_compose_buffer buffer = {0};
            CHECK(pc_compose_render(pc_doc_arc_get(state->doc), pc_rect_make(4, 4, 5, 5), &buffer, NULL) ==
                  PC_OK);
            CHECK(buffer.len && buffer.px[0][0] > .99f && buffer.px[0][1] > .99f && buffer.px[0][2] > .99f);
            pc_compose_buffer_fini(&buffer);
            CHECK(event(s, &ui, PC_UI_POINTER_DOWN, -1, 0) == PC_ERR_INVALID);
            CHECK(pc_ui_layer_picker_armed(s, &ui) && pc_history_past_len(state->history) == history + 1);
            CHECK(pc_ui_layer_picker_cancel(&ui) && !pc_ui_layer_picker_armed(s, &ui));
            pc_ui_state_fini(&ui);
            pc_engine_session_free(s);
        }
}
static void test_picker_saved_state_pruning_and_gray_mode(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(command(s, "file.new", "{\"width\":8,\"height\":8}") == PC_OK);
    CHECK(command(s, "layer.newAdjustmentLayer.levels", "{}") == PC_OK);
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_MOVE;
    CHECK(pc_ui_layer_picker_arm(s, &ui, PC_TONE_PICK_GRAY, NULL) == PC_OK);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *state = NULL;
    pc_buf text = PC_BUF_INIT;
    yyjson_doc *read = NULL;
    CHECK(pc_ui_state_to_json(&ui, d, &state, NULL) == PC_OK);
    CHECK(pc_json_write(state, 0, &text, NULL) == PC_OK);
    CHECK(pc_json_parse(pc_buf_cstr(&text), text.len, &read, NULL) == PC_OK);
    pc_ui_state restored;
    pc_ui_state_init(&restored);
    CHECK(pc_ui_state_from_json(&restored, yyjson_doc_get_root(read), NULL) == PC_OK);
    CHECK(restored.layer_picker.armed && restored.layer_picker.point == PC_TONE_PICK_GRAY &&
          restored.layer_picker.tool == PC_TOOL_MOVE);
    CHECK(pc_ui_layer_picker_armed(s, &restored));
    yyjson_doc_free(read);
    yyjson_mut_doc_free(d);
    pc_buf_fini(&text);
    pc_ui_state_fini(&restored);
    ui.tool = PC_TOOL_BRUSH;
    pc_ui_layer_picker_prune(s, &ui);
    CHECK(!ui.layer_picker.armed);
    ui.tool = PC_TOOL_MOVE;
    CHECK(!pc_ui_layer_picker_armed(s, &ui));
    CHECK(pc_ui_layer_picker_arm(s, &ui, PC_TONE_PICK_BLACK, NULL) == PC_OK);
    CHECK(command(s, "layer.delete", "{}") == PC_OK);
    pc_ui_layer_picker_prune(s, &ui);
    CHECK(!ui.layer_picker.armed);
    CHECK(command(s, "file.new", "{\"width\":8,\"height\":8,\"mode\":\"gray\"}") == PC_OK);
    CHECK(command(s, "layer.newAdjustmentLayer.curves", "{}") == PC_OK);
    CHECK(pc_ui_layer_picker_arm(s, &ui, PC_TONE_PICK_BLACK, NULL) == PC_OK);
    CHECK(pc_ui_layer_picker_arm(s, &ui, PC_TONE_PICK_GRAY, NULL) == PC_ERR_INVALID);
    CHECK(pc_ui_layer_picker_armed(s, &ui) && ui.layer_picker.point == PC_TONE_PICK_BLACK);
    CHECK(command(s, "file.new", "{\"width\":8,\"height\":8}") == PC_OK);
    pc_ui_layer_picker_prune(s, &ui);
    CHECK(!ui.layer_picker.armed);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
int main(int argc, char **argv)
{
    RUN(test_layer_picker_samples_only_on_down_one_undo_and_remains_armed);
    RUN(test_picker_saved_state_pruning_and_gray_mode);
    return pc_t_finish();
}
