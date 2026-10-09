/* Panasonic / Leica RW2 with the bit-packed sensor layout (PanasonicRaw RawFormat 5), a port of
 * crates/raw/src/rw2.rs. From RawDataOffset the data is stored in 0x4000-byte pages, each split at
 * 0x1FF8 with its second part first; the logical stream is 16-byte little-endian blocks of ten
 * 12-bit (or nine 14-bit) pixels from the least significant bit up. Older RawFormats are
 * vendor-compressed and not decoded. */
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "raw/internal.h"

#define SENSOR_WIDTH 0x0002
#define SENSOR_HEIGHT 0x0003
#define SENSOR_TOP_BORDER 0x0004
#define SENSOR_LEFT_BORDER 0x0005
#define SENSOR_BOTTOM_BORDER 0x0006
#define SENSOR_RIGHT_BORDER 0x0007
#define CFA_PATTERN 0x0009
#define BITS_PER_SAMPLE 0x000A
#define RAW_FORMAT 0x002D
#define RAW_DATA_OFFSET 0x0118
static const uint16_t LINEARITY_LIMIT[3] = {0x000E, 0x000F, 0x0010};
static const uint16_t BLACK_LEVEL[3] = {0x001C, 0x001D, 0x001E};
static const uint16_t WB_LEVEL[3] = {0x0024, 0x0025, 0x0026};

#define PAGE 0x4000u
#define SPLIT 0x1FF8u

size_t rx_rw2_physical(size_t i)
{
    size_t page = i - i % PAGE;
    return page + (i % PAGE + SPLIT) % PAGE;
}

void rx_rw2_unpack_block(const uint8_t b[16], uint32_t bits, uint16_t *out, size_t n)
{
    uint64_t lo = 0;
    uint64_t hi = 0;
    uint32_t mask = (1u << bits) - 1;
    size_t i;
    int k;
    for (k = 7; k >= 0; k--) {
        lo = lo << 8 | b[k];
        hi = hi << 8 | b[8 + k];
    }
    for (i = 0; i < n; i++) {
        uint32_t at = bits * (uint32_t)i;
        uint64_t v;
        if (at + bits > 128) {
            break;
        }
        if (at >= 64) {
            v = hi >> (at - 64);
        } else if (at == 0) {
            v = lo;
        } else {
            v = lo >> at | hi << (64 - at);
        }
        out[i] = (uint16_t)(v & mask);
    }
}

typedef struct rw2_ctx {
    const uint8_t *src;
    size_t srclen;
    uint16_t *data;
    size_t width;
    size_t height;
    size_t band;
    size_t row_bytes;
    size_t ppb;
    uint32_t bits;
} rw2_ctx;

/* The 8 bytes at logical offset l (zeros when they are not all stored). */
static void half(const rw2_ctx *c, size_t l, uint8_t out[8])
{
    size_t p = rx_rw2_physical(l);
    if (p <= c->srclen && c->srclen - p >= 8) {
        memcpy(out, c->src + p, 8);
    } else {
        memset(out, 0, 8);
    }
}

static pc_status rw2_band(void *ctx, size_t chunk, size_t begin, size_t end)
{
    rw2_ctx *c = ctx;
    size_t y;
    (void)chunk;
    for (y = begin; y < end; y++) {
        size_t at = y * c->row_bytes;
        uint16_t *out = c->data + y * c->width;
        size_t k;
        for (k = 0; k < c->width / c->ppb; k++) {
            uint8_t b[16];
            size_t l = at + k * 16;
            half(c, l, b);
            half(c, l + 8, b + 8);
            rx_rw2_unpack_block(b, c->bits, out + k * c->ppb, c->ppb);
        }
    }
    return PC_OK;
}

pc_status rx_rw2_decode(const rx_tiff *t, const pc_raw_limits *limits, pc_raw_sensor *out, pc_error *err)
{
    rx_ifd ifd;
    uint32_t v;
    size_t width;
    size_t height;
    uint32_t bits;
    size_t ppb;
    pc_raw_cfa cfa;
    const char *pat;
    size_t offset;
    size_t row_bytes;
    size_t need;
    rw2_ctx c;
    pc_raw_sensor s;
    pc_raw_rect full;
    uint32_t lv[3];
    bool have[3];
    int k;
    float max;
    size_t bd[4];
    bool hb[4];
    bool nomem = false;
    pc_status st;
    static const uint16_t BORDERS[4] = {SENSOR_LEFT_BORDER, SENSOR_TOP_BORDER, SENSOR_RIGHT_BORDER,
                                        SENSOR_BOTTOM_BORDER};
    if (!rx_ifd_at(t, t->first_ifd, 0, &ifd)) {
        return rx_malformed(err, "RW2 has no IFD0");
    }
    if (!rx_tag_uint(t, &ifd, RAW_FORMAT, &v)) {
        return rx_unsupported(err, "Panasonic RW2 raw format unknown (vendor-compressed) is not decoded yet");
    }
    if (v != 5) {
        return rx_unsupported(err, "Panasonic RW2 raw format %u (vendor-compressed) is not decoded yet", (unsigned)v);
    }
    if (!rx_tag_uint(t, &ifd, SENSOR_WIDTH, &v)) {
        return rx_malformed(err, "RW2 has no sensor width");
    }
    width = v;
    if (!rx_tag_uint(t, &ifd, SENSOR_HEIGHT, &v)) {
        return rx_malformed(err, "RW2 has no sensor height");
    }
    height = v;
    bits = rx_tag_uint(t, &ifd, BITS_PER_SAMPLE, &v) ? v : 12;
    if (bits == 12) {
        ppb = 10;
    } else if (bits == 14) {
        ppb = 9;
    } else {
        return rx_unsupported(err, "Panasonic RW2 with %u-bit samples", (unsigned)bits);
    }
    if (width % ppb != 0) {
        return rx_unsupported(err, "Panasonic RW2 %zu pixels wide (not whole %zu-pixel blocks)", width, ppb);
    }
    if ((st = pc_raw_limits_check(limits, width, height, 2, err)) != PC_OK) {
        return st;
    }
    switch (rx_tag_uint(t, &ifd, CFA_PATTERN, &v) ? v : 0) {
    case 1:
        pat = "RGGB";
        break;
    case 2:
        pat = "GRBG";
        break;
    case 3:
        pat = "GBRG";
        break;
    case 4:
        pat = "BGGR";
        break;
    default:
        return rx_unsupported(err, "Panasonic RW2 without a CFA pattern");
    }
    if (!pc_raw_cfa_bayer(pat, &cfa)) {
        return rx_malformed(err, "bad CFA");
    }
    if (!rx_tag_uint(t, &ifd, RAW_DATA_OFFSET, &v) && !rx_tag_uint(t, &ifd, RX_STRIP_OFFSETS, &v)) {
        return rx_malformed(err, "RW2 has no raw data offset");
    }
    offset = v;
    row_bytes = width / ppb * 16;
    if (offset > t->len) {
        return rx_malformed(err, "RW2 raw data lies outside the file");
    }
    need = row_bytes * height;
    /* Whole pages are stored; tolerate a short final page (its missing bytes read as 0). */
    if (t->len - offset < (need > PAGE ? need - PAGE : 0)) {
        return rx_malformed(err, "RW2 raw data is truncated");
    }
    pc_raw_sensor_init(&s);
    s.data = pc_calloc_array(width * height, sizeof *s.data);
    if (s.data == NULL) {
        return rx_nomem(err);
    }
    s.ndata = width * height;
    c.src = t->data + offset;
    c.srclen = t->len - offset;
    c.data = s.data;
    c.width = width;
    c.height = height;
    c.band = rx_band_rows(width);
    c.row_bytes = row_bytes;
    c.ppb = ppb;
    c.bits = bits;
    (void)pc_pool_for(pc_pool_global(), height, c.band, rw2_band, &c);
    full = pc_raw_rect_make(0, 0, width, height);
    for (k = 0; k < 3; k++) {
        have[k] = rx_tag_uint(t, &ifd, BLACK_LEVEL[k], &lv[k]);
    }
    if (have[0] && have[1] && have[2]) {
        uint8_t p[4];
        float bv[4];
        int i;
        pc_raw_cfa_phase(&cfa, 0, 0, p);
        for (i = 0; i < 4; i++) {
            bv[i] = (float)(double)lv[p[i] < 2 ? p[i] : 2];
        }
        st = rx_black_2x2(&s.black, bv, err);
    } else {
        st = rx_warn(&s.warnings, err, "RW2: black level not recorded; assumed 0");
        if (st == PC_OK) {
            st = rx_black_uniform(&s.black, 0.0f, err);
        }
    }
    if (st != PC_OK) {
        goto fail;
    }
    max = (float)((1u << bits) - 1);
    for (k = 0; k < 3; k++) {
        uint32_t l;
        float f = rx_tag_uint(t, &ifd, LINEARITY_LIMIT[k], &l) ? (float)(double)l : 0.0f;
        s.white[k] = f > 0.0f && f <= max ? f : max;
    }
    for (k = 0; k < 3; k++) {
        have[k] = rx_tag_uint(t, &ifd, WB_LEVEL[k], &lv[k]);
    }
    if (have[0] && have[1] && have[2] && lv[1] > 0) {
        double g = lv[1];
        double m[3] = {(double)lv[0] / g, 1.0, (double)lv[2] / g};
        bool ok = true;
        for (k = 0; k < 3; k++) {
            ok = ok && m[k] >= 0.25 && m[k] < 8.0;
        }
        if (ok) {
            s.has_camera_wb = true;
            memcpy(s.camera_wb, m, sizeof m);
        }
    }
    for (k = 0; k < 4; k++) {
        uint32_t b;
        hb[k] = rx_tag_uint(t, &ifd, BORDERS[k], &b);
        bd[k] = hb[k] ? b : 0;
    }
    s.crop = full;
    if (hb[0] && hb[1] && hb[2] && hb[3] && bd[2] > bd[0] && bd[3] > bd[1]) {
        pc_raw_rect r = pc_raw_rect_intersect(pc_raw_rect_make(bd[0], bd[1], bd[2] - bd[0], bd[3] - bd[1]), full);
        s.crop = pc_raw_rect_is_empty(r) ? full : r;
    }
    s.format = PC_RAW_RW2;
    s.make = rx_tag_ascii(t, &ifd, RX_MAKE, &nomem);
    s.model = rx_tag_ascii(t, &ifd, RX_MODEL, &nomem);
    if (nomem) {
        st = rx_nomem(err);
        goto fail;
    }
    s.width = width;
    s.height = height;
    s.samples = 1;
    s.has_cfa = true;
    s.cfa = cfa;
    s.active = full;
    s.orientation = rx_orientation(t, &ifd);
    *out = s;
    return PC_OK;
fail:
    pc_raw_sensor_fini(&s);
    return st;
}
