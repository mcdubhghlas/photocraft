/* Interleaved float image regions (port of crates/algo/src/image.rs). */
#include "algo/image.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"

bool pc_algo_float_count(pc_rect r, size_t ch, size_t *out)
{
    size_t n;
    if (!pc_size_mul3(pc_rect_width(r), pc_rect_height(r), ch, &n) || n > PC_MAX_ALLOC / sizeof(float)) {
        return false;
    }
    *out = n;
    return true;
}

pc_status pc_algo_image_init(pc_algo_image *img, pc_rect rect, size_t ch, pc_error *err)
{
    size_t n;
    float *d = NULL;
    if (!pc_algo_float_count(rect, ch, &n)) {
        return pc_fail(err, PC_ERR_LIMIT, "image of %ux%u x %zu channels is too large", pc_rect_width(rect),
                       pc_rect_height(rect), ch);
    }
    if (n > 0) {
        d = pc_calloc_array(n, sizeof(float));
        if (d == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for a %zu-sample image", n);
        }
    }
    img->rect = rect;
    img->ch = ch;
    img->data = d;
    img->len = n;
    return PC_OK;
}

pc_status pc_algo_image_wrap(pc_algo_image *img, pc_rect rect, size_t ch, float *data, size_t len,
                             pc_error *err)
{
    size_t n;
    if (!pc_algo_float_count(rect, ch, &n) || n != len) {
        return pc_fail(err, PC_ERR_INVALID, "image data has %zu samples, expected %ux%u x %zu", len,
                       pc_rect_width(rect), pc_rect_height(rect), ch);
    }
    img->rect = rect;
    img->ch = ch;
    img->data = n > 0 ? data : NULL;
    img->len = n;
    if (n == 0) {
        free(data);
    }
    return PC_OK;
}

void pc_algo_image_fini(pc_algo_image *img)
{
    if (img == NULL) {
        return;
    }
    free(img->data);
    img->data = NULL;
    img->len = 0;
    img->rect = PC_RECT_EMPTY;
}

pc_status pc_algo_image_clone(const pc_algo_image *src, pc_algo_image *out, pc_error *err)
{
    float *d = NULL;
    if (src->len > 0) {
        d = pc_malloc_array(src->len, sizeof(float));
        if (d == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory copying an image");
        }
        memcpy(d, src->data, src->len * sizeof(float));
    }
    *out = *src;
    out->data = d;
    return PC_OK;
}

pc_status pc_algo_image_read(const pc_surface *s, pc_rect rect, pc_algo_image *out, pc_error *err)
{
    float *d = NULL;
    size_t len = 0;
    size_t ch = pc_surface_channels(s);
    pc_status st = pc_surface_read_region(s, rect, &d, &len, err);
    if (st != PC_OK) {
        return st;
    }
    out->rect = rect;
    out->ch = ch;
    out->data = d;
    out->len = len;
    return PC_OK;
}

pc_status pc_algo_image_read_clamped(const pc_surface *s, pc_rect rect, pc_rect extent, pc_algo_image *out,
                                     pc_error *err)
{
    pc_rect inner = pc_rect_intersect(rect, extent);
    pc_algo_image src, img;
    size_t ch, w;
    int32_t x, y;
    pc_status st;
    if (pc_rect_is_empty(inner) || pc_rect_eq(inner, rect)) {
        return pc_algo_image_read(s, rect, out, err);
    }
    st = pc_algo_image_read(s, inner, &src, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_algo_image_init(&img, rect, src.ch, err);
    if (st != PC_OK) {
        pc_algo_image_fini(&src);
        return st;
    }
    ch = src.ch;
    w = pc_rect_width(rect);
    for (y = rect.y0; y < rect.y1; y++) {
        int32_t sy = pc_clamp_i32(y, inner.y0, inner.y1 - 1);
        size_t row = (size_t)((int64_t)y - rect.y0) * w * ch;
        for (x = rect.x0; x < rect.x1; x++) {
            int32_t sx = pc_clamp_i32(x, inner.x0, inner.x1 - 1);
            size_t d = row + (size_t)((int64_t)x - rect.x0) * ch;
            const float *p = pc_algo_image_px(&src, sx, sy);
            if (p != NULL && ch > 0 && img.data != NULL && d + ch <= img.len) {
                memcpy(img.data + d, p, ch * sizeof(float));
            }
        }
    }
    pc_algo_image_fini(&src);
    *out = img;
    return PC_OK;
}

/* Index of (x, y) channel 0; (x, y) must be inside rect. */
static size_t index_of(const pc_algo_image *img, int32_t x, int32_t y)
{
    size_t w = pc_rect_width(img->rect);
    return ((size_t)((int64_t)y - img->rect.y0) * w + (size_t)((int64_t)x - img->rect.x0)) * img->ch;
}

float pc_algo_image_get(const pc_algo_image *img, int32_t x, int32_t y, size_t c)
{
    size_t i;
    if (!pc_rect_contains(img->rect, x, y)) {
        return 0.0f;
    }
    i = index_of(img, x, y) + c;
    return img->data != NULL && i < img->len ? img->data[i] : 0.0f;
}

const float *pc_algo_image_px(const pc_algo_image *img, int32_t x, int32_t y)
{
    size_t i;
    if (!pc_rect_contains(img->rect, x, y)) {
        return NULL;
    }
    i = index_of(img, x, y);
    return img->data != NULL && i + img->ch <= img->len ? img->data + i : NULL;
}

const float *pc_algo_image_row(const pc_algo_image *img, pc_rect r)
{
    size_t i;
    if (pc_rect_is_empty(r) || !pc_rect_contains_rect(img->rect, r)) {
        return NULL;
    }
    i = index_of(img, r.x0, r.y0);
    if (img->data == NULL) {
        return NULL;
    }
    return i + (size_t)pc_rect_width(r) * img->ch <= img->len ? img->data + i : NULL;
}

/* Rust's i32::rem_euclid (m != 0; computed in 64 bits so i32::MIN % -1 is defined). */
static int32_t rem_euclid_i32(int32_t a, int32_t m)
{
    int64_t r = (int64_t)a % (int64_t)m;
    if (r < 0) {
        r += m < 0 ? -(int64_t)m : (int64_t)m;
    }
    return (int32_t)r;
}

float pc_algo_image_get_edge(const pc_algo_image *img, int32_t x, int32_t y, size_t c, pc_algo_edge edge,
                             pc_rect area)
{
    if (pc_rect_is_empty(area)) {
        return 0.0f;
    }
    if (pc_rect_contains(area, x, y)) {
        return pc_algo_image_get(img, x, y, c);
    }
    switch (edge) {
    case PC_ALGO_EDGE_TRANSPARENT:
        return 0.0f;
    case PC_ALGO_EDGE_REPEAT:
        return pc_algo_image_get(img, pc_clamp_i32(x, area.x0, area.x1 - 1),
                                 pc_clamp_i32(y, area.y0, area.y1 - 1), c);
    case PC_ALGO_EDGE_WRAP: {
        /* `area.width().max(1) as i32` (wraps for widths past i32::MAX, like the reference). */
        uint32_t uw = pc_rect_width(area), uh = pc_rect_height(area);
        int32_t w = pc_u32_as_i32(uw > 0 ? uw : 1), h = pc_u32_as_i32(uh > 0 ? uh : 1);
        int32_t rx = rem_euclid_i32(pc_i32_wrapping_sub(x, area.x0), w);
        int32_t ry = rem_euclid_i32(pc_i32_wrapping_sub(y, area.y0), h);
        return pc_algo_image_get(img, pc_i32_wrapping_add(area.x0, rx), pc_i32_wrapping_add(area.y0, ry), c);
    }
    }
    return 0.0f;
}

void pc_algo_image_sample(const pc_algo_image *img, float x, float y, pc_algo_edge edge, pc_rect area,
                          bool alpha, float *out, size_t out_len)
{
    static const int32_t DX[4] = {0, 1, 0, 1}, DY[4] = {0, 0, 1, 1};
    float fx = x - 0.5f, fy = y - 0.5f;
    float x0f = floorf(fx), y0f = floorf(fy);
    float ax = fx - x0f, ay = fy - y0f;
    int32_t x0 = pc_f32_to_i32(x0f), y0 = pc_f32_to_i32(y0f);
    size_t n = img->ch, i, c, take;
    float wgt[4];
    float acc_a = 0.0f;
    wgt[0] = (1.0f - ax) * (1.0f - ay);
    wgt[1] = ax * (1.0f - ay);
    wgt[2] = (1.0f - ax) * ay;
    wgt[3] = ax * ay;
    for (i = 0; i < out_len; i++) {
        out[i] = 0.0f;
    }
    if (alpha && n == 0) {
        return;
    }
    take = alpha ? n - 1 : n;
    if (take > out_len) {
        take = out_len;
    }
    for (i = 0; i < 4; i++) {
        float a;
        int32_t sx = pc_i32_wrapping_add(x0, DX[i]), sy = pc_i32_wrapping_add(y0, DY[i]);
        if (wgt[i] <= 0.0f) {
            continue;
        }
        a = alpha ? pc_algo_image_get_edge(img, sx, sy, n - 1, edge, area) : 1.0f;
        for (c = 0; c < take; c++) {
            out[c] += pc_algo_image_get_edge(img, sx, sy, c, edge, area) * a * wgt[i];
        }
        acc_a += a * wgt[i];
    }
    if (alpha) {
        if (acc_a > 0.0f) {
            for (c = 0; c < n - 1 && c < out_len; c++) {
                out[c] /= acc_a;
            }
        }
        if (n - 1 < out_len) {
            out[n - 1] = acc_a;
        }
    }
}

void pc_algo_image_crop_into(const pc_algo_image *img, pc_rect out, float *dst)
{
    size_t k = 0, c;
    int32_t x, y;
    for (y = out.y0; y < out.y1; y++) {
        for (x = out.x0; x < out.x1; x++) {
            const float *p = pc_algo_image_px(img, x, y);
            for (c = 0; c < img->ch; c++) {
                dst[k++] = p != NULL ? p[c] : 0.0f;
            }
        }
    }
}

pc_status pc_algo_image_crop(const pc_algo_image *img, pc_rect out, float **data, size_t *len, pc_error *err)
{
    size_t n;
    float *d = NULL;
    if (pc_rect_is_empty(out)) {
        out = PC_RECT_EMPTY;
    }
    if (!pc_algo_float_count(out, img->ch, &n)) {
        return pc_fail(err, PC_ERR_LIMIT, "crop of %ux%u is too large", pc_rect_width(out),
                       pc_rect_height(out));
    }
    if (n > 0) {
        d = pc_malloc_array(n, sizeof(float));
        if (d == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for a %zu-sample crop", n);
        }
        pc_algo_image_crop_into(img, out, d);
    }
    *data = d;
    *len = n;
    return PC_OK;
}

void pc_algo_premultiply(float *data, size_t len, size_t ch, bool alpha)
{
    size_t i, c;
    if (!alpha || ch == 0) {
        return;
    }
    for (i = 0; i + ch <= len; i += ch) {
        float a = data[i + ch - 1];
        for (c = 0; c + 1 < ch; c++) {
            data[i + c] *= a;
        }
    }
}

void pc_algo_unpremultiply(float *data, size_t len, size_t ch, bool alpha)
{
    size_t i, c;
    if (!alpha || ch == 0) {
        return;
    }
    for (i = 0; i + ch <= len; i += ch) {
        float a = data[i + ch - 1];
        for (c = 0; c + 1 < ch; c++) {
            data[i + c] = a > 1e-7f ? data[i + c] / a : 0.0f;
        }
    }
}
