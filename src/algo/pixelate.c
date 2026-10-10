/* Pixelate: Color Halftone, Crystallize, Facet, Fragment, Mezzotint, Pointillize (port of
 * crates/algo/src/pixelate.rs).
 *
 * Cell layouts are anchored at the reference bounds' origin and random choices hash document
 * coordinates, so output is independent of tiling. */
#include <float.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fx.h"
#include "algo/fx_defs.h"
#include "base/mem.h"

/* ---- Color Halftone ---- */

typedef struct halftone_ctx {
    const pc_algo_image *src;
    const pc_algo_ctx *ctx;
    pc_rect clip;
    float cell;
    size_t n;
    bool rgb;
} halftone_ctx;

/* Channel value at a point averaged over five taps (for Lab, of the RGB conversion). */
static float halftone_value(const halftone_ctx *h, float x, float y, size_t c)
{
    float d = h->cell * 0.25f, acc = 0.0f, px[PC_ALGO_MAXC];
    const float taps[5][2] = {{0.0f, 0.0f}, {-d, -d}, {d, -d}, {-d, d}, {d, d}};
    size_t t, k;
    for (t = 0; t < 5; t++) {
        /* Clamp to the bounds so cells straddling the edge read real pixels. */
        int32_t ix = pc_clamp_i32(pc_f32_to_i32(floorf(x + taps[t][0])), h->clip.x0, h->clip.x1 - 1);
        int32_t iy = pc_clamp_i32(pc_f32_to_i32(floorf(y + taps[t][1])), h->clip.y0, h->clip.y1 - 1);
        if (h->rgb) {
            float rgba[4];
            for (k = 0; k < h->n && k < PC_ALGO_MAXC; k++) {
                px[k] = pc_algo_image_get(h->src, ix, iy, k);
            }
            pc_algo_rgba(h->ctx, px, h->n, rgba);
            acc += rgba[c < 4 ? c : 3];
        } else {
            acc += pc_algo_image_get(h->src, ix, iy, c);
        }
    }
    return acc / 5.0f;
}

/* Each channel is screened at its own angle into dots whose area follows the channel's ink
 * (1 - value in additive models). */
static pc_status halftone_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                            const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_halftone_params *hp = p;
    size_t n = src->ch, cc = pc_algo_ncol(ctx, n), nrot = cc > 3 ? cc : 3, chans, i, c;
    float rmax = pc_rs_maxf_k(hp->max_radius, 1.0f), rs[PC_ALGO_MAXC], rc[PC_ALGO_MAXC], vals[PC_ALGO_MAXC];
    bool sub = pc_algo_subtractive(ctx);
    pc_rect b = ctx->bounds;
    float ox = (float)b.x0, oy = (float)b.y0;
    halftone_ctx h;
    (void)prep, (void)err;
    pc_algo_image_crop_into(src, out, dst);
    if (n == 0 || n > PC_ALGO_MAXC) {
        return PC_OK;
    }
    h.src = src;
    h.ctx = ctx;
    h.n = n;
    h.rgb = pc_algo_via_rgb(ctx);
    /* Cell side such that a full-size dot just covers the cell's corners. */
    h.cell = rmax * PC_FX_SQRT_2;
    for (c = 0; c < nrot && c < PC_ALGO_MAXC; c++) {
        float a = pc_fx_rad(hp->angles[c < 3 ? c : 3]);
        rs[c] = sinf(a);
        rc[c] = cosf(a);
    }
    h.clip = pc_rect_intersect(b, src->rect);
    if (pc_rect_is_empty(h.clip)) {
        return PC_OK;
    }
    chans = h.rgb ? 3 : cc;
    for (i = 0; i < pc_fx_npx(out); i++) {
        float *px = dst + i * n;
        int32_t x, y;
        float fx, fy;
        pc_algo_xy(out, i, &x, &y);
        fx = (float)x + 0.5f - ox;
        fy = (float)y + 0.5f - oy;
        for (c = 0; c < chans && c < PC_ALGO_MAXC; c++) {
            float s = rs[c], co = rc[c], cell = h.cell;
            /* Rotate into screen space. */
            float u = fx * co + fy * s, w2 = -fx * s + fy * co;
            float cu = floorf(u / cell), cv = floorf(w2 / cell), cov = 0.0f;
            int dj, di;
            for (dj = -1; dj <= 1; dj++) {
                for (di = -1; di <= 1; di++) {
                    float mu = (cu + (float)di + 0.5f) * cell, mv = (cv + (float)dj + 0.5f) * cell;
                    float d = sqrtf(pc_fx_sq(u - mu) + pc_fx_sq(w2 - mv)), raw, ink, r;
                    const float touch = PC_FX_FRAC_PI_4;
                    if (d >= rmax + 0.5f) {
                        continue;
                    }
                    /* Back to document space to read the cell's value. */
                    raw = pc_clampf(halftone_value(&h, mu * co - mv * s + ox, mu * s + mv * co + oy, c), 0.0f,
                                    1.0f);
                    ink = sub && !h.rgb ? raw : 1.0f - raw;
                    /* Dot area tracks ink: pi r^2 = ink cell^2 until dots touch, then grow to cover the
                     * corners. */
                    if (ink <= touch) {
                        r = cell * sqrtf(ink / PC_FX_PI);
                    } else {
                        r = cell * (0.5f + (ink - touch) / (1.0f - touch) * (PC_FX_FRAC_1_SQRT_2 - 0.5f));
                    }
                    /* Tiny dots cannot cover more than their own area. */
                    cov = pc_rs_maxf(cov, pc_rs_minf(pc_clampf(r - d + 0.5f, 0.0f, 1.0f), PC_FX_PI * r * r));
                }
            }
            vals[c] = sub && !h.rgb ? cov : 1.0f - cov;
        }
        if (h.rgb) {
            float a = ctx->alpha ? px[n - 1] : 1.0f;
            const float col[4] = {vals[0], vals[1], vals[2], a};
            pc_algo_set_rgba(ctx, px, n, col);
        } else {
            memcpy(px, vals, cc * sizeof(float));
        }
    }
    return PC_OK;
}

/* ---- Crystallize ---- */

typedef struct cpoint {
    int32_t x, y;
} cpoint;

/* The cell whose jittered point is nearest to the pixel centre. */
static cpoint crystal_owner(int32_t x, int32_t y, float cell, uint32_t seed, float ox, float oy)
{
    float fx = (float)x + 0.5f, fy = (float)y + 0.5f, best = FLT_MAX;
    int32_t cx = pc_f32_to_i32(floorf((fx - ox) / cell)), cy = pc_f32_to_i32(floorf((fy - oy) / cell));
    cpoint o = {cx, cy};
    int dx, dy;
    for (dy = -1; dy <= 1; dy++) {
        for (dx = -1; dx <= 1; dx++) {
            int32_t gx = pc_i32_wrapping_add(cx, dx), gy = pc_i32_wrapping_add(cy, dy);
            float px, py, d;
            pc_algo_cell_point(gx, gy, cell, seed, ox, oy, &px, &py);
            d = pc_fx_sq(px - fx) + pc_fx_sq(py - fy);
            if (d < best) {
                best = d;
                o.x = gx;
                o.y = gy;
            }
        }
    }
    return o;
}

/* Voronoi cells around jittered points, each filled with the (premultiplied) average colour of
 * its pixels. */
static pc_status crystallize_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                               const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_crystallize_params *cp = p;
    size_t n = src->ch, ow = pc_rect_width(out), npx = pc_fx_npx(out), gw, gh, nacc, i, c;
    float cell = pc_rs_maxf_k(cp->cell_size, 2.0f);
    pc_rect b = ctx->bounds, win;
    float ox = (float)b.x0, oy = (float)b.y0;
    int32_t c0x, c0y, c1x, c1y;
    float *acc;
    cpoint *owners;
    int32_t x, y;
    (void)prep;
    pc_algo_image_crop_into(src, out, dst);
    if (npx == 0 || n == 0) {
        return PC_OK;
    }
    /* Cells whose territory can reach `out`, and the window holding all their pixels. */
    c0x = pc_i32_wrapping_sub(pc_f32_to_i32(floorf((float)pc_i32_wrapping_sub(out.x0, b.x0) / cell)), 2);
    c0y = pc_i32_wrapping_sub(pc_f32_to_i32(floorf((float)pc_i32_wrapping_sub(out.y0, b.y0) / cell)), 2);
    c1x = pc_i32_wrapping_add(pc_f32_to_i32(floorf((float)pc_i32_wrapping_sub(out.x1, b.x0) / cell)), 2);
    c1y = pc_i32_wrapping_add(pc_f32_to_i32(floorf((float)pc_i32_wrapping_sub(out.y1, b.y0) / cell)), 2);
    if (c1x < c0x || c1y < c0y) {
        return pc_fail(err, PC_ERR_LIMIT, "Crystallize cell grid overflows");
    }
    gw = (size_t)((int64_t)c1x - c0x + 1);
    gh = (size_t)((int64_t)c1y - c0y + 1);
    if (!pc_size_mul3(gw, gh, n + 1, &nacc) || nacc > PC_MAX_ALLOC / sizeof(float)) {
        return pc_fail(err, PC_ERR_LIMIT, "Crystallize cell grid is too large");
    }
    acc = pc_calloc_array(nacc, sizeof(float));
    owners = pc_calloc_array(npx, sizeof *owners);
    if (acc == NULL || owners == NULL) {
        free(acc), free(owners);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for Crystallize");
    }
    win = pc_rect_make(
        pc_i32_wrapping_add(b.x0, pc_f32_to_i32(floorf((float)c0x * cell))),
        pc_i32_wrapping_add(b.y0, pc_f32_to_i32(floorf((float)c0y * cell))),
        pc_i32_wrapping_add(b.x0, pc_f32_to_i32(ceilf((float)pc_i32_wrapping_add(c1x, 1) * cell))),
        pc_i32_wrapping_add(b.y0, pc_f32_to_i32(ceilf((float)pc_i32_wrapping_add(c1y, 1) * cell))));
    win = pc_rect_intersect(pc_rect_intersect(win, src->rect), b);
    for (y = win.y0; y < win.y1; y++) {
        for (x = win.x0; x < win.x1; x++) {
            cpoint o = crystal_owner(x, y, cell, cp->seed, ox, oy);
            size_t k;
            float a;
            if (pc_rect_contains(out, x, y)) {
                owners[(size_t)(y - out.y0) * ow + (size_t)(x - out.x0)] = o;
            }
            if (o.x < c0x || o.y < c0y || o.x > c1x || o.y > c1y) {
                continue;
            }
            k = ((size_t)(o.y - c0y) * gw + (size_t)(o.x - c0x)) * (n + 1);
            a = ctx->alpha ? pc_algo_image_get(src, x, y, n - 1) : 1.0f;
            for (c = 0; c < n; c++) {
                float s = pc_algo_image_get(src, x, y, c);
                acc[k + c] += ctx->alpha && c < n - 1 ? s * a : s;
            }
            acc[k + n] += 1.0f;
        }
    }
    /* Pixels of `out` outside the bounds keep their own value; resolve owners for any missed. */
    for (i = 0; i < npx; i++) {
        float *px = dst + i * n, cnt;
        cpoint o;
        size_t k;
        pc_algo_xy(out, i, &x, &y);
        if (!pc_rect_contains(b, x, y)) {
            continue;
        }
        o = pc_rect_contains(win, x, y) ? owners[i] : crystal_owner(x, y, cell, cp->seed, ox, oy);
        if (o.x < c0x || o.y < c0y || o.x > c1x || o.y > c1y) {
            continue;
        }
        k = ((size_t)(o.y - c0y) * gw + (size_t)(o.x - c0x)) * (n + 1);
        cnt = acc[k + n];
        if (cnt <= 0.0f) {
            continue;
        }
        for (c = 0; c < n; c++) {
            px[c] = acc[k + c] / cnt;
        }
        pc_algo_unpremul_px(px, n, ctx->alpha);
    }
    free(acc);
    free(owners);
    return PC_OK;
}

/* ---- Facet ---- */

/* A Kuwahara filter (radius 2): each pixel takes the mean of the least-varied of its four
 * overlapping quadrants, clumping similar colours into flat facets while keeping edges. */
static pc_status facet_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                         const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    static const int32_t quads[4][2] = {{-2, -2}, {0, -2}, {-2, 0}, {0, 0}};
    const int32_t r = 2;
    size_t n = src->ch, i, c, q;
    (void)p, (void)prep, (void)err;
    pc_algo_image_crop_into(src, out, dst);
    if (n == 0 || n > PC_ALGO_MAXC) {
        return PC_OK;
    }
    for (i = 0; i < pc_fx_npx(out); i++) {
        float best = FLT_MAX, bestv[PC_ALGO_MAXC] = {0}, tmp[PC_ALGO_MAXC] = {0};
        int32_t x, y;
        pc_algo_xy(out, i, &x, &y);
        for (q = 0; q < 4; q++) {
            float sum[PC_ALGO_MAXC] = {0}, s = 0.0f, s2 = 0.0f, var;
            const float cnt = (float)((r + 1) * (r + 1));
            int32_t xx, yy;
            for (yy = y + quads[q][1]; yy <= y + quads[q][1] + r; yy++) {
                for (xx = x + quads[q][0]; xx <= x + quads[q][0] + r; xx++) {
                    float l;
                    for (c = 0; c < n; c++) {
                        tmp[c] = pc_algo_image_get(src, xx, yy, c);
                        sum[c] += tmp[c];
                    }
                    l = pc_algo_luma(ctx, tmp, n);
                    s += l;
                    s2 += l * l;
                }
            }
            var = s2 / cnt - pc_fx_sq(s / cnt);
            if (var < best) {
                for (c = 0; c < PC_ALGO_MAXC; c++) {
                    bestv[c] = sum[c] / cnt;
                }
                best = var;
            }
        }
        memcpy(dst + i * n, bestv, n * sizeof(float));
    }
    return PC_OK;
}

/* ---- Fragment ---- */

/* The average of four copies offset by 4 px (left, right, up, down). */
static pc_status fragment_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                            const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    static const int32_t offs[4][2] = {{-4, 0}, {4, 0}, {0, -4}, {0, 4}};
    size_t n = src->ch, k = 0, c, j;
    int32_t x, y;
    (void)p, (void)prep, (void)err;
    if (n == 0 || n > PC_ALGO_MAXC) {
        return PC_OK;
    }
    for (y = out.y0; y < out.y1; y++) {
        for (x = out.x0; x < out.x1; x++) {
            float acc[PC_ALGO_MAXC] = {0};
            for (j = 0; j < 4; j++) {
                int32_t sx = x + offs[j][0], sy = y + offs[j][1];
                float a = ctx->alpha ? pc_algo_image_get(src, sx, sy, n - 1) : 1.0f;
                for (c = 0; c < n; c++) {
                    float s = pc_algo_image_get(src, sx, sy, c);
                    acc[c] += (ctx->alpha && c < n - 1 ? s * a : s) * 0.25f;
                }
            }
            pc_algo_unpremul_px(acc, n, ctx->alpha);
            memcpy(dst + k, acc, n * sizeof(float));
            k += n;
        }
    }
    return PC_OK;
}

/* ---- Mezzotint ---- */

static float mezzo_lines(int32_t len, int32_t x, int32_t y, uint32_t c, uint32_t seed)
{
    int32_t phase = pc_f32_to_i32(pc_algo_hash01(0, y, c + 40, seed) * (float)len);
    return pc_algo_hash01(pc_fx_div_euclid(pc_i32_wrapping_add(x, phase), len), y, c, seed);
}

static float mezzo_strokes(int32_t len, int32_t x, int32_t y, uint32_t c, uint32_t seed)
{
    int32_t d = pc_i32_wrapping_sub(x, y);
    int32_t phase = pc_f32_to_i32(pc_algo_hash01(d, 0, c + 50, seed) * (float)len);
    return pc_algo_hash01(pc_fx_div_euclid(pc_i32_wrapping_add(pc_i32_wrapping_add(x, y), phase), len), d,
                          c + 60, seed);
}

/* Random threshold of pattern `kind` at a pixel. */
static float mezzo_threshold(pc_algo_mezzotint_type kind, int32_t x, int32_t y, uint32_t c, uint32_t seed)
{
    switch (kind) {
    case PC_ALGO_MEZZO_MEDIUM_DOTS:
        return pc_algo_hash01(pc_fx_div_euclid(x, 2), pc_fx_div_euclid(y, 2), c, seed);
    case PC_ALGO_MEZZO_GRAINY_DOTS:
        return 0.5f * pc_algo_hash01(x, y, c, seed) +
               0.5f * pc_algo_hash01(pc_fx_div_euclid(x, 3), pc_fx_div_euclid(y, 3), c + 7, seed);
    case PC_ALGO_MEZZO_COARSE_DOTS:
        return pc_algo_hash01(pc_fx_div_euclid(x, 3), pc_fx_div_euclid(y, 3), c, seed);
    case PC_ALGO_MEZZO_SHORT_LINES:
        return mezzo_lines(6, x, y, c, seed);
    case PC_ALGO_MEZZO_MEDIUM_LINES:
        return mezzo_lines(14, x, y, c, seed);
    case PC_ALGO_MEZZO_LONG_LINES:
        return mezzo_lines(32, x, y, c, seed);
    case PC_ALGO_MEZZO_SHORT_STROKES:
        return mezzo_strokes(5, x, y, c, seed);
    case PC_ALGO_MEZZO_MEDIUM_STROKES:
        return mezzo_strokes(10, x, y, c, seed);
    case PC_ALGO_MEZZO_LONG_STROKES:
        return mezzo_strokes(20, x, y, c, seed);
    default:
        return pc_algo_hash01(x, y, c, seed);
    }
}

/* Every colour channel is thresholded against a random pattern, giving pure channel values. */
static pc_status mezzotint_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                             const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_mezzotint_params *m = p;
    size_t n = src->ch, cc = pc_algo_ncol(ctx, n), i, k;
    bool rgb = pc_algo_via_rgb(ctx);
    (void)prep, (void)err;
    pc_algo_image_crop_into(src, out, dst);
    for (i = 0; i < pc_fx_npx(out); i++) {
        float *px = dst + i * n;
        int32_t x, y;
        pc_algo_xy(out, i, &x, &y);
        if (rgb) {
            float c[4];
            pc_algo_rgba(ctx, px, n, c);
            for (k = 0; k < 3; k++) {
                c[k] = c[k] > mezzo_threshold(m->kind, x, y, (uint32_t)k, m->seed) ? 1.0f : 0.0f;
            }
            pc_algo_set_rgba(ctx, px, n, c);
        } else {
            for (k = 0; k < cc; k++) {
                px[k] = px[k] > mezzo_threshold(m->kind, x, y, (uint32_t)k, m->seed) ? 1.0f : 0.0f;
            }
        }
    }
    return PC_OK;
}

/* ---- Pointillize ---- */

/* Randomly sized dots, coloured from the image at their centre, on a canvas of the background
 * colour. */
static pc_status pointillize_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                               const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_pointillize_params *pp = p;
    size_t n = src->ch, cc = pc_algo_ncol(ctx, n), i, c;
    float cell = pc_rs_maxf_k(pp->cell_size, 2.0f), bg[PC_ALGO_MAXC];
    pc_rect b = ctx->bounds;
    float ox = (float)b.x0, oy = (float)b.y0;
    bool sub = pc_algo_subtractive(ctx);
    (void)prep, (void)err;
    pc_algo_image_crop_into(src, out, dst);
    if (n == 0 || n > PC_ALGO_MAXC) {
        return PC_OK;
    }
    pc_algo_native(ctx, pp->background, bg);
    for (i = 0; i < pc_fx_npx(out); i++) {
        float *px = dst + i * n, fx, fy, canvas[PC_ALGO_MAXC] = {0}, a_orig;
        float bdepth = 0.0f, bpx = 0.0f, bpy = 0.0f;
        int32_t x, y, cx, cy, bgx = 0, bgy = 0;
        bool found = false;
        int dx, dy;
        pc_algo_xy(out, i, &x, &y);
        fx = (float)x + 0.5f;
        fy = (float)y + 0.5f;
        cx = pc_f32_to_i32(floorf((fx - ox) / cell));
        cy = pc_f32_to_i32(floorf((fy - oy) / cell));
        /* The dot whose edge the pixel is deepest inside wins. */
        for (dy = -1; dy <= 1; dy++) {
            for (dx = -1; dx <= 1; dx++) {
                int32_t gx = pc_i32_wrapping_add(cx, dx), gy = pc_i32_wrapping_add(cy, dy);
                float px0, py0, rad, d, depth;
                pc_algo_cell_point(gx, gy, cell, pp->seed, ox, oy, &px0, &py0);
                rad = cell * (0.45f + 0.35f * pc_algo_hash01(gx, gy, 13, pp->seed));
                d = sqrtf(pc_fx_sq(px0 - fx) + pc_fx_sq(py0 - fy));
                depth = rad - d;
                if (depth > -0.5f && (!found || depth > bdepth)) {
                    found = true;
                    bdepth = depth, bgx = gx, bgy = gy, bpx = px0, bpy = py0;
                }
            }
        }
        a_orig = ctx->alpha ? px[n - 1] : 1.0f;
        /* Canvas first, then the dot over it with an anti-aliased edge. */
        memcpy(canvas, bg, n * sizeof(float));
        if (ctx->alpha) {
            canvas[n - 1] = pc_rs_maxf(a_orig, pc_rs_minf_k(bg[n - 1], 1.0f));
        }
        if (found) {
            int32_t sx = pc_f32_to_i32(floorf(bpx)), sy = pc_f32_to_i32(floorf(bpy));
            float dot[PC_ALGO_MAXC], j, k;
            for (c = 0; c < n; c++) {
                dot[c] = pc_algo_image_get(src, sx, sy, c);
            }
            /* Slight per-dot brightness jitter, as in pointillist strokes. */
            j = (pc_algo_hash01(bgx, bgy, 17, pp->seed) - 0.5f) * 0.08f;
            for (c = 0; c < cc; c++) {
                dot[c] = pc_clampf(dot[c] + (sub ? -j : j), 0.0f, 1.0f);
            }
            k = pc_clampf(bdepth + 0.5f, 0.0f, 1.0f);
            for (c = 0; c < n; c++) {
                canvas[c] += (dot[c] - canvas[c]) * k;
            }
        }
        memcpy(px, canvas, n * sizeof(float));
    }
    return PC_OK;
}

/* ---- halos and defs ---- */

static pc_algo_halo halftone_halo(const void *p, const pc_rect *b)
{
    (void)b;
    return pc_algo_halo_reach(((const pc_algo_halftone_params *)p)->max_radius * 3.0f);
}

static pc_algo_halo crystallize_halo(const void *p, const pc_rect *b)
{
    (void)b;
    return pc_algo_halo_reach(pc_rs_maxf_k(((const pc_algo_crystallize_params *)p)->cell_size, 1.0f) * 3.0f);
}

static pc_algo_halo pointillize_halo(const void *p, const pc_rect *b)
{
    (void)b;
    return pc_algo_halo_reach(pc_rs_maxf_k(((const pc_algo_pointillize_params *)p)->cell_size, 1.0f) * 2.0f);
}

static pc_algo_halo facet_halo(const void *p, const pc_rect *b)
{
    (void)p, (void)b;
    return pc_algo_halo_reach(3.0f);
}

static pc_algo_halo fragment_halo(const void *p, const pc_rect *b)
{
    (void)p, (void)b;
    return pc_algo_halo_reach(4.0f);
}

static const pc_algo_jfield HALFTONE_F[] = {
    PC_ALGO_JF("maxRadius", pc_algo_halftone_params, max_radius, PC_ALGO_JF_F32),
    PC_ALGO_JFA("angles", pc_algo_halftone_params, angles, 4, false),
};
static const pc_algo_jfield CRYSTALLIZE_F[] = {
    PC_ALGO_JF("cellSize", pc_algo_crystallize_params, cell_size, PC_ALGO_JF_F32),
    PC_ALGO_JF("seed", pc_algo_crystallize_params, seed, PC_ALGO_JF_U32),
};
static const pc_algo_jfield MEZZOTINT_F[] = {
    PC_ALGO_JFE("kind", pc_algo_mezzotint_params, kind, PC_ALGO_ENUM_MEZZOTINT_TYPE, false),
    PC_ALGO_JF("seed", pc_algo_mezzotint_params, seed, PC_ALGO_JF_U32),
};
static const pc_algo_jfield POINTILLIZE_F[] = {
    PC_ALGO_JF("cellSize", pc_algo_pointillize_params, cell_size, PC_ALGO_JF_F32),
    PC_ALGO_JF("seed", pc_algo_pointillize_params, seed, PC_ALGO_JF_U32),
    PC_ALGO_JFA("background", pc_algo_pointillize_params, background, 4, false),
};

PC_FX_DEF(pc_algo_def_color_halftone, "colorHalftone", pc_algo_halftone_params, HALFTONE_F, NULL,
          halftone_halo, halftone_k);
PC_FX_DEF(pc_algo_def_crystallize, "crystallize", pc_algo_crystallize_params, CRYSTALLIZE_F, NULL,
          crystallize_halo, crystallize_k);
PC_FX_UNIT(pc_algo_def_facet, "facet", facet_halo, facet_k);
PC_FX_UNIT(pc_algo_def_fragment, "fragment", fragment_halo, fragment_k);
PC_FX_DEF(pc_algo_def_mezzotint, "mezzotint", pc_algo_mezzotint_params, MEZZOTINT_F, NULL, NULL, mezzotint_k);
PC_FX_DEF(pc_algo_def_pointillize, "pointillize", pc_algo_pointillize_params, POINTILLIZE_F, NULL,
          pointillize_halo, pointillize_k);
