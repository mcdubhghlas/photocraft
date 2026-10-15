/* Stamping (port of crates/engine/src/stamp_cmds.rs, E4, #217): merge layers into a layer while
 * keeping the originals. Stamp Visible (every visible layer into a new layer) and Stamp Down (a
 * copy of the active layer merged into the pixel layer below; several selected: into a new layer).
 *
 * Photoshop has no menu items for these (the Alt variants of the merge commands); they are
 * commands with default shortcuts. The composite is rendered in bands
 * (pc_compose_flatten_to_surface). */
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "base/str.h"
#include "color/color.h"
#include "compose/compose.h"
#include "doc/doc.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#include "engine_internal.h"
#include "fill_cmds.h"
#include "layer_cmds.h"

/* The document's pixel format with an alpha channel (a stamp has transparency). */
static pc_pixel_format with_alpha(const pc_document *doc)
{
    pc_pixel_format f = pc_document_pixel_format(doc);
    f.alpha = true;
    return f;
}

/* A new pixel layer holding the composite of doc's visible layers, rendered in bands. */
static pc_status stamp_layer(const pc_document *doc, const char *name, pc_layer *out, pc_error *err)
{
    pc_pixel_format fmt = with_alpha(doc);
    pc_surface surf;
    pc_status rc = pc_compose_flatten_to_surface(doc, fmt, NULL, &surf, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_layer_content content = {.kind = PC_LAYER_RASTER, .u.raster = surf};
    rc = pc_layer_init(out, name, &content, err);
    if (rc != PC_OK) {
        pc_layer_content_fini(&content);
        return rc;
    }
    return PC_OK;
}

/* Make id the only selected layer and the active one. */
static void select_only(pc_engine_session *s, pc_layer_id id)
{
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL) {
        return;
    }
    pc_layer_id *sel = malloc(sizeof *sel);
    if (sel != NULL) {
        sel[0] = id;
        free(st->selected_layers);
        st->selected_layers = sel;
        st->nselected = 1;
    }
    st->has_active_layer = true;
    st->active_layer = id;
    st->has_anchor = true;
    st->layer_anchor = id;
    pc_engine_fix_selection(st);
}

/* Replace d's layers with the n layers in `layers` (moved); the old ones are freed. */
static void replace_layers(pc_document *d, pc_layer *layers, size_t n)
{
    for (size_t i = 0; i < d->nlayers; i++) {
        pc_layer_fini(&d->layers[i]);
    }
    free(d->layers);
    d->layers = layers;
    d->nlayers = n;
}

/* Is the layer at walk item `i` visible, with every ancestor visible, and not a group? */
static bool item_visible_leaf(const pc_document *doc, const pc_walk *w, size_t i)
{
    const pc_layer *l = w->items[i].layer;
    if (pc_layer_is_group(l)) {
        return false;
    }
    size_t depth = w->items[i].depth;
    const size_t *path = &w->paths[w->items[i].path_start];
    for (size_t n = 1; n <= depth + 1; n++) {
        const pc_layer *a = pc_document_layer_at(doc, path, n);
        if (a == NULL || !a->visible) {
            return false;
        }
    }
    return true;
}

struct visible_ctx {
    pc_layer_id out;
};

static pc_status stamp_visible_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx,
                                    pc_error *err)
{
    (void)ha;
    struct visible_ctx *c = vctx;
    pc_walk w;
    pc_status rc = pc_document_walk(doc, &w, err);
    if (rc != PC_OK) {
        return rc;
    }
    bool any = false;
    for (size_t i = 0; i < w.len; i++) {
        if (item_visible_leaf(doc, &w, i)) {
            any = true;
            break;
        }
    }
    pc_walk_fini(&w);
    if (!any) {
        return pc_engine_err_other(err, "there are no visible layers to stamp");
    }
    pc_str name;
    rc = pc_document_next_layer_name(doc, "Layer", &name, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_layer layer;
    rc = stamp_layer(doc, pc_str_c(&name), &layer, err);
    pc_str_fini(&name);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_layer_id *above = *ha ? active : NULL;
    pc_layer_id id;
    rc = pc_document_insert_above(doc, above, &layer, &id, err);
    if (rc != PC_OK) {
        pc_layer_fini(&layer);
        return rc;
    }
    *active = id;
    *ha = true;
    c->out = id;
    return PC_OK;
}

static pc_status run_stamp_visible(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    if (p != NULL && !yyjson_is_obj(p) && !yyjson_is_null(p)) {
        return pc_engine_err_bad_params(err, "layer.stampVisible", "expected an object");
    }
    struct visible_ctx ctx = {0};
    pc_status rc = pc_engine_session_edit(s, "Stamp Visible", stamp_visible_edit, &ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    select_only(s, ctx.out);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "layer", ctx.out);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* Several layers selected: a copy of them merged into a new layer above the top one. */
struct selected_ctx {
    const pc_layer_id *sel;
    size_t nsel;
    pc_layer_id out;
};

static pc_status stamp_selected_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx,
                                     pc_error *err)
{
    (void)ha;
    struct selected_ctx *c = vctx;
    pc_layer_id *ids = NULL;
    size_t nids = 0;
    pc_status rc = pc_engine_top_level(doc, c->sel, c->nsel, &ids, &nids, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (nids == 0) {
        free(ids);
        return pc_engine_err_other(err, "no layers selected");
    }
    pc_layer_id top = ids[nids - 1];

    pc_document solo;
    rc = pc_document_clone(doc, &solo, err);
    if (rc != PC_OK) {
        free(ids);
        return rc;
    }
    /* solo.layers = the selected visible layers, cloned. */
    pc_layer *picked = malloc(nids * sizeof *picked);
    if (picked == NULL && nids > 0) {
        free(ids);
        pc_document_fini(&solo);
        return pc_fail(err, PC_ERR_NOMEM, "stamp");
    }
    size_t np = 0;
    for (size_t i = 0; i < nids; i++) {
        const pc_layer *l = pc_document_layer(doc, ids[i]);
        if (l == NULL || !l->visible) {
            continue;
        }
        rc = pc_layer_clone(l, &picked[np], err);
        if (rc != PC_OK) {
            for (size_t j = 0; j < np; j++) {
                pc_layer_fini(&picked[j]);
            }
            free(picked);
            free(ids);
            pc_document_fini(&solo);
            return rc;
        }
        np++;
    }
    if (np == 0) {
        free(picked);
        free(ids);
        pc_document_fini(&solo);
        return pc_engine_err_other(err, "the selected layers are all hidden");
    }
    replace_layers(&solo, picked, np);

    const pc_layer *topl = pc_document_layer(doc, top);
    char namebuf[256];
    snprintf(namebuf, sizeof namebuf, "%s (merged)", topl != NULL ? pc_str_c(&topl->name) : "Layer");
    pc_layer layer;
    rc = stamp_layer(&solo, namebuf, &layer, err);
    pc_document_fini(&solo);
    if (rc != PC_OK) {
        free(ids);
        return rc;
    }
    pc_layer_id id;
    rc = pc_document_insert_above(doc, &top, &layer, &id, err);
    free(ids);
    if (rc != PC_OK) {
        pc_layer_fini(&layer);
        return rc;
    }
    *active = id;
    *ha = true;
    c->out = id;
    return PC_OK;
}

struct down_ctx {
    pc_layer_id id;
};

static pc_status stamp_down_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    (void)active;
    struct down_ctx *c = vctx;
    pc_layer_id id = c->id;
    pc_layer_path path;
    pc_status rc = pc_document_path_of(doc, id, &path, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (path.len == 0) {
        pc_layer_path_fini(&path);
        return pc_engine_err_no_layer(err, id);
    }
    size_t idx = path.idx[path.len - 1];
    if (idx == 0) {
        pc_layer_path_fini(&path);
        return pc_engine_err_other(err, "there is no layer below to stamp into");
    }
    /* below = parent path + (idx - 1). */
    size_t below_len = path.len;
    size_t *below = malloc(below_len * sizeof *below);
    if (below == NULL) {
        pc_layer_path_fini(&path);
        return pc_fail(err, PC_ERR_NOMEM, "stamp");
    }
    memcpy(below, path.idx, below_len * sizeof *below);
    below[below_len - 1] = idx - 1;
    pc_layer_path_fini(&path);

    const pc_layer *lower = pc_document_layer_at(doc, below, below_len);
    const pc_layer *upper = pc_document_layer(doc, id);
    if (lower == NULL || upper == NULL) {
        free(below);
        return pc_engine_err_no_layer(err, id);
    }
    if (lower->content.kind != PC_LAYER_RASTER) {
        free(below);
        return pc_engine_err_other(err, "the layer below is not a pixel layer");
    }
    pc_locks locks = pc_document_effective_locks(doc, lower->id);
    if (locks.all || locks.pixels) {
        free(below);
        return pc_engine_err_other(err, "the layer below is locked");
    }
    if (!upper->visible) {
        free(below);
        return pc_engine_err_other(err, "the layer is hidden");
    }

    /* The two layers on their own, composited in bands; the lower one keeps its id/name/blend. */
    pc_layer_id lower_id = lower->id;
    pc_str lower_name;
    rc = pc_str_clone(&lower->name, &lower_name, err);
    if (rc != PC_OK) {
        free(below);
        return rc;
    }
    pc_blend_mode lower_blend = lower->blend;
    bool lower_visible = lower->visible;
    pc_locks lower_locks = lower->locks;
    bool lock_transparency = lower->locks.transparency;

    pc_document tmp;
    rc = pc_document_clone(doc, &tmp, err);
    if (rc != PC_OK) {
        pc_str_fini(&lower_name);
        free(below);
        return rc;
    }
    pc_layer two[2];
    rc = pc_layer_clone(lower, &two[0], err);
    if (rc == PC_OK) {
        two[0].visible = true;
        rc = pc_layer_clone(upper, &two[1], err);
        if (rc != PC_OK) {
            pc_layer_fini(&two[0]);
        }
    }
    if (rc != PC_OK) {
        pc_document_fini(&tmp);
        pc_str_fini(&lower_name);
        free(below);
        return rc;
    }
    pc_layer *two_heap = malloc(2 * sizeof *two_heap);
    if (two_heap == NULL) {
        pc_layer_fini(&two[0]);
        pc_layer_fini(&two[1]);
        pc_document_fini(&tmp);
        pc_str_fini(&lower_name);
        free(below);
        return pc_fail(err, PC_ERR_NOMEM, "stamp");
    }
    two_heap[0] = two[0];
    two_heap[1] = two[1];
    replace_layers(&tmp, two_heap, 2);

    pc_pixel_format fmt = pc_document_pixel_format(doc);
    if (!lock_transparency) {
        fmt.alpha = true;
    }
    pc_surface surf;
    rc = pc_compose_flatten_to_surface(&tmp, fmt, NULL, &surf, err);
    pc_document_fini(&tmp);
    if (rc != PC_OK) {
        pc_str_fini(&lower_name);
        free(below);
        return rc;
    }

    pc_layer_content content = {.kind = PC_LAYER_RASTER, .u.raster = surf};
    pc_layer merged;
    rc = pc_layer_init(&merged, pc_str_c(&lower_name), &content, err);
    pc_str_fini(&lower_name);
    if (rc != PC_OK) {
        pc_layer_content_fini(&content);
        free(below);
        return rc;
    }
    merged.id = lower_id;
    merged.blend = lower_blend;
    merged.visible = lower_visible;
    merged.locks = lower_locks;
    merged.opacity = 1.0f;

    pc_layer *slot = pc_document_layer_at_mut(doc, below, below_len);
    free(below);
    if (slot == NULL) {
        pc_layer_fini(&merged);
        return pc_engine_err_no_layer(err, lower_id);
    }
    pc_layer_fini(slot);
    *slot = merged;
    return PC_OK;
}

static pc_status run_stamp_down(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    if (p != NULL && !yyjson_is_obj(p) && !yyjson_is_null(p)) {
        return pc_engine_err_bad_params(err, "layer.stampDown", "expected an object");
    }
    pc_layer_id *sel = NULL;
    size_t nsel = 0;
    pc_status rc = pc_engine_selected(s, &sel, &nsel, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (nsel >= 2) {
        struct selected_ctx ctx = {sel, nsel, 0};
        rc = pc_engine_session_edit(s, "Stamp Layers", stamp_selected_edit, &ctx, err);
        free(sel);
        if (rc != PC_OK) {
            return rc;
        }
        select_only(s, ctx.out);
        yyjson_mut_doc *d = NULL;
        if (pc_engine_result_doc(out, &d, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(d);
        yyjson_mut_obj_add_uint(d, root, "layer", ctx.out);
        yyjson_mut_doc_set_root(d, root);
        return PC_OK;
    }
    free(sel);
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    if (!st->has_active_layer) {
        return pc_engine_err_other(err, "no active layer");
    }
    struct down_ctx ctx = {st->active_layer};
    rc = pc_engine_session_edit(s, "Stamp Down", stamp_down_edit, &ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "layer", ctx.id);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status en_has_doc(const pc_engine_session *s, pc_error *reason)
{
    return pc_engine_has_doc(s, reason);
}

static pc_status en_has_layer(const pc_engine_session *s, pc_error *reason)
{
    return pc_engine_has_layer(s, reason);
}

static const pc_engine_command SPECS[] = {
    {"layer.stampVisible", "Stamp Visible", NULL, 0, "Cmd+Alt+Shift+E",
     "{} → {layer} (every visible layer merged into a new layer above the active one; the originals "
     "stay)",
     en_has_doc, run_stamp_visible, true},
    {"layer.stampDown", "Stamp Down", NULL, 0, "Cmd+Alt+E",
     "{} → {layer} (a copy of the active layer merged into the pixel layer below; several selected: "
     "into a new layer above them)",
     en_has_layer, run_stamp_down, true},
};

const pc_engine_command *pc_engine_stamp_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}

/* The Alt variant of a Layer-menu merge command (stamp_cmds::alt_variant), used by dispatch (E12). */
const char *pc_engine_stamp_alt_variant(const char *id)
{
    if (strcmp(id, "layer.mergeVisible") == 0) {
        return "layer.stampVisible";
    }
    if (strcmp(id, "layer.mergeDown") == 0 || strcmp(id, "layer.mergeLayers") == 0) {
        return "layer.stampDown";
    }
    return NULL;
}
