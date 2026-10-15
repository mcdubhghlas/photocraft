/* Layers panel disclosure state: expanding and collapsing layer groups (port of
 * crates/engine/src/group_view_cmds.rs, #126).
 *
 * A group's open/closed state is document data (pc_group::expanded, saved in .pcraft), but
 * toggling it is a view change: no history step, and a clean document stays clean. */
#include <stdlib.h>

#include "layer_cmds.h"

#define CMD_ID "layer.setExpanded"

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

/* Recursively set every group's expanded flag; returns how many groups were touched. */
static size_t set_all(pc_layer *layers, size_t n, bool expanded)
{
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        if (layers[i].content.kind == PC_LAYER_GROUP) {
            pc_group *g = &layers[i].content.u.group;
            g->expanded = expanded;
            count += 1 + set_all(g->children, g->nchildren, expanded);
        }
    }
    return count;
}

static bool any_group_expanded(const pc_layer *layers, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (layers[i].content.kind == PC_LAYER_GROUP) {
            if (layers[i].content.u.group.expanded) {
                return true;
            }
            if (any_group_expanded(layers[i].content.u.group.children, layers[i].content.u.group.nchildren)) {
                return true;
            }
        }
    }
    return false;
}

static pc_status has_group(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_walk w;
    if (pc_document_walk(doc, &w, NULL) != PC_OK) {
        return pc_fail(reason, PC_ERR_INVALID, "the document has no layer groups");
    }
    bool found = false;
    for (size_t i = 0; i < w.len; i++) {
        if (pc_layer_is_group(w.items[i].layer)) {
            found = true;
            break;
        }
    }
    pc_walk_fini(&w);
    if (!found) {
        return pc_fail(reason, PC_ERR_INVALID, "the document has no layer groups");
    }
    return PC_OK;
}

static pc_status run_set_expanded(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    bool have_want = false;
    bool want = false;
    if (opt_bool(p, "expanded", &have_want, &want, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    bool have_all = false;
    bool all = false;
    if (opt_bool(p, "all", &have_all, &all, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (!have_all) {
        all = false;
    }

    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);

    bool present = false;
    pc_layer_id layer = 0;
    if (pc_engine_explicit_layer(s, p, &present, &layer, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    bool have_layer = present;
    if (!present && !all) {
        if (!d->has_active_layer) {
            return pc_engine_err_bad_params(err, CMD_ID, "no layer given and no active layer");
        }
        layer = d->active_layer;
        have_layer = true;
    }

    bool current;
    if (have_layer) {
        const pc_layer *l = pc_document_layer(doc, layer);
        if (l == NULL) {
            return pc_engine_err_no_layer(err, layer);
        }
        if (l->content.kind != PC_LAYER_GROUP) {
            return pc_engine_err_bad_params(err, CMD_ID, "layer %llu is not a group",
                                            (unsigned long long)layer);
        }
        current = l->content.u.group.expanded;
    } else {
        current = any_group_expanded(doc->layers, doc->nlayers);
    }
    bool expanded = have_want ? want : !current;

    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_document *mut_doc = NULL;
    if (pc_doc_arc_make_mut(&st->doc, &mut_doc, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    size_t changed;
    if (all) {
        changed = set_all(mut_doc->layers, mut_doc->nlayers, expanded);
    } else {
        pc_layer *l = pc_document_layer_mut(mut_doc, layer);
        if (l == NULL || l->content.kind != PC_LAYER_GROUP) {
            return pc_engine_err_no_layer(err, layer);
        }
        l->content.u.group.expanded = expanded;
        changed = 1;
    }

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_bool(rd, root, "expanded", expanded);
    yyjson_mut_obj_add_uint(rd, root, "groups", changed);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static const pc_engine_command SPECS[] = {
    {CMD_ID, "Expand/Collapse Group", NULL, 0, NULL,
     "{\"layer\":id?,\"expanded\":bool?,\"all\":bool?} (no expanded: toggle; all: every group; not an "
     "undo step)",
     has_group, run_set_expanded, false},
};

const pc_engine_command *pc_engine_group_view_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
