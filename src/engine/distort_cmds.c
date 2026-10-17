/* Filter › Liquify, Edit › Puppet Warp and Edit › Perspective Warp, plus their Layer › Smart
 * Objects twins (port of crates/engine/src/distort_cmds.rs).
 *
 * All three take their whole edit as data, so each is one replayable command and one history step:
 * Liquify the brush strokes (replayed onto a fresh displacement field), Puppet Warp the pins (ARAP
 * on a mesh of the opaque region), Perspective Warp the planes. On a smart object each becomes a
 * smart filter (the params are stored and re-applied to a fresh render); on a pixel layer the pixels
 * are edited in place. The params are the data in every case, so nothing needs a blob.
 *
 * Never crashes (REWRITE_PLAN.md rule 9): every param is validated and every bound checked before
 * indexing/dividing/allocating; a result too large is refused (PC_ERR_LIMIT -> an error), not
 * clipped; the smart-filter re-apply hook skips (applied=false) on any parse error. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "algo/liquify.h"
#include "algo/params.h"
#include "algo/perspective.h"
#include "algo/puppet.h"
#include "algo/transform.h"
#include "algo/warp.h"
#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "geom/geom.h"
#include "raster/surface.h"

#include "allocation.h"
#include "distort_cmds.h"
#include "engine_internal.h"
#include "extra_cmds.h"      /* pc_engine_background_to_layer */
#include "smart_cmds.h"      /* pc_engine_add_smart_filter */
#include "transform_cmds.h"  /* split_selected, composite_over, group_locks */

#define LIQUIFY "filter.liquify"
#define PUPPET "edit.puppetWarp"
#define PUPPET_SMART "layer.smartObjects.puppetWarp"
#define PERSPECTIVE "edit.perspectiveWarp"
#define PERSPECTIVE_SMART "layer.smartObjects.perspectiveWarp"

static double now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0.0;
    }
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static pc_algo_interp interp_of(yyjson_val *p)
{
    char buf[32] = "bicubic";
    yyjson_val *v = p != NULL ? pc_json_get(p, "interpolation") : NULL;
    if (v != NULL) {
        size_t len = 0;
        const char *s = pc_json_as_str(v, &len);
        if (s != NULL && len < sizeof buf) {
            memcpy(buf, s, len);
            buf[len] = '\0';
        }
    }
    return pc_algo_interp_parse(buf);
}

static pc_pixel_format with_alpha(pc_pixel_format f)
{
    f.alpha = true;
    return f;
}

/* ---------- params ---------- */

/* Liquify params: the strokes and the field resolution. *strokes / *nstrokes owned on success
 * (pc_algo_liquify_strokes_free). */
static pc_status liquify_params(const char *cmd, yyjson_val *p, pc_rect canvas,
                                pc_algo_liquify_stroke **strokes, size_t *nstrokes, double *cell,
                                pc_error *err)
{
    *strokes = NULL;
    *nstrokes = 0;
    yyjson_val *js = p != NULL ? pc_json_get(p, "strokes") : NULL;
    if (js != NULL) {
        pc_error se = {0};
        if (pc_algo_liquify_strokes_from_json(js, strokes, nstrokes, &se) != PC_OK) {
            return pc_engine_err_bad_params(err, cmd, "bad `strokes`: %s", se.msg);
        }
    }
    for (size_t i = 0; i < *nstrokes; i++) {
        pc_error ve = {0};
        if (pc_algo_liquify_stroke_validate(&(*strokes)[i], &ve) != PC_OK) {
            pc_status rc = pc_engine_err_bad_params(err, cmd, "%s", ve.msg);
            pc_algo_liquify_strokes_free(*strokes, *nstrokes);
            *strokes = NULL;
            *nstrokes = 0;
            return rc;
        }
    }
    yyjson_val *jm = p != NULL ? pc_json_get(p, "meshSize") : NULL;
    if (jm == NULL || yyjson_is_null(jm)) {
        *cell = pc_algo_liquify_auto_cell(canvas);
    } else {
        double c = 0.0;
        if (!pc_json_as_f64(jm, &c)) {
            pc_algo_liquify_strokes_free(*strokes, *nstrokes);
            *strokes = NULL;
            *nstrokes = 0;
            return pc_engine_err_bad_params(err, cmd, "`meshSize` is px per field node (1..64)");
        }
        if (!(c >= 1.0 && c <= 64.0)) {
            pc_algo_liquify_strokes_free(*strokes, *nstrokes);
            *strokes = NULL;
            *nstrokes = 0;
            return pc_engine_err_bad_params(err, cmd, "`meshSize` must be 1..64");
        }
        *cell = c;
    }
    return PC_OK;
}

/* Puppet Warp params. *out owned on success (pc_algo_puppet_warp_fini). */
static pc_status puppet_params(const char *cmd, yyjson_val *p, pc_algo_puppet_warp *out, pc_error *err)
{
    pc_error se = {0};
    if (pc_algo_puppet_warp_from_json(p, out, &se) != PC_OK) {
        return pc_engine_err_bad_params(err, cmd, "bad `pins` (each {\"src\":[x,y],\"dst\":[x,y],"
                                                   "\"rotate\":deg?,\"depth\":n?}): %s",
                                        se.msg);
    }
    for (size_t i = 0; i < out->npins; i++) {
        const pc_algo_puppet_pin *q = &out->pins[i];
        bool finite = isfinite(q->src[0]) && isfinite(q->src[1]) && isfinite(q->dst[0]) &&
                      isfinite(q->dst[1]) && (!q->rotate.set || isfinite(q->rotate.deg));
        if (!finite) {
            pc_algo_puppet_warp_fini(out);
            return pc_engine_err_bad_params(err, cmd, "pin coordinates must be finite");
        }
    }
    if (!isfinite(out->expansion) || fabs(out->expansion) > 200.0) {
        pc_algo_puppet_warp_fini(out);
        return pc_engine_err_bad_params(err, cmd, "`expansion` must be -200..200 px");
    }
    return PC_OK;
}

/* Parse one [x,y] pair of finite numbers. */
static bool pair_of(yyjson_val *v, double out[2])
{
    if (v == NULL || !yyjson_is_arr(v) || yyjson_arr_size(v) != 2) {
        return false;
    }
    if (!pc_json_as_f64(yyjson_arr_get(v, 0), &out[0]) || !pc_json_as_f64(yyjson_arr_get(v, 1), &out[1])) {
        return false;
    }
    return isfinite(out[0]) && isfinite(out[1]);
}

/* Parse a quad of 4 [x,y] pairs into q[4][2]. */
static bool quad_of(yyjson_val *v, double q[4][2])
{
    if (v == NULL || !yyjson_is_arr(v) || yyjson_arr_size(v) != 4) {
        return false;
    }
    for (size_t i = 0; i < 4; i++) {
        if (!pair_of(yyjson_arr_get(v, i), q[i])) {
            return false;
        }
    }
    return true;
}

/* Perspective Warp params (planes, after the optional straighten). *planes / *n owned on success
 * (free()). */
static pc_status perspective_params(const char *cmd, yyjson_val *p, pc_algo_persp_plane **planes,
                                    size_t *n, pc_error *err)
{
    *planes = NULL;
    *n = 0;
    yyjson_val *jp = p != NULL ? pc_json_get(p, "planes") : NULL;
    if (jp == NULL) {
        return pc_engine_err_bad_params(err, cmd,
                                        "pass `planes`: [{\"src\":[[x,y]x4],\"dst\":[[x,y]x4]}]");
    }
    if (!yyjson_is_arr(jp)) {
        return pc_engine_err_bad_params(err, cmd, "bad `planes` (each {\"src\":[4x[x,y]],"
                                                  "\"dst\":[4x[x,y]]}): not an array");
    }
    size_t cnt = yyjson_arr_size(jp);
    if (cnt == 0) {
        return pc_engine_err_bad_params(err, cmd, "`planes` is empty");
    }
    pc_algo_persp_plane *arr = calloc(cnt, sizeof *arr);
    if (arr == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "perspective warp");
    }
    for (size_t i = 0; i < cnt; i++) {
        yyjson_val *e = yyjson_arr_get(jp, i);
        yyjson_val *jsrc = e != NULL ? pc_json_get(e, "src") : NULL;
        yyjson_val *jdst = e != NULL ? pc_json_get(e, "dst") : NULL;
        if (!quad_of(jsrc, arr[i].src) || !quad_of(jdst, arr[i].dst)) {
            free(arr);
            return pc_engine_err_bad_params(err, cmd, "bad `planes` (each {\"src\":[4x[x,y]],"
                                                      "\"dst\":[4x[x,y]]}): corners must be finite");
        }
    }
    yyjson_val *jstr = p != NULL ? pc_json_get(p, "straighten") : NULL;
    if (jstr != NULL) {
        size_t slen = 0;
        const char *ss = pc_json_as_str(jstr, &slen);
        pc_algo_persp_straighten mode;
        if (ss == NULL || !pc_algo_persp_straighten_parse(ss, slen, &mode)) {
            free(arr);
            return pc_engine_err_bad_params(err, cmd,
                                            "unknown straighten (horizontal|vertical|auto)");
        }
        if (pc_algo_persp_straighten_planes(arr, cnt, mode, err) != PC_OK) {
            free(arr);
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
    }
    pc_algo_persp_map *m = NULL;
    pc_error me = {0};
    if (pc_algo_persp_map_new(arr, cnt, &m, &me) != PC_OK) {
        free(arr);
        return pc_engine_err_bad_params(err, cmd,
                                        "a plane is degenerate (its corners must form a quad)");
    }
    pc_algo_persp_map_free(m);
    *planes = arr;
    *n = cnt;
    return PC_OK;
}

/* planes -> JSON array of {src,dst}. NULL on NOMEM. */
static yyjson_mut_val *planes_to_json(yyjson_mut_doc *d, const pc_algo_persp_plane *planes, size_t n)
{
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    if (arr == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        yyjson_mut_val *o = yyjson_mut_obj(d);
        yyjson_mut_val *js = yyjson_mut_arr(d), *jd = yyjson_mut_arr(d);
        if (o == NULL || js == NULL || jd == NULL) {
            return NULL;
        }
        for (size_t c = 0; c < 4; c++) {
            yyjson_mut_val *ps = yyjson_mut_arr(d), *pd = yyjson_mut_arr(d);
            if (ps == NULL || pd == NULL) {
                return NULL;
            }
            if (!yyjson_mut_arr_add_real(d, ps, planes[i].src[c][0]) ||
                !yyjson_mut_arr_add_real(d, ps, planes[i].src[c][1]) ||
                !yyjson_mut_arr_add_real(d, pd, planes[i].dst[c][0]) ||
                !yyjson_mut_arr_add_real(d, pd, planes[i].dst[c][1])) {
                return NULL;
            }
            if (!yyjson_mut_arr_append(js, ps) || !yyjson_mut_arr_append(jd, pd)) {
                return NULL;
            }
        }
        yyjson_mut_obj_add_val(d, o, "src", js);
        yyjson_mut_obj_add_val(d, o, "dst", jd);
        if (!yyjson_mut_arr_append(arr, o)) {
            return NULL;
        }
    }
    return arr;
}

/* ---------- pixel operations ---------- */

/* Mixes `new` over `old` by the selection's coverage (premultiplied when there is alpha);
 * distort_cmds::mix_by_selection. *out initialised on success. */
static pc_status mix_by_selection(const pc_surface *old, const pc_surface *neu, const pc_surface *sel,
                                  pc_surface *out, pc_error *err)
{
    pc_status rc = pc_surface_clone(neu, out, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_rect area = pc_rect_union(pc_surface_content_bounds(old), pc_surface_content_bounds(neu));
    if (pc_rect_is_empty(area)) {
        return PC_OK;
    }
    pc_pixel_format fmt = pc_surface_format(old);
    size_t n = pc_pixel_format_channels(fmt);
    if (n == 0) {
        return PC_OK;
    }
    float *o = NULL, *w = NULL, *r = NULL;
    size_t ol = 0, wl = 0;
    rc = pc_engine_alloc_read_region(old, area, "liquify selection mix", &o, &ol, err);
    if (rc != PC_OK) {
        goto done;
    }
    rc = pc_engine_alloc_read_region(neu, area, "liquify selection mix", &w, &wl, err);
    if (rc != PC_OK) {
        goto done;
    }
    rc = pc_engine_alloc_filled(ol, sizeof(float), "liquify selection mix", (void **)&r, err);
    if (rc != PC_OK) {
        goto done;
    }
    uint32_t aw = pc_rect_width(area);
    size_t count = ol / n;
    for (size_t i = 0; i < count; i++) {
        int32_t x = area.x0 + (int32_t)(i % aw), y = area.y0 + (int32_t)(i / aw);
        float k = pc_surface_sample_channel(sel, x, y, 0);
        k = k < 0.0f ? 0.0f : (k > 1.0f ? 1.0f : k);
        const float *op = &o[i * n], *np = &w[i * n];
        float *rp = &r[i * n];
        if (!fmt.alpha) {
            for (size_t c = 0; c < n; c++) {
                rp[c] = op[c] + (np[c] - op[c]) * k;
            }
            continue;
        }
        size_t a = n - 1;
        float oa = op[a] + (np[a] - op[a]) * k;
        rp[a] = oa;
        for (size_t c = 0; c < a; c++) {
            float pm = op[c] * op[a] + (np[c] * np[a] - op[c] * op[a]) * k;
            float val = oa > 0.0f ? pm / oa : 0.0f;
            rp[c] = val < 0.0f ? 0.0f : (val > 1.0f ? 1.0f : val);
        }
    }
    rc = pc_surface_write_region(out, area, r, ol, err);
    if (rc == PC_OK) {
        pc_surface_prune(out);
    }
done:
    free(o);
    free(w);
    free(r);
    if (rc != PC_OK) {
        pc_surface_fini(out);
    }
    return rc;
}

/* Puppet-warps a surface's content. PC_ERR_LIMIT when the result would be too large. */
static pc_status puppet_surface(const pc_surface *surf, const pc_algo_puppet_warp *w,
                                pc_algo_interp interp, pc_surface *out, pc_error *err)
{
    pc_rect b = pc_surface_content_bounds(surf);
    if (pc_algo_puppet_warp_is_identity(w) || pc_rect_is_empty(b)) {
        return pc_surface_clone(surf, out, err);
    }
    return pc_algo_puppet_apply(surf, b, w, interp, out, err);
}

/* Forward map ctx for warp_mesh_surface / warp_mesh_gray. */
static void persp_map_fn(void *ctx, double x, double y, double *ox, double *oy)
{
    pc_algo_persp_map_apply((const pc_algo_persp_map *)ctx, x, y, ox, oy);
}

/* Perspective-warps a surface's content. Never fails for a non-degenerate map. */
static pc_status perspective_surface(const pc_surface *surf, const pc_algo_persp_plane *planes,
                                     size_t n, pc_algo_interp interp, pc_surface *out, pc_error *err)
{
    pc_rect b = pc_surface_content_bounds(surf);
    if (pc_algo_persp_is_identity(planes, n) || pc_rect_is_empty(b)) {
        return pc_surface_clone(surf, out, err);
    }
    pc_algo_persp_map *m = NULL;
    pc_error me = {0};
    if (pc_algo_persp_map_new(planes, n, &m, &me) != PC_OK) {
        return pc_surface_clone(surf, out, err); /* degenerate: leave it (reference's None) */
    }
    pc_status rc = pc_algo_warp_mesh_surface(surf, b, persp_map_fn, m, interp, out, err);
    pc_algo_persp_map_free(m);
    return rc;
}

/* ---------- smart-filter re-apply hook (distort_cmds::apply_to_surface) ---------- */

pc_status pc_engine_distort_apply_to_surface(const char *id, yyjson_val *params, const pc_surface *surf,
                                             pc_rect canvas, pc_surface *out, bool *applied, pc_error *err)
{
    *applied = false;
    if (id == NULL || surf == NULL || out == NULL) {
        return PC_OK;
    }
    pc_error ignore = {0};
    if (strcmp(id, LIQUIFY) == 0) {
        pc_algo_liquify_stroke *strokes = NULL;
        size_t ns = 0;
        double cell = 0.0;
        if (liquify_params(id, params, canvas, &strokes, &ns, &cell, &ignore) != PC_OK) {
            return PC_OK; /* parse error: skip (reference .ok()?) */
        }
        pc_algo_liquify_field field;
        pc_status rc = pc_algo_liquify_field_from_strokes(canvas, cell, strokes, ns, &field, &ignore);
        pc_algo_liquify_strokes_free(strokes, ns);
        if (rc != PC_OK) {
            return PC_OK;
        }
        rc = pc_algo_liquify_apply(surf, &field, out, &ignore);
        pc_algo_liquify_field_fini(&field);
        if (rc != PC_OK) {
            return PC_OK;
        }
        *applied = true;
        return PC_OK;
    }
    if (strcmp(id, PUPPET) == 0 || strcmp(id, PUPPET_SMART) == 0) {
        pc_algo_puppet_warp w;
        if (puppet_params(id, params, &w, &ignore) != PC_OK) {
            return PC_OK;
        }
        pc_status rc = puppet_surface(surf, &w, interp_of(params), out, &ignore);
        pc_algo_puppet_warp_fini(&w);
        if (rc != PC_OK) {
            /* too far to render: leave the smart object as it is (#1019) */
            rc = pc_surface_clone(surf, out, err);
            if (rc != PC_OK) {
                return rc;
            }
        }
        *applied = true;
        return PC_OK;
    }
    if (strcmp(id, PERSPECTIVE) == 0 || strcmp(id, PERSPECTIVE_SMART) == 0) {
        pc_algo_persp_plane *planes = NULL;
        size_t n = 0;
        if (perspective_params(id, params, &planes, &n, &ignore) != PC_OK) {
            return PC_OK;
        }
        pc_status rc = perspective_surface(surf, planes, n, interp_of(params), out, &ignore);
        free(planes);
        if (rc != PC_OK) {
            return PC_OK;
        }
        *applied = true;
        return PC_OK;
    }
    return PC_OK;
}


/* ---------- commands ---------- */

static const pc_document *active_doc(const pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    return d != NULL ? pc_doc_arc_get(d->doc) : NULL;
}

/* Resolve the target layer id (the `layer` param, else the active layer). */
static pc_status target(const pc_engine_session *s, yyjson_val *p, pc_layer_id *out, pc_error *err)
{
    yyjson_val *jl = p != NULL ? pc_json_get(p, "layer") : NULL;
    uint64_t v = 0;
    if (jl != NULL && pc_json_as_u64(jl, &v)) {
        *out = v;
        return PC_OK;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL || !d->has_active_layer) {
        return pc_engine_err_other(err, "no active layer");
    }
    *out = d->active_layer;
    return PC_OK;
}

static bool is_smart_layer(const pc_engine_session *s, pc_layer_id id)
{
    const pc_document *doc = active_doc(s);
    if (doc == NULL) {
        return false;
    }
    const pc_layer *l = pc_document_layer(doc, id);
    return l != NULL && l->content.kind == PC_LAYER_SMART;
}

/* `enabled` for Puppet/Perspective (non-smart): a pixel layer or a smart object with a cache. */
static pc_status pixel_layer(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (!d->has_active_layer || doc == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    const pc_layer *l = pc_document_layer(doc, d->active_layer);
    if (l == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    if (l->content.kind == PC_LAYER_RASTER) {
        return PC_OK;
    }
    if (l->content.kind == PC_LAYER_SMART && l->content.u.smart.has_cache) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID,
                   "needs a pixel layer or smart object (active layer is %s %s layer)",
                   pc_layer_content_article(&l->content), pc_layer_content_kind_name(&l->content));
}

/* `enabled` for the smart-object twins: the active layer must be a smart object. */
static pc_status smart_layer(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = d != NULL ? pc_doc_arc_get(d->doc) : NULL;
    const pc_layer *l = (doc != NULL && d->has_active_layer) ? pc_document_layer(doc, d->active_layer)
                                                             : NULL;
    if (l == NULL || l->content.kind != PC_LAYER_SMART) {
        return pc_fail(reason, PC_ERR_INVALID, "the active layer is not a smart object");
    }
    return PC_OK;
}

static pc_status check_locks(const pc_layer *l, pc_locks group, bool position, pc_error *err)
{
    pc_locks locks = pc_locks_union(l->locks, group);
    if (locks.all || locks.pixels || (position && locks.position)) {
        return pc_engine_err_other(
            err, "Could not complete your request because the layer \"%.*s\" is locked",
            (int)l->name.len, l->name.ptr);
    }
    return PC_OK;
}

/* A raster deform of lifted/whole pixels: in -> out (deform_selected's inner f). */
typedef pc_status (*inner_fn)(void *ctx, const pc_surface *in, pc_surface *out, pc_error *err);

/* Lifts the selected pixels (when there is a selection), deforms them with `f` and puts them back
 * over the rest; without a selection deforms the whole surface (distort_cmds::deform_selected). */
static pc_status deform_selected(const pc_surface *surf, const pc_surface *sel, inner_fn f, void *fctx,
                                 pc_surface *out, pc_error *err)
{
    if (sel != NULL) {
        pc_surface lifted, rest, moved;
        pc_status rc = pc_engine_transform_split_selected(surf, sel, &lifted, &rest, err);
        if (rc != PC_OK) {
            return rc;
        }
        rc = f(fctx, &lifted, &moved, err);
        pc_surface_fini(&lifted);
        if (rc != PC_OK) {
            pc_surface_fini(&rest);
            return rc;
        }
        rc = pc_engine_transform_composite_over_native(&rest, &moved, err);
        pc_surface_fini(&moved);
        if (rc != PC_OK) {
            pc_surface_fini(&rest);
            return rc;
        }
        pc_surface_prune(&rest);
        *out = rest;
        return PC_OK;
    }
    pc_pixel_format fmt = pc_surface_format(surf);
    if (fmt.alpha) {
        return f(fctx, surf, out, err);
    }
    pc_surface src;
    pc_status rc = pc_surface_convert(surf, with_alpha(fmt), &src, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = f(fctx, &src, out, err);
    pc_surface_fini(&src);
    return rc;
}

/* A raster-layer deform (whole layer surface + the document selection). */
typedef pc_status (*raster_fn)(void *ctx, const pc_surface *surf, const pc_surface *sel,
                               pc_surface *out, pc_error *err);
/* A linked-mask deform (the mask surface + the document selection). */
typedef pc_status (*mask_fn)(void *ctx, const pc_surface *m, const pc_surface *sel, pc_surface *out,
                             pc_error *err);

/* The shared driver (distort_cmds::run_on_layer). On a smart object records `cmd` as a smart
 * filter; on a pixel layer edits the pixels with `rf` (and a linked mask with `mf`, when present). */
struct run_ctx {
    const char *cmd;
    yyjson_val *params;
    bool moves;
    pc_layer_id id;
    raster_fn rf;
    void *rf_ctx;
    mask_fn mf;
    void *mf_ctx;
};

static pc_status run_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    (void)active;
    struct run_ctx *c = vctx;
    pc_layer_id id = c->id;
    const pc_surface *selection = doc->has_selection ? &doc->selection : NULL;
    pc_locks group = pc_engine_transform_group_locks(doc, id);
    if (c->moves) {
        pc_engine_background_to_layer(doc, id);
    }
    pc_layer *l = pc_document_layer_mut(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    if (l->content.kind == PC_LAYER_SMART) {
        pc_status rc = check_locks(l, group, false, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_smart_filter sf;
        memset(&sf, 0, sizeof sf);
        rc = pc_str_set(&sf.command, c->cmd, strlen(c->cmd), err);
        if (rc == PC_OK) {
            rc = pc_json_value_from_val(c->params, &sf.params, err);
        }
        if (rc == PC_OK) {
            yyjson_mut_val *root = pc_json_value_root(&sf.params);
            if (root != NULL && yyjson_mut_is_obj(root)) {
                yyjson_mut_obj_remove_key(root, "layer");
            }
        }
        if (rc != PC_OK) {
            pc_smart_filter_fini(&sf);
            return rc;
        }
        sf.blend = PC_BLEND_NORMAL;
        sf.opacity = 1.0f;
        sf.visible = true;
        return pc_engine_add_smart_filter(doc, id, &sf, selection, err);
    }
    pc_status rc = check_locks(l, group, c->moves, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (l->content.kind != PC_LAYER_RASTER) {
        return pc_engine_err_other(err, "%s needs a pixel layer (rasterize it first)",
                                   c->cmd[0] == 'f' ? "Liquify" : "Warp");
    }
    const pc_surface *surf = &l->content.u.raster;
    pc_surface newsurf;
    rc = c->rf(c->rf_ctx, surf, selection, &newsurf, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_surface_fini(&l->content.u.raster);
    l->content.u.raster = newsurf;
    if (c->mf != NULL && l->has_mask && l->mask.linked) {
        pc_surface newmask;
        rc = c->mf(c->mf_ctx, &l->mask.surface, selection, &newmask, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_surface_fini(&l->mask.surface);
        l->mask.surface = newmask;
    }
    return PC_OK;
}

/* Build {"layer": id, "changed": bool}; *rd_out / *root_out let the caller add fields. */
static pc_status changed_json(pc_layer_id id, bool changed, yyjson_mut_doc **rd_out,
                              yyjson_mut_val **root_out, pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", id);
    yyjson_mut_obj_add_bool(rd, root, "changed", changed);
    yyjson_mut_doc_set_root(rd, root);
    if (rd_out != NULL) {
        *rd_out = rd;
    }
    if (root_out != NULL) {
        *root_out = root;
    }
    return PC_OK;
}

/* ---- Liquify ---- */

static pc_status liquify_raster(void *ctx, const pc_surface *surf, const pc_surface *sel,
                                pc_surface *out, pc_error *err)
{
    const pc_algo_liquify_field *field = ctx;
    pc_surface applied;
    pc_status rc = pc_algo_liquify_apply(surf, field, &applied, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (sel != NULL) {
        rc = mix_by_selection(surf, &applied, sel, out, err);
        pc_surface_fini(&applied);
        return rc;
    }
    *out = applied;
    return PC_OK;
}

static pc_status run_liquify(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_document *doc = active_doc(s);
    if (doc == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_rect canvas = pc_document_bounds(doc);
    pc_algo_liquify_stroke *strokes = NULL;
    size_t ns = 0;
    double cell = 0.0;
    pc_status rc = liquify_params(LIQUIFY, p, canvas, &strokes, &ns, &cell, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_layer_id id = 0;
    rc = target(s, p, &id, err);
    if (rc != PC_OK) {
        pc_algo_liquify_strokes_free(strokes, ns);
        return rc;
    }
    if (ns == 0) {
        pc_algo_liquify_strokes_free(strokes, ns);
        return changed_json(id, false, NULL, NULL, out, err);
    }
    double t0 = now_ms();
    pc_algo_liquify_field field;
    rc = pc_algo_liquify_field_from_strokes(canvas, cell, strokes, ns, &field, err);
    pc_algo_liquify_strokes_free(strokes, ns);
    if (rc != PC_OK) {
        return rc;
    }
    bool smart = is_smart_layer(s, id);
    if (pc_algo_liquify_is_identity(&field) && !smart) {
        pc_algo_liquify_field_fini(&field);
        return changed_json(id, false, NULL, NULL, out, err);
    }
    float maxd = pc_algo_liquify_max_displacement(&field);
    struct run_ctx ctx = {LIQUIFY, p, false, id, liquify_raster, &field, NULL, NULL};
    rc = pc_engine_session_edit(s, "Liquify", run_edit, &ctx, err);
    pc_algo_liquify_field_fini(&field);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    yyjson_mut_val *root = NULL;
    rc = changed_json(id, true, &rd, &root, out, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_obj_add_real(rd, root, "ms", now_ms() - t0);
    yyjson_mut_obj_add_real(rd, root, "maxDisplacement", (double)maxd);
    return PC_OK;
}

/* ---- Puppet Warp ---- */

struct puppet_lf {
    const pc_algo_puppet_warp *w;
    pc_algo_interp interp;
    const char *cmd;
};

static pc_status puppet_inner(void *ctx, const pc_surface *in, pc_surface *out, pc_error *err)
{
    struct puppet_lf *c = ctx;
    pc_status rc = puppet_surface(in, c->w, c->interp, out, err);
    if (rc == PC_ERR_LIMIT) {
        return pc_engine_err_bad_params(err, c->cmd,
                                        "the pins stretch the content too far: the result would "
                                        "cover more than 2^30 pixels");
    }
    return rc;
}

static pc_status puppet_raster(void *ctx, const pc_surface *surf, const pc_surface *sel,
                               pc_surface *out, pc_error *err)
{
    return deform_selected(surf, sel, puppet_inner, ctx, out, err);
}

static pc_status run_puppet(pc_engine_session *s, yyjson_val *p, const char *cmd, pc_json_value *out,
                            pc_error *err)
{
    pc_algo_puppet_warp w;
    pc_status rc = puppet_params(cmd, p, &w, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_layer_id id = 0;
    rc = target(s, p, &id, err);
    if (rc != PC_OK) {
        pc_algo_puppet_warp_fini(&w);
        return rc;
    }
    bool smart = is_smart_layer(s, id);
    if (pc_algo_puppet_warp_is_identity(&w) && !smart) {
        pc_algo_puppet_warp_fini(&w);
        return changed_json(id, false, NULL, NULL, out, err);
    }
    struct puppet_lf lf = {&w, interp_of(p), cmd};
    struct run_ctx ctx = {cmd, p, true, id, puppet_raster, &lf, NULL, NULL};
    rc = pc_engine_session_edit(s, "Puppet Warp", run_edit, &ctx, err);
    pc_algo_puppet_warp_fini(&w);
    if (rc != PC_OK) {
        return rc;
    }
    return changed_json(id, true, NULL, NULL, out, err);
}

/* ---- Perspective Warp ---- */

struct persp_lf {
    const pc_algo_persp_plane *planes;
    size_t n;
    pc_algo_interp interp;
    pc_algo_persp_map *map; /* for the linked-mask follow (NULL = degenerate/identity) */
};

static pc_status persp_inner(void *ctx, const pc_surface *in, pc_surface *out, pc_error *err)
{
    struct persp_lf *c = ctx;
    return perspective_surface(in, c->planes, c->n, c->interp, out, err);
}

static pc_status persp_raster(void *ctx, const pc_surface *surf, const pc_surface *sel,
                              pc_surface *out, pc_error *err)
{
    return deform_selected(surf, sel, persp_inner, ctx, out, err);
}

/* Linked-mask follow (distort_cmds perspective mask_fn via move_gray_selected / warp_gray). */
static pc_status persp_mask_fn(void *ctx, const pc_surface *m, const pc_surface *sel, pc_surface *out,
                               pc_error *err)
{
    struct persp_lf *c = ctx;
    if (c->map == NULL) {
        return pc_surface_clone(m, out, err);
    }
    pc_pixel_format fmt = pc_surface_format(m);
    bool gray = !fmt.alpha && pc_pixel_format_channels(fmt) == 1;
    if (sel == NULL || !gray) {
        return pc_algo_warp_mesh_gray(m, persp_map_fn, c->map, PC_ALGO_INTERP_BILINEAR, out, err);
    }
    float def;
    {
        float d[PC_RASTER_MAX_CHANNELS];
        pc_surface_default_pixel(m, d);
        def = d[0];
    }
    pc_pixel_format wa = with_alpha(fmt);
    pc_rect area = pc_surface_content_bounds(sel);
    pc_status rc = pc_surface_clone(m, out, err); /* `rest` */
    if (rc != PC_OK) {
        return rc;
    }
    if (pc_rect_is_empty(area)) {
        return PC_OK;
    }
    float *v = NULL, *lp = NULL, *rp = NULL;
    size_t vlen = 0;
    pc_surface tmp, warped = {0};
    bool have_tmp = false, have_warp = false;
    rc = pc_engine_alloc_read_region(m, area, "perspective mask", &v, &vlen, err);
    if (rc != PC_OK) {
        goto done;
    }
    rc = pc_engine_alloc_filled(vlen * 2, sizeof(float), "perspective mask", (void **)&lp, err);
    if (rc != PC_OK) {
        goto done;
    }
    rc = pc_engine_alloc_filled(vlen, sizeof(float), "perspective mask", (void **)&rp, err);
    if (rc != PC_OK) {
        goto done;
    }
    {
        uint32_t w = pc_rect_width(area);
        for (size_t i = 0; i < vlen; i++) {
            int32_t x = area.x0 + (int32_t)(i % w), y = area.y0 + (int32_t)(i / w);
            float k = pc_surface_sample_channel(sel, x, y, 0);
            lp[i * 2] = v[i];
            lp[i * 2 + 1] = k;
            rp[i] = v[i] * (1.0f - k) + def * k;
        }
    }
    pc_surface_init(&tmp, wa);
    have_tmp = true;
    rc = pc_surface_write_region(&tmp, area, lp, vlen * 2, err);
    if (rc == PC_OK) {
        pc_surface_prune(&tmp);
        rc = pc_surface_write_region(out, area, rp, vlen, err);
    }
    if (rc == PC_OK) {
        pc_surface_prune(out);
        pc_rect src2 = pc_surface_content_bounds(&tmp);
        if (!pc_rect_is_empty(src2)) {
            rc = pc_algo_warp_mesh_surface(&tmp, src2, persp_map_fn, c->map, PC_ALGO_INTERP_BILINEAR,
                                           &warped, err);
            have_warp = (rc == PC_OK);
        }
    }
    if (rc != PC_OK) {
        goto done;
    }
    if (have_warp) {
        pc_rect b = pc_surface_content_bounds(&warped);
        if (!pc_rect_is_empty(b)) {
            float *moved = NULL, *under = NULL, *flat = NULL;
            size_t ml = 0, ul = 0;
            rc = pc_engine_alloc_read_region(&warped, b, "perspective mask", &moved, &ml, err);
            if (rc == PC_OK) {
                rc = pc_engine_alloc_read_region(out, b, "perspective mask", &under, &ul, err);
            }
            if (rc == PC_OK) {
                rc = pc_engine_alloc_filled(ul, sizeof(float), "perspective mask", (void **)&flat, err);
            }
            if (rc == PC_OK) {
                for (size_t i = 0; i < ul; i++) {
                    flat[i] = moved[i * 2] * moved[i * 2 + 1] + under[i] * (1.0f - moved[i * 2 + 1]);
                }
                rc = pc_surface_write_region(out, b, flat, ul, err);
            }
            free(moved);
            free(under);
            free(flat);
        }
    }
    if (rc == PC_OK) {
        pc_surface_prune(out);
    }
done:
    free(v);
    free(lp);
    free(rp);
    if (have_tmp) {
        pc_surface_fini(&tmp);
    }
    if (have_warp) {
        pc_surface_fini(&warped);
    }
    if (rc != PC_OK) {
        pc_surface_fini(out);
    }
    return rc;
}

static pc_status run_perspective(pc_engine_session *s, yyjson_val *p, const char *cmd,
                                 pc_json_value *out, pc_error *err)
{
    pc_algo_persp_plane *planes = NULL;
    size_t n = 0;
    pc_status rc = perspective_params(cmd, p, &planes, &n, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_layer_id id = 0;
    rc = target(s, p, &id, err);
    if (rc != PC_OK) {
        free(planes);
        return rc;
    }
    bool smart = is_smart_layer(s, id);
    bool identity = pc_algo_persp_is_identity(planes, n);
    if (identity && !smart) {
        yyjson_mut_doc *rd = NULL;
        yyjson_mut_val *root = NULL;
        rc = changed_json(id, false, &rd, &root, out, err);
        if (rc == PC_OK) {
            yyjson_mut_val *pj = planes_to_json(rd, planes, n);
            yyjson_mut_obj_add_val(rd, root, "planes", pj != NULL ? pj : yyjson_mut_null(rd));
        }
        free(planes);
        return rc;
    }
    pc_algo_persp_map *map = NULL;
    pc_error me = {0};
    (void)pc_algo_persp_map_new(planes, n, &map, &me); /* NULL on degenerate; mask follow then skips */
    struct persp_lf lf = {planes, n, interp_of(p), map};
    struct run_ctx ctx = {cmd, p, true, id, persp_raster, &lf, persp_mask_fn, &lf};
    rc = pc_engine_session_edit(s, "Perspective Warp", run_edit, &ctx, err);
    pc_algo_persp_map_free(map);
    if (rc != PC_OK) {
        free(planes);
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    yyjson_mut_val *root = NULL;
    rc = changed_json(id, true, &rd, &root, out, err);
    if (rc == PC_OK) {
        yyjson_mut_val *pj = planes_to_json(rd, planes, n);
        yyjson_mut_obj_add_val(rd, root, "planes", pj != NULL ? pj : yyjson_mut_null(rd));
    }
    free(planes);
    return rc;
}

/* run() thunks */
static pc_status run_puppet_edit(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return run_puppet(s, p, PUPPET, out, err);
}
static pc_status run_puppet_smart(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return run_puppet(s, p, PUPPET_SMART, out, err);
}
static pc_status run_persp_edit(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return run_perspective(s, p, PERSPECTIVE, out, err);
}
static pc_status run_persp_smart(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return run_perspective(s, p, PERSPECTIVE_SMART, out, err);
}

/* ---------- registry ---------- */

static const char *const MENU_FILTER[] = {"Filter"};
static const char *const MENU_EDIT[] = {"Edit"};
static const char *const MENU_SMART[] = {"Layer", "Smart Objects"};

#define L_PARAMS                                                                                   \
    "{\"strokes\":[{\"tool\":\"forwardWarp|reconstruct|smooth|twirlCw|twirlCcw|pucker|bloat|"      \
    "pushLeft|freeze|thaw|lassoMask|reconstructAll\",\"size\":px=100,\"density\":0-100=50,"        \
    "\"pressure\":0-100=100,\"rate\":0-100=80,\"points\":[[x,y,pressure?]...]}...],"               \
    "\"meshSize\":px?,\"layer\":id?}"
#define P_PARAMS                                                                                   \
    "{\"pins\":[{\"src\":[x,y],\"dst\":[x,y],\"rotate\":deg?,\"depth\":n?}...],"                    \
    "\"mode\":\"rigid|normal|distort\"=\"normal\",\"density\":\"fewer|normal|more\"=\"normal\","   \
    "\"expansion\":px=2,\"interpolation\":\"bicubic|bilinear|nearest\",\"layer\":id?}"
#define Q_PARAMS                                                                                   \
    "{\"planes\":[{\"src\":[[x,y]x4],\"dst\":[[x,y]x4]}...],"                                       \
    "\"straighten\":\"horizontal|vertical|auto\"?,"                                                \
    "\"interpolation\":\"bicubic|bilinear|nearest\",\"layer\":id?}"

static const pc_engine_command SPECS[] = {
    {LIQUIFY, "Liquify...", MENU_FILTER, 1, "Cmd+Shift+X", L_PARAMS, pc_engine_filterable, run_liquify,
     true},
    {PUPPET, "Puppet Warp", MENU_EDIT, 1, NULL, P_PARAMS, pixel_layer, run_puppet_edit, true},
    {PUPPET_SMART, "Puppet Warp", MENU_SMART, 2, NULL, P_PARAMS, smart_layer, run_puppet_smart, true},
    {PERSPECTIVE, "Perspective Warp", MENU_EDIT, 1, NULL, Q_PARAMS, pixel_layer, run_persp_edit, true},
    {PERSPECTIVE_SMART, "Perspective Warp", MENU_SMART, 2, NULL, Q_PARAMS, smart_layer, run_persp_smart,
     true},
};

const pc_engine_command *pc_engine_distort_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
