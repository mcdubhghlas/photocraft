/* Layer > Rasterize > Layer Style: layer.rasterize.layerStyle (port of
 * crates/engine/src/rasterize_style_cmds.rs).
 *
 * Each target layer with effects is rendered alone by the compositor at 100 % opacity in Normal
 * mode, so its effects, fill opacity and masks are baked into its pixels; the layer's own opacity,
 * blend mode, knockout and Blend If stay layer properties and apply to the baked pixels. Type,
 * shape, fill and Smart Object layers are rasterized first, in the same undo step.
 *
 * Never crashes (REWRITE_PLAN.md rule 9): a layer with nothing to bake is skipped, every param is
 * validated, and the one history step is all-or-nothing. */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "base/mem.h"
#include "compose/compose.h"
#include "compose/effects.h"
#include "doc/doc.h"
#include "geom/geom.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#include "allocation.h"
#include "engine_internal.h"
#include "layer_cmds.h" /* pc_engine_layer_param, pc_engine_selected */
#include "rasterize_style_cmds.h"

#define CMD "layer.rasterize.layerStyle"

static const char *const MENU_LAYER_RASTERIZE[] = {"Layer", "Rasterize"};

/* Visible effects on a layer whose content can become pixels. */
static bool can_bake(const pc_layer *l)
{
    return pc_compose_has_effects(l) && l->content.kind != PC_LAYER_GROUP &&
           l->content.kind != PC_LAYER_ADJUSTMENT;
}

static pc_status rls_enabled(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (doc == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    pc_layer_id *sel = NULL;
    size_t nsel = 0;
    (void)pc_engine_selected(s, &sel, &nsel, NULL);
    bool found = false;
    for (size_t i = 0; i < nsel; i++) {
        const pc_layer *l = pc_document_layer(doc, sel[i]);
        if (l != NULL && can_bake(l)) {
            found = true;
            break;
        }
    }
    free(sel);
    if (!found && d->has_active_layer) {
        const pc_layer *l = pc_document_layer(doc, d->active_layer);
        found = l != NULL && can_bake(l);
    }
    if (!found) {
        return pc_fail(reason, PC_ERR_INVALID, "no selected layer has layer effects to rasterize");
    }
    return PC_OK;
}

/* targets(): "layer" -> that one; otherwise the selected layers, else the active layer. */
static pc_status collect(const pc_engine_session *s, yyjson_val *p, pc_layer_id **out, size_t *n,
                         pc_error *err)
{
    *out = NULL;
    *n = 0;
    if (p != NULL && pc_json_get(p, "layer") != NULL) {
        pc_layer_id id = 0;
        pc_status rc = pc_engine_layer_param(s, p, &id, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_layer_id *a = pc_malloc_array(1, sizeof *a);
        if (a == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "rasterize layer style");
        }
        a[0] = id;
        *out = a;
        *n = 1;
        return PC_OK;
    }
    pc_layer_id *sel = NULL;
    size_t nsel = 0;
    (void)pc_engine_selected(s, &sel, &nsel, NULL);
    if (nsel > 0) {
        *out = sel;
        *n = nsel;
        return PC_OK;
    }
    free(sel);
    pc_layer_id id = 0;
    pc_status rc = pc_engine_layer_param(s, NULL, &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_layer_id *a = pc_malloc_array(1, sizeof *a);
    if (a == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "rasterize layer style");
    }
    a[0] = id;
    *out = a;
    *n = 1;
    return PC_OK;
}

static pc_status bake_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    (void)active;
    pc_layer_id id = *(pc_layer_id *)vctx;
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    if (l->content.kind != PC_LAYER_RASTER) {
        return pc_engine_err_other(err, "the layer could not be rasterized");
    }
    pc_rect canvas = pc_document_bounds(doc);
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    size_t n = pc_pixel_format_channels(fmt);
    if (n == 0) {
        return pc_engine_err_other(err, "the layer has no channels");
    }
    int32_t margin = pc_compose_effects_margin(l);
    if (margin < 0) {
        margin = 0;
    }
    pc_rect area = pc_rect_inflate(pc_compose_layer_bounds(l, canvas), margin);

    /* Render the layer alone with the document's settings. */
    pc_layer alone;
    pc_status rc = pc_layer_clone(l, &alone, err);
    if (rc != PC_OK) {
        return rc;
    }
    alone.visible = true;
    alone.opacity = 1.0f;
    alone.blend = PC_BLEND_NORMAL;
    alone.clipped = false;
    alone.advanced.knockout = PC_KNOCKOUT_NONE;
    pc_blend_if_fini(&alone.blend_if);
    memset(&alone.blend_if, 0, sizeof alone.blend_if);
    alone.excluded_channels = 0;

    float *data = NULL;
    pc_surface px;
    pc_surface_init(&px, fmt);
    if (!pc_rect_is_empty(area)) {
        pc_layer *saved = doc->layers;
        size_t saved_n = doc->nlayers;
        doc->layers = &alone;
        doc->nlayers = 1;
        pc_compose_buffer buf;
        rc = pc_compose_render(doc, area, &buf, err);
        doc->layers = saved;
        doc->nlayers = saved_n;
        if (rc != PC_OK) {
            pc_layer_fini(&alone);
            pc_surface_fini(&px);
            return rc;
        }
        size_t want = buf.len * n;
        if (pc_engine_alloc_filled(want ? want : 1, sizeof(float), "rasterize layer style",
                                   (void **)&data, err) != PC_OK) {
            pc_compose_buffer_fini(&buf);
            pc_layer_fini(&alone);
            pc_surface_fini(&px);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        for (size_t i = 0; i < buf.len; i++) {
            (void)pc_from_rgba(fmt, buf.px[i], &data[i * n]);
        }
        pc_rect brect = buf.rect;
        size_t blen = buf.len;
        pc_compose_buffer_fini(&buf);
        rc = pc_surface_write_region(&px, brect, data, blen * n, err);
        free(data);
        if (rc != PC_OK) {
            pc_layer_fini(&alone);
            pc_surface_fini(&px);
            return rc;
        }
    }
    pc_layer_fini(&alone);
    pc_surface_prune(&px);

    /* Replace content and strip what is now in the pixels. */
    pc_layer *m = pc_document_layer_mut(doc, id);
    if (m == NULL) {
        pc_surface_fini(&px);
        return pc_engine_err_no_layer(err, id);
    }
    pc_layer_content_fini(&m->content);
    m->content.kind = PC_LAYER_RASTER;
    m->content.u.raster = px; /* moved */
    if (m->has_fill_cache) {
        pc_fill_fini(&m->fill_cache.fill);
        pc_surface_fini(&m->fill_cache.surface);
        m->has_fill_cache = false;
    }
    m->fill_opacity = 1.0f;
    if (m->has_mask) {
        pc_layer_mask_fini(&m->mask);
        m->has_mask = false;
    }
    if (m->has_vector_mask) {
        pc_vector_mask_fini(&m->vector_mask);
        m->has_vector_mask = false;
    }
    pc_effects_fini(&m->effects);
    memset(&m->effects, 0, sizeof m->effects);
    m->effects.enabled = true;
    m->advanced.blend_interior = false;
    m->advanced.layer_mask_hides_effects = false;
    m->advanced.vector_mask_hides_effects = false;
    return PC_OK;
}

struct rls_ctx {
    pc_layer_id *ids;
    size_t n;
    size_t *done; /* appended ids actually baked */
    size_t ndone;
};

static pc_status rls_compound(pc_engine_session *s, void *vctx, pc_error *err)
{
    struct rls_ctx *c = vctx;
    for (size_t i = 0; i < c->n; i++) {
        pc_layer_id id = c->ids[i];
        const pc_engine_doc_state *d = pc_engine_session_active(s);
        const pc_document *doc = d != NULL ? pc_doc_arc_get(d->doc) : NULL;
        const pc_layer *l = doc != NULL ? pc_document_layer(doc, id) : NULL;
        if (l == NULL || !can_bake(l)) {
            continue;
        }
        /* Type/shape/fill/smart content must become pixels first (same step). */
        if (l->content.kind != PC_LAYER_RASTER) {
            char buf[64];
            (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)id);
            pc_json_value sub = PC_JSON_VALUE_INIT;
            pc_status rc = pc_engine_session_execute_json(s, "layer.rasterize.layer", buf, &sub, err);
            pc_json_value_fini(&sub);
            if (rc != PC_OK) {
                return rc;
            }
        }
        pc_status rc = pc_engine_session_edit(s, "Rasterize Layer Style", bake_edit, &id, err);
        if (rc != PC_OK) {
            return rc;
        }
        c->done[c->ndone++] = id;
    }
    return PC_OK;
}

static pc_status run_rls(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_layer_id *ids = NULL;
    size_t nids = 0;
    pc_status rc = collect(s, p, &ids, &nids, err);
    if (rc != PC_OK) {
        return rc;
    }
    bool many = nids > 1;
    size_t *done = pc_malloc_array(nids ? nids : 1, sizeof *done);
    if (done == NULL) {
        free(ids);
        return pc_fail(err, PC_ERR_NOMEM, "rasterize layer style");
    }
    struct rls_ctx c = {ids, nids, done, 0};
    rc = pc_engine_compound(s, "Rasterize Layer Style", rls_compound, &c, err);
    free(ids);
    if (rc != PC_OK) {
        free(done);
        return rc;
    }
    if (c.ndone == 0) {
        free(done);
        return pc_engine_err_other(err, many ? "none of the selected layers has layer effects"
                                              : "the layer has no layer effects");
    }

    /* The active layer if it was baked, else the first baked. */
    uint64_t first = done[0];
    uint64_t active = first;
    const pc_engine_doc_state *nd = pc_engine_session_active(s);
    if (nd != NULL && nd->has_active_layer) {
        for (size_t i = 0; i < c.ndone; i++) {
            if (done[i] == nd->active_layer) {
                active = nd->active_layer;
                break;
            }
        }
    }

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        free(done);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", active);
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    for (size_t i = 0; i < c.ndone; i++) {
        yyjson_mut_arr_add_uint(rd, arr, done[i]);
    }
    yyjson_mut_obj_add_val(rd, root, "layers", arr);
    yyjson_mut_doc_set_root(rd, root);
    free(done);
    return PC_OK;
}

#define RLS_PARAMS                                                                                   \
    "{\"layer\":id?} (no layer: every selected layer with effects; bakes the effects, fill opacity " \
    "and masks into the pixels)"

static const pc_engine_command SPECS[] = {
    {CMD, "Layer Style", MENU_LAYER_RASTERIZE, 2, NULL, RLS_PARAMS, rls_enabled, run_rls, true},
};

const pc_engine_command *pc_engine_rasterize_style_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
