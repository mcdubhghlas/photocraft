/* Motion and Radial Blur kernels (port of blur.rs: average_samples, motion_offsets, motion,
 * radial_intervals, radial) and the merged motion taps shared with the FFT path. */
#include "algo/blur_int.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* ---- merged taps ---- */

static uint64_t tap_hash(int32_t x, int32_t y)
{
    uint64_t h = ((uint64_t)(uint32_t)x << 32) | (uint32_t)y;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return h;
}

pc_status pc_algo_motion_taps(float angle, float distance, pc_algo_motion_tap **out, size_t *count,
                              pc_error *err)
{
    float d = fabsf(distance), rad = pc_algo_to_radians(angle), s = sinf(rad), c = cosf(rad), k;
    int32_t steps = pc_f32_to_i32(ceilf(d)), i;
    size_t cap, slots, mask, n = 0, j;
    pc_algo_motion_tap *taps;
    int32_t *slot;
    if (steps > PC_ALGO_MAX_MOTION_STEPS) {
        return pc_fail(err, PC_ERR_LIMIT, "Motion Blur distance %g is too large", (double)distance);
    }
    if (steps < 0) {
        steps = 0; /* unreachable for |d| >= 0; keeps the arithmetic below defined */
    }
    k = 1.0f / (float)((size_t)steps + 1);
    cap = 4 * ((size_t)steps + 1);
    for (slots = 16; slots < 2 * cap; slots *= 2) {
    }
    mask = slots - 1;
    taps = pc_malloc_array(cap, sizeof *taps);
    slot = pc_malloc_array(slots, sizeof *slot);
    if (taps == NULL || slot == NULL) {
        free(taps);
        free(slot);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for motion taps");
    }
    memset(slot, 0xff, slots * sizeof *slot); /* -1: empty */
    for (i = 0; i <= steps; i++) {
        float t = (float)i / (float)steps - 0.5f;
        float ox = c * d * t, oy = -s * d * t;
        float fx0 = floorf(ox), fy0 = floorf(oy);
        float ax = ox - fx0, ay = oy - fy0;
        int32_t x0 = pc_f32_to_i32(fx0), y0 = pc_f32_to_i32(fy0);
        const int32_t dx[4] = {0, 1, 0, 1}, dy[4] = {0, 0, 1, 1};
        float w[4];
        int q;
        w[0] = (1.0f - ax) * (1.0f - ay);
        w[1] = ax * (1.0f - ay);
        w[2] = (1.0f - ax) * ay;
        w[3] = ax * ay;
        for (q = 0; q < 4; q++) {
            int32_t tx = pc_i32_wrapping_add(x0, dx[q]), ty = pc_i32_wrapping_add(y0, dy[q]);
            size_t h;
            if (w[q] <= 0.0f) {
                continue;
            }
            /* First-seen order, as the reference's HashMap + Vec merge. */
            for (h = (size_t)tap_hash(tx, ty) & mask;; h = (h + 1) & mask) {
                if (slot[h] < 0) {
                    slot[h] = (int32_t)n;
                    taps[n].x = tx;
                    taps[n].y = ty;
                    taps[n].w = 0.0f;
                    n++;
                    break;
                }
                if (taps[slot[h]].x == tx && taps[slot[h]].y == ty) {
                    break;
                }
            }
            j = (size_t)slot[h];
            taps[j].w += w[q] * k;
        }
    }
    free(slot);
    *out = taps;
    *count = n;
    return PC_OK;
}

/* ---- average_samples ---- */

typedef struct avg_job {
    const pc_algo_image *src;
    pc_rect out;
    const pc_algo_ctx *ctx;
    pc_algo_offsets_fn offsets;
    const void *octx;
    size_t cap;
    float *dst;
} avg_job;

static pc_status avg_rows(void *vctx, size_t chunk, size_t begin, size_t end)
{
    const avg_job *j = vctx;
    size_t n = j->src->ch, ow = pc_rect_width(j->out), r, i, c, np;
    float (*pts)[2] = pc_malloc_array(j->cap + 1, sizeof *pts);
    float tmp[PC_ALGO_MAXC] = {0}, acc[PC_ALGO_MAXC] = {0};
    bool alpha = j->ctx->alpha;
    (void)chunk;
    if (pts == NULL) {
        return PC_ERR_NOMEM;
    }
    if (n == 0 || n > PC_ALGO_MAXC) { /* checked by the caller; keeps the arrays visibly in bounds */
        free(pts);
        return PC_ERR_INVALID;
    }
    for (r = begin; r < end; r++) {
        int32_t y = (int32_t)((int64_t)j->out.y0 + (int64_t)r);
        float *row = j->dst + r * ow * n;
        for (i = 0; i < ow; i++) {
            int32_t x = (int32_t)((int64_t)j->out.x0 + (int64_t)i);
            float cx = (float)x + 0.5f, cy = (float)y + 0.5f, k;
            float *d = row + i * n;
            np = j->offsets(j->octx, cx, cy, pts, j->cap);
            for (c = 0; c < n; c++) {
                acc[c] = 0.0f;
            }
            for (size_t q = 0; q < np; q++) {
                float a;
                pc_algo_image_sample(j->src, pts[q][0], pts[q][1], PC_ALGO_EDGE_TRANSPARENT, j->src->rect,
                                     alpha, tmp, n);
                /* Accumulate premultiplied. */
                a = alpha ? tmp[n - 1] : 1.0f;
                for (c = 0; c < n; c++) {
                    acc[c] += alpha && c < n - 1 ? tmp[c] * a : tmp[c];
                }
            }
            k = 1.0f / (float)(np > 1 ? np : 1);
            for (c = 0; c < n; c++) {
                d[c] = acc[c] * k;
            }
            if (alpha) {
                float al = acc[n - 1] * k;
                for (c = 0; c + 1 < n; c++) {
                    d[c] = al > 1e-7f ? d[c] / al : 0.0f;
                }
            }
        }
    }
    free(pts);
    return PC_OK;
}

pc_status pc_algo_average_samples(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx,
                                  pc_algo_offsets_fn offsets, const void *octx, size_t cap, float *dst,
                                  pc_error *err)
{
    avg_job j = {src, out, ctx, offsets, octx, cap, dst};
    pc_status st;
    if (pc_rect_width(out) == 0 || pc_rect_height(out) == 0 || src->ch == 0) {
        return PC_OK;
    }
    if (src->ch > PC_ALGO_MAXC) {
        return pc_fail(err, PC_ERR_INVALID, "too many channels (%zu)", src->ch);
    }
    st = pc_algo_par_for(pc_rect_height(out), 1, avg_rows, &j);
    if (st != PC_OK) {
        return pc_fail(err, st, "out of memory averaging samples");
    }
    return PC_OK;
}

/* ---- motion ---- */

pc_status pc_algo_motion_offsets(float angle, float distance, float (**pts)[2], size_t *n, pc_error *err)
{
    float d = fabsf(distance), rad = pc_algo_to_radians(angle), s = sinf(rad), c = cosf(rad);
    int32_t steps = pc_f32_to_i32(ceilf(d)), i;
    float (*v)[2];
    if (steps > PC_ALGO_MAX_MOTION_STEPS) {
        return pc_fail(err, PC_ERR_LIMIT, "Motion Blur distance %g is too large", (double)distance);
    }
    if (steps < 0) {
        steps = 0;
    }
    v = pc_malloc_array((size_t)steps + 1, sizeof *v);
    if (v == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for motion offsets");
    }
    for (i = 0; i <= steps; i++) {
        float t = (float)i / (float)steps - 0.5f;
        v[i][0] = c * d * t;
        v[i][1] = -s * d * t;
    }
    *pts = v;
    *n = (size_t)steps + 1;
    return PC_OK;
}

pc_status pc_algo_motion_k(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx, float angle,
                           float distance, float *dst, pc_error *err)
{
    size_t n = src->ch, ow = pc_rect_width(out), oh = pc_rect_height(out), ntaps, i, ww, wn, oy, q, c;
    pc_algo_motion_tap *taps;
    int32_t reach = 0, x, y;
    pc_rect win;
    float *pw;
    pc_status st;
    if (fabsf(distance) < 0.5f) {
        pc_algo_image_crop_into(src, out, dst);
        return PC_OK;
    }
    if (n == 0 || ow == 0 || oh == 0) {
        return PC_OK;
    }
    st = pc_algo_motion_taps(angle, distance, &taps, &ntaps, err);
    if (st != PC_OK) {
        return st;
    }
    for (i = 0; i < ntaps; i++) {
        int32_t ax = taps[i].x < 0 ? -taps[i].x : taps[i].x, ay = taps[i].y < 0 ? -taps[i].y : taps[i].y;
        int32_t m = ax > ay ? ax : ay;
        reach = m > reach ? m : reach;
    }
    win = pc_rect_inflate(out, reach);
    ww = pc_rect_width(win);
    /* The window must hold every tap's reads: a saturated inflate (coordinates near i32 limits)
     * would not, where the reference panics on the slice. */
    if (ww != ow + 2 * (size_t)reach || pc_rect_height(win) != oh + 2 * (size_t)reach) {
        free(taps);
        return pc_fail(err, PC_ERR_LIMIT, "motion blur window outside the coordinate range");
    }
    if (!pc_algo_float_count(win, n, &wn)) {
        free(taps);
        return pc_fail(err, PC_ERR_LIMIT, "motion blur window too large");
    }
    pw = pc_malloc_array(wn > 0 ? wn : 1, sizeof(float));
    if (pw == NULL) {
        free(taps);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a motion blur window");
    }
    /* Premultiplied window; outside the source reads as transparent, as the sampler does. */
    for (y = win.y0; y < win.y1; y++) {
        for (x = win.x0; x < win.x1; x++) {
            size_t k = ((size_t)((int64_t)y - win.y0) * ww + (size_t)((int64_t)x - win.x0)) * n;
            float a = ctx->alpha ? pc_algo_image_get(src, x, y, n - 1) : 1.0f;
            for (c = 0; c < n; c++) {
                float v = pc_algo_image_get(src, x, y, c);
                pw[k + c] = ctx->alpha && c < n - 1 ? v * a : v;
            }
        }
    }
    for (oy = 0; oy < oh; oy++) {
        float *row = dst + oy * ow * n;
        for (q = 0; q < ntaps; q++) {
            size_t start =
                ((size_t)((int64_t)oy + reach + taps[q].y) * ww + (size_t)((int64_t)reach + taps[q].x)) * n;
            const float *p = pw + start;
            float w = taps[q].w;
            for (i = 0; i < ow * n; i++) {
                row[i] += w * p[i];
            }
        }
        if (ctx->alpha) {
            for (i = 0; i < ow; i++) {
                float *px = row + i * n, a = px[n - 1];
                for (c = 0; c + 1 < n; c++) {
                    px[c] = a > 1e-7f ? px[c] / a : 0.0f;
                }
            }
        }
    }
    free(pw);
    free(taps);
    return PC_OK;
}

/* ---- radial ---- */

size_t pc_algo_radial_intervals(float path_length, pc_algo_radial_quality quality)
{
    float max = quality == PC_ALGO_RADIAL_DRAFT ? 64.0f : quality == PC_ALGO_RADIAL_BEST ? 4096.0f : 256.0f;
    if (isnan(path_length)) {
        return 1;
    }
    return pc_f32_to_size(pc_clampf(ceilf(path_length), 1.0f, max));
}

typedef struct radial_ctx {
    float cx, cy, amount;
    pc_algo_radial_method method;
    pc_algo_radial_quality quality;
} radial_ctx;

static size_t radial_offsets(const void *octx, float x, float y, float (*pts)[2], size_t cap)
{
    const radial_ctx *r = octx;
    float dx = x - r->cx, dy = y - r->cy, rr = sqrtf(dx * dx + dy * dy);
    size_t n, i, k = 0;
    if (r->method == PC_ALGO_RADIAL_SPIN) {
        /* Arc of `amount` degrees centred on the pixel. */
        float arc = pc_algo_to_radians(r->amount);
        n = pc_algo_radial_intervals(arc * rr, r->quality);
        for (i = 0; i <= n && k < cap; i++) {
            float t = ((float)i / (float)n - 0.5f) * arc, s = sinf(t), c = cosf(t);
            pts[k][0] = r->cx + dx * c - dy * s;
            pts[k][1] = r->cy + dx * s + dy * c;
            k++;
        }
    } else {
        /* Samples along the ray, up to amount/2 % closer to the centre. */
        float span = r->amount / 200.0f;
        n = pc_algo_radial_intervals(span * rr, r->quality);
        for (i = 0; i <= n && k < cap; i++) {
            float kk = 1.0f - span * (float)i / (float)n;
            pts[k][0] = r->cx + dx * kk;
            pts[k][1] = r->cy + dy * kk;
            k++;
        }
    }
    return k;
}

pc_status pc_algo_radial_k(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx,
                           const pc_algo_radial_params *p, float *dst, pc_error *err)
{
    pc_rect b = ctx->bounds;
    radial_ctx r;
    r.cx = (float)b.x0 + (float)pc_rect_width(b) * p->center_x;
    r.cy = (float)b.y0 + (float)pc_rect_height(b) * p->center_y;
    r.amount = pc_clampf(p->amount, 0.0f, 100.0f);
    r.method = p->method;
    r.quality = p->quality;
    return pc_algo_average_samples(src, out, ctx, radial_offsets, &r, 4097, dst, err);
}
