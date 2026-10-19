#include "artboard.h"
#include "doc/comps.h"
#include "ui/canvas.h"
#include "ui/transform.h"
#include "base/mem.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
void pc_ui_artboard_layout_fini(pc_ui_artboard_layout *l)
{
    if (!l)
        return;
    free(l->boards);
    free(l->gaps);
    *l = (pc_ui_artboard_layout){0};
}
pc_status pc_ui_artboard_layout_update(pc_ui_artboard_layout *l, const pc_engine_doc_state *state,
                                       pc_error *err)
{
    if (!l || !state)
        return pc_fail(err, PC_ERR_INVALID, "no artboard document");
    const pc_document *doc = pc_doc_arc_get(state->doc);
    if (!doc)
        return pc_fail(err, PC_ERR_INVALID, "missing artboard document");
    if (l->valid && l->document == doc->id && l->revision == state->revision)
        return PC_OK;
    size_t count = 0;
    for (size_t i = 0; i < doc->nlayers; i++)
        if (pc_layer_artboard(&doc->layers[i]))
            count++;
    if (count > 65536)
        return pc_fail(err, PC_ERR_LIMIT, "too many interactive artboards");
    pc_ui_artboard_entry *boards = count ? pc_malloc_array(count, sizeof *boards) : NULL;
    pc_rect *taken = count ? pc_malloc_array(count, sizeof *taken) : NULL;
    if (count && (!boards || !taken)) {
        free(boards);
        free(taken);
        return pc_fail(err, PC_ERR_NOMEM, "artboard layout");
    }
    size_t n = 0;
    for (size_t i = 0; i < doc->nlayers; i++) {
        const pc_artboard *a = pc_layer_artboard(&doc->layers[i]);
        if (a) {
            if (!boards || !taken || n >= count) {
                free(boards);
                free(taken);
                return pc_fail(err, PC_ERR_INVALID, "artboard layout changed during collection");
            }
            boards[n] = (pc_ui_artboard_entry){doc->layers[i].id, i, a->rect};
            taken[n++] = a->rect;
        }
    }
    bool same = (!count || (boards && l->boards)) && l->valid && l->document == doc->id &&
                l->count == count && l->size.width == doc->size.width && l->size.height == doc->size.height;
    for (size_t i = 0; same && boards && l->boards && i < count; i++)
        same = pc_rect_eq(boards[i].rect, l->boards[i].rect);
    pc_rect *gaps = NULL;
    size_t ngaps = 0;
    pc_status status =
        count && !same ? pc_auto_slices(pc_document_bounds(doc), taken, count, &gaps, &ngaps, err) : PC_OK;
    free(taken);
    if (status != PC_OK) {
        free(boards);
        return status;
    }
    free(l->boards);
    l->boards = boards;
    l->count = count;
    if (!same) {
        free(l->gaps);
        l->gaps = gaps;
        l->ngaps = ngaps;
    }
    l->valid = true;
    l->document = doc->id;
    l->revision = state->revision;
    l->size = doc->size;
    return PC_OK;
}
bool pc_ui_artboard_active(const pc_engine_doc_state *state, pc_layer_id *id, pc_rect *rect)
{
    if (!state || !state->has_active_layer)
        return false;
    const pc_document *doc = pc_doc_arc_get(state->doc);
    pc_layer_id board;
    if (!pc_document_artboard_of(doc, state->active_layer, &board))
        return false;
    const pc_artboard *a = pc_layer_artboard(pc_document_layer(doc, board));
    if (!a)
        return false;
    if (id)
        *id = board;
    if (rect)
        *rect = a->rect;
    return true;
}
void pc_ui_artboard_plus_points(pc_rect r, double zoom, double out[4][2])
{
    double x = ((double)r.x0 + r.x1) / 2, y = ((double)r.y0 + r.y1) / 2, off = 18 / fmax(.01, zoom);
    const double points[4][2] = {{x, r.y0 - off}, {r.x1 + off, y}, {x, r.y1 + off}, {r.x0 - off, y}};
    memcpy(out, points, sizeof points);
}
static int32_t bounded(double v)
{
    return (int32_t)fmax(INT32_MIN, fmin(INT32_MAX, round(v)));
}
pc_rect pc_ui_artboard_drag_rect(const pc_ui_artboard_tool *t)
{
    if (!t->kind)
        return pc_rect_make(
            bounded(fmin(t->start[0], t->current[0])), bounded(fmin(t->start[1], t->current[1])),
            bounded(fmax(t->start[0], t->current[0])), bounded(fmax(t->start[1], t->current[1])));
    int32_t dx = bounded(t->current[0] - t->start[0]), dy = bounded(t->current[1] - t->start[1]);
    if (t->kind == 1)
        return pc_rect_translate(t->rect, dx, dy);
    double x0 = t->rect.x0, y0 = t->rect.y0, x1 = t->rect.x1, y1 = t->rect.y1;
    if (t->keep && t->hx && t->hy) {
        double w = fmax(1, x1 - x0), h = fmax(1, y1 - y0);
        double ax = t->hx > 0 ? x0 : x1, ay = t->hy > 0 ? y0 : y1, dw = (t->hx > 0 ? x1 : x0) + dx - ax,
               dh = (t->hy > 0 ? y1 : y0) + dy - ay;
        double k = fmax(fabs(dw) / w, fabs(dh) / h), nw = fmax(1, round(w * k)), nh = fmax(1, round(h * k));
        x0 = dw < 0 ? ax - nw : ax;
        x1 = x0 + nw;
        y0 = dh < 0 ? ay - nh : ay;
        y1 = y0 + nh;
    } else {
        if (t->hx < 0)
            x0 += dx;
        else if (t->hx > 0)
            x1 += dx;
        if (t->hy < 0)
            y0 += dy;
        else if (t->hy > 0)
            y1 += dy;
        double lo = fmin(x0, x1), hi = fmax(x0, x1);
        x0 = lo;
        x1 = fmax(lo + 1, hi);
        lo = fmin(y0, y1);
        hi = fmax(y0, y1);
        y0 = lo;
        y1 = fmax(lo + 1, hi);
    }
    return pc_rect_make(bounded(x0), bounded(y0), bounded(x1), bounded(y1));
}
static pc_status result(pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *o = d ? yyjson_mut_obj(d) : NULL;
    pc_status status =
        o ? pc_json_value_from_mut(o, out, err) : pc_fail(err, PC_ERR_NOMEM, "artboard result");
    yyjson_mut_doc_free(d);
    return status;
}
static bool hit_board(const pc_document *doc, const double p[2], pc_layer_id *id, pc_rect *rect)
{
    for (size_t i = doc->nlayers; i > 0; i--) {
        const pc_artboard *a = pc_layer_artboard(&doc->layers[i - 1]);
        if (a && p[0] >= a->rect.x0 && p[0] < a->rect.x1 && p[1] >= a->rect.y0 && p[1] < a->rect.y1) {
            *id = doc->layers[i - 1].id;
            *rect = a->rect;
            return true;
        }
    }
    return false;
}
pc_status pc_ui_artboard_pointer(pc_engine_session *session, pc_ui_state *ui, pc_ui_pointer_kind kind,
                                 pc_ui_tool_point point, unsigned mods, double zoom, pc_json_value *out,
                                 pc_error *err)
{
    pc_ui_artboard_tool *t = &ui->artboard_tool;
    if (ui->tool != PC_TOOL_ARTBOARD || ui->transform.active) {
        t->active = false;
        return PC_ERR_NOT_FOUND;
    }
    if (kind == PC_UI_POINTER_CANCEL) {
        t->active = false;
        return result(out, err);
    }
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (!state || ui->n_dialogs || !isfinite(zoom) || zoom <= 0)
        return pc_fail(err, PC_ERR_INVALID, "artboard tool needs a document");
    if (!isfinite(point.x) || !isfinite(point.y) || fabsf(point.x) > 1000000 || fabsf(point.y) > 1000000) {
        if (kind == PC_UI_POINTER_UP)
            t->active = false;
        return pc_fail(err, PC_ERR_INVALID, "invalid artboard pointer");
    }
    const pc_document *doc = pc_doc_arc_get(state->doc);
    double p[2] = {point.x, point.y};
    if (t->active && (t->document != doc->id || t->revision != state->revision ||
                      t->has_active != state->has_active_layer ||
                      (t->has_active && t->active_layer != state->active_layer))) {
        t->active = false;
        return pc_fail(err, PC_ERR_INVALID, "the artboard document or selection changed");
    }
    if (kind == PC_UI_POINTER_DOWN) {
        pc_layer_id selected = 0;
        pc_rect selected_rect = {0};
        bool has_board = pc_ui_artboard_active(state, &selected, &selected_rect);
        if (has_board) {
            double plus[4][2];
            pc_ui_artboard_plus_points(selected_rect, zoom, plus);
            const char *sides[] = {"top", "right", "bottom", "left"};
            for (size_t i = 0; i < 4; i++)
                if (hypot(p[0] - plus[i][0], p[1] - plus[i][1]) <= 8 / fmax(.01, zoom)) {
                    char params[160];
                    (void)snprintf(params, sizeof params, "{\"layer\":%llu,\"side\":\"%s\",\"duplicate\":%s}",
                                   (unsigned long long)selected, sides[i],
                                   (mods & PC_UI_MOD_ALT) ? "true" : "false");
                    t->active = false;
                    return pc_engine_session_execute_json(session, "layer.artboard.addBeside", params, out,
                                                          err);
                }
        }
        *t = (pc_ui_artboard_tool){.active = true,
                                   .document = doc->id,
                                   .revision = state->revision,
                                   .keep = (mods & PC_UI_MOD_SHIFT) != 0};
        if (has_board) {
            double r[4] = {selected_rect.x0, selected_rect.y0, selected_rect.x1, selected_rect.y1};
            pc_ui_transform_frame f;
            pc_ui_transform_frame_rect(&f, r);
            pc_ui_transform_hit hit = pc_ui_transform_hit_test(&f, p, 6 / fmax(.01, zoom));
            if (hit.part == PC_TRANSFORM_CORNER || hit.part == PC_TRANSFORM_EDGE) {
                static const int corner[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}},
                                 edge[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
                const int *h = hit.part == PC_TRANSFORM_CORNER ? corner[hit.index % 4] : edge[hit.index % 4];
                t->kind = 2;
                t->hx = h[0];
                t->hy = h[1];
                t->layer = selected;
                t->rect = selected_rect;
            }
        }
        if (!t->kind && hit_board(doc, p, &t->layer, &t->rect)) {
            t->kind = 1;
            if (!state->has_active_layer || state->active_layer != t->layer) {
                char params[80];
                (void)snprintf(params, sizeof params, "{\"layer\":%llu}", (unsigned long long)t->layer);
                pc_status status = pc_engine_session_execute_json(session, "layer.select", params, out, err);
                if (status != PC_OK) {
                    t->active = false;
                    return status;
                }
                state = pc_engine_session_active(session);
            }
        }
        t->revision = state->revision;
        t->has_active = state->has_active_layer;
        t->active_layer = state->active_layer;
        memcpy(t->start, p, sizeof p);
        memcpy(t->current, p, sizeof p);
    } else if (t->active) {
        memcpy(t->current, p, sizeof p);
        t->keep = (mods & PC_UI_MOD_SHIFT) != 0;
        if (kind == PC_UI_POINTER_UP) {
            t->active = false;
            pc_rect r = pc_ui_artboard_drag_rect(t);
            if (!t->kind && (hypot(p[0] - t->start[0], p[1] - t->start[1]) * zoom < 3 || pc_rect_is_empty(r)))
                return result(out, err);
            if (t->kind && pc_rect_eq(r, t->rect))
                return result(out, err);
            char params[240];
            if (!t->kind)
                (void)snprintf(params, sizeof params, "{\"rect\":[%d,%d,%u,%u]}", r.x0, r.y0,
                               pc_rect_width(r), pc_rect_height(r));
            else if (t->kind == 1)
                (void)snprintf(params, sizeof params, "{\"layer\":%llu,\"x\":%d,\"y\":%d}",
                               (unsigned long long)t->layer, r.x0, r.y0);
            else
                (void)snprintf(params, sizeof params,
                               "{\"layer\":%llu,\"rect\":[%d,%d,%u,%u],\"moveContents\":false}",
                               (unsigned long long)t->layer, r.x0, r.y0, pc_rect_width(r), pc_rect_height(r));
            return pc_engine_session_execute_json(
                session, t->kind ? "layer.artboard.set" : "layer.new.artboard", params, out, err);
        }
    }
    return result(out, err);
}
pc_status pc_ui_artboard_fit(pc_ui_view *view, const pc_engine_doc_state *state, float width, float height,
                             float scale, pc_layer_id *board, pc_rect *rect, pc_error *err)
{
    if (!view || !state || !isfinite(width) || !isfinite(height) || !isfinite(scale) || scale <= 0)
        return pc_fail(err, PC_ERR_INVALID, "invalid artboard view");
    pc_layer_id id;
    pc_rect bounds;
    if (!pc_ui_artboard_active(state, &id, &bounds)) {
        const pc_document *doc = pc_doc_arc_get(state->doc);
        bool found = false;
        for (size_t i = doc->nlayers; i > 0; i--) {
            const pc_artboard *a = pc_layer_artboard(&doc->layers[i - 1]);
            if (a) {
                id = doc->layers[i - 1].id;
                bounds = a->rect;
                found = true;
                break;
            }
        }
        if (!found)
            return pc_fail(err, PC_ERR_INVALID, "the document has no artboards");
    }
    if (pc_rect_is_empty(bounds))
        return pc_fail(err, PC_ERR_INVALID, "the artboard is empty");
    if (width <= 50) {
        width = 1200;
        height = 800;
    }
    const pc_document *doc = pc_doc_arc_get(state->doc);
    uint32_t size[2] = {doc->size.width, doc->size.height};
    view->zoom = pc_ui_zoom_clamp(
        fminf((width - 60) / (float)pc_rect_width(bounds), (height - 80) / (float)pc_rect_height(bounds)) *
            scale,
        size);
    view->center[0] = (float)(((double)bounds.x0 + bounds.x1) / 2);
    view->center[1] = (float)(((double)bounds.y0 + bounds.y1) / 2);
    view->fit_pending = false;
    view->fill_pending = false;
    if (board)
        *board = id;
    if (rect)
        *rect = bounds;
    return PC_OK;
}
