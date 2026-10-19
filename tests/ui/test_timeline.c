#include "harness.h"
#include "ui/timeline.h"
#include "ui/state.h"
#include <math.h>
#include <string.h>

static pc_status configure(pc_ui_timeline *state, const char *json)
{
    yyjson_doc *d = NULL;
    pc_status status = pc_json_parse(json, strlen(json), &d, NULL);
    if (status == PC_OK)
        status = pc_ui_timeline_configure(state, yyjson_doc_get_root(d), NULL);
    yyjson_doc_free(d);
    return status;
}
static void test_playback_clock_loop_scope_and_boundaries(void)
{
    pc_ui_timeline state = {.open = true, .playing = true};
    pc_timeline timeline = pc_timeline_new(3, 20);
    size_t frame = 99;
    CHECK(!pc_ui_timeline_step(&state, 1, &timeline, 1000, &frame));
    CHECK(!pc_ui_timeline_step(&state, 1, &timeline, 1049, &frame));
    CHECK(pc_ui_timeline_step(&state, 1, &timeline, 1050, &frame) && frame == 1);
    timeline.current = frame;
    CHECK(pc_ui_timeline_step(&state, 1, &timeline, 1100, &frame) && frame == 2);
    timeline.current = frame;
    CHECK(pc_ui_timeline_step(&state, 1, &timeline, 1150, &frame) && frame == 0);
    CHECK(!pc_ui_timeline_step(&state, 1, &timeline, 10, &frame));
    CHECK(!pc_ui_timeline_step(&state, 1, &timeline, NAN, &frame));
    CHECK(!pc_ui_timeline_step(&state, 2, &timeline, 1200, &frame) && !state.playing);
    state.playing = true;
    CHECK(!pc_ui_timeline_step(&state, 1, NULL, 1200, &frame) && !state.playing);
    state.playing = true;
    timeline.duration = 1;
    timeline.current = 0;
    CHECK(!pc_ui_timeline_step(&state, 1, &timeline, 1200, &frame));
    timeline.duration = SIZE_MAX;
    timeline.current = SIZE_MAX - 1;
    CHECK(pc_ui_timeline_step(&state, 1, &timeline, 1250, &frame) && frame == 0);
}
static void test_configuration_atomic_and_transient_fields(void)
{
    pc_ui_timeline state = {0};
    CHECK(configure(&state, "{\"open\":true,\"playing\":true}") == PC_OK);
    CHECK(state.open && state.playing);
    CHECK(configure(&state, "{\"open\":false,\"playing\":true}") == PC_ERR_INVALID);
    CHECK(state.open && state.playing);
    CHECK(configure(&state, "{\"open\":false,\"unknown\":true}") == PC_ERR_INVALID);
    CHECK(state.open && state.playing);
    CHECK(configure(&state, "{\"open\":false}") == PC_OK);
    CHECK(!state.open && !state.playing);
    CHECK(configure(&state, "{\"open\":1}") == PC_ERR_INVALID);
    pc_ui_state ui;
    pc_ui_state_init(&ui);
    ui.timeline = (pc_ui_timeline){.open = true, .playing = true, .document = 42, .last_time = 12345};
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    CHECK(d != NULL);
    yyjson_mut_val *root = NULL;
    CHECK(pc_ui_state_to_json(&ui, d, &root, NULL) == PC_OK);
    CHECK(yyjson_mut_obj_size(yyjson_mut_obj_get(root, "timeline")) == 2);
    pc_buf json = PC_BUF_INIT;
    CHECK(pc_json_write(root, 0, &json, NULL) == PC_OK);
    yyjson_doc *read = NULL;
    CHECK(pc_json_parse(pc_buf_cstr(&json), json.len, &read, NULL) == PC_OK);
    pc_ui_state restored;
    pc_ui_state_init(&restored);
    CHECK(pc_ui_state_from_json(&restored, yyjson_doc_get_root(read), NULL) == PC_OK);
    CHECK(restored.timeline.open && restored.timeline.playing);
    CHECK(restored.timeline.document == 0 && restored.timeline.last_time == 0);
    pc_ui_state_fini(&restored);
    pc_ui_state_fini(&ui);
    yyjson_doc_free(read);
    pc_buf_fini(&json);
    yyjson_mut_doc_free(d);
}
int main(int argc, char **argv)
{
    RUN(test_playback_clock_loop_scope_and_boundaries);
    RUN(test_configuration_atomic_and_transient_fields);
    return pc_t_finish();
}
