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

/* pixelate::screen: tile-local memo of each screen cell's dot radius, in rotated screen
 * coordinates. A cell's radius doesn't depend on the output pixel (the five taps included), so
 * the result is the direct computation's. */
typedef struct screen_memo {
    int32_t x0, y0;
    size_t width, height;
    float *radii; /* NaN: not computed yet */
} screen_memo;

static bool screen_new(screen_memo *m, pc_rect out, float ox, float oy, float s, float co, float cell)
{
    float lo_x = INFINITY, lo_y = INFINITY, hi_x = -INFINITY, hi_y = -INFINITY;
    int32_t ys[2], xs[2];
    int64_t width, height;
    size_t len, i;
    int a, b;
    m->radii = NULL;
    if (pc_rect_is_empty(out) || !isfinite(cell) || cell < 1.0f) {
        return false;
    }
    ys[0] = out.y0, ys[1] = out.y1 - 1;
    xs[0] = out.x0, xs[1] = out.x1 - 1;
    for (a = 0; a < 2; a++) {
        for (b = 0; b < 2; b++) {
            float fx = (float)xs[b] + 0.5f - ox, fy = (float)ys[a] + 0.5f - oy;
            float u = (fx * co + fy * s) / cell, v = (-fx * s + fy * co) / cell;
            if (!isfinite(u) || !isfinite(v) || fabsf(u) > 4000000.0f || fabsf(v) > 4000000.0f) {
                return false;
            }
            lo_x = pc_rs_minf(lo_x, u);
            lo_y = pc_rs_minf(lo_y, v);
            hi_x = pc_rs_maxf(hi_x, u);
            hi_y = pc_rs_maxf(hi_y, v);
        }
    }
    /* Include the neighbouring dots and a rounding margin; a key outside is computed directly. */
    m->x0 = pc_f32_to_i32(floorf(lo_x)) - 2;
    m->y0 = pc_f32_to_i32(floorf(lo_y)) - 2;
    width = (int64_t)pc_f32_to_i32(floorf(hi_x)) + 3 - m->x0;
    height = (int64_t)pc_f32_to_i32(floorf(hi_y)) + 3 - m->y0;
    if (width < 0 || height < 0) {
        return false;
    }
    m->width = (size_t)width;
    m->height = (size_t)height;
    len = m->width * m->height;
    /* At most 1 MiB per channel, even for an explicit oversized tile. */
    if (len > 256 * 1024) {
        return false;
    }
    m->radii = pc_malloc_array(len > 0 ? len : 1, sizeof *m->radii);
    if (m->radii == NULL) {
        return false;
    }
    for (i = 0; i < len; i++) {
        m->radii[i] = NAN;
    }
    return true;
}

/* The memo slot of screen cell (x, y), or NULL when it has none. Keys must be exact integers:
 * non-finite or large coordinates are never aliased onto other cells. */
static float *screen_slot(const screen_memo *m, float x, float y)
{
    int64_t cx, cy;
    if (m->radii == NULL || !isfinite(x) || !isfinite(y) || fabsf(x) > 4000001.0f || fabsf(y) > 4000001.0f) {
        return NULL;
    }
    cx = (int64_t)pc_f32_to_i32(x) - m->x0;
    cy = (int64_t)pc_f32_to_i32(y) - m->y0;
    if (cx < 0 || cy < 0 || (uint64_t)cx >= m->width || (uint64_t)cy >= m->height) {
        return NULL;
    }
    return &m->radii[(size_t)cy * m->width + (size_t)cx];
}

/* A dot's radius for the cell centred at (mu, mv) in screen space. */
static float halftone_radius(const halftone_ctx *h, float mu, float mv, float s, float co, float ox, float oy,
                             size_t c, bool sub)
{
    float cell = h->cell, raw, ink;
    const float touch = PC_FX_FRAC_PI_4;
    /* Back to document space to read the cell's value. */
    raw = pc_clampf(halftone_value(h, mu * co - mv * s + ox, mu * s + mv * co + oy, c), 0.0f, 1.0f);
    ink = sub && !h->rgb ? raw : 1.0f - raw;
    /* Dot area tracks ink: pi r^2 = ink cell^2 until dots touch, then grow to cover the corners. */
    if (ink <= touch) {
        return cell * sqrtf(ink / PC_FX_PI);
    }
    return cell * (0.5f + (ink - touch) / (1.0f - touch) * (PC_FX_FRAC_1_SQRT_2 - 0.5f));
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
    screen_memo screens[PC_ALGO_MAXC];
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
    /* Keep the memo metadata on the stack; only the optional, bounded tables allocate. */
    for (c = 0; c < PC_ALGO_MAXC; c++) {
        screens[c].radii = NULL;
        if (c < chans && c < nrot && !pc_algo_fx_direct_only()) {
            (void)screen_new(&screens[c], out, ox, oy, rs[c], rc[c], h.cell);
        }
    }
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
                    float d = sqrtf(pc_fx_sq(u - mu) + pc_fx_sq(w2 - mv)), r, *slot;
                    if (d >= rmax + 0.5f) {
                        continue;
                    }
                    slot = screen_slot(&screens[c], cu + (float)di, cv + (float)dj);
                    if (slot != NULL && !isnan(*slot)) {
                        r = *slot;
                    } else {
                        r = halftone_radius(&h, mu, mv, s, co, ox, oy, c, sub);
                        if (slot != NULL) {
                            *slot = r;
                        }
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
    for (c = 0; c < PC_ALGO_MAXC; c++) {
        free(screens[c].radii);
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
        {
            /* The colour at the cell's point, as Photoshop's cells are (solid, hard edges). */
            float spx, spy;
            int32_t sx, sy;
            pc_algo_cell_point(o.x, o.y, cell, cp->seed, ox, oy, &spx, &spy);
            sx = pc_f32_to_i32(floorf(spx));
            sy = pc_f32_to_i32(floorf(spy));
            if (pc_rect_contains(src->rect, sx, sy) && pc_rect_contains(b, sx, sy)) {
                for (c = 0; c < n; c++) {
                    px[c] = pc_algo_image_get(src, sx, sy, c);
                }
                continue;
            }
        }
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

typedef struct quadrant {
    float variance;
    float mean[PC_ALGO_MAXC];
} quadrant;

/* One row of 3x3 quadrant statistics (window row y .. y + 2), in the direct path's nine-sample
 * addition order (reassociating the sums can change variance ties). */
static void facet_row(const pc_algo_image *src, pc_rect win, const float *lum, size_t y, quadrant *line)
{
    size_t n = src->ch, w = pc_rect_width(win), qw = w - 2, x, dy, dx, c;
    size_t stride = (size_t)pc_rect_width(src->rect) * n, offset = (size_t)(win.x0 - src->rect.x0) * n;
    for (x = 0; x < qw; x++) {
        float sum[PC_ALGO_MAXC] = {0}, s = 0.0f, s2 = 0.0f;
        for (dy = 0; dy < 3; dy++) {
            const float *row =
                src->data + (size_t)(win.y0 + (int32_t)(y + dy) - src->rect.y0) * stride + offset;
            for (dx = 0; dx < 3; dx++) {
                const float *px = row + (x + dx) * n;
                float l = lum[(y + dy) * w + x + dx];
                for (c = 0; c < n; c++) {
                    sum[c] += px[c];
                }
                s += l;
                s2 += l * l;
            }
        }
        for (c = 0; c < PC_ALGO_MAXC; c++) {
            sum[c] /= 9.0f;
        }
        line[x].variance = s2 / 9.0f - pc_fx_sq(s / 9.0f);
        memcpy(line[x].mean, sum, sizeof sum);
    }
}

/* pixelate::facet::run: reuses each 3x3 quadrant at its four output pixels. False when the
 * source doesn't cover the window or scratch is out of bounds; *nomem on allocation failure. */
static bool facet_run(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx, float *dst, bool *nomem)
{
    size_t n = src->ch, w, h, len, qw, width, height, x, y, k;
    int64_t wx0 = (int64_t)out.x0 - 2, wy0 = (int64_t)out.y0 - 2, wx1 = (int64_t)out.x1 + 2,
            wy1 = (int64_t)out.y1 + 2;
    pc_rect win;
    float *lum;
    quadrant *q[3], *rot;
    *nomem = false;
    if (n == 0 || n > PC_ALGO_MAXC || pc_rect_is_empty(out) || wx0 < INT32_MIN || wy0 < INT32_MIN ||
        wx1 > INT32_MAX || wy1 > INT32_MAX) {
        return false;
    }
    win = pc_rect_make((int32_t)wx0, (int32_t)wy0, (int32_t)wx1, (int32_t)wy1);
    if (!pc_rect_eq(pc_rect_intersect(win, src->rect), win)) {
        return false;
    }
    w = pc_rect_width(win);
    h = pc_rect_height(win);
    len = w * h;
    /* A normal 256 px tile needs 264 KiB. Bound scratch for explicit tile overrides. */
    if (len > 1024 * 1024) {
        return false;
    }
    lum = pc_malloc_array(len, sizeof *lum);
    qw = w - 2;
    q[0] = pc_malloc_array(qw, sizeof *q[0]);
    q[1] = pc_malloc_array(qw, sizeof *q[1]);
    q[2] = pc_malloc_array(qw, sizeof *q[2]);
    if (lum == NULL || q[0] == NULL || q[1] == NULL || q[2] == NULL) {
        free(lum), free(q[0]), free(q[1]), free(q[2]);
        *nomem = true;
        return false;
    }
    {
        size_t stride = (size_t)pc_rect_width(src->rect) * n, offset = (size_t)(win.x0 - src->rect.x0) * n;
        for (y = 0; y < h; y++) {
            const float *row = src->data + (size_t)(win.y0 + (int32_t)y - src->rect.y0) * stride + offset;
            for (x = 0; x < w; x++) {
                lum[y * w + x] = pc_algo_luma(ctx, row + x * n, n);
            }
        }
    }
    facet_row(src, win, lum, 0, q[0]);
    facet_row(src, win, lum, 1, q[1]);
    width = pc_rect_width(out);
    height = pc_rect_height(out);
    for (y = 0; y < height; y++) {
        facet_row(src, win, lum, y + 2, q[2]);
        for (x = 0; x < width; x++) {
            const quadrant *cand[4] = {&q[0][x], &q[0][x + 2], &q[2][x], &q[2][x + 2]};
            float best = FLT_MAX;
            const float *mean = NULL;
            /* Preserve quadrant order, including exact ties and non-finite variances. */
            for (k = 0; k < 4; k++) {
                if (cand[k]->variance < best) {
                    best = cand[k]->variance;
                    mean = cand[k]->mean;
                }
            }
            if (mean != NULL) {
                memcpy(dst + (y * width + x) * n, mean, n * sizeof(float));
            } else {
                memset(dst + (y * width + x) * n, 0, n * sizeof(float));
            }
        }
        rot = q[0];
        q[0] = q[1];
        q[1] = q[2];
        q[2] = rot;
    }
    free(lum), free(q[0]), free(q[1]), free(q[2]);
    return true;
}

/* A Kuwahara filter (radius 2): each pixel takes the mean of the least-varied of its four
 * overlapping quadrants, clumping similar colours into flat facets while keeping edges. */
static pc_status facet_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                         const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    static const int32_t quads[4][2] = {{-2, -2}, {0, -2}, {-2, 0}, {0, 0}};
    const int32_t r = 2;
    size_t n = src->ch, i, c, q;
    bool nomem;
    (void)p, (void)prep;
    nomem = false;
    if (!pc_algo_fx_direct_only() && facet_run(src, out, ctx, dst, &nomem)) {
        return PC_OK;
    }
    if (nomem) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for Facet");
    }
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
