/* UI frames read a stable document. Widget actions run after the frame has finished reading it. */
#ifndef PC_UI_COMMAND_QUEUE_H
#define PC_UI_COMMAND_QUEUE_H
#include "base/json.h"
#define PC_UI_COMMAND_QUEUE_CAP 128
typedef struct pc_ui_queued_command {
    char *id;
    pc_buf params;
} pc_ui_queued_command;
typedef struct pc_ui_command_queue {
    pc_ui_queued_command items[PC_UI_COMMAND_QUEUE_CAP];
    size_t count, bytes;
} pc_ui_command_queue;
typedef pc_status (*pc_ui_command_dispatch)(void *context, const char *id, yyjson_val *params,
                                            pc_json_value *out, pc_error *err);
void pc_ui_command_queue_fini(pc_ui_command_queue *queue);
pc_status pc_ui_command_queue_push(pc_ui_command_queue *queue, const char *id, const yyjson_mut_val *params,
                                   pc_error *err);
/* Stops at the first error and clears the rest. Actions queued by a dispatch are left for the
 * next frame. No borrowed layer or JSON pointers survive a widget's draw call. */
pc_status pc_ui_command_queue_apply(pc_ui_command_queue *queue, pc_ui_command_dispatch dispatch,
                                    void *context, pc_error *err);
#endif
