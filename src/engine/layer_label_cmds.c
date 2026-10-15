/* Undoable layer colour labels (port of crates/engine/src/layer_label_cmds.rs). A group applies
 * the label to its existing descendants, without changing pixels, visibility, locks or the layer
 * selection. */
#include <stdlib.h>
#include <string.h>

#include "layer_cmds.h"

#define CMD_ID "layer.setLabelColor"

typedef struct {
    pc_layer_id *ids;
    size_t n;
    pc_label_color color;
} label_ctx;

static pc_status edit_label(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    label_ctx *ctx = vctx;
    for (size_t i = 0; i < ctx->n; i++) {
        pc_layer *l = pc_document_layer_mut(doc, ctx->ids[i]);
        if (l == NULL) {
            return pc_engine_err_no_layer(err, ctx->ids[i]);
        }
        l->label = ctx->color;
    }
    return PC_OK;
}

static pc_status enabled(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    pc_layer_id *sel = NULL;
    size_t n = 0;
    (void)pc_engine_doc_state_selected_layers(d, &sel, &n, NULL);
    free(sel);
    if (n == 0) {
        return pc_fail(reason, PC_ERR_INVALID, "no layer selected");
    }
    return PC_OK;
}

/* Is root_path (rn entries) a prefix of path (pn entries)? */
static bool path_prefix(const size_t *root, size_t rn, const size_t *path, size_t pn)
{
    if (rn > pn) {
        return false;
    }
    for (size_t i = 0; i < rn; i++) {
        if (root[i] != path[i]) {
            return false;
        }
    }
    return true;
}

static pc_status run_set_label(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    if (p == NULL || !yyjson_is_obj(p)) {
        return pc_engine_err_bad_params(err, CMD_ID, "expected an object");
    }
    size_t clen = 0;
    const char *cstr = pc_json_as_str(pc_json_get(p, "color"), &clen);
    pc_label_color color;
    if (cstr == NULL || !pc_label_color_from_id(cstr, clen, &color)) {
        return pc_engine_err_bad_params(
            err, CMD_ID,
            "`color` must be a layer colour id (none, red, orange, yellow, green, seafoam, blue, "
            "indigo, magenta, fuchsia, violet, gray)");
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);

    /* roots: the explicit `layer` (id only) or every selected layer. */
    pc_layer_id *roots = NULL;
    size_t nroots = 0;
    yyjson_val *lv = pc_json_get(p, "layer");
    if (lv != NULL && !yyjson_is_null(lv)) {
        uint64_t id;
        if (!pc_json_as_u64(lv, &id)) {
            return pc_engine_err_bad_params(err, CMD_ID, "`layer` must be an unsigned integer");
        }
        roots = malloc(sizeof *roots);
        if (roots == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "set label");
        }
        roots[0] = id;
        nroots = 1;
    } else {
        if (pc_engine_doc_state_selected_layers(d, &roots, &nroots, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
    }
    if (nroots == 0) {
        free(roots);
        return pc_engine_err_bad_params(err, CMD_ID, "no layer selected");
    }

    pc_walk w;
    if (pc_document_walk(doc, &w, err) != PC_OK) {
        free(roots);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    /* Root paths (each root must exist). */
    pc_layer_path *root_paths = calloc(nroots, sizeof *root_paths);
    if (root_paths == NULL) {
        pc_walk_fini(&w);
        free(roots);
        return pc_fail(err, PC_ERR_NOMEM, "set label");
    }
    for (size_t i = 0; i < nroots; i++) {
        if (pc_document_path_of(doc, roots[i], &root_paths[i], err) != PC_OK) {
            pc_status rc = pc_engine_err_no_layer(err, roots[i]);
            for (size_t j = 0; j < i; j++) {
                pc_layer_path_fini(&root_paths[j]);
            }
            free(root_paths);
            pc_walk_fini(&w);
            free(roots);
            return rc;
        }
    }
    /* Targets: every layer in a root's subtree (a single walk also deduplicates). */
    pc_layer_id *targets = w.len > 0 ? malloc(w.len * sizeof *targets) : NULL;
    if (w.len > 0 && targets == NULL) {
        for (size_t j = 0; j < nroots; j++) {
            pc_layer_path_fini(&root_paths[j]);
        }
        free(root_paths);
        pc_walk_fini(&w);
        free(roots);
        return pc_fail(err, PC_ERR_NOMEM, "set label");
    }
    size_t ntargets = 0;
    bool changed = false;
    for (size_t i = 0; i < w.len; i++) {
        const size_t *path = &w.paths[w.items[i].path_start];
        size_t plen = w.items[i].depth + 1;
        bool hit = false;
        for (size_t r = 0; r < nroots; r++) {
            if (path_prefix(root_paths[r].idx, root_paths[r].len, path, plen)) {
                hit = true;
                break;
            }
        }
        if (hit) {
            if (w.items[i].layer->label != color) {
                changed = true;
            }
            targets[ntargets++] = w.items[i].layer->id;
        }
    }
    for (size_t j = 0; j < nroots; j++) {
        pc_layer_path_fini(&root_paths[j]);
    }
    free(root_paths);
    pc_walk_fini(&w);
    free(roots);

    if (changed) {
        label_ctx ctx = {targets, ntargets, color};
        if (pc_engine_session_edit(s, "Layer Color", edit_label, &ctx, err) != PC_OK) {
            free(targets);
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        /* Saved document metadata; no canvas pixels need recompositing. */
        pc_engine_doc_state *st = pc_engine_session_active_mut(s);
        if (st != NULL) {
            st->has_last_damage = true;
            st->last_damage = PC_RECT_EMPTY;
        }
    }

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        free(targets);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_str(rd, root, "color", pc_label_color_id(color));
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    for (size_t i = 0; i < ntargets; i++) {
        yyjson_mut_arr_add_uint(rd, arr, targets[i]);
    }
    yyjson_mut_obj_add_val(rd, root, "layers", arr);
    yyjson_mut_obj_add_bool(rd, root, "changed", changed);
    yyjson_mut_doc_set_root(rd, root);
    free(targets);
    return PC_OK;
}

static const pc_engine_command SPECS[] = {
    {CMD_ID, "Layer Color", NULL, 0, NULL,
     "{\"color\":\"none|red|orange|yellow|green|seafoam|blue|indigo|magenta|fuchsia|violet|gray\","
     "\"layer\":id?} (no layer: selected layers; groups include descendants)",
     enabled, run_set_label, true},
};

const pc_engine_command *pc_engine_layer_label_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
