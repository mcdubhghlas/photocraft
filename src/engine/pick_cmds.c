/* Move tool Auto-Select (port of crates/engine/src/pick_cmds.rs, E10): pick the topmost visible,
 * not fully locked layer with pixels at a canvas point, or every layer a marquee touches. A type
 * layer is hit anywhere inside its rendered text's bounds (#2370). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "base/mem.h"
#include "compose/compose.h"
#include "doc/doc.h"
#include "raster/surface.h"

#include "engine_internal.h"
#include "layer_cmds.h"

/* A growable pc_layer_id list. */
typedef struct {
    pc_layer_id *v;
    size_t n, cap;
} idlist;

static bool idlist_push(idlist *l, pc_layer_id id)
{
    pc_layer_id *g = pc_grow_array(l->v, &l->cap, l->n + 1, sizeof *l->v);
    if (g == NULL) {
        return false;
    }
    l->v = g;
    l->v[l->n++] = id;
    return true;
}

static bool idlist_contains(const idlist *l, pc_layer_id id)
{
    for (size_t i = 0; i < l->n; i++) {
        if (l->v[i] == id) {
            return true;
        }
    }
    return false;
}

/* The artboard of a layer (a top-level group with has_artboard), or NULL. */
static const pc_artboard *artboard_of(const pc_layer *l)
{
    if (l != NULL && l->content.kind == PC_LAYER_GROUP && l->content.u.group.has_artboard) {
        return &l->content.u.group.artboard;
    }
    return NULL;
}

/* Every enclosing layer along `path` (length n) is visible. */
static bool path_visible(const pc_document *doc, const size_t *path, size_t n)
{
    for (size_t k = 1; k <= n; k++) {
        const pc_layer *a = pc_document_layer_at(doc, path, k);
        if (a == NULL || !a->visible) {
            return false;
        }
    }
    return true;
}

/* Layers with visible pixels at (x, y), topmost first. With type_bounds, a type layer is hit
 * anywhere inside its rendered text's bounds. */
static pc_status layers_hit(const pc_document *doc, int32_t x, int32_t y, bool type_bounds, idlist *out,
                            pc_error *err)
{
    if (x == INT32_MAX || y == INT32_MAX) {
        return PC_OK;
    }
    pc_walk w = {0};
    if (pc_document_walk(doc, &w, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    pc_status rc = PC_OK;
    for (size_t ri = w.len; ri-- > 0;) {
        const pc_walk_item *it = &w.items[ri];
        const pc_layer *l = it->layer;
        const size_t *path = &w.paths[it->path_start];
        size_t plen = it->depth + 1;
        if (!path_visible(doc, path, plen)) {
            continue;
        }
        const pc_layer *top = &doc->layers[path[0]];
        const pc_artboard *board = artboard_of(top);
        if (board != NULL && !pc_rect_contains(board->rect, x, y)) {
            continue;
        }
        if (artboard_of(l) != NULL) {
            if (!idlist_push(out, l->id)) {
                rc = pc_fail(err, PC_ERR_NOMEM, "pick");
                break;
            }
            continue;
        }
        if (l->content.kind == PC_LAYER_GROUP || l->content.kind == PC_LAYER_ADJUSTMENT) {
            continue;
        }
        float alpha;
        if (l->content.kind == PC_LAYER_FILL) {
            alpha = 1.0f;
        } else if (type_bounds && l->content.kind == PC_LAYER_TEXT && l->content.u.text.has_cache &&
                   pc_rect_contains(pc_surface_content_bounds(&l->content.u.text.cache), x, y)) {
            alpha = 1.0f;
        } else {
            const pc_surface *s = pc_layer_surface(l);
            if (s != NULL) {
                float c[4];
                pc_surface_rgba(s, x, y, c);
                alpha = c[3];
            } else {
                alpha = 0.0f;
            }
        }
        float mask = l->has_mask ? pc_layer_mask_value(&l->mask, x, y) : 1.0f;
        if (alpha * mask > 0.0f) {
            if (!idlist_push(out, l->id)) {
                rc = pc_fail(err, PC_ERR_NOMEM, "pick");
                break;
            }
        }
    }
    pc_walk_fini(&w);
    return rc;
}

/* The outermost group containing `id` (the layer itself when not grouped; an artboard is not a
 * group here, so Group mode stops at the outermost group on the board). */
static pc_layer_id top_group(const pc_document *doc, pc_layer_id id)
{
    pc_walk w = {0};
    if (pc_document_walk(doc, &w, NULL) != PC_OK) {
        return id;
    }
    pc_layer_id out = id;
    for (size_t i = 0; i < w.len; i++) {
        if (w.items[i].layer->id != id) {
            continue;
        }
        const size_t *path = &w.paths[w.items[i].path_start];
        size_t plen = w.items[i].depth + 1;
        const pc_layer *t = pc_document_layer_at(doc, path, 1);
        size_t depth = (t != NULL && artboard_of(t) != NULL) ? 2 : 1;
        if (depth > plen) {
            depth = plen;
        }
        const pc_layer *g = pc_document_layer_at(doc, path, depth);
        if (g != NULL) {
            out = g->id;
        }
        break;
    }
    pc_walk_fini(&w);
    return out;
}

/* The layer an Auto-Select click picks (clicking through fully locked layers). */
static bool auto_select_target(const pc_document *doc, int32_t x, int32_t y, bool group, pc_layer_id *out)
{
    idlist hits = {0};
    if (layers_hit(doc, x, y, true, &hits, NULL) != PC_OK) {
        free(hits.v);
        return false;
    }
    bool found = false;
    for (size_t i = 0; i < hits.n; i++) {
        if (!pc_document_effective_locks(doc, hits.v[i]).all) {
            *out = group ? top_group(doc, hits.v[i]) : hits.v[i];
            found = true;
            break;
        }
    }
    free(hits.v);
    return found;
}

/* Layers whose content touches rect r, top to bottom (Select > All Layers rules). */
static pc_status layers_in_rect(const pc_document *doc, pc_rect r, bool group, idlist *out, pc_error *err)
{
    if (pc_rect_is_empty(r)) {
        return PC_OK;
    }
    pc_rect canvas = pc_document_bounds(doc);
    pc_walk w = {0};
    if (pc_document_walk(doc, &w, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    pc_status rc = PC_OK;
    for (size_t ri = w.len; ri-- > 0;) {
        const pc_walk_item *it = &w.items[ri];
        const pc_layer *l = it->layer;
        const size_t *path = &w.paths[it->path_start];
        size_t plen = it->depth + 1;
        if (l->content.kind == PC_LAYER_GROUP || l->content.kind == PC_LAYER_ADJUSTMENT ||
            pc_document_is_background(doc, l->id) || pc_document_effective_locks(doc, l->id).all ||
            !path_visible(doc, path, plen)) {
            continue;
        }
        pc_rect b = pc_compose_layer_bounds(l, canvas);
        const pc_artboard *board = artboard_of(&doc->layers[path[0]]);
        if (board != NULL) {
            b = pc_rect_intersect(b, board->rect);
        }
        if (pc_rect_is_empty(b) || pc_rect_is_empty(pc_rect_intersect(b, r))) {
            continue;
        }
        pc_layer_id id = group ? top_group(doc, l->id) : l->id;
        if (!idlist_contains(out, id)) {
            if (!idlist_push(out, id)) {
                rc = pc_fail(err, PC_ERR_NOMEM, "pick");
                break;
            }
        }
    }
    pc_walk_fini(&w);
    return rc;
}

/* ---- commands ----------------------------------------------------------------------------- */

static pc_status en_has_doc(const pc_engine_session *s, pc_error *reason)
{
    return pc_engine_session_active(s) != NULL ? PC_OK : pc_fail(reason, PC_ERR_INVALID, "no document");
}

static bool read_i32_floor(yyjson_val *p, const char *k, int32_t *out)
{
    double v;
    yyjson_val *x = pc_json_get(p, k);
    if (x != NULL && pc_json_as_f64(x, &v)) {
        double f = floor(v);
        if (f < (double)INT32_MIN) {
            f = (double)INT32_MIN;
        } else if (f > (double)INT32_MAX) {
            f = (double)INT32_MAX;
        }
        *out = (int32_t)f;
        return true;
    }
    return false;
}

static pc_status run_pick(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    int32_t x, y;
    if (!read_i32_floor(p, "x", &x)) {
        return pc_engine_err_bad_params(err, "layer.pickAt", "missing `x`");
    }
    if (!read_i32_floor(p, "y", &y)) {
        return pc_engine_err_bad_params(err, "layer.pickAt", "missing `y`");
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);

    bool list = false;
    yyjson_val *lv = pc_json_get(p, "list");
    (void)(lv != NULL && pc_json_as_bool(lv, &list));
    if (list) {
        idlist hits = {0};
        pc_status rc = layers_hit(doc, x, y, true, &hits, err);
        if (rc != PC_OK) {
            free(hits.v);
            return rc;
        }
        yyjson_mut_doc *md = NULL;
        if (pc_engine_result_doc(out, &md, err) != PC_OK) {
            free(hits.v);
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(md);
        yyjson_mut_val *arr = yyjson_mut_arr(md);
        for (size_t i = 0; i < hits.n; i++) {
            const pc_layer *l = pc_document_layer(doc, hits.v[i]);
            if (l == NULL) {
                continue;
            }
            yyjson_mut_val *o = yyjson_mut_obj(md);
            yyjson_mut_obj_add_uint(md, o, "layer", hits.v[i]);
            yyjson_mut_obj_add_strcpy(md, o, "name", pc_str_c(&l->name));
            yyjson_mut_arr_add_val(arr, o);
        }
        yyjson_mut_obj_add_val(md, root, "layers", arr);
        yyjson_mut_doc_set_root(md, root);
        free(hits.v);
        return PC_OK;
    }

    size_t tlen = 0;
    yyjson_val *tv = pc_json_get(p, "target");
    const char *ts = tv != NULL ? pc_json_as_str(tv, &tlen) : NULL;
    bool group = ts != NULL && tlen == 5 && strncmp(ts, "group", 5) == 0;

    pc_layer_id target;
    if (!auto_select_target(doc, x, y, group, &target)) {
        yyjson_mut_doc *md = NULL;
        if (pc_engine_result_doc(out, &md, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(md);
        yyjson_mut_obj_add_null(md, root, "layer");
        yyjson_mut_doc_set_root(md, root);
        return PC_OK;
    }

    bool select = true;
    yyjson_val *sv = pc_json_get(p, "select");
    (void)(sv != NULL && pc_json_as_bool(sv, &select));
    if (select) {
        size_t mlen = 0;
        yyjson_val *mv = pc_json_get(p, "mode");
        const char *mode = mv != NULL ? pc_json_as_str(mv, &mlen) : NULL;
        bool is_replace = mode == NULL || (mlen == 7 && strncmp(mode, "replace", 7) == 0);
        bool keeps = false;
        if (is_replace) {
            pc_layer_id *sel = NULL;
            size_t nsel = 0;
            if (pc_engine_doc_state_selected_layers(d, &sel, &nsel, NULL) == PC_OK) {
                bool selected = false;
                for (size_t i = 0; i < nsel; i++) {
                    if (sel[i] == target) {
                        selected = true;
                        break;
                    }
                }
                keeps = selected && nsel > 1;
            }
            free(sel);
        }
        if (!keeps) {
            /* layer.select is not yet ported (E5); apply the replace/add/toggle selection through
             * the underlying primitive instead (pc_engine_set_selection). */
            bool is_add = mode != NULL && mlen == 3 && strncmp(mode, "add", 3) == 0;
            bool is_toggle = mode != NULL && mlen == 6 && strncmp(mode, "toggle", 6) == 0;
            idlist ids = {0};
            if (is_add || is_toggle) {
                pc_layer_id *cur = NULL;
                size_t ncur = 0;
                (void)pc_engine_doc_state_selected_layers(d, &cur, &ncur, NULL);
                bool present = false;
                for (size_t i = 0; i < ncur; i++) {
                    if (cur[i] == target) {
                        present = true;
                        if (is_toggle) {
                            continue; /* toggle removes it */
                        }
                    }
                    if (!idlist_push(&ids, cur[i])) {
                        free(cur);
                        free(ids.v);
                        return pc_fail(err, PC_ERR_NOMEM, "pick");
                    }
                }
                free(cur);
                if (!(is_toggle && present)) {
                    if (!idlist_contains(&ids, target) && !idlist_push(&ids, target)) {
                        free(ids.v);
                        return pc_fail(err, PC_ERR_NOMEM, "pick");
                    }
                }
            } else { /* replace */
                if (!idlist_push(&ids, target)) {
                    free(ids.v);
                    return pc_fail(err, PC_ERR_NOMEM, "pick");
                }
            }
            bool has_active = idlist_contains(&ids, target);
            pc_status rc = pc_engine_set_selection(s, ids.v, ids.n, has_active, target, has_active,
                                                   target, err);
            free(ids.v);
            if (rc != PC_OK) {
                return rc;
            }
        }
    }
    yyjson_mut_doc *md = NULL;
    if (pc_engine_result_doc(out, &md, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(md);
    yyjson_mut_obj_add_uint(md, root, "layer", target);
    yyjson_mut_doc_set_root(md, root);
    return PC_OK;
}

static bool read_finite(yyjson_val *p, const char *k, double *out)
{
    double v;
    yyjson_val *x = pc_json_get(p, k);
    if (x != NULL && pc_json_as_f64(x, &v) && isfinite(v)) {
        *out = v;
        return true;
    }
    return false;
}

static int32_t clamp_half(double v)
{
    double lo = (double)(INT32_MIN / 2), hi = (double)(INT32_MAX / 2);
    if (v < lo) {
        v = lo;
    } else if (v > hi) {
        v = hi;
    }
    return (int32_t)v;
}

static pc_status run_select_in_rect(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    double x, y, wv, hv;
    if (!read_finite(p, "x", &x) || !read_finite(p, "y", &y) || !read_finite(p, "width", &wv) ||
        !read_finite(p, "height", &hv)) {
        return pc_engine_err_bad_params(err, "layer.selectInRect", "missing or invalid coordinate");
    }
    double x0 = x < x + wv ? x : x + wv, x1 = x > x + wv ? x : x + wv;
    double y0 = y < y + hv ? y : y + hv, y1 = y > y + hv ? y : y + hv;
    pc_rect r = {clamp_half(floor(x0)), clamp_half(floor(y0)), clamp_half(ceil(x1)), clamp_half(ceil(y1))};
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    size_t tlen = 0;
    yyjson_val *tv = pc_json_get(p, "target");
    const char *ts = tv != NULL ? pc_json_as_str(tv, &tlen) : NULL;
    bool group = ts != NULL && tlen == 5 && strncmp(ts, "group", 5) == 0;
    size_t mlen = 0;
    yyjson_val *mv = pc_json_get(p, "mode");
    const char *mode = mv != NULL ? pc_json_as_str(mv, &mlen) : NULL;
    bool add;
    if (mode == NULL || (mlen == 7 && strncmp(mode, "replace", 7) == 0)) {
        add = false;
    } else if (mlen == 3 && strncmp(mode, "add", 3) == 0) {
        add = true;
    } else {
        return pc_engine_err_bad_params(err, "layer.selectInRect", "unknown mode `%.*s`", (int)mlen, mode);
    }
    idlist hits = {0};
    pc_status rc = layers_in_rect(doc, r, group, &hits, err);
    if (rc != PC_OK) {
        free(hits.v);
        return rc;
    }
    if (hits.n > 0) {
        idlist ids = {0};
        if (add) {
            pc_layer_id *cur = NULL;
            size_t ncur = 0;
            if (pc_engine_doc_state_selected_layers(d, &cur, &ncur, NULL) == PC_OK) {
                for (size_t i = 0; i < ncur; i++) {
                    if (!idlist_push(&ids, cur[i])) {
                        free(cur);
                        free(ids.v);
                        free(hits.v);
                        return pc_fail(err, PC_ERR_NOMEM, "pick");
                    }
                }
            }
            free(cur);
        }
        for (size_t i = 0; i < hits.n; i++) {
            if (!idlist_contains(&ids, hits.v[i])) {
                if (!idlist_push(&ids, hits.v[i])) {
                    free(ids.v);
                    free(hits.v);
                    return pc_fail(err, PC_ERR_NOMEM, "pick");
                }
            }
        }
        rc = pc_engine_set_selection(s, ids.v, ids.n, true, hits.v[0], true, hits.v[0], err);
        free(ids.v);
        if (rc != PC_OK) {
            free(hits.v);
            return rc;
        }
    }
    yyjson_mut_doc *md = NULL;
    if (pc_engine_result_doc(out, &md, err) != PC_OK) {
        free(hits.v);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(md);
    yyjson_mut_val *arr = yyjson_mut_arr(md);
    for (size_t i = 0; i < hits.n; i++) {
        yyjson_mut_arr_add_uint(md, arr, hits.v[i]);
    }
    yyjson_mut_obj_add_val(md, root, "layers", arr);
    yyjson_mut_doc_set_root(md, root);
    free(hits.v);
    return PC_OK;
}

static const pc_engine_command SPECS[] = {
    {"layer.selectInRect", "Select Layers in Marquee", NULL, 0, NULL,
     "{\"x\":px,\"y\":px,\"width\":px,\"height\":px,\"target\":\"layer|group\"=\"layer\",\"mode\":"
     "\"replace|add\"=\"replace\"} → {layers} (the layers whose content touches the rect, topmost "
     "first; none leaves the selection as it is)",
     en_has_doc, run_select_in_rect, true},
    {"layer.pickAt", "Auto-Select Layer", NULL, 0, NULL,
     "{\"x\":px,\"y\":px,\"target\":\"layer|group\"=\"layer\",\"select\":bool=true,\"mode\":"
     "\"replace|toggle|add\"=\"replace\",\"list\":bool=false (return visible layers at the point, "
     "including text bounds, topmost first)} → {layer} | {layers}",
     en_has_doc, run_pick, false},
};

const pc_engine_command *pc_engine_pick_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
