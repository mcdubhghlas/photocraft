/* Grayscale tiles, the tile base64, mip pyramids and tiling patterns (tile.h). */
#include "paint/tile.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "paint/pmath.h"

struct pc_paint_tile_data {
    atomic_size_t refs;
    size_t len;
    uint16_t samples[];
};

uint16_t pc_paint_q16(float v)
{
    return pc_f32_to_u16(pc_clampf(v, 0.0f, 1.0f) * 65535.0f + 0.5f);
}

/* A block for n samples with one reference; NULL on overflow, cap or out of memory. */
static pc_paint_tile_data *data_new(size_t n, pc_status *why)
{
    size_t bytes;
    pc_paint_tile_data *d;
    if (!pc_size_mul(n, sizeof(uint16_t), &bytes) || !pc_size_add(bytes, sizeof *d, &bytes) ||
        bytes > PC_MAX_ALLOC) {
        *why = PC_ERR_LIMIT;
        return NULL;
    }
    d = pc_malloc(bytes);
    if (d == NULL) {
        *why = PC_ERR_NOMEM;
        return NULL;
    }
    atomic_init(&d->refs, 1);
    d->len = n;
    return d;
}

static void data_release(pc_paint_tile_data *d)
{
    if (d != NULL && atomic_fetch_sub_explicit(&d->refs, 1, memory_order_acq_rel) == 1) {
        free(d);
    }
}

static pc_status fail_alloc(pc_status why, pc_error *err)
{
    if (why == PC_ERR_LIMIT) {
        return pc_fail(err, PC_ERR_LIMIT, "gray tile too large");
    }
    return pc_fail(err, PC_ERR_NOMEM, "out of memory for a gray tile");
}

pc_status pc_paint_gray_tile_from_fn(uint32_t width, uint32_t height, pc_paint_tile_fn f, void *ctx,
                                     pc_paint_gray_tile *out, pc_error *err)
{
    size_t n;
    pc_status why = PC_ERR_NOMEM;
    pc_paint_tile_data *d;
    if (!pc_size_mul(width, height, &n)) {
        return fail_alloc(PC_ERR_LIMIT, err);
    }
    d = data_new(n, &why);
    if (d == NULL) {
        return fail_alloc(why, err);
    }
    for (uint32_t y = 0, i = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++, i++) {
            d->samples[i] = pc_paint_q16(f(ctx, x, y));
        }
    }
    out->width = width;
    out->height = height;
    out->data = d;
    return PC_OK;
}

pc_status pc_paint_gray_tile_from_f32(uint32_t width, uint32_t height, const float *v, size_t n,
                                      pc_paint_gray_tile *out, pc_error *err)
{
    size_t want;
    pc_status why = PC_ERR_NOMEM;
    pc_paint_tile_data *d;
    if (!pc_size_mul(width, height, &want) || want != n) {
        return pc_fail(err, PC_ERR_INVALID, "GrayTile size mismatch");
    }
    d = data_new(n, &why);
    if (d == NULL) {
        return fail_alloc(why, err);
    }
    for (size_t i = 0; i < n; i++) {
        d->samples[i] = pc_paint_q16(v[i]);
    }
    out->width = width;
    out->height = height;
    out->data = d;
    return PC_OK;
}

pc_status pc_paint_gray_tile_from_u16(uint32_t width, uint32_t height, const uint16_t *v, size_t n,
                                      pc_paint_gray_tile *out, pc_error *err)
{
    pc_status why = PC_ERR_NOMEM;
    pc_paint_tile_data *d = data_new(n, &why);
    if (d == NULL) {
        return fail_alloc(why, err);
    }
    if (n > 0) {
        memcpy(d->samples, v, n * sizeof(uint16_t));
    }
    out->width = width;
    out->height = height;
    out->data = d;
    return PC_OK;
}

void pc_paint_gray_tile_copy(pc_paint_gray_tile *dst, const pc_paint_gray_tile *src)
{
    if (src->data != NULL) {
        atomic_fetch_add_explicit(&src->data->refs, 1, memory_order_relaxed);
    }
    *dst = *src;
}

void pc_paint_gray_tile_fini(pc_paint_gray_tile *t)
{
    if (t == NULL) {
        return;
    }
    data_release(t->data);
    t->data = NULL;
    t->width = 0;
    t->height = 0;
}

size_t pc_paint_gray_tile_len(const pc_paint_gray_tile *t)
{
    return t->data != NULL ? t->data->len : 0;
}

const uint16_t *pc_paint_gray_tile_samples(const pc_paint_gray_tile *t)
{
    return t->data != NULL ? t->data->samples : NULL;
}

float pc_paint_gray_tile_get(const pc_paint_gray_tile *t, uint32_t x, uint32_t y)
{
    /* The reference's u32 index arithmetic wraps in release builds. */
    size_t i = (size_t)(uint32_t)(y * t->width + x);
    if (t->data == NULL || i >= t->data->len) {
        return 0.0f;
    }
    return (float)t->data->samples[i] / 65535.0f;
}

pc_status pc_paint_gray_tile_to_f32(const pc_paint_gray_tile *t, float **out, size_t *n, pc_error *err)
{
    size_t len = pc_paint_gray_tile_len(t);
    float *v = NULL;
    if (len > 0) {
        v = pc_malloc_array(len, sizeof *v);
        if (v == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory converting a gray tile");
        }
        for (size_t i = 0; i < len; i++) {
            v[i] = (float)t->data->samples[i] / 65535.0f;
        }
    }
    *out = v;
    *n = len;
    return PC_OK;
}

bool pc_paint_gray_tile_is_valid(const pc_paint_gray_tile *t)
{
    return t->width > 0 && t->height > 0 && pc_paint_gray_tile_len(t) == (size_t)t->width * t->height;
}

bool pc_paint_gray_tile_eq(const pc_paint_gray_tile *a, const pc_paint_gray_tile *b)
{
    size_t n = pc_paint_gray_tile_len(a);
    if (a->width != b->width || a->height != b->height || n != pc_paint_gray_tile_len(b)) {
        return false;
    }
    return n == 0 || a->data == b->data ||
           memcmp(a->data->samples, b->data->samples, n * sizeof(uint16_t)) == 0;
}

pc_status pc_paint_gray_tile_preview(const pc_paint_gray_tile *t, uint32_t max_side, pc_paint_gray_tile *out,
                                     pc_error *err)
{
    size_t w = t->width, h = t->height, big = w > h ? w : h, m = max_side > 0 ? max_side : 1, f, nw, nh, n;
    uint64_t *sum;
    uint32_t *cnt;
    uint16_t *q;
    pc_status st;
    if (!pc_paint_gray_tile_is_valid(t)) {
        uint16_t zero = 0;
        return pc_paint_gray_tile_from_u16(1, 1, &zero, 1, out, err);
    }
    f = big / m + (big % m != 0);
    f = f > 0 ? f : 1;
    nw = w / f + (w % f != 0);
    nh = h / f + (h % f != 0);
    n = nw * nh; /* <= w * h, which is allocated */
    sum = pc_calloc_array(n, sizeof *sum);
    cnt = pc_calloc_array(n, sizeof *cnt);
    q = pc_malloc_array(n, sizeof *q);
    if (sum == NULL || cnt == NULL || q == NULL) {
        free(sum);
        free(cnt);
        free(q);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a tile preview");
    }
    for (size_t y = 0; y < h; y++) {
        const uint16_t *row = t->data->samples + y * w;
        size_t base = (y / f) * nw;
        for (size_t x = 0; x < w; x++) {
            sum[base + x / f] += row[x];
            cnt[base + x / f]++;
        }
    }
    for (size_t i = 0; i < n; i++) {
        uint32_t v = cnt[i] > 0 ? (uint32_t)(sum[i] / cnt[i]) : 0;
        q[i] = (uint16_t)((v + 128) / 257 * 257);
    }
    st = pc_paint_gray_tile_from_u16((uint32_t)nw, (uint32_t)nh, q, n, out, err);
    free(sum);
    free(cnt);
    free(q);
    return st;
}

/* ---- stored tiles ---- */

pc_status pc_paint_stored_tile_init(const char *key, size_t key_len, uint32_t width, uint32_t height,
                                    const pc_paint_gray_tile *preview, const pc_paint_gray_tile *full,
                                    pc_paint_stored_tile *out, pc_error *err)
{
    pc_blob *k = NULL;
    if (key_len > 0) {
        pc_status st = pc_blob_new(key, key_len, &k, err);
        if (st != PC_OK) {
            return st;
        }
    }
    memset(out, 0, sizeof *out);
    out->key = k;
    out->width = width;
    out->height = height;
    pc_paint_gray_tile_copy(&out->preview, preview);
    if (full != NULL) {
        out->loaded = true;
        pc_paint_gray_tile_copy(&out->full, full);
    }
    return PC_OK;
}

void pc_paint_stored_tile_copy(pc_paint_stored_tile *dst, const pc_paint_stored_tile *src)
{
    *dst = *src;
    dst->key = pc_blob_retain(src->key);
    pc_paint_gray_tile_copy(&dst->preview, &src->preview);
    pc_paint_gray_tile_copy(&dst->full, &src->full);
}

void pc_paint_stored_tile_fini(pc_paint_stored_tile *t)
{
    if (t == NULL) {
        return;
    }
    pc_blob_release(t->key);
    pc_paint_gray_tile_fini(&t->preview);
    pc_paint_gray_tile_fini(&t->full);
    memset(t, 0, sizeof *t);
}

const char *pc_paint_stored_tile_key(const pc_paint_stored_tile *t, size_t *len)
{
    *len = pc_blob_len(t->key);
    return t->key != NULL ? (const char *)pc_blob_data(t->key) : "";
}

bool pc_paint_stored_tile_eq(const pc_paint_stored_tile *a, const pc_paint_stored_tile *b)
{
    size_t la, lb;
    const char *ka = pc_paint_stored_tile_key(a, &la), *kb = pc_paint_stored_tile_key(b, &lb);
    return la == lb && (la == 0 || memcmp(ka, kb, la) == 0) && a->width == b->width && a->height == b->height;
}

const pc_paint_gray_tile *pc_paint_stored_tile_bitmap(const pc_paint_stored_tile *t)
{
    return t->loaded ? &t->full : &t->preview;
}

/* ---- base64 ---- */

static const char B64[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

pc_status pc_paint_b64_encode(const uint8_t *b, size_t n, pc_buf *out, pc_error *err)
{
    size_t groups = n / 3 + (n % 3 != 0), need;
    pc_status st;
    if (!pc_size_mul(groups, 4, &need)) {
        return pc_fail(err, PC_ERR_LIMIT, "base64 output too large");
    }
    st = pc_buf_reserve(out, need, err);
    if (st != PC_OK) {
        return st;
    }
    for (size_t i = 0; i < n; i += 3) {
        size_t m = n - i < 3 ? n - i : 3;
        uint32_t v = (uint32_t)b[i] << 16 | (m > 1 ? (uint32_t)b[i + 1] << 8 : 0) | (m > 2 ? b[i + 2] : 0);
        uint8_t q[4];
        q[0] = (uint8_t)B64[(v >> 18) & 63];
        q[1] = (uint8_t)B64[(v >> 12) & 63];
        q[2] = m > 1 ? (uint8_t)B64[(v >> 6) & 63] : (uint8_t)'=';
        q[3] = m > 2 ? (uint8_t)B64[v & 63] : (uint8_t)'=';
        /* Cannot fail: reserved above. */
        (void)pc_buf_append(out, q, 4, NULL);
    }
    return PC_OK;
}

static int b64_val(uint8_t c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

/* Rust's u8::is_ascii_whitespace: space, \t, \n, \x0C, \r (not \x0B). */
static bool b64_space(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == 0x0C || c == '\r';
}

pc_status pc_paint_b64_decode(const char *s, size_t n, pc_buf *out, pc_error *err)
{
    const uint8_t *u = (const uint8_t *)s;
    uint8_t q[4];
    size_t k = 0, chars = 0;
    pc_buf res = PC_BUF_INIT;
    for (size_t i = 0; i < n; i++) {
        chars += !b64_space(u[i]);
    }
    if (chars % 4 != 0) {
        return pc_fail(err, PC_ERR_FORMAT, "invalid base64");
    }
    if (pc_buf_reserve(&res, chars / 4 * 3, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    for (size_t i = 0; i < n; i++) {
        if (b64_space(u[i])) {
            continue;
        }
        q[k++] = u[i];
        if (k == 4) {
            size_t pad = 0;
            uint32_t v = 0;
            uint8_t o[3];
            while (pad < 4 && q[3 - pad] == '=') {
                pad++;
            }
            for (size_t j = 0; j < 4; j++) {
                int x = 0;
                if (j < 4 - pad) {
                    x = b64_val(q[j]);
                    if (x < 0) {
                        pc_buf_fini(&res);
                        return pc_fail(err, PC_ERR_FORMAT, "invalid base64");
                    }
                }
                v = v << 6 | (uint32_t)x;
            }
            o[0] = (uint8_t)(v >> 16);
            o[1] = (uint8_t)(v >> 8);
            o[2] = (uint8_t)v;
            /* Cannot fail: reserved above. */
            (void)pc_buf_append(&res, o, pad < 2 ? (pad < 1 ? 3 : 2) : 1, NULL);
            k = 0;
        }
    }
    pc_buf_fini(out);
    *out = res;
    return PC_OK;
}

/* ---- mips ---- */

pc_status pc_paint_mips_new(const pc_paint_gray_tile *t, pc_paint_mips **out, pc_error *err)
{
    pc_paint_mips *m;
    size_t cap = 1, w = t->width, h = t->height;
    float *l0 = NULL;
    size_t n0 = 0;
    if (!pc_paint_gray_tile_is_valid(t)) {
        return pc_fail(err, PC_ERR_INVALID, "mips of an invalid tile");
    }
    /* Levels: halve until 1 x 1 (at most ~65 for 32-bit sides). */
    for (size_t a = w, b = h; a > 1 || b > 1; a = (a + 1) / 2, b = (b + 1) / 2) {
        cap++;
    }
    m = calloc(1, sizeof *m);
    if (m == NULL || (m->levels = pc_calloc_array(cap, sizeof *m->levels)) == NULL) {
        free(m);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for brush tip mips");
    }
    atomic_init(&m->refs, 1);
    if (pc_paint_gray_tile_to_f32(t, &l0, &n0, err) != PC_OK) {
        free(m->levels);
        free(m);
        return PC_ERR_NOMEM;
    }
    m->levels[0] = (pc_paint_mip_level){w, h, l0};
    m->count = 1;
    while (m->count < cap) {
        const pc_paint_mip_level *p = &m->levels[m->count - 1];
        size_t nw = (p->width + 1) / 2, nh = (p->height + 1) / 2;
        float *nv = pc_malloc_array(nw * nh, sizeof *nv);
        if (nv == NULL) {
            pc_paint_mips_release(m);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for brush tip mips");
        }
        for (size_t y = 0; y < nh; y++) {
            for (size_t x = 0; x < nw; x++) {
                float s = 0.0f, c = 0.0f;
                for (size_t dy = 0; dy < 2; dy++) {
                    for (size_t dx = 0; dx < 2; dx++) {
                        size_t sx = x * 2 + dx, sy = y * 2 + dy;
                        if (sx < p->width && sy < p->height) {
                            s += p->data[sy * p->width + sx];
                            c += 1.0f;
                        }
                    }
                }
                nv[y * nw + x] = s / c;
            }
        }
        m->levels[m->count++] = (pc_paint_mip_level){nw, nh, nv};
    }
    *out = m;
    return PC_OK;
}

pc_paint_mips *pc_paint_mips_retain(pc_paint_mips *m)
{
    if (m != NULL) {
        atomic_fetch_add_explicit(&m->refs, 1, memory_order_relaxed);
    }
    return m;
}

void pc_paint_mips_release(pc_paint_mips *m)
{
    if (m == NULL || atomic_fetch_sub_explicit(&m->refs, 1, memory_order_acq_rel) != 1) {
        return;
    }
    for (size_t i = 0; i < m->count; i++) {
        free(m->levels[i].data);
    }
    free(m->levels);
    free(m);
}

size_t pc_paint_mips_level_for(const pc_paint_mips *m, float px)
{
    size_t w = m->levels[0].width, h = m->levels[0].height;
    float ratio = pc_paint_maxf_k((float)(w > h ? w : h) / pc_paint_maxf_k(px, 1.0f), 1.0f);
    size_t l = pc_f32_to_size(floorf(log2f(ratio)));
    return l < m->count - 1 ? l : m->count - 1;
}

/* x + 1 with the reference's release-build wrap. */
static int64_t i64_inc(int64_t x)
{
    return x == INT64_MAX ? INT64_MIN : x + 1;
}

static float mip_get(const pc_paint_mip_level *l, int64_t x, int64_t y)
{
    if (x < 0 || y < 0 || (uint64_t)x >= l->width || (uint64_t)y >= l->height) {
        return 0.0f;
    }
    return l->data[(size_t)y * l->width + (size_t)x];
}

float pc_paint_mips_sample_clamped(const pc_paint_mips *m, size_t level, float u, float v)
{
    const pc_paint_mip_level *l;
    float fx, fy, x0f, y0f, tx, ty, a, b;
    int64_t x0, y0;
    if (level >= m->count) {
        return 0.0f;
    }
    l = &m->levels[level];
    fx = u * (float)l->width - 0.5f;
    fy = v * (float)l->height - 0.5f;
    x0f = floorf(fx);
    y0f = floorf(fy);
    tx = fx - x0f;
    ty = fy - y0f;
    x0 = pc_f32_to_i64(x0f);
    y0 = pc_f32_to_i64(y0f);
    a = mip_get(l, x0, y0) + (mip_get(l, i64_inc(x0), y0) - mip_get(l, x0, y0)) * tx;
    b = mip_get(l, x0, i64_inc(y0)) +
        (mip_get(l, i64_inc(x0), i64_inc(y0)) - mip_get(l, x0, i64_inc(y0))) * tx;
    return a + (b - a) * ty;
}

/* ---- patterns ---- */

pc_status pc_paint_pattern_image_new(size_t width, size_t height, pc_paint_pattern_image **out, pc_error *err)
{
    pc_paint_pattern_image *p;
    size_t n, bytes;
    if (width == 0 || height == 0) {
        return pc_fail(err, PC_ERR_INVALID, "empty pattern image");
    }
    if (!pc_size_mul(width, height, &n) || !pc_size_mul(n, sizeof(float), &bytes) || bytes > PC_MAX_ALLOC) {
        return pc_fail(err, PC_ERR_LIMIT, "pattern image too large");
    }
    p = calloc(1, sizeof *p);
    if (p == NULL || (p->data = pc_calloc_array(n, sizeof(float))) == NULL) {
        free(p);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a pattern image");
    }
    p->width = width;
    p->height = height;
    atomic_init(&p->refs, 1);
    *out = p;
    return PC_OK;
}

pc_paint_pattern_image *pc_paint_pattern_image_retain(pc_paint_pattern_image *p)
{
    if (p != NULL) {
        atomic_fetch_add_explicit(&p->refs, 1, memory_order_relaxed);
    }
    return p;
}

void pc_paint_pattern_image_release(pc_paint_pattern_image *p)
{
    if (p == NULL || atomic_fetch_sub_explicit(&p->refs, 1, memory_order_acq_rel) != 1) {
        return;
    }
    free(p->data);
    free(p);
}

static size_t wrap_index(float v, size_t n)
{
    int64_t m = (int64_t)n, r = pc_f32_to_i64(floorf(v)) % m;
    return (size_t)(r < 0 ? r + m : r);
}

float pc_paint_pattern_sample_wrap(const pc_paint_pattern_image *p, float x, float y)
{
    return p->data[wrap_index(y, p->height) * p->width + wrap_index(x, p->width)];
}
