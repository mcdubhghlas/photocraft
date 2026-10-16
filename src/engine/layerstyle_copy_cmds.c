/* Layer > Layer Style > Copy Layer Style / Paste Layer Style (port of the two entries in
 * crates/engine/src/extra_cmds.rs plus the style_targets helper). Copy snapshots the active
 * layer's effects, blend mode, fill opacity and advanced-blending into the session clipboard;
 * Paste writes them onto every targeted layer (recolouring the effects to this document's mode).
 *
 * Never crashes on hostile params (REWRITE_PLAN.md rule 9): a missing layer or empty clipboard is
 * a graceful error, and the targets are validated before any edit. */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "doc/blend_if.h"
#include "doc/doc.h"
#include "doc/effects.h"

#include "engine_internal.h"
#include "layer_cmds.h" /* pc_engine_layer_param, pc_engine_selected, pc_engine_explicit_layer */

void pc_engine_style_clipboard_fini(pc_engine_session *s)
{
    if (s == NULL || !s->has_style_clip) {
        return;
    }
    pc_effects_fini(&s->style_clip_effects);
    s->has_style_clip = false;
}

/* enabled: there must be a layer style on the clipboard (plus an active layer). */
static pc_status has_style_clip(const pc_engine_session *s, pc_error *reason)
{
    pc_status rc = pc_engine_has_layer(s, reason);
    if (rc != PC_OK) {
        return rc;
    }
    if (!s->has_style_clip) {
        return pc_fail(reason, PC_ERR_INVALID, "no layer style has been copied");
    }
    return PC_OK;
}

/* ---- copy ---------------------------------------------------------------------------------- */

static pc_status run_copy(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_layer_id id;
    if (pc_engine_layer_param(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(d->doc), id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    pc_effects copy;
    if (pc_effects_clone(&l->effects, &copy, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_engine_style_clipboard_fini(s);
    s->style_clip_effects = copy;
    s->style_clip_blend = l->blend;
    s->style_clip_fill = l->fill_opacity;
    s->style_clip_advanced = l->advanced;
    s->has_style_clip = true;
    return pc_engine_result_null(out, err);
}

/* ---- targets (layer_multi_cmds::targets + style_targets) ---------------------------------- */

/* The layers the paste applies to: the explicit "layer", else the selection (or the active layer).
 * With more than one, the Background and any not-fitting layer are dropped. *out owned (free). */
static pc_status style_targets(pc_engine_session *s, yyjson_val *p, pc_layer_id **out, size_t *nout,
                               pc_error *err)
{
    *out = NULL;
    *nout = 0;
    bool present = false;
    pc_layer_id explicit_id = 0;
    if (pc_engine_explicit_layer(s, p, &present, &explicit_id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_layer_id *ids = NULL;
    size_t n = 0;
    if (present) {
        ids = malloc(sizeof *ids);
        if (ids == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "paste style");
        }
        ids[0] = explicit_id;
        n = 1;
    } else {
        if (pc_engine_selected(s, &ids, &n, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        if (n == 0) {
            free(ids);
            pc_layer_id one;
            if (pc_engine_layer_param(s, p, &one, err) != PC_OK) {
                return err != NULL ? err->code : PC_ERR_INVALID;
            }
            ids = malloc(sizeof *ids);
            if (ids == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "paste style");
            }
            ids[0] = one;
            n = 1;
        }
    }
    if (n < 2) {
        *out = ids;
        *nout = n;
        return PC_OK;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = d != NULL ? pc_doc_arc_get(d->doc) : NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const pc_layer *l = doc != NULL ? pc_document_layer(doc, ids[i]) : NULL;
        if (l != NULL && !pc_document_is_background(doc, l->id)) {
            ids[k++] = ids[i];
        }
    }
    if (k == 0) {
        free(ids);
        return pc_engine_err_other(err, "none of the selected layers can take this layer style change");
    }
    *out = ids;
    *nout = k;
    return PC_OK;
}

/* ---- paste --------------------------------------------------------------------------------- */

struct paste_ctx {
    const pc_layer_id *ids;
    size_t n;
    const pc_engine_session *s;
};

static pc_status paste_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    (void)active;
    struct paste_ctx *c = vctx;
    const pc_engine_session *s = c->s;
    for (size_t i = 0; i < c->n; i++) {
        pc_layer *l = pc_document_layer_mut(doc, c->ids[i]);
        if (l == NULL) {
            return pc_engine_err_no_layer(err, c->ids[i]);
        }
        pc_effects fx;
        if (pc_effects_clone(&s->style_clip_effects, &fx, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        /* A style copied from a document in another mode takes this one's colours. */
        for (size_t e = 0; e < fx.nitems; e++) {
            pc_effect_in_mode(&fx.items[e], doc->mode);
        }
        pc_effects_fini(&l->effects);
        l->effects = fx;
        l->blend = s->style_clip_blend;
        l->fill_opacity = s->style_clip_fill;
        l->advanced = s->style_clip_advanced;
    }
    return PC_OK;
}

static pc_status run_paste(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    if (!s->has_style_clip) {
        return pc_engine_err_other(err, "no layer style has been copied");
    }
    pc_layer_id *ids = NULL;
    size_t n = 0;
    pc_status rc = style_targets(s, p, &ids, &n, err);
    if (rc != PC_OK) {
        return rc;
    }
    struct paste_ctx c = {ids, n, s};
    rc = pc_engine_session_edit(s, "Paste Layer Style", paste_edit, &c, err);
    free(ids);
    if (rc != PC_OK) {
        return rc;
    }
    return pc_engine_result_null(out, err);
}

/* ---- registry ------------------------------------------------------------------------------ */

static const char *const MENU_STYLE[] = {"Layer", "Layer Style"};

static const pc_engine_command SPECS[] = {
    {"layer.layerStyle.copyLayerStyle", "Copy Layer Style", MENU_STYLE, 2, NULL, "{\"layer\":id?}",
     pc_engine_has_layer, run_copy, true},
    {"layer.layerStyle.pasteLayerStyle", "Paste Layer Style", MENU_STYLE, 2, NULL,
     "{\"layer\":id?} (no layer: every selected layer but the Background)", has_style_clip, run_paste,
     true},
};

const pc_engine_command *pc_engine_layerstyle_copy_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
