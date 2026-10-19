#include "ui/i18n.h"
#include "transform_ui.h"
#include "jobs_ui.h"
#include "pen_ui.h"
#include "ui/pen_tool.h"
#include "ui/direct_tool.h"
#include "ui/view_options.h"
#include "ui/widgets.h"
#include "base/hash.h"
#include "base/mem.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static void report(pc_ui_shell_app *app, pc_status status, const pc_error *error)
{
    pc_ui_shell_app_changed(app);
    if (status != PC_OK && status != PC_ERR_NOT_FOUND) {
        pc_ui_state *ui = pc_ui_shell_app_state(app);
        free(ui->status);
        ui->status = pc_strndup(error->msg, strlen(error->msg));
        ui->status_error = true;
    }
}
bool pc_ui_shell_pen_input(pc_ui_shell_app *app, const SDL_Event *event)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        ui->pen.dragging = ui->pen.has_moving = false;
        pc_ui_direct_cancel(&ui->direct);
    }
    if ((!ui->has_pen && !ui->direct.active && !pc_ui_direct_handles(ui, pc_ui_shell_input_modifiers(app))) ||
        ui->n_dialogs || pc_ui_shell_jobs_locked(app) || pc_ui_shell_keyboard_captured(app) ||
        event->type != SDL_EVENT_KEY_DOWN) {
        return false;
    }
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error error = {0};
    pc_status status =
        pc_ui_direct_key(pc_ui_shell_app_session(app), ui, pc_ui_shell_input_key(event->key.key),
                         pc_ui_shell_input_mods(event->key.mod), &out, &error);
    if (status == PC_ERR_NOT_FOUND)
        status = pc_ui_pen_key(pc_ui_shell_app_session(app), ui, pc_ui_shell_input_key(event->key.key),
                               pc_ui_shell_input_mods(event->key.mod), &out, &error);
    pc_json_value_fini(&out);
    if (status != PC_ERR_NOT_FOUND) {
        report(app, status, &error);
        return true;
    }
    return false;
}
bool pc_ui_shell_pen_canvas(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, const SDL_Event *event,
                            float factor)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (ui->n_dialogs || pc_ui_shell_jobs_locked(app) || pc_ui_shell_keyboard_captured(app) ||
        (!pc_ui_pen_tool(ui->tool) && !pc_ui_direct_handles(ui, pc_ui_shell_input_modifiers(app)) &&
         !ui->direct.active && (!ui->has_pen || !ui->pen.dragging))) {
        return false;
    }
    bool moving = event->type == SDL_EVENT_MOUSE_MOTION, down = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    if (!moving && event->type != SDL_EVENT_MOUSE_BUTTON_DOWN && event->type != SDL_EVENT_MOUSE_BUTTON_UP) {
        return false;
    }
    if (!moving && event->button.button != SDL_BUTTON_LEFT) {
        return false;
    }
    float x = (moving ? event->motion.x : event->button.x) * factor,
          y = (moving ? event->motion.y : event->button.y) * factor;
    if (!ui->pen.dragging && !ui->direct.active &&
        (x < xf->x || y < xf->y || x >= xf->x + xf->width || y >= xf->y + xf->height)) {
        return false;
    }
    float document[2];
    if (pc_ui_canvas_to_doc(xf, x, y, document) != PC_OK) {
        return ui->pen.dragging;
    }
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error error = {0};
    pc_status status = PC_ERR_NOT_FOUND;
    if (down && event->button.clicks > 1 && ui->has_pen && ui->pen.curvature) {
        status = pc_ui_pen_curvature_toggle(ui, document, xf->zoom, &error);
    }
    if (status == PC_ERR_NOT_FOUND) {
        status =
            pc_ui_shell_app_pointer(app,
                                    down     ? PC_UI_POINTER_DOWN
                                    : moving ? PC_UI_POINTER_MOVE
                                             : PC_UI_POINTER_UP,
                                    (pc_ui_tool_point){.x = document[0], .y = document[1], .pressure = 1},
                                    pc_ui_shell_input_modifiers(app), &out, &error);
    }
    pc_json_value_fini(&out);
    report(app, status, &error);
    return status != PC_ERR_NOT_FOUND;
}
static const pc_path *selected_path(const pc_ui_state *ui, const pc_engine_doc_state *state)
{
    if (!state) {
        return NULL;
    }
    const pc_document *doc = pc_doc_arc_get(state->doc);
    const char *name = ui->selected_path;
    const pc_layer *layer = state->has_active_layer ? pc_document_layer(doc, state->active_layer) : NULL;
    if (name && !strcmp(name, "work")) {
        return doc->has_work_path ? &doc->work_path : NULL;
    }
    if (name && strcmp(name, "layer")) {
        for (size_t i = 0; i < doc->npaths; i++) {
            if (pc_str_eq_c(&doc->paths[i].name, name)) {
                return &doc->paths[i].path;
            }
        }
        return NULL;
    }
    if (layer && layer->content.kind == PC_LAYER_SHAPE) {
        return &layer->content.u.shape.path;
    }
    return layer && layer->has_vector_mask ? &layer->vector_mask.path : NULL;
}
static void segment(struct nk_command_buffer *canvas, const pc_ui_canvas_xform *xf, const double a[2],
                    const double b[2], struct nk_color accent)
{
    float start[2], end[2];
    if (pc_ui_canvas_to_screen(xf, (float)a[0], (float)a[1], start) != PC_OK ||
        pc_ui_canvas_to_screen(xf, (float)b[0], (float)b[1], end) != PC_OK) {
        return;
    }
    nk_stroke_line(canvas, start[0], start[1], end[0], end[1], 3, nk_rgba(0, 0, 0, 210));
    nk_stroke_line(canvas, start[0], start[1], end[0], end[1], 1, accent);
}
pc_status pc_ui_shell_pen_draw(pc_ui_shell_app *app, pc_ui_vector_preview *cache,
                               const pc_ui_canvas_xform *xf, pc_error *err)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (!state) {
        pc_ui_vector_preview_fini(cache);
        return PC_OK;
    }
    pc_engine_doc_state transformed_state;
    const pc_ui_transform_runtime *transform = pc_ui_shell_transform_runtime(app);
    bool transforming = ui->transform.active && ui->transform.path;
    if (transforming && transform->preview.result) {
        transformed_state = *state;
        transformed_state.doc = transform->preview.result;
        transformed_state.revision = UINT64_MAX / 2 - transform->preview.generation;
        state = &transformed_state;
    }
    bool editing = ui->has_pen && (!ui->pen.document || ui->pen.document == pc_doc_arc_get(state->doc)->id);
    pc_path path = {0};
    bool *target_path = pc_ui_view_show_flag(&ui->view, "targetPath");
    bool vector_tool = pc_ui_pen_tool(ui->tool) ||
                       (ui->tool >= PC_TOOL_ADD_ANCHOR_POINT && ui->tool <= PC_TOOL_CUSTOM_SHAPE);
    bool shows = ui->view.extras && target_path && *target_path &&
                 (vector_tool || ui->vector_mask_target || ui->selected_path);
    const pc_path *source = editing || !shows ? NULL : selected_path(ui, state);
    bool direct = pc_ui_direct_handles(ui, pc_ui_shell_input_modifiers(app)) || ui->direct.active;
    if (direct && !editing && shows) {
        source = ui->direct.has_preview ? &ui->direct.preview : pc_ui_direct_path(&ui->direct, state);
    }
    pc_status status = PC_OK;
    if (editing && ui->pen.count) {
        status = pc_ui_pen_path(&ui->pen, false, &path, err);
        source = &path;
    }
    pc_ui_tool_gesture preview = {0};
    preview.active = preview.has_shape_preview = source != NULL;
    preview.document = pc_doc_arc_get(state->doc)->id;
    preview.preview_domain = 1;
    preview.preview_revision = editing                            ? ui->pen.generation
                               : direct && ui->direct.has_preview ? ui->direct.generation
                                                                  : state->revision;
    if (!editing) {
        uint8_t hash[32];
        const char *name = direct              ? pc_str_c(&ui->direct.name)
                           : ui->selected_path ? ui->selected_path
                                               : "layer";
        pc_blake3(name, strlen(name), hash);
        memcpy(&preview.preview_domain, hash, sizeof preview.preview_domain);
        preview.preview_domain ^= direct ? ui->direct.layer : state->active_layer;
        preview.preview_domain |= UINT64_C(1) << 63;
        if (direct && ui->direct.has_preview)
            preview.preview_domain ^= UINT64_C(1) << 62;
    }
    if (source) {
        preview.shape_preview.path = *source;
    }
    if (status == PC_OK) {
        status = pc_ui_shell_vector_preview(app, cache, xf, &preview, err);
    }
    pc_path_fini(&path);
    if (status == PC_OK && direct && !editing && source && shows && !transforming) {
        struct nk_command_buffer *canvas = nk_window_get_canvas(pc_ui_shell_app_nk(app));
        struct nk_rect clip = canvas->clip;
        nk_push_scissor(canvas, nk_rect(xf->x, xf->y, xf->width, xf->height));
        struct nk_color accent = pc_ui_nk_color(pc_ui_shell_app_tokens(app)->accent);
        for (size_t i = 0; i < source->nsubpaths; i++)
            for (size_t j = 0; j < source->subpaths[i].nknots; j++) {
                const pc_knot *k = &source->subpaths[i].knots[j];
                bool selected = false;
                for (size_t a = 0; a < ui->direct.count; a++)
                    selected =
                        selected || (ui->direct.anchors[a].sub == i && ui->direct.anchors[a].knot == j);
                float screen[2];
                if (pc_ui_canvas_to_screen(xf, (float)k->anchor.x, (float)k->anchor.y, screen) != PC_OK)
                    continue;
                if (selected)
                    nk_fill_rect(canvas, nk_rect(screen[0] - 3, screen[1] - 3, 6, 6), 0, accent);
                nk_stroke_rect(canvas, nk_rect(screen[0] - 3, screen[1] - 3, 6, 6), 0, 1, accent);
                pc_point handles[2] = {k->in_ctrl, k->out_ctrl};
                for (size_t h = 0; h < 2; h++) {
                    bool segment_handle =
                        ui->direct.has_segment && ui->direct.segment.sub == i &&
                        ((!h && (ui->direct.segment.knot + 1) % source->subpaths[i].nknots == j) ||
                         (h && ui->direct.segment.knot == j));
                    if ((!selected && !segment_handle) ||
                        (handles[h].x == k->anchor.x && handles[h].y == k->anchor.y))
                        continue;
                    double a[2] = {k->anchor.x, k->anchor.y}, b[2] = {handles[h].x, handles[h].y};
                    segment(canvas, xf, a, b, accent);
                    if (pc_ui_canvas_to_screen(xf, (float)b[0], (float)b[1], screen) == PC_OK)
                        nk_fill_circle(canvas, nk_rect(screen[0] - 3, screen[1] - 3, 6, 6), accent);
                }
            }
        if (ui->direct.active && ui->direct.marquee) {
            float a[2], b[2];
            if (pc_ui_canvas_to_screen(xf, (float)ui->direct.start[0], (float)ui->direct.start[1], a) ==
                    PC_OK &&
                pc_ui_canvas_to_screen(xf, (float)ui->direct.end[0], (float)ui->direct.end[1], b) == PC_OK)
                nk_stroke_rect(
                    canvas,
                    nk_rect(fminf(a[0], b[0]), fminf(a[1], b[1]), fabsf(b[0] - a[0]), fabsf(b[1] - a[1])), 0,
                    1, accent);
        }
        nk_push_scissor(canvas, clip);
    }
    if (status != PC_OK || !editing) {
        return status;
    }
    struct nk_command_buffer *canvas = nk_window_get_canvas(pc_ui_shell_app_nk(app));
    struct nk_rect clip = canvas->clip;
    nk_push_scissor(canvas, nk_rect(xf->x, xf->y, xf->width, xf->height));
    struct nk_color accent = pc_ui_nk_color(pc_ui_shell_app_tokens(app)->accent);
    if (ui->pen.freeform) {
        size_t step = ui->pen.ntrace > 4096 ? (ui->pen.ntrace + 4095) / 4096 : 1;
        for (size_t i = step; i < ui->pen.ntrace; i += step) {
            segment(canvas, xf, ui->pen.trace[i - step].point, ui->pen.trace[i].point, accent);
        }
        if (ui->pen.ntrace > 1 && (ui->pen.ntrace - 1) % step) {
            size_t last = ((ui->pen.ntrace - 1) / step) * step;
            segment(canvas, xf, ui->pen.trace[last].point, ui->pen.trace[ui->pen.ntrace - 1].point, accent);
        }
    }
    for (size_t i = 0; i < ui->pen.count; i++) {
        const pc_ui_pen_knot *knot = &ui->pen.knots[i];
        float anchor[2];
        if (pc_ui_canvas_to_screen(xf, (float)knot->points[0][0], (float)knot->points[0][1], anchor) !=
            PC_OK) {
            continue;
        }
        for (size_t j = 1; i + 1 == ui->pen.count && j < 3; j++) {
            if (!memcmp(knot->points[0], knot->points[j], 2 * sizeof(double))) {
                continue;
            }
            segment(canvas, xf, knot->points[0], knot->points[j], accent);
            float handle[2];
            if (pc_ui_canvas_to_screen(xf, (float)knot->points[j][0], (float)knot->points[j][1], handle) ==
                PC_OK) {
                nk_fill_circle(canvas, nk_rect(handle[0] - 3, handle[1] - 3, 6, 6), accent);
            }
        }
        if (i + 1 == ui->pen.count) {
            nk_fill_rect(canvas, nk_rect(anchor[0] - 3, anchor[1] - 3, 6, 6), 0, accent);
        }
        nk_stroke_rect(canvas, nk_rect(anchor[0] - 3, anchor[1] - 3, 6, 6), 0, 1, nk_rgb(255, 255, 255));
    }
    if (ui->pen.count && ui->pen.has_hover && !ui->pen.dragging) {
        segment(canvas, xf, ui->pen.knots[ui->pen.count - 1].points[0], ui->pen.hover, accent);
    }
    nk_push_scissor(canvas, clip);
    return PC_OK;
}
static void select_path(pc_ui_state *ui, const char *name)
{
    char *copy = pc_strndup(name, strlen(name));
    if (copy) {
        free(ui->selected_path);
        ui->selected_path = copy;
        ui->vector_mask_target = !strcmp(name, "layer");
    }
}
static void path_action(pc_ui_shell_app *app, const char *id, const char *name)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = doc ? yyjson_mut_obj(doc) : NULL;
    if (p && yyjson_mut_obj_add_strcpy(doc, p, "name", name)) {
        pc_ui_shell_queue(app, id, p);
    }
    yyjson_mut_doc_free(doc);
}
void pc_ui_shell_paths_panel(pc_ui_shell_app *app)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    nk_layout_row_dynamic(ctx, 24, 1);
    if (!state) {
        nk_label(ctx, pc_i18n_t("No document"), NK_TEXT_LEFT);
        return;
    }
    const pc_document *doc = pc_doc_arc_get(state->doc);
    const pc_layer *layer = state->has_active_layer ? pc_document_layer(doc, state->active_layer) : NULL;
    bool layer_path = layer && (layer->has_vector_mask || layer->content.kind == PC_LAYER_SHAPE);
    size_t rows = doc->npaths + (doc->has_work_path ? 1 : 0) + (layer_path ? 1 : 0);
    nk_layout_row_dynamic(ctx, 150, 1);
    struct nk_list_view view;
    if (nk_list_view_begin(ctx, &view, "document-paths", NK_WINDOW_BORDER, 25,
                           (int)(rows > 100000 ? 100000 : rows))) {
        for (int row = view.begin; row < view.end; row++) {
            if (row < 0 || (size_t)row >= rows) {
                continue;
            }
            size_t index = (size_t)row;
            const char *name = NULL, *label = NULL;
            if (doc->has_work_path && !index) {
                name = "work";
                label = "Work Path";
            } else {
                index -= doc->has_work_path ? 1u : 0u;
                if (index < doc->npaths) {
                    name = label = pc_str_c(&doc->paths[index].name);
                } else if (layer_path) {
                    name = "layer";
                    label = "Layer Path";
                }
            }
            if (!name) {
                continue;
            }
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_bool selected = ui->selected_path && !strcmp(ui->selected_path, name);
            char id[64];
            (void)snprintf(id, sizeof id, "path.select.%s", name);
            pc_ui_shell_widget_mapping(app, id, nk_widget_bounds(ctx));
            if (nk_selectable_label(ctx, label, NK_TEXT_LEFT, &selected)) {
                select_path(ui, name);
            }
        }
        nk_list_view_end(&view);
    }
    const char *name = ui->selected_path    ? ui->selected_path
                       : doc->has_work_path ? "work"
                       : layer_path         ? "layer"
                                            : NULL;
    nk_layout_row_dynamic(ctx, 26, 3);
    if (nk_button_label(ctx, pc_i18n_t("Selection")) && name) {
        path_action(app, "path.toSelection", name);
    }
    if (nk_button_label(ctx, pc_i18n_t("From Selection"))) {
        pc_ui_shell_queue(app, "select.toWorkPath", NULL);
        select_path(ui, "work");
        name = ui->selected_path;
    }
    if (nk_button_label(ctx, pc_i18n_t("Vector Mask")) && name) {
        path_action(app, "layer.vectorMask.fromPath", name);
    }
    if (nk_button_label(ctx, pc_i18n_t("Fill")) && name) {
        path_action(app, "path.fill", name);
    }
    if (nk_button_label(ctx, pc_i18n_t("Stroke")) && name) {
        path_action(app, "path.stroke", name);
    }
    if (nk_button_label(ctx, pc_i18n_t("Delete")) && name) {
        path_action(app, "path.delete", name);
    }
}
