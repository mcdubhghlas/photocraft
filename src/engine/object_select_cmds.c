/* Object Selection (select.object): the GrabCut object finder (port of the object_selection half of
 * crates/engine/src/smartselect_cmds.rs). Two modes: a rectangle (the user drags a box around the
 * object) runs rectangle-mode GrabCut; a point (a click, or the hover preview) runs the Object
 * Finder (pc_algo_grabcut_object_at). Finding nothing leaves the selection (and history) alone, in
 * every mode, so a miss on a busy photo never wipes the user's work.
 *
 * The Object Finder is pure and cheap enough to run while the pointer hovers; the reference caches
 * its results per document (ObjectFinderCache) so a click reuses the hover's computation. The C
 * session has no such cache yet (see §8), so select.object recomputes each call: the result is
 * identical, only a repeat click costs the same as the first.
 *
 * Never crashes on hostile params (REWRITE_PLAN.md rule 9): a missing/short/non-finite rect or
 * point is a graceful error, and the algo bounds its own work. */
#include <math.h>
#include <string.h>

#include "algo/segment.h"
#include "algo/selection.h"
#include "compose/compose.h"
#include "doc/doc.h"

#include "object_select_cmds.h"

/* ---- a sampler over the active layer (or the composite) ----------------------------------- */

static pc_status obj_composite_rgba(const void *ctx, pc_rect r, float (*out)[4], size_t n, pc_error *err)
{
    const pc_document *doc = ctx;
    pc_compose_buffer buf;
    pc_status rc = pc_compose_render(doc, r, &buf, err);
    if (rc != PC_OK) {
        return rc;
    }
    for (size_t i = 0; i < n && i < buf.len; i++) {
        out[i][0] = buf.px[i][0];
        out[i][1] = buf.px[i][1];
        out[i][2] = buf.px[i][2];
        out[i][3] = buf.px[i][3];
    }
    pc_compose_buffer_fini(&buf);
    return PC_OK;
}

static pc_algo_sampler obj_doc_sampler(const pc_engine_doc_state *d, bool all_layers)
{
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (!all_layers && d->has_active_layer) {
        const pc_layer *l = pc_document_layer(doc, d->active_layer);
        const pc_surface *surf = l != NULL ? pc_layer_surface(l) : NULL;
        if (surf != NULL) {
            return pc_algo_surface_sampler(surf);
        }
    }
    pc_algo_sampler s = {obj_composite_rgba, doc};
    return s;
}

/* ---- params ------------------------------------------------------------------------------- */

static bool obj_bool(yyjson_val *p, const char *k, bool def)
{
    bool v;
    yyjson_val *x = pc_json_get(p, k);
    if (x != NULL && pc_json_as_bool(x, &v)) {
        return v;
    }
    return def;
}

static pc_algo_selection_mode obj_mode(yyjson_val *p)
{
    size_t len = 0;
    yyjson_val *m = pc_json_get(p, "mode");
    const char *s = m != NULL ? pc_json_as_str(m, &len) : NULL;
    char buf[16] = {0};
    if (s == NULL) {
        return pc_algo_selection_mode_parse("replace");
    }
    if (len == 3 && memcmp(s, "new", 3) == 0) {
        return PC_ALGO_SELECTION_REPLACE;
    }
    if (len < sizeof buf) {
        memcpy(buf, s, len);
    }
    return pc_algo_selection_mode_parse(buf);
}

/* "rect": [x, y, w, h] normalising negative sizes (dragging up/left); false when absent/short. */
static bool obj_rect_param(yyjson_val *p, pc_rect *out)
{
    yyjson_val *arr = pc_json_get(p, "rect");
    if (arr == NULL || !yyjson_is_arr(arr)) {
        return false;
    }
    double v[4];
    size_t got = 0;
    size_t i, max;
    yyjson_val *e;
    yyjson_arr_foreach(arr, i, max, e) {
        double d;
        if (got < 4 && e != NULL && pc_json_as_f64(e, &d)) {
            v[got++] = d;
        }
    }
    if (got < 4) {
        return false;
    }
    double x0 = fmin(v[0], v[0] + v[2]), x1 = fmax(v[0], v[0] + v[2]);
    double y0 = fmin(v[1], v[1] + v[3]), y1 = fmax(v[1], v[1] + v[3]);
    if (!isfinite(x0) || !isfinite(x1) || !isfinite(y0) || !isfinite(y1)) {
        return false;
    }
    const double lo = -1e8, hi = 1e8;
    x0 = floor(x0);
    y0 = floor(y0);
    x1 = ceil(x1);
    y1 = ceil(y1);
    x0 = x0 < lo ? lo : (x0 > hi ? hi : x0);
    y0 = y0 < lo ? lo : (y0 > hi ? hi : y0);
    x1 = x1 < lo ? lo : (x1 > hi ? hi : x1);
    y1 = y1 < lo ? lo : (y1 > hi ? hi : y1);
    *out = pc_rect_make((int32_t)x0, (int32_t)y0, (int32_t)x1, (int32_t)y1);
    return true;
}

/* "point": [x, y] of finite numbers; false when absent/short/non-finite. */
static bool obj_point_param(yyjson_val *p, float *x, float *y)
{
    yyjson_val *arr = pc_json_get(p, "point");
    if (arr == NULL || !yyjson_is_arr(arr)) {
        return false;
    }
    yyjson_val *vx = yyjson_arr_get(arr, 0);
    yyjson_val *vy = yyjson_arr_get(arr, 1);
    double dx, dy;
    if (vx == NULL || vy == NULL || !pc_json_as_f64(vx, &dx) || !pc_json_as_f64(vy, &dy) ||
        !isfinite(dx) || !isfinite(dy)) {
        return false;
    }
    *x = (float)dx;
    *y = (float)dy;
    return true;
}

/* ---- apply a region as the selection (Finding nothing keeps everything) -------------------- */

struct obj_apply_ctx {
    const pc_algo_region *region;
    pc_algo_selection_mode mode;
    bool out_some;
};

static pc_status obj_apply_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct obj_apply_ctx *c = vctx;
    pc_surface outs;
    bool some = false;
    pc_status rc = pc_algo_selection_combine_region(doc->has_selection ? &doc->selection : NULL,
                                                    c->region, c->mode, &outs, &some, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (doc->has_selection) {
        pc_surface_fini(&doc->selection);
        doc->has_selection = false;
    }
    if (some) {
        doc->selection = outs;
        doc->has_selection = true;
    } else {
        pc_surface_fini(&outs);
    }
    c->out_some = some;
    return PC_OK;
}

static pc_status obj_apply(pc_engine_session *s, pc_algo_region *region, pc_algo_selection_mode mode,
                           pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    bool cur = d != NULL && pc_doc_arc_get(d->doc)->has_selection;
    yyjson_mut_doc *rd = NULL;
    if (region == NULL || pc_algo_region_is_none(region)) {
        if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_bool(rd, root, "selected", cur);
        yyjson_mut_obj_add_bool(rd, root, "changed", false);
        yyjson_mut_doc_set_root(rd, root);
        return PC_OK;
    }
    pc_rect b = region->bbox;
    struct obj_apply_ctx c = {region, mode, false};
    pc_status rc = pc_engine_session_edit(s, "Object Selection", obj_apply_edit, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_bool(rd, root, "selected", c.out_some);
    yyjson_mut_obj_add_bool(rd, root, "changed", true);
    yyjson_mut_val *bb = yyjson_mut_arr(rd);
    yyjson_mut_arr_add_int(rd, bb, b.x0);
    yyjson_mut_arr_add_int(rd, bb, b.y0);
    yyjson_mut_arr_add_int(rd, bb, (int64_t)pc_rect_width(b));
    yyjson_mut_arr_add_int(rd, bb, (int64_t)pc_rect_height(b));
    yyjson_mut_obj_add_val(rd, root, "bounds", bb);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- the command ------------------------------------------------------------------------- */

static pc_status obj_has_doc(const pc_engine_session *s, pc_error *reason)
{
    return pc_engine_session_active(s) != NULL ? PC_OK
                                               : pc_fail(reason, PC_ERR_INVALID, "no document open");
}

static pc_status run_object(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_rect canvas = pc_document_bounds(doc);
    bool all_layers = obj_bool(p, "sampleAllLayers", false);
    pc_algo_sampler smp = obj_doc_sampler(d, all_layers);
    pc_algo_region region;
    memset(&region, 0, sizeof region);

    /* Point mode: the Object Finder's object under the point (unless a rect is also given). */
    float px, py;
    if (pc_json_get(p, "rect") == NULL && obj_point_param(p, &px, &py)) {
        pc_status rc = pc_algo_grabcut_object_at(&smp, canvas, px, py, PC_ALGO_OBJECT_AT_WORK_PX, &region, err);
        if (rc != PC_OK) {
            return rc;
        }
        rc = obj_apply(s, &region, obj_mode(p), out, err);
        pc_algo_region_fini(&region);
        return rc;
    }

    /* Rectangle mode. */
    pc_rect rect;
    if (!obj_rect_param(p, &rect)) {
        return pc_engine_err_bad_params(err, "select.object",
                                        "needs \"rect\": [x, y, w, h] or \"point\": [x, y]");
    }
    pc_status rc = pc_algo_grabcut_object_select(&smp, canvas, rect, 160000, &region, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = obj_apply(s, &region, obj_mode(p), out, err);
    pc_algo_region_fini(&region);
    return rc;
}

static const pc_engine_command SPECS[] = {
    {"select.object", "Object Selection", NULL, 0, NULL,
     "{\"rect\":[x,y,w,h] | \"point\":[x,y],\"mode\":\"replace|add|subtract|intersect\"=\"replace\","
     "\"sampleAllLayers\":bool=false} (point: the Object Finder's object under the point, as hovering "
     "previews it)",
     obj_has_doc, run_object, true},
};

const pc_engine_command *pc_engine_object_select_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
