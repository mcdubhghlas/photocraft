/* Keyboard layer navigation (port of crates/engine/src/layer_nav_cmds.rs, #352): ⌥] / ⌥[ select
 * the layer above / below, ⌥. / ⌥, the top / bottom layer, ⇧⌥] / ⇧⌥[ add the layer above / below
 * to the selection. They follow the Layers panel's rows, top to bottom: a group's row comes before
 * its children, and the children of a collapsed group are skipped. */
#include <stdlib.h>

#include "layer_cmds.h"

typedef enum { STEP_ABOVE, STEP_BELOW, STEP_TOP, STEP_BOTTOM } nav_step;

static pc_status rows_rec(const pc_layer *layers, size_t n, pc_layer_id **buf, size_t *len, size_t *cap,
                         pc_error *err)
{
    for (size_t i = n; i-- > 0;) { /* top to bottom */
        if (*len == *cap) {
            size_t nc = *cap == 0 ? 16 : *cap * 2;
            pc_layer_id *g = realloc(*buf, nc * sizeof **buf);
            if (g == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "panel rows");
            }
            *buf = g;
            *cap = nc;
        }
        (*buf)[(*len)++] = layers[i].id;
        if (layers[i].content.kind == PC_LAYER_GROUP && layers[i].content.u.group.expanded) {
            pc_status rc = rows_rec(layers[i].content.u.group.children,
                                    layers[i].content.u.group.nchildren, buf, len, cap, err);
            if (rc != PC_OK) {
                return rc;
            }
        }
    }
    return PC_OK;
}

pc_status pc_engine_panel_rows(const pc_document *doc, pc_layer_id **out, size_t *n, pc_error *err)
{
    pc_layer_id *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    pc_status rc = rows_rec(doc->layers, doc->nlayers, &buf, &len, &cap, err);
    if (rc != PC_OK) {
        free(buf);
        return rc;
    }
    *out = buf;
    *n = len;
    return PC_OK;
}

static bool rows_find(const pc_layer_id *rows, size_t n, pc_layer_id id, size_t *pos)
{
    for (size_t i = 0; i < n; i++) {
        if (rows[i] == id) {
            *pos = i;
            return true;
        }
    }
    return false;
}

/* The row of `id`: a layer inside a collapsed group counts as that group's row (the nearest
 * enclosing one that has a row). */
static bool row_of(const pc_document *doc, const pc_layer_id *rows, size_t nrows, pc_layer_id id,
                   size_t *pos)
{
    pc_layer_path path;
    if (pc_document_path_of(doc, id, &path, NULL) != PC_OK) {
        return false;
    }
    bool found = false;
    for (size_t n = path.len; n >= 1; n--) {
        const pc_layer *l = pc_document_layer_at(doc, path.idx, n);
        if (l != NULL && rows_find(rows, nrows, l->id, pos)) {
            found = true;
            break;
        }
    }
    pc_layer_path_fini(&path);
    return found;
}

static pc_status has_layers(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    if (pc_doc_arc_get(d->doc)->nlayers == 0) {
        return pc_fail(reason, PC_ERR_INVALID, "the document has no layers");
    }
    return PC_OK;
}

static pc_status navigate(pc_engine_session *s, nav_step step, bool add, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_layer_id *rows = NULL;
    size_t nrows = 0;
    if (pc_engine_panel_rows(doc, &rows, &nrows, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (nrows == 0) {
        free(rows);
        return pc_engine_err_other(err, "the document has no layers");
    }
    size_t last = nrows - 1;
    bool have_current = false;
    size_t current = 0;
    if (d->has_active_layer) {
        have_current = row_of(doc, rows, nrows, d->active_layer, &current);
    }
    size_t target;
    switch (step) {
    case STEP_TOP:
        target = 0;
        break;
    case STEP_BOTTOM:
        target = last;
        break;
    case STEP_ABOVE:
        target = !have_current ? 0 : (current == 0 ? 0 : current - 1);
        break;
    case STEP_BELOW:
    default:
        target = !have_current ? last : (current + 1 > last ? last : current + 1);
        break;
    }
    pc_layer_id id = rows[target];
    free(rows);

    if (add) {
        pc_layer_id *sel = NULL;
        size_t nsel = 0;
        if (pc_engine_doc_state_selected_layers(d, &sel, &nsel, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        bool contained = false;
        for (size_t i = 0; i < nsel; i++) {
            if (sel[i] == id) {
                contained = true;
                break;
            }
        }
        pc_layer_id *ns = malloc((nsel + 1) * sizeof *ns);
        if (ns == NULL) {
            free(sel);
            return pc_fail(err, PC_ERR_NOMEM, "navigate");
        }
        for (size_t i = 0; i < nsel; i++) {
            ns[i] = sel[i];
        }
        size_t m = nsel;
        if (!contained) {
            ns[m++] = id;
        }
        free(sel);
        bool has_anchor = d->has_anchor;
        pc_layer_id anchor = d->layer_anchor;
        pc_status rc = pc_engine_set_selection(s, ns, m, true, id, has_anchor, anchor, err);
        free(ns);
        if (rc != PC_OK) {
            return rc;
        }
    } else {
        if (pc_engine_session_select_layer(s, id, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
    }

    pc_layer_id *sel = NULL;
    size_t nsel = 0;
    (void)pc_engine_selected(s, &sel, &nsel, NULL);
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        free(sel);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", id);
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    for (size_t i = 0; i < nsel; i++) {
        yyjson_mut_arr_add_uint(rd, arr, sel[i]);
    }
    yyjson_mut_obj_add_val(rd, root, "selected", arr);
    yyjson_mut_doc_set_root(rd, root);
    free(sel);
    return PC_OK;
}

static pc_status run_above(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return navigate(s, STEP_ABOVE, false, out, err);
}
static pc_status run_below(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return navigate(s, STEP_BELOW, false, out, err);
}
static pc_status run_top(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return navigate(s, STEP_TOP, false, out, err);
}
static pc_status run_bottom(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return navigate(s, STEP_BOTTOM, false, out, err);
}
static pc_status run_add_above(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return navigate(s, STEP_ABOVE, true, out, err);
}
static pc_status run_add_below(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return navigate(s, STEP_BELOW, true, out, err);
}

#define NAV_PARAMS "{} (follows the Layers panel's rows; collapsed groups are one row)"

static const pc_engine_command SPECS[] = {
    {"layer.selectAbove", "Select Layer Above", NULL, 0, "Alt+]", NAV_PARAMS, has_layers, run_above, true},
    {"layer.selectBelow", "Select Layer Below", NULL, 0, "Alt+[", NAV_PARAMS, has_layers, run_below, true},
    {"layer.selectTop", "Select Top Layer", NULL, 0, "Alt+.", NAV_PARAMS, has_layers, run_top, true},
    {"layer.selectBottom", "Select Bottom Layer", NULL, 0, "Alt+,", NAV_PARAMS, has_layers, run_bottom, true},
    {"layer.addAboveToSelection", "Add Layer Above to Selection", NULL, 0, "Alt+Shift+]", NAV_PARAMS,
     has_layers, run_add_above, true},
    {"layer.addBelowToSelection", "Add Layer Below to Selection", NULL, 0, "Alt+Shift+[", NAV_PARAMS,
     has_layers, run_add_below, true},
};

const pc_engine_command *pc_engine_layer_nav_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
