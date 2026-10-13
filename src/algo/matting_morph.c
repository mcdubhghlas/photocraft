/* Exact flat octagonal morphology (port of crates/algo/src/matting/morphology.rs).
 *
 * r alternating 3x3 square / cross steps equal one square of radius ceil(r/2) followed by a
 * Manhattan ball of radius floor(r/2). The square is a separable running extremum; the ball is
 * built by doubling (a radius-2d ball is the union of five radius-d balls, centred here and d
 * pixels away along each axis, centres clamped at the canvas edge). O(w h log r) instead of
 * O(w h r). Masks holding NaNs or negative zeros, and radii up to 2, keep the original step by
 * step order (it decides which NaN payload / zero sign survives). The reference also tiles large
 * masks to bound scratch memory; that path computes the same values, so it is not ported. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "algo/matting.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* Cross steps over at least this many samples run on the global pool. */
#define PAR_MIN 262144

static pc_status count2(size_t w, size_t h, size_t *n, pc_error *err)
{
    if (!pc_size_mul(w, h, n) || *n > PC_MAX_ALLOC / sizeof(float)) {
        return pc_fail(err, PC_ERR_LIMIT, "mask too large");
    }
    return PC_OK;
}

pc_status pc_algo_matting_morph_repeated(const float *src, size_t w, size_t h, size_t r, bool grow,
                                         float **out_p, pc_error *err)
{
    size_t n, x, y, k, cap;
    float *cur, *nxt;
    pc_status st = count2(w, h, &n, err);
    if (st != PC_OK) {
        return st;
    }
    cur = pc_calloc_array(n ? n : 1, sizeof(float));
    nxt = pc_calloc_array(n ? n : 1, sizeof(float));
    if (cur == NULL || nxt == NULL) {
        free(cur);
        free(nxt);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a morphology pass");
    }
    if (n > 0) {
        memcpy(cur, src, n * sizeof(float));
        memcpy(nxt, src, n * sizeof(float));
    }
    /* After 2(w + h) steps every pixel holds the image's extremum, so more steps change nothing
     * (the reference would just keep looping). */
    cap = 2 * (w + h) + 2;
    if (r > cap) {
        r = cap + (r - cap) % 2;
    }
    for (k = 0; k < r; k++) {
        bool square = k % 2 == 0;
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                float v = cur[y * w + x];
                int dy, dx;
                for (dy = -1; dy <= 1; dy++) {
                    for (dx = -1; dx <= 1; dx++) {
                        int64_t nx = (int64_t)x + dx, ny = (int64_t)y + dy;
                        if ((dx != 0 && dy != 0 && !square) || (dx == 0 && dy == 0)) {
                            continue;
                        }
                        if (nx >= 0 && ny >= 0 && (size_t)nx < w && (size_t)ny < h) {
                            float o = cur[(size_t)ny * w + (size_t)nx];
                            v = grow ? pc_rs_maxf(v, o) : pc_rs_minf(v, o);
                        }
                    }
                }
                nxt[y * w + x] = v;
            }
        }
        {
            float *t = cur;
            cur = nxt;
            nxt = t;
        }
    }
    free(nxt);
    *out_p = cur;
    return PC_OK;
}

typedef struct cross_ctx {
    const float *src;
    float *dst;
    size_t w, h, d;
    bool grow;
} cross_ctx;

static pc_status cross_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const cross_ctx *c = ctx;
    size_t w = c->w, d = c->d, y, x;
    (void)chunk;
    for (y = begin; y < end; y++) {
        const float *a = c->src + y * w;
        const float *up = c->src + (y > d ? y - d : 0) * w;
        const float *down = c->src + (c->h - 1 - y > d ? y + d : c->h - 1) * w;
        float *o = c->dst + y * w;
        for (x = 0; x < w; x++) {
            size_t l = x > d ? x - d : 0, r = w - 1 - x > d ? x + d : w - 1;
            float v;
            if (c->grow) {
                v = pc_rs_maxf(pc_rs_maxf(pc_rs_maxf(pc_rs_maxf(a[x], a[l]), a[r]), up[x]), down[x]);
            } else {
                v = pc_rs_minf(pc_rs_minf(pc_rs_minf(pc_rs_minf(a[x], a[l]), a[r]), up[x]), down[x]);
            }
            o[x] = v;
        }
    }
    return PC_OK;
}

static void cross(const float *src, float *dst, size_t w, size_t h, size_t d, bool grow)
{
    cross_ctx c = {src, dst, w, h, d, grow};
    if (w * h >= PAR_MIN) {
        (void)pc_algo_par_for(h, 1, cross_rows, &c);
    } else {
        (void)cross_rows(&c, 0, 0, h);
    }
}

pc_status pc_algo_matting_morph(const float *src, size_t w, size_t h, size_t r, bool grow, float **out_p,
                                pc_error *err)
{
    size_t n, i, extent, square, diamond, built = 0, sum;
    int bit, top;
    float *cur = NULL, *next;
    pc_status st = count2(w, h, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (n == 0 || r == 0) {
        cur = pc_malloc_array(n ? n : 1, sizeof(float));
        if (cur == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for a morphology pass");
        }
        if (n > 0) {
            memcpy(cur, src, n * sizeof(float));
        }
        *out_p = cur;
        return PC_OK;
    }
    if (r <= 2) {
        return pc_algo_matting_morph_repeated(src, w, h, r, grow, out_p, err);
    }
    for (i = 0; i < n; i++) {
        if (isnan(src[i]) || (src[i] == 0.0f && signbit(src[i]))) {
            return pc_algo_matting_morph_repeated(src, w, h, r, grow, out_p, err);
        }
    }
    /* ceil(r/2) unit squares plus floor(r/2) unit crosses, saturated at the image extent:
     * more growth or erosion cannot reach another pixel. */
    extent = (w > h ? w : h) - 1;
    square = r / 2 + r % 2;
    square = square < extent ? square : extent;
    sum = w > SIZE_MAX - h ? SIZE_MAX : w + h;
    diamond = r / 2 < sum - 2 ? r / 2 : sum - 2;
    st = pc_algo_matting_max_square(src, w, h, square, grow, &cur, err);
    if (st != PC_OK || diamond == 0) {
        if (st == PC_OK) {
            *out_p = cur;
        }
        return st;
    }
    next = pc_malloc_array(n, sizeof(float));
    if (next == NULL) {
        free(cur);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a morphology pass");
    }
    for (top = 0; ((size_t)1 << top) <= diamond && top < (int)(sizeof(size_t) * 8 - 1); top++) {
    }
    for (bit = top - 1; bit >= 0; bit--) {
        float *t;
        if (built > 0) {
            cross(cur, next, w, h, built, grow);
            t = cur;
            cur = next;
            next = t;
            built *= 2;
        }
        if ((diamond >> bit) & 1u) {
            cross(cur, next, w, h, 1, grow);
            t = cur;
            cur = next;
            next = t;
            built += 1;
        }
    }
    free(next);
    *out_p = cur;
    return PC_OK;
}
