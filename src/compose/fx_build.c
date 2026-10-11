/* Effect maps of one layer (port of effects.rs FxMaps / build_maps_prepared): everything derived
 * from the layer's shape, computed once over its whole region and cropped per render tile. They
 * depend only on the shape, the effect settings and the global light, never on the backdrop, so
 * the core caches them across tiles and edits (fx_cache.c). A built map is immutable: several
 * threads may read it at once. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "compose/fx_internal.h"

void pc_compose_fx_stroke_widths(const pc_stroke_fx *st, float *in_w, float *out_w)
{
    switch (st->position) {
    case PC_STROKE_OUTSIDE:
        *in_w = 0.0f;
        *out_w = st->size;
        break;
    case PC_STROKE_INSIDE:
        *in_w = st->size;
        *out_w = 0.0f;
        break;
    default:
        *in_w = st->size / 2.0f;
        *out_w = st->size / 2.0f;
        break;
    }
}

static uint32_t f32_bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

/* The frame a gradient stroke aligned with the layer is laid out in: the pixel bounds of the
 * effect shape grown by the stroke's outside width less one pixel (Photoshop spans the gradient
 * over the stroke's extent, not the layer's). */
static pc_rect stroke_frame(const pc_fxm *shape, float out_w, pc_rect rect)
{
    size_t x0 = SIZE_MAX;
    size_t y0 = SIZE_MAX;
    size_t x1 = 0;
    size_t y1 = 0;
    size_t x;
    size_t y;
    int32_t grow = 0;
    for (y = 0; y < shape->h; y++) {
        for (x = 0; x < shape->w; x++) {
            if (shape->v[y * shape->w + x] > PC_FX_INSIDE_EPS) {
                x0 = x < x0 ? x : x0;
                y0 = y < y0 ? y : y0;
                x1 = x + 1 > x1 ? x + 1 : x1;
                y1 = y + 1 > y1 ? y + 1 : y1;
            }
        }
    }
    if (x0 == SIZE_MAX) {
        return rect;
    }
    if (isfinite(out_w)) {
        /* (ceil(min(w, MAX_REACH)) as i32 - 1).max(0), wrapping like the release build. */
        grow = pc_i32_wrapping_sub(pc_f32_to_i32(ceilf(pc_rs_minf_k(out_w, PC_COMPOSE_FX_MAX_REACH))), 1);
        grow = grow > 0 ? grow : 0;
    }
    return pc_rect_inflate(
        pc_rect_make(pc_i32_wrapping_add(rect.x0, (int32_t)x0), pc_i32_wrapping_add(rect.y0, (int32_t)y0),
                     pc_i32_wrapping_add(rect.x0, (int32_t)x1), pc_i32_wrapping_add(rect.y0, (int32_t)y1)),
        grow);
}

bool pc_compose_fx_maps_stroke_frame(const pc_compose_fx_maps *maps, const pc_stroke_fx *st, pc_rect *out)
{
    float in_w;
    float out_w;
    uint32_t bits;
    size_t i;
    if (maps == NULL) {
        return false;
    }
    pc_compose_fx_stroke_widths(st, &in_w, &out_w);
    bits = f32_bits(out_w);
    for (i = 0; i < maps->nframes; i++) {
        if (maps->frames[i].bits == bits) {
            *out = maps->frames[i].rect;
            return true;
        }
    }
    return false;
}

size_t pc_compose_fx_maps_bytes(const pc_compose_fx_maps *maps)
{
    size_t n;
    size_t count = 1;
    size_t i;
    if (maps == NULL) {
        return 0;
    }
    n = pc_fxm_len(&maps->shape);
    for (i = 0; i < maps->nitems; i++) {
        count += maps->per[i].n;
    }
    count += (size_t)(maps->din != NULL) + (size_t)(maps->dout != NULL) + (size_t)(maps->vdout != NULL);
    return count * n * 4;
}

void pc_compose_fx_maps_free(pc_compose_fx_maps *maps)
{
    size_t i;
    if (maps == NULL) {
        return;
    }
    pc_fxm_free(&maps->shape);
    for (i = 0; i < maps->nitems; i++) {
        pc_fxm_free(&maps->per[i].m[0]);
        pc_fxm_free(&maps->per[i].m[1]);
    }
    free(maps->per);
    free(maps->din);
    free(maps->dout);
    free(maps->vdout);
    free(maps->frames);
    free(maps);
}

pc_status pc_fx_crop_vec(const pc_compose_fx_maps *maps, const float *v, pc_rect to, float fill, float **out,
                         pc_error *err)
{
    size_t w = pc_cmp_w(to);
    size_t h = pc_rect_height(to);
    size_t n = w * h;
    size_t i;
    pc_rect r = maps->rect;
    pc_rect inter = pc_rect_intersect(r, to);
    float *o = pc_malloc_array(n > 0 ? n : 1, sizeof *o);
    int32_t y;
    if (o == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
    }
    for (i = 0; i < n; i++) {
        o[i] = fill;
    }
    if (!pc_rect_is_empty(inter) && v != NULL) {
        size_t sw = pc_cmp_w(r);
        size_t cw = pc_cmp_w(inter);
        for (y = inter.y0; y < inter.y1; y++) {
            size_t s0 = (size_t)((int64_t)y - r.y0) * sw + (size_t)((int64_t)inter.x0 - r.x0);
            size_t d0 = (size_t)((int64_t)y - to.y0) * w + (size_t)((int64_t)inter.x0 - to.x0);
            memcpy(o + d0, v + s0, cw * sizeof *o);
        }
    }
    *out = o;
    return PC_OK;
}

pc_status pc_fx_crop(const pc_compose_fx_maps *maps, const pc_fxm *m, pc_rect to, float fill, pc_fxm *out,
                     pc_error *err)
{
    float *v = NULL;
    pc_status st = pc_fx_crop_vec(maps, pc_fxm_len(m) > 0 ? m->v : NULL, to, fill, &v, err);
    if (st != PC_OK) {
        return st;
    }
    *out = pc_fxm_wrap(pc_cmp_w(to), pc_rect_height(to), v);
    return PC_OK;
}

static pc_status build_item(const pc_effect *e, const pc_fxm *shape, pc_rect rect,
                            const pc_global_light *light, const pc_compose_texture_ctx *tex,
                            const pc_compose_patterns *pats, pc_fx_item *it, pc_error *err)
{
    pc_status st = PC_OK;
    it->n = 0;
    it->bevel_paint = PC_COMPOSE_BEVEL_PAINT_INNER;
    switch (e->kind) {
    case PC_FX_DROP_SHADOW:
    case PC_FX_INNER_SHADOW:
        st = pc_fx_shadow_map(shape, &e->u.shadow, light, e->kind == PC_FX_INNER_SHADOW, rect.x0, rect.y0,
                              &it->m[0], err);
        it->n = 1;
        break;
    case PC_FX_OUTER_GLOW:
    case PC_FX_INNER_GLOW:
        st = pc_fx_glow_map(shape, &e->u.glow, e->kind == PC_FX_INNER_GLOW, rect.x0, rect.y0, &it->m[0], err);
        it->n = 1;
        break;
    case PC_FX_SATIN:
        st = pc_fx_satin_map(shape, &e->u.satin, &it->m[0], err);
        it->n = 1;
        break;
    case PC_FX_BEVEL_EMBOSS:
        st = pc_fx_bevel_maps(shape, &e->u.bevel, light, tex, pats, it->m, &it->bevel_paint, err);
        it->n = 2;
        break;
    default:
        break;
    }
    if (st != PC_OK) {
        it->n = 0;
    }
    return st;
}

pc_status pc_compose_fx_build_maps(const pc_layer *layer, float *shape, size_t shape_len, pc_rect region,
                                   const pc_global_light *light, const pc_compose_texture_ctx *tex,
                                   const pc_compose_patterns *patterns, pc_compose_fx_maps **out,
                                   pc_error *err)
{
    size_t w = pc_cmp_w(region);
    size_t h = pc_rect_height(region);
    size_t n;
    size_t i;
    size_t k;
    pc_compose_fx_maps *m;
    pc_compose_patterns *own = NULL;
    pc_compose_texture_ctx no_tex;
    pc_global_light default_light = pc_global_light_default();
    bool has_stroke = false;
    bool vector_shape = layer->content.kind == PC_LAYER_SHAPE;
    pc_status st = PC_OK;
    if (!pc_size_mul(w, h, &n) || n > PC_COMPOSE_MAX_PIXELS) {
        free(shape);
        return pc_fail(err, PC_ERR_LIMIT, "compose: effect region too large");
    }
    if (shape_len != n || (n > 0 && shape == NULL)) {
        free(shape);
        return pc_fail(err, PC_ERR_INVALID, "compose: effect shape has %zu values for a %zux%zu region",
                       shape_len, w, h);
    }
    if (n == 0) {
        free(shape);
        shape = NULL;
    }
    m = calloc(1, sizeof *m);
    if (m == NULL) {
        free(shape);
        return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
    }
    m->rect = region;
    m->shape = pc_fxm_wrap(w, h, shape);
    if (tex == NULL) {
        no_tex.rect = region;
        no_tex.patterns = NULL;
        no_tex.npatterns = 0;
        no_tex.anchor[0] = (double)region.x0;
        no_tex.anchor[1] = (double)region.y0;
        tex = &no_tex;
    }
    if (light == NULL) {
        light = &default_light;
    }
    /* build_maps without a prepared set prepares the texture context's patterns. */
    if (patterns == NULL && tex->npatterns > 0) {
        st = pc_compose_patterns_new(tex->patterns, tex->npatterns, PC_COMPOSE_PREPARED_PATTERN_BYTES, &own,
                                     err);
        if (st != PC_OK) {
            pc_compose_fx_maps_free(m);
            return st;
        }
        patterns = own;
    }
    for (i = 0; i < layer->effects.nitems; i++) {
        if (pc_effect_enabled(&layer->effects.items[i])) {
            m->nitems++;
        }
    }
    m->per = pc_calloc_array(m->nitems > 0 ? m->nitems : 1, sizeof *m->per);
    m->frames = pc_calloc_array(m->nitems > 0 ? m->nitems : 1, sizeof *m->frames);
    if (m->per == NULL || m->frames == NULL) {
        pc_compose_fx_maps_free(m);
        pc_compose_patterns_free(own);
        return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
    }
    for (i = 0, k = 0; i < layer->effects.nitems && st == PC_OK; i++) {
        const pc_effect *e = &layer->effects.items[i];
        if (!pc_effect_enabled(e)) {
            continue;
        }
        st = build_item(e, &m->shape, region, light, tex, patterns, &m->per[k], err);
        if (e->kind == PC_FX_STROKE) {
            has_stroke = true;
        }
        k++;
    }
    m->outline = pc_compose_effect_outline(layer) != NULL;
    if (st == PC_OK && has_stroke) {
        st = pc_fx_dist_inside(&m->shape, PC_FX_CHAMFER, &m->din, err);
        if (st == PC_OK) {
            st = pc_fx_dist_outside(&m->shape, PC_FX_CHAMFER, &m->dout, err);
        }
        /* Without an outline (unfilled shapes), estimate the vector outline from local coverage. */
        if (st == PC_OK && vector_shape && !m->outline) {
            pc_fxm cov;
            st = pc_fx_local_coverage(&m->shape, &cov, err);
            if (st == PC_OK) {
                st = pc_fx_dist_outside(&cov, PC_FX_CHAMFER, &m->vdout, err);
                pc_fxm_free(&cov);
            }
        }
    }
    for (i = 0; i < layer->effects.nitems && st == PC_OK; i++) {
        const pc_effect *e = &layer->effects.items[i];
        float in_w;
        float out_w;
        uint32_t bits;
        size_t j;
        bool seen = false;
        if (!pc_effect_enabled(e) || e->kind != PC_FX_STROKE ||
            e->u.stroke.paint.kind != PC_FX_PAINT_GRADIENT) {
            continue;
        }
        pc_compose_fx_stroke_widths(&e->u.stroke, &in_w, &out_w);
        bits = f32_bits(out_w);
        for (j = 0; j < m->nframes; j++) {
            seen = seen || m->frames[j].bits == bits;
        }
        if (!seen) {
            m->frames[m->nframes].bits = bits;
            m->frames[m->nframes].rect = stroke_frame(&m->shape, out_w, region);
            m->nframes++;
        }
    }
    pc_compose_patterns_free(own);
    if (st != PC_OK) {
        pc_compose_fx_maps_free(m);
        return st;
    }
    *out = m;
    return PC_OK;
}
