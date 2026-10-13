/* Stylize: Diffuse, Extrude, Tiles, Trace Contour, Wind (port of crates/algo/src/stylize2.rs). */
#include <float.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fx.h"
#include "algo/fx_defs.h"
#include "base/mem.h"

static atomic_bool direct_only;

void pc_algo_fx_set_direct_only(bool on)
{
    atomic_store(&direct_only, on);
}

bool pc_algo_fx_direct_only(void)
{
    return atomic_load(&direct_only);
}

static void read_px(const pc_algo_image *src, int32_t x, int32_t y, float *buf, size_t n)
{
    size_t c;
    for (c = 0; c < n; c++) {
        buf[c] = pc_algo_image_get(src, x, y, c);
    }
}

/* ---- Diffuse ---- */

/* Every pixel is swapped for a random neighbour (within 1 px); Darken/Lighten Only accept the
 * neighbour only when darker/lighter; Anisotropic moves halfway to the most similar neighbour. */
static pc_status diffuse_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                           const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    static const int32_t ring[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
    const pc_algo_diffuse_params *dp = p;
    size_t n = src->ch, i, c;
    float nb[PC_ALGO_MAXC];
    (void)prep, (void)err;
    pc_algo_image_crop_into(src, out, dst);
    if (n == 0 || n > PC_ALGO_MAXC) {
        return PC_OK;
    }
    for (i = 0; i < pc_fx_npx(out); i++) {
        float *px = dst + i * n;
        int32_t x, y;
        pc_algo_xy(out, i, &x, &y);
        if (dp->mode == PC_ALGO_DIFFUSE_ANISOTROPIC) {
            float me = pc_algo_luma(ctx, px, n), best = FLT_MAX;
            int32_t bx = 0, by = 0;
            /* Random start so ties do not bias one direction. */
            size_t start = pc_f32_to_size(pc_algo_hash01(x, y, 3, dp->seed) * 8.0f), k;
            for (k = 0; k < 8; k++) {
                const int32_t *o = ring[(start + k) % 8];
                float d;
                read_px(src, x + o[0], y + o[1], nb, n);
                d = fabsf(pc_algo_luma(ctx, nb, n) - me);
                if (d < best) {
                    best = d, bx = o[0], by = o[1];
                }
            }
            read_px(src, x + bx, y + by, nb, n);
            /* Half-way move: anisotropic diffusion softens rather than shuffles. */
            for (c = 0; c < n; c++) {
                px[c] = 0.5f * (px[c] + nb[c]);
            }
        } else {
            int32_t dx = pc_f32_to_i32(pc_algo_hash01(x, y, 1, dp->seed) * 3.0f) - 1;
            int32_t dy = pc_f32_to_i32(pc_algo_hash01(x, y, 2, dp->seed) * 3.0f) - 1;
            bool take;
            read_px(src, x + dx, y + dy, nb, n);
            if (ctx->alpha && nb[n - 1] <= 0.0f && px[n - 1] > 0.0f && dp->mode != PC_ALGO_DIFFUSE_NORMAL) {
                continue;
            }
            switch (dp->mode) {
            case PC_ALGO_DIFFUSE_DARKEN_ONLY:
                take = pc_algo_luma(ctx, nb, n) < pc_algo_luma(ctx, px, n);
                break;
            case PC_ALGO_DIFFUSE_LIGHTEN_ONLY:
                take = pc_algo_luma(ctx, nb, n) > pc_algo_luma(ctx, px, n);
                break;
            default:
                take = true;
                break;
            }
            if (take) {
                memcpy(px, nb, n * sizeof(float));
            }
        }
    }
    return PC_OK;
}

/* ---- Extrude ---- */

float pc_algo_stylize2_extrude_reach(float depth)
{
    return pc_clampf(depth, 0.0f, 255.0f) * 0.25f;
}

typedef struct cell_info {
    int32_t gx, gy;
    float col[PC_ALGO_MAXC];
    float h;
} cell_info;

/* Recently queried cells (results don't depend on it: cell_info is a pure function). */
#define CACHE 64

typedef struct extrude_ctx {
    const pc_algo_image *src;
    const pc_algo_ctx *ctx;
    const pc_algo_extrude_params *spec;
    pc_rect b;
    int32_t cs;
    size_t n;
    cell_info cache[CACHE];
    size_t ncache, next;
} extrude_ctx;

static pc_rect cell_rect(const extrude_ctx *e, int32_t gx, int32_t gy)
{
    pc_rect b = e->b;
    int32_t cs = e->cs;
    return pc_rect_make(pc_i32_wrapping_add(b.x0, pc_i32_wrapping_mul(gx, cs)),
                        pc_i32_wrapping_add(b.y0, pc_i32_wrapping_mul(gy, cs)),
                        pc_i32_wrapping_add(b.x0, pc_i32_wrapping_mul(pc_i32_wrapping_add(gx, 1), cs)),
                        pc_i32_wrapping_add(b.y0, pc_i32_wrapping_mul(pc_i32_wrapping_add(gy, 1), cs)));
}

static bool incomplete(const extrude_ctx *e, pc_rect r)
{
    return !pc_rect_eq(pc_rect_intersect(r, e->b), r);
}

/* Average colour and height of a cell. */
static void compute_cell(const extrude_ctx *e, int32_t gx, int32_t gy, cell_info *ci)
{
    pc_rect r = pc_rect_intersect(cell_rect(e, gx, gy), e->b);
    size_t n = e->n, c;
    float cnt = 0.0f;
    /* Sparse sampling keeps big cells cheap; the average only colours faces. */
    int64_t step = e->cs / 8 > 1 ? e->cs / 8 : 1, xx, yy;
    memset(ci, 0, sizeof *ci);
    ci->gx = gx;
    ci->gy = gy;
    for (yy = r.y0; yy < r.y1; yy += step) {
        for (xx = r.x0; xx < r.x1; xx += step) {
            float a = e->ctx->alpha ? pc_algo_image_get(e->src, (int32_t)xx, (int32_t)yy, n - 1) : 1.0f;
            for (c = 0; c < n; c++) {
                float s = pc_algo_image_get(e->src, (int32_t)xx, (int32_t)yy, c);
                ci->col[c] += e->ctx->alpha && c < n - 1 ? s * a : s;
            }
            cnt += 1.0f;
        }
    }
    if (cnt > 0.0f) {
        for (c = 0; c < n; c++) {
            ci->col[c] /= cnt;
        }
    }
    pc_algo_unpremul_px(ci->col, n, e->ctx->alpha);
    ci->h = e->spec->level_based ? pc_clampf(pc_algo_luma(e->ctx, ci->col, n), 0.0f, 1.0f)
                                 : pc_algo_hash01(gx, gy, 21, e->spec->seed);
}

static const cell_info *info(extrude_ctx *e, int32_t gx, int32_t gy)
{
    size_t i;
    cell_info *slot;
    for (i = 0; i < e->ncache; i++) {
        if (e->cache[i].gx == gx && e->cache[i].gy == gy) {
            return &e->cache[i];
        }
    }
    slot = &e->cache[e->next];
    e->next = (e->next + 1) % CACHE;
    if (e->ncache < CACHE) {
        e->ncache++;
    }
    compute_cell(e, gx, gy, slot);
    return slot;
}

static void shade_px(const extrude_ctx *e, float *px, const float *col, float k)
{
    size_t n = e->n, cc = pc_algo_ncol(e->ctx, n), c;
    bool sub = pc_algo_subtractive(e->ctx);
    for (c = 0; c < cc; c++) {
        px[c] = pc_clampf(sub ? 1.0f - (1.0f - col[c]) * k : col[c] * k, 0.0f, 1.0f);
    }
    if (e->ctx->alpha) {
        px[n - 1] = col[n - 1];
    }
}

/* Sweep t in [0, 1] from base to front: the t range that puts p inside the square |p - (m + o t)|
 * <= half. */
static bool sweep(float p, float m, float o, float half, float *lo, float *hi)
{
    float a, bb;
    if (fabsf(o) < 1e-6f) {
        *lo = 0.0f;
        *hi = 1.0f;
        return fabsf(p - m) <= half;
    }
    a = (p - m - half) / o;
    bb = (p - m + half) / o;
    *lo = pc_rs_maxf_k(pc_rs_minf(a, bb), 0.0f);
    *hi = pc_rs_minf_k(pc_rs_maxf(a, bb), 1.0f);
    return *lo <= *hi;
}

static pc_status extrude_direct(const pc_algo_extrude_params *spec, const pc_algo_image *src, pc_rect out,
                                const pc_algo_ctx *ctx, float *dst, pc_error *err);

#define EXTRUDE_MAX_PIXELS 4194304u
#define EXTRUDE_MAX_CELLS 262144
/* Beyond this, f32 pixel centres can lose enough precision to widen the sweep bounds. */
#define EXTRUDE_MAX_COORD (1 << 22)

static bool beyond_coord(int32_t v)
{
    return (v < 0 ? -(int64_t)v : (int64_t)v) > EXTRUDE_MAX_COORD;
}

/* extrude::blocks (Blocks only): rasterizes each swept block only where it can hit, in the
 * original cell order (ascending gy, then gx: the tie order of every pixel's candidate search), so
 * the result is the direct search's. dst holds the crop of src; false (dst may be partly written)
 * when this path doesn't apply and the direct one must run; *nomem on allocation failure. */
static bool extrude_blocks(const pc_algo_extrude_params *spec, const pc_algo_image *src, pc_rect out,
                           const pc_algo_ctx *ctx, float *dst, bool *nomem)
{
    pc_rect b = ctx->bounds, area;
    size_t n = src->ch, w = pc_rect_width(out), count, i, cc;
    int32_t cs, k, gx0, gy0, gx1, gy1, cx0, cy0, cx1, cy1, cx, cy;
    float reach, bcx, bcy, half_diag, half, *heights;
    bool sub;
    *nomem = false;
    if (spec->kind != PC_ALGO_EXTRUDE_BLOCKS || n == 0 || n > PC_ALGO_MAXC || !isfinite(spec->size) ||
        !isfinite(spec->depth)) {
        return false;
    }
    if (beyond_coord(b.x0) || beyond_coord(b.y0) || beyond_coord(b.x1) || beyond_coord(b.y1) ||
        beyond_coord(out.x0) || beyond_coord(out.y0) || beyond_coord(out.x1) || beyond_coord(out.y1)) {
        return false;
    }
    count = w * (size_t)pc_rect_height(out);
    if (count > EXTRUDE_MAX_PIXELS) {
        return false;
    }
    area = pc_rect_intersect(out, b);
    if (pc_rect_is_empty(area)) {
        return true;
    }
    cs = pc_f32_to_i32(roundf(pc_clampf(spec->size, 2.0f, 255.0f)));
    reach = pc_algo_stylize2_extrude_reach(spec->depth);
    k = pc_f32_to_i32(ceilf(reach / (float)cs)) + 1;
    gx0 = pc_fx_div_euclid(area.x0 - b.x0, cs);
    gy0 = pc_fx_div_euclid(area.y0 - b.y0, cs);
    gx1 = pc_fx_div_euclid(area.x1 - 1 - b.x0, cs);
    gy1 = pc_fx_div_euclid(area.y1 - 1 - b.y0, cs);
    cx0 = gx0 - k > 0 ? gx0 - k : 0;
    cy0 = gy0 - k > 0 ? gy0 - k : 0;
    cx1 = gx1 + k < ((int32_t)pc_rect_width(b) - 1) / cs ? gx1 + k : ((int32_t)pc_rect_width(b) - 1) / cs;
    cy1 = gy1 + k < ((int32_t)pc_rect_height(b) - 1) / cs ? gy1 + k : ((int32_t)pc_rect_height(b) - 1) / cs;
    if ((int64_t)(cx1 - cx0 + 1) * (int64_t)(cy1 - cy0 + 1) > EXTRUDE_MAX_CELLS) {
        return false;
    }
    heights = pc_malloc_array(count, sizeof *heights);
    if (heights == NULL) {
        *nomem = true;
        return false;
    }
    for (i = 0; i < count; i++) {
        heights[i] = -INFINITY;
    }
    bcx = (float)(b.x0 + b.x1) / 2.0f;
    bcy = (float)(b.y0 + b.y1) / 2.0f;
    half_diag = pc_rs_maxf_k(hypotf((float)pc_rect_width(b), (float)pc_rect_height(b)) / 2.0f, 1.0f);
    half = (float)cs / 2.0f;
    cc = pc_algo_ncol(ctx, n);
    sub = pc_algo_subtractive(ctx);
    for (cy = cy0; cy <= cy1; cy++) {
        for (cx = cx0; cx <= cx1; cx++) {
            pc_rect r =
                pc_rect_make(b.x0 + cx * cs, b.y0 + cy * cs, b.x0 + (cx + 1) * cs, b.y0 + (cy + 1) * cs);
            pc_rect clipped = pc_rect_intersect(r, b), sw;
            float col[PC_ALGO_MAXC] = {0}, cnt = 0.0f, h, mx, my, off, ox, oy;
            int64_t step = cs / 8 > 1 ? cs / 8 : 1, xx, yy;
            int32_t x, y;
            size_t c;
            if (spec->mask_incomplete && !pc_rect_eq(clipped, r)) {
                continue;
            }
            for (yy = clipped.y0; yy < clipped.y1; yy += step) {
                for (xx = clipped.x0; xx < clipped.x1; xx += step) {
                    float a = ctx->alpha ? pc_algo_image_get(src, (int32_t)xx, (int32_t)yy, n - 1) : 1.0f;
                    for (c = 0; c < n; c++) {
                        float sv = pc_algo_image_get(src, (int32_t)xx, (int32_t)yy, c);
                        col[c] += ctx->alpha && c < n - 1 ? sv * a : sv;
                    }
                    cnt += 1.0f;
                }
            }
            if (cnt > 0.0f) {
                for (c = 0; c < n; c++) {
                    col[c] /= cnt;
                }
            }
            pc_algo_unpremul_px(col, n, ctx->alpha);
            h = spec->level_based ? pc_clampf(pc_algo_luma(ctx, col, n), 0.0f, 1.0f)
                                  : pc_algo_hash01(cx, cy, 21, spec->seed);
            if (!isfinite(h)) {
                free(heights);
                return false;
            }
            mx = (float)r.x0 + half;
            my = (float)r.y0 + half;
            off = reach * h / half_diag;
            ox = (mx - bcx) * off;
            oy = (my - bcy) * off;
            /* Two extra pixels conservatively cover rounding in the unchanged interval test. */
            sw = pc_rect_intersect(pc_rect_make(pc_f32_to_i32(floorf(pc_rs_minf(mx, mx + ox) - half)) - 2,
                                                pc_f32_to_i32(floorf(pc_rs_minf(my, my + oy) - half)) - 2,
                                                pc_f32_to_i32(ceilf(pc_rs_maxf(mx, mx + ox) + half)) + 2,
                                                pc_f32_to_i32(ceilf(pc_rs_maxf(my, my + oy) + half)) + 2),
                                   area);
            for (y = sw.y0; y < sw.y1; y++) {
                int32_t gy = pc_fx_div_euclid(y - b.y0, cs);
                float fy = (float)y + 0.5f, iy0, iy1;
                if (abs(cy - gy) > k || !sweep(fy, my, oy, half, &iy0, &iy1)) {
                    continue;
                }
                for (x = sw.x0; x < sw.x1; x++) {
                    size_t idx = (size_t)(y - out.y0) * w + (size_t)(x - out.x0);
                    float fx = (float)x + 0.5f, ix0, ix1, lo, hi, *px;
                    int face;
                    if (abs(cx - pc_fx_div_euclid(x - b.x0, cs)) > k || heights[idx] >= h) {
                        continue;
                    }
                    if (!sweep(fx, mx, ox, half, &ix0, &ix1)) {
                        continue;
                    }
                    lo = pc_rs_maxf(ix0, iy0);
                    hi = pc_rs_minf(ix1, iy1);
                    if (lo > hi) {
                        continue;
                    }
                    heights[idx] = h;
                    if (hi >= 1.0f) {
                        face = 0;
                    } else if (ix1 < iy1) {
                        face = ox > 0.0f ? 1 : 2;
                    } else {
                        face = oy > 0.0f ? 3 : 4;
                    }
                    px = dst + idx * n;
                    if (face == 0 && !spec->solid_front) {
                        int32_t sx = pc_f32_to_i32(floorf(fx - (mx - bcx) * off));
                        int32_t sy = pc_f32_to_i32(floorf(fy - (my - bcy) * off));
                        read_px(src, pc_clamp_i32(sx, r.x0, r.x1 - 1), pc_clamp_i32(sy, r.y0, r.y1 - 1), px,
                                n);
                    } else {
                        static const float shades[5] = {1.0f, 0.85f, 0.55f, 0.95f, 0.45f};
                        for (c = 0; c < cc; c++) {
                            px[c] =
                                pc_clampf(sub ? 1.0f - (1.0f - col[c]) * shades[face] : col[c] * shades[face],
                                          0.0f, 1.0f);
                        }
                        if (ctx->alpha) {
                            px[n - 1] = col[n - 1];
                        }
                    }
                }
            }
        }
    }
    free(heights);
    return true;
}

/* The bounds are cut into square cells. Blocks are pushed toward the viewer (front faces shift
 * away from the bounds' centre by their height, with shaded sides); Pyramids raise four shaded
 * triangular faces. Heights are random or follow the cell's brightness. */
static pc_status extrude_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                           const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_extrude_params *spec = p;
    bool nomem = false;
    (void)prep;
    pc_algo_image_crop_into(src, out, dst);
    if (!pc_algo_fx_direct_only() && extrude_blocks(spec, src, out, ctx, dst, &nomem)) {
        return PC_OK;
    }
    if (!pc_algo_fx_direct_only() && nomem) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for Extrude");
    }
    return extrude_direct(spec, src, out, ctx, dst, err);
}

/* The direct search (extrude_direct in the reference): every pixel looks for the block whose front
 * face or sides cover it. */
static pc_status extrude_direct(const pc_algo_extrude_params *spec, const pc_algo_image *src, pc_rect out,
                                const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    pc_rect b = ctx->bounds;
    size_t n = src->ch, w = pc_rect_width(out), i, c;
    float bcx = (float)pc_i32_wrapping_add(b.x0, b.x1) / 2.0f,
          bcy = (float)pc_i32_wrapping_add(b.y0, b.y1) / 2.0f;
    float half_diag = pc_rs_maxf_k(hypotf((float)pc_rect_width(b), (float)pc_rect_height(b)) / 2.0f, 1.0f);
    float reach = pc_algo_stylize2_extrude_reach(spec->depth), csf;
    extrude_ctx *e;
    pc_algo_image_crop_into(src, out, dst);
    if (n == 0 || n > PC_ALGO_MAXC || w == 0) {
        return PC_OK;
    }
    e = calloc(1, sizeof *e);
    if (e == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for Extrude");
    }
    e->src = src;
    e->ctx = ctx;
    e->spec = spec;
    e->b = b;
    e->n = n;
    e->ncache = 0;
    e->next = 0;
    e->cs = pc_f32_to_i32(roundf(pc_clampf(spec->size, 2.0f, 255.0f)));
    if (e->cs <= 0) {
        /* A NaN size: the reference divides by zero (panics). */
        free(e);
        return pc_fail(err, PC_ERR_INVALID, "Extrude size must be a number");
    }
    csf = (float)e->cs;
    for (i = 0; i < pc_fx_npx(out); i++) {
        float *px = dst + i * n, fx, fy;
        int32_t x, y, gx, gy;
        pc_algo_xy(out, i, &x, &y);
        if (!pc_rect_contains(b, x, y)) {
            continue;
        }
        fx = (float)x + 0.5f;
        fy = (float)y + 0.5f;
        gx = pc_fx_div_euclid(pc_i32_wrapping_sub(x, b.x0), e->cs);
        gy = pc_fx_div_euclid(pc_i32_wrapping_sub(y, b.y0), e->cs);
        if (spec->kind == PC_ALGO_EXTRUDE_PYRAMIDS) {
            pc_rect r = cell_rect(e, gx, gy);
            const cell_info *ci;
            float u, v, hgt, s, nx, ny, norm, lambert;
            if (spec->mask_incomplete && incomplete(e, r)) {
                continue;
            }
            ci = info(e, gx, gy);
            u = fx - ((float)r.x0 + csf / 2.0f);
            v = fy - ((float)r.y0 + csf / 2.0f);
            /* Face normals of a pyramid of height H over a cell of half-size s; light from the top-left. */
            hgt = ci->h * pc_clampf(spec->depth, 1.0f, 255.0f) / 255.0f * csf;
            s = csf / 2.0f;
            if (fabsf(u) >= fabsf(v)) {
                nx = pc_fx_signum(u) * hgt, ny = 0.0f;
            } else {
                nx = 0.0f, ny = pc_fx_signum(v) * hgt;
            }
            norm = sqrtf(nx * nx + ny * ny + s * s);
            lambert = pc_rs_maxf_k((nx * -0.5f + ny * -0.5f + s * PC_FX_FRAC_1_SQRT_2) / norm, 0.0f);
            shade_px(e, px, ci->col, 0.35f + 0.9f * lambert);
        } else {
            /* Candidate blocks: own cell and the neighbours whose front faces can reach here. */
            int32_t k = pc_i32_wrapping_add(pc_f32_to_i32(ceilf(reach / csf)), 1), dx, dy;
            bool found = false;
            float bh = 0.0f, mx, my, off;
            int32_t bxc = 0, byc = 0, face = 0;
            pc_rect r;
            const cell_info *ci;
            for (dy = -k; dy <= k; dy++) {
                for (dx = -k; dx <= k; dx++) {
                    int32_t cx = pc_i32_wrapping_add(gx, dx), cy = pc_i32_wrapping_add(gy, dy);
                    float h, ox, oy, half, ix0, ix1, iy0, iy1, lo, hi;
                    r = cell_rect(e, cx, cy);
                    if (r.x1 <= b.x0 || r.y1 <= b.y0 || r.x0 >= b.x1 || r.y0 >= b.y1) {
                        continue;
                    }
                    if (spec->mask_incomplete && incomplete(e, r)) {
                        continue;
                    }
                    h = info(e, cx, cy)->h;
                    if (found && bh >= h) {
                        continue;
                    }
                    /* Front face offset grows with height and distance from the centre. */
                    mx = (float)r.x0 + csf / 2.0f;
                    my = (float)r.y0 + csf / 2.0f;
                    off = reach * h / half_diag;
                    ox = (mx - bcx) * off;
                    oy = (my - bcy) * off;
                    half = csf / 2.0f;
                    if (!sweep(fx, mx, ox, half, &ix0, &ix1) || !sweep(fy, my, oy, half, &iy0, &iy1)) {
                        continue;
                    }
                    lo = pc_rs_maxf(ix0, iy0);
                    hi = pc_rs_minf(ix1, iy1);
                    if (lo > hi) {
                        continue;
                    }
                    found = true;
                    bh = h, bxc = cx, byc = cy;
                    if (hi >= 1.0f) {
                        face = 0;
                    } else if (ix1 < iy1) {
                        /* Left the square through an x side first. */
                        face = ox > 0.0f ? 1 : 2;
                    } else {
                        face = oy > 0.0f ? 3 : 4;
                    }
                }
            }
            if (!found) {
                continue;
            }
            r = cell_rect(e, bxc, byc);
            mx = (float)r.x0 + csf / 2.0f;
            my = (float)r.y0 + csf / 2.0f;
            off = reach * bh / half_diag;
            ci = info(e, bxc, byc);
            if (face == 0) {
                if (spec->solid_front) {
                    shade_px(e, px, ci->col, 1.0f);
                } else {
                    /* Image texture shifted with the face. */
                    int32_t sx = pc_f32_to_i32(floorf(fx - (mx - bcx) * off));
                    int32_t sy = pc_f32_to_i32(floorf(fy - (my - bcy) * off));
                    sx = pc_clamp_i32(sx, r.x0, r.x1 - 1);
                    sy = pc_clamp_i32(sy, r.y0, r.y1 - 1);
                    for (c = 0; c < n; c++) {
                        px[c] = pc_algo_image_get(src, sx, sy, c);
                    }
                }
            } else {
                /* Light from the top-left: the left/top sides are lit. */
                const float shades[5] = {1.0f, 0.85f, 0.55f, 0.95f, 0.45f};
                shade_px(e, px, ci->col, shades[face]);
            }
        }
    }
    free(e);
    return PC_OK;
}

/* ---- Tiles ---- */

/* The bounds are cut into square tiles (`count` along the shorter side), each shifted randomly by
 * up to max_offset % of its size; later tiles overlap earlier ones and gaps get the chosen fill. */
static pc_status tiles_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                         const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_tiles_params *tp = p;
    pc_rect b = ctx->bounds;
    size_t n = src->ch, cc = pc_algo_ncol(ctx, n), i, c;
    uint32_t bw = pc_rect_width(b), bh = pc_rect_height(b);
    uint32_t count = tp->count < 1 ? 1 : (tp->count > 99 ? 99 : tp->count);
    float ts = pc_rs_maxf_k((float)(bw < bh ? bw : bh) / (float)count, 1.0f);
    float m = pc_clampf(tp->max_offset, 0.0f, 99.0f) / 100.0f * ts, fg[PC_ALGO_MAXC], bg[PC_ALGO_MAXC];
    (void)prep, (void)err;
    pc_algo_image_crop_into(src, out, dst);
    if (n == 0 || n > PC_ALGO_MAXC) {
        return PC_OK;
    }
    pc_algo_native(ctx, tp->foreground, fg);
    pc_algo_native(ctx, tp->background, bg);
    for (i = 0; i < pc_fx_npx(out); i++) {
        float *px = dst + i * n, fx, fy, hox = 0.0f, hoy = 0.0f;
        int32_t x, y, gx, gy, hcy = 0, hcx = 0, dx, dy;
        bool hit = false;
        pc_algo_xy(out, i, &x, &y);
        if (!pc_rect_contains(b, x, y)) {
            continue;
        }
        fx = (float)x + 0.5f - (float)b.x0;
        fy = (float)y + 0.5f - (float)b.y0;
        gx = pc_f32_to_i32(floorf(fx / ts));
        gy = pc_f32_to_i32(floorf(fy / ts));
        /* Topmost tile (drawn last = highest row-major index) covering the pixel. */
        for (dy = -1; dy <= 1; dy++) {
            for (dx = -1; dx <= 1; dx++) {
                int32_t cx = pc_i32_wrapping_add(gx, dx), cy = pc_i32_wrapping_add(gy, dy);
                float ox = (pc_algo_hash01(cx, cy, 31, tp->seed) * 2.0f - 1.0f) * m;
                float oy = (pc_algo_hash01(cx, cy, 32, tp->seed) * 2.0f - 1.0f) * m;
                float u = fx - ox - (float)cx * ts, v = fy - oy - (float)cy * ts;
                if (u >= 0.0f && v >= 0.0f && u < ts && v < ts && (float)cx * ts < (float)bw &&
                    (float)cy * ts < (float)bh && cx >= 0 && cy >= 0) {
                    if (!hit || cy > hcy || (cy == hcy && cx > hcx)) {
                        hit = true;
                        hcy = cy, hcx = cx, hox = ox, hoy = oy;
                    }
                }
            }
        }
        if (hit) {
            int32_t sx = pc_f32_to_i32(floorf((float)x + 0.5f - hox));
            int32_t sy = pc_f32_to_i32(floorf((float)y + 0.5f - hoy));
            sx = pc_clamp_i32(sx, b.x0, b.x1 - 1);
            sy = pc_clamp_i32(sy, b.y0, b.y1 - 1);
            for (c = 0; c < n; c++) {
                px[c] = pc_algo_image_get(src, sx, sy, c);
            }
        } else {
            switch (tp->fill) {
            case PC_ALGO_TILEFILL_BACKGROUND:
                memcpy(px, bg, n * sizeof(float));
                break;
            case PC_ALGO_TILEFILL_FOREGROUND:
                memcpy(px, fg, n * sizeof(float));
                break;
            case PC_ALGO_TILEFILL_INVERSE:
                for (c = 0; c < cc; c++) {
                    px[c] = 1.0f - px[c];
                }
                break;
            default:
                break;
            }
        }
    }
    return PC_OK;
}

/* ---- Trace Contour ---- */

/* Per channel, outlines where values cross `level` (0..255) with a 1 px line (dark in additive
 * models) on a white canvas. */
static pc_status trace_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                         const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    static const int32_t nb4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    const pc_algo_trace_params *tp = p;
    size_t n = src->ch, cc = pc_algo_ncol(ctx, n), i, c, k;
    bool sub = pc_algo_subtractive(ctx);
    pc_rect bounds = pc_rect_intersect(ctx->bounds, src->rect);
    pc_rect area = pc_rect_is_empty(bounds) ? src->rect : bounds;
    float t = pc_clampf(tp->level, 0.0f, 255.0f) / 255.0f;
    (void)prep, (void)err;
    pc_algo_image_crop_into(src, out, dst);
#define BRIGHT(xx, yy)                                                                                       \
    (sub ? 1.0f - pc_algo_image_get_edge(src, (xx), (yy), c, PC_ALGO_EDGE_REPEAT, area)                      \
         : pc_algo_image_get_edge(src, (xx), (yy), c, PC_ALGO_EDGE_REPEAT, area))
    for (i = 0; i < pc_fx_npx(out); i++) {
        int32_t x, y;
        pc_algo_xy(out, i, &x, &y);
        for (c = 0; c < cc; c++) {
            bool me = BRIGHT(x, y) > t, line = false;
            float paper;
            if (me == tp->upper) {
                for (k = 0; k < 4 && !line; k++) {
                    line = (BRIGHT(x + nb4[k][0], y + nb4[k][1]) > t) != me;
                }
            }
            paper = line ? 0.0f : 1.0f;
            dst[i * n + c] = sub ? 1.0f - paper : paper;
        }
    }
#undef BRIGHT
    return PC_OK;
}

/* ---- Wind ---- */

float pc_algo_stylize2_wind_reach(pc_algo_wind_method method)
{
    switch (method) {
    case PC_ALGO_WIND_BLAST:
        return 40.0f;
    case PC_ALGO_WIND_STAGGER:
        return 12.0f;
    default:
        return 16.0f;
    }
}

#define WIND_MAX_ROW 1048576u

typedef struct wind_hit {
    float weight;
    int32_t k;
} wind_hit;

/* wind::streaks (Wind and Blast): scatters each source streak once, keeping the per-pixel search's
 * nearest-source tie rule (equal weights prefer the smallest k). dst holds the crop of src; false
 * (dst untouched) when this path doesn't apply; *nomem on allocation failure. */
static bool wind_streaks(const pc_algo_wind_params *wp, const pc_algo_image *src, pc_rect out,
                         const pc_algo_ctx *ctx, float *dst, bool *nomem)
{
    size_t n = src->ch, w = pc_rect_width(out), sources, step, x;
    int32_t lmax = pc_f32_to_i32(pc_algo_stylize2_wind_reach(wp->method)), d = wp->from_right ? -1 : 1, y;
    int64_t start;
    float tmp[PC_ALGO_MAXC], strength = wp->method == PC_ALGO_WIND_BLAST ? 8.0f : 4.0f;
    wind_hit *best;
    *nomem = false;
    if (wp->method == PC_ALGO_WIND_STAGGER || n == 0 || n > PC_ALGO_MAXC || w == 0 || w > WIND_MAX_ROW ||
        pc_rect_is_empty(out)) {
        return false;
    }
    /* Include the upstream halo and one more source for the edge difference. */
    start = wp->from_right ? (int64_t)out.x1 + (lmax - 1) : (int64_t)out.x0 - lmax;
    if (start > INT32_MAX || start < INT32_MIN || start - d > INT32_MAX || start - d < INT32_MIN) {
        return false;
    }
    sources = w + (size_t)lmax;
    if (start + (int64_t)d * (int64_t)(sources - 1) > INT32_MAX ||
        start + (int64_t)d * (int64_t)(sources - 1) < INT32_MIN) {
        return false;
    }
    best = pc_malloc_array(w, sizeof *best);
    if (best == NULL) {
        *nomem = true;
        return false;
    }
    for (y = out.y0; y < out.y1; y++) {
        float prev;
        size_t row = (size_t)(y - out.y0) * w * n;
        for (x = 0; x < w; x++) {
            best[x].weight = 0.0f;
            best[x].k = lmax + 1;
        }
        read_px(src, (int32_t)(start - d), y, tmp, n);
        prev = pc_algo_luma(ctx, tmp, n);
        for (step = 0; step < sources; step++) {
            int32_t s = (int32_t)(start + (int64_t)d * (int64_t)step), kk, kmax;
            float lum, edge, hl, len;
            read_px(src, s, y, tmp, n);
            lum = pc_algo_luma(ctx, tmp, n);
            edge = pc_rs_minf_k(fabsf(lum - prev) * strength, 1.0f);
            prev = lum;
            if (edge <= 0.0f) {
                continue;
            }
            hl = pc_algo_hash01(s, y, 44, wp->seed);
            len = pc_rs_maxf_k(hl * hl * (float)lmax, 1.0f);
            kmax = pc_f32_to_i32(floorf(len));
            kmax = kmax < lmax ? kmax : lmax;
            for (kk = 1; kk <= kmax; kk++) {
                int64_t xt = (int64_t)s + (int64_t)d * kk;
                float weight;
                wind_hit *hit;
                if (xt < out.x0 || xt >= out.x1) {
                    continue;
                }
                weight = edge * (1.0f - (float)kk / (len + 1.0f));
                hit = &best[xt - out.x0];
                /* Sources are visited downwind; equal weights prefer the smallest original k. */
                if (weight > hit->weight || (weight == hit->weight && kk < hit->k)) {
                    hit->weight = weight;
                    hit->k = kk;
                }
            }
        }
        for (x = 0; x < w; x++) {
            if (best[x].weight > 0.0f) {
                int64_t sx = (int64_t)out.x0 + (int64_t)x - (int64_t)d * best[x].k;
                float *px = dst + row + x * n;
                size_t c;
                read_px(src, (int32_t)sx, y, tmp, n);
                for (c = 0; c < n; c++) {
                    px[c] += (tmp[c] - px[c]) * best[x].weight;
                }
            }
        }
    }
    free(best);
    return true;
}

/* Streaks trail downwind from edges (Blast: longer and denser); Stagger shifts random row segments
 * along the wind. */
static pc_status wind_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                        const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_wind_params *wp = p;
    size_t n = src->ch, i, c;
    int32_t lmax = pc_f32_to_i32(pc_algo_stylize2_wind_reach(wp->method));
    /* Streaks move downwind: from the left they extend to the right (+x). */
    int32_t d = wp->from_right ? -1 : 1;
    float tmp[PC_ALGO_MAXC], tmp2[PC_ALGO_MAXC];
    bool nomem = false;
    (void)prep;
    pc_algo_image_crop_into(src, out, dst);
    if (!pc_algo_fx_direct_only() && wind_streaks(wp, src, out, ctx, dst, &nomem)) {
        return PC_OK;
    }
    if (!pc_algo_fx_direct_only() && nomem) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for Wind");
    }
    if (n == 0 || n > PC_ALGO_MAXC) {
        return PC_OK;
    }
    for (i = 0; i < pc_fx_npx(out); i++) {
        float *px = dst + i * n, strength, best_w = 0.0f;
        int32_t x, y, best_s, k;
        pc_algo_xy(out, i, &x, &y);
        if (wp->method == PC_ALGO_WIND_STAGGER) {
            /* Rows split into segments of 8-40 px, each shifted downwind by 0-lmax px. */
            int32_t seg_len = 8 + pc_f32_to_i32(pc_algo_hash01(0, y, 41, wp->seed) * 32.0f);
            int32_t phase = pc_f32_to_i32(pc_algo_hash01(1, y, 42, wp->seed) * (float)seg_len);
            int32_t seg = pc_fx_div_euclid(pc_i32_wrapping_add(pc_i32_wrapping_mul(x, d), phase), seg_len);
            float hs = pc_algo_hash01(seg, y, 43, wp->seed);
            int32_t shift = pc_f32_to_i32(hs * hs * (float)lmax);
            read_px(src, pc_i32_wrapping_sub(x, d * shift), y, tmp, n);
            memcpy(px, tmp, n * sizeof(float));
            continue;
        }
        strength = wp->method == PC_ALGO_WIND_BLAST ? 8.0f : 4.0f;
        best_s = x;
        for (k = 1; k <= lmax; k++) {
            int32_t s = pc_i32_wrapping_sub(x, d * k);
            /* Each source pixel starts a streak of random length; only edges start visible ones. */
            float hl = pc_algo_hash01(s, y, 44, wp->seed), len = pc_rs_maxf_k(hl * hl * (float)lmax, 1.0f);
            float edge, wgt;
            if ((float)k > len) {
                continue;
            }
            read_px(src, s, y, tmp, n);
            read_px(src, pc_i32_wrapping_sub(s, d), y, tmp2, n);
            edge =
                pc_rs_minf_k(fabsf(pc_algo_luma(ctx, tmp, n) - pc_algo_luma(ctx, tmp2, n)) * strength, 1.0f);
            wgt = edge * (1.0f - (float)k / (len + 1.0f));
            if (wgt > best_w) {
                best_w = wgt, best_s = s;
            }
        }
        if (best_w > 0.0f) {
            read_px(src, best_s, y, tmp, n);
            for (c = 0; c < n; c++) {
                px[c] += (tmp[c] - px[c]) * best_w;
            }
        }
    }
    return PC_OK;
}

/* ---- halos and defs ---- */

static pc_algo_halo diffuse_halo(const void *p, const pc_rect *b)
{
    (void)p, (void)b;
    return pc_algo_halo_reach(1.0f);
}

static pc_algo_halo extrude_halo(const void *p, const pc_rect *b)
{
    const pc_algo_extrude_params *e = p;
    (void)b;
    return pc_algo_halo_reach(pc_algo_stylize2_extrude_reach(e->depth) +
                              pc_clampf(e->size, 2.0f, 255.0f) * 3.0f + 1.0f);
}

static pc_algo_halo wind_halo(const void *p, const pc_rect *b)
{
    (void)b;
    return pc_algo_halo_reach(pc_algo_stylize2_wind_reach(((const pc_algo_wind_params *)p)->method));
}

static const pc_algo_jfield DIFFUSE_F[] = {
    PC_ALGO_JFE("mode", pc_algo_diffuse_params, mode, PC_ALGO_ENUM_DIFFUSE_MODE, false),
    PC_ALGO_JF("seed", pc_algo_diffuse_params, seed, PC_ALGO_JF_U32),
};
static const pc_algo_jfield EXTRUDE_F[] = {
    PC_ALGO_JFE("kind", pc_algo_extrude_params, kind, PC_ALGO_ENUM_EXTRUDE_TYPE, false),
    PC_ALGO_JF("size", pc_algo_extrude_params, size, PC_ALGO_JF_F32),
    PC_ALGO_JF("depth", pc_algo_extrude_params, depth, PC_ALGO_JF_F32),
    PC_ALGO_JF("levelBased", pc_algo_extrude_params, level_based, PC_ALGO_JF_BOOL),
    PC_ALGO_JF("solidFront", pc_algo_extrude_params, solid_front, PC_ALGO_JF_BOOL),
    PC_ALGO_JF("maskIncomplete", pc_algo_extrude_params, mask_incomplete, PC_ALGO_JF_BOOL),
    PC_ALGO_JF("seed", pc_algo_extrude_params, seed, PC_ALGO_JF_U32),
};
static const pc_algo_jfield TILES_F[] = {
    PC_ALGO_JF("count", pc_algo_tiles_params, count, PC_ALGO_JF_U32),
    PC_ALGO_JF("maxOffset", pc_algo_tiles_params, max_offset, PC_ALGO_JF_F32),
    PC_ALGO_JFE("fill", pc_algo_tiles_params, fill, PC_ALGO_ENUM_TILE_FILL, false),
    PC_ALGO_JFA("foreground", pc_algo_tiles_params, foreground, 4, false),
    PC_ALGO_JFA("background", pc_algo_tiles_params, background, 4, false),
    PC_ALGO_JF("seed", pc_algo_tiles_params, seed, PC_ALGO_JF_U32),
};
static const pc_algo_jfield TRACE_F[] = {
    PC_ALGO_JF("level", pc_algo_trace_params, level, PC_ALGO_JF_F32),
    PC_ALGO_JF("upper", pc_algo_trace_params, upper, PC_ALGO_JF_BOOL),
};
static const pc_algo_jfield WIND_F[] = {
    PC_ALGO_JFE("method", pc_algo_wind_params, method, PC_ALGO_ENUM_WIND_METHOD, false),
    PC_ALGO_JF("fromRight", pc_algo_wind_params, from_right, PC_ALGO_JF_BOOL),
    PC_ALGO_JF("seed", pc_algo_wind_params, seed, PC_ALGO_JF_U32),
};

PC_FX_DEF(pc_algo_def_diffuse, "diffuse", pc_algo_diffuse_params, DIFFUSE_F, NULL, diffuse_halo, diffuse_k);
PC_FX_DEF(pc_algo_def_extrude, "extrude", pc_algo_extrude_params, EXTRUDE_F, NULL, extrude_halo, extrude_k);
PC_FX_DEF(pc_algo_def_tiles, "tiles", pc_algo_tiles_params, TILES_F, NULL, pc_fx_halo_bounds, tiles_k);
PC_FX_DEF(pc_algo_def_trace_contour, "traceContour", pc_algo_trace_params, TRACE_F, NULL, diffuse_halo,
          trace_k);
PC_FX_DEF(pc_algo_def_wind, "wind", pc_algo_wind_params, WIND_F, NULL, wind_halo, wind_k);
