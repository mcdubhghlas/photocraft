#include "ui/command_queue.h"
#include "base/mem.h"
#include <stdlib.h>
#include <string.h>
void pc_ui_command_queue_fini(pc_ui_command_queue *queue)
{
    if (!queue) {
        return;
    }
    for (size_t i = 0; i < queue->count; i++) {
        free(queue->items[i].id);
        pc_buf_fini(&queue->items[i].params);
    }
    *queue = (pc_ui_command_queue){0};
}
pc_status pc_ui_command_queue_push(pc_ui_command_queue *queue, const char *id, const yyjson_mut_val *params,
                                   pc_error *err)
{
    if (!queue || !id || !*id) {
        return pc_fail(err, PC_ERR_INVALID, "missing UI command");
    }
    if (queue->count == PC_UI_COMMAND_QUEUE_CAP || strlen(id) > 255) {
        return pc_fail(err, PC_ERR_LIMIT, "UI command queue limit");
    }
    pc_buf text = PC_BUF_INIT;
    pc_status rc = params ? pc_json_write(params, 0, &text, err) : PC_OK;
    if (rc != PC_OK) {
        return rc;
    }
    size_t bytes = strlen(id) + text.len;
    if (bytes > ((size_t)1 << 20) - queue->bytes) {
        pc_buf_fini(&text);
        return pc_fail(err, PC_ERR_LIMIT, "UI command bytes limit");
    }
    char *copy = pc_strndup(id, strlen(id));
    if (!copy) {
        pc_buf_fini(&text);
        return pc_fail(err, PC_ERR_NOMEM, "UI command queue");
    }
    queue->items[queue->count++] = (pc_ui_queued_command){.id = copy, .params = text};
    queue->bytes += bytes;
    return PC_OK;
}
pc_status pc_ui_command_queue_apply(pc_ui_command_queue *queue, pc_ui_command_dispatch dispatch,
                                    void *context, pc_error *err)
{
    if (!queue || !dispatch) {
        return pc_fail(err, PC_ERR_INVALID, "missing UI dispatcher");
    }
    if (!queue->count) {
        return PC_OK;
    }
    pc_ui_command_queue *batch = malloc(sizeof *batch);
    if (!batch) {
        return pc_fail(err, PC_ERR_NOMEM, "UI command batch");
    }
    *batch = *queue;
    *queue = (pc_ui_command_queue){0};
    pc_status rc = PC_OK;
    for (size_t i = 0; rc == PC_OK && i < batch->count; i++) {
        yyjson_doc *params = NULL;
        if(batch->items[i].params.len) { rc = pc_json_parse(pc_buf_cstr(&batch->items[i].params), batch->items[i].params.len, &params, err); }
        pc_json_value out = PC_JSON_VALUE_INIT;
        if (rc == PC_OK) {
            rc = dispatch(context, batch->items[i].id, yyjson_doc_get_root(params), &out, err);
        }
        pc_json_value_fini(&out);
        yyjson_doc_free(params);
    }
    pc_ui_command_queue_fini(batch);
    free(batch);
    return rc;
}
