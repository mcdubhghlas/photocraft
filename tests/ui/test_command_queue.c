#include "harness.h"
#include "ui/command_queue.h"
typedef struct dispatch_state {
    pc_ui_command_queue *queue;
    unsigned calls;
} dispatch_state;
static pc_status dispatch(void *context, const char *id, yyjson_val *params, pc_json_value *out,
                          pc_error *err)
{
    (void)out;
    dispatch_state *state = context;
    state->calls++;
    if (strcmp(id, "fail") == 0) {
        CHECK(params == NULL);
        return pc_fail(err, PC_ERR_INVALID, "test error");
    }
    CHECK(strcmp(id, "copied") == 0);
    CHECK(yyjson_get_int(pc_json_get(params, "value")) == 42);
    CHECK(pc_ui_command_queue_push(state->queue, "next-frame", NULL, NULL) == PC_OK);
    return PC_OK;
}
static void test_copy_order_failure_and_reentry(void)
{
    pc_ui_command_queue queue = {0};
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    CHECK(doc != NULL);
    yyjson_mut_val *params = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, params, "value", 42);
    CHECK(pc_ui_command_queue_push(&queue, "copied", params, NULL) == PC_OK);
    yyjson_mut_doc_free(doc);
    CHECK(pc_ui_command_queue_push(&queue, "fail", NULL, NULL) == PC_OK);
    CHECK(pc_ui_command_queue_push(&queue, "must-not-run", NULL, NULL) == PC_OK);
    dispatch_state state = {.queue = &queue};
    pc_error err;
    CHECK(pc_ui_command_queue_apply(&queue, dispatch, &state, &err) == PC_ERR_INVALID);
    CHECK(state.calls == 2 && queue.count == 1);
    CHECK(strcmp(queue.items[0].id, "next-frame") == 0);
    CHECK_CONTAINS(err.msg, "test error");
    pc_ui_command_queue_fini(&queue);
    for (size_t i = 0; i < PC_UI_COMMAND_QUEUE_CAP; i++) {
        CHECK(pc_ui_command_queue_push(&queue, "test", NULL, NULL) == PC_OK);
    }
    CHECK(pc_ui_command_queue_push(&queue, "test", NULL, NULL) == PC_ERR_LIMIT);
    CHECK(queue.count == PC_UI_COMMAND_QUEUE_CAP);
    pc_ui_command_queue_fini(&queue);
}
int main(int argc, char **argv)
{
    RUN(test_copy_order_failure_and_reentry);
    return pc_t_finish();
}
