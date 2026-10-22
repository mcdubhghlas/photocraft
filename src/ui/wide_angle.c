/* Filter > Adaptive Wide Angle dialog model. See wide_angle.h. Port of wide_angle_ui.rs. */
#include "ui/wide_angle.h"

#include <math.h>
#include <string.h>

#include "base/json.h"
#include "base/mem.h"

/* ---- params clone / snapshots ---- */

static pc_status wide_clone(const pc_algo_wide_angle *src, pc_algo_wide_angle *dst, pc_error *err)
{
    dst->model = src->model;
    dst->focal_length = src->focal_length;
    dst->crop_factor = src->crop_factor;
    dst->scale = src->scale;
    dst->constraints = NULL;
    dst->n_constraints = 0;
    if (src->n_constraints > 0) {
        dst->constraints = pc_malloc_array(src->n_constraints, sizeof *dst->constraints);
        if (dst->constraints == NULL)
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        memcpy(dst->constraints, src->constraints, src->n_constraints * sizeof *dst->constraints);
        dst->n_constraints = src->n_constraints;
    }
    return PC_OK;
}

static void clear_stack(pc_algo_wide_angle *stack, size_t n)
{
    for (size_t i = 0; i < n; i++)
        pc_algo_wide_angle_fini(&stack[i]);
}

static pc_status push_stack(pc_algo_wide_angle **stack, size_t *n, size_t *cap, pc_algo_wide_angle v,
                            pc_error *err)
{
    void *grown = pc_grow_array(*stack, cap, *n + 1, sizeof **stack);
    if (grown == NULL) {
        pc_algo_wide_angle_fini(&v);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    *stack = grown;
    (*stack)[(*n)++] = v;
    return PC_OK;
}

/* Record the current params as an undo step and clear redo (call before a mutation). */
static pc_status push_undo(pc_ui_wide_dialog *s, pc_error *err)
{
    pc_algo_wide_angle snap;
    pc_status rc = wide_clone(&s->params, &snap, err);
    if (rc != PC_OK)
        return rc;
    clear_stack(s->redo, s->nredo);
    s->nredo = 0;
    return push_stack(&s->undo, &s->nundo, &s->cap_undo, snap, err);
}

/* ---- lifecycle ---- */

pc_status pc_ui_wide_begin(pc_ui_wide_dialog *s, uint64_t key, uint64_t layer, pc_rect frame, pc_error *err)
{
    if (s == NULL)
        return pc_fail(err, PC_ERR_INVALID, "null session");
    pc_ui_wide_fini(s);
    memset(s, 0, sizeof *s);
    s->active = true;
    s->key = key;
    s->layer = layer;
    s->frame = frame;
    pc_algo_wide_angle_default(&s->params);
    s->preview = true;
    return PC_OK;
}

void pc_ui_wide_fini(pc_ui_wide_dialog *s)
{
    if (s == NULL)
        return;
    pc_algo_wide_angle_fini(&s->params);
    clear_stack(s->undo, s->nundo);
    clear_stack(s->redo, s->nredo);
    free(s->undo);
    free(s->redo);
    memset(s, 0, sizeof *s);
}

static pc_status push_constraint(pc_ui_wide_dialog *s, pc_algo_wide_constraint c, pc_error *err)
{
    size_t cap = s->params.n_constraints;
    void *grown = pc_grow_array(s->params.constraints, &cap, s->params.n_constraints + 1,
                                sizeof *s->params.constraints);
    if (grown == NULL)
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    s->params.constraints = grown;
    s->params.constraints[s->params.n_constraints++] = c;
    return PC_OK;
}

/* ---- pointer ---- */

pc_status pc_ui_wide_pointer(pc_ui_wide_dialog *s, pc_ui_pointer_kind kind, double x, double y, double zoom,
                             bool shift, pc_error *err)
{
    if (s == NULL || !s->active)
        return pc_fail(err, PC_ERR_INVALID, "no wide-angle session");
    if (kind == PC_UI_POINTER_CANCEL) {
        s->has_drag = false;
        return PC_OK;
    }
    if (kind != PC_UI_POINTER_UP && (!isfinite(x) || !isfinite(y)))
        return pc_fail(err, PC_ERR_INVALID, "bad wide-angle pointer");
    if (kind == PC_UI_POINTER_DOWN) {
        s->drag_a[0] = s->drag_b[0] = x;
        s->drag_a[1] = s->drag_b[1] = y;
        s->has_drag = true;
        return PC_OK;
    }
    if (kind == PC_UI_POINTER_MOVE) {
        if (s->has_drag) {
            s->drag_b[0] = x;
            s->drag_b[1] = y;
        }
        return PC_OK;
    }
    /* UP */
    if (!s->has_drag)
        return PC_OK;
    s->has_drag = false;
    if (isfinite(x) && isfinite(y)) {
        s->drag_b[0] = x;
        s->drag_b[1] = y;
    }
    double dx = s->drag_a[0] - s->drag_b[0], dy = s->drag_a[1] - s->drag_b[1];
    if (!(zoom > 0.0) || hypot(dx, dy) * zoom <= 6.0)
        return PC_OK; /* too short to be a constraint */
    pc_algo_wide_orientation o = PC_ALGO_WIDE_FREE;
    if (shift)
        o = fabs(s->drag_b[0] - s->drag_a[0]) >= fabs(s->drag_b[1] - s->drag_a[1]) ? PC_ALGO_WIDE_HORIZONTAL
                                                                                   : PC_ALGO_WIDE_VERTICAL;
    return pc_ui_wide_add(s, s->drag_a, s->drag_b, o, err);
}

bool pc_ui_wide_nearest(const pc_ui_wide_dialog *s, double x, double y, double zoom, size_t *out)
{
    if (s == NULL || !(zoom > 0.0))
        return false;
    pc_algo_wide_camera cam;
    pc_algo_wide_camera_init(&cam, &s->params, s->frame);
    bool found = false;
    double best = 10.0;
    for (size_t i = 0; i < s->params.n_constraints; i++) {
        double(*arc)[2] = NULL;
        size_t n = 0;
        if (pc_algo_wide_camera_arc(&cam, s->params.constraints[i].a, s->params.constraints[i].b, 24, &arc,
                                    &n, NULL) != PC_OK)
            continue;
        for (size_t k = 0; k < n; k++) {
            double d = hypot(arc[k][0] - x, arc[k][1] - y) * zoom;
            if (d <= best) {
                best = d;
                *out = i;
                found = true;
            }
        }
        free(arc);
    }
    return found;
}

/* ---- control-channel helpers ---- */

pc_status pc_ui_wide_add(pc_ui_wide_dialog *s, const double a[2], const double b[2],
                         pc_algo_wide_orientation orientation, pc_error *err)
{
    if (s == NULL || a == NULL || b == NULL)
        return pc_fail(err, PC_ERR_INVALID, "null arg");
    if (!isfinite(a[0]) || !isfinite(a[1]) || !isfinite(b[0]) || !isfinite(b[1]))
        return pc_fail(err, PC_ERR_INVALID, "constraint points must be finite");
    pc_status rc = push_undo(s, err);
    if (rc != PC_OK)
        return rc;
    pc_algo_wide_constraint c;
    c.a[0] = a[0];
    c.a[1] = a[1];
    c.b[0] = b[0];
    c.b[1] = b[1];
    c.orientation = orientation;
    return push_constraint(s, c, err);
}

pc_status pc_ui_wide_remove(pc_ui_wide_dialog *s, size_t index, pc_error *err)
{
    if (s == NULL)
        return pc_fail(err, PC_ERR_INVALID, "null session");
    if (index >= s->params.n_constraints)
        return PC_OK;
    pc_status rc = push_undo(s, err);
    if (rc != PC_OK)
        return rc;
    memmove(&s->params.constraints[index], &s->params.constraints[index + 1],
            (s->params.n_constraints - index - 1) * sizeof *s->params.constraints);
    s->params.n_constraints--;
    return PC_OK;
}

pc_status pc_ui_wide_apply_json(pc_ui_wide_dialog *s, yyjson_val *set, pc_error *err)
{
    if (s == NULL)
        return pc_fail(err, PC_ERR_INVALID, "null session");
    if (set == NULL || !yyjson_is_obj(set))
        return pc_fail(err, PC_ERR_INVALID, "set must be an object");
    /* Merge onto the current params by serialising, overlaying and reparsing (ui.set behaviour). */
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (d == NULL)
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    yyjson_mut_val *cur = pc_algo_wide_angle_to_json(d, &s->params);
    if (cur == NULL) {
        yyjson_mut_doc_free(d);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    size_t idx, max;
    yyjson_val *k, *v;
    yyjson_obj_foreach(set, idx, max, k, v)
    {
        yyjson_mut_val *mk = yyjson_mut_strcpy(d, yyjson_get_str(k));
        yyjson_mut_val *mv = yyjson_val_mut_copy(d, v);
        if (mk == NULL || mv == NULL) {
            yyjson_mut_doc_free(d);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        yyjson_mut_obj_put(cur, mk, mv);
    }
    /* Reparse the merged object as WideAngle (round-trips through immutable yyjson). */
    yyjson_mut_doc_set_root(d, cur);
    yyjson_doc *imm = yyjson_mut_doc_imut_copy(d, NULL);
    yyjson_mut_doc_free(d);
    if (imm == NULL)
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    pc_algo_wide_angle merged;
    pc_status rc = pc_algo_wide_angle_from_json(yyjson_doc_get_root(imm), &merged, err);
    yyjson_doc_free(imm);
    if (rc != PC_OK)
        return rc;
    rc = push_undo(s, err);
    if (rc != PC_OK) {
        pc_algo_wide_angle_fini(&merged);
        return rc;
    }
    pc_algo_wide_angle_fini(&s->params);
    s->params = merged;
    return PC_OK;
}

void pc_ui_wide_set_preview(pc_ui_wide_dialog *s, bool on)
{
    if (s != NULL)
        s->preview = on;
}

/* ---- undo / redo ---- */

static bool pop_restore(pc_ui_wide_dialog *s, pc_algo_wide_angle **from, size_t *nfrom,
                        pc_algo_wide_angle **to, size_t *nto, size_t *cap_to)
{
    if (*nfrom == 0)
        return false;
    pc_algo_wide_angle cur;
    if (wide_clone(&s->params, &cur, NULL) != PC_OK)
        return false;
    pc_algo_wide_angle snap = (*from)[--(*nfrom)];
    if (push_stack(to, nto, cap_to, cur, NULL) != PC_OK) {
        (*from)[(*nfrom)++] = snap;
        return false;
    }
    pc_algo_wide_angle_fini(&s->params);
    s->params = snap; /* take ownership */
    s->has_drag = false;
    return true;
}

bool pc_ui_wide_undo(pc_ui_wide_dialog *s)
{
    return s != NULL && pop_restore(s, &s->undo, &s->nundo, &s->redo, &s->nredo, &s->cap_redo);
}

bool pc_ui_wide_redo(pc_ui_wide_dialog *s)
{
    return s != NULL && pop_restore(s, &s->redo, &s->nredo, &s->undo, &s->nundo, &s->cap_undo);
}

/* ---- describe / commit ---- */

pc_status pc_ui_wide_describe(const pc_ui_wide_dialog *s, yyjson_mut_doc *doc, yyjson_mut_val **out,
                             pc_error *err)
{
    if (s == NULL || doc == NULL || out == NULL)
        return pc_fail(err, PC_ERR_INVALID, "null arg");
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_val *params = pc_algo_wide_angle_to_json(doc, &s->params);
    if (obj == NULL || params == NULL)
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    yyjson_mut_obj_add_uint(doc, obj, "layer", s->layer);
    yyjson_mut_obj_add_val(doc, obj, "params", params);
    yyjson_mut_obj_add_real(doc, obj, "residual", s->residual);
    yyjson_mut_obj_add_bool(doc, obj, "preview", s->preview);
    *out = obj;
    return PC_OK;
}

pc_status pc_ui_wide_commit_params(const pc_ui_wide_dialog *s, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                   pc_error *err)
{
    if (s == NULL || doc == NULL || out == NULL)
        return pc_fail(err, PC_ERR_INVALID, "null arg");
    yyjson_mut_val *obj = pc_algo_wide_angle_to_json(doc, &s->params);
    if (obj == NULL)
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    yyjson_mut_obj_add_uint(doc, obj, "layer", s->layer);
    *out = obj;
    return PC_OK;
}
