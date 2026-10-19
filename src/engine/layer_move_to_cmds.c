/* Atomic Layers-panel reordering and Alt-drag duplication. */
#include "engine/layer_cmds.h"
#include "engine/layer_base_cmds.h"
#include "base/mem.h"
#include <stdlib.h>
#include <string.h>

typedef struct move_to {
    pc_layer_id *ids, *copies;
    size_t count;
    pc_layer *moving;
    pc_layer_id target, active, background;
    bool copy, batch, has_background;
    enum { ABOVE, BELOW, INTO } position;
} move_to;
static bool contains(const pc_layer_id *ids, size_t n, pc_layer_id id)
{
    for (size_t i = 0; i < n; i++)
        if (ids[i] == id)
            return true;
    return false;
}
static pc_status edit_move(pc_document *doc, bool *has_active, pc_layer_id *active, void *ud, pc_error *err)
{
    move_to *c = ud;
    pc_engine_clip_before before = {0};
    pc_status status = c->copy ? PC_OK : pc_engine_clip_before_read(doc, c->ids, c->count, &before, err);
    for (size_t i = 0; status == PC_OK && i < c->count; i++) {
        status = c->copy ? pc_engine_layer_copy(doc, c->ids[i], &c->moving[i], err)
                         : pc_document_remove(doc, c->ids[i], &c->moving[i], err);
        if (status == PC_OK)
            c->copies[i] = c->moving[i].id;
    }
    pc_layer_path target = {0};
    if (status == PC_OK)
        status = pc_document_path_of(doc, c->target, &target, err);
    pc_layer **siblings = &doc->layers;
    size_t *length = &doc->nlayers;
    size_t at = 0;
    if (status == PC_OK && !target.len)
        status = pc_engine_err_no_layer(err, c->target);
    if (status == PC_OK) {
        pc_layer *parent = c->position == INTO ? pc_document_layer_at_mut(doc, target.idx, target.len)
                           : target.len > 1    ? pc_document_layer_at_mut(doc, target.idx, target.len - 1)
                                               : NULL;
        if (parent) {
            if (!pc_layer_is_group(parent))
                status = pc_engine_err_bad_params(err, "layer.moveTo", "target is not a group");
            else {
                siblings = &parent->content.u.group.children;
                length = &parent->content.u.group.nchildren;
            }
        } else if (c->position == INTO || target.len > 1)
            status = pc_engine_err_no_layer(err, c->target);
        at = c->position == INTO ? *length : target.idx[target.len - 1] + (c->position == ABOVE ? 1u : 0u);
    }
    pc_layer *next = NULL;
    if (status == PC_OK) {
        if (at > *length || c->count > SIZE_MAX - *length)
            status = pc_fail(err, PC_ERR_LIMIT, "layer destination size");
        else {
            next = pc_malloc_array(*length + c->count, sizeof *next);
            if (!next)
                status = pc_fail(err, PC_ERR_NOMEM, "layer destination");
        }
    }
    if (status == PC_OK) {
        if (at)
            memcpy(next, *siblings, at * sizeof *next);
        if (*length > at)
            memcpy(next + at + c->count, *siblings + at, (*length - at) * sizeof *next);
        memcpy(next + at, c->moving, c->count * sizeof *next);
        memset(c->moving, 0, c->count * sizeof *c->moving);
        free(*siblings);
        *siblings = next;
        *length += c->count;
        size_t depth = 0;
        status = pc_document_max_group_depth(doc, &depth, err);
        if (status == PC_OK && depth > PC_MAX_GROUP_DEPTH)
            status = pc_engine_err_bad_params(err, "layer.moveTo", "layer groups would be too deep");
        if (status == PC_OK && c->has_background && (!doc->nlayers || doc->layers[0].id != c->background))
            status = pc_engine_err_bad_params(err, "layer.moveTo", "the Background stays at the bottom");
        if (status == PC_OK)
            status = pc_engine_clip_settle(doc, c->copy ? c->copies : c->ids, c->count,
                                           c->copy ? NULL : &before, false, err);
        if (status == PC_OK) {
            *has_active = true;
            *active = c->copy ? c->copies[c->count - 1] : c->active;
        }
    }
    pc_layer_path_fini(&target);
    pc_engine_clip_before_fini(&before);
    return status;
}
static pc_status run(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "layer.moveTo";
    const pc_engine_doc_state *state = pc_engine_session_active(s);
    if (!state)
        return pc_engine_err_no_document(err);
    const pc_document *doc = pc_doc_arc_get(state->doc);
    move_to c = {0};
    if (!pc_json_as_u64(pc_json_get(p, "target"), &c.target))
        return pc_engine_err_bad_params(err, cmd, "missing target layer id");
    const char *position = pc_json_as_str(pc_json_get(p, "position"), NULL);
    if (!position || !strcmp(position, "above"))
        c.position = ABOVE;
    else if (!strcmp(position, "below"))
        c.position = BELOW;
    else if (!strcmp(position, "into"))
        c.position = INTO;
    else
        return pc_engine_err_bad_params(err, cmd, "position must be above, below, or into");
    yyjson_val *copy = pc_json_get(p, "copy");
    if (copy && !yyjson_is_bool(copy))
        return pc_engine_err_bad_params(err, cmd, "copy must be a boolean");
    c.copy = copy && yyjson_get_bool(copy);
    yyjson_val *batch = pc_json_get(p, "layers");
    c.batch = batch != NULL;
    if (batch && pc_json_get(p, "layer"))
        return pc_engine_err_bad_params(err, cmd, "give layer or layers, not both");
    size_t count = batch ? yyjson_arr_size(batch) : 1;
    if (!count || count > 4096 || (batch && !yyjson_is_arr(batch)))
        return pc_engine_err_bad_params(err, cmd, "layers must be a nonempty array of at most 4096 ids");
    pc_layer_id *requested = pc_calloc_array(count, sizeof *requested);
    if (!requested)
        return pc_fail(err, PC_ERR_NOMEM, "requested layers");
    pc_status status = PC_OK;
    for (size_t i = 0; status == PC_OK && i < count; i++) {
        if (batch) {
            if (!pc_json_as_u64(yyjson_arr_get(batch, i), &requested[i]) ||
                contains(requested, i, requested[i]))
                status = pc_engine_err_bad_params(err, cmd, "layer ids must be unique unsigned integers");
        } else
            status = pc_engine_layer_param(s, p, &requested[i], err);
        if (status == PC_OK && !pc_document_layer(doc, requested[i]))
            status = pc_engine_err_no_layer(err, requested[i]);
    }
    pc_layer_path target = {0};
    if (status == PC_OK)
        status = pc_document_path_of(doc, c.target, &target, err);
    if (status == PC_OK)
        status = pc_engine_top_level(doc, requested, count, &c.ids, &c.count, err);
    if (status == PC_OK && !c.count)
        status = pc_engine_err_bad_params(err, cmd, "no source layers");
    if (status == PC_OK && c.count == 1 && c.ids[0] == c.target && !c.copy && !batch) {
        free(requested);
        free(c.ids);
        pc_layer_path_fini(&target);
        return pc_engine_result_null(out, err);
    }
    for (size_t i = 0; status == PC_OK && i < c.count; i++) {
        pc_layer_path path = {0};
        status = pc_document_path_of(doc, c.ids[i], &path, err);
        bool self_copy = c.copy && c.ids[i] == c.target && c.position != INTO;
        if (status == PC_OK && !self_copy && path.len && path.len <= target.len &&
            !memcmp(path.idx, target.idx, path.len * sizeof *path.idx))
            status = pc_engine_err_bad_params(err, cmd, "target is a moved layer or its descendant");
        if (status == PC_OK && !c.copy && pc_document_is_background(doc, c.ids[i]))
            status = pc_engine_err_bad_params(err, cmd, "the Background cannot be moved");
        pc_layer_path_fini(&path);
    }
    const pc_layer *destination = pc_document_layer(doc, c.target), *background = pc_document_background(doc);
    c.has_background = background != NULL;
    c.background = background ? background->id : 0;
    if (status == PC_OK && c.position == BELOW && c.has_background && c.target == c.background)
        status = pc_engine_err_bad_params(err, cmd, "the Background stays at the bottom");
    if (status == PC_OK && c.position == INTO && (!destination || !pc_layer_is_group(destination)))
        status = pc_engine_err_bad_params(err, cmd, "target is not a group");
    c.active = state->has_active_layer && contains(requested, count, state->active_layer)
                   ? state->active_layer
                   : requested[count - 1];
    if (!c.batch && c.count)
        c.active = c.ids[0];
    if (status == PC_OK) {
        c.moving = pc_calloc_array(c.count, sizeof *c.moving);
        c.copies = pc_malloc_array(c.count, sizeof *c.copies);
        if (!c.moving || !c.copies)
            status = pc_fail(err, PC_ERR_NOMEM, "moving layers");
    }
    if (status == PC_OK)
        status = pc_engine_session_edit(s, c.copy ? "Duplicate Layer" : "Reorder Layers", edit_move, &c, err);
    if (status == PC_OK) {
        if (c.batch || c.copy)
            pc_engine_reselect(s, c.copy ? c.copies : requested, c.copy ? c.count : count, true,
                               c.copy ? c.copies[c.count - 1] : c.active);
        yyjson_mut_doc *d = NULL;
        status = pc_engine_result_doc(out, &d, err);
        if (status == PC_OK) {
            yyjson_mut_val *root = yyjson_mut_null(d);
            if (c.batch) {
                root = yyjson_mut_obj(d);
                yyjson_mut_obj_add_uint(d, root, "moved", c.count);
                if (c.copy) {
                    yyjson_mut_val *ids = yyjson_mut_arr(d);
                    for (size_t i = 0; i < c.count; i++)
                        yyjson_mut_arr_add_uint(d, ids, c.copies[i]);
                    yyjson_mut_obj_add_val(d, root, "layers", ids);
                }
            } else if (c.copy) {
                root = yyjson_mut_obj(d);
                yyjson_mut_obj_add_uint(d, root, "layer", c.copies[0]);
            }
            yyjson_mut_doc_set_root(d, root);
        }
    }
    if (c.moving)
        for (size_t i = 0; i < c.count; i++)
            pc_layer_fini(&c.moving[i]);
    free(c.moving);
    free(c.copies);
    free(c.ids);
    free(requested);
    pc_layer_path_fini(&target);
    return status;
}
const pc_engine_command *pc_engine_layer_move_to_specs(size_t *n)
{
    static const pc_engine_command specs[] = {
        {"layer.moveTo", "Reorder Layer", NULL, 0, NULL,
         "{\"layer\":id?|\"layers\":[id,...]?,\"target\":id,\"position\":\"above|below|into\"=\"above\","
         "\"copy\":bool=false}",
         pc_engine_has_layer, run, true}};
    *n = sizeof specs / sizeof specs[0];
    return specs;
}
