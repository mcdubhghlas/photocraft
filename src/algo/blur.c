/* Gaussian and Box Blur (port of crates/algo/src/blur.rs: gaussian_kernel, conv_sep, the box-pass
 * machinery and lib.rs apply_box_blur), plus the small shared helpers of the L5 filters.
 *
 * Small radii convolve with a direct separable kernel; larger ones run as running-sum box passes
 * (three for a Gaussian, Kovesi 2010) whose cost doesn't depend on the radius. Box passes repeat
 * the edge pixels of their source rectangle instead of materialising 2-D margins. */
#include "algo/blur_int.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

size_t pc_algo_partition_point(size_t n, bool (*pred)(const void *ctx, size_t i), const void *ctx)
{
    /* core::slice::binary_search_by as of Rust 1.95 (no early exit on Equal; pred true = Less). */
    size_t size = n, base = 0;
    if (size == 0) {
        return 0;
    }
    while (size > 1) {
        size_t half = size / 2, mid = base + half;
        if (pred(ctx, mid)) {
            base = mid;
        }
        size -= half;
    }
    return base + (pred(ctx, base) ? 1u : 0u);
}

/* ---- direct kernels ---- */

/* Longest direct kernel (2^24 + 1 taps). */
#define MAX_KERNEL_RADIUS ((int32_t)1 << 23)

pc_status pc_algo_gaussian_kernel(float sigma, float **k, size_t *len, pc_error *err)
{
    float *v, s = -0.0f; /* Iterator::sum starts at -0.0 */
    int32_t r, i;
    size_t n, j;
    if (sigma < 0.05f) {
        v = malloc(sizeof(float));
        if (v == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for a Gaussian kernel");
        }
        v[0] = 1.0f;
        *k = v;
        *len = 1;
        return PC_OK;
    }
    r = pc_f32_to_i32(ceilf(sigma * 3.0f));
    if (r > MAX_KERNEL_RADIUS) {
        return pc_fail(err, PC_ERR_LIMIT, "Gaussian radius %g is too large for a direct kernel",
                       (double)sigma);
    }
    if (r < 0) {
        r = -1; /* an empty range, as (-r..=r) is in the reference */
    }
    n = (size_t)(2 * (int64_t)r + 1);
    v = pc_malloc_array(n > 0 ? n : 1, sizeof(float));
    if (v == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a Gaussian kernel");
    }
    for (i = -r, j = 0; j < n; i++, j++) {
        /* -(i * i) as f32: i32 arithmetic, wrapping like the reference's release build. */
        int32_t sq = pc_i32_wrapping_mul(i, i);
        float num = (float)pc_i32_wrapping_sub(0, sq);
        v[j] = expf(num / (2.0f * sigma * sigma));
        s += v[j];
    }
    for (j = 0; j < n; j++) {
        v[j] = v[j] / s;
    }
    *k = v;
    *len = n;
    return PC_OK;
}

pc_status pc_algo_conv_sep(const pc_algo_image *src, pc_rect out, const float *kx, size_t nkx,
                           const float *ky, size_t nky, bool alpha, float *dst, pc_error *err)
{
    size_t n = src->ch, ow = pc_rect_width(out), oh = pc_rect_height(out);
    int32_t rx = (int32_t)(nkx / 2), ry = (int32_t)(nky / 2);
    size_t th, ww, wn, tn, ty, ox, oy, i, c;
    pc_rect win;
    float *p, *tmp;
    int32_t x, y;
    if (n == 0 || ow == 0 || oh == 0) {
        return PC_OK;
    }
    if (nkx > ((size_t)1 << 25) || nky > ((size_t)1 << 25)) {
        return pc_fail(err, PC_ERR_LIMIT, "convolution kernel too long");
    }
    th = oh + 2 * (size_t)ry;
    win = pc_rect_make(pc_i32_wrapping_sub(out.x0, rx), pc_i32_wrapping_sub(out.y0, ry),
                       pc_i32_wrapping_add(out.x1, rx), pc_i32_wrapping_add(out.y1, ry));
    ww = pc_rect_width(win);
    if (!pc_algo_float_count(win, n, &wn) || !pc_size_mul3(ow, th, n, &tn) || tn > PC_MAX_ALLOC / 4) {
        return pc_fail(err, PC_ERR_LIMIT, "convolution window too large");
    }
    p = pc_calloc_array(wn > 0 ? wn : 1, sizeof(float));
    tmp = pc_calloc_array(tn > 0 ? tn : 1, sizeof(float));
    if (p == NULL || tmp == NULL) {
        free(p);
        free(tmp);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a convolution");
    }
    /* Premultiplied copy of the needed source window. */
    for (y = win.y0; y < win.y1; y++) {
        for (x = win.x0; x < win.x1; x++) {
            size_t o = ((size_t)((int64_t)y - win.y0) * ww + (size_t)((int64_t)x - win.x0)) * n;
            for (c = 0; c < n; c++) {
                p[o + c] = pc_algo_image_get(src, x, y, c);
            }
        }
    }
    pc_algo_premultiply(p, wn, n, alpha);
    /* Horizontal pass (ow x th): taps in order per pixel, accumulators from 0. */
    if (ww >= nkx) {
        for (ty = 0; ty < th && ty < pc_rect_height(win); ty++) {
            const float *row = p + ty * ww * n;
            float *d = tmp + ty * ow * n;
            for (ox = 0; ox < ow; ox++) {
                float acc[PC_ALGO_MAXC];
                float *dp = d + ox * n;
                if (n <= PC_ALGO_MAXC) {
                    for (c = 0; c < n; c++) {
                        acc[c] = 0.0f;
                    }
                    for (i = 0; i < nkx && ox + i < ww; i++) {
                        const float *s = row + (ox + i) * n;
                        for (c = 0; c < n; c++) {
                            acc[c] += s[c] * kx[i];
                        }
                    }
                    memcpy(dp, acc, n * sizeof(float));
                } else {
                    for (i = 0; i < nkx && ox + i < ww; i++) {
                        const float *s = row + (ox + i) * n;
                        for (c = 0; c < n; c++) {
                            dp[c] += s[c] * kx[i];
                        }
                    }
                }
            }
        }
    }
    /* Vertical pass. */
    for (oy = 0; oy < oh; oy++) {
        float *d = dst + oy * ow * n;
        for (i = 0; i < nky && oy + i < th; i++) {
            const float *s = tmp + (oy + i) * ow * n;
            for (c = 0; c < ow * n; c++) {
                d[c] += s[c] * ky[i];
            }
        }
    }
    pc_algo_unpremultiply(dst, ow * oh * n, n, alpha);
    free(p);
    free(tmp);
    return PC_OK;
}

/* ---- box widths ---- */

/* blur::boxes_for_gauss(sigma, 3). Widths are i32 in the reference (wl + 2 wraps at i32::MAX),
 * then `as usize`; anything beyond PC_ALGO_MAX_BOX_RADIUS is refused by the caller. */
static void boxes_for_gauss(float sigma, int64_t out[3])
{
    const float nf = 3.0f;
    float w_ideal = sqrtf(12.0f * sigma * sigma / nf + 1.0f), m_ideal;
    int32_t wl = pc_f32_to_i32(floorf(w_ideal)), wu;
    size_t m, i;
    if (wl % 2 == 0) {
        wl = pc_i32_wrapping_sub(wl, 1);
    }
    wl = wl > 1 ? wl : 1;
    wu = pc_i32_wrapping_add(wl, 2);
    m_ideal = (12.0f * sigma * sigma - nf * (float)pc_i32_wrapping_mul(wl, wl) - 4.0f * nf * (float)wl -
               3.0f * nf) /
              (-4.0f * (float)wl - 4.0f);
    m = pc_f32_to_size(pc_clampf(roundf(m_ideal), 0.0f, nf));
    for (i = 0; i < 3; i++) {
        out[i] = i < m ? wl : wu;
    }
}

pc_status pc_algo_box_widths(const pc_algo_filter_def *def, const void *params, size_t out[3], size_t *count,
                             pc_error *err)
{
    float radius = ((const pc_algo_radius_params *)params)->radius;
    int64_t w[3];
    size_t i;
    *count = 0;
    if (def == &PC_ALGO_FILTER_GAUSSIAN_BLUR) {
        if (!(radius > 4.0f)) {
            return PC_OK;
        }
        boxes_for_gauss(radius, w);
        for (i = 0; i < 3; i++) {
            /* A negative i32 width becomes a huge usize in the reference. */
            if (w[i] < 0 || (uint64_t)w[i] / 2 > PC_ALGO_MAX_BOX_RADIUS) {
                return pc_fail(err, PC_ERR_LIMIT, "Gaussian Blur radius %g is too large", (double)radius);
            }
            out[i] = (size_t)w[i];
        }
        *count = 3;
    } else if (def == &PC_ALGO_FILTER_BOX_BLUR) {
        size_t r = pc_f32_to_size(roundf(pc_rs_maxf_k(radius, 0.0f)));
        if (r <= 4) {
            return PC_OK;
        }
        if (r > PC_ALGO_MAX_BOX_RADIUS) {
            return pc_fail(err, PC_ERR_LIMIT, "Box Blur radius %g is too large", (double)radius);
        }
        out[0] = 2 * r + 1;
        *count = 1;
    }
    return PC_OK;
}

/* ---- running-sum box passes ---- */

/* One box pass of radius r along a line of elements of w floats: element i of dst (dlen elements)
 * is the mean of elements i ..= i + 2r of src (slen elements, normally dlen + 2r). acc: w floats. */
static void box_pass(const float *src, size_t slen, float *dst, size_t dlen, size_t w, size_t r, float *acc)
{
    float norm = 1.0f / (float)(2 * r + 1);
    size_t i, c, take = 2 * r + 1 < slen ? 2 * r + 1 : slen;
    if (w == 0) {
        return;
    }
    for (c = 0; c < w; c++) {
        acc[c] = 0.0f;
    }
    for (i = 0; i < take; i++) {
        for (c = 0; c < w; c++) {
            acc[c] += src[i * w + c];
        }
    }
    for (i = 0; i < dlen; i++) {
        for (c = 0; c < w; c++) {
            dst[i * w + c] = acc[c] * norm;
        }
        if (i + 2 * r + 1 < slen) {
            const float *add = src + (i + 2 * r + 1) * w, *sub = src + i * w;
            for (c = 0; c < w; c++) {
                acc[c] += add[c] - sub[c];
            }
        }
    }
}

/* Box passes of `radii` in turn along the line in *line (cur elements, consumed; *next is scratch
 * of the same capacity), ending in dst (len elements of w floats). */
static void line_passes(float *dst, size_t len, size_t w, const size_t *radii, size_t nr, float **line,
                        size_t cur, float **next, float *acc)
{
    size_t tail = 0, k;
    for (k = 0; k < nr; k++) {
        tail += radii[k];
    }
    for (k = 0; k < nr; k++) {
        tail -= radii[k];
        if (k + 1 == nr) {
            box_pass(*line, cur, dst, len, w, radii[k], acc);
        } else {
            size_t nl = len + 2 * tail;
            float *t;
            box_pass(*line, cur, *next, nl, w, radii[k], acc);
            t = *line;
            *line = *next;
            *next = t;
            cur = nl;
        }
    }
}

/* Reads a one-pixel-high rect inside the box source into buf (len floats). */
typedef pc_status (*row_reader)(const void *rctx, pc_rect row, float *buf, size_t len);

typedef struct box_job {
    row_reader read;
    const void *rctx;
    pc_rect used, out;
    size_t n, ow, oh, left, right, top, bottom, reach;
    bool alpha;
    const size_t *radii;
    size_t nr;
    float *h;      /* used.h rows of ow * n: the horizontal results */
    float *dst;    /* oh rows of ow * n */
    int64_t first; /* x of the first strip (a multiple of the strip width) */
    const pc_interrupt *ctl;
} box_job;

static pc_status box_rows(void *vctx, size_t chunk, size_t begin, size_t end)
{
    const box_job *j = vctx;
    size_t uw = pc_rect_width(j->used), n = j->n, linelen = (j->ow + 2 * j->reach) * n, k, i;
    float *row = pc_malloc_array(uw * n + 1, sizeof(float));
    float *line = pc_malloc_array(linelen + 1, sizeof(float));
    float *next = pc_malloc_array(linelen + 1, sizeof(float));
    float *acc = pc_malloc_array(n + 1, sizeof(float));
    pc_status st = PC_OK;
    (void)chunk;
    if (row == NULL || line == NULL || next == NULL || acc == NULL) {
        st = PC_ERR_NOMEM;
        goto done;
    }
    for (k = begin; k < end; k++) {
        int32_t y = (int32_t)((int64_t)j->used.y0 + (int64_t)k);
        float *l = line, *nx = next, *p;
        if (pc_interrupt_cancelled(j->ctl)) {
            st = PC_ERR_CANCELLED;
            goto done;
        }
        st = j->read(j->rctx, pc_rect_make(j->used.x0, y, j->used.x1, y + 1), row, uw * n);
        if (st != PC_OK) {
            goto done;
        }
        pc_algo_premultiply(row, uw * n, n, j->alpha);
        p = l;
        for (i = 0; i < j->left; i++, p += n) {
            memcpy(p, row, n * sizeof(float));
        }
        memcpy(p, row, uw * n * sizeof(float));
        p += uw * n;
        for (i = 0; i < j->right; i++, p += n) {
            memcpy(p, row + (uw - 1) * n, n * sizeof(float));
        }
        line_passes(j->h + k * j->ow * n, j->ow, n, j->radii, j->nr, &l, j->left + uw + j->right, &nx, acc);
    }
done:
    free(row), free(line), free(next), free(acc);
    return st;
}

static pc_status box_strips(void *vctx, size_t chunk, size_t begin, size_t end)
{
    const box_job *j = vctx;
    size_t n = j->n, rows = pc_rect_height(j->used), stride = j->ow * n, k, i, y;
    size_t linelen = (j->oh + 2 * j->reach) * PC_ALGO_BOX_STRIP * n;
    float *line = pc_malloc_array(linelen + 1, sizeof(float));
    float *next = pc_malloc_array(linelen + 1, sizeof(float));
    float *res = pc_malloc_array(j->oh * PC_ALGO_BOX_STRIP * n + 1, sizeof(float));
    float *acc = pc_malloc_array(PC_ALGO_BOX_STRIP * n + 1, sizeof(float));
    pc_status st = PC_OK;
    (void)chunk;
    if (line == NULL || next == NULL || res == NULL || acc == NULL) {
        st = PC_ERR_NOMEM;
        goto done;
    }
    for (k = begin; k < end; k++) {
        int64_t x = j->first + (int64_t)k * PC_ALGO_BOX_STRIP;
        int64_t x0 = x > j->out.x0 ? x : j->out.x0;
        int64_t x1 = x + PC_ALGO_BOX_STRIP < j->out.x1 ? x + PC_ALGO_BOX_STRIP : j->out.x1;
        size_t sx = (size_t)(x0 - j->out.x0) * n, w = (size_t)(x1 - x0) * n;
        float *l = line, *nx = next, *p = line;
        if (pc_interrupt_cancelled(j->ctl)) {
            st = PC_ERR_CANCELLED;
            goto done;
        }
        for (i = 0; i < j->top; i++, p += w) {
            memcpy(p, j->h + sx, w * sizeof(float));
        }
        for (y = 0; y < rows; y++, p += w) {
            memcpy(p, j->h + y * stride + sx, w * sizeof(float));
        }
        for (i = 0; i < j->bottom; i++, p += w) {
            memcpy(p, j->h + (rows - 1) * stride + sx, w * sizeof(float));
        }
        line_passes(res, j->oh, w, j->radii, j->nr, &l, j->top + rows + j->bottom, &nx, acc);
        pc_algo_unpremultiply(res, j->oh * w, n, j->alpha);
        for (y = 0; y < j->oh; y++) {
            memcpy(j->dst + y * stride + sx, res + y * w, w * sizeof(float));
        }
    }
done:
    free(line), free(next), free(res), free(acc);
    return st;
}

/* blur::box_blur: box blurs of the given odd widths along each axis of the pixels of `src`
 * extended by repeating its edge pixels, for `out` into dst (oh rows of ow * n; zero where src
 * gives nothing). */
static pc_status box_blur(pc_rect src, row_reader read, const void *rctx, pc_rect out, size_t n, bool alpha,
                          const size_t *boxes, size_t nb, const pc_interrupt *ctl, float *dst, pc_error *err)
{
    size_t radii[3], reach = 0, k, nh, nstrips, linelen;
    box_job j;
    pc_status st;
    if (nb > 3) {
        return pc_fail(err, PC_ERR_INVALID, "too many box passes");
    }
    for (k = 0; k < nb; k++) {
        radii[k] = boxes[k] / 2;
        reach += radii[k];
    }
    memset(&j, 0, sizeof j);
    j.read = read;
    j.rctx = rctx;
    j.out = out;
    j.n = n;
    j.alpha = alpha;
    j.radii = radii;
    j.nr = nb;
    j.ow = pc_rect_width(out);
    j.oh = pc_rect_height(out);
    j.reach = reach;
    j.ctl = ctl;
    j.dst = dst;
    j.used = pc_rect_intersect(src, pc_rect_inflate(out, reach > INT32_MAX ? INT32_MAX : (int32_t)reach));
    if (pc_rect_is_empty(j.used) || n == 0 || j.ow == 0 || j.oh == 0) {
        /* Nothing to read: transparent, as Image::get reads outside an image (dst is zeroed). */
        return PC_OK;
    }
    {
        int32_t r32 = (int32_t)reach;
        j.left = (size_t)((int64_t)j.used.x0 - pc_i32_sat_sub(out.x0, r32));
        j.right = (size_t)((int64_t)pc_i32_sat_add(out.x1, r32) - j.used.x1);
        j.top = (size_t)((int64_t)j.used.y0 - pc_i32_sat_sub(out.y0, r32));
        j.bottom = (size_t)((int64_t)pc_i32_sat_add(out.y1, r32) - j.used.y1);
    }
    /* Line scratch per worker: (len + 2 reach) elements; refuse what can never be allocated. */
    if (!pc_size_mul3(pc_rect_height(j.used), j.ow, n, &nh) ||
        !pc_size_mul3(j.oh + 2 * reach, PC_ALGO_BOX_STRIP, n, &linelen) || nh > PC_MAX_ALLOC / 4 ||
        linelen > PC_MAX_ALLOC / 4 || (j.ow + 2 * reach) * n > PC_MAX_ALLOC / 4) {
        return pc_fail(err, PC_ERR_LIMIT, "box blur of %ux%u is too large", pc_rect_width(out),
                       pc_rect_height(out));
    }
    j.h = pc_malloc_array(nh > 0 ? nh : 1, sizeof(float));
    if (j.h == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a box blur");
    }
    /* Horizontal passes over used's rows only (the repeated rows above and below would just
     * repeat their results), for the output columns. */
    st = pc_algo_par_for(pc_rect_height(j.used), 8, box_rows, &j);
    if (st == PC_OK) {
        /* Vertical passes, strip by strip; strips lie at multiples of the strip width. */
        j.first = (int64_t)out.x0 -
                  ((((int64_t)out.x0 % PC_ALGO_BOX_STRIP) + PC_ALGO_BOX_STRIP) % PC_ALGO_BOX_STRIP);
        nstrips = (size_t)(((int64_t)out.x1 - j.first + PC_ALGO_BOX_STRIP - 1) / PC_ALGO_BOX_STRIP);
        st = pc_algo_par_for(nstrips, 1, box_strips, &j);
    }
    free(j.h);
    if (st == PC_OK && pc_interrupt_cancelled(ctl)) {
        st = PC_ERR_CANCELLED;
    }
    if (st == PC_ERR_CANCELLED) {
        return pc_fail(err, PC_ERR_CANCELLED, "cancelled");
    }
    if (st != PC_OK) {
        return pc_fail(err, st, "box blur failed (out of memory)");
    }
    return PC_OK;
}

static pc_status image_row_reader(const void *rctx, pc_rect row, float *buf, size_t len)
{
    const float *p = pc_algo_image_row(rctx, row);
    if (p == NULL) {
        memset(buf, 0, len * sizeof(float));
        return PC_OK;
    }
    memcpy(buf, p, len * sizeof(float));
    return PC_OK;
}

pc_status pc_algo_box_passes(const pc_algo_image *src, pc_rect out, bool alpha, const size_t *boxes,
                             size_t nboxes, float *dst, pc_error *err)
{
    return box_blur(src->rect, image_row_reader, src, out, src->ch, alpha, boxes, nboxes, NULL, dst, err);
}

pc_status pc_algo_gaussian_k(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx, float radius,
                             float *dst, pc_error *err)
{
    pc_algo_radius_params p = {radius};
    size_t boxes[3], nb;
    float *k;
    size_t nk;
    pc_status st = pc_algo_box_widths(&PC_ALGO_FILTER_GAUSSIAN_BLUR, &p, boxes, &nb, err);
    if (st != PC_OK) {
        return st;
    }
    if (nb > 0) {
        return pc_algo_box_passes(src, out, ctx->alpha, boxes, nb, dst, err);
    }
    st = pc_algo_gaussian_kernel(radius, &k, &nk, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_algo_conv_sep(src, out, k, nk, k, nk, ctx->alpha, dst, err);
    free(k);
    return st;
}

pc_status pc_algo_boxed_k(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx, float radius,
                          float *dst, pc_error *err)
{
    pc_algo_radius_params p = {radius};
    size_t boxes[3], nb, r, nk, i;
    float *k;
    pc_status st = pc_algo_box_widths(&PC_ALGO_FILTER_BOX_BLUR, &p, boxes, &nb, err);
    if (st != PC_OK) {
        return st;
    }
    if (nb > 0) {
        return pc_algo_box_passes(src, out, ctx->alpha, boxes, nb, dst, err);
    }
    r = pc_f32_to_size(roundf(pc_rs_maxf_k(radius, 0.0f)));
    nk = 2 * r + 1;
    k = pc_malloc_array(nk, sizeof(float));
    if (k == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a box kernel");
    }
    for (i = 0; i < nk; i++) {
        k[i] = 1.0f / (float)nk;
    }
    st = pc_algo_conv_sep(src, out, k, nk, k, nk, ctx->alpha, dst, err);
    free(k);
    return st;
}

/* ---- lib.rs apply_box_blur: the whole area at once ---- */

static pc_status surface_row_reader(const void *rctx, pc_rect row, float *buf, size_t len)
{
    return pc_surface_read_region_into(rctx, row, buf, len, NULL);
}

pc_status pc_algo_box_blur_apply(const pc_algo_apply_args *a, pc_surface *out, bool *handled, pc_error *err)
{
    const pc_surface *surface = a->surface;
    pc_pixel_format fmt = pc_surface_format(surface);
    size_t n = pc_pixel_format_channels(fmt), boxes[3], nb, reach = 0, k, column_bytes, band, nbands, bi;
    pc_rect grown, src;
    pc_surface res;
    pc_status st;
    int64_t x;
    *handled = false;
    st = pc_algo_box_widths(a->filter->def, a->filter->params, boxes, &nb, err);
    if (st != PC_OK) {
        *handled = true;
        return st;
    }
    if (nb == 0) {
        return PC_OK;
    }
    *handled = true;
    for (k = 0; k < nb; k++) {
        reach += boxes[k] / 2;
    }
    grown = pc_rect_inflate(a->area, reach > INT32_MAX ? INT32_MAX : (int32_t)reach);
    src = grown;
    if (a->extent != NULL) {
        pc_rect e = pc_rect_intersect(grown, *a->extent);
        if (!pc_rect_is_empty(e)) {
            src = e;
        }
    }
    /* Per output column: the horizontal results for every source row, plus the final samples. */
    column_bytes = ((size_t)pc_rect_height(src) + pc_rect_height(a->area)) * n * sizeof(float);
    band = PC_ALGO_RESULT_BUDGET / (column_bytes > 0 ? column_bytes : 1);
    if (band > INT32_MAX) {
        band = INT32_MAX;
    }
    if (band < PC_ALGO_BOX_STRIP) {
        band = PC_ALGO_BOX_STRIP;
    }
    band = band / PC_ALGO_BOX_STRIP * PC_ALGO_BOX_STRIP;
    nbands = (size_t)(((uint64_t)pc_rect_width(a->area) + band - 1) / band);
    st = pc_surface_clone(surface, &res, err);
    if (st != PC_OK) {
        return st;
    }
    for (bi = 0, x = a->area.x0; x < a->area.x1; bi++, x += (int64_t)band) {
        int64_t x1 = x + (int64_t)band < a->area.x1 ? x + (int64_t)band : a->area.x1;
        pc_rect b = pc_rect_make((int32_t)x, a->area.y0, (int32_t)x1, a->area.y1);
        size_t len;
        float *data;
        if (!pc_algo_float_count(b, n, &len)) {
            st = pc_fail(err, PC_ERR_LIMIT, "blur band too large");
            break;
        }
        data = pc_calloc_array(len > 0 ? len : 1, sizeof(float));
        if (data == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory for a blur band");
            break;
        }
        st = box_blur(src, surface_row_reader, surface, b, n, fmt.alpha, boxes, nb, a->ctl, data, err);
        if (st == PC_OK && a->selection != NULL) {
            pc_algo_image orig;
            st = pc_algo_image_read(surface, b, &orig, err);
            if (st == PC_OK) {
                pc_algo_mix_selection(data, b, a->selection, &orig, fmt.alpha);
                pc_algo_image_fini(&orig);
            }
        }
        if (st == PC_OK) {
            st = pc_surface_write_region(&res, b, data, len, err);
        }
        free(data);
        if (st != PC_OK) {
            break;
        }
        pc_interrupt_progress(a->ctl, 0.98f * (float)(bi + 1) / (float)nbands);
    }
    if (st != PC_OK) {
        pc_surface_fini(&res);
        return st;
    }
    pc_surface_prune(&res);
    pc_interrupt_progress(a->ctl, 1.0f);
    *out = res;
    return PC_OK;
}
