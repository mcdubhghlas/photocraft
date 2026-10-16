/* Layer > Align and Layer > Distribute (port of crates/engine/src/layer_multi_cmds.rs:
 * align / distribute / can_align / can_distribute, with the Edge coordinate helpers and the
 * move_layers / with_links / dedup_moves machinery those two commands use).
 *
 * Align moves the selected top-level layers so a chosen edge (or centre) lines up: with two or
 * more layers they align to each other's combined bounds; a single layer aligns to the active
 * selection or, with none, to the canvas (`to` overrides: canvas | selection | layers). Distribute
 * spaces three or more layers evenly -- by an edge/centre, or with equal gaps (Horizontally /
 * Vertically). Layers move by whole pixels through pc_engine_translate_layer (pixels, linked mask,
 * type, smart object) plus pc_engine_translate_vectors (shapes, linked vector masks); linked layers
 * travel together, position-locked layers refuse. Content bounds come from pc_engine_layer_bounds
 * (layer_multi_cmds.c). Pure reads/edits of the open document; never crashes on hostile params. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "align_distribute_cmds.h"
#include "doc/doc.h"
#include "geom/geom.h"
#include "layer_cmds.h"    /* pc_engine_selected / top_level / layer_bounds + engine_internal.h */
#include "raster/surface.h" /* pc_surface_content_bounds (selection bounds) */
#include "vector_cmds.h"   /* pc_engine_translate_vectors */

/* ---- Edge: the six align/distribute axes, doubled so centres stay integral --------------- */

enum edge {
    EDGE_TOP,      /* vertical: top edge */
    EDGE_VCENTER,  /* vertical: vertical centre */
    EDGE_BOTTOM,   /* vertical: bottom edge */
    EDGE_LEFT,     /* horizontal: left edge */
    EDGE_HCENTER,  /* horizontal: horizontal centre */
    EDGE_RIGHT,    /* horizontal: right edge */
};

static bool edge_vertical(enum edge e)
{
    return e == EDGE_TOP || e == EDGE_VCENTER || e == EDGE_BOTTOM;
}

/* The edge's coordinate on `r`, doubled (so a centre is a + b, not (a + b) / 2). */
static int64_t edge_value2(enum edge e, pc_rect r)
{
    int64_t a = edge_vertical(e) ? (int64_t)r.y0 : (int64_t)r.x0;
    int64_t b = edge_vertical(e) ? (int64_t)r.y1 : (int64_t)r.x1;
    switch (e) {
    case EDGE_TOP:
    case EDGE_LEFT:
        return 2 * a;
    case EDGE_BOTTOM:
    case EDGE_RIGHT:
        return 2 * b;
    default: /* EDGE_VCENTER, EDGE_HCENTER */
        return a + b;
    }
}

/* Split a scalar delta into (dx, dy) along the edge's axis. */
static void edge_delta(enum edge e, int32_t d, int32_t *dx, int32_t *dy)
{
    if (edge_vertical(e)) {
        *dx = 0;
        *dy = d;
    } else {
        *dx = d;
        *dy = 0;
    }
}

/* Euclidean division by 2 (floor; matches Rust i64::div_euclid(2)). */
static int64_t floordiv2(int64_t n)
{
    int64_t q = n / 2;
    if ((n % 2) != 0 && n < 0) {
        q--;
    }
    return q;
}

/* Clamp an i64 delta to i32 (Rust `as i32` truncates; a coordinate delta never overflows in
 * practice, but a hostile document could, so saturate instead of invoking UB). */
static int32_t clamp_i32(int64_t v)
{
    if (v > INT32_MAX) {
        return INT32_MAX;
    }
    if (v < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)v;
}

/* ---- bounded_selection: selected top-level layers that have content bounds ---------------- */

typedef struct {
    pc_layer_id id;
    pc_rect r;
} item_t;

/* top_level(sel) filtered to those with content bounds (pc_engine_layer_bounds), bottom-to-top.
 * *out owned (free); *nout the count. */
static pc_status bounded_selection(const pc_document *doc, const pc_layer_id *sel, size_t nsel,
                                   item_t **out, size_t *nout, pc_error *err)
{
    *out = NULL;
    *nout = 0;
    pc_layer_id *top = NULL;
    size_t ntop = 0;
    if (pc_engine_top_level(doc, sel, nsel, &top, &ntop, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    item_t *items = ntop > 0 ? malloc(ntop * sizeof *items) : NULL;
    if (ntop > 0 && items == NULL) {
        free(top);
        return pc_fail(err, PC_ERR_NOMEM, "align: selection");
    }
    size_t k = 0;
    for (size_t i = 0; i < ntop; i++) {
        const pc_layer *l = pc_document_layer(doc, top[i]);
        pc_rect b;
        if (l != NULL && pc_engine_layer_bounds(l, &b)) {
            items[k].id = top[i];
            items[k].r = b;
            k++;
        }
    }
    free(top);
    if (k == 0) {
        free(items);
        items = NULL;
    }
    *out = items;
    *nout = k;
    return PC_OK;
}

/* ---- moves: a growable (layer, dx, dy) list --------------------------------------------- */

typedef struct {
    pc_layer_id id;
    int32_t dx, dy;
} move_t;

typedef struct {
    move_t *v;
    size_t n, cap;
} moves_t;

static bool moves_push(moves_t *m, pc_layer_id id, int32_t dx, int32_t dy)
{
    if (m->n == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 8;
        move_t *nv = realloc(m->v, cap * sizeof *nv);
        if (nv == NULL) {
            return false;
        }
        m->v = nv;
        m->cap = cap;
    }
    m->v[m->n].id = id;
    m->v[m->n].dx = dx;
    m->v[m->n].dy = dy;
    m->n++;
    return true;
}

static bool moves_has(const moves_t *m, pc_layer_id id)
{
    for (size_t i = 0; i < m->n; i++) {
        if (m->v[i].id == id) {
            return true;
        }
    }
    return false;
}

/* with_links(doc, [id]) as a move: push (id, dx, dy), then every layer linked to it (walk order),
 * each carrying the same delta. Best-effort on OOM (returns false). */
static bool push_with_links(const pc_document *doc, moves_t *m, pc_layer_id id, int32_t dx, int32_t dy,
                            pc_error *err)
{
    if (!moves_push(m, id, dx, dy)) {
        return false;
    }
    const pc_layer *base = pc_document_layer(doc, id);
    if (base == NULL || !base->has_link_group) {
        return true;
    }
    uint64_t g = base->link_group;
    pc_walk w;
    if (pc_document_walk(doc, &w, err) != PC_OK) {
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < w.len; i++) {
        const pc_layer *l = w.items[i].layer;
        if (l->id != id && l->has_link_group && l->link_group == g) {
            if (!moves_push(m, l->id, dx, dy)) {
                ok = false;
                break;
            }
        }
    }
    pc_walk_fini(&w);
    return ok;
}

/* dedup_moves: one move per layer (first wins), then keep only layers not nested in another moved
 * layer (top_level). Rewrites *m in place. */
static pc_status dedup_moves(const pc_document *doc, moves_t *m, pc_error *err)
{
    moves_t uniq = {0};
    for (size_t i = 0; i < m->n; i++) {
        if (!moves_has(&uniq, m->v[i].id)) {
            if (!moves_push(&uniq, m->v[i].id, m->v[i].dx, m->v[i].dy)) {
                free(uniq.v);
                return pc_fail(err, PC_ERR_NOMEM, "align: moves");
            }
        }
    }
    pc_layer_id *ids = uniq.n > 0 ? malloc(uniq.n * sizeof *ids) : NULL;
    if (uniq.n > 0 && ids == NULL) {
        free(uniq.v);
        return pc_fail(err, PC_ERR_NOMEM, "align: moves");
    }
    for (size_t i = 0; i < uniq.n; i++) {
        ids[i] = uniq.v[i].id;
    }
    pc_layer_id *keep = NULL;
    size_t nkeep = 0;
    pc_status rc = pc_engine_top_level(doc, ids, uniq.n, &keep, &nkeep, err);
    free(ids);
    if (rc != PC_OK) {
        free(uniq.v);
        return rc;
    }
    moves_t out = {0};
    for (size_t i = 0; i < uniq.n; i++) {
        bool in_keep = false;
        for (size_t j = 0; j < nkeep; j++) {
            if (keep[j] == uniq.v[i].id) {
                in_keep = true;
                break;
            }
        }
        if (in_keep && !moves_push(&out, uniq.v[i].id, uniq.v[i].dx, uniq.v[i].dy)) {
            free(keep);
            free(uniq.v);
            free(out.v);
            return pc_fail(err, PC_ERR_NOMEM, "align: moves");
        }
    }
    free(keep);
    free(uniq.v);
    free(m->v);
    *m = out;
    return PC_OK;
}

/* move_layers: apply the (already-deduped) moves, by whole pixels, refusing position-locked
 * layers. `before` is the document as it was before any move (type/shape re-render context).
 * *moved gets the count of layers that actually shifted. */
static pc_status apply_moves(pc_document *doc, const pc_document *before, const moves_t *m,
                             size_t *moved, pc_error *err)
{
    size_t count = 0;
    for (size_t i = 0; i < m->n; i++) {
        int32_t dx = m->v[i].dx, dy = m->v[i].dy;
        if (dx == 0 && dy == 0) {
            continue;
        }
        pc_locks locks = pc_document_effective_locks(doc, m->v[i].id);
        pc_layer *l = pc_document_layer_mut(doc, m->v[i].id);
        if (l == NULL) {
            return pc_engine_err_no_layer(err, m->v[i].id);
        }
        if (locks.position || locks.all) {
            return pc_engine_err_other(err, "layer \"%s\" is position-locked", pc_str_c(&l->name));
        }
        pc_engine_translate_layer(before, l, dx, dy);
        pc_engine_translate_vectors(before, l, (double)dx, (double)dy);
        count++;
    }
    *moved = count;
    return PC_OK;
}

/* ---- enabled predicates ------------------------------------------------------------------- */

static pc_status can_align(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    pc_layer_id *sel = NULL;
    size_t n = 0;
    (void)pc_engine_doc_state_selected_layers(d, &sel, &n, NULL);
    free(sel);
    /* Photoshop aligns a single layer to the canvas (or an active selection); 2+ to each other. */
    if (n == 0) {
        return pc_fail(reason, PC_ERR_INVALID, "no layer selected");
    }
    return PC_OK;
}

static pc_status can_distribute(const pc_engine_session *s, pc_error *reason)
{
    pc_layer_id *sel = NULL;
    size_t n = 0;
    (void)pc_engine_selected(s, &sel, &n, NULL);
    free(sel);
    if (n >= 3) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "select three or more layers to distribute");
}

/* ---- align -------------------------------------------------------------------------------- */

typedef struct {
    const pc_layer_id *sel;
    size_t nsel;
    enum edge e;
    const char *to;  /* "auto" | "layers" | "selection" | "canvas" */
    const char *cmd; /* the command id, for a bad-`to` message */
    size_t moved;    /* out */
} align_ctx;

static pc_status align_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    (void)active;
    align_ctx *c = vctx;
    item_t *items = NULL;
    size_t nitems = 0;
    pc_status rc = bounded_selection(doc, c->sel, c->nsel, &items, &nitems, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (nitems == 0) {
        free(items);
        return pc_engine_err_other(err, "the selected layers have no content to align");
    }
    /* The rectangle to align to. */
    pc_rect target = PC_RECT_EMPTY;
    bool is_auto = strcmp(c->to, "auto") == 0;
    if (strcmp(c->to, "canvas") == 0) {
        target = pc_document_bounds(doc);
    } else if (strcmp(c->to, "selection") == 0) {
        if (!doc->has_selection) {
            free(items);
            return pc_engine_err_other(err, "there is no selection to align to");
        }
        target = pc_surface_content_bounds(&doc->selection);
    } else if (is_auto || strcmp(c->to, "layers") == 0) {
        if (is_auto && doc->has_selection && c->nsel == 1) {
            /* One layer plus an active selection aligns to the selection bounds. */
            target = pc_surface_content_bounds(&doc->selection);
        } else if (is_auto && !doc->has_selection && c->nsel == 1) {
            /* One layer with no selection aligns to the canvas (else it would align to itself). */
            target = pc_document_bounds(doc);
        } else {
            for (size_t i = 0; i < nitems; i++) {
                target = pc_rect_union(target, items[i].r);
            }
        }
    } else {
        pc_status brc = pc_engine_err_bad_params(err, c->cmd,
                                                 "`to` must be auto|layers|selection|canvas, not `%s`",
                                                 c->to);
        free(items);
        return brc;
    }
    if (pc_rect_is_empty(target)) {
        free(items);
        return pc_engine_err_other(err, "nothing to align to");
    }
    /* before = doc as it is now (pre-move); translate_* re-render type/shapes against it. */
    pc_document before;
    rc = pc_document_clone(doc, &before, err);
    if (rc != PC_OK) {
        free(items);
        return rc;
    }
    int64_t t2 = edge_value2(c->e, target);
    moves_t moves = {0};
    for (size_t i = 0; i < nitems; i++) {
        int64_t d = floordiv2(t2 - edge_value2(c->e, items[i].r));
        int32_t dx, dy;
        edge_delta(c->e, clamp_i32(d), &dx, &dy);
        if (!push_with_links(doc, &moves, items[i].id, dx, dy, err)) {
            rc = PC_ERR_NOMEM;
            goto done;
        }
    }
    rc = dedup_moves(doc, &moves, err);
    if (rc != PC_OK) {
        goto done;
    }
    rc = apply_moves(doc, &before, &moves, &c->moved, err);
done:
    if (rc == PC_ERR_NOMEM && (err == NULL || err->code == PC_OK)) {
        rc = pc_fail(err, PC_ERR_NOMEM, "align: moves");
    }
    free(moves.v);
    free(items);
    pc_document_fini(&before);
    return rc;
}

static pc_status do_align(pc_engine_session *s, yyjson_val *p, pc_json_value *out, enum edge e,
                          const char *cmd, pc_error *err)
{
    pc_layer_id *sel = NULL;
    size_t nsel = 0;
    if (pc_engine_selected(s, &sel, &nsel, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    const char *to = "auto";
    if (p != NULL) {
        yyjson_val *v = pc_json_get(p, "to");
        if (v != NULL && yyjson_is_str(v)) {
            const char *ts = yyjson_get_str(v);
            if (ts != NULL) {
                to = ts;
            }
        }
    }
    align_ctx ctx = {sel, nsel, e, to, cmd, 0};
    pc_status rc = pc_engine_session_edit(s, "Align", align_edit, &ctx, err);
    free(sel);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "moved", ctx.moved);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ---- distribute --------------------------------------------------------------------------- */

/* Distribute modes: edges 0..5 reuse enum edge; the two gap modes get their own codes. */
#define DIST_HORIZONTALLY 6
#define DIST_VERTICALLY 7

typedef struct {
    const pc_layer_id *sel;
    size_t nsel;
    int mode;
    size_t moved; /* out */
} dist_ctx;

/* lo/hi of an item's rect along an axis (v = vertical). */
static int64_t item_lo(pc_rect r, bool v) { return v ? (int64_t)r.y0 : (int64_t)r.x0; }
static int64_t item_hi(pc_rect r, bool v) { return v ? (int64_t)r.y1 : (int64_t)r.x1; }

/* Stable insertion sort by the gap key (centre = lo + hi, then lo) -- matches Rust sort_by_key. */
static void sort_for_gaps(item_t *it, size_t n, bool v)
{
    for (size_t i = 1; i < n; i++) {
        item_t key = it[i];
        int64_t kc = item_lo(key.r, v) + item_hi(key.r, v);
        int64_t kl = item_lo(key.r, v);
        size_t j = i;
        while (j > 0) {
            int64_t jc = item_lo(it[j - 1].r, v) + item_hi(it[j - 1].r, v);
            int64_t jl = item_lo(it[j - 1].r, v);
            if (jc > kc || (jc == kc && jl > kl)) {
                it[j] = it[j - 1];
                j--;
            } else {
                break;
            }
        }
        it[j] = key;
    }
}

/* Stable insertion sort by edge_value2. */
static void sort_for_edges(item_t *it, size_t n, enum edge e)
{
    for (size_t i = 1; i < n; i++) {
        item_t key = it[i];
        int64_t kv = edge_value2(e, key.r);
        size_t j = i;
        while (j > 0 && edge_value2(e, it[j - 1].r) > kv) {
            it[j] = it[j - 1];
            j--;
        }
        it[j] = key;
    }
}

static pc_status distribute_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx,
                                 pc_error *err)
{
    (void)ha;
    (void)active;
    dist_ctx *c = vctx;
    item_t *items = NULL;
    size_t n = 0;
    pc_status rc = bounded_selection(doc, c->sel, c->nsel, &items, &n, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (n < 3) {
        free(items);
        return pc_engine_err_other(err, "distributing needs three or more layers with content");
    }
    pc_document before;
    rc = pc_document_clone(doc, &before, err);
    if (rc != PC_OK) {
        free(items);
        return rc;
    }
    moves_t moves = {0};
    if (c->mode == DIST_HORIZONTALLY || c->mode == DIST_VERTICALLY) {
        bool v = c->mode == DIST_VERTICALLY;
        sort_for_gaps(items, n, v);
        int64_t span = item_hi(items[n - 1].r, v) - item_lo(items[0].r, v);
        int64_t total = 0;
        for (size_t i = 0; i < n; i++) {
            total += item_hi(items[i].r, v) - item_lo(items[i].r, v);
        }
        double gap = (double)(span - total) / (double)(n - 1);
        double cursor = (double)item_lo(items[0].r, v);
        for (size_t i = 0; i < n; i++) {
            int64_t want = (int64_t)round(cursor);
            int32_t d = clamp_i32(want - item_lo(items[i].r, v));
            int32_t dx = v ? 0 : d;
            int32_t dy = v ? d : 0;
            if (!push_with_links(doc, &moves, items[i].id, dx, dy, err)) {
                rc = PC_ERR_NOMEM;
                goto done;
            }
            cursor += (double)(item_hi(items[i].r, v) - item_lo(items[i].r, v)) + gap;
        }
    } else {
        enum edge e = (enum edge)c->mode;
        sort_for_edges(items, n, e);
        double first = (double)edge_value2(e, items[0].r);
        double last = (double)edge_value2(e, items[n - 1].r);
        for (size_t i = 0; i < n; i++) {
            double want2 = first + (last - first) * (double)i / (double)(n - 1);
            int64_t d = (int64_t)round((want2 - (double)edge_value2(e, items[i].r)) / 2.0);
            int32_t dx, dy;
            edge_delta(e, clamp_i32(d), &dx, &dy);
            if (!push_with_links(doc, &moves, items[i].id, dx, dy, err)) {
                rc = PC_ERR_NOMEM;
                goto done;
            }
        }
    }
    rc = dedup_moves(doc, &moves, err);
    if (rc != PC_OK) {
        goto done;
    }
    rc = apply_moves(doc, &before, &moves, &c->moved, err);
done:
    if (rc == PC_ERR_NOMEM && (err == NULL || err->code == PC_OK)) {
        rc = pc_fail(err, PC_ERR_NOMEM, "distribute: moves");
    }
    free(moves.v);
    free(items);
    pc_document_fini(&before);
    return rc;
}

static pc_status do_distribute(pc_engine_session *s, pc_json_value *out, int mode, pc_error *err)
{
    pc_layer_id *sel = NULL;
    size_t nsel = 0;
    if (pc_engine_selected(s, &sel, &nsel, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    dist_ctx ctx = {sel, nsel, mode, 0};
    pc_status rc = pc_engine_session_edit(s, "Distribute", distribute_edit, &ctx, err);
    free(sel);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "moved", ctx.moved);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ---- per-command run wrappers (one spec per axis) ---------------------------------------- */

static pc_status run_align_top(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e)
{ return do_align(s, p, o, EDGE_TOP, "layer.align.topEdges", e); }
static pc_status run_align_vcenter(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e)
{ return do_align(s, p, o, EDGE_VCENTER, "layer.align.verticalCenters", e); }
static pc_status run_align_bottom(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e)
{ return do_align(s, p, o, EDGE_BOTTOM, "layer.align.bottomEdges", e); }
static pc_status run_align_left(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e)
{ return do_align(s, p, o, EDGE_LEFT, "layer.align.leftEdges", e); }
static pc_status run_align_hcenter(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e)
{ return do_align(s, p, o, EDGE_HCENTER, "layer.align.horizontalCenters", e); }
static pc_status run_align_right(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e)
{ return do_align(s, p, o, EDGE_RIGHT, "layer.align.rightEdges", e); }

static pc_status run_dist_top(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e)
{ (void)p; return do_distribute(s, o, EDGE_TOP, e); }
static pc_status run_dist_vcenter(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e)
{ (void)p; return do_distribute(s, o, EDGE_VCENTER, e); }
static pc_status run_dist_bottom(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e)
{ (void)p; return do_distribute(s, o, EDGE_BOTTOM, e); }
static pc_status run_dist_left(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e)
{ (void)p; return do_distribute(s, o, EDGE_LEFT, e); }
static pc_status run_dist_hcenter(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e)
{ (void)p; return do_distribute(s, o, EDGE_HCENTER, e); }
static pc_status run_dist_right(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e)
{ (void)p; return do_distribute(s, o, EDGE_RIGHT, e); }
static pc_status run_dist_horizontally(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e)
{ (void)p; return do_distribute(s, o, DIST_HORIZONTALLY, e); }
static pc_status run_dist_vertically(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e)
{ (void)p; return do_distribute(s, o, DIST_VERTICALLY, e); }

/* ---- registry ----------------------------------------------------------------------------- */

static const char *const MENU_ALIGN[] = {"Layer", "Align"};
static const char *const MENU_DISTRIBUTE[] = {"Layer", "Distribute"};

#define ALIGN_PARAMS                                                                              \
    "{\"to\":\"auto|layers|selection|canvas\"=\"auto\"} (auto: the selection bounds with one "    \
    "layer and an active selection, else the selected layers' bounds)"

static const pc_engine_command SPECS[] = {
    {"layer.align.topEdges", "Top Edges", MENU_ALIGN, 2, NULL, ALIGN_PARAMS, can_align, run_align_top, true},
    {"layer.align.verticalCenters", "Vertical Centers", MENU_ALIGN, 2, NULL, ALIGN_PARAMS, can_align,
     run_align_vcenter, true},
    {"layer.align.bottomEdges", "Bottom Edges", MENU_ALIGN, 2, NULL, ALIGN_PARAMS, can_align,
     run_align_bottom, true},
    {"layer.align.leftEdges", "Left Edges", MENU_ALIGN, 2, NULL, ALIGN_PARAMS, can_align, run_align_left,
     true},
    {"layer.align.horizontalCenters", "Horizontal Centers", MENU_ALIGN, 2, NULL, ALIGN_PARAMS, can_align,
     run_align_hcenter, true},
    {"layer.align.rightEdges", "Right Edges", MENU_ALIGN, 2, NULL, ALIGN_PARAMS, can_align,
     run_align_right, true},
    {"layer.distribute.topEdges", "Top Edges", MENU_DISTRIBUTE, 2, NULL, "{}", can_distribute,
     run_dist_top, true},
    {"layer.distribute.verticalCenters", "Vertical Centers", MENU_DISTRIBUTE, 2, NULL, "{}",
     can_distribute, run_dist_vcenter, true},
    {"layer.distribute.bottomEdges", "Bottom Edges", MENU_DISTRIBUTE, 2, NULL, "{}", can_distribute,
     run_dist_bottom, true},
    {"layer.distribute.leftEdges", "Left Edges", MENU_DISTRIBUTE, 2, NULL, "{}", can_distribute,
     run_dist_left, true},
    {"layer.distribute.horizontalCenters", "Horizontal Centers", MENU_DISTRIBUTE, 2, NULL, "{}",
     can_distribute, run_dist_hcenter, true},
    {"layer.distribute.rightEdges", "Right Edges", MENU_DISTRIBUTE, 2, NULL, "{}", can_distribute,
     run_dist_right, true},
    {"layer.distribute.horizontally", "Horizontally", MENU_DISTRIBUTE, 2, NULL,
     "{} (equal horizontal gaps)", can_distribute, run_dist_horizontally, true},
    {"layer.distribute.vertically", "Vertically", MENU_DISTRIBUTE, 2, NULL, "{} (equal vertical gaps)",
     can_distribute, run_dist_vertically, true},
};

const pc_engine_command *pc_engine_align_distribute_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
