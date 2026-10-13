/* The Remove Tool (port of crates/algo/src/remove.rs); see remove.h. */
#include "algo/remove.h"

#include <stdlib.h>
#include <string.h>

#include "algo/nonlocal.h"
#include "algo/poisson.h"
#include "base/mem.h"
#include "base/num.h"

/* Holes covering less than this share of their bounding box are completed tile by tile. */
#define SPARSE 0.25
/* Smallest tile side, in pixels. */
#define MIN_TILE ((size_t)256)

static pc_status copy_bools(const bool *m, size_t len, bool **out, pc_error *err)
{
    bool *o = pc_malloc_array(len > 0 ? len : 1, sizeof *o);
    if (o == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (len > 0) {
        memcpy(o, m, len * sizeof *o);
    }
    *out = o;
    return PC_OK;
}

static bool size_is(size_t w, size_t h, size_t len)
{
    size_t n;
    return pc_size_mul(w, h, &n) && n == len;
}

pc_status pc_algo_remove_close_loops(size_t w, size_t h, const bool *mask, size_t len, bool **out, pc_error *err)
{
    bool *outside;
    size_t *stack, top = 0, x, y, i;
    if (len > 0 && mask == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "missing mask");
    }
    if (w == 0 || h == 0 || !size_is(w, h, len)) {
        return copy_bools(mask, len, out, err);
    }
    outside = pc_calloc_array(len, sizeof *outside);
    stack = pc_malloc_array(len, sizeof *stack);
    if (outside == NULL || stack == NULL) {
        free(outside);
        free(stack);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
#define VISIT(k)                                                                                             \
    do {                                                                                                     \
        size_t k_ = (k);                                                                                     \
        if (!mask[k_] && !outside[k_]) {                                                                     \
            outside[k_] = true;                                                                              \
            stack[top++] = k_;                                                                               \
        }                                                                                                    \
    } while (0)
    for (x = 0; x < w; x++) {
        VISIT(x);
        VISIT((h - 1) * w + x);
    }
    for (y = 0; y < h; y++) {
        VISIT(y * w);
        VISIT(y * w + w - 1);
    }
    while (top > 0) {
        i = stack[--top];
        x = i % w, y = i / w;
        if (x > 0) {
            VISIT(i - 1);
        }
        if (x + 1 < w) {
            VISIT(i + 1);
        }
        if (y > 0) {
            VISIT(i - w);
        }
        if (y + 1 < h) {
            VISIT(i + w);
        }
    }
#undef VISIT
    for (i = 0; i < len; i++) {
        outside[i] = !outside[i];
    }
    free(stack);
    *out = outside;
    return PC_OK;
}

/* One dilation pass: each cell is set when any cell within r along one axis is. */
static void dilate_pass(size_t w, size_t h, const bool *src, bool *dst, size_t r, bool horizontal)
{
    size_t lines = horizontal ? h : w, len = horizontal ? w : h, line, k;
#define AT(l, kk) (horizontal ? (l) * w + (kk) : (kk) * w + (l))
    for (line = 0; line < lines; line++) {
        size_t count = 0, lim = len < r ? len : r;
        for (k = 0; k < lim; k++) {
            count += src[AT(line, k)] ? 1u : 0u;
        }
        for (k = 0; k < len; k++) {
            if (k + r < len) {
                count += src[AT(line, k + r)] ? 1u : 0u;
            }
            if (k > r) {
                count -= src[AT(line, k - r - 1)] ? 1u : 0u;
            }
            dst[AT(line, k)] = count > 0;
        }
    }
#undef AT
}

pc_status pc_algo_remove_dilate(size_t w, size_t h, const bool *mask, size_t len, size_t r, bool **out,
                                pc_error *err)
{
    bool *tmp, *o;
    if (len > 0 && mask == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "missing mask");
    }
    if (r == 0 || !size_is(w, h, len)) {
        return copy_bools(mask, len, out, err);
    }
    tmp = pc_malloc_array(len > 0 ? len : 1, sizeof *tmp);
    o = pc_malloc_array(len > 0 ? len : 1, sizeof *o);
    if (tmp == NULL || o == NULL) {
        free(tmp);
        free(o);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    dilate_pass(w, h, mask, tmp, r, true);
    dilate_pass(w, h, tmp, o, r, false);
    free(tmp);
    *out = o;
    return PC_OK;
}

pc_status pc_algo_remove_hole_radius(size_t w, size_t h, const bool *hole, size_t len, uint32_t *out, pc_error *err)
{
    uint32_t *d, best = 0;
    size_t x, y, i;
    if (!size_is(w, h, len) || (len > 0 && hole == NULL)) {
        *out = 0;
        return PC_OK;
    }
    d = pc_malloc_array(len > 0 ? len : 1, sizeof *d);
    if (d == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < len; i++) {
        d[i] = hole[i] ? UINT32_MAX : 0;
    }
#define RELAX(a, b)                                                                                          \
    do {                                                                                                     \
        uint32_t v_ = d[b] == UINT32_MAX ? UINT32_MAX : d[b] + 1;                                            \
        if (v_ < d[a]) {                                                                                     \
            d[a] = v_;                                                                                       \
        }                                                                                                    \
    } while (0)
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            i = y * w + x;
            if (x > 0) {
                RELAX(i, i - 1);
            }
            if (y > 0) {
                RELAX(i, i - w);
                if (x > 0) {
                    RELAX(i, i - w - 1);
                }
                if (x + 1 < w) {
                    RELAX(i, i - w + 1);
                }
            }
        }
    }
    for (y = h; y-- > 0;) {
        for (x = w; x-- > 0;) {
            i = y * w + x;
            if (x + 1 < w) {
                RELAX(i, i + 1);
            }
            if (y + 1 < h) {
                RELAX(i, i + w);
                if (x + 1 < w) {
                    RELAX(i, i + w + 1);
                }
                if (x > 0) {
                    RELAX(i, i + w - 1);
                }
            }
        }
    }
#undef RELAX
    for (i = 0; i < len; i++) {
        if (hole[i] && d[i] > best) {
            best = d[i];
        }
    }
    free(d);
    *out = best;
    return PC_OK;
}

/* Complete one hole, or diffuse it when there is no fully-known patch to copy from. */
static pc_status fill_one(size_t w, size_t h, size_t ch, const float *img, const bool *hole, uint64_t seed,
                          const pc_interrupt *ctl, float **out, pc_error *err)
{
    pc_algo_nonlocal_params p = pc_algo_nonlocal_params_default();
    size_t n = w * h;
    float *o = NULL;
    pc_status st;
    p.seed = seed;
    st = pc_algo_nonlocal_complete(w, h, ch, img, n * ch, hole, n, &p, ctl, &o, err);
    if (st != PC_OK) {
        return st;
    }
    if (o == NULL) {
        return pc_algo_poisson_membrane_fill(w, h, ch, img, n * ch, hole, n, ctl, out, err);
    }
    *out = o;
    return PC_OK;
}

/* A tile's interrupt: the caller's cancel, progress scaled into the tile's share. */
typedef struct tile_ctl {
    const pc_interrupt *outer;
    float k, count;
} tile_ctl;

static bool tile_cancel(void *ctx)
{
    return pc_interrupt_cancelled(((const tile_ctl *)ctx)->outer);
}

static void tile_progress(void *ctx, float f)
{
    const tile_ctl *t = ctx;
    pc_interrupt_progress(t->outer, (t->k + f) / t->count);
}

pc_status pc_algo_remove(size_t w, size_t h, size_t ch, const float *img, size_t len, const bool *hole,
                         size_t hole_len, uint64_t seed, const pc_interrupt *ctl, float **out, pc_error *err)
{
    size_t n, nch, bx0, by0, bx1 = 0, by1 = 0, count = 0, i, tile, margin, area, thick, tx0, ty0, norig, k;
    uint32_t radius;
    float *o = NULL, *sub = NULL, *filled = NULL;
    bool *left = NULL, *subh = NULL;
    pc_status st;
    if (len > 0 && img == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "missing buffer");
    }
    if (ch == 0 || !pc_size_mul(w, h, &n) || hole_len != n || !pc_size_mul(n, ch, &nch) || len != nch ||
        (n > 0 && hole == NULL)) {
        o = pc_malloc_array(len > 0 ? len : 1, sizeof *o);
        if (o == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        if (len > 0) {
            memcpy(o, img, len * sizeof *o);
        }
        *out = o;
        return PC_OK;
    }
    if ((st = pc_algo_heal_dims(w, h, ch, len, NULL, err)) != PC_OK) {
        return st;
    }
    bx0 = w, by0 = h;
    for (i = 0; i < n; i++) {
        if (hole[i]) {
            size_t x = i % w, y = i / w;
            bx0 = x < bx0 ? x : bx0, by0 = y < by0 ? y : by0;
            bx1 = x + 1 > bx1 ? x + 1 : bx1, by1 = y + 1 > by1 ? y + 1 : by1;
            count++;
        }
    }
    if (count == 0) {
        o = pc_malloc_array(len > 0 ? len : 1, sizeof *o);
        if (o == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        if (len > 0) {
            memcpy(o, img, len * sizeof *o);
        }
        *out = o;
        return PC_OK;
    }
    if ((st = pc_algo_remove_hole_radius(w, h, hole, n, &radius, err)) != PC_OK) {
        return st;
    }
    thick = radius < (1u << 20) ? radius : (1u << 20);
    tile = 8 * thick > MIN_TILE ? 8 * thick : MIN_TILE;
    area = (bx1 - bx0) * (by1 - by0);
    if ((double)count >= SPARSE * (double)area || (bx1 - bx0 > by1 - by0 ? bx1 - bx0 : by1 - by0) <= 2 * tile) {
        return fill_one(w, h, ch, img, hole, seed, ctl, out, err);
    }
    margin = 6 * thick > 48 ? 6 * thick : 48;
    o = pc_malloc_array(len, sizeof *o);
    left = pc_malloc_array(n, sizeof *left);
    if (o == NULL || left == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    memcpy(o, img, len * sizeof *o);
    memcpy(left, hole, n * sizeof *left);
    norig = ((by1 - by0 + tile - 1) / tile) * ((bx1 - bx0 + tile - 1) / tile);
    k = 0;
    for (ty0 = by0; ty0 < by1; ty0 += tile) {
        for (tx0 = bx0; tx0 < bx1; tx0 += tile, k++) {
            size_t tx1 = tx0 + tile < bx1 ? tx0 + tile : bx1, ty1 = ty0 + tile < by1 ? ty0 + tile : by1;
            size_t x0, y0, x1, y1, ww, hh, x, y;
            bool any = false;
            tile_ctl tc = {ctl, (float)k, (float)norig};
            pc_interrupt inner = {tile_cancel, tile_progress, &tc};
            if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
                goto cleanup;
            }
            for (y = ty0; y < ty1 && !any; y++) {
                for (x = tx0; x < tx1 && !any; x++) {
                    any = left[y * w + x];
                }
            }
            if (!any) {
                continue;
            }
            x0 = tx0 > margin ? tx0 - margin : 0, y0 = ty0 > margin ? ty0 - margin : 0;
            x1 = tx1 + margin < w ? tx1 + margin : w, y1 = ty1 + margin < h ? ty1 + margin : h;
            ww = x1 - x0, hh = y1 - y0;
            sub = pc_malloc_array(ww * hh, ch * sizeof *sub);
            subh = pc_malloc_array(ww * hh, sizeof *subh);
            if (sub == NULL || subh == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
                goto cleanup;
            }
            for (y = y0; y < y1; y++) {
                memcpy(&sub[(y - y0) * ww * ch], &o[(y * w + x0) * ch], ww * ch * sizeof *sub);
                memcpy(&subh[(y - y0) * ww], &left[y * w + x0], ww * sizeof *subh);
            }
            if ((st = fill_one(ww, hh, ch, sub, subh, seed + (uint64_t)k, &inner, &filled, err)) != PC_OK) {
                goto cleanup;
            }
            for (y = ty0; y < ty1; y++) {
                for (x = tx0; x < tx1; x++) {
                    size_t p = y * w + x;
                    if (left[p]) {
                        size_t q = (y - y0) * ww + (x - x0);
                        memcpy(&o[p * ch], &filled[q * ch], ch * sizeof *o);
                        left[p] = false;
                    }
                }
            }
            free(sub), free(subh), free(filled);
            sub = NULL, subh = NULL, filled = NULL;
        }
    }
cleanup:
    free(sub);
    free(subh);
    free(filled);
    free(left);
    if (st != PC_OK) {
        free(o);
        return st;
    }
    *out = o;
    return PC_OK;
}
