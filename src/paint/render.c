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
#include "paint/rng.h"

#define SQRT_2_F 1.41421356237309504880f
#define NOISE_SALT 0x006E6F697365ull

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
    float br, ct;
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
        const pc_paint_gray_tile *g = &t->pattern.tile;
        const uint16_t *s = pc_paint_gray_tile_samples(g);
        pc_status st;
        if (!pc_paint_gray_tile_is_valid(g)) {
            return PC_OK;
        }
        st = pc_paint_pattern_image_new(g->width, g->height, &img, err);
        if (st != PC_OK) {
            return st;
        }
        for (size_t i = 0, n = pc_paint_gray_tile_len(g); i < n; i++) {
            img->data[i] = (float)s[i] / 65535.0f;
        }
    }
    br = pc_clampf(t->brightness, -1.0f, 1.0f);
    ct = pc_clampf(t->contrast, -1.0f, 1.0f);
    for (size_t i = 0, n = img->width * img->height; i < n; i++) {
        float x = pc_clampf((img->data[i] - 0.5f) * (1.0f + ct) + 0.5f + br, 0.0f, 1.0f);
        img->data[i] = t->invert ? 1.0f - x : x;
    }
    *out = img;
    return PC_OK;
}

static pc_status tip_mips(const pc_paint_tip *t, pc_paint_mips **out, pc_error *err)
{
    *out = NULL;
    if (t->kind == PC_PAINT_TIP_SAMPLED && pc_paint_gray_tile_is_valid(&t->tile)) {
        return pc_paint_mips_new(&t->tile, out, err);
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
    return pc_paint_rect_around(d, sampled ? d->radius * SQRT_2_F : d->radius);
}

pc_status pc_paint_context_rasterize(const pc_paint_context *ctx, const pc_paint_dab *d, bool dual,
                                     pc_rect rect, pc_paint_fbuf *out, pc_error *err)
{
    const pc_paint_brush *b = &ctx->brush;
    size_t w = pc_rect_width(rect), h = pc_rect_height(rect), area;
    float hardness = dual ? b->dual_brush.hardness : b->hardness;
    const pc_paint_mips *mips = dual ? ctx->dual_tip : ctx->tip;
    bool aliased = b->aliased && !dual, wet = b->wet_edges && !dual, noise = b->noise && !dual;
    bool tex_tip = !dual && b->texture.enabled && b->texture.each_tip && ctx->texture != NULL;
    float cx, cy, sn, cs, fxs, fys, r, ro, rm, reach2, inv_scale = 1.0f, tw = 1.0f, th = 1.0f;
    bool proj = !dual && d->proj_scale < 0.999f;
    float pax = 0.0f, pay = 0.0f, pk = 0.0f;
    size_t level = 0;
    pc_status st;
    if (!pc_size_mul(w, h, &area) || area > PC_PAINT_MAX_DAB_PIXELS) {
        return pc_fail(err, PC_ERR_LIMIT, "dab too large (%zux%zu pixels)", w, h);
    }
    st = pc_paint_fbuf_zeroed(out, area, err);
    if (st != PC_OK || area == 0) {
        return st;
    }
    if (aliased) {
        pc_paint_grid_center(d->center.x, d->center.y, 2.0f * d->radius, &cx, &cy);
    } else {
        cx = pc_f64_to_f32(d->center.x);
        cy = pc_f64_to_f32(d->center.y);
    }
    sn = sinf(d->angle);
    cs = cosf(d->angle);
    fxs = d->flip_x ? -1.0f : 1.0f;
    fys = d->flip_y ? -1.0f : 1.0f;
    /* Brush Projection: stretch the sampling coordinates along the tilt direction, which
     * foreshortens the tip there by proj_scale. */
    if (proj) {
        pay = sinf(d->proj_angle);
        pax = cosf(d->proj_angle);
        pk = 1.0f / pc_paint_maxf_k(d->proj_scale, 0.05f) - 1.0f;
    }
    r = d->radius;
    ro = pc_paint_minf_k(pc_rs_maxf(d->roundness, 0.5f / r), 1.0f);
    rm = pc_paint_maxf_k(r * ro, 0.5f);
    reach2 = (rm + 0.5f) * (rm + 0.5f);
    if (mips != NULL) {
        size_t w0 = mips->levels[0].width, h0 = mips->levels[0].height;
        float big = (float)(w0 > h0 ? w0 : h0);
        level = pc_paint_mips_level_for(mips, 2.0f * r * pc_paint_maxf_k(ro, 0.25f));
        inv_scale = big / (2.0f * r);
        tw = (float)w0;
        th = (float)h0;
    }
    for (size_t yy = 0; yy < h; yy++) {
        int32_t y = (int32_t)((int64_t)rect.y0 + (int64_t)yy);
        float dy = (float)y + 0.5f - cy;
        for (size_t xx = 0; xx < w; xx++) {
            int32_t x = (int32_t)((int64_t)rect.x0 + (int64_t)xx);
            float dx = (float)x + 0.5f - cx;
            /* To y-up, project, rotate by -angle, flip. */
            float ux = dx, uy = -dy, u, v, val, rn;
            if (proj) {
                float t = (ux * pax + uy * pay) * pk;
                ux += t * pax;
                uy += t * pay;
            }
            u = (ux * cs + uy * sn) * fxs;
            v = (-ux * sn + uy * cs) * fys;
            if (mips == NULL) {
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
                val = pc_paint_mask_combine(b->texture.mode, val, pc_paint_context_texture_at(ctx, x, y),
                                            d->depth);
            }
            if (wet) {
                val *= 0.5f + 0.5f * smoothstep(0.5f, 1.0f, rn);
            }
            out->v[yy * w + xx] = val * d->alpha;
        }
    }
    return PC_OK;
}

float pc_paint_context_stroke_mask(const pc_paint_context *ctx, float c, bool has_dual, float dual, int32_t x,
                                   int32_t y)
{
    const pc_paint_brush *b = &ctx->brush;
    float m = c;
    if (has_dual) {
        m = pc_paint_mask_combine(b->dual_brush.mode, m, dual, 1.0f);
    }
    if (ctx->texture != NULL && !b->texture.each_tip) {
        m = pc_paint_mask_combine(b->texture.mode, m, pc_paint_context_texture_at(ctx, x, y),
                                  pc_clampf(b->texture.depth, 0.0f, 1.0f));
    }
    return m;
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
