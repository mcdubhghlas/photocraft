#include "sampling_ui.h"
#include "canvas_view.h"
#include "canvas_ink.h"
#include "jobs_ui.h"
#include "engine/prefs.h"
#include "ui/i18n.h"
#include "ui/widgets.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
bool pc_ui_shell_sampling_event(pc_ui_shell_app *app, const SDL_Event *event)
{
    pc_ui_sampling_tool *r = pc_ui_shell_sampling_runtime(app);
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST)
        r->active = false;
    if (event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_ESCAPE && r->active) {
        r->active = false;
        return true;
    }
    return false;
}
bool pc_ui_shell_sampling_canvas(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, const SDL_Event *event,
                                 float factor)
{
    pc_ui_sampling_tool *r = pc_ui_shell_sampling_runtime(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    unsigned mods = pc_ui_shell_input_modifiers(app);
    if (ui->n_dialogs || ui->transform.active || pc_ui_shell_jobs_locked(app) ||
        (!r->active && !pc_ui_sampling_tool_kind(ui->tool) && !pc_ui_alt_samples_color(ui->tool, mods)))
        return false;
    bool moving = event->type == SDL_EVENT_MOUSE_MOTION, down = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    if (!moving && !down && event->type != SDL_EVENT_MOUSE_BUTTON_UP)
        return false;
    if (!moving && event->button.button != SDL_BUTTON_LEFT)
        return false;
    float x = (moving ? event->motion.x : event->button.x) * factor,
          y = (moving ? event->motion.y : event->button.y) * factor;
    if (!r->active && (!down || x < xf->x || y < xf->y || x >= xf->x + xf->width || y >= xf->y + xf->height))
        return false;
    float doc[2];
    if (pc_ui_canvas_to_doc(xf, x, y, doc) != PC_OK)
        return false;
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status status = pc_ui_shell_app_pointer(app,
                                               down     ? PC_UI_POINTER_DOWN
                                               : moving ? PC_UI_POINTER_MOVE
                                                        : PC_UI_POINTER_UP,
                                               (pc_ui_tool_point){.x = doc[0], .y = doc[1], .pressure = 1},
                                               mods, &out, &err);
    pc_json_value_fini(&out);
    if (status != PC_OK && status != PC_ERR_NOT_FOUND)
        pc_ui_shell_report(app, "Sampling", err.msg, true);
    pc_ui_shell_app_changed(app);
    return status != PC_ERR_NOT_FOUND;
}
static struct nk_color color(const float rgba[4])
{
    return nk_rgba((int)lroundf(fminf(1, fmaxf(0, rgba[0])) * 255),
                   (int)lroundf(fminf(1, fmaxf(0, rgba[1])) * 255),
                   (int)lroundf(fminf(1, fmaxf(0, rgba[2])) * 255), 255);
}
static void arc(struct nk_command_buffer *canvas, const pc_ui_canvas_xform *xf, float x, float y,
                float radius, double start, double end, float width, struct nk_color ink)
{
    for (unsigned i = 0; i < 24; i++) {
        double a = start + (end - start) * i / 24, b = start + (end - start) * (i + 1) / 24;
        pc_ui_canvas_ink_line(canvas, xf, x + (float)cos(a) * radius, y + (float)sin(a) * radius,
                              x + (float)cos(b) * radius, y + (float)sin(b) * radius, width, ink);
    }
}
void pc_ui_shell_sampling_draw(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_sampling_tool *r = pc_ui_shell_sampling_runtime(app);
    pc_engine_session *session = pc_ui_shell_app_session(app);
    pc_ui_sampling_sync(r, session, ui);
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (!state)
        return;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    const pc_ui_tokens *tokens = pc_ui_shell_app_tokens(app);
    for (size_t i = 0; i < state->ncolor_samplers && i < 10; i++) {
        float p[2];
        double x = state->color_samplers[i][0], y = state->color_samplers[i][1];
        if (r->active && r->mode == 2 && r->index == i) {
            x = r->point[0];
            y = r->point[1];
        }
        if (pc_ui_canvas_to_screen(xf, (float)x, (float)y, p) != PC_OK ||
            !pc_ui_canvas_ink_visible(xf, p[0], p[1], 20))
            continue;
        for (unsigned pass = 0; pass < 2; pass++) {
            float width = pass ? 1 : 3;
            struct nk_color ink = pass ? nk_rgb(255, 255, 255) : nk_rgba(0, 0, 0, 170);
            arc(canvas, xf, p[0], p[1], 5, 0, 6.283185307179586, width, ink);
            for (unsigned axis = 0; axis < 2; axis++)
                for (unsigned side = 0; side < 2; side++) {
                    float sign = side ? 1 : -1, a[2] = {p[0], p[1]}, b[2] = {p[0], p[1]};
                    a[axis] += sign * 6;
                    b[axis] += sign * 8;
                    pc_ui_canvas_ink_line(canvas, xf, a[0], a[1], b[0], b[1], width, ink);
                }
        }
        if (pc_ui_canvas_ink_visible(xf, p[0] + 7, p[1] - 20, 0)) {
            char label[12];
            int n = snprintf(label, sizeof label, "%zu", i + 1);
            nk_draw_text(canvas, nk_rect(p[0] + 7, p[1] - 20, 24, 16), label, n, ctx->style.font,
                         nk_rgba(0, 0, 0, 0), pc_ui_nk_color(tokens->text));
        }
    }
    if (!r->active || !r->has_sample || r->mode == 2 || !ui->tool_options.eyedropper_ring)
        return;
    pc_json_value preference = PC_JSON_VALUE_INIT;
    (void)pc_engine_prefs_get(session, "cursors.other", &preference, NULL);
    const char *cursor = yyjson_mut_get_str(pc_json_value_root(&preference));
    bool precise = cursor && !strcmp(cursor, "precise");
    pc_json_value_fini(&preference);
    if (precise)
        return;
    float p[2];
    if (pc_ui_canvas_to_screen(xf, r->point[0], r->point[1], p) != PC_OK)
        return;
    const double pi = 3.141592653589793;
    for (unsigned pass = 0; pass < 2; pass++)
        for (unsigned half = 0; half < 2; half++) {
            double start = half ? .16 : pi + .16, end = half ? pi - .16 : 2 * pi - .16;
            struct nk_color ink = pass ? color(half ? r->before : r->sample) : pc_ui_nk_color(tokens->shadow);
            arc(canvas, xf, p[0], p[1], 11, start, end, pass ? 4 : 5.5f, ink);
        }
}
void pc_ui_shell_sampling_options(pc_ui_shell_app *app)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (ui->tool == PC_TOOL_COLOR_SAMPLER) {
        const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
        size_t count = state ? state->ncolor_samplers : 0;
        char number[32];
        (void)snprintf(number, sizeof number, "%zu", count);
        const char *keys[] = {"n", "max"}, *values[] = {number, "10"};
        pc_buf label = PC_BUF_INIT;
        (void)pc_i18n_fmt(pc_i18n_t("{n} of {max} sample points"), keys, values, 2, &label, NULL);
        nk_layout_row_begin(ctx, NK_STATIC, 24, 2);
        nk_layout_row_push(ctx, 180);
        nk_label(ctx, label.len ? pc_buf_cstr(&label) : number, NK_TEXT_LEFT);
        pc_buf_fini(&label);
        nk_layout_row_push(ctx, 100);
        pc_ui_shell_widget_mapping(app, "view.colorSamplers.clear", nk_widget_bounds(ctx));
        if (!count)
            nk_widget_disable_begin(ctx);
        if (nk_button_label(ctx, pc_i18n_t("Clear All")) && count)
            pc_ui_shell_queue(app, "view.colorSamplers.clear", NULL);
        if (!count)
            nk_widget_disable_end(ctx);
        nk_layout_row_end(ctx);
        return;
    }
    const uint32_t sizes[] = {1, 3, 5, 11, 31, 51, 101};
    const char *labels[] = {"Point Sample",     "3 by 3 Average",   "5 by 5 Average",    "11 by 11 Average",
                            "31 by 31 Average", "51 by 51 Average", "101 by 101 Average"};
    const char *translated[7];
    int choice = 0;
    for (size_t i = 0; i < 7; i++) {
        translated[i] = pc_i18n_t(labels[i]);
        if (ui->tool_options.eyedropper_size == sizes[i])
            choice = (int)i;
    }
    nk_layout_row_begin(ctx, NK_STATIC, 24, 5);
    nk_layout_row_push(ctx, 90);
    nk_label(ctx, pc_i18n_t("Sample Size:"), NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 160);
    pc_ui_shell_widget_mapping(app, "eyedropper.size", nk_widget_bounds(ctx));
    int next = nk_combo(ctx, translated, 7, choice, 24, nk_vec2(200, 210));
    if (next >= 0 && next < 7)
        ui->tool_options.eyedropper_size = sizes[next];
    nk_layout_row_push(ctx, 70);
    nk_label(ctx, pc_i18n_t("Sample:"), NK_TEXT_LEFT);
    const char *names[] = {"Current Layer", "Current & Below", "All Layers", "All Layers No Adjustments",
                           "Current & Below No Adjustments"};
    const char *values[] = {"current", "currentAndBelow", "all", "allNoAdjustments",
                            "currentAndBelowNoAdjustments"};
    const char *translations[5];
    choice = 2;
    for (size_t i = 0; i < 5; i++) {
        translations[i] = pc_i18n_t(names[i]);
        if (pc_str_eq_c(&ui->tool_options.eyedropper_sample, values[i]))
            choice = (int)i;
    }
    nk_layout_row_push(ctx, 240);
    pc_ui_shell_widget_mapping(app, "eyedropper.sample", nk_widget_bounds(ctx));
    next = nk_combo(ctx, translations, 5, choice, 24, nk_vec2(300, 180));
    if (next >= 0 && next < 5 && next != choice)
        (void)pc_str_set_c(&ui->tool_options.eyedropper_sample, values[next], NULL);
    nk_layout_row_push(ctx, 190);
    nk_bool ring = ui->tool_options.eyedropper_ring;
    pc_ui_shell_widget_mapping(app, "eyedropper.ring", nk_widget_bounds(ctx));
    if (nk_checkbox_label(ctx, pc_i18n_t("Show Sampling Ring"), &ring))
        ui->tool_options.eyedropper_ring = ring != 0;
    nk_layout_row_end(ctx);
}
