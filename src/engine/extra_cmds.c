/* Everyday Photoshop commands that build on the core ones (port of the self-contained parts of
 * crates/engine/src/extra_cmds.rs, E12).
 *
 * Covered here: Edit > Stroke, Reselect, Equalize, Layer From Background (+ the Background<->Layer
 * helpers other chunks reuse), Hide/Show All Effects, the layer-mask enable/link toggles, the
 * Rasterize variants, Delete Hidden / Empty Layers, Ungroup, Hide/Show Layers, Lock Transparent
 * Pixels, Show Only This Layer, and Filter > Blur > Average / Render > Clouds.
 *
 * See the §7 note at the foot of this file for the extra_cmds.rs entries deferred to sibling
 * chunks (the fixed Transform presets, Paste Into/Outside, Copy/Paste Layer Style) and for the
 * commands the coordinator routed here that live in other reference modules (edit.fade / edit.purge
 * in edit_menu_cmds.rs, filter.other.colorToAlpha in color_to_alpha_cmds.rs, the layerMask
 * apply/hideSelection/fromTransparency in layer_menu_cmds.rs). */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "algo/selection.h"
#include "brush_cmds.h"
#include "extra_cmds.h"
#include "fill_cmds.h"
#include "layer_cmds.h"
#include "raster/pixel.h"
#include "raster/surface.h"
#include "smart_cmds.h"

/* ============================================================== shared helpers */

static pc_status err_code(pc_error *err) { return err != NULL ? err->code : PC_ERR_INVALID; }

static const pc_document *active_doc(const pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    return d == NULL ? NULL : pc_doc_arc_get(d->doc);
}

/* The active layer, or NULL with a reason filled in. */
static pc_status has_doc(const pc_engine_session *s, pc_error *reason)
{
    return pc_engine_has_doc(s, reason);
}

static pc_status has_layer(const pc_engine_session *s, pc_error *reason)
{
    return pc_engine_has_layer(s, reason);
}

static pc_status has_pixels(const pc_engine_session *s, pc_error *reason)
{
    const pc_layer *l = pc_engine_active_layer_of(s, reason);
    if (l == NULL) {
        return err_code(reason);
    }
    if (l->content.kind == PC_LAYER_RASTER) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "active layer is %s %s layer, not a pixel layer",
                   pc_layer_content_article(&l->content), pc_layer_content_kind_name(&l->content));
}

static pc_status has_mask(const pc_engine_session *s, pc_error *reason)
{
    const pc_layer *l = pc_engine_active_layer_of(s, reason);
    if (l == NULL) {
        return err_code(reason);
    }
    if (!l->has_mask) {
        return pc_fail(reason, PC_ERR_INVALID, "the active layer has no layer mask");
    }
    return PC_OK;
}

static pc_status has_background(const pc_engine_session *s, pc_error *reason)
{
    const pc_document *doc = active_doc(s);
    if (doc == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    if (pc_document_background(doc) == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "the document has no Background layer");
    }
    return PC_OK;
}

static pc_status is_group_layer(const pc_engine_session *s, pc_error *reason)
{
    const pc_layer *l = pc_engine_active_layer_of(s, reason);
    if (l == NULL) {
        return err_code(reason);
    }
    if (l->content.kind != PC_LAYER_GROUP) {
        return pc_fail(reason, PC_ERR_INVALID, "the active layer is not a group");
    }
    return PC_OK;
}

/* A key that makes the sub-commands of one command share a single history step
 * (extra_cmds::step_key): "<what>#<revision>". */
static void step_key(const pc_engine_session *s, const char *what, char *out, size_t cap)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    unsigned long long rev = d == NULL ? 0ULL : (unsigned long long)d->revision;
    (void)snprintf(out, cap, "%s#%llu", what, rev);
}

static void set_coalesce(pc_engine_session *s, const char *key)
{
    free(s->coalesce_request);
    s->coalesce_request = NULL;
    if (key != NULL) {
        s->coalesce_request = pc_strndup(key, strlen(key));
    }
}

/* The layers a command targets (port of layer_multi_cmds::targets): the explicit `layer` param, or
 * the selected layers (which always include the active one). *out is an owned array (free). */
static pc_status collect_targets(const pc_engine_session *s, yyjson_val *p, pc_layer_id **out,
                                 size_t *n, bool *explicit_out, pc_error *err)
{
    bool present = false;
    pc_layer_id id = 0;
    if (pc_engine_explicit_layer(s, p, &present, &id, err) != PC_OK) {
        return err_code(err);
    }
    if (explicit_out != NULL) {
        *explicit_out = present;
    }
    if (present) {
        pc_layer_id *one = malloc(sizeof *one);
        if (one == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "targets");
        }
        one[0] = id;
        *out = one;
        *n = 1;
        return PC_OK;
    }
    return pc_engine_selected(s, out, n, err);
}

/* ============================================================== Background <-> Layer */

void pc_engine_unlock_background(pc_layer *l)
{
    if (l == NULL) {
        return;
    }
    (void)pc_str_set_c(&l->name, "Layer 0", NULL);
    l->locks.transparency = false;
    l->locks.position = false;
}

void pc_engine_background_to_layer(pc_document *doc, pc_layer_id id)
{
    if (doc == NULL) {
        return;
    }
    pc_layer *l = pc_document_background_mut(doc);
    if (l != NULL && l->id == id) {
        pc_engine_unlock_background(l);
    }
}

void pc_engine_background_to_layer_for_mask(pc_document *doc, pc_layer_id id)
{
    pc_engine_background_to_layer(doc, id);
}

/* ---- layer.new.layerFromBackground -------------------------------------------------------- */

typedef struct {
    bool select;
    bool found_id;
    pc_layer_id out_id;
} from_bg_ctx;

static pc_status from_bg_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *ud,
                             pc_error *err)
{
    from_bg_ctx *c = ud;
    pc_layer *l = pc_document_background_mut(doc);
    if (l == NULL) {
        return pc_engine_err_other(err, "the document has no Background layer");
    }
    pc_engine_unlock_background(l);
    c->found_id = true;
    c->out_id = l->id;
    if (c->select || !*has_active) {
        *has_active = true;
        *active = l->id;
    }
    return PC_OK;
}

static pc_status run_layer_from_background(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                          pc_error *err)
{
    const pc_document *doc = active_doc(s);
    if (doc == NULL) {
        return pc_engine_err_no_document(err);
    }
    bool if_present = false;
    yyjson_val *ip = pc_json_get(p, "ifPresent");
    if (ip != NULL) {
        (void)pc_json_as_bool(ip, &if_present);
    }
    if (pc_document_background(doc) == NULL && if_present) {
        yyjson_mut_doc *rd = NULL;
        if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_null(rd, root, "layer");
        yyjson_mut_doc_set_root(rd, root);
        return PC_OK;
    }
    from_bg_ctx c = {.select = true, .found_id = false, .out_id = 0};
    yyjson_val *sel = pc_json_get(p, "select");
    if (sel != NULL) {
        (void)pc_json_as_bool(sel, &c.select);
    }
    if (pc_engine_session_edit(s, "Layer From Background", from_bg_edit, &c, err) != PC_OK) {
        return err_code(err);
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", (uint64_t)c.out_id);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ============================================================== layer-mask toggles */

typedef struct {
    pc_layer_id id;
    bool link;    /* false = enabled, true = linked */
    bool have_want;
    bool want;
    bool result;
} mask_toggle_ctx;

static pc_status mask_toggle_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *ud,
                                 pc_error *err)
{
    (void)has_active;
    (void)active;
    mask_toggle_ctx *c = ud;
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL || !l->has_mask) {
        return pc_engine_err_other(err, "the layer has no layer mask");
    }
    bool *slot = c->link ? &l->mask.linked : &l->mask.enabled;
    *slot = c->have_want ? c->want : !*slot;
    c->result = *slot;
    return PC_OK;
}

static pc_status run_mask_toggle(pc_engine_session *s, yyjson_val *p, pc_json_value *out, bool link,
                                const char *label, const char *key, pc_error *err)
{
    mask_toggle_ctx c;
    memset(&c, 0, sizeof c);
    c.link = link;
    if (pc_engine_layer_param(s, p, &c.id, err) != PC_OK) {
        return err_code(err);
    }
    yyjson_val *w = pc_json_get(p, key);
    if (w != NULL && !yyjson_is_null(w)) {
        if (!pc_json_as_bool(w, &c.want)) {
            return pc_engine_err_bad_params(err, link ? "layer.layerMask.linked" : "layer.layerMask.enabled",
                                            "`%s` must be true or false", key);
        }
        c.have_want = true;
    }
    if (pc_engine_session_edit(s, label, mask_toggle_edit, &c, err) != PC_OK) {
        return err_code(err);
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_bool(rd, root, key, c.result);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_mask_enabled(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return run_mask_toggle(s, p, out, false, "Enable/Disable Layer Mask", "enabled", err);
}

static pc_status run_mask_linked(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return run_mask_toggle(s, p, out, true, "Link/Unlink Layer Mask", "linked", err);
}

/* ============================================================== Hide/Show All Effects */

static void set_effects_rec(pc_layer *layers, size_t n, bool on)
{
    for (size_t i = 0; i < n; i++) {
        if (layers[i].effects.nitems != 0) {
            layers[i].effects.enabled = on;
        }
        if (layers[i].content.kind == PC_LAYER_GROUP) {
            set_effects_rec(layers[i].content.u.group.children, layers[i].content.u.group.nchildren, on);
        }
    }
}

typedef struct {
    bool on;
} effects_ctx;

static pc_status effects_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *ud,
                             pc_error *err)
{
    (void)has_active;
    (void)active;
    (void)err;
    effects_ctx *c = ud;
    set_effects_rec(doc->layers, doc->nlayers, c->on);
    return PC_OK;
}

static pc_status run_set_all_effects(pc_engine_session *s, pc_json_value *out, bool on, pc_error *err)
{
    effects_ctx c = {.on = on};
    if (pc_engine_session_edit(s, on ? "Show All Effects" : "Hide All Effects", effects_edit, &c, err)
        != PC_OK) {
        return err_code(err);
    }
    return pc_engine_result_null(out, err);
}

static pc_status run_hide_all_effects(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return run_set_all_effects(s, out, false, err);
}

static pc_status run_show_all_effects(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return run_set_all_effects(s, out, true, err);
}

/* ============================================================== Delete Hidden / Empty Layers */

typedef bool (*layer_pred)(const pc_layer *l);

static bool pred_hidden(const pc_layer *l) { return !l->visible; }

static bool pred_empty(const pc_layer *l)
{
    switch (l->content.kind) {
    case PC_LAYER_RASTER:
        return pc_rect_is_empty(pc_surface_content_bounds(&l->content.u.raster)) && !l->has_mask
               && l->effects.nitems == 0;
    case PC_LAYER_GROUP: {
        const pc_group *g = &l->content.u.group;
        for (size_t i = 0; i < g->nchildren; i++) {
            if (!pred_empty(&g->children[i])) {
                return false;
            }
        }
        return true;
    }
    default:
        return false;
    }
}

/* Remove every layer matching pred (whole subtrees); returns how many top-level removals happened. */
static size_t delete_rec(pc_layer *layers, size_t *n, layer_pred pred)
{
    size_t removed = 0;
    size_t w = 0;
    for (size_t r = 0; r < *n; r++) {
        if (pred(&layers[r])) {
            pc_layer_fini(&layers[r]);
            removed++;
            continue;
        }
        if (w != r) {
            layers[w] = layers[r];
        }
        w++;
    }
    *n = w;
    for (size_t i = 0; i < *n; i++) {
        if (layers[i].content.kind == PC_LAYER_GROUP) {
            pc_group *g = &layers[i].content.u.group;
            removed += delete_rec(g->children, &g->nchildren, pred);
        }
    }
    return removed;
}

typedef struct {
    layer_pred pred;
    size_t deleted;
} delete_ctx;

static pc_status delete_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *ud,
                            pc_error *err)
{
    delete_ctx *c = ud;
    /* session_edit works on a private copy and discards it on error, so an emptied tree never
     * reaches the document: just mutate and refuse when nothing would be left. */
    size_t removed = delete_rec(doc->layers, &doc->nlayers, c->pred);
    if (doc->nlayers == 0) {
        return pc_engine_err_other(err, "a document must keep at least one layer");
    }
    c->deleted = removed;
    if (!*has_active || pc_document_layer(doc, *active) == NULL) {
        pc_layer_id top = 0;
        if (pc_document_top_layer(doc, &top)) {
            *has_active = true;
            *active = top;
        } else {
            *has_active = false;
        }
    }
    return PC_OK;
}

static pc_status run_delete_where(pc_engine_session *s, pc_json_value *out, const char *label,
                                 layer_pred pred, pc_error *err)
{
    if (active_doc(s) == NULL) {
        return pc_engine_err_no_document(err);
    }
    delete_ctx c = {.pred = pred, .deleted = 0};
    if (pc_engine_session_edit(s, label, delete_edit, &c, err) != PC_OK) {
        return err_code(err);
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "deleted", c.deleted);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_delete_hidden(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return run_delete_where(s, out, "Delete Hidden Layers", pred_hidden, err);
}

static pc_status run_delete_empty(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return run_delete_where(s, out, "Delete All Empty Layers", pred_empty, err);
}

/* ============================================================== Ungroup Layers */

typedef struct {
    pc_layer_id id;
} ungroup_ctx;

static pc_status ungroup_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *ud,
                             pc_error *err)
{
    ungroup_ctx *c = ud;
    const pc_layer *g = pc_document_layer(doc, c->id);
    if (g == NULL || g->content.kind != PC_LAYER_GROUP) {
        return pc_engine_err_other(err, "the active layer is not a group");
    }
    pc_layer_path path;
    if (pc_document_path_of(doc, c->id, &path, err) != PC_OK) {
        return err_code(err);
    }
    size_t idx = path.idx[path.len - 1];
    /* The sibling array that holds the group (the root, or the enclosing group's children). These
     * pointers stay valid: the owning layer does not move, only its children array is replaced. */
    pc_layer **children_ptr = &doc->layers;
    size_t *n_ptr = &doc->nlayers;
    if (path.len > 1) {
        pc_layer *parent = pc_document_layer_at_mut(doc, path.idx, path.len - 1);
        if (parent == NULL || parent->content.kind != PC_LAYER_GROUP) {
            pc_layer_path_fini(&path);
            return pc_engine_err_no_layer(err, c->id);
        }
        children_ptr = &parent->content.u.group.children;
        n_ptr = &parent->content.u.group.nchildren;
    }
    pc_layer_path_fini(&path);

    pc_layer *siblings = *children_ptr;
    size_t old_n = *n_ptr;
    /* Take the group shell out of its slot, keeping its children detached. */
    pc_layer group = siblings[idx];
    pc_layer *kids = group.content.u.group.children;
    size_t nkids = group.content.u.group.nchildren;
    group.content.u.group.children = NULL;
    group.content.u.group.nchildren = 0;

    size_t new_n = old_n - 1 + nkids;
    pc_layer *grown = malloc((new_n == 0 ? 1 : new_n) * sizeof *grown);
    if (grown == NULL) {
        group.content.u.group.children = kids; /* restore so nothing leaks */
        group.content.u.group.nchildren = nkids;
        return pc_fail(err, PC_ERR_NOMEM, "ungroup");
    }
    pc_layer_id top = 0;
    bool have_top = nkids > 0;
    if (have_top) {
        top = kids[nkids - 1].id;
    }
    size_t w = 0;
    for (size_t i = 0; i < idx; i++) {
        grown[w++] = siblings[i];
    }
    for (size_t i = 0; i < nkids; i++) {
        grown[w++] = kids[i];
    }
    for (size_t i = idx + 1; i < old_n; i++) {
        grown[w++] = siblings[i];
    }
    free(kids);
    free(siblings);
    pc_layer_fini(&group); /* the now-empty group shell (name, effects, mask, ...) */
    *children_ptr = grown;
    *n_ptr = new_n;

    if (have_top) {
        *has_active = true;
        *active = top;
    } else {
        pc_layer_id t = 0;
        if (pc_document_top_layer(doc, &t)) {
            *has_active = true;
            *active = t;
        } else {
            *has_active = false;
        }
    }
    return PC_OK;
}

static pc_status run_ungroup(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    ungroup_ctx c;
    memset(&c, 0, sizeof c);
    if (pc_engine_layer_param(s, p, &c.id, err) != PC_OK) {
        return err_code(err);
    }
    if (pc_engine_session_edit(s, "Ungroup Layers", ungroup_edit, &c, err) != PC_OK) {
        return err_code(err);
    }
    return pc_engine_result_null(out, err);
}

/* ============================================================== Hide/Show Layers, Lock Transparency */

typedef struct {
    const pc_layer_id *ids;
    size_t n;
    bool visible;
} visible_ctx;

static pc_status visible_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *ud,
                             pc_error *err)
{
    (void)has_active;
    (void)active;
    visible_ctx *c = ud;
    for (size_t i = 0; i < c->n; i++) {
        pc_layer *l = pc_document_layer_mut(doc, c->ids[i]);
        if (l == NULL) {
            return pc_engine_err_no_layer(err, c->ids[i]);
        }
        l->visible = c->visible;
    }
    return PC_OK;
}

static pc_status set_visible(pc_engine_session *s, const pc_layer_id *ids, size_t n, bool visible,
                            const char *label, pc_error *err)
{
    visible_ctx c = {.ids = ids, .n = n, .visible = visible};
    return pc_engine_session_edit(s, label, visible_edit, &c, err);
}

static pc_status run_hide_layers(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_document *doc = active_doc(s);
    if (doc == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_layer_id *ids = NULL;
    size_t n = 0;
    if (collect_targets(s, p, &ids, &n, NULL, err) != PC_OK) {
        return err_code(err);
    }
    /* Show them when every targeted layer is already hidden, so the shortcut toggles. */
    bool all_hidden = true;
    for (size_t i = 0; i < n; i++) {
        const pc_layer *l = pc_document_layer(doc, ids[i]);
        if (l != NULL && l->visible) {
            all_hidden = false;
            break;
        }
    }
    bool visible = all_hidden;
    pc_status rc = set_visible(s, ids, n, visible, visible ? "Show Layer" : "Hide Layer", err);
    free(ids);
    if (rc != PC_OK) {
        return err_code(err);
    }
    return pc_engine_result_null(out, err);
}

static pc_status run_show_layers(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_layer_id *ids = NULL;
    size_t n = 0;
    if (collect_targets(s, p, &ids, &n, NULL, err) != PC_OK) {
        return err_code(err);
    }
    pc_status rc = set_visible(s, ids, n, true, "Show Layer", err);
    free(ids);
    if (rc != PC_OK) {
        return err_code(err);
    }
    return pc_engine_result_null(out, err);
}

typedef struct {
    const pc_layer_id *ids;
    size_t n;
    bool lock;
} lock_ctx;

static pc_status lock_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *ud, pc_error *err)
{
    (void)has_active;
    (void)active;
    lock_ctx *c = ud;
    for (size_t i = 0; i < c->n; i++) {
        pc_layer *l = pc_document_layer_mut(doc, c->ids[i]);
        if (l == NULL) {
            return pc_engine_err_no_layer(err, c->ids[i]);
        }
        l->locks.transparency = c->lock;
    }
    return PC_OK;
}

static pc_status run_lock_transparency(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                      pc_error *err)
{
    const pc_document *doc = active_doc(s);
    if (doc == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_layer_id *all = NULL;
    size_t nall = 0;
    if (collect_targets(s, p, &all, &nall, NULL, err) != PC_OK) {
        return err_code(err);
    }
    /* Drop the Background: it can't take this lock. */
    pc_layer_id *ids = malloc((nall == 0 ? 1 : nall) * sizeof *ids);
    if (ids == NULL) {
        free(all);
        return pc_fail(err, PC_ERR_NOMEM, "lock");
    }
    size_t n = 0;
    for (size_t i = 0; i < nall; i++) {
        if (pc_document_layer(doc, all[i]) != NULL && !pc_document_is_background(doc, all[i])) {
            ids[n++] = all[i];
        }
    }
    free(all);
    if (n == 0) {
        free(ids);
        return pc_engine_err_other(err, "the Background layer can't lock transparent pixels");
    }
    bool all_locked = true;
    for (size_t i = 0; i < n; i++) {
        const pc_layer *l = pc_document_layer(doc, ids[i]);
        if (l != NULL && !l->locks.transparency) {
            all_locked = false;
            break;
        }
    }
    bool lock = !all_locked;
    lock_ctx c = {.ids = ids, .n = n, .lock = lock};
    pc_status rc = pc_engine_session_edit(s, lock ? "Lock Transparent Pixels" : "Unlock Transparent Pixels",
                                          lock_edit, &c, err);
    free(ids);
    if (rc != PC_OK) {
        return err_code(err);
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_bool(rd, root, "locked", lock);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ============================================================== Show Only This Layer */

/* `a` (alen indices) is a prefix of `b` (blen indices). */
static bool path_is_prefix(const size_t *a, size_t alen, const size_t *b, size_t blen)
{
    if (alen > blen) {
        return false;
    }
    for (size_t i = 0; i < alen; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

/* Is the layer at `path` shown alone (it and its enclosing groups visible, every layer outside it
 * hidden; layers inside it keep their own visibility)? */
static bool shown_alone(const pc_walk *w, const size_t *path, size_t plen)
{
    for (size_t i = 0; i < w->len; i++) {
        const size_t *p = &w->paths[w->items[i].path_start];
        size_t pl = w->items[i].depth + 1;
        bool visible = w->items[i].layer->visible;
        if (path_is_prefix(p, pl, path, plen)) { /* p is self/ancestor of the solo layer */
            if (!visible) {
                return false;
            }
        } else if (!path_is_prefix(path, plen, p, pl) && visible) {
            /* outside the solo layer, yet visible */
            return false;
        }
    }
    return true;
}

typedef struct {
    pc_layer_id id;
    bool solo; /* becoming solo (true) or restoring (false) */
} showonly_ctx;

static pc_status showonly_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *ud,
                              pc_error *err)
{
    (void)has_active;
    (void)active;
    showonly_ctx *c = ud;
    pc_layer_path path;
    if (pc_document_path_of(doc, c->id, &path, err) != PC_OK) {
        return err_code(err);
    }
    pc_walk w;
    if (pc_document_walk(doc, &w, err) != PC_OK) {
        pc_layer_path_fini(&path);
        return err_code(err);
    }
    for (size_t i = 0; i < w.len; i++) {
        const size_t *p = &w.paths[w.items[i].path_start];
        size_t pl = w.items[i].depth + 1;
        pc_layer *l = pc_document_layer_mut(doc, w.items[i].layer->id);
        if (l == NULL) {
            continue;
        }
        if (c->solo) {
            /* Layers inside the solo layer keep their visibility; the rest are shown only when they
             * are self/ancestor of it. */
            bool inside = pl > path.len && path_is_prefix(path.idx, path.len, p, pl);
            if (!inside) {
                l->visible = path_is_prefix(p, pl, path.idx, path.len);
            }
        } else {
            l->visible = true; /* no snapshot kept (see §7): Show Layers reveals everything */
        }
    }
    pc_walk_fini(&w);
    pc_layer_path_fini(&path);
    return PC_OK;
}

static pc_status run_show_only(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    showonly_ctx c;
    memset(&c, 0, sizeof c);
    if (pc_engine_layer_param(s, p, &c.id, err) != PC_OK) {
        return err_code(err);
    }
    const pc_document *doc = active_doc(s);
    if (doc == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_layer_path path;
    if (pc_document_path_of(doc, c.id, &path, err) != PC_OK) {
        return err_code(err);
    }
    pc_walk w;
    if (pc_document_walk(doc, &w, err) != PC_OK) {
        pc_layer_path_fini(&path);
        return err_code(err);
    }
    c.solo = !shown_alone(&w, path.idx, path.len);
    pc_walk_fini(&w);
    pc_layer_path_fini(&path);
    if (pc_engine_session_edit(s, c.solo ? "Show Only This Layer" : "Show Layers", showonly_edit, &c,
                               err) != PC_OK) {
        return err_code(err);
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_bool(rd, root, "shownAlone", c.solo);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ============================================================== Reselect */

/* The most recent selection in the past history states, when the document has none now. *found is
 * false otherwise; on success *out is an owned clone of that selection surface. */
static bool reselect_target(const pc_engine_session *s, pc_surface *out)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return false;
    }
    if (pc_doc_arc_get(d->doc)->has_selection) {
        return false;
    }
    size_t past = pc_history_past_len(d->history);
    for (size_t k = past; k > 0; k--) {
        pc_doc_arc *arc = pc_history_state(d->history, k - 1);
        if (arc == NULL) {
            continue;
        }
        const pc_document *hd = pc_doc_arc_get(arc);
        if (hd->has_selection) {
            if (pc_surface_clone(&hd->selection, out, NULL) != PC_OK) {
                return false;
            }
            return true;
        }
    }
    return false;
}

static pc_status can_reselect(const pc_engine_session *s, pc_error *reason)
{
    if (pc_engine_has_doc(s, reason) != PC_OK) {
        return err_code(reason);
    }
    pc_surface tmp;
    if (!reselect_target(s, &tmp)) {
        return pc_fail(reason, PC_ERR_INVALID, "there is no selection to restore");
    }
    pc_surface_fini(&tmp);
    return PC_OK;
}

typedef struct {
    pc_surface sel; /* moved into the document */
} reselect_ctx;

static pc_status reselect_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *ud,
                              pc_error *err)
{
    (void)has_active;
    (void)active;
    (void)err;
    reselect_ctx *c = ud;
    if (doc->has_selection) {
        pc_surface_fini(&doc->selection);
    }
    doc->selection = c->sel;
    doc->has_selection = true;
    c->sel = (pc_surface){0}; /* moved out */
    return PC_OK;
}

static pc_status run_reselect(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    reselect_ctx c;
    memset(&c, 0, sizeof c);
    if (!reselect_target(s, &c.sel)) {
        return pc_engine_err_other(err, "there is no selection to restore");
    }
    pc_status rc = pc_engine_session_edit(s, "Reselect", reselect_edit, &c, err);
    pc_surface_fini(&c.sel); /* NULL-safe; no-op when moved into the document */
    if (rc != PC_OK) {
        return err_code(err);
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_bool(rd, root, "selected", true);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ============================================================== pixel mapping (Equalize/Average/Clouds) */

/* Rewrite the target surface's RGBA over the selection bounds (or the canvas). `fn` gets the area,
 * the pixels and the selection coverage per pixel; results are blended back by coverage, and
 * transparency stays locked when the layer locks it (port of extra_cmds::map_pixels, pixels/mask
 * target path; see §7 for the alpha-channel target). */
typedef void (*px_fn)(pc_rect area, float (*px)[4], const float *cover, size_t count, void *ud);

typedef struct {
    px_fn fn;
    void *ud;
} map_ctx;

static pc_status map_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *ud, pc_error *err)
{
    map_ctx *c = ud;
    pc_surface *surf = NULL;
    bool lock = false;
    if (pc_engine_target_surface(doc, *has_active, *has_active ? *active : (pc_layer_id)0, NULL,
                                 &surf, &lock, err) != PC_OK) {
        return err_code(err);
    }
    pc_rect canvas = pc_document_bounds(doc);
    pc_rect area = canvas;
    if (doc->has_selection) {
        area = pc_rect_intersect(pc_surface_content_bounds(&doc->selection), canvas);
    }
    if (pc_rect_is_empty(area)) {
        return PC_OK;
    }
    size_t wpx = pc_rect_width(area), hpx = pc_rect_height(area);
    size_t count = wpx * hpx;
    size_t nch = pc_surface_channels(surf);
    pc_pixel_format fmt = pc_surface_format(surf);
    float *raw = NULL;
    size_t rawlen = 0;
    if (pc_surface_read_region(surf, area, &raw, &rawlen, err) != PC_OK) {
        return err_code(err);
    }
    float (*orig)[4] = malloc(count * sizeof *orig);
    float (*px)[4] = malloc(count * sizeof *px);
    float *cover = malloc(count * sizeof *cover);
    if (orig == NULL || px == NULL || cover == NULL) {
        free(orig);
        free(px);
        free(cover);
        free(raw);
        return pc_fail(err, PC_ERR_NOMEM, "map_pixels");
    }
    for (size_t i = 0; i < count; i++) {
        pc_to_rgba(fmt, &raw[i * nch], nch, orig[i]);
        px[i][0] = orig[i][0];
        px[i][1] = orig[i][1];
        px[i][2] = orig[i][2];
        px[i][3] = orig[i][3];
        if (doc->has_selection) {
            int32_t x = area.x0 + (int32_t)(i % wpx);
            int32_t y = area.y0 + (int32_t)(i / wpx);
            cover[i] = pc_surface_sample_channel(&doc->selection, x, y, 0);
        } else {
            cover[i] = 1.0f;
        }
    }
    c->fn(area, px, cover, count, c->ud);
    for (size_t i = 0; i < count; i++) {
        float k = cover[i];
        if (k <= 0.0f) {
            continue;
        }
        float v[4];
        for (int ch = 0; ch < 4; ch++) {
            v[ch] = orig[i][ch] + (px[i][ch] - orig[i][ch]) * k;
        }
        if (lock) {
            v[3] = orig[i][3];
        }
        float enc[PC_RASTER_MAX_CHANNELS];
        size_t nw = pc_from_rgba(fmt, v, enc);
        size_t m = nw < nch ? nw : nch;
        for (size_t ch = 0; ch < m; ch++) {
            raw[i * nch + ch] = enc[ch];
        }
    }
    pc_status rc = pc_surface_write_region(surf, area, raw, rawlen, err);
    pc_surface_prune(surf);
    free(orig);
    free(px);
    free(cover);
    free(raw);
    return rc;
}

static pc_status run_map_pixels(pc_engine_session *s, const char *label, px_fn fn, void *ud, pc_error *err)
{
    map_ctx c = {.fn = fn, .ud = ud};
    return pc_engine_session_edit(s, label, map_edit, &c, err);
}

/* ---- Equalize ---------------------------------------------------------------------------- */

static void equalize_fn(pc_rect area, float (*px)[4], const float *cover, size_t count, void *ud)
{
    (void)area;
    (void)ud;
    double hist[256];
    memset(hist, 0, sizeof hist);
    for (size_t i = 0; i < count; i++) {
        if (cover[i] > 0.0f && px[i][3] > 0.0f) {
            for (int ch = 0; ch < 3; ch++) {
                float v = px[i][ch] < 0.0f ? 0.0f : (px[i][ch] > 1.0f ? 1.0f : px[i][ch]);
                size_t bin = (size_t)lrintf(v * 255.0f);
                if (bin > 255) {
                    bin = 255;
                }
                hist[bin] += 1.0;
            }
        }
    }
    double total = 0.0;
    for (int i = 0; i < 256; i++) {
        total += hist[i];
    }
    if (total == 0.0) {
        return;
    }
    double first = 0.0;
    for (int i = 0; i < 256; i++) {
        if (hist[i] > 0.0) {
            first = hist[i];
            break;
        }
    }
    float lut[256];
    double acc = 0.0;
    for (int i = 0; i < 256; i++) {
        acc += hist[i];
        if (total > first) {
            double t = (acc - first) / (total - first);
            if (t < 0.0) {
                t = 0.0;
            } else if (t > 1.0) {
                t = 1.0;
            }
            lut[i] = (float)t;
        } else {
            lut[i] = (float)i / 255.0f;
        }
    }
    for (size_t i = 0; i < count; i++) {
        for (int ch = 0; ch < 3; ch++) {
            float v = px[i][ch] < 0.0f ? 0.0f : (px[i][ch] > 1.0f ? 1.0f : px[i][ch]);
            size_t bin = (size_t)lrintf(v * 255.0f);
            if (bin > 255) {
                bin = 255;
            }
            px[i][ch] = lut[bin];
        }
    }
}

static pc_status run_equalize(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    if (run_map_pixels(s, "Equalize", equalize_fn, NULL, err) != PC_OK) {
        return err_code(err);
    }
    return pc_engine_result_null(out, err);
}

/* ---- Average ----------------------------------------------------------------------------- */

static void average_fn(pc_rect area, float (*px)[4], const float *cover, size_t count, void *ud)
{
    (void)area;
    (void)ud;
    double sum[3] = {0.0, 0.0, 0.0};
    double wsum = 0.0, asum = 0.0, n = 0.0;
    for (size_t i = 0; i < count; i++) {
        double w = (double)(px[i][3] * cover[i]);
        for (int ch = 0; ch < 3; ch++) {
            sum[ch] += (double)px[i][ch] * w;
        }
        wsum += w;
        asum += (double)(px[i][3] * cover[i]);
        n += (double)cover[i];
    }
    if (wsum <= 0.0) {
        return;
    }
    float avg[4];
    avg[0] = (float)(sum[0] / wsum);
    avg[1] = (float)(sum[1] / wsum);
    avg[2] = (float)(sum[2] / wsum);
    avg[3] = (float)(asum / (n > 1e-9 ? n : 1e-9));
    for (size_t i = 0; i < count; i++) {
        px[i][0] = avg[0];
        px[i][1] = avg[1];
        px[i][2] = avg[2];
        px[i][3] = avg[3];
    }
}

static pc_status run_average(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    if (run_map_pixels(s, "Average", average_fn, NULL, err) != PC_OK) {
        return err_code(err);
    }
    return pc_engine_result_null(out, err);
}

/* ---- Clouds / Difference Clouds ---------------------------------------------------------- */

static float clouds_hash(int32_t x, int32_t y, uint32_t seed)
{
    uint32_t h = (uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 13;
    h = h * 0x5bd1e995u;
    h ^= h >> 15;
    return (float)(h & 0x00ffffffu) / (float)0x00ffffffu;
}

static float clouds_value(float x, float y, float base, uint32_t seed)
{
    float sum = 0.0f, amp = 1.0f, freq = 1.0f / base, norm = 0.0f;
    for (uint32_t o = 0; o < 8u; o++) {
        float fx = x * freq, fy = y * freq;
        int32_t ix = (int32_t)floorf(fx), iy = (int32_t)floorf(fy);
        float tx = fx - (float)ix, ty = fy - (float)iy;
        float sx = tx * tx * (3.0f - 2.0f * tx), sy = ty * ty * (3.0f - 2.0f * ty);
        uint32_t sd = seed + o * 7919u;
        float top = clouds_hash(ix, iy, sd) + (clouds_hash(ix + 1, iy, sd) - clouds_hash(ix, iy, sd)) * sx;
        float bot = clouds_hash(ix, iy + 1, sd)
                    + (clouds_hash(ix + 1, iy + 1, sd) - clouds_hash(ix, iy + 1, sd)) * sx;
        sum += (top + (bot - top) * sy) * amp;
        norm += amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    float v = (sum / norm - 0.5f) * 2.2f + 0.5f;
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

typedef struct {
    float fg[4];
    float bg[4];
    float base;
    uint32_t seed;
    bool difference;
} clouds_ud;

static void clouds_fn(pc_rect area, float (*px)[4], const float *cover, size_t count, void *ud)
{
    (void)cover;
    clouds_ud *c = ud;
    size_t w = pc_rect_width(area);
    for (size_t i = 0; i < count; i++) {
        float x = (float)(area.x0 + (int32_t)(i % w));
        float y = (float)(area.y0 + (int32_t)(i / w));
        float t = clouds_value(x, y, c->base, c->seed);
        float col[3];
        for (int k = 0; k < 3; k++) {
            col[k] = c->fg[k] + (c->bg[k] - c->fg[k]) * t;
        }
        if (c->difference) {
            for (int k = 0; k < 3; k++) {
                px[i][k] = fabsf(px[i][k] - col[k]);
            }
        } else {
            px[i][0] = col[0];
            px[i][1] = col[1];
            px[i][2] = col[2];
            px[i][3] = 1.0f;
        }
    }
}

static pc_status run_clouds_impl(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                bool difference, pc_error *err)
{
    clouds_ud u;
    memset(&u, 0, sizeof u);
    const pc_engine_tool_state *t = pc_engine_session_tools(s);
    memcpy(u.fg, t->foreground, sizeof u.fg);
    memcpy(u.bg, t->background, sizeof u.bg);
    u.difference = difference;
    uint64_t seed = 0;
    yyjson_val *sv = pc_json_get(p, "seed");
    if (sv != NULL) {
        (void)pc_json_as_u64(sv, &seed);
    }
    u.seed = (uint32_t)seed;
    double size = 256.0;
    const pc_document *doc = active_doc(s);
    if (doc != NULL) {
        double mx = doc->size.width > doc->size.height ? doc->size.width : doc->size.height;
        size = mx;
    }
    double base = size / 4.0;
    if (base < 16.0) {
        base = 16.0;
    } else if (base > 512.0) {
        base = 512.0;
    }
    u.base = (float)base;
    if (run_map_pixels(s, difference ? "Difference Clouds" : "Clouds", clouds_fn, &u, err) != PC_OK) {
        return err_code(err);
    }
    return pc_engine_result_null(out, err);
}

static pc_status run_clouds(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return run_clouds_impl(s, p, out, false, err);
}

static pc_status run_difference_clouds(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return run_clouds_impl(s, p, out, true, err);
}

/* ============================================================== Edit > Stroke */

typedef struct {
    size_t width;
    float color[4];
    bool has_picked;
    float picked[4];
    pc_blend_mode mode;
    float opacity;
    bool preserve;
    int loc; /* 0 center, 1 inside, 2 outside */
    pc_layer_id id;
} stroke_ctx;

static pc_status stroke_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *ud, pc_error *err)
{
    (void)has_active;
    (void)active;
    stroke_ctx *c = ud;
    pc_rect canvas = pc_document_bounds(doc);
    size_t w = pc_rect_width(canvas), h = pc_rect_height(canvas);
    if (w == 0 || h == 0) {
        return PC_OK;
    }
    size_t wh = w * h;
    float *edge = NULL;
    if (doc->has_selection) {
        size_t elen = 0;
        if (pc_algo_selection_mask_from_surface(&doc->selection, canvas, &edge, &elen, err) != PC_OK) {
            return err_code(err);
        }
        if (edge == NULL) {
            edge = calloc(wh, sizeof *edge);
            if (edge == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "stroke");
            }
        }
    } else {
        const pc_layer *l = pc_document_layer(doc, c->id);
        const pc_surface *surf = l == NULL ? NULL : pc_layer_surface(l);
        if (surf == NULL) {
            return pc_engine_err_other(err, "not a pixel layer");
        }
        float (*rgba)[4] = malloc(wh * sizeof *rgba);
        edge = malloc(wh * sizeof *edge);
        if (rgba == NULL || edge == NULL) {
            free(rgba);
            free(edge);
            return pc_fail(err, PC_ERR_NOMEM, "stroke");
        }
        if (pc_surface_read_rgba_into(surf, canvas, rgba, wh, err) != PC_OK) {
            free(rgba);
            free(edge);
            return err_code(err);
        }
        for (size_t i = 0; i < wh; i++) {
            edge[i] = rgba[i][3];
        }
        free(rgba);
    }

    size_t pad = c->width + 1;
    size_t pw = w + 2 * pad, ph = h + 2 * pad;
    float *padded = calloc(pw * ph, sizeof *padded);
    if (padded == NULL) {
        free(edge);
        return pc_fail(err, PC_ERR_NOMEM, "stroke");
    }
    for (size_t r = 0; r < h; r++) {
        memcpy(padded + (r + pad) * pw + pad, edge + r * w, w * sizeof *padded);
    }
    free(edge);

    size_t out_px, in_px;
    if (c->loc == 1) { /* inside */
        out_px = 0;
        in_px = c->width;
    } else if (c->loc == 2) { /* outside */
        out_px = c->width;
        in_px = 0;
    } else { /* center */
        out_px = c->width / 2;
        in_px = (c->width + 1) / 2;
    }
    bool hard = true;
    for (size_t i = 0; i < pw * ph; i++) {
        if (padded[i] > 0.0f && padded[i] < 1.0f) {
            hard = false;
            break;
        }
    }
    float in_r = (hard && in_px > 0) ? (float)in_px - 0.5f : (float)in_px;
    float *outer = NULL;
    float *inner = NULL;
    if (pc_algo_selection_expand(padded, pw, ph, (float)out_px, &outer, err) != PC_OK) {
        free(padded);
        return err_code(err);
    }
    if (pc_algo_selection_contract(padded, pw, ph, in_r, &inner, err) != PC_OK) {
        free(padded);
        free(outer);
        return err_code(err);
    }
    free(padded);
    if (hard) {
        for (size_t i = 0; i < pw * ph; i++) {
            outer[i] = outer[i] > 0.0f ? 1.0f : 0.0f;
            inner[i] = inner[i] >= 1.0f ? 1.0f : 0.0f;
        }
    }
    float *band = malloc(wh * sizeof *band);
    if (band == NULL) {
        free(outer);
        free(inner);
        return pc_fail(err, PC_ERR_NOMEM, "stroke");
    }
    for (size_t r = 0; r < h; r++) {
        for (size_t col = 0; col < w; col++) {
            size_t oi = (r + pad) * pw + (col + pad);
            float v = outer[oi] - inner[oi];
            v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            band[r * w + col] = v;
        }
    }
    free(outer);
    free(inner);
    pc_surface band_surf;
    if (pc_algo_selection_mask_to_surface(band, wh, canvas, &band_surf, err) != PC_OK) {
        free(band);
        return err_code(err);
    }
    free(band);

    bool lock = pc_document_effective_locks(doc, c->id).transparency;
    pc_surface *target = NULL;
    if (pc_engine_paint_surface(doc, c->id, NULL, &target, err) != PC_OK) {
        pc_surface_fini(&band_surf);
        return err_code(err);
    }
    pc_rect area = pc_rect_intersect(pc_surface_content_bounds(&band_surf), canvas);
    pc_status rc = PC_OK;
    if (!pc_rect_is_empty(area)) {
        rc = pc_engine_blend_color_mask(target, area, c->color, c->has_picked ? c->picked : NULL,
                                        &band_surf, c->mode, c->opacity, c->preserve || lock, err);
        if (rc == PC_OK) {
            pc_surface_prune(target);
        }
    }
    pc_surface_fini(&band_surf);
    return rc;
}

static pc_status run_stroke(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    stroke_ctx c;
    memset(&c, 0, sizeof c);
    const pc_engine_tool_state *t = pc_engine_session_tools(s);
    pc_engine_param_color(p, "color", t->foreground, c.color);
    float rgb[3] = {c.color[0], c.color[1], c.color[2]};
    c.has_picked = pc_engine_tool_state_picked_cmyk(t, rgb, c.picked);

    c.mode = PC_BLEND_NORMAL;
    yyjson_val *mv = pc_json_get(p, "mode");
    if (mv != NULL && !yyjson_is_null(mv)) {
        size_t mlen = 0;
        const char *ms = pc_json_as_str(mv, &mlen);
        if (ms == NULL) {
            return pc_engine_err_bad_params(err, "edit.stroke", "mode must be a blend mode name");
        }
        if (!pc_engine_blend_from_str(ms, mlen, &c.mode) || c.mode == PC_BLEND_PASS_THROUGH) {
            return pc_engine_err_bad_params(err, "edit.stroke", "unknown blend mode `%.*s`", (int)mlen, ms);
        }
    }

    c.opacity = 1.0f;
    yyjson_val *ov = pc_json_get(p, "opacity");
    if (ov != NULL && !yyjson_is_null(ov)) {
        double o = 0.0;
        if (!pc_json_as_f64(ov, &o) || !isfinite(o)) {
            return pc_engine_err_bad_params(err, "edit.stroke",
                                            "opacity must be a finite number from 0 to 100");
        }
        if (o < 0.0) {
            o = 0.0;
        } else if (o > 100.0) {
            o = 100.0;
        }
        c.opacity = (float)(o / 100.0);
    }

    c.preserve = false;
    yyjson_val *pv = pc_json_get(p, "preserveTransparency");
    if (pv != NULL && !yyjson_is_null(pv)) {
        if (!pc_json_as_bool(pv, &c.preserve)) {
            return pc_engine_err_bad_params(err, "edit.stroke", "preserveTransparency must be true or false");
        }
    }

    double width = 1.0;
    yyjson_val *wv = pc_json_get(p, "width");
    double tmp = 0.0;
    if (wv != NULL && pc_json_as_f64(wv, &tmp) && isfinite(tmp)) {
        width = tmp;
    }
    if (width < 1.0) {
        width = 1.0;
    } else if (width > 250.0) {
        width = 250.0;
    }
    c.width = (size_t)ceil(width);

    c.loc = 0;
    yyjson_val *lv = pc_json_get(p, "location");
    if (lv != NULL) {
        size_t llen = 0;
        const char *ls = pc_json_as_str(lv, &llen);
        if (ls != NULL) {
            if (llen == 6 && memcmp(ls, "inside", 6) == 0) {
                c.loc = 1;
            } else if (llen == 7 && memcmp(ls, "outside", 7) == 0) {
                c.loc = 2;
            }
        }
    }

    if (pc_engine_layer_param(s, p, &c.id, err) != PC_OK) {
        return err_code(err);
    }
    if (pc_engine_session_edit(s, "Stroke", stroke_edit, &c, err) != PC_OK) {
        return err_code(err);
    }
    return pc_engine_result_null(out, err);
}

/* ============================================================== Rasterize */

static const char *kind_of(const pc_layer *l)
{
    if (l == NULL) {
        return NULL;
    }
    switch (l->content.kind) {
    case PC_LAYER_TEXT:
        return "type";
    case PC_LAYER_SHAPE:
        return "shape";
    case PC_LAYER_FILL:
        return "fill";
    case PC_LAYER_SMART:
        return "smart";
    default:
        return NULL;
    }
}

/* Pixels of a fill or smart-object layer's content alone (no mask, effects or opacity), rendered
 * in the document's format (port of extra_cmds::content_pixels). *out initialised on success. */
static pc_status content_pixels(const pc_document *doc, const pc_layer *l, pc_surface *out, pc_error *err)
{
    pc_document solo;
    if (pc_document_init(&solo, "", doc->size, doc->mode, doc->depth, err) != PC_OK) {
        return err_code(err);
    }
    pc_layer copy;
    if (pc_layer_clone(l, &copy, err) != PC_OK) {
        pc_document_fini(&solo);
        return err_code(err);
    }
    if (copy.has_mask) {
        pc_layer_mask_fini(&copy.mask);
        copy.has_mask = false;
    }
    if (copy.has_vector_mask) {
        pc_vector_mask_fini(&copy.vector_mask);
        copy.has_vector_mask = false;
    }
    pc_effects_fini(&copy.effects);
    memset(&copy.effects, 0, sizeof copy.effects);
    copy.opacity = 1.0f;
    copy.fill_opacity = 1.0f;
    copy.blend = PC_BLEND_NORMAL;
    copy.clipped = false;
    copy.visible = true;
    pc_layer_id nid = 0;
    if (pc_document_insert_above(&solo, NULL, &copy, &nid, err) != PC_OK) {
        pc_layer_fini(&copy);
        pc_document_fini(&solo);
        return err_code(err);
    }
    pc_status rc = pc_engine_composite_layers(&solo, pc_document_pixel_format(doc), NULL, out, err);
    pc_document_fini(&solo);
    return rc;
}

/* Drop the PSD additional-layer blocks whose keys appear in `keys` (n four-byte keys). */
static void strip_psd_blocks(pc_layer *l, const char (*keys)[4], size_t nkeys)
{
    size_t w = 0;
    for (size_t r = 0; r < l->npsd_blocks; r++) {
        bool drop = false;
        for (size_t k = 0; k < nkeys; k++) {
            if (memcmp(l->psd_blocks[r].key, keys[k], 4) == 0) {
                drop = true;
                break;
            }
        }
        if (drop) {
            pc_blob_release(l->psd_blocks[r].data);
            continue;
        }
        if (w != r) {
            l->psd_blocks[w] = l->psd_blocks[r];
        }
        w++;
    }
    l->npsd_blocks = w;
}

static pc_status set_raster_content(pc_layer *l, pc_surface *surface, pc_pixel_format fmt, pc_error *err)
{
    pc_surface final = *surface;
    *surface = (pc_surface){0};
    if (!pc_pixel_format_eq(pc_surface_format(&final), fmt)) {
        pc_surface conv;
        if (pc_surface_convert(&final, fmt, &conv, err) != PC_OK) {
            pc_surface_fini(&final);
            return err_code(err);
        }
        pc_surface_fini(&final);
        final = conv;
    }
    pc_layer_content_fini(&l->content);
    l->content.kind = PC_LAYER_RASTER;
    l->content.u.raster = final;
    return PC_OK;
}

typedef struct {
    pc_layer_id id;
    bool smart; /* smart (true) vs fill (false) */
} raster_inline_ctx;

static pc_status raster_inline_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *ud,
                                   pc_error *err)
{
    (void)has_active;
    (void)active;
    raster_inline_ctx *c = ud;
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    pc_surface surface;
    memset(&surface, 0, sizeof surface);
    if (c->smart) {
        if (l->content.kind != PC_LAYER_SMART) {
            return pc_engine_err_other(err, "layer %llu is not a smart object",
                                       (unsigned long long)c->id);
        }
        pc_smart_object *sm = &l->content.u.smart;
        if (!sm->has_cache) {
            bool updated = false;
            (void)pc_engine_smart_refresh_layer(doc, l, &updated, err);
            sm = &l->content.u.smart; /* re-fetch; l unchanged */
        }
        if (sm->has_cache) {
            surface = sm->cache;
            sm->has_cache = false;
            memset(&sm->cache, 0, sizeof sm->cache);
        } else {
            if (content_pixels(doc, l, &surface, err) != PC_OK) {
                return err_code(err);
            }
        }
        if (set_raster_content(l, &surface, fmt, err) != PC_OK) {
            pc_surface_fini(&surface);
            return err_code(err);
        }
        static const char keys[3][4] = {{'S', 'o', 'L', 'd'}, {'S', 'o', 'L', 'E'}, {'P', 'l', 'L', 'd'}};
        strip_psd_blocks(l, keys, 3);
    } else {
        if (content_pixels(doc, l, &surface, err) != PC_OK) {
            return err_code(err);
        }
        if (set_raster_content(l, &surface, fmt, err) != PC_OK) {
            pc_surface_fini(&surface);
            return err_code(err);
        }
        if (l->has_fill_cache) {
            pc_fill_fini(&l->fill_cache.fill);
            pc_surface_fini(&l->fill_cache.surface);
            l->has_fill_cache = false;
        }
        static const char keys[6][4] = {{'S', 'o', 'C', 'o'}, {'G', 'd', 'F', 'l'}, {'P', 't', 'F', 'l'},
                                        {'S', 'o', 'L', 'd'}, {'P', 'l', 'L', 'd'}, {'S', 'o', 'L', 'E'}};
        strip_psd_blocks(l, keys, 6);
    }
    return PC_OK;
}

/* Rasterize the content of one layer; `only` restricts it to one kind (NULL = any). *did reports
 * whether anything was done. */
static pc_status rasterize_content(pc_engine_session *s, pc_layer_id id, const char *only,
                                  const char *key, bool *did, pc_error *err)
{
    *did = false;
    const pc_document *doc = active_doc(s);
    if (doc == NULL) {
        return pc_engine_err_no_document(err);
    }
    const char *kind = kind_of(pc_document_layer(doc, id));
    if (kind == NULL || (only != NULL && strcmp(only, kind) != 0)) {
        return PC_OK;
    }
    char buf[128];
    pc_json_value sub;
    memset(&sub, 0, sizeof sub);
    if (strcmp(kind, "type") == 0) {
        (void)snprintf(buf, sizeof buf, "{\"layer\":%llu,\"coalesce\":\"%s\"}",
                       (unsigned long long)id, key);
        pc_status rc = pc_engine_session_execute_json(s, "type.rasterize", buf, &sub, err);
        pc_json_value_fini(&sub);
        if (rc != PC_OK) {
            return rc;
        }
    } else if (strcmp(kind, "shape") == 0) {
        (void)snprintf(buf, sizeof buf, "{\"layer\":%llu,\"coalesce\":\"%s\"}",
                       (unsigned long long)id, key);
        pc_status rc = pc_engine_session_execute_json(s, "layer.rasterize.shape", buf, &sub, err);
        pc_json_value_fini(&sub);
        if (rc != PC_OK) {
            return rc;
        }
    } else {
        raster_inline_ctx c = {.id = id, .smart = strcmp(kind, "smart") == 0};
        set_coalesce(s, key);
        pc_status rc = pc_engine_session_edit(s, c.smart ? "Rasterize Smart Object" : "Rasterize Fill Content",
                                              raster_inline_edit, &c, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    *did = true;
    return PC_OK;
}

static pc_status rasterize_one(pc_engine_session *s, pc_layer_id id, const char *only, const char *key,
                              bool *did, pc_error *err)
{
    *did = false;
    const pc_document *doc = active_doc(s);
    bool vector_mask = only == NULL && doc != NULL && pc_document_layer(doc, id) != NULL
                       && pc_document_layer(doc, id)->has_vector_mask;
    bool content_did = false;
    pc_status rc = rasterize_content(s, id, only, key, &content_did, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (vector_mask) {
        char buf[128];
        pc_json_value sub;
        memset(&sub, 0, sizeof sub);
        (void)snprintf(buf, sizeof buf, "{\"layer\":%llu,\"coalesce\":\"%s\"}",
                       (unsigned long long)id, key);
        rc = pc_engine_session_execute_json(s, "layer.rasterize.vectorMask", buf, &sub, err);
        pc_json_value_fini(&sub);
        if (rc != PC_OK) {
            return rc;
        }
    }
    *did = content_did || vector_mask;
    return PC_OK;
}

static pc_status rasterize(pc_engine_session *s, yyjson_val *p, const char *only, pc_json_value *out,
                          pc_error *err)
{
    pc_layer_id *ids = NULL;
    size_t n = 0;
    if (collect_targets(s, p, &ids, &n, NULL, err) != PC_OK) {
        return err_code(err);
    }
    bool many = n > 1;
    char key[64];
    step_key(s, "rasterize", key, sizeof key);
    pc_layer_id *done = malloc((n == 0 ? 1 : n) * sizeof *done);
    if (done == NULL) {
        free(ids);
        return pc_fail(err, PC_ERR_NOMEM, "rasterize");
    }
    size_t ndone = 0;
    for (size_t i = 0; i < n; i++) {
        bool did = false;
        pc_status rc = rasterize_one(s, ids[i], only, key, &did, err);
        if (rc != PC_OK) {
            free(ids);
            free(done);
            return rc;
        }
        if (did) {
            done[ndone++] = ids[i];
        }
    }
    free(ids);
    if (ndone == 0) {
        free(done);
        if (only != NULL) {
            return pc_engine_err_other(err, many ? "none of the selected layers is a %s layer"
                                                  : "the layer is not a %s layer",
                                       only);
        }
        return pc_engine_err_other(err, many ? "none of the selected layers has anything to rasterize"
                                              : "the layer has nothing to rasterize");
    }
    pc_layer_id active = done[0];
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d != NULL && d->has_active_layer) {
        for (size_t i = 0; i < ndone; i++) {
            if (done[i] == d->active_layer) {
                active = d->active_layer;
                break;
            }
        }
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        free(done);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", (uint64_t)active);
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    for (size_t i = 0; i < ndone; i++) {
        yyjson_mut_arr_add_uint(rd, arr, (uint64_t)done[i]);
    }
    yyjson_mut_obj_add_val(rd, root, "layers", arr);
    yyjson_mut_doc_set_root(rd, root);
    free(done);
    return PC_OK;
}

static pc_status run_rasterize_layer(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return rasterize(s, p, NULL, out, err);
}
static pc_status run_rasterize_type(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return rasterize(s, p, "type", out, err);
}
static pc_status run_rasterize_fill(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return rasterize(s, p, "fill", out, err);
}
static pc_status run_rasterize_smart(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return rasterize(s, p, "smart", out, err);
}

/* The no-`layer` path of layer.rasterize.shape (registered by E11's vector_specs.c): rasterize
 * every selected shape layer, each dispatching back to layer.rasterize.shape with its own id. */
pc_status pc_engine_rasterize_selected_shapes(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                              pc_error *err)
{
    return rasterize(s, p, "shape", out, err);
}

static pc_status run_rasterize_all(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    const pc_document *doc = active_doc(s);
    if (doc == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_walk w;
    if (pc_document_walk(doc, &w, err) != PC_OK) {
        return err_code(err);
    }
    size_t cnt = w.len;
    pc_layer_id *ids = malloc((cnt == 0 ? 1 : cnt) * sizeof *ids);
    if (ids == NULL) {
        pc_walk_fini(&w);
        return pc_fail(err, PC_ERR_NOMEM, "rasterize");
    }
    for (size_t i = 0; i < cnt; i++) {
        ids[i] = w.items[i].layer->id;
    }
    pc_walk_fini(&w);
    char key[64];
    step_key(s, "rasterizeAll", key, sizeof key);
    size_t done = 0;
    for (size_t i = 0; i < cnt; i++) {
        bool did = false;
        pc_status rc = rasterize_one(s, ids[i], NULL, key, &did, err);
        if (rc != PC_OK) {
            free(ids);
            return rc;
        }
        if (did) {
            done++;
        }
    }
    free(ids);
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "rasterized", done);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ============================================================== registry */

const pc_engine_command *pc_engine_extra_specs(size_t *n)
{
    static const char *const M_EDIT[] = {"Edit"};
    static const char *const M_SELECT[] = {"Select"};
    static const char *const M_IMG_ADJ[] = {"Image", "Adjustments"};
    static const char *const M_LAYER_NEW[] = {"Layer", "New"};
    static const char *const M_LAYER_STYLE[] = {"Layer", "Layer Style"};
    static const char *const M_LAYER_MASK[] = {"Layer", "Layer Mask"};
    static const char *const M_LAYER_RASTER[] = {"Layer", "Rasterize"};
    static const char *const M_LAYER_DELETE[] = {"Layer", "Delete"};
    static const char *const M_FILE_SCRIPTS[] = {"File", "Scripts"};
    static const char *const M_LAYER[] = {"Layer"};
    static const char *const M_FILTER_BLUR[] = {"Filter", "Blur"};
    static const char *const M_FILTER_RENDER[] = {"Filter", "Render"};

    static const pc_engine_command specs[] = {
        {"edit.stroke", "Stroke\xE2\x80\xA6", M_EDIT, 1, NULL,
         "{\"width\":1..250=1,\"color\":\"#rrggbb|[r,g,b,a]\"=foreground,\"location\":"
         "\"inside|center|outside\"=\"center\",\"mode\":\"any layer blend mode\"=\"normal\","
         "\"opacity\":0..100=100,\"preserveTransparency\":bool=false}",
         has_pixels, run_stroke, true},
        {"select.reselect", "Reselect", M_SELECT, 1, "Cmd+Shift+D", "{}", can_reselect, run_reselect, true},
        {"image.adjustments.equalize", "Equalize", M_IMG_ADJ, 2, NULL, "{}", has_pixels, run_equalize, true},
        {"layer.new.layerFromBackground", "Layer From Background\xE2\x80\xA6", M_LAYER_NEW, 2, NULL,
         "{\"ifPresent\":bool=false (true: a document without a Background is left as it is, as a "
         "Photoshop action's step needs on an image that opened as a normal layer),\"select\":bool=true "
         "(false: the targeted layer stays targeted)}",
         has_background, run_layer_from_background, true},
        {"layer.layerStyle.hideAllEffects", "Hide All Effects", M_LAYER_STYLE, 2, NULL, "{}", has_doc,
         run_hide_all_effects, true},
        {"layer.layerStyle.showAllEffects", "Show All Effects", NULL, 0, NULL, "{}", has_doc,
         run_show_all_effects, true},
        {"layer.layerMask.enabled", "Disable Layer Mask", M_LAYER_MASK, 2, NULL,
         "{\"layer\":id?,\"enabled\":bool? (default: toggle)}", has_mask, run_mask_enabled, true},
        {"layer.layerMask.linked", "Unlink Layer Mask", M_LAYER_MASK, 2, NULL,
         "{\"layer\":id?,\"linked\":bool? (default: toggle)}", has_mask, run_mask_linked, true},
        {"layer.rasterize.layer", "Layer", M_LAYER_RASTER, 2, NULL,
         "{\"layer\":id?} (no layer: every selected layer it applies to)", has_layer,
         run_rasterize_layer, true},
        {"layer.rasterize.allLayers", "All Layers", M_LAYER_RASTER, 2, NULL, "{}", has_doc,
         run_rasterize_all, true},
        {"layer.rasterize.type", "Type", M_LAYER_RASTER, 2, NULL,
         "{\"layer\":id?} (no layer: every selected layer it applies to)", has_layer,
         run_rasterize_type, true},
        {"layer.rasterize.fillContent", "Fill Content", M_LAYER_RASTER, 2, NULL,
         "{\"layer\":id?} (no layer: every selected layer it applies to)", has_layer,
         run_rasterize_fill, true},
        {"layer.rasterize.smartObject", "Smart Object", M_LAYER_RASTER, 2, NULL,
         "{\"layer\":id?} (no layer: every selected layer it applies to)", has_layer,
         run_rasterize_smart, true},
        {"layer.delete.hiddenLayers", "Hidden Layers", M_LAYER_DELETE, 2, NULL, "{}", has_doc,
         run_delete_hidden, true},
        {"file.scripts.deleteAllEmptyLayers", "Delete All Empty Layers", M_FILE_SCRIPTS, 2, NULL, "{}",
         has_doc, run_delete_empty, true},
        {"layer.ungroupLayers", "Ungroup Layers", M_LAYER, 1, "Cmd+Shift+G", "{\"layer\":id?}",
         is_group_layer, run_ungroup, true},
        {"layer.hideLayers", "Hide Layers", M_LAYER, 1, "Cmd+,",
         "{\"layer\":id?} (shows them instead when every targeted layer is already hidden, so "
         "\xE2\x8C\x98, toggles)",
         has_layer, run_hide_layers, true},
        {"layer.showLayers", "Show Layers", NULL, 0, NULL, "{\"layer\":id?}", has_layer,
         run_show_layers, true},
        {"layer.toggleLockTransparency", "Lock Transparent Pixels", NULL, 0, "/",
         "{\"layer\":id?} (toggles; several layers selected: locks them all unless every one is locked)",
         has_layer, run_lock_transparency, true},
        {"layer.showOnly", "Show Only This Layer", NULL, 0, NULL,
         "{\"layer\":id?} \xE2\x86\x92 {shownAlone} (\xE2\x8C\xA5-click a layer's eye; again restores "
         "the other layers' visibility)",
         has_layer, run_show_only, true},
        {"filter.blur.average", "Average", M_FILTER_BLUR, 2, NULL, "{}", has_pixels, run_average, true},
        {"filter.render.clouds", "Clouds", M_FILTER_RENDER, 2, NULL, "{\"seed\":u32=0}", has_pixels,
         run_clouds, true},
        {"filter.render.differenceClouds", "Difference Clouds", M_FILTER_RENDER, 2, NULL,
         "{\"seed\":u32=0}", has_pixels, run_difference_clouds, true},
    };
    *n = sizeof specs / sizeof specs[0];
    return specs;
}

/* =====================================================================================
 * §7 -- extra_cmds.rs entries NOT registered here, and the commands the coordinator routed to
 * E12-extra that live in other reference modules:
 *
 *  - image.revealAll: already registered by E3 (image_cmds.c); not duplicated here.
 *  - edit.transform.{rotate180,rotate90Cw,rotate90Ccw,flipHorizontal,flipVertical}: these dispatch
 *    to edit.transform, which no chunk has ported. Owner: the Free-Transform follow-up (E4's
 *    transform_cmds deferred "FULL transform commands" to this chunk's background_to_layer export,
 *    now available via pc_engine_background_to_layer). Deferred until edit.transform exists.
 *  - edit.pasteSpecial.pasteInto / pasteOutside: need channel_clip::paste and edit_cmds::
 *    paste_to_target plus layer_multi_cmds::note_insert, which E4/E7 did not export. Owner: E12-extra
 *    follow-up after the channel-clipboard export.
 *  - layer.layerStyle.copyLayerStyle / pasteLayerStyle: need a Session style_clipboard EXTENSION
 *    field and effects "in_mode" recolouring (layer_style). Owner: E12-extra / layer_style follow-up.
 *  - layer.rasterize.shape: the reference implements it in vector_cmds.rs (E11), which did not
 *    register it in C; rasterize of a shape layer dispatches to it and surfaces a graceful
 *    not-found error until E11's follow-up registers it. Owner: E11.
 *  - edit.fade, edit.purge.*: live in the reference's edit_menu_cmds.rs (a separate, large module:
 *    Fade, Define Pattern, Define Custom Shape, Purge), not extra_cmds.rs. Owner: an edit_menu_cmds
 *    port (E12 follow-up). Purge's compositor mask-cache purge export is available as the compose
 *    cache API; the smart-filter stack purge is pc_engine_smart_purge_stack_cache (smart_cmds).
 *  - filter.other.colorToAlpha: the reference's color_to_alpha_cmds.rs, registered via a separate
 *    commands.rs v.extend; it wraps the algo color_to_alpha filter through the E9 filter runner.
 *    Owner: a color_to_alpha_cmds port (E9/E12 follow-up).
 *  - layer.layerMask.apply / hideSelection / fromTransparency: the reference's layer_menu_cmds.rs,
 *    not extra_cmds.rs. Owner: a layer_menu_cmds port.
 *
 * Divergence noted: layer.showOnly keeps no view-state snapshot of prior visibility (the reference's
 * DocState.show_only), so un-soloing reveals every layer (the reference's own no-snapshot fallback)
 * rather than restoring the pre-solo visibility. Adding the DocState field is a follow-up.
 * ===================================================================================== */
