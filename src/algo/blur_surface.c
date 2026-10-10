/* Surface Blur (port of blur.rs surface, surface_8bit, surface_ranked, surface_direct): the mean of
 * the (2r + 1)^2 window weighted by 1 - |v - v0| / t, computed by a sliding 256-level histogram
 * for 8-bit samples, by a sliding Fenwick tree of value ranks for wide windows at any depth, or
 * directly. All three give the reference's numbers (the oracle tests hold them to the direct
 * sum). */
#include "algo/blur_int.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* Radius from which the ranked sums beat the direct sum (measured by the reference). */
#define RANKED_MIN_RADIUS 10

/* The centre sample of an output pixel (the reference indexes src.px, which panics outside src;
 * C reads 0 there). */
static float centre(const pc_algo_image *src, int32_t x, int32_t y, size_t c)
{
    const float *p = pc_algo_image_px(src, x, y);
    return p != NULL ? p[c] : 0.0f;
}

pc_status pc_algo_surface_8bit(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx, int32_t r,
                               float t, size_t reach, float *dst, bool *declined, pc_error *err)
{
    enum { SKIP = 256 };
    size_t n = src->ch, cc = pc_algo_ncol(ctx, n), ww, wh, ow = pc_rect_width(out), oh = pc_rect_height(out);
    size_t side = 2 * (size_t)r + 1, oy, ox, c, wx, wy, nl, k;
    pc_rect win = pc_rect_inflate(out, r);
    uint16_t *lv;
    uint32_t hist[257];
    int32_t x, y;
    *declined = false;
    ww = pc_rect_width(win);
    wh = pc_rect_height(win);
    if (n == 0 || ow == 0 || oh == 0) {
        return PC_OK;
    }
    if (ww != ow + side - 1 || wh != oh + side - 1 || !pc_size_mul3(ww, wh, cc, &nl) ||
        nl > PC_MAX_ALLOC / 2) {
        return pc_fail(err, PC_ERR_LIMIT, "surface blur window too large");
    }
    lv = pc_malloc_array(nl > 0 ? nl : 1, sizeof *lv);
    if (lv == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a surface blur");
    }
    /* Level of every colour sample in the window; SKIP where the pixel is transparent. */
    for (y = win.y0; y < win.y1; y++) {
        for (x = win.x0; x < win.x1; x++) {
            size_t i = ((size_t)((int64_t)y - win.y0) * ww + (size_t)((int64_t)x - win.x0)) * cc;
            bool skip = ctx->alpha && pc_algo_image_get(src, x, y, n - 1) <= 0.0f;
            for (c = 0; c < cc; c++) {
                float v = pc_algo_image_get(src, x, y, c), kf = roundf(v * 255.0f), back = kf / 255.0f;
                if (!(kf >= 0.0f && kf <= 255.0f) || memcmp(&back, &v, sizeof v) != 0) {
                    free(lv);
                    *declined = true;
                    return PC_OK;
                }
                lv[i + c] = skip ? (uint16_t)SKIP : (uint16_t)kf;
            }
        }
    }
    for (oy = 0; oy < oh; oy++) {
        for (c = 0; c < cc; c++) {
            memset(hist, 0, sizeof hist);
            for (wx = 0; wx < side; wx++) {
                for (wy = oy; wy < oy + side; wy++) {
                    hist[lv[(wy * ww + wx) * cc + c]]++;
                }
            }
            for (ox = 0; ox < ow; ox++) {
                float v0 = centre(src, (int32_t)((int64_t)out.x0 + (int64_t)ox),
                                  (int32_t)((int64_t)out.y0 + (int64_t)oy), c);
                size_t k0 = pc_f32_to_size(roundf(v0 * 255.0f)), lo = k0 > reach ? k0 - reach : 0;
                size_t hi = k0 + reach + 1 < 256 ? k0 + reach + 1 : 256;
                double acc = 0.0, wsum = 0.0;
                for (k = lo; k < hi; k++) {
                    float v, w;
                    if (hist[k] == 0) {
                        continue;
                    }
                    v = (float)k / 255.0f;
                    w = pc_rs_maxf_k(1.0f - fabsf(v - v0) / t, 0.0f);
                    acc += (double)(v * w) * (double)hist[k];
                    wsum += (double)w * (double)hist[k];
                }
                dst[(oy * ow + ox) * n + c] = wsum > 0.0 ? (float)(acc / wsum) : v0;
                if (ox + 1 < ow) {
                    for (wy = oy; wy < oy + side; wy++) {
                        hist[lv[(wy * ww + ox) * cc + c]]--;
                        hist[lv[(wy * ww + ox + side) * cc + c]]++;
                    }
                }
            }
        }
        if (ctx->alpha) {
            for (ox = 0; ox < ow; ox++) {
                dst[(oy * ow + ox) * n + n - 1] = centre(src, (int32_t)((int64_t)out.x0 + (int64_t)ox),
                                                         (int32_t)((int64_t)out.y0 + (int64_t)oy), n - 1);
            }
        }
    }
    free(lv);
    return PC_OK;
}

/* ---- ranked sums ---- */

/* Fenwick tree over value ranks, each node holding (count, sum v, sum v^2) of its range. */
typedef struct rank_sums {
    double (*node)[3];
    size_t len;
} rank_sums;

static void rs_add(rank_sums *t, size_t rank, double sign, double v)
{
    size_t i = rank + 1;
    while (i < t->len) {
        t->node[i][0] += sign;
        t->node[i][1] += sign * v;
        t->node[i][2] += sign * v * v;
        i += i & (0 - i);
    }
}

static void rs_prefix(const rank_sums *t, size_t end, double acc[3])
{
    size_t i = end < (t->len > 0 ? t->len - 1 : 0) ? end : (t->len > 0 ? t->len - 1 : 0);
    acc[0] = acc[1] = acc[2] = 0.0;
    while (i > 0) {
        acc[0] += t->node[i][0];
        acc[1] += t->node[i][1];
        acc[2] += t->node[i][2];
        i -= i & (0 - i);
    }
}

static void rs_range(const rank_sums *t, size_t start, size_t end, double out[3])
{
    double a[3], b[3];
    rs_prefix(t, start, a);
    rs_prefix(t, end, b);
    out[0] = b[0] - a[0];
    out[1] = b[1] - a[1];
    out[2] = b[2] - a[2];
}

/* f32::total_cmp. */
static int total_cmp(const void *pa, const void *pb)
{
    float a = *(const float *)pa, b = *(const float *)pb;
    int32_t ia, ib;
    memcpy(&ia, &a, 4);
    memcpy(&ib, &b, 4);
    ia ^= (int32_t)((uint32_t)(ia >> 31) >> 1);
    ib ^= (int32_t)((uint32_t)(ib >> 31) >> 1);
    return ia < ib ? -1 : ia > ib;
}

typedef struct lv_pred {
    const float *levels;
    float v0, t;
} lv_pred;

static bool pred_less(const void *ctx, size_t i)
{
    const lv_pred *p = ctx;
    return p->levels[i] < p->v0;
}
static bool pred_lo(const void *ctx, size_t i)
{
    const lv_pred *p = ctx;
    float l = p->levels[i];
    return 1.0f - fabsf(l - p->v0) / p->t <= 0.0f && l < p->v0;
}
static bool pred_mid(const void *ctx, size_t i)
{
    const lv_pred *p = ctx;
    return p->levels[i] <= p->v0;
}
static bool pred_hi(const void *ctx, size_t i)
{
    const lv_pred *p = ctx;
    float l = p->levels[i];
    return l <= p->v0 || 1.0f - fabsf(l - p->v0) / p->t > 0.0f;
}

typedef struct ranked_win {
    size_t ww, cc, side;
    const bool *opaque;
    const size_t *rank;
    const float *vals;
} ranked_win;

static void rs_column(rank_sums *tree, const ranked_win *w, size_t oy, size_t c, size_t wx, double sign)
{
    size_t wy;
    for (wy = oy; wy < oy + w->side; wy++) {
        size_t p = wy * w->ww + wx;
        if (w->opaque[p]) {
            rs_add(tree, w->rank[p * w->cc + c], sign, (double)w->vals[p * w->cc + c]);
        }
    }
}

pc_status pc_algo_surface_ranked(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx, int32_t r,
                                 float t, float *dst, bool *declined, pc_error *err)
{
    size_t n = src->ch, cc = pc_algo_ncol(ctx, n), ww, wh, ow = pc_rect_width(out), oh = pc_rect_height(out);
    size_t side = 2 * (size_t)r + 1, npx, nv, nlev = 0, i, oy, ox, c, wx;
    pc_rect win = pc_rect_inflate(out, r);
    float *vals = NULL, *levels = NULL;
    bool *opaque = NULL;
    size_t *rank = NULL;
    rank_sums tree = {NULL, 0};
    double t64 = (double)t;
    ranked_win rw;
    pc_status st = PC_OK;
    int32_t x, y;
    *declined = false;
    ww = pc_rect_width(win);
    wh = pc_rect_height(win);
    if (n == 0) {
        return PC_OK;
    }
    if (ww != ow + side - 1 || wh != oh + side - 1 || !pc_size_mul(ww, wh, &npx) ||
        !pc_size_mul(npx, cc, &nv) || nv > PC_MAX_ALLOC / 16) {
        return pc_fail(err, PC_ERR_LIMIT, "surface blur window too large");
    }
    vals = pc_malloc_array(nv + 1, sizeof *vals);
    levels = pc_malloc_array(nv + 1, sizeof *levels);
    opaque = pc_malloc_array(npx + 1, sizeof *opaque);
    rank = pc_malloc_array(nv + 1, sizeof *rank);
    if (vals == NULL || levels == NULL || opaque == NULL || rank == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for a surface blur");
        goto done;
    }
    /* Every colour sample of the window and whether its pixel counts. */
    i = 0;
    for (y = win.y0; y < win.y1; y++) {
        for (x = win.x0; x < win.x1; x++) {
            opaque[(size_t)((int64_t)y - win.y0) * ww + (size_t)((int64_t)x - win.x0)] =
                !(ctx->alpha && pc_algo_image_get(src, x, y, n - 1) <= 0.0f);
            for (c = 0; c < cc; c++) {
                float v = pc_algo_image_get(src, x, y, c);
                if (!isfinite(v)) {
                    *declined = true;
                    goto done;
                }
                /* + 0.0 turns -0.0 into 0.0, so equal values share a rank. */
                vals[i++] = v + 0.0f;
            }
        }
    }
    if (nv > 0) {
        memcpy(levels, vals, nv * sizeof *levels);
        qsort(levels, nv, sizeof *levels, total_cmp);
        nlev = 1;
        for (i = 1; i < nv; i++) {
            if (levels[i] != levels[nlev - 1]) {
                levels[nlev++] = levels[i];
            }
        }
    }
    for (i = 0; i < nv; i++) {
        lv_pred p = {levels, vals[i], t};
        rank[i] = pc_algo_partition_point(nlev, pred_less, &p);
    }
    tree.len = nlev + 1;
    tree.node = pc_calloc_array(tree.len, sizeof *tree.node);
    if (tree.node == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for a surface blur");
        goto done;
    }
    if (ow == 0 || oh == 0) {
        goto done;
    }
    rw.ww = ww;
    rw.cc = cc;
    rw.side = side;
    rw.opaque = opaque;
    rw.rank = rank;
    rw.vals = vals;
    for (oy = 0; oy < oh; oy++) {
        for (c = 0; c < cc; c++) {
            for (wx = 0; wx < side; wx++) {
                rs_column(&tree, &rw, oy, c, wx, 1.0);
            }
            for (ox = 0; ox < ow; ox++) {
                float v0 = centre(src, (int32_t)((int64_t)out.x0 + (int64_t)ox),
                                  (int32_t)((int64_t)out.y0 + (int64_t)oy), c);
                lv_pred p = {levels, v0, t};
                size_t lo = pc_algo_partition_point(nlev, pred_lo, &p);
                size_t mid = pc_algo_partition_point(nlev, pred_mid, &p);
                size_t hi = pc_algo_partition_point(nlev, pred_hi, &p);
                double below[3], above[3], v0d = (double)v0, wsum, acc;
                rs_range(&tree, lo, mid, below);
                rs_range(&tree, mid, hi, above);
                wsum = ((t64 - v0d) * below[0] + below[1] + (t64 + v0d) * above[0] - above[1]) / t64;
                acc = ((t64 - v0d) * below[1] + below[2] + (t64 + v0d) * above[1] - above[2]) / t64;
                dst[(oy * ow + ox) * n + c] =
                    below[0] + above[0] > 0.0 && wsum > 0.0 ? (float)(acc / wsum) : v0;
                if (ox + 1 < ow) {
                    rs_column(&tree, &rw, oy, c, ox, -1.0);
                    rs_column(&tree, &rw, oy, c, ox + side, 1.0);
                }
            }
            /* Empty the tree for the next row and channel. */
            for (wx = ow - 1; wx < ow - 1 + side; wx++) {
                rs_column(&tree, &rw, oy, c, wx, -1.0);
            }
        }
        if (ctx->alpha) {
            for (ox = 0; ox < ow; ox++) {
                dst[(oy * ow + ox) * n + n - 1] = centre(src, (int32_t)((int64_t)out.x0 + (int64_t)ox),
                                                         (int32_t)((int64_t)out.y0 + (int64_t)oy), n - 1);
            }
        }
    }
done:
    free(vals), free(levels), free(opaque), free(rank), free(tree.node);
    return st;
}

pc_status pc_algo_surface_direct(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx, int32_t r,
                                 float t, float *dst, pc_error *err)
{
    size_t n = src->ch, cc = pc_algo_ncol(ctx, n), k = 0, c;
    int32_t x, y, xx, yy;
    (void)err;
    if (n == 0) {
        return PC_OK;
    }
    for (y = out.y0; y < out.y1; y++) {
        for (x = out.x0; x < out.x1; x++) {
            for (c = 0; c < cc; c++) {
                float v0 = centre(src, x, y, c), acc = 0.0f, wsum = 0.0f;
                for (yy = pc_i32_wrapping_sub(y, r); yy <= pc_i32_wrapping_add(y, r); yy++) {
                    for (xx = pc_i32_wrapping_sub(x, r); xx <= pc_i32_wrapping_add(x, r); xx++) {
                        float v, w;
                        if (ctx->alpha && pc_algo_image_get(src, xx, yy, n - 1) <= 0.0f) {
                            if (xx == INT32_MAX) {
                                break;
                            }
                            continue;
                        }
                        v = pc_algo_image_get(src, xx, yy, c);
                        w = pc_rs_maxf_k(1.0f - fabsf(v - v0) / t, 0.0f);
                        acc += v * w;
                        wsum += w;
                        if (xx == INT32_MAX) {
                            break;
                        }
                    }
                    if (yy == INT32_MAX) {
                        break;
                    }
                }
                dst[k++] = wsum > 0.0f ? acc / wsum : v0;
            }
            if (ctx->alpha) {
                dst[k++] = centre(src, x, y, n - 1);
            }
        }
    }
    return PC_OK;
}

pc_status pc_algo_surface_blur_k(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx, float radius,
                                 float threshold, float *dst, pc_error *err)
{
    int32_t r = pc_f32_to_i32(roundf(pc_rs_maxf_k(radius, 0.0f)));
    float t = (pc_rs_maxf_k(threshold, 1.0f) / 255.0f) * 2.5f;
    size_t reach = pc_f32_to_size(ceilf(t * 255.0f)), side;
    bool declined;
    pc_status st;
    if (reach > 255) {
        reach = 255;
    }
    if (r > PC_ALGO_MAX_WINDOW_RADIUS) {
        return pc_fail(err, PC_ERR_LIMIT, "Surface Blur radius %g is too large", (double)radius);
    }
    side = 2 * (size_t)r + 1;
    if (side * side > 2 * reach + 1 + 4 * side) {
        st = pc_algo_surface_8bit(src, out, ctx, r, t, reach, dst, &declined, err);
        if (st != PC_OK || !declined) {
            return st;
        }
    }
    /* 16-bit and float (and 8-bit where the histogram declined). */
    if (r >= RANKED_MIN_RADIUS) {
        st = pc_algo_surface_ranked(src, out, ctx, r, t, dst, &declined, err);
        if (st != PC_OK || !declined) {
            return st;
        }
    }
    return pc_algo_surface_direct(src, out, ctx, r, t, dst, err);
}
