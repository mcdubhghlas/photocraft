/* Minimum, Maximum and Offset (port of crates/algo/src/other.rs).
 *
 * Minimum/Maximum use the van Herk / Gil-Werman running extreme, so the cost per pixel does not
 * grow with the radius (square: separable rows then columns; round: one horizontal window per
 * row of the disc). */
#include <float.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fx.h"
#include "algo/fx_defs.h"
#include "base/mem.h"

/* Radii past these would take hours in the reference (it has no limit). */
#define MAX_SQUARE_RADIUS ((int64_t)1 << 24)
#define MAX_ROUND_RADIUS ((int64_t)1 << 15)

static float pick(bool max, float a, float b)
{
    return max ? pc_rs_maxf(a, b) : pc_rs_minf(a, b);
}

size_t pc_algo_other_running_extreme(const float *v, size_t len, size_t k, bool max, float *scratch,
                                     float *out)
{
    float *pre = scratch, *suf = scratch + len;
    size_t i;
    if (k <= 1) {
        if (len > 0) {
            memcpy(out, v, len * sizeof(float));
        }
        return len;
    }
    if (len < k) {
        return 0;
    }
    memcpy(pre, v, len * sizeof(float));
    memcpy(suf, v, len * sizeof(float));
    for (i = 1; i < len; i++) {
        if (i % k != 0) {
            pre[i] = pick(max, pre[i - 1], v[i]);
        }
    }
    for (i = len - 1; i-- > 0;) {
        if ((i + 1) % k != 0) {
            suf[i] = pick(max, suf[i + 1], v[i]);
        }
    }
    for (i = 0; i + k <= len; i++) {
        out[i] = pick(max, suf[i], pre[i + k - 1]);
    }
    return len - k + 1;
}

/* Line, scratch and result buffers for one pass. */
typedef struct lines {
    float *line, *scratch, *ext;
} lines;

static pc_status lines_init(lines *l, size_t len, pc_error *err)
{
    l->line = pc_malloc_array(len > 0 ? len : 1, sizeof(float));
    l->scratch = pc_malloc_array(len > 0 ? 2 * len : 1, sizeof(float));
    l->ext = pc_malloc_array(len > 0 ? len : 1, sizeof(float));
    if (l->line == NULL || l->scratch == NULL || l->ext == NULL) {
        free(l->line), free(l->scratch), free(l->ext);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for Minimum/Maximum");
    }
    return PC_OK;
}

static void lines_fini(lines *l)
{
    free(l->line), free(l->scratch), free(l->ext);
}

/* Minimum or Maximum over a square of 2r+1 (separable). */
static pc_status min_max(const pc_algo_image *src, pc_rect out, float radius, bool max, float *dst,
                         pc_error *err)
{
    size_t n = src->ch, ow = pc_rect_width(out), oh = pc_rect_height(out), ww, wh, k, linelen, tmpn;
    int64_t r = (int64_t)pc_f32_to_i32(roundf(pc_rs_maxf_k(radius, 0.0f))), y;
    float *tmp;
    lines l;
    size_t x, c, i;
    pc_status st;
    if (r == 0) {
        pc_algo_image_crop_into(src, out, dst);
        return PC_OK;
    }
    if (r > MAX_SQUARE_RADIUS) {
        return pc_fail(err, PC_ERR_LIMIT, "Minimum/Maximum radius %lld is too large", (long long)r);
    }
    if (ow == 0 || oh == 0 || n == 0) {
        return PC_OK;
    }
    k = (size_t)(2 * r + 1);
    ww = ow;
    wh = oh + (size_t)(2 * r);
    linelen = ww + (size_t)(2 * r) > wh ? ww + (size_t)(2 * r) : wh;
    if (!pc_size_mul3(ww, wh, n, &tmpn) || tmpn > PC_MAX_ALLOC / sizeof(float)) {
        return pc_fail(err, PC_ERR_LIMIT, "Minimum/Maximum window is too large");
    }
    tmp = pc_malloc_array(tmpn, sizeof(float));
    if (tmp == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for Minimum/Maximum");
    }
    st = lines_init(&l, linelen, err);
    if (st != PC_OK) {
        free(tmp);
        return st;
    }
    /* Rows: the extreme over x - r ..= x + r, for every row the column pass needs. */
    for (y = (int64_t)out.y0 - r; y < (int64_t)out.y1 + r; y++) {
        size_t o = (size_t)(y - ((int64_t)out.y0 - r)) * ww * n;
        for (c = 0; c < n; c++) {
            size_t m = 0, got;
            int64_t xx;
            for (xx = (int64_t)out.x0 - r; xx < (int64_t)out.x1 + r; xx++) {
                l.line[m++] = pc_fx_get64(src, xx, y, c);
            }
            got = pc_algo_other_running_extreme(l.line, m, k, max, l.scratch, l.ext);
            for (x = 0; x < got && x < ww; x++) {
                tmp[o + x * n + c] = l.ext[x];
            }
        }
    }
    /* Columns: the extreme over y - r ..= y + r of the row results. */
    for (x = 0; x < ow; x++) {
        for (c = 0; c < n; c++) {
            size_t got;
            for (i = 0; i < wh; i++) {
                l.line[i] = tmp[(i * ww + x) * n + c];
            }
            got = pc_algo_other_running_extreme(l.line, wh, k, max, l.scratch, l.ext);
            for (i = 0; i < got && i < oh; i++) {
                dst[(i * ow + x) * n + c] = l.ext[i];
            }
        }
    }
    lines_fini(&l);
    free(tmp);
    return PC_OK;
}

/* Minimum or Maximum over a disc: every offset with dx^2 + dy^2 <= radius^2. */
static pc_status min_max_round(const pc_algo_image *src, pc_rect out, float radius, bool max, float *dst,
                               pc_error *err)
{
    size_t n = src->ch, w = pc_rect_width(out), total = pc_fx_npx(out) * n, i;
    float r = pc_rs_maxf_k(radius, 0.0f);
    int64_t ri = (int64_t)pc_f32_to_i32(floorf(r)), dy, y;
    lines l;
    pc_status st;
    if (ri == 0) {
        pc_algo_image_crop_into(src, out, dst);
        return PC_OK;
    }
    if (ri > MAX_ROUND_RADIUS) {
        return pc_fail(err, PC_ERR_LIMIT, "Minimum/Maximum radius %lld is too large", (long long)ri);
    }
    for (i = 0; i < total; i++) {
        dst[i] = max ? -FLT_MAX : FLT_MAX;
    }
    if (w == 0 || n == 0) {
        return PC_OK;
    }
    st = lines_init(&l, w + (size_t)(2 * ri) + 1, err);
    if (st != PC_OK) {
        return st;
    }
    for (dy = -ri; dy <= ri; dy++) {
        /* Half-width of the disc on this row (the epsilon keeps exact squares, e.g. r 5, dy 3 -> 4). */
        int64_t hw = pc_f32_to_i32(floorf(sqrtf(pc_rs_maxf_k(r * r - (float)(dy * dy), 0.0f)) + 1e-4f));
        size_t k = (size_t)(2 * hw + 1);
        for (y = out.y0; y < out.y1; y++) {
            size_t o = (size_t)(y - out.y0) * w * n, c;
            for (c = 0; c < n; c++) {
                size_t m = 0, got, x;
                int64_t xx;
                for (xx = (int64_t)out.x0 - hw; xx < (int64_t)out.x1 + hw; xx++) {
                    l.line[m++] = pc_fx_get64(src, xx, y + dy, c);
                }
                got = pc_algo_other_running_extreme(l.line, m, k, max, l.scratch, l.ext);
                for (x = 0; x < got && x < w; x++) {
                    size_t j = o + x * n + c;
                    dst[j] = pick(max, dst[j], l.ext[x]);
                }
            }
        }
    }
    lines_fini(&l);
    return PC_OK;
}

static pc_status min_max_k(const void *p, const pc_algo_image *src, pc_rect out, bool max, float *dst,
                           pc_error *err)
{
    const pc_algo_minmax_params *m = p;
    return m->preserve == PC_ALGO_PRESERVE_ROUNDNESS ? min_max_round(src, out, m->radius, max, dst, err)
                                                     : min_max(src, out, m->radius, max, dst, err);
}

static pc_status minimum_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                           const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    (void)prep, (void)ctx;
    return min_max_k(p, src, out, false, dst, err);
}

static pc_status maximum_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                           const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    (void)prep, (void)ctx;
    return min_max_k(p, src, out, true, dst, err);
}

static pc_algo_halo minmax_halo(const void *p, const pc_rect *b)
{
    (void)b;
    return pc_algo_halo_reach(((const pc_algo_minmax_params *)p)->radius);
}

pc_algo_edge pc_fx_edge_of(pc_algo_undefined_areas u)
{
    switch (u) {
    case PC_ALGO_UNDEF_WRAP:
        return PC_ALGO_EDGE_WRAP;
    case PC_ALGO_UNDEF_REPEAT:
        return PC_ALGO_EDGE_REPEAT;
    default:
        return PC_ALGO_EDGE_TRANSPARENT;
    }
}

/* Shifts the bounds' content by whole pixels. */
static pc_status offset_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                          const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_offset_params *o = p;
    pc_algo_edge e = pc_fx_edge_of(o->undefined);
    size_t k = 0, c;
    int32_t x, y;
    (void)prep, (void)err;
    for (y = out.y0; y < out.y1; y++) {
        for (x = out.x0; x < out.x1; x++) {
            for (c = 0; c < src->ch; c++) {
                dst[k++] = pc_algo_image_get_edge(src, pc_i32_wrapping_sub(x, o->horizontal),
                                                  pc_i32_wrapping_sub(y, o->vertical), c, e, ctx->bounds);
            }
        }
    }
    return PC_OK;
}

static const pc_algo_jfield MINMAX_F[] = {
    PC_ALGO_JF("radius", pc_algo_minmax_params, radius, PC_ALGO_JF_F32),
    PC_ALGO_JFE("preserve", pc_algo_minmax_params, preserve, PC_ALGO_ENUM_PRESERVE, true),
};
static const pc_algo_jfield OFFSET_F[] = {
    PC_ALGO_JF("horizontal", pc_algo_offset_params, horizontal, PC_ALGO_JF_I32),
    PC_ALGO_JF("vertical", pc_algo_offset_params, vertical, PC_ALGO_JF_I32),
    PC_ALGO_JFE("undefined", pc_algo_offset_params, undefined, PC_ALGO_ENUM_UNDEFINED_AREAS, false),
};

PC_FX_DEF(pc_algo_def_minimum, "minimum", pc_algo_minmax_params, MINMAX_F, NULL, minmax_halo, minimum_k);
PC_FX_DEF(pc_algo_def_maximum, "maximum", pc_algo_minmax_params, MINMAX_F, NULL, minmax_halo, maximum_k);
PC_FX_DEF(pc_algo_def_offset, "offset", pc_algo_offset_params, OFFSET_F, NULL, pc_fx_halo_bounds, offset_k);
