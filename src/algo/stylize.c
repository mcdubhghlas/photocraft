/* Mosaic, Emboss, Find Edges, Solarize, Invert and Desaturate (port of
 * crates/algo/src/stylize.rs). */
#include <stdlib.h>
#include <string.h>

#include "algo/fx.h"
#include "algo/fx_defs.h"
#include "base/mem.h"

/* stylize.rs luma: Rec. 601 of the sRGB conversion (in every model). */
static float luma(const pc_algo_ctx *ctx, const float *px, size_t n)
{
    float c[4];
    pc_algo_rgba(ctx, px, n, c);
    return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
}

/* Replaces the colour with gray g in the pixel's own model (alpha kept). */
static void set_gray(const pc_algo_ctx *ctx, float *px, size_t n, float g)
{
    float a = ctx->alpha && n > 0 ? px[n - 1] : 1.0f;
    const float c[4] = {g, g, g, a};
    pc_algo_set_rgba(ctx, px, n, c);
}

static pc_status invert_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                          const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    size_t n = src->ch, cc = pc_algo_ncol(ctx, n), i, c, npx = pc_fx_npx(out);
    (void)p, (void)prep, (void)err;
    pc_algo_image_crop_into(src, out, dst);
    for (i = 0; i < npx; i++) {
        for (c = 0; c < cc; c++) {
            dst[i * n + c] = 1.0f - dst[i * n + c];
        }
    }
    return PC_OK;
}

static pc_status solarize_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                            const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    size_t n = src->ch, cc = pc_algo_ncol(ctx, n), i, c, npx = pc_fx_npx(out);
    (void)p, (void)prep, (void)err;
    pc_algo_image_crop_into(src, out, dst);
    for (i = 0; i < npx; i++) {
        for (c = 0; c < cc; c++) {
            float v = dst[i * n + c];
            dst[i * n + c] = v > 0.5f ? 1.0f - v : v;
        }
    }
    return PC_OK;
}

static pc_status desaturate_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                              const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    size_t n = src->ch, i, npx = pc_fx_npx(out);
    (void)p, (void)prep, (void)err;
    pc_algo_image_crop_into(src, out, dst);
    if (n == 0) {
        return PC_OK;
    }
    for (i = 0; i < npx; i++) {
        float *px = dst + i * n;
        set_gray(ctx, px, n, luma(ctx, px, n));
    }
    return PC_OK;
}

/* Mosaic cells anchored at the bounds' origin; each cell becomes its (premultiplied) average over
 * the part inside the bounds. Each cell is averaged once per tile. */
static pc_status mosaic_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                          const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    size_t n = src->ch, width = pc_rect_width(out), height = pc_rect_height(out), c;
    int32_t cs = pc_f32_to_i32(roundf(pc_rs_maxf_k(((const pc_algo_mosaic_params *)p)->cell_size, 1.0f)));
    pc_rect b = ctx->bounds;
    int32_t cx0, cx1, cy0, cy1;
    int64_t cx, cy;
    float acc[PC_ALGO_MAXC];
    (void)prep, (void)err;
    if (width == 0 || height == 0 || n == 0 || n > PC_ALGO_MAXC) {
        return PC_OK;
    }
    cx0 = pc_fx_div_euclid(pc_i32_wrapping_sub(out.x0, b.x0), cs);
    cx1 = pc_fx_div_euclid(pc_i32_wrapping_sub(pc_i32_wrapping_sub(out.x1, 1), b.x0), cs);
    cy0 = pc_fx_div_euclid(pc_i32_wrapping_sub(out.y0, b.y0), cs);
    cy1 = pc_fx_div_euclid(pc_i32_wrapping_sub(pc_i32_wrapping_sub(out.y1, 1), b.y0), cs);
    for (cy = cy0; cy <= cy1; cy++) {
        for (cx = cx0; cx <= cx1; cx++) {
            int32_t ix = (int32_t)cx, iy = (int32_t)cy;
            pc_rect cell =
                pc_rect_make(pc_i32_wrapping_add(b.x0, pc_i32_wrapping_mul(ix, cs)),
                             pc_i32_wrapping_add(b.y0, pc_i32_wrapping_mul(iy, cs)),
                             pc_i32_wrapping_add(b.x0, pc_i32_wrapping_mul(pc_i32_wrapping_add(ix, 1), cs)),
                             pc_i32_wrapping_add(b.y0, pc_i32_wrapping_mul(pc_i32_wrapping_add(iy, 1), cs)));
            pc_rect r = pc_rect_intersect(cell, b), wr;
            float count = 0.0f;
            int32_t xx, yy;
            size_t row, col;
            memset(acc, 0, sizeof acc);
            for (yy = r.y0; yy < r.y1; yy++) {
                for (xx = r.x0; xx < r.x1; xx++) {
                    float a = ctx->alpha ? pc_algo_image_get(src, xx, yy, n - 1) : 1.0f;
                    for (c = 0; c < n; c++) {
                        float s = pc_algo_image_get(src, xx, yy, c);
                        acc[c] += ctx->alpha && c < n - 1 ? s * a : s;
                    }
                    count += 1.0f;
                }
            }
            if (count > 0.0f) {
                for (c = 0; c < n; c++) {
                    acc[c] /= count;
                }
            }
            if (ctx->alpha) {
                float a = acc[n - 1];
                for (c = 0; c + 1 < n; c++) {
                    acc[c] = a > 1e-7f ? acc[c] / a : 0.0f;
                }
            }
            /* The full cell clips the output, so partial cells beyond the bounds are kept. */
            wr = pc_rect_intersect(cell, out);
            if (pc_rect_is_empty(wr)) {
                continue;
            }
            for (row = (size_t)(wr.y0 - out.y0); row < (size_t)(wr.y1 - out.y0); row++) {
                for (col = (size_t)(wr.x0 - out.x0); col < (size_t)(wr.x1 - out.x0); col++) {
                    memcpy(dst + (row * width + col) * n, acc, n * sizeof(float));
                }
            }
        }
    }
    return PC_OK;
}

/* Emboss: gray relief from luminance differences along the light angle. */
static pc_status emboss_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                          const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_emboss_params *e = p;
    size_t n = src->ch, i, c, npx = pc_fx_npx(out);
    float a = pc_fx_rad(e->angle), s = sinf(a), co = cosf(a), h = pc_rs_maxf_k(e->height, 1.0f);
    int32_t dx = pc_f32_to_i32(roundf(co * h)), dy = pc_f32_to_i32(roundf(-s * h));
    float k = e->amount / 100.0f, t1[PC_ALGO_MAXC], t2[PC_ALGO_MAXC];
    (void)prep, (void)err;
    pc_algo_image_crop_into(src, out, dst);
    if (n == 0 || n > PC_ALGO_MAXC) {
        return PC_OK;
    }
    for (i = 0; i < npx; i++) {
        int32_t x, y;
        float g;
        pc_algo_xy(out, i, &x, &y);
        for (c = 0; c < n; c++) {
            t1[c] = pc_algo_image_get(src, pc_i32_wrapping_add(x, dx), pc_i32_wrapping_add(y, dy), c);
            t2[c] = pc_algo_image_get(src, pc_i32_wrapping_sub(x, dx), pc_i32_wrapping_sub(y, dy), c);
        }
        g = 0.5f + k * (luma(ctx, t1, n) - luma(ctx, t2, n));
        set_gray(ctx, dst + i * n, n, pc_clampf(g, 0.0f, 1.0f));
    }
    return PC_OK;
}

/* Find Edges: Sobel magnitude per colour channel, dark edges on white. */
static pc_status find_edges_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                              const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    size_t n = src->ch, cc = pc_algo_ncol(ctx, n), i, c, npx = pc_fx_npx(out);
    (void)p, (void)prep, (void)err;
    pc_algo_image_crop_into(src, out, dst);
    for (i = 0; i < npx; i++) {
        int32_t x, y;
        pc_algo_xy(out, i, &x, &y);
        for (c = 0; c < cc; c++) {
#define G(ddx, ddy) pc_algo_image_get(src, x + (ddx), y + (ddy), c)
            float gx = (G(1, -1) + 2.0f * G(1, 0) + G(1, 1)) - (G(-1, -1) + 2.0f * G(-1, 0) + G(-1, 1));
            float gy = (G(-1, 1) + 2.0f * G(0, 1) + G(1, 1)) - (G(-1, -1) + 2.0f * G(0, -1) + G(1, -1));
#undef G
            dst[i * n + c] = pc_clampf(1.0f - sqrtf(gx * gx + gy * gy) / 4.0f, 0.0f, 1.0f);
        }
    }
    return PC_OK;
}

static pc_algo_halo mosaic_halo(const void *p, const pc_rect *b)
{
    (void)b;
    return pc_algo_halo_reach(pc_rs_maxf_k(((const pc_algo_mosaic_params *)p)->cell_size, 1.0f));
}

static pc_algo_halo emboss_halo(const void *p, const pc_rect *b)
{
    (void)b;
    return pc_algo_halo_radius(
        pc_i32_wrapping_add(pc_f32_to_i32(ceilf(fabsf(((const pc_algo_emboss_params *)p)->height))), 2));
}

static pc_algo_halo one_halo(const void *p, const pc_rect *b)
{
    (void)p, (void)b;
    return pc_algo_halo_radius(1);
}

static const pc_algo_jfield MOSAIC_F[] = {
    PC_ALGO_JF("cellSize", pc_algo_mosaic_params, cell_size, PC_ALGO_JF_F32)};
static const pc_algo_jfield EMBOSS_F[] = {
    PC_ALGO_JF("angle", pc_algo_emboss_params, angle, PC_ALGO_JF_F32),
    PC_ALGO_JF("height", pc_algo_emboss_params, height, PC_ALGO_JF_F32),
    PC_ALGO_JF("amount", pc_algo_emboss_params, amount, PC_ALGO_JF_F32),
};

PC_FX_DEF(pc_algo_def_mosaic, "mosaic", pc_algo_mosaic_params, MOSAIC_F, NULL, mosaic_halo, mosaic_k);
PC_FX_DEF(pc_algo_def_emboss, "emboss", pc_algo_emboss_params, EMBOSS_F, NULL, emboss_halo, emboss_k);
PC_FX_UNIT(pc_algo_def_find_edges, "findEdges", one_halo, find_edges_k);
PC_FX_UNIT(pc_algo_def_solarize, "solarize", NULL, solarize_k);
PC_FX_UNIT(pc_algo_def_invert, "invert", NULL, invert_k);
PC_FX_UNIT(pc_algo_def_desaturate, "desaturate", NULL, desaturate_k);
