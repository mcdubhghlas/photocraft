#include "engine/floating.h"
#include "canvas_view.h"
#include "canvas_ink.h"
#include "transform_ui.h"
#include "ui/pen_tool.h"
#include "ui/type_tool.h"
#include "ui/widgets.h"
#include "ui/i18n.h"
#include "ui/menus.h"
#include "ui/sizing.h"
#include "engine/transform_cmds.h"
#include "engine/channel_display.h"
#include "engine/prefs.h"
#include "jobs_ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static const char *engine_command(const pc_ui_transform *t)
{
    return t->selection ? "select.transformSelection" : t->path ? "path.transform" : "edit.transform";
}
static const pc_path *path_for(const pc_document *doc, const char *name, pc_layer_id id)
{
    if (!strcmp(name, "layer")) {
        const pc_layer *layer = pc_document_layer(doc, id);
        return !layer                                  ? NULL
               : layer->content.kind == PC_LAYER_SHAPE ? &layer->content.u.shape.path
               : layer->has_vector_mask                ? &layer->vector_mask.path
                                                       : NULL;
    }
    if (!strcmp(name, "work"))
        return doc->has_work_path ? &doc->work_path : NULL;
    for (size_t i = 0; i < doc->npaths; i++)
        if (pc_str_eq_c(&doc->paths[i].name, name))
            return &doc->paths[i].path;
    return NULL;
}
static unsigned target(pc_ui_shell_app *app, size_t *alpha)
{
    const pc_engine_session *s = pc_ui_shell_app_session(app);
    bool quick = false;
    if (pc_engine_targeted_channel(s, &quick, alpha))
        return quick ? 2u : 3u;
    return pc_ui_shell_app_state(app)->mask_target || pc_engine_targeted_mask(s) ||
                   pc_engine_session_tools(s)->mask_targeted
               ? 1u
               : 0u;
}
static bool valid_scope(pc_ui_shell_app *app)
{
    const pc_ui_transform *t = &pc_ui_shell_app_state(app)->transform;
    const pc_ui_transform_runtime *r = pc_ui_shell_transform_runtime(app);
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (!t->active || r->session != t->session || !state || pc_doc_arc_get(state->doc)->id != t->document ||
        state->revision != t->revision || state->nselected != r->nselected)
        return false;
    for (size_t i = 0; i < r->nselected; i++)
        if (state->selected_layers[i] != r->selected[i])
            return false;
    if (!t->selection && !t->path) {
        size_t alpha = 0;
        unsigned current = target(app, &alpha);
        if (current != t->target || (current == 3 && alpha != t->alpha) ||
            (state->has_active_layer ? state->active_layer : 0) != t->layer)
            return false;
    }
    if (t->path) {
        const pc_ui_state *ui = pc_ui_shell_app_state(app);
        const char *name = ui->selected_path     ? ui->selected_path
                           : ui->direct.name.len ? pc_str_c(&ui->direct.name)
                                                 : "work";
        if (strcmp(name, t->path_name) || !path_for(pc_doc_arc_get(state->doc), t->path_name, t->layer))
            return false;
    }
    return true;
}
static bool put_target(yyjson_mut_doc *d, yyjson_mut_val *p, const pc_ui_transform *t)
{
    yyjson_mut_val *v;
    if (t->target == 3) {
        v = yyjson_mut_obj(d);
        if (!v || !yyjson_mut_obj_add_uint(d, v, "channel", t->alpha))
            return false;
    } else
        v = yyjson_mut_str(d, t->target == 1 ? "mask" : t->target == 2 ? "quickMask" : "pixels");
    return v && yyjson_mut_obj_add_val(d, p, "target", v);
}
static yyjson_mut_doc *parameters(pc_ui_shell_app *app, bool preview)
{
    const pc_ui_transform *t = &pc_ui_shell_app_state(app)->transform;
    const pc_ui_transform_runtime *r = pc_ui_shell_transform_runtime(app);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL, *rect = d ? yyjson_mut_arr(d) : NULL,
                   *quad = d ? yyjson_mut_arr(d) : NULL;
    bool ok = p && rect && quad;
    for (size_t i = 0; ok && i < 4; i++) {
        yyjson_mut_val *point = yyjson_mut_arr(d);
        ok = yyjson_mut_arr_add_real(d, rect, t->frame.rect[i]) && point &&
             yyjson_mut_arr_add_real(d, point, t->frame.quad[i][0]) &&
             yyjson_mut_arr_add_real(d, point, t->frame.quad[i][1]) && yyjson_mut_arr_append(quad, point);
    }
    ok = ok && yyjson_mut_obj_add_val(d, p, "rect", rect) && yyjson_mut_obj_add_val(d, p, "quad", quad) &&
         yyjson_mut_obj_add_strcpy(d, p, "interpolation", t->interpolation);
    if (t->path) {
        ok = ok && yyjson_mut_obj_add_strcpy(d, p, "name", t->path_name);
        if (!strcmp(t->path_name, "layer"))
            ok = ok && yyjson_mut_obj_add_uint(d, p, "layer", t->layer);
    } else if (!t->selection) {
        ok = ok && put_target(d, p, t);
        if (t->target < 2)
            ok = ok && yyjson_mut_obj_add_uint(d, p, "layer", t->layer);
    }
    if (t->copy)
        ok = ok && yyjson_mut_obj_add_bool(d, p, "copy", true);
    if (preview) {
        const pc_engine_session *s = pc_ui_shell_app_session(app);
        bool quick = false;
        size_t alpha = 0;
        bool channel = pc_engine_targeted_channel(s, &quick, &alpha);
        yyjson_mut_val *layers = yyjson_mut_arr(d);
        for (size_t i = 0; ok && layers && i < r->nselected; i++)
            ok = yyjson_mut_arr_add_uint(d, layers, r->selected[i]);
        ok = ok && layers && yyjson_mut_obj_add_val(d, p, "__layers", layers) &&
             yyjson_mut_obj_add_str(d, p, "__command", engine_command(t)) &&
             yyjson_mut_obj_add_bool(d, p, "__filter", true) &&
             yyjson_mut_obj_add_bool(d, p, "preview", true) &&
             yyjson_mut_obj_add_uint(d, p, "__document", t->document) &&
             yyjson_mut_obj_add_uint(d, p, "__revision", t->revision) &&
             yyjson_mut_obj_add_uint(d, p, "__layer", t->layer) &&
             yyjson_mut_obj_add_uint(d, p, "__color", pc_engine_targeted_color(s)) &&
             yyjson_mut_obj_add_bool(d, p, "__mask", pc_engine_targeted_mask(s)) &&
             yyjson_mut_obj_add_bool(d, p, "__channel", channel) &&
             yyjson_mut_obj_add_bool(d, p, "__quick", quick) &&
             yyjson_mut_obj_add_uint(d, p, "__alpha", alpha);
    }
    if (!ok) {
        yyjson_mut_doc_free(d);
        return NULL;
    }
    yyjson_mut_doc_set_root(d, p);
    return d;
}
static pc_status reply(pc_ui_shell_app *app, pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL, *state = NULL;
    pc_status status = p ? pc_ui_transform_to_json(&pc_ui_shell_app_state(app)->transform, d, &state, err)
                         : pc_fail(err, PC_ERR_NOMEM, "transform reply");
    if (status == PC_OK && !yyjson_mut_obj_add_val(d, p, "transform", state))
        status = pc_fail(err, PC_ERR_NOMEM, "transform reply");
    if (status == PC_OK)
        status = pc_json_value_from_mut(p, out, err);
    yyjson_mut_doc_free(d);
    return status;
}
static void stop(pc_ui_shell_app *app)
{
    pc_ui_transform_runtime *r = pc_ui_shell_transform_runtime(app);
    pc_ui_shell_app_state(app)->transform.active = false;
    r->dragging = false;
    (void)pc_ui_filter_preview_update(&r->preview, pc_ui_shell_app_session(app), NULL, NULL);
    pc_ui_shell_app_changed(app);
}
void pc_ui_shell_transform_fini(pc_ui_transform_runtime *r)
{
    pc_ui_filter_preview_fini(&r->preview);
    pc_doc_arc_release(r->committing_preview);
    memset(r, 0, sizeof *r);
}
static void push(pc_ui_transform_frame *steps, size_t *count, const pc_ui_transform_frame *frame)
{
    if (*count == 128) {
        memmove(steps, steps + 1, 127 * sizeof *steps);
        *count = 127;
    }
    steps[(*count)++] = *frame;
}
static bool same_pose(const pc_ui_transform_frame *a, const pc_ui_transform_frame *b)
{
    return !memcmp(a->quad, b->quad, sizeof a->quad) && !memcmp(a->pivot, b->pivot, sizeof a->pivot);
}
static void settle(pc_ui_shell_app *app, const pc_ui_transform_frame *before)
{
    pc_ui_transform_runtime *r = pc_ui_shell_transform_runtime(app);
    if (!same_pose(before, &pc_ui_shell_app_state(app)->transform.frame)) {
        push(r->undo, &r->nundo, before);
        r->nredo = 0;
        pc_ui_shell_app_state(app)->transform.can_undo = true;
        pc_ui_shell_app_state(app)->transform.can_redo = false;
        pc_ui_shell_app_changed(app);
    }
}
static pc_status begin(pc_ui_shell_app *app, const char *command, pc_json_value *out, pc_error *err)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_transform_runtime *r = pc_ui_shell_transform_runtime(app);
    pc_engine_session *s = pc_ui_shell_app_session(app);
    if (ui->n_dialogs || pc_ui_shell_jobs_locked(app))
        return pc_fail(err, PC_ERR_INVALID, "finish the dialog or document job before transforming");
    if (ui->transform.active) {
        if (!strcmp(command, "edit.freeTransformCopy"))
            return pc_fail(err, PC_ERR_INVALID, "finish the transform first");
        if (ui->transform.type_layer &&
            (!strcmp(command, "edit.transform.distort") || !strcmp(command, "edit.transform.perspective")))
            return pc_fail(err, PC_ERR_INVALID, "editable type cannot be distorted");
        ui->transform.mode = pc_ui_transform_mode_for_command(command);
        return reply(app, out, err);
    }
    pc_json_value ignored = PC_JSON_VALUE_INIT;
    pc_status status = PC_OK;
    if (ui->has_pen)
        status = pc_ui_pen_commit(s, ui, false, &ignored, err);
    if (status == PC_OK && ui->has_text_edit)
        status = pc_ui_type_commit(s, ui, 0, &ignored, err);
    if (status == PC_OK && pc_engine_floating_offset(s, NULL))
        status = pc_engine_session_execute_json(s, "select.drop", "{}", &ignored, err);
    pc_json_value_fini(&ignored);
    if (status != PC_OK)
        return status;
    const pc_engine_doc_state *state = pc_engine_session_active(s);
    if (!state)
        return pc_fail(err, PC_ERR_INVALID, "no document to transform");
    if (state->nselected > 4096)
        return pc_fail(err, PC_ERR_LIMIT, "too many selected layers");
    pc_ui_transform next = {.active = true,
                            .document = pc_doc_arc_get(state->doc)->id,
                            .revision = state->revision,
                            .layer = state->has_active_layer ? state->active_layer : 0,
                            .selection = !strcmp(command, "select.transformSelection"),
                            .copy = !strcmp(command, "edit.freeTransformCopy"),
                            .mode = pc_ui_transform_mode_for_command(command)};
    pc_json_value interpolation = PC_JSON_VALUE_INIT;
    (void)pc_engine_prefs_get(s, "general.imageInterpolation", &interpolation, NULL);
    const char *preferred = pc_ui_resample_preference(yyjson_mut_get_str(pc_json_value_root(&interpolation)));
    (void)snprintf(next.interpolation, sizeof next.interpolation, "%s",
                   !strcmp(preferred, "nearest") || !strcmp(preferred, "bilinear") ? preferred : "bicubic");
    pc_json_value_fini(&interpolation);
    next.target = target(app, &next.alpha);
    pc_rect bounds;
    double path_bounds[4] = {0};
    if (!next.selection && next.target == 0 &&
        (ui->tool == PC_TOOL_PATH_SELECTION || ui->tool == PC_TOOL_DIRECT_SELECTION)) {
        const char *name = ui->selected_path     ? ui->selected_path
                           : ui->direct.name.len ? pc_str_c(&ui->direct.name)
                                                 : "work";
        const pc_path *path = path_for(pc_doc_arc_get(state->doc), name, next.layer);
        if (path && pc_path_control_bounds(path, path_bounds)) {
            if (strlen(name) >= sizeof next.path_name)
                return pc_fail(err, PC_ERR_LIMIT, "path name too long");
            if (!strcmp(name, "layer") && pc_engine_transform_locked(pc_doc_arc_get(state->doc), next.layer))
                return pc_fail(err, PC_ERR_INVALID, "the path layer is locked");
            next.path = true;
            memcpy(next.path_name, name, strlen(name) + 1);
            for (size_t a = 0; a < 2; a++)
                if (path_bounds[a + 2] - path_bounds[a] < 1)
                    path_bounds[a + 2] = path_bounds[a] + 1;
        }
    }
    if (next.path) {
        for (size_t i = 0; i < 4; i++)
            if (!isfinite(path_bounds[i]) || fabs(path_bounds[i]) > 1000000)
                return pc_fail(err, PC_ERR_LIMIT, "path frame exceeds the coordinate budget");
        bounds = pc_rect_make((int32_t)floor(path_bounds[0]), (int32_t)floor(path_bounds[1]),
                              (int32_t)ceil(path_bounds[2]), (int32_t)ceil(path_bounds[3]));
    } else if (next.selection) {
        const pc_document *doc = pc_doc_arc_get(state->doc);
        bounds = doc->has_selection ? pc_surface_content_bounds(&doc->selection) : PC_RECT_EMPTY;
        if (pc_rect_is_empty(bounds))
            return pc_fail(err, PC_ERR_INVALID, "no selection to transform");
        memcpy(next.interpolation, "bilinear", 9);
    } else {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
        yyjson_doc *parsed = NULL;
        pc_buf json = PC_BUF_INIT;
        status = p && put_target(d, p, &next) ? pc_json_write(p, 0, &json, err)
                                              : pc_fail(err, PC_ERR_NOMEM, "transform target");
        if (status == PC_OK)
            status = pc_json_parse(pc_buf_cstr(&json), json.len, &parsed, err);
        if (status == PC_OK)
            status =
                pc_engine_transform_frame(s, yyjson_doc_get_root(parsed), &bounds, &next.type_layer, err);
        yyjson_doc_free(parsed);
        yyjson_mut_doc_free(d);
        pc_buf_fini(&json);
        if (status != PC_OK)
            return status;
        if (next.type_layer && (next.mode == PC_TRANSFORM_DISTORT || next.mode == PC_TRANSFORM_PERSPECTIVE))
            return pc_fail(err, PC_ERR_INVALID, "editable type cannot be distorted");
    }
    if (next.copy && (next.path || next.target != 0))
        return pc_fail(err, PC_ERR_INVALID, "transform a copy needs layer pixels");
    if (bounds.x0 < -1000000 || bounds.y0 < -1000000 || bounds.x1 > 1000000 || bounds.y1 > 1000000)
        return pc_fail(err, PC_ERR_LIMIT, "transform frame exceeds the coordinate budget");
    next.session = pc_ui_state_alloc_id(ui);
    if (!next.session)
        return pc_fail(err, PC_ERR_LIMIT, "transform session identifiers exhausted");
    double rect[4] = {bounds.x0, bounds.y0, bounds.x1, bounds.y1};
    pc_ui_transform_frame_rect(&next.frame, next.path ? path_bounds : rect);
    r->session = next.session;
    r->tool = ui->tool;
    r->dragging = r->committing = false;
    r->nundo = r->nredo = 0;
    r->numeric_edit = false;
    r->reference = 9;
    r->nselected = state->nselected;
    if (state->nselected)
        memcpy(r->selected, state->selected_layers, state->nselected * sizeof *r->selected);
    pc_doc_arc_release(r->committing_preview);
    r->committing_preview = NULL;
    pc_ui_shell_cancel_canvas_gesture(app);
    ui->transform = next;
    pc_ui_shell_app_changed(app);
    return reply(app, out, err);
}
static pc_status commit(pc_ui_shell_app *app, pc_json_value *out, bool background, pc_error *err)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_transform_runtime *r = pc_ui_shell_transform_runtime(app);
    if (!valid_scope(app))
        return pc_fail(err, PC_ERR_INVALID, "the transform target changed");
    pc_algo_homography h;
    if (!ui->transform.copy &&
        pc_algo_homography_rect_to_quad(ui->transform.frame.rect,
                                        (const double (*)[2])ui->transform.frame.quad, &h) &&
        pc_algo_homography_is_identity(&h)) {
        stop(app);
        return reply(app, out, err);
    }
    uint64_t generation = 0;
    (void)pc_ui_shell_transform_preview(app, &generation);
    yyjson_mut_doc *d = parameters(app, false);
    if (!d)
        return pc_fail(err, PC_ERR_NOMEM, "transform parameters");
    pc_buf json = PC_BUF_INIT;
    yyjson_doc *parsed = NULL;
    pc_status status = pc_json_write(yyjson_mut_doc_get_root(d), 0, &json, err);
    if (status == PC_OK)
        status = pc_json_parse(pc_buf_cstr(&json), json.len, &parsed, err);
    if (status == PC_OK)
        status = background
                     ? pc_ui_shell_jobs_execute(app, engine_command(&ui->transform),
                                                yyjson_doc_get_root(parsed), out, err)
                     : pc_engine_session_execute(pc_ui_shell_app_session(app), engine_command(&ui->transform),
                                                 yyjson_doc_get_root(parsed), out, err);
    if (status == PC_OK) {
        r->committing = yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(out), "job")) != 0;
        pc_doc_arc_release(r->committing_preview);
        r->committing_preview = r->committing ? pc_doc_arc_retain(r->preview.result) : NULL;
        stop(app);
    }
    pc_buf_fini(&json);
    yyjson_doc_free(parsed);
    yyjson_mut_doc_free(d);
    return status;
}
static bool start_command(const char *id)
{
    return !strcmp(id, "edit.freeTransformCopy") || !strcmp(id, "edit.freeTransform") ||
           !strcmp(id, "edit.transform.scale") || !strcmp(id, "edit.transform.rotate") ||
           !strcmp(id, "edit.transform.skew") || !strcmp(id, "edit.transform.distort") ||
           !strcmp(id, "edit.transform.perspective") || !strcmp(id, "select.transformSelection");
}
pc_status pc_ui_shell_transform_command(pc_ui_shell_app *app, const char *id, yyjson_val *params,
                                        pc_json_value *out, pc_error *err)
{
    if (start_command(id) && (!params || !pc_engine_command_find(id)))
        return begin(app, id, out, err);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_transform_runtime *r = pc_ui_shell_transform_runtime(app);
    if (!ui->transform.active)
        return PC_ERR_NOT_FOUND;
    if (r->numeric_edit) {
        settle(app, &r->numeric_before);
        r->numeric_edit = false;
    }
    if (!strcmp(id, "ui.transform.cancel")) {
        stop(app);
        return reply(app, out, err);
    }
    if (!strcmp(id, "ui.transform.commit"))
        return commit(app, out, true, err);
    if (!strcmp(id, "edit.undo") || !strcmp(id, "edit.redo")) {
        bool redo = !strcmp(id, "edit.redo");
        size_t *from_count = redo ? &r->nredo : &r->nundo, *to_count = redo ? &r->nundo : &r->nredo;
        pc_ui_transform_frame *from = redo ? r->redo : r->undo, *to = redo ? r->undo : r->redo;
        if (!r->dragging && *from_count) {
            push(to, to_count, &ui->transform.frame);
            ui->transform.frame = from[--*from_count];
            ui->transform.can_undo = r->nundo != 0;
            ui->transform.can_redo = r->nredo != 0;
            pc_ui_shell_app_changed(app);
        }
        return reply(app, out, err);
    }
    if (!strncmp(id, "edit.transform.", 15)) {
        const char *preset = id + 15;
        double a = 1, b = 0, c = 0, d = 1;
        bool recognized = true;
        if (!strcmp(preset, "rotate180"))
            a = d = -1;
        else if (!strcmp(preset, "rotate90Cw")) {
            a = d = 0;
            b = -1;
            c = 1;
        } else if (!strcmp(preset, "rotate90Ccw")) {
            a = d = 0;
            b = 1;
            c = -1;
        } else if (!strcmp(preset, "flipHorizontal"))
            a = -1;
        else if (!strcmp(preset, "flipVertical"))
            d = -1;
        else
            recognized = false;
        if (recognized) {
            if (!valid_scope(app)) {
                stop(app);
                return pc_fail(err, PC_ERR_INVALID, "the transform target changed");
            }
            pc_ui_transform_frame before = ui->transform.frame;
            for (size_t i = 0; i < 4; i++) {
                double x = before.quad[i][0] - before.pivot[0], y = before.quad[i][1] - before.pivot[1];
                ui->transform.frame.quad[i][0] = before.pivot[0] + a * x + b * y;
                ui->transform.frame.quad[i][1] = before.pivot[1] + c * x + d * y;
            }
            settle(app, &before);
            return reply(app, out, err);
        }
    }
    if (!strcmp(id, "edit.toggleLastState"))
        return pc_fail(err, PC_ERR_INVALID, "commit or cancel the transform first");
    pc_ui_tool tool;
    bool leaving_tool = pc_ui_tool_from_select_id(id, &tool) && tool != r->tool;
    const pc_engine_command *spec = pc_engine_command_find(id);
    bool editing = spec && spec->journal && strncmp(id, "tools.", 6) && strncmp(id, "prefs.", 6) &&
                   strncmp(id, "view.", 5) && strncmp(id, "jobs.", 5);
    if (!ui->n_dialogs && (leaving_tool || editing || !strncmp(id, "window.document.", 16))) {
        pc_json_value ignored = PC_JSON_VALUE_INIT;
        pc_status status = commit(app, &ignored, false, err);
        pc_json_value_fini(&ignored);
        if (status != PC_OK)
            return status;
    }
    return PC_ERR_NOT_FOUND;
}
pc_status pc_ui_shell_transform_pointer(pc_ui_shell_app *app, pc_ui_pointer_kind kind, pc_ui_tool_point point,
                                        unsigned modifiers, pc_json_value *out, pc_error *err)
{
    pc_ui_transform *t = &pc_ui_shell_app_state(app)->transform;
    pc_ui_transform_runtime *r = pc_ui_shell_transform_runtime(app);
    if (!t->active) {
        pc_ui_transform_frame frame;
        pc_ui_view *view = pc_ui_canvas_active_view(app);
        double p[2] = {point.x, point.y}, zoom = view ? view->zoom / pc_ui_shell_app_ui_scale(app) : 1;
        if (kind != PC_UI_POINTER_DOWN || !pc_ui_shell_transform_controls_frame(app, &frame) ||
            !pc_ui_shell_transform_controls_hit(&frame, p, zoom))
            return PC_ERR_NOT_FOUND;
        pc_status status = begin(app, "edit.freeTransform", out, err);
        if (status != PC_OK)
            return status;
    }
    if (!valid_scope(app)) {
        stop(app);
        return pc_fail(err, PC_ERR_INVALID, "the transform target changed");
    }
    if (pc_ui_shell_app_state(app)->n_dialogs)
        return pc_fail(err, PC_ERR_INVALID, "finish the dialog before transforming");
    if (!isfinite(point.x) || !isfinite(point.y) || fabsf(point.x) > 1000000 || fabsf(point.y) > 1000000) {
        if (kind == PC_UI_POINTER_UP || kind == PC_UI_POINTER_CANCEL) {
            if (r->dragging)
                t->frame = r->drag.before;
            r->dragging = false;
        }
        return pc_fail(err, PC_ERR_INVALID, "invalid transform pointer");
    }
    if (kind == PC_UI_POINTER_DOWN && r->numeric_edit) {
        settle(app, &r->numeric_before);
        r->numeric_edit = false;
    }
    double p[2] = {point.x, point.y};
    pc_ui_canvas_xform xf;
    double tolerance = pc_ui_shell_app_canvas_mapping(app, &xf) ? 12 / fmax(.01, xf.zoom) : 12;
    bool shift = (modifiers & PC_UI_MOD_SHIFT) != 0, alt = (modifiers & PC_UI_MOD_ALT) != 0,
         command = (modifiers & PC_UI_MOD_COMMAND) != 0;
    if (kind == PC_UI_POINTER_CANCEL) {
        if (r->dragging)
            t->frame = r->drag.before;
        r->dragging = false;
    } else if (kind == PC_UI_POINTER_DOWN) {
        pc_ui_transform_hit hit = pc_ui_transform_hit_test(&t->frame, p, tolerance);
        pc_ui_transform_frame before = t->frame;
        if (alt && (hit.part == PC_TRANSFORM_INSIDE || hit.part == PC_TRANSFORM_OUTSIDE)) {
            memcpy(t->frame.pivot, p, sizeof p);
            hit = (pc_ui_transform_hit){PC_TRANSFORM_PIVOT, 0};
        }
        r->drag = (pc_ui_transform_drag){.hit = hit, .before = before};
        memcpy(r->drag.start, p, sizeof p);
        r->dragging = true;
    } else if (r->dragging) {
        bool finish = kind == PC_UI_POINTER_UP;
        if (finish && r->drag.hit.part == PC_TRANSFORM_OUTSIDE &&
            hypot(p[0] - r->drag.start[0], p[1] - r->drag.start[1]) <= 2 * tolerance / 12) {
            r->dragging = false;
            return commit(app, out, true, err);
        }
        pc_json_value value = PC_JSON_VALUE_INIT;
        bool legacy = false;
        if (pc_engine_prefs_get(pc_ui_shell_app_session(app), "general.useLegacyFreeTransform", &value,
                                NULL) == PC_OK)
            legacy = yyjson_mut_get_bool(pc_json_value_root(&value));
        pc_json_value_fini(&value);
        (void)pc_ui_transform_apply(&t->frame, &r->drag, p, t->mode, t->type_layer, shift, alt, command,
                                    legacy, pc_ui_shell_app_state(app)->transform_unlinked);
        if (finish) {
            r->dragging = false;
            settle(app, &r->drag.before);
        }
    }
    return reply(app, out, err);
}
bool pc_ui_shell_transform_canvas(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, const SDL_Event *event,
                                  float factor)
{
    if (!pc_ui_shell_app_state(app)->transform.active || pc_ui_shell_app_state(app)->n_dialogs)
        return false;
    bool motion = event->type == SDL_EVENT_MOUSE_MOTION;
    bool down = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    if (!motion && event->type != SDL_EVENT_MOUSE_BUTTON_DOWN && event->type != SDL_EVENT_MOUSE_BUTTON_UP)
        return false;
    if (!motion && event->button.button != SDL_BUTTON_LEFT)
        return false;
    const pc_ui_transform_runtime *r = pc_ui_shell_transform_runtime(app);
    float x = (motion ? event->motion.x : event->button.x) * factor,
          y = (motion ? event->motion.y : event->button.y) * factor;
    if (!r->dragging &&
        (!down || x < xf->x || y < xf->y || x >= xf->x + xf->width || y >= xf->y + xf->height))
        return false;
    float point[2];
    if (pc_ui_canvas_to_doc(xf, x, y, point) != PC_OK)
        return true;
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status status =
        pc_ui_shell_app_pointer(app,
                                down     ? PC_UI_POINTER_DOWN
                                : motion ? PC_UI_POINTER_MOVE
                                         : PC_UI_POINTER_UP,
                                (pc_ui_tool_point){.x = point[0], .y = point[1], .pressure = 1},
                                pc_ui_shell_input_modifiers(app), &out, &err);
    if (status != PC_OK)
        pc_ui_shell_report(app, "Free Transform", err.msg, true);
    pc_json_value_fini(&out);
    return true;
}
bool pc_ui_shell_transform_key(pc_ui_shell_app *app, const SDL_Event *event)
{
    pc_ui_transform *t = &pc_ui_shell_app_state(app)->transform;
    pc_ui_transform_runtime *r = pc_ui_shell_transform_runtime(app);
    if (!t->active)
        return false;
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        if (r->dragging)
            t->frame = r->drag.before;
        r->dragging = false;
        return false;
    }
    if (event->type != SDL_EVENT_KEY_DOWN || pc_ui_shell_app_state(app)->n_dialogs ||
        pc_ui_menu_nav_is_open(pc_ui_shell_app_nav(app)))
        return false;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    for (const struct nk_window *window = ctx ? ctx->begin : NULL; window; window = window->next)
        if (!(window->flags & (NK_WINDOW_HIDDEN | NK_WINDOW_CLOSED)) && window->popup.active)
            return false;
    if (event->key.key != SDLK_ESCAPE && pc_ui_shell_keyboard_captured(app))
        return false;
    if (event->key.key == SDLK_ESCAPE || event->key.key == SDLK_RETURN || event->key.key == SDLK_KP_ENTER) {
        if (!event->key.repeat) {
            pc_json_value out = PC_JSON_VALUE_INIT;
            pc_error err = {0};
            pc_status status =
                event->key.key == SDLK_ESCAPE
                    ? pc_ui_shell_transform_command(app, "ui.transform.cancel", NULL, &out, &err)
                    : commit(app, &out, true, &err);
            if (status != PC_OK)
                pc_ui_shell_report(app, "Free Transform", err.msg, true);
            pc_json_value_fini(&out);
        }
        return true;
    }
    if (pc_ui_shell_keyboard_captured(app) || r->dragging)
        return false;
    double dx = event->key.key == SDLK_LEFT    ? -1
                : event->key.key == SDLK_RIGHT ? 1
                                               : 0,
           dy = event->key.key == SDLK_UP     ? -1
                : event->key.key == SDLK_DOWN ? 1
                                              : 0;
    if (dx == 0 && dy == 0)
        return false;
    if (!valid_scope(app)) {
        stop(app);
        return true;
    }
    double step = (event->key.mod & SDL_KMOD_SHIFT) ? 10 : 1;
    pc_ui_transform_frame before = t->frame;
    for (size_t i = 0; i < 4; i++) {
        t->frame.quad[i][0] += dx * step;
        t->frame.quad[i][1] += dy * step;
    }
    t->frame.pivot[0] += dx * step;
    t->frame.pivot[1] += dy * step;
    settle(app, &before);
    return true;
}
pc_doc_arc *pc_ui_shell_transform_preview(pc_ui_shell_app *app, uint64_t *generation)
{
    pc_ui_transform *t = &pc_ui_shell_app_state(app)->transform;
    pc_ui_transform_runtime *r = pc_ui_shell_transform_runtime(app);
    pc_engine_session *s = pc_ui_shell_app_session(app);
    if (r->committing) {
        const pc_engine_doc_state *state = pc_engine_session_active(s);
        if (pc_engine_session_has_jobs(s) && state && pc_doc_arc_get(state->doc)->id == t->document &&
            state->revision == t->revision) {
            *generation = r->preview.generation;
            return r->committing_preview;
        }
        r->committing = false;
        pc_doc_arc_release(r->committing_preview);
        r->committing_preview = NULL;
    }
    if (!t->active) {
        (void)pc_ui_filter_preview_update(&r->preview, s, NULL, NULL);
        return NULL;
    }
    if (!valid_scope(app)) {
        stop(app);
        pc_ui_shell_report(app, "Free Transform",
                           "The document or target changed; the transform was cancelled.", true);
        return NULL;
    }
    pc_algo_homography h;
    if (!t->copy && pc_algo_homography_rect_to_quad(t->frame.rect, (const double (*)[2])t->frame.quad, &h) &&
        pc_algo_homography_is_identity(&h)) {
        (void)pc_ui_filter_preview_update(&r->preview, s, NULL, NULL);
        return NULL;
    }
    yyjson_mut_doc *fields = parameters(app, true);
    if (!fields)
        return NULL;
    pc_status status = pc_ui_filter_preview_update(&r->preview, s, yyjson_mut_doc_get_root(fields), NULL);
    yyjson_mut_doc_free(fields);
    if (status != PC_OK)
        return NULL;
    *generation = r->preview.generation;
    return r->preview.result;
}
void pc_ui_shell_transform_draw(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf)
{
    const pc_ui_transform *t = &pc_ui_shell_app_state(app)->transform;
    if (!t->active || !valid_scope(app))
        return;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    float points[4][2], pivot[2];
    for (size_t i = 0; i < 4; i++)
        if (pc_ui_canvas_to_screen(xf, (float)t->frame.quad[i][0], (float)t->frame.quad[i][1], points[i]) !=
            PC_OK)
            return;
    if (pc_ui_canvas_to_screen(xf, (float)t->frame.pivot[0], (float)t->frame.pivot[1], pivot) != PC_OK)
        return;
    struct nk_color color = pc_ui_nk_color(pc_ui_shell_app_tokens(app)->accent_border);
    for (size_t i = 0; i < 4; i++) {
        size_t next = (i + 1) % 4;
        pc_ui_canvas_ink_line(canvas, xf, points[i][0], points[i][1], points[next][0], points[next][1], 1,
                              color);
        float handles[2][2] = {{points[i][0], points[i][1]},
                               {(points[i][0] + points[next][0]) / 2, (points[i][1] + points[next][1]) / 2}};
        for (size_t h = 0; h < 2; h++) {
            if (!pc_ui_canvas_ink_visible(xf, handles[h][0], handles[h][1], 8))
                continue;
            struct nk_rect rect = nk_rect(handles[h][0] - 3, handles[h][1] - 3, 6, 6);
            nk_fill_rect(canvas, rect, 0, pc_ui_nk_color(pc_ui_shell_app_tokens(app)->card));
            nk_stroke_rect(canvas, rect, 0, 1, color);
            char id[80];
            (void)snprintf(id, sizeof id, "transform.%s.%zu", h ? "edge" : "corner", i);
            pc_ui_shell_widget_mapping(app, id, nk_rect(rect.x - 5, rect.y - 5, 16, 16));
        }
    }
    if (!pc_ui_canvas_ink_visible(xf, pivot[0], pivot[1], 10))
        return;
    nk_stroke_circle(canvas, nk_rect(pivot[0] - 4, pivot[1] - 4, 8, 8), 1, color);
    pc_ui_canvas_ink_line(canvas, xf, pivot[0] - 7, pivot[1], pivot[0] + 7, pivot[1], 1, color);
    pc_ui_canvas_ink_line(canvas, xf, pivot[0], pivot[1] - 7, pivot[0], pivot[1] + 7, 1, color);
    pc_ui_shell_widget_mapping(app, "transform.pivot", nk_rect(pivot[0] - 7, pivot[1] - 7, 14, 14));
}
void pc_ui_shell_transform_options(pc_ui_shell_app *app)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_ui_transform *t = &pc_ui_shell_app_state(app)->transform;
    nk_layout_row_begin(ctx, NK_STATIC, 24, 7);
    nk_layout_row_push(ctx, 110);
    nk_label(ctx, pc_i18n_t("Free Transform"), NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 100);
    const char *modes[] = {"Free", "Rotate", "Skew", "Distort", "Perspective"};
    int mode = nk_combo(ctx, modes, 5, (int)t->mode, 24, nk_vec2(200, 180));
    if (mode >= 0 && mode < 5 && (!t->type_layer || mode < PC_TRANSFORM_DISTORT))
        t->mode = (pc_ui_transform_mode)mode;
    nk_layout_row_push(ctx, 180);
    nk_bool linked = !pc_ui_shell_app_state(app)->transform_unlinked;
    if (nk_checkbox_label(ctx, pc_i18n_t("Maintain aspect ratio"), &linked))
        pc_ui_shell_app_state(app)->transform_unlinked = !linked;
    nk_layout_row_push(ctx, 120);
    const char *interpolations[] = {"bicubic", "bilinear", "nearest"};
    int current = !strcmp(t->interpolation, "bilinear") ? 1 : !strcmp(t->interpolation, "nearest") ? 2 : 0;
    int next = nk_combo(ctx, interpolations, 3, current, 24, nk_vec2(180, 130));
    if (next >= 0 && next < 3 && next != current)
        (void)snprintf(t->interpolation, sizeof t->interpolation, "%s", interpolations[next]);
    nk_layout_row_push(ctx, 60);
    pc_ui_shell_widget_mapping(app, "ui.transform.commit", nk_widget_bounds(ctx));
    if (nk_button_label(ctx, pc_i18n_t("Apply")))
        pc_ui_shell_queue(app, "ui.transform.commit", NULL);
    nk_layout_row_push(ctx, 60);
    pc_ui_shell_widget_mapping(app, "ui.transform.cancel", nk_widget_bounds(ctx));
    if (nk_button_label(ctx, pc_i18n_t("Cancel")))
        pc_ui_shell_queue(app, "ui.transform.cancel", NULL);
    nk_layout_row_end(ctx);
    pc_ui_transform_runtime *r = pc_ui_shell_transform_runtime(app);
    nk_layout_row_begin(ctx, NK_STATIC, 24, 8);
    nk_layout_row_push(ctx, 100);
    const char *references[] = {"Top left", "Top",         "Top right", "Left",         "Center",
                                "Right",    "Bottom left", "Bottom",    "Bottom right", "Pivot"};
    int ref = nk_combo(ctx, references, 10, (int)r->reference, 24, nk_vec2(180, 280));
    if (ref >= 0 && ref < 10)
        r->reference = (unsigned)ref;
    const char *labels[] = {"X", "Y", "W", "H", "Angle", "H skew", "V skew"};
    for (unsigned i = 0; i < 7; i++) {
        double values[7];
        pc_ui_transform_measure(&t->frame, r->reference, values);
        double value = values[i];
        nk_layout_row_push(ctx, i < 4 ? 90 : 120);
        char id[64];
        (void)snprintf(id, sizeof id, "transform.numeric.%u", i);
        pc_ui_shell_widget_mapping(app, id, nk_widget_bounds(ctx));
        nk_property_double(ctx, pc_i18n_t(labels[i]),
                           i == 2 || i == 3 ? .01
                           : i > 4          ? -85
                                            : -1000000,
                           &value, i > 4 ? 85 : 1000000, i < 4 ? 1 : .1, .1f);
        if (value != values[i]) {
            pc_ui_transform_frame before = t->frame;
            if (pc_ui_transform_numeric(&t->frame, r->reference, i, value,
                                        !pc_ui_shell_app_state(app)->transform_unlinked)) {
                if (!r->numeric_edit)
                    r->numeric_before = before;
                r->numeric_edit = true;
            }
        }
    }
    nk_layout_row_end(ctx);
    if (r->numeric_edit && !ctx->input.mouse.buttons[NK_BUTTON_LEFT].down &&
        !pc_ui_shell_keyboard_captured(app)) {
        settle(app, &r->numeric_before);
        r->numeric_edit = false;
    }
}
SDL_SystemCursor pc_ui_shell_frame_cursor(const pc_ui_transform_frame *frame, bool rotate,
                                          const pc_ui_canvas_xform *xf, float x, float y)
{
    float point[2];
    if (pc_ui_canvas_to_doc(xf, x, y, point) != PC_OK)
        return SDL_SYSTEM_CURSOR_DEFAULT;
    double p[2] = {point[0], point[1]};
    pc_ui_transform_hit hit = pc_ui_transform_hit_test(frame, p, 12 / fmax(.01, xf->zoom));
    if (hit.part == PC_TRANSFORM_INSIDE)
        return SDL_SYSTEM_CURSOR_MOVE;
    if (hit.part == PC_TRANSFORM_PIVOT || hit.part == PC_TRANSFORM_OUTSIDE || rotate)
        return SDL_SYSTEM_CURSOR_CROSSHAIR;
    size_t index = hit.index % 4;
    double dx, dy;
    if (hit.part == PC_TRANSFORM_CORNER) {
        dx = frame->quad[index][0] - frame->pivot[0];
        dy = frame->quad[index][1] - frame->pivot[1];
    } else {
        dx = -(frame->quad[(index + 1) % 4][1] - frame->quad[index][1]);
        dy = frame->quad[(index + 1) % 4][0] - frame->quad[index][0];
    }
    float p0[2], p1[2];
    if (pc_ui_canvas_to_screen(xf, 0, 0, p0) != PC_OK ||
        pc_ui_canvas_to_screen(xf, (float)dx, (float)dy, p1) != PC_OK)
        return SDL_SYSTEM_CURSOR_DEFAULT;
    double angle = atan2(p1[1] - p0[1], p1[0] - p0[0]);
    int direction = (int)lround(angle / (3.14159265358979323846 / 4));
    direction = (direction % 4 + 4) % 4;
    const SDL_SystemCursor cursors[] = {SDL_SYSTEM_CURSOR_EW_RESIZE, SDL_SYSTEM_CURSOR_NWSE_RESIZE,
                                        SDL_SYSTEM_CURSOR_NS_RESIZE, SDL_SYSTEM_CURSOR_NESW_RESIZE};
    return cursors[direction];
}

SDL_SystemCursor pc_ui_shell_transform_cursor(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, float x,
                                              float y)
{
    const pc_ui_transform *t = &pc_ui_shell_app_state(app)->transform;
    return pc_ui_shell_frame_cursor(&t->frame, t->mode == PC_TRANSFORM_ROTATE, xf, x, y);
}

bool pc_ui_shell_transform_controls_frame(pc_ui_shell_app *app, pc_ui_transform_frame *frame)
{
    const pc_ui_state *ui = pc_ui_shell_app_state(app);
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (!frame || ui->tool != PC_TOOL_MOVE || !ui->tool_options.move_show_transform || ui->transform.active ||
        ui->n_dialogs || pc_ui_shell_jobs_locked(app) || pc_ui_shell_canvas_pointer_active(app) || !state ||
        !state->has_active_layer ||
        pc_document_is_background(pc_doc_arc_get(state->doc), state->active_layer))
        return false;
    pc_rect bounds;
    bool type;
    if (pc_engine_transform_frame(pc_ui_shell_app_session(app), NULL, &bounds, &type, NULL) != PC_OK ||
        pc_rect_is_empty(bounds))
        return false;
    int32_t offset[2] = {0, 0};
    (void)pc_engine_floating_offset(pc_ui_shell_app_session(app), offset);
    const double rect[4] = {(double)bounds.x0 + offset[0], (double)bounds.y0 + offset[1],
                            (double)bounds.x1 + offset[0], (double)bounds.y1 + offset[1]};
    pc_ui_transform_frame_rect(frame, rect);
    return pc_ui_transform_frame_valid(frame);
}
bool pc_ui_shell_transform_controls_hit(const pc_ui_transform_frame *frame, const double point[2],
                                        double zoom)
{
    double tolerance = 12 / fmax(.01, zoom);
    pc_ui_transform_hit hit = pc_ui_transform_hit_test(frame, point, tolerance);
    if (hit.part == PC_TRANSFORM_CORNER || hit.part == PC_TRANSFORM_EDGE)
        return true;
    return hit.part == PC_TRANSFORM_OUTSIDE && point[0] >= frame->rect[0] - tolerance &&
           point[1] >= frame->rect[1] - tolerance && point[0] <= frame->rect[2] + tolerance &&
           point[1] <= frame->rect[3] + tolerance;
}
void pc_ui_shell_transform_controls_draw(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf)
{
    pc_ui_transform_frame frame;
    if (!pc_ui_shell_transform_controls_frame(app, &frame))
        return;
    struct nk_command_buffer *canvas = nk_window_get_canvas(pc_ui_shell_app_nk(app));
    struct nk_color color = pc_ui_nk_color(pc_ui_shell_app_tokens(app)->accent_border);
    for (size_t i = 0; i < 4; i++) {
        size_t next = (i + 1) % 4;
        float a[2], b[2];
        if (pc_ui_canvas_to_screen(xf, (float)frame.quad[i][0], (float)frame.quad[i][1], a) != PC_OK ||
            pc_ui_canvas_to_screen(xf, (float)frame.quad[next][0], (float)frame.quad[next][1], b) != PC_OK)
            continue;
        pc_ui_canvas_ink_line(canvas, xf, a[0], a[1], b[0], b[1], 1, color);
        const float handles[2][2] = {{a[0], a[1]}, {(a[0] + b[0]) / 2, (a[1] + b[1]) / 2}};
        for (size_t h = 0; h < 2; h++)
            if (pc_ui_canvas_ink_visible(xf, handles[h][0], handles[h][1], 4)) {
                struct nk_rect box = nk_rect(handles[h][0] - 3, handles[h][1] - 3, 6, 6);
                nk_fill_rect(canvas, box, 0, color);
                char id[80];
                (void)snprintf(id, sizeof id, "move.transform.%s.%zu", h ? "edge" : "corner", i);
                pc_ui_shell_widget_mapping(app, id, box);
            }
    }
}
