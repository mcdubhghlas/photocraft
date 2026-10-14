/* Brush contexts, dab rasterisation, masks and the native "over" helper (render.h). The stroke
 * buffer and the incremental renderer are in stroke.c. */
#include "paint/render.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "paint/pmath.h"
#include "paint/procedural.h"
#include "paint/render_priv.h"
#include "paint/rng.h"

#define SQRT_2_F 1.41421356237309504880f
#define NOISE_SALT 0x006E6F697365ull
/* Computed tips with a smaller radius (in pixels) are rasterised by area, not at pixel centres. */
#define SMALL_TIP_RADIUS 3.0f
/* How far a Height-mode dab at full depth and coverage reaches into the pattern, in pattern
 * ranges (measured against Photoshop). */
#define HEIGHT_REACH 12.0f
/* Sub-pixel sample offsets (a 4x4 grid) for the area coverage of small tips. */
static const float SUBPIXEL[4] = {-0.375f, -0.125f, 0.125f, 0.375f};

void pc_paint_grid_center(double x, double y, float diameter, float *cx, float *cy)
{
    int64_t n = isfinite(diameter) ? pc_f32_to_i64(pc_paint_maxf_k(roundf(diameter), 1.0f)) : 1;
    bool odd = n % 2 == 1;
    *cx = pc_f64_to_f32(odd ? floor(x) + 0.5 : round(x));
    *cy = pc_f64_to_f32(odd ? floor(y) + 0.5 : round(y));
}

void pc_paint_grid_square(double x, double y, float diameter, double out[4])
{
    double n = isfinite(diameter) ? (double)pc_clampf(roundf(diameter), 1.0f, 100000.0f) : 1.0, h = n / 2.0;
    float cx, cy;
    pc_paint_grid_center(x, y, (float)n, &cx, &cy);
    out[0] = (double)cx - h;
    out[1] = (double)cy - h;
    out[2] = (double)cx + h;
    out[3] = (double)cy + h;
}

static float smoothstep(float e0, float e1, float x)
{
    float t = pc_clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float pc_paint_mask_combine(pc_paint_mask_mode mode, float v, float t, float d)
{
    float r, o;
    if (v <= 0.0f) {
        return 0.0f;
    }
#define LERP(a, b) ((a) + ((b) - (a)) * d)
    switch (mode) {
    case PC_PAINT_MASK_SUBTRACT:
        r = pc_paint_maxf_k(v - d * (1.0f - t), 0.0f);
        break;
    case PC_PAINT_MASK_DARKEN:
        r = pc_rs_minf(v, 1.0f - d + d * t);
        break;
    case PC_PAINT_MASK_OVERLAY:
        o = v < 0.5f ? 2.0f * v * t : 1.0f - 2.0f * (1.0f - v) * (1.0f - t);
        r = LERP(v, o);
        break;
    case PC_PAINT_MASK_COLOR_DODGE:
        o = pc_paint_minf_k(v / pc_paint_maxf_k(1.0f - t, 1e-3f), 1.0f);
        r = LERP(v, o);
        break;
    case PC_PAINT_MASK_COLOR_BURN:
        o = 1.0f - pc_paint_minf_k((1.0f - v) / pc_paint_maxf_k(t, 1e-3f), 1.0f);
        r = LERP(v, o);
        break;
    case PC_PAINT_MASK_LINEAR_BURN:
        o = pc_paint_maxf_k(v + t - 1.0f, 0.0f);
        r = LERP(v, o);
        break;
    case PC_PAINT_MASK_HARD_MIX:
        o = v + t > 1.0f ? 1.0f : 0.0f;
        r = LERP(v, o);
        break;
    case PC_PAINT_MASK_LINEAR_HEIGHT:
        r = v * pc_clampf((t - (1.0f - d * v)) * 4.0f + 0.5f, 0.0f, 1.0f);
        break;
    case PC_PAINT_MASK_HEIGHT:
        r = t >= 1.0f - d * v ? v : 0.0f;
        break;
    default: /* Multiply */
        r = v * (1.0f - d + d * t);
        break;
    }
#undef LERP
    return pc_clampf(r, 0.0f, 1.0f);
}

/* Wet Edges: how much paint stays at a pixel whose nearest dab reaches it at rn (0 at a dab's
 * centre, 1 at its rim). The interior keeps half the paint, the rim all of it. */
float pc_paint_wet_factor(float rn)
{
    return 0.5f + 0.5f * smoothstep(0.5f, 1.0f, rn);
}

float pc_paint_texture_combine(pc_paint_mask_mode mode, float v, float t, float d)
{
    float r, reach;
    if (v <= 0.0f) {
        return 0.0f;
    }
    switch (mode) {
    case PC_PAINT_MASK_SUBTRACT:
        r = v - d * t;
        break;
    case PC_PAINT_MASK_COLOR_DODGE:
        r = v / pc_paint_maxf_k(1.0f - d * t, 1e-3f);
        break;
    case PC_PAINT_MASK_COLOR_BURN:
        r = 1.0f - (1.0f - v) / pc_paint_maxf_k(1.0f - d + d * t, 1e-3f);
        break;
    case PC_PAINT_MASK_HEIGHT:
        /* Photoshop steps the depth in 1/255ths here, which shows at the lowest depths. */
        r = HEIGHT_REACH * roundf(d * 255.0f) / 255.0f * v - t;
        break;
    case PC_PAINT_MASK_LINEAR_HEIGHT:
        reach = HEIGHT_REACH * roundf(d * 255.0f) / 255.0f * v;
        r = reach < 1.0f ? reach * (1.0f - t) : reach - t;
        break;
    default:
        r = pc_paint_mask_combine(mode, v, t, d);
        break;
    }
    return isfinite(r) ? pc_clampf(r, 0.0f, 1.0f) : 0.0f;
}

pc_rect pc_paint_rect_around(const pc_paint_dab *d, float reach)
{
    int32_t rr = pc_i32_sat_add(pc_f32_to_i32(ceilf(reach)), 1);
    int32_t cx = pc_f64_to_i32(floor(d->center.x)), cy = pc_f64_to_i32(floor(d->center.y));
    return pc_rect_make(pc_i32_sat_sub(cx, rr), pc_i32_sat_sub(cy, rr),
                        pc_i32_sat_add(pc_i32_sat_add(cx, rr), 1), pc_i32_sat_add(pc_i32_sat_add(cy, rr), 1));
}

pc_status pc_paint_fbuf_zeroed(pc_paint_fbuf *b, size_t n, pc_error *err)
{
    if (n > b->cap) {
        float *nv = pc_grow_array(b->v, &b->cap, n, sizeof *nv);
        if (nv == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for a brush buffer");
        }
        b->v = nv;
    }
    if (n > 0) {
        memset(b->v, 0, n * sizeof *b->v);
    }
    b->len = n;
    return PC_OK;
}

void pc_paint_fbuf_fini(pc_paint_fbuf *b)
{
    free(b->v);
    b->v = NULL;
    b->len = 0;
    b->cap = 0;
}

/* ---- context ---- */

static pc_status prepare_pattern(const pc_paint_brush *b, pc_paint_pattern_image **out, pc_error *err)
{
    const pc_paint_texture *t = &b->texture;
    pc_paint_pattern_image *img = NULL;
    *out = NULL;
    if (!t->enabled) {
        return PC_OK;
    }
    if (t->pattern.kind == PC_PAINT_PATTERN_PROCEDURAL) {
        pc_status st =
            pc_paint_pattern_generate(t->pattern.style, t->pattern.size, t->pattern.seed, &img, err);
        if (st != PC_OK) {
            return st;
        }
    } else {
        const pc_paint_gray_tile *g = pc_paint_pattern_bitmap(&t->pattern);
        const uint16_t *s;
        pc_status st;
        if (g == NULL || !pc_paint_gray_tile_is_valid(g)) {
            return PC_OK;
        }
        s = pc_paint_gray_tile_samples(g);
        st = pc_paint_pattern_image_new(g->width, g->height, &img, err);
        if (st != PC_OK) {
            return st;
        }
        for (size_t i = 0, n = pc_paint_gray_tile_len(g); i < n; i++) {
            img->data[i] = (float)s[i] / 65535.0f;
        }
    }
    for (size_t i = 0, n = img->width * img->height; i < n; i++) {
        img->data[i] = pc_paint_texture_level(t, img->data[i]);
    }
    *out = img;
    return PC_OK;
}

static pc_status tip_mips(const pc_paint_tip *t, pc_paint_mips **out, pc_error *err)
{
    const pc_paint_gray_tile *g = pc_paint_tip_bitmap(t);
    *out = NULL;
    if (g != NULL && pc_paint_gray_tile_is_valid(g)) {
        return pc_paint_mips_new(g, out, err);
    }
    return PC_OK;
}

pc_status pc_paint_context_init(pc_paint_context *ctx, const pc_paint_brush *brush, pc_error *err)
{
    pc_paint_context c;
    pc_status st;
    memset(&c, 0, sizeof c);
    pc_paint_brush_bounded(brush, &c.brush);
    st = tip_mips(&c.brush.tip, &c.tip, err);
    if (st == PC_OK && c.brush.dual_brush.enabled) {
        st = tip_mips(&c.brush.dual_brush.tip, &c.dual_tip, err);
    }
    if (st == PC_OK) {
        st = prepare_pattern(&c.brush, &c.texture, err);
    }
    if (st != PC_OK) {
        pc_paint_context_fini(&c);
        return st;
    }
    *ctx = c;
    return PC_OK;
}

void pc_paint_context_fini(pc_paint_context *ctx)
{
    if (ctx == NULL) {
        return;
    }
    pc_paint_brush_fini(&ctx->brush);
    pc_paint_mips_release(ctx->tip);
    pc_paint_mips_release(ctx->dual_tip);
    pc_paint_pattern_image_release(ctx->texture);
    memset(ctx, 0, sizeof *ctx);
}

void pc_paint_context_copy(pc_paint_context *dst, const pc_paint_context *src)
{
    pc_paint_brush_copy(&dst->brush, &src->brush);
    dst->tip = pc_paint_mips_retain(src->tip);
    dst->dual_tip = pc_paint_mips_retain(src->dual_tip);
    dst->texture = pc_paint_pattern_image_retain(src->texture);
}

static int32_t rem_euclid_i32(int32_t a, int32_t m)
{
    int32_t r = a % m;
    return r < 0 ? r + m : r;
}

float pc_paint_context_texture_at(const pc_paint_context *ctx, int32_t x, int32_t y)
{
    const pc_paint_pattern_image *p = ctx->texture;
    float s, fx, fy, x0, y0, tx, ty, a, b;
    if (p == NULL) {
        return 1.0f;
    }
    s = pc_paint_maxf_k(ctx->brush.texture.scale, 0.01f);
    if (s == 1.0f) {
        /* Pixel centres land exactly on texels at 100 %. */
        return p->data[(size_t)rem_euclid_i32(y, (int32_t)p->height) * p->width +
                       (size_t)rem_euclid_i32(x, (int32_t)p->width)];
    }
    fx = ((float)x + 0.5f) / s - 0.5f;
    fy = ((float)y + 0.5f) / s - 0.5f;
    x0 = floorf(fx);
    y0 = floorf(fy);
    tx = fx - x0;
    ty = fy - y0;
    a = pc_paint_pattern_sample_wrap(p, x0, y0) +
        (pc_paint_pattern_sample_wrap(p, x0 + 1.0f, y0) - pc_paint_pattern_sample_wrap(p, x0, y0)) * tx;
    b = pc_paint_pattern_sample_wrap(p, x0, y0 + 1.0f) +
        (pc_paint_pattern_sample_wrap(p, x0 + 1.0f, y0 + 1.0f) -
         pc_paint_pattern_sample_wrap(p, x0, y0 + 1.0f)) *
            tx;
    return a + (b - a) * ty;
}

pc_rect pc_paint_context_dab_rect(const pc_paint_context *ctx, const pc_paint_dab *d, bool dual)
{
    bool sampled = dual ? ctx->dual_tip != NULL : ctx->tip != NULL;
    float hardness = dual ? ctx->brush.dual_brush.hardness : ctx->brush.hardness;
    bool aliased = ctx->brush.aliased && !dual;
    float reach;
    /* A computed soft tip paints past its nominal radius (see pc_paint_tip_shape). */
    if (sampled) {
        reach = d->radius * SQRT_2_F;
    } else if (aliased) {
        reach = d->radius;
    } else {
        reach = pc_rs_maxf(pc_paint_tip_reach(d->radius, hardness), d->radius);
    }
    return pc_paint_rect_around(d, reach);
}

/* The zero-angle computed round tip without features (the common case), kept apart from the
 * general loop so its rows stay simple; same bits as the general loop (tests/paint). */
static void rasterize_round(const pc_paint_dab *d, pc_rect rect, float *out, size_t w, size_t h,
                            float hardness, bool aliased)
{
    float cx, cy, ro, rm, reach, reach2;
    if (aliased) {
        pc_paint_grid_center(d->center.x, d->center.y, 2.0f * d->radius, &cx, &cy);
    } else {
        cx = pc_f64_to_f32(d->center.x);
        cy = pc_f64_to_f32(d->center.y);
    }
    ro = pc_paint_minf_k(pc_rs_maxf(d->roundness, 0.5f / d->radius), 1.0f);
    rm = pc_paint_maxf_k(d->radius * ro, 0.5f);
    /* A computed soft tip paints past its nominal radius (see pc_paint_tip_shape). */
    reach = aliased ? rm : pc_rs_maxf(pc_paint_tip_reach(rm, hardness), rm);
    reach2 = (reach + 0.5f) * (reach + 0.5f);
    for (size_t yy = 0; yy < h; yy++) {
        float dy = (float)(int32_t)((int64_t)rect.y0 + (int64_t)yy) + 0.5f - cy;
        float *row = out + yy * w;
        for (size_t xx = 0; xx < w; xx++) {
            float dx = (float)(int32_t)((int64_t)rect.x0 + (int64_t)xx) + 0.5f - cx;
            float dr = dx * ro, d2 = dr * dr + dy * dy;
            if (aliased) {
                row[xx] = d2 < reach2 && sqrtf(d2) <= rm ? d->alpha : 0.0f;
            } else {
                float val = d2 >= reach2 ? 0.0f : pc_paint_tip_falloff(sqrtf(d2), rm, hardness);
                row[xx] = val <= 0.0f ? 0.0f : val * d->alpha;
            }
        }
    }
}

/* A pixel offset from the dab centre in tip space: to y-up, project, rotate by -angle, flip. */
typedef struct tip_xf {
    bool proj;
    float pax, pay, pk, sn, cs, fx, fy;
} tip_xf;

static void to_tip(const tip_xf *t, float dx, float dy, float *u, float *v)
{
    float ux = dx, uy = -dy;
    if (t->proj) {
        float k = (ux * t->pax + uy * t->pay) * t->pk;
        ux += k * t->pax;
        uy += k * t->pay;
    }
    *u = (ux * t->cs + uy * t->sn) * t->fx;
    *v = (-ux * t->sn + uy * t->cs) * t->fy;
}

pc_status pc_paint_rasterize_into(const pc_paint_context *ctx, const pc_paint_dab *d, bool dual, pc_rect rect,
                                  pc_paint_fbuf *out, pc_paint_fbuf *depth, bool fast, pc_error *err)
{
    const pc_paint_brush *b = &ctx->brush;
    size_t w = pc_rect_width(rect), h = pc_rect_height(rect), area;
    float hardness = dual ? b->dual_brush.hardness : b->hardness;
    const pc_paint_mips *mips = dual ? ctx->dual_tip : ctx->tip;
    bool aliased = b->aliased && !dual, wet = b->wet_edges && !dual && depth == NULL,
         noise = b->noise && !dual;
    bool tex_tip = !dual && b->texture.enabled && b->texture.each_tip && ctx->texture != NULL;
    bool square = aliased && b->square, small;
    float cx, cy, r, ro, rm, reach, reach2, small_reach2, inv_scale = 1.0f, tw = 1.0f, th = 1.0f;
    tip_xf xf;
    size_t level = 0;
    pc_status st;
    if (!pc_size_mul(w, h, &area) || area > PC_PAINT_MAX_DAB_PIXELS) {
        return pc_fail(err, PC_ERR_LIMIT, "dab too large (%zux%zu pixels)", w, h);
    }
    st = pc_paint_fbuf_zeroed(out, area, err);
    if (st == PC_OK && depth != NULL) {
        st = pc_paint_fbuf_zeroed(depth, area, err);
    }
    if (st != PC_OK || area == 0) {
        return st;
    }
    if (aliased) {
        pc_paint_grid_center(d->center.x, d->center.y, 2.0f * d->radius, &cx, &cy);
    } else {
        cx = pc_f64_to_f32(d->center.x);
        cy = pc_f64_to_f32(d->center.y);
    }
    memset(&xf, 0, sizeof xf);
    xf.sn = sinf(d->angle);
    xf.cs = cosf(d->angle);
    xf.fx = d->flip_x ? -1.0f : 1.0f;
    xf.fy = d->flip_y ? -1.0f : 1.0f;
    /* Brush Projection: stretch the sampling coordinates along the tilt direction, which
     * foreshortens the tip there by proj_scale. */
    xf.proj = !dual && d->proj_scale < 0.999f;
    if (xf.proj) {
        xf.pay = sinf(d->proj_angle);
        xf.pax = cosf(d->proj_angle);
        xf.pk = 1.0f / pc_paint_maxf_k(d->proj_scale, 0.05f) - 1.0f;
    }
    r = d->radius;
    ro = pc_paint_minf_k(pc_rs_maxf(d->roundness, 0.5f / r), 1.0f);
    rm = pc_paint_maxf_k(r * ro, 0.5f);
    /* A computed soft tip paints past its nominal radius (see pc_paint_tip_shape). */
    reach = (aliased || mips != NULL) ? rm : pc_rs_maxf(pc_paint_tip_reach(rm, hardness), rm);
    reach2 = (reach + 0.5f) * (reach + 0.5f);
    if (mips != NULL) {
        size_t w0 = mips->levels[0].width, h0 = mips->levels[0].height;
        float big = (float)(w0 > h0 ? w0 : h0);
        level = pc_paint_mips_level_for(mips, 2.0f * r * pc_paint_maxf_k(ro, 0.25f));
        inv_scale = big / (2.0f * r);
        tw = (float)w0;
        th = (float)h0;
    }
    /* A tip a few pixels across takes each pixel's covered area (a 4x4 grid inside it): sampling
     * only the pixel centre made a tiny dab's total coverage swing with its sub-pixel position. */
    small = mips == NULL && !aliased && rm < SMALL_TIP_RADIUS;
    small_reach2 = (reach + 0.75f) * (reach + 0.75f);
    if (fast && !(mips != NULL || small || square || noise || tex_tip || wet) && depth == NULL && !xf.proj &&
        d->angle == 0.0f && isfinite(d->radius) && isfinite(d->roundness) && isfinite(rm) && isfinite(cx) &&
        isfinite(cy) && isfinite(hardness) && isfinite(d->alpha)) {
        rasterize_round(d, rect, out->v, w, h, hardness, aliased);
        return PC_OK;
    }
    for (size_t yy = 0; yy < h; yy++) {
        int32_t y = (int32_t)((int64_t)rect.y0 + (int64_t)yy);
        float dy = (float)y + 0.5f - cy;
        for (size_t xx = 0; xx < w; xx++) {
            int32_t x = (int32_t)((int64_t)rect.x0 + (int64_t)xx);
            float dx = (float)x + 0.5f - cx, u, v, val, rn;
            to_tip(&xf, dx, dy, &u, &v);
            if (square) {
                /* Block: the whole-pixel square, hard (the Eraser's Block mode). */
                if (fabsf(dx) >= r || fabsf(dy) >= r) {
                    continue;
                }
                val = 1.0f;
                rn = 0.0f;
            } else if (mips == NULL && small) {
                float ur = u * ro, d2 = ur * ur + v * v, sum = 0.0f;
                if (d2 >= small_reach2) {
                    continue;
                }
                for (int sy = 0; sy < 4; sy++) {
                    for (int sx = 0; sx < 4; sx++) {
                        float su, sv, sur, sd;
                        to_tip(&xf, dx + SUBPIXEL[sx], dy + SUBPIXEL[sy], &su, &sv);
                        sur = su * ro;
                        sd = sqrtf(sur * sur + sv * sv);
                        /* Each sample takes the tip's own edge, so the covered area changes
                         * smoothly with the dab's position. */
                        sum += pc_paint_tip_falloff(sd, rm, hardness);
                    }
                }
                val = sum / 16.0f;
                rn = sqrtf(d2) / rm;
            } else if (mips == NULL) {
                float ur = u * ro, d2 = ur * ur + v * v, dd;
                if (d2 >= reach2) {
                    continue;
                }
                dd = sqrtf(d2);
                val = aliased ? (dd <= rm ? 1.0f : 0.0f) : pc_paint_dab_coverage(dd, rm, hardness);
                rn = dd / rm;
            } else {
                float px = u * inv_scale + tw / 2.0f, py = -(v / ro) * inv_scale + th / 2.0f, s;
                if (px < -1.0f || py < -1.0f || px > tw + 1.0f || py > th + 1.0f) {
                    continue;
                }
                s = pc_paint_mips_sample_clamped(mips, level, px / tw, py / th);
                val = aliased ? (s >= 0.5f ? 1.0f : 0.0f) : s;
                rn = 1.0f - s;
            }
            if (val <= 0.0f) {
                continue;
            }
            if (noise && val < 1.0f) {
                float n = pc_paint_hash2(x, y, NOISE_SALT);
                float hard = n < val ? 1.0f : 0.0f;
                val += (hard - val) * 0.7f;
            }
            if (tex_tip) {
                val = pc_paint_texture_combine(b->texture.mode, val, pc_paint_context_texture_at(ctx, x, y),
                                               d->depth);
            }
            if (wet) {
                val *= pc_paint_wet_factor(rn);
            }
            if (depth != NULL) {
                depth->v[yy * w + xx] = pc_clampf(1.0f - rn, 0.0f, 1.0f);
            }
            out->v[yy * w + xx] = val * d->alpha;
        }
    }
    return PC_OK;
}

pc_status pc_paint_context_rasterize(const pc_paint_context *ctx, const pc_paint_dab *d, bool dual,
                                     pc_rect rect, pc_paint_fbuf *out, pc_error *err)
{
    return pc_paint_rasterize_into(ctx, d, dual, rect, out, NULL, true, err);
}

float pc_paint_context_stroke_mask_wet(const pc_paint_context *ctx, float c, const float *depth,
                                       bool has_dual, float dual, int32_t x, int32_t y)
{
    const pc_paint_brush *b = &ctx->brush;
    float m = depth != NULL ? c * pc_paint_wet_factor(1.0f - *depth) : c;
    if (has_dual) {
        m = pc_paint_mask_combine(b->dual_brush.mode, m, dual, 1.0f);
    }
    if (ctx->texture != NULL && !b->texture.each_tip) {
        m = pc_paint_texture_combine(b->texture.mode, m, pc_paint_context_texture_at(ctx, x, y),
                                     pc_clampf(b->texture.depth, 0.0f, 1.0f));
    }
    return m;
}

float pc_paint_context_stroke_mask(const pc_paint_context *ctx, float c, bool has_dual, float dual, int32_t x,
                                   int32_t y)
{
    return pc_paint_context_stroke_mask_wet(ctx, c, NULL, has_dual, dual, x, y);
}

pc_status pc_paint_context_footprint(const pc_paint_context *ctx, const pc_paint_dab *d, size_t index,
                                     pc_paint_footprint *out, pc_error *err)
{
    pc_paint_fbuf buf = {0};
    pc_rect rect = pc_paint_context_dab_rect(ctx, d, false);
    pc_status st = pc_paint_context_rasterize(ctx, d, false, rect, &buf, err);
    if (st != PC_OK) {
        pc_paint_fbuf_fini(&buf);
        return st;
    }
    out->dab = *d;
    out->index = index;
    out->rect = rect;
    out->cov = buf.v;
    out->len = buf.len;
    return PC_OK;
}

void pc_paint_footprint_fini(pc_paint_footprint *f)
{
    if (f == NULL) {
        return;
    }
    free(f->cov);
    f->cov = NULL;
    f->len = 0;
}

float pc_paint_footprint_at(const pc_paint_footprint *f, int32_t x, int32_t y)
{
    size_t i;
    if (!pc_rect_contains(f->rect, x, y)) {
        return 0.0f;
    }
    i = (size_t)((int64_t)y - f->rect.y0) * pc_rect_width(f->rect) + (size_t)((int64_t)x - f->rect.x0);
    return i < f->len ? f->cov[i] : 0.0f;
}

bool pc_paint_alpha_index(pc_pixel_format fmt, size_t *index)
{
    if (!fmt.alpha) {
        return false;
    }
    *index = pc_color_mode_channels(fmt.mode);
    return true;
}

void pc_paint_over_native(pc_pixel_format fmt, float *dst, const float *src, float k, bool lock_transparency)
{
    size_t a;
    if (pc_paint_alpha_index(fmt, &a)) {
        float da = dst[a], sa = src[a] * k, oa = sa + da * (1.0f - sa);
        if (oa > 0.0f) {
            for (size_t c = 0; c < a; c++) {
                dst[c] = (src[c] * sa + dst[c] * da * (1.0f - sa)) / oa;
            }
        }
        if (!lock_transparency) {
            dst[a] = oa;
        }
    } else {
        for (size_t c = 0, n = pc_pixel_format_channels(fmt); c < n; c++) {
            dst[c] += (src[c] - dst[c]) * k;
        }
    }
}
