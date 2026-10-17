/* Select > Sky and Select > Isolate Layers (port of crates/engine/src/select_extra_cmds.rs, E7).
 *
 * Select > Transform Selection needs transform_cmds::warp_gray / check_budget (E4) and
 * warp_cmds::warp_from_params (E9); it is deferred to those chunks (see §8). */
#include <stdlib.h>
#include <string.h>

#include <math.h>

#include "algo/params.h"
#include "algo/segment.h"
#include "algo/selection.h"
#include "algo/transform.h"
#include "algo/warp.h"
#include "compose/compose.h"
#include "doc/doc.h"
#include "geom/warp.h"
#include "raster/surface.h"

#include "selection_cmds.h"
#include "transform_cmds.h"
#include "warp_cmds.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- composite sampler ------------------------------------------------------------------- */

static pc_status composite_rgba(const void *ctx, pc_rect r, float (*out)[4], size_t n, pc_error *err)
{
    const pc_document *doc = ctx;
    pc_compose_buffer buf;
    pc_status rc = pc_compose_render(doc, r, &buf, err);
    if (rc != PC_OK) {
        return rc;
    }
    for (size_t i = 0; i < n; i++) {
        if (i < buf.len) {
            out[i][0] = buf.px[i][0];
            out[i][1] = buf.px[i][1];
            out[i][2] = buf.px[i][2];
            out[i][3] = buf.px[i][3];
        }
    }
    pc_compose_buffer_fini(&buf);
    return PC_OK;
}

static pc_status has_doc(const pc_engine_session *s, pc_error *reason)
{
    return pc_engine_session_active(s) != NULL ? PC_OK
                                               : pc_fail(reason, PC_ERR_INVALID, "no document open");
}

static pc_status has_layer(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    return d->has_active_layer ? PC_OK : pc_fail(reason, PC_ERR_INVALID, "no active layer");
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

/* ---- Sky --------------------------------------------------------------------------------- */

struct sky_ctx {
    pc_algo_region region;
    pc_algo_selection_mode mode;
    bool out_some;
};

static pc_status sky_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct sky_ctx *c = vctx;
    pc_surface outs;
    bool some = false;
    pc_status rc = pc_algo_selection_combine_region(doc->has_selection ? &doc->selection : NULL,
                                                    &c->region, c->mode, &outs, &some, err);
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

static pc_status run_sky(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_rect canvas = pc_document_bounds(doc);
    pc_algo_sky_params params = {pf(p, "threshold", 50.0f), pf(p, "softness", 50.0f)};
    bool all = true;
    yyjson_val *av = pc_json_get(p, "sampleAllLayers");
    if (av != NULL) {
        (void)pc_json_as_bool(av, &all);
    }
    const pc_surface *layer = NULL;
    if (!all && d->has_active_layer) {
        const pc_layer *l = pc_document_layer(doc, d->active_layer);
        layer = l != NULL ? pc_layer_surface(l) : NULL;
    }
    pc_algo_sampler sampler;
    if (layer != NULL) {
        sampler = pc_algo_surface_sampler(layer);
    } else {
        sampler.rgba = composite_rgba;
        sampler.ctx = doc;
    }
    pc_algo_region region;
    pc_status rc = pc_algo_sky_select(&sampler, canvas, params, &region, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t mode_len = 0;
    yyjson_val *mv = pc_json_get(p, "mode");
    const char *ms = mv != NULL ? pc_json_as_str(mv, &mode_len) : NULL;
    char mbuf[16] = {0};
    if (ms != NULL && mode_len < sizeof mbuf) {
        memcpy(mbuf, ms, mode_len);
    }
    pc_algo_selection_mode mode = pc_algo_selection_mode_parse(
        (ms != NULL && mode_len == 3 && memcmp(ms, "new", 3) == 0) ? "replace" : mbuf);

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        pc_algo_region_fini(&region);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    if (pc_algo_region_is_none(&region)) {
        pc_algo_region_fini(&region);
        yyjson_mut_obj_add_bool(rd, root, "selected", doc->has_selection);
        yyjson_mut_obj_add_bool(rd, root, "changed", false);
        yyjson_mut_obj_add_real(rd, root, "coverage", 0.0);
        yyjson_mut_obj_add_str(rd, root, "message", "No sky was detected");
        yyjson_mut_doc_set_root(rd, root);
        return PC_OK;
    }
    double total = 0.0;
    for (size_t i = 0; i < region.len; i++) {
        total += (double)region.mask[i] / 255.0;
    }
    double area = (double)pc_rect_width(canvas) * (double)pc_rect_height(canvas);
    double coverage = total / (area > 1.0 ? area : 1.0);
    pc_rect b = region.bbox;

    struct sky_ctx c = {region, mode, false};
    pc_status erc = pc_engine_session_edit(s, "Select Sky", sky_edit, &c, err);
    pc_algo_region_fini(&region);
    if (erc != PC_OK) {
        pc_json_value_fini(out);
        return erc;
    }
    yyjson_mut_obj_add_bool(rd, root, "selected", c.out_some);
    yyjson_mut_obj_add_bool(rd, root, "changed", true);
    yyjson_mut_obj_add_real(rd, root, "coverage", coverage);
    yyjson_mut_val *bb = yyjson_mut_arr(rd);
    yyjson_mut_arr_add_int(rd, bb, b.x0);
    yyjson_mut_arr_add_int(rd, bb, b.y0);
    yyjson_mut_arr_add_int(rd, bb, (int64_t)pc_rect_width(b));
    yyjson_mut_arr_add_int(rd, bb, (int64_t)pc_rect_height(b));
    yyjson_mut_obj_add_val(rd, root, "bounds", bb);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- Isolate Layers ---------------------------------------------------------------------- */

static bool group_contains(const pc_layer *l, const pc_layer_id *set, size_t n, int depth)
{
    if (depth > 128 || !pc_layer_is_group(l)) {
        return false;
    }
    size_t nc = 0;
    const pc_layer *ch = pc_layer_children(l, &nc);
    for (size_t i = 0; i < nc; i++) {
        for (size_t k = 0; k < n; k++) {
            if (ch[i].id == set[k]) {
                return true;
            }
        }
        if (group_contains(&ch[i], set, n, depth + 1)) {
            return true;
        }
    }
    return false;
}

bool pc_engine_isolation_shows(const pc_document *doc, const pc_layer_id *isolated, size_t nisolated,
                               pc_layer_id id)
{
    if (nisolated == 0) {
        return true;
    }
    for (size_t i = 0; i < nisolated; i++) {
        if (isolated[i] == id) {
            return true;
        }
    }
    const pc_layer *l = pc_document_layer(doc, id);
    return l != NULL && group_contains(l, isolated, nisolated, 0);
}

static pc_status run_isolate(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    bool on;
    yyjson_val *ov = pc_json_get(p, "on");
    if (ov == NULL || !pc_json_as_bool(ov, &on)) {
        on = st->nisolated == 0; /* toggle */
    }
    free(st->isolated_layers);
    st->isolated_layers = NULL;
    st->nisolated = 0;
    if (on) {
        pc_layer_id *sel = NULL;
        size_t nsel = 0;
        pc_status rc = pc_engine_doc_state_selected_layers(st, &sel, &nsel, err);
        if (rc != PC_OK) {
            return rc;
        }
        st->isolated_layers = sel;
        st->nisolated = nsel;
    }
    st->revision++;
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_bool(rd, root, "isolated", st->nisolated != 0);
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    for (size_t i = 0; i < st->nisolated; i++) {
        yyjson_mut_arr_add_uint(rd, arr, st->isolated_layers[i]);
    }
    yyjson_mut_obj_add_val(rd, root, "layers", arr);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---------------------------------------------------------------- Transform Selection */

static pc_status has_sel(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    if (!pc_doc_arc_get(d->doc)->has_selection) {
        return pc_fail(reason, PC_ERR_INVALID, "no active selection");
    }
    return PC_OK;
}

static double ts_f(yyjson_val *p, const char *key, double def)
{
    double d;
    yyjson_val *v = pc_json_get(p, key);
    if (v != NULL && pc_json_as_f64(v, &d) && isfinite(d)) {
        return d;
    }
    return def;
}

static void reference_point(yyjson_val *p, const double r[4], double out[2])
{
    double cx = (r[0] + r[2]) / 2.0, cy = (r[1] + r[3]) / 2.0;
    out[0] = cx;
    out[1] = cy;
    yyjson_val *v = pc_json_get(p, "reference");
    if (v == NULL) {
        return;
    }
    if (yyjson_is_arr(v) && yyjson_arr_size(v) >= 2) {
        double x, y;
        if (pc_json_as_f64(yyjson_arr_get(v, 0), &x)) {
            out[0] = x;
        }
        if (pc_json_as_f64(yyjson_arr_get(v, 1), &y)) {
            out[1] = y;
        }
        return;
    }
    size_t kl = 0;
    const char *k = pc_json_as_str(v, &kl);
    if (k == NULL) {
        return;
    }
    struct {
        const char *name;
        double x, y;
    } opts[] = {{"topLeft", r[0], r[1]}, {"top", cx, r[1]},     {"topRight", r[2], r[1]},
                {"left", r[0], cy},      {"right", r[2], cy},    {"bottomLeft", r[0], r[3]},
                {"bottom", cx, r[3]},    {"bottomRight", r[2], r[3]}};
    for (size_t i = 0; i < sizeof opts / sizeof opts[0]; i++) {
        if (strlen(opts[i].name) == kl && memcmp(opts[i].name, k, kl) == 0) {
            out[0] = opts[i].x;
            out[1] = opts[i].y;
            return;
        }
    }
}

/* [[x,y]x4] from "quad" or "corners"; false when absent/malformed. */
static bool ts_quad(yyjson_val *p, double q[4][2])
{
    yyjson_val *a = pc_json_get(p, "quad");
    if (a == NULL) {
        a = pc_json_get(p, "corners");
    }
    if (a == NULL || !yyjson_is_arr(a) || yyjson_arr_size(a) != 4) {
        return false;
    }
    for (size_t i = 0; i < 4; i++) {
        yyjson_val *c = yyjson_arr_get(a, i);
        if (c == NULL || !yyjson_is_arr(c) || yyjson_arr_size(c) < 2 ||
            !pc_json_as_f64(yyjson_arr_get(c, 0), &q[i][0]) ||
            !pc_json_as_f64(yyjson_arr_get(c, 1), &q[i][1])) {
            return false;
        }
    }
    return true;
}

static pc_algo_homography hom(double a, double b, double c, double d, double e, double f)
{
    return (pc_algo_homography){{a, b, c, d, e, f, 0.0, 0.0, 1.0}};
}

/* The homography of the scale / rotate / skew / move params about the reference point. */
static pc_algo_homography affine_params(yyjson_val *p, const double r[4])
{
    double ref[2];
    reference_point(p, r, ref);
    double sx = ts_f(p, "scaleX", 100.0) / 100.0, sy = ts_f(p, "scaleY", 100.0) / 100.0;
    double a = ts_f(p, "rotate", 0.0) * (M_PI / 180.0);
    double kx = tan(ts_f(p, "skewX", 0.0) * (M_PI / 180.0)), ky = tan(ts_f(p, "skewY", 0.0) * (M_PI / 180.0));
    double dx = ts_f(p, "dx", ts_f(p, "x", 0.0)), dy = ts_f(p, "dy", ts_f(p, "y", 0.0));
    double sa = sin(a), ca = cos(a);
    pc_algo_homography tpos = hom(1.0, 0.0, ref[0] + dx, 0.0, 1.0, ref[1] + dy);
    pc_algo_homography rot = hom(ca, -sa, 0.0, sa, ca, 0.0);
    pc_algo_homography skew = hom(1.0, kx, 0.0, ky, 1.0, 0.0);
    pc_algo_homography scale = hom(sx, 0.0, 0.0, 0.0, sy, 0.0);
    pc_algo_homography tneg = hom(1.0, 0.0, -ref[0], 0.0, 1.0, -ref[1]);
    pc_algo_homography m = pc_algo_homography_mul(&tpos, &rot);
    m = pc_algo_homography_mul(&m, &skew);
    m = pc_algo_homography_mul(&m, &scale);
    m = pc_algo_homography_mul(&m, &tneg);
    return m;
}

static void ts_warp_map(void *ctx, double x, double y, double *ox, double *oy)
{
    pc_point q = pc_warp_map((const pc_warp *)ctx, x, y);
    *ox = q.x;
    *oy = q.y;
}

struct ts_ctx {
    bool is_warp;
    pc_warp *warp;
    pc_algo_homography h;
    pc_algo_interp interp;
    bool selected; /* out */
};

static pc_status ts_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    (void)active;
    struct ts_ctx *c = vctx;
    if (!doc->has_selection) {
        c->selected = false;
        return PC_OK;
    }
    pc_surface out;
    pc_status rc;
    if (c->is_warp) {
        rc = pc_algo_warp_mesh_gray(&doc->selection, ts_warp_map, c->warp, c->interp, &out, err);
    } else {
        rc = pc_engine_transform_check_budget(pc_surface_content_bounds(&doc->selection), &c->h,
                                              "selection", err);
        if (rc != PC_OK) {
            return rc;
        }
        rc = pc_engine_transform_warp_gray(&doc->selection, &c->h, c->interp, &out, err);
    }
    if (rc != PC_OK) {
        return rc;
    }
    pc_surface_fini(&doc->selection);
    if (pc_rect_is_empty(pc_surface_content_bounds(&out))) {
        pc_surface_fini(&out);
        doc->has_selection = false;
        c->selected = false;
    } else {
        doc->selection = out;
        doc->has_selection = true;
        c->selected = true;
    }
    return PC_OK;
}

static pc_status ts_result(pc_engine_session *s, bool changed, bool selected, pc_json_value *out,
                           pc_error *err)
{
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_bool(rd, root, "changed", changed);
    if (changed) {
        yyjson_mut_obj_add_bool(rd, root, "selected", selected);
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    pc_rect b = {0, 0, 0, 0};
    bool have = false;
    if (d != NULL) {
        const pc_document *doc = pc_doc_arc_get(d->doc);
        if (doc->has_selection) {
            b = pc_surface_content_bounds(&doc->selection);
            have = !pc_rect_is_empty(b);
        }
    }
    if (have) {
        yyjson_mut_val *arr = yyjson_mut_arr(rd);
        yyjson_mut_arr_add_int(rd, arr, b.x0);
        yyjson_mut_arr_add_int(rd, arr, b.y0);
        yyjson_mut_arr_add_int(rd, arr, (int)pc_rect_width(b));
        yyjson_mut_arr_add_int(rd, arr, (int)pc_rect_height(b));
        yyjson_mut_obj_add_val(rd, root, "bounds", arr);
    } else {
        yyjson_mut_obj_add_val(rd, root, "bounds", yyjson_mut_null(rd));
    }
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_transform_selection(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                         pc_error *err)
{
    const char *cmd = "select.transformSelection";
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (!doc->has_selection) {
        return pc_engine_err_other(err, "no selection");
    }
    pc_rect cb = pc_surface_content_bounds(&doc->selection);
    double rect[4] = {(double)cb.x0, (double)cb.y0, (double)cb.x1, (double)cb.y1};
    yyjson_val *rv = pc_json_get(p, "rect");
    if (rv != NULL && yyjson_is_arr(rv) && yyjson_arr_size(rv) == 4) {
        for (size_t i = 0; i < 4; i++) {
            (void)pc_json_as_f64(yyjson_arr_get(rv, i), &rect[i]);
        }
    }
    char ib[32] = "bilinear";
    yyjson_val *iv = pc_json_get(p, "interpolation");
    if (iv != NULL) {
        size_t il = 0;
        const char *is = pc_json_as_str(iv, &il);
        if (is != NULL && il < sizeof ib) {
            memcpy(ib, is, il);
            ib[il] = '\0';
        }
    }
    pc_algo_interp interp = pc_algo_interp_parse(ib);
    bool is_warp = pc_json_get(p, "warp") != NULL || pc_json_get(p, "style") != NULL ||
                   pc_json_get(p, "mesh") != NULL || pc_json_get(p, "grid") != NULL;
    if (is_warp) {
        double wrect[4] = {floor(rect[0]), floor(rect[1]), ceil(rect[2]), ceil(rect[3])};
        pc_warp w;
        pc_status rc = pc_engine_warp_from_params(cmd, p, wrect, &w, err);
        if (rc != PC_OK) {
            return rc;
        }
        if (pc_warp_is_identity(&w)) {
            pc_warp_fini(&w);
            return ts_result(s, false, true, out, err);
        }
        struct ts_ctx c = {true, &w, PC_ALGO_HOMOGRAPHY_IDENTITY, interp, false};
        rc = pc_engine_session_edit(s, "Transform Selection", ts_edit, &c, err);
        pc_warp_fini(&w);
        if (rc != PC_OK) {
            return rc;
        }
        return ts_result(s, true, c.selected, out, err);
    }
    pc_algo_homography h;
    double q[4][2];
    if (ts_quad(p, q)) {
        if (!pc_algo_homography_rect_to_quad(rect, (const double(*)[2])q, &h)) {
            return pc_engine_err_bad_params(err, cmd, "degenerate quad");
        }
    } else {
        yyjson_val *mv = pc_json_get(p, "matrix");
        if (mv != NULL && yyjson_is_arr(mv)) {
            if (yyjson_arr_size(mv) != 6) {
                return pc_engine_err_bad_params(err, cmd, "matrix must be [a, b, c, d, e, f]");
            }
            double v[6];
            for (size_t i = 0; i < 6; i++) {
                if (!pc_json_as_f64(yyjson_arr_get(mv, i), &v[i])) {
                    return pc_engine_err_bad_params(err, cmd, "matrix must be [a, b, c, d, e, f]");
                }
            }
            h = hom(v[0], v[2], v[4], v[1], v[3], v[5]);
        } else {
            h = affine_params(p, rect);
        }
    }
    pc_algo_homography inv;
    if (!pc_algo_homography_inverse(&h, &inv)) {
        return pc_engine_err_bad_params(err, cmd, "the transform collapses the selection");
    }
    bool identity = true;
    pc_algo_homography id = PC_ALGO_HOMOGRAPHY_IDENTITY;
    for (size_t i = 0; i < 9; i++) {
        if (fabs(h.m[i] - id.m[i]) >= 1e-12) {
            identity = false;
            break;
        }
    }
    if (identity) {
        return ts_result(s, false, true, out, err);
    }
    struct ts_ctx c = {false, NULL, h, interp, false};
    pc_status rc = pc_engine_session_edit(s, "Transform Selection", ts_edit, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    return ts_result(s, true, c.selected, out, err);
}

static const pc_engine_command SPECS[] = {
    {"select.transformSelection", "Transform Selection", (const char *const[]){"Select"}, 1, NULL,
     "{\"rect\":[x0,y0,x1,y1]?,\"quad\"|\"corners\":[[x,y]x4]?,\"matrix\":[a,b,c,d,e,f]?,\"scaleX\":%="
     "100,\"scaleY\":%=100,\"rotate\":deg=0,\"skewX\":deg=0,\"skewY\":deg=0,\"dx\":px=0,\"dy\":px=0,"
     "\"reference\":\"center|topLeft|top|topRight|left|right|bottomLeft|bottom|bottomRight\"|[x,y]="
     "\"center\",\"style\"|\"mesh\"|\"grid\"|\"warp\":…,\"interpolation\":\"bilinear|bicubic|nearest\"="
     "\"bilinear\"}",
     has_sel, run_transform_selection, true},
    {"select.sky", "Sky", (const char *const[]){"Select"}, 1, NULL,
     "{\"mode\":\"replace|add|subtract|intersect\"=\"replace\",\"sampleAllLayers\":bool=true,"
     "\"threshold\":0..100=50,\"softness\":0..100=50} → {selected, changed, coverage (0–1 of the "
     "canvas), bounds [x,y,w,h]}",
     has_doc, run_sky, true},
    {"select.isolateLayers", "Isolate Layers", (const char *const[]){"Select"}, 1, NULL,
     "{\"on\":bool? (default: toggle)} — Layers panel lists only the selected layers → {isolated, "
     "layers}",
     has_layer, run_isolate, false},
};

const pc_engine_command *pc_engine_select_extra_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
