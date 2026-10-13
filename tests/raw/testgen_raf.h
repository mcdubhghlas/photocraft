/* Synthetic Fujifilm RAF files and any-CFA mosaics, byte for byte as crates/raw/src/testgen.rs
 * writes them (RafSpec, XTRANS, mosaic_cfa). Include after testgen.h. */
#ifndef PC_TESTS_RAW_TESTGEN_RAF_H
#define PC_TESTS_RAW_TESTGEN_RAF_H

#include "testgen.h"

/* The X-Trans repeat at the data origin, as observed (row major, 0 = R, 1 = G, 2 = B). */
static const uint8_t TG_XTRANS[36] = {1, 1, 0, 1, 1, 2, 1, 1, 2, 1, 1, 0, 2, 0, 1, 0, 2, 1,
                                      1, 1, 2, 1, 1, 0, 1, 1, 0, 1, 1, 2, 0, 2, 1, 2, 0, 1};

typedef enum tg_raf_packing {
    TG_RAF_U16_LE,
    TG_RAF_U16_BE,
    TG_RAF_LSB12,
    TG_RAF_WORDS14,
    TG_RAF_COMPRESSED
} tg_raf_packing;

typedef struct tg_raf {
    size_t width;
    size_t height;
    const uint16_t *data; /* borrowed */
    uint32_t bits;
    tg_raf_packing packing;
    uint8_t cfa[36]; /* 36 (X-Trans) or 4 (Bayer) colours at the data origin */
    size_t ncfa;
    bool has_crop;
    uint16_t crop[4]; /* top, left, height, width */
    uint32_t black[36];
    size_t nblack;
    bool has_wb;
    uint32_t wb_grb[3];
    bool has_bias;
    int16_t bias[2];
    uint16_t orientation;
    bool cfa_tiff;
} tg_raf;

static inline tg_raf tg_raf_new(size_t width, size_t height, const uint16_t *data, const uint8_t *cfa,
                                size_t ncfa)
{
    tg_raf r;
    memset(&r, 0, sizeof r);
    r.width = width;
    r.height = height;
    r.data = data;
    r.bits = 14;
    r.packing = TG_RAF_U16_LE;
    memcpy(r.cfa, cfa, ncfa);
    r.ncfa = ncfa;
    r.black[0] = 1024;
    r.nblack = 1;
    r.has_wb = true;
    r.wb_grb[0] = 302;
    r.wb_grb[1] = 604;
    r.wb_grb[2] = 453;
    r.orientation = 1;
    r.cfa_tiff = true;
    return r;
}

static inline tg_bytes tg_raf_strip(const tg_raf *r)
{
    tg_bytes out = {NULL, 0, 0};
    size_t n = r->width * r->height;
    size_t i;
    if (r->packing == TG_RAF_U16_LE || r->packing == TG_RAF_U16_BE) {
        for (i = 0; i < n; i++) {
            if (r->packing == TG_RAF_U16_LE) {
                tg_le16(&out, r->data[i]);
            } else {
                tg_be16(&out, r->data[i]);
            }
        }
    } else if (r->packing == TG_RAF_LSB12) {
        uint64_t acc = 0;
        uint32_t k = 0;
        for (i = 0; i < n; i++) {
            acc |= (uint64_t)(r->data[i] & 0xFFF) << k;
            k += 12;
            while (k >= 8) {
                tg_byte(&out, (uint8_t)acc);
                acc >>= 8;
                k -= 8;
            }
        }
        if (k > 0) {
            tg_byte(&out, (uint8_t)acc);
        }
    } else if (r->packing == TG_RAF_WORDS14) {
        tg_bytes logical = {NULL, 0, 0};
        uint64_t acc = 0;
        uint32_t k = 0;
        for (i = 0; i < n; i++) {
            acc = (acc << 14) | (r->data[i] & 0x3FFF);
            k += 14;
            while (k >= 8) {
                tg_byte(&logical, (uint8_t)(acc >> (k - 8)));
                k -= 8;
            }
        }
        if (k > 0) {
            tg_byte(&logical, (uint8_t)(acc << (8 - k)));
        }
        while (logical.n % 4 != 0) {
            tg_byte(&logical, 0);
        }
        for (i = 0; i < logical.n; i += 4) {
            tg_byte(&out, logical.d[i + 3]);
            tg_byte(&out, logical.d[i + 2]);
            tg_byte(&out, logical.d[i + 1]);
            tg_byte(&out, logical.d[i]);
        }
        tg_free(&logical);
    } else {
        static const uint8_t IS[4] = {'I', 'S', 1, 0};
        tg_put(&out, IS, 4);
        while (out.n < n / 2) {
            tg_byte(&out, 0x5A);
        }
        if (n / 2 < out.n) {
            out.n = n / 2; /* Vec::resize truncates */
        }
    }
    return out;
}

/* The CFA region: a TIFF whose IFD0 points (0xF000) at the Fuji raw IFD. */
static inline tg_bytes tg_raf_region(const tg_raf *r)
{
    bool le = r->packing != TG_RAF_U16_BE;
    tg_bytes strip = tg_raf_strip(r);
    tg_bytes out = {NULL, 0, 0};
    tg_bytes extra = {NULL, 0, 0};
    const uint32_t strip_at = 2048;
    uint16_t tags[7] = {0xF001, 0xF002, 0xF003, 0xF007, 0xF008, 0xF00A, 0xF00E};
    const uint32_t *vals[7];
    size_t counts[7];
    uint32_t one[5];
    size_t ne = r->has_wb ? 7 : 6;
    size_t fuji_at;
    size_t extra_at;
    size_t i;
    size_t k;
#define TG_U16(v) (le ? tg_le16(&out, (uint16_t)(v)) : tg_be16(&out, (uint16_t)(v)))
#define TG_U32(b, v) (le ? tg_le32((b), (uint32_t)(v)) : tg_be32((b), (uint32_t)(v)))
    one[0] = (uint32_t)r->width;
    one[1] = (uint32_t)r->height;
    one[2] = r->bits;
    one[3] = strip_at;
    one[4] = (uint32_t)strip.n;
    for (i = 0; i < 5; i++) {
        vals[i] = &one[i];
        counts[i] = 1;
    }
    vals[5] = r->black;
    counts[5] = r->nblack;
    vals[6] = r->wb_grb;
    counts[6] = 3;
    if (le) {
        tg_put(&out, "II*\0", 4);
    } else {
        tg_put(&out, "MM\0*", 4);
    }
    TG_U32(&out, 8);
    TG_U16(1);
    TG_U16(0xF000);
    TG_U16(13);
    TG_U32(&out, 1);
    TG_U32(&out, 26);
    TG_U32(&out, 0);
    fuji_at = out.n;
    extra_at = fuji_at + 2 + 12 * ne + 4;
    TG_U16(ne);
    for (i = 0; i < ne; i++) {
        TG_U16(tags[i]);
        TG_U16(4);
        TG_U32(&out, counts[i]);
        if (counts[i] == 1) {
            TG_U32(&out, vals[i][0]);
        } else {
            TG_U32(&out, extra_at);
            for (k = 0; k < counts[i]; k++) {
                TG_U32(&extra, vals[i][k]);
            }
            extra_at += 4 * counts[i];
        }
    }
    TG_U32(&out, 0);
    tg_put(&out, extra.d, extra.n);
    tg_free(&extra);
    while (out.n < strip_at) {
        tg_byte(&out, 0);
    }
    tg_put(&out, strip.d, strip.n);
    tg_free(&strip);
#undef TG_U16
#undef TG_U32
    return out;
}

/* A tiny baseline-JPEG preview: APP1 EXIF (make, model, orientation), SOF0 32 x 24, EOI. */
static inline tg_bytes tg_raf_preview(const tg_raf *r)
{
    tg_tiff t;
    tg_entry e[3];
    size_t i0;
    tg_bytes tb;
    tg_bytes j = {NULL, 0, 0};
    static const uint8_t SOF[15] = {0xFF, 0xC0, 0x00, 0x0B, 8, 0x00, 24, 0x00, 32, 1, 1, 0x11, 0, 0xFF, 0xD9};
    memset(&t, 0, sizeof t);
    e[0].tag = 271;
    e[0].val = tg_v_ascii("FUJIFILM");
    e[1].tag = 272;
    e[1].val = tg_v_ascii("X-Synthetic");
    e[2].tag = 274;
    e[2].val = tg_v_short1(r->orientation);
    i0 = tg_ifd_add(&t, e, 3);
    tg_set_chain(&t, &i0, 1);
    tb = tg_tiff_build(&t);
    tg_tiff_free(&t);
    tg_put(&j, "\xFF\xD8\xFF\xE1", 4);
    tg_be16(&j, (uint16_t)(6 + tb.n + 2));
    tg_put(&j, "Exif\0\0", 6);
    tg_put(&j, tb.d, tb.n);
    tg_free(&tb);
    tg_put(&j, SOF, sizeof SOF);
    return j;
}

static inline void tg_raf_rec(tg_bytes *b, uint16_t tag, const uint8_t *d, uint16_t n)
{
    tg_be16(b, tag);
    tg_be16(b, n);
    tg_put(b, d, n);
}

static inline tg_bytes tg_raf_meta(const tg_raf *r)
{
    tg_bytes recs = {NULL, 0, 0};
    tg_bytes out = {NULL, 0, 0};
    uint32_t n = 0;
    size_t i;
    if (r->has_crop) {
        uint8_t a[4] = {(uint8_t)(r->crop[0] >> 8), (uint8_t)r->crop[0], (uint8_t)(r->crop[1] >> 8),
                        (uint8_t)r->crop[1]};
        uint8_t b[4] = {(uint8_t)(r->crop[2] >> 8), (uint8_t)r->crop[2], (uint8_t)(r->crop[3] >> 8),
                        (uint8_t)r->crop[3]};
        tg_raf_rec(&recs, 0x0110, a, 4);
        tg_raf_rec(&recs, 0x0111, b, 4);
        n += 2;
    }
    if (r->ncfa == 36) {
        uint8_t l[4] = {0x0C, 0x0C, 0x0C, 0x0C};
        uint8_t rev[36];
        for (i = 0; i < 36; i++) {
            rev[i] = r->cfa[35 - i];
        }
        tg_raf_rec(&recs, 0x0130, l, 4);
        tg_raf_rec(&recs, 0x0131, rev, 36);
        n += 2;
    } else {
        /* Bayer: reversed, colour in the low two bits, the second green as 3. */
        uint8_t codes[4];
        uint8_t rev[4];
        int greens = 0;
        for (i = 0; i < 4; i++) {
            if (r->cfa[i] == 1) {
                greens++;
                codes[i] = greens == 1 ? 0x09 : 0x0B;
            } else {
                codes[i] = (uint8_t)(0x08 | r->cfa[i]);
            }
        }
        for (i = 0; i < 4; i++) {
            rev[i] = codes[3 - i];
        }
        tg_raf_rec(&recs, 0x0130, rev, 4);
        n += 1;
    }
    if (r->has_bias) {
        uint8_t b[4] = {(uint8_t)((uint16_t)r->bias[0] >> 8), (uint8_t)r->bias[0],
                        (uint8_t)((uint16_t)r->bias[1] >> 8), (uint8_t)r->bias[1]};
        tg_raf_rec(&recs, 0x9650, b, 4);
        n += 1;
    }
    tg_be32(&out, n);
    tg_put(&out, recs.d, recs.n);
    tg_free(&recs);
    return out;
}

static inline tg_bytes tg_raf_build(const tg_raf *r)
{
    tg_bytes b = {NULL, 0, 0};
    tg_bytes preview = tg_raf_preview(r);
    tg_bytes meta = tg_raf_meta(r);
    tg_bytes region = r->cfa_tiff ? tg_raf_region(r) : tg_raf_strip(r);
    size_t at[6];
    size_t i;
    tg_put(&b, "FUJIFILMCCD-RAW 0201FF000000", 28);
    tg_put(&b, "X-Synthetic", 11);
    while (b.n < 60) {
        tg_byte(&b, 0);
    }
    tg_put(&b, "0100", 4);
    while (b.n < 160) {
        tg_byte(&b, 0);
    }
    at[0] = b.n;
    at[1] = preview.n;
    at[2] = b.n + preview.n;
    at[3] = meta.n;
    at[4] = at[2] + meta.n;
    at[5] = region.n;
    for (i = 0; i < 6; i++) {
        size_t o = 84 + 4 * i;
        b.d[o] = (uint8_t)(at[i] >> 24);
        b.d[o + 1] = (uint8_t)(at[i] >> 16);
        b.d[o + 2] = (uint8_t)(at[i] >> 8);
        b.d[o + 3] = (uint8_t)at[i];
    }
    tg_put(&b, preview.d, preview.n);
    tg_put(&b, meta.d, meta.n);
    tg_put(&b, region.d, region.n);
    tg_free(&preview);
    tg_free(&meta);
    tg_free(&region);
    return b;
}

/* Mosaics linear RGB through any CFA repeat (cfa_w colours per row) anchored at (0, 0) (malloc'd). */
static inline uint16_t *tg_mosaic_cfa(const float (*rgb)[3], size_t n, size_t width, const uint8_t *cfa,
                                      size_t ncfa, size_t cfa_w, uint16_t black, uint16_t white)
{
    uint16_t *out = tg_xalloc(n * sizeof *out);
    size_t cfa_h = ncfa / cfa_w;
    size_t i;
    for (i = 0; i < n; i++) {
        size_t x = i % width;
        size_t y = i / width;
        size_t c = cfa[(y % cfa_h) * cfa_w + x % cfa_w];
        out[i] = pc_f32_to_u16(
            roundf((float)black + pc_clampf(rgb[i][c], 0.0f, 1.0f) * (float)(uint16_t)(white - black)));
    }
    return out;
}

#endif
