#include "artboard_ui.h"
#include "context_ui.h"
#include "canvas_view.h"
#include "canvas_ink.h"
#include "transform_ui.h"
#include "dialogs.h"
#include "jobs_ui.h"
#include "doc/comps.h"
#include "engine/artboard_cmds.h"
#include "ui/i18n.h"
#include "ui/widgets.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static void rect_points(const pc_ui_canvas_xform *xf, pc_rect r, float q[4][2])
{
    for (size_t i = 0; i < 4; i++)
        if (pc_ui_canvas_to_screen(xf, (float)(i == 1 || i == 2 ? r.x1 : r.x0), (float)(i >= 2 ? r.y1 : r.y0),
                                   q[i]) != PC_OK)
            q[i][0] = q[i][1] = NAN;
}
/* A rotated gap is convex. Clip it before entering Nuklear's short-coordinate command buffer. */
static void fill_gap(struct nk_command_buffer *canvas, const pc_ui_canvas_xform *xf, pc_rect r,
                     struct nk_color color)
{
    float polygon[16][2] = {{0}}, temp[16][2];
    rect_points(xf, r, polygon);
    size_t count = 4;
    const double bounds[] = {fmax(-32760, xf->x), fmin(32760, (double)xf->x + xf->width), fmax(-32760, xf->y),
                             fmin(32760, (double)xf->y + xf->height)};
    for (size_t edge = 0; edge < 4 && count; edge++) {
        size_t axis = edge / 2, n = 0;
        bool upper = edge % 2 != 0;
        for (size_t i = 0; i < count; i++) {
            const float *p = polygon[i], *q = polygon[(i + 1) % count];
            if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(q[0]) || !isfinite(q[1]))
                return;
            double a = upper ? bounds[edge] - p[axis] : p[axis] - bounds[edge],
                   b = upper ? bounds[edge] - q[axis] : q[axis] - bounds[edge];
            if (a >= 0 && n < 16) {
                temp[n][0] = p[0];
                temp[n++][1] = p[1];
            }
            if ((a >= 0) != (b >= 0) && n < 16) {
                double t = a / (a - b);
                temp[n][0] = (float)(p[0] + ((double)q[0] - p[0]) * t);
                temp[n++][1] = (float)(p[1] + ((double)q[1] - p[1]) * t);
            }
        }
        memcpy(polygon, temp, n * sizeof *polygon);
        count = n;
    }
    if (count >= 3)
        nk_fill_polygon(canvas, &polygon[0][0], (int)count, color);
}
static pc_ui_artboard_layout *layout(pc_ui_shell_app *app)
{
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    pc_ui_artboard_layout *l = pc_ui_shell_artboard_layout(app);
    return state && pc_ui_artboard_layout_update(l, state, NULL) == PC_OK ? l : NULL;
}
void pc_ui_shell_artboard_pasteboard(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf)
{
    pc_ui_artboard_layout *l = layout(app);
    if (!l || !l->count)
        return;
    struct nk_command_buffer *canvas = nk_window_get_canvas(pc_ui_shell_app_nk(app));
    struct nk_color color = pc_ui_nk_color(pc_ui_shell_canvas_color(app));
    for (size_t i = 0; i < l->ngaps; i++)
        fill_gap(canvas, xf, l->gaps[i], color);
}
static void outline(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, pc_rect rect, struct nk_color color,
                    bool handles)
{
    float q[4][2];
    rect_points(xf, rect, q);
    struct nk_command_buffer *canvas = nk_window_get_canvas(pc_ui_shell_app_nk(app));
    for (size_t i = 0; i < 4; i++) {
        size_t next = (i + 1) % 4;
        pc_ui_canvas_ink_line(canvas, xf, q[i][0], q[i][1], q[next][0], q[next][1], 1, color);
        if (handles) {
            float points[2][2] = {{q[i][0], q[i][1]},
                                  {(q[i][0] + q[next][0]) / 2, (q[i][1] + q[next][1]) / 2}};
            for (size_t h = 0; h < 2; h++)
                if (pc_ui_canvas_ink_visible(xf, points[h][0], points[h][1], 4)) {
                    struct nk_rect marker = nk_rect(points[h][0] - 3, points[h][1] - 3, 6, 6);
                    nk_fill_rect(canvas, marker, 0, color);
                    char id[80];
                    (void)snprintf(id, sizeof id, "artboard.%s.%zu", h ? "edge" : "corner", i);
                    pc_ui_shell_widget_mapping(app, id, nk_rect(marker.x - 3, marker.y - 3, 12, 12));
                }
        }
    }
}
void pc_ui_shell_artboard_frames(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf)
{
    pc_ui_artboard_layout *l = layout(app);
    if (!l)
        return;
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    const pc_document *doc = pc_doc_arc_get(state->doc);
    pc_layer_id active = 0;
    pc_rect active_rect;
    bool has_active = pc_ui_artboard_active(state, &active, &active_rect);
    const pc_ui_tokens *tokens = pc_ui_shell_app_tokens(app);
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    struct nk_rect saved = canvas->clip;
    nk_push_scissor(canvas, nk_rect(xf->x, xf->y, xf->width, xf->height));
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    for (size_t i = 0; i < l->count; i++) {
        const pc_ui_artboard_entry *b = &l->boards[i];
        bool selected = has_active && b->id == active;
        outline(app, xf, b->rect, pc_ui_nk_color(selected ? tokens->accent_border : tokens->text_faint),
                ui->tool == PC_TOOL_ARTBOARD && selected);
        float p[2];
        if (b->index < doc->nlayers &&
            pc_ui_canvas_to_screen(xf, (float)b->rect.x0, (float)b->rect.y0, p) == PC_OK &&
            pc_ui_canvas_ink_visible(xf, p[0], p[1] - 18, 0)) {
            const char *name = pc_str_c(&doc->layers[b->index].name);
            size_t length = strlen(name);
            if (length > 1024)
                length = 1024;
            nk_draw_text(canvas, nk_rect(p[0], p[1] - 18, 250, 16), name, (int)length, ctx->style.font,
                         pc_ui_nk_color(pc_ui_shell_canvas_color(app)),
                         pc_ui_nk_color(selected ? tokens->text : tokens->text_dim));
        }
    }
    if (ui->tool == PC_TOOL_ARTBOARD && has_active) {
        double points[4][2];
        pc_ui_artboard_plus_points(active_rect, xf->zoom, points);
        const char *sides[] = {"top", "right", "bottom", "left"};
        for (size_t i = 0; i < 4; i++) {
            float p[2];
            if (pc_ui_canvas_to_screen(xf, (float)points[i][0], (float)points[i][1], p) != PC_OK ||
                !pc_ui_canvas_ink_visible(xf, p[0], p[1], 8))
                continue;
            struct nk_rect button = nk_rect(p[0] - 8, p[1] - 8, 16, 16);
            nk_fill_rect(canvas, button, 2, pc_ui_nk_color(tokens->card));
            nk_stroke_rect(canvas, button, 2, 1, pc_ui_nk_color(tokens->text_dim));
            nk_stroke_line(canvas, p[0] - 4, p[1], p[0] + 4, p[1], 1, pc_ui_nk_color(tokens->text));
            nk_stroke_line(canvas, p[0], p[1] - 4, p[0], p[1] + 4, 1, pc_ui_nk_color(tokens->text));
            char id[80];
            (void)snprintf(id, sizeof id, "artboard.plus.%s", sides[i]);
            pc_ui_shell_widget_mapping(app, id, button);
        }
    }
    if (ui->artboard_tool.active) {
        if (ui->tool != PC_TOOL_ARTBOARD || ui->artboard_tool.document != doc->id ||
            ui->artboard_tool.revision != state->revision)
            ui->artboard_tool.active = false;
        else
            outline(app, xf, pc_ui_artboard_drag_rect(&ui->artboard_tool),
                    pc_ui_nk_color(tokens->accent_border), true);
    }
    nk_push_scissor(canvas, saved);
}
bool pc_ui_shell_artboard_event(pc_ui_shell_app *app, const SDL_Event *event)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        ui->artboard_tool.active = false;
        return false;
    }
    if (ui->tool != PC_TOOL_ARTBOARD || ui->transform.active || ui->n_dialogs ||
        pc_ui_shell_keyboard_captured(app))
        return false;
    if (event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_ESCAPE && ui->artboard_tool.active) {
        ui->artboard_tool.active = false;
        return true;
    }
    return false;
}
bool pc_ui_shell_artboard_canvas(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, const SDL_Event *event,
                                 float factor)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (ui->tool != PC_TOOL_ARTBOARD || ui->transform.active || ui->n_dialogs ||
        pc_ui_shell_jobs_locked(app) || pc_ui_shell_keyboard_captured(app))
        return false;
    bool moving = event->type == SDL_EVENT_MOUSE_MOTION, down = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    if (!moving && !down && event->type != SDL_EVENT_MOUSE_BUTTON_UP)
        return false;
    if (!moving && event->button.button != SDL_BUTTON_LEFT)
        return false;
    float x = (moving ? event->motion.x : event->button.x) * factor,
          y = (moving ? event->motion.y : event->button.y) * factor;
    if (!ui->artboard_tool.active &&
        (!down || x < xf->x || y < xf->y || x >= xf->x + xf->width || y >= xf->y + xf->height))
        return false;
    float p[2];
    if (pc_ui_canvas_to_doc(xf, x, y, p) != PC_OK)
        return false;
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status status = pc_ui_shell_app_pointer(app,
                                               down     ? PC_UI_POINTER_DOWN
                                               : moving ? PC_UI_POINTER_MOVE
                                                        : PC_UI_POINTER_UP,
                                               (pc_ui_tool_point){.x = p[0], .y = p[1], .pressure = 1},
                                               pc_ui_shell_input_modifiers(app), &out, &err);
    pc_json_value_fini(&out);
    if (status != PC_OK)
        pc_ui_shell_report(app, "Artboard", err.msg, true);
    pc_ui_shell_app_changed(app);
    return true;
}
static void queue_property(pc_ui_shell_app *app, pc_layer_id layer, const char *field, int64_t value)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
    char coalesce[96];
    (void)snprintf(coalesce, sizeof coalesce, "artboard-%llu-%s", (unsigned long long)layer,
                   (!strcmp(field, "x") || !strcmp(field, "y")) ? "xy" : "wh");
    if (p && yyjson_mut_obj_add_uint(d, p, "layer", layer) && yyjson_mut_obj_add_sint(d, p, field, value) &&
        yyjson_mut_obj_add_strcpy(d, p, "coalesce", coalesce))
        pc_ui_shell_queue(app, "layer.artboard.set", p);
    yyjson_mut_doc_free(d);
}
static void number(pc_ui_shell_app *app, pc_layer_id layer, const char *prefix, const char *field,
                   const char *label, double original, double min, double max)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    double value = original;
    char id[80];
    (void)snprintf(id, sizeof id, "%s.%s", prefix, field);
    pc_ui_shell_widget_mapping(app, id, nk_widget_bounds(ctx));
    nk_property_double(ctx, pc_i18n_t(label), fmin(min, original), &value, fmax(max, original), 1, 1);
    if (value != original && isfinite(value))
        queue_property(app, layer, field, (int64_t)round(value));
}
static void preset(pc_ui_shell_app *app, pc_layer_id layer, const pc_artboard *board, const char *widget)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    size_t count = 0;
    const pc_engine_artboard_preset *presets = pc_engine_artboard_presets(&count);
    const char *names[64] = {"Custom"};
    int current = 0;
    if (count > 63)
        count = 63;
    for (size_t i = 0; i < count; i++) {
        names[i + 1] = presets[i].name;
        if (pc_str_eq_c(&board->preset, presets[i].name))
            current = (int)i + 1;
    }
    pc_ui_shell_widget_mapping(app, widget, nk_widget_bounds(ctx));
    int choice = nk_combo(ctx, names, (int)count + 1, current, 24, nk_vec2(220, 280));
    if (choice > 0 && choice <= (int)count && choice != current) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
        if (p && yyjson_mut_obj_add_uint(d, p, "layer", layer) &&
            yyjson_mut_obj_add_str(d, p, "preset", presets[(size_t)choice - 1].name))
            pc_ui_shell_queue(app, "layer.artboard.set", p);
        yyjson_mut_doc_free(d);
    }
}
void pc_ui_shell_artboard_properties(pc_ui_shell_app *app, const pc_layer *layer)
{
    const pc_artboard *board = pc_layer_artboard(layer);
    if (!board)
        return;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_layout_row_dynamic(ctx, 24, 1);
    nk_label(ctx, pc_i18n_t("Artboard"), NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, 24, 1);
    preset(app, layer->id, board, "artboard.props.preset");
    nk_layout_row_dynamic(ctx, 24, 2);
    number(app, layer->id, "artboard.props", "width", "Width", pc_rect_width(board->rect), 1, 300000);
    number(app, layer->id, "artboard.props", "height", "Height", pc_rect_height(board->rect), 1, 300000);
    nk_layout_row_dynamic(ctx, 24, 2);
    number(app, layer->id, "artboard.props", "x", "X", board->rect.x0, -300000, 300000);
    number(app, layer->id, "artboard.props", "y", "Y", board->rect.y0, -300000, 300000);
    nk_layout_row_dynamic(ctx, 24, 1);
    const char *names[] = {"White", "Black", "Transparent", "Other…"},
               *values[] = {"white", "black", "transparent", "custom"};
    pc_ui_shell_widget_mapping(app, "artboard.props.background", nk_widget_bounds(ctx));
    int choice = nk_combo(ctx, names, 4, (int)board->background.kind, 24, nk_vec2(180, 150));
    if (choice >= 0 && choice < 4 && choice != (int)board->background.kind) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
        if (p && yyjson_mut_obj_add_uint(d, p, "layer", layer->id) &&
            yyjson_mut_obj_add_str(d, p, "background", values[choice])) {
            if (choice == 3)
                (void)yyjson_mut_obj_add_str(d, p, "color", "#ffffff");
            pc_ui_shell_queue(app, "layer.artboard.set", p);
        }
        yyjson_mut_doc_free(d);
    }
    if (board->background.kind == PC_ARTBOARD_CUSTOM) {
        nk_layout_row_dynamic(ctx, 24, 1);
        pc_ui_shell_widget_mapping(app, "artboard.props.color", nk_widget_bounds(ctx));
        if (nk_button_label(ctx, pc_i18n_t("Color…"))) {
            float rgba[4] = {1, 1, 1, 1};
            (void)pc_artboard_background_rgba(&board->background, rgba);
            yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
            yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
            pc_json_value out = PC_JSON_VALUE_INIT;
            pc_error err = {0};
            pc_status status =
                p && yyjson_mut_obj_add_uint(d, p, "layer", layer->id) &&
                        yyjson_mut_obj_add_str(d, p, "background", "custom")
                    ? pc_ui_shell_dialog_color_command(app, "Artboard Background Color", rgba,
                                                       "layer.artboard.set", p, "color", &out, &err)
                    : pc_fail(&err, PC_ERR_NOMEM, "artboard color");
            if (status != PC_OK)
                pc_ui_shell_report(app, "Artboard", err.msg, true);
            pc_json_value_fini(&out);
            yyjson_mut_doc_free(d);
        }
    }
}
void pc_ui_shell_artboard_options(pc_ui_shell_app *app)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    pc_layer_id id;
    pc_rect rect;
    nk_layout_row_begin(ctx, NK_STATIC, 24, 7);
    nk_layout_row_push(ctx, 100);
    nk_label(ctx, pc_i18n_t("Artboard"), NK_TEXT_LEFT);
    if (pc_ui_artboard_active(state, &id, &rect)) {
        const pc_artboard *board = pc_layer_artboard(pc_document_layer(pc_doc_arc_get(state->doc), id));
        nk_layout_row_push(ctx, 180);
        preset(app, id, board, "artboard.preset");
        nk_layout_row_push(ctx, 130);
        number(app, id, "artboard", "width", "Width", pc_rect_width(rect), 1, 300000);
        nk_layout_row_push(ctx, 130);
        number(app, id, "artboard", "height", "Height", pc_rect_height(rect), 1, 300000);
        const char *labels[] = {"Portrait", "Landscape", "Add Artboard"},
                   *commands[] = {"ui.artboard.portrait", "ui.artboard.landscape", "ui.artboard.addRight"};
        for (size_t i = 0; i < 3; i++) {
            nk_layout_row_push(ctx, i == 2 ? 130 : 100);
            pc_ui_shell_widget_mapping(app, commands[i], nk_widget_bounds(ctx));
            if (nk_button_label(ctx, pc_i18n_t(labels[i])))
                pc_ui_shell_queue(app, commands[i], NULL);
        }
    } else {
        nk_layout_row_push(ctx, 400);
        nk_label(ctx, pc_i18n_t("Drag to create an artboard"), NK_TEXT_LEFT);
    }
    nk_layout_row_end(ctx);
}
pc_status pc_ui_shell_artboard_command(pc_ui_shell_app *app, const char *id, yyjson_val *params,
                                       pc_json_value *out, pc_error *err)
{
    (void)params;
    if (!strcmp(id, "view.fitArtboardOnScreen")) {
        pc_ui_view *view = pc_ui_canvas_active_view(app);
        const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
        pc_ui_canvas_xform xf = {0};
        bool mapped = pc_ui_shell_app_canvas_mapping(app, &xf);
        pc_layer_id layer;
        pc_rect rect;
        pc_status status = pc_ui_artboard_fit(view, state, mapped ? xf.width : 1200, mapped ? xf.height : 800,
                                              pc_ui_shell_app_ui_scale(app), &layer, &rect, err);
        if (status != PC_OK)
            return status;
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *o = d ? yyjson_mut_obj(d) : NULL, *r = d ? yyjson_mut_arr(d) : NULL;
        bool ok = o && r && yyjson_mut_obj_add_real(d, o, "zoom", view->zoom) &&
                  yyjson_mut_obj_add_uint(d, o, "artboard", layer) &&
                  yyjson_mut_arr_add_sint(d, r, rect.x0) && yyjson_mut_arr_add_sint(d, r, rect.y0) &&
                  yyjson_mut_arr_add_sint(d, r, rect.x1) && yyjson_mut_arr_add_sint(d, r, rect.y1) &&
                  yyjson_mut_obj_add_val(d, o, "bounds", r);
        status = ok ? pc_json_value_from_mut(o, out, err) : pc_fail(err, PC_ERR_NOMEM, "artboard view");
        yyjson_mut_doc_free(d);
        return status;
    }
    if (strcmp(id, "ui.artboard.portrait") && strcmp(id, "ui.artboard.landscape") &&
        strcmp(id, "ui.artboard.addRight"))
        return PC_ERR_NOT_FOUND;
    if (pc_ui_shell_jobs_locked(app) || pc_ui_shell_app_state(app)->n_dialogs)
        return pc_fail(err, PC_ERR_INVALID, "finish the open dialog or document job first");
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    pc_layer_id layer;
    pc_rect rect;
    if (!pc_ui_artboard_active(state, &layer, &rect))
        return pc_fail(err, PC_ERR_INVALID, "select an artboard");
    char text[180];
    const char *command = "layer.artboard.set";
    if (!strcmp(id, "ui.artboard.addRight")) {
        command = "layer.artboard.addBeside";
        (void)snprintf(text, sizeof text, "{\"layer\":%llu,\"side\":\"right\",\"duplicate\":%s}",
                       (unsigned long long)layer,
                       (pc_ui_shell_input_modifiers(app) & PC_UI_MOD_ALT) ? "true" : "false");
    } else {
        uint32_t w = pc_rect_width(rect), h = pc_rect_height(rect);
        bool portrait = !strcmp(id, "ui.artboard.portrait");
        uint32_t width = portrait ? (w < h ? w : h) : (w > h ? w : h),
                 height = portrait ? (w > h ? w : h) : (w < h ? w : h);
        (void)snprintf(text, sizeof text, "{\"layer\":%llu,\"width\":%u,\"height\":%u}",
                       (unsigned long long)layer, width, height);
    }
    return pc_engine_session_execute_json(pc_ui_shell_app_session(app), command, text, out, err);
}
