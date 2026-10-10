/* Overlap-save convolution of the discrete Motion Blur kernel (port of
 * crates/algo/src/blur/motion_fft.rs).
 *
 * The spectrum uses the row kernel's merged f32 coefficients, with no intermediate resampling.
 * Transforms run in doubles (pocketfft, REWRITE_PLAN.md D11, where the reference used rustfft)
 * and keep alpha premultiplied until the final output. The summation order differs from the row
 * kernel, which stays the fallback; results match it to FFT round-off, not bit for bit. */
#include "algo/blur_int.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <pocketfft.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

#define MAX_POINTS ((size_t)4 << 20)
#define MAX_SIDE ((size_t)8192)
#define MAX_BYTES ((size_t)128 << 20)
#define MAX_SAMPLE 1e10f
#define COORD PC_ALGO_MOTION_COORDINATE_LIMIT

/* One merged tap with its weight widened to f64 (motion_fft::Tap; 16 bytes like the reference's). */
typedef struct tap64 {
    int32_t x, y;
    double weight;
} tap64;

struct pc_algo_motion_plan {
    float distance;
    size_t width, height, tile_width, tile_height;
    int32_t min_x, max_x, min_y, max_y;
    double *kernel; /* width * height complex values (re, im) */
    tap64 *taps;
    size_t ntaps;
    cfft_plan row, col;
    size_t scratch_len;
};

/* buffer(): a zeroed allocation of len elements of `size` bytes, NULL past MAX_BYTES. */
static void *buffer(size_t len, size_t size)
{
    size_t bytes;
    if (!pc_size_mul(len, size, &bytes) || bytes > MAX_BYTES) {
        return NULL;
    }
    return calloc(len > 0 ? len : 1, size);
}

static bool dimensions(pc_rect r, size_t *w, size_t *h)
{
    int64_t dw = (int64_t)r.x1 - r.x0, dh = (int64_t)r.y1 - r.y0;
    if (dw < 0 || dh < 0) {
        return false;
    }
    *w = (size_t)dw;
    *h = (size_t)dh;
    return true;
}

bool pc_algo_motion_ordinary_coordinates(pc_rect r)
{
    return r.x0 >= -COORD && r.x0 <= COORD && r.y0 >= -COORD && r.y0 <= COORD && r.x1 >= -COORD &&
           r.x1 <= COORD && r.y1 >= -COORD && r.y1 <= COORD;
}

/* Smallest 2^a 3^b 5^c >= needed (<= MAX_SIDE), or 0. */
static size_t fft_size(size_t needed)
{
    size_t best = MAX_SIDE, a, b, c;
    if (needed == 0 || needed > MAX_SIDE) {
        return 0;
    }
    for (a = 1; a <= MAX_SIDE; a *= 2) {
        for (b = a; b <= MAX_SIDE; b *= 3) {
            for (c = b; c <= MAX_SIDE; c *= 5) {
                if (c >= needed && c < best) {
                    best = c;
                }
            }
        }
    }
    return best;
}

/* motion_fft::taps: NULL (declined) unless the angle is finite and |distance| in 64..=2000. */
static bool fft_taps(float angle, float distance, const pc_rect clip[2], tap64 **out, size_t *n)
{
    pc_algo_motion_tap *m;
    size_t nm, i, k = 0;
    tap64 *t;
    float d = fabsf(distance);
    if (!isfinite(angle) || !(d >= 64.0f && d <= 2000.0f)) {
        return false;
    }
    if (pc_algo_motion_taps(angle, d, &m, &nm, NULL) != PC_OK) {
        return false;
    }
    t = pc_malloc_array(nm + 1, sizeof *t);
    if (t == NULL) {
        free(m);
        return false;
    }
    for (i = 0; i < nm; i++) {
        if (clip != NULL) {
            /* Omitted reads are transparent; keep the complete kernel's coefficients. */
            pc_rect source = clip[0], o = clip[1];
            if ((int64_t)o.x0 + m[i].x >= source.x1 || (int64_t)o.x1 - 1 + m[i].x < source.x0 ||
                (int64_t)o.y0 + m[i].y >= source.y1 || (int64_t)o.y1 - 1 + m[i].y < source.y0) {
                continue;
            }
        }
        t[k].x = m[i].x;
        t[k].y = m[i].y;
        t[k].weight = (double)m[i].w;
        k++;
    }
    free(m);
    *out = t;
    *n = k;
    return true;
}

static void tap_bounds(const tap64 *t, size_t n, int32_t *min_x, int32_t *max_x, int32_t *min_y,
                       int32_t *max_y)
{
    size_t i;
    *min_x = *max_x = *min_y = *max_y = 0;
    for (i = 0; i < n; i++) {
        *min_x = t[i].x < *min_x ? t[i].x : *min_x;
        *max_x = t[i].x > *max_x ? t[i].x : *max_x;
        *min_y = t[i].y < *min_y ? t[i].y : *min_y;
        *max_y = t[i].y > *max_y ? t[i].y : *max_y;
    }
}

static size_t ilog2(size_t v)
{
    size_t r = 0;
    while (v > 1) {
        v >>= 1;
        r++;
    }
    return r;
}

static size_t div_ceil(size_t a, size_t b)
{
    return a / b + (a % b != 0 ? 1u : 0u);
}

#define CK(expr)                                                                                             \
    do {                                                                                                     \
        if (!(expr)) {                                                                                       \
            *ok = false;                                                                                     \
            return false;                                                                                    \
        }                                                                                                    \
    } while (0)

bool pc_algo_motion_worthwhile(float angle, float distance, size_t tile, pc_rect area, size_t row_tile,
                               size_t channels, size_t workers, bool *ok)
{
    size_t pairs, sx, sy, width, height, points, ow, oh, tiles, row_tiles, row_workers, source, result,
        scratch;
    size_t worker, resident, metadata, available, memory_workers, fft_workers, transform_work, row_work, t1,
        t2;
    int32_t min_x, max_x, min_y, max_y;
    tap64 *taps;
    size_t ntaps;
    bool parallel;
    *ok = true;
    if (workers < 4 || channels < 1 || channels > 5 || row_tile == 0) {
        return false;
    }
    pairs = div_ceil(channels, 2);
    /* Transforming a channel pair costs almost the same as one channel. */
    if (!isfinite(distance) || fabsf(distance) * (float)channels < 1500.0f * (float)pairs) {
        return false;
    }
    CK(tile > 0);
    CK(fft_taps(angle, distance, NULL, &taps, &ntaps));
    tap_bounds(taps, ntaps, &min_x, &max_x, &min_y, &max_y);
    free(taps);
    sx = (size_t)((int64_t)max_x - min_x);
    sy = (size_t)((int64_t)max_y - min_y);
    width = fft_size(tile + sx);
    height = fft_size(tile + sy);
    CK(width != 0 && height != 0);
    points = width * height;
    if (points > MAX_POINTS) {
        return false;
    }
    CK(dimensions(area, &ow, &oh));
    CK(pc_size_mul(div_ceil(ow, tile), div_ceil(oh, tile), &tiles));
    CK(pc_size_mul(div_ceil(ow, row_tile), div_ceil(oh, row_tile), &row_tiles));
    row_workers = workers < row_tiles ? workers : row_tiles;
    CK(pc_size_mul3(tile + sx, tile + sy, channels, &t1) && pc_size_mul(t1, 4, &source));
    CK(pc_size_mul3(tile, tile, channels, &result));
    scratch = (width > height ? width : height) * 4;
    CK(pc_size_mul(points, 2, &t1) && pc_size_add(t1, scratch, &t1) && pc_size_mul(t1, 16, &t1) &&
       pc_size_add(t1, source, &t1) && pc_size_mul(result, 8, &t2) && pc_size_add(t1, t2, &worker));
    CK(pc_size_mul(source, 2, &t1));
    worker = worker > t1 ? worker : t1;
    CK(pc_size_mul(points, 16, &resident) && pc_size_mul(width + height, 256, &t1) &&
       pc_size_add(resident, t1, &resident) && pc_size_mul(ntaps, sizeof(tap64), &t1) &&
       pc_size_add(resident, t1, &resident));
    CK(pc_size_mul(tiles, 4 * sizeof(int32_t) * 2, &metadata));
    CK(((size_t)512 << 20) >= resident && ((size_t)512 << 20) - resident >= metadata);
    available = ((size_t)512 << 20) - resident - metadata;
    CK(pc_size_mul(result, 4, &t1) && pc_size_add(worker, t1, &t1) && t1 > 0);
    memory_workers = available / t1;
    fft_workers = workers < memory_workers ? workers : memory_workers;
    fft_workers = fft_workers < tiles ? fft_workers : tiles;
    /* A few large diagonal transforms can lose to well-parallelised rows. */
    CK(pc_size_mul(row_workers, 2, &t1));
    CK(pc_size_mul(fft_workers, 2, &t2));
    parallel = fft_workers >= t1 || (fft_workers >= 4 && t2 >= row_workers);
    CK(pc_size_mul3(points, ilog2(width) + ilog2(height), tiles, &transform_work));
    CK(pc_size_mul3(ow, oh, ntaps, &row_work));
    return parallel && transform_work < row_work;
}

/* Blocked transpose of a height x width matrix of complex values into width x height. */
static bool transpose(const double *src, double *dst, size_t width, size_t height, const pc_interrupt *ctl)
{
    size_t by, bx, y, x;
    for (by = 0; by < height; by += 32) {
        if (pc_interrupt_cancelled(ctl)) {
            return false;
        }
        for (bx = 0; bx < width; bx += 32) {
            size_t ye = by + 32 < height ? by + 32 : height, xe = bx + 32 < width ? bx + 32 : width;
            for (y = by; y < ye; y++) {
                for (x = bx; x < xe; x++) {
                    dst[2 * (x * height + y)] = src[2 * (y * width + x)];
                    dst[2 * (x * height + y) + 1] = src[2 * (y * width + x) + 1];
                }
            }
        }
    }
    return true;
}

static bool transform(const pc_algo_motion_plan *p, double *data, double *transposed, bool inverse,
                      const pc_interrupt *ctl)
{
    size_t i;
    for (i = 0; i < p->height; i++) {
        if (i % 8 == 0 && pc_interrupt_cancelled(ctl)) {
            return false;
        }
        if ((inverse ? cfft_backward(p->row, data + 2 * i * p->width, 1.0)
                     : cfft_forward(p->row, data + 2 * i * p->width, 1.0)) != 0) {
            return false;
        }
    }
    if (!transpose(data, transposed, p->width, p->height, ctl)) {
        return false;
    }
    for (i = 0; i < p->width; i++) {
        if (i % 8 == 0 && pc_interrupt_cancelled(ctl)) {
            return false;
        }
        if ((inverse ? cfft_backward(p->col, transposed + 2 * i * p->height, 1.0)
                     : cfft_forward(p->col, transposed + 2 * i * p->height, 1.0)) != 0) {
            return false;
        }
    }
    return transpose(transposed, data, p->height, p->width, ctl);
}

void pc_algo_motion_plan_free(pc_algo_motion_plan *p)
{
    if (p == NULL) {
        return;
    }
    if (p->row != NULL) {
        destroy_cfft_plan(p->row);
    }
    if (p->col != NULL) {
        destroy_cfft_plan(p->col);
    }
    free(p->kernel);
    free(p->taps);
    free(p);
}

pc_algo_motion_plan *pc_algo_motion_plan_new(float angle, float distance, size_t tile_w, size_t tile_h,
                                             const pc_rect clip[2], const pc_interrupt *ctl)
{
    pc_algo_motion_plan *p;
    double *transposed;
    size_t count, i;
    if (pc_interrupt_cancelled(ctl)) {
        return NULL;
    }
    if (tile_w == 0 || tile_h == 0 || tile_w > 512 || tile_h > 512) {
        return NULL;
    }
    p = calloc(1, sizeof *p);
    if (p == NULL) {
        return NULL;
    }
    p->distance = distance;
    p->tile_width = tile_w;
    p->tile_height = tile_h;
    if (!fft_taps(angle, distance, clip, &p->taps, &p->ntaps)) {
        free(p);
        return NULL;
    }
    tap_bounds(p->taps, p->ntaps, &p->min_x, &p->max_x, &p->min_y, &p->max_y);
    p->width = fft_size(tile_w + (size_t)((int64_t)p->max_x - p->min_x));
    p->height = fft_size(tile_h + (size_t)((int64_t)p->max_y - p->min_y));
    if (p->width == 0 || p->height == 0 || (count = p->width * p->height) > MAX_POINTS) {
        pc_algo_motion_plan_free(p);
        return NULL;
    }
    p->kernel = buffer(count, 2 * sizeof(double));
    if (p->kernel == NULL) {
        pc_algo_motion_plan_free(p);
        return NULL;
    }
    for (i = 0; i < p->ntaps; i++) {
        /* Convolution reverses the displacement of a correlation tap. */
        int64_t w = (int64_t)p->width, h = (int64_t)p->height;
        int64_t x = ((-(int64_t)p->taps[i].x) % w + w) % w, y = ((-(int64_t)p->taps[i].y) % h + h) % h;
        p->kernel[2 * ((size_t)y * p->width + (size_t)x)] += p->taps[i].weight;
    }
    p->row = make_cfft_plan(p->width);
    p->col = make_cfft_plan(p->height);
    /* rustfft's in-place scratch; pocketfft allocates its own per call. The reference's estimate
     * only sizes groups, so a conservative bound keeps the same decisions in practice. */
    p->scratch_len = (p->width > p->height ? p->width : p->height) * 4;
    transposed = buffer(count, 2 * sizeof(double));
    if (p->row == NULL || p->col == NULL || transposed == NULL ||
        !transform(p, p->kernel, transposed, false, ctl)) {
        free(transposed);
        pc_algo_motion_plan_free(p);
        return NULL;
    }
    free(transposed);
    return p;
}

bool pc_algo_motion_plan_resident_bytes(const pc_algo_motion_plan *p, size_t *out)
{
    /* Includes an allowance for the FFT plans' twiddle tables. */
    *out = p->width * p->height * 16 + p->ntaps * sizeof(tap64) + (p->width + p->height) * 256;
    return true;
}

bool pc_algo_motion_plan_worker_bytes(const pc_algo_motion_plan *p, size_t channels, size_t tile, size_t *out)
{
    size_t source, t1, t2;
    if (!pc_size_mul3(tile + (size_t)((int64_t)p->max_x - p->min_x),
                      tile + (size_t)((int64_t)p->max_y - p->min_y), channels, &source) ||
        !pc_size_mul(source, 4, &source) || !pc_size_mul(p->width * p->height, 2, &t1) ||
        !pc_size_add(t1, p->scratch_len, &t1) || !pc_size_mul(t1, 16, &t1) || !pc_size_add(t1, source, &t1) ||
        !pc_size_mul3(tile, tile, channels, &t2) || !pc_size_mul(t2, 8, &t2) || !pc_size_add(t1, t2, out)) {
        return false;
    }
    return true;
}

bool pc_algo_motion_plan_read_rect(const pc_algo_motion_plan *p, pc_rect out, pc_rect *read)
{
    int64_t x0 = (int64_t)out.x0 + p->min_x, y0 = (int64_t)out.y0 + p->min_y;
    int64_t x1 = (int64_t)out.x1 + p->max_x, y1 = (int64_t)out.y1 + p->max_y;
    if (x0 < INT32_MIN || y0 < INT32_MIN || x1 > INT32_MAX || y1 > INT32_MAX) {
        return false;
    }
    *read = pc_rect_make((int32_t)x0, (int32_t)y0, (int32_t)x1, (int32_t)y1);
    return true;
}

/* sample_row_point: one pixel of the row kernel, in its f32 order, without a square window. */
static bool sample_row_point(const pc_algo_image *src, int32_t x, int32_t y, bool alpha, const tap64 *taps,
                             size_t ntaps, const pc_interrupt *ctl, float out[5])
{
    size_t w, h, i, c, n = src->ch, len;
    if (!dimensions(src->rect, &w, &h) || n < 1 || n > 5 || !pc_size_mul3(w, h, n, &len) || len != src->len ||
        !pc_algo_motion_ordinary_coordinates(src->rect)) {
        return false;
    }
    for (c = 0; c < 5; c++) {
        out[c] = 0.0f;
    }
    for (i = 0; i < ntaps; i++) {
        int64_t sx = (int64_t)x + taps[i].x, sy = (int64_t)y + taps[i].y;
        float a, weight;
        if (i % 64 == 0 && pc_interrupt_cancelled(ctl)) {
            return false;
        }
        if (sx < INT32_MIN || sx > INT32_MAX || sy < INT32_MIN || sy > INT32_MAX) {
            return false;
        }
        a = alpha ? pc_algo_image_get(src, (int32_t)sx, (int32_t)sy, n - 1) : 1.0f;
        /* Each stored coefficient came from f32 and converts back exactly. */
        weight = (float)taps[i].weight;
        for (c = 0; c < n; c++) {
            float sample = pc_algo_image_get(src, (int32_t)sx, (int32_t)sy, c);
            float pm = alpha && c + 1 < n ? sample * a : sample;
            out[c] += weight * pm;
        }
    }
    if (alpha) {
        float a = out[n - 1];
        for (c = 0; c + 1 < n; c++) {
            out[c] = a > 1e-7f ? out[c] / a : 0.0f;
        }
    }
    return true;
}

bool pc_algo_motion_row_point(const pc_algo_image *src, int32_t x, int32_t y, bool alpha, float angle,
                              float distance, float out[5])
{
    tap64 *taps;
    size_t n;
    bool ok;
    if (!fft_taps(angle, distance, NULL, &taps, &n)) {
        return false;
    }
    ok = sample_row_point(src, x, y, alpha, taps, n, NULL, out);
    free(taps);
    return ok;
}

bool pc_algo_motion_plan_filter(const pc_algo_motion_plan *p, const pc_algo_image *src, pc_rect out,
                                bool alpha, const pc_interrupt *ctl, float **data_out, size_t *len_out)
{
    size_t ow, oh, sw, sh, n = src->ch, len, i, c, count, ch;
    double scales[5] = {0, 0, 0, 0, 0}, norm, *result = NULL, *data = NULL, *transposed = NULL;
    float *output = NULL;
    pc_rect read, used;
    bool ok = false;
    int32_t y;
    if (!dimensions(out, &ow, &oh)) {
        return false;
    }
    if (pc_rect_is_empty(out)) {
        *data_out = NULL;
        *len_out = 0;
        return true;
    }
    if (!dimensions(src->rect, &sw, &sh) || n < 1 || n > 5 || !pc_size_mul3(sw, sh, n, &len) ||
        len != src->len || ow > p->tile_width || oh > p->tile_height ||
        !pc_algo_motion_ordinary_coordinates(src->rect) || !pc_algo_motion_ordinary_coordinates(out)) {
        return false;
    }
    for (i = 0; i < sw * sh; i++) {
        const float *px = src->data + i * n;
        float a;
        if (i % 4096 == 0 && pc_interrupt_cancelled(ctl)) {
            return false;
        }
        for (c = 0; c < n; c++) {
            if (!isfinite(px[c]) || fabsf(px[c]) > MAX_SAMPLE) {
                return false;
            }
        }
        if (alpha && !(px[n - 1] >= 0.0f && px[n - 1] <= 1.0f)) {
            return false;
        }
        a = alpha ? px[n - 1] : 1.0f;
        for (c = 0; c < n; c++) {
            float v = alpha && c + 1 < n ? px[c] * a : px[c];
            scales[c] = pc_rs_max(scales[c], (double)fabsf(v));
        }
    }
    len = ow * oh * n;
    result = buffer(len, sizeof(double));
    if (result == NULL || !pc_algo_motion_plan_read_rect(p, out, &read)) {
        goto done;
    }
    used = pc_rect_intersect(read, src->rect);
    {
        bool all_zero = true;
        for (c = 0; c < n; c++) {
            all_zero = all_zero && scales[c] == 0.0;
        }
        if (pc_rect_is_empty(used) || all_zero) {
            output = buffer(len, sizeof(float));
            ok = output != NULL;
            goto done;
        }
    }
    count = p->width * p->height;
    data = buffer(count, 2 * sizeof(double));
    transposed = buffer(count, 2 * sizeof(double));
    if (data == NULL || transposed == NULL) {
        goto done;
    }
    norm = 1.0 / (double)count;
    for (ch = 0; ch < n; ch += 2) {
        size_t x0 = (size_t)(-(int64_t)p->min_x), y0 = (size_t)(-(int64_t)p->min_y), oy, ox;
        if (pc_interrupt_cancelled(ctl)) {
            goto done;
        }
        memset(data, 0, count * 2 * sizeof(double));
        for (y = used.y0; y < used.y1; y++) {
            size_t ss =
                ((size_t)((int64_t)y - src->rect.y0) * sw + (size_t)((int64_t)used.x0 - src->rect.x0)) * n;
            size_t start = (size_t)((int64_t)y - read.y0) * p->width + (size_t)((int64_t)used.x0 - read.x0);
            size_t uw = pc_rect_width(used), k;
            if ((y - used.y0) % 8 == 0 && pc_interrupt_cancelled(ctl)) {
                goto done;
            }
            for (k = 0; k < uw; k++) {
                const float *px = src->data + ss + k * n;
                float a = alpha ? px[n - 1] : 1.0f;
                double v[2];
                int q;
                /* Normalise each channel before packing a pair, so a bright HDR colour's FFT
                 * residual can't swamp a small alpha sharing the imaginary part. */
                for (q = 0; q < 2; q++) {
                    size_t cq = ch + (size_t)q;
                    v[q] = cq < n && scales[cq] > 0.0
                               ? (double)(alpha && cq + 1 < n ? px[cq] * a : px[cq]) / scales[cq]
                               : 0.0;
                }
                data[2 * (start + k)] = v[0];
                data[2 * (start + k) + 1] = v[1];
            }
        }
        if (!transform(p, data, transposed, false, ctl)) {
            goto done;
        }
        for (i = 0; i < count; i++) {
            double ar = data[2 * i], ai = data[2 * i + 1], br = p->kernel[2 * i], bi = p->kernel[2 * i + 1];
            data[2 * i] = ar * br - ai * bi;
            data[2 * i + 1] = ar * bi + ai * br;
        }
        if (!transform(p, data, transposed, true, ctl)) {
            goto done;
        }
        for (oy = 0; oy < oh; oy++) {
            size_t start = (oy + y0) * p->width + x0;
            for (ox = 0; ox < ow; ox++) {
                double *px = result + (oy * ow + ox) * n;
                px[ch] = data[2 * (start + ox)] * norm * scales[ch];
                if (ch + 1 < n) {
                    px[ch + 1] = data[2 * (start + ox) + 1] * norm * scales[ch + 1];
                }
            }
        }
    }
    if (alpha) {
        /* The row kernel accumulates in f32: recompute very small alpha near its cutoff so a
         * different summation order can't expose colour. */
        double uncertainty = (double)ceilf(fabsf(p->distance)) * (double)1.1920929e-07f * scales[n - 1];
        for (i = 0; i < ow * oh; i++) {
            double *px = result + i * n, a;
            float af;
            if (i % 16 == 0 && pc_interrupt_cancelled(ctl)) {
                goto done;
            }
            /* Valid alpha is non-negative; remove transform round-off at zero. */
            if (fabs(px[n - 1]) < 1e-14 * scales[n - 1]) {
                px[n - 1] = 0.0;
            }
            px[n - 1] = pc_clampd(px[n - 1], 0.0, 1.0);
            a = px[n - 1];
            af = (float)a;
            if (a > 0.0 && (a <= 1.01e-7 + uncertainty || (af >= 0.99e-7f && af <= 1.01e-7f))) {
                float orig[5];
                int32_t x = (int32_t)((int64_t)out.x0 + (int64_t)(i % ow));
                int32_t yy = (int32_t)((int64_t)out.y0 + (int64_t)(i / ow));
                if (!sample_row_point(src, x, yy, alpha, p->taps, p->ntaps, ctl, orig)) {
                    goto done;
                }
                for (c = 0; c < n; c++) {
                    px[c] = (double)orig[c];
                }
                continue;
            }
            for (c = 0; c + 1 < n; c++) {
                px[c] = (float)a > 1e-7f ? px[c] / a : 0.0;
            }
        }
    }
    output = buffer(len, sizeof(float));
    if (output == NULL) {
        goto done;
    }
    for (i = 0; i < len; i++) {
        output[i] = (float)result[i];
    }
    ok = true;
done:
    free(result);
    free(data);
    free(transposed);
    if (!ok) {
        free(output);
        return false;
    }
    *data_out = output;
    *len_out = len;
    return true;
}

bool pc_algo_motion_fft_filter(const pc_algo_image *src, pc_rect out, bool alpha, float angle, float distance,
                               float **data, size_t *len)
{
    pc_rect clip[2];
    size_t w, h;
    pc_algo_motion_plan *p;
    bool ok;
    if (pc_rect_is_empty(out)) {
        *data = NULL;
        *len = 0;
        return true;
    }
    if (!dimensions(out, &w, &h)) {
        return false;
    }
    clip[0] = src->rect;
    clip[1] = out;
    p = pc_algo_motion_plan_new(angle, distance, w, h, clip, NULL);
    if (p == NULL) {
        return false;
    }
    ok = pc_algo_motion_plan_filter(p, src, out, alpha, NULL, data, len);
    pc_algo_motion_plan_free(p);
    return ok;
}
