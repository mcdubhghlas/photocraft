/* layer.list: the layer tree alone, for agents and scripts (#5059; port of
 * crates/engine/src/layer_list_cmds.rs). document.inspect also carries it, buried among history,
 * channels and much more; an agent looking for the layers' ids looks for a layer.* command first.
 *
 * A pure query (no history step). Never crashes (REWRITE_PLAN.md rule 9): the `document` and
 * `flat` params are validated, and the recursion is depth-bounded (documents can be deep). */
#include <stddef.h>

#include "base/json.h"
#include "doc/doc.h"
#include "geom/geom.h"

#include "engine_internal.h"
#include "layer_cmds.h"       /* pc_engine_layer_bounds */
#include "layer_list_cmds.h"

#define CMD "layer.list"

/* Groups nest at most this deep in the listing; deeper layers are left out. */
enum { MAX_DEPTH = 256 };

static bool ids_contain(const pc_layer_id *v, size_t n, pc_layer_id id)
{
    for (size_t i = 0; i < n; i++) {
        if (v[i] == id) {
            return true;
        }
    }
    return false;
}

/* One layer's row: id, name, kind, visible, bounds, selected, active. */
static yyjson_mut_val *row(yyjson_mut_doc *d, const pc_layer *l, const pc_layer_id *selected,
                           size_t nselected, bool has_active, pc_layer_id active)
{
    yyjson_mut_val *v = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, v, "id", l->id);
    yyjson_mut_obj_add_strcpy(d, v, "name", pc_str_c(&l->name));
    yyjson_mut_obj_add_str(d, v, "kind", pc_layer_content_kind_name(&l->content));
    yyjson_mut_obj_add_bool(d, v, "visible", l->visible);
    pc_rect r;
    if (pc_engine_layer_bounds(l, &r)) {
        yyjson_mut_val *b = yyjson_mut_arr(d);
        yyjson_mut_arr_add_int(d, b, r.x0);
        yyjson_mut_arr_add_int(d, b, r.y0);
        yyjson_mut_arr_add_int(d, b, (int64_t)pc_rect_width(r));
        yyjson_mut_arr_add_int(d, b, (int64_t)pc_rect_height(r));
        yyjson_mut_obj_add_val(d, v, "bounds", b);
    } else {
        yyjson_mut_obj_add_null(d, v, "bounds");
    }
    yyjson_mut_obj_add_bool(d, v, "selected", ids_contain(selected, nselected, l->id));
    yyjson_mut_obj_add_bool(d, v, "active", has_active && active == l->id);
    return v;
}

/* The layers top-down (the Layers panel's order), groups with their "children". */
static void tree(yyjson_mut_doc *d, yyjson_mut_val *arr, const pc_layer *layers, size_t n,
                 const pc_layer_id *selected, size_t nselected, bool has_active, pc_layer_id active,
                 size_t depth)
{
    if (depth >= MAX_DEPTH) {
        return;
    }
    for (size_t i = n; i-- > 0;) { /* stored bottom-to-top; list top-down */
        const pc_layer *l = &layers[i];
        yyjson_mut_val *v = row(d, l, selected, nselected, has_active, active);
        if (l->content.kind == PC_LAYER_GROUP) {
            const pc_group *g = &l->content.u.group;
            yyjson_mut_val *children = yyjson_mut_arr(d);
            tree(d, children, g->children, g->nchildren, selected, nselected, has_active, active,
                 depth + 1);
            yyjson_mut_obj_add_val(d, v, "children", children);
        }
        yyjson_mut_arr_append(arr, v);
    }
}

/* The layers top-down as one list, each with its "depth" (0 = top level) and "parent" group id. */
static void flat(yyjson_mut_doc *d, yyjson_mut_val *arr, const pc_layer *layers, size_t n,
                 const pc_layer_id *selected, size_t nselected, bool has_active, pc_layer_id active,
                 bool has_parent, pc_layer_id parent, size_t depth)
{
    if (depth >= MAX_DEPTH) {
        return;
    }
    for (size_t i = n; i-- > 0;) {
        const pc_layer *l = &layers[i];
        yyjson_mut_val *v = row(d, l, selected, nselected, has_active, active);
        yyjson_mut_obj_add_uint(d, v, "depth", depth);
        if (has_parent) {
            yyjson_mut_obj_add_uint(d, v, "parent", parent);
        } else {
            yyjson_mut_obj_add_null(d, v, "parent");
        }
        yyjson_mut_arr_append(arr, v);
        if (l->content.kind == PC_LAYER_GROUP) {
            const pc_group *g = &l->content.u.group;
            flat(d, arr, g->children, g->nchildren, selected, nselected, has_active, active, true,
                 l->id, depth + 1);
        }
    }
}

static pc_status has_doc(const pc_engine_session *s, pc_error *reason)
{
    if (pc_engine_session_active(s) == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    return PC_OK;
}

static pc_status run_list(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = NULL;
    yyjson_val *jd = p != NULL ? pc_json_get(p, "document") : NULL;
    if (jd == NULL || yyjson_is_null(jd)) {
        d = pc_engine_session_active(s);
        if (d == NULL) {
            return pc_engine_err_no_document(err);
        }
    } else {
        uint64_t i = 0;
        if (!pc_json_as_u64(jd, &i)) {
            return pc_engine_err_bad_params(err, CMD, "`document` must be an index");
        }
        size_t ndoc = 0;
        const pc_engine_doc_state *docs = pc_engine_session_documents(s, &ndoc);
        if (i >= ndoc) {
            return pc_engine_err_other(err, "no document at index %llu", (unsigned long long)i);
        }
        d = &docs[i];
    }
    bool as_flat = false;
    yyjson_val *jf = p != NULL ? pc_json_get(p, "flat") : NULL;
    if (jf != NULL && !yyjson_is_null(jf)) {
        if (!yyjson_is_bool(jf)) {
            return pc_engine_err_bad_params(err, CMD, "`flat` must be true or false");
        }
        as_flat = yyjson_get_bool(jf);
    }

    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (doc == NULL) {
        return pc_engine_err_no_document(err);
    }

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *layers = yyjson_mut_arr(rd);
    if (as_flat) {
        flat(rd, layers, doc->layers, doc->nlayers, d->selected_layers, d->nselected,
             d->has_active_layer, d->active_layer, false, 0, 0);
    } else {
        tree(rd, layers, doc->layers, doc->nlayers, d->selected_layers, d->nselected,
             d->has_active_layer, d->active_layer, 0);
    }
    yyjson_mut_obj_add_val(rd, root, "layers", layers);
    if (d->has_active_layer) {
        yyjson_mut_obj_add_uint(rd, root, "activeLayer", d->active_layer);
    } else {
        yyjson_mut_obj_add_null(rd, root, "activeLayer");
    }
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

#define LIST_PARAMS                                                                                \
    "{\"document\":index?,\"flat\":bool=false} -> {\"layers\":[{id,name,kind,visible,"             \
    "bounds:[x,y,w,h]|null,selected,active,children?}],\"activeLayer\":id?} (top-down; groups "    \
    "list their children, or with flat every layer is one row with its depth and parent group id)"

static const pc_engine_command SPECS[] = {
    {CMD, "List Layers", NULL, 0, NULL, LIST_PARAMS, has_doc, run_list, false},
};

const pc_engine_command *pc_engine_layer_list_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
