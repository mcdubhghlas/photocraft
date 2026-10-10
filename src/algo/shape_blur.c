/* Shape Blur's row-wise convolution (port of crates/algo/src/shape_blur.rs): each sample sees the
 * same f64 additions, in the same span order, as the scalar prefix implementation in blur2.c. */
#include "algo/blur_int.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "base/mem.h"
#include "base/num.h"

/* Bound on scratch and output samples, including hostile direct input (a 36 MP image at all eight
 * channels still fits). */
#define MAX_SAMPLES ((size_t)512 * 1024 * 1024)

float pc_algo_shape_blur_radius(float r)
{
    return isfinite(r) ? pc_clampf(r, 0.0f, 1000.0f) : 0.0f;
}

pc_status pc_algo_shape_blur_prepare(const void *params, void **prep, pc_error *err)
{
    const pc_algo_shape_blur_params *p = params;
    pc_algo_spans *s = calloc(1, sizeof *s);
    pc_algo_shape_ctx k;
    pc_status st;
    if (s == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a Shape Blur kernel");
    }
    pc_algo_shape_ctx_init(&k, p->shape);
    st = pc_algo_spans_new(pc_algo_shape_blur_radius(p->radius), pc_algo_shape_inside, &k, s, err);
    if (st != PC_OK) {
        free(s);
        return st;
    }
    *prep = s;
    return PC_OK;
}

void pc_algo_shape_blur_prep_free(void *prep)
{
    pc_algo_spans_fini(prep);
    free(prep);
}

pc_status pc_algo_shape_blur_run(const pc_algo_spans *spans, const pc_algo_image *src, pc_rect out,
                                 const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    size_t n = src->ch, w = pc_rect_width(src->rect), h = pc_rect_height(src->rect), input_row, stride;
    size_t prefix_len, row_len, output_len, ow = pc_rect_width(out), y, i, c, k = 0, s;
    int64_t x;
    double *prefix, *acc;
    int32_t yy32;
    if (n == 0 || n > PC_ALGO_MAXC || pc_rect_is_empty(out)) {
        return n == 0 || pc_rect_is_empty(out)
                   ? PC_OK
                   : pc_fail(err, PC_ERR_INVALID, "unsupported channel count %zu", n);
    }
    if (!pc_size_mul(w, n, &input_row) || !pc_size_mul(input_row, h, &s) || s != src->len) {
        return pc_fail(err, PC_ERR_INVALID, "malformed image");
    }
    if (!pc_size_mul(w + 1, n, &stride) || !pc_size_mul(stride, h, &prefix_len) ||
        !pc_size_mul(ow, n, &row_len) || !pc_size_mul(row_len, pc_rect_height(out), &output_len) ||
        prefix_len > MAX_SAMPLES || output_len > MAX_SAMPLES) {
        return pc_fail(err, PC_ERR_LIMIT, "Shape Blur area too large");
    }
    prefix = pc_calloc_array(prefix_len > 0 ? prefix_len : 1, sizeof *prefix);
    acc = pc_malloc_array(row_len > 0 ? row_len : 1, sizeof *acc);
    if (prefix == NULL || acc == NULL) {
        free(prefix);
        free(acc);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for Shape Blur");
    }
    for (y = 0; y < h && input_row != 0; y++) {
        const float *srow = src->data + y * input_row;
        double *d = prefix + y * stride, sum[PC_ALGO_MAXC] = {0};
        for (i = 0; i < w; i++) {
            const float *px = srow + i * n;
            float alpha = ctx->alpha ? px[n - 1] : 1.0f;
            for (c = 0; c < n; c++) {
                /* The multiplication stays f32, exactly as premul_window. */
                float v = ctx->alpha && c + 1 < n ? px[c] * alpha : px[c];
                sum[c] += (double)v;
                d[(i + 1) * n + c] = sum[c];
            }
        }
    }
    x = (int64_t)out.x0 - src->rect.x0;
    for (yy32 = out.y0; yy32 < out.y1; yy32++) {
        size_t q;
        for (i = 0; i < row_len; i++) {
            acc[i] = 0.0;
        }
        for (q = 0; q < spans->n; q++) {
            int64_t yy = (int64_t)yy32 - src->rect.y0 + spans->rows[q][0];
            int64_t lo = x + spans->rows[q][1], hi = x + spans->rows[q][2] + 1, end = (int64_t)ow;
            const double *row;
            if (yy < 0 || yy >= (int64_t)h) {
                continue;
            }
            row = prefix + (size_t)yy * stride;
            if (lo >= 0 && hi + end <= (int64_t)w) {
                const double *low = row + (size_t)lo * n, *high = row + (size_t)hi * n;
                for (i = 0; i < row_len; i++) {
                    acc[i] += high[i] - low[i];
                }
            } else {
                /* Direct callers may omit the halo: the scalar kernel's zero extension and fixed
                 * normalisation. */
                for (i = 0; i < ow; i++) {
                    int64_t l = lo + (int64_t)i, hh = hi + (int64_t)i;
                    size_t low = (size_t)(l<0 ? 0 : l>(int64_t) w ? (int64_t)w : l) * n;
                    size_t high = (size_t)(hh<0 ? 0 : hh>(int64_t) w ? (int64_t)w : hh) * n;
                    for (c = 0; c < n; c++) {
                        acc[i * n + c] += row[high + c] - row[low + c];
                    }
                }
            }
        }
        for (i = 0; i < ow; i++) {
            float sample[PC_ALGO_MAXC];
            for (c = 0; c < n; c++) {
                sample[c] = (float)(acc[i * n + c] / spans->count);
            }
            pc_algo_unpremul_px(sample, n, ctx->alpha);
            memcpy(dst + k, sample, n * sizeof(float));
            k += n;
        }
    }
    free(prefix);
    free(acc);
    return PC_OK;
}
