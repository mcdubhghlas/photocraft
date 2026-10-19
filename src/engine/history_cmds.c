/* History panel state actions and named COW snapshots (reference history_cmds.rs). */
#include "engine/engine_internal.h"
#include "compose/compose.h"
#include "base/mem.h"
#include "base/utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct pc_engine_snapshot {
    uint64_t id;
    pc_str name;
    pc_doc_arc *doc;
    pc_layer_target layers;
} pc_engine_snapshot;
static void snapshot_fini(pc_engine_snapshot *snapshot)
{
    pc_str_fini(&snapshot->name);
    pc_doc_arc_release(snapshot->doc);
    pc_layer_target_fini(&snapshot->layers);
    *snapshot = (pc_engine_snapshot){0};
}
void pc_engine_snapshots_fini(pc_engine_doc_state *st)
{
    for (size_t i = 0; i < st->nsnapshots; i++) {
        snapshot_fini(&st->snapshots[i]);
    }
    free(st->snapshots);
    st->snapshots = NULL;
    st->nsnapshots = st->cap_snapshots = 0;
}
void pc_engine_inspect_snapshots(yyjson_mut_doc *d, yyjson_mut_val *root, const pc_engine_doc_state *st)
{
    yyjson_mut_val *items = yyjson_mut_arr(d);
    for (size_t i = 0; i < st->nsnapshots; i++) {
        const pc_engine_snapshot *snap = &st->snapshots[i];
        yyjson_mut_val *row = yyjson_mut_obj(d);
        yyjson_mut_obj_add_uint(d, row, "id", snap->id);
        yyjson_mut_obj_add_strcpy(d, row, "name", pc_str_c(&snap->name));
        yyjson_mut_arr_add_val(items, row);
    }
    yyjson_mut_obj_add_val(d, root, "snapshots", items);
}
static pc_status result_snapshot(pc_json_value *out, const pc_engine_snapshot *snap, pc_error *err)
{
    yyjson_mut_doc *d;
    pc_status rc = pc_engine_result_doc(out, &d, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "snapshot", snap->id);
    yyjson_mut_obj_add_strcpy(d, root, "name", pc_str_c(&snap->name));
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}
static pc_status index_of(pc_engine_session *s, yyjson_val *p, const char *cmd, size_t *out, pc_error *err)
{
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (!st) {
        return pc_engine_err_no_document(err);
    }
    uint64_t id;
    if (!pc_json_as_u64(pc_json_get(p, "snapshot"), &id)) {
        return pc_engine_err_bad_params(err, cmd, "`snapshot` must be a snapshot id");
    }
    for (size_t i = 0; i < st->nsnapshots; i++) {
        if (st->snapshots[i].id == id) {
            *out = i;
            return PC_OK;
        }
    }
    return pc_engine_err_bad_params(err, cmd, "no snapshot %llu", (unsigned long long)id);
}
static pc_status name_param(yyjson_val *p, const char *cmd, pc_str *out, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, "name");
    if (!v || yyjson_is_null(v)) {
        return PC_OK;
    }
    size_t len;
    const char *name = pc_json_as_str(v, &len);
    if (!name) {
        return pc_engine_err_bad_params(err, cmd, "`name` must be a non-empty string");
    }
    size_t start = 0;
    len = pc_utf8_trim(name, len, &start);
    name += start;
    if (!len) {
        return pc_engine_err_bad_params(err, cmd, "`name` must be a non-empty string");
    }
    return pc_str_set(out, name, len, err);
}
static pc_status state_index(pc_engine_session *s, yyjson_val *p, const char *cmd, size_t *index,
                             pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (!st) {
        return pc_engine_err_no_document(err);
    }
    size_t current = pc_history_past_len(st->history);
    uint64_t value = current;
    yyjson_val *v = pc_json_get(p, "state");
    if (v && !yyjson_is_null(v) && (!pc_json_as_u64(v, &value) || value > current)) {
        return pc_engine_err_bad_params(err, cmd, "`state` is not a held history state");
    }
    *index = (size_t)value;
    return PC_OK;
}
pc_status pc_engine_inspect_history(const pc_engine_doc_state *st, pc_json_value *out, pc_error *err)
{
    if (!st || !out) {
        return pc_fail(err, PC_ERR_INVALID, "missing history");
    }
    yyjson_mut_doc *d;
    pc_status rc = pc_engine_result_doc(out, &d, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d), *states = yyjson_mut_arr(d), *redo_states = yyjson_mut_arr(d);
    for (size_t i = 0; i < pc_history_entry_count(st->history); i++) {
        const pc_str *label = pc_history_entry(st->history, i);
        yyjson_mut_arr_add_strcpy(d, states, label ? pc_str_c(label) : "");
    }
    for (size_t i = 0; i < pc_history_redo_count(st->history); i++) {
        const pc_str *label = pc_history_redo_label_at(st->history, i);
        yyjson_mut_arr_add_strcpy(d, redo_states, label ? pc_str_c(label) : "");
    }
    yyjson_mut_obj_add_val(d, root, "states", states);
    yyjson_mut_obj_add_val(d, root, "redo", redo_states);
    yyjson_mut_obj_add_uint(d, root, "current", pc_history_past_len(st->history));
    pc_engine_inspect_snapshots(d, root, st);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}
static pc_status select_state(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (!st) {
        return pc_engine_err_no_document(err);
    }
    uint64_t target;
    size_t current = pc_history_past_len(st->history), future = pc_history_redo_count(st->history);
    if (!pc_json_as_u64(pc_json_get(p, "state"), &target) || target > current + future) {
        return pc_engine_err_bad_params(err, "history.selectState", "`state` is not a held history state");
    }
    if (pc_engine_active_job(s)) {
        return pc_engine_err_other(err, "the document is busy with a background job");
    }
    while (current != target) {
        bool ok = target < current ? pc_engine_session_undo(s) : pc_engine_session_redo(s);
        if (!ok) {
            return pc_engine_err_other(err, "the history can't step right now");
        }
        current = pc_history_past_len(st->history);
    }
    return pc_engine_inspect_history(st, out, err);
}
static pc_status delete_state(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    size_t i = 0;
    pc_status rc = state_index(s, p, "history.deleteState", &i, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (!i) {
        return pc_engine_err_bad_params(err, "history.deleteState", "the oldest state can't be deleted");
    }
    size_t current = pc_history_past_len(pc_engine_session_active(s)->history);
    for (size_t step = i; step <= current; step++) {
        if (!pc_engine_session_undo(s)) {
            return pc_engine_err_other(err, "the history can't step back right now");
        }
    }
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    pc_history_clear_redo(st->history);
    yyjson_mut_doc *d;
    rc = pc_engine_result_doc(out, &d, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    size_t count = pc_history_entry_count(st->history);
    const pc_str *label = count ? pc_history_entry(st->history, count - 1) : NULL;
    yyjson_mut_obj_add_uint(d, root, "states", count);
    yyjson_mut_obj_add_strcpy(d, root, "current", label ? pc_str_c(label) : "");
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}
static pc_status new_document(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    size_t index = 0;
    pc_status rc = state_index(s, p, "history.newDocument", &index, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    pc_doc_arc *held = index == pc_history_past_len(st->history) ? pc_doc_arc_retain(st->doc)
                                                                 : pc_history_state(st->history, index);
    if (!held) {
        return pc_engine_err_bad_params(err, "history.newDocument", "state is not held");
    }
    pc_document doc = {0};
    rc = pc_document_clone(pc_doc_arc_get(held), &doc, err);
    pc_doc_arc_release(held);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_str *label = pc_history_entry(st->history, index);
    size_t len;
    const char *name = pc_json_as_str(pc_json_get(p, "name"), &len);
    size_t trimmed_start = 0;
    if (!name || !pc_utf8_trim(name, len, &trimmed_start)) {
        name = label ? pc_str_c(label) : pc_str_c(&doc.name);
        len = strlen(name);
    }
    rc = pc_str_set(&doc.name, name, len, err);
    doc.id = pc_doc_id_fresh();
    if (rc == PC_OK) {
        rc = pc_engine_reidentify_copied_layers(&doc, err);
    }
    pc_str saved = PC_STR_INIT;
    if (rc == PC_OK) {
        rc = pc_str_set(&saved, pc_str_c(&doc.name), doc.name.len, err);
    }
    size_t added = 0;
    if (rc == PC_OK) {
        rc = pc_engine_session_add_document(s, &doc, NULL, &added, err);
    }
    pc_document_fini(&doc);
    if (rc == PC_OK) {
        yyjson_mut_doc *d;
        rc = pc_engine_result_doc(out, &d, err);
        if (rc == PC_OK) {
            yyjson_mut_val *root = yyjson_mut_obj(d);
            yyjson_mut_obj_add_uint(d, root, "document", added);
            yyjson_mut_obj_add_strcpy(d, root, "name", pc_str_c(&saved));
            yyjson_mut_doc_set_root(d, root);
        }
    }
    pc_str_fini(&saved);
    return rc;
}
static pc_status new_snapshot(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "history.newSnapshot";
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (!st) {
        return pc_engine_err_no_document(err);
    }
    if (st->nsnapshots >= 10000 || st->next_snapshot == UINT64_MAX) {
        return pc_fail(err, PC_ERR_LIMIT, "snapshot limit");
    }
    pc_engine_snapshot snap = {.id = st->next_snapshot};
    pc_status rc = name_param(p, cmd, &snap.name, err);
    if (rc != PC_OK) {
        return rc;
    }
    const char *from = pc_json_as_str(pc_json_get(p, "from"), NULL);
    yyjson_val *fv = pc_json_get(p, "from");
    if (fv && !yyjson_is_null(fv) &&
        (!from ||
         (strcmp(from, "fullDocument") && strcmp(from, "currentLayer") && strcmp(from, "mergedLayers")))) {
        snapshot_fini(&snap);
        return pc_engine_err_bad_params(err, cmd, "unknown `from` snapshot source");
    }
    if (!snap.name.len) {
        char name[64];
        (void)snprintf(name, sizeof name, "Snapshot %llu", (unsigned long long)snap.id);
        rc = pc_str_set(&snap.name, name, strlen(name), err);
    }
    const pc_document *source = pc_doc_arc_get(st->doc);
    if (rc == PC_OK && from && strcmp(from, "fullDocument")) {
        pc_layer layer = {0};
        pc_document doc = {0};
        if (strcmp(from, "currentLayer") == 0) {
            const pc_layer *active =
                st->has_active_layer ? pc_document_layer(source, st->active_layer) : NULL;
            rc = active ? pc_layer_clone(active, &layer, err)
                        : pc_engine_err_bad_params(err, cmd, "no active layer");
        } else {
            rc = pc_layer_raster(&layer, "Merged Layers", pc_document_pixel_format(source), err);
            if (rc == PC_OK) {
                pc_surface rendered = {0};
                rc = pc_compose_flatten_to_surface(source, pc_document_pixel_format(source), NULL, &rendered,
                                                   err);
                if (rc == PC_OK) {
                    pc_surface_fini(&layer.content.u.raster);
                    layer.content.u.raster = rendered;
                }
            }
        }
        if (rc == PC_OK) {
            rc = pc_document_clone(source, &doc, err);
        }
        if (rc == PC_OK) {
            for (size_t i = 0; i < doc.nlayers; i++) {
                pc_layer_fini(&doc.layers[i]);
            }
            free(doc.layers);
            doc.layers = calloc(1, sizeof *doc.layers);
            doc.nlayers = 0;
            if (!doc.layers) {
                rc = pc_fail(err, PC_ERR_NOMEM, "snapshot layer");
            } else {
                doc.layers[0] = layer;
                layer = (pc_layer){0};
                doc.nlayers = 1;
            }
        }
        if (rc == PC_OK) {
            snap.layers.has_active = true;
            snap.layers.active = doc.layers[0].id;
            snap.layers.selected = pc_malloc_array(1, sizeof *snap.layers.selected);
            if (!snap.layers.selected) {
                rc = pc_fail(err, PC_ERR_NOMEM, "snapshot selection");
            } else {
                snap.layers.selected[0] = snap.layers.active;
                snap.layers.nselected = 1;
            }
        }
        if (rc == PC_OK) {
            rc = pc_doc_arc_new(&doc, &snap.doc, err);
        }
        pc_layer_fini(&layer);
        pc_document_fini(&doc);
    } else if (rc == PC_OK) {
        snap.doc = pc_doc_arc_retain(st->doc);
        snap.layers.has_active = st->has_active_layer;
        snap.layers.active = st->active_layer;
        rc = pc_engine_doc_state_selected_layers(st, &snap.layers.selected, &snap.layers.nselected, err);
    }
    pc_engine_snapshot *grown =
        rc == PC_OK ? pc_grow_array(st->snapshots, &st->cap_snapshots, st->nsnapshots + 1, sizeof *grown)
                    : NULL;
    if (rc == PC_OK && !grown) {
        rc = pc_fail(err, PC_ERR_NOMEM, "snapshots");
    }
    if (rc == PC_OK) {
        rc = result_snapshot(out, &snap, err);
    }
    if (rc == PC_OK) {
        st->snapshots = grown;
        st->snapshots[st->nsnapshots++] = snap;
        st->next_snapshot++;
    } else {
        if (grown) {
            st->snapshots = grown;
        }
        snapshot_fini(&snap);
    }
    return rc;
}
static pc_status restore(pc_document *doc, bool *has_active, pc_layer_id *active, void *context,
                         pc_error *err)
{
    const pc_engine_snapshot *snap = context;
    pc_document copy = {0};
    pc_status rc = pc_document_clone(pc_doc_arc_get(snap->doc), &copy, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_str_fini(&copy.name);
    copy.name = doc->name;
    doc->name = (pc_str)PC_STR_INIT;
    copy.id = doc->id;
    pc_document_fini(doc);
    *doc = copy;
    *has_active = snap->layers.has_active && pc_document_layer(doc, snap->layers.active);
    if (*has_active) {
        *active = snap->layers.active;
    } else {
        *has_active = pc_document_top_layer(doc, active);
    }
    return PC_OK;
}
static pc_status select_snapshot(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    size_t index = 0;
    pc_status rc = index_of(s, p, "history.selectSnapshot", &index, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (pc_engine_active_job(s)) {
        return pc_engine_err_other(err, "the document is busy with a background job");
    }
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    pc_engine_snapshot *snap = &st->snapshots[index];
    pc_layer_id *selected = NULL;
    if (snap->layers.nselected) {
        selected = pc_malloc_array(snap->layers.nselected, sizeof *selected);
        if (!selected) {
            return pc_fail(err, PC_ERR_NOMEM, "snapshot selection");
        }
        memcpy(selected, snap->layers.selected, snap->layers.nselected * sizeof *selected);
    }
    rc = pc_engine_session_edit(s, pc_str_c(&snap->name), restore, snap, err);
    if (rc == PC_OK && st->has_active_layer == snap->layers.has_active &&
        (!st->has_active_layer || st->active_layer == snap->layers.active)) {
        free(st->selected_layers);
        st->selected_layers = selected;
        selected = NULL;
        st->nselected = snap->layers.nselected;
        pc_engine_fix_selection(st);
    }
    free(selected);
    if (rc == PC_OK) {
        rc = result_snapshot(out, snap, err);
    }
    return rc;
}
static pc_status rename_snapshot(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    size_t index = 0;
    pc_status rc = index_of(s, p, "history.renameSnapshot", &index, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_str name = PC_STR_INIT;
    rc = name_param(p, "history.renameSnapshot", &name, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (!name.len) {
        return pc_engine_err_bad_params(err, "history.renameSnapshot", "`name` is required");
    }
    pc_engine_snapshot *snap = &pc_engine_session_active_mut(s)->snapshots[index];
    pc_str_fini(&snap->name);
    snap->name = name;
    return result_snapshot(out, snap, err);
}
static pc_status delete_snapshot(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    size_t index = 0;
    pc_status rc = index_of(s, p, "history.deleteSnapshot", &index, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    uint64_t id = st->snapshots[index].id;
    snapshot_fini(&st->snapshots[index]);
    memmove(st->snapshots + index, st->snapshots + index + 1,
            (st->nsnapshots - index - 1) * sizeof *st->snapshots);
    st->nsnapshots--;
    yyjson_mut_doc *d;
    rc = pc_engine_result_doc(out, &d, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "snapshot", id);
    yyjson_mut_obj_add_uint(d, root, "snapshots", st->nsnapshots);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}
static pc_status can_undo(const pc_engine_session *s, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    return st && pc_history_can_undo(st->history) ? PC_OK : pc_fail(err, PC_ERR_INVALID, "nothing to undo");
}
static pc_status can_redo(const pc_engine_session *s, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    return st && pc_history_can_redo(st->history) ? PC_OK : pc_fail(err, PC_ERR_INVALID, "nothing to redo");
}
static pc_status boolean_result(pc_json_value *out, bool value, pc_error *err)
{
    yyjson_mut_doc *d;
    pc_status rc = pc_engine_result_doc(out, &d, err);
    if (rc == PC_OK) {
        yyjson_mut_doc_set_root(d, yyjson_mut_bool(d, value));
    }
    return rc;
}
static pc_status undo(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return boolean_result(out, pc_engine_session_undo(s), err);
}
static pc_status redo(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return boolean_result(out, pc_engine_session_redo(s), err);
}
static const pc_engine_command specs[] = {
    {"history.undo", "Undo", NULL, 0, NULL, "{}", can_undo, undo, false},
    {"history.redo", "Redo", NULL, 0, NULL, "{}", can_redo, redo, false},
    {"history.selectState", "Select History State", NULL, 0, NULL, "{\"state\":index}", pc_engine_has_doc,
     select_state, false},
    {"history.deleteState", "Delete State", NULL, 0, NULL, "{\"state\":index?}", can_undo, delete_state,
     true},
    {"history.newDocument", "New Document from State", NULL, 0, NULL, "{\"state\":index?,\"name\":str?}",
     pc_engine_has_doc, new_document, true},
    {"history.newSnapshot", "New Snapshot", NULL, 0, NULL,
     "{\"name\":str?,\"from\":\"fullDocument|mergedLayers|currentLayer\"?}", pc_engine_has_doc, new_snapshot,
     true},
    {"history.selectSnapshot", "Select Snapshot", NULL, 0, NULL, "{\"snapshot\":id}", pc_engine_has_doc,
     select_snapshot, true},
    {"history.renameSnapshot", "Rename Snapshot", NULL, 0, NULL, "{\"snapshot\":id,\"name\":str}",
     pc_engine_has_doc, rename_snapshot, true},
    {"history.deleteSnapshot", "Delete Snapshot", NULL, 0, NULL, "{\"snapshot\":id}", pc_engine_has_doc,
     delete_snapshot, true},
};
const pc_engine_command *pc_engine_history_specs(size_t *n)
{
    *n = sizeof specs / sizeof *specs;
    return specs;
}
