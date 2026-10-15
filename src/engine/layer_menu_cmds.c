/* Layer-menu commands deferred by E5 (port of crates/engine/src/layer_menu_cmds.rs):
 *   - layer.flattenImage: composite everything onto a white Background.
 *   - layer.layerMask.apply: multiply the pixels' alpha by the (feather-aware) mask, delete it.
 *   - layer.layerMask.fromTransparency: the alpha becomes a layer mask, the pixels become opaque.
 *   - layer.layerMask.hideSelection: a mask that hides the selected area.
 *   - layer.smartObjects.stackMode.*: the image-stack statistic a smart object renders its
 *     source layers with (the combine runs in the smart-object re-render, pc_engine_smart_refresh).
 *
 * Never crashes on any document/selection/params state (REWRITE_PLAN.md rule 9): bounds are
 * checked, allocations are capped by pc_malloc_array, and pixel work routes through the shared
 * surface and compose primitives. */
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "base/mem.h"
#include "compose/compose.h"
#include "compose/masks.h"
#include "doc/doc.h"
#include "doc/mode.h"
#include "doc/vector.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#include "brush_cmds.h" /* pc_engine_paint_surface */
#include "engine_internal.h"
#include "extra_cmds.h" /* pc_engine_background_to_layer_for_mask */
#include "layer_cmds.h" /* pc_engine_layer_param, pc_engine_selected */
#include "smart_cmds.h" /* pc_engine_smart_refresh */

/* ---- shared pixel helpers (layer_menu_cmds::read_rgba / write_rgba) ------------------------ */

/* Encode `count` straight-RGBA pixels back into `surf` over `r`. */
static pc_status write_rgba(pc_surface *surf, pc_rect r, const float (*px)[4], size_t count, pc_error *err)
{
    pc_pixel_format fmt = pc_surface_format(surf);
    size_t n = pc_surface_channels(surf);
    if (count == 0 || n == 0) {
        return PC_OK;
    }
    float *buf = pc_malloc_array(count, n * sizeof *buf);
    if (buf == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "write_rgba");
    }
    for (size_t i = 0; i < count; i++) {
        float enc[PC_RASTER_MAX_CHANNELS];
        (void)pc_from_rgba(fmt, px[i], enc);
        memcpy(buf + i * n, enc, n * sizeof *buf);
    }
    pc_status rc = pc_surface_write_region(surf, r, buf, count * n, err);
    free(buf);
    return rc;
}

/* ---- enabled predicates -------------------------------------------------------------------- */

static pc_status has_raster(const pc_engine_session *s, pc_error *reason)
{
    const pc_layer *l = pc_engine_active_layer_of(s, reason);
    if (l == NULL) {
        return reason != NULL ? reason->code : PC_ERR_INVALID;
    }
    if (l->content.kind == PC_LAYER_RASTER) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "active layer is %s %s layer, not a pixel layer",
                   pc_layer_content_article(&l->content), pc_layer_content_kind_name(&l->content));
}

static pc_status has_raster_with_mask(const pc_engine_session *s, pc_error *reason)
{
    pc_status rc = has_raster(s, reason);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_layer *l = pc_engine_active_layer_of(s, reason);
    if (l != NULL && l->has_mask) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "the layer has no layer mask");
}

static pc_status has_selection_layer(const pc_engine_session *s, pc_error *reason)
{
    pc_status rc = pc_engine_has_layer(s, reason);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d != NULL && pc_doc_arc_get(d->doc)->has_selection) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "no selection");
}

static pc_status has_smart(const pc_engine_session *s, pc_error *reason)
{
    const pc_layer *l = pc_engine_active_layer_of(s, reason);
    if (l == NULL) {
        return reason != NULL ? reason->code : PC_ERR_INVALID;
    }
    if (l->content.kind == PC_LAYER_SMART) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "the active layer is not a smart object");
}

/* ---- layer.flattenImage -------------------------------------------------------------------- */

static pc_status edit_flatten(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                              pc_error *err)
{
    (void)vctx;
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    pc_layer bg;
    if (pc_layer_raster(&bg, "Background", fmt, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    bg.locks.transparency = true;
    bg.locks.position = true;
    /* flatten_to_surface(doc, fmt, Some([1,1,1])): composite the whole canvas over white, so empty
     * regions become opaque white (not pc_engine_composite_layers, which covers only content). */
    const float white[3] = {1.0f, 1.0f, 1.0f};
    pc_surface comp;
    pc_status rc = pc_compose_composite_to_surface(doc, pc_document_bounds(doc), fmt, white, &comp, err);
    if (rc != PC_OK) {
        pc_layer_fini(&bg);
        return rc;
    }
    pc_surface *s = pc_layer_surface_mut(&bg);
    if (s == NULL) {
        pc_surface_fini(&comp);
        pc_layer_fini(&bg);
        return pc_fail(err, PC_ERR_INVALID, "flatten: no raster surface");
    }
    pc_surface_fini(s);
    *s = comp;
    for (size_t i = 0; i < doc->nlayers; i++) {
        pc_layer_fini(&doc->layers[i]);
    }
    doc->layers[0] = bg;
    doc->nlayers = 1;
    *active = bg.id;
    *has_active = true;
    return PC_OK;
}

static pc_status run_flatten(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    if (pc_engine_session_edit(s, "Flatten Image", edit_flatten, NULL, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return pc_engine_result_null(out, err);
}

/* ---- layer.layerMask.apply ----------------------------------------------------------------- */

struct mask_ctx {
    pc_layer_id id;
};

static pc_status edit_apply_mask(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct mask_ctx *c = vctx;
    const pc_layer *l0 = pc_document_layer(doc, c->id);
    if (l0 == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    if (!l0->has_mask) {
        return pc_engine_err_other(err, "the layer has no layer mask");
    }
    /* Bake the softened coverage the compositor shows for a feathered mask (values_into accounts
     * for density but not feather). Compute it before the mask is removed; do not bake an
     * independent vector mask that stays attached after Apply. */
    bool feathered = false;
    pc_surface softened;
    if (l0->mask.enabled && pc_compose_feather_sigma(l0->mask.feather) > 0.0f) {
        pc_layer clone;
        if (pc_layer_clone(l0, &clone, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        if (clone.has_vector_mask) {
            pc_vector_mask_fini(&clone.vector_mask);
            clone.has_vector_mask = false;
        }
        pc_rect area = pc_document_bounds(doc);
        const pc_surface *csurf = pc_layer_surface(&clone);
        if (csurf != NULL) {
            area = pc_rect_union(area, pc_surface_content_bounds(csurf));
        }
        bool found = false;
        pc_status rc = pc_compose_combined_mask(&clone, area, &found, &softened, err);
        pc_layer_fini(&clone);
        if (rc != PC_OK) {
            return rc;
        }
        feathered = found;
    }

    /* Take the mask out of the layer (keeping a copy for values_into). */
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL) {
        if (feathered) {
            pc_surface_fini(&softened);
        }
        return pc_engine_err_no_layer(err, c->id);
    }
    pc_layer_mask taken;
    pc_status rc = pc_layer_mask_clone(&l->mask, &taken, err);
    if (rc != PC_OK) {
        if (feathered) {
            pc_surface_fini(&softened);
        }
        return rc;
    }
    pc_layer_mask_fini(&l->mask);
    l->has_mask = false;

    pc_surface *surf = NULL;
    rc = pc_engine_paint_surface(doc, c->id, NULL, &surf, err);
    if (rc != PC_OK) {
        goto done;
    }
    pc_rect r = pc_surface_content_bounds(surf);
    if (!pc_rect_is_empty(r)) {
        uint32_t w = pc_rect_width(r), h = pc_rect_height(r);
        size_t count = (size_t)w * (size_t)h;
        float(*px)[4] = pc_malloc_array(count, sizeof *px);
        float *m = pc_malloc_array(count, sizeof *m);
        if (px == NULL || m == NULL) {
            free(px);
            free(m);
            rc = pc_fail(err, PC_ERR_NOMEM, "apply mask");
            goto done;
        }
        if (pc_surface_read_rgba_into(surf, r, px, count, err) != PC_OK) {
            free(px);
            free(m);
            rc = err != NULL ? err->code : PC_ERR_NOMEM;
            goto done;
        }
        if (feathered) {
            rc = pc_surface_read_region_into(&softened, r, m, count, err);
        } else {
            rc = pc_layer_mask_values_into(&taken, r, m, count, err);
        }
        if (rc != PC_OK) {
            free(px);
            free(m);
            goto done;
        }
        for (size_t i = 0; i < count; i++) {
            px[i][3] *= m[i];
        }
        rc = write_rgba(surf, r, (const float (*)[4])px, count, err);
        free(px);
        free(m);
        if (rc != PC_OK) {
            goto done;
        }
        pc_surface_prune(surf);
    }
    rc = PC_OK;
done:
    pc_layer_mask_fini(&taken);
    if (feathered) {
        pc_surface_fini(&softened);
    }
    return rc;
}

static pc_status run_apply_mask(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    struct mask_ctx c;
    if (pc_engine_layer_param(s, p, &c.id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (pc_engine_session_edit(s, "Apply Layer Mask", edit_apply_mask, &c, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return pc_engine_result_null(out, err);
}

/* ---- layer.layerMask.fromTransparency ------------------------------------------------------ */

static pc_status edit_from_transparency(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx,
                                        pc_error *err)
{
    (void)ha;
    (void)a;
    struct mask_ctx *c = vctx;
    pc_surface *surf = NULL;
    pc_status rc = pc_engine_paint_surface(doc, c->id, NULL, &surf, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_layer_mask mask;
    pc_layer_mask_hide_all(&mask);
    pc_rect r = pc_surface_content_bounds(surf);
    if (!pc_rect_is_empty(r)) {
        uint32_t w = pc_rect_width(r), h = pc_rect_height(r);
        size_t count = (size_t)w * (size_t)h;
        float(*px)[4] = pc_malloc_array(count, sizeof *px);
        float *alpha = pc_malloc_array(count, sizeof *alpha);
        if (px == NULL || alpha == NULL) {
            free(px);
            free(alpha);
            pc_layer_mask_fini(&mask);
            return pc_fail(err, PC_ERR_NOMEM, "from transparency");
        }
        if (pc_surface_read_rgba_into(surf, r, px, count, err) != PC_OK) {
            free(px);
            free(alpha);
            pc_layer_mask_fini(&mask);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        for (size_t i = 0; i < count; i++) {
            alpha[i] = px[i][3];
            px[i][3] = 1.0f;
        }
        rc = pc_surface_write_region(&mask.surface, r, alpha, count, err);
        if (rc == PC_OK) {
            pc_surface_prune(&mask.surface);
            rc = write_rgba(surf, r, (const float (*)[4])px, count, err);
        }
        free(px);
        free(alpha);
        if (rc != PC_OK) {
            pc_layer_mask_fini(&mask);
            return rc;
        }
    }
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL) {
        pc_layer_mask_fini(&mask);
        return pc_engine_err_no_layer(err, c->id);
    }
    if (l->has_mask) {
        pc_layer_mask_fini(&l->mask);
    }
    l->mask = mask;
    l->has_mask = true;
    return PC_OK;
}

static pc_status run_from_transparency(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                       pc_error *err)
{
    struct mask_ctx c;
    if (pc_engine_layer_param(s, p, &c.id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (pc_engine_session_edit(s, "From Transparency", edit_from_transparency, &c, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return pc_engine_result_null(out, err);
}

/* ---- layer.layerMask.hideSelection --------------------------------------------------------- */

/* targets(): the explicit `layer`, else the selected layers, else the active layer. */
static pc_status targets(pc_engine_session *s, yyjson_val *p, pc_layer_id **out, size_t *n, pc_error *err)
{
    *out = NULL;
    *n = 0;
    bool present = false;
    pc_layer_id id;
    if (pc_engine_explicit_layer(s, p, &present, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (!present) {
        pc_layer_id *sel = NULL;
        size_t nsel = 0;
        if (pc_engine_selected(s, &sel, &nsel, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        if (nsel > 0) {
            *out = sel;
            *n = nsel;
            return PC_OK;
        }
        free(sel);
        if (pc_engine_layer_param(s, p, &id, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
    }
    pc_layer_id *one = pc_malloc_array(1, sizeof *one);
    if (one == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "targets");
    }
    one[0] = id;
    *out = one;
    *n = 1;
    return PC_OK;
}

/* mask_targets(): targets, but with two or more selected only those that can take a mask. */
static pc_status mask_targets(pc_engine_session *s, yyjson_val *p, pc_layer_id **out, size_t *n,
                              pc_error *err)
{
    pc_layer_id *ids = NULL;
    size_t nids = 0;
    if (targets(s, p, &ids, &nids, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (nids < 2) {
        *out = ids;
        *n = nids;
        return PC_OK;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        free(ids);
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    size_t keep = 0;
    for (size_t i = 0; i < nids; i++) {
        const pc_layer *l = pc_document_layer(doc, ids[i]);
        if (l != NULL && !l->has_mask && !pc_document_is_background(doc, ids[i])) {
            ids[keep++] = ids[i];
        }
    }
    *out = ids;
    *n = keep;
    return PC_OK;
}

struct hide_ctx {
    const pc_layer_id *ids;
    size_t n;
};

static pc_status edit_hide_selection(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct hide_ctx *c = vctx;
    if (!doc->has_selection) {
        return pc_engine_err_other(err, "no selection");
    }
    pc_rect bounds = pc_document_bounds(doc);
    uint32_t w = pc_rect_width(bounds), h = pc_rect_height(bounds);
    size_t count = (size_t)w * (size_t)h;
    float *v = NULL;
    size_t len = 0;
    if (pc_surface_read_region(&doc->selection, bounds, &v, &len, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    for (size_t i = 0; i < len; i++) {
        v[i] = 1.0f - v[i];
    }
    pc_layer_mask mask;
    pc_layer_mask_reveal_all(&mask);
    pc_status rc = PC_OK;
    if (count > 0) {
        rc = pc_surface_write_region(&mask.surface, bounds, v, len, err);
        if (rc == PC_OK) {
            pc_surface_prune(&mask.surface);
        }
    }
    free(v);
    if (rc != PC_OK) {
        pc_layer_mask_fini(&mask);
        return rc;
    }
    for (size_t i = 0; i < c->n; i++) {
        pc_engine_background_to_layer_for_mask(doc, c->ids[i]);
        pc_layer *l = pc_document_layer_mut(doc, c->ids[i]);
        if (l == NULL) {
            pc_layer_mask_fini(&mask);
            return pc_engine_err_no_layer(err, c->ids[i]);
        }
        pc_layer_mask copy;
        rc = pc_layer_mask_clone(&mask, &copy, err);
        if (rc != PC_OK) {
            pc_layer_mask_fini(&mask);
            return rc;
        }
        if (l->has_mask) {
            pc_layer_mask_fini(&l->mask);
        }
        l->mask = copy;
        l->has_mask = true;
    }
    pc_layer_mask_fini(&mask);
    pc_surface_fini(&doc->selection);
    doc->has_selection = false;
    return PC_OK;
}

static pc_status run_hide_selection(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_layer_id *ids = NULL;
    size_t n = 0;
    if (mask_targets(s, p, &ids, &n, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    struct hide_ctx c = {ids, n};
    pc_status rc = pc_engine_session_edit(s, "Add Layer Mask", edit_hide_selection, &c, err);
    free(ids);
    if (rc != PC_OK) {
        return rc;
    }
    return pc_engine_result_null(out, err);
}

/* ---- layer.smartObjects.stackMode.* -------------------------------------------------------- */

struct stack_ctx {
    pc_layer_id id;
    bool has_mode;
    pc_stack_mode mode;
};

static pc_status edit_stack_mode(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct stack_ctx *c = vctx;
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    if (l->content.kind != PC_LAYER_SMART) {
        return pc_engine_err_other(err, "the active layer is not a smart object");
    }
    pc_smart_object *sm = &l->content.u.smart;
    sm->has_stack_mode = c->has_mode;
    if (c->has_mode) {
        sm->stack_mode = c->mode;
    }
    bool updated = false;
    return pc_engine_smart_refresh(doc, c->id, &updated, err);
}

static pc_status stack_mode_run(pc_engine_session *s, yyjson_val *p, bool has_mode, pc_stack_mode mode,
                                pc_json_value *out, pc_error *err)
{
    struct stack_ctx c;
    if (pc_engine_layer_param(s, p, &c.id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    c.has_mode = has_mode;
    c.mode = mode;
    char label[64];
    (void)snprintf(label, sizeof label, "Stack Mode: %s",
                   has_mode ? pc_stack_mode_label(mode) : "None");
    if (pc_engine_session_edit(s, label, edit_stack_mode, &c, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    if (has_mode) {
        yyjson_mut_obj_add_strcpy(rd, root, "stackMode", pc_stack_mode_id(mode));
    } else {
        yyjson_mut_obj_add_null(rd, root, "stackMode");
    }
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

#define STACK_RUN(fn, HAS, MODE)                                                                     \
    static pc_status fn(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)       \
    {                                                                                                \
        return stack_mode_run(s, p, HAS, MODE, out, err);                                             \
    }

STACK_RUN(run_sm_entropy, true, PC_STACK_ENTROPY)
STACK_RUN(run_sm_kurtosis, true, PC_STACK_KURTOSIS)
STACK_RUN(run_sm_maximum, true, PC_STACK_MAXIMUM)
STACK_RUN(run_sm_mean, true, PC_STACK_MEAN)
STACK_RUN(run_sm_median, true, PC_STACK_MEDIAN)
STACK_RUN(run_sm_minimum, true, PC_STACK_MINIMUM)
STACK_RUN(run_sm_range, true, PC_STACK_RANGE)
STACK_RUN(run_sm_skewness, true, PC_STACK_SKEWNESS)
STACK_RUN(run_sm_stddev, true, PC_STACK_STANDARD_DEVIATION)
STACK_RUN(run_sm_summation, true, PC_STACK_SUMMATION)
STACK_RUN(run_sm_variance, true, PC_STACK_VARIANCE)
STACK_RUN(run_sm_none, false, PC_STACK_ENTROPY)

/* ---- specs --------------------------------------------------------------------------------- */

static const char *const MENU_LAYER[] = {"Layer"};
static const char *const MENU_LAYER_MASK[] = {"Layer", "Layer Mask"};
static const char *const MENU_STACK[] = {"Layer", "Smart Objects", "Stack Mode"};

#define LAYER_PARAM "{\"layer\":id?}"

static const pc_engine_command SPECS[] = {
    {"layer.flattenImage", "Flatten Image", MENU_LAYER, 1, NULL, "{}", pc_engine_has_doc, run_flatten,
     true},
    {"layer.layerMask.apply", "Apply", MENU_LAYER_MASK, 2, NULL, LAYER_PARAM, has_raster_with_mask,
     run_apply_mask, true},
    {"layer.layerMask.fromTransparency", "From Transparency", MENU_LAYER_MASK, 2, NULL, LAYER_PARAM,
     has_raster, run_from_transparency, true},
    {"layer.layerMask.hideSelection", "Hide Selection", MENU_LAYER_MASK, 2, NULL,
     "{\"layer\":id?} (no layer: every selected layer that can take a mask)", has_selection_layer,
     run_hide_selection, true},
    {"layer.smartObjects.stackMode.entropy", "Entropy", MENU_STACK, 3, NULL, LAYER_PARAM, has_smart,
     run_sm_entropy, true},
    {"layer.smartObjects.stackMode.kurtosis", "Kurtosis", MENU_STACK, 3, NULL, LAYER_PARAM, has_smart,
     run_sm_kurtosis, true},
    {"layer.smartObjects.stackMode.maximum", "Maximum", MENU_STACK, 3, NULL, LAYER_PARAM, has_smart,
     run_sm_maximum, true},
    {"layer.smartObjects.stackMode.mean", "Mean", MENU_STACK, 3, NULL, LAYER_PARAM, has_smart,
     run_sm_mean, true},
    {"layer.smartObjects.stackMode.median", "Median", MENU_STACK, 3, NULL, LAYER_PARAM, has_smart,
     run_sm_median, true},
    {"layer.smartObjects.stackMode.minimum", "Minimum", MENU_STACK, 3, NULL, LAYER_PARAM, has_smart,
     run_sm_minimum, true},
    {"layer.smartObjects.stackMode.range", "Range", MENU_STACK, 3, NULL, LAYER_PARAM, has_smart,
     run_sm_range, true},
    {"layer.smartObjects.stackMode.skewness", "Skewness", MENU_STACK, 3, NULL, LAYER_PARAM, has_smart,
     run_sm_skewness, true},
    {"layer.smartObjects.stackMode.standardDeviation", "Standard Deviation", MENU_STACK, 3, NULL,
     LAYER_PARAM, has_smart, run_sm_stddev, true},
    {"layer.smartObjects.stackMode.summation", "Summation", MENU_STACK, 3, NULL, LAYER_PARAM, has_smart,
     run_sm_summation, true},
    {"layer.smartObjects.stackMode.variance", "Variance", MENU_STACK, 3, NULL, LAYER_PARAM, has_smart,
     run_sm_variance, true},
    {"layer.smartObjects.stackMode.none", "None", MENU_STACK, 3, NULL, LAYER_PARAM, has_smart,
     run_sm_none, true},
};

const pc_engine_command *pc_engine_layer_menu_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
