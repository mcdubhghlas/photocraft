#include "harness.h"
#include "ui/shell/control.h"
#include <string.h>
#include <time.h>
static pc_status call(pc_ui_shell_app *app, const char *method, const char *json, pc_json_value *out)
{
    yyjson_doc *request = NULL;
    pc_status status = json ? pc_json_parse(json, strlen(json), &request, NULL) : PC_OK;
    if (status == PC_OK)
        status = pc_ui_control_handle(app, method, request ? yyjson_doc_get_root(request) : NULL, out, NULL);
    yyjson_doc_free(request);
    return status;
}
static void invoke(pc_ui_shell_app *app, const char *method, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(call(app, method, json, &out) == PC_OK);
    pc_json_value_fini(&out);
}
static void wait_jobs(pc_ui_shell_app *app)
{
    for (size_t i = 0; i < 3000 && pc_engine_session_has_jobs(pc_ui_shell_app_session(app)); i++) {
        pc_json_value out = PC_JSON_VALUE_INIT;
        CHECK(call(app, "ui.inspect", NULL, &out) == PC_OK);
        pc_json_value_fini(&out);
        struct timespec delay = {0, 1000000};
        (void)nanosleep(&delay, NULL);
    }
    CHECK(!pc_engine_session_has_jobs(pc_ui_shell_app_session(app)));
}
static void test_headless_background_controls_atomic_enable_poll_and_error(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(call(app, "ui.set", "{\"backgroundJobs\":true,\"theme\":\"invalid\"}", &out) != PC_OK);
    CHECK(call(app, "ui.inspect", NULL, &out) == PC_OK);
    CHECK(yyjson_mut_is_false(
        yyjson_mut_obj_get(yyjson_mut_obj_get(pc_json_value_root(&out), "jobs"), "background")));
    pc_json_value_fini(&out);
    invoke(app, "ui.set", "{\"backgroundJobs\":true}");
    invoke(app, "engine.execute",
           "{\"id\":\"file.new\",\"params\":{\"width\":80,\"height\":60,\"background\":\"white\"}}");
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    uint64_t revision = state->revision;
    size_t history = pc_history_past_len(state->history);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"filter.blur.gaussianBlur\",\"params\":{\"radius\":2}}",
               &out) == PC_OK);
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "job")) > 0);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision);
    wait_jobs(app);
    state = pc_engine_session_active(pc_ui_shell_app_session(app));
    CHECK(state->revision > revision && pc_history_past_len(state->history) == history + 1);
    revision = state->revision;
    CHECK(call(app, "ui.menu.invoke",
               "{\"id\":\"image.adjustments.exposure\",\"params\":{\"gamma\":\"invalid\"}}", &out) == PC_OK);
    pc_json_value_fini(&out);
    wait_jobs(app);
    CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision);
    CHECK(call(app, "ui.inspect", NULL, &out) == PC_OK);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&out), "statusError")));
    const char *error = yyjson_mut_get_str(yyjson_mut_obj_get(pc_json_value_root(&out), "status"));
    CHECK(error && strstr(error, "gamma"));
    pc_json_value_fini(&out);
    CHECK(call(app, "ui.set", "{\"backgroundJobs\":\"invalid\"}", &out) != PC_OK);
    invoke(app, "ui.set", "{\"backgroundJobs\":false}");
    CHECK(call(app, "ui.inspect", NULL, &out) == PC_OK);
    CHECK(yyjson_mut_is_false(
        yyjson_mut_obj_get(yyjson_mut_obj_get(pc_json_value_root(&out), "jobs"), "background")));
    pc_json_value_fini(&out);
    pc_ui_shell_app_free(app);
}
int main(int argc, char **argv)
{
    RUN(test_headless_background_controls_atomic_enable_poll_and_error);
    return pc_t_finish();
}
