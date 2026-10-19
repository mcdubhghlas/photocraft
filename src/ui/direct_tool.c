#include "ui/direct_tool.h"
#include "ui/pen_tool.h"
#include "base/mem.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
bool pc_ui_direct_tool(pc_ui_tool tool)
{
    return tool == PC_TOOL_DIRECT_SELECTION || tool == PC_TOOL_PATH_SELECTION ||
           tool == PC_TOOL_ADD_ANCHOR_POINT || tool == PC_TOOL_DELETE_ANCHOR_POINT ||
           tool == PC_TOOL_CONVERT_POINT;
}
bool pc_ui_direct_handles(const pc_ui_state *ui, unsigned mods)
{
    return pc_ui_direct_tool(ui->tool) || (pc_ui_pen_tool(ui->tool) && !ui->has_pen &&
                                           (mods & (PC_UI_MOD_COMMAND | PC_UI_MOD_CTRL | PC_UI_MOD_ALT)));
}
const pc_path *pc_ui_direct_path(const pc_ui_direct *s, const pc_engine_doc_state *state)
{
    if (!state)
        return NULL;
    const pc_document *doc = pc_doc_arc_get(state->doc);
    if (s->document && s->document != doc->id)
        return NULL;
    if (pc_str_eq_c(&s->name, "layer")) {
        const pc_layer *layer = pc_document_layer(doc, s->layer);
        if (!layer)
            return NULL;
        return layer->content.kind == PC_LAYER_SHAPE ? &layer->content.u.shape.path
               : layer->has_vector_mask              ? &layer->vector_mask.path
                                                     : NULL;
    }
    if (!s->name.len || pc_str_eq_c(&s->name, "work"))
        return doc->has_work_path ? &doc->work_path : NULL;
    for (size_t i = 0; i < doc->npaths; i++)
        if (pc_str_eq(&doc->paths[i].name, &s->name))
            return &doc->paths[i].path;
    return NULL;
}
static bool same(pc_vec_knot_ref a, pc_vec_knot_ref b)
{
    return a.sub == b.sub && a.knot == b.knot;
}
static bool selected(const pc_ui_direct *s, pc_vec_knot_ref r)
{
    for (size_t i = 0; i < s->count; i++)
        if (same(s->anchors[i], r))
            return true;
    return false;
}
static pc_status select_anchor(pc_ui_direct *s, pc_vec_knot_ref r, bool toggle, pc_error *err)
{
    for (size_t i = 0; i < s->count; i++)
        if (same(s->anchors[i], r)) {
            if (toggle) {
                memmove(s->anchors + i, s->anchors + i + 1, (s->count - i - 1) * sizeof *s->anchors);
                s->count--;
            }
            return PC_OK;
        }
    if (s->count >= 4096)
        return pc_fail(err, PC_ERR_LIMIT, "too many selected anchors");
    pc_vec_knot_ref *next = realloc(s->anchors, (s->count + 1) * sizeof *next);
    if (!next)
        return pc_fail(err, PC_ERR_NOMEM, "selected anchors");
    s->anchors = next;
    s->anchors[s->count++] = r;
    return PC_OK;
}
static bool hit_path(const pc_ui_direct *s, const pc_path *path, pc_point point, double tolerance,
                     pc_vec_hit *hit)
{
    pc_vec_handle_ref handles[8196];
    size_t n = 0;
    for (size_t i = 0; i < s->count && n + 2 <= 8196; i++) {
        handles[n++] = (pc_vec_handle_ref){s->anchors[i], PC_VEC_HANDLE_IN};
        handles[n++] = (pc_vec_handle_ref){s->anchors[i], PC_VEC_HANDLE_OUT};
    }
    if (s->has_segment && s->segment.sub < path->nsubpaths) {
        const pc_subpath *sub = &path->subpaths[s->segment.sub];
        if (sub->nknots && s->segment.knot < pc_subpath_segment_count(sub) && n + 2 <= 8196) {
            handles[n++] = (pc_vec_handle_ref){s->segment, PC_VEC_HANDLE_OUT};
            handles[n++] =
                (pc_vec_handle_ref){{s->segment.sub, (s->segment.knot + 1) % sub->nknots}, PC_VEC_HANDLE_IN};
        }
    }
    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        pc_vec_knot_ref r = handles[i].knot;
        if (r.sub >= path->nsubpaths || r.knot >= path->subpaths[r.sub].nknots)
            continue;
        const pc_knot *k = &path->subpaths[r.sub].knots[r.knot];
        pc_point h = handles[i].handle == PC_VEC_HANDLE_IN ? k->in_ctrl : k->out_ctrl;
        if (hypot(h.x - k->anchor.x, h.y - k->anchor.y) > 1e-6)
            handles[kept++] = handles[i];
    }
    return pc_vec_hit_test(path, point, tolerance, handles, kept, hit);
}
static pc_status choose(pc_ui_direct *s, const pc_ui_state *ui, const pc_engine_doc_state *state,
                        pc_point point, double tol, pc_vec_hit *hit, bool *found, pc_error *err)
{
    const pc_document *doc = pc_doc_arc_get(state->doc);
    if (s->document && s->document != doc->id) {
        pc_ui_direct_fini(s);
    }
    const pc_path *path = pc_ui_direct_path(s, state);
    if (s->name.len && path && hit_path(s, path, point, tol, hit)) {
        if (!s->name.len) {
            pc_status status = pc_str_set_c(&s->name, "work", err);
            if (status != PC_OK)
                return status;
        }
        *found = true;
        return PC_OK;
    }
    const pc_layer *layer = state->has_active_layer ? pc_document_layer(doc, state->active_layer) : NULL;
    const pc_path *lp = layer ? (ui->vector_mask_target && layer->has_vector_mask ? &layer->vector_mask.path
                                 : layer->content.kind == PC_LAYER_SHAPE ? &layer->content.u.shape.path
                                                                         : NULL)
                              : NULL;
    const pc_path *candidates[2] = {lp, doc->has_work_path ? &doc->work_path : NULL};
    for (size_t i = 0; i < 2; i++)
        if (candidates[i] && pc_vec_hit_test(candidates[i], point, tol, NULL, 0, hit)) {
            pc_status status = pc_str_set_c(&s->name, i ? "work" : "layer", err);
            if (status != PC_OK)
                return status;
            s->count = 0;
            s->has_segment = false;
            s->layer = i ? 0 : state->active_layer;
            s->document = doc->id;
            *found = true;
            return PC_OK;
        }
    *found = false;
    if (!path) {
        pc_status status = pc_str_set_c(&s->name, lp ? "layer" : "work", err);
        if (status != PC_OK)
            return status;
        s->layer = lp ? state->active_layer : 0;
        s->document = doc->id;
    }
    return PC_OK;
}
static pc_status execute(pc_engine_session *session, pc_ui_direct *s, const char *command,
                         yyjson_mut_doc *doc, yyjson_mut_val *p, pc_json_value *out, pc_error *err)
{
    if (!p || !yyjson_mut_obj_add_strcpy(doc, p, "name", pc_str_c(&s->name)) ||
        !yyjson_mut_obj_add_uint(doc, p, "layer", s->layer))
        return pc_fail(err, PC_ERR_NOMEM, "path edit parameters");
    pc_buf text = PC_BUF_INIT;
    pc_status status = pc_json_write(p, 0, &text, err);
    if (status == PC_OK)
        status = pc_engine_session_execute_json(session, command, pc_buf_cstr(&text), out, err);
    pc_buf_fini(&text);
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (status == PC_OK && state)
        s->revision = state->revision;
    return status;
}
static yyjson_mut_val *point_json(yyjson_mut_doc *doc, double x, double y)
{
    yyjson_mut_val *v = yyjson_mut_arr(doc);
    return v && yyjson_mut_arr_add_real(doc, v, x) && yyjson_mut_arr_add_real(doc, v, y) ? v : NULL;
}
static pc_status commit(pc_engine_session *session, pc_ui_direct *s, pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = doc ? yyjson_mut_obj(doc) : NULL;
    const char *command = NULL;
    bool ok = p != NULL;
    double dx = s->end[0] - s->start[0], dy = s->end[1] - s->start[1];
    if (s->edit == 0) {
        command = "path.moveAnchors";
        yyjson_mut_val *a = doc ? yyjson_mut_arr(doc) : NULL;
        ok = ok && a;
        for (size_t i = 0; ok && i < s->count; i++) {
            yyjson_mut_val *r = yyjson_mut_arr(doc);
            ok = r && yyjson_mut_arr_add_uint(doc, r, s->anchors[i].sub) &&
                 yyjson_mut_arr_add_uint(doc, r, s->anchors[i].knot) && yyjson_mut_arr_add_val(a, r);
        }
        ok = ok && yyjson_mut_obj_add_val(doc, p, "anchors", a) &&
             yyjson_mut_obj_add_val(doc, p, "move", point_json(doc, dx, dy));
    } else {
        ok = ok && yyjson_mut_obj_add_uint(doc, p, "subpath", s->hit.knot.sub) &&
             yyjson_mut_obj_add_uint(doc, p, "knot", s->hit.knot.knot);
        if (s->edit == 1) {
            command = "path.moveHandle";
            ok = ok && yyjson_mut_obj_add_str(doc, p, "handle", pc_vec_handle_name(s->hit.handle)) &&
                 yyjson_mut_obj_add_bool(doc, p, "independent", s->independent) &&
                 yyjson_mut_obj_add_val(doc, p, "to",
                                        point_json(doc, s->handle_start.x + dx, s->handle_start.y + dy));
        } else if (s->edit == 2) {
            command = "path.bendSegment";
            ok = ok && yyjson_mut_obj_add_real(doc, p, "t", s->hit.t) &&
                 yyjson_mut_obj_add_val(doc, p, "move", point_json(doc, dx, dy));
        } else if (s->edit == 3) {
            command = "path.convertPoint";
            if (hypot(dx, dy) > .01) {
                const pc_knot *k = &s->base.subpaths[s->hit.knot.sub].knots[s->hit.knot.knot];
                ok = ok && yyjson_mut_obj_add_val(doc, p, "out",
                                                  point_json(doc, k->anchor.x + dx, k->anchor.y + dy));
            }
        } else if (s->edit == 4) {
            command = "path.addAnchor";
            ok = ok && yyjson_mut_obj_add_real(doc, p, "t", fmin(.999999, fmax(.000001, s->hit.t)));
        } else
            command = "path.deleteAnchor";
    }
    pc_status status = ok ? execute(session, s, command, doc, p, out, err)
                          : pc_fail(err, PC_ERR_NOMEM, "path edit parameters");
    yyjson_mut_doc_free(doc);
    return status;
}
static pc_status preview(pc_ui_direct *s, pc_error *err)
{
    pc_path next = {0};
    pc_status status = pc_path_clone(&s->base, &next, err);
    if (status != PC_OK)
        return status;
    double move[2] = {s->end[0] - s->start[0], s->end[1] - s->start[1]};
    if (s->edit == 0)
        status = pc_vec_move_anchors(&next, s->anchors, s->count, move, err);
    else if (s->edit == 1)
        status = pc_vec_move_handle(&next, s->hit.knot, s->hit.handle,
                                    pc_point_make(s->handle_start.x + move[0], s->handle_start.y + move[1]),
                                    s->independent, err);
    else if (s->edit == 2)
        status = pc_vec_bend_segment(&next, s->hit.knot, s->hit.t, move, err);
    else if (s->edit == 3) {
        pc_point to;
        pc_point *value = NULL;
        if (hypot(move[0], move[1]) > .01) {
            const pc_knot *k = &s->base.subpaths[s->hit.knot.sub].knots[s->hit.knot.knot];
            to = pc_point_make(k->anchor.x + move[0], k->anchor.y + move[1]);
            value = &to;
        }
        status = pc_vec_convert_point(&next, s->hit.knot, value, err);
    }
    if (status == PC_OK) {
        pc_path_fini(&s->preview);
        s->preview = next;
        s->has_preview = true;
        s->generation++;
    } else
        pc_path_fini(&next);
    return status;
}
static pc_status marquee(pc_ui_direct *s, const pc_path *path, pc_error *err)
{
    double box[4] = {s->start[0], s->start[1], s->end[0], s->end[1]};
    pc_vec_knot_ref *refs = NULL;
    size_t count = 0;
    pc_status status = pc_vec_anchors_in(path, box, &refs, &count, err);
    if (status == PC_OK) {
        if (!s->add)
            s->count = 0;
        for (size_t i = 0; status == PC_OK && i < count; i++)
            status = select_anchor(s, refs[i], false, err);
    }
    free(refs);
    return status;
}
pc_status pc_ui_direct_pointer(pc_engine_session *session, pc_ui_state *ui, pc_ui_pointer_kind kind,
                               pc_ui_tool_point point, unsigned mods, float zoom, pc_json_value *out,
                               pc_error *err)
{
    pc_ui_direct *s = &ui->direct;
    if (kind == PC_UI_POINTER_CANCEL) {
        pc_ui_direct_cancel(s);
        return PC_OK;
    }
    if (!s->active && !pc_ui_direct_handles(ui, mods))
        return PC_ERR_NOT_FOUND;
    if (!isfinite(point.x) || !isfinite(point.y) || fabsf(point.x) > 1e6f || fabsf(point.y) > 1e6f ||
        !isfinite(zoom) || zoom <= 0)
        return pc_fail(err, PC_ERR_INVALID, "invalid path pointer");
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (!state)
        return pc_fail(err, PC_ERR_INVALID, "path editing needs a document");
    const pc_document *doc = pc_doc_arc_get(state->doc);
    if (s->active &&
        (s->document != doc->id || s->revision != state->revision ||
         (pc_str_eq_c(&s->name, "layer") && (!state->has_active_layer || s->layer != state->active_layer)))) {
        pc_ui_direct_cancel(s);
        return pc_fail(err, PC_ERR_INVALID, "path changed during drag");
    }
    if (kind == PC_UI_POINTER_DOWN) {
        pc_ui_direct_cancel(s);
        pc_vec_hit hit = {0};
        bool found = false;
        pc_status status =
            choose(s, ui, state, pc_point_make(point.x, point.y), 6. / zoom, &hit, &found, err);
        if (status != PC_OK)
            return status;
        const pc_path *path = pc_ui_direct_path(s, state);
        if (!path)
            return PC_OK;
        bool command = (mods & (PC_UI_MOD_COMMAND | PC_UI_MOD_CTRL)) != 0;
        bool convert = !command && (ui->tool == PC_TOOL_CONVERT_POINT ||
                                    (pc_ui_pen_tool(ui->tool) && (mods & PC_UI_MOD_ALT)));
        if ((ui->tool == PC_TOOL_ADD_ANCHOR_POINT || ui->tool == PC_TOOL_DELETE_ANCHOR_POINT) && !command) {
            if (!found || (ui->tool == PC_TOOL_ADD_ANCHOR_POINT ? hit.kind != PC_VEC_HIT_SEGMENT
                                                                : hit.kind != PC_VEC_HIT_ANCHOR))
                return PC_OK;
            s->hit = hit;
            s->edit = ui->tool == PC_TOOL_ADD_ANCHOR_POINT ? 4u : 5u;
            status = commit(session, s, out, err);
            if (status == PC_OK) {
                s->count = 0;
                s->has_segment = false;
            }
            return status;
        }
        if (convert && (!found || hit.kind == PC_VEC_HIT_SEGMENT))
            return PC_OK;
        if (!found) {
            s->marquee = true;
            s->add = (mods & PC_UI_MOD_SHIFT) != 0;
            if (!s->add)
                s->count = 0;
            s->has_segment = false;
        } else {
            s->marquee = false;
            s->hit = hit;
            if (hit.kind == PC_VEC_HIT_ANCHOR) {
                if ((mods & PC_UI_MOD_SHIFT) && !convert) {
                    status = select_anchor(s, hit.knot, true, err);
                    if (status != PC_OK)
                        return status;
                    if (!selected(s, hit.knot))
                        return PC_OK;
                } else if (!selected(s, hit.knot)) {
                    s->count = 0;
                    status = select_anchor(s, hit.knot, false, err);
                    if (status != PC_OK)
                        return status;
                }
                s->has_segment = false;
                s->edit = convert ? 3u : 0u;
            } else if (hit.kind == PC_VEC_HIT_HANDLE) {
                s->edit = 1;
                s->independent = convert || (mods & PC_UI_MOD_ALT);
                const pc_knot *k = &path->subpaths[hit.knot.sub].knots[hit.knot.knot];
                s->handle_start = hit.handle == PC_VEC_HANDLE_IN ? k->in_ctrl : k->out_ctrl;
            } else {
                s->edit = 2;
                s->has_segment = true;
                s->segment = hit.knot;
            }
            if (ui->tool == PC_TOOL_PATH_SELECTION ||
                (hit.kind == PC_VEC_HIT_ANCHOR && (mods & PC_UI_MOD_ALT) && !convert)) {
                pc_vec_knot_ref *refs = NULL;
                size_t count = 0;
                status = pc_vec_subpath_anchors(path, hit.knot.sub, &refs, &count, err);
                if (!(mods & PC_UI_MOD_SHIFT))
                    s->count = 0;
                for (size_t i = 0; status == PC_OK && i < count; i++)
                    status = select_anchor(s, refs[i], false, err);
                free(refs);
                if (status != PC_OK)
                    return status;
                s->edit = 0;
            }
            status = pc_path_clone(path, &s->base, err);
            if (status != PC_OK)
                return status;
        }
        s->active = true;
        s->document = doc->id;
        s->revision = state->revision;
        s->start[0] = s->end[0] = point.x;
        s->start[1] = s->end[1] = point.y;
        return PC_OK;
    }
    if (!s->active)
        return PC_OK;
    s->end[0] = point.x;
    s->end[1] = point.y;
    if (s->edit == 1)
        s->independent = ui->tool == PC_TOOL_CONVERT_POINT || (mods & PC_UI_MOD_ALT) != 0;
    if ((mods & PC_UI_MOD_SHIFT) && !s->marquee) {
        double dx = s->end[0] - s->start[0], dy = s->end[1] - s->start[1];
        if (fabs(dx) > fabs(dy))
            s->end[1] = s->start[1];
        else
            s->end[0] = s->start[0];
    }
    if (s->marquee) {
        if (kind == PC_UI_POINTER_UP) {
            const pc_path *path = pc_ui_direct_path(s, state);
            pc_status status = path ? marquee(s, path, err) : PC_OK;
            pc_ui_direct_cancel(s);
            return status;
        }
        return PC_OK;
    }
    pc_status status = preview(s, err);
    if (kind == PC_UI_POINTER_UP) {
        if (status == PC_OK &&
            (s->edit == 3 || hypot(s->end[0] - s->start[0], s->end[1] - s->start[1]) > .01))
            status = commit(session, s, out, err);
        pc_ui_direct_cancel(s);
    }
    return status;
}
pc_status pc_ui_pen_auto(pc_engine_session *session, pc_ui_state *ui, pc_ui_tool_point point, float zoom,
                         pc_json_value *out, pc_error *err)
{
    if (ui->tool != PC_TOOL_PEN || ui->has_pen || !ui->tool_options.pen_auto_add_delete)
        return PC_ERR_NOT_FOUND;
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (!state)
        return PC_ERR_NOT_FOUND;
    if (!isfinite(zoom) || zoom <= 0 || !isfinite(point.x) || !isfinite(point.y))
        return pc_fail(err, PC_ERR_INVALID, "invalid path pointer");
    pc_vec_hit hit = {0};
    bool found = false;
    pc_ui_direct *s = &ui->direct;
    pc_status status = choose(s, ui, state, pc_point_make(point.x, point.y), 6. / zoom, &hit, &found, err);
    if (status != PC_OK)
        return status;
    if (!found || hit.kind == PC_VEC_HIT_HANDLE)
        return PC_ERR_NOT_FOUND;
    s->hit = hit;
    s->edit = hit.kind == PC_VEC_HIT_ANCHOR ? 5u : 4u;
    status = commit(session, s, out, err);
    if (status == PC_OK) {
        s->count = 0;
        s->has_segment = false;
    }
    return status;
}
pc_status pc_ui_direct_key(pc_engine_session *session, pc_ui_state *ui, pc_ui_key key, unsigned mods,
                           pc_json_value *out, pc_error *err)
{
    (void)session;
    (void)out;
    (void)err;
    if (!mods && key == PC_KEY_ESCAPE && (ui->direct.active || ui->direct.count)) {
        pc_ui_direct_cancel(&ui->direct);
        ui->direct.count = 0;
        ui->direct.has_segment = false;
        return PC_OK;
    }
    return PC_ERR_NOT_FOUND;
}
