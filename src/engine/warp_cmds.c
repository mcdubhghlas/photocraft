/* Edit > Transform > Warp, Layer > Smart Objects > Warp, the split warps and the warp grid, plus
 * the warp primitives (port of crates/engine/src/warp_cmds.rs, E9 / E-tail warp commands).
 *
 * A warp is a pc_warp (preset style + bend/distortions, or a custom bicubic Bezier mesh) defined
 * over a source box in document space. Pixel layers are resampled once (pc_algo_warp_mesh_surface);
 * smart objects store the warp in their source space and re-render from the source, so editing a
 * smart object's warp never degrades it. The split/grid commands edit a mesh: either one passed in
 * ("warp", returned edited -- what the Free Transform warp UI uses) or the active smart object's
 * stored warp (a history step).
 *
 * Never crashes (REWRITE_PLAN.md rule 9): every param is validated, every bound checked before
 * indexing/dividing/allocating, and a result past MAX_WARP_SIDE is refused, not clipped.
 *
 * The one deviation from the reference, documented in REWRITE_PLAN.md §7: a selection does not yet
 * limit a *linked mask's* warp to the selected region (#4175); the whole linked mask follows the
 * warp. The pixels themselves honour the selection exactly. */
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "algo/params.h"
#include "algo/transform.h"
#include "algo/warp.h"
#include "base/json.h"
#include "base/str.h"
#include "color/color.h"
#include "doc/doc.h"
#include "doc/text.h"
#include "doc/vector.h"
#include "geom/geom.h"
#include "geom/warp.h"
#include "raster/surface.h"

#include "engine_internal.h"
#include "extra_cmds.h"     /* pc_engine_background_to_layer */
#include "smart_cmds.h"     /* placement / refresh */
#include "transform_cmds.h" /* split_selected, composite_over_native, group_locks */
#include "vector_cmds.h"    /* map_path, map_shape */
#include "warp_cmds.h"

/* ---- JSON -> geom (serde Deserialize of geom::warp::{Warp, BezierMesh}, camelCase) ---------- *
 *
 * These sub-parsers write a short message into msg[] on failure (the caller wraps it as the
 * reference does: "bad `warp`: <msg>" / "bad `mesh`: <msg>"). The style field uses the lenient
 * WarpStyle::parse (pc_warp_style_parse), a hair looser than serde's exact variant match for the
 * embedded `warp` object; the happy path and mesh validity are exact (REWRITE_PLAN.md §7). */

static bool num_at(yyjson_val *arr, size_t i, double *out)
{
    yyjson_val *v = yyjson_arr_get(arr, i);
    return v != NULL && pc_json_as_f64(v, out);
}

/* us/vs: an array of finite numbers (copied). */
static double *f64_array(yyjson_val *v, size_t *n)
{
    *n = 0;
    if (v == NULL || !yyjson_is_arr(v)) {
        return NULL;
    }
    size_t len = yyjson_arr_size(v);
    double *a = calloc(len ? len : 1, sizeof *a);
    if (a == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < len; i++) {
        if (!num_at(v, i, &a[i])) {
            free(a);
            return NULL;
        }
    }
    *n = len;
    return a;
}

static bool mesh_json(yyjson_val *v, pc_mesh *out, char *msg, size_t cap)
{
    memset(out, 0, sizeof *out);
    if (v == NULL || !yyjson_is_obj(v)) {
        (void)snprintf(msg, cap, "invalid type: expected a mesh object");
        return false;
    }
    yyjson_val *jus = pc_json_get(v, "us");
    yyjson_val *jvs = pc_json_get(v, "vs");
    yyjson_val *jpts = pc_json_get(v, "points");
    if (jus == NULL) {
        (void)snprintf(msg, cap, "missing field `us`");
        return false;
    }
    if (jvs == NULL) {
        (void)snprintf(msg, cap, "missing field `vs`");
        return false;
    }
    if (jpts == NULL) {
        (void)snprintf(msg, cap, "missing field `points`");
        return false;
    }
    size_t nus = 0, nvs = 0;
    double *us = f64_array(jus, &nus);
    double *vs = f64_array(jvs, &nvs);
    if ((us == NULL && !yyjson_is_arr(jus)) || (vs == NULL && !yyjson_is_arr(jvs)) ||
        (yyjson_is_arr(jus) && us == NULL) || (yyjson_is_arr(jvs) && vs == NULL)) {
        free(us);
        free(vs);
        (void)snprintf(msg, cap, "invalid mesh knots");
        return false;
    }
    if (!yyjson_is_arr(jpts)) {
        free(us);
        free(vs);
        (void)snprintf(msg, cap, "invalid field `points`");
        return false;
    }
    size_t npts = yyjson_arr_size(jpts);
    pc_point *pts = calloc(npts ? npts : 1, sizeof *pts);
    if (pts == NULL) {
        free(us);
        free(vs);
        (void)snprintf(msg, cap, "out of memory");
        return false;
    }
    for (size_t i = 0; i < npts; i++) {
        yyjson_val *pt = yyjson_arr_get(jpts, i);
        if (pt == NULL || !yyjson_is_arr(pt) || yyjson_arr_size(pt) < 2 || !num_at(pt, 0, &pts[i].x) ||
            !num_at(pt, 1, &pts[i].y)) {
            free(us);
            free(vs);
            free(pts);
            (void)snprintf(msg, cap, "invalid control point");
            return false;
        }
    }
    out->us = us;
    out->nus = nus;
    out->vs = vs;
    out->nvs = nvs;
    out->points = pts;
    out->npoints = npts;
    return true;
}

static bool warp_json(yyjson_val *v, pc_warp *out, char *msg, size_t cap)
{
    memset(out, 0, sizeof *out);
    if (v == NULL || !yyjson_is_obj(v)) {
        (void)snprintf(msg, cap, "invalid type: expected a warp object");
        return false;
    }
    yyjson_val *js = pc_json_get(v, "style");
    if (js == NULL) {
        (void)snprintf(msg, cap, "missing field `style`");
        return false;
    }
    size_t slen = 0;
    const char *sstr = pc_json_as_str(js, &slen);
    pc_warp_style style;
    if (sstr == NULL || !pc_warp_style_parse(sstr, slen, &style)) {
        (void)snprintf(msg, cap, "unknown variant for field `style`");
        return false;
    }
    yyjson_val *jb = pc_json_get(v, "bounds");
    double bounds[4];
    if (jb == NULL || !yyjson_is_arr(jb) || yyjson_arr_size(jb) < 4) {
        (void)snprintf(msg, cap, "missing or malformed field `bounds`");
        return false;
    }
    for (size_t i = 0; i < 4; i++) {
        if (!num_at(jb, i, &bounds[i])) {
            (void)snprintf(msg, cap, "malformed field `bounds`");
            return false;
        }
    }
    double bend = 0.0, hd = 0.0, vd = 0.0;
    yyjson_val *t;
    if ((t = pc_json_get(v, "bend")) != NULL && !pc_json_as_f64(t, &bend)) {
        (void)snprintf(msg, cap, "invalid field `bend`");
        return false;
    }
    if ((t = pc_json_get(v, "hDistort")) != NULL && !pc_json_as_f64(t, &hd)) {
        (void)snprintf(msg, cap, "invalid field `hDistort`");
        return false;
    }
    if ((t = pc_json_get(v, "vDistort")) != NULL && !pc_json_as_f64(t, &vd)) {
        (void)snprintf(msg, cap, "invalid field `vDistort`");
        return false;
    }
    bool vertical = false;
    if ((t = pc_json_get(v, "vertical")) != NULL && !pc_json_as_bool(t, &vertical)) {
        (void)snprintf(msg, cap, "invalid field `vertical`");
        return false;
    }
    pc_warp_preset(out, style, bend, bounds);
    out->h_distort = hd;
    out->v_distort = vd;
    out->vertical = vertical;
    yyjson_val *jm = pc_json_get(v, "mesh");
    if (jm != NULL && !yyjson_is_null(jm)) {
        pc_mesh mesh;
        if (!mesh_json(jm, &mesh, msg, cap)) {
            pc_warp_fini(out);
            return false;
        }
        out->mesh = mesh; /* takes ownership */
        out->has_mesh = true;
    }
    return true;
}

static double num_param(yyjson_val *p, const char *k, double d)
{
    yyjson_val *v = pc_json_get(p, k);
    double out;
    if (v != NULL && pc_json_as_f64(v, &out)) {
        return out;
    }
    return d;
}

pc_status pc_engine_warp_from_params(const char *cmd, yyjson_val *p, const double rect[4], pc_warp *out,
                                     pc_error *err)
{
    yyjson_val *wv = pc_json_get(p, "warp");
    if (wv != NULL) {
        char msg[160];
        if (!warp_json(wv, out, msg, sizeof msg)) {
            return pc_engine_err_bad_params(err, cmd, "bad `warp`: %s", msg);
        }
        if (out->has_mesh && !pc_mesh_is_valid(&out->mesh)) {
            pc_warp_fini(out);
            return pc_engine_err_bad_params(err, cmd, "malformed warp mesh");
        }
        return PC_OK;
    }

    pc_warp_style style;
    yyjson_val *jstyle = pc_json_get(p, "style");
    size_t slen = 0;
    /* Like the reference: only a *string* style counts; anything else is treated as absent. */
    const char *sstr = jstyle != NULL ? pc_json_as_str(jstyle, &slen) : NULL;
    bool has_mesh_p = pc_json_get(p, "mesh") != NULL;
    bool has_grid_p = pc_json_get(p, "grid") != NULL;
    if (sstr != NULL) {
        if (!pc_warp_style_parse(sstr, slen, &style)) {
            char tmp[96];
            size_t n = slen < sizeof tmp - 1 ? slen : sizeof tmp - 1;
            memcpy(tmp, sstr, n);
            tmp[n] = '\0';
            return pc_engine_err_bad_params(err, cmd, "unknown warp style \"%s\"", tmp);
        }
    } else if (has_mesh_p || has_grid_p) {
        style = PC_WARP_CUSTOM;
    } else {
        return pc_engine_err_bad_params(err, cmd,
                                        "pass `style` (arc, flag, \xE2\x80\xA6, custom), `mesh` or `warp`");
    }

    pc_warp_preset(out, style, num_param(p, "bend", 50.0), rect);
    out->h_distort = num_param(p, "hDistort", 0.0);
    out->v_distort = num_param(p, "vDistort", 0.0);
    bool vertical = false;
    yyjson_val *jv = pc_json_get(p, "vertical");
    if (jv != NULL) {
        (void)pc_json_as_bool(jv, &vertical);
    }
    out->vertical = vertical;

    if (style == PC_WARP_CUSTOM) {
        pc_mesh mesh;
        yyjson_val *jm = pc_json_get(p, "mesh");
        if (jm != NULL) {
            char msg[160];
            if (!mesh_json(jm, &mesh, msg, sizeof msg)) {
                pc_warp_fini(out);
                return pc_engine_err_bad_params(err, cmd, "bad `mesh`: %s", msg);
            }
            if (!pc_mesh_is_valid(&mesh)) {
                pc_mesh_fini(&mesh);
                pc_warp_fini(out);
                return pc_engine_err_bad_params(
                    err, cmd,
                    "malformed mesh (need (3\xC2\xB7" "cols+1)\xC3\x97(3\xC2\xB7rows+1) points and "
                    "knots 0..1)");
            }
        } else {
            yyjson_val *jg = pc_json_get(p, "grid");
            size_t cols = 1, rows = 1;
            if (jg != NULL && yyjson_is_arr(jg)) {
                /* as_u64().unwrap_or(1): only an unsigned integer counts. */
                yyjson_val *gc = yyjson_arr_get(jg, 0);
                yyjson_val *gr = yyjson_arr_get(jg, 1);
                if (gc != NULL && yyjson_is_uint(gc)) {
                    cols = (size_t)yyjson_get_uint(gc);
                }
                if (gr != NULL && yyjson_is_uint(gr)) {
                    rows = (size_t)yyjson_get_uint(gr);
                }
            }
            if (pc_mesh_identity(rect, cols, rows, &mesh, err) != PC_OK) {
                pc_warp_fini(out);
                return err != NULL ? err->code : PC_ERR_NOMEM;
            }
        }
        out->mesh = mesh;
        out->has_mesh = true;
    }
    return PC_OK;
}

/* ======================================================================================= *
 * Edit > Transform > Warp and friends (warp_cmds.rs command surface).
 * ======================================================================================= */

/* Largest warp frame side: far above any real layer, far below what would exhaust memory. */
#define MAX_WARP_SIDE 100000u

static const char *const MENU_EDIT_TRANSFORM[] = {"Edit", "Transform"};
static const char *const MENU_SMART[] = {"Layer", "Smart Objects"};

/* ---- map callbacks ------------------------------------------------------------------------ */

/* Forward map (source document -> destination) through a warp, for warp_mesh_surface/gray. The ctx
 * is a warp_map_ctx (its address is non-const, so passing a const warp through discards nothing). */
struct warp_map_ctx {
    const pc_warp *w;
};

static void warp_map_cb(void *ctx, double x, double y, double *ox, double *oy)
{
    const struct warp_map_ctx *c = ctx;
    pc_point q = pc_warp_map(c->w, x, y);
    *ox = q.x;
    *oy = q.y;
}

/* A mesh control point through a homography, for pc_mesh_map_points. */
static pc_point hom_point_cb(void *ctx, pc_point p)
{
    double ox = 0.0, oy = 0.0;
    pc_algo_homography_apply((const pc_algo_homography *)ctx, p.x, p.y, &ox, &oy);
    return pc_point_make(ox, oy);
}

/* A mesh evaluated at (s, t), for pc_mesh_fit. */
static pc_point mesh_eval_cb(void *ctx, double s, double t)
{
    return pc_mesh_eval((const pc_mesh *)ctx, s, t);
}

/* ---- small helpers ------------------------------------------------------------------------ */

static const pc_document *warp_active_doc(const pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    return d != NULL ? pc_doc_arc_get(d->doc) : NULL;
}

static void rect_f(pc_rect r, double out[4])
{
    out[0] = (double)r.x0;
    out[1] = (double)r.y0;
    out[2] = (double)r.x1;
    out[3] = (double)r.y1;
}

static int32_t round_i32(double v)
{
    if (!isfinite(v)) {
        return 0;
    }
    double r = round(v);
    if (r >= 2147483647.0) {
        return INT32_MAX;
    }
    if (r <= -2147483648.0) {
        return INT32_MIN;
    }
    return (int32_t)r;
}

/* Bounding box of the four corners of `box` mapped through `h`, into out[4]. */
static void mapped_box_bounds(const pc_algo_homography *h, const double box[4], double out[4])
{
    double corners[4][2] = {{box[0], box[1]}, {box[2], box[1]}, {box[2], box[3]}, {box[0], box[3]}};
    out[0] = out[1] = DBL_MAX;
    out[2] = out[3] = -DBL_MAX;
    for (size_t i = 0; i < 4; i++) {
        double ox = 0.0, oy = 0.0;
        pc_algo_homography_apply(h, corners[i][0], corners[i][1], &ox, &oy);
        if (ox < out[0]) {
            out[0] = ox;
        }
        if (oy < out[1]) {
            out[1] = oy;
        }
        if (ox > out[2]) {
            out[2] = ox;
        }
        if (oy > out[3]) {
            out[3] = oy;
        }
    }
}

/* The box a warp of `l` starts from (Free Transform's frame). For a warped smart object that is its
 * (unwarped) source box mapped back into the document, so re-warping replaces the warp over the same
 * frame (warp_cmds::warp_bounds). */
static pc_rect warp_bounds(const pc_document *doc, const pc_layer *l)
{
    if (l->content.kind == PC_LAYER_SMART && l->content.u.smart.has_warp) {
        const pc_smart_object *sm = &l->content.u.smart;
        pc_algo_homography t;
        pc_engine_smart_placement(sm, &t);
        double b[4];
        mapped_box_bounds(&t, sm->warp.bounds, b);
        return pc_rect_make(round_i32(b[0]), round_i32(b[1]), round_i32(b[2]), round_i32(b[3]));
    }
    return pc_engine_hook_transform_bounds(doc, l);
}

static bool moves_whole(const pc_layer *l)
{
    switch (l->content.kind) {
    case PC_LAYER_GROUP:
    case PC_LAYER_TEXT:
    case PC_LAYER_SHAPE:
    case PC_LAYER_SMART:
        return true;
    default:
        return false;
    }
}

static pc_pixel_format with_alpha(pc_pixel_format f)
{
    f.alpha = true;
    return f;
}

/* ---- target (layer id + source rect) ------------------------------------------------------ */

static pc_status warp_target(const pc_engine_session *s, const char *cmd, yyjson_val *p, pc_layer_id *id_out,
                             pc_rect *rect_out, pc_error *err)
{
    const pc_document *doc = warp_active_doc(s);
    if (doc == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    pc_layer_id id = 0;
    yyjson_val *jl = p != NULL ? pc_json_get(p, "layer") : NULL;
    uint64_t v = 0;
    if (jl != NULL && pc_json_as_u64(jl, &v)) {
        id = v;
    } else if (st->has_active_layer) {
        id = st->active_layer;
    } else {
        return pc_engine_err_other(err, "no active layer");
    }
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    pc_rect rect;
    yyjson_val *jr = p != NULL ? pc_json_get(p, "rect") : NULL;
    if (jr != NULL && yyjson_is_arr(jr) && yyjson_arr_size(jr) == 4) {
        double r[4] = {0, 0, 0, 0};
        for (size_t i = 0; i < 4; i++) {
            (void)pc_json_as_f64(yyjson_arr_get(jr, i), &r[i]);
        }
        rect = pc_rect_make(round_i32(r[0]), round_i32(r[1]), round_i32(r[2]), round_i32(r[3]));
    } else {
        rect = warp_bounds(doc, l);
    }
    if (pc_rect_is_empty(rect)) {
        return pc_engine_err_bad_params(err, cmd, "nothing to warp");
    }
    uint32_t w = pc_rect_width(rect), h = pc_rect_height(rect);
    if (w > MAX_WARP_SIDE || h > MAX_WARP_SIDE) {
        return pc_engine_err_bad_params(err, cmd,
                                        "rect too large for warp: %u x %u (max %u per side)", w, h,
                                        MAX_WARP_SIDE);
    }
    *id_out = id;
    *rect_out = rect;
    return PC_OK;
}

/* ---- type-layer warp (sets Warp Text, keeps the type editable) ----------------------------- */

static float clamp_pct(double v)
{
    if (!isfinite(v)) {
        return 0.0f;
    }
    if (v < -100.0) {
        v = -100.0;
    } else if (v > 100.0) {
        v = 100.0;
    }
    return (float)v;
}

/* Build the Warp Text settings a transform warp gives a type layer; *have is false for "no warp".
 * PC_ERR_* with a message for a custom (non-identity) mesh, which type cannot take. */
static pc_status type_warp(const char *cmd, const pc_warp *w, pc_text_warp *out, bool *have, pc_error *err)
{
    *have = false;
    if (w->style == PC_WARP_NONE) {
        return PC_OK;
    }
    if (w->style == PC_WARP_CUSTOM) {
        if (pc_warp_is_identity(w)) {
            return PC_OK;
        }
        return pc_engine_err_bad_params(
            err, cmd, "type can only take a warp style (arc, flag, ...); a custom mesh needs rasterized type");
    }
    memset(out, 0, sizeof *out);
    if (pc_str_set_c(&out->style, pc_warp_style_psd_name(w->style), err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    out->value = clamp_pct(w->bend);
    out->horizontal_distortion = clamp_pct(w->h_distort);
    out->vertical_distortion = clamp_pct(w->v_distort);
    out->horizontal = !w->vertical;
    *have = true;
    return PC_OK;
}

struct type_ctx {
    const char *cmd;
    pc_layer_id id;
    bool have;
    pc_text_warp tw; /* valid when have */
};

static pc_status type_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    (void)active;
    struct type_ctx *c = vctx;
    pc_locks group = pc_engine_transform_group_locks(doc, c->id);
    pc_document snap;
    bool have_snap = pc_document_clone(doc, &snap, err) == PC_OK;
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL) {
        if (have_snap) {
            pc_document_fini(&snap);
        }
        return pc_engine_err_no_layer(err, c->id);
    }
    pc_locks locks = pc_locks_union(l->locks, group);
    if (locks.all || locks.position) {
        if (have_snap) {
            pc_document_fini(&snap);
        }
        return pc_engine_err_other(err, "layer \"%.*s\" is locked", (int)l->name.len, l->name.ptr);
    }
    if (l->content.kind != PC_LAYER_TEXT) {
        if (have_snap) {
            pc_document_fini(&snap);
        }
        return pc_engine_err_other(err, "not a type layer");
    }
    pc_text_layer *t = &l->content.u.text;
    if (t->has_warp) {
        pc_text_warp_fini(&t->warp);
    }
    if (c->have) {
        pc_status rc = pc_text_warp_clone(&c->tw, &t->warp, err);
        if (rc != PC_OK) {
            t->has_warp = false;
            if (have_snap) {
                pc_document_fini(&snap);
            }
            return rc;
        }
        t->has_warp = true;
    } else {
        t->has_warp = false;
    }
    if (have_snap) {
        pc_engine_hook_refresh_text(&snap, l);
        pc_document_fini(&snap);
    }
    return PC_OK;
}

/* Build {"layer":id,"changed":bool} and optionally expose the mutable doc/root to add more. */
static pc_status changed_json(pc_layer_id id, bool changed, yyjson_mut_doc **rd_out, yyjson_mut_val **root_out,
                             pc_json_value *out, pc_error *err)
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

static bool text_warp_same(const pc_layer *l, bool have, const pc_text_warp *tw)
{
    if (l->content.kind != PC_LAYER_TEXT) {
        return false;
    }
    const pc_text_layer *t = &l->content.u.text;
    if (t->has_warp != have) {
        return false;
    }
    return !have || pc_text_warp_eq(&t->warp, tw);
}

static pc_status warp_type_layer(pc_engine_session *s, const char *cmd, pc_layer_id id, const pc_warp *w,
                                 pc_json_value *out, pc_error *err)
{
    struct type_ctx c = {0};
    c.cmd = cmd;
    c.id = id;
    pc_status rc = type_warp(cmd, w, &c.tw, &c.have, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_document *doc = warp_active_doc(s);
    const pc_layer *l = doc != NULL ? pc_document_layer(doc, id) : NULL;
    if (l != NULL && text_warp_same(l, c.have, &c.tw)) {
        if (c.have) {
            pc_text_warp_fini(&c.tw);
        }
        return changed_json(id, false, NULL, NULL, out, err);
    }
    rc = pc_engine_session_edit(s, "Warp", type_edit, &c, err);
    if (c.have) {
        pc_text_warp_fini(&c.tw);
    }
    if (rc != PC_OK) {
        return rc;
    }
    return changed_json(id, true, NULL, NULL, out, err);
}

/* ---- warp_layer (the core deform) ---------------------------------------------------------- */

static pc_status warp_raster(const pc_surface *doc_sel, pc_surface *surf, const pc_warp *w, pc_rect rect,
                             pc_algo_interp interp, pc_error *err)
{
    struct warp_map_ctx wc = {w};
    if (doc_sel != NULL) {
        pc_surface lifted, rest, moved;
        pc_status rc = pc_engine_transform_split_selected(surf, doc_sel, &lifted, &rest, err);
        if (rc != PC_OK) {
            return rc;
        }
        rc = pc_algo_warp_mesh_surface(&lifted, rect, warp_map_cb, &wc, interp, &moved, err);
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
        pc_surface_fini(surf);
        *surf = rest;
        return PC_OK;
    }
    pc_surface warped, rest;
    pc_status rc = pc_algo_warp_mesh_surface(surf, rect, warp_map_cb, &wc, interp, &warped, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = pc_surface_convert(surf, with_alpha(pc_surface_format(surf)), &rest, err);
    if (rc != PC_OK) {
        pc_surface_fini(&warped);
        return rc;
    }
    rc = pc_engine_try_clear_surface(&rest, rect, NULL, err);
    if (rc != PC_OK) {
        pc_surface_fini(&warped);
        pc_surface_fini(&rest);
        return rc;
    }
    rc = pc_engine_transform_composite_over_native(&rest, &warped, err);
    pc_surface_fini(&warped);
    if (rc != PC_OK) {
        pc_surface_fini(&rest);
        return rc;
    }
    pc_surface_prune(&rest);
    pc_surface_fini(surf);
    *surf = rest;
    return PC_OK;
}

/* Store a warp in a smart object's source space and warp its fallback cache / filter mask. */
static pc_status warp_smart(pc_smart_object *sm, const pc_warp *w, pc_rect rect, pc_algo_interp interp,
                            pc_error *err)
{
    pc_algo_homography pl, inv;
    pc_engine_smart_placement(sm, &pl);
    if (!pc_algo_homography_inverse(&pl, &inv)) {
        return pc_engine_err_other(err, "the smart object's transform is degenerate");
    }
    double sb[4];
    mapped_box_bounds(&inv, w->bounds, sb);
    pc_warp src;
    bool have_src = false;
    if (w->style == PC_WARP_NONE) {
        have_src = false;
    } else if (w->style == PC_WARP_CUSTOM) {
        pc_mesh m0, mesh;
        pc_status rc = pc_warp_to_mesh(w, 1, 1, &m0, err);
        if (rc != PC_OK) {
            return rc;
        }
        rc = pc_mesh_map_points(&m0, hom_point_cb, &inv, &mesh, err);
        pc_mesh_fini(&m0);
        if (rc != PC_OK) {
            return rc;
        }
        pc_warp_custom(&src, &mesh, sb); /* takes the mesh */
        have_src = true;
    } else {
        pc_status rc = pc_warp_clone(w, &src, err);
        if (rc != PC_OK) {
            return rc;
        }
        memcpy(src.bounds, sb, sizeof src.bounds);
        have_src = true;
    }
    if (have_src && pc_warp_is_identity(&src)) {
        pc_warp_fini(&src);
        have_src = false;
    }
    if (sm->has_warp) {
        pc_warp_fini(&sm->warp);
    }
    sm->has_warp = have_src;
    if (have_src) {
        sm->warp = src; /* move */
    }
    /* Fallback appearance for sources that can't be re-rendered. */
    struct warp_map_ctx wc = {w};
    if (sm->has_cache) {
        pc_surface nc;
        pc_status rc = pc_algo_warp_mesh_surface(&sm->cache, rect, warp_map_cb, &wc, interp, &nc, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_surface_fini(&sm->cache);
        sm->cache = nc;
    }
    if (sm->has_filter_mask) {
        pc_surface nm;
        pc_status rc =
            pc_algo_warp_mesh_gray(&sm->filter_mask.surface, warp_map_cb, &wc, interp, &nm, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_surface_fini(&sm->filter_mask.surface);
        sm->filter_mask.surface = nm;
    }
    return PC_OK;
}

static pc_status warp_layer(const pc_surface *doc_sel, pc_locks group, pc_layer *l, const pc_warp *w,
                           pc_rect rect, pc_algo_interp interp, pc_error *err)
{
    pc_locks locks = pc_locks_union(l->locks, group);
    if (locks.position || locks.all) {
        return pc_engine_err_other(err, "layer \"%.*s\" is locked", (int)l->name.len, l->name.ptr);
    }
    switch (l->content.kind) {
    case PC_LAYER_GROUP: {
        pc_group *g = &l->content.u.group;
        for (size_t i = 0; i < g->nchildren; i++) {
            pc_status rc = warp_layer(NULL, locks, &g->children[i], w, rect, interp, err);
            if (rc != PC_OK) {
                return rc;
            }
        }
        break;
    }
    case PC_LAYER_TEXT:
        return pc_engine_err_other(err, "Warp needs rasterized type (or use Type > Warp Text)");
    case PC_LAYER_SHAPE: {
        pc_status rc = pc_engine_map_shape(&l->content.u.shape, NULL, w, err);
        if (rc != PC_OK) {
            return rc;
        }
        break;
    }
    case PC_LAYER_SMART: {
        pc_status rc = warp_smart(&l->content.u.smart, w, rect, interp, err);
        if (rc != PC_OK) {
            return rc;
        }
        break;
    }
    case PC_LAYER_RASTER: {
        pc_status rc = warp_raster(doc_sel, &l->content.u.raster, w, rect, interp, err);
        if (rc != PC_OK) {
            return rc;
        }
        break;
    }
    default:
        break; /* adjustment / fill: no pixels */
    }
    (void)moves_whole; /* reserved for the #4175 selection-limited mask (deferred) */
    if (l->has_mask && l->mask.linked) {
        pc_surface nm;
        struct warp_map_ctx wc = {w};
        pc_status rc = pc_algo_warp_mesh_gray(&l->mask.surface, warp_map_cb, &wc, interp, &nm, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_surface_fini(&l->mask.surface);
        l->mask.surface = nm;
    }
    if (l->has_vector_mask && l->vector_mask.linked) {
        pc_path np;
        pc_status rc = pc_engine_map_path(&l->vector_mask.path, NULL, w, &np, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_path_fini(&l->vector_mask.path);
        l->vector_mask.path = np;
    }
    return PC_OK;
}

static void refresh_rec(const pc_document *snapshot, pc_layer *l)
{
    switch (l->content.kind) {
    case PC_LAYER_TEXT:
        pc_engine_hook_refresh_text(snapshot, l);
        break;
    case PC_LAYER_SHAPE:
        pc_engine_hook_refresh_shape(snapshot, l);
        break;
    case PC_LAYER_SMART:
        pc_engine_hook_refresh_smart(snapshot, l);
        break;
    case PC_LAYER_GROUP: {
        pc_group *g = &l->content.u.group;
        for (size_t i = 0; i < g->nchildren; i++) {
            refresh_rec(snapshot, &g->children[i]);
        }
        break;
    }
    default:
        break;
    }
}

struct apply_ctx {
    pc_layer_id id;
    const pc_warp *w;
    pc_rect rect;
    pc_algo_interp interp;
};

static pc_status apply_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    (void)active;
    struct apply_ctx *c = vctx;
    const pc_surface *sel = doc->has_selection ? &doc->selection : NULL;
    bool is_group = false;
    {
        const pc_layer *l0 = pc_document_layer(doc, c->id);
        is_group = l0 != NULL && pc_layer_is_group(l0);
    }
    pc_locks group = pc_engine_transform_group_locks(doc, c->id);
    pc_engine_background_to_layer(doc, c->id);
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    pc_status rc = warp_layer(is_group ? NULL : sel, group, l, c->w, c->rect, c->interp, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_document snap;
    if (pc_document_clone(doc, &snap, err) == PC_OK) {
        pc_layer *ll = pc_document_layer_mut(doc, c->id);
        if (ll != NULL) {
            refresh_rec(&snap, ll);
        }
        pc_document_fini(&snap);
    }
    if (doc->has_selection) {
        pc_surface ns;
        struct warp_map_ctx wc = {c->w};
        rc = pc_algo_warp_mesh_gray(&doc->selection, warp_map_cb, &wc, PC_ALGO_INTERP_BILINEAR, &ns, err);
        if (rc != PC_OK) {
            return rc;
        }
        if (pc_rect_is_empty(pc_surface_content_bounds(&ns))) {
            pc_surface_fini(&ns);
            pc_surface_fini(&doc->selection);
            doc->has_selection = false;
        } else {
            pc_surface_fini(&doc->selection);
            doc->selection = ns;
        }
    }
    return PC_OK;
}

static pc_algo_interp interp_of(yyjson_val *p)
{
    char buf[32] = "bicubic";
    yyjson_val *v = p != NULL ? pc_json_get(p, "interpolation") : NULL;
    if (v != NULL) {
        size_t len = 0;
        const char *str = pc_json_as_str(v, &len);
        if (str != NULL && len < sizeof buf) {
            memcpy(buf, str, len);
            buf[len] = '\0';
        }
    }
    return pc_algo_interp_parse(buf);
}

static pc_status apply_warp(pc_engine_session *s, yyjson_val *p, const char *cmd, pc_json_value *out,
                           pc_error *err)
{
    pc_layer_id id = 0;
    pc_rect rect;
    pc_status rc = warp_target(s, cmd, p, &id, &rect, err);
    if (rc != PC_OK) {
        return rc;
    }
    double rf[4];
    rect_f(rect, rf);
    pc_warp w;
    rc = pc_engine_warp_from_params(cmd, p, rf, &w, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_document *doc = warp_active_doc(s);
    const pc_layer *l = doc != NULL ? pc_document_layer(doc, id) : NULL;
    if (l != NULL && l->content.kind == PC_LAYER_TEXT) {
        rc = warp_type_layer(s, cmd, id, &w, out, err);
        pc_warp_fini(&w);
        return rc;
    }
    bool is_smart = l != NULL && l->content.kind == PC_LAYER_SMART;
    if (pc_warp_is_identity(&w) && !is_smart) {
        pc_warp_fini(&w);
        return changed_json(id, false, NULL, NULL, out, err);
    }
    struct apply_ctx c = {id, &w, rect, interp_of(p)};
    rc = pc_engine_session_edit(s, "Warp", apply_edit, &c, err);
    pc_warp_fini(&w);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    yyjson_mut_val *root = NULL;
    rc = changed_json(id, true, &rd, &root, out, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_val *jr = yyjson_mut_arr(rd);
    yyjson_mut_arr_add_int(rd, jr, rect.x0);
    yyjson_mut_arr_add_int(rd, jr, rect.y0);
    yyjson_mut_arr_add_int(rd, jr, rect.x1);
    yyjson_mut_arr_add_int(rd, jr, rect.y1);
    yyjson_mut_obj_add_val(rd, root, "rect", jr);
    return PC_OK;
}

/* ---- warp -> JSON (for split/grid results) ------------------------------------------------- */

static yyjson_mut_val *warp_to_json(yyjson_mut_doc *d, const pc_warp *w)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    if (o == NULL) {
        return NULL;
    }
    yyjson_mut_obj_add_str(d, o, "style", pc_warp_style_id(w->style));
    yyjson_mut_obj_add_real(d, o, "bend", w->bend);
    yyjson_mut_obj_add_real(d, o, "hDistort", w->h_distort);
    yyjson_mut_obj_add_real(d, o, "vDistort", w->v_distort);
    yyjson_mut_obj_add_bool(d, o, "vertical", w->vertical);
    yyjson_mut_val *jb = yyjson_mut_arr(d);
    for (size_t i = 0; i < 4; i++) {
        yyjson_mut_arr_add_real(d, jb, w->bounds[i]);
    }
    yyjson_mut_obj_add_val(d, o, "bounds", jb);
    if (w->has_mesh) {
        const pc_mesh *m = &w->mesh;
        yyjson_mut_val *jm = yyjson_mut_obj(d);
        yyjson_mut_val *jus = yyjson_mut_arr(d), *jvs = yyjson_mut_arr(d), *jpts = yyjson_mut_arr(d);
        if (jm == NULL || jus == NULL || jvs == NULL || jpts == NULL) {
            return NULL;
        }
        for (size_t i = 0; i < m->nus; i++) {
            yyjson_mut_arr_add_real(d, jus, m->us[i]);
        }
        for (size_t i = 0; i < m->nvs; i++) {
            yyjson_mut_arr_add_real(d, jvs, m->vs[i]);
        }
        for (size_t i = 0; i < m->npoints; i++) {
            yyjson_mut_val *pt = yyjson_mut_arr(d);
            if (pt == NULL) {
                return NULL;
            }
            yyjson_mut_arr_add_real(d, pt, m->points[i].x);
            yyjson_mut_arr_add_real(d, pt, m->points[i].y);
            yyjson_mut_arr_append(jpts, pt);
        }
        yyjson_mut_obj_add_val(d, jm, "us", jus);
        yyjson_mut_obj_add_val(d, jm, "vs", jvs);
        yyjson_mut_obj_add_val(d, jm, "points", jpts);
        yyjson_mut_obj_add_val(d, o, "mesh", jm);
    } else {
        yyjson_mut_obj_add_null(d, o, "mesh");
    }
    return o;
}

/* ---- split warp ---------------------------------------------------------------------------- */

typedef enum { SPLIT_CROSSWISE, SPLIT_HORIZONTAL, SPLIT_VERTICAL, SPLIT_REMOVE } split_how;

static void commit_default_sections(pc_mesh *m)
{
    bool did = false;
    pc_error e = {0};
    (void)pc_mesh_split_u(m, 1.0 / 3.0, &did, &e);
    (void)pc_mesh_split_u(m, 2.0 / 3.0, &did, &e);
    (void)pc_mesh_split_v(m, 1.0 / 3.0, &did, &e);
    (void)pc_mesh_split_v(m, 2.0 / 3.0, &did, &e);
}

static double mid_of(const double *k, size_t n, double v)
{
    size_t i = 0;
    for (; i + 1 < n; i++) {
        if (v <= k[i + 1]) {
            break;
        }
    }
    if (i + 1 >= n) {
        i = (n >= 2) ? n - 2 : 0;
    }
    return (k[i] + k[i + 1]) / 2.0;
}

/* Nearest interior boundary (indices 1..n-2) to v; returns true + *idx + *dist when any exist. */
static bool nearest_interior(const double *k, size_t n, double v, size_t *idx, double *dist)
{
    if (n < 3) {
        return false;
    }
    bool found = false;
    for (size_t i = 1; i + 1 < n; i++) {
        double dd = fabs(k[i] - v);
        if (!found || dd < *dist) {
            found = true;
            *dist = dd;
            *idx = i;
        }
    }
    return found;
}

/* Applies a split (or removes the split nearest `at`) on w's mesh; *out owned on success. */
static bool split_warp(const pc_warp *w, const double *at, split_how how, pc_warp *out)
{
    pc_mesh mesh;
    pc_error e = {0};
    if (pc_warp_to_mesh(w, 1, 1, &mesh, &e) != PC_OK) {
        return false;
    }
    if (how != SPLIT_REMOVE && w->style == PC_WARP_CUSTOM && mesh.nus == 2 && mesh.nvs == 2) {
        commit_default_sections(&mesh);
    }
    double s = 0.5, t = 0.5;
    if (at != NULL) {
        pc_mesh_param_at(&mesh, pc_point_make(at[0], at[1]), &s, &t);
    } else {
        s = mid_of(mesh.us, mesh.nus, s);
        t = mid_of(mesh.vs, mesh.nvs, t);
    }
    bool ok = false;
    bool did = false;
    switch (how) {
    case SPLIT_CROSSWISE: {
        bool a = false, b = false;
        (void)pc_mesh_split_u(&mesh, s, &a, &e);
        (void)pc_mesh_split_v(&mesh, t, &b, &e);
        ok = a || b;
        break;
    }
    case SPLIT_HORIZONTAL:
        (void)pc_mesh_split_v(&mesh, t, &did, &e);
        ok = did;
        break;
    case SPLIT_VERTICAL:
        (void)pc_mesh_split_u(&mesh, s, &did, &e);
        ok = did;
        break;
    case SPLIT_REMOVE: {
        size_t iu = 0, iv = 0;
        double du = DBL_MAX, dv = DBL_MAX;
        bool hu = nearest_interior(mesh.us, mesh.nus, s, &iu, &du);
        bool hv = nearest_interior(mesh.vs, mesh.nvs, t, &iv, &dv);
        if (hu && du <= dv) {
            (void)pc_mesh_remove_split_u(&mesh, iu, &did, &e);
        } else if (hv) {
            (void)pc_mesh_remove_split_v(&mesh, iv, &did, &e);
        } else if (hu) {
            (void)pc_mesh_remove_split_u(&mesh, iu, &did, &e);
        }
        ok = did;
        break;
    }
    }
    if (!ok) {
        pc_mesh_fini(&mesh);
        return false;
    }
    pc_warp_custom(out, &mesh, w->bounds); /* takes the mesh */
    return true;
}

static void param_rect(yyjson_val *p, double out[4])
{
    out[0] = 0.0;
    out[1] = 0.0;
    out[2] = 1.0;
    out[3] = 1.0;
    yyjson_val *r = p != NULL ? pc_json_get(p, "rect") : NULL;
    if (r != NULL && yyjson_is_arr(r)) {
        for (size_t i = 0; i < 4; i++) {
            double v;
            if (pc_json_as_f64(yyjson_arr_get(r, i), &v)) {
                out[i] = v;
            }
        }
    }
}

/* A warp passed inline (warp, mesh or style). *have is false => use the active smart object. */
static pc_status inline_warp(const char *cmd, yyjson_val *p, pc_warp *out, bool *have, pc_error *err)
{
    *have = false;
    bool any = p != NULL && (pc_json_get(p, "warp") != NULL || pc_json_get(p, "mesh") != NULL ||
                             pc_json_get(p, "style") != NULL);
    if (!any) {
        return PC_OK;
    }
    double rect[4];
    param_rect(p, rect);
    pc_status rc = pc_engine_warp_from_params(cmd, p, rect, out, err);
    if (rc == PC_OK) {
        *have = true;
    }
    return rc;
}

/* A smart object's stored warp (document space is kept as source space here), or a Warp::none over
 * the layer's content mapped back into source space when it has none. *out owned on success. */
static pc_status smart_stored_warp(const pc_layer *l, pc_warp *out, pc_error *err)
{
    const pc_smart_object *sm = &l->content.u.smart;
    if (sm->has_warp) {
        return pc_warp_clone(&sm->warp, out, err);
    }
    const pc_surface *surf = pc_layer_surface(l);
    pc_rect b = surf != NULL ? pc_surface_content_bounds(surf) : PC_RECT_EMPTY;
    pc_algo_homography pl, inv;
    pc_engine_smart_placement(sm, &pl);
    double box[4] = {(double)b.x0, (double)b.y0, (double)b.x1, (double)b.y1};
    double sb[4];
    if (pc_algo_homography_inverse(&pl, &inv)) {
        mapped_box_bounds(&inv, box, sb);
    } else {
        memcpy(sb, box, sizeof sb);
    }
    pc_warp_none(out, sb);
    (void)err;
    return PC_OK;
}

struct smart_warp_ctx {
    pc_layer_id id;
    const pc_warp *w;
};

static pc_status set_smart_warp_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    (void)active;
    struct smart_warp_ctx *c = vctx;
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL || l->content.kind != PC_LAYER_SMART) {
        return pc_engine_err_other(err, "not a smart object");
    }
    pc_smart_object *sm = &l->content.u.smart;
    pc_warp copy;
    pc_status rc = pc_warp_clone(c->w, &copy, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (sm->has_warp) {
        pc_warp_fini(&sm->warp);
    }
    sm->warp = copy;
    sm->has_warp = true;
    bool updated = false;
    return pc_engine_smart_refresh(doc, c->id, &updated, err);
}

static pc_status smart_warp_result(pc_engine_session *s, const char *label, pc_layer_id id,
                                   const pc_warp *out, pc_json_value *result, pc_error *err)
{
    struct smart_warp_ctx c = {id, out};
    pc_status rc = pc_engine_session_edit(s, label, set_smart_warp_edit, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(result, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", id);
    yyjson_mut_val *jw = warp_to_json(rd, out);
    yyjson_mut_obj_add_val(rd, root, "warp", jw != NULL ? jw : yyjson_mut_null(rd));
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status inline_warp_result(pc_json_value *result, const pc_warp *out, pc_error *err)
{
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(result, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *jw = warp_to_json(rd, out);
    yyjson_mut_obj_add_val(rd, root, "warp", jw != NULL ? jw : yyjson_mut_null(rd));
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static bool at_param(yyjson_val *p, double out[2])
{
    yyjson_val *a = p != NULL ? pc_json_get(p, "at") : NULL;
    if (a == NULL || !yyjson_is_arr(a) || yyjson_arr_size(a) < 2) {
        return false;
    }
    return pc_json_as_f64(yyjson_arr_get(a, 0), &out[0]) && pc_json_as_f64(yyjson_arr_get(a, 1), &out[1]);
}

static pc_status run_split(pc_engine_session *s, yyjson_val *p, const char *cmd, split_how how,
                           pc_json_value *out, pc_error *err)
{
    double at[2];
    bool have_at = at_param(p, at);
    pc_warp inl;
    bool have_inl = false;
    pc_status rc = inline_warp(cmd, p, &inl, &have_inl, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (have_inl) {
        pc_warp result;
        bool ok = split_warp(&inl, have_at ? at : NULL, how, &result);
        pc_warp_fini(&inl);
        if (!ok) {
            return pc_engine_err_bad_params(err, cmd, "nothing to split or remove there");
        }
        rc = inline_warp_result(out, &result, err);
        pc_warp_fini(&result);
        return rc;
    }
    /* The active smart object's stored warp. */
    const pc_document *doc = warp_active_doc(s);
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (doc == NULL || st == NULL) {
        return pc_engine_err_no_document(err);
    }
    if (!st->has_active_layer) {
        return pc_engine_err_other(err, "no active layer");
    }
    pc_layer_id id = st->active_layer;
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    if (l->content.kind != PC_LAYER_SMART) {
        return pc_engine_err_bad_params(err, cmd,
                                        "pass the warp being edited (`warp`), or select a warped smart object");
    }
    pc_warp stored;
    rc = smart_stored_warp(l, &stored, err);
    if (rc != PC_OK) {
        return rc;
    }
    double at_src[2];
    const double *at_ptr = NULL;
    if (have_at) {
        pc_algo_homography pl, inv;
        pc_engine_smart_placement(&l->content.u.smart, &pl);
        if (pc_algo_homography_inverse(&pl, &inv)) {
            pc_algo_homography_apply(&inv, at[0], at[1], &at_src[0], &at_src[1]);
            at_ptr = at_src;
        }
    }
    pc_warp result;
    bool ok = split_warp(&stored, at_ptr, how, &result);
    pc_warp_fini(&stored);
    if (!ok) {
        return pc_engine_err_bad_params(err, cmd, "nothing to split or remove there");
    }
    const char *label = how == SPLIT_REMOVE ? "Remove Warp Split" : "Split Warp";
    rc = smart_warp_result(s, label, id, &result, out, err);
    pc_warp_fini(&result);
    return rc;
}

/* ---- warp grid ----------------------------------------------------------------------------- */

#define GRID_CMD "edit.transform.warpGrid"

static pc_status grid_size(yyjson_val *p, size_t *out, pc_error *err)
{
    yyjson_val *v = p != NULL ? pc_json_get(p, "size") : NULL;
    if (v == NULL && p != NULL) {
        v = pc_json_get(p, "grid");
    }
    if (v == NULL) {
        return pc_engine_err_bad_params(err, GRID_CMD, "pass `size` (1, 3, 4, 5 or \"default\")");
    }
    if (yyjson_is_arr(v)) {
        return pc_engine_err_bad_params(err, GRID_CMD,
                                        "pass `size` as a number or \"default\", \"3\", \"4\" or \"5\"");
    }
    uint64_t n = 0;
    size_t slen = 0;
    const char *str = pc_json_as_str(v, &slen);
    if (str != NULL) {
        if (slen == 7 && memcmp(str, "default", 7) == 0) {
            n = 1;
        } else if ((slen == 1 && str[0] == '3') || (slen == 3 && memcmp(str, "3x3", 3) == 0)) {
            n = 3;
        } else if ((slen == 1 && str[0] == '4') || (slen == 3 && memcmp(str, "4x4", 3) == 0)) {
            n = 4;
        } else if ((slen == 1 && str[0] == '5') || (slen == 3 && memcmp(str, "5x5", 3) == 0)) {
            n = 5;
        } else {
            return pc_engine_err_bad_params(err, GRID_CMD, "grid size must be default, 3, 4 or 5");
        }
    } else if (pc_json_as_u64(v, &n)) {
        /* whole number */
    } else {
        return pc_engine_err_bad_params(err, GRID_CMD,
                                        "grid size must be a number or default, 3, 4 or 5");
    }
    if (n < 1 || n > 64) {
        return pc_engine_err_bad_params(err, GRID_CMD, "grid size must be from 1 to 64");
    }
    *out = (size_t)n;
    return PC_OK;
}

/* Even n x n patches fitted to w (the surface is kept; knots sit at i/n). *out owned on success. */
static pc_status resize_warp(const pc_warp *w, size_t n, pc_warp *out, pc_error *err)
{
    pc_mesh base;
    pc_status rc = pc_warp_to_mesh(w, 1, 1, &base, err);
    if (rc != PC_OK) {
        return rc;
    }
    double *knots = calloc(n + 1, sizeof *knots);
    if (knots == NULL) {
        pc_mesh_fini(&base);
        return pc_fail(err, PC_ERR_NOMEM, "warp grid");
    }
    for (size_t i = 0; i <= n; i++) {
        knots[i] = (double)i / (double)n;
    }
    pc_mesh fit;
    rc = pc_mesh_fit(mesh_eval_cb, &base, knots, n + 1, knots, n + 1, &fit, err);
    free(knots);
    pc_mesh_fini(&base);
    if (rc != PC_OK) {
        return rc;
    }
    pc_warp_custom(out, &fit, w->bounds);
    return PC_OK;
}

static pc_status run_grid(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    size_t n = 0;
    pc_status rc = grid_size(p, &n, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_warp inl;
    bool have_inl = false;
    rc = inline_warp(GRID_CMD, p, &inl, &have_inl, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (have_inl) {
        pc_warp result;
        rc = resize_warp(&inl, n, &result, err);
        pc_warp_fini(&inl);
        if (rc != PC_OK) {
            return rc;
        }
        rc = inline_warp_result(out, &result, err);
        pc_warp_fini(&result);
        return rc;
    }
    const pc_document *doc = warp_active_doc(s);
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (doc == NULL || st == NULL) {
        return pc_engine_err_no_document(err);
    }
    if (!st->has_active_layer) {
        return pc_engine_err_other(err, "no active layer");
    }
    pc_layer_id id = st->active_layer;
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    if (l->content.kind != PC_LAYER_SMART) {
        return pc_engine_err_bad_params(err, GRID_CMD,
                                        "pass the warp being edited (`warp`), or select a smart object");
    }
    pc_warp stored;
    rc = smart_stored_warp(l, &stored, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_warp result;
    rc = resize_warp(&stored, n, &result, err);
    pc_warp_fini(&stored);
    if (rc != PC_OK) {
        return rc;
    }
    rc = smart_warp_result(s, "Warp Grid", id, &result, out, err);
    pc_warp_fini(&result);
    return rc;
}

/* ---- run thunks ---------------------------------------------------------------------------- */

static pc_status run_warp_edit(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return apply_warp(s, p, "edit.transform.warp", out, err);
}
static pc_status run_warp_smart(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return apply_warp(s, p, "layer.smartObjects.warp", out, err);
}
static pc_status run_split_crosswise(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return run_split(s, p, "edit.transform.splitWarpCrosswise", SPLIT_CROSSWISE, out, err);
}
static pc_status run_split_horizontal(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return run_split(s, p, "edit.transform.splitWarpHorizontally", SPLIT_HORIZONTAL, out, err);
}
static pc_status run_split_vertical(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return run_split(s, p, "edit.transform.splitWarpVertically", SPLIT_VERTICAL, out, err);
}
static pc_status run_remove_split(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return run_split(s, p, "edit.transform.removeWarpSplit", SPLIT_REMOVE, out, err);
}

/* ---- enabled predicates -------------------------------------------------------------------- */

static pc_status warp_has_layer(const pc_engine_session *s, pc_error *reason)
{
    return pc_engine_has_layer(s, reason);
}

static pc_status warp_active_smart(const pc_engine_session *s, pc_error *reason)
{
    const pc_layer *l = pc_engine_active_layer_of(s, reason);
    if (l == NULL) {
        return PC_ERR_INVALID;
    }
    if (l->content.kind != PC_LAYER_SMART) {
        return pc_fail(reason, PC_ERR_INVALID, "the active layer is not a smart object");
    }
    return PC_OK;
}

/* ---- registry ------------------------------------------------------------------------------ */

#define WARP_PARAMS                                                                                      \
    "{\"layer\":id?,\"rect\":[x0,y0,x1,y1]? (warp box; default = layer content \xE2\x88\xA9 selection),"  \
    "\"style\":\"custom|none|arc|arcLower|arcUpper|arch|bulge|shellLower|shellUpper|flag|wave|fish|rise|" \
    "fisheye|inflate|squeeze|twist\",\"bend\":%=50,\"hDistort\":%,\"vDistort\":%,\"vertical\":bool,"      \
    "\"mesh\":{\"us\":[0,...,1],\"vs\":[0,...,1],\"points\":[[x,y]...]},\"grid\":[cols,rows]?,"           \
    "\"warp\":{full warp object}?,\"interpolation\":\"bicubic|bilinear|nearest\"} \xE2\x80\x94 on a type "\
    "layer a style (or none) sets its Warp Text and the type stays editable"
#define SPLIT_PARAMS                                                                                     \
    "{\"warp\":{...}? (the warp being edited; returned split),\"rect\":[x0,y0,x1,y1]?,"                   \
    "\"at\":[x,y]? (document point; default = middle of the patch)} \xE2\x80\x94 without `warp`, edits "  \
    "the active smart object's warp"
#define GRID_PARAMS                                                                                      \
    "{\"warp\":{...}? (the warp being edited; returned with the new mesh),\"rect\":[x0,y0,x1,y1]?,"       \
    "\"size\":n|\"default\"|\"3\"|\"4\"|\"5\" (square patches, 1..=64)} \xE2\x80\x94 without "            \
    "`warp`/`style`/`mesh`, edits the active smart object's warp"

static const pc_engine_command WARP_SPECS[] = {
    {"edit.transform.warp", "Warp", MENU_EDIT_TRANSFORM, 2, NULL, WARP_PARAMS, warp_has_layer, run_warp_edit,
     true},
    {"layer.smartObjects.warp", "Warp", MENU_SMART, 2, NULL, WARP_PARAMS, warp_active_smart, run_warp_smart,
     true},
    {"edit.transform.splitWarpCrosswise", "Split Warp Crosswise", MENU_EDIT_TRANSFORM, 2, NULL, SPLIT_PARAMS,
     warp_has_layer, run_split_crosswise, true},
    {"edit.transform.splitWarpHorizontally", "Split Warp Horizontally", MENU_EDIT_TRANSFORM, 2, NULL,
     SPLIT_PARAMS, warp_has_layer, run_split_horizontal, true},
    {"edit.transform.splitWarpVertically", "Split Warp Vertically", MENU_EDIT_TRANSFORM, 2, NULL, SPLIT_PARAMS,
     warp_has_layer, run_split_vertical, true},
    {"edit.transform.removeWarpSplit", "Remove Warp Split", MENU_EDIT_TRANSFORM, 2, NULL, SPLIT_PARAMS,
     warp_has_layer, run_remove_split, true},
    {GRID_CMD, "Warp Grid", NULL, 0, NULL, GRID_PARAMS, warp_has_layer, run_grid, true},
};

const pc_engine_command *pc_engine_warp_specs(size_t *n)
{
    *n = sizeof WARP_SPECS / sizeof WARP_SPECS[0];
    return WARP_SPECS;
}
