/* JSON views of engine state for automation, tests and debugging
 * (port of crates/engine/src/inspect.rs).
 *
 * E1 reports the structural fields. Fields owned by later chunks are filled through the inspect
 * hooks (channels E7, layer-mask view E5, adjustment serde E8) and enriched as those land; the
 * only field whose absence changes a default layer's view is "only when non-default" data, which
 * is absent for a default layer anyway. */
#include <stdlib.h>

#include "color/blend.h"
#include "color/color.h"
#include "engine_internal.h"

static bool ids_contain(const pc_layer_id *v, size_t n, pc_layer_id id)
{
    for (size_t i = 0; i < n; i++) {
        if (v[i] == id) {
            return true;
        }
    }
    return false;
}

static void bounds_json(yyjson_mut_doc *d, yyjson_mut_val *obj, const char *key, const pc_surface *surf)
{
    if (surf == NULL) {
        yyjson_mut_obj_add_null(d, obj, key);
        return;
    }
    pc_rect r = pc_surface_content_bounds(surf);
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    yyjson_mut_arr_add_int(d, arr, r.x0);
    yyjson_mut_arr_add_int(d, arr, r.y0);
    yyjson_mut_arr_add_int(d, arr, (int64_t)pc_rect_width(r));
    yyjson_mut_arr_add_int(d, arr, (int64_t)pc_rect_height(r));
    yyjson_mut_obj_add_val(d, obj, key, arr);
}

static yyjson_mut_val *layer_sel(yyjson_mut_doc *d, const pc_layer *l, const pc_layer_id *selected,
                                 size_t nselected);

static void children_json(yyjson_mut_doc *d, yyjson_mut_val *v, const pc_group *g,
                          const pc_layer_id *selected, size_t nselected)
{
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    /* Display order: top-to-bottom (children are stored bottom-to-top). */
    for (size_t i = g->nchildren; i-- > 0;) {
        yyjson_mut_arr_append(arr, layer_sel(d, &g->children[i], selected, nselected));
    }
    yyjson_mut_obj_add_val(d, v, "children", arr);
}

static yyjson_mut_val *layer_sel(yyjson_mut_doc *d, const pc_layer *l, const pc_layer_id *selected,
                                 size_t nselected)
{
    yyjson_mut_val *v = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, v, "id", l->id);
    yyjson_mut_obj_add_strcpy(d, v, "name", pc_str_c(&l->name));
    yyjson_mut_obj_add_str(d, v, "kind", pc_layer_content_kind_name(&l->content));
    yyjson_mut_obj_add_bool(d, v, "visible", l->visible);
    yyjson_mut_obj_add_str(d, v, "labelColor", pc_label_color_id(l->label));
    yyjson_mut_obj_add_real(d, v, "opacity", (double)l->opacity);
    yyjson_mut_obj_add_real(d, v, "fill", (double)l->fill_opacity);
    yyjson_mut_obj_add_str(d, v, "blend", pc_blend_label(l->blend));
    yyjson_mut_obj_add_bool(d, v, "clipped", l->clipped);
    yyjson_mut_obj_add_bool(d, v, "hasMask", l->has_mask);
    yyjson_mut_obj_add_bool(d, v, "hasVectorMask", l->has_vector_mask);
    if (l->has_vector_mask) {
        yyjson_mut_obj_add_bool(d, v, "vectorMaskEnabled", l->vector_mask.enabled);
    } else {
        yyjson_mut_obj_add_null(d, v, "vectorMaskEnabled");
    }
    bounds_json(d, v, "bounds", pc_layer_surface(l));
    yyjson_mut_obj_add_bool(d, v, "selected", ids_contain(selected, nselected, l->id));
    if (l->has_link_group) {
        yyjson_mut_obj_add_uint(d, v, "linkGroup", l->link_group);
    } else {
        yyjson_mut_obj_add_null(d, v, "linkGroup");
    }
    switch (l->content.kind) {
    case PC_LAYER_GROUP:
        children_json(d, v, &l->content.u.group, selected, nselected);
        yyjson_mut_obj_add_bool(d, v, "expanded", l->content.u.group.expanded);
        break;
    case PC_LAYER_ADJUSTMENT:
        pc_engine_hook_inspect_adjustment(d, v, l); /* E8 fills "adjustment" */
        break;
    case PC_LAYER_SMART: {
        const pc_smart_object *sm = &l->content.u.smart;
        yyjson_mut_val *fx = yyjson_mut_arr(d);
        for (size_t i = 0; i < sm->nsmart_filters; i++) {
            yyjson_mut_val *f = yyjson_mut_obj(d);
            yyjson_mut_obj_add_strcpy(d, f, "command", pc_str_c(&sm->smart_filters[i].command));
            yyjson_mut_val *pcopy = yyjson_mut_val_mut_copy(d, pc_json_value_root(&sm->smart_filters[i].params));
            if (pcopy != NULL) {
                yyjson_mut_obj_add_val(d, f, "params", pcopy);
            }
            yyjson_mut_obj_add_bool(d, f, "visible", sm->smart_filters[i].visible);
            yyjson_mut_obj_add_real(d, f, "opacity", (double)sm->smart_filters[i].opacity);
            yyjson_mut_obj_add_str(d, f, "blend", pc_blend_label(sm->smart_filters[i].blend));
            yyjson_mut_arr_append(fx, f);
        }
        yyjson_mut_obj_add_val(d, v, "smartFilters", fx);
        yyjson_mut_obj_add_bool(d, v, "smartFiltersEnabled", sm->filters_enabled);
        yyjson_mut_val *src = yyjson_mut_obj(d);
        if (sm->source.linked) {
            yyjson_mut_obj_add_str(d, src, "kind", "linked");
            yyjson_mut_obj_add_strcpy(d, src, "path", pc_str_c(&sm->source.path));
        } else {
            yyjson_mut_obj_add_str(d, src, "kind", "embedded");
            yyjson_mut_obj_add_strcpy(d, src, "fileName", pc_str_c(&sm->source.file_name));
        }
        yyjson_mut_obj_add_val(d, v, "smartSource", src);
        break;
    }
    case PC_LAYER_TEXT: {
        const pc_text_layer *t = &l->content.u.text;
        yyjson_mut_val *tv = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, tv, "text", pc_str_c(&t->text));
        yyjson_mut_obj_add_strcpy(d, tv, "font", pc_str_c(&t->font_family));
        yyjson_mut_obj_add_real(d, tv, "sizePt", (double)t->size_pt);
        yyjson_mut_obj_add_val(d, v, "text", tv);
        break;
    }
    default:
        break;
    }
    return v;
}

pc_status pc_engine_inspect_layer(const pc_layer *l, pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_doc_set_root(d, layer_sel(d, l, NULL, 0));
    return PC_OK;
}

static void colors_json(yyjson_mut_doc *d, yyjson_mut_val *obj, const char *key, const float c[4])
{
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (int i = 0; i < 4; i++) {
        yyjson_mut_arr_add_real(d, arr, (double)c[i]);
    }
    yyjson_mut_obj_add_val(d, obj, key, arr);
}

pc_status pc_engine_inspect_session(const pc_engine_session *s, pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    size_t active;
    if (pc_engine_session_active_index(s, &active)) {
        yyjson_mut_obj_add_uint(d, root, "active", active);
    } else {
        yyjson_mut_obj_add_null(d, root, "active");
    }
    size_t n;
    const pc_engine_doc_state *docs = pc_engine_session_documents(s, &n);
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (size_t i = 0; i < n; i++) {
        const pc_document *doc = pc_doc_arc_get(docs[i].doc);
        yyjson_mut_val *o = yyjson_mut_obj(d);
        yyjson_mut_obj_add_uint(d, o, "index", i);
        yyjson_mut_obj_add_strcpy(d, o, "name", pc_str_c(&doc->name));
        if (docs[i].path != NULL) {
            yyjson_mut_obj_add_strcpy(d, o, "path", docs[i].path);
        } else {
            yyjson_mut_obj_add_null(d, o, "path");
        }
        yyjson_mut_obj_add_uint(d, o, "width", doc->size.width);
        yyjson_mut_obj_add_uint(d, o, "height", doc->size.height);
        yyjson_mut_obj_add_bool(d, o, "dirty", pc_engine_doc_state_is_dirty(&docs[i]));
        yyjson_mut_obj_add_uint(d, o, "revision", docs[i].revision);
        yyjson_mut_arr_append(arr, o);
    }
    yyjson_mut_obj_add_val(d, root, "documents", arr);
    colors_json(d, root, "foreground", s->tools.foreground);
    colors_json(d, root, "background", s->tools.background);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

pc_status pc_engine_inspect_document(const pc_engine_doc_state *dst, pc_json_value *out, pc_error *err)
{
    const pc_document *doc = pc_doc_arc_get(dst->doc);
    pc_layer_id *selected = NULL;
    size_t nselected = 0;
    (void)pc_engine_doc_state_selected_layers(dst, &selected, &nselected, NULL);

    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        free(selected);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "name", pc_str_c(&doc->name));
    if (dst->path != NULL) {
        yyjson_mut_obj_add_strcpy(d, root, "path", dst->path);
    } else {
        yyjson_mut_obj_add_null(d, root, "path");
    }
    yyjson_mut_obj_add_uint(d, root, "width", doc->size.width);
    yyjson_mut_obj_add_uint(d, root, "height", doc->size.height);
    yyjson_mut_obj_add_str(d, root, "mode", pc_color_mode_name(doc->mode));
    yyjson_mut_obj_add_uint(d, root, "depth", pc_sample_type_bits(doc->depth));
    yyjson_mut_obj_add_real(d, root, "resolution", (double)doc->resolution_dpi);
    if (dst->has_active_layer) {
        yyjson_mut_obj_add_uint(d, root, "activeLayer", dst->active_layer);
    } else {
        yyjson_mut_obj_add_null(d, root, "activeLayer");
    }
    yyjson_mut_val *sel = yyjson_mut_arr(d);
    for (size_t i = 0; i < nselected; i++) {
        yyjson_mut_arr_add_uint(d, sel, selected[i]);
    }
    yyjson_mut_obj_add_val(d, root, "selectedLayers", sel);
    yyjson_mut_val *iso = yyjson_mut_arr(d); /* E5 */
    for (size_t i = 0; i < dst->nisolated; i++) {
        yyjson_mut_arr_add_uint(d, iso, dst->isolated_layers[i]);
    }
    yyjson_mut_obj_add_val(d, root, "isolatedLayers", iso);
    yyjson_mut_obj_add_bool(d, root, "hasSelection", doc->has_selection);
    if (doc->has_selection) {
        bounds_json(d, root, "selectionBounds", &doc->selection);
    } else {
        yyjson_mut_obj_add_null(d, root, "selectionBounds");
    }
    yyjson_mut_val *layers = yyjson_mut_arr(d);
    for (size_t i = doc->nlayers; i-- > 0;) { /* top-to-bottom */
        yyjson_mut_arr_append(layers, layer_sel(d, &doc->layers[i], selected, nselected));
    }
    yyjson_mut_obj_add_val(d, root, "layers", layers);
    yyjson_mut_val *hist = yyjson_mut_arr(d);
    size_t ne = pc_history_entry_count(dst->history);
    for (size_t i = 0; i < ne; i++) {
        const pc_str *label = pc_history_entry(dst->history, i);
        yyjson_mut_arr_add_strcpy(d, hist, label != NULL ? pc_str_c(label) : "");
    }
    yyjson_mut_obj_add_val(d, root, "history", hist);
    yyjson_mut_obj_add_val(d, root, "snapshots", yyjson_mut_arr(d)); /* E12 fills this */
    yyjson_mut_obj_add_bool(d, root, "canUndo", pc_history_can_undo(dst->history));
    yyjson_mut_obj_add_bool(d, root, "canRedo", pc_history_can_redo(dst->history));
    yyjson_mut_obj_add_uint(d, root, "revision", dst->revision);
    pc_engine_hook_inspect_channels(d, root, dst); /* E7 fills "channels" */
    yyjson_mut_obj_add_bool(d, root, "quickMask", doc->has_quick_mask);
    yyjson_mut_obj_add_null(d, root, "symmetry"); /* E10 */
    pc_engine_hook_inspect_mask_view(d, root, dst); /* E5 fills "layerMaskView" */
    yyjson_mut_val *comps = yyjson_mut_arr(d);
    for (size_t i = 0; i < doc->nlayer_comps; i++) {
        yyjson_mut_val *c = yyjson_mut_obj(d);
        yyjson_mut_obj_add_uint(d, c, "id", doc->layer_comps[i].id);
        yyjson_mut_obj_add_strcpy(d, c, "name", pc_str_c(&doc->layer_comps[i].name));
        yyjson_mut_arr_append(comps, c);
    }
    yyjson_mut_obj_add_val(d, root, "layerComps", comps);
    if (doc->has_last_applied_comp) {
        yyjson_mut_obj_add_uint(d, root, "lastAppliedComp", doc->last_applied_comp);
    } else {
        yyjson_mut_obj_add_null(d, root, "lastAppliedComp");
    }
    yyjson_mut_doc_set_root(d, root);
    free(selected);
    return PC_OK;
}
