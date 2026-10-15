/* Single-layer base commands: Layer > New > Group, Duplicate, Delete, Select, Properties, Merge
 * Down / Merge Group, and Layer Mask add/delete. Plus the shared commands::translate_layer and
 * layer_copy helpers E5 deferred to this chunk. Port of the Layer-menu specs in
 * crates/engine/src/commands.rs build(). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "brush_cmds.h"  /* pc_engine_blend_from_str */
#include "doc/tree.h"    /* pc_doc_fini_layers (Merge Group's temp document) */
#include "geom/geom.h"
#include "layer_base_cmds.h"
#include "smart_cmds.h"  /* pc_engine_smart_shift */
#include "type_cmds.h"   /* pc_engine_type_refresh */

/* ---- commands::translate_layer (exported) ------------------------------------------------- */

static void translate_surface_inplace(pc_surface *s, int32_t dx, int32_t dy)
{
    pc_surface out;
    if (pc_surface_translated(s, dx, dy, pc_surface_content_bounds(s), &out, NULL) == PC_OK) {
        pc_surface_fini(s);
        *s = out;
    }
}

void pc_engine_translate_layer(const pc_document *doc, pc_layer *l, int32_t dx, int32_t dy)
{
    if (l == NULL) {
        return;
    }
    if (l->effects.has_reference) {
        l->effects.reference[0] += (double)dx;
        l->effects.reference[1] += (double)dy;
    }
    if (l->has_mask && l->mask.linked) {
        translate_surface_inplace(&l->mask.surface, dx, dy);
    }
    switch (l->content.kind) {
    case PC_LAYER_RASTER:
        translate_surface_inplace(&l->content.u.raster, dx, dy);
        break;
    case PC_LAYER_TEXT: {
        pc_text_layer *t = &l->content.u.text;
        t->transform = pc_affine_mul(pc_affine_translate((double)dx, (double)dy), t->transform);
        if (t->has_cache) {
            /* A whole-pixel move shifts the rendered text as it is (re-rendering would drop type
             * whose font is missing). Faithful TySh-position translation is deferred (§7). */
            translate_surface_inplace(&t->cache, dx, dy);
        } else {
            pc_engine_type_refresh(doc, t);
        }
        break;
    }
    case PC_LAYER_GROUP: {
        pc_group *g = &l->content.u.group;
        if (g->has_artboard) {
            g->artboard.rect = pc_rect_translate(g->artboard.rect, dx, dy);
        }
        for (size_t i = 0; i < g->nchildren; i++) {
            pc_engine_translate_layer(doc, &g->children[i], dx, dy);
        }
        break;
    }
    case PC_LAYER_SMART:
        (void)pc_engine_smart_shift(&l->content.u.smart, dx, dy, NULL);
        break;
    default:
        /* Shapes move through pc_engine_translate_vectors, fill/adjustment have no pixels. */
        break;
    }
}

/* ---- layer_copy (commands::layer_copy) ---------------------------------------------------- */

static pc_status layer_copy(const pc_document *doc, pc_layer_id id, pc_layer *out, pc_error *err)
{
    const pc_layer *src = pc_document_layer(doc, id);
    if (src == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    bool from_background = pc_document_is_background(doc, id);
    if (pc_layer_duplicate(src, out, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_str cn = {0};
    if (pc_document_copy_name(doc, src->name.ptr, src->name.len, &cn, err) != PC_OK) {
        pc_layer_fini(out);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_str_fini(&out->name);
    out->name = cn;
    if (from_background) {
        out->locks = (pc_locks){0};
    }
    return PC_OK;
}

/* ---- shared selection helpers (layer_multi_cmds::targets / mask_targets / multi) ---------- */

static bool has_layer_param(yyjson_val *p)
{
    yyjson_val *v = (p != NULL) ? pc_json_get(p, "layer") : NULL;
    return v != NULL && !yyjson_is_null(v);
}

/* targets(): the explicit `layer`, else the selected layers, else the active layer. */
static pc_status targets(pc_engine_session *s, yyjson_val *p, pc_layer_id **out, size_t *n, pc_error *err)
{
    *out = NULL;
    *n = 0;
    if (has_layer_param(p)) {
        pc_layer_id id;
        if (pc_engine_layer_param(s, p, &id, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        *out = pc_malloc_array(1, sizeof **out);
        if (*out == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "targets");
        }
        (*out)[0] = id;
        *n = 1;
        return PC_OK;
    }
    pc_layer_id *sel = NULL;
    size_t nsel = 0;
    if (pc_engine_selected(s, &sel, &nsel, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (nsel == 0) {
        free(sel);
        pc_layer_id id;
        if (pc_engine_layer_param(s, p, &id, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        *out = pc_malloc_array(1, sizeof **out);
        if (*out == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "targets");
        }
        (*out)[0] = id;
        *n = 1;
        return PC_OK;
    }
    *out = sel;
    *n = nsel;
    return PC_OK;
}

/* mask_targets(): targets, but with two or more selected only those that can take a mask. */
static pc_status mask_targets(pc_engine_session *s, yyjson_val *p, pc_layer_id **out, size_t *n,
                              pc_error *err)
{
    pc_layer_id *ids = NULL;
    size_t nids = 0;
    if (targets(s, p, &ids, &nids, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (nids < 2) {
        *out = ids;
        *n = nids;
        return PC_OK;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        free(ids);
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    size_t keep = 0;
    for (size_t i = 0; i < nids; i++) {
        const pc_layer *l = pc_document_layer(doc, ids[i]);
        if (l != NULL && !l->has_mask && !pc_document_is_background(doc, ids[i])) {
            ids[keep++] = ids[i];
        }
    }
    if (keep == 0) {
        free(ids);
        return pc_engine_err_other(err, "none of the selected layers can take a layer mask");
    }
    *out = ids;
    *n = keep;
    return PC_OK;
}

/* multi(): act on the whole selection (no explicit layer and more than one selected). */
static bool multi(pc_engine_session *s, yyjson_val *p)
{
    if (has_layer_param(p)) {
        return false;
    }
    pc_layer_id *sel = NULL;
    size_t n = 0;
    (void)pc_engine_selected(s, &sel, &n, NULL);
    free(sel);
    return n > 1;
}

/* ---- Layer > New > Group (empty group above the active layer) ----------------------------- */

typedef struct {
    bool have_name;
    char *name;
    pc_layer_id out_id;
} group_ctx;

static pc_status edit_new_group(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    group_ctx *ctx = vctx;
    pc_str nm = {0};
    if (ctx->have_name) {
        if (pc_str_set_c(&nm, ctx->name, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
    } else if (pc_document_next_layer_name(doc, "Group", &nm, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_layer grp;
    pc_status rc = pc_layer_group(&grp, pc_str_c(&nm), NULL, 0, err);
    pc_str_fini(&nm);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_layer_id *above = *ha ? active : NULL;
    pc_layer_id id;
    if (pc_document_insert_above(doc, above, &grp, &id, err) != PC_OK) {
        pc_layer_fini(&grp);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    ctx->out_id = id;
    *ha = true;
    *active = id;
    return PC_OK;
}

static pc_status run_new_group(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    group_ctx ctx = {0};
    yyjson_val *nv = (p != NULL) ? pc_json_get(p, "name") : NULL;
    if (nv != NULL && !yyjson_is_null(nv)) {
        size_t len = 0;
        const char *name = pc_json_as_str(nv, &len);
        if (name == NULL) {
            return pc_engine_err_bad_params(err, "layer.new.group", "`name` must be a string");
        }
        ctx.name = malloc(len + 1);
        if (ctx.name == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "new group");
        }
        memcpy(ctx.name, name, len);
        ctx.name[len] = '\0';
        ctx.have_name = true;
    }
    pc_status rc = pc_engine_session_edit(s, "New Group", edit_new_group, &ctx, err);
    free(ctx.name);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", ctx.out_id);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- Layer > Duplicate Layer -------------------------------------------------------------- */

typedef struct {
    pc_layer_id *sel; /* multi: top-level selection (owned); single: one id */
    size_t n;
    bool have_name;
    char *name;
    bool multi;
    /* outputs */
    pc_layer_id *copies;
    size_t ncopies;
    bool have_active;
    pc_layer_id active;
} dup_ctx;

static pc_status edit_duplicate(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    dup_ctx *ctx = vctx;
    pc_layer_id old_active = (*ha) ? *active : (pc_layer_id)0;
    bool had_active = *ha;
    pc_layer_id *roots = NULL;
    size_t nroots = 0;
    if (ctx->multi) {
        if (pc_engine_top_level(doc, ctx->sel, ctx->n, &roots, &nroots, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
    } else {
        roots = pc_malloc_array(1, sizeof *roots);
        if (roots == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "duplicate");
        }
        roots[0] = ctx->sel[0];
        nroots = 1;
    }
    ctx->copies = nroots > 0 ? pc_malloc_array(nroots, sizeof *ctx->copies) : NULL;
    if (nroots > 0 && ctx->copies == NULL) {
        free(roots);
        return pc_fail(err, PC_ERR_NOMEM, "duplicate");
    }
    bool have_new_active = false;
    pc_layer_id new_active = 0;
    for (size_t i = 0; i < nroots; i++) {
        pc_layer dup;
        if (layer_copy(doc, roots[i], &dup, err) != PC_OK) {
            free(roots);
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        if (!ctx->multi && ctx->have_name) {
            if (pc_str_set_c(&dup.name, ctx->name, err) != PC_OK) {
                pc_layer_fini(&dup);
                free(roots);
                return PC_ERR_NOMEM;
            }
        }
        pc_layer_id nid;
        if (pc_document_insert_above(doc, &roots[i], &dup, &nid, err) != PC_OK) {
            pc_layer_fini(&dup);
            free(roots);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        /* NOTE (§7): Rust duplicate (inPlace=false) offsets an artboard copy beside its board via
         * artboard_cmds::place_copy, which is deferred with the artboard commands (E6-artboard).
         * A non-artboard copy is placed in place either way, so this matches for ordinary layers. */
        if (had_active && roots[i] == old_active) {
            new_active = nid;
            have_new_active = true;
        }
        ctx->copies[ctx->ncopies++] = nid;
    }
    free(roots);
    if (have_new_active) {
        ctx->active = new_active;
        ctx->have_active = true;
    } else if (ctx->ncopies > 0 && ctx->copies != NULL) {
        ctx->active = ctx->copies[ctx->ncopies - 1];
        ctx->have_active = true;
    }
    *ha = ctx->have_active;
    if (ctx->have_active) {
        *active = ctx->active;
    }
    return PC_OK;
}

static pc_status run_duplicate(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    dup_ctx ctx = {0};
    ctx.multi = multi(s, p);
    bool in_place = false;
    yyjson_val *ip = (p != NULL) ? pc_json_get(p, "inPlace") : NULL;
    if (ip != NULL) {
        (void)pc_json_as_bool(ip, &in_place);
    }
    (void)in_place; /* only affects artboard copy placement (deferred, see edit_duplicate §7) */
    if (ctx.multi) {
        if (pc_engine_selected(s, &ctx.sel, &ctx.n, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
    } else {
        pc_layer_id id;
        if (pc_engine_layer_param(s, p, &id, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        ctx.sel = pc_malloc_array(1, sizeof *ctx.sel);
        if (ctx.sel == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "duplicate");
        }
        ctx.sel[0] = id;
        ctx.n = 1;
        yyjson_val *nv = (p != NULL) ? pc_json_get(p, "name") : NULL;
        if (nv != NULL && !yyjson_is_null(nv)) {
            size_t len = 0;
            const char *name = pc_json_as_str(nv, &len);
            if (name != NULL && len > 0) {
                ctx.name = malloc(len + 1);
                if (ctx.name == NULL) {
                    free(ctx.sel);
                    return pc_fail(err, PC_ERR_NOMEM, "duplicate");
                }
                memcpy(ctx.name, name, len);
                ctx.name[len] = '\0';
                ctx.have_name = true;
            }
        }
    }
    const char *label = ctx.multi ? "Duplicate Layers" : "Duplicate Layer";
    pc_status rc = pc_engine_session_edit(s, label, edit_duplicate, &ctx, err);
    if (rc == PC_OK && ctx.have_active) {
        (void)pc_engine_set_selection(s, ctx.copies, ctx.ncopies, true, ctx.active, true, ctx.active, NULL);
    }
    yyjson_mut_doc *rd = NULL;
    if (rc == PC_OK && pc_engine_result_doc(out, &rd, err) == PC_OK) {
        yyjson_mut_val *root = yyjson_mut_obj(rd);
        if (ctx.multi) {
            yyjson_mut_val *arr = yyjson_mut_arr(rd);
            for (size_t i = 0; i < ctx.ncopies; i++) {
                yyjson_mut_arr_add_uint(rd, arr, ctx.copies[i]);
            }
            yyjson_mut_obj_add_val(rd, root, "layers", arr);
            if (ctx.have_active) {
                yyjson_mut_obj_add_uint(rd, root, "layer", ctx.active);
            } else {
                yyjson_mut_obj_add_null(rd, root, "layer");
            }
        } else if (ctx.ncopies > 0) {
            yyjson_mut_obj_add_uint(rd, root, "layer", ctx.copies[0]);
        }
        yyjson_mut_doc_set_root(rd, root);
    } else if (rc == PC_OK) {
        rc = PC_ERR_NOMEM;
    }
    free(ctx.sel);
    free(ctx.copies);
    free(ctx.name);
    return rc;
}

/* ---- Layer > Delete Layer ----------------------------------------------------------------- */

/* deletion_neighbours: next visible row below the target, then rows above in reverse (panel
 * order, group header before children). *out owned. */
static pc_status deletion_neighbours(const pc_document *doc, pc_layer_id target, pc_layer_id **out,
                                     size_t *n, pc_error *err)
{
    *out = NULL;
    *n = 0;
    size_t total = 0;
    (void)pc_document_layer_count(doc, &total, NULL);
    if (total == 0) {
        return PC_OK;
    }
    pc_layer_id *order = pc_malloc_array(total, sizeof *order);
    const pc_layer **stack = pc_malloc_array(total, sizeof *stack);
    if (order == NULL || stack == NULL) {
        free(order);
        free(stack);
        return pc_fail(err, PC_ERR_NOMEM, "deletion neighbours");
    }
    for (int pass = 0; pass < 2; pass++) {
        bool expand_all = pass == 1;
        size_t norder = 0, top = 0;
        for (size_t i = 0; i < doc->nlayers; i++) {
            stack[top++] = &doc->layers[i];
        }
        while (top > 0) {
            const pc_layer *l = stack[--top];
            if (norder < total) {
                order[norder++] = l->id;
            }
            if (pc_layer_is_group(l) && (expand_all || l->content.u.group.expanded)) {
                const pc_group *g = &l->content.u.group;
                for (size_t i = 0; i < g->nchildren && top < total; i++) {
                    stack[top++] = &g->children[i];
                }
            }
        }
        size_t index = norder;
        for (size_t i = 0; i < norder; i++) {
            if (order[i] == target) {
                index = i;
                break;
            }
        }
        if (index == norder) {
            if (!expand_all) {
                continue; /* retry expanding collapsed groups */
            }
            free(order);
            free(stack);
            return PC_OK; /* not found */
        }
        pc_layer_id *res = pc_malloc_array(norder, sizeof *res);
        if (res == NULL) {
            free(order);
            free(stack);
            return pc_fail(err, PC_ERR_NOMEM, "deletion neighbours");
        }
        size_t k = 0;
        for (size_t i = index + 1; i < norder; i++) {
            res[k++] = order[i];
        }
        for (size_t i = index; i-- > 0;) {
            res[k++] = order[i];
        }
        free(order);
        free(stack);
        *out = res;
        *n = k;
        return PC_OK;
    }
    free(order);
    free(stack);
    return PC_OK;
}

typedef struct {
    pc_layer_id *sel; /* multi: selection; single: one id */
    size_t n;
    bool multi;
} del_ctx;

static pc_status edit_delete(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    del_ctx *ctx = vctx;
    pc_layer_id *ids = NULL;
    size_t nids = 0;
    if (ctx->multi) {
        if (pc_engine_top_level(doc, ctx->sel, ctx->n, &ids, &nids, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
    } else {
        ids = pc_malloc_array(1, sizeof *ids);
        if (ids == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "delete");
        }
        ids[0] = ctx->sel[0];
        nids = 1;
    }
    /* Neighbours of the active layer (multi) or the deleted layer (single). */
    pc_layer_id *neighbours = NULL;
    size_t nneighbours = 0;
    pc_layer_id around = ctx->multi ? ((*ha) ? *active : (pc_layer_id)0) : ids[0];
    bool have_around = ctx->multi ? *ha : true;
    if (have_around) {
        if (deletion_neighbours(doc, around, &neighbours, &nneighbours, err) != PC_OK) {
            free(ids);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
    }
    pc_engine_clip_before before;
    if (pc_engine_clip_before_read(doc, ids, nids, &before, err) != PC_OK) {
        free(ids);
        free(neighbours);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    for (size_t i = 0; i < nids; i++) {
        pc_layer removed;
        if (pc_document_remove(doc, ids[i], &removed, err) != PC_OK) {
            pc_layer_id bad = ids[i];
            pc_engine_clip_before_fini(&before);
            free(ids);
            free(neighbours);
            return pc_engine_err_no_layer(err, bad);
        }
        pc_layer_fini(&removed);
    }
    pc_status rc = pc_engine_clip_settle(doc, NULL, 0, &before, false, err);
    pc_engine_clip_before_fini(&before);
    free(ids);
    if (rc != PC_OK) {
        free(neighbours);
        return rc;
    }
    if (doc->nlayers == 0) {
        free(neighbours);
        return pc_engine_err_other(err, "a document must keep at least one layer");
    }
    /* Keep the active layer valid: single always re-picks; multi re-picks when stale. */
    bool stale = !ctx->multi || !(*ha) || pc_document_layer(doc, *active) == NULL;
    if (stale) {
        *ha = false;
        for (size_t i = 0; i < nneighbours; i++) {
            if (pc_document_layer(doc, neighbours[i]) != NULL) {
                *active = neighbours[i];
                *ha = true;
                break;
            }
        }
    }
    free(neighbours);
    return PC_OK;
}

static pc_status run_delete(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    del_ctx ctx = {0};
    ctx.multi = multi(s, p);
    if (ctx.multi) {
        if (pc_engine_selected(s, &ctx.sel, &ctx.n, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
    } else {
        pc_layer_id id;
        if (pc_engine_layer_param(s, p, &id, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        ctx.sel = pc_malloc_array(1, sizeof *ctx.sel);
        if (ctx.sel == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "delete");
        }
        ctx.sel[0] = id;
        ctx.n = 1;
    }
    size_t deleted = ctx.n;
    pc_status rc = pc_engine_session_edit(s, ctx.multi ? "Delete Layers" : "Delete Layer",
                                          edit_delete, &ctx, err);
    free(ctx.sel);
    if (rc != PC_OK) {
        return rc;
    }
    if (ctx.multi) {
        yyjson_mut_doc *rd = NULL;
        if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_uint(rd, root, "deleted", deleted);
        yyjson_mut_doc_set_root(rd, root);
        return PC_OK;
    }
    return pc_engine_result_null(out, err);
}

/* ---- Layer > Select ----------------------------------------------------------------------- */

static bool id_in(const pc_layer_id *a, size_t n, pc_layer_id id)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] == id) {
            return true;
        }
    }
    return false;
}

static void range_order(const pc_layer *layers, size_t n, bool include_collapsed, pc_layer_id *out,
                        size_t cap, size_t *count, size_t depth)
{
    if (depth > PC_MAX_GROUP_DEPTH) {
        return;
    }
    for (size_t i = n; i-- > 0;) {
        if (*count < cap) {
            out[(*count)++] = layers[i].id;
        }
        if (pc_layer_is_group(&layers[i])) {
            const pc_group *g = &layers[i].content.u.group;
            if (g->expanded || include_collapsed) {
                range_order(g->children, g->nchildren, include_collapsed, out, cap, count, depth + 1);
            }
        }
    }
}

static pc_status select_result(pc_engine_session *s, pc_json_value *out, pc_error *err)
{
    pc_layer_id *sel = NULL;
    size_t n = 0;
    if (pc_engine_selected(s, &sel, &n, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        free(sel);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    for (size_t i = 0; i < n; i++) {
        yyjson_mut_arr_add_uint(rd, arr, sel[i]);
    }
    yyjson_mut_obj_add_val(rd, root, "selected", arr);
    yyjson_mut_doc_set_root(rd, root);
    free(sel);
    return PC_OK;
}

static pc_status run_select(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    bool present = false;
    pc_layer_id id = 0;
    if (pc_engine_explicit_layer(s, p, &present, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (!present) {
        return pc_engine_err_bad_params(err, "layer.select", "missing `layer`");
    }
    size_t mlen = 0;
    const char *mode = (p != NULL) ? pc_json_as_str(pc_json_get(p, "mode"), &mlen) : NULL;
    if (mode == NULL) {
        mode = "replace";
        mlen = 7;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (pc_document_layer(doc, id) == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    bool has_active = d->has_active_layer;
    pc_layer_id active = d->active_layer;
    pc_layer_id *sel = NULL;
    size_t nsel = 0;
    if (pc_engine_selected(s, &sel, &nsel, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_status rc = PC_OK;
    if (mlen == 7 && memcmp(mode, "replace", 7) == 0) {
        rc = pc_engine_session_select_layer(s, id, err);
    } else if (mlen == 3 && memcmp(mode, "add", 3) == 0) {
        pc_layer_id *ns = pc_malloc_array(nsel + 1, sizeof *ns);
        if (ns == NULL) {
            free(sel);
            return pc_fail(err, PC_ERR_NOMEM, "select");
        }
        size_t k = 0;
        for (size_t i = 0; i < nsel; i++) {
            ns[k++] = sel[i];
        }
        if (!id_in(sel, nsel, id)) {
            ns[k++] = id;
        }
        rc = pc_engine_set_selection(s, ns, k, true, id, true, id, err);
        free(ns);
    } else if (mlen == 6 && memcmp(mode, "toggle", 6) == 0) {
        if (id_in(sel, nsel, id)) {
            if (nsel == 1) {
                free(sel);
                return select_result(s, out, err); /* keep the clicked layer targeted */
            }
            pc_layer_id *ns = pc_malloc_array(nsel, sizeof *ns);
            if (ns == NULL) {
                free(sel);
                return pc_fail(err, PC_ERR_NOMEM, "select");
            }
            size_t k = 0;
            for (size_t i = 0; i < nsel; i++) {
                if (sel[i] != id) {
                    ns[k++] = sel[i];
                }
            }
            bool na = has_active;
            pc_layer_id av = active;
            if (has_active && active == id) {
                na = k > 0;
                av = k > 0 ? ns[k - 1] : (pc_layer_id)0;
            }
            rc = pc_engine_set_selection(s, ns, k, na, av, na, av, err);
            free(ns);
        } else {
            pc_layer_id *ns = pc_malloc_array(nsel + 1, sizeof *ns);
            if (ns == NULL) {
                free(sel);
                return pc_fail(err, PC_ERR_NOMEM, "select");
            }
            for (size_t i = 0; i < nsel; i++) {
                ns[i] = sel[i];
            }
            ns[nsel] = id;
            rc = pc_engine_set_selection(s, ns, nsel + 1, true, id, true, id, err);
            free(ns);
        }
    } else if (mlen == 6 && memcmp(mode, "remove", 6) == 0) {
        if (id_in(sel, nsel, id) && nsel > 1) {
            pc_layer_id *ns = pc_malloc_array(nsel, sizeof *ns);
            if (ns == NULL) {
                free(sel);
                return pc_fail(err, PC_ERR_NOMEM, "select");
            }
            size_t k = 0;
            for (size_t i = 0; i < nsel; i++) {
                if (sel[i] != id) {
                    ns[k++] = sel[i];
                }
            }
            bool na = has_active;
            pc_layer_id av = active;
            if (has_active && active == id) {
                na = k > 0;
                av = k > 0 ? ns[k - 1] : (pc_layer_id)0;
            }
            rc = pc_engine_set_selection(s, ns, k, na, av, na, av, err);
            free(ns);
        }
        /* else: a layer that isn't selected stays unselected (no change). */
    } else if (mlen == 5 && memcmp(mode, "range", 5) == 0) {
        pc_layer_id anchor;
        if (d->has_anchor && pc_document_layer(doc, d->layer_anchor) != NULL) {
            anchor = d->layer_anchor;
        } else if (has_active) {
            anchor = active;
        } else {
            anchor = id;
        }
        size_t total = 0;
        (void)pc_document_layer_count(doc, &total, NULL);
        /* A document always has at least one layer; allocate at least one slot so the ordering
         * buffer is never NULL. */
        pc_layer_id *order = pc_malloc_array(total > 0 ? total : 1, sizeof *order);
        if (order == NULL) {
            free(sel);
            return pc_fail(err, PC_ERR_NOMEM, "select");
        }
        size_t norder = 0;
        range_order(doc->layers, doc->nlayers, false, order, total, &norder, 0);
        if (!id_in(order, norder, anchor) || !id_in(order, norder, id)) {
            norder = 0;
            range_order(doc->layers, doc->nlayers, true, order, total, &norder, 0);
        }
        size_t a = 0, b = 0;
        for (size_t i = 0; i < norder; i++) {
            if (order[i] == anchor) {
                a = i;
            }
            if (order[i] == id) {
                b = i;
            }
        }
        size_t lo = a < b ? a : b, hi = a < b ? b : a;
        size_t rn = hi - lo + 1;
        rc = pc_engine_set_selection(s, order + lo, rn, true, id, true, anchor, err);
        free(order);
    } else {
        free(sel);
        return pc_engine_err_bad_params(err, "layer.select", "unknown mode `%.*s`", (int)mlen, mode);
    }
    free(sel);
    if (rc != PC_OK) {
        return rc;
    }
    return select_result(s, out, err);
}

/* ---- Layer > Properties (layer.setProps) -------------------------------------------------- */

typedef struct {
    pc_layer_id id;
    yyjson_val *p;
} props_ctx;

static pc_status edit_props(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    props_ctx *ctx = vctx;
    yyjson_val *p = ctx->p;
    pc_layer *l = pc_document_layer_mut(doc, ctx->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, ctx->id);
    }
    yyjson_val *v;
    size_t len;
    if ((v = pc_json_get(p, "name")) != NULL) {
        const char *str = pc_json_as_str(v, &len);
        if (str != NULL && pc_str_set(&l->name, str, len, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
    }
    bool bv;
    if ((v = pc_json_get(p, "visible")) != NULL && pc_json_as_bool(v, &bv)) {
        l->visible = bv;
    }
    double dv;
    if ((v = pc_json_get(p, "opacity")) != NULL && pc_json_as_f64(v, &dv) && isfinite(dv)) {
        l->opacity = (float)(dv < 0.0 ? 0.0 : dv > 1.0 ? 1.0 : dv);
    }
    if ((v = pc_json_get(p, "fill")) != NULL && pc_json_as_f64(v, &dv) && isfinite(dv)) {
        l->fill_opacity = (float)(dv < 0.0 ? 0.0 : dv > 1.0 ? 1.0 : dv);
    }
    if ((v = pc_json_get(p, "blend")) != NULL) {
        const char *str = pc_json_as_str(v, &len);
        pc_blend_mode m;
        if (str == NULL || !pc_engine_blend_from_str(str, len, &m)) {
            return pc_engine_err_bad_params(err, "layer.setProps", "unknown blend mode `%s`",
                                            str != NULL ? str : "");
        }
        l->blend = m;
    }
    if ((v = pc_json_get(p, "clipped")) != NULL && pc_json_as_bool(v, &bv)) {
        l->clipped = bv;
    }
    if ((v = pc_json_get(p, "locked")) != NULL && pc_json_as_bool(v, &bv)) {
        l->locks.all = bv;
    }
    if ((v = pc_json_get(p, "channels")) != NULL && yyjson_is_arr(v)) {
        uint32_t mask = 0;
        size_t idx = 0;
        yyjson_val *item;
        yyjson_arr_iter it;
        yyjson_arr_iter_init(v, &it);
        while ((item = yyjson_arr_iter_next(&it)) != NULL && idx < 32) {
            bool chon;
            if (pc_json_as_bool(item, &chon) && !chon) {
                mask |= (uint32_t)1u << idx;
            }
            idx++;
        }
        l->excluded_channels = mask;
    }
    if ((v = pc_json_get(p, "locks")) != NULL && yyjson_is_obj(v)) {
        yyjson_obj_iter it;
        yyjson_obj_iter_init(v, &it);
        yyjson_val *key;
        while ((key = yyjson_obj_iter_next(&it)) != NULL) {
            yyjson_val *val = yyjson_obj_iter_get_val(key);
            bool on = false;
            (void)pc_json_as_bool(val, &on);
            const char *k = yyjson_get_str(key);
            if (k == NULL) {
                continue;
            }
            if (strcmp(k, "transparency") == 0) {
                l->locks.transparency = on;
            } else if (strcmp(k, "pixels") == 0) {
                l->locks.pixels = on;
            } else if (strcmp(k, "position") == 0) {
                l->locks.position = on;
            } else if (strcmp(k, "artboard") == 0) {
                l->locks.artboard = on;
            } else if (strcmp(k, "all") == 0) {
                l->locks.all = on;
            } else {
                return pc_engine_err_bad_params(err, "layer.setProps", "unknown lock `%s`", k);
            }
        }
    }
    return PC_OK;
}

static pc_status run_set_props(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_layer_id id;
    if (pc_engine_layer_param(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    bool only_visibility = false;
    if (p != NULL && yyjson_is_obj(p) && pc_json_get(p, "visible") != NULL) {
        only_visibility = true;
        yyjson_obj_iter it;
        yyjson_obj_iter_init(p, &it);
        yyjson_val *key;
        while ((key = yyjson_obj_iter_next(&it)) != NULL) {
            const char *k = yyjson_get_str(key);
            if (k == NULL ||
                (strcmp(k, "layer") != 0 && strcmp(k, "visible") != 0 && strcmp(k, "coalesce") != 0)) {
                only_visibility = false;
                break;
            }
        }
    }
    props_ctx ctx = {id, p};
    const char *label = only_visibility ? "Layer Visibility" : "Layer Properties";
    if (pc_engine_session_edit(s, label, edit_props, &ctx, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return pc_engine_result_null(out, err);
}

/* ---- Layer > Merge Down / Merge Group ----------------------------------------------------- */

static pc_status edit_merge_down(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    pc_layer_id id = *(pc_layer_id *)vctx;
    pc_layer_path path;
    if (pc_document_path_of(doc, id, &path, err) != PC_OK) {
        return pc_engine_err_no_layer(err, id);
    }
    if (path.len < 1) {
        pc_layer_path_fini(&path);
        return pc_engine_err_no_layer(err, id);
    }
    size_t idx = path.idx[path.len - 1];
    if (idx == 0) {
        pc_layer_path_fini(&path);
        return pc_engine_err_other(err, "no layer below to merge into");
    }
    size_t *below = pc_malloc_array(path.len, sizeof *below);
    if (below == NULL) {
        pc_layer_path_fini(&path);
        return pc_fail(err, PC_ERR_NOMEM, "merge down");
    }
    for (size_t i = 0; i < path.len; i++) {
        below[i] = path.idx[i];
    }
    below[path.len - 1] = idx - 1;
    size_t below_len = path.len;
    pc_layer_path_fini(&path);

    const pc_layer *lower_ref = pc_document_layer_at(doc, below, below_len);
    const pc_layer *upper_ref = pc_document_layer(doc, id);
    if (lower_ref == NULL || upper_ref == NULL) {
        free(below);
        return pc_engine_err_no_layer(err, id);
    }
    pc_layer_id lower_id = lower_ref->id;
    pc_layer lower, upper, merged;
    if (pc_layer_clone(lower_ref, &lower, err) != PC_OK) {
        free(below);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (pc_layer_clone(upper_ref, &upper, err) != PC_OK) {
        pc_layer_fini(&lower);
        free(below);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_status rc = pc_engine_merge_down(doc, &lower, &upper, &merged, err);
    pc_layer_fini(&lower);
    pc_layer_fini(&upper);
    if (rc != PC_OK) {
        free(below);
        return rc;
    }
    pc_layer removed;
    if (pc_document_remove(doc, id, &removed, err) != PC_OK) {
        pc_layer_fini(&merged);
        free(below);
        return pc_engine_err_no_layer(err, id);
    }
    pc_layer_fini(&removed);
    pc_layer *dst = pc_document_layer_at_mut(doc, below, below_len);
    free(below);
    if (dst == NULL) {
        pc_layer_fini(&merged);
        return pc_engine_err_other(err, "layer tree changed");
    }
    pc_layer_fini(dst);
    *dst = merged;
    *ha = true;
    *active = lower_id;
    return PC_OK;
}

static pc_status run_merge_down(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_layer_id id;
    if (pc_engine_layer_param(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (pc_engine_session_edit(s, "Merge Down", edit_merge_down, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return pc_engine_result_null(out, err);
}

static pc_status edit_merge_group(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    pc_layer_id id = *(pc_layer_id *)vctx;
    const pc_layer *gref = pc_document_layer(doc, id);
    if (gref == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    if (!pc_layer_is_group(gref)) {
        return pc_engine_err_other(err, "Merge Group needs a group");
    }
    if (!gref->visible) {
        return pc_engine_err_other(err, "the group is hidden");
    }
    /* Save the group's identity. */
    pc_layer_id gid = gref->id;
    pc_blend_mode gblend = gref->blend;
    float gopacity = gref->opacity;
    bool gclipped = gref->clipped;
    pc_locks glocks = gref->locks;
    pc_str gname;
    if (pc_str_clone(&gref->name, &gname, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    /* A solo copy of the group at full opacity / Normal / unclipped, in a temp document. */
    pc_layer solo;
    if (pc_layer_clone(gref, &solo, err) != PC_OK) {
        pc_str_fini(&gname);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    solo.opacity = 1.0f;
    solo.blend = PC_BLEND_NORMAL;
    solo.clipped = false;
    pc_document tmp;
    if (pc_document_clone(doc, &tmp, err) != PC_OK) {
        pc_layer_fini(&solo);
        pc_str_fini(&gname);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_doc_fini_layers(tmp.layers, tmp.nlayers);
    tmp.layers = pc_malloc_array(1, sizeof *tmp.layers);
    if (tmp.layers == NULL) {
        tmp.nlayers = 0;
        pc_document_fini(&tmp);
        pc_layer_fini(&solo);
        pc_str_fini(&gname);
        return pc_fail(err, PC_ERR_NOMEM, "merge group");
    }
    tmp.layers[0] = solo; /* moved */
    tmp.nlayers = 1;
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    fmt.alpha = true;
    pc_surface surf;
    pc_status rc = pc_engine_composite_layers(&tmp, fmt, NULL, &surf, err);
    pc_document_fini(&tmp);
    if (rc != PC_OK) {
        pc_str_fini(&gname);
        return rc;
    }
    pc_layer_content content = {.kind = PC_LAYER_RASTER, .u.raster = surf};
    pc_layer merged;
    if (pc_layer_init(&merged, pc_str_c(&gname), &content, err) != PC_OK) {
        pc_layer_content_fini(&content);
        pc_str_fini(&gname);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_str_fini(&gname);
    merged.id = gid;
    merged.opacity = gopacity;
    merged.blend = gblend == PC_BLEND_PASS_THROUGH ? PC_BLEND_NORMAL : gblend;
    merged.clipped = gclipped;
    merged.locks = glocks;
    pc_layer *dst = pc_document_layer_mut(doc, id);
    if (dst == NULL) {
        pc_layer_fini(&merged);
        return pc_engine_err_no_layer(err, id);
    }
    pc_layer_fini(dst);
    *dst = merged;
    *ha = true;
    *active = id;
    return PC_OK;
}

static pc_status run_merge_group(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_layer_id id;
    if (pc_engine_layer_param(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (pc_engine_session_edit(s, "Merge Group", edit_merge_group, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", id);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- Layer > Layer Mask (add reveal/hide, from selection, delete) ------------------------- */

typedef enum { MASK_REVEAL_ALL, MASK_HIDE_ALL, MASK_REVEAL_SEL, MASK_DELETE } mask_kind;

/* Background can't take a mask: it becomes a normal layer first (extra_cmds::background_to_layer).
 * Only the small unlock is needed here; the rest of extra_cmds (Background commands) is E12. */
static void background_to_layer_for_mask(pc_document *doc, pc_layer_id id)
{
    pc_layer *bg = pc_document_background_mut(doc);
    if (bg != NULL && bg->id == id) {
        (void)pc_str_set_c(&bg->name, "Layer 0", NULL);
        bg->locks.transparency = false;
        bg->locks.position = false;
    }
}

static pc_status mask_to_depth(pc_layer_mask *m, pc_sample_type depth, pc_error *err)
{
    pc_pixel_format cur = pc_surface_format(&m->surface);
    pc_pixel_format want = cur;
    want.sample = depth;
    if (!pc_pixel_format_eq(cur, want)) {
        pc_surface conv;
        if (pc_surface_convert(&m->surface, want, &conv, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        pc_surface_fini(&m->surface);
        m->surface = conv;
    }
    return PC_OK;
}

typedef struct {
    mask_kind kind;
    pc_layer_id *ids;
    size_t n;
} mask_ctx;

static pc_status edit_mask(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    mask_ctx *ctx = vctx;
    for (size_t i = 0; i < ctx->n; i++) {
        if (ctx->kind == MASK_DELETE) {
            pc_layer *l = pc_document_layer_mut(doc, ctx->ids[i]);
            if (l == NULL) {
                return pc_engine_err_no_layer(err, ctx->ids[i]);
            }
            if (l->has_mask) {
                pc_layer_mask_fini(&l->mask);
                l->has_mask = false;
            }
            continue;
        }
        background_to_layer_for_mask(doc, ctx->ids[i]);
        pc_layer *l = pc_document_layer_mut(doc, ctx->ids[i]);
        if (l == NULL) {
            return pc_engine_err_no_layer(err, ctx->ids[i]);
        }
        pc_layer_mask m;
        if (ctx->kind == MASK_HIDE_ALL) {
            pc_layer_mask_hide_all(&m);
        } else {
            pc_layer_mask_reveal_all(&m);
        }
        if (ctx->kind == MASK_REVEAL_SEL) {
            if (!doc->has_selection) {
                pc_layer_mask_fini(&m);
                return pc_engine_err_other(err, "no selection");
            }
            pc_surface sel;
            if (pc_surface_clone(&doc->selection, &sel, err) != PC_OK) {
                pc_layer_mask_fini(&m);
                return err != NULL ? err->code : PC_ERR_NOMEM;
            }
            pc_surface_fini(&m.surface);
            m.surface = sel;
        }
        if (mask_to_depth(&m, doc->depth, err) != PC_OK) {
            pc_layer_mask_fini(&m);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        if (l->has_mask) {
            pc_layer_mask_fini(&l->mask);
        }
        l->mask = m;
        l->has_mask = true;
    }
    return PC_OK;
}

static pc_status run_mask(pc_engine_session *s, yyjson_val *p, pc_json_value *out, mask_kind kind,
                          const char *label, pc_error *err)
{
    mask_ctx ctx = {kind, NULL, 0};
    if (kind == MASK_DELETE) {
        pc_layer_id id;
        if (pc_engine_layer_param(s, p, &id, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        ctx.ids = pc_malloc_array(1, sizeof *ctx.ids);
        if (ctx.ids == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "layer mask");
        }
        ctx.ids[0] = id;
        ctx.n = 1;
    } else {
        if (mask_targets(s, p, &ctx.ids, &ctx.n, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
    }
    pc_status rc = pc_engine_session_edit(s, label, edit_mask, &ctx, err);
    free(ctx.ids);
    if (rc != PC_OK) {
        return rc;
    }
    return pc_engine_result_null(out, err);
}

static pc_status run_mask_reveal_all(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return run_mask(s, p, out, MASK_REVEAL_ALL, "Add Layer Mask", err);
}
static pc_status run_mask_hide_all(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return run_mask(s, p, out, MASK_HIDE_ALL, "Add Layer Mask", err);
}
static pc_status run_mask_reveal_sel(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return run_mask(s, p, out, MASK_REVEAL_SEL, "Add Layer Mask", err);
}
static pc_status run_mask_delete(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return run_mask(s, p, out, MASK_DELETE, "Delete Layer Mask", err);
}

/* ---- enabled predicates ------------------------------------------------------------------- */

static pc_status enabled_has_selection(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    if (!pc_doc_arc_get(d->doc)->has_selection) {
        return pc_fail(reason, PC_ERR_INVALID, "no selection");
    }
    return PC_OK;
}

/* ---- registries --------------------------------------------------------------------------- */

static const char *const MENU_LAYER[] = {"Layer"};
static const char *const MENU_NEW[] = {"Layer", "New"};
static const char *const MENU_DELETE[] = {"Layer", "Delete"};
static const char *const MENU_MASK[] = {"Layer", "Layer Mask"};

static const pc_engine_command SPECS[] = {
    {"layer.new.group", "Group\xe2\x80\xa6", MENU_NEW, 2, NULL, "{\"name\":str?}", pc_engine_has_doc,
     run_new_group, true},
    {"layer.duplicate", "Duplicate Layer\xe2\x80\xa6", MENU_LAYER, 1, NULL,
     "{\"layer\":id?,\"inPlace\":bool=false,\"name\":str?} (no layer: every selected layer)",
     pc_engine_has_layer, run_duplicate, true},
    {"layer.delete", "Delete Layer", MENU_DELETE, 2, NULL,
     "{\"layer\":id?} (no layer: every selected layer)", pc_engine_has_layer, run_delete, true},
    {"layer.select", "Select Layer", NULL, 0, NULL,
     "{\"layer\":id,\"mode\":\"replace|toggle|range|add|remove\"=\"replace\"}", pc_engine_has_doc,
     run_select, true},
    {"layer.setProps", "Layer Properties", NULL, 0, NULL,
     "{\"layer\":id?,\"name\":str?,\"visible\":bool?,\"opacity\":0..1?,\"fill\":0..1?,\"blend\":str?,"
     "\"clipped\":bool?,\"locked\":bool?,\"locks\":{…}?,\"channels\":[bool,…]?}",
     pc_engine_has_layer, run_set_props, true},
    {"layer.mergeDown", "Merge Down", MENU_LAYER, 1, NULL, "{\"layer\":id?}", pc_engine_has_layer,
     run_merge_down, true},
    {"layer.mergeGroup", "Merge Group", MENU_LAYER, 1, NULL, "{\"layer\":id?}", pc_engine_has_layer,
     run_merge_group, true},
    {"layer.layerMask.revealAll", "Reveal All", MENU_MASK, 2, NULL,
     "{\"layer\":id?} (no layer: every selected layer that can take a mask)", pc_engine_has_layer,
     run_mask_reveal_all, true},
    {"layer.layerMask.hideAll", "Hide All", MENU_MASK, 2, NULL,
     "{\"layer\":id?} (no layer: every selected layer that can take a mask)", pc_engine_has_layer,
     run_mask_hide_all, true},
    {"layer.layerMask.revealSelection", "Reveal Selection", MENU_MASK, 2, NULL,
     "{\"layer\":id?} (no layer: every selected layer that can take a mask)", enabled_has_selection,
     run_mask_reveal_sel, true},
    {"layer.layerMask.delete", "Delete", MENU_MASK, 2, NULL, "{\"layer\":id?}", pc_engine_has_layer,
     run_mask_delete, true},
};

const pc_engine_command *pc_engine_layer_base_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
