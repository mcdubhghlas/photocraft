/* Edit > Transform > Warp and the warp primitives (port of crates/engine/src/warp_cmds.rs, E9).
 *
 * The warp COMMANDS (Edit/Layer > Transform > Warp, split/resize warp grid) are deeply coupled to
 * unported chunks -- transform_cmds (E4), smart_cmds (E6), vector_cmds/type_cmds (E11) and
 * extra_cmds -- so they are DEFERRED and not registered here (see REWRITE_PLAN.md §8). What this
 * file provides now is pc_engine_warp_from_params: building a geom pc_warp from a command's JSON
 * (the reference's warp_cmds::warp_from_params), which E4's select.transformSelection / edit.transform
 * depends on and E7 deferred to this chunk. */
#include <stdlib.h>
#include <string.h>

#include "engine_internal.h"
#include "geom/warp.h"
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
