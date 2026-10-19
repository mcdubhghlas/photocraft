/* Multi-layer selection and the structural commands that act on it (port of
 * crates/engine/src/layer_multi_cmds.rs): Select > All Layers / Deselect Layers / Find Layers,
 * Layer > Select Linked Layers / Link Layers / Merge Layers / Group from Layers / Lock Layers /
 * Rename Layer.
 *
 * The selection itself lives in DocState (selected_layers / active_layer, UI state like the active
 * layer): changing it never adds a history step. Every structural change is one history step.
 *
 * E5 scope note: the Align/Distribute/Reverse commands and the translate/move/duplicate/delete
 * helpers depend on sibling chunks not yet ported (commands::translate_layer and layer_copy,
 * vector_cmds translate, background_cmds, clip_group, artboard_cmds); they are deferred to those
 * owners. This module ports the selection core, the structural commands that need none of them,
 * and the cross-module helpers the siblings will call. */
#include <stdlib.h>
#include <string.h>

#include "compose/compose.h"
#include "compose/effects.h" /* pc_compose_has_effects (step_damage) */
#include "doc/tree.h"        /* pc_doc_bytes_eq (step_damage) */
#include "layer_cmds.h"

/* ---- layer-targeting helpers (commands::explicit_layer / layer_param) ---------------------- */

pc_status pc_engine_explicit_layer(const pc_engine_session *s, yyjson_val *p, bool *present, pc_layer_id *out,
                                   pc_error *err)
{
    yyjson_val *v = pc_json_get(p, "layer");
    if (v == NULL || yyjson_is_null(v)) {
        *present = false;
        return PC_OK;
    }
    uint64_t id;
    if (yyjson_is_num(v)) {
        if (!pc_json_as_u64(v, &id)) {
            return pc_engine_err_other(err, "layer id is not a non-negative integer");
        }
        *present = true;
        *out = id;
        return PC_OK;
    }
    size_t len = 0;
    const char *name = pc_json_as_str(v, &len);
    if (name == NULL) {
        return pc_engine_err_other(err, "`layer` must be an id or a name");
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_walk w;
    if (pc_document_walk(doc, &w, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    /* Topmost layer of that name: walk is bottom-to-top, so scan from the top down. */
    bool found = false;
    pc_layer_id fid = 0;
    for (size_t i = w.len; i-- > 0;) {
        if (pc_str_eq_mem(&w.items[i].layer->name, name, len)) {
            fid = w.items[i].layer->id;
            found = true;
            break;
        }
    }
    pc_walk_fini(&w);
    if (!found) {
        return pc_engine_err_other(err, "no layer named \"%.*s\"", (int)len, name);
    }
    *present = true;
    *out = fid;
    return PC_OK;
}

pc_status pc_engine_layer_param(const pc_engine_session *s, yyjson_val *p, pc_layer_id *out, pc_error *err)
{
    bool present = false;
    pc_status rc = pc_engine_explicit_layer(s, p, &present, out, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (present) {
        return PC_OK;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL || !d->has_active_layer) {
        return pc_engine_err_other(err, "no active layer");
    }
    *out = d->active_layer;
    return PC_OK;
}

/* ---- selection state ---------------------------------------------------------------------- */

static bool ids_contain(const pc_layer_id *v, size_t n, pc_layer_id id)
{
    for (size_t i = 0; i < n; i++) {
        if (v[i] == id) {
            return true;
        }
    }
    return false;
}

pc_status pc_engine_selected(const pc_engine_session *s, pc_layer_id **out, size_t *n, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        *out = NULL;
        *n = 0;
        return PC_OK;
    }
    return pc_engine_doc_state_selected_layers(d, out, n, err);
}

pc_status pc_engine_set_selection(pc_engine_session *s, const pc_layer_id *ids, size_t n, bool has_active,
                                  pc_layer_id active, bool has_anchor, pc_layer_id anchor, pc_error *err)
{
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    for (size_t i = 0; i < n; i++) {
        if (pc_document_layer(doc, ids[i]) == NULL) {
            return pc_engine_err_no_layer(err, ids[i]);
        }
    }
    if (has_active && pc_document_layer(doc, active) == NULL) {
        return pc_engine_err_no_layer(err, active);
    }
    /* active = active.or(ids.last()) */
    if (!has_active && n > 0) {
        has_active = true;
        active = ids[n - 1];
    }
    bool push_active = has_active && !ids_contain(ids, n, active);
    size_t newn = n + (push_active ? 1 : 0);
    pc_layer_id *sel = NULL;
    if (newn > 0) {
        sel = malloc(newn * sizeof *sel);
        if (sel == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "set selection");
        }
        if (n > 0) {
            memcpy(sel, ids, n * sizeof *sel);
        }
        if (push_active) {
            sel[n] = active;
        }
    }
    free(st->selected_layers);
    st->selected_layers = sel;
    st->nselected = newn;
    st->has_active_layer = has_active;
    st->active_layer = active;
    if (has_anchor) {
        st->has_anchor = true;
        st->layer_anchor = anchor;
    } else if (has_active) {
        st->has_anchor = true;
        st->layer_anchor = active;
    } else {
        st->has_anchor = false;
    }
    pc_engine_fix_selection(st);
    /* Selecting layers is not an edit: keep a clean document clean. */
    bool clean = st->saved_revision == st->revision;
    st->revision++;
    if (clean) {
        st->saved_revision = st->revision;
    }
    st->has_last_damage = true;
    st->last_damage = PC_RECT_EMPTY;
    return PC_OK;
}

/* After a structural edit, make `ids` the selection (dropping any that no longer exist), which is
 * also what the edit's history state targets (reselect). Best-effort on OOM. */
void pc_engine_reselect(pc_engine_session *s, const pc_layer_id *ids, size_t n, bool has_active,
                        pc_layer_id active)
{
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL) {
        return;
    }
    pc_layer_id *copy = NULL;
    if (n > 0) {
        copy = malloc(n * sizeof *copy);
        if (copy == NULL) {
            return;
        }
        memcpy(copy, ids, n * sizeof *copy);
    }
    free(st->selected_layers);
    st->selected_layers = copy;
    st->nselected = copy != NULL ? n : 0;
    if (has_active) {
        st->has_active_layer = true;
        st->active_layer = active;
        st->has_anchor = true;
        st->layer_anchor = active;
    }
    pc_engine_fix_selection(st);
    pc_layer_target tgt = {0};
    tgt.has_active = st->has_active_layer;
    tgt.active = st->active_layer;
    if (st->nselected > 0) {
        tgt.selected = malloc(st->nselected * sizeof *tgt.selected);
        if (tgt.selected != NULL) {
            memcpy(tgt.selected, st->selected_layers, st->nselected * sizeof *tgt.selected);
            tgt.nselected = st->nselected;
        }
    }
    (void)pc_history_set_current_layers(st->history, &tgt, NULL);
    pc_layer_target_fini(&tgt);
}

/* ---- top-level / link helpers ------------------------------------------------------------- */

pc_status pc_engine_top_level(const pc_document *doc, const pc_layer_id *ids, size_t n, pc_layer_id **out,
                              size_t *nout, pc_error *err)
{
    *out = NULL;
    *nout = 0;
    if (n == 0) {
        return PC_OK;
    }
    pc_walk w;
    if (pc_document_walk(doc, &w, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    /* Selected items with their paths, in walk (bottom-to-top) order. */
    size_t *sel_idx = malloc(w.len * sizeof *sel_idx); /* index into w.items */
    pc_layer_id *res = malloc(n * sizeof *res);
    if ((w.len > 0 && sel_idx == NULL) || res == NULL) {
        free(sel_idx);
        free(res);
        pc_walk_fini(&w);
        return pc_fail(err, PC_ERR_NOMEM, "top level");
    }
    size_t nsel = 0;
    for (size_t i = 0; i < w.len; i++) {
        if (ids_contain(ids, n, w.items[i].layer->id)) {
            sel_idx[nsel++] = i;
        }
    }
    size_t nres = 0;
    for (size_t a = 0; a < nsel; a++) {
        size_t ia = sel_idx[a];
        size_t da = w.items[ia].depth + 1; /* path length = depth + 1 */
        const size_t *pa = &w.paths[w.items[ia].path_start];
        bool has_ancestor = false;
        for (size_t b = 0; b < nsel && !has_ancestor; b++) {
            size_t ib = sel_idx[b];
            size_t db = w.items[ib].depth + 1;
            if (db >= da) {
                continue;
            }
            const size_t *pb = &w.paths[w.items[ib].path_start];
            bool prefix = true;
            for (size_t k = 0; k < db; k++) {
                if (pa[k] != pb[k]) {
                    prefix = false;
                    break;
                }
            }
            if (prefix) {
                has_ancestor = true;
            }
        }
        if (!has_ancestor) {
            res[nres++] = w.items[ia].layer->id;
        }
    }
    free(sel_idx);
    pc_walk_fini(&w);
    if (nres == 0) {
        free(res);
        res = NULL;
    }
    *out = res;
    *nout = nres;
    return PC_OK;
}

/* Sorted-unique link groups of the selected layers. *out owned (free), *n count. */
static pc_status link_groups(const pc_document *doc, const pc_layer_id *ids, size_t n, uint64_t **out,
                             size_t *nout, pc_error *err)
{
    *out = NULL;
    *nout = 0;
    uint64_t *g = n > 0 ? malloc(n * sizeof *g) : NULL;
    if (n > 0 && g == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "link groups");
    }
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        const pc_layer *l = pc_document_layer(doc, ids[i]);
        if (l != NULL && l->has_link_group) {
            g[m++] = l->link_group;
        }
    }
    /* sort + dedup (insertion sort; selections are small) */
    for (size_t i = 1; i < m; i++) {
        uint64_t v = g[i];
        size_t j = i;
        while (j > 0 && g[j - 1] > v) {
            g[j] = g[j - 1];
            j--;
        }
        g[j] = v;
    }
    size_t u = 0;
    for (size_t i = 0; i < m; i++) {
        if (u == 0 || g[u - 1] != g[i]) {
            g[u++] = g[i];
        }
    }
    *out = g;
    *nout = u;
    return PC_OK;
}

static bool groups_contain(const uint64_t *g, size_t n, uint64_t v)
{
    for (size_t i = 0; i < n; i++) {
        if (g[i] == v) {
            return true;
        }
    }
    return false;
}

/* ---- enabled predicates ------------------------------------------------------------------- */

static pc_status has_doc(const pc_engine_session *s, pc_error *reason)
{
    if (pc_engine_session_active(s) != NULL) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "no document open");
}

static pc_status has_layer(const pc_engine_session *s, pc_error *reason)
{
    pc_layer_id *sel = NULL;
    size_t n = 0;
    (void)pc_engine_selected(s, &sel, &n, NULL);
    free(sel);
    if (n == 0) {
        return pc_fail(reason, PC_ERR_INVALID, "no layer selected");
    }
    return PC_OK;
}

static pc_status can_link(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    pc_layer_id *sel = NULL;
    size_t n = 0;
    (void)pc_engine_doc_state_selected_layers(d, &sel, &n, NULL);
    pc_status rc = PC_OK;
    if (n == 0) {
        rc = pc_fail(reason, PC_ERR_INVALID, "no layer selected");
    } else if (n == 1) {
        const pc_layer *l = pc_document_layer(pc_doc_arc_get(d->doc), sel[0]);
        if (l == NULL || !l->has_link_group) {
            rc = pc_fail(reason, PC_ERR_INVALID, "select two or more layers to link");
        }
    }
    free(sel);
    return rc;
}

static pc_status has_linked(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    pc_layer_id *sel = NULL;
    size_t n = 0;
    (void)pc_engine_doc_state_selected_layers(d, &sel, &n, NULL);
    uint64_t *g = NULL;
    size_t ng = 0;
    (void)link_groups(pc_doc_arc_get(d->doc), sel, n, &g, &ng, NULL);
    free(sel);
    free(g);
    if (ng == 0) {
        return pc_fail(reason, PC_ERR_INVALID, "the selected layers are not linked");
    }
    return PC_OK;
}

/* ---- commands ----------------------------------------------------------------------------- */

static pc_status result_selected_count_key(pc_json_value *out, size_t n, const char *key, pc_error *err);

static pc_status result_selected_count(pc_json_value *out, size_t n, pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "selected", n);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status result_selected_ids(pc_json_value *out, const pc_layer_id *ids, size_t n, pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (size_t i = 0; i < n; i++) {
        yyjson_mut_arr_add_uint(d, arr, ids[i]);
    }
    yyjson_mut_obj_add_val(d, root, "selected", arr);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status run_select_all_layers(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_walk w;
    if (pc_document_walk(doc, &w, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_layer_id *ids = w.len > 0 ? malloc(w.len * sizeof *ids) : NULL;
    if (w.len > 0 && ids == NULL) {
        pc_walk_fini(&w);
        return pc_fail(err, PC_ERR_NOMEM, "select all layers");
    }
    size_t n = 0;
    for (size_t i = 0; i < w.len; i++) {
        pc_layer_id id = w.items[i].layer->id;
        if (!pc_document_is_background(doc, id)) {
            ids[n++] = id;
        }
    }
    pc_walk_fini(&w);
    if (n == 0) {
        free(ids);
        return pc_engine_err_other(err, "there are no layers to select");
    }
    bool has_active = d->has_active_layer && ids_contain(ids, n, d->active_layer);
    pc_layer_id active = has_active ? d->active_layer : ids[n - 1];
    pc_status rc = pc_engine_set_selection(s, ids, n, true, active, false, 0, err);
    free(ids);
    if (rc != PC_OK) {
        return rc;
    }
    return result_selected_count(out, n, err);
}

static pc_status run_deselect_layers(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    pc_status rc = pc_engine_set_selection(s, NULL, 0, false, 0, false, 0, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st != NULL) {
        st->has_active_layer = false;
        free(st->selected_layers);
        st->selected_layers = NULL;
        st->nselected = 0;
    }
    return pc_engine_result_null(out, err);
}

/* ASCII lowercase substring test (Rust uses Unicode to_lowercase; names are overwhelmingly ASCII,
 * noted as a deviation in REWRITE_PLAN.md §7). */
static bool ascii_contains_ci(const char *hay, size_t hn, const char *needle, size_t nn)
{
    if (nn == 0) {
        return true;
    }
    if (nn > hn) {
        return false;
    }
    for (size_t i = 0; i + nn <= hn; i++) {
        size_t k = 0;
        while (k < nn) {
            unsigned char a = (unsigned char)hay[i + k];
            unsigned char b = (unsigned char)needle[k];
            if (a >= 'A' && a <= 'Z') {
                a = (unsigned char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (unsigned char)(b - 'A' + 'a');
            }
            if (a != b) {
                break;
            }
            k++;
        }
        if (k == nn) {
            return true;
        }
    }
    return false;
}

static pc_status run_find_layers(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    size_t nlen = 0;
    const char *needle = pc_json_as_str(pc_json_get(p, "name"), &nlen);
    if (needle == NULL) {
        return pc_engine_err_bad_params(err, "select.findLayers", "missing `name`");
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_walk w;
    if (pc_document_walk(doc, &w, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_layer_id *ids = w.len > 0 ? malloc(w.len * sizeof *ids) : NULL;
    if (w.len > 0 && ids == NULL) {
        pc_walk_fini(&w);
        return pc_fail(err, PC_ERR_NOMEM, "find layers");
    }
    size_t n = 0;
    for (size_t i = 0; i < w.len; i++) {
        const pc_str *name = &w.items[i].layer->name;
        if (ascii_contains_ci(pc_str_c(name), strlen(pc_str_c(name)), needle, nlen)) {
            ids[n++] = w.items[i].layer->id;
        }
    }
    pc_walk_fini(&w);
    if (n == 0) {
        free(ids);
        return pc_engine_err_other(err, "no layer name contains \"%.*s\"", (int)nlen, needle);
    }
    pc_status rc = pc_engine_set_selection(s, ids, n, false, 0, false, 0, err);
    if (rc != PC_OK) {
        free(ids);
        return rc;
    }
    rc = result_selected_ids(out, ids, n, err);
    free(ids);
    return rc;
}

static pc_status run_select_linked(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_layer_id *sel = NULL;
    size_t nsel = 0;
    (void)pc_engine_doc_state_selected_layers(d, &sel, &nsel, NULL);
    uint64_t *g = NULL;
    size_t ng = 0;
    (void)link_groups(doc, sel, nsel, &g, &ng, NULL);
    free(sel);
    pc_walk w;
    if (pc_document_walk(doc, &w, err) != PC_OK) {
        free(g);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_layer_id *ids = w.len > 0 ? malloc(w.len * sizeof *ids) : NULL;
    if (w.len > 0 && ids == NULL) {
        free(g);
        pc_walk_fini(&w);
        return pc_fail(err, PC_ERR_NOMEM, "select linked");
    }
    size_t n = 0;
    for (size_t i = 0; i < w.len; i++) {
        const pc_layer *l = w.items[i].layer;
        if (l->has_link_group && groups_contain(g, ng, l->link_group)) {
            ids[n++] = l->id;
        }
    }
    free(g);
    pc_walk_fini(&w);
    bool has_active = d->has_active_layer && ids_contain(ids, n, d->active_layer);
    pc_layer_id active = has_active ? d->active_layer : (n > 0 ? ids[n - 1] : 0);
    pc_status rc =
        pc_engine_set_selection(s, ids, n, n > 0 || has_active, active, n > 0 || has_active, active, err);
    free(ids);
    if (rc != PC_OK) {
        return rc;
    }
    return result_selected_count(out, n, err);
}

/* ---- Link Layers (edit) ------------------------------------------------------------------- */

typedef struct {
    const pc_layer_id *sel;
    size_t n;
    bool linked; /* out: did it link (true) or unlink (false) */
} link_ctx;

static pc_status edit_link(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    link_ctx *ctx = vctx;
    /* Already one link group (or a single linked layer): unlink (Photoshop's toggle). */
    bool first_present = false;
    uint64_t first = 0;
    bool unlink = ctx->n > 0;
    for (size_t i = 0; i < ctx->n; i++) {
        const pc_layer *l = pc_document_layer(doc, ctx->sel[i]);
        bool has = l != NULL && l->has_link_group;
        uint64_t g = has ? l->link_group : 0;
        if (i == 0) {
            first_present = has;
            first = g;
        }
        if (!first_present || !has || g != first) {
            unlink = false;
        }
    }
    if (!first_present) {
        unlink = false;
    }
    bool next_present = false;
    uint64_t next = 0;
    if (!unlink) {
        bool found = false;
        if (pc_document_unused_link_group(doc, &found, &next, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        if (!found) {
            return pc_engine_err_bad_params(err, "layer.linkLayers", "no free link group id is left");
        }
        next_present = true;
    }
    for (size_t i = 0; i < ctx->n; i++) {
        pc_layer *l = pc_document_layer_mut(doc, ctx->sel[i]);
        if (l == NULL) {
            return pc_engine_err_no_layer(err, ctx->sel[i]);
        }
        l->has_link_group = next_present;
        l->link_group = next;
    }
    /* A group left with a single member is no longer a link. */
    pc_walk w;
    if (pc_document_walk(doc, &w, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    for (size_t i = 0; i < w.len; i++) {
        const pc_layer *li = w.items[i].layer;
        if (!li->has_link_group) {
            continue;
        }
        size_t count = 0;
        for (size_t j = 0; j < w.len; j++) {
            if (w.items[j].layer->has_link_group && w.items[j].layer->link_group == li->link_group) {
                count++;
            }
        }
        if (count < 2) {
            pc_layer *m = pc_document_layer_mut(doc, li->id);
            if (m != NULL) {
                m->has_link_group = false;
            }
        }
    }
    pc_walk_fini(&w);
    ctx->linked = !unlink;
    return PC_OK;
}

static pc_status run_link_layers(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    pc_layer_id *sel = NULL;
    size_t n = 0;
    if (pc_engine_selected(s, &sel, &n, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    link_ctx ctx = {sel, n, false};
    pc_status rc = pc_engine_session_edit(s, "Link Layers", edit_link, &ctx, err);
    free(sel);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_bool(d, root, "linked", ctx.linked);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ---- Lock Layers (edit) ------------------------------------------------------------------- */

typedef struct {
    const pc_layer_id *sel;
    size_t n;
    bool has[5]; /* transparency, pixels, position, artboard, all */
    bool val[5];
    size_t given;
} lock_ctx;

static pc_status edit_lock(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    lock_ctx *ctx = vctx;
    bool has[5];
    bool val[5];
    memcpy(has, ctx->has, sizeof has);
    memcpy(val, ctx->val, sizeof val);
    size_t given = ctx->given;
    if (given == 0) {
        /* No explicit locks: toggle "lock all" (on unless every selected layer is already all-locked). */
        bool all_locked = true;
        for (size_t i = 0; i < ctx->n; i++) {
            const pc_layer *l = pc_document_layer(doc, ctx->sel[i]);
            if (l == NULL || !l->locks.all) {
                all_locked = false;
                break;
            }
        }
        has[4] = true;
        val[4] = !all_locked;
    }
    for (size_t i = 0; i < ctx->n; i++) {
        pc_layer *l = pc_document_layer_mut(doc, ctx->sel[i]);
        if (l == NULL) {
            return pc_engine_err_no_layer(err, ctx->sel[i]);
        }
        if (has[0]) {
            l->locks.transparency = val[0];
        }
        if (has[1]) {
            l->locks.pixels = val[1];
        }
        if (has[2]) {
            l->locks.position = val[2];
        }
        if (has[3]) {
            l->locks.artboard = val[3];
        }
        if (has[4]) {
            l->locks.all = val[4];
        }
    }
    return PC_OK;
}

static pc_status run_lock_layers(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_layer_id *sel = NULL;
    size_t n = 0;
    if (pc_engine_selected(s, &sel, &n, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    lock_ctx ctx = {sel, n, {false, false, false, false, false}, {false, false, false, false, false}, 0};
    static const char *const keys[5] = {"transparency", "pixels", "position", "artboard", "all"};
    for (int i = 0; i < 5; i++) {
        yyjson_val *v = pc_json_get(p, keys[i]);
        bool b;
        if (v != NULL && pc_json_as_bool(v, &b)) {
            ctx.has[i] = true;
            ctx.val[i] = b;
            ctx.given++;
        }
    }
    pc_status rc = pc_engine_session_edit(s, "Lock Layers", edit_lock, &ctx, err);
    free(sel);
    if (rc != PC_OK) {
        return rc;
    }
    return result_selected_count_key(out, n, "layers", err);
}

/* ---- Rename Layer (edit) ------------------------------------------------------------------ */

typedef struct {
    pc_layer_id id;
    const char *name;
    size_t len;
} rename_ctx;

static pc_status edit_rename(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    rename_ctx *ctx = vctx;
    pc_layer *l = pc_document_layer_mut(doc, ctx->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, ctx->id);
    }
    pc_str name = {0};
    /* name is a yyjson string: NUL-terminated at len. */
    pc_status rc = pc_str_set_c(&name, ctx->name, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_str_fini(&l->name);
    l->name = name;
    return PC_OK;
}

/* Trim-whitespace emptiness test (Rust: name.trim().is_empty()). */
static bool blank(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != '\v') {
            return false;
        }
    }
    return true;
}

static pc_status run_rename_layer(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    size_t len = 0;
    const char *name = pc_json_as_str(pc_json_get(p, "name"), &len);
    if (name == NULL || blank(name, len)) {
        return pc_engine_err_bad_params(err, "layer.renameLayer", "missing `name`");
    }
    pc_layer_id id;
    if (pc_engine_layer_param(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    rename_ctx ctx = {id, name, len};
    if (pc_engine_session_edit(s, "Rename Layer", edit_rename, &ctx, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return pc_engine_result_null(out, err);
}

/* ---- Group from Layers (edit) ------------------------------------------------------------- */

typedef struct {
    pc_layer_id *ids; /* explicit or selected, raw */
    size_t n;
    const char *name; /* optional, NUL-terminated at len */
    size_t name_len;
    bool has_name;
    pc_layer_id gid; /* out */
} group_ctx;

static pc_status edit_group(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    group_ctx *ctx = vctx;
    pc_layer_id *ids = NULL;
    size_t nids = 0;
    if (pc_engine_top_level(doc, ctx->ids, ctx->n, &ids, &nids, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (nids == 0) {
        free(ids);
        return pc_engine_err_no_layer(err, 0);
    }
    pc_layer_id top = ids[nids - 1];
    pc_str gname = {0};
    pc_status rc;
    if (ctx->has_name) {
        rc = pc_str_set_c(&gname, ctx->name, err);
    } else {
        rc = pc_document_next_layer_name(doc, "Group", &gname, err);
    }
    if (rc != PC_OK) {
        free(ids);
        return rc;
    }
    pc_layer group;
    rc = pc_layer_group(&group, pc_str_c(&gname), NULL, 0, err);
    pc_str_fini(&gname);
    if (rc != PC_OK) {
        free(ids);
        return rc;
    }
    pc_layer_id gid = 0;
    rc = pc_document_insert_above(doc, &top, &group, &gid, err);
    if (rc != PC_OK) {
        pc_layer_fini(&group);
        free(ids);
        return rc;
    }
    /* Move each selected top-level layer into the new group, bottom-to-top. */
    pc_layer *children = calloc(nids, sizeof *children);
    if (children == NULL) {
        free(ids);
        return pc_fail(err, PC_ERR_NOMEM, "group layers");
    }
    size_t nchild = 0;
    for (size_t i = 0; i < nids; i++) {
        rc = pc_document_remove(doc, ids[i], &children[nchild], err);
        if (rc != PC_OK) {
            for (size_t j = 0; j < nchild; j++) {
                pc_layer_fini(&children[j]);
            }
            free(children);
            free(ids);
            return rc;
        }
        nchild++;
    }
    free(ids);
    pc_layer *g = pc_document_layer_mut(doc, gid);
    if (g == NULL || g->content.kind != PC_LAYER_GROUP) {
        for (size_t j = 0; j < nchild; j++) {
            pc_layer_fini(&children[j]);
        }
        free(children);
        return pc_engine_err_no_layer(err, gid);
    }
    g->content.u.group.children = children;
    g->content.u.group.nchildren = nchild;
    /* Refuse an over-deep nesting (leaves document and history untouched on error). */
    size_t depth = 0;
    if (pc_document_max_group_depth(doc, &depth, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (depth > PC_MAX_GROUP_DEPTH) {
        return pc_engine_err_other(err, "Group Layers would nest layers deeper than %d groups",
                                   PC_MAX_GROUP_DEPTH);
    }
    *active = gid;
    ctx->gid = gid;
    return PC_OK;
}

static pc_status run_group_layers(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    bool present = false;
    pc_layer_id one = 0;
    if (pc_engine_explicit_layer(s, p, &present, &one, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_layer_id *ids = NULL;
    size_t n = 0;
    if (present) {
        ids = malloc(sizeof *ids);
        if (ids == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "group layers");
        }
        ids[0] = one;
        n = 1;
    } else {
        if (pc_engine_selected(s, &ids, &n, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
    }
    if (n == 0) {
        free(ids);
        return pc_engine_err_other(err, "no layer selected");
    }
    size_t name_len = 0;
    const char *name = pc_json_as_str(pc_json_get(p, "name"), &name_len);
    group_ctx ctx = {ids, n, name, name_len, name != NULL, 0};
    pc_status rc = pc_engine_session_edit(s, "Group Layers", edit_group, &ctx, err);
    free(ids);
    if (rc != PC_OK) {
        return rc;
    }
    pc_engine_reselect(s, &ctx.gid, 1, true, ctx.gid);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "layer", ctx.gid);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ---- Merge Layers (edit) ------------------------------------------------------------------ */

typedef struct {
    const pc_layer_id *sel;
    size_t n;
    pc_layer_id mid; /* out */
} merge_ctx;

static pc_status edit_merge(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    merge_ctx *ctx = vctx;
    pc_layer_id *ids = NULL;
    size_t nids = 0;
    if (pc_engine_top_level(doc, ctx->sel, ctx->n, &ids, &nids, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (nids < 2) {
        free(ids);
        return pc_engine_err_other(err, "Merge Layers needs two or more layers");
    }
    pc_layer_id top_id = ids[nids - 1];
    const pc_layer *top = pc_document_layer(doc, top_id);
    if (top == NULL) {
        free(ids);
        return pc_engine_err_no_layer(err, top_id);
    }
    pc_str top_name = {0};
    pc_status rc = pc_str_clone(&top->name, &top_name, err);
    if (rc != PC_OK) {
        free(ids);
        return rc;
    }
    pc_locks top_locks = top->locks;
    /* solo = clone of doc with only the visible selected top-level layers. */
    pc_document solo;
    rc = pc_document_clone(doc, &solo, err);
    if (rc != PC_OK) {
        pc_str_fini(&top_name);
        free(ids);
        return rc;
    }
    for (size_t i = 0; i < solo.nlayers; i++) {
        pc_layer_fini(&solo.layers[i]);
    }
    free(solo.layers);
    solo.layers = NULL;
    solo.nlayers = 0;
    pc_layer *arr = calloc(nids, sizeof *arr);
    if (arr == NULL) {
        pc_document_fini(&solo);
        pc_str_fini(&top_name);
        free(ids);
        return pc_fail(err, PC_ERR_NOMEM, "merge layers");
    }
    size_t nvis = 0;
    for (size_t i = 0; i < nids; i++) {
        const pc_layer *l = pc_document_layer(doc, ids[i]);
        if (l != NULL && l->visible) {
            rc = pc_layer_clone(l, &arr[nvis], err);
            if (rc != PC_OK) {
                for (size_t j = 0; j < nvis; j++) {
                    pc_layer_fini(&arr[j]);
                }
                free(arr);
                pc_document_fini(&solo);
                pc_str_fini(&top_name);
                free(ids);
                return rc;
            }
            nvis++;
        }
    }
    solo.layers = arr;
    solo.nlayers = nvis;
    if (nvis == 0) {
        pc_document_fini(&solo);
        pc_str_fini(&top_name);
        free(ids);
        return pc_engine_err_other(err, "the selected layers are all hidden");
    }
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    pc_pixel_format fmt2 = {fmt.mode, fmt.sample, true};
    pc_surface surf;
    rc = pc_engine_composite_layers(&solo, fmt2, NULL, &surf, err);
    pc_document_fini(&solo);
    if (rc != PC_OK) {
        pc_str_fini(&top_name);
        free(ids);
        return rc;
    }
    pc_layer merged;
    pc_layer_content content;
    content.kind = PC_LAYER_RASTER;
    content.u.raster = surf;
    rc = pc_layer_init(&merged, pc_str_c(&top_name), &content, err);
    pc_str_fini(&top_name);
    if (rc != PC_OK) {
        pc_surface_fini(&surf);
        free(ids);
        return rc;
    }
    merged.locks = top_locks;
    pc_layer_id mid = merged.id;
    /* Discard the hidden/lower selected layers, then replace the topmost with the merge. */
    for (size_t i = 0; i + 1 < nids; i++) {
        pc_layer gone;
        if (pc_document_remove(doc, ids[i], &gone, NULL) == PC_OK) {
            pc_layer_fini(&gone);
        }
    }
    pc_layer *slot = pc_document_layer_mut(doc, top_id);
    if (slot == NULL) {
        pc_layer_fini(&merged);
        free(ids);
        return pc_engine_err_no_layer(err, top_id);
    }
    pc_layer_fini(slot);
    *slot = merged;
    free(ids);
    *active = mid;
    ctx->mid = mid;
    return PC_OK;
}

static pc_status run_merge_layers(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    pc_layer_id *sel = NULL;
    size_t n = 0;
    if (pc_engine_selected(s, &sel, &n, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (n < 2) {
        /* One layer: Merge Group for a group, else Merge Down (registered by the Layer-menu chunk;
         * until then those return "unknown command", a graceful error). */
        const pc_engine_doc_state *d = pc_engine_session_active(s);
        bool is_group = false;
        pc_layer_id one = n == 1 ? sel[0] : 0;
        if (n == 1 && d != NULL) {
            const pc_layer *l = pc_document_layer(pc_doc_arc_get(d->doc), one);
            is_group = l != NULL && l->content.kind == PC_LAYER_GROUP;
        }
        free(sel);
        if (is_group) {
            char buf[64];
            (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)one);
            return pc_engine_session_execute_json(s, "layer.mergeGroup", buf, out, err);
        }
        return pc_engine_session_execute_json(s, "layer.mergeDown", "{}", out, err);
    }
    merge_ctx ctx = {sel, n, 0};
    pc_status rc = pc_engine_session_edit(s, "Merge Layers", edit_merge, &ctx, err);
    free(sel);
    if (rc != PC_OK) {
        return rc;
    }
    pc_engine_reselect(s, &ctx.mid, 1, true, ctx.mid);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "layer", ctx.mid);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ---- Align/Distribute content bounds (layer_bounds, used by align/distribute helpers) ------ */

bool pc_engine_layer_bounds(const pc_layer *l, pc_rect *out)
{
    pc_rect r = PC_RECT_EMPTY;
    if (l->content.kind == PC_LAYER_GROUP) {
        const pc_group *g = &l->content.u.group;
        for (size_t i = 0; i < g->nchildren; i++) {
            pc_rect b;
            if (pc_engine_layer_bounds(&g->children[i], &b)) {
                r = pc_rect_is_empty(r) ? b : pc_rect_union(r, b);
            }
        }
    } else {
        const pc_surface *surf = pc_layer_surface(l);
        if (surf != NULL) {
            r = pc_surface_content_bounds(surf);
        }
    }
    if (pc_rect_is_empty(r)) {
        return false;
    }
    *out = r;
    return true;
}

/* ---- step_damage (relocated hook: layer_multi_cmds::step_damage) --------------------------- */

#include "layer_damage.c.inc"

/* ---- registry ----------------------------------------------------------------------------- */

static const char *const MENU_SELECT[] = {"Select"};
static const char *const MENU_LAYER[] = {"Layer"};
static const char *const MENU_LAYER_NEW[] = {"Layer", "New"};

static const pc_engine_command SPECS[] = {
    {"select.allLayers", "All Layers", MENU_SELECT, 1, "Cmd+Alt+A", "{}", has_doc, run_select_all_layers,
     true},
    {"select.deselectLayers", "Deselect Layers", MENU_SELECT, 1, NULL, "{}", has_doc, run_deselect_layers,
     true},
    {"select.findLayers", "Find Layers", MENU_SELECT, 1, "Cmd+Alt+Shift+F",
     "{\"name\":str (case-insensitive substring)}", has_doc, run_find_layers, true},
    {"layer.selectLinkedLayers", "Select Linked Layers", MENU_LAYER, 1, NULL, "{}", has_linked,
     run_select_linked, true},
    {"layer.linkLayers", "Link Layers", MENU_LAYER, 1, NULL,
     "{} (toggles: unlinks when the selection is already one link group)", can_link, run_link_layers, true},
    {"layer.mergeLayers", "Merge Layers", MENU_LAYER, 1, "Cmd+E", "{} (one layer selected: Merge Down)",
     has_layer, run_merge_layers, true},
    {"layer.new.groupFromLayers", "Group from Layers…", MENU_LAYER_NEW, 2, NULL, "{\"name\":str?}", has_layer,
     run_group_layers, true},
    {"layer.lockLayers", "Lock Layers…", MENU_LAYER, 1, NULL,
     "{\"transparency\":bool?,\"pixels\":bool?,\"position\":bool?,\"artboard\":bool?,\"all\":bool?} (none "
     "given: toggle lock all)",
     has_layer, run_lock_layers, true},
    {"layer.renameLayer", "Rename Layer", MENU_LAYER, 1, NULL, "{\"layer\":id?,\"name\":str}", has_layer,
     run_rename_layer, true},
    /* E5-base: Group Layers (Cmd+G) shares group_layers with layer.new.groupFromLayers, exactly as
     * the reference (commands.rs -> layer_multi_cmds::group_layers). */
    {"layer.groupLayers", "Group Layers", MENU_LAYER, 1, "Cmd+G",
     "{\"layer\":id?,\"name\":str?} (no layer: every selected layer)", has_layer, run_group_layers, true},
};

static pc_status result_selected_count_key(pc_json_value *out, size_t n, const char *key, pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, key, n);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

const pc_engine_command *pc_engine_layer_multi_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
