/* Filter > Vanishing Point: filter.vanishingPoint (port of crates/engine/src/vp_cmds.rs).
 *
 * Perspective planes (drawn or torn off an edge of another), pasting an image onto a plane and the
 * perspective clone stamp, as one replayable command (photocraft_algo vanishing). The edit lands on
 * the active pixel layer, or on a new layer with "newLayer": true.
 *
 * Runs synchronously. Never crashes (REWRITE_PLAN.md rule 9): planes and params are validated,
 * counts are bounded, and the one history step is all-or-nothing. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "algo/vanishing.h"
#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "geom/geom.h"
#include "raster/surface.h"

#include "engine_internal.h"
#include "vp_cmds.h"

#define CMD "filter.vanishingPoint"
#define VP_MAX_PLANES 64u
#define VP_MAX_PASTES 64u
#define VP_MAX_CLONES 64u
#define VP_MAX_POINTS 4096u

static const char *const MENU_FILTER[] = {"Filter"};

static bool pt2(yyjson_val *v, double out[2])
{
    if (v == NULL || !yyjson_is_arr(v) || yyjson_arr_size(v) < 2) {
        return false;
    }
    return pc_json_as_f64(yyjson_arr_get(v, 0), &out[0]) && pc_json_as_f64(yyjson_arr_get(v, 1), &out[1]);
}

static bool corners_of(yyjson_val *plane, double c[4][2])
{
    yyjson_val *a = plane != NULL ? pc_json_get(plane, "corners") : NULL;
    if (a == NULL || !yyjson_is_arr(a) || yyjson_arr_size(a) != 4) {
        return false;
    }
    for (size_t i = 0; i < 4; i++) {
        if (!pt2(yyjson_arr_get(a, i), c[i])) {
            return false;
        }
    }
    return true;
}

static pc_status build_scene(yyjson_val *p, pc_rect canvas, pc_algo_vp_scene *sc, pc_error *err)
{
    yyjson_val *planes = p != NULL ? pc_json_get(p, "planes") : NULL;
    if (planes == NULL || !yyjson_is_arr(planes) || yyjson_arr_size(planes) == 0) {
        return pc_engine_err_bad_params(
            err, CMD, "pass `planes`: [{\"corners\":[[x,y]x4]}, {\"from\":0,\"edge\":\"top\",\"angle\":90}]");
    }
    if (yyjson_arr_size(planes) > VP_MAX_PLANES) {
        return pc_engine_err_bad_params(err, CMD, "too many planes");
    }
    double center[2] = {((double)canvas.x0 + (double)canvas.x1) / 2.0,
                        ((double)canvas.y0 + (double)canvas.y1) / 2.0};
    pc_algo_vp_plane first;
    if (!corners_of(yyjson_arr_get(planes, 0), first.corners)) {
        return pc_engine_err_bad_params(err, CMD, "the first plane needs four `corners`");
    }
    if (pc_algo_vp_plane_check_at(&first, center) == PC_ALGO_VP_INVALID) {
        return pc_engine_err_bad_params(err, CMD, "plane 0: the four corners must form a convex quad");
    }
    double focal = 0.0;
    bool has_focal = false;
    yyjson_val *jf = p != NULL ? pc_json_get(p, "focalLength") : NULL;
    if (jf != NULL && pc_json_as_f64(jf, &focal) && focal > 0.0) {
        has_focal = true;
    }
    double fallback = (double)(pc_rect_width(canvas) > pc_rect_height(canvas) ? pc_rect_width(canvas)
                                                                             : pc_rect_height(canvas));
    pc_status rc = pc_algo_vp_scene_init(sc, &first, center, has_focal, focal, fallback, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t np = yyjson_arr_size(planes);
    for (size_t i = 1; i < np; i++) {
        yyjson_val *pl = yyjson_arr_get(planes, i);
        pc_algo_vp_plane dp;
        if (corners_of(pl, dp.corners)) {
            if (pc_algo_vp_plane_check_at(&dp, sc->center) == PC_ALGO_VP_INVALID) {
                pc_algo_vp_scene_fini(sc);
                return pc_engine_err_bad_params(err, CMD, "plane %zu: the four corners must form a convex quad",
                                                i);
            }
            if (sc->n == sc->cap) {
                size_t nc = sc->cap == 0 ? 4 : sc->cap * 2;
                pc_algo_vp_plane *nv = realloc(sc->planes, nc * sizeof *nv);
                if (nv == NULL) {
                    pc_algo_vp_scene_fini(sc);
                    return pc_fail(err, PC_ERR_NOMEM, "vanishing point planes");
                }
                sc->planes = nv;
                sc->cap = nc;
            }
            sc->planes[sc->n++] = dp;
            continue;
        }
        uint64_t from = 0;
        yyjson_val *jfrom = pl != NULL ? pc_json_get(pl, "from") : NULL;
        if (jfrom == NULL || !pc_json_as_u64(jfrom, &from) || from >= sc->n) {
            pc_algo_vp_scene_fini(sc);
            return pc_engine_err_bad_params(err, CMD, "plane %zu: `corners` or `from` (an earlier plane)", i);
        }
        const char *es = "top";
        size_t el = 0;
        const char *e = pl != NULL ? pc_json_as_str(pc_json_get(pl, "edge"), &el) : NULL;
        if (e != NULL) {
            es = e;
        }
        pc_algo_vp_edge edge;
        if (strncmp(es, "top", el ? el : 3) == 0 && (el == 0 || el == 3)) {
            edge = PC_ALGO_VP_TOP;
        } else if (el == 5 && strncmp(es, "right", 5) == 0) {
            edge = PC_ALGO_VP_RIGHT;
        } else if (el == 6 && strncmp(es, "bottom", 6) == 0) {
            edge = PC_ALGO_VP_BOTTOM;
        } else if (el == 4 && strncmp(es, "left", 4) == 0) {
            edge = PC_ALGO_VP_LEFT;
        } else {
            pc_algo_vp_scene_fini(sc);
            return pc_engine_err_bad_params(err, CMD, "plane %zu: unknown edge", i);
        }
        double angle = 90.0, depth = 1.0;
        yyjson_val *ja = pl != NULL ? pc_json_get(pl, "angle") : NULL;
        if (ja != NULL) {
            (void)pc_json_as_f64(ja, &angle);
        }
        yyjson_val *jd = pl != NULL ? pc_json_get(pl, "depth") : NULL;
        if (jd != NULL) {
            (void)pc_json_as_f64(jd, &depth);
        }
        size_t idx = 0;
        rc = pc_algo_vp_scene_tear_off(sc, (size_t)from, edge, angle, depth, &idx, err);
        if (rc != PC_OK) {
            pc_algo_vp_scene_fini(sc);
            return pc_engine_err_bad_params(err, CMD, "plane %zu would lie behind the camera", i);
        }
    }
    for (size_t i = 0; i < sc->n; i++) {
        pc_algo_homography h;
        if (!pc_algo_vp_plane_homography(&sc->planes[i], &h)) {
            pc_algo_vp_scene_fini(sc);
            return pc_engine_err_bad_params(err, CMD, "a plane is degenerate");
        }
    }
    return PC_OK;
}

static pc_status vp_has_layer(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (doc == NULL || !d->has_active_layer) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    const pc_layer *l = pc_document_layer(doc, d->active_layer);
    if (l == NULL || l->content.kind != PC_LAYER_RASTER) {
        return pc_fail(reason, PC_ERR_INVALID, "Vanishing Point needs a pixel layer");
    }
    return PC_OK;
}

/* top (straight alpha) over dst (same format after convert), source-over. */
static void over(pc_surface *dst, const pc_surface *top)
{
    pc_rect b = pc_surface_content_bounds(top);
    if (pc_rect_is_empty(b)) {
        return;
    }
    pc_pixel_format fmt = pc_surface_format(dst);
    size_t n = pc_pixel_format_channels(fmt);
    if (n == 0) {
        return;
    }
    pc_surface conv;
    const pc_surface *src = top;
    bool converted = false;
    if (!pc_pixel_format_eq(pc_surface_format(top), fmt)) {
        if (pc_surface_convert(top, fmt, &conv, NULL) != PC_OK) {
            return;
        }
        src = &conv;
        converted = true;
    }
    float *t = NULL, *d = NULL;
    size_t tl = 0, dl = 0;
    if (pc_surface_read_region(src, b, &t, &tl, NULL) == PC_OK &&
        pc_surface_read_region(dst, b, &d, &dl, NULL) == PC_OK) {
        size_t px = dl / n;
        for (size_t i = 0; i < px; i++) {
            float *tp = &t[i * n], *dp = &d[i * n];
            if (!fmt.alpha) {
                memcpy(dp, tp, n * sizeof(float));
                continue;
            }
            float ta = tp[n - 1], da = dp[n - 1];
            float oa = ta + da * (1.0f - ta);
            for (size_t c = 0; c < n - 1; c++) {
                dp[c] = oa > 0.0f ? (tp[c] * ta + dp[c] * da * (1.0f - ta)) / oa : 0.0f;
            }
            dp[n - 1] = oa;
        }
        (void)pc_surface_write_region(dst, b, d, dl, NULL);
    }
    free(t);
    free(d);
    if (converted) {
        pc_surface_fini(&conv);
    }
}

struct vp_paste {
    size_t plane;
    pc_surface src; /* owned */
    pc_rect rect;
    double at[2];
    double width;
};

struct vp_ctx {
    pc_algo_vp_scene *sc;
    struct vp_paste *pastes;
    size_t npastes;
    yyjson_val *clones; /* borrowed array */
    bool new_layer;
    size_t dabs;
    pc_layer_id target;
};

static pc_status vp_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    struct vp_ctx *c = vctx;
    pc_layer_id id;
    if (c->new_layer) {
        pc_str name;
        pc_status rc = pc_document_next_layer_name(doc, "Vanishing Point", &name, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_layer l;
        rc = pc_layer_raster(&l, pc_str_c(&name), pc_document_pixel_format(doc), err);
        pc_str_fini(&name);
        if (rc != PC_OK) {
            return rc;
        }
        pc_layer_id above = (active != NULL && *active != 0) ? *active : 0;
        pc_layer_id nid = 0;
        rc = pc_document_insert_above(doc, above != 0 ? &above : NULL, &l, &nid, err);
        if (rc != PC_OK) {
            pc_layer_fini(&l);
            return rc;
        }
        id = nid;
        if (active != NULL) {
            *active = nid;
        }
    } else {
        if (active == NULL || *active == 0) {
            return pc_engine_err_other(err, "no active layer");
        }
        id = *active;
    }
    pc_surface base;
    bool have_base = false;
    if (c->new_layer) {
        if (pc_engine_flattened(doc, pc_document_pixel_format(doc), &base, err) == PC_OK) {
            have_base = true;
        }
    }
    pc_locks locks = pc_document_effective_locks(doc, id);
    pc_layer *l = pc_document_layer_mut(doc, id);
    if (l == NULL) {
        if (have_base) {
            pc_surface_fini(&base);
        }
        return pc_engine_err_no_layer(err, id);
    }
    if (locks.all || locks.pixels) {
        if (have_base) {
            pc_surface_fini(&base);
        }
        return pc_engine_err_other(err, "layer \"%s\" is locked", pc_str_c(&l->name));
    }
    pc_surface *surf = pc_layer_surface_mut(l);
    if (surf == NULL) {
        if (have_base) {
            pc_surface_fini(&base);
        }
        return pc_engine_err_other(err, "Vanishing Point needs a pixel layer");
    }
    for (size_t i = 0; i < c->npastes; i++) {
        struct vp_paste *ps = &c->pastes[i];
        pc_surface out;
        if (pc_algo_vp_paste(c->sc, ps->plane, &ps->src, ps->rect, ps->at, ps->width, &out, NULL) ==
            PC_OK) {
            over(surf, &out);
            pc_surface_fini(&out);
        }
    }
    size_t nclones = c->clones != NULL ? yyjson_arr_size(c->clones) : 0;
    for (size_t k = 0; k < nclones && k < VP_MAX_CLONES; k++) {
        yyjson_val *cl = yyjson_arr_get(c->clones, k);
        double source[2];
        if (!pt2(pc_json_get(cl, "source"), source)) {
            if (have_base) {
                pc_surface_fini(&base);
            }
            return pc_engine_err_bad_params(err, CMD, "clone %zu: `source` [x,y]", k);
        }
        yyjson_val *jpts = pc_json_get(cl, "points");
        size_t npts = jpts != NULL && yyjson_is_arr(jpts) ? yyjson_arr_size(jpts) : 0;
        if (npts > VP_MAX_POINTS) {
            npts = VP_MAX_POINTS;
        }
        double(*pts)[2] = npts > 0 ? malloc(npts * sizeof *pts) : NULL;
        size_t got = 0;
        for (size_t i = 0; i < npts; i++) {
            if (pt2(yyjson_arr_get(jpts, i), pts[got])) {
                got++;
            }
        }
        double size = 40.0, hardness = 50.0, opacity = 100.0;
        yyjson_val *js = pc_json_get(cl, "size");
        if (js != NULL) {
            (void)pc_json_as_f64(js, &size);
        }
        yyjson_val *jh = pc_json_get(cl, "hardness");
        if (jh != NULL) {
            (void)pc_json_as_f64(jh, &hardness);
        }
        yyjson_val *jo = pc_json_get(cl, "opacity");
        if (jo != NULL) {
            (void)pc_json_as_f64(jo, &opacity);
        }
        size = size < 1.0 ? 1.0 : (size > 2000.0 ? 2000.0 : size);
        hardness = (hardness < 0.0 ? 0.0 : (hardness > 100.0 ? 100.0 : hardness)) / 100.0;
        opacity = (opacity < 0.0 ? 0.0 : (opacity > 100.0 ? 100.0 : opacity)) / 100.0;

        pc_surface work;
        if (have_base) {
            if (pc_surface_clone(&base, &work, err) != PC_OK) {
                free(pts);
                pc_surface_fini(&base);
                return err != NULL ? err->code : PC_ERR_NOMEM;
            }
            over(&work, surf);
        } else {
            if (pc_surface_clone(surf, &work, err) != PC_OK) {
                free(pts);
                return err != NULL ? err->code : PC_ERR_NOMEM;
            }
        }
        pc_surface before;
        if (pc_surface_clone(&work, &before, err) != PC_OK) {
            free(pts);
            pc_surface_fini(&work);
            if (have_base) {
                pc_surface_fini(&base);
            }
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        size_t dabs = 0;
        (void)pc_algo_vp_clone_stroke(c->sc, &work, source, (const double(*)[2])pts, got, size, hardness,
                                      (float)opacity, &dabs, NULL);
        c->dabs += dabs;
        free(pts);
        /* Copy back only what the stroke changed. */
        pc_rect b = pc_rect_union(pc_surface_content_bounds(&work), pc_surface_content_bounds(&before));
        if (!pc_rect_is_empty(b)) {
            size_t n = pc_pixel_format_channels(pc_surface_format(surf));
            float *nw = NULL, *ow = NULL, *cur = NULL;
            size_t nl = 0, ol = 0, cl2 = 0;
            if (n > 0 && pc_surface_read_region(&work, b, &nw, &nl, NULL) == PC_OK &&
                pc_surface_read_region(&before, b, &ow, &ol, NULL) == PC_OK &&
                pc_surface_read_region(surf, b, &cur, &cl2, NULL) == PC_OK) {
                size_t px = cl2 / n;
                for (size_t i = 0; i < px; i++) {
                    if (memcmp(&nw[i * n], &ow[i * n], n * sizeof(float)) != 0) {
                        memcpy(&cur[i * n], &nw[i * n], n * sizeof(float));
                    }
                }
                (void)pc_surface_write_region(surf, b, cur, cl2, NULL);
            }
            free(nw);
            free(ow);
            free(cur);
        }
        pc_surface_fini(&work);
        pc_surface_fini(&before);
    }
    pc_surface_prune(surf);
    if (have_base) {
        pc_surface_fini(&base);
    }
    c->target = id;
    return PC_OK;
}

static pc_status run_vp(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_rect canvas = pc_document_bounds(doc);
    pc_algo_vp_scene sc;
    memset(&sc, 0, sizeof sc); /* build_scene initialises it; keep the analyzer sure of it */
    pc_status rc = build_scene(p, canvas, &sc, err);
    if (rc != PC_OK) {
        return rc;
    }
    /* Resolve paste sources up front. */
    struct vp_paste *pastes = NULL;
    size_t npastes = 0;
    yyjson_val *jp = p != NULL ? pc_json_get(p, "paste") : NULL;
    size_t npj = jp != NULL && yyjson_is_arr(jp) ? yyjson_arr_size(jp) : 0;
    if (npj > VP_MAX_PASTES) {
        npj = VP_MAX_PASTES;
    }
    if (npj > 0) {
        pastes = calloc(npj, sizeof *pastes);
        if (pastes == NULL) {
            pc_algo_vp_scene_fini(&sc);
            return pc_fail(err, PC_ERR_NOMEM, "vanishing point");
        }
    }
    for (size_t k = 0; k < npj; k++) {
        yyjson_val *item = yyjson_arr_get(jp, k);
        uint64_t plane = 0;
        yyjson_val *jpl = pc_json_get(item, "plane");
        if (jpl != NULL) {
            (void)pc_json_as_u64(jpl, &plane);
        }
        if (plane >= sc.n) {
            rc = pc_engine_err_bad_params(err, CMD, "paste %zu: no plane %llu", k,
                                          (unsigned long long)plane);
            goto fail_pastes;
        }
        const pc_surface *sf = NULL;
        uint64_t lid = 0;
        yyjson_val *jl = pc_json_get(item, "layer");
        if (jl != NULL && pc_json_as_u64(jl, &lid)) {
            const pc_layer *ll = pc_document_layer(doc, (pc_layer_id)lid);
            sf = ll != NULL ? pc_layer_surface(ll) : NULL;
            if (sf == NULL) {
                rc = pc_engine_err_no_layer(err, (pc_layer_id)lid);
                goto fail_pastes;
            }
        } else if (pc_engine_clipboard_surface(s) != NULL) {
            sf = pc_engine_clipboard_surface(s);
        } else {
            rc = pc_engine_err_bad_params(err, CMD, "paste %zu: nothing on the clipboard (or pass `layer`)",
                                          k);
            goto fail_pastes;
        }
        struct vp_paste *ps = &pastes[npastes];
        if (pc_surface_clone(sf, &ps->src, err) != PC_OK) {
            rc = err != NULL ? err->code : PC_ERR_NOMEM;
            goto fail_pastes;
        }
        ps->plane = (size_t)plane;
        ps->rect = pc_surface_content_bounds(&ps->src);
        ps->at[0] = 0.25;
        ps->at[1] = 0.25;
        double at[2];
        if (pt2(pc_json_get(item, "at"), at)) {
            ps->at[0] = at[0];
            ps->at[1] = at[1];
        }
        double width = 0.5;
        yyjson_val *jw = pc_json_get(item, "width");
        if (jw != NULL) {
            (void)pc_json_as_f64(jw, &width);
        }
        ps->width = width < 0.01 ? 0.01 : (width > 10.0 ? 10.0 : width);
        npastes++;
    }

    bool new_layer = false;
    yyjson_val *jnl = p != NULL ? pc_json_get(p, "newLayer") : NULL;
    if (jnl != NULL) {
        (void)pc_json_as_bool(jnl, &new_layer);
    }
    struct vp_ctx c;
    memset(&c, 0, sizeof c);
    c.sc = &sc;
    c.pastes = pastes;
    c.npastes = npastes;
    c.clones = p != NULL ? pc_json_get(p, "clone") : NULL;
    if (c.clones != NULL && !yyjson_is_arr(c.clones)) {
        c.clones = NULL;
    }
    c.new_layer = new_layer;
    rc = pc_engine_session_edit(s, "Vanishing Point", vp_edit, &c, err);
    for (size_t i = 0; i < npastes; i++) {
        pc_surface_fini(&pastes[i].src);
    }
    free(pastes);
    if (rc != PC_OK) {
        pc_algo_vp_scene_fini(&sc);
        return rc;
    }

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        pc_algo_vp_scene_fini(&sc);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", c.target);
    yyjson_mut_obj_add_real(rd, root, "focal", sc.focal);
    yyjson_mut_val *pl = yyjson_mut_arr(rd);
    for (size_t i = 0; i < sc.n; i++) {
        pc_algo_vp_lifted lifted;
        pc_algo_vp_scene_lifted(&sc, i, &lifted);
        double lu = 0, lv = 0;
        pc_algo_vp_lifted_lengths(&lifted, &lu, &lv);
        yyjson_mut_val *po = yyjson_mut_obj(rd);
        yyjson_mut_val *cs = yyjson_mut_arr(rd);
        for (size_t j = 0; j < 4; j++) {
            yyjson_mut_val *pr = yyjson_mut_arr(rd);
            yyjson_mut_arr_add_real(rd, pr, sc.planes[i].corners[j][0]);
            yyjson_mut_arr_add_real(rd, pr, sc.planes[i].corners[j][1]);
            yyjson_mut_arr_append(cs, pr);
        }
        yyjson_mut_obj_add_val(rd, po, "corners", cs);
        yyjson_mut_obj_add_real(rd, po, "aspect", lv != 0.0 ? lu / lv : 0.0);
        yyjson_mut_arr_append(pl, po);
    }
    yyjson_mut_obj_add_val(rd, root, "planes", pl);
    yyjson_mut_obj_add_uint(rd, root, "pasted", npastes);
    yyjson_mut_obj_add_uint(rd, root, "dabs", c.dabs);
    yyjson_mut_doc_set_root(rd, root);
    pc_algo_vp_scene_fini(&sc);
    return PC_OK;

fail_pastes:
    for (size_t i = 0; i < npastes; i++) {
        pc_surface_fini(&pastes[i].src);
    }
    free(pastes);
    pc_algo_vp_scene_fini(&sc);
    return rc;
}

#define VP_PARAMS                                                                                    \
    "{\"planes\":[{\"corners\":[[x,y]x4]} | {\"from\":plane,\"edge\":\"top|right|bottom|left\","      \
    "\"angle\":deg=90,\"depth\":1}],\"focalLength\":px=0,\"paste\":[{\"plane\":0,\"layer\":id? (else " \
    "the clipboard),\"at\":[u,v]=[0.25,0.25],\"width\":0..1=0.5}],\"clone\":[{\"source\":[x,y],"       \
    "\"points\":[[x,y]],\"size\":px=40,\"hardness\":0..100=50,\"opacity\":0..100=100}],\"newLayer\":"  \
    "bool=false} -> {layer,focal,planes,pasted,dabs}"

static const pc_engine_command SPECS[] = {
    {CMD, "Vanishing Point…", MENU_FILTER, 1, "Cmd+Alt+V", VP_PARAMS, vp_has_layer, run_vp, true},
};

const pc_engine_command *pc_engine_vp_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
