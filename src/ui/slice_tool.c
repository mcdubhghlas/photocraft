#include "slice_tool.h"
#include "engine/slice_ui.h"
#include "base/mem.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
void pc_ui_slice_fini(pc_ui_slice_state *s)
{
    if (!s)
        return;
    pc_resolved_slices_free(s->resolved, s->count);
    free(s->z);
    *s = (pc_ui_slice_state){0};
}
bool pc_ui_slice_tool(pc_ui_tool t)
{
    return t == PC_TOOL_SLICE || t == PC_TOOL_SLICE_SELECT;
}
static pc_status result(pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *o = d ? yyjson_mut_obj(d) : NULL;
    pc_status status = o ? pc_json_value_from_mut(o, out, err) : pc_fail(err, PC_ERR_NOMEM, "slice result");
    yyjson_mut_doc_free(d);
    return status;
}
typedef struct rank {
    uint32_t id;
    size_t z;
} rank;
static int compare_rank(const void *a, const void *b)
{
    uint32_t x = ((const rank *)a)->id, y = ((const rank *)b)->id;
    return x < y ? -1 : x > y ? 1 : 0;
}
pc_status pc_ui_slice_resolve(pc_ui_slice_state *s, const pc_engine_doc_state *state, pc_error *err)
{
    if (!s || !state)
        return pc_fail(err, PC_ERR_INVALID, "no slice document");
    const pc_document *doc = pc_doc_arc_get(state->doc);
    if (s->cached && s->cache_document == doc->id && s->cache_revision == state->revision)
        return PC_OK;
    if (doc->slices.len > 65536)
        return pc_fail(err, PC_ERR_LIMIT, "too many interactive slices");
    pc_resolved_slice *resolved = NULL;
    size_t count = 0;
    pc_status status = pc_slices_resolve(doc, &resolved, &count, err);
    if (status != PC_OK)
        return status;
    rank *ranks = doc->slices.len ? pc_malloc_array(doc->slices.len, sizeof *ranks) : NULL;
    size_t *z = count ? pc_calloc_array(count, sizeof *z) : NULL;
    if ((doc->slices.len && !ranks) || (count && !z)) {
        free(ranks);
        free(z);
        pc_resolved_slices_free(resolved, count);
        return pc_fail(err, PC_ERR_NOMEM, "slice ordering");
    }
    for (size_t i = 0; i < doc->slices.len; i++)
        ranks[i] = (rank){doc->slices.list[i].id, i + 1};
    if (doc->slices.len)
        qsort(ranks, doc->slices.len, sizeof *ranks, compare_rank);
    for (size_t i = 0; i < count; i++)
        if (resolved[i].has_id) {
            rank key = {.id = resolved[i].id};
            const rank *found =
                doc->slices.len ? bsearch(&key, ranks, doc->slices.len, sizeof *ranks, compare_rank) : NULL;
            if (found)
                z[i] = found->z;
        }
    free(ranks);
    pc_resolved_slices_free(s->resolved, s->count);
    free(s->z);
    if (s->cache_document && s->cache_document != doc->id)
        s->has_selected = false;
    s->resolved = resolved;
    s->count = count;
    s->z = z;
    s->cache_document = doc->id;
    s->cache_revision = state->revision;
    s->cached = true;
    if (s->has_selected && !pc_ui_slice_selected(s))
        s->has_selected = false;
    return PC_OK;
}
const pc_resolved_slice *pc_ui_slice_selected(pc_ui_slice_state *s)
{
    for (size_t i = 0; s->has_selected && i < s->count; i++)
        if (s->resolved[i].number == s->selected)
            return &s->resolved[i];
    return NULL;
}
const pc_resolved_slice *pc_ui_slice_hit(pc_ui_slice_state *s, double x, double y)
{
    if (!isfinite(x) || !isfinite(y) || fabs(x) > 1000000 || fabs(y) > 1000000)
        return NULL;
    const pc_resolved_slice *hit = NULL;
    size_t best = 0;
    for (size_t i = 0; i < s->count; i++)
        if (pc_rect_contains(s->resolved[i].rect, (int32_t)floor(x), (int32_t)floor(y)) &&
            (!hit || s->z[i] > best)) {
            hit = &s->resolved[i];
            best = s->z[i];
        }
    return hit;
}
pc_rect pc_ui_slice_drag_rect(const pc_ui_slice_state *s)
{
    if (s->moving) {
        int32_t dx = (int32_t)round(s->current[0] - s->start[0]),
                dy = (int32_t)round(s->current[1] - s->start[1]);
        return pc_rect_translate(s->rect, dx, dy);
    }
    return pc_rect_make(
        (int32_t)floor(fmin(s->start[0], s->current[0])), (int32_t)floor(fmin(s->start[1], s->current[1])),
        (int32_t)ceil(fmax(s->start[0], s->current[0])), (int32_t)ceil(fmax(s->start[1], s->current[1])));
}
pc_status pc_ui_slice_pointer(pc_engine_session *session, pc_ui_state *ui, pc_ui_pointer_kind kind,
                              pc_ui_tool_point point, pc_json_value *out, pc_error *err)
{
    if (!pc_ui_slice_tool(ui->tool) || ui->transform.active) {
        ui->slices.dragging = false;
        return PC_ERR_NOT_FOUND;
    }
    pc_ui_slice_state *s = &ui->slices;
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (!state || ui->n_dialogs) {
        s->dragging = false;
        return pc_fail(err, PC_ERR_INVALID, "slice tool needs a document");
    }
    if (kind == PC_UI_POINTER_CANCEL) {
        s->dragging = false;
        return result(out, err);
    }
    if (!isfinite(point.x) || !isfinite(point.y) || fabsf(point.x) > 1000000 || fabsf(point.y) > 1000000) {
        if (kind == PC_UI_POINTER_UP)
            s->dragging = false;
        return pc_fail(err, PC_ERR_INVALID, "invalid slice pointer");
    }
    pc_status status = pc_ui_slice_resolve(s, state, err);
    if (status != PC_OK)
        return status;
    if (s->dragging && (s->document != pc_doc_arc_get(state->doc)->id || s->revision != state->revision ||
                        s->tool != (int)ui->tool)) {
        s->dragging = false;
        return pc_fail(err, PC_ERR_INVALID, "slice document changed");
    }
    bool locked = pc_engine_slices_locked(session);
    if (locked && !(kind == PC_UI_POINTER_DOWN && ui->tool == PC_TOOL_SLICE_SELECT)) {
        s->dragging = false;
        return result(out, err);
    }
    if (kind == PC_UI_POINTER_DOWN) {
        s->document = pc_doc_arc_get(state->doc)->id;
        s->revision = state->revision;
        s->tool = (int)ui->tool;
        s->start[0] = s->current[0] = point.x;
        s->start[1] = s->current[1] = point.y;
        s->moving = ui->tool == PC_TOOL_SLICE_SELECT;
        s->dragging = !s->moving;
        if (s->moving) {
            const pc_resolved_slice *hit = pc_ui_slice_hit(s, point.x, point.y);
            s->has_selected = hit != NULL;
            s->selected = hit ? hit->number : 0;
            if (!locked && hit && hit->has_id && hit->origin == PC_SLICE_USER) {
                s->dragging = true;
                s->slice = hit->id;
                s->rect = hit->rect;
            }
        }
    } else if (s->dragging) {
        s->current[0] = point.x;
        s->current[1] = point.y;
        if (kind == PC_UI_POINTER_UP) {
            s->dragging = false;
            pc_rect rect = pc_ui_slice_drag_rect(s);
            bool changed =
                s->moving ? !pc_rect_eq(rect, s->rect) : pc_rect_width(rect) > 2 || pc_rect_height(rect) > 2;
            if (changed && !pc_rect_is_empty(rect)) {
                yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
                yyjson_mut_val *params = d ? yyjson_mut_obj(d) : NULL, *r = d ? yyjson_mut_arr(d) : NULL;
                bool ok = params && r && yyjson_mut_arr_add_sint(d, r, rect.x0) &&
                          yyjson_mut_arr_add_sint(d, r, rect.y0) &&
                          yyjson_mut_arr_add_uint(d, r, pc_rect_width(rect)) &&
                          yyjson_mut_arr_add_uint(d, r, pc_rect_height(rect)) &&
                          yyjson_mut_obj_add_val(d, params, "rect", r);
                if (s->moving)
                    ok = ok && yyjson_mut_obj_add_uint(d, params, "slice", s->slice);
                pc_buf text = PC_BUF_INIT;
                status =
                    ok ? pc_json_write(params, 0, &text, err) : pc_fail(err, PC_ERR_NOMEM, "slice command");
                if (status == PC_OK)
                    status = pc_engine_session_execute_json(session, s->moving ? "slice.set" : "slice.new",
                                                            pc_buf_cstr(&text), out, err);
                pc_buf_fini(&text);
                yyjson_mut_doc_free(d);
                if (status == PC_OK) {
                    yyjson_mut_val *n = yyjson_mut_obj_get(pc_json_value_root(out), "number");
                    if (yyjson_mut_is_uint(n)) {
                        s->has_selected = true;
                        s->selected = (size_t)yyjson_mut_get_uint(n);
                    }
                    s->cached = false;
                }
                return status;
            }
            const pc_resolved_slice *hit = pc_ui_slice_hit(s, point.x, point.y);
            s->has_selected = hit != NULL;
            s->selected = hit ? hit->number : 0;
        }
    }
    return result(out, err);
}
