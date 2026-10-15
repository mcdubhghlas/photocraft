/* Select > Sky and Select > Isolate Layers (port of crates/engine/src/select_extra_cmds.rs, E7).
 *
 * Select > Transform Selection needs transform_cmds::warp_gray / check_budget (E4) and
 * warp_cmds::warp_from_params (E9); it is deferred to those chunks (see §8). */
#include <stdlib.h>
#include <string.h>

#include "algo/segment.h"
#include "algo/selection.h"
#include "compose/compose.h"
#include "doc/doc.h"
#include "raster/surface.h"

#include "selection_cmds.h"

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

static const pc_engine_command SPECS[] = {
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
