#include "harness.h"
#include "ui/sampling_tool.h"
#include "ui/shortcuts.h"
#include <math.h>
#include <string.h>
static pc_status command(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status status = pc_engine_session_execute_json(s, id, json, &out, NULL);
    pc_json_value_fini(&out);
    return status;
}
static pc_status corner(pc_document *doc, bool *has_active, pc_layer_id *active, void *context, pc_error *err)
{
    (void)has_active;
    (void)context;
    const float black[4] = {0, 0, 0, 1};
    return pc_surface_fill_rect(pc_layer_surface_mut(pc_document_layer_mut(doc, *active)),
                                pc_rect_make(0, 0, 3, 3), black, 4, err);
}
static pc_engine_session *session(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(command(s, "file.new", "{\"width\":40,\"height\":40}") == PC_OK);
    CHECK(pc_engine_session_edit(s, "black corner", corner, NULL, NULL) == PC_OK);
    return s;
}
static pc_status event(pc_ui_sampling_tool *r, pc_engine_session *s, const pc_ui_state *ui,
                       pc_ui_pointer_kind kind, float x, float y, unsigned mods)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status status =
        pc_ui_sampling_pointer(r, s, ui, kind, (pc_ui_tool_point){.x = x, .y = y}, mods, 1, &out, &err);
    if (status != PC_OK && status != PC_ERR_NOT_FOUND)
        fprintf(stderr, "sampling: %s\n", err.msg);
    pc_json_value_fini(&out);
    return status;
}
static void click(pc_ui_sampling_tool *r, pc_engine_session *s, const pc_ui_state *ui, float x, float y,
                  unsigned mods)
{
    CHECK(event(r, s, ui, PC_UI_POINTER_DOWN, x, y, mods) == PC_OK);
    CHECK(event(r, s, ui, PC_UI_POINTER_UP, x, y, mods) == PC_OK);
}
static void test_eyedropper_average_background_and_latched_alt(void)
{
    pc_engine_session *s = session();
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_EYEDROPPER;
    pc_ui_sampling_tool r = {0};
    const pc_engine_doc_state *state = pc_engine_session_active(s);
    uint64_t revision = state->revision;
    size_t history = pc_history_past_len(state->history);
    click(&r, s, &ui, 3.5f, 3.5f, 0);
    CHECK(pc_engine_session_tools(s)->foreground[0] == 1);
    ui.tool_options.eyedropper_size = 3;
    click(&r, s, &ui, 3.5f, 3.5f, 0);
    CHECK(pc_engine_session_tools(s)->foreground[0] == 227.f / 255);
    click(&r, s, &ui, 3.5f, 3.5f, PC_UI_MOD_ALT);
    CHECK(pc_engine_session_tools(s)->background[0] == 227.f / 255);
    ui.tool_options.eyedropper_size = 5;
    click(&r, s, &ui, .5f, .5f, 0);
    CHECK(pc_engine_session_tools(s)->foreground[0] == 0);
    ui.tool = PC_TOOL_BRUSH;
    ui.tool_options.eyedropper_size = 101;
    CHECK(event(&r, s, &ui, PC_UI_POINTER_DOWN, 20, 20, PC_UI_MOD_ALT) == PC_OK);
    CHECK(r.active && r.mode == 3 && r.has_sample);
    CHECK(event(&r, s, &ui, PC_UI_POINTER_MOVE, 21, 20, 0) == PC_OK);
    CHECK(event(&r, s, &ui, PC_UI_POINTER_UP, 21, 20, 0) == PC_OK);
    CHECK(pc_engine_session_tools(s)->foreground[0] == 254.f / 255 && !r.active);
    CHECK(state->revision == revision && pc_history_past_len(state->history) == history);
    CHECK(event(&r, s, &ui, PC_UI_POINTER_DOWN, 10, 10, PC_UI_MOD_ALT | PC_UI_MOD_CTRL) == PC_ERR_NOT_FOUND);
    CHECK(event(&r, s, &ui, PC_UI_POINTER_DOWN, 10, 10, 0) == PC_ERR_NOT_FOUND);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
static void test_color_sampler_add_preview_move_cancel_delete_and_outside(void)
{
    pc_engine_session *s = session();
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_COLOR_SAMPLER;
    pc_ui_sampling_tool r = {0};
    const pc_engine_doc_state *state = pc_engine_session_active(s);
    size_t history = pc_history_past_len(state->history);
    click(&r, s, &ui, 10, 10, 0);
    CHECK(state->ncolor_samplers == 1 && state->color_samplers[0][0] == 10);
    CHECK(event(&r, s, &ui, PC_UI_POINTER_DOWN, 10, 10, 0) == PC_OK);
    CHECK(event(&r, s, &ui, PC_UI_POINTER_MOVE, 20, 15, 0) == PC_OK);
    CHECK(state->color_samplers[0][0] == 10 && r.index == 0 && r.point[0] == 20);
    CHECK(event(&r, s, &ui, PC_UI_POINTER_CANCEL, 20, 15, 0) == PC_OK);
    CHECK(state->color_samplers[0][0] == 10 && !r.active);
    CHECK(event(&r, s, &ui, PC_UI_POINTER_DOWN, 10, 10, 0) == PC_OK);
    CHECK(event(&r, s, &ui, PC_UI_POINTER_UP, 20, 15, 0) == PC_OK);
    CHECK(state->color_samplers[0][0] == 20 && state->color_samplers[0][1] == 15);
    click(&r, s, &ui, 20, 15, PC_UI_MOD_ALT);
    CHECK(state->ncolor_samplers == 0);
    click(&r, s, &ui, 15, 15, PC_UI_MOD_ALT);
    CHECK(state->ncolor_samplers == 0);
    click(&r, s, &ui, -1, 15, 0);
    CHECK(state->ncolor_samplers == 0);
    click(&r, s, &ui, 10, 10, 0);
    CHECK(event(&r, s, &ui, PC_UI_POINTER_DOWN, 10, 10, 0) == PC_OK);
    CHECK(event(&r, s, &ui, PC_UI_POINTER_UP, -10, 10, 0) == PC_OK);
    CHECK(state->ncolor_samplers == 0 && pc_history_past_len(state->history) == history);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
static void test_sampling_scope_tool_and_hostile_input(void)
{
    pc_engine_session *s = session();
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.tool = PC_TOOL_EYEDROPPER;
    pc_ui_sampling_tool r = {0};
    CHECK(event(&r, s, &ui, PC_UI_POINTER_DOWN, NAN, 0, 0) == PC_ERR_INVALID && !r.active);
    CHECK(event(&r, s, &ui, PC_UI_POINTER_MOVE, 20, 20, 0) == PC_OK && !r.active);
    CHECK(event(&r, s, &ui, PC_UI_POINTER_DOWN, 1, 1, 0) == PC_OK);
    ui.tool = PC_TOOL_PENCIL;
    pc_ui_sampling_sync(&r, s, &ui);
    CHECK(!r.active);
    ui.tool = PC_TOOL_EYEDROPPER;
    CHECK(event(&r, s, &ui, PC_UI_POINTER_DOWN, 1, 1, 0) == PC_OK);
    CHECK(command(s, "file.new", "{\"width\":10,\"height\":10}") == PC_OK);
    CHECK(event(&r, s, &ui, PC_UI_POINTER_MOVE, 2, 2, 0) == PC_ERR_INVALID && !r.active);
    ui.tool_options.eyedropper_size = 2;
    CHECK(event(&r, s, &ui, PC_UI_POINTER_DOWN, 2, 2, 0) == PC_ERR_INVALID && !r.active);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
int main(int argc, char **argv)
{
    RUN(test_eyedropper_average_background_and_latched_alt);
    RUN(test_color_sampler_add_preview_move_cancel_delete_and_outside);
    RUN(test_sampling_scope_tool_and_hostile_input);
    return pc_t_finish();
}
