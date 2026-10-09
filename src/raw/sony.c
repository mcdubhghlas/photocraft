/* Sony compressed ARW ("cRAW"), a port of crates/raw/src/sony.rs: rows of 32-pixel runs, two
 * interleaved 16-byte blocks (even columns first), each an 11-bit max and min, their 4-bit
 * positions and fourteen 7-bit deltas at the smallest power-of-two step covering the range; the
 * 11-bit codes go through a five-segment tone curve given by SonyToneCurve (0x7010). */
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "raw/internal.h"

#define SONY_TONE_CURVE 0x7010
#define MAX_14 16383u

bool rx_sony_tone_lut(const uint32_t *knots, size_t n, uint16_t lut[2048])
{
    uint32_t pts[6];
    uint32_t code;
    int i;
    if (n != 4) {
        return false;
    }
    pts[0] = 0;
    memcpy(pts + 1, knots, 4 * sizeof *knots);
    pts[5] = 1u << 14;
    for (i = 0; i < 5; i++) {
        if (pts[i] > pts[i + 1]) {
            return false;
        }
    }
    for (code = 0; code < 2048; code++) {
        /* On the knots' scale a code is 8 x code; the first segment maps it to 2 x code. */
        uint32_t x = code * 8;
        uint32_t y = 0;
        for (i = 0; i < 5; i++) {
            if (x > pts[i]) {
                y += ((x < pts[i + 1] ? x : pts[i + 1]) - pts[i]) << i;
            }
        }
        y /= 4;
        lut[code] = (uint16_t)(y < MAX_14 ? y : MAX_14);
    }
    return true;
}

void rx_sony_decode_block(const uint8_t b[16], uint16_t out[16])
{
    uint64_t lo = 0;
    uint64_t hi = 0;
    uint16_t max;
    uint16_t min;
    size_t imax;
    size_t imin;
    uint16_t range;
    uint32_t sh = 0;
    uint32_t at = 30;
    size_t i;
    int k;
    for (k = 7; k >= 0; k--) {
        lo = lo << 8 | b[k];
        hi = hi << 8 | b[8 + k];
    }
#define FIELD(a, n)                                                                                \
    ((uint16_t)(((a) >= 64 ? hi >> ((a)-64) : (a) == 0 ? lo : (lo >> (a) | hi << (64 - (a)))) & ((1u << (n)) - 1)))
    max = FIELD(0u, 11);
    min = FIELD(11u, 11);
    imax = FIELD(22u, 4);
    imin = FIELD(26u, 4);
    range = (uint16_t)(max > min ? max - min : 0);
    while ((uint16_t)(128u << sh) <= range) {
        sh++;
    }
    for (i = 0; i < 16; i++) {
        if (i == imax) {
            out[i] = max;
        } else if (i == imin) {
            out[i] = min;
        } else if (at + 7 <= 128) {
            uint16_t d = FIELD(at, 7);
            uint32_t v = (uint32_t)min + ((uint32_t)d << sh);
            at += 7;
            out[i] = (uint16_t)(v < 2047 ? v : 2047);
        } else {
            /* Only when a damaged block names the same position twice. */
            out[i] = min;
        }
    }
#undef FIELD
}

bool rx_sony_is_craw(const rx_tiff *t, const rx_ifd *ifd)
{
    uint32_t c;
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t kind;
    rx_entry e;
    uint64_t bytes = 0;
    if (!rx_tag_uint(t, ifd, RX_COMPRESSION, &c) || c != 32767) {
        return false;
    }
    (void)rx_tag_uint(t, ifd, RX_IMAGE_WIDTH, &w);
    (void)rx_tag_uint(t, ifd, RX_IMAGE_LENGTH, &h);
    if (rx_ifd_get(t, ifd, RX_STRIP_BYTE_COUNTS, &e)) {
        size_t n = rx_uints_n(t, &e);
        size_t i;
        for (i = 0; i < n; i++) {
            uint32_t v;
            (void)rx_uint(t, &e, i, &v);
            bytes += v;
        }
    }
    if (rx_tag_uint(t, ifd, RX_SONY_RAW_FILE_TYPE, &kind)) {
        return kind == 2;
    }
    return bytes > 0 && bytes == (uint64_t)w * h;
}

typedef struct craw_ctx {
    const uint8_t **rows;
    uint16_t *data;
    size_t width;
    const uint16_t *lut;
} craw_ctx;

static pc_status craw_band(void *ctx, size_t chunk, size_t begin, size_t end)
{
    craw_ctx *c = ctx;
    size_t y;
    (void)chunk;
    for (y = begin; y < end; y++) {
        const uint8_t *src = c->rows[y];
        uint16_t *px = c->data + y * c->width;
        size_t g;
        for (g = 0; g < c->width / 32; g++) {
            uint16_t e[16];
            uint16_t o[16];
            size_t k;
            rx_sony_decode_block(src + 32 * g, e);
            rx_sony_decode_block(src + 32 * g + 16, o);
            for (k = 0; k < 16; k++) {
                px[32 * g + 2 * k] = c->lut[e[k]];
                px[32 * g + 2 * k + 1] = c->lut[o[k]];
            }
        }
    }
    return PC_OK;
}

pc_status rx_sony_read_craw(const rx_tiff *t, const rx_ifd *ifd, const pc_raw_limits *limits, rx_plane *out,
                            pc_error *err)
{
    uint32_t v;
    size_t width;
    size_t height;
    uint32_t knots[4];
    size_t nk;
    rx_entry e;
    uint16_t lut[2048];
    rx_segment *segs = NULL;
    size_t nseg;
    const uint8_t **rows = NULL;
    size_t nrows = 0;
    size_t i;
    craw_ctx c;
    pc_status st;
    if (!rx_tag_uint(t, ifd, RX_IMAGE_WIDTH, &v)) {
        return rx_malformed(err, "raw image has no width");
    }
    width = v;
    if (!rx_tag_uint(t, ifd, RX_IMAGE_LENGTH, &v)) {
        return rx_malformed(err, "raw image has no height");
    }
    height = v;
    if (width % 32 != 0) {
        return rx_unsupported(err, "Sony compressed ARW %zu pixels wide (not a multiple of 32)", width);
    }
    nk = rx_ifd_get(t, ifd, SONY_TONE_CURVE, &e) ? rx_uints(t, &e, knots, 4) : 0;
    if (!rx_sony_tone_lut(knots, nk, lut)) {
        return rx_unsupported(err, "Sony compressed ARW without a usable tone curve (0x7010)");
    }
    if ((st = pc_raw_limits_check(limits, width, height, 2, err)) != PC_OK ||
        (st = rx_segments(t, ifd, width, height, &segs, &nseg, err)) != PC_OK) {
        return st;
    }
    {
        size_t total = 0;
        for (i = 0; i < nseg; i++) {
            size_t left = height > segs[i].y ? height - segs[i].y : 0;
            total += segs[i].h < left ? segs[i].h : left;
        }
        rows = pc_malloc_array(total > 0 ? total : 1, sizeof *rows);
    }
    if (rows == NULL) {
        free(segs);
        return rx_nomem(err);
    }
    /* One byte per pixel: locate every row in the strips. */
    for (i = 0; i < nseg; i++) {
        const rx_segment *s = &segs[i];
        size_t n = s->h < (height > s->y ? height - s->y : 0) ? s->h : (height > s->y ? height - s->y : 0);
        size_t r;
        for (r = 0; r < n; r++) {
            const uint8_t *b;
            if (s->offset > SIZE_MAX - r * width) {
                st = rx_malformed(err, "raw strip offset overflows");
                goto done;
            }
            if ((r + 1) * width > s->len) {
                st = rx_malformed(err, "compressed ARW strip is shorter than its rows");
                goto done;
            }
            if ((b = rx_bytes(t, s->offset + r * width, width)) == NULL) {
                st = rx_malformed(err, "compressed ARW data lies outside the file");
                goto done;
            }
            rows[nrows++] = b;
        }
    }
    if (nrows != height) {
        st = rx_malformed(err, "compressed ARW strips do not cover the image");
        goto done;
    }
    out->data = pc_calloc_array(width * height, sizeof *out->data);
    if (out->data == NULL) {
        st = rx_nomem(err);
        goto done;
    }
    c.rows = rows;
    c.data = out->data;
    c.width = width;
    c.lut = lut;
    (void)pc_pool_for(pc_pool_global(), height, rx_band_rows(width), craw_band, &c);
    out->width = width;
    out->height = height;
    out->samples = 1;
    out->bits = 14;
    st = PC_OK;
done:
    free(rows);
    free(segs);
    return st;
}
