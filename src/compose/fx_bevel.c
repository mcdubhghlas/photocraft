/* Layer-effect maps, part 3 (port of effects.rs): Bevel & Emboss geometry, height maps (smooth
 * tent, chiselled distance ramps, contour and texture elements, soften) and the highlight /
 * shadow maps shaded from them. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/num.h"
#include "base/rmath.h"
#include "compose/fx_internal.h"

#define PI_F32 3.14159265358979323846f

static float rad_f32(float deg)
{
    const float k = PI_F32 / 180.0f;
    return deg * k;
}

pc_compose_bevel_geom pc_compose_fx_bevel_geom(const pc_bevel *b)
{
    pc_compose_bevel_geom g;
    float size = pc_rs_maxf_k(b->size, 1.0f);
    bool emboss = b->style == PC_BEVEL_EMBOSS || b->style == PC_BEVEL_PILLOW_EMBOSS;
    bool smooth = b->technique == PC_BEVEL_SMOOTH;
    float sign = b->up ? 1.0f : -1.0f;
    switch (b->style) {
    case PC_BEVEL_OUTER:
        g.paint = PC_COMPOSE_BEVEL_PAINT_OUTER;
        break;
    case PC_BEVEL_EMBOSS:
    case PC_BEVEL_PILLOW_EMBOSS:
        g.paint = PC_COMPOSE_BEVEL_PAINT_BOTH;
        break;
    default:
        g.paint = PC_COMPOSE_BEVEL_PAINT_INNER;
        break;
    }
    /* Emboss styles straddle the edge with half the size, rounded up to whole pixels: a 41 px
     * emboss blurs over 21 (psd-tools layer_effects). */
    g.width = emboss ? ceilf(size / 2.0f) : size;
    /* A bevel rises depth x width / 2 px: Smooth with a slope falling from depth at the edge (the
     * tent), a chisel with the constant slope of its ramp (see chisel_h). */
    g.depth = b->depth * (smooth ? g.width : size / 2.0f) * sign;
    g.pillow = b->style == PC_BEVEL_PILLOW_EMBOSS;
    g.chisel_soft = b->technique == PC_BEVEL_CHISEL_SOFT ? pc_rs_maxf_k(size / 4.0f, 1.0f) : 0.0f;
    return g;
}

/* f32::clamp(0, hi) where the reference panics on an invalid hi (NaN, or below 0: a NaN or
 * negative size): such a bound counts as 0 here. */
static float clamp_to(float x, float hi)
{
    if (!(hi >= 0.0f)) {
        hi = 0.0f;
    }
    return pc_clampf(x, 0.0f, hi);
}

float pc_compose_fx_bevel_chisel_h(pc_compose_bevel_paint paint, float size, float din, float dout, bool soft)
{
    /* Photoshop 27.11, across straight edges (sizes 10 and 20, depth 100 and 300 %): a facet has
     * the constant slope depth x size / (2 (size + 1)), one height step of 1 / (size + 1) per
     * pixel at the depth x size / 2 scale of bevel_geom. */
    float ramp = size + 1.0f;
    float half;
    switch (paint) {
    case PC_COMPOSE_BEVEL_PAINT_OUTER:
        if (!soft) {
            float top = (size + 2.0f) / ramp;
            return dout > 0.0f ? clamp_to((size + 1.5f - dout) / ramp, top) : top;
        }
        /* Outer Soft: Outer Hard's ramp a pixel shorter, with a steeper edge pixel. */
        return dout > 0.0f ? 1.0f - pc_clampf((dout + 0.5f) / ramp, 0.0f, 1.0f) : (size + 2.0f) / ramp;
    case PC_COMPOSE_BEVEL_PAINT_BOTH:
        half = size / 2.0f + 0.5f;
        if (din > 0.0f) {
            return 0.5f + 0.5f * pc_clampf(din / half, 0.0f, 1.0f);
        }
        return 0.5f - 0.5f * pc_clampf(dout / half, 0.0f, 1.0f);
    default:
        if (soft) {
            return din > 0.0f ? clamp_to((din + 1.5f) / ramp, (size + 2.0f) / ramp) : 0.0f;
        }
        return din > 0.0f ? pc_clampf((din + 0.5f) / ramp, 0.0f, 1.0f) : 0.0f;
    }
}

/* The bevel's height map in 0..=1 (plus texture). */
static pc_status bevel_height(const pc_fxm *shape, const pc_bevel *b, const pc_compose_bevel_geom *g,
                              const pc_compose_texture_ctx *tex, const pc_compose_patterns *patterns,
                              pc_fxm *out, pc_error *err)
{
    float size = pc_rs_maxf_k(b->size, 1.0f);
    size_t n = pc_fxm_len(shape);
    size_t i;
    pc_fxm h = {0, 0, NULL};
    pc_status st;
    if (b->technique == PC_BEVEL_SMOOTH) {
        st = pc_fxm_clone(shape, &h, err);
        if (st == PC_OK) {
            st = pc_fxm_tent(&h, g->width, err);
        }
    } else {
        /* Linear ramps of the distance to the edge, placed by the anti-aliased coverage of its
         * pixels so a facet is planar at any angle (#1926). */
        float *din = NULL;
        float *dout = NULL;
        bool soft = g->chisel_soft > 0.0f;
        pc_fx_sharp_edges sharp;
        st = pc_fx_sharp_edges_find(shape, &sharp, err);
        if (st == PC_OK) {
            st = pc_fx_bevel_dist_inside(shape, &sharp, size, &din, err);
            if (st == PC_OK) {
                st = pc_fx_bevel_dist_outside(shape, &sharp, size, &dout, err);
            }
            pc_fx_sharp_edges_fini(&sharp);
        }
        if (st == PC_OK) {
            st = pc_fxm_new(shape->w, shape->h, 0.0f, &h, err);
        }
        if (st == PC_OK) {
            for (i = 0; i < n; i++) {
                h.v[i] = pc_compose_fx_bevel_chisel_h(g->paint, size, din[i], dout[i], soft);
            }
            /* Inner and outer Chisel Soft are not blurred (their facets equal Chisel Hard's across a
             * straight edge); emboss styles keep the blur. */
            if (soft && g->paint == PC_COMPOSE_BEVEL_PAINT_BOTH) {
                st = pc_fxm_tent(&h, g->chisel_soft, err);
            }
        }
        free(din);
        free(dout);
    }
    /* Contour element: the height profile through the contour. */
    if (st == PC_OK && b->has_contour) {
        float *l = NULL;
        st = pc_compose_fx_bevel_contour_lut(&b->contour, &l, err);
        if (st == PC_OK) {
            pc_fxm_apply_lut(&h, l, PC_COMPOSE_FX_LUT_SIZE);
        }
        free(l);
    }
    /* Texture element: the pattern's luminance as extra height, scaled so a full-contrast step at
     * 100 % depth slopes like the bevel at its depth. */
    if (st == PC_OK && b->has_texture) {
        const pc_bevel_texture *t = &b->texture;
        pc_compose_pattern_tile *tile = NULL;
        st = pc_fx_tile_get(patterns, &t->id, &t->name, &tile, err);
        if (st == PC_OK && tile != NULL) {
            float unit = fabsf(b->depth) > 1e-6f ? pc_rs_maxf_k(fabsf(g->depth / b->depth), 1e-3f)
                                                 : pc_rs_maxf_k(g->width, 1.0f);
            pc_compose_placement place =
                pc_compose_placement_anchored(tex->anchor, t->link, t->phase, t->scale, 0.0f);
            float k = t->depth / unit;
            int32_t x0 = tex->rect.x0;
            int32_t y0 = tex->rect.y0;
            size_t x;
            size_t y;
            for (y = 0; y < h.h; y++) {
                for (x = 0; x < h.w; x++) {
                    double u;
                    double v;
                    float p[4];
                    float l;
                    pc_compose_placement_map(&place, (double)pc_i32_wrapping_add(x0, (int32_t)x) + 0.5,
                                             (double)pc_i32_wrapping_add(y0, (int32_t)y) + 0.5, &u, &v);
                    pc_compose_pattern_tile_sample(tile, u, v, p);
                    l = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) * p[3];
                    h.v[y * h.w + x] += k * (t->invert ? 1.0f - l : l);
                }
            }
            pc_compose_pattern_tile_release(tile);
        }
    }
    if (st == PC_OK && b->soften >= 1.0f) {
        st = pc_fxm_tent(&h, b->soften, err);
    }
    if (st != PC_OK) {
        pc_fxm_free(&h);
        return st;
    }
    *out = h;
    return PC_OK;
}

typedef struct shade_ctx {
    const pc_fxm *shape;
    const pc_fxm *hmap;
    float light[3];
    float se;
    bool reach_past;
} shade_ctx;

/* A pixel is on the bevel where its height is raised. Emboss styles also shade the last pixel past
 * the blur's reach (a raised 4-neighbour still slopes), outer bevels don't. */
static bool on_bevel(const shade_ctx *c, int64_t x, int64_t y)
{
    if (pc_fxm_get(c->hmap, x, y) > PC_COMPOSE_BEVEL_H_EPS) {
        return true;
    }
    return c->reach_past && (pc_fxm_get(c->hmap, x + 1, y) > PC_COMPOSE_BEVEL_H_EPS ||
                             pc_fxm_get(c->hmap, x - 1, y) > PC_COMPOSE_BEVEL_H_EPS ||
                             pc_fxm_get(c->hmap, x, y + 1) > PC_COMPOSE_BEVEL_H_EPS ||
                             pc_fxm_get(c->hmap, x, y - 1) > PC_COMPOSE_BEVEL_H_EPS);
}

/* Highlight and shadow of the height map's slopes at depth, in region 0 (inside, x the shape's
 * alpha) or 1 (under the layer's edge and outside), through the gloss contour. */
static pc_status shade_into(const shade_ctx *c, float depth, int region_kind, const float *gloss,
                            pc_fxm *hi_out, pc_fxm *sh_out, pc_error *err)
{
    const pc_fxm *shape = c->shape;
    pc_fxm hi = {0, 0, NULL};
    pc_fxm sh = {0, 0, NULL};
    int64_t x;
    int64_t y;
    pc_status st = pc_fxm_new(shape->w, shape->h, 0.0f, &hi, err);
    if (st == PC_OK) {
        st = pc_fxm_new(shape->w, shape->h, 0.0f, &sh, err);
    }
    if (st != PC_OK) {
        pc_fxm_free(&hi);
        return st;
    }
    for (y = 0; y < (int64_t)shape->h; y++) {
        for (x = 0; x < (int64_t)shape->w; x++) {
            size_t i = (size_t)y * shape->w + (size_t)x;
            float gx = (pc_fxm_get(c->hmap, x + 1, y) - pc_fxm_get(c->hmap, x - 1, y)) * 0.5f * depth;
            float gy = (pc_fxm_get(c->hmap, x, y + 1) - pc_fxm_get(c->hmap, x, y - 1)) * 0.5f * depth;
            float n0 = -gx;
            float n1 = -gy;
            float len = sqrtf(n0 * n0 + n1 * n1 + 1.0f);
            float shade = (n0 * c->light[0] + n1 * c->light[1] + 1.0f * c->light[2]) / len;
            float region = region_kind == 0
                               ? shape->v[i]
                               : ((shape->v[i] < 1.0f - PC_FX_INSIDE_EPS && on_bevel(c, x, y)) ? 1.0f : 0.0f);
            float k = shade - c->se;
            if (k > 0.0f) {
                hi.v[i] = pc_clampf(k / pc_rs_maxf_k(1.0f - c->se, 1e-3f), 0.0f, 1.0f) * region;
            } else {
                sh.v[i] = pc_clampf(-k / pc_rs_maxf_k(c->se, 1e-3f), 0.0f, 1.0f) * region;
            }
        }
    }
    pc_fxm_apply_lut(&hi, gloss, PC_COMPOSE_FX_LUT_SIZE);
    pc_fxm_apply_lut(&sh, gloss, PC_COMPOSE_FX_LUT_SIZE);
    *hi_out = hi;
    *sh_out = sh;
    return PC_OK;
}

/* One map per colour over the whole emboss: the inside half where the shape is (x its alpha),
 * the outside half for the rest of each pixel. Consumes a and b into a. */
static void mix_halves(pc_fxm *a, const pc_fxm *b, const pc_fxm *shape)
{
    size_t i;
    size_t n = pc_fxm_len(a);
    for (i = 0; i < n; i++) {
        float s = shape->v[i];
        a->v[i] = (s > PC_FX_INSIDE_EPS ? a->v[i] : 0.0f) + b->v[i] * (1.0f - pc_rs_minf_k(s, 1.0f));
    }
}

pc_status pc_fx_bevel_maps(const pc_fxm *shape, const pc_bevel *b, const pc_global_light *light,
                           const pc_compose_texture_ctx *tex, const pc_compose_patterns *patterns,
                           pc_fxm maps[2], pc_compose_bevel_paint *paint, pc_error *err)
{
    pc_compose_bevel_geom g = pc_compose_fx_bevel_geom(b);
    pc_fxm hmap = {0, 0, NULL};
    float angle = b->use_global_light ? light->angle : b->angle;
    float altitude = b->use_global_light ? light->altitude : b->altitude;
    float ar = rad_f32(angle);
    float er = rad_f32(altitude);
    float sa = sinf(ar);
    float ca = cosf(ar);
    float se = sinf(er);
    float ce = cosf(er);
    float *gloss = NULL;
    shade_ctx c;
    pc_fxm hi = {0, 0, NULL};
    pc_fxm sh = {0, 0, NULL};
    pc_status st = bevel_height(shape, b, &g, tex, patterns, &hmap, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_compose_fx_contour_lut(&b->gloss_contour, &gloss, err);
    if (st != PC_OK) {
        pc_fxm_free(&hmap);
        return st;
    }
    c.shape = shape;
    c.hmap = &hmap;
    c.light[0] = ca * ce;
    c.light[1] = -sa * ce;
    c.light[2] = se;
    c.se = se;
    /* Outer chisel bevels also shade the pixel where their ramp reaches zero (Photoshop 27.11, half
     * strength at size + 1.5 px out). */
    c.reach_past = g.paint == PC_COMPOSE_BEVEL_PAINT_BOTH ||
                   (g.paint == PC_COMPOSE_BEVEL_PAINT_OUTER && b->technique != PC_BEVEL_SMOOTH);
    if (g.paint != PC_COMPOSE_BEVEL_PAINT_BOTH) {
        st = shade_into(&c, g.depth, g.paint == PC_COMPOSE_BEVEL_PAINT_OUTER ? 1 : 0, gloss, &hi, &sh, err);
    } else {
        /* A 1 % edge pixel of a pillow emboss takes the outside shading, a 75 % one of an emboss
         * mostly the inside. */
        pc_fxm ho = {0, 0, NULL};
        pc_fxm so = {0, 0, NULL};
        st = shade_into(&c, g.depth, 0, gloss, &hi, &sh, err);
        if (st == PC_OK) {
            st = shade_into(&c, g.pillow ? -g.depth : g.depth, 1, gloss, &ho, &so, err);
            if (st == PC_OK) {
                mix_halves(&hi, &ho, shape);
                mix_halves(&sh, &so, shape);
                pc_fxm_free(&ho);
                pc_fxm_free(&so);
            } else {
                pc_fxm_free(&hi);
                pc_fxm_free(&sh);
            }
        }
    }
    free(gloss);
    pc_fxm_free(&hmap);
    if (st != PC_OK) {
        return st;
    }
    maps[0] = hi;
    maps[1] = sh;
    *paint = g.paint;
    return PC_OK;
}
