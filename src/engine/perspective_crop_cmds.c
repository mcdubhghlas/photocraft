/* The Perspective Crop tool's commit: image.perspectiveCrop (port of
 * crates/engine/src/perspective_crop_cmds.rs).
 *
 * Four corners (TL, TR, BR, BL in document px) mark a quad that is a rectangle seen in perspective.
 * The whole document is mapped through the homography taking that quad onto an upright W x H
 * rectangle, which becomes the new canvas, in one undo step: every raster layer, layer mask, alpha
 * channel and the Quick Mask is resampled through the map (bounded by the new canvas), the
 * Background stays a Background over the background colour where the quad reaches past the image, and
 * pixels outside the quad are deleted. Type, shape, fill and smart object layers are rasterized
 * first; vector masks, paths, notes, slices and guides follow the affine map closest to the
 * homography (exact when the quad is a parallelogram).
 *
 * Never crashes (REWRITE_PLAN.md rule 9): corners and sizes are validated, the output is bounded
 * before anything is built, and a degenerate quad is refused. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "algo/params.h"
#include "algo/transform.h"
#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "geom/geom.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#include "engine_internal.h"
#include "fill_cmds.h"     /* pc_engine_transform_composite_over (straight RGBA over) */
#include "perspective_crop_cmds.h"

#define CMD "image.perspectiveCrop"
#define MAX_SIDE 300000u
#define MAX_COORD 1.0e7
#define MAX_RESOLUTION 10000.0
/* Largest output, total, so a hostile size cannot exhaust memory (bytes). */
#define MAX_OUTPUT_BYTES ((uint64_t)1 << 31)

/* ---- params -------------------------------------------------------------------------------- */

static pc_status corners(yyjson_val *p, double q[4][2], pc_error *err)
{
    yyjson_val *list = p != NULL ? pc_json_get(p, "corners") : NULL;
    if (list == NULL || !yyjson_is_arr(list) || yyjson_arr_size(list) != 4) {
        return pc_engine_err_bad_params(
            err, CMD, "`corners` must be four [x, y] points: top-left, top-right, bottom-right, bottom-left");
    }
    for (size_t i = 0; i < 4; i++) {
        yyjson_val *c = yyjson_arr_get(list, i);
        double x = 0.0, y = 0.0;
        if (c == NULL || !yyjson_is_arr(c) || yyjson_arr_size(c) != 2 ||
            !pc_json_as_f64(yyjson_arr_get(c, 0), &x) || !pc_json_as_f64(yyjson_arr_get(c, 1), &y)) {
            return pc_engine_err_bad_params(err, CMD, "each corner must be a [x, y] number pair");
        }
        if (!(isfinite(x) && isfinite(y)) || fabs(x) > MAX_COORD || fabs(y) > MAX_COORD) {
            return pc_engine_err_bad_params(err, CMD, "a corner must be finite and within +-%.0f px",
                                            MAX_COORD);
        }
        q[i][0] = x;
        q[i][1] = y;
    }
    if (!pc_algo_convex_quad((const double(*)[2])q)) {
        return pc_engine_err_bad_params(
            err, CMD,
            "the corners must form a convex quadrilateral at least 1 px in area, in order top-left, "
            "top-right, bottom-right, bottom-left (clockwise on screen)");
    }
    return PC_OK;
}

/* One side; *present reports whether the key was given; validated 1..MAX_SIDE. */
static pc_status side_of(yyjson_val *p, const char *key, bool *present, double *out, pc_error *err)
{
    *present = false;
    yyjson_val *v = p != NULL ? pc_json_get(p, key) : NULL;
    if (v == NULL || yyjson_is_null(v)) {
        return PC_OK;
    }
    double n = 0.0;
    double r;
    if (!pc_json_as_f64(v, &n) || !isfinite(n) || (r = round(n)) < 1.0 || r > (double)MAX_SIDE) {
        return pc_engine_err_bad_params(err, CMD, "`%s` must be a number of pixels from 1 to %u", key,
                                        MAX_SIDE);
    }
    *present = true;
    *out = round(n);
    return PC_OK;
}

static pc_status out_size(yyjson_val *p, const double q[4][2], uint32_t *ow, uint32_t *oh, pc_error *err)
{
    double qw = 0.0, qh = 0.0;
    pc_algo_quad_size(q, &qw, &qh);
    if (!(qw > 0.0 && qh > 0.0)) {
        return pc_engine_err_bad_params(err, CMD, "the corners don't define a perspective");
    }
    bool hw = false, hh = false;
    double pw = 0.0, ph = 0.0;
    pc_status rc = side_of(p, "width", &hw, &pw, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = side_of(p, "height", &hh, &ph, err);
    if (rc != PC_OK) {
        return rc;
    }
    double w, h;
    if (hw && hh) {
        w = pw;
        h = ph;
    } else if (hw) {
        w = pw;
        h = pw * qh / qw;
    } else if (hh) {
        w = ph * qw / qh;
        h = ph;
    } else {
        w = qw;
        h = qh;
    }
    double rw = round(w), rh = round(h);
    if (rw < 1.0) {
        rw = 1.0;
    }
    if (rh < 1.0) {
        rh = 1.0;
    }
    if (!(isfinite(rw) && isfinite(rh)) || rw > (double)MAX_SIDE || rh > (double)MAX_SIDE) {
        return pc_engine_err_bad_params(err, CMD, "the output is larger than %u px on a side", MAX_SIDE);
    }
    *ow = (uint32_t)rw;
    *oh = (uint32_t)rh;
    return PC_OK;
}

static pc_status resolution_of(yyjson_val *p, bool *present, float *out, pc_error *err)
{
    *present = false;
    yyjson_val *v = p != NULL ? pc_json_get(p, "resolution") : NULL;
    if (v == NULL || yyjson_is_null(v)) {
        return PC_OK;
    }
    double r = 0.0;
    if (!pc_json_as_f64(v, &r) || !isfinite(r) || r < 1.0 || r > MAX_RESOLUTION) {
        return pc_engine_err_bad_params(err, CMD,
                                        "`resolution` must be a number of pixels per inch from 1 to %.0f",
                                        MAX_RESOLUTION);
    }
    *present = true;
    *out = (float)r;
    return PC_OK;
}

static pc_status interp_of(yyjson_val *p, pc_algo_interp *out, pc_error *err)
{
    yyjson_val *v = p != NULL ? pc_json_get(p, "interpolation") : NULL;
    if (v == NULL || yyjson_is_null(v)) {
        *out = pc_algo_interp_parse("bicubic");
        return PC_OK;
    }
    size_t len = 0;
    const char *s = pc_json_as_str(v, &len);
    char buf[32];
    if (s == NULL || len >= sizeof buf) {
        return pc_engine_err_bad_params(err, CMD,
                                        "`interpolation` must be \"bicubic\", \"bilinear\" or \"nearest\"");
    }
    memcpy(buf, s, len);
    buf[len] = '\0';
    if (strcmp(buf, "nearest") != 0 && strcmp(buf, "nearestNeighbor") != 0 && strcmp(buf, "bilinear") != 0 &&
        strcmp(buf, "bicubic") != 0) {
        return pc_engine_err_bad_params(err, CMD,
                                        "`interpolation` must be \"bicubic\", \"bilinear\" or \"nearest\"");
    }
    *out = pc_algo_interp_parse(buf);
    return PC_OK;
}

/* The affine map closest to the crop for geometry that can't take a homography. */
static void nearest_affine(const double q[4][2], double w, double h, pc_affine *out)
{
    const double *tl = q[0], *tr = q[1], *br = q[2], *bl = q[3];
    double u[2] = {(tr[0] - tl[0] + br[0] - bl[0]) / 2.0, (tr[1] - tl[1] + br[1] - bl[1]) / 2.0};
    double v[2] = {(bl[0] - tl[0] + br[0] - tr[0]) / 2.0, (bl[1] - tl[1] + br[1] - tr[1]) / 2.0};
    double det = u[0] * v[1] - v[0] * u[1];
    if (!isfinite(det) || fabs(det) < 1e-12) {
        *out = (pc_affine){{1.0, 0.0, 0.0, 1.0, 0.0, 0.0}};
        return;
    }
    double m00 = w * v[1] / det, m01 = -w * v[0] / det, m10 = -h * u[1] / det, m11 = h * u[0] / det;
    double cx = (tl[0] + tr[0] + br[0] + bl[0]) / 4.0, cy = (tl[1] + tr[1] + br[1] + bl[1]) / 4.0;
    double tx = w / 2.0 - (m00 * cx + m01 * cy), ty = h / 2.0 - (m10 * cx + m11 * cy);
    *out = (pc_affine){{m00, m10, m01, m11, tx, ty}};
}

/* ---- surface warps ------------------------------------------------------------------------- */

/* A one-channel surface (mask, alpha channel, Quick Mask) through `h`, kept inside `clip`; what the
 * map uncovers takes the surface's default (a mask's reveal or hide). *out owned on success. */
static pc_status warp_gray(const pc_surface *s, const pc_algo_homography *h, pc_algo_interp interp,
                           pc_rect clip, pc_surface *out, pc_error *err)
{
    float def[PC_RASTER_MAX_CHANNELS];
    pc_surface_default_pixel(s, def);
    float d0 = def[0];
    pc_pixel_format fmt = pc_surface_format(s);
    pc_surface_init_default(out, fmt, &d0, 1);
    pc_rect src = pc_surface_content_bounds(s);
    if (pc_rect_is_empty(src)) {
        return PC_OK;
    }
    float *v = NULL, *lifted = NULL, *flat = NULL;
    size_t vlen = 0;
    pc_surface tmp, warped = {0};
    bool have_tmp = false, have_warped = false;
    pc_status rc = pc_surface_read_region(s, src, &v, &vlen, err);
    if (rc != PC_OK) {
        goto fail;
    }
    lifted = calloc(vlen ? vlen * 2 : 1, sizeof *lifted);
    if (lifted == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, "a perspective crop of a mask or channel");
        goto fail;
    }
    for (size_t i = 0; i < vlen; i++) {
        lifted[i * 2] = v[i];
        lifted[i * 2 + 1] = 1.0f;
    }
    pc_pixel_format wa = fmt;
    wa.alpha = true;
    pc_surface_init(&tmp, wa);
    have_tmp = true;
    rc = pc_surface_write_region(&tmp, src, lifted, vlen * 2, err);
    if (rc != PC_OK) {
        goto fail;
    }
    pc_surface_prune(&tmp);
    rc = pc_algo_warp_surface_clipped(&tmp, src, h, interp, clip, &warped, err);
    if (rc != PC_OK) {
        goto fail;
    }
    have_warped = true;
    pc_rect b = pc_rect_intersect(pc_surface_content_bounds(&warped), clip);
    if (!pc_rect_is_empty(b)) {
        float *px = NULL;
        size_t pl = 0;
        rc = pc_surface_read_region(&warped, b, &px, &pl, err);
        if (rc != PC_OK) {
            goto fail;
        }
        size_t npx = pl / 2;
        flat = calloc(npx ? npx : 1, sizeof *flat);
        if (flat == NULL) {
            free(px);
            rc = pc_fail(err, PC_ERR_NOMEM, "a perspective crop of a mask or channel");
            goto fail;
        }
        for (size_t i = 0; i < npx; i++) {
            float g = px[i * 2], a = px[i * 2 + 1];
            flat[i] = g * a + d0 * (1.0f - a);
        }
        free(px);
        rc = pc_surface_write_region(out, b, flat, npx, err);
        if (rc != PC_OK) {
            goto fail;
        }
    }
    pc_surface_prune(out);
    rc = PC_OK;
fail:
    free(v);
    free(lifted);
    free(flat);
    if (have_tmp) {
        pc_surface_fini(&tmp);
    }
    if (have_warped) {
        pc_surface_fini(&warped);
    }
    if (rc != PC_OK) {
        pc_surface_fini(out);
    }
    return rc;
}

static pc_status warp_layers(pc_layer *layers, size_t n, const pc_algo_homography *h, pc_algo_interp interp,
                             pc_rect clip, const float *bg, size_t bgn, bool has_bg, pc_layer_id bg_id,
                             pc_error *err)
{
    for (size_t i = 0; i < n; i++) {
        pc_layer *l = &layers[i];
        bool background = has_bg && l->id == bg_id;
        switch (l->content.kind) {
        case PC_LAYER_GROUP: {
            pc_group *g = &l->content.u.group;
            pc_status rc = warp_layers(g->children, g->nchildren, h, interp, clip, bg, bgn, false, 0, err);
            if (rc != PC_OK) {
                return rc;
            }
            break;
        }
        case PC_LAYER_RASTER: {
            pc_surface *surf = &l->content.u.raster;
            pc_surface warped;
            pc_status rc = pc_algo_warp_surface_clipped(surf, pc_surface_content_bounds(surf), h, interp,
                                                        clip, &warped, err);
            if (rc != PC_OK) {
                return rc;
            }
            if (background) {
                pc_surface base;
                pc_surface_init(&base, pc_surface_format(surf));
                rc = pc_surface_fill_rect(&base, clip, bg, bgn, err);
                if (rc != PC_OK) {
                    pc_surface_fini(&warped);
                    pc_surface_fini(&base);
                    return rc;
                }
                pc_engine_transform_composite_over(&base, &warped, err);
                pc_surface_fini(&warped);
                pc_surface_prune(&base);
                pc_surface_fini(surf);
                *surf = base;
            } else {
                pc_surface_fini(surf);
                *surf = warped;
            }
            break;
        }
        default:
            break; /* adjustment: no pixels; type/shape/fill/smart were rasterized first */
        }
        if (l->has_mask) {
            pc_surface nm;
            pc_status rc = warp_gray(&l->mask.surface, h, interp, clip, &nm, err);
            if (rc != PC_OK) {
                return rc;
            }
            pc_surface_fini(&l->mask.surface);
            l->mask.surface = nm;
        }
        if (l->has_fill_cache) {
            pc_fill_fini(&l->fill_cache.fill);
            pc_surface_fini(&l->fill_cache.surface);
            l->has_fill_cache = false;
        }
    }
    return PC_OK;
}

struct pcrop_ctx {
    bool rasterize;
    double q[4][2];
    pc_algo_homography map;
    uint32_t w, h;
    pc_algo_interp interp;
    bool has_res;
    float res;
    float bg[4];
};

static pc_status apply_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    (void)active;
    struct pcrop_ctx *c = vctx;
    pc_rect clip = pc_rect_from_xywh(0, 0, c->w, c->h);
    float bgpx[PC_RASTER_MAX_CHANNELS];
    size_t bgn = pc_from_rgba(pc_document_pixel_format(doc), c->bg, bgpx);
    const pc_layer *bgl = pc_document_background(doc);
    bool has_bg = bgl != NULL;
    pc_layer_id bg_id = has_bg ? bgl->id : 0;
    pc_status rc = warp_layers(doc->layers, doc->nlayers, &c->map, c->interp, clip, bgpx, bgn, has_bg,
                               bg_id, err);
    if (rc != PC_OK) {
        return rc;
    }
    for (size_t i = 0; i < doc->nchannels; i++) {
        pc_surface nm;
        rc = warp_gray(&doc->channels[i].surface, &c->map, c->interp, clip, &nm, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_surface_fini(&doc->channels[i].surface);
        doc->channels[i].surface = nm;
    }
    if (doc->has_quick_mask) {
        pc_surface nm;
        rc = warp_gray(&doc->quick_mask.surface, &c->map, c->interp, clip, &nm, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_surface_fini(&doc->quick_mask.surface);
        doc->quick_mask.surface = nm;
    }
    if (doc->has_selection) {
        pc_surface_fini(&doc->selection);
        doc->has_selection = false;
    }
    pc_affine aff;
    nearest_affine((const double(*)[2])c->q, (double)c->w, (double)c->h, &aff);
    pc_engine_transform_geometry(doc, &aff);
    doc->size = pc_size_make(c->w, c->h);
    pc_engine_canvas_refresh(doc, false);
    if (c->has_res) {
        doc->resolution_dpi = c->res;
    }
    return PC_OK;
}

static pc_status pcrop_compound(pc_engine_session *s, void *vctx, pc_error *err)
{
    struct pcrop_ctx *c = vctx;
    if (c->rasterize) {
        pc_json_value sub = PC_JSON_VALUE_INIT;
        pc_status rc = pc_engine_session_execute_json(s, "layer.rasterize.allLayers", "{}", &sub, err);
        pc_json_value_fini(&sub);
        if (rc != PC_OK) {
            return rc;
        }
    }
    return pc_engine_session_edit(s, "Perspective Crop", apply_edit, c, err);
}

static pc_status run_pcrop(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    struct pcrop_ctx c;
    memset(&c, 0, sizeof c);
    pc_status rc = corners(p, c.q, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = out_size(p, (const double(*)[2])c.q, &c.w, &c.h, err);
    if (rc != PC_OK) {
        return rc;
    }
    uint64_t bpp = pc_pixel_format_bytes_per_pixel(pc_document_pixel_format(doc));
    if (bpp == 0) {
        bpp = 1;
    }
    if ((uint64_t)c.w * (uint64_t)c.h > MAX_OUTPUT_BYTES / bpp) {
        return pc_engine_err_bad_params(err, CMD, "%u x %u px is too large to resample at this bit depth",
                                        c.w, c.h);
    }
    rc = resolution_of(p, &c.has_res, &c.res, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = interp_of(p, &c.interp, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (!pc_algo_quad_to_rect((const double(*)[2])c.q, (double)c.w, (double)c.h, &c.map)) {
        return pc_engine_err_bad_params(err, CMD, "the corners don't define a perspective");
    }
    /* Type / shape / fill / smart object layers must be rasterized: a projective map has no editable
     * equivalent for them. */
    c.rasterize = false;
    {
        pc_walk w;
        if (pc_document_walk(doc, &w, err) == PC_OK) {
            for (size_t i = 0; i < w.len; i++) {
                pc_layer_kind k = w.items[i].layer->content.kind;
                if (k == PC_LAYER_TEXT || k == PC_LAYER_SHAPE || k == PC_LAYER_FILL ||
                    k == PC_LAYER_SMART) {
                    c.rasterize = true;
                    break;
                }
            }
            pc_walk_fini(&w);
        }
    }
    memcpy(c.bg, s->tools.background, sizeof c.bg);

    rc = pc_engine_compound(s, "Perspective Crop", pcrop_compound, &c, err);
    if (rc != PC_OK) {
        return rc;
    }

    const pc_engine_doc_state *nd = pc_engine_session_active(s);
    float dpi = nd != NULL ? pc_doc_arc_get(nd->doc)->resolution_dpi : 0.0f;
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "width", c.w);
    yyjson_mut_obj_add_uint(rd, root, "height", c.h);
    yyjson_mut_obj_add_real(rd, root, "resolution", (double)dpi);
    yyjson_mut_val *jq = yyjson_mut_arr(rd);
    for (size_t i = 0; i < 4; i++) {
        yyjson_mut_val *pt = yyjson_mut_arr(rd);
        yyjson_mut_arr_add_real(rd, pt, c.q[i][0]);
        yyjson_mut_arr_add_real(rd, pt, c.q[i][1]);
        yyjson_mut_arr_append(jq, pt);
    }
    yyjson_mut_obj_add_val(rd, root, "corners", jq);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

#define PCROP_PARAMS                                                                                  \
    "{\"corners\":[[x,y],[x,y],[x,y],[x,y]] (top-left, top-right, bottom-right, bottom-left; a "       \
    "convex quad, document px),\"width\":px?,\"height\":px?,\"resolution\":ppi?,"                       \
    "\"interpolation\":\"bicubic|bilinear|nearest\"=\"bicubic\"} -> {width,height,resolution,corners} " \
    "(the quad is mapped onto an upright width x height canvas, all layers, one undo step; no "         \
    "width/height: the quad's mean side lengths; type/shape/fill/smart layers are rasterized)"

static const pc_engine_command SPECS[] = {
    {CMD, "Perspective Crop", NULL, 0, NULL, PCROP_PARAMS, pc_engine_has_doc, run_pcrop, true},
};

const pc_engine_command *pc_engine_perspective_crop_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
