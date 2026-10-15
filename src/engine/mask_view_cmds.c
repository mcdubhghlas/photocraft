/* View > layer mask (port of crates/engine/src/mask_view_cmds.rs, #196): ⌥-click a layer-mask
 * thumbnail shows the mask as a grayscale image; ⇧⌥-click (or `\`) shows it as a red rubylith over
 * the composite.
 *
 * This is view state, like the Channels panel's eyes: no history step, a clean document stays
 * clean, and it never changes a pixel. In the reference the state lives on
 * channel_cmds::ChannelView::layer_mask (E7); here it is a standalone DocState field so mask view
 * does not depend on the unported channel chunk (REWRITE_PLAN.md §7). current() re-validates
 * liveness on every read, so a stale value is never reported. This module also carries the two E5
 * hooks relocated out of hooks.c: pc_engine_hook_inspect_mask_view and pc_engine_hook_mask_view_fix. */
#include <string.h>

#include "layer_cmds.h"

#define CMD_ID "view.layerMask"
#define OVERLAY_ID "layer.toggleMaskOverlay"

/* The live mask view of `d`: its layer is the active layer and still has a mask. */
static bool mv_current(const pc_engine_doc_state *d, pc_layer_id *layer, int *mode)
{
    if (!d->has_layer_mask_view || !d->has_active_layer ||
        d->active_layer != d->layer_mask_view_layer) {
        return false;
    }
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(d->doc), d->layer_mask_view_layer);
    if (l == NULL || !l->has_mask) {
        return false;
    }
    *layer = d->layer_mask_view_layer;
    *mode = d->layer_mask_view_mode;
    return true;
}

static const char *mode_name_of(bool present, int mode)
{
    if (!present) {
        return "off";
    }
    return mode == PC_ENGINE_MASK_VIEW_OVERLAY ? "overlay" : "gray";
}

static bool streq(const char *s, size_t len, const char *lit)
{
    return strlen(lit) == len && memcmp(s, lit, len) == 0;
}

/* ---- relocated E5 hooks (were defaults in hooks.c) ----------------------------------------- */

void pc_engine_hook_mask_view_fix(pc_engine_doc_state *st)
{
    if (st->has_layer_mask_view) {
        pc_layer_id l;
        int m;
        if (!mv_current(st, &l, &m)) {
            st->has_layer_mask_view = false;
        }
    }
}

void pc_engine_hook_inspect_mask_view(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                                      const pc_engine_doc_state *d)
{
    pc_layer_id l;
    int m;
    if (mv_current(d, &l, &m)) {
        yyjson_mut_val *v = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_uint(doc, v, "layer", l);
        yyjson_mut_obj_add_str(doc, v, "mode", mode_name_of(true, m));
        yyjson_mut_obj_add_val(doc, obj, "layerMaskView", v);
    } else {
        yyjson_mut_obj_add_null(doc, obj, "layerMaskView");
    }
}

/* ---- commands ----------------------------------------------------------------------------- */

static pc_status has_doc(const pc_engine_session *s, pc_error *reason)
{
    if (pc_engine_session_active(s) != NULL) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "no document open");
}

/* `\` needs a layer mask on the active layer; without one Photoshop's key does nothing. */
static pc_status active_has_mask(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    const pc_layer *l = d->has_active_layer ? pc_document_layer(pc_doc_arc_get(d->doc), d->active_layer)
                                            : NULL;
    if (l == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    if (!l->has_mask) {
        return pc_fail(reason, PC_ERR_INVALID, "layer \"%s\" has no layer mask", pc_str_c(&l->name));
    }
    return PC_OK;
}

/* Core of view.layerMask (shared by the overlay toggle). `p` supplies the optional `layer`. */
static pc_status set_view(pc_engine_session *s, yyjson_val *p, const char *mode, const char *cmd,
                          pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_layer_id layer;
    yyjson_val *lv = pc_json_get(p, "layer");
    if (lv == NULL || yyjson_is_null(lv)) {
        if (!d->has_active_layer) {
            return pc_engine_err_bad_params(err, cmd, "no layer given and no active layer");
        }
        layer = d->active_layer;
    } else {
        uint64_t id;
        if (!pc_json_as_u64(lv, &id)) {
            return pc_engine_err_bad_params(err, cmd, "`layer` must be a layer id");
        }
        layer = id;
    }
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(d->doc), layer);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, layer);
    }
    bool has_mask = l->has_mask;
    char name[256];
    (void)snprintf(name, sizeof name, "%s", pc_str_c(&l->name));

    /* The mode currently shown for THIS layer, if any. */
    pc_layer_id cl;
    int cm;
    bool shown_present = mv_current(d, &cl, &cm) && cl == layer;
    int shown_mode = shown_present ? cm : -1;

    size_t mlen = strlen(mode);
    bool want_present;
    int want_mode = PC_ENGINE_MASK_VIEW_GRAY;
    if (streq(mode, mlen, "off")) {
        want_present = false;
    } else if (streq(mode, mlen, "gray")) {
        want_present = true;
        want_mode = PC_ENGINE_MASK_VIEW_GRAY;
    } else if (streq(mode, mlen, "overlay")) {
        want_present = true;
        want_mode = PC_ENGINE_MASK_VIEW_OVERLAY;
    } else if (streq(mode, mlen, "toggleGray")) {
        want_present = !(shown_present && shown_mode == PC_ENGINE_MASK_VIEW_GRAY);
        want_mode = PC_ENGINE_MASK_VIEW_GRAY;
    } else if (streq(mode, mlen, "toggleOverlay")) {
        want_present = !(shown_present && shown_mode == PC_ENGINE_MASK_VIEW_OVERLAY);
        want_mode = PC_ENGINE_MASK_VIEW_OVERLAY;
    } else {
        return pc_engine_err_bad_params(
            err, cmd, "unknown mode `%s` (off|gray|overlay|toggleGray|toggleOverlay)", mode);
    }

    if (want_present && !has_mask) {
        return pc_engine_err_bad_params(err, cmd, "layer \"%s\" has no layer mask", name);
    }
    /* ⌥-clicking another layer's mask selects that layer, as in Photoshop. */
    if (want_present && !(d->has_active_layer && d->active_layer == layer)) {
        if (pc_engine_session_select_layer(s, layer, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
    }
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    /* "off" for a layer whose mask isn't the one shown leaves the other view alone. */
    bool applies = want_present ||
                   (st->has_layer_mask_view && st->layer_mask_view_layer == layer);
    bool stored_present = st->has_layer_mask_view;
    bool differs = (stored_present != want_present) ||
                   (want_present && (st->layer_mask_view_layer != layer || st->layer_mask_view_mode != want_mode));
    if (applies && differs) {
        st->has_layer_mask_view = want_present;
        if (want_present) {
            st->layer_mask_view_layer = layer;
            st->layer_mask_view_mode = want_mode;
        }
        /* Hiding the mask while every colour channel is off would leave a blank canvas (E7). */
        pc_engine_hook_fix_view(st);
        /* A view change: redraw without recompositing, and keep a clean document clean. */
        bool clean = st->saved_revision == st->revision;
        st->revision++;
        if (clean) {
            st->saved_revision = st->revision;
        }
        st->has_last_damage = true;
        st->last_damage = PC_RECT_EMPTY;
    }

    bool now_present = mv_current(st, &cl, &cm) && cl == layer;
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", layer);
    yyjson_mut_obj_add_str(rd, root, "mode", mode_name_of(now_present, cm));
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_set_view(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *mode = "toggleGray";
    yyjson_val *mv = pc_json_get(p, "mode");
    size_t mlen = 0;
    if (mv != NULL && !yyjson_is_null(mv)) {
        const char *m = pc_json_as_str(mv, &mlen);
        if (m == NULL) {
            return pc_engine_err_bad_params(err, CMD_ID, "`mode` must be a string");
        }
        mode = m;
    }
    return set_view(s, p, mode, CMD_ID, out, err);
}

static pc_status run_toggle_overlay(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    /* {"layer":id?}: ID with mode toggleOverlay (a `mode` param is ignored). */
    if (p != NULL && !yyjson_is_obj(p) && !yyjson_is_null(p)) {
        return pc_engine_err_bad_params(err, OVERLAY_ID, "expected an object");
    }
    return set_view(s, p, "toggleOverlay", OVERLAY_ID, out, err);
}

static const pc_engine_command SPECS[] = {
    {CMD_ID, "View Layer Mask", NULL, 0, NULL,
     "{\"layer\":id?,\"mode\":\"off\"|\"gray\"|\"overlay\"|\"toggleGray\"|\"toggleOverlay\"=\"toggleGray\"} "
     "(gray: the mask alone, ⌥-click its thumbnail; overlay: red rubylith over the composite, ⇧⌥-click "
     "or `\\`; view state, not an undo step; painting while shown paints the mask)",
     has_doc, run_set_view, false},
    {OVERLAY_ID, "Toggle Layer Mask Overlay", NULL, 0, "\\",
     "{\"layer\":id?} (shows or hides the active layer's mask as a 50% red rubylith over the composite, "
     "the `\\` key; view state, not an undo step; painting while shown paints the mask)",
     active_has_mask, run_toggle_overlay, false},
};

const pc_engine_command *pc_engine_mask_view_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
