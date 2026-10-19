#include "move.h"
#include "preview.h"
#include "layer_cmds.h"
#include "smart_cmds.h"
#include "brush_cmds.h"
#include "type_cmds.h"
#include "vector/vector.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
static int compare_id(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}
static bool contains(const uint64_t *ids, size_t count, uint64_t id)
{
    return count && bsearch(&id, ids, count, sizeof *ids, compare_id) != NULL;
}
static bool rect_fits(pc_rect r, int32_t dx, int32_t dy)
{
    return pc_rect_is_empty(r) || ((int64_t)r.x0 + dx >= INT32_MIN && (int64_t)r.x1 + dx <= INT32_MAX &&
                                   (int64_t)r.y0 + dy >= INT32_MIN && (int64_t)r.y1 + dy <= INT32_MAX);
}
static pc_status shift(pc_surface *s, int32_t dx, int32_t dy, pc_error *err)
{
    pc_rect bounds = pc_surface_content_bounds(s);
    if (!rect_fits(bounds, dx, dy)) {
        return pc_fail(err, PC_ERR_INVALID, "Move exceeds the coordinate range");
    }
    pc_surface out = {0};
    pc_status status = pc_surface_translated(s, dx, dy, bounds, &out, err);
    if (status == PC_OK) {
        pc_surface_fini(s);
        *s = out;
    }
    return status;
}
static pc_status path_shift(pc_path *path, int32_t dx, int32_t dy, pc_error *err)
{
    pc_path out;
    pc_status status = pc_path_transform(path, pc_affine_translate(dx, dy), &out, err);
    if (status == PC_OK) {
        pc_path_fini(path);
        *path = out;
    }
    return status;
}
static pc_status move_layer(const pc_document *before, pc_layer *layer, int32_t dx, int32_t dy,
                            unsigned depth, pc_error *err)
{
    if (depth >= 100) {
        return pc_fail(err, PC_ERR_LIMIT, "Move layer nesting limit");
    }
    pc_status status = PC_OK;
    if (layer->effects.has_reference) {
        layer->effects.reference[0] += dx;
        layer->effects.reference[1] += dy;
    }
    if (layer->has_mask && layer->mask.linked) {
        status = shift(&layer->mask.surface, dx, dy, err);
    }
    if (status == PC_OK && layer->has_vector_mask && layer->vector_mask.linked) {
        status = path_shift(&layer->vector_mask.path, dx, dy, err);
    }
    if (status != PC_OK) {
        return status;
    }
    switch (layer->content.kind) {
    case PC_LAYER_RASTER:
        return shift(&layer->content.u.raster, dx, dy, err);
    case PC_LAYER_TEXT: {
        pc_text_layer *text = &layer->content.u.text;
        text->transform = pc_affine_mul(pc_affine_translate(dx, dy), text->transform);
        if (text->has_cache) {
            status = shift(&text->cache, dx, dy, err);
        }
        if (status == PC_OK) {
            pc_engine_type_retag(before->resolution_dpi, text);
        }
        return status;
    }
    case PC_LAYER_SHAPE: {
        pc_shape_layer *shape = &layer->content.u.shape;
        status = path_shift(&shape->path, dx, dy, err);
        if (status != PC_OK) {
            return status;
        }
        if (shape->has_live) {
            if (shape->live.kind == PC_LIVE_LINE) {
                shape->live.from[0] += dx;
                shape->live.from[1] += dy;
                shape->live.to[0] += dx;
                shape->live.to[1] += dy;
            } else {
                shape->live.rect[0] += dx;
                shape->live.rect[1] += dy;
            }
        }
        pc_blob_release(shape->psd_raw);
        shape->psd_raw = NULL;
        pc_surface rendered = {0};
        status = pc_vec_render_shape(shape, pc_document_pixel_format(before), pc_document_bounds(before),
                                     &rendered, err);
        if (status == PC_OK) {
            if (shape->has_cache) {
                pc_surface_fini(&shape->cache);
            }
            shape->cache = rendered;
            shape->has_cache = true;
        }
        return status;
    }
    case PC_LAYER_SMART:
        return pc_engine_smart_shift(&layer->content.u.smart, dx, dy, err);
    case PC_LAYER_GROUP: {
        pc_group *group = &layer->content.u.group;
        if (group->has_artboard) {
            if (!rect_fits(group->artboard.rect, dx, dy)) {
                return pc_fail(err, PC_ERR_INVALID, "artboard Move exceeds the coordinate range");
            }
            group->artboard.rect = pc_rect_translate(group->artboard.rect, dx, dy);
        }
        for (size_t i = 0; i < group->nchildren && status == PC_OK; i++) {
            status = move_layer(before, &group->children[i], dx, dy, depth + 1, err);
        }
        return status;
    }
    default:
        return PC_OK;
    }
}
typedef struct move_context {
    const pc_document *before;
    pc_layer_id *ids;
    bool *mask_only;
    size_t count;
    int32_t dx, dy;
} move_context;
static void context_fini(move_context *c)
{
    free(c->ids);
    free(c->mask_only);
    *c = (move_context){0};
}
static pc_status offsets(const pc_document *doc, yyjson_val *params, int32_t *dx, int32_t *dy, pc_error *err)
{
    const char *units = pc_json_as_str(pc_json_get(params, "units"), NULL);
    yyjson_val *u = pc_json_get(params, "units");
    if (u && (!units || (strcmp(units, "px") && strcmp(units, "pt")))) {
        return pc_fail(err, PC_ERR_INVALID, "Move units must be px or pt");
    }
    int32_t values[2] = {0, 0};
    const char *keys[] = {"dx", "dy"};
    for (size_t i = 0; i < 2; i++) {
        if (units && !strcmp(units, "pt")) {
            double value = 0;
            yyjson_val *given = pc_json_get(params, keys[i]);
            if (given && !pc_json_as_f64(given, &value)) {
                return pc_fail(err, PC_ERR_INVALID, "Move offset must be numeric");
            }
            value = round(value * doc->resolution_dpi / 72.0);
            if (!isfinite(value) || value < INT32_MIN || value > INT32_MAX) {
                return pc_fail(err, PC_ERR_INVALID, "Move offset exceeds the coordinate range");
            }
            values[i] = (int32_t)value;
        } else {
            bool present = false;
            pc_status status =
                pc_engine_param_int_i32("layer.translate", params, keys[i], &present, &values[i], err);
            if (status != PC_OK) {
                return status;
            }
        }
    }
    *dx = values[0];
    *dy = values[1];
    return PC_OK;
}
static pc_status prepare(const pc_engine_session *session, yyjson_val *params, move_context *c, pc_error *err)
{
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (!state) {
        return pc_engine_err_no_document(err);
    }
    c->before = pc_doc_arc_get(state->doc);
    pc_status status = offsets(c->before, params, &c->dx, &c->dy, err);
    if (status != PC_OK || (!c->dx && !c->dy)) {
        return status;
    }
    bool explicit = false;
    pc_layer_id id = 0;
    status = pc_engine_explicit_layer(session, params, &explicit, &id, err);
    pc_layer_id *roots = NULL;
    size_t nroots = 0;
    if (status == PC_OK) {
        if (explicit) {
            roots = malloc(sizeof *roots);
            if (roots) {
                roots[0] = id;
                nroots = 1;
            } else {
                status = pc_fail(err, PC_ERR_NOMEM, "Move target");
            }
        } else {
            status = pc_engine_selected(session, &roots, &nroots, err);
        }
    }
    if (status != PC_OK) {
        free(roots);
        return status;
    }
    if (!nroots) {
        free(roots);
        return pc_fail(err, PC_ERR_INVALID, "no layer selected");
    }
    qsort(roots, nroots, sizeof *roots, compare_id);
    pc_walk walk = {0};
    status = pc_document_walk(c->before, &walk, err);
    uint64_t *groups = status == PC_OK ? malloc(nroots * sizeof *groups) : NULL;
    size_t ngroups = 0;
    if (status == PC_OK) {
        c->ids = malloc((walk.len ? walk.len : 1) * sizeof *c->ids);
        c->mask_only = calloc(walk.len ? walk.len : 1, sizeof *c->mask_only);
        if (!groups || !c->ids || !c->mask_only) {
            status = pc_fail(err, PC_ERR_NOMEM, "Move targets");
        }
    }
    bool mask = pc_engine_is_mask_target(params);
    for (size_t i = 0; i < walk.len && status == PC_OK; i++) {
        const pc_layer *layer = walk.items[i].layer;
        bool alone = mask && layer->has_mask && !layer->mask.linked;
        if (contains(roots, nroots, layer->id) && !alone && layer->has_link_group) {
            groups[ngroups++] = layer->link_group;
        }
    }
    if (ngroups > 1) {
        qsort(groups, ngroups, sizeof *groups, compare_id);
    }
    bool ancestor[101] = {false};
    for (size_t i = 0; i < walk.len && status == PC_OK; i++) {
        const pc_walk_item *item = &walk.items[i];
        const pc_layer *layer = item->layer;
        if (item->depth >= 100) {
            status = pc_fail(err, PC_ERR_LIMIT, "Move layer nesting limit");
            break;
        }
        bool alone = mask && layer->has_mask && !layer->mask.linked && contains(roots, nroots, layer->id);
        bool selected = contains(roots, nroots, layer->id) ||
                        (layer->has_link_group && contains(groups, ngroups, layer->link_group));
        bool parent = item->depth ? ancestor[item->depth - 1] : false;
        ancestor[item->depth] = parent || (selected && !alone);
        if ((selected && !parent) || alone) {
            c->ids[c->count] = layer->id;
            c->mask_only[c->count++] = alone;
        }
    }
    free(groups);
    free(roots);
    pc_walk_fini(&walk);
    if (status == PC_OK && !c->count) {
        status = pc_fail(err, PC_ERR_INVALID, "no movable layer");
    }
    return status;
}
static pc_status fit_boards(pc_document *doc, pc_error *err)
{
    pc_artboard_ref *boards = NULL;
    size_t count = 0;
    pc_status status = pc_document_artboards(doc, &boards, &count, err);
    if (status != PC_OK || !count) {
        free(boards);
        return status;
    }
    int64_t x0 = 0, y0 = 0, x1 = doc->size.width, y1 = doc->size.height;
    for (size_t i = 0; i < count; i++) {
        pc_rect r = boards[i].artboard->rect;
        if (r.x0 < x0) {
            x0 = r.x0;
        }
        if (r.y0 < y0) {
            y0 = r.y0;
        }
        if (r.x1 > x1) {
            x1 = r.x1;
        }
        if (r.y1 > y1) {
            y1 = r.y1;
        }
    }
    free(boards);
    if (x1 - x0 > 300000 || y1 - y0 > 300000 || -x0 > INT32_MAX || -y0 > INT32_MAX) {
        return pc_fail(err, PC_ERR_LIMIT, "Move exceeds the artboard canvas limit");
    }
    if (x0 || y0) {
        status = pc_engine_translate_doc(doc, "layer.translate", (int32_t)-x0, (int32_t)-y0, err);
    }
    if (status == PC_OK) {
        bool changed = doc->size.width != (uint32_t)(x1 - x0) || doc->size.height != (uint32_t)(y1 - y0);
        doc->size = pc_size_make((uint32_t)(x1 - x0), (uint32_t)(y1 - y0));
        if (changed) {
            pc_walk walk = {0};
            status = pc_document_walk(doc, &walk, err);
            for (size_t i = 0; i < walk.len && status == PC_OK; i++) {
                if (walk.items[i].layer->content.kind != PC_LAYER_SHAPE) {
                    continue;
                }
                pc_layer *layer = pc_document_layer_mut(doc, walk.items[i].layer->id);
                pc_surface rendered = {0};
                status = pc_vec_render_shape(&layer->content.u.shape, pc_document_pixel_format(doc),
                                             pc_document_bounds(doc), &rendered, err);
                if (status == PC_OK) {
                    pc_shape_layer *shape = &layer->content.u.shape;
                    if (shape->has_cache) {
                        pc_surface_fini(&shape->cache);
                    }
                    shape->cache = rendered;
                    shape->has_cache = true;
                }
            }
            pc_walk_fini(&walk);
        }
    }
    return status;
}
static pc_status apply(pc_document *doc, bool *has_active, pc_layer_id *active, void *userdata, pc_error *err)
{
    (void)has_active;
    (void)active;
    move_context *c = userdata;
    bool board = false;
    for (size_t i = 0; i < c->count; i++) {
        pc_locks locks = pc_document_effective_locks(doc, c->ids[i]);
        pc_layer *layer = pc_document_layer_mut(doc, c->ids[i]);
        if (!layer) {
            return pc_engine_err_no_layer(err, c->ids[i]);
        }
        if (locks.position || locks.all) {
            return pc_fail(err, PC_ERR_INVALID, "the target layer is position-locked");
        }
        pc_status status;
        if (c->mask_only[i]) {
            status = shift(&layer->mask.surface, c->dx, c->dy, err);
        } else {
            board = board || (layer->content.kind == PC_LAYER_GROUP && layer->content.u.group.has_artboard);
            status = move_layer(c->before, layer, c->dx, c->dy, 0, err);
        }
        if (status != PC_OK) {
            return status;
        }
    }
    return board ? fit_boards(doc, err) : PC_OK;
}
static pc_status stage_copy_move(const pc_engine_session *session, yyjson_val *params,
                                 pc_engine_session **out, pc_error *err)
{
    pc_engine_session *prepared = NULL;
    pc_status status = pc_engine_preview_clone(session, &prepared, err);
    if (status != PC_OK)
        return status;
    prepared->authorize = session->authorize;
    pc_json_value reply = PC_JSON_VALUE_INIT;
    yyjson_val *layer = pc_json_get(params, "layer");
    if (layer) {
        uint64_t id = 0;
        if (!pc_json_as_u64(layer, &id))
            status = pc_fail(err, PC_ERR_INVALID, "invalid layer id");
        char select[80];
        (void)snprintf(select, sizeof select, "{\"layer\":%llu}", (unsigned long long)id);
        if (status == PC_OK)
            status = pc_engine_session_execute_json(prepared, "layer.select", select, &reply, err);
    }
    if (status == PC_OK)
        status =
            pc_engine_session_execute_json(prepared, "layer.duplicate", "{\"inPlace\":true}", &reply, err);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = d ? yyjson_val_mut_copy(d, params) : NULL;
    pc_buf text = PC_BUF_INIT;
    if (status == PC_OK && !p)
        status = pc_fail(err, PC_ERR_NOMEM, "copy Move parameters");
    if (status == PC_OK) {
        (void)yyjson_mut_obj_remove_str(p, "copy");
        /* Copies become the selected targets; an explicit source id must not move the original. */
        (void)yyjson_mut_obj_remove_str(p, "layer");
        status = pc_json_write(p, 0, &text, err);
    }
    if (status == PC_OK)
        status = pc_engine_session_execute_json(prepared, "layer.translate", pc_buf_cstr(&text), &reply, err);
    pc_json_value_fini(&reply);
    pc_buf_fini(&text);
    yyjson_mut_doc_free(d);
    if (status != PC_OK)
        pc_engine_session_free(prepared);
    else
        *out = prepared;
    return status;
}
static pc_status adopt_move_copy(pc_document *doc, bool *has_active, pc_layer_id *active, void *context,
                                 pc_error *err)
{
    const pc_engine_doc_state *state = context;
    pc_document copy;
    pc_status status = pc_document_clone(pc_doc_arc_get(state->doc), &copy, err);
    if (status == PC_OK) {
        pc_document_fini(doc);
        *doc = copy;
        *has_active = state->has_active_layer;
        *active = state->active_layer;
    }
    return status;
}
pc_status pc_engine_move_preview_layer(const pc_engine_session *session, yyjson_val *params, pc_doc_arc **out,
                                       pc_layer_id *active_layer, pc_error *err)
{
    if (!session || !out) {
        return pc_fail(err, PC_ERR_INVALID, "missing Move context");
    }
    bool copy = false;
    if (pc_json_get(params, "copy") && !pc_json_as_bool(pc_json_get(params, "copy"), &copy))
        return pc_fail(err, PC_ERR_INVALID, "copy must be a boolean");
    if (copy) {
        pc_engine_session *prepared = NULL;
        pc_status status = stage_copy_move(session, params, &prepared, err);
        if (status == PC_OK) {
            const pc_engine_doc_state *state = pc_engine_session_active(prepared);
            *out = pc_doc_arc_retain(state->doc);
            if (active_layer)
                *active_layer = state->has_active_layer ? state->active_layer : 0;
        }
        pc_engine_session_free(prepared);
        return status;
    }
    move_context c = {0};
    pc_status status = prepare(session, params, &c, err);
    pc_document doc;
    if (status == PC_OK) {
        status = pc_document_clone(c.before, &doc, err);
    }
    if (status == PC_OK) {
        bool has_active = false;
        pc_layer_id active = 0;
        status = apply(&doc, &has_active, &active, &c, err);
        if (status == PC_OK) {
            status = pc_doc_arc_new(&doc, out, err);
        }
        if (status != PC_OK) {
            pc_document_fini(&doc);
        }
    }
    if (status == PC_OK && active_layer) {
        const pc_engine_doc_state *state = pc_engine_session_active(session);
        *active_layer = state && state->has_active_layer ? state->active_layer : 0;
    }
    context_fini(&c);
    return status;
}
pc_status pc_engine_move_preview(const pc_engine_session *session, yyjson_val *params, pc_doc_arc **out,
                                 pc_error *err)
{
    return pc_engine_move_preview_layer(session, params, out, NULL, err);
}
static pc_status run(pc_engine_session *session, yyjson_val *params, pc_json_value *out, pc_error *err)
{
    move_context c = {0};
    pc_status status = prepare(session, params, &c, err);
    bool copy = false;
    if (status == PC_OK && pc_json_get(params, "copy") &&
        !pc_json_as_bool(pc_json_get(params, "copy"), &copy))
        status = pc_fail(err, PC_ERR_INVALID, "copy must be a boolean");
    if (status == PC_OK && (c.dx || c.dy)) {
        if (copy) {
            pc_engine_session *prepared = NULL;
            status = stage_copy_move(session, params, &prepared, err);
            pc_engine_doc_state *state = prepared ? pc_engine_session_active_mut(prepared) : NULL;
            if (status == PC_OK)
                status = pc_engine_session_edit(session, "Duplicate + Move", adopt_move_copy, state, err);
            if (status == PC_OK)
                pc_engine_reselect(session, state->selected_layers, state->nselected, state->has_active_layer,
                                   state->active_layer);
            pc_engine_session_free(prepared);
        } else
            status = pc_engine_session_edit(session, "Move", apply, &c, err);
    }
    context_fini(&c);
    return status == PC_OK ? pc_engine_result_null(out, err) : status;
}
const pc_engine_command *pc_engine_move_specs(size_t *count)
{
    static const pc_engine_command specs[] = {
        {"layer.translate", "Move Layer", NULL, 0, NULL,
         "{\"layer\":id?,\"dx\":i32=0,\"dy\":i32=0,\"units\":\"px|pt\"="
         "\"px\",\"target\":\"pixels|mask\"=\"pixels\",\"copy\":bool=false}",
         pc_engine_has_layer, run, true}};
    *count = sizeof specs / sizeof *specs;
    return specs;
}
