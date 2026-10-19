#include "pen_tool.h"
#include "ui/vector_tool.h"
#include "engine/vector_cmds.h"
#include "base/mem.h"
#include "vector/vector.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
bool pc_ui_pen_tool(pc_ui_tool tool)
{
    return tool == PC_TOOL_PEN || tool == PC_TOOL_CURVATURE_PEN || tool == PC_TOOL_FREEFORM_PEN;
}
bool pc_ui_pen_drawn_with(const pc_ui_pen *pen, pc_ui_tool tool)
{
    return tool == (pen->freeform    ? PC_TOOL_FREEFORM_PEN
                    : pen->curvature ? PC_TOOL_CURVATURE_PEN
                                     : PC_TOOL_PEN);
}
void pc_ui_pen_cancel(pc_ui_state *ui)
{
    pc_ui_pen_fini(&ui->pen);
    ui->has_pen = false;
}
static pc_status changed(pc_ui_state *ui, pc_error *err)
{
    uint64_t generation = pc_ui_state_alloc_id(ui);
    if (!generation) {
        return pc_fail(err, PC_ERR_LIMIT, "pen preview generation exhausted");
    }
    ui->pen.generation = generation;
    return PC_OK;
}
static pc_status guard(pc_engine_session *session, pc_ui_state *ui, const pc_engine_doc_state **out,
                       pc_error *err)
{
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (!state || ui->n_dialogs) {
        return pc_fail(err, PC_ERR_INVALID, "pen needs an active document without a modal dialog");
    }
    pc_ui_pen *pen = &ui->pen;
    const pc_document *doc = pc_doc_arc_get(state->doc);
    if (pen->document &&
        (pen->document != doc->id || pen->revision != state->revision ||
         pen->has_layer != state->has_active_layer || (pen->has_layer && pen->layer != state->active_layer) ||
         pen->mask_target != ui->vector_mask_target)) {
        return pc_fail(err, PC_ERR_INVALID, "the pen document or target changed");
    }
    if (!pen->document) {
        pen->document = doc->id;
        pen->revision = state->revision;
        pen->layer = state->active_layer;
        pen->has_layer = state->has_active_layer;
        pen->mask_target = ui->vector_mask_target;
    }
    *out = state;
    return PC_OK;
}
static double distance(const double a[2], const double b[2])
{
    return hypot(a[0] - b[0], a[1] - b[1]);
}
static bool closing(const pc_ui_pen *pen)
{
    return pen->count >= 3 &&
           !memcmp(pen->knots[0].points[0], pen->knots[pen->count - 1].points[0], 2 * sizeof(double));
}
static pc_status append(pc_ui_pen *pen, const double point[2], pc_error *err)
{
    if (pen->count >= 4096) {
        return pc_fail(err, PC_ERR_LIMIT, "pen path exceeds 4096 knots");
    }
    double anchor[2] = {point[0], point[1]};
    pc_ui_pen_knot *knots = pc_grow_array(pen->knots, &pen->capacity, pen->count + 1, sizeof *knots);
    if (!knots) {
        return pc_fail(err, PC_ERR_NOMEM, "pen knot");
    }
    pen->knots = knots;
    pc_ui_pen_knot *knot = &pen->knots[pen->count++];
    memset(knot, 0, sizeof *knot);
    for (size_t i = 0; i < 3; i++) {
        memcpy(knot->points[i], anchor, 2 * sizeof(double));
    }
    return PC_OK;
}
static pc_status curvature(pc_ui_pen *pen, bool closed, pc_error *err)
{
    size_t n = pen->count;
    if (!n) {
        return PC_OK;
    }
    double (*anchors)[2] = pc_malloc_array(n, sizeof *anchors),
           (*points)[3][2] = pc_malloc_array(n, sizeof *points);
    bool *corners = pc_malloc_array(n, sizeof *corners);
    if (!anchors || !points || !corners) {
        free(anchors);
        free(points);
        free(corners);
        return pc_fail(err, PC_ERR_NOMEM, "curvature pen");
    }
    for (size_t i = 0; i < n; i++) {
        memcpy(anchors[i], pen->knots[i].points[0], sizeof anchors[i]);
        corners[i] = pen->knots[i].unlinked;
    }
    pc_vec_curvature_knots((const double (*)[2])anchors, n, corners, n, closed, points);
    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < 3; j++) {
            for (size_t axis = 0; axis < 2; axis++) {
                if (!isfinite(points[i][j][axis]) || fabs(points[i][j][axis]) > 1e6) {
                    free(anchors);
                    free(points);
                    free(corners);
                    return pc_fail(err, PC_ERR_INVALID, "curvature control point is out of range");
                }
            }
        }
    }
    for (size_t i = 0; i < n; i++) {
        memcpy(pen->knots[i].points, points[i], sizeof points[i]);
    }
    free(anchors);
    free(points);
    free(corners);
    return PC_OK;
}
pc_status pc_ui_pen_path(const pc_ui_pen *pen, bool closed, pc_path *out, pc_error *err)
{
    if (!pen || !out || pen->count > 4096 || (pen->count && !pen->knots)) {
        return pc_fail(err, PC_ERR_INVALID, "invalid pen path");
    }
    bool fold = closing(pen);
    size_t count = pen->count - (fold ? 1u : 0u);
    pc_path path = {0};
    path.subpaths = pc_calloc_array(1, sizeof *path.subpaths);
    if (!path.subpaths) {
        return pc_fail(err, PC_ERR_NOMEM, "pen subpath");
    }
    path.nsubpaths = 1;
    pc_subpath *sub = &path.subpaths[0];
    sub->closed = closed || fold;
    sub->knots = pc_calloc_array(count ? count : 1, sizeof *sub->knots);
    if (!sub->knots) {
        pc_path_fini(&path);
        return pc_fail(err, PC_ERR_NOMEM, "pen path knots");
    }
    sub->nknots = count;
    for (size_t i = 0; i < count; i++) {
        pc_ui_pen_knot knot = pen->knots[i];
        if (i == 0 && fold) {
            const pc_ui_pen_knot *stand = &pen->knots[pen->count - 1];
            if (distance(stand->points[0], stand->points[1]) > 0 ||
                distance(stand->points[0], stand->points[2]) > 0) {
                memcpy(knot.points, stand->points, sizeof knot.points);
                knot.unlinked = stand->unlinked;
            }
        }
        sub->knots[i] = (pc_knot){.anchor = {knot.points[0][0], knot.points[0][1]},
                                  .in_ctrl = {knot.points[1][0], knot.points[1][1]},
                                  .out_ctrl = {knot.points[2][0], knot.points[2][1]},
                                  .smooth = !knot.unlinked && (distance(knot.points[0], knot.points[1]) > 0 ||
                                                               distance(knot.points[0], knot.points[2]) > 0)};
    }
    *out = path;
    return PC_OK;
}
static pc_status execute(pc_engine_session *session, const char *id, yyjson_mut_val *params,
                         pc_json_value *out, pc_error *err)
{
    pc_buf json = PC_BUF_INIT;
    pc_status status = pc_json_write(params, 0, &json, err);
    if (status == PC_OK) {
        status = pc_engine_session_execute_json(session, id, pc_buf_cstr(&json), out, err);
    }
    pc_buf_fini(&json);
    return status;
}
static pc_status commit_path(pc_engine_session *session, pc_ui_state *ui, const pc_path *path,
                             pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *state;
    pc_status status = guard(session, ui, &state, err);
    if (status != PC_OK) {
        return status;
    }
    const pc_document *document = pc_doc_arc_get(state->doc);
    const pc_layer *layer = state->has_active_layer ? pc_document_layer(document, state->active_layer) : NULL;
    bool shape = pc_str_eq_c(&ui->tool_options.vector_mode, "shape"),
         mask = !shape && ui->vector_mask_target && layer && layer->has_vector_mask;
    pc_path merged = {0};
    const pc_path *target = path;
    if (mask) {
        status = pc_path_clone(&layer->vector_mask.path, &merged, err);
        if (status != PC_OK) {
            return status;
        }
        if (merged.nsubpaths >= 4096 || path->nsubpaths > 4096 - merged.nsubpaths) {
            pc_path_fini(&merged);
            return pc_fail(err, PC_ERR_LIMIT, "vector mask has too many subpaths");
        }
        size_t cap = merged.nsubpaths;
        pc_subpath *subs =
            pc_grow_array(merged.subpaths, &cap, merged.nsubpaths + path->nsubpaths, sizeof *subs);
        if (!subs) {
            pc_path_fini(&merged);
            return pc_fail(err, PC_ERR_NOMEM, "vector mask pen path");
        }
        merged.subpaths = subs;
        for (size_t i = 0; status == PC_OK && i < path->nsubpaths; i++) {
            pc_subpath sub = {0};
            status = pc_subpath_clone(&path->subpaths[i], &sub, err);
            if (status == PC_OK) {
                merged.subpaths[merged.nsubpaths++] = sub;
            }
        }
        if (status != PC_OK) {
            pc_path_fini(&merged);
            return status;
        }
        target = &merged;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = doc ? yyjson_mut_obj(doc) : NULL;
    bool ok = p && yyjson_mut_obj_add_val(doc, p, "path", pc_engine_path_to_json(doc, target));
    char *selected = NULL;
    if (shape) {
        ok = ok && yyjson_mut_obj_add_str(doc, p, "kind", "path");
        pc_ui_tool_options options = ui->tool_options;
        bool closed = path->nsubpaths && path->subpaths[0].closed;
        const pc_engine_tool_state *tools = pc_engine_session_tools(session);
        if (!closed) {
            options.shape_fill = false;
            options.stroke_width = fmaxf(1, options.stroke_width);
        }
        pc_json_value paint = PC_JSON_VALUE_INIT;
        const float start[2] = {0, 0}, end[2] = {1, 1};
        status = pc_ui_shape_drag_params(PC_TOOL_RECTANGLE, &options, tools->foreground,
                                         closed ? tools->background : tools->foreground, start, end, 0,
                                         &paint, err);
        if (status == PC_OK) {
            const char *keys[] = {"fill", "stroke"};
            for (size_t i = 0; ok && i < 2; i++) {
                ok = yyjson_mut_obj_add_val(
                    doc, p, keys[i],
                    yyjson_mut_val_mut_copy(doc, yyjson_mut_obj_get(pc_json_value_root(&paint), keys[i])));
            }
        }
        pc_json_value_fini(&paint);
    } else if (mask) {
        ok = ok && yyjson_mut_obj_add_uint(doc, p, "layer", layer->id);
    } else {
        ok = ok && yyjson_mut_obj_add_str(doc, p, "name", "work");
        selected = pc_strndup("work", 4);
        if (!selected) {
            status = pc_fail(err, PC_ERR_NOMEM, "selected work path");
        }
    }
    if (status == PC_OK) {
        status = ok ? execute(session,
                              shape  ? "shape.create"
                              : mask ? "layer.vectorMask.edit"
                                     : "path.set",
                              p, out, err)
                    : pc_fail(err, PC_ERR_NOMEM, "pen command");
    }
    if (status == PC_OK) {
        if (selected) {
            free(ui->selected_path);
            ui->selected_path = selected;
            selected = NULL;
        }
        if (shape) {
            ui->mask_target = ui->vector_mask_target = false;
        }
    }
    free(selected);
    pc_path_fini(&merged);
    yyjson_mut_doc_free(doc);
    return status;
}
static pc_status freeform_finish(pc_engine_session *session, pc_ui_state *ui, float zoom, pc_json_value *out,
                                 pc_error *err);
pc_status pc_ui_pen_commit(pc_engine_session *session, pc_ui_state *ui, bool closed, pc_json_value *out,
                           pc_error *err)
{
    if (!ui->has_pen) {
        return PC_OK;
    }
    if (ui->pen.freeform) {
        const pc_engine_doc_state *state;
        pc_status status = guard(session, ui, &state, err);
        if (status != PC_OK) {
            return status;
        }
        return freeform_finish(session, ui, ui->pen.zoom > 0 ? ui->pen.zoom : 1, out, err);
    }
    if (ui->pen.count < 2) {
        pc_ui_pen_cancel(ui);
        return PC_OK;
    }
    pc_status status = PC_OK;
    if (ui->pen.curvature) {
        status = curvature(&ui->pen, closed, err);
    }
    pc_path path = {0};
    if (status == PC_OK) {
        status = pc_ui_pen_path(&ui->pen, closed, &path, err);
    }
    if (status == PC_OK) {
        status = commit_path(session, ui, &path, out, err);
    }
    pc_path_fini(&path);
    if (status == PC_OK) {
        pc_ui_pen_cancel(ui);
    }
    return status;
}
static pc_status fit_run(pc_ui_pen *out, const pc_ui_pen_sample *trace, size_t count, double tolerance,
                         bool closed, bool retract_first, bool retract_last, pc_error *err)
{
    double (*points)[2] = pc_malloc_array(count ? count : 1, sizeof *points);
    if (!points) {
        return pc_fail(err, PC_ERR_NOMEM, "freeform trace");
    }
    for (size_t i = 0; i < count; i++) {
        memcpy(points[i], trace[i].point, sizeof points[i]);
    }
    pc_subpath sub = {0};
    bool fitted = false;
    pc_status status =
        pc_vec_fit_trace((const double (*)[2])points, count, tolerance, closed, &sub, &fitted, err);
    free(points);
    size_t first = out->count;
    if (status == PC_OK && fitted) {
        for (size_t i = 0; status == PC_OK && i < sub.nknots; i++) {
            const pc_knot *k = &sub.knots[i];
            double anchor[2] = {k->anchor.x, k->anchor.y};
            status = append(out, anchor, err);
            if (status == PC_OK) {
                pc_ui_pen_knot *target = &out->knots[out->count - 1];
                target->points[1][0] = k->in_ctrl.x;
                target->points[1][1] = k->in_ctrl.y;
                target->points[2][0] = k->out_ctrl.x;
                target->points[2][1] = k->out_ctrl.y;
                target->unlinked = !k->smooth;
            }
        }
    } else if (status == PC_OK && count) {
        status = append(out, trace[0].point, err);
    }
    if (status == PC_OK && first < out->count && retract_first) {
        memcpy(out->knots[first].points[1], out->knots[first].points[0], 2 * sizeof(double));
        out->knots[first].unlinked = true;
    }
    if (status == PC_OK && out->count && retract_last) {
        pc_ui_pen_knot *last = &out->knots[out->count - 1];
        memcpy(last->points[2], last->points[0], 2 * sizeof(double));
        last->unlinked = true;
    }
    pc_subpath_fini(&sub);
    return status;
}
static pc_status freeform_finish(pc_engine_session *session, pc_ui_state *ui, float zoom, pc_json_value *out,
                                 pc_error *err)
{
    const pc_ui_pen *pen = &ui->pen;
    size_t n = pen->ntrace;
    if (n < 2) {
        pc_ui_pen_cancel(ui);
        return PC_OK;
    }
    if (!isfinite(ui->tool_options.curve_fit)) {
        return pc_fail(err, PC_ERR_INVALID, "invalid freeform curve fit");
    }
    double length = 0;
    bool cuts = false;
    for (size_t i = 1; i < n; i++) {
        length += distance(pen->trace[i - 1].point, pen->trace[i].point);
        cuts = cuts || pen->trace[i].straight;
    }
    double reach = 6 / fmax(.01, zoom);
    bool closed = distance(pen->trace[0].point, pen->trace[n - 1].point) < reach && length > 4 * reach;
    pc_ui_pen fitted = {0};
    pc_status status = PC_OK;
    if (!cuts) {
        status = fit_run(&fitted, pen->trace, n, ui->tool_options.curve_fit, closed, false, false, err);
    } else {
        size_t start = 0;
        for (size_t end = 1; status == PC_OK && end <= n; end++) {
            if (end < n && !pen->trace[end].straight) {
                continue;
            }
            status = fit_run(&fitted, pen->trace + start, end - start, ui->tool_options.curve_fit, false,
                             start > 0, end < n, err);
            start = end;
        }
        closed = closed && fitted.count > 3 &&
                 distance(fitted.knots[0].points[0], fitted.knots[fitted.count - 1].points[0]) < reach;
        if (closed) {
            const pc_ui_pen_knot last = fitted.knots[--fitted.count];
            pc_ui_pen_knot *first = &fitted.knots[0];
            for (size_t axis = 0; axis < 2; axis++) {
                first->points[1][axis] = last.points[1][axis] - last.points[0][axis] + first->points[0][axis];
            }
            first->unlinked = true;
        }
    }
    pc_path path = {0};
    if (status == PC_OK && fitted.count >= 2) {
        status = pc_ui_pen_path(&fitted, closed, &path, err);
    }
    if (status == PC_OK && fitted.count >= 2) {
        status = commit_path(session, ui, &path, out, err);
    }
    pc_path_fini(&path);
    pc_ui_pen_fini(&fitted);
    if (status == PC_OK) {
        pc_ui_pen_cancel(ui);
    }
    return status;
}
static size_t nearest(const pc_ui_pen *pen, const double point[2], double tolerance)
{
    size_t index = SIZE_MAX;
    double best = tolerance;
    for (size_t i = 0; i < pen->count; i++) {
        double d = distance(pen->knots[i].points[0], point);
        if (d < best) {
            best = d;
            index = i;
        }
    }
    return index;
}
pc_status pc_ui_pen_curvature_toggle(pc_ui_state *ui, const float point[2], float zoom, pc_error *err)
{
    if (!ui->has_pen || !ui->pen.curvature || !isfinite(point[0]) || !isfinite(point[1]) || !isfinite(zoom) ||
        zoom <= 0) {
        return PC_ERR_NOT_FOUND;
    }
    double at[2] = {point[0], point[1]};
    size_t index = nearest(&ui->pen, at, 6 / fmax(.01, zoom));
    if (index == SIZE_MAX) {
        return PC_ERR_NOT_FOUND;
    }
    ui->pen.knots[index].unlinked = !ui->pen.knots[index].unlinked;
    pc_status status = curvature(&ui->pen, false, err);
    return status == PC_OK ? changed(ui, err) : status;
}
pc_status pc_ui_pen_pointer(pc_engine_session *session, pc_ui_state *ui, pc_ui_pointer_kind kind,
                            pc_ui_tool_point sample, unsigned modifiers, float zoom, pc_json_value *out,
                            pc_error *err)
{
    if (!session || !ui || !out || (unsigned)kind > PC_UI_POINTER_CANCEL) {
        return pc_fail(err, PC_ERR_INVALID, "missing pen context");
    }
    if (kind == PC_UI_POINTER_CANCEL) {
        pc_ui_pen_cancel(ui);
        return PC_OK;
    }
    if (!pc_ui_pen_tool(ui->tool) && (!ui->has_pen || !ui->pen.dragging)) {
        return PC_ERR_NOT_FOUND;
    }
    if (!isfinite(sample.x) || !isfinite(sample.y) || fabsf(sample.x) > 1e6 || fabsf(sample.y) > 1e6 ||
        !isfinite(zoom) || zoom <= 0) {
        return pc_fail(err, PC_ERR_INVALID, "invalid pen pointer");
    }
    if (!ui->has_pen && kind != PC_UI_POINTER_DOWN) {
        return PC_OK;
    }
    if (ui->has_pen && !pc_ui_pen_drawn_with(&ui->pen, ui->tool) && kind == PC_UI_POINTER_DOWN) {
        pc_status status = pc_ui_pen_commit(session, ui, false, out, err);
        if (status != PC_OK) {
            return status;
        }
    }
    const pc_engine_doc_state *state;
    pc_status status = guard(session, ui, &state, err);
    if (status != PC_OK) {
        return status;
    }
    (void)state;
    bool alt = (modifiers & PC_UI_MOD_ALT) != 0;
    double point[2] = {sample.x, sample.y}, tol = 6 / fmax(.01, zoom);
    pc_ui_pen *pen = &ui->pen;
    if (!ui->has_pen) {
        ui->has_pen = true;
        pen->curvature = ui->tool == PC_TOOL_CURVATURE_PEN;
        pen->freeform = ui->tool == PC_TOOL_FREEFORM_PEN;
    }
    pen->zoom = zoom;
    memcpy(pen->hover, point, sizeof point);
    pen->has_hover = true;
    if (pen->freeform) {
        if (kind == PC_UI_POINTER_MOVE && !pen->dragging) {
            return PC_OK;
        }
        if (kind == PC_UI_POINTER_DOWN) {
            pen->dragging = true;
        }
        if (pen->dragging || kind == PC_UI_POINTER_UP) {
            bool straight = kind == PC_UI_POINTER_DOWN && alt;
            bool different =
                !pen->ntrace || distance(pen->trace[pen->ntrace - 1].point, point) > 0 || straight;
            if (different) {
                if (pen->ntrace >= 100000) {
                    return pc_fail(err, PC_ERR_LIMIT, "freeform trace exceeds 100000 samples");
                }
                pc_ui_pen_sample *trace =
                    pc_grow_array(pen->trace, &pen->trace_capacity, pen->ntrace + 1, sizeof *trace);
                if (!trace) {
                    return pc_fail(err, PC_ERR_NOMEM, "freeform pen trace");
                }
                pen->trace = trace;
                memcpy(trace[pen->ntrace].point, point, sizeof point);
                trace[pen->ntrace++].straight = straight;
            }
        }
        if (kind == PC_UI_POINTER_UP) {
            pen->dragging = false;
            if (!alt) {
                return freeform_finish(session, ui, zoom, out, err);
            }
        }
        return changed(ui, err);
    }
    if (pen->curvature) {
        if (kind == PC_UI_POINTER_MOVE && !pen->has_moving) {
            return PC_OK;
        }
        if (kind == PC_UI_POINTER_DOWN) {
            size_t hit = nearest(pen, point, tol);
            if (hit == 0 && pen->count >= 3) {
                return pc_ui_pen_commit(session, ui, true, out, err);
            }
            if (hit == SIZE_MAX) {
                status = append(pen, point, err);
                if (status != PC_OK) {
                    return status;
                }
                hit = pen->count - 1;
            }
            pen->moving = hit;
            pen->has_moving = true;
            pen->dragging = true;
        } else if (kind == PC_UI_POINTER_MOVE && pen->has_moving && pen->moving < pen->count) {
            memcpy(pen->knots[pen->moving].points[0], point, sizeof point);
        }
        if (kind == PC_UI_POINTER_UP) {
            pen->has_moving = false;
            pen->dragging = false;
        }
        status = curvature(pen, false, err);
        return status == PC_OK ? changed(ui, err) : status;
    }
    if (kind == PC_UI_POINTER_MOVE && !pen->dragging) {
        return PC_OK;
    }
    if (kind == PC_UI_POINTER_DOWN) {
        if (pen->count >= 2 && distance(pen->knots[0].points[0], point) < tol) {
            status = append(pen, pen->knots[0].points[0], err);
        } else if (pen->count) {
            pc_ui_pen_knot *last = &pen->knots[pen->count - 1];
            bool handle = alt && distance(last->points[2], last->points[0]) > 0 &&
                          distance(last->points[2], point) < tol &&
                          distance(last->points[2], point) < distance(last->points[0], point);
            if (handle || distance(last->points[0], point) < tol) {
                if (!handle) {
                    memcpy(last->points[2], last->points[0], sizeof point);
                }
                last->unlinked = true;
                pen->adjusting_last = true;
            } else {
                status = append(pen, point, err);
                pen->adjusting_last = false;
            }
        } else {
            status = append(pen, point, err);
            pen->adjusting_last = false;
        }
        if (status != PC_OK) {
            return status;
        }
        pen->dragging = true;
    } else if ((kind == PC_UI_POINTER_MOVE || kind == PC_UI_POINTER_UP) && pen->dragging && pen->count) {
        pc_ui_pen_knot *last = &pen->knots[pen->count - 1];
        if (alt && !pen->adjusting_last) {
            last->unlinked = true;
            pen->adjusting_last = true;
        }
        for (size_t axis = 0; axis < 2; axis++) {
            if (!pen->adjusting_last && fabs(2 * last->points[0][axis] - point[axis]) > 1e6) {
                return pc_fail(err, PC_ERR_INVALID, "pen control point is out of range");
            }
        }
        memcpy(last->points[2], point, sizeof point);
        if (!pen->adjusting_last) {
            for (size_t axis = 0; axis < 2; axis++) {
                last->points[1][axis] = 2 * last->points[0][axis] - point[axis];
            }
        }
    }
    if (kind == PC_UI_POINTER_UP) {
        pen->dragging = false;
        pen->adjusting_last = false;
        if (closing(pen)) {
            pc_ui_pen_knot *last = &pen->knots[pen->count - 1];
            if (distance(last->points[2], last->points[0]) < 2 / fmax(.01, zoom)) {
                memcpy(last->points[1], last->points[0], sizeof point);
                memcpy(last->points[2], last->points[0], sizeof point);
            }
            return pc_ui_pen_commit(session, ui, true, out, err);
        }
    }
    return changed(ui, err);
}
pc_status pc_ui_pen_key(pc_engine_session *session, pc_ui_state *ui, pc_ui_key key, unsigned modifiers,
                        pc_json_value *out, pc_error *err)
{
    if (!ui->has_pen) {
        return PC_ERR_NOT_FOUND;
    }
    if (!modifiers && (key == PC_KEY_ENTER || key == PC_KEY_ESCAPE)) {
        return pc_ui_pen_commit(session, ui, false, out, err);
    }
    char binding[128];
    pc_ui_shortcut shortcut;
    bool undo = pc_ui_effective_shortcut(session, "edit.undo", binding, sizeof binding) &&
                pc_ui_shortcut_parse(binding, &shortcut) &&
                pc_ui_shortcut_key_matches(&shortcut, key, modifiers);
    bool remove = !modifiers && (key == PC_KEY_BACKSPACE || key == PC_KEY_DELETE);
    if (!undo && !remove) {
        return PC_ERR_NOT_FOUND;
    }
    pc_ui_pen *pen = &ui->pen;
    if (pen->freeform) {
        if (pen->ntrace) {
            pen->ntrace--;
        }
        if (!pen->ntrace) {
            pc_ui_pen_cancel(ui);
            return PC_OK;
        }
        return changed(ui, err);
    }
    if (!pen->count) {
        return PC_ERR_NOT_FOUND;
    }
    pen->count--;
    pen->dragging = pen->has_moving = false;
    if (!pen->count) {
        pc_ui_pen_cancel(ui);
        return PC_OK;
    }
    pc_status status = pen->curvature ? curvature(pen, false, err) : PC_OK;
    return status == PC_OK ? changed(ui, err) : status;
}
