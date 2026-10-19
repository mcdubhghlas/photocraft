#include "slice_ui.h"
#include "canvas_ink.h"
#include "canvas_view.h"
#include "dialogs.h"
#include "jobs_ui.h"
#include "engine/slice_ui.h"
#include "engine/channel_display.h"
#include "engine/prefs.h"
#include "ui/i18n.h"
#include "ui/widgets.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static pc_status invoke(pc_ui_shell_app *app, const char *command, const yyjson_mut_val *params,
                        pc_json_value *out, pc_error *err)
{
    pc_buf text = PC_BUF_INIT;
    pc_status status = pc_json_write(params, 0, &text, err);
    if (status == PC_OK)
        status = pc_engine_session_execute_json(pc_ui_shell_app_session(app), command, pc_buf_cstr(&text),
                                                out, err);
    pc_buf_fini(&text);
    return status;
}
static pc_status open_form(pc_ui_shell_app *app, bool divide, const pc_resolved_slice *slice,
                           pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    const pc_document *doc = state ? pc_doc_arc_get(state->doc) : NULL;
    if (!doc)
        return pc_fail(err, PC_ERR_INVALID, "no slice document");
    const pc_slice *stored = slice->has_id ? pc_slices_get(&doc->slices, slice->id) : NULL;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = d ? yyjson_mut_obj(d) : NULL;
    bool ok = root && yyjson_mut_obj_add_str(d, root, "__command", divide ? "slice.divide" : "slice.set") &&
              yyjson_mut_obj_add_str(d, root, "__label", divide ? "Divide Slice" : "Slice Options") &&
              yyjson_mut_obj_add_bool(d, root, "__form", true) &&
              yyjson_mut_obj_add_uint(d, root, "number", slice->number) &&
              yyjson_mut_obj_add_uint(d, root, "__document", doc->id) &&
              yyjson_mut_obj_add_uint(d, root, "__revision", state->revision);
    bool quick = false;
    size_t alpha = 0;
    bool channel = pc_engine_targeted_channel(pc_ui_shell_app_session(app), &quick, &alpha);
    yyjson_mut_val *layers = d ? yyjson_mut_arr(d) : NULL;
    for (size_t i = 0; ok && i < state->nselected; i++)
        ok = yyjson_mut_arr_add_uint(d, layers, state->selected_layers[i]);
    ok =
        ok && layers && yyjson_mut_obj_add_val(d, root, "__layers", layers) &&
        yyjson_mut_obj_add_uint(d, root, "__layer", state->has_active_layer ? state->active_layer : 0) &&
        yyjson_mut_obj_add_uint(d, root, "__color", pc_engine_targeted_color(pc_ui_shell_app_session(app))) &&
        yyjson_mut_obj_add_bool(d, root, "__mask", pc_engine_targeted_mask(pc_ui_shell_app_session(app))) &&
        yyjson_mut_obj_add_bool(d, root, "__channel", channel) &&
        yyjson_mut_obj_add_bool(d, root, "__quick", quick) &&
        yyjson_mut_obj_add_uint(d, root, "__alpha", alpha);
    if (divide)
        ok = ok && yyjson_mut_obj_add_uint(d, root, "horizontal", 2) &&
             yyjson_mut_obj_add_uint(d, root, "vertical", 1);
    else {
        const char *names[] = {"name", "url", "target", "alt", "message", "cellText"};
        const pc_str *values[] = {&slice->name,
                                  stored ? &stored->url : NULL,
                                  stored ? &stored->target : NULL,
                                  stored ? &stored->alt : NULL,
                                  stored ? &stored->message : NULL,
                                  stored ? &stored->cell_text : NULL};
        for (size_t i = 0; ok && i < 6; i++)
            ok = yyjson_mut_obj_add_strcpy(d, root, names[i], values[i] ? pc_str_c(values[i]) : "");
        ok = ok && yyjson_mut_obj_add_str(d, root, "kind", pc_slice_kind_id(slice->kind)) &&
             yyjson_mut_obj_add_str(d, root, "background", "none");
        yyjson_mut_val *choices = yyjson_mut_obj(d), *kinds = yyjson_mut_arr(d);
        ok = ok && choices && kinds && yyjson_mut_arr_add_str(d, kinds, "image") &&
             yyjson_mut_arr_add_str(d, kinds, "noImage") &&
             yyjson_mut_obj_add_val(d, choices, "kind", kinds) &&
             yyjson_mut_obj_add_val(d, root, "__choices", choices);
        if (stored && stored->has_background) {
            char color[12];
            (void)snprintf(color, sizeof color, "#%02x%02x%02x%02x", stored->background[1],
                           stored->background[2], stored->background[3], stored->background[0]);
            (void)yyjson_mut_obj_remove_key(root, "background");
            ok = ok && yyjson_mut_obj_add_strcpy(d, root, "background", color);
        }
    }
    if (!ok) {
        yyjson_mut_doc_free(d);
        return pc_fail(err, PC_ERR_NOMEM, "slice dialog");
    }
    return pc_ui_shell_dialog_store(app, PC_DIALOG_COMMAND, d, root, out, err);
}
pc_status pc_ui_shell_slice_command(pc_ui_shell_app *app, const char *command, pc_json_value *out,
                                    pc_error *err)
{
    if (strncmp(command, "ui.slice.", 9))
        return PC_ERR_NOT_FOUND;
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (ui->n_dialogs)
        return pc_fail(err, PC_ERR_INVALID, "close the dialog before editing slices");
    if (pc_engine_slices_locked(pc_ui_shell_app_session(app)))
        return pc_fail(err, PC_ERR_INVALID, "slices are locked");
    pc_status status =
        pc_ui_slice_resolve(&ui->slices, pc_engine_session_active(pc_ui_shell_app_session(app)), err);
    if (status != PC_OK)
        return status;
    const pc_resolved_slice *slice = pc_ui_slice_selected(&ui->slices);
    if (!slice)
        return pc_fail(err, PC_ERR_INVALID, "select a slice first");
    if (!strcmp(command, "ui.slice.options") || !strcmp(command, "ui.slice.divide"))
        return open_form(app, !strcmp(command, "ui.slice.divide"), slice, out, err);
    const char *engine = !strcmp(command, "ui.slice.promote")  ? "slice.promote"
                         : !strcmp(command, "ui.slice.delete") ? "slice.delete"
                                                               : NULL;
    if (!engine)
        return PC_ERR_NOT_FOUND;
    if (!slice->has_id && !strcmp(engine, "slice.delete"))
        return pc_fail(err, PC_ERR_INVALID, "promote the auto slice before deleting it");
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *params = d ? yyjson_mut_obj(d) : NULL;
    status = params && yyjson_mut_obj_add_uint(d, params, "number", slice->number)
                 ? invoke(app, engine, params, out, err)
                 : pc_fail(err, PC_ERR_NOMEM, "slice command");
    yyjson_mut_doc_free(d);
    if (status == PC_OK) {
        ui->slices.cached = false;
        if (!strcmp(engine, "slice.delete"))
            ui->slices.has_selected = false;
        else {
            yyjson_mut_val *number = yyjson_mut_obj_get(pc_json_value_root(out), "number");
            if (yyjson_mut_is_uint(number))
                ui->slices.selected = (size_t)yyjson_mut_get_uint(number);
        }
    }
    return status;
}
bool pc_ui_shell_slice_event(pc_ui_shell_app *app, const SDL_Event *event)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        ui->slices.dragging = false;
        return false;
    }
    if (!pc_ui_slice_tool(ui->tool) || ui->transform.active || ui->n_dialogs ||
        pc_ui_shell_keyboard_captured(app) || event->type != SDL_EVENT_KEY_DOWN)
        return false;
    if (event->key.key == SDLK_ESCAPE && ui->slices.dragging) {
        ui->slices.dragging = false;
        return true;
    }
    if ((event->key.key == SDLK_DELETE || event->key.key == SDLK_BACKSPACE) && ui->slices.has_selected) {
        pc_json_value out = PC_JSON_VALUE_INIT;
        pc_error err = {0};
        pc_status status = pc_ui_shell_slice_command(app, "ui.slice.delete", &out, &err);
        pc_json_value_fini(&out);
        if (status != PC_OK)
            pc_ui_shell_report(app, "Slice", err.msg, true);
        return true;
    }
    return false;
}
bool pc_ui_shell_slice_canvas(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, const SDL_Event *event,
                              float factor)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (!pc_ui_slice_tool(ui->tool) || ui->n_dialogs || ui->transform.active ||
        pc_ui_shell_jobs_locked(app) || pc_ui_shell_keyboard_captured(app))
        return false;
    bool motion = event->type == SDL_EVENT_MOUSE_MOTION, down = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    if (!motion && !down && event->type != SDL_EVENT_MOUSE_BUTTON_UP)
        return false;
    if (!motion && event->button.button != SDL_BUTTON_LEFT)
        return false;
    float x = (motion ? event->motion.x : event->button.x) * factor,
          y = (motion ? event->motion.y : event->button.y) * factor;
    if (!ui->slices.dragging &&
        (!down || x < xf->x || y < xf->y || x >= xf->x + xf->width || y >= xf->y + xf->height))
        return false;
    float p[2];
    if (pc_ui_canvas_to_doc(xf, x, y, p) != PC_OK)
        return false;
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status status = pc_ui_shell_app_pointer(app,
                                               down     ? PC_UI_POINTER_DOWN
                                               : motion ? PC_UI_POINTER_MOVE
                                                        : PC_UI_POINTER_UP,
                                               (pc_ui_tool_point){.x = p[0], .y = p[1], .pressure = 1},
                                               pc_ui_shell_input_modifiers(app), &out, &err);
    if (status == PC_OK && down && event->button.clicks > 1 && ui->tool == PC_TOOL_SLICE_SELECT) {
        ui->slices.dragging = false;
        pc_json_value_fini(&out);
        status = pc_ui_shell_slice_command(app, "ui.slice.options", &out, &err);
    }
    pc_json_value_fini(&out);
    if (status != PC_OK)
        pc_ui_shell_report(app, "Slice", err.msg, true);
    pc_ui_shell_app_changed(app);
    return true;
}
static void outline(struct nk_command_buffer *canvas, const pc_ui_canvas_xform *xf, pc_rect rect,
                    struct nk_color color, float width, bool handles, bool dashed, size_t *budget)
{
    float q[4][2];
    for (size_t i = 0; i < 4; i++)
        if (pc_ui_canvas_to_screen(xf, (float)(i == 1 || i == 2 ? rect.x1 : rect.x0),
                                   (float)(i >= 2 ? rect.y1 : rect.y0), q[i]) != PC_OK)
            return;
    for (size_t i = 0; i < 4; i++) {
        size_t next = (i + 1) % 4;
        if (dashed)
            pc_ui_canvas_ink_dash(canvas, xf, q[i][0], q[i][1], q[next][0], q[next][1], width, color, 3, 3, 0,
                                  budget);
        else if (*budget) {
            pc_ui_canvas_ink_line(canvas, xf, q[i][0], q[i][1], q[next][0], q[next][1], width, color);
            --*budget;
        }
        if (handles) {
            float points[2][2] = {{q[i][0], q[i][1]},
                                  {(q[i][0] + q[next][0]) / 2, (q[i][1] + q[next][1]) / 2}};
            for (size_t h = 0; h < 2; h++)
                if (pc_ui_canvas_ink_visible(xf, points[h][0], points[h][1], 4))
                    nk_fill_rect(canvas, nk_rect(points[h][0] - 2, points[h][1] - 2, 4, 4), 0, color);
        }
    }
}
void pc_ui_shell_slice_draw(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_slice_state *s = &ui->slices;
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (!state)
        return;
    bool tool = pc_ui_slice_tool(ui->tool);
    if (!tool &&
        !(ui->view.extras && ui->view.show[6] && !pc_slices_is_empty(&pc_doc_arc_get(state->doc)->slices)))
        return;
    if (pc_ui_slice_resolve(s, state, NULL) != PC_OK)
        return;
    if (s->dragging && (s->document != pc_doc_arc_get(state->doc)->id || s->revision != state->revision ||
                        s->tool != (int)ui->tool))
        s->dragging = false;
    pc_json_value prefs = PC_JSON_VALUE_INIT;
    (void)pc_engine_prefs_get(pc_ui_shell_app_session(app), "guidesGridAndSlices", &prefs, NULL);
    yyjson_mut_val *p = pc_json_value_root(&prefs);
    const char *hex = yyjson_mut_get_str(yyjson_mut_obj_get(p, "sliceColor"));
    unsigned rgb[3] = {0x38, 0xb5, 0xff};
    if (hex && strlen(hex) == 7 && hex[0] == '#') {
        unsigned r, g, b;
        if (sscanf(hex + 1, "%2x%2x%2x", &r, &g, &b) == 3) {
            rgb[0] = r;
            rgb[1] = g;
            rgb[2] = b;
        }
    }
    struct nk_color color = nk_rgb((int)rgb[0], (int)rgb[1], (int)rgb[2]),
                    selected_color = nk_rgb(255, 176, 0);
    bool numbers = yyjson_mut_get_bool(yyjson_mut_obj_get(p, "showSliceNumbers"));
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    struct nk_rect saved = canvas->clip;
    nk_push_scissor(canvas, nk_rect(xf->x, xf->y, xf->width, xf->height));
    size_t budget = 65536;
    for (unsigned pass = 0; pass < 2; pass++)
        for (size_t i = 0; i < s->count && i < 65536; i++) {
            const pc_resolved_slice *slice = &s->resolved[i];
            bool automatic = slice->origin == PC_SLICE_AUTO;
            if (automatic != (pass == 0) || (automatic && (!tool || s->hide_auto)))
                continue;
            bool chosen = s->has_selected && s->selected == slice->number;
            outline(canvas, xf, slice->rect, chosen ? selected_color : color, chosen && tool ? 2 : 1,
                    chosen && tool && slice->origin == PC_SLICE_USER, automatic, &budget);
            float top[2];
            if (numbers && (double)pc_rect_width(slice->rect) * xf->zoom > 18 &&
                (double)pc_rect_height(slice->rect) * xf->zoom > 12 &&
                pc_ui_canvas_to_screen(xf, (float)slice->rect.x0, (float)slice->rect.y0, top) == PC_OK &&
                pc_ui_canvas_ink_visible(xf, top[0], top[1], 0)) {
                char label[32];
                (void)snprintf(label, sizeof label, "%02zu%s", slice->number,
                               slice->origin == PC_SLICE_LAYER ? " *" : "");
                struct nk_rect badge = nk_rect(top[0] + 2, top[1] + 2, 36, 18);
                struct nk_color background = automatic ? nk_rgb(110, 110, 110)
                                             : chosen  ? selected_color
                                                       : color;
                nk_fill_rect(canvas, badge, 2, background);
                nk_draw_text(canvas, badge, label, (int)strlen(label), ctx->style.font, background,
                             nk_rgb(255, 255, 255));
            }
        }
    if (s->dragging)
        outline(canvas, xf, pc_ui_slice_drag_rect(s), selected_color, 2, false, false, &budget);
    nk_push_scissor(canvas, saved);
    pc_json_value_fini(&prefs);
}
static void button(pc_ui_shell_app *app, const char *id, const char *label, float width, bool enabled)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_layout_row_push(ctx, width);
    if (!enabled)
        nk_widget_disable_begin(ctx);
    pc_ui_shell_widget_mapping(app, id, nk_widget_bounds(ctx));
    if (nk_button_label(ctx, pc_i18n_t(label)) && enabled)
        pc_ui_shell_queue(app, id, NULL);
    if (!enabled)
        nk_widget_disable_end(ctx);
}
void pc_ui_shell_slice_options(pc_ui_shell_app *app)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    bool locked = pc_engine_slices_locked(pc_ui_shell_app_session(app));
    if (state)
        (void)pc_ui_slice_resolve(&ui->slices, state, NULL);
    const pc_resolved_slice *selected = pc_ui_slice_selected(&ui->slices);
    nk_layout_row_begin(ctx, NK_STATIC, 24, 7);
    nk_layout_row_push(ctx, 120);
    nk_label(ctx, pc_i18n_t(ui->tool == PC_TOOL_SLICE ? "Style: Normal" : "Slice Select"), NK_TEXT_LEFT);
    if (ui->tool == PC_TOOL_SLICE)
        button(app, "slice.fromGuides", "Slices From Guides", 160, state && !locked);
    else {
        button(app, "ui.slice.promote", "Promote", 90, selected && !locked);
        button(app, "ui.slice.divide", "Divide…", 90, selected && !locked);
        button(app, "ui.slice.delete", "Delete", 80, selected && selected->has_id && !locked);
        nk_layout_row_push(ctx, 150);
        nk_bool hide = ui->slices.hide_auto;
        (void)nk_checkbox_label(ctx, pc_i18n_t("Hide Auto Slices"), &hide);
        ui->slices.hide_auto = hide != 0;
        button(app, "ui.slice.options", "Slice Options…", 140, selected && !locked);
    }
    if (locked) {
        nk_layout_row_push(ctx, 150);
        nk_label(ctx, pc_i18n_t("Slices are locked"), NK_TEXT_LEFT);
    }
    nk_layout_row_end(ctx);
}
