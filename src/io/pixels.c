/* PSD planar <-> interleaved sample shuffles (port of crates/io/src/pixels.rs). */
#include "io/pixels.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"

/* Pixels per pc_pool chunk: large enough to amortise the scheduling. */
#define GRAIN 65536

uint16_t pc_io_psd_depth(pc_sample_type s)
{
    switch (s) {
    case PC_SAMPLE_TYPE_U16:
        return 16;
    case PC_SAMPLE_TYPE_F32:
        return 32;
    default:
        return 8;
    }
}

pc_sample_type pc_io_sample_for_depth(uint16_t depth)
{
    if (depth == 16) {
        return PC_SAMPLE_TYPE_U16;
    }
    if (depth == 32) {
        return PC_SAMPLE_TYPE_F32;
    }
    return PC_SAMPLE_TYPE_U8;
}

/* Native-endian sample <-> integer bits. */
static uint16_t ld16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

static void st16(uint8_t *p, uint16_t v)
{
    memcpy(p, &v, 2);
}

/* Inverts one native-endian sample in place (max - v; 1 - v for floats). */
static void invert_sample(uint8_t *b, pc_sample_type s)
{
    if (s == PC_SAMPLE_TYPE_U8) {
        b[0] = (uint8_t)(255 - b[0]);
    } else if (s == PC_SAMPLE_TYPE_U16) {
        st16(b, (uint16_t)(65535 - ld16(b)));
    } else {
        float f;
        memcpy(&f, b, 4);
        f = 1.0f - f;
        memcpy(b, &f, 4);
    }
}

/* Big-endian bytes of one sample -> native-endian. */
static void be_to_native(const uint8_t *src, uint8_t *dst, size_t bps)
{
    if (bps == 1) {
        dst[0] = src[0];
    } else if (bps == 2) {
        st16(dst, (uint16_t)((unsigned)src[0] << 8 | src[1]));
    } else {
        uint32_t v = (uint32_t)src[0] << 24 | (uint32_t)src[1] << 16 | (uint32_t)src[2] << 8 | src[3];
        memcpy(dst, &v, 4);
    }
}

static void native_to_be(const uint8_t *src, uint8_t *dst, size_t bps)
{
    if (bps == 1) {
        dst[0] = src[0];
    } else if (bps == 2) {
        uint16_t v = ld16(src);
        dst[0] = (uint8_t)(v >> 8);
        dst[1] = (uint8_t)v;
    } else {
        uint32_t v;
        memcpy(&v, src, 4);
        dst[0] = (uint8_t)(v >> 24);
        dst[1] = (uint8_t)(v >> 16);
        dst[2] = (uint8_t)(v >> 8);
        dst[3] = (uint8_t)v;
    }
}

typedef struct il_ctx {
    const pc_io_plane *planes;
    size_t ch;
    const uint8_t (*fill)[4];
    pc_sample_type s;
    size_t bps;
    const bool *invert;
    uint8_t *out;
} il_ctx;

static pc_status il_chunk(void *vctx, size_t chunk, size_t begin, size_t end)
{
    const il_ctx *c = vctx;
    (void)chunk;
    for (size_t p = begin; p < end; p++) {
        for (size_t k = 0; k < c->ch; k++) {
            uint8_t *dst = c->out + (p * c->ch + k) * c->bps;
            const pc_io_plane *pl = &c->planes[k];
            if (pl->present && pl->data != NULL && pl->len / c->bps > p) {
                be_to_native(pl->data + p * c->bps, dst, c->bps);
                if (c->invert[k]) {
                    invert_sample(dst, c->s);
                }
            } else {
                memcpy(dst, c->fill[k], c->bps);
            }
        }
    }
    return PC_OK;
}

pc_status pc_io_interleave(const pc_io_plane *planes, size_t ch, const uint8_t (*fill)[4], size_t n,
                           pc_sample_type s, const bool *invert, uint8_t **out, size_t *out_len,
                           pc_error *err)
{
    il_ctx c;
    size_t len;
    uint8_t *buf;
    c.bps = pc_sample_type_bytes(s);
    if (!pc_size_mul3(n, ch, c.bps, &len) || len > PC_MAX_ALLOC) {
        return pc_fail(err, PC_ERR_LIMIT, "limit exceeded: %zu pixels of %zu channels", n, ch);
    }
    buf = pc_malloc(len);
    if (buf == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory interleaving %zu pixels", n);
    }
    c.planes = planes;
    c.ch = ch;
    c.fill = fill;
    c.s = s;
    c.invert = invert;
    c.out = buf;
    if (ch > 0) {
        (void)pc_pool_for(pc_pool_global(), n, GRAIN, il_chunk, &c);
    }
    *out = buf;
    *out_len = len;
    return PC_OK;
}

typedef struct dl_ctx {
    const uint8_t *bytes;
    size_t ch;
    pc_sample_type s;
    size_t bps;
    size_t n;
    const bool *invert;
    uint8_t *out;
} dl_ctx;

static pc_status dl_chunk(void *vctx, size_t chunk, size_t begin, size_t end)
{
    const dl_ctx *c = vctx;
    size_t plane = c->n * c->bps;
    uint8_t tmp[4];
    (void)chunk;
    for (size_t p = begin; p < end; p++) {
        for (size_t k = 0; k < c->ch; k++) {
            memcpy(tmp, c->bytes + (p * c->ch + k) * c->bps, c->bps);
            if (c->invert[k]) {
                invert_sample(tmp, c->s);
            }
            native_to_be(tmp, c->out + k * plane + p * c->bps, c->bps);
        }
    }
    return PC_OK;
}

pc_status pc_io_deinterleave(const uint8_t *bytes, size_t len, size_t ch, pc_sample_type s,
                             const bool *invert, uint8_t **out, size_t *plane_len, pc_error *err)
{
    dl_ctx c;
    size_t px_bytes;
    size_t total;
    c.bps = pc_sample_type_bytes(s);
    px_bytes = ch * c.bps;
    c.n = len / (px_bytes > 0 ? px_bytes : 1);
    if (ch == 0 || c.n == 0) {
        *out = NULL;
        *plane_len = 0;
        return PC_OK;
    }
    total = c.n * px_bytes; /* <= len: no overflow */
    c.out = pc_malloc(total);
    if (c.out == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory splitting %zu pixels into planes", c.n);
    }
    c.bytes = bytes;
    c.ch = ch;
    c.s = s;
    c.invert = invert;
    (void)pc_pool_for(pc_pool_global(), c.n, GRAIN, dl_chunk, &c);
    *out = c.out;
    *plane_len = c.n * c.bps;
    return PC_OK;
}

size_t pc_io_max_sample(pc_sample_type s, uint8_t out[4])
{
    if (s == PC_SAMPLE_TYPE_U8) {
        out[0] = 255;
        return 1;
    }
    if (s == PC_SAMPLE_TYPE_U16) {
        st16(out, 65535);
        return 2;
    }
    {
        float one = 1.0f;
        memcpy(out, &one, 4);
        return 4;
    }
}

size_t pc_io_zero_sample(pc_sample_type s, uint8_t out[4])
{
    size_t n = pc_sample_type_bytes(s);
    memset(out, 0, n);
    return n;
}

void pc_io_lab16_chroma(uint8_t *bytes, size_t len, size_t ch, bool to_doc)
{
    double k;
    size_t px;
    if (ch < 3) {
        return;
    }
    k = to_doc ? 65535.0 / (double)PC_IO_LAB16_CHROMA_MAX : (double)PC_IO_LAB16_CHROMA_MAX / 65535.0;
    px = ch * 2;
    for (size_t at = 0; at + px <= len; at += px) {
        for (size_t c = 1; c < 3; c++) {
            uint16_t v = ld16(bytes + at + 2 * c);
            double w = round((double)v * k);
            st16(bytes + at + 2 * c, pc_f64_to_u16(w < 65535.0 ? w : 65535.0));
        }
    }
}

size_t pc_io_encode_be(float v, pc_sample_type s, uint8_t out[4])
{
    if (s == PC_SAMPLE_TYPE_U8) {
        out[0] = pc_f32_to_u8(pc_clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        return 1;
    }
    if (s == PC_SAMPLE_TYPE_U16) {
        uint16_t q = pc_f32_to_u16(pc_clampf(v, 0.0f, 1.0f) * 65535.0f + 0.5f);
        out[0] = (uint8_t)(q >> 8);
        out[1] = (uint8_t)q;
        return 2;
    }
    {
        uint32_t b;
        memcpy(&b, &v, 4);
        out[0] = (uint8_t)(b >> 24);
        out[1] = (uint8_t)(b >> 16);
        out[2] = (uint8_t)(b >> 8);
        out[3] = (uint8_t)b;
        return 4;
    }
}

float pc_io_decode_be(const uint8_t *bytes, size_t len, size_t index, pc_sample_type s)
{
    size_t bps = pc_sample_type_bytes(s);
    size_t at;
    if (!pc_size_mul(index, bps, &at) || at >= len || len - at < bps) {
        return 0.0f;
    }
    bytes += at;
    if (s == PC_SAMPLE_TYPE_U8) {
        return (float)bytes[0] / 255.0f;
    }
    if (s == PC_SAMPLE_TYPE_U16) {
        return (float)((unsigned)bytes[0] << 8 | bytes[1]) / 65535.0f;
    }
    {
        uint32_t b = (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3];
        float f;
        memcpy(&f, &b, 4);
        return f;
    }
}

float pc_io_matte(float c, float a, float white)
{
    return c * a + white * (1.0f - a);
}

float pc_io_unmatte(float m, float a, float white)
{
    return a <= 0.0f ? white : (m - white * (1.0f - a)) / a;
}
