/* Smart Objects and Smart Filters commands: Layer > Smart Objects and Layer > Smart Filter
 * (port of the specs() of crates/engine/src/smart_cmds.rs, E6). The rendering, placement and
 * source primitives live in smart_cmds.c; this file is the command surface on top of them.
 *
 * Landed here: New Smart Object via Copy, Rasterize, and the smart-filter editing commands
 * (disable/delete/blending options/clear/show-hide/edit params/delete/move/transfer). The
 * smart-filter re-render now runs the stack through E9's pc_engine_apply_filter_to_surface
 * (smart_cmds.c apply_smart_filters).
 *
 * Deferred (reported to the coordinator, §7):
 *  - Convert to Smart Object / Convert for Smart Filters (layer_to_smart / layers_to_smart) need
 *    the whole-document compose + .pcraft encode nesting; Convert to Layers needs the full
 *    transform (transform_cmds, E4) and the layer-slice depth converter (image_cmds). -> E4/E3.
 *  - Edit/Save/Replace/Export Contents, Relink, Update*, Convert to Embedded/Linked need file I/O
 *    (E3), the Edit-Contents document links (Session::smart_links) and io linked (I/O). -> E3.
 *  - Recording a filter.* command onto a smart object (add_smart_filter) is wired in
 *    filters_cmds.c::edit_filter (E9's file), outside the subtype hook. -> E9/E12.
 *  - Stack Mode rendering needs the algo stack-combine. -> layer_menu + stack render. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "base/blob.h"
#include "base/json.h"
#include "base/str.h"
#include "color/blend.h"
#include "doc/doc.h"
#include "io/smart_map.h" /* PC_IO_UNSUPPORTED_FILTER */

#include "brush_cmds.h" /* pc_engine_blend_from_str */
#include "engine_internal.h"
#include "layer_cmds.h" /* pc_engine_layer_param */
#include "smart_cmds.h"

/* PSD placed-layer keys that describe the smart object's source; stale once we change it. */
static const uint8_t PLACED_KEYS[3][4] = {{'S', 'o', 'L', 'd'}, {'P', 'l', 'L', 'd'}, {'S', 'o', 'L', 'E'}};

/* Forget PSD placed-layer data that no longer describes the smart object (its source changed). */
static void detach_psd(pc_layer *l)
{
    if (l->content.kind == PC_LAYER_SMART && l->content.u.smart.psd_raw != NULL) {
        pc_blob_release(l->content.u.smart.psd_raw);
        l->content.u.smart.psd_raw = NULL;
    }
    size_t w = 0;
    for (size_t i = 0; i < l->npsd_blocks; i++) {
        bool placed = false;
        for (size_t k = 0; k < 3; k++) {
            if (memcmp(l->psd_blocks[i].key, PLACED_KEYS[k], 4) == 0) {
                placed = true;
                break;
            }
        }
        if (placed) {
            pc_blob_release(l->psd_blocks[i].data);
        } else {
            l->psd_blocks[w++] = l->psd_blocks[i];
        }
    }
    l->npsd_blocks = w;
}

/* ============================================================== enablement predicates */

/* The active layer's smart object (const), or an error message. */
static pc_status active_smart(const pc_engine_session *s, const pc_smart_object **out, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    if (!d->has_active_layer) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(d->doc), d->active_layer);
    if (l == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    if (l->content.kind != PC_LAYER_SMART) {
        return pc_fail(reason, PC_ERR_INVALID, "the active layer is %s %s layer, not a smart object",
                       pc_layer_content_article(&l->content), pc_layer_content_kind_name(&l->content));
    }
    if (out != NULL) {
        *out = &l->content.u.smart;
    }
    return PC_OK;
}

static pc_status has_doc(const pc_engine_session *s, pc_error *reason)
{
    return pc_engine_session_active(s) != NULL ? PC_OK : pc_fail(reason, PC_ERR_INVALID, "no document open");
}

static pc_status has_smart(const pc_engine_session *s, pc_error *reason)
{
    return active_smart(s, NULL, reason);
}

static pc_status has_smart_filters(const pc_engine_session *s, pc_error *reason)
{
    const pc_smart_object *sm = NULL;
    pc_status rc = active_smart(s, &sm, reason);
    if (rc != PC_OK) {
        return rc;
    }
    return sm->nsmart_filters > 0 ? PC_OK
                                  : pc_fail(reason, PC_ERR_INVALID, "the smart object has no smart filters");
}

static pc_status has_filter_mask(const pc_engine_session *s, pc_error *reason)
{
    const pc_smart_object *sm = NULL;
    pc_status rc = active_smart(s, &sm, reason);
    if (rc != PC_OK) {
        return rc;
    }
    return sm->has_filter_mask ? PC_OK
                               : pc_fail(reason, PC_ERR_INVALID, "the smart object has no filter mask");
}

/* ============================================================== smart-filter editing */

enum sf_op {
    SF_SET_VISIBLE,
    SF_SET_PARAMS,
    SF_BLENDING,
    SF_DELETE,
    SF_MOVE,
    SF_CLEAR,
    SF_DISABLE_ALL,
    SF_DELETE_MASK,
    SF_DISABLE_MASK
};

typedef struct sf_ctx {
    enum sf_op op;
    const char *cmd;
    pc_layer_id id;
    bool has_index;
    size_t index;
    bool has_to;
    size_t to;
    bool has_bool;
    bool bval; /* visible / enabled */
    bool has_blend;
    pc_blend_mode blend;
    bool has_opacity;
    float opacity;
    yyjson_val *params; /* SF_SET_PARAMS: the merged-in object */
} sf_ctx;

/* The mutable smart object of layer `id`, or NULL (err filled: no such layer, or not smart). */
static pc_smart_object *smart_mut(pc_document *doc, pc_layer_id id, pc_error *err)
{
    pc_layer *l = pc_document_layer_mut(doc, id);
    if (l == NULL) {
        (void)pc_engine_err_no_layer(err, id);
        return NULL;
    }
    if (l->content.kind != PC_LAYER_SMART) {
        (void)pc_engine_err_other(err, "the layer is not a smart object");
        return NULL;
    }
    return &l->content.u.smart;
}

/* The filter the command names: `index` or, by default, the top one. */
static pc_status filter_index(const char *cmd, const sf_ctx *c, const pc_smart_object *sm, size_t *out,
                              pc_error *err)
{
    size_t n = sm->nsmart_filters;
    if (n == 0) {
        return pc_engine_err_other(err, "the smart object has no smart filters");
    }
    if (!c->has_index) {
        *out = n - 1;
        return PC_OK;
    }
    if (c->index >= n) {
        return pc_engine_err_bad_params(err, cmd, "`index` must be 0..%zu (0 = bottom filter)", n);
    }
    *out = c->index;
    return PC_OK;
}

/* Merge the object `nw` into the smart filter's params (both objects), else replace them. */
static pc_status merge_params(pc_json_value *params, yyjson_val *nw, pc_error *err)
{
    yyjson_mut_val *old = pc_json_value_root(params);
    bool old_obj = old != NULL && yyjson_mut_is_obj(old);
    bool new_obj = nw != NULL && yyjson_is_obj(nw);
    if (!old_obj || !new_obj) {
        pc_json_value tmp;
        if (pc_json_value_from_val(nw, &tmp, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        pc_json_value_fini(params);
        *params = tmp;
        return PC_OK;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (doc == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    pc_status rc = PC_OK;
    if (obj == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    /* keep old keys the new object does not overwrite (function iterators, not the foreach
     * macro, whose empty-object increment confuses the static analyser). */
    yyjson_mut_obj_iter oit = yyjson_mut_obj_iter_with(old);
    yyjson_mut_val *okey;
    while (rc == PC_OK && (okey = yyjson_mut_obj_iter_next(&oit)) != NULL) {
        const char *ks = yyjson_mut_get_str(okey);
        size_t kl = yyjson_mut_get_len(okey);
        if (ks == NULL || yyjson_obj_getn(nw, ks, kl) != NULL) {
            continue;
        }
        yyjson_mut_val *kc = yyjson_mut_strncpy(doc, ks, kl);
        yyjson_mut_val *vc = pc_json_normalized_copy(doc, yyjson_mut_obj_iter_get_val(okey));
        if (kc == NULL || vc == NULL || !yyjson_mut_obj_add(obj, kc, vc)) {
            rc = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
    }
    /* add the new keys */
    yyjson_obj_iter nit;
    (void)yyjson_obj_iter_init(nw, &nit);
    yyjson_val *nkey;
    while (rc == PC_OK && (nkey = yyjson_obj_iter_next(&nit)) != NULL) {
        const char *ks = yyjson_get_str(nkey);
        size_t kl = yyjson_get_len(nkey);
        yyjson_mut_val *kc = yyjson_mut_strncpy(doc, ks, kl);
        yyjson_mut_val *vc = pc_json_normalized_copy_val(doc, yyjson_obj_iter_get_val(nkey));
        if (kc == NULL || vc == NULL || !yyjson_mut_obj_add(obj, kc, vc)) {
            rc = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
    }
    if (rc == PC_OK) {
        pc_json_value merged;
        rc = pc_json_value_from_mut(obj, &merged, err);
        if (rc == PC_OK) {
            pc_json_value_fini(params);
            *params = merged;
        }
    }
    yyjson_mut_doc_free(doc);
    return rc;
}

/* Mutate the named filter(s), then re-render in the same undo step. */
static pc_status sf_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)has_active;
    (void)active;
    sf_ctx *c = vctx;
    pc_smart_object *sm = smart_mut(doc, c->id, err);
    if (sm == NULL) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_status rc = PC_OK;
    size_t i = 0;
    switch (c->op) {
    case SF_SET_VISIBLE:
        rc = filter_index(c->cmd, c, sm, &i, err);
        if (rc != PC_OK) {
            return rc;
        }
        sm->smart_filters[i].visible = c->has_bool ? c->bval : !sm->smart_filters[i].visible;
        break;
    case SF_SET_PARAMS:
        rc = filter_index(c->cmd, c, sm, &i, err);
        if (rc != PC_OK) {
            return rc;
        }
        if (strcmp(pc_str_c(&sm->smart_filters[i].command), PC_IO_UNSUPPORTED_FILTER) == 0) {
            return pc_engine_err_bad_params(
                err, c->cmd,
                "this smart filter isn't implemented in PhotoCraft: it is kept as is (it can be hidden, "
                "moved or deleted)");
        }
        rc = merge_params(&sm->smart_filters[i].params, c->params, err);
        if (rc != PC_OK) {
            return rc;
        }
        break;
    case SF_BLENDING:
        rc = filter_index(c->cmd, c, sm, &i, err);
        if (rc != PC_OK) {
            return rc;
        }
        if (c->has_blend) {
            sm->smart_filters[i].blend = c->blend;
        }
        if (c->has_opacity) {
            float o = c->opacity;
            sm->smart_filters[i].opacity = o < 0.0f ? 0.0f : (o > 1.0f ? 1.0f : o);
        }
        break;
    case SF_DELETE:
        rc = filter_index(c->cmd, c, sm, &i, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_smart_filter_fini(&sm->smart_filters[i]);
        memmove(&sm->smart_filters[i], &sm->smart_filters[i + 1],
                (sm->nsmart_filters - i - 1) * sizeof *sm->smart_filters);
        sm->nsmart_filters--;
        break;
    case SF_MOVE: {
        rc = filter_index(c->cmd, c, sm, &i, err);
        if (rc != PC_OK) {
            return rc;
        }
        size_t n = sm->nsmart_filters;
        if (!c->has_to || c->to >= n) {
            return pc_engine_err_bad_params(err, c->cmd, "`to` must be 0..%zu", n);
        }
        pc_smart_filter moved = sm->smart_filters[i];
        if (c->to < i) {
            memmove(&sm->smart_filters[c->to + 1], &sm->smart_filters[c->to],
                    (i - c->to) * sizeof *sm->smart_filters);
        } else if (c->to > i) {
            memmove(&sm->smart_filters[i], &sm->smart_filters[i + 1],
                    (c->to - i) * sizeof *sm->smart_filters);
        }
        sm->smart_filters[c->to] = moved;
        break;
    }
    case SF_CLEAR:
        for (size_t j = 0; j < sm->nsmart_filters; j++) {
            pc_smart_filter_fini(&sm->smart_filters[j]);
        }
        free(sm->smart_filters);
        sm->smart_filters = NULL;
        sm->nsmart_filters = 0;
        if (sm->has_filter_mask) {
            pc_layer_mask_fini(&sm->filter_mask);
            sm->has_filter_mask = false;
        }
        break;
    case SF_DISABLE_ALL:
        sm->filters_enabled = c->has_bool ? c->bval : !sm->filters_enabled;
        break;
    case SF_DELETE_MASK:
        if (sm->has_filter_mask) {
            pc_layer_mask_fini(&sm->filter_mask);
            sm->has_filter_mask = false;
        }
        break;
    case SF_DISABLE_MASK:
        if (sm->has_filter_mask) {
            sm->filter_mask.enabled = c->has_bool ? c->bval : !sm->filter_mask.enabled;
        }
        break;
    }
    bool updated = false;
    rc = pc_engine_smart_refresh(doc, c->id, &updated, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (!updated) {
        return pc_engine_err_other(
            err, "the smart object's contents are unavailable (missing linked file?), so it can't be "
                 "re-rendered");
    }
    return PC_OK;
}

/* Result {"layer": id}. */
static pc_status result_layer(pc_json_value *out, pc_layer_id id, pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, o);
    (void)yyjson_mut_obj_add_uint(d, o, "layer", id);
    return PC_OK;
}

/* Read an optional `index` (a non-negative integer) into the ctx. */
static void read_index(sf_ctx *c, yyjson_val *p)
{
    yyjson_val *v = pc_json_get(p, "index");
    uint64_t u;
    if (v != NULL && pc_json_as_u64(v, &u)) {
        c->has_index = true;
        c->index = (size_t)u;
    }
}

static pc_status run_edit(pc_engine_session *s, yyjson_val *p, const char *label, sf_ctx *c,
                          pc_json_value *out, pc_error *err)
{
    pc_status rc = pc_engine_layer_param(s, p, &c->id, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = pc_engine_session_edit(s, label, sf_edit, c, err);
    if (rc != PC_OK) {
        return rc;
    }
    return result_layer(out, c->id, err);
}

static pc_status run_set_visible(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    sf_ctx c;
    memset(&c, 0, sizeof c);
    c.op = SF_SET_VISIBLE;
    c.cmd = "layer.smartFilter.setVisible";
    read_index(&c, p);
    yyjson_val *v = pc_json_get(p, "visible");
    if (v != NULL && pc_json_as_bool(v, &c.bval)) {
        c.has_bool = true;
    }
    return run_edit(s, p, "Smart Filter Visibility", &c, out, err);
}

static pc_status run_set_params(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "layer.smartFilter.setParams";
    yyjson_val *params = pc_json_get(p, "params");
    if (params == NULL) {
        return pc_engine_err_bad_params(err, cmd, "pass `params` (merged into the filter's parameters)");
    }
    sf_ctx c;
    memset(&c, 0, sizeof c);
    c.op = SF_SET_PARAMS;
    c.cmd = cmd;
    c.params = params;
    read_index(&c, p);
    return run_edit(s, p, "Edit Smart Filter", &c, out, err);
}

static pc_status run_blending(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "layer.smartFilter.blendingOptions";
    sf_ctx c;
    memset(&c, 0, sizeof c);
    c.op = SF_BLENDING;
    c.cmd = cmd;
    read_index(&c, p);
    const char *b = pc_json_as_str(pc_json_get(p, "blend"), NULL);
    if (b != NULL) {
        if (!pc_engine_blend_from_str(b, strlen(b), &c.blend) || c.blend == PC_BLEND_PASS_THROUGH) {
            return pc_engine_err_bad_params(err, cmd, "unknown blend mode `%s`", b);
        }
        c.has_blend = true;
    }
    double o;
    if (pc_json_as_f64(pc_json_get(p, "opacity"), &o)) {
        c.opacity = (float)(o > 1.0 ? o / 100.0 : o);
        c.has_opacity = true;
    }
    return run_edit(s, p, "Smart Filter Blending Options", &c, out, err);
}

static pc_status run_delete(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    sf_ctx c;
    memset(&c, 0, sizeof c);
    c.op = SF_DELETE;
    c.cmd = "layer.smartFilter.delete";
    read_index(&c, p);
    return run_edit(s, p, "Delete Smart Filter", &c, out, err);
}

static pc_status run_move(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    sf_ctx c;
    memset(&c, 0, sizeof c);
    c.op = SF_MOVE;
    c.cmd = "layer.smartFilter.move";
    read_index(&c, p);
    uint64_t u;
    if (pc_json_as_u64(pc_json_get(p, "to"), &u)) {
        c.has_to = true;
        c.to = (size_t)u;
    }
    return run_edit(s, p, "Move Smart Filter", &c, out, err);
}

static pc_status run_clear(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    sf_ctx c;
    memset(&c, 0, sizeof c);
    c.op = SF_CLEAR;
    c.cmd = "layer.smartFilter.clearSmartFilters";
    return run_edit(s, p, "Clear Smart Filters", &c, out, err);
}

static pc_status run_disable_all(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    sf_ctx c;
    memset(&c, 0, sizeof c);
    c.op = SF_DISABLE_ALL;
    c.cmd = "layer.smartFilter.disableSmartFilters";
    if (pc_json_as_bool(pc_json_get(p, "enabled"), &c.bval)) {
        c.has_bool = true;
    }
    return run_edit(s, p, "Disable Smart Filters", &c, out, err);
}

static pc_status run_delete_mask(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    sf_ctx c;
    memset(&c, 0, sizeof c);
    c.op = SF_DELETE_MASK;
    c.cmd = "layer.smartFilter.deleteFilterMask";
    return run_edit(s, p, "Delete Filter Mask", &c, out, err);
}

static pc_status run_disable_mask(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    sf_ctx c;
    memset(&c, 0, sizeof c);
    c.op = SF_DISABLE_MASK;
    c.cmd = "layer.smartFilter.disableFilterMask";
    if (pc_json_as_bool(pc_json_get(p, "enabled"), &c.bval)) {
        c.has_bool = true;
    }
    return run_edit(s, p, "Disable Filter Mask", &c, out, err);
}

/* ---- transfer: move or copy filters between two smart objects (#4271) --------------------- */

typedef struct transfer_ctx {
    pc_layer_id from, to;
    bool copy;
    bool whole;  /* all filters (with the mask) */
    size_t index; /* one filter, when !whole */
} transfer_ctx;

static pc_status transfer_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    (void)active;
    transfer_ctx *c = vctx;
    pc_smart_object *src = smart_mut(doc, c->from, err);
    if (src == NULL) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_smart_object *dst = smart_mut(doc, c->to, err);
    if (dst == NULL) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_status rc = PC_OK;
    /* Which filters move: one, or the whole row (with the filter mask when the target has none). */
    size_t add_n = c->whole ? src->nsmart_filters : 1;
    pc_smart_filter *grown =
        realloc(dst->smart_filters, (dst->nsmart_filters + add_n) * sizeof *grown);
    if (grown == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    dst->smart_filters = grown;
    for (size_t j = 0; j < add_n; j++) {
        size_t si = c->whole ? j : c->index;
        pc_smart_filter clone;
        if (pc_smart_filter_clone(&src->smart_filters[si], &clone, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        dst->smart_filters[dst->nsmart_filters++] = clone;
    }
    if (c->whole && !dst->has_filter_mask && src->has_filter_mask) {
        if (pc_layer_mask_clone(&src->filter_mask, &dst->filter_mask, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        dst->has_filter_mask = true;
    }
    bool updated = false;
    rc = pc_engine_smart_refresh(doc, c->to, &updated, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (!updated) {
        return pc_engine_err_other(err, "the smart object's contents are unavailable, so it can't be "
                                        "re-rendered");
    }
    if (c->copy) {
        return PC_OK;
    }
    /* Move: drop the transferred filters from the source and re-render it. */
    src = smart_mut(doc, c->from, err);
    if (src == NULL) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (c->whole) {
        for (size_t j = 0; j < src->nsmart_filters; j++) {
            pc_smart_filter_fini(&src->smart_filters[j]);
        }
        free(src->smart_filters);
        src->smart_filters = NULL;
        src->nsmart_filters = 0;
        if (src->has_filter_mask) {
            pc_layer_mask_fini(&src->filter_mask);
            src->has_filter_mask = false;
        }
    } else {
        pc_smart_filter_fini(&src->smart_filters[c->index]);
        memmove(&src->smart_filters[c->index], &src->smart_filters[c->index + 1],
                (src->nsmart_filters - c->index - 1) * sizeof *src->smart_filters);
        src->nsmart_filters--;
    }
    updated = false;
    rc = pc_engine_smart_refresh(doc, c->from, &updated, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (!updated) {
        return pc_engine_err_other(err, "the smart object's contents are unavailable, so it can't be "
                                        "re-rendered");
    }
    return PC_OK;
}

static pc_status run_transfer(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "layer.smartFilter.transfer";
    transfer_ctx c;
    memset(&c, 0, sizeof c);
    uint64_t u;
    if (!pc_json_as_u64(pc_json_get(p, "from"), &u)) {
        return pc_engine_err_bad_params(err, cmd, "`from` must be a layer id");
    }
    c.from = (pc_layer_id)u;
    if (!pc_json_as_u64(pc_json_get(p, "to"), &u)) {
        return pc_engine_err_bad_params(err, cmd, "`to` must be a layer id");
    }
    c.to = (pc_layer_id)u;
    if (c.from == c.to) {
        return pc_engine_err_bad_params(err, cmd, "the smart filters are already on that layer");
    }
    if (pc_json_get(p, "index") != NULL) {
        return pc_engine_err_bad_params(err, cmd, "name the filter with `filter` (no `filter`: all of them)");
    }
    (void)pc_json_as_bool(pc_json_get(p, "copy"), &c.copy);
    /* Validate against the current source (const) before the edit. */
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_layer *lf = pc_document_layer(doc, c.from);
    const pc_layer *lt = pc_document_layer(doc, c.to);
    if (lf == NULL) {
        return pc_engine_err_no_layer(err, c.from);
    }
    if (lf->content.kind != PC_LAYER_SMART || lt == NULL || lt->content.kind != PC_LAYER_SMART) {
        return pc_engine_err_bad_params(err, cmd, "smart filters can only be dropped on a smart object");
    }
    size_t n = lf->content.u.smart.nsmart_filters;
    if (n == 0) {
        return pc_engine_err_other(err, "the smart object has no smart filters");
    }
    yyjson_val *fv = pc_json_get(p, "filter");
    if (fv == NULL || yyjson_is_null(fv)) {
        c.whole = true;
    } else {
        if (!pc_json_as_u64(fv, &u) || (size_t)u >= n) {
            return pc_engine_err_bad_params(err, cmd, "`filter` must be 0..%zu (0 = bottom filter)", n);
        }
        c.index = (size_t)u;
    }
    const char *label = c.copy ? (c.whole ? "Copy Smart Filters" : "Copy Smart Filter")
                               : (c.whole ? "Move Smart Filters" : "Move Smart Filter");
    pc_status rc = pc_engine_session_edit(s, label, transfer_edit, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *o = yyjson_mut_obj(rd);
    yyjson_mut_doc_set_root(rd, o);
    (void)yyjson_mut_obj_add_uint(rd, o, "from", c.from);
    (void)yyjson_mut_obj_add_uint(rd, o, "to", c.to);
    return PC_OK;
}

/* ============================================================== smart-object commands */

typedef struct via_ctx {
    pc_layer_id id;
    pc_layer_id new_id;
} via_ctx;

static pc_status via_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx, pc_error *err)
{
    via_ctx *c = vctx;
    const pc_layer *src = pc_document_layer(doc, c->id);
    if (src == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    if (src->content.kind != PC_LAYER_SMART) {
        return pc_engine_err_other(err, "the layer is not a smart object");
    }
    pc_layer copy;
    if (pc_layer_duplicate(src, &copy, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    pc_str nm;
    if (pc_document_copy_name(doc, pc_str_c(&src->name), src->name.len, &nm, err) != PC_OK) {
        pc_layer_fini(&copy);
        return PC_ERR_NOMEM;
    }
    pc_str_fini(&copy.name);
    copy.name = nm;
    pc_smart_object *sm = &copy.content.u.smart;
    sm->contents_id = pc_smart_contents_id_fresh();
    /* Independent contents: resolve to embedded bytes and drop the shared PSD uuid. */
    bool found = false;
    pc_str sname = PC_STR_INIT;
    pc_blob *sbytes = NULL;
    if (pc_engine_smart_source_bytes(&doc->metadata, &sm->source, &found, &sname, &sbytes, err) != PC_OK) {
        pc_layer_fini(&copy);
        return PC_ERR_INVALID;
    }
    if (found) {
        pc_str_fini(&sm->source.file_name);
        pc_str_fini(&sm->source.path);
        if (sm->source.bytes != NULL) {
            pc_blob_release(sm->source.bytes);
        }
        sm->source.linked = false;
        sm->source.file_name = sname;
        sm->source.path = (pc_str)PC_STR_INIT;
        sm->source.bytes = sbytes;
    } else {
        pc_str_fini(&sname);
        if (sbytes != NULL) {
            pc_blob_release(sbytes);
        }
    }
    detach_psd(&copy);
    pc_layer_id nid = 0;
    if (pc_document_insert_above(doc, &c->id, &copy, &nid, err) != PC_OK) {
        pc_layer_fini(&copy);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    c->new_id = nid;
    *has_active = true;
    *active = nid;
    return PC_OK;
}

static pc_status run_via_copy(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    via_ctx c;
    memset(&c, 0, sizeof c);
    pc_status rc = pc_engine_layer_param(s, p, &c.id, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = pc_engine_session_edit(s, "New Smart Object via Copy", via_edit, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    return result_layer(out, c.new_id, err);
}

static pc_status run_rasterize(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return pc_engine_session_execute(s, "layer.rasterize.smartObject", p, out, err);
}

/* ============================================================== registry */

const pc_engine_command *pc_engine_smart_specs(size_t *n)
{
    static const char *const SO[] = {"Layer", "Smart Objects"};
    static const char *const SF[] = {"Layer", "Smart Filter"};
    static const pc_engine_command specs[] = {
        {"layer.smartObjects.newSmartObjectViaCopy", "New Smart Object via Copy", SO, 2, NULL,
         "{\"layer\":id?}", has_smart, run_via_copy, true},
        {"layer.smartObjects.rasterize", "Rasterize", SO, 2, NULL, "{\"layer\":id?}", has_smart,
         run_rasterize, true},
        /* Smart filters */
        {"layer.smartFilter.disableSmartFilters", "Disable Smart Filters", SF, 2, NULL,
         "{\"layer\":id?,\"enabled\":bool? (default: toggle)}", has_smart_filters, run_disable_all, true},
        {"layer.smartFilter.deleteFilterMask", "Delete Filter Mask", SF, 2, NULL, "{\"layer\":id?}",
         has_filter_mask, run_delete_mask, true},
        {"layer.smartFilter.disableFilterMask", "Disable Filter Mask", SF, 2, NULL,
         "{\"layer\":id?,\"enabled\":bool? (default: toggle)}", has_filter_mask, run_disable_mask, true},
        {"layer.smartFilter.blendingOptions", "Blending Options\xE2\x80\xA6", SF, 2, NULL,
         "{\"layer\":id?,\"index\":u32? (0 = bottom; default top),\"blend\":str?,\"opacity\":0..1?}",
         has_smart_filters, run_blending, true},
        {"layer.smartFilter.clearSmartFilters", "Clear Smart Filters", SF, 2, NULL, "{\"layer\":id?}",
         has_smart_filters, run_clear, true},
        {"layer.smartFilter.setVisible", "Show/Hide Smart Filter", NULL, 0, NULL,
         "{\"layer\":id?,\"index\":u32?,\"visible\":bool? (default: toggle)}", has_smart_filters,
         run_set_visible, true},
        {"layer.smartFilter.setParams", "Edit Smart Filter", NULL, 0, NULL,
         "{\"layer\":id?,\"index\":u32?,\"params\":{\xE2\x80\xA6} (merged)}", has_smart_filters,
         run_set_params, true},
        {"layer.smartFilter.delete", "Delete Smart Filter", NULL, 0, NULL, "{\"layer\":id?,\"index\":u32?}",
         has_smart_filters, run_delete, true},
        {"layer.smartFilter.move", "Move Smart Filter", NULL, 0, NULL,
         "{\"layer\":id?,\"index\":u32?,\"to\":u32}", has_smart_filters, run_move, true},
        {"layer.smartFilter.transfer", "Move or Copy Smart Filters", NULL, 0, NULL,
         "{\"from\":id,\"to\":id,\"filter\":u32? (default: all, with the filter mask),\"copy\":bool=false}",
         has_doc, run_transfer, true},
    };
    *n = sizeof specs / sizeof specs[0];
    return specs;
}
