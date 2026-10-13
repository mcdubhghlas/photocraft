/* JPEG quality and WebP encoding read back from a file (port of
 * crates/codecs/src/source_settings.rs; see source_settings.h). */
#include "codecs/source_settings.h"

#include <math.h>
#include <string.h>

#include "codecs/internal.h"

/* ITU-T T.81 Annex K.1 / K.2, the example quantisation tables, in natural (row) order. */
static const uint16_t ANNEX_K_LUMA[64] = {
    16, 11,  10,  16, 24, 40, 51, 61, 12,  12,  14,  19,  26, 58, 60, 55,  14,  13,  16,  24, 40, 57,
    69, 56,  14,  17, 22, 29, 51, 87, 80,  62,  18,  22,  37, 56, 68, 109, 103, 77,  24,  35, 55, 64,
    81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98,  112, 100, 103, 99,
};

static const uint16_t ANNEX_K_CHROMA[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99,
    99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
};

/* The natural-order index of the k-th coefficient in zig-zag order (T.81 Figure A.6). */
static const uint8_t ZIGZAG[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

typedef struct table {
    bool has;
    bool wide; /* 16-bit entries (Pq = 1) */
    uint16_t values[64];
} table;

/* The IJG quality scaling of base (libjpeg's jpeg_quality_scaling / jpeg_add_quant_table). */
static uint32_t scaled(uint16_t base, uint8_t q8, bool wide)
{
    uint32_t q = q8 < 1 ? 1 : q8 > 100 ? 100 : q8;
    uint32_t scale = q < 50 ? 5000 / q : 200 - 2 * q;
    uint32_t v = ((uint32_t)base * scale + 50) / 100;
    uint32_t max = wide ? 32767 : 255;
    return v < 1 ? 1 : v > max ? max : v;
}

static uint64_t table_error(const table *t, const uint16_t *base, uint8_t q)
{
    uint64_t sum = 0;
    size_t k;
    for (k = 0; k < 64; k++) {
        uint32_t a = t->values[k];
        uint32_t b = scaled(base[k], q, t->wide);
        sum += a > b ? a - b : b - a;
    }
    return sum;
}

/* The tables of one DQT segment; a truncated or malformed table ends the segment. */
static void read_dqt(const uint8_t *seg, size_t n, table *tables)
{
    while (n > 0) {
        uint8_t pq_tq = seg[0];
        bool wide = (pq_tq >> 4) == 1;
        size_t slot = pq_tq & 0x0F;
        size_t len = wide ? 128 : 64;
        table t;
        size_t k;
        if ((pq_tq >> 4) > 1 || n - 1 < len) {
            return;
        }
        t.has = true;
        t.wide = wide;
        for (k = 0; k < 64; k++) {
            t.values[ZIGZAG[k]] = wide ? cx_be16(seg + 1 + 2 * k) : seg[1 + k];
        }
        if (slot < 4) {
            tables[slot] = t;
        }
        seg += 1 + len;
        n -= 1 + len;
    }
}

/* The quantisation tables 0..=3 defined before the first scan. */
static void jpeg_tables(const uint8_t *b, size_t n, table *tables)
{
    size_t i = 2;
    memset(tables, 0, 4 * sizeof *tables);
    if (n < 2 || b[0] != 0xFF || b[1] != 0xD8) {
        return;
    }
    while (i < n && b[i] == 0xFF && i + 1 < n) {
        uint8_t marker = b[i + 1];
        size_t len;
        if (marker == 0xFF) {
            i += 1;
            continue;
        }
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
            i += 2;
            continue;
        }
        if (marker == 0xDA || marker == 0xD9) {
            break;
        }
        if (n - i < 4) {
            break;
        }
        len = cx_be16(b + i + 2);
        if (len < 2 || n - (i + 4) < len - 2) {
            break;
        }
        if (marker == 0xDB) {
            read_dqt(b + i + 4, len - 2, tables);
        }
        i += 2 + len;
    }
}

bool pc_jpeg_quality(const uint8_t *b, size_t n, uint8_t *quality)
{
    table t[4];
    uint64_t best = 0;
    uint8_t best_q = 0;
    bool found = false;
    unsigned q;
    jpeg_tables(b, n, t);
    if (!t[0].has) {
        return false;
    }
    for (q = 1; q <= 100; q++) {
        uint64_t e = table_error(&t[0], ANNEX_K_LUMA, (uint8_t)q);
        if (t[1].has) {
            uint64_t c = table_error(&t[1], ANNEX_K_CHROMA, (uint8_t)q);
            e = e > UINT64_MAX - c ? UINT64_MAX : e + c;
        }
        if (!found || e < best) {
            best = e;
            best_q = (uint8_t)q;
            found = true;
        }
    }
    *quality = best_q;
    return true;
}

/* ---- WebP ---- */

uint8_t pc_webp_quantizer_index(uint8_t quality)
{
    /* libwebp's VP8SetSegmentParams for a segment of neutral susceptibility: quality / 100
     * through QualityToCompression (a cube root of a two-piece linear map), q = 127 (1 - c). */
    double c = (quality > 100 ? 100 : quality) / 100.0;
    double linear = c < 0.75 ? c * (2.0 / 3.0) : 2.0 * c - 1.0;
    double v = pow(linear, 1.0 / 3.0);
    int q = (int)(127.0 * (1.0 - v));
    return (uint8_t)(q < 0 ? 0 : q > 127 ? 127 : q);
}

/* RFC 6386 section 7's boolean decoder, reading zeros past the end of its data. */
typedef struct booldec {
    const uint8_t *data;
    size_t n;
    size_t pos;
    uint32_t value;
    uint32_t range;
    uint32_t bit_count;
    size_t overrun;
} booldec;

static uint32_t next_byte(booldec *d)
{
    if (d->pos < d->n) {
        return d->data[d->pos++];
    }
    d->overrun++;
    return 0;
}

static bool dbool(booldec *d, uint32_t prob)
{
    uint32_t split = 1 + (((d->range - 1) * prob) >> 8);
    uint32_t big_split = split << 8;
    bool bit;
    if (d->value >= big_split) {
        d->range -= split;
        d->value -= big_split;
        bit = true;
    } else {
        d->range = split;
        bit = false;
    }
    while (d->range < 128) {
        d->value <<= 1;
        d->range <<= 1;
        d->bit_count++;
        if (d->bit_count == 8) {
            d->bit_count = 0;
            d->value |= next_byte(d);
        }
    }
    return bit;
}

static bool flag(booldec *d)
{
    return dbool(d, 128);
}

static uint32_t literal(booldec *d, unsigned bits)
{
    uint32_t v = 0;
    unsigned k;
    for (k = 0; k < bits; k++) {
        v = (v << 1) | (flag(d) ? 1u : 0u);
    }
    return v;
}

static int32_t sgn(booldec *d, unsigned bits)
{
    int32_t v = (int32_t)literal(d, bits);
    return flag(d) ? -v : v;
}

/* The base quantiser index of a VP8 key frame (RFC 6386 sections 9.2-9.6, 19.2). */
static bool vp8_base_quantizer(const uint8_t *f, size_t n, uint8_t *out)
{
    booldec d;
    bool seg = false;
    bool absolute = false;
    int32_t q[4] = {0, 0, 0, 0};
    int32_t qi;
    int k;
    uint32_t hi;
    if (n < 3 || (f[0] & 1) != 0 || n < 6 || f[3] != 0x9D || f[4] != 0x01 || f[5] != 0x2A || n < 10) {
        return false;
    }
    memset(&d, 0, sizeof d);
    d.data = f + 10;
    d.n = n - 10;
    d.range = 255;
    hi = next_byte(&d);
    d.value = (hi << 8) | next_byte(&d);
    (void)literal(&d, 1); /* color_space */
    (void)literal(&d, 1); /* clamping_type */
    if (flag(&d)) {
        bool update_map = flag(&d);
        if (flag(&d)) {
            absolute = flag(&d);
            for (k = 0; k < 4; k++) {
                q[k] = flag(&d) ? sgn(&d, 7) : 0;
            }
            for (k = 0; k < 4; k++) {
                if (flag(&d)) {
                    (void)sgn(&d, 6);
                }
            }
            seg = true;
        }
        if (update_map) {
            for (k = 0; k < 3; k++) {
                if (flag(&d)) {
                    (void)literal(&d, 8);
                }
            }
        }
    }
    (void)literal(&d, 1); /* filter_type */
    (void)literal(&d, 6); /* loop_filter_level */
    (void)literal(&d, 3); /* sharpness_level */
    if (flag(&d) && flag(&d)) {
        for (k = 0; k < 8; k++) {
            if (flag(&d)) {
                (void)sgn(&d, 6);
            }
        }
    }
    (void)literal(&d, 2); /* log2_nbr_of_dct_partitions */
    if (d.overrun > 2) {
        return false;
    }
    qi = (int32_t)literal(&d, 7);
    if (seg) {
        int32_t sum = q[0] + q[1] + q[2] + q[3];
        qi = absolute ? sum / 4 : qi + sum / 4;
    }
    *out = (uint8_t)(qi < 0 ? 0 : qi > 127 ? 127 : qi);
    return true;
}

static bool vp8_quality(const uint8_t *f, size_t n, uint8_t *out)
{
    uint8_t qi;
    unsigned q;
    unsigned best = 256;
    uint8_t matches[101];
    size_t nm = 0;
    if (!vp8_base_quantizer(f, n, &qi)) {
        return false;
    }
    for (q = 0; q <= 100; q++) {
        uint8_t idx = pc_webp_quantizer_index((uint8_t)q);
        unsigned dist = idx > qi ? (unsigned)(idx - qi) : (unsigned)(qi - idx);
        if (dist < best) {
            best = dist;
            nm = 0;
        }
        if (dist == best) {
            matches[nm++] = (uint8_t)q;
        }
    }
    *out = matches[nm / 2];
    return true;
}

static bool find_image_chunk(const uint8_t *c, size_t n, int depth, pc_webp_encoding *out)
{
    while (n >= 8) {
        uint64_t size = cx_le32(c + 4);
        uint64_t end = 8 + size;
        size_t plen = (size_t)(end < n ? end : n) - 8;
        const uint8_t *payload = c + 8;
        if (memcmp(c, "VP8L", 4) == 0) {
            out->kind = PC_WEBP_LOSSLESS;
            out->has_quality = false;
            out->quality = 0;
            return true;
        }
        if (memcmp(c, "VP8 ", 4) == 0) {
            out->kind = PC_WEBP_LOSSY;
            out->quality = 0;
            out->has_quality = vp8_quality(payload, plen, &out->quality);
            return true;
        }
        if (memcmp(c, "ANMF", 4) == 0 && depth == 0 && plen >= 16 &&
            find_image_chunk(payload + 16, plen - 16, 1, out)) {
            return true;
        }
        end += size % 2;
        if (end >= n) {
            return false;
        }
        c += end;
        n -= (size_t)end;
    }
    return false;
}

bool pc_webp_encoding_of(const uint8_t *b, size_t n, pc_webp_encoding *out)
{
    if (n < 12 || memcmp(b, "RIFF", 4) != 0 || memcmp(b + 8, "WEBP", 4) != 0) {
        return false;
    }
    return find_image_chunk(b + 12, n - 12, 0, out);
}
