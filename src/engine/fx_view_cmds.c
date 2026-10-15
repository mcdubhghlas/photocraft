/* Layers panel: showing or hiding a layer's effects list under its row (the triangle beside the
 * fx badge, port of crates/engine/src/fx_view_cmds.rs, E9, #144).
 *
 * View state (DocState::fx_collapsed): no history step, and a clean document stays clean. */
#include <stdlib.h>

#include "engine_internal.h"

#define CMD_ID "layer.setEffectsExpanded"

static bool ids_contains(const pc_layer_id *a, size_t n, pc_layer_id id)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] == id) {
            return true;
        }
    }
    return false;
}

static pc_status has_effects(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_walk w;
    if (pc_document_walk(doc, &w, NULL) != PC_OK) {
        return pc_fail(reason, PC_ERR_INVALID, "no layer has effects");
    }
    bool found = false;
    for (size_t i = 0; i < w.len; i++) {
        if (w.items[i].layer->effects.nitems > 0) {
            found = true;
            break;
        }
    }
    pc_walk_fini(&w);
    if (!found) {
        return pc_fail(reason, PC_ERR_INVALID, "no layer has effects");
    }
    return PC_OK;
}

/* An optional boolean param: *present=false when absent/null; error on a non-boolean. */
static pc_status opt_bool(yyjson_val *p, const char *key, bool *present, bool *out, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, key);
    if (v == NULL || yyjson_is_null(v)) {
        *present = false;
        return PC_OK;
    }
    if (!pc_json_as_bool(v, out)) {
        return pc_engine_err_bad_params(err, CMD_ID, "`%s` must be true or false", key);
    }
    *present = true;
    return PC_OK;
}

/* Every layer in the document that has at least one effect, bottom-to-top. */
static pc_status layers_with_fx(const pc_document *doc, pc_layer_id **out, size_t *n, pc_error *err)
{
    *out = NULL;
    *n = 0;
    pc_walk w;
    if (pc_document_walk(doc, &w, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    pc_layer_id *ids = NULL;
    size_t count = 0;
    if (w.len > 0) {
        ids = calloc(w.len, sizeof *ids);
        if (ids == NULL) {
            pc_walk_fini(&w);
            return pc_fail(err, PC_ERR_NOMEM, "fx layers");
        }
        for (size_t i = 0; i < w.len; i++) {
            if (w.items[i].layer->effects.nitems > 0) {
                ids[count++] = w.items[i].layer->id;
            }
        }
    }
    pc_walk_fini(&w);
    *out = ids;
    *n = count;
    return PC_OK;
}

static pc_status run_set_expanded(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    bool have_want = false, want = false;
    if (opt_bool(p, "expanded", &have_want, &want, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    bool have_all = false, all = false;
    if (opt_bool(p, "all", &have_all, &all, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (!have_all) {
        all = false;
    }

    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);

    /* Resolve the targeted layer (a direct id, not the name-lookup helper). */
    bool have_layer = false;
    pc_layer_id layer = 0;
    yyjson_val *lv = pc_json_get(p, "layer");
    if (lv == NULL || yyjson_is_null(lv)) {
        if (!all) {
            if (!st->has_active_layer) {
                return pc_engine_err_bad_params(err, CMD_ID, "no layer given and no active layer");
            }
            layer = st->active_layer;
            have_layer = true;
        }
    } else if (yyjson_is_uint(lv)) {
        layer = (pc_layer_id)yyjson_get_uint(lv);
        have_layer = true;
    } else {
        return pc_engine_err_bad_params(err, CMD_ID, "`layer` must be a layer id");
    }

    pc_layer_id *with_fx = NULL;
    size_t nwith = 0;
    if (layers_with_fx(doc, &with_fx, &nwith, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }

    /* Whether the target(s) are currently expanded. */
    bool current;
    if (have_layer) {
        const pc_layer *l = pc_document_layer(doc, layer);
        if (l == NULL) {
            free(with_fx);
            return pc_engine_err_no_layer(err, layer);
        }
        if (l->effects.nitems == 0) {
            free(with_fx);
            return pc_engine_err_bad_params(err, CMD_ID, "layer %llu has no effects",
                                            (unsigned long long)layer);
        }
        current = !ids_contains(st->fx_collapsed, st->nfx_collapsed, layer);
    } else {
        current = false;
        for (size_t i = 0; i < nwith; i++) {
            if (!ids_contains(st->fx_collapsed, st->nfx_collapsed, with_fx[i])) {
                current = true;
                break;
            }
        }
    }
    bool expanded = have_want ? want : !current;

    /* Targets: every fx layer with `all`, else the single layer (or none). */
    pc_layer_id *targets;
    size_t ntargets;
    if (all) {
        targets = with_fx;
        ntargets = nwith;
    } else if (have_layer) {
        targets = &layer;
        ntargets = 1;
    } else {
        targets = NULL;
        ntargets = 0;
    }

    /* Forget ids no longer present or being retargeted, so the list can't grow unbounded. */
    size_t kept = 0;
    for (size_t i = 0; i < st->nfx_collapsed; i++) {
        pc_layer_id id = st->fx_collapsed[i];
        if (pc_document_layer(doc, id) != NULL && !ids_contains(targets, ntargets, id)) {
            st->fx_collapsed[kept++] = id;
        }
    }
    st->nfx_collapsed = kept;

    /* Collapse the targets when the new state is "not expanded". */
    if (!expanded && ntargets > 0) {
        size_t newcap = kept + ntargets;
        pc_layer_id *grown = realloc(st->fx_collapsed, newcap * sizeof *grown);
        if (grown == NULL) {
            free(with_fx); /* targets may alias with_fx (when `all`); we return either way */
            return pc_fail(err, PC_ERR_NOMEM, "fx_collapsed");
        }
        st->fx_collapsed = grown;
        for (size_t i = 0; i < ntargets; i++) {
            st->fx_collapsed[st->nfx_collapsed++] = targets[i];
        }
    }

    free(with_fx); /* targets aliased with_fx only when `all`; done with it now */

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_bool(rd, root, "expanded", expanded);
    yyjson_mut_obj_add_uint(rd, root, "layers", ntargets);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static const pc_engine_command SPECS[] = {
    {CMD_ID, "Expand/Collapse Effects", NULL, 0, NULL,
     "{\"layer\":id?,\"expanded\":bool?,\"all\":bool?} (no expanded: toggle; all: every layer with "
     "effects; view state, not an undo step)",
     has_effects, run_set_expanded, false},
};

const pc_engine_command *pc_engine_fx_view_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
