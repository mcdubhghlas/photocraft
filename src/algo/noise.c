/* Add Noise, Median and Dust & Scratches (port of crates/algo/src/noise.rs; hash01 lives in
 * fxutil.c). The median slides a histogram of value ranks over a disc (Huang), giving exactly the
 * sample a selection over the disc would pick. */
#include "algo/blur_int.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

#define TAU_F 6.28318530717958647692f

pc_status pc_algo_add_noise_k(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx,
                              const pc_algo_add_noise_params *p, float *dst, pc_error *err)
{
    size_t n = src->ch, cc = pc_algo_ncol(ctx, n), w = pc_rect_width(out), npx = w * pc_rect_height(out), i,
           c;
    float a = pc_rs_maxf_k(p->amount, 0.0f) / 100.0f * 0.5f;
    (void)err;
    pc_algo_image_crop_into(src, out, dst);
    if (n == 0) {
        return PC_OK;
    }
    for (i = 0; i < npx; i++) {
        float *px = dst + i * n;
        int32_t x = (int32_t)((int64_t)out.x0 + (int64_t)(i % w)),
                y = (int32_t)((int64_t)out.y0 + (int64_t)(i / w));
        if (ctx->alpha && px[n - 1] <= 0.0f) {
            continue;
        }
        for (c = 0; c < cc; c++) {
            uint32_t ch = p->monochromatic ? 0u : (uint32_t)c;
            float v;
            if (p->distribution == PC_ALGO_DIST_UNIFORM) {
                v = (pc_algo_hash01(x, y, ch, p->seed) - 0.5f) * 2.0f * a;
            } else {
                float u1 = pc_rs_maxf_k(pc_algo_hash01(x, y, ch * 2u + 101u, p->seed), 1e-7f);
                float u2 = pc_algo_hash01(x, y, ch * 2u + 102u, p->seed);
                v = sqrtf(-2.0f * logf(u1)) * cosf(TAU_F * u2) * a * 0.5f;
            }
            /* Integer storage clamps on write; float surfaces keep the value. */
            px[c] += v;
        }
    }
    return PC_OK;
}

/* f32::total_cmp ordering key. */
static int32_t total_key(float v)
{
    int32_t i;
    memcpy(&i, &v, 4);
    return i ^ (int32_t)((uint32_t)(i >> 31) >> 1);
}

static int total_cmp(const void *pa, const void *pb)
{
    int32_t a = total_key(*(const float *)pa), b = total_key(*(const float *)pb);
    return a < b ? -1 : a > b;
}

static bool same_bits(float a, float b)
{
    return memcmp(&a, &b, sizeof a) == 0;
}

static float pick(float o, float m, bool has_t, float t)
{
    return has_t && fabsf(o - m) <= t ? o : m;
}

pc_status pc_algo_median_direct(const pc_algo_image *src, pc_rect out, int32_t r, bool has_t, float t,
                                float *dst, pc_error *err)
{
    size_t n = src->ch, noffs = 0, k = 0, c, q, side = 2 * (size_t)r + 1;
    int32_t (*offs)[2], dx, dy, x, y;
    float *vals;
    if (n == 0) {
        return PC_OK;
    }
    offs = pc_malloc_array(side * side, sizeof *offs);
    vals = pc_malloc_array(side * side, sizeof *vals);
    if (offs == NULL || vals == NULL) {
        free(offs);
        free(vals);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a median");
    }
    for (dy = -r; dy <= r; dy++) {
        for (dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r + r) {
                offs[noffs][0] = dx;
                offs[noffs][1] = dy;
                noffs++;
            }
        }
    }
    for (y = out.y0; y < out.y1; y++) {
        for (x = out.x0; x < out.x1; x++) {
            for (c = 0; c < n; c++) {
                float m, o;
                for (q = 0; q < noffs; q++) {
                    vals[q] = pc_algo_image_get(src, x + offs[q][0], y + offs[q][1], c);
                }
                /* The element at the middle rank in total_cmp order (select_nth_unstable_by). */
                qsort(vals, noffs, sizeof *vals, total_cmp);
                m = vals[noffs / 2];
                o = pc_algo_image_get(src, x, y, c);
                dst[k++] = pick(o, m, has_t, t);
            }
        }
    }
    free(offs);
    free(vals);
    return PC_OK;
}

/* Half-width of the median disc on each row dy in -r..=r: the largest dx with
 * dx^2 + dy^2 <= r^2 + r. */
static void disc_rows(int32_t r, int32_t *rows)
{
    int32_t dy;
    for (dy = -r; dy <= r; dy++) {
        int32_t lim = r * r + r - dy * dy, hw = pc_f64_to_i32(sqrt((double)lim));
        while (hw * hw > lim) {
            hw--;
        }
        while ((hw + 1) * (hw + 1) <= lim) {
            hw++;
        }
        rows[dy + r] = hw;
    }
}

typedef struct rank_pred {
    const float *sorted;
    float v;
} rank_pred;

static bool pred_rank(const void *ctx, size_t i)
{
    const rank_pred *p = ctx;
    return total_key(p->sorted[i]) < total_key(p->v);
}

pc_status pc_algo_median_ranked(const pc_algo_image *src, pc_rect out, int32_t r, bool has_t, float t,
                                float *dst, pc_error *err)
{
    size_t n = src->ch, ww, wh, ow = pc_rect_width(out), oh = pc_rect_height(out), ru = (size_t)r, npx,
           count = 0;
    size_t mid, c, j, k, shift, bs, nblocks, i;
    pc_rect win = pc_rect_inflate(out, r);
    int32_t *rows = NULL;
    uint32_t *plane = NULL, *hist = NULL, *blocks = NULL;
    float *values = NULL, *sorted = NULL, levels8[256];
    pc_status st = PC_OK;
    ww = pc_rect_width(win);
    wh = pc_rect_height(win);
    if (n == 0 || ow == 0 || oh == 0) {
        return PC_OK;
    }
    if (ww != ow + 2 * ru || wh != oh + 2 * ru || !pc_size_mul(ww, wh, &npx) || npx > PC_MAX_ALLOC / 16) {
        return pc_fail(err, PC_ERR_LIMIT, "median window too large");
    }
    rows = pc_malloc_array(2 * ru + 1, sizeof *rows);
    plane = pc_malloc_array(npx, sizeof *plane);
    values = pc_malloc_array(npx, sizeof *values);
    sorted = pc_malloc_array(npx, sizeof *sorted);
    hist = pc_malloc_array(npx > 256 ? npx : 256, sizeof *hist);
    blocks = pc_malloc_array(npx > 256 ? npx : 256, sizeof *blocks);
    if (rows == NULL || plane == NULL || values == NULL || sorted == NULL || hist == NULL || blocks == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for a median");
        goto done;
    }
    disc_rows(r, rows);
    for (j = 0; j <= 2 * ru; j++) {
        count += (size_t)(2 * rows[j] + 1);
    }
    mid = count / 2;
    for (k = 0; k < 256; k++) {
        levels8[k] = (float)k / 255.0f;
    }
    for (c = 0; c < n; c++) {
        const float *levels;
        bool eight_bit = true;
        size_t nl, m = 0, below = 0, oy, step, isq;
        int32_t x, y;
        /* Window samples of this channel; outside the source reads as 0. */
        for (i = 0, y = win.y0; y < win.y1; y++) {
            for (x = win.x0; x < win.x1; x++) {
                values[i++] = pc_algo_image_get(src, x, y, c);
            }
        }
        for (i = 0; i < npx && eight_bit; i++) {
            float kf = roundf(values[i] * 255.0f), back = kf / 255.0f;
            eight_bit = kf >= 0.0f && kf <= 255.0f && same_bits(back, values[i]);
        }
        if (eight_bit) {
            for (i = 0; i < npx; i++) {
                plane[i] = (uint32_t)roundf(values[i] * 255.0f);
            }
            levels = levels8;
            nl = 256;
        } else {
            memcpy(sorted, values, npx * sizeof *sorted);
            qsort(sorted, npx, sizeof *sorted, total_cmp);
            nl = npx > 0 ? 1 : 0;
            for (i = 1; i < npx; i++) {
                if (!same_bits(sorted[i], sorted[nl - 1])) {
                    sorted[nl++] = sorted[i];
                }
            }
            for (i = 0; i < npx; i++) {
                rank_pred rp = {sorted, values[i]};
                plane[i] = (uint32_t)pc_algo_partition_point(nl, pred_rank, &rp);
            }
            levels = sorted;
        }
        /* Block size: about sqrt(nl) bins, a power of two. */
        for (isq = 0; (isq + 1) * (isq + 1) <= nl; isq++) {
        }
        isq = isq > 1 ? isq : 1;
        for (shift = 0; (isq >> shift) != 0; shift++) {
        }
        bs = (size_t)1 << shift;
        nblocks = (nl + bs - 1) / bs;
        memset(hist, 0, nl * sizeof *hist);
        memset(blocks, 0, (nblocks > 0 ? nblocks : 1) * sizeof *blocks);
        /* Disc of output pixel (0, 0), centred on window pixel (r, r). */
        for (j = 0; j <= 2 * ru; j++) {
            size_t hw = (size_t)rows[j];
            for (i = j * ww + ru - hw; i <= j * ww + ru + hw; i++) {
                hist[plane[i]]++;
                blocks[plane[i] >> shift]++;
            }
        }
        /* The disc visits the rows in a serpentine, so every move swaps one end of each disc row
         * or column and the histogram is never rebuilt. */
        for (oy = 0; oy < oh; oy++) {
            for (step = 0; step < ow; step++) {
                size_t ox = oy % 2 == 0 ? step : ow - 1 - step, cx = ox + ru, cy = oy + ru;
                if (oy > 0 || step > 0) {
                    for (j = 0; j <= 2 * ru; j++) {
                        size_t hw = (size_t)rows[j], gone, nw, yrow;
                        if (step == 0) {
                            /* Down: column cx - r + j loses its top, gains a bottom. */
                            size_t xx = cx + j - ru;
                            gone = (cy - 1 - hw) * ww + xx;
                            nw = (cy + hw) * ww + xx;
                        } else if (oy % 2 == 0) {
                            yrow = (cy + j - ru) * ww;
                            gone = yrow + cx - 1 - hw;
                            nw = yrow + cx + hw;
                        } else {
                            yrow = (cy + j - ru) * ww;
                            gone = yrow + cx + 1 + hw;
                            nw = yrow + cx - hw;
                        }
                        gone = plane[gone];
                        nw = plane[nw];
                        hist[gone]--;
                        blocks[gone >> shift]--;
                        hist[nw]++;
                        blocks[nw >> shift]++;
                        below -= gone < m ? 1u : 0u;
                        below += nw < m ? 1u : 0u;
                    }
                }
                while (below > mid) {
                    if ((m & (bs - 1)) == 0 && m > 0 && below - blocks[(m >> shift) - 1] > mid) {
                        below -= blocks[(m >> shift) - 1];
                        m -= bs;
                    } else {
                        m--;
                        below -= hist[m];
                    }
                }
                for (;;) {
                    if ((m & (bs - 1)) == 0 && (m >> shift) < nblocks && below + blocks[m >> shift] <= mid) {
                        below += blocks[m >> shift];
                        m += bs;
                    } else if (m < nl && below + hist[m] <= mid) {
                        below += hist[m];
                        m++;
                    } else {
                        break;
                    }
                }
                {
                    float mv = levels[m < nl ? m : nl - 1];
                    float o = pc_algo_image_get(src, (int32_t)((int64_t)out.x0 + (int64_t)ox),
                                                (int32_t)((int64_t)out.y0 + (int64_t)oy), c);
                    dst[(oy * ow + ox) * n + c] = pick(o, mv, has_t, t);
                }
            }
        }
    }
done:
    free(rows), free(plane), free(values), free(sorted), free(hist), free(blocks);
    return st;
}

pc_status pc_algo_median_k(const pc_algo_image *src, pc_rect out, float radius, bool has_threshold,
                           float threshold, float *dst, pc_error *err)
{
    int32_t r = pc_f32_to_i32(roundf(pc_rs_maxf_k(radius, 0.0f)));
    float t = threshold / 255.0f;
    if (r == 0) {
        pc_algo_image_crop_into(src, out, dst);
        return PC_OK;
    }
    if (r > PC_ALGO_MAX_WINDOW_RADIUS) {
        return pc_fail(err, PC_ERR_LIMIT, "median radius %g is too large", (double)radius);
    }
    if (r >= 2) {
        return pc_algo_median_ranked(src, out, r, has_threshold, t, dst, err);
    }
    return pc_algo_median_direct(src, out, r, has_threshold, t, dst, err);
}
