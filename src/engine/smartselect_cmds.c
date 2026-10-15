/* Smart selection (port of crates/engine/src/smartselect_cmds.rs, E7): Quick Selection, Select
 * Subject, Select and Mask (Refine Edge) and Focus Area. The algorithms live in
 * photocraft_algo::{segment,matting}; these commands pick the pixels to analyse and store the
 * result.
 *
 * Object Selection (select.object) needs the algo Object Finder (grabcut::object_at) and the
 * session Object-Finder cache, neither ported yet; it is deferred (see §8). The subject/object
 * result caches (same-pixels keyed) are likewise deferred: Select Subject recomputes each run,
 * which gives the identical selection. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/matting.h"
#include "algo/segment.h"
#include "algo/selection.h"
#include "base/blob.h"
#include "compose/compose.h"
#include "doc/doc.h"
#include "doc/mode.h"
#include "raster/surface.h"

#include "selection_cmds.h"

/* ---- helpers ----------------------------------------------------------------------------- */

static pc_status composite_rgba(const void *ctx, pc_rect r, float (*out)[4], size_t n, pc_error *err)
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

/* A sampler over the active layer (all_layers false and it has pixels) or the composite. */
static pc_algo_sampler doc_sampler(const pc_engine_doc_state *d, bool all_layers)
{
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (!all_layers && d->has_active_layer) {
        const pc_layer *l = pc_document_layer(doc, d->active_layer);
        const pc_surface *surf = l != NULL ? pc_layer_surface(l) : NULL;
        if (surf != NULL) {
            return pc_algo_surface_sampler(surf);
        }
    }
    pc_algo_sampler s = {composite_rgba, doc};
    return s;
}

static float pf(yyjson_val *p, const char *k, float def)
{
    double v;
    yyjson_val *x = pc_json_get(p, k);
    if (x != NULL && pc_json_as_f64(x, &v)) {
        return (float)v;
    }
    return def;
}

static bool pb(yyjson_val *p, const char *k, bool def)
{
    bool v;
    yyjson_val *x = pc_json_get(p, k);
    if (x != NULL && pc_json_as_bool(x, &v)) {
        return v;
    }
    return def;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static pc_algo_selection_mode mode_or(yyjson_val *p, const char *def)
{
    size_t len = 0;
    yyjson_val *m = pc_json_get(p, "mode");
    const char *s = m != NULL ? pc_json_as_str(m, &len) : NULL;
    char buf[16] = {0};
    if (s == NULL) {
        return pc_algo_selection_mode_parse(def);
    }
    if (len == 3 && memcmp(s, "new", 3) == 0) {
        return PC_ALGO_SELECTION_REPLACE;
    }
    if (len < sizeof buf) {
        memcpy(buf, s, len);
    }
    return pc_algo_selection_mode_parse(buf);
}

static pc_status has_doc(const pc_engine_session *s, pc_error *reason)
{
    return pc_engine_session_active(s) != NULL ? PC_OK
                                               : pc_fail(reason, PC_ERR_INVALID, "no document open");
}

static pc_status has_selection_or_mask(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (doc->has_selection) {
        return PC_OK;
    }
    if (d->has_active_layer) {
        const pc_layer *l = pc_document_layer(doc, d->active_layer);
        if (l != NULL && l->has_mask) {
            return PC_OK;
        }
    }
    return pc_fail(reason, PC_ERR_INVALID, "no selection or layer mask");
}

/* Point array from "points". *out owned (free()). */
static float (*parse_points(yyjson_val *p, size_t *n))[2]
{
    *n = 0;
    yyjson_val *arr = pc_json_get(p, "points");
    if (arr == NULL || !yyjson_is_arr(arr)) {
        return NULL;
    }
    size_t cap = yyjson_arr_size(arr);
    float (*pts)[2] = malloc(cap ? cap * sizeof(*pts) : sizeof(*pts));
    if (pts == NULL) {
        return NULL;
    }
    size_t i, max, k = 0;
    yyjson_val *v;
    yyjson_arr_foreach(arr, i, max, v) {
        double x, y;
        yyjson_val *vx = (v != NULL && yyjson_is_arr(v)) ? yyjson_arr_get(v, 0) : NULL;
        yyjson_val *vy = vx != NULL ? yyjson_arr_get(v, 1) : NULL;
        if (vx != NULL && vy != NULL && pc_json_as_f64(vx, &x) && pc_json_as_f64(vy, &y)) {
            pts[k][0] = (float)x;
            pts[k][1] = (float)y;
            k++;
        }
    }
    *n = k;
    return pts;
}

/* ---- apply a region as the selection ----------------------------------------------------- */

struct apply_ctx {
    const pc_algo_region *region;
    pc_algo_selection_mode mode;
    bool out_some;
};

static pc_status apply_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct apply_ctx *c = vctx;
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

/* Store `region` (may be None) combined with the selection; None keeps the selection unchanged. */
static pc_status apply_region(pc_engine_session *s, const char *label, pc_algo_region *region,
                             pc_algo_selection_mode mode, pc_json_value *out, pc_error *err)
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
    struct apply_ctx c = {region, mode, false};
    pc_status rc = pc_engine_session_edit(s, label, apply_edit, &c, err);
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

/* ---- Quick Selection --------------------------------------------------------------------- */

static const pc_algo_refine_params ENHANCE = {4.0f, true, 15.0f, 0.0f, 25.0f, 0.0f};

static pc_status run_quick(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    size_t npts = 0;
    float (*pts)[2] = parse_points(p, &npts);
    if (npts == 0) {
        free(pts);
        return pc_engine_err_bad_params(err, "select.quick", "needs at least one point");
    }
    float size = clampf(pf(p, "size", 30.0f), 1.0f, 5000.0f);
    size_t mlen = 0;
    yyjson_val *mv = pc_json_get(p, "mode");
    const char *ms = mv != NULL ? pc_json_as_str(mv, &mlen) : NULL;
    pc_algo_selection_mode mode = PC_ALGO_SELECTION_ADD;
    if (ms != NULL && mlen == 8 && memcmp(ms, "subtract", 8) == 0) {
        mode = PC_ALGO_SELECTION_SUBTRACT;
    } else if (ms != NULL && ((mlen == 7 && memcmp(ms, "replace", 7) == 0) ||
                              (mlen == 3 && memcmp(ms, "new", 3) == 0))) {
        mode = PC_ALGO_SELECTION_REPLACE;
    }
    bool enhance = pb(p, "enhanceEdge", false);
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        free(pts);
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_rect canvas = pc_document_bounds(doc);
    pc_algo_sampler smp = doc_sampler(d, pb(p, "sampleAllLayers", false));
    pc_algo_region region;
    pc_status rc = pc_algo_quick_select(&smp, canvas, (const float (*)[2])pts, npts, size, &region, err);
    free(pts);
    if (rc != PC_OK) {
        return rc;
    }
    if (enhance && !pc_algo_region_is_none(&region)) {
        pc_algo_region refined;
        rc = pc_algo_matting_refine_mask(&smp, pc_algo_matting_region_reader, &region, region.bbox,
                                         canvas, &ENHANCE, &refined, err);
        if (rc == PC_OK) {
            pc_algo_region_fini(&region);
            region = refined;
        }
    }
    rc = apply_region(s, "Quick Selection", &region, mode, out, err);
    pc_algo_region_fini(&region);
    return rc;
}

/* ---- Select Subject ---------------------------------------------------------------------- */

static pc_status run_subject(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_algo_sampler smp = doc_sampler(d, pb(p, "sampleAllLayers", true));
    pc_algo_region region;
    pc_status rc = pc_algo_subject_select(&smp, pc_document_bounds(doc), &region, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = apply_region(s, "Select Subject", &region, mode_or(p, "replace"), out, err);
    pc_algo_region_fini(&region);
    return rc;
}

/* ---- Focus Area -------------------------------------------------------------------------- */

static pc_status run_focus(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    float range = clampf(pf(p, "range", 0.5f), 0.0f, 1.0f);
    float noise = clampf(pf(p, "noise", 0.0f), 0.0f, 1.0f);
    pc_algo_sampler smp = doc_sampler(d, pb(p, "sampleAllLayers", true));
    pc_algo_region region;
    pc_status rc = pc_algo_focus_area(&smp, pc_document_bounds(doc), range, noise, &region, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = apply_region(s, "Focus Area", &region, mode_or(p, "replace"), out, err);
    pc_algo_region_fini(&region);
    return rc;
}

/* ---- Refine Edge / Select and Mask ------------------------------------------------------- */

#define MAX_STROKES 4096u
#define MAX_STROKE_POINTS 100000u

static pc_algo_refine_params refine_params(yyjson_val *p)
{
    pc_algo_refine_params r;
    r.radius = clampf(pf(p, "radius", 0.0f), 0.0f, 250.0f);
    r.smart_radius = pb(p, "smartRadius", false);
    r.smooth = clampf(pf(p, "smooth", 0.0f), 0.0f, 100.0f);
    r.feather = clampf(pf(p, "feather", 0.0f), 0.0f, 1000.0f);
    r.contrast = clampf(pf(p, "contrast", 0.0f), 0.0f, 100.0f);
    r.shift_edge = clampf(pf(p, "shiftEdge", 0.0f), -100.0f, 100.0f);
    return r;
}

/* Parsed Refine Edge Brush strokes (owning the point arrays). */
struct strokes {
    pc_algo_edge_stroke *list;
    size_t n;
    float (**pts)[2]; /* per-stroke point arrays to free */
};

static void strokes_fini(struct strokes *s)
{
    for (size_t i = 0; i < s->n; i++) {
        free(s->pts[i]);
    }
    free(s->pts);
    free(s->list);
    s->list = NULL;
    s->pts = NULL;
    s->n = 0;
}

static pc_status parse_strokes(yyjson_val *p, struct strokes *out, pc_error *err)
{
    const char *cmd = "select.refineEdge";
    out->list = NULL;
    out->pts = NULL;
    out->n = 0;
    yyjson_val *v = pc_json_get(p, "refineStrokes");
    if (v == NULL || yyjson_is_null(v)) {
        return PC_OK;
    }
    if (!yyjson_is_arr(v)) {
        return pc_engine_err_bad_params(err, cmd, "refineStrokes must be an array of strokes");
    }
    size_t count = yyjson_arr_size(v);
    if (count > MAX_STROKES) {
        return pc_engine_err_bad_params(err, cmd, "refineStrokes holds more than %u strokes", MAX_STROKES);
    }
    out->list = calloc(count ? count : 1, sizeof(pc_algo_edge_stroke));
    out->pts = calloc(count ? count : 1, sizeof(float (*)[2]));
    if (out->list == NULL || out->pts == NULL) {
        strokes_fini(out);
        return pc_fail(err, PC_ERR_NOMEM, "strokes");
    }
    size_t total = 0;
    size_t i, max;
    yyjson_val *stroke;
    yyjson_arr_foreach(v, i, max, stroke) {
        if (!yyjson_is_obj(stroke)) {
            strokes_fini(out);
            return pc_engine_err_bad_params(
                err, cmd, "each refineStrokes entry must be {\"points\":[[x,y],…],\"size\":px,\"mode\":"
                          "\"add|subtract\"}");
        }
        yyjson_val *pa = yyjson_obj_get(stroke, "points");
        size_t np = (pa != NULL && yyjson_is_arr(pa)) ? yyjson_arr_size(pa) : 0;
        if (np == 0) {
            strokes_fini(out);
            return pc_engine_err_bad_params(err, cmd, "each stroke needs a non-empty points array");
        }
        total += np;
        if (total > MAX_STROKE_POINTS) {
            strokes_fini(out);
            return pc_engine_err_bad_params(err, cmd, "refineStrokes holds more than %u points",
                                            MAX_STROKE_POINTS);
        }
        float (*pts)[2] = malloc(np * sizeof(*pts));
        if (pts == NULL) {
            strokes_fini(out);
            return pc_fail(err, PC_ERR_NOMEM, "stroke");
        }
        out->pts[out->n] = pts;
        size_t j, jmax, k = 0;
        yyjson_val *pt;
        yyjson_arr_foreach(pa, j, jmax, pt) {
            double x, y;
            yyjson_val *vx = (pt != NULL && yyjson_is_arr(pt) && yyjson_arr_size(pt) == 2)
                                 ? yyjson_arr_get(pt, 0)
                                 : NULL;
            yyjson_val *vy = vx != NULL ? yyjson_arr_get(pt, 1) : NULL;
            if (vx == NULL || vy == NULL || !pc_json_as_f64(vx, &x) || !pc_json_as_f64(vy, &y) ||
                !isfinite(x) || !isfinite(y) || fabs(x) > 1e7 || fabs(y) > 1e7) {
                out->n++; /* so strokes_fini frees this pts array */
                strokes_fini(out);
                return pc_engine_err_bad_params(err, cmd,
                                                "stroke points must be [x,y] pairs of finite numbers");
            }
            pts[k][0] = (float)x;
            pts[k][1] = (float)y;
            k++;
        }
        float size = 30.0f;
        yyjson_val *sv = yyjson_obj_get(stroke, "size");
        if (sv != NULL) {
            double sz;
            if (!pc_json_as_f64(sv, &sz) || !isfinite(sz) ||
                sz < PC_ALGO_EDGE_BRUSH_SIZE_MIN || sz > PC_ALGO_EDGE_BRUSH_SIZE_MAX) {
                out->n++;
                strokes_fini(out);
                return pc_engine_err_bad_params(err, cmd, "stroke size must be %g..%g px",
                                                (double)PC_ALGO_EDGE_BRUSH_SIZE_MIN,
                                                (double)PC_ALGO_EDGE_BRUSH_SIZE_MAX);
            }
            size = (float)sz;
        }
        bool add = true;
        yyjson_val *mv = yyjson_obj_get(stroke, "mode");
        if (mv != NULL) {
            size_t ml = 0;
            const char *ms = pc_json_as_str(mv, &ml);
            if (ms != NULL && ml == 3 && memcmp(ms, "add", 3) == 0) {
                add = true;
            } else if (ms != NULL && ml == 8 && memcmp(ms, "subtract", 8) == 0) {
                add = false;
            } else {
                out->n++;
                strokes_fini(out);
                return pc_engine_err_bad_params(err, cmd, "stroke mode must be add|subtract");
            }
        }
        out->list[out->n].points = (const float (*)[2])pts;
        out->list[out->n].npoints = np;
        out->list[out->n].size = size;
        out->list[out->n].add = add;
        out->n++;
    }
    if (pc_algo_edge_brush_swept_area(out->list, out->n) > PC_ALGO_EDGE_BRUSH_MAX_SWEPT_AREA) {
        strokes_fini(out);
        return pc_engine_err_bad_params(err, cmd, "refineStrokes paint too large an area");
    }
    return PC_OK;
}

/* Build a raster layer owning `surf` (moved) with an optional mask surface `mask` (moved). */
static pc_status raster_layer(const char *name, pc_surface *surf, pc_surface *mask, pc_layer *out,
                             pc_error *err)
{
    pc_layer_content content;
    memset(&content, 0, sizeof content);
    content.kind = PC_LAYER_RASTER;
    content.u.raster = *surf;
    memset(surf, 0, sizeof *surf);
    pc_status rc = pc_layer_init(out, name, &content, err);
    if (rc != PC_OK) {
        pc_layer_content_fini(&content);
        return rc;
    }
    if (mask != NULL) {
        pc_layer_mask_reveal_all(&out->mask);
        pc_surface_fini(&out->mask.surface);
        out->mask.surface = *mask;
        memset(mask, 0, sizeof *mask);
        out->has_mask = true;
    }
    return PC_OK;
}

/* out initialised to the region's coverage surface (GRAY8 default 0 for None). */
static pc_status region_to_surface(const pc_algo_region *region, pc_surface *out, pc_error *err)
{
    if (region == NULL || pc_algo_region_is_none(region)) {
        pc_surface_init(out, PC_PIXEL_FORMAT_GRAY8);
        return PC_OK;
    }
    return pc_algo_matting_region_surface(region, out, err);
}

struct refine_sel_ctx {
    const pc_algo_region *region;
    bool out_some;
};

static pc_status refine_sel_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct refine_sel_ctx *c = vctx;
    if (doc->has_selection) {
        pc_surface_fini(&doc->selection);
        doc->has_selection = false;
    }
    if (c->region != NULL && !pc_algo_region_is_none(c->region)) {
        pc_surface surf;
        pc_status rc = pc_algo_matting_region_surface(c->region, &surf, err);
        if (rc != PC_OK) {
            return rc;
        }
        doc->selection = surf;
        doc->has_selection = true;
        c->out_some = true;
    } else {
        c->out_some = false;
    }
    return PC_OK;
}

struct refine_mask_ctx {
    pc_layer_id layer;
    const pc_algo_region *region;
};

static pc_status refine_mask_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct refine_mask_ctx *c = vctx;
    pc_layer *l = pc_document_layer_mut(doc, c->layer);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, c->layer);
    }
    pc_surface surf;
    pc_status rc = region_to_surface(c->region, &surf, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (l->has_mask) {
        pc_layer_mask_fini(&l->mask);
    }
    pc_layer_mask_reveal_all(&l->mask);
    pc_surface_fini(&l->mask.surface);
    l->mask.surface = surf;
    l->has_mask = true;
    if (doc->has_selection) {
        pc_surface_fini(&doc->selection);
        doc->has_selection = false;
    }
    return PC_OK;
}

struct refine_new_ctx {
    pc_layer_id src;
    pc_layer layer; /* moved into the document */
    pc_layer_id out_id;
};

static pc_status refine_new_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx,
                                 pc_error *err)
{
    struct refine_new_ctx *c = vctx;
    pc_layer_id nid;
    pc_status rc = pc_document_insert_above(doc, &c->src, &c->layer, &nid, err);
    if (rc != PC_OK) {
        return rc;
    }
    memset(&c->layer, 0, sizeof c->layer); /* moved */
    pc_layer *src = pc_document_layer_mut(doc, c->src);
    if (src != NULL) {
        src->visible = false;
    }
    if (doc->has_selection) {
        pc_surface_fini(&doc->selection);
        doc->has_selection = false;
    }
    *ha = true;
    *active = nid;
    c->out_id = nid;
    return PC_OK;
}

static pc_status run_refine(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "select.refineEdge";
    pc_algo_refine_params params = refine_params(p);
    struct strokes strokes = {0};
    pc_status rc = parse_strokes(p, &strokes, err);
    if (rc != PC_OK) {
        return rc;
    }
    bool decontaminate = pb(p, "decontaminate", false);
    float amount = clampf(pf(p, "amount", 100.0f), 0.0f, 100.0f);
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        strokes_fini(&strokes);
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    bool has_active = d->has_active_layer;
    pc_layer_id active = d->active_layer;

    /* Default output. */
    bool active_has_mask = false;
    if (has_active) {
        const pc_layer *l = pc_document_layer(doc, active);
        active_has_mask = l != NULL && l->has_mask;
    }
    const char *dflt = (!doc->has_selection && active_has_mask) ? "layerMask" : "selection";
    size_t olen = 0;
    const char *os = pc_json_as_str(pc_json_get(p, "output"), &olen);
    char output[32];
    (void)snprintf(output, sizeof output, "%.*s", os ? (int)olen : (int)strlen(dflt), os ? os : dflt);
    const char *valid[] = {"selection",     "layerMask",      "newLayer",
                           "newLayerWithMask", "newDocument",  "newDocumentWithMask"};
    bool ok = false;
    for (size_t i = 0; i < sizeof valid / sizeof valid[0]; i++) {
        if (strcmp(output, valid[i]) == 0) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        strokes_fini(&strokes);
        return pc_engine_err_bad_params(err, cmd,
                                        "output must be selection|layerMask|newLayer|newLayerWithMask|"
                                        "newDocument|newDocumentWithMask");
    }
    if (decontaminate && (strcmp(output, "selection") == 0 || strcmp(output, "layerMask") == 0)) {
        (void)snprintf(output, sizeof output, "newLayerWithMask");
    }

    /* The mask source: the selection, else the active layer's mask. */
    const pc_surface *sel = NULL;
    if (doc->has_selection) {
        sel = &doc->selection;
    } else if (has_active) {
        const pc_layer *l = pc_document_layer(doc, active);
        if (l != NULL && l->has_mask) {
            sel = &l->mask.surface;
        }
    }
    pc_algo_region region;
    memset(&region, 0, sizeof region);
    if (sel != NULL) {
        pc_algo_sampler smp = doc_sampler(d, pb(p, "sampleAllLayers", false));
        pc_rect content = pc_surface_content_bounds(sel);
        pc_rect canvas = pc_document_bounds(doc);
        pc_algo_edge_brush brush;
        bool have_brush = false;
        if (strokes.n > 0) {
            bool some = false;
            rc = pc_algo_edge_brush_new(strokes.list, strokes.n, canvas, &brush, &some, err);
            if (rc != PC_OK) {
                strokes_fini(&strokes);
                return rc;
            }
            have_brush = some;
        }
        rc = pc_algo_matting_refine_mask_brushed(&smp, pc_algo_matting_surface_reader, sel, content,
                                                 canvas, &params, have_brush ? &brush : NULL, &region,
                                                 err);
        if (have_brush) {
            pc_algo_edge_brush_fini(&brush);
        }
        if (rc != PC_OK) {
            strokes_fini(&strokes);
            return rc;
        }
    }
    strokes_fini(&strokes);

    /* Re-read the active layer (document may have changed under select-only edits: it hasn't). */
    yyjson_mut_doc *rd = NULL;
    if (strcmp(output, "selection") == 0) {
        struct refine_sel_ctx c = {pc_algo_region_is_none(&region) ? NULL : &region, false};
        rc = pc_engine_session_edit(s, "Select and Mask", refine_sel_edit, &c, err);
        pc_algo_region_fini(&region);
        if (rc != PC_OK) {
            return rc;
        }
        if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_str(rd, root, "output", "selection");
        yyjson_mut_obj_add_bool(rd, root, "selected", c.out_some);
        yyjson_mut_doc_set_root(rd, root);
        return PC_OK;
    }
    if (strcmp(output, "layerMask") == 0) {
        if (!has_active) {
            pc_algo_region_fini(&region);
            return pc_engine_err_other(err, "no active layer");
        }
        struct refine_mask_ctx c = {active, pc_algo_region_is_none(&region) ? NULL : &region};
        rc = pc_engine_session_edit(s, "Select and Mask", refine_mask_edit, &c, err);
        pc_algo_region_fini(&region);
        if (rc != PC_OK) {
            return rc;
        }
        if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_str(rd, root, "output", "layerMask");
        yyjson_mut_obj_add_uint(rd, root, "layer", active);
        yyjson_mut_obj_add_bool(rd, root, "selected", false);
        yyjson_mut_doc_set_root(rd, root);
        return PC_OK;
    }
    /* New layer / new document outputs need the source layer's pixels. */
    if (!has_active) {
        pc_algo_region_fini(&region);
        return pc_engine_err_other(err, "no active layer");
    }
    const pc_layer *src_layer = pc_document_layer(doc, active);
    const pc_surface *src = src_layer != NULL ? pc_layer_surface(src_layer) : NULL;
    if (src == NULL) {
        pc_algo_region_fini(&region);
        return pc_engine_err_other(err, "the active layer is %s %s layer without pixels",
                                   src_layer ? pc_layer_content_article(&src_layer->content) : "a",
                                   src_layer ? pc_layer_content_kind_name(&src_layer->content) : "pixel");
    }
    bool with_mask = strcmp(output, "newLayerWithMask") == 0 || strcmp(output, "newDocumentWithMask") == 0;
    /* pixels = decontaminate or a clone of the source. */
    pc_surface pixels;
    if (decontaminate) {
        rc = pc_algo_matting_decontaminate(src, &region, params.radius, amount, &pixels, err);
    } else {
        rc = pc_surface_clone(src, &pixels, err);
    }
    if (rc != PC_OK) {
        pc_algo_region_fini(&region);
        return rc;
    }
    pc_surface content;
    pc_surface *mask_ptr = NULL;
    pc_surface maskbuf;
    if (with_mask) {
        content = pixels; /* keep full pixels */
        memset(&pixels, 0, sizeof pixels);
        rc = region_to_surface(pc_algo_region_is_none(&region) ? NULL : &region, &maskbuf, err);
        if (rc != PC_OK) {
            pc_surface_fini(&content);
            pc_algo_region_fini(&region);
            return rc;
        }
        mask_ptr = &maskbuf;
    } else {
        rc = pc_algo_matting_masked_copy(&pixels, &region, &content, err);
        pc_surface_fini(&pixels);
        if (rc != PC_OK) {
            pc_algo_region_fini(&region);
            return rc;
        }
    }

    if (strncmp(output, "newDocument", 11) == 0) {
        pc_document nd;
        rc = pc_document_init(&nd, "Untitled", doc->size, doc->mode, doc->depth, err);
        if (rc != PC_OK) {
            pc_surface_fini(&content);
            if (mask_ptr) {
                pc_surface_fini(mask_ptr);
            }
            pc_algo_region_fini(&region);
            return rc;
        }
        nd.resolution_dpi = doc->resolution_dpi;
        nd.icc_profile = pc_blob_retain(doc->icc_profile);
        if (doc->has_color_table && pc_color_table_clone(&doc->color_table, &nd.color_table, NULL) == PC_OK) {
            nd.has_color_table = true;
        }
        if (doc->has_duotone && pc_duotone_clone(&doc->duotone, &nd.duotone, NULL) == PC_OK) {
            nd.has_duotone = true;
        }
        pc_layer layer;
        rc = raster_layer(pc_str_c(&src_layer->name), &content, mask_ptr, &layer, err);
        if (rc != PC_OK) {
            pc_surface_fini(&content);
            if (mask_ptr) {
                pc_surface_fini(mask_ptr);
            }
            pc_document_fini(&nd);
            pc_algo_region_fini(&region);
            return rc;
        }
        pc_layer_id lid = layer.id;
        pc_layer *lg = realloc(nd.layers, (nd.nlayers + 1) * sizeof(pc_layer));
        if (lg == NULL) {
            pc_layer_fini(&layer);
            pc_document_fini(&nd);
            pc_algo_region_fini(&region);
            return pc_fail(err, PC_ERR_NOMEM, "refine");
        }
        nd.layers = lg;
        nd.layers[nd.nlayers++] = layer;
        pc_algo_region_fini(&region);
        size_t di;
        rc = pc_engine_session_add_document(s, &nd, NULL, &di, err);
        if (rc != PC_OK) {
            pc_document_fini(&nd);
            return rc;
        }
        if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_str(rd, root, "output", output);
        yyjson_mut_obj_add_uint(rd, root, "document", di);
        yyjson_mut_obj_add_uint(rd, root, "layer", lid);
        yyjson_mut_obj_add_bool(rd, root, "selected", false);
        yyjson_mut_doc_set_root(rd, root);
        return PC_OK;
    }

    /* newLayer / newLayerWithMask */
    pc_str copyname;
    memset(&copyname, 0, sizeof copyname);
    (void)pc_document_copy_name(doc, pc_str_c(&src_layer->name), strlen(pc_str_c(&src_layer->name)),
                                &copyname, err);
    pc_layer layer;
    rc = raster_layer(pc_str_c(&copyname), &content, mask_ptr, &layer, err);
    pc_str_fini(&copyname);
    if (rc != PC_OK) {
        pc_surface_fini(&content);
        if (mask_ptr) {
            pc_surface_fini(mask_ptr);
        }
        pc_algo_region_fini(&region);
        return rc;
    }
    pc_algo_region_fini(&region);
    struct refine_new_ctx c = {active, layer, 0};
    rc = pc_engine_session_edit(s, "Select and Mask", refine_new_edit, &c, err);
    if (rc != PC_OK) {
        pc_layer_fini(&c.layer);
        return rc;
    }
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_str(rd, root, "output", output);
    yyjson_mut_obj_add_uint(rd, root, "layer", c.out_id);
    yyjson_mut_obj_add_bool(rd, root, "selected", false);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- registry ---------------------------------------------------------------------------- */

static const pc_engine_command SPECS[] = {
    {"select.quick", "Quick Selection", NULL, 0, NULL,
     "{\"points\":[[x,y],…],\"size\":px=30,\"mode\":\"add|subtract|replace\"=\"add\","
     "\"sampleAllLayers\":bool=false,\"enhanceEdge\":bool=false}",
     has_doc, run_quick, true},
    {"select.subject", "Subject", (const char *const[]){"Select"}, 1, NULL,
     "{\"sampleAllLayers\":bool=true,\"mode\":\"replace|add|subtract|intersect\"=\"replace\"}",
     has_doc, run_subject, true},
    {"select.refineEdge", "Refine Edge", NULL, 0, NULL,
     "{\"radius\":px=0,\"smartRadius\":bool=false,\"smooth\":0..100=0,\"feather\":px=0,\"contrast\":"
     "0..100=0,\"shiftEdge\":-100..100=0,\"decontaminate\":bool=false,\"amount\":0..100=100,"
     "\"output\":\"selection|layerMask|newLayer|newLayerWithMask|newDocument|newDocumentWithMask\"="
     "\"selection\",\"sampleAllLayers\":bool=false,\"refineStrokes\":[{\"points\":[[x,y],…],\"size\":"
     "px=30,\"mode\":\"add|subtract\"=\"add\"},…]=[]}",
     has_selection_or_mask, run_refine, true},
    {"select.focusArea", "Focus Area…", (const char *const[]){"Select"}, 1, NULL,
     "{\"range\":0..1=0.5,\"noise\":0..1=0,\"sampleAllLayers\":bool=true,\"mode\":\"replace|add|"
     "subtract|intersect\"=\"replace\"}",
     has_doc, run_focus, true},
};

const pc_engine_command *pc_engine_smartselect_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
