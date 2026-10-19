#include "ui/filter_preview.h"
#include "ui/filter_dialog.h"
#include "engine/preview.h"
#include "base/hash.h"
#include "base/mem.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
struct pc_ui_filter_task {
    pthread_t thread;
    atomic_bool done;
    pc_engine_session *session;
    pc_engine_job_ctx *cancel;
    char *command;
    pc_buf params;
    uint8_t key[32];
    pc_doc_arc *result;
    pc_engine_tone_histogram histogram;
    bool compute_histogram, has_histogram, show_result;
    uint64_t histogram_scope[8];
    pc_status status;
    pc_error error;
};
static void *work(void *context)
{
    struct pc_ui_filter_task *t = context;
    pc_json_value out = PC_JSON_VALUE_INIT;
    t->status = pc_engine_job_ctx_check(t->cancel, &t->error);
    if (t->status == PC_OK && t->compute_histogram)
        t->has_histogram = pc_engine_tone_histogram_compute(t->session, false, &t->histogram, NULL) == PC_OK;
    if (t->status == PC_OK && t->show_result)
        t->status =
            pc_engine_session_execute_json(t->session, t->command, pc_buf_cstr(&t->params), &out, &t->error);
    const pc_engine_doc_state *state = pc_engine_session_active(t->session);
    if (t->status == PC_OK && state && t->show_result)
        t->result = pc_doc_arc_retain(state->doc);
    pc_json_value_fini(&out);
    atomic_store_explicit(&t->done, true, memory_order_release);
    return NULL;
}
static void task_free(struct pc_ui_filter_task *t)
{
    if (!t)
        return;
    pc_engine_session_free(t->session);
    pc_engine_job_ctx_free(t->cancel);
    free(t->command);
    pc_buf_fini(&t->params);
    pc_doc_arc_release(t->result);
    free(t);
}
void pc_ui_filter_preview_fini(pc_ui_filter_preview *p)
{
    if (p->task) {
        pc_engine_job_ctx_cancel(p->task->cancel);
        (void)pthread_join(p->task->thread, NULL);
        task_free(p->task);
    }
    pc_doc_arc_release(p->result);
    memset(p, 0, sizeof *p);
}
bool pc_ui_filter_preview_busy(const pc_ui_filter_preview *p)
{
    return p->task != NULL;
}
pc_status pc_ui_filter_preview_update(pc_ui_filter_preview *p, const pc_engine_session *source,
                                      const yyjson_mut_val *fields, pc_error *err)
{
    const char *command = fields ? yyjson_mut_get_str(yyjson_mut_obj_get(fields, "__command")) : NULL;
    bool tone =
        command &&
        (!strcmp(command, "image.adjustments.levels") || !strcmp(command, "image.adjustments.curves") ||
         !strcmp(command, "image.adjustments.threshold") || !strcmp(command, "filter.cameraRaw"));
    bool show_result = fields && yyjson_mut_get_bool(yyjson_mut_obj_get(fields, "preview"));
    bool enabled =
        fields && yyjson_mut_get_bool(yyjson_mut_obj_get(fields, "__filter")) && (show_result || tone);
    uint64_t histogram_scope[8] = {0};
    const char *scope_keys[] = {"__document", "__revision", "__layer", "__color",
                                "__mask",     "__channel",  "__alpha", "__quick"};
    if (enabled)
        for (size_t i = 0; i < 8; i++) {
            yyjson_mut_val *v = yyjson_mut_obj_get(fields, scope_keys[i]);
            histogram_scope[i] =
                yyjson_mut_is_bool(v) ? (uint64_t)yyjson_mut_get_bool(v) : yyjson_mut_get_uint(v);
        }
    if (p->has_histogram && enabled && memcmp(histogram_scope, p->histogram_scope, sizeof histogram_scope))
        p->has_histogram = false;
    pc_status status = enabled ? pc_ui_filter_dialog_guard(source, fields, err) : PC_OK;
    if (status != PC_OK)
        enabled = false;
    pc_buf serialized = PC_BUF_INIT;
    uint8_t key[32] = {0};
    if (enabled) {
        status = pc_ui_filter_dialog_params(fields, &serialized, err);
        if (status == PC_OK)
            status = pc_buf_append_byte(&serialized, 0, err);
        if (status == PC_OK)
            status = pc_buf_append_str(&serialized, command ? command : "", err);
        if (status == PC_OK)
            status = pc_buf_append_byte(&serialized, show_result ? 1 : 0, err);
        if (status == PC_OK)
            status = pc_buf_append(&serialized, histogram_scope, sizeof histogram_scope, err);
        if (status != PC_OK) {
            pc_buf_fini(&serialized);
            return status;
        }
        pc_blake3(serialized.data, serialized.len, key);
    }
    pc_buf_fini(&serialized);
    if (!enabled || !p->has_wanted || memcmp(key, p->wanted, 32)) {
        if (p->task)
            pc_engine_job_ctx_cancel(p->task->cancel);
        pc_doc_arc_release(p->result);
        p->result = NULL;
        p->has_ready = false;
        p->has_wanted = enabled;
        memcpy(p->wanted, key, 32);
        p->status = status;
        memset(&p->error, 0, sizeof p->error);
    }
    if (p->task && atomic_load_explicit(&p->task->done, memory_order_acquire)) {
        struct pc_ui_filter_task *t = p->task;
        (void)pthread_join(t->thread, NULL);
        p->task = NULL;
        if (enabled && !memcmp(t->key, p->wanted, 32)) {
            p->status = t->status;
            p->error = t->error;
            p->has_ready = true;
            memcpy(p->ready, t->key, 32);
            pc_doc_arc_release(p->result);
            p->result = t->result;
            t->result = NULL;
            p->generation++;
        }
        if (t->has_histogram && enabled && !memcmp(t->key, p->wanted, 32)) {
            p->histogram = t->histogram;
            memcpy(p->histogram_scope, t->histogram_scope, sizeof p->histogram_scope);
            p->has_histogram = true;
        }
        task_free(t);
    }
    if (!enabled)
        return status;
    if (!p->task && !p->has_ready) {
        struct pc_ui_filter_task *t = calloc(1, sizeof *t);
        if (!t)
            return pc_fail(err, PC_ERR_NOMEM, "filter preview");
        atomic_init(&t->done, false);
        memcpy(t->key, p->wanted, 32);
        t->command = command ? pc_strndup(command, strlen(command)) : NULL;
        t->show_result = show_result;
        memcpy(t->histogram_scope, histogram_scope, sizeof histogram_scope);
        t->compute_histogram = tone && !p->has_histogram;

        t->cancel = pc_engine_job_ctx_new();
        status = t->command && t->cancel ? pc_ui_filter_dialog_params(fields, &t->params, err)
                                         : pc_fail(err, PC_ERR_NOMEM, "filter preview");
        if (status == PC_OK)
            status = pc_engine_preview_clone(source, &t->session, err);
        if (status == PC_OK) {
            pc_engine_session_set_inline_job_ctx(t->session, t->cancel);
            if (pthread_create(&t->thread, NULL, work, t) != 0)
                status = pc_fail(err, PC_ERR_IO, "cannot start filter preview");
        }
        if (status != PC_OK) {
            task_free(t);
            return status;
        }
        p->task = t;
    }
    if (p->has_ready && p->status != PC_OK) {
        if (err)
            *err = p->error;
        return p->status;
    }
    return PC_OK;
}
