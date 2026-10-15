/* Canvas-wide geometry (port of crates/engine/src/canvas_geom.rs): Image Rotation (90s, flips),
 * Canvas Size, Crop, Trim and Image Size move everything in the document. Pixels are remapped by
 * the callers / turn_canvas; this module moves the rest. Vector, type and smart-object content is
 * moved through the transform hooks (owned by E6/E11); E1 moves the pixels, fills, guides, paths,
 * notes, measurement marks, slices, effect reference points and artboards. */
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "doc/fill.h"
#include "doc/vector.h"
#include "engine_internal.h"

static const double EPS = 1e-9;
static const double PI = 3.14159265358979323846; /* std::f64::consts::PI (M_PI is not ISO C) */

static void map_pt(const pc_affine *a, const double in[2], double out[2])
{
    pc_point q = pc_affine_apply(*a, pc_point_make(in[0], in[1]));
    out[0] = q.x;
    out[1] = q.y;
}

/* Bounding box [x0,y0,x1,y1] of the four mapped corners of b. */
static void map_box(const pc_affine *a, const double b[4], double out[4])
{
    double corners[4][2] = {{b[0], b[1]}, {b[2], b[1]}, {b[2], b[3]}, {b[0], b[3]}};
    out[0] = DBL_MAX;
    out[1] = DBL_MAX;
    out[2] = -DBL_MAX;
    out[3] = -DBL_MAX;
    for (int i = 0; i < 4; i++) {
        double p[2];
        map_pt(a, corners[i], p);
        out[0] = fmin(out[0], p[0]);
        out[1] = fmin(out[1], p[1]);
        out[2] = fmax(out[2], p[0]);
        out[3] = fmax(out[3], p[1]);
    }
}

pc_rect pc_engine_map_rect(const pc_affine *a, pc_rect r)
{
    if (pc_rect_is_empty(r)) {
        return r;
    }
    double b[4] = {(double)r.x0, (double)r.y0, (double)r.x1, (double)r.y1};
    double m[4];
    map_box(a, b, m);
    double lo = (double)(INT32_MIN / 2), hi = (double)(INT32_MAX / 2);
    int32_t v[4];
    for (int i = 0; i < 4; i++) {
        double q = round(m[i]);
        if (q < lo) {
            q = lo;
        } else if (q > hi) {
            q = hi;
        }
        v[i] = (int32_t)q;
    }
    return pc_rect_make(v[0], v[1], v[2], v[3]);
}

float pc_engine_map_angle(const pc_affine *a, float deg)
{
    double rad = (double)deg * (PI / 180.0);
    double sn = sin(rad), cs = cos(rad);
    double dx = cs, dy = -sn; /* direction in y-down document space */
    double nx = a->m[0] * dx + a->m[2] * dy;
    double ny = a->m[1] * dx + a->m[3] * dy;
    if (fabs(nx) < EPS && fabs(ny) < EPS) {
        return deg;
    }
    double out = atan2(-ny, nx) * (180.0 / PI);
    if (fabs(out - round(out)) < 1e-6) {
        out = round(out);
    }
    if (out <= -180.0) {
        out += 360.0;
    }
    return (float)out;
}

static void map_fill(const pc_affine *a, pc_fill *f)
{
    if (f->kind != PC_FILL_GRADIENT) {
        return;
    }
    f->u.gradient.angle = pc_engine_map_angle(a, f->u.gradient.angle);
    double ox = (double)f->u.gradient.offset[0], oy = (double)f->u.gradient.offset[1];
    f->u.gradient.offset[0] = (float)(a->m[0] * ox + a->m[2] * oy);
    f->u.gradient.offset[1] = (float)(a->m[1] * ox + a->m[3] * oy);
}

static void transform_layer_geometry(pc_layer *l, const pc_affine *a, bool content, float dpi)
{
    if (l->has_vector_mask && (content || !l->vector_mask.linked)) {
        pc_path np;
        if (pc_path_transform(&l->vector_mask.path, *a, &np, NULL) == PC_OK) {
            pc_path_fini(&l->vector_mask.path);
            l->vector_mask.path = np;
        }
    }
    if (l->effects.has_reference) {
        double p[2];
        map_pt(a, l->effects.reference, p);
        l->effects.reference[0] = p[0];
        l->effects.reference[1] = p[1];
    }
    if (!content) {
        if (l->has_fill_cache) {
            pc_fill_fini(&l->fill_cache.fill);
            pc_surface_fini(&l->fill_cache.surface);
            l->has_fill_cache = false;
        }
    } else if (l->has_fill_cache) {
        map_fill(a, &l->fill_cache.fill);
    }
    switch (l->content.kind) {
    case PC_LAYER_FILL:
        map_fill(a, &l->content.u.fill);
        break;
    case PC_LAYER_SHAPE:
        /* A shape layer's fill angle/offset moves whether or not Free Transform moves the path
         * (matches the reference's transform_layer_geometry; E11). */
        if (l->content.u.shape.has_fill) {
            map_fill(a, &l->content.u.shape.fill);
        }
        if (content) {
            pc_engine_hook_transform_shape(l, a);
        }
        break;
    case PC_LAYER_TEXT:
        if (content) {
            pc_engine_hook_transform_text(l, a, dpi);
        }
        break;
    case PC_LAYER_SMART:
        if (content) {
            pc_engine_hook_transform_smart(l, a);
        }
        break;
    case PC_LAYER_GROUP: {
        pc_group *g = &l->content.u.group;
        if (g->has_artboard) {
            g->artboard.rect = pc_engine_map_rect(a, g->artboard.rect);
        }
        for (size_t i = 0; i < g->nchildren; i++) {
            transform_layer_geometry(&g->children[i], a, content, dpi);
        }
        break;
    }
    default:
        break;
    }
}

void pc_engine_transform_doc_marks(pc_document *doc, const pc_affine *a)
{
    for (size_t i = 0; i < doc->npaths; i++) {
        pc_path np;
        if (pc_path_transform(&doc->paths[i].path, *a, &np, NULL) == PC_OK) {
            pc_path_fini(&doc->paths[i].path);
            doc->paths[i].path = np;
        }
    }
    if (doc->has_work_path) {
        pc_path np;
        if (pc_path_transform(&doc->work_path, *a, &np, NULL) == PC_OK) {
            pc_path_fini(&doc->work_path);
            doc->work_path = np;
        }
    }
    for (size_t i = 0; i < doc->nnotes; i++) {
        double p[2];
        map_pt(a, doc->notes[i].position, p);
        doc->notes[i].position[0] = p[0];
        doc->notes[i].position[1] = p[1];
        double pb[4];
        map_box(a, doc->notes[i].popup, pb);
        memcpy(doc->notes[i].popup, pb, sizeof pb);
    }
    for (size_t g = 0; g < doc->measurement.ncount_groups; g++) {
        pc_count_group *cg = &doc->measurement.count_groups[g];
        for (size_t i = 0; i < cg->npoints; i++) {
            double p[2];
            map_pt(a, cg->points[i], p);
            cg->points[i][0] = p[0];
            cg->points[i][1] = p[1];
        }
    }
    if (doc->measurement.has_ruler) {
        pc_ruler *r = &doc->measurement.ruler;
        double s[2], e[2];
        map_pt(a, r->start, s);
        map_pt(a, r->end, e);
        memcpy(r->start, s, sizeof s);
        memcpy(r->end, e, sizeof e);
        if (r->has_protractor) {
            double pp[2];
            map_pt(a, r->protractor, pp);
            memcpy(r->protractor, pp, sizeof pp);
        }
    }
    for (size_t i = 0; i < doc->slices.len; i++) {
        doc->slices.list[i].rect = pc_engine_map_rect(a, doc->slices.list[i].rect);
    }
    double m0 = a->m[0], m1 = a->m[1], m2 = a->m[2], m3 = a->m[3], tx = a->m[4], ty = a->m[5];
    pc_guides *g = &doc->guides;
    if (fabs(m1) < EPS && fabs(m2) < EPS) {
        for (size_t i = 0; i < g->nhorizontal; i++) {
            g->horizontal[i] = m3 * g->horizontal[i] + ty;
        }
        for (size_t i = 0; i < g->nvertical; i++) {
            g->vertical[i] = m0 * g->vertical[i] + tx;
        }
    } else if (fabs(m0) < EPS && fabs(m3) < EPS) {
        /* A horizontal guide at y becomes a vertical one at x' = m2*y + tx, and vice versa. */
        size_t oh = g->nhorizontal, ov = g->nvertical;
        double *newv = oh > 0 ? pc_malloc_array(oh, sizeof(double)) : NULL; /* from old horizontal */
        double *newh = ov > 0 ? pc_malloc_array(ov, sizeof(double)) : NULL; /* from old vertical */
        if ((oh == 0 || newv != NULL) && (ov == 0 || newh != NULL)) {
            for (size_t i = 0; i < oh; i++) {
                newv[i] = m2 * g->horizontal[i] + tx;
            }
            for (size_t i = 0; i < ov; i++) {
                newh[i] = m1 * g->vertical[i] + ty;
            }
            free(g->horizontal);
            free(g->vertical);
            g->horizontal = newh;
            g->nhorizontal = ov;
            g->vertical = newv;
            g->nvertical = oh;
        } else {
            free(newv);
            free(newh);
        }
    } else {
        free(g->horizontal);
        free(g->vertical);
        g->horizontal = NULL;
        g->nhorizontal = 0;
        g->vertical = NULL;
        g->nvertical = 0;
    }
}

void pc_engine_transform_geometry(pc_document *doc, const pc_affine *a)
{
    float dpi = doc->resolution_dpi;
    for (size_t i = 0; i < doc->nlayers; i++) {
        transform_layer_geometry(&doc->layers[i], a, true, dpi);
    }
    pc_engine_transform_doc_marks(doc, a);
}

/* ---- right-angle turns -------------------------------------------------------------------- */

pc_affine pc_engine_turn_affine(pc_engine_turn t, double w, double h)
{
    pc_affine a = PC_AFFINE_IDENTITY;
    switch (t) {
    case PC_ENGINE_TURN_FLIP_H:
        a.m[0] = -1.0; a.m[1] = 0.0; a.m[2] = 0.0; a.m[3] = 1.0; a.m[4] = w; a.m[5] = 0.0; break;
    case PC_ENGINE_TURN_FLIP_V:
        a.m[0] = 1.0; a.m[1] = 0.0; a.m[2] = 0.0; a.m[3] = -1.0; a.m[4] = 0.0; a.m[5] = h; break;
    case PC_ENGINE_TURN_ROTATE_180:
        a.m[0] = -1.0; a.m[1] = 0.0; a.m[2] = 0.0; a.m[3] = -1.0; a.m[4] = w; a.m[5] = h; break;
    case PC_ENGINE_TURN_CW90:
        a.m[0] = 0.0; a.m[1] = 1.0; a.m[2] = -1.0; a.m[3] = 0.0; a.m[4] = h; a.m[5] = 0.0; break;
    case PC_ENGINE_TURN_CCW90:
        a.m[0] = 0.0; a.m[1] = -1.0; a.m[2] = 1.0; a.m[3] = 0.0; a.m[4] = 0.0; a.m[5] = w; break;
    }
    return a;
}

bool pc_engine_turn_swaps(pc_engine_turn t)
{
    return t == PC_ENGINE_TURN_CW90 || t == PC_ENGINE_TURN_CCW90;
}

typedef struct turn_ctx {
    pc_engine_turn turn;
    int32_t w, h;
} turn_ctx;

static void turn_map(int32_t x, int32_t y, int32_t *nx, int32_t *ny, void *vctx)
{
    const turn_ctx *c = vctx;
    switch (c->turn) {
    case PC_ENGINE_TURN_FLIP_H: *nx = c->w - 1 - x; *ny = y; break;
    case PC_ENGINE_TURN_FLIP_V: *nx = x; *ny = c->h - 1 - y; break;
    case PC_ENGINE_TURN_ROTATE_180: *nx = c->w - 1 - x; *ny = c->h - 1 - y; break;
    case PC_ENGINE_TURN_CW90: *nx = c->h - 1 - y; *ny = x; break;
    case PC_ENGINE_TURN_CCW90: *nx = y; *ny = c->w - 1 - x; break;
    }
}

static void remap_in_place(pc_surface *s, turn_ctx *ctx)
{
    pc_surface n = pc_engine_remap_surface(s, turn_map, ctx, NULL);
    pc_surface_fini(s);
    *s = n;
}

static void remap_layers(pc_layer *layers, size_t n, turn_ctx *ctx)
{
    for (size_t i = 0; i < n; i++) {
        pc_layer *l = &layers[i];
        if (l->has_mask) {
            remap_in_place(&l->mask.surface, ctx);
        }
        if (l->has_fill_cache) {
            remap_in_place(&l->fill_cache.surface, ctx);
        }
        switch (l->content.kind) {
        case PC_LAYER_RASTER:
            remap_in_place(&l->content.u.raster, ctx);
            break;
        case PC_LAYER_SMART:
            if (l->content.u.smart.has_cache) {
                remap_in_place(&l->content.u.smart.cache, ctx);
            }
            if (l->content.u.smart.has_filter_mask) {
                remap_in_place(&l->content.u.smart.filter_mask.surface, ctx);
            }
            break;
        case PC_LAYER_GROUP:
            remap_layers(l->content.u.group.children, l->content.u.group.nchildren, ctx);
            break;
        default:
            /* Text/shape caches are remapped by E11 when those contents exist; a raster-only
             * document (E1's path) has none. */
            break;
        }
    }
}

pc_status pc_engine_turn_canvas(pc_document *doc, pc_engine_turn turn, pc_error *err)
{
    (void)err;
    turn_ctx ctx = {turn, (int32_t)doc->size.width, (int32_t)doc->size.height};
    remap_layers(doc->layers, doc->nlayers, &ctx);
    for (size_t i = 0; i < doc->nchannels; i++) {
        remap_in_place(&doc->channels[i].surface, &ctx);
    }
    if (doc->has_quick_mask) {
        remap_in_place(&doc->quick_mask.surface, &ctx);
    }
    if (doc->has_selection) {
        remap_in_place(&doc->selection, &ctx);
    }
    pc_affine a = pc_engine_turn_affine(turn, (double)doc->size.width, (double)doc->size.height);
    if (pc_engine_turn_swaps(turn)) {
        doc->size = pc_size_make(doc->size.height, doc->size.width);
    }
    pc_engine_transform_geometry(doc, &a);
    return PC_OK;
}
