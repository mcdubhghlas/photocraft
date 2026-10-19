#include "timeline_ui.h"
#include "shell_internal.h"
#include "jobs_ui.h"
#include "ui/timeline.h"
#include "ui/i18n.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <math.h>

static void request(pc_ui_shell_app *app, const char *command, const char *field, double value, bool integer)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
    if ((!isfinite(value) || value < 0 || value >= 18446744073709551616.0) && field) {
        yyjson_mut_doc_free(d);
        return;
    }
    if (p && (!field || (integer ? yyjson_mut_obj_add_uint(d, p, field, (uint64_t)value)
                                 : yyjson_mut_obj_add_real(d, p, field, value))))
        pc_ui_shell_queue(app, command, p);
    yyjson_mut_doc_free(d);
}
static bool button(pc_ui_shell_app *app, const char *label, const char *command)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_ui_shell_widget_mapping(app, command, nk_widget_bounds(ctx));
    bool clicked = nk_button_label(ctx, pc_i18n_t(label));
    if (clicked)
        request(app, command, NULL, 0, false);
    return clicked;
}
void pc_ui_shell_timeline_draw(pc_ui_shell_app *app)
{
    pc_ui_timeline *ui = &pc_ui_shell_app_state(app)->timeline;
    if (!ui->open) {
        ui->playing = false;
        return;
    }
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    const pc_document *doc = state ? pc_doc_arc_get(state->doc) : NULL;
    const pc_timeline *timeline = doc && doc->has_timeline ? &doc->timeline : NULL;
    bool locked = pc_ui_shell_jobs_locked(app);
    if (nk_begin(ctx, "Timeline", nk_rect(80, 580, 850, 180),
                 NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_CLOSABLE)) {
        if (locked)
            nk_widget_disable_begin(ctx);
        if (!doc) {
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_label(ctx, pc_i18n_t("Open a document to use the timeline."), NK_TEXT_LEFT);
            ui->playing = false;
        } else if (!timeline) {
            nk_layout_row_dynamic(ctx, 30, 1);
            button(app, "Create Video Timeline", "timeline.create");
            ui->playing = false;
        } else if (!timeline->duration || timeline->duration > 1000000 ||
                   timeline->current >= timeline->duration || !isfinite(timeline->fps) ||
                   timeline->fps <= 0 || timeline->fps > 1000 || timeline->work_start >= timeline->duration ||
                   timeline->work_end > timeline->duration) {
            nk_layout_row_dynamic(ctx, 28, 1);
            nk_label_wrap(ctx, pc_i18n_t("Timeline settings are outside the supported range."));
            ui->playing = false;
        } else {
            nk_layout_row_dynamic(ctx, 28, 6);
            pc_ui_shell_widget_mapping(app, "timeline.play", nk_widget_bounds(ctx));
            if (nk_button_label(ctx, pc_i18n_t(ui->playing ? "Pause" : "Play"))) {
                ui->playing = !ui->playing;
                ui->document = doc->id;
                ui->last_time = (double)SDL_GetTicks();
            }
            button(app, "Previous frame", "timeline.previousFrame");
            char label[128];
            (void)snprintf(label, sizeof label, "%zu / %zu   %.2fs", timeline->current + 1,
                           timeline->duration, (double)pc_timeline_time(timeline));
            nk_label(ctx, label, NK_TEXT_CENTERED);
            button(app, "Next frame", "timeline.nextFrame");
            (void)snprintf(label, sizeof label, "%.2f fps", (double)timeline->fps);
            nk_label(ctx, label, NK_TEXT_RIGHT);
            if (button(app, "Delete Timeline", "timeline.delete"))
                ui->playing = false;
            nk_layout_row_dynamic(ctx, 24, 1);
            int current = (int)timeline->current;
            pc_ui_shell_widget_mapping(app, "timeline.playhead", nk_widget_bounds(ctx));
            if (nk_slider_int(ctx, 0, &current, (int)(timeline->duration ? timeline->duration - 1 : 0), 1)) {
                ui->playing = false;
                request(app, "timeline.setFrame", "frame", current, true);
            }
            nk_layout_row_dynamic(ctx, 26, 4);
            int duration = (int)timeline->duration;
            pc_ui_shell_widget_mapping(app, "timeline.duration", nk_widget_bounds(ctx));
            nk_property_int(ctx, pc_i18n_t("Duration"), 1, &duration, 1000000, 1, 1);
            if (duration != (int)timeline->duration) {
                ui->playing = false;
                request(app, "timeline.setProps", "duration", duration, true);
            }
            float fps = timeline->fps;
            pc_ui_shell_widget_mapping(app, "timeline.fps", nk_widget_bounds(ctx));
            nk_property_float(ctx, pc_i18n_t("Frame Rate"), .01f, &fps, 1000, 1, .1f);
            if (fps != timeline->fps) {
                ui->playing = false;
                request(app, "timeline.setProps", "fps", fps, false);
            }
            int start = (int)timeline->work_start, end = (int)timeline->work_end;
            pc_ui_shell_widget_mapping(app, "timeline.workStart", nk_widget_bounds(ctx));
            nk_property_int(ctx, pc_i18n_t("Work Start"), 0, &start, (int)timeline->duration - 1, 1, 1);
            pc_ui_shell_widget_mapping(app, "timeline.workEnd", nk_widget_bounds(ctx));
            nk_property_int(ctx, pc_i18n_t("Work End"), 1, &end, (int)timeline->duration, 1, 1);
            if (start != (int)timeline->work_start) {
                ui->playing = false;
                request(app, "timeline.setProps", "workStart", start, true);
            }
            if (end != (int)timeline->work_end) {
                ui->playing = false;
                request(app, "timeline.setProps", "workEnd", end, true);
            }
        }
        if (locked)
            nk_widget_disable_end(ctx);
    }
    nk_end(ctx);
    if (nk_window_is_closed(ctx, "Timeline")) {
        ui->open = ui->playing = false;
        nk_window_show(ctx, "Timeline", NK_SHOWN);
    }
    size_t frame;
    if (!locked && doc && pc_ui_timeline_step(ui, doc->id, timeline, (double)SDL_GetTicks(), &frame))
        request(app, "timeline.setFrame", "frame", (double)frame, true);
}
