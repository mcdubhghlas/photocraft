/* Vector commands and the cross-module vector primitives (port of
 * crates/engine/src/vector_cmds.rs, E11).
 *
 * This file also hosts the canvas-geometry content hooks for shape layers
 * (pc_engine_hook_transform_shape / pc_engine_hook_refresh_shape), relocated from hooks.c: when
 * Image Rotation / Free Transform moves a shape layer's non-pixel geometry, canvas_geom.c calls
 * these. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/blob.h"
#include "base/rmath.h"
#include "vector/vector.h"

#include "engine_internal.h"
#include "vector_cmds.h"

/* ---- cross-module primitives -------------------------------------------------------------- */

void pc_engine_refresh_shape(const pc_document *doc, pc_shape_layer *sh)
{
    pc_surface rendered;
    if (pc_vec_render_shape(sh, pc_document_pixel_format(doc), pc_document_bounds(doc), &rendered, NULL) !=
        PC_OK) {
        return; /* best effort: keep the old cache on OOM */
    }
    if (sh->has_cache) {
        pc_surface_fini(&sh->cache);
    }
    sh->cache = rendered;
    sh->has_cache = true;
}

void pc_engine_transform_shape(pc_shape_layer *sh, const pc_affine *a)
{
    pc_path np;
    if (pc_path_transform(&sh->path, *a, &np, NULL) == PC_OK) {
        pc_path_fini(&sh->path);
        sh->path = np;
    }
    const double m0 = a->m[0], m1 = a->m[1], m2 = a->m[2], m3 = a->m[3], tx = a->m[4], ty = a->m[5];
    bool axis_aligned = fabs(m1) < 1e-12 && fabs(m2) < 1e-12 && m0 > 0.0 && m3 > 0.0;
    bool had_live = sh->has_live;
    pc_live_shape l = sh->live;
    sh->has_live = false; /* default: the shape is no longer parametric */

    if (had_live && axis_aligned) {
        pc_live_shape out = l;
        /* scaled rect [x, y, w, h] and point helpers */
        double rr[4] = {l.rect[0] * m0 + tx, l.rect[1] * m3 + ty, l.rect[2] * m0, l.rect[3] * m3};
        switch (l.kind) {
        case PC_LIVE_RECT: {
            double k = pc_rs_min(m0, m3);
            memcpy(out.rect, rr, sizeof rr);
            for (int i = 0; i < 4; i++) {
                out.radii[i] = l.radii[i] * k;
            }
            break;
        }
        case PC_LIVE_ELLIPSE:
            memcpy(out.rect, rr, sizeof rr);
            break;
        case PC_LIVE_POLYGON:
            memcpy(out.rect, rr, sizeof rr);
            break;
        case PC_LIVE_LINE: {
            double dx = l.to[0] - l.from[0], dy = l.to[1] - l.from[1];
            double old_length = hypot(dx, dy);
            double new_length = hypot(dx * m0, dy * m3);
            double width_scale =
                (old_length > 0.0 && new_length > 0.0) ? m0 * m3 * old_length / new_length : pc_rs_min(m0, m3);
            out.from[0] = l.from[0] * m0 + tx;
            out.from[1] = l.from[1] * m3 + ty;
            out.to[0] = l.to[0] * m0 + tx;
            out.to[1] = l.to[1] * m3 + ty;
            out.weight = l.weight * width_scale;
            break;
        }
        default:
            break;
        }
        sh->live = out;
        sh->has_live = true;
    } else if (had_live && ((fabs(m1) < 1e-12 && fabs(m2) < 1e-12) || (fabs(m0) < 1e-12 && fabs(m3) < 1e-12))) {
        /* Flips and 90-degree turns keep rectangles, ellipses and lines live (Image Rotation). */
        double k = pc_rs_min(fabs(m0) + fabs(m1), fabs(m2) + fabs(m3));
        /* r(): bounding box of the two mapped rect corners */
        double p0x = m0 * l.rect[0] + m2 * l.rect[1] + tx;
        double p0y = m1 * l.rect[0] + m3 * l.rect[1] + ty;
        double p1x = m0 * (l.rect[0] + l.rect[2]) + m2 * (l.rect[1] + l.rect[3]) + tx;
        double p1y = m1 * (l.rect[0] + l.rect[2]) + m3 * (l.rect[1] + l.rect[3]) + ty;
        double rr[4] = {pc_rs_min(p0x, p1x), pc_rs_min(p0y, p1y), fabs(p1x - p0x), fabs(p1y - p0y)};
        pc_live_shape out = l;
        bool keep = false;
        switch (l.kind) {
        case PC_LIVE_RECT: {
            bool equal = fabs(l.radii[1] - l.radii[0]) < 1e-9 && fabs(l.radii[2] - l.radii[0]) < 1e-9 &&
                         fabs(l.radii[3] - l.radii[0]) < 1e-9;
            if (equal) {
                memcpy(out.rect, rr, sizeof rr);
                for (int i = 0; i < 4; i++) {
                    out.radii[i] = l.radii[i] * k;
                }
                keep = true;
            }
            break;
        }
        case PC_LIVE_ELLIPSE:
            memcpy(out.rect, rr, sizeof rr);
            keep = true;
            break;
        case PC_LIVE_LINE:
            out.from[0] = m0 * l.from[0] + m2 * l.from[1] + tx;
            out.from[1] = m1 * l.from[0] + m3 * l.from[1] + ty;
            out.to[0] = m0 * l.to[0] + m2 * l.to[1] + tx;
            out.to[1] = m1 * l.to[0] + m3 * l.to[1] + ty;
            out.weight = l.weight * k;
            keep = true;
            break;
        default:
            break;
        }
        if (keep) {
            sh->live = out;
            sh->has_live = true;
        }
    }
    pc_blob_release(sh->psd_raw);
    sh->psd_raw = NULL;
}

void pc_engine_translate_vectors(const pc_document *doc, pc_layer *l, double dx, double dy)
{
    pc_affine a = pc_affine_translate(dx, dy);
    if (l->has_vector_mask && l->vector_mask.linked) {
        pc_path np;
        if (pc_path_transform(&l->vector_mask.path, a, &np, NULL) == PC_OK) {
            pc_path_fini(&l->vector_mask.path);
            l->vector_mask.path = np;
        }
    }
    switch (l->content.kind) {
    case PC_LAYER_SHAPE:
        pc_engine_transform_shape(&l->content.u.shape, &a);
        pc_engine_refresh_shape(doc, &l->content.u.shape);
        break;
    case PC_LAYER_GROUP: {
        pc_group *g = &l->content.u.group;
        for (size_t i = 0; i < g->nchildren; i++) {
            pc_engine_translate_vectors(doc, &g->children[i], dx, dy);
        }
        break;
    }
    default:
        break;
    }
}

/* Is every anchor and control point of `path` a finite point? */
static bool finite_path(const pc_path *path)
{
    for (size_t s = 0; s < path->nsubpaths; s++) {
        const pc_subpath *sp = &path->subpaths[s];
        for (size_t k = 0; k < sp->nknots; k++) {
            const pc_knot *kn = &sp->knots[k];
            const pc_point ps[3] = {kn->anchor, kn->in_ctrl, kn->out_ctrl};
            for (int i = 0; i < 3; i++) {
                if (!isfinite(ps[i].x) || !isfinite(ps[i].y)) {
                    return false;
                }
            }
        }
    }
    return true;
}

static pc_point homography_map(pc_point p, void *ctx)
{
    const pc_algo_homography *h = ctx;
    double ox, oy;
    pc_algo_homography_apply(h, p.x, p.y, &ox, &oy);
    return pc_point_make(ox, oy);
}

static pc_point warp_map(pc_point p, void *ctx)
{
    const pc_warp *w = ctx;
    return pc_warp_map(w, p.x, p.y);
}

pc_status pc_engine_map_path(const pc_path *path, const pc_algo_homography *h, const pc_warp *warp,
                             pc_path *out, pc_error *err)
{
    pc_path tmp;
    pc_status st;
    if (warp != NULL) {
        st = pc_vec_map_path(path, warp_map, (void *)(uintptr_t)warp, 0.1, &tmp, err);
    } else if (h != NULL) {
        st = pc_vec_map_points(path, homography_map, (void *)(uintptr_t)h, &tmp, err);
    } else {
        st = pc_path_clone(path, &tmp, err);
    }
    if (st != PC_OK) {
        return st;
    }
    if (!finite_path(&tmp)) {
        pc_path_fini(&tmp);
        return pc_engine_err_other(err, "the distortion sends part of the path out of bounds");
    }
    *out = tmp;
    return PC_OK;
}

pc_status pc_engine_map_shape(pc_shape_layer *sh, const pc_algo_homography *h, const pc_warp *warp,
                              pc_error *err)
{
    pc_path mapped;
    pc_status st = pc_engine_map_path(&sh->path, h, warp, &mapped, err);
    if (st != PC_OK) {
        return st;
    }
    pc_path_fini(&sh->path);
    sh->path = mapped;
    sh->has_live = false;
    pc_blob_release(sh->psd_raw);
    sh->psd_raw = NULL;
    return PC_OK;
}

/* ---- relocated canvas_geom content hooks (were E1-safe noops in hooks.c) ------------------- */

void pc_engine_hook_transform_shape(pc_layer *l, const pc_affine *a)
{
    if (l->content.kind == PC_LAYER_SHAPE) {
        pc_engine_transform_shape(&l->content.u.shape, a);
    }
}

void pc_engine_hook_refresh_shape(const pc_document *snapshot, pc_layer *l)
{
    if (l->content.kind == PC_LAYER_SHAPE) {
        pc_engine_refresh_shape(snapshot, &l->content.u.shape);
    }
}
