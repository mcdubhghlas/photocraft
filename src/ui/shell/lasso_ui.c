#include "canvas_ink.h"
#include "lasso_ui.h"
#include "canvas_view.h"
#include "jobs_ui.h"
#include "ui/i18n.h"
#include <math.h>
#include <string.h>
static void report(pc_ui_shell_app *app, pc_status status, pc_error *err)
{
    if (status != PC_OK && status != PC_ERR_NOT_FOUND)
        pc_ui_shell_report(app, "Lasso", err->msg, true);
    pc_ui_shell_app_changed(app);
}
pc_status pc_ui_shell_lasso_command(pc_ui_shell_app *app, const char *id, pc_json_value *out, pc_error *err)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_lasso_sync(pc_ui_shell_app_session(app), ui);
    if (!ui->lasso.active || ui->transform.active)
        return PC_ERR_NOT_FOUND;
    if (!strcmp(id, "edit.undo"))
        return pc_ui_lasso_retract(pc_ui_shell_app_session(app), ui, out, err);
    if (!strcmp(id, "ui.lasso.finish"))
        return pc_ui_lasso_finish(pc_ui_shell_app_session(app), ui, false, out, err);
    if (!strcmp(id, "ui.lasso.cancel"))
        return pc_ui_lasso_key(pc_ui_shell_app_session(app), ui, PC_KEY_ESCAPE, 0, 1, out, err);
    return PC_ERR_NOT_FOUND;
}
bool pc_ui_shell_lasso_event(pc_ui_shell_app *app, const SDL_Event *event)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_lasso_sync(pc_ui_shell_app_session(app), ui);
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        if (ui->lasso.down && ui->tool == PC_TOOL_LASSO && !ui->lasso.polygonal)
            pc_ui_lasso_fini(&ui->lasso);
        ui->lasso.down = false;
        return false;
    }
    if (ui->transform.active || ui->n_dialogs || ui->context_menu.open ||
        pc_ui_shell_keyboard_captured(app) || pc_ui_menu_nav_is_open(pc_ui_shell_app_nav(app)) ||
        pc_ui_shell_jobs_locked(app))
        return false;
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status status = PC_ERR_NOT_FOUND;
    if (event->type == SDL_EVENT_KEY_UP && (event->key.key == SDLK_LALT || event->key.key == SDLK_RALT) &&
        ui->tool == PC_TOOL_LASSO && ui->lasso.active && ui->lasso.polygonal && !ui->lasso.down)
        status = pc_ui_lasso_finish(pc_ui_shell_app_session(app), ui, true, &out, &err);
    else if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat) {
        pc_ui_view *view = pc_ui_canvas_active_view(app);
        status = pc_ui_lasso_key(pc_ui_shell_app_session(app), ui, pc_ui_shell_input_key(event->key.key),
                                 pc_ui_shell_input_mods(event->key.mod), view ? view->zoom : 1, &out, &err);
    }
    pc_json_value_fini(&out);
    if (status != PC_ERR_NOT_FOUND) {
        report(app, status, &err);
        return true;
    }
    return false;
}
bool pc_ui_shell_lasso_canvas(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, const SDL_Event *event,
                              float factor)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_lasso_sync(pc_ui_shell_app_session(app), ui);
    if (!pc_ui_lasso_tool(ui->tool) || ui->transform.active || ui->n_dialogs ||
        pc_ui_shell_keyboard_captured(app) || pc_ui_shell_jobs_locked(app))
        return false;
    bool moving = event->type == SDL_EVENT_MOUSE_MOTION, down = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    if (!moving && event->type != SDL_EVENT_MOUSE_BUTTON_UP && !down)
        return false;
    if (!moving && event->button.button != SDL_BUTTON_LEFT)
        return false;
    float x = (moving ? event->motion.x : event->button.x) * factor;
    float y = (moving ? event->motion.y : event->button.y) * factor;
    if (!pc_ui_shell_canvas_pointer_active(app) &&
        (x < xf->x || y < xf->y || x >= xf->x + xf->width || y >= xf->y + xf->height))
        return false;
    float p[2];
    if (pc_ui_canvas_to_doc(xf, x, y, p) != PC_OK)
        return false;
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    bool close = down && event->button.clicks > 1 && ui->lasso.path.n >= 3 &&
                 hypot(ui->lasso.path.v[ui->lasso.path.n - 1][0] - p[0],
                       ui->lasso.path.v[ui->lasso.path.n - 1][1] - p[1]) < 8 / fmax(.01, xf->zoom);
    pc_status status = close
                           ? pc_ui_lasso_finish(pc_ui_shell_app_session(app), ui, false, &out, &err)
                           : pc_ui_shell_app_pointer(app,
                                                     down     ? PC_UI_POINTER_DOWN
                                                     : moving ? PC_UI_POINTER_MOVE
                                                              : PC_UI_POINTER_UP,
                                                     (pc_ui_tool_point){.x = p[0], .y = p[1], .pressure = 1},
                                                     pc_ui_shell_input_modifiers(app), &out, &err);
    pc_json_value_fini(&out);
    report(app, status, &err);
    return status != PC_ERR_NOT_FOUND;
}
static void segment(struct nk_command_buffer *canvas, const pc_ui_canvas_xform *xf, const double a[2],
                    const double b[2], struct nk_color color, double phase, size_t *budget)
{
    float p[2], q[2];
    if (pc_ui_canvas_to_screen(xf, (float)a[0], (float)a[1], p) == PC_OK &&
        pc_ui_canvas_to_screen(xf, (float)b[0], (float)b[1], q) == PC_OK &&
        pc_ui_canvas_clip_line(xf, p, q)) {
        if (!*budget)
            return;
        nk_stroke_line(canvas, p[0], p[1], q[0], q[1], 1, nk_rgba(0, 0, 0, 235));
        --*budget;
        pc_ui_canvas_ink_dash(canvas, xf, p[0], p[1], q[0], q[1], 1, color, 4, 4, phase, budget);
    }
}
void pc_ui_shell_lasso_draw(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_lasso_sync(pc_ui_shell_app_session(app), ui);
    const pc_ui_lasso *l = &ui->lasso;
    if (!l->active)
        return;
    struct nk_command_buffer *canvas = nk_window_get_canvas(pc_ui_shell_app_nk(app));
    struct nk_rect saved = canvas->clip;
    nk_push_scissor(canvas, nk_rect(xf->x, xf->y, xf->width, xf->height));
    size_t budget = 65536;
    double phase = fmod((double)SDL_GetTicks() / 75, 8);
    for (unsigned list = 0; list < 2; list++) {
        const pc_ui_lasso_points *p = list ? &l->live : &l->path;
        for (size_t i = 1; i < p->n; i++)
            segment(canvas, xf, p->v[i - 1], p->v[i], nk_rgb(255, 255, 255), phase, &budget);
    }
    if (l->path.n && !l->live.n)
        segment(canvas, xf, l->path.v[l->path.n - 1], l->cursor, nk_rgb(255, 255, 255), phase, &budget);
    for (size_t i = 0; l->tool == PC_TOOL_MAGNETIC_LASSO && i < l->nanchors; i++) {
        if (l->anchors[i] >= l->path.n)
            continue;
        float p[2];
        const double *a = l->path.v[l->anchors[i]];
        if (pc_ui_canvas_to_screen(xf, (float)a[0], (float)a[1], p) == PC_OK &&
            pc_ui_canvas_ink_visible(xf, p[0], p[1], 4))
            nk_fill_rect(canvas, nk_rect(p[0] - 2, p[1] - 2, 4, 4), 0, nk_rgb(255, 255, 255));
    }
    nk_push_scissor(canvas, saved);
}
void pc_ui_shell_lasso_options(pc_ui_shell_app *app)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_tool_options *o = &ui->tool_options;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_layout_row_begin(ctx, NK_STATIC, 24, 9);
    nk_layout_row_push(ctx, 150);
    nk_label(ctx, pc_i18n_t(pc_ui_tool_label(ui->tool)), NK_TEXT_LEFT);
    const char *modes[] = {"New", "Add", "Subtract", "Intersect"};
    nk_layout_row_push(ctx, 100);
    int mode =
        nk_combo(ctx, modes, 4, ui->selection_mode < 4 ? ui->selection_mode : 0, 24, nk_vec2(140, 140));
    ui->selection_mode = (uint8_t)mode;
    nk_layout_row_push(ctx, 140);
    nk_property_float(ctx, pc_i18n_t("Feather"), 0, &o->feather, 1000, 1, .1f);
    nk_layout_row_push(ctx, 100);
    nk_bool antialias = o->anti_alias;
    (void)nk_checkbox_label(ctx, pc_i18n_t("Anti-alias"), &antialias);
    o->anti_alias = antialias != 0;
    if (ui->tool == PC_TOOL_MAGNETIC_LASSO) {
        nk_layout_row_push(ctx, 130);
        nk_property_float(ctx, pc_i18n_t("Width"), 1, &o->magnetic_width, 256, 1, .1f);
        nk_layout_row_push(ctx, 130);
        nk_property_float(ctx, pc_i18n_t("Contrast"), 1, &o->magnetic_contrast, 100, 1, .1f);
        nk_layout_row_push(ctx, 150);
        nk_property_float(ctx, pc_i18n_t("Frequency"), 0, &o->magnetic_frequency, 100, 1, .1f);
        nk_layout_row_push(ctx, 110);
        nk_bool pressure = o->magnetic_pressure;
        (void)nk_checkbox_label(ctx, pc_i18n_t("Pressure"), &pressure);
        o->magnetic_pressure = pressure != 0;
    }
    nk_layout_row_end(ctx);
}
