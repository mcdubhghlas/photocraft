/* Quick Selection brush, behaving like Photoshop's (port of crates/algo/src/segment/quick.rs).
 *
 * Photoshop 25.4 was measured as a black box (synthetic images and public-domain paintings, the
 * exact selection masks read back; no Photoshop code or assets). The brush footprint is always
 * selected; beyond it the selection grows to image edges but every selected pixel costs a little,
 * so a click takes a whole object only while it is small enough for that brush; a stroke is one
 * set of seeds; colours elsewhere make no difference. It works on the whole document up to
 * 512 x 512 pixels and at exactly half resolution above, the mask scaled up bilinearly.
 *
 * The model is one min cut (Boykov & Jolly, ICCV 2001) over the working image minimising
 *
 *   sum over selected pixels p beyond the footprint of  AREA * (1 + DIST * d(p)^POW)
 *   + sum over cut neighbour pairs p~q of  w(|I_p - I_q|) / |p - q|,
 *   w(D) = FLOOR + 1 / (1 + (D / SIGMA)^K),
 *
 * the (slightly grown) footprint hard foreground and d the distance from it. Larger windows are
 * cut coarse to fine (Lombaert, Sun, Grady & Xu, ICCV 2005). The constants were fitted to
 * Photoshop's masks. */
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/segment.h"
#include "algo/selection.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* Band half-width (pixels) re-cut at each finer level. */
#define BAND 3.0f

pc_algo_quick_tuning pc_algo_quick_tuning_default(void)
{
    pc_algo_quick_tuning t;
    t.area = 0.004079f;
    t.dist = 0.002651f;
    t.pow = 0.98885f;
    t.floor = 0.018847f;
    t.sigma = 0.048450f;
    t.k = 1.98549f;
    t.grow = 1.69371f;
    t.grow_frac = 0.103003f;
    t.direct_px = 300000;
    return t;
}

size_t pc_algo_quick_working_step(size_t w, size_t h)
{
    size_t n;
    return pc_size_mul(w, h, &n) && n <= 512 * 512 ? 1 : 2;
}

void pc_algo_quick_stroke_mask(const float (*pts)[2], size_t n, float radius, size_t w, size_t h, bool *m)
{
    size_t nseg = n == 1 ? 1 : (n >= 2 ? n - 1 : 0), k, i;
    memset(m, 0, w * h * sizeof(bool));
    for (k = 0; k < nseg; k++) {
        const float *a = pts[k], *b = n == 1 ? pts[0] : pts[k + 1];
        size_t x0 = pc_f32_to_size(pc_rs_maxf_k(floorf(pc_rs_minf(a[0], b[0]) - radius), 0.0f));
        size_t y0 = pc_f32_to_size(pc_rs_maxf_k(floorf(pc_rs_minf(a[1], b[1]) - radius), 0.0f));
        size_t x1 = pc_f32_to_size(pc_rs_maxf_k(ceilf(pc_rs_maxf(a[0], b[0]) + radius), 0.0f));
        size_t y1 = pc_f32_to_size(pc_rs_maxf_k(ceilf(pc_rs_maxf(a[1], b[1]) + radius), 0.0f));
        float dx = b[0] - a[0], dy = b[1] - a[1], len2 = dx * dx + dy * dy;
        size_t x, y;
        x1 = x1 < w ? x1 : w;
        y1 = y1 < h ? y1 : h;
        for (y = y0; y < y1; y++) {
            for (x = x0; x < x1; x++) {
                float px = (float)x + 0.5f, py = (float)y + 0.5f, t, qx, qy;
                t = len2 > 0.0f ? pc_clampf(((px - a[0]) * dx + (py - a[1]) * dy) / len2, 0.0f, 1.0f) : 0.0f;
                qx = a[0] + t * dx - px;
                qy = a[1] + t * dy - py;
                if (qx * qx + qy * qy <= radius * radius) {
                    m[y * w + x] = true;
                }
            }
        }
    }
    for (i = 0; i < n; i++) {
        float x = pts[i][0], y = pts[i][1];
        if (x >= 0.0f && y >= 0.0f && pc_f32_to_size(x) < w && pc_f32_to_size(y) < h) {
            m[pc_f32_to_size(y) * w + pc_f32_to_size(x)] = true;
        }
    }
}

pc_status pc_algo_quick_upsample(const bool *low, size_t lw, size_t lh, size_t step, size_t w, size_t h,
                                 uint8_t *out, pc_error *err)
{
    size_t x, y, n;
    float s = (float)step;
    if (!pc_size_mul(w, h, &n)) {
        return pc_fail(err, PC_ERR_LIMIT, "selection too large");
    }
    if (step == 1) {
        if (lw * lh != n) {
            return pc_fail(err, PC_ERR_INVALID, "working mask %zux%zu for %zux%zu", lw, lh, w, h);
        }
        for (x = 0; x < n; x++) {
            out[x] = low[x] ? 255 : 0;
        }
        return PC_OK;
    }
    if (n > 0 && (lw == 0 || lh == 0)) {
        return pc_fail(err, PC_ERR_INVALID, "empty working mask");
    }
#define AT(xx, yy)                                                                                           \
    (low[(size_t)pc_clamp_i64((yy), 0, (int64_t)lh - 1) * lw +                                               \
         (size_t)pc_clamp_i64((xx), 0, (int64_t)lw - 1)]                                                     \
         ? 1.0f                                                                                              \
         : 0.0f)
    for (y = 0; y < h; y++) {
        float fy = ((float)y + 0.5f) / s - 0.5f, ty = fy - floorf(fy);
        int64_t iy = (int64_t)floorf(fy);
        for (x = 0; x < w; x++) {
            float fx = ((float)x + 0.5f) / s - 0.5f, tx = fx - floorf(fx), v;
            int64_t ix = (int64_t)floorf(fx);
            v = (AT(ix, iy) * (1.0f - tx) + AT(ix + 1, iy) * tx) * (1.0f - ty) +
                (AT(ix, iy + 1) * (1.0f - tx) + AT(ix + 1, iy + 1) * tx) * ty;
            out[y * w + x] = pc_f32_to_u8(roundf(v * 255.0f));
        }
    }
#undef AT
    return PC_OK;
}

typedef struct qweight {
    float floor, sigma, k, scale;
} qweight;

static float quick_weight(void *ctx, float dd)
{
    const qweight *q = ctx;
    float wv = q->floor + 1.0f / (1.0f + powf(sqrtf(dd) / q->sigma, q->k));
    return q->scale * wv;
}

/* Min cut with only foreground costs, coarse to fine above direct_px pixels: the half-size
 * problem (colours averaged, pixel costs summed, cut weights doubled) gives a boundary and the
 * full-size cut is redone only within BAND of it. Seeds stay foreground. */
static pc_status cut_levels(const pc_algo_rgb_image *img, const uint8_t *fixed, const float *cost_fg,
                            qweight q, size_t direct_px, bool optimized, bool *out, pc_error *err)
{
    size_t w = img->w, h = img->h, n = w * h, cw, ch, cn, i, x, y;
    float *zeros = NULL, *ccost = NULL, *near = NULL;
    uint8_t *cfix = NULL, *banded = NULL;
    bool *coarse = NULL, *proj = NULL, *edge = NULL, any = false;
    pc_algo_rgb_image cimg = {0, 0, NULL};
    pc_status st = PC_OK;
    zeros = pc_calloc_array(n ? n : 1, sizeof(float));
    if (zeros == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for Quick Selection");
    }
    if (n <= direct_px || w < 8 || h < 8) {
        st = pc_algo_segment_grid_cut_impl(img, cost_fg, zeros, fixed, quick_weight, &q, NULL, optimized, out,
                                           err);
        free(zeros);
        return st;
    }
    cw = (w + 1) / 2;
    ch = (h + 1) / 2;
    cn = cw * ch;
    cfix = pc_calloc_array(cn, 1);
    ccost = pc_calloc_array(cn, sizeof(float));
    coarse = pc_malloc_array(cn, sizeof(bool));
    proj = pc_malloc_array(n, sizeof(bool));
    edge = pc_malloc_array(n, sizeof(bool));
    banded = pc_malloc_array(n, 1);
    if (cfix == NULL || ccost == NULL || coarse == NULL || proj == NULL || edge == NULL || banded == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for Quick Selection");
        goto done;
    }
    if ((st = pc_algo_rgb_image_downsample(img, 2, &cimg, err)) != PC_OK) {
        goto done;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            size_t c = (y / 2) * cw + x / 2;
            ccost[c] += cost_fg[y * w + x];
            if (fixed[y * w + x] == PC_ALGO_SEGMENT_HARD_FG) {
                cfix[c] = PC_ALGO_SEGMENT_HARD_FG;
            }
        }
    }
    {
        qweight q2 = q;
        q2.scale = q.scale * 2.0f;
        if ((st = cut_levels(&cimg, cfix, ccost, q2, direct_px, optimized, coarse, err)) != PC_OK) {
            goto done;
        }
    }
    for (i = 0; i < n; i++) {
        proj[i] = coarse[(i / w / 2) * cw + (i % w) / 2];
    }
    for (i = 0; i < n; i++) {
        bool v = proj[i];
        x = i % w;
        y = i / w;
        edge[i] = (x > 0 && proj[i - 1] != v) || (x + 1 < w && proj[i + 1] != v) ||
                  (y > 0 && proj[i - w] != v) || (y + 1 < h && proj[i + w] != v);
        any |= edge[i];
    }
    if (!any) {
        for (i = 0; i < n; i++) {
            out[i] = proj[i] || fixed[i] == PC_ALGO_SEGMENT_HARD_FG;
        }
        goto done;
    }
    if ((st = pc_algo_selection_edt(edge, w, h, &near, err)) != PC_OK) {
        goto done;
    }
    for (i = 0; i < n; i++) {
        if (fixed[i] == PC_ALGO_SEGMENT_HARD_FG) {
            banded[i] = PC_ALGO_SEGMENT_HARD_FG;
        } else if (near[i] <= BAND) {
            banded[i] = PC_ALGO_SEGMENT_FREE;
        } else {
            banded[i] = proj[i] ? PC_ALGO_SEGMENT_HARD_FG : PC_ALGO_SEGMENT_HARD_BG;
        }
    }
    st = pc_algo_segment_grid_cut_impl(img, cost_fg, zeros, banded, quick_weight, &q, NULL, optimized, out,
                                       err);
done:
    pc_algo_rgb_image_fini(&cimg);
    free(zeros);
    free(ccost);
    free(near);
    free(cfix);
    free(banded);
    free(coarse);
    free(proj);
    free(edge);
    return st;
}

static pc_status select_working(const pc_algo_rgb_image *img, const bool *seeds, float radius,
                                const pc_algo_quick_tuning *t, bool optimized, bool *low, pc_error *err)
{
    size_t n, i;
    float *d = NULL, *cost = NULL, grow;
    uint8_t *fixed = NULL;
    qweight q;
    pc_status st;
    if (!pc_size_mul(img->w, img->h, &n) || n > PC_MAX_ALLOC / sizeof(float)) {
        return pc_fail(err, PC_ERR_LIMIT, "Quick Selection window too large");
    }
    if (n == 0) {
        return PC_OK;
    }
    if ((st = pc_algo_selection_edt(seeds, img->w, img->h, &d, err)) != PC_OK) {
        return st;
    }
    cost = pc_malloc_array(n, sizeof(float));
    fixed = pc_malloc_array(n, 1);
    if (cost == NULL || fixed == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for Quick Selection");
        goto done;
    }
    grow = pc_rs_maxf_k(t->grow + t->grow_frac * radius, 0.0f);
    for (i = 0; i < n; i++) {
        cost[i] = t->area * (1.0f + t->dist * powf(pc_rs_maxf_k(d[i] - grow, 0.0f), t->pow));
        fixed[i] = d[i] <= grow ? PC_ALGO_SEGMENT_HARD_FG : PC_ALGO_SEGMENT_FREE;
    }
    q.floor = t->floor;
    q.sigma = pc_rs_maxf_k(t->sigma, 1e-4f);
    q.k = t->k;
    q.scale = 1.0f;
    st = cut_levels(img, fixed, cost, q, t->direct_px > 1024 ? t->direct_px : 1024, optimized, low, err);
done:
    free(d);
    free(cost);
    free(fixed);
    return st;
}

pc_status pc_algo_quick_select_working(const pc_algo_rgb_image *img, const bool *seeds, float radius,
                                       const pc_algo_quick_tuning *t, bool *low, pc_error *err)
{
    return select_working(img, seeds, radius, t, true, low, err);
}

static int32_t clamp_coord(float v)
{
    return pc_f32_to_i32(pc_clampf(v, -1073741824.0f, 1073741824.0f));
}

static int32_t sat_mul2(int32_t v)
{
    int64_t m = (int64_t)v * 2;
    return m > INT32_MAX ? INT32_MAX : (m < INT32_MIN ? INT32_MIN : (int32_t)m);
}

/* o + (v - o).div_euclid(step) * step */
static int32_t snap0(int32_t v, int32_t o, int32_t step)
{
    int64_t d = (int64_t)v - o, q = d / step;
    if (d % step < 0) {
        q -= 1;
    }
    return (int32_t)(o + q * step);
}

pc_status pc_algo_quick_select_using(const pc_algo_sampler *s, pc_rect canvas, const float (*points)[2],
                                     size_t npoints, float size, const pc_algo_quick_tuning *t,
                                     bool optimized, pc_algo_region *out, pc_error *err)
{
    float r, rw, sf, x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX;
    pc_rect bbox;
    int32_t step, margin;
    size_t i;
    pc_status st = PC_OK;
    out->bbox = PC_RECT_EMPTY;
    out->mask = NULL;
    out->len = 0;
    if (npoints == 0 || pc_rect_is_empty(canvas)) {
        return PC_OK;
    }
    r = pc_rs_maxf_k(size, 1.0f) / 2.0f;
    for (i = 0; i < npoints; i++) {
        x0 = pc_rs_minf(x0, points[i][0] - r);
        y0 = pc_rs_minf(y0, points[i][1] - r);
        x1 = pc_rs_maxf(x1, points[i][0] + r);
        y1 = pc_rs_maxf(y1, points[i][1] + r);
    }
    bbox = pc_rect_intersect(pc_rect_make(clamp_coord(floorf(x0)), clamp_coord(floorf(y0)),
                                          pc_i32_sat_add(clamp_coord(ceilf(x1)), 1),
                                          pc_i32_sat_add(clamp_coord(ceilf(y1)), 1)),
                             canvas);
    if (pc_rect_is_empty(bbox)) {
        return PC_OK;
    }
    step = (int32_t)pc_algo_quick_working_step(pc_rect_width(canvas), pc_rect_height(canvas));
    sf = (float)step;
    rw = r / sf;
    /* Work in a window around the stroke; grow it while the selection runs into its border. */
    margin = pc_i32_wrapping_mul(pc_f32_to_i32(pc_rs_maxf(48.0f, 2.0f * rw)), step);
    for (;;) {
        pc_rect grown = pc_rect_intersect(pc_rect_inflate(bbox, margin), canvas), window;
        pc_algo_rgb_image img = {0, 0, NULL};
        float (*pts)[2] = NULL;
        bool *seeds = NULL, *low = NULL, any = false, inner[4], touch = false, again = false;
        uint8_t *mask = NULL;
        size_t lw, lh, n, x, y, ww, wh;
        int k;
        window = pc_rect_make(snap0(grown.x0, canvas.x0, step), snap0(grown.y0, canvas.y0, step), grown.x1,
                              grown.y1);
        if ((st = pc_algo_sampler_rgb_scaled(s, window, (size_t)step, &img, err)) != PC_OK) {
            return st;
        }
        lw = img.w;
        lh = img.h;
        n = lw * lh;
        pts = pc_malloc_array(npoints, sizeof *pts);
        seeds = pc_malloc_array(n ? n : 1, sizeof(bool));
        low = pc_calloc_array(n ? n : 1, sizeof(bool));
        if (pts == NULL || seeds == NULL || low == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory for Quick Selection");
            goto round_done;
        }
        for (i = 0; i < npoints; i++) {
            pts[i][0] = (points[i][0] - (float)window.x0) / sf;
            pts[i][1] = (points[i][1] - (float)window.y0) / sf;
        }
        pc_algo_quick_stroke_mask((const float (*)[2])pts, npoints, pc_rs_maxf_k(rw, 0.5f), lw, lh, seeds);
        for (i = 0; i < n && !any; i++) {
            any = seeds[i];
        }
        if (!any) {
            goto round_done;
        }
        /* The window's sides are free like the canvas' edges, so a selection clear of them is the
         * best over the whole canvas too; one that reaches a side inside the canvas is recomputed
         * in a larger window. */
        inner[0] = window.x0 > canvas.x0;
        inner[1] = window.y0 > canvas.y0;
        inner[2] = window.x1 < canvas.x1;
        inner[3] = window.y1 < canvas.y1;
        if ((st = select_working(&img, seeds, rw, t, optimized, low, err)) != PC_OK) {
            goto round_done;
        }
        for (k = 0; k < 4 && !touch; k++) {
            if (!inner[k]) {
                continue;
            }
            switch (k) {
            case 0:
                for (y = 0; y < lh && !touch; y++) {
                    touch = low[y * lw];
                }
                break;
            case 1:
                for (x = 0; x < lw && !touch; x++) {
                    touch = low[x];
                }
                break;
            case 2:
                for (y = 0; y < lh && !touch; y++) {
                    touch = low[y * lw + lw - 1];
                }
                break;
            default:
                for (x = 0; x < lw && !touch; x++) {
                    touch = low[(lh - 1) * lw + x];
                }
                break;
            }
        }
        if (touch) {
            margin = sat_mul2(margin);
            again = true;
            goto round_done;
        }
        ww = pc_rect_width(window);
        wh = pc_rect_height(window);
        mask = pc_malloc_array(ww * wh > 0 ? ww * wh : 1, 1);
        if (mask == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory for Quick Selection");
            goto round_done;
        }
        if ((st = pc_algo_quick_upsample(low, lw, lh, (size_t)step, ww, wh, mask, err)) == PC_OK) {
            pc_algo_region reg;
            reg.bbox = window;
            reg.mask = mask;
            reg.len = ww * wh;
            mask = NULL;
            st = pc_algo_segment_trim_region(&reg, out, err);
        }
    round_done:
        free(mask);
        pc_algo_rgb_image_fini(&img);
        free(pts);
        free(seeds);
        free(low);
        if (!again) {
            return st;
        }
    }
}

pc_status pc_algo_quick_select_with(const pc_algo_sampler *s, pc_rect canvas, const float (*points)[2],
                                    size_t npoints, float size, const pc_algo_quick_tuning *t,
                                    pc_algo_region *out, pc_error *err)
{
    return pc_algo_quick_select_using(s, canvas, points, npoints, size, t, true, out, err);
}

pc_status pc_algo_quick_select(const pc_algo_sampler *s, pc_rect canvas, const float (*points)[2],
                               size_t npoints, float size, pc_algo_region *out, pc_error *err)
{
    pc_algo_quick_tuning t = pc_algo_quick_tuning_default();
    return pc_algo_quick_select_using(s, canvas, points, npoints, size, &t, true, out, err);
}
