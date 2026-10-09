/* Nikon Huffman-compressed NEF sensor data (TIFF compression 34713), lossless and lossy, 12 and
 * 14 bits: a port of crates/raw/src/nefc.rs (see it for the clean-room sources). One strip, an
 * MSB-first bit stream without stuffing; per pixel a Huffman category and that many extra bits;
 * each pixel predicted from the same-colour pixel two to the left, the first two of a row from
 * the previous row of the same parity, starting at the maker note 0x0096 seeds. Lossy files index
 * a curve (type 1: the whole curve; type 2: interpolated points). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "raw/internal.h"

/* A code word that never occurred in any analysed sample. */
#define UNSEEN 0xFF
/* Longest code word in the tables. */
#define MAX_LEN 9u

typedef struct table {
    const uint8_t *counts;
    size_t ncounts;
    const uint8_t *symbols;
    size_t nsymbols;
} table;

static const uint8_t LL12_C[] = {0, 1, 4, 2, 3, 1, 2};
static const uint8_t LL12_S[] = {5, 4, 6, 3, 7, 2, 8, 1, 9, 0, 10, 11, 12};
static const uint8_t LL14_C[] = {0, 1, 4, 2, 2, 3, 1, 2};
static const uint8_t LL14_S[] = {7, 6, 8, 5, 9, 4, 10, 3, 11, 12, 2, 0, 1, 13, 14};
static const uint8_t LY12_C[] = {0, 1, 5, 1, 1, 1, 1, 1, 1};
static const uint8_t LY12_S[] = {5, 4, 3, 6, 2, 7, 1, 0, 8, 9, UNSEEN, 10};
static const uint8_t LY14_C[] = {0, 1, 4, 3, 1, 1, 1, 1, 1};
static const uint8_t LY14_S[] = {5, 6, 4, 7, 8, 3, 9, 2, 1, 0, 10, 11, 12};

#define T(c, s) {c, sizeof c, s, sizeof s}
static const table LOSSLESS_12 = T(LL12_C, LL12_S);
static const table LOSSLESS_14 = T(LL14_C, LL14_S);
static const table LOSSY_12 = T(LY12_C, LY12_S);
static const table LOSSY_14 = T(LY14_C, LY14_S);
#undef T

typedef struct huffman {
    uint8_t cat[1u << MAX_LEN];
    uint8_t len[1u << MAX_LEN]; /* 0 = no such code */
} huffman;

static pc_status huffman_new(const table *t, huffman *h, pc_error *err)
{
    uint32_t code = 0;
    size_t sym = 0;
    size_t i;
    memset(h, 0, sizeof *h);
    for (i = 0; i < t->ncounts; i++) {
        uint32_t len = (uint32_t)i + 1;
        size_t n = t->counts[i];
        size_t j;
        if (len > MAX_LEN && n > 0) {
            return rx_malformed(err, "NEF Huffman code longer than the lookup");
        }
        for (j = 0; j < n; j++) {
            uint8_t s;
            size_t first;
            size_t span;
            if (sym >= t->nsymbols) {
                return rx_malformed(err, "NEF Huffman table is short of symbols");
            }
            s = t->symbols[sym++];
            first = (size_t)code << (MAX_LEN - len);
            span = (size_t)1 << (MAX_LEN - len);
            if (s != UNSEEN) {
                size_t k;
                if (first + span > (1u << MAX_LEN)) {
                    return rx_malformed(err, "NEF Huffman table overflows");
                }
                for (k = first; k < first + span; k++) {
                    h->cat[k] = s;
                    h->len[k] = (uint8_t)len;
                }
            }
            code++;
        }
        code <<= 1;
    }
    return PC_OK;
}

/* MSB-first bit reader without stuffing; reads past the end return zero bits. */
typedef struct bits {
    const uint8_t *src;
    size_t n_src;
    size_t pos;
    uint64_t acc;
    uint32_t n;
    size_t consumed;
} bits;

static void fill(bits *b)
{
    while (b->n <= 56) {
        uint8_t v = b->pos < b->n_src ? b->src[b->pos] : 0;
        b->pos++;
        b->acc |= (uint64_t)v << (56 - b->n);
        b->n += 8;
    }
}

static uint32_t peek(bits *b, uint32_t k)
{
    fill(b);
    return (uint32_t)(b->acc >> (64 - k));
}

static void skip(bits *b, uint32_t k)
{
    b->acc <<= k;
    b->n -= k;
    b->consumed += k;
}

static uint32_t take(bits *b, uint32_t k)
{
    uint32_t v;
    if (k == 0) {
        return 0;
    }
    v = peek(b, k);
    skip(b, k);
    return v;
}

/* One coded difference; false for a code word not in the table. */
static bool diff(bits *b, const huffman *h, int32_t *out)
{
    uint32_t i = peek(b, MAX_LEN);
    uint32_t cat;
    int32_t v;
    if (h->len[i] == 0) {
        return false;
    }
    skip(b, h->len[i]);
    cat = h->cat[i];
    if (cat > 16) {
        return false;
    }
    v = (int32_t)take(b, cat);
    /* T.81 F.1.2.1: a leading 0 bit means a negative difference. */
    *out = cat > 0 && v < (1 << (cat - 1)) ? v - (1 << cat) + 1 : v;
    return true;
}

static bool u16_at(const uint8_t *t, size_t n, size_t o, bool le, uint16_t *v)
{
    if (o > n || n - o < 2) {
        return false;
    }
    *v = (uint16_t)(le ? (unsigned)((unsigned)t[o] | (unsigned)t[o + 1] << 8) : (unsigned)((unsigned)t[o] << 8 | (unsigned)t[o + 1]));
    return true;
}

void rx_nef_table_fini(rx_nef_table *t)
{
    free(t->curve);
    t->curve = NULL;
    t->ncurve = 0;
}

pc_status rx_nef_parse_table(const uint8_t *t, size_t n, bool le, uint32_t bits_, rx_nef_table *out, pc_error *err)
{
    rx_nef_table r;
    uint8_t v0;
    uint8_t v1;
    int i;
    memset(&r, 0, sizeof r);
    if (bits_ != 12 && bits_ != 14) {
        return rx_unsupported(err, "Nikon compressed NEF with %u-bit samples", (unsigned)bits_);
    }
    if (n < 2) {
        return rx_malformed(err, "NEF linearization table too short (%zu bytes)", n);
    }
    v0 = t[0];
    v1 = t[1];
    for (i = 0; i < 4; i++) {
        uint16_t s;
        if (!u16_at(t, n, 2 + 2 * (size_t)i, le, &s)) {
            return rx_malformed(err, "NEF linearization table too short (%zu bytes)", n);
        }
        r.seeds[i / 2][i % 2] = s;
    }
    if (v0 == 0x46) {
        *out = r;
        return PC_OK;
    }
    if (v0 == 0x44 && (v1 == 0x10 || v1 == 0x20 || v1 == 0x40)) {
        uint16_t n16;
        size_t np;
        uint16_t *points;
        size_t k;
        if (!u16_at(t, n, 10, le, &n16)) {
            return rx_malformed(err, "NEF linearization table too short (%zu bytes)", n);
        }
        np = n16;
        if (np < 2) {
            return rx_malformed(err, "NEF linearization curve of %zu points", np);
        }
        points = pc_malloc_array(np, sizeof *points);
        if (points == NULL) {
            return rx_nomem(err);
        }
        for (k = 0; k < np; k++) {
            if (!u16_at(t, n, 12 + 2 * k, le, &points[k])) {
                free(points);
                return rx_malformed(err, "NEF linearization table too short (%zu bytes)", n);
            }
        }
        if (v1 == 0x10) {
            r.curve = points;
            r.ncurve = np;
        } else {
            uint16_t split = 0;
            size_t range = (size_t)1 << bits_;
            uint32_t step;
            size_t at = 0;
            (void)u16_at(t, n, 562, le, &split);
            if (split != 0) {
                free(points);
                return rx_unsupported(err, "Nikon \"lossy after split\" compressed NEF (split at row %u)",
                                      (unsigned)split);
            }
            if (np - 1 > range || range % (np - 1) != 0) {
                free(points);
                return rx_unsupported(err, "Nikon compressed NEF: %zu-point curve for %u-bit data", np,
                                      (unsigned)bits_);
            }
            step = (uint32_t)(range / (np - 1));
            r.ncurve = (np - 1) * step + 1;
            r.curve = pc_malloc_array(r.ncurve, sizeof *r.curve);
            if (r.curve == NULL) {
                free(points);
                return rx_nomem(err);
            }
            for (k = 0; k + 1 < np; k++) {
                uint32_t a = points[k];
                uint32_t b = points[k + 1];
                uint32_t q;
                for (q = 0; q < step; q++) {
                    r.curve[at++] = (uint16_t)((a * (step - q) + b * q) / step);
                }
            }
            r.curve[at] = points[np - 1];
            free(points);
        }
        *out = r;
        return PC_OK;
    }
    return rx_unsupported(err, "Nikon compressed NEF version 0x%02x 0x%02x", (unsigned)v0, (unsigned)v1);
}

pc_status rx_nef_decode(const uint8_t *src, size_t nsrc, size_t w, size_t h, uint32_t bits_, const rx_nef_table *t,
                        uint16_t **out, pc_error *err)
{
    size_t n;
    huffman *huff;
    const table *tb;
    int32_t max;
    uint16_t *o;
    bits b;
    int32_t vpred[2][2];
    size_t clipped = 0;
    size_t y;
    pc_status st;
    if (bits_ != 12 && bits_ != 14) {
        return rx_unsupported(err, "Nikon compressed NEF with %u-bit samples", (unsigned)bits_);
    }
    if ((h != 0 && w > SIZE_MAX / h) || (n = w * h) == 0) {
        return rx_malformed(err, "NEF image is empty");
    }
    /* Every code word is at least 2 bits: a strip that can't hold the image is truncated. */
    if (n / 4 > nsrc) {
        return rx_malformed(err, "NEF: %zu bytes of compressed data for %zux%zu pixels", nsrc, w, h);
    }
    if (t->curve == NULL) {
        tb = bits_ == 12 ? &LOSSLESS_12 : &LOSSLESS_14;
        max = (1 << bits_) - 1;
    } else {
        tb = bits_ == 12 ? &LOSSY_12 : &LOSSY_14;
        max = t->ncurve > (size_t)INT32_MAX ? INT32_MAX - 1 : (int32_t)t->ncurve - 1;
    }
    huff = malloc(sizeof *huff);
    if (huff == NULL) {
        return rx_nomem(err);
    }
    if ((st = huffman_new(tb, huff, err)) != PC_OK) {
        free(huff);
        return st;
    }
    o = pc_malloc_array(n, sizeof *o);
    if (o == NULL) {
        free(huff);
        return rx_nomem(err);
    }
    memset(&b, 0, sizeof b);
    b.src = src;
    b.n_src = nsrc;
    memcpy(vpred, t->seeds, sizeof vpred);
    for (y = 0; y < h; y++) {
        int32_t *seeds = vpred[y & 1];
        int32_t hpred[2] = {0, 0};
        size_t x;
        for (x = 0; x < w; x++) {
            int32_t d;
            int32_t p;
            int32_t v;
            if (!diff(&b, huff, &d)) {
                free(huff);
                free(o);
                return rx_malformed(err, "NEF: invalid Huffman code in row %zu", y);
            }
            if (x < 2) {
                seeds[x] = pc_i32_sat_add(seeds[x], d);
                hpred[x] = seeds[x];
                p = seeds[x];
            } else {
                hpred[x & 1] = pc_i32_sat_add(hpred[x & 1], d);
                p = hpred[x & 1];
            }
            v = p < 0 ? 0 : p > max ? max : p;
            clipped += v != p;
            if (t->curve == NULL) {
                o[y * w + x] = (uint16_t)v;
            } else {
                o[y * w + x] = (size_t)v < t->ncurve ? t->curve[v] : 0;
            }
        }
        /* The camera pads the strip; reading well past its end means truncated or corrupt data. */
        if (b.consumed > (nsrc > (SIZE_MAX - 64) / 8 ? SIZE_MAX : nsrc * 8 + 64)) {
            free(huff);
            free(o);
            return rx_malformed(err, "NEF: compressed data ends in row %zu of %zu", y, h);
        }
    }
    free(huff);
    /* A wrong table or a corrupt stream drifts out of range quickly; real files stay inside. */
    if (clipped > n / 1000 + 16) {
        free(o);
        return rx_malformed(err, "NEF: %zu decoded values out of range", clipped);
    }
    *out = o;
    return PC_OK;
}

pc_status rx_nef_read_compressed(const rx_tiff *t, const rx_ifd *raw, const rx_tiff *note_t, const rx_ifd *note,
                                 const pc_raw_limits *limits, rx_plane *out, pc_error *err)
{
    uint32_t v;
    size_t width;
    size_t height;
    uint32_t samples;
    uint32_t bits_;
    rx_entry e;
    const uint8_t *tb = NULL;
    size_t tblen = 0;
    rx_nef_table tbl;
    rx_entry oe;
    rx_entry ce;
    size_t noffs;
    size_t ncounts;
    uint32_t first;
    uint32_t last;
    uint32_t last_len;
    size_t start;
    size_t end;
    uint16_t *data;
    pc_status st;
    if (!rx_tag_uint(t, raw, RX_IMAGE_WIDTH, &v)) {
        return rx_malformed(err, "raw image has no width");
    }
    width = v;
    if (!rx_tag_uint(t, raw, RX_IMAGE_LENGTH, &v)) {
        return rx_malformed(err, "raw image has no height");
    }
    height = v;
    samples = rx_tag_uint(t, raw, RX_SAMPLES_PER_PIXEL, &v) ? v : 1;
    if (samples != 1) {
        return rx_unsupported(err, "Nikon compressed NEF with %u samples per pixel", (unsigned)samples);
    }
    bits_ = rx_tag_uint(t, raw, RX_BITS_PER_SAMPLE, &v) ? v : 0;
    if (note_t == NULL || !rx_ifd_get(note_t, note, RX_NIKON_LINEARIZATION_TABLE, &e) ||
        (tb = rx_raw(note_t, &e, &tblen)) == NULL) {
        return rx_unsupported(err, "Nikon compressed NEF without a linearization table (maker note 0x96)");
    }
    if ((st = rx_nef_parse_table(tb, tblen, note_t->le, bits_, &tbl, err)) != PC_OK) {
        return st;
    }
    if ((st = pc_raw_limits_check(limits, width, height, 2, err)) != PC_OK) {
        rx_nef_table_fini(&tbl);
        return st;
    }
    /* One strip in every sample; several would be consecutive parts of the same bit stream. */
    noffs = rx_ifd_get(t, raw, RX_STRIP_OFFSETS, &oe) ? rx_uints_n(t, &oe) : 0;
    ncounts = rx_ifd_get(t, raw, RX_STRIP_BYTE_COUNTS, &ce) ? rx_uints_n(t, &ce) : 0;
    if (noffs == 0 || ncounts == 0) {
        rx_nef_table_fini(&tbl);
        return rx_malformed(err, "NEF: no image data");
    }
    (void)rx_uint(t, &oe, 0, &first);
    (void)rx_uint(t, &oe, noffs - 1, &last);
    (void)rx_uint(t, &ce, ncounts - 1, &last_len);
    start = first;
    end = (size_t)last + last_len;
    if (end > t->len) {
        end = t->len;
    }
    if (start >= end) {
        rx_nef_table_fini(&tbl);
        return rx_malformed(err, "raw data lies outside the file");
    }
    st = rx_nef_decode(t->data + start, end - start, width, height, bits_, &tbl, &data, err);
    rx_nef_table_fini(&tbl);
    if (st != PC_OK) {
        return st;
    }
    out->width = width;
    out->height = height;
    out->samples = 1;
    out->bits = bits_;
    out->data = data;
    return PC_OK;
}
