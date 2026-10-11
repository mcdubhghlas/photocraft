/* Layer-effect maps, part 2 (port of effects.rs): painting colour, gradients and patterns
 * (C2's prepared tiles, compose/pattern.h) through a coverage map, and glow gradients. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "compose/fx_internal.h"

static pc_status nomem(pc_error *err)
{
    return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
}

/* ---- pattern tiles ---- */

pc_status pc_fx_tile_get(const pc_compose_patterns *patterns, const pc_str *id, const pc_str *name,
                         pc_compose_pattern_tile **out, pc_error *err)
{
    return pc_compose_patterns_get(patterns, id->ptr != NULL ? id->ptr : "", id->len,
                                   name->ptr != NULL ? name->ptr : "", name->len, out, err);
}

/* ---- gradients ---- */

/* PreparedGradient: stops converted to RGB once per paint call (CMYK stops through that call's
 * active profile). */
typedef struct prep_gradient {
    float (*color)[4]; /* pos, r, g, b */
    size_t ncolor;
    float (*opacity)[4];
    size_t nopacity;
} prep_gradient;

static void stops_sample(const float (*stops)[4], size_t n, float t, float out[3])
{
    size_t i;
    if (n == 0) {
        out[0] = out[1] = out[2] = t;
        return;
    }
    if (n == 1 || t <= stops[0][0]) {
        out[0] = stops[0][1];
        out[1] = stops[0][2];
        out[2] = stops[0][3];
        return;
    }
    for (i = 0; i + 1 < n; i++) {
        const float *a = stops[i];
        const float *b = stops[i + 1];
        if (t <= b[0]) {
            float k = b[0] > a[0] ? (t - a[0]) / (b[0] - a[0]) : 0.0f;
            int c;
            for (c = 0; c < 3; c++) {
                out[c] = a[c + 1] + (b[c + 1] - a[c + 1]) * k;
            }
            return;
        }
    }
    out[0] = stops[n - 1][1];
    out[1] = stops[n - 1][2];
    out[2] = stops[n - 1][3];
}

static pc_status prep_new(const pc_gradient *g, prep_gradient *out, pc_error *err)
{
    size_t i;
    out->ncolor = g->nstops;
    out->nopacity = g->nopacity_stops;
    out->color = pc_malloc_array(g->nstops > 0 ? g->nstops : 1, sizeof *out->color);
    out->opacity = pc_malloc_array(g->nopacity_stops > 0 ? g->nopacity_stops : 1, sizeof *out->opacity);
    if (out->color == NULL || out->opacity == NULL) {
        free(out->color);
        free(out->opacity);
        return nomem(err);
    }
    for (i = 0; i < g->nstops; i++) {
        out->color[i][0] = g->stops[i].pos;
        pc_color_to_rgb(&g->stops[i].color, &out->color[i][1]);
    }
    for (i = 0; i < g->nopacity_stops; i++) {
        out->opacity[i][0] = g->opacity_stops[i].pos;
        out->opacity[i][1] = out->opacity[i][2] = out->opacity[i][3] = g->opacity_stops[i].opacity;
    }
    return PC_OK;
}

static void prep_fini(prep_gradient *p)
{
    free(p->color);
    free(p->opacity);
}

static void prep_sample(const prep_gradient *p, float t, float out[4])
{
    float a[3];
    stops_sample((const float (*)[4])p->color, p->ncolor, t, out);
    if (p->nopacity == 0) {
        out[3] = 1.0f;
    } else {
        stops_sample((const float (*)[4])p->opacity, p->nopacity, t, a);
        out[3] = a[0];
    }
}

void pc_compose_fx_sample_gradient(const pc_gradient *g, float t, float out[4])
{
    /* Same walk as prep_sample, converting the two stops it reads on the fly (no allocation). */
    float st[2][4];
    size_t n = g->nstops;
    size_t i;
    out[3] = 1.0f;
    if (n == 0) {
        out[0] = out[1] = out[2] = t;
    } else if (n == 1 || t <= g->stops[0].pos) {
        pc_color_to_rgb(&g->stops[0].color, out);
    } else {
        bool done = false;
        for (i = 0; i + 1 < n && !done; i++) {
            if (t <= g->stops[i + 1].pos) {
                float out3[3];
                st[0][0] = g->stops[i].pos;
                st[1][0] = g->stops[i + 1].pos;
                pc_color_to_rgb(&g->stops[i].color, &st[0][1]);
                pc_color_to_rgb(&g->stops[i + 1].color, &st[1][1]);
                stops_sample((const float (*)[4])st, 2, t, out3);
                /* With two stops the walk above reaches the same pair: t > the first position
                 * (checked) and t <= the second. */
                out[0] = out3[0];
                out[1] = out3[1];
                out[2] = out3[2];
                done = true;
            }
        }
        if (!done) {
            pc_color_to_rgb(&g->stops[n - 1].color, out);
        }
    }
    if (g->nopacity_stops > 0) {
        size_t m = g->nopacity_stops;
        const pc_opacity_stop *o = g->opacity_stops;
        if (m == 1 || t <= o[0].pos) {
            out[3] = o[0].opacity;
        } else {
            out[3] = o[m - 1].opacity;
            for (i = 0; i + 1 < m; i++) {
                if (t <= o[i + 1].pos) {
                    float k = o[i + 1].pos > o[i].pos ? (t - o[i].pos) / (o[i + 1].pos - o[i].pos) : 0.0f;
                    out[3] = o[i].opacity + (o[i + 1].opacity - o[i].opacity) * k;
                    break;
                }
            }
        }
    }
}

/* ---- painting ---- */

/* Composite colour(i) x coverage into dst (both over the same rectangle). */
#define PAINT_LOOP(dst, m, opacity, blend, COLOR)                                                            \
    do {                                                                                                     \
        size_t i_;                                                                                           \
        size_t n_ = (dst)->len < pc_fxm_len(m) ? (dst)->len : pc_fxm_len(m);                                 \
        for (i_ = 0; i_ < n_; i_++) {                                                                        \
            float a_ = (m)->v[i_] * (opacity);                                                               \
            float c_[4];                                                                                     \
            if (a_ <= 0.0f) {                                                                                \
                continue;                                                                                    \
            }                                                                                                \
            COLOR(i_, c_);                                                                                   \
            c_[3] *= a_;                                                                                     \
            pc_psblend_composite((blend), (dst)->px[i_], c_, 1.0f, (dst)->px[i_]);                           \
        }                                                                                                    \
    } while (0)

void pc_fx_paint_color(pc_compose_buffer *dst, const pc_fxm *m, const float c[3], pc_blend_mode blend,
                       float opacity)
{
#define SOLID(i, out) ((out)[0] = c[0], (out)[1] = c[1], (out)[2] = c[2], (out)[3] = 1.0f)
    PAINT_LOOP(dst, m, opacity, blend, SOLID);
#undef SOLID
}

pc_status pc_fx_paint_pattern(pc_compose_buffer *dst, const pc_fxm *m, const pc_compose_patterns *patterns,
                              const pc_str *name, const pc_str *id, const pc_compose_placement *place,
                              pc_rect big, pc_blend_mode blend, float opacity, pc_error *err)
{
    pc_compose_pattern_tile *tile = NULL;
    size_t w = pc_cmp_w(big);
    pc_status st = pc_fx_tile_get(patterns, id, name, &tile, err);
    if (st != PC_OK || tile == NULL || w == 0) {
        pc_compose_pattern_tile_release(tile);
        return st;
    }
#define PAT(i, out)                                                                                          \
    do {                                                                                                     \
        double u_;                                                                                           \
        double v_;                                                                                           \
        double x_ = (double)pc_i32_wrapping_add(big.x0, (int32_t)((i) % w)) + 0.5;                           \
        double y_ = (double)pc_i32_wrapping_add(big.y0, (int32_t)((i) / w)) + 0.5;                           \
        pc_compose_placement_map(place, x_, y_, &u_, &v_);                                                   \
        pc_compose_pattern_tile_sample(tile, u_, v_, (out));                                                 \
    } while (0)
    PAINT_LOOP(dst, m, opacity, blend, PAT);
#undef PAT
    pc_compose_pattern_tile_release(tile);
    return PC_OK;
}

pc_status pc_fx_paint_gradient(pc_compose_buffer *dst, const pc_fxm *m, const pc_gradient *g,
                               pc_rect shape_bounds, pc_rect big, pc_blend_mode blend, float opacity,
                               pc_error *err)
{
    prep_gradient prep;
    pc_compose_gradient_geom geom =
        pc_compose_gradient_layout(g->style, g->angle, g->scale, g->offset, shape_bounds);
    size_t w = pc_cmp_w(big);
    pc_status st;
    if (w == 0) {
        return PC_OK;
    }
    st = prep_new(g, &prep, err);
    if (st != PC_OK) {
        return st;
    }
#define GRAD(i, out)                                                                                         \
    do {                                                                                                     \
        float x_ = (float)pc_i32_wrapping_add(big.x0, (int32_t)((i) % w)) + 0.5f;                            \
        float y_ = (float)pc_i32_wrapping_add(big.y0, (int32_t)((i) / w)) + 0.5f;                            \
        prep_sample(&prep,                                                                                   \
                    pc_compose_gradient_t(g->style, geom.angle, geom.scale, g->reverse, geom.offset,         \
                                          shape_bounds, x_, y_),                                             \
                    (out));                                                                                  \
    } while (0)
    PAINT_LOOP(dst, m, opacity, blend, GRAD);
#undef GRAD
    prep_fini(&prep);
    return PC_OK;
}

pc_status pc_fx_paint_fx(pc_compose_buffer *dst, const pc_fxm *m, const pc_fx_paint *p, pc_rect shape_bounds,
                         const double anchor[2], pc_rect big, pc_blend_mode blend, float opacity,
                         const pc_compose_patterns *patterns, pc_error *err)
{
    switch (p->kind) {
    case PC_FX_PAINT_COLOR: {
        float c[3];
        pc_color_to_rgb(&p->u.color, c);
        pc_fx_paint_color(dst, m, c, blend, opacity);
        return PC_OK;
    }
    case PC_FX_PAINT_GRADIENT:
        return pc_fx_paint_gradient(dst, m, &p->u.gradient, shape_bounds, big, blend, opacity, err);
    default: {
        const float phase[2] = {0.0f, 0.0f};
        pc_compose_placement place =
            pc_compose_placement_anchored(anchor, true, phase, p->u.pattern.scale, 0.0f);
        return pc_fx_paint_pattern(dst, m, patterns, &p->u.pattern.name, &p->u.pattern.id, &place, big, blend,
                                   opacity, err);
    }
    }
}

/* A glow's paint over its map. A gradient runs along the glow: where its strength is v the
 * colour is the gradient at 1 - v (the first stop hugs the edge), opaque from v = range^2 on. */
pc_status pc_fx_paint_glow(pc_compose_buffer *dst, const pc_fxm *m, const pc_glow *g, pc_rect shape_bounds,
                           const double anchor[2], pc_rect big, const pc_compose_patterns *patterns,
                           pc_error *err)
{
    prep_gradient prep;
    float gain;
    pc_fxm strength = {0, 0, NULL};
    size_t i;
    size_t n = pc_fxm_len(m);
    pc_status st;
    if (g->paint.kind != PC_FX_PAINT_GRADIENT) {
        return pc_fx_paint_fx(dst, m, &g->paint, shape_bounds, anchor, big, g->common.blend,
                              g->common.opacity, patterns, err);
    }
    gain = pc_compose_fx_glow_gradient_gain(g->range);
    st = pc_fxm_new(m->w, m->h, 0.0f, &strength, err);
    if (st != PC_OK) {
        return st;
    }
    for (i = 0; i < n; i++) {
        strength.v[i] = pc_rs_minf_k(m->v[i] * gain, 1.0f);
    }
    st = prep_new(&g->paint.u.gradient, &prep, err);
    if (st != PC_OK) {
        pc_fxm_free(&strength);
        return st;
    }
#define GLOW(i, out) prep_sample(&prep, 1.0f - pc_clampf((i) < n ? m->v[(i)] : 0.0f, 0.0f, 1.0f), (out))
    PAINT_LOOP(dst, &strength, g->common.opacity, g->common.blend, GLOW);
#undef GLOW
    prep_fini(&prep);
    pc_fxm_free(&strength);
    return PC_OK;
}
