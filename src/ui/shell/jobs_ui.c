#include "jobs_ui.h"
#include "shell_internal.h"
#include "engine/preview.h"
#include "ui/i18n.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
void pc_ui_shell_jobs_fini(pc_ui_jobs *jobs)
{
    if (jobs) {
        pc_json_value_fini(&jobs->info);
        *jobs = (pc_ui_jobs){0};
    }
}
static yyjson_mut_val *running(pc_ui_jobs *jobs)
{
    yyjson_mut_val *rows = yyjson_mut_obj_get(pc_json_value_root(&jobs->info), "jobs"), *first = NULL;
    for (size_t i = 0; i < yyjson_mut_arr_size(rows); i++) {
        yyjson_mut_val *row = yyjson_mut_arr_get(rows, i);
        const char *state = yyjson_mut_get_str(yyjson_mut_obj_get(row, "state"));
        if (!state || strcmp(state, "running"))
            continue;
        if (yyjson_mut_get_uint(yyjson_mut_obj_get(row, "id")) == jobs->focus)
            return row;
        if (!first)
            first = row;
    }
    return first;
}
static void cancel_button(pc_ui_shell_app *app, yyjson_mut_val *row, const char *mapping)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_ui_shell_widget_mapping(app, mapping, nk_widget_bounds(ctx));
    if (nk_button_label(ctx, pc_i18n_t("Cancel"))) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
        if (p && yyjson_mut_obj_add_uint(d, p, "job", yyjson_mut_get_uint(yyjson_mut_obj_get(row, "id"))))
            pc_ui_shell_queue(app, "jobs.cancel", p);
        yyjson_mut_doc_free(d);
    }
}
void pc_ui_shell_jobs_tick(pc_ui_shell_app *app)
{
    pc_ui_jobs *jobs = pc_ui_shell_jobs_runtime(app);
    pc_engine_session *session = pc_ui_shell_app_session(app);
    bool was_running = pc_engine_session_has_jobs(session);
    pc_engine_job_event *events = NULL;
    size_t count = 0;
    pc_error error = {0};
    if (pc_engine_session_poll_jobs(session, &events, &count, &error) != PC_OK)
        pc_ui_shell_report(app, "Background jobs", error.msg, true);
    for (size_t i = 0; i < count; i++) {
        if (events[i].outcome == PC_ENGINE_JOB_FAILED)
            pc_ui_shell_report(app, events[i].label, events[i].error ? events[i].error : "The job failed",
                               true);
        if (events[i].outcome == PC_ENGINE_JOB_DONE) {
            (void)pc_ui_shell_app_sync_views(app, NULL);
            pc_ui_shell_app_changed(app);
        }
    }
    pc_engine_job_events_free(events, count);
    if (was_running || count || pc_engine_session_has_jobs(session)) {
        pc_json_value fresh = PC_JSON_VALUE_INIT;
        if (pc_engine_session_execute(session, "jobs.list", NULL, &fresh, NULL) == PC_OK) {
            pc_json_value_fini(&jobs->info);
            jobs->info = fresh;
        }
    }
    jobs->locked = jobs->modal = false;
    const pc_engine_doc_state *active = pc_engine_session_active(session);
    pc_doc_id document = active ? pc_doc_arc_get(active->doc)->id : 0;
    yyjson_mut_val *rows = yyjson_mut_obj_get(pc_json_value_root(&jobs->info), "jobs");
    for (size_t i = 0; i < yyjson_mut_arr_size(rows); i++) {
        yyjson_mut_val *row = yyjson_mut_arr_get(rows, i);
        const char *state = yyjson_mut_get_str(yyjson_mut_obj_get(row, "state"));
        if (state && !strcmp(state, "running") && document &&
            yyjson_mut_get_uint(yyjson_mut_obj_get(row, "document")) == document) {
            jobs->locked = true;
            jobs->focus = yyjson_mut_get_uint(yyjson_mut_obj_get(row, "id"));
            jobs->modal = yyjson_mut_get_num(yyjson_mut_obj_get(row, "elapsedMs")) >= 250;
            break;
        }
    }
}
bool pc_ui_shell_jobs_locked(pc_ui_shell_app *app)
{
    return pc_ui_shell_jobs_runtime(app)->locked;
}
bool pc_ui_shell_jobs_event(pc_ui_shell_app *app, const SDL_Event *event)
{
    pc_ui_jobs *jobs = pc_ui_shell_jobs_runtime(app);
    if (jobs->locked && event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_ESCAPE) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
        if (p && yyjson_mut_obj_add_uint(d, p, "job", jobs->focus))
            pc_ui_shell_queue(app, "jobs.cancel", p);
        yyjson_mut_doc_free(d);
        return true;
    }
    return false;
}
bool pc_ui_shell_jobs_status(pc_ui_shell_app *app)
{
    pc_ui_jobs *jobs = pc_ui_shell_jobs_runtime(app);
    yyjson_mut_val *row = running(jobs);
    if (!row)
        return false;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const char *label = yyjson_mut_get_str(yyjson_mut_obj_get(row, "label"));
    nk_layout_row_begin(ctx, NK_DYNAMIC, 16, 3);
    nk_layout_row_push(ctx, .65f);
    nk_label(ctx, label ? label : pc_i18n_t("Working…"), NK_TEXT_LEFT);
    nk_layout_row_push(ctx, .25f);
    double value = yyjson_mut_get_num(yyjson_mut_obj_get(row, "progress"));
    nk_size progress = (nk_size)(isfinite(value) ? fmin(1000, fmax(0, value * 1000)) : 0);
    nk_progress(ctx, &progress, 1000, false);
    nk_layout_row_push(ctx, .1f);
    cancel_button(app, row, "jobs.status.cancel");
    nk_layout_row_end(ctx);
    return true;
}
void pc_ui_shell_jobs_draw(pc_ui_shell_app *app, float width, float height)
{
    pc_ui_jobs *jobs = pc_ui_shell_jobs_runtime(app);
    yyjson_mut_val *row = running(jobs);
    if (!jobs->modal || !row)
        return;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const char *label = yyjson_mut_get_str(yyjson_mut_obj_get(row, "label")),
               *message = yyjson_mut_get_str(yyjson_mut_obj_get(row, "message"));
    float w = fminf(440, width - 24), h = fminf(160, height - 24);
    if (w < 100 || h < 100)
        return;
    if (nk_begin_titled(ctx, "job-progress", label ? label : pc_i18n_t("Working…"),
                        nk_rect((width - w) / 2, (height - h) / 2, w, h),
                        NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_NO_SCROLLBAR)) {
        nk_layout_row_dynamic(ctx, 28, 1);
        nk_label(ctx, message && *message ? message : pc_i18n_t("Working…"), NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 24, 1);
        double value = yyjson_mut_get_num(yyjson_mut_obj_get(row, "progress"));
        nk_size progress = (nk_size)(isfinite(value) ? fmin(1000, fmax(0, value * 1000)) : 0);
        nk_progress(ctx, &progress, 1000, false);
        nk_layout_row_dynamic(ctx, 28, 1);
        cancel_button(app, row, "jobs.cancel");
    }
    nk_end(ctx);
}
pc_status pc_ui_shell_jobs_execute(pc_ui_shell_app *app, const char *command, yyjson_val *params,
                                   pc_json_value *out, pc_error *err)
{
    pc_ui_jobs *jobs = pc_ui_shell_jobs_runtime(app);
    if (!jobs->enabled || !pc_engine_preview_job_command(command)) {
        pc_status status = pc_engine_session_execute(pc_ui_shell_app_session(app), command, params, out, err);
        if (status == PC_OK)
            pc_ui_shell_result_warnings(app, command, out);
        return status;
    }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = d ? yyjson_mut_obj(d) : NULL;
    pc_json_value reply = PC_JSON_VALUE_INIT;
    bool ok = root && yyjson_mut_obj_add_uint(d, root, "job", 0) &&
              yyjson_mut_obj_add_str(d, root, "state", "running");
    pc_status status =
        ok ? pc_json_value_from_mut(root, &reply, err) : pc_fail(err, PC_ERR_NOMEM, "job reply");
    yyjson_mut_doc_free(d);
    if (status != PC_OK)
        return status;
    bool started = false;
    uint64_t id = 0;
    status =
        pc_engine_preview_start_job(pc_ui_shell_app_session(app), command, params, &started, &id, out, err);
    if (status == PC_OK && started) {
        jobs->focus = id;
        jobs->locked = true;
        jobs->modal = false;
        pc_ui_shell_cancel_canvas_gesture(app);
        (void)yyjson_mut_set_uint(yyjson_mut_obj_get(pc_json_value_root(&reply), "job"), id);
        pc_json_value_fini(out);
        *out = reply;
        reply = (pc_json_value)PC_JSON_VALUE_INIT;
        pc_json_value fresh = PC_JSON_VALUE_INIT;
        if (pc_engine_session_execute(pc_ui_shell_app_session(app), "jobs.list", NULL, &fresh, NULL) ==
            PC_OK) {
            pc_json_value_fini(&jobs->info);
            jobs->info = fresh;
        }
    }
    pc_json_value_fini(&reply);
    return status;
}
