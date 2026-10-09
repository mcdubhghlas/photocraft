/* Synthetic raw-file generators for the raw tests (port of crates/raw/src/testgen.rs): a
 * lossless-JPEG encoder, a small TIFF writer, and DNG / CR2 / TIFF-EP / NEF / Sony cRAW /
 * Panasonic RW2 / Olympus ORF builders. The bytes match the reference generator exactly
 * (tests/raw/test_parity.c checks it). Test-only: allocation failures abort. */
#ifndef PC_TESTS_RAW_TESTGEN_H
#define PC_TESTS_RAW_TESTGEN_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/num.h"

/* ---- growable bytes ---- */

typedef struct tg_bytes {
    uint8_t *d;
    size_t n;
    size_t cap;
} tg_bytes;

static inline void *tg_xalloc(size_t n)
{
    void *p = calloc(n > 0 ? n : 1, 1);
    if (p == NULL) {
        (void)fprintf(stderr, "testgen: out of memory\n");
        abort();
    }
    return p;
}

static inline void tg_reserve(tg_bytes *b, size_t extra)
{
    if (b->n + extra > b->cap) {
        size_t cap = b->cap > 0 ? b->cap : 64;
        uint8_t *d;
        while (cap < b->n + extra) {
            cap *= 2;
        }
        d = realloc(b->d, cap);
        if (d == NULL) {
            abort();
        }
        b->d = d;
        b->cap = cap;
    }
}

static inline void tg_put(tg_bytes *b, const void *p, size_t n)
{
    tg_reserve(b, n);
    if (n > 0) {
        memcpy(b->d + b->n, p, n);
    }
    b->n += n;
}

static inline void tg_byte(tg_bytes *b, uint8_t v) { tg_put(b, &v, 1); }

static inline void tg_be16(tg_bytes *b, uint16_t v)
{
    uint8_t x[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    tg_put(b, x, 2);
}

static inline void tg_be32(tg_bytes *b, uint32_t v)
{
    uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    tg_put(b, x, 4);
}

static inline void tg_le16(tg_bytes *b, uint16_t v)
{
    uint8_t x[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    tg_put(b, x, 2);
}

static inline void tg_le32(tg_bytes *b, uint32_t v)
{
    uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    tg_put(b, x, 4);
}

static inline void tg_resize(tg_bytes *b, size_t n)
{
    if (n > b->n) {
        tg_reserve(b, n - b->n);
        memset(b->d + b->n, 0, n - b->n);
    }
    b->n = n;
}

static inline void tg_free(tg_bytes *b)
{
    free(b->d);
    memset(b, 0, sizeof *b);
}

/* ---- lossless JPEG ---- */

/* Huffman code lengths assigned to categories in decreasing frequency order. */
static const uint8_t TG_LENGTHS[17] = {2, 3, 3, 3, 4, 4, 5, 5, 6, 6, 7, 8, 9, 10, 11, 12, 13};

static inline uint32_t tg_category(int32_t d)
{
    uint32_t a;
    uint32_t n = 0;
    if (d == 0) {
        return 0;
    }
    if (d == 32768) {
        return 16;
    }
    a = d < 0 ? (uint32_t)(-(int64_t)d) : (uint32_t)d;
    while (a != 0) {
        n++;
        a >>= 1;
    }
    return n;
}

typedef struct tg_bitwriter {
    tg_bytes *out;
    uint32_t acc;
    uint32_t n;
} tg_bitwriter;

static inline void tg_bw_put(tg_bitwriter *w, uint32_t v, uint32_t bits)
{
    uint32_t i;
    for (i = bits; i > 0; i--) {
        w->acc = (w->acc << 1) | ((v >> (i - 1)) & 1);
        w->n++;
        if (w->n == 8) {
            uint8_t b = (uint8_t)w->acc;
            tg_byte(w->out, b);
            if (b == 0xFF) {
                tg_byte(w->out, 0);
            }
            w->acc = 0;
            w->n = 0;
        }
    }
}

static inline void tg_bw_flush(tg_bitwriter *w)
{
    if (w->n > 0) {
        uint32_t pad = 8 - w->n;
        tg_bw_put(w, (1u << pad) - 1, pad);
    }
}

/* Encodes samples (raster order, components interleaved) as a lossless JPEG with the given
 * predictor (1-7) and a restart interval of restart_rows rows (0 = none). */
static inline tg_bytes tg_lj92_encode(const uint16_t *samples, size_t width, size_t height, size_t components,
                                      uint8_t precision, uint8_t predictor, size_t restart_rows)
{
    size_t row = width * components;
    size_t interval = restart_rows > 0 ? restart_rows : SIZE_MAX;
    size_t total = width * height * components;
    int32_t *diffs = tg_xalloc(total * sizeof *diffs);
    int32_t initial = 1 << (precision - 1);
    size_t freq[17] = {0};
    size_t order[17];
    uint8_t len_of[17] = {0};
    uint8_t counts[16] = {0};
    uint8_t vals[17];
    size_t nvals = 0;
    uint32_t code_of[17][2] = {{0}};
    uint32_t code = 0;
    size_t x;
    size_t y;
    size_t c;
    size_t i;
    tg_bytes out = {NULL, 0, 0};
    tg_bitwriter w;
    uint8_t rst = 0;
    size_t k = 0;
    for (y = 0; y < height; y++) {
        bool first_line = y % interval == 0;
        for (x = 0; x < width; x++) {
            for (c = 0; c < components; c++) {
                size_t idx = y * row + x * components + c;
                int32_t px;
                int32_t d;
                if (x == 0) {
                    px = first_line ? initial : (int32_t)samples[idx - row];
                } else if (first_line) {
                    px = samples[idx - components];
                } else {
                    int32_t ra = samples[idx - components];
                    int32_t rb = samples[idx - row];
                    int32_t rc = samples[idx - row - components];
                    switch (predictor) {
                    case 1:
                        px = ra;
                        break;
                    case 2:
                        px = rb;
                        break;
                    case 3:
                        px = rc;
                        break;
                    case 4:
                        px = ra + rb - rc;
                        break;
                    case 5:
                        px = ra + (int32_t)floor((double)(rb - rc) / 2.0);
                        break;
                    case 6:
                        px = rb + (int32_t)floor((double)(ra - rc) / 2.0);
                        break;
                    default:
                        px = (ra + rb) >> 1;
                    }
                }
                d = (int32_t)(((uint32_t)samples[idx] - (uint32_t)px) & 0xFFFF);
                if (d > 32768) {
                    d -= 65536;
                }
                diffs[k++] = d;
            }
        }
    }
    for (i = 0; i < total; i++) {
        freq[tg_category(diffs[i])]++;
    }
    /* Stable sort of categories by decreasing frequency. */
    for (i = 0; i < 17; i++) {
        order[i] = i;
    }
    for (i = 1; i < 17; i++) {
        size_t j = i;
        while (j > 0 && freq[order[j - 1]] < freq[order[j]]) {
            size_t t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
            j--;
        }
    }
    for (i = 0; i < 17; i++) {
        len_of[order[i]] = TG_LENGTHS[i];
    }
    for (x = 1; x <= 16; x++) {
        for (c = 0; c < 17; c++) {
            if (len_of[c] == x) {
                counts[x - 1]++;
                vals[nvals++] = (uint8_t)c;
                code_of[c][0] = code;
                code_of[c][1] = (uint32_t)x;
                code++;
            }
        }
        code <<= 1;
    }
    tg_byte(&out, 0xFF);
    tg_byte(&out, 0xD8);
    tg_byte(&out, 0xFF);
    tg_byte(&out, 0xC3);
    tg_be16(&out, (uint16_t)(8 + 3 * components));
    tg_byte(&out, precision);
    tg_be16(&out, (uint16_t)height);
    tg_be16(&out, (uint16_t)width);
    tg_byte(&out, (uint8_t)components);
    for (c = 0; c < components; c++) {
        uint8_t s3[3] = {(uint8_t)(c + 1), 0x11, 0};
        tg_put(&out, s3, 3);
    }
    tg_byte(&out, 0xFF);
    tg_byte(&out, 0xC4);
    tg_be16(&out, (uint16_t)(2 + 17 + nvals));
    tg_byte(&out, 0x00);
    tg_put(&out, counts, 16);
    tg_put(&out, vals, nvals);
    if (restart_rows > 0) {
        uint8_t dri[4] = {0xFF, 0xDD, 0, 4};
        tg_put(&out, dri, 4);
        tg_be16(&out, (uint16_t)(restart_rows * width));
    }
    tg_byte(&out, 0xFF);
    tg_byte(&out, 0xDA);
    tg_be16(&out, (uint16_t)(6 + 2 * components));
    tg_byte(&out, (uint8_t)components);
    for (c = 0; c < components; c++) {
        tg_byte(&out, (uint8_t)(c + 1));
        tg_byte(&out, 0x00);
    }
    tg_byte(&out, predictor);
    tg_byte(&out, 0);
    tg_byte(&out, 0);
    w.out = &out;
    w.acc = 0;
    w.n = 0;
    for (i = 0; i < total; i++) {
        int32_t d = diffs[i];
        uint32_t s;
        size_t period = interval > SIZE_MAX / (row > 0 ? row : 1) ? SIZE_MAX : interval * row;
        if (i > 0 && i % period == 0) {
            uint8_t m[2];
            tg_bw_flush(&w);
            m[0] = 0xFF;
            m[1] = (uint8_t)(0xD0 + rst);
            tg_put(&out, m, 2);
            rst = (uint8_t)((rst + 1) % 8);
        }
        s = tg_category(d);
        tg_bw_put(&w, code_of[s][0], code_of[s][1]);
        if (s >= 1 && s < 16) {
            uint32_t extra = d < 0 ? (uint32_t)(d - 1) & ((1u << s) - 1) : (uint32_t)d;
            tg_bw_put(&w, extra, s);
        }
    }
    tg_bw_flush(&w);
    tg_byte(&out, 0xFF);
    tg_byte(&out, 0xD9);
    free(diffs);
    return out;
}

/* ---- TIFF writer ---- */

typedef enum tg_kind {
    TG_BYTE,
    TG_ASCII,
    TG_SHORT,
    TG_LONG,
    TG_DOUBLE,
    TG_RATIONAL,
    TG_SRATIONAL,
    TG_UNDEFINED,
    TG_BLOBS,    /* offsets of these blobs (LONG) */
    TG_IFDS,     /* offsets of these IFDs (LONG) */
    TG_IFDBYTES  /* an UNDEFINED value whose bytes are this IFD (a maker note) */
} tg_kind;

/* Element storage: bytes for BYTE/ASCII/UNDEFINED, else one uint64 per value (two per rational;
 * double bits for DOUBLE; indices for BLOBS/IFDS/IFDBYTES). */
typedef struct tg_val {
    tg_kind k;
    size_t n;
    uint8_t *bytes;
    uint64_t *v;
} tg_val;

typedef struct tg_entry {
    uint16_t tag;
    tg_val val;
} tg_entry;

typedef struct tg_ifd {
    tg_entry *e;
    size_t n;
} tg_ifd;

typedef struct tg_tiff {
    bool big_endian;
    tg_ifd *ifds;
    size_t nifds;
    size_t *chain;
    size_t nchain;
    tg_bytes *blobs;
    size_t nblobs;
    bool has_cr2;
    size_t cr2_raw_ifd;
} tg_tiff;

static inline tg_val tg_v_bytes(tg_kind k, const void *b, size_t n)
{
    tg_val v;
    v.k = k;
    v.n = n;
    v.bytes = tg_xalloc(n);
    if (n > 0) {
        memcpy(v.bytes, b, n);
    }
    v.v = NULL;
    return v;
}

static inline tg_val tg_v_ascii(const char *s) { return tg_v_bytes(TG_ASCII, s, strlen(s)); }

static inline tg_val tg_v_nums(tg_kind k, const uint64_t *vals, size_t n)
{
    tg_val v;
    size_t per = k == TG_RATIONAL || k == TG_SRATIONAL ? 2 : 1;
    v.k = k;
    v.n = n;
    v.bytes = NULL;
    v.v = tg_xalloc(n * per * sizeof *v.v);
    if (n > 0) {
        memcpy(v.v, vals, n * per * sizeof *vals);
    }
    return v;
}

static inline tg_val tg_v_short(const uint16_t *s, size_t n)
{
    uint64_t *t = tg_xalloc(n * sizeof *t);
    size_t i;
    tg_val v;
    for (i = 0; i < n; i++) {
        t[i] = s[i];
    }
    v = tg_v_nums(TG_SHORT, t, n);
    free(t);
    return v;
}

static inline tg_val tg_v_long(const uint32_t *s, size_t n)
{
    uint64_t *t = tg_xalloc(n * sizeof *t);
    size_t i;
    tg_val v;
    for (i = 0; i < n; i++) {
        t[i] = s[i];
    }
    v = tg_v_nums(TG_LONG, t, n);
    free(t);
    return v;
}

static inline tg_val tg_v_short1(uint16_t a) { return tg_v_short(&a, 1); }
static inline tg_val tg_v_long1(uint32_t a) { return tg_v_long(&a, 1); }
static inline tg_val tg_v_ref(tg_kind k, size_t i)
{
    uint64_t v = i;
    return tg_v_nums(k, &v, 1);
}

static inline void tg_val_free(tg_val *v)
{
    free(v->bytes);
    free(v->v);
}

static inline size_t tg_ifd_add(tg_tiff *t, tg_entry *e, size_t n)
{
    tg_ifd *ifds = realloc(t->ifds, (t->nifds + 1) * sizeof *ifds);
    if (ifds == NULL) {
        abort();
    }
    t->ifds = ifds;
    t->ifds[t->nifds].e = tg_xalloc(n * sizeof *e);
    memcpy(t->ifds[t->nifds].e, e, n * sizeof *e);
    t->ifds[t->nifds].n = n;
    return t->nifds++;
}

static inline size_t tg_blob(tg_tiff *t, tg_bytes b)
{
    tg_bytes *bl = realloc(t->blobs, (t->nblobs + 1) * sizeof *bl);
    if (bl == NULL) {
        abort();
    }
    t->blobs = bl;
    t->blobs[t->nblobs] = b;
    return t->nblobs++;
}

static inline void tg_tiff_free(tg_tiff *t)
{
    size_t i;
    size_t k;
    for (i = 0; i < t->nifds; i++) {
        for (k = 0; k < t->ifds[i].n; k++) {
            tg_val_free(&t->ifds[i].e[k].val);
        }
        free(t->ifds[i].e);
    }
    free(t->ifds);
    for (i = 0; i < t->nblobs; i++) {
        tg_free(&t->blobs[i]);
    }
    free(t->blobs);
    free(t->chain);
    memset(t, 0, sizeof *t);
}

static inline size_t tg_ifd_size(const tg_tiff *t, size_t i) { return 2 + 12 * t->ifds[i].n + 4; }

static inline size_t tg_val_size(const tg_tiff *t, const tg_val *v)
{
    switch (v->k) {
    case TG_BYTE:
    case TG_UNDEFINED:
        return v->n;
    case TG_ASCII:
        return v->n + 1;
    case TG_SHORT:
        return 2 * v->n;
    case TG_LONG:
    case TG_BLOBS:
    case TG_IFDS:
        return 4 * v->n;
    case TG_IFDBYTES:
        return tg_ifd_size(t, (size_t)v->v[0]);
    default:
        return 8 * v->n;
    }
}

/* Entry order sorted by tag (stable). */
static inline void tg_sorted(const tg_tiff *t, size_t i, size_t *order)
{
    size_t n = t->ifds[i].n;
    size_t a;
    for (a = 0; a < n; a++) {
        order[a] = a;
    }
    for (a = 1; a < n; a++) {
        size_t j = a;
        while (j > 0 && t->ifds[i].e[order[j - 1]].tag > t->ifds[i].e[order[j]].tag) {
            size_t x = order[j];
            order[j] = order[j - 1];
            order[j - 1] = x;
            j--;
        }
    }
}

static inline void tg_w16(const tg_tiff *t, uint8_t *o, size_t at, uint16_t v)
{
    if (t->big_endian) {
        o[at] = (uint8_t)(v >> 8);
        o[at + 1] = (uint8_t)v;
    } else {
        o[at] = (uint8_t)v;
        o[at + 1] = (uint8_t)(v >> 8);
    }
}

static inline void tg_w32(const tg_tiff *t, uint8_t *o, size_t at, uint32_t v)
{
    int k;
    for (k = 0; k < 4; k++) {
        o[at + (size_t)k] = (uint8_t)(t->big_endian ? v >> (24 - 8 * k) : v >> (8 * k));
    }
}

/* (type, count, bytes) of a value. */
static inline uint16_t tg_encode(const tg_tiff *t, const tg_val *v, const size_t *blob_pos, const size_t *ifd_pos,
                                 uint32_t *count, tg_bytes *bytes)
{
    size_t i;
    uint8_t tmp[8];
    bytes->n = 0;
    switch (v->k) {
    case TG_BYTE:
        *count = (uint32_t)v->n;
        tg_put(bytes, v->bytes, v->n);
        return 1;
    case TG_UNDEFINED:
        *count = (uint32_t)v->n;
        tg_put(bytes, v->bytes, v->n);
        return 7;
    case TG_ASCII:
        *count = (uint32_t)v->n + 1;
        tg_put(bytes, v->bytes, v->n);
        tg_byte(bytes, 0);
        return 2;
    case TG_SHORT:
        *count = (uint32_t)v->n;
        for (i = 0; i < v->n; i++) {
            tg_w16(t, tmp, 0, (uint16_t)v->v[i]);
            tg_put(bytes, tmp, 2);
        }
        return 3;
    case TG_LONG:
    case TG_BLOBS:
    case TG_IFDS:
        *count = (uint32_t)v->n;
        for (i = 0; i < v->n; i++) {
            uint64_t x = v->k == TG_LONG ? v->v[i] : v->k == TG_BLOBS ? blob_pos[v->v[i]] : ifd_pos[v->v[i]];
            tg_w32(t, tmp, 0, (uint32_t)x);
            tg_put(bytes, tmp, 4);
        }
        return 4;
    case TG_DOUBLE:
        *count = (uint32_t)v->n;
        for (i = 0; i < v->n; i++) {
            uint64_t x = v->v[i];
            int k;
            for (k = 0; k < 8; k++) {
                tmp[k] = (uint8_t)(t->big_endian ? x >> (56 - 8 * k) : x >> (8 * k));
            }
            tg_put(bytes, tmp, 8);
        }
        return 12;
    case TG_RATIONAL:
    case TG_SRATIONAL:
        *count = (uint32_t)v->n;
        for (i = 0; i < 2 * v->n; i++) {
            tg_w32(t, tmp, 0, (uint32_t)v->v[i]);
            tg_put(bytes, tmp, 4);
        }
        return v->k == TG_RATIONAL ? 5 : 10;
    default: /* TG_IFDBYTES */
        *count = (uint32_t)tg_ifd_size(t, (size_t)v->v[0]);
        tg_resize(bytes, 8);
        return 7;
    }
}

static inline tg_bytes tg_tiff_build(const tg_tiff *t)
{
    size_t header = t->has_cr2 ? 16 : 8;
    size_t pos = header;
    size_t *ifd_pos = tg_xalloc(t->nifds * sizeof *ifd_pos);
    size_t **val_pos = tg_xalloc(t->nifds * sizeof *val_pos);
    bool *embedded = tg_xalloc(t->nifds * sizeof *embedded);
    size_t *blob_pos = tg_xalloc(t->nblobs * sizeof *blob_pos);
    size_t *order = tg_xalloc(4096 * sizeof *order);
    size_t i;
    size_t k;
    tg_bytes out = {NULL, 0, 0};
    tg_bytes tmp = {NULL, 0, 0};
    for (i = 0; i < t->nifds; i++) {
        val_pos[i] = tg_xalloc((t->ifds[i].n + 1) * sizeof **val_pos);
        for (k = 0; k < t->ifds[i].n; k++) {
            if (t->ifds[i].e[k].val.k == TG_IFDBYTES) {
                embedded[t->ifds[i].e[k].val.v[0]] = true;
            }
        }
    }
    for (i = 0; i < t->nifds; i++) {
        if (embedded[i]) {
            continue;
        }
        ifd_pos[i] = pos;
        pos += tg_ifd_size(t, i);
        tg_sorted(t, i, order);
        for (k = 0; k < t->ifds[i].n; k++) {
            const tg_val *v = &t->ifds[i].e[order[k]].val;
            size_t size = tg_val_size(t, v);
            if (size > 4) {
                val_pos[i][k] = pos;
                if (v->k == TG_IFDBYTES) {
                    size_t e = (size_t)v->v[0];
                    size_t q = pos + tg_ifd_size(t, e);
                    size_t *inner = tg_xalloc(t->ifds[e].n * sizeof *inner + 1);
                    size_t j;
                    ifd_pos[e] = pos;
                    tg_sorted(t, e, inner);
                    for (j = 0; j < t->ifds[e].n; j++) {
                        size_t s = tg_val_size(t, &t->ifds[e].e[inner[j]].val);
                        if (s > 4) {
                            val_pos[e][j] = q;
                            q += s + (s & 1);
                        } else {
                            val_pos[e][j] = 0;
                        }
                    }
                    free(inner);
                    pos = q;
                    tg_sorted(t, i, order);
                } else {
                    pos += size + (size & 1);
                }
            } else {
                val_pos[i][k] = 0;
            }
        }
    }
    for (i = 0; i < t->nblobs; i++) {
        blob_pos[i] = pos;
        pos += t->blobs[i].n + (t->blobs[i].n & 1);
    }
    tg_resize(&out, pos);
    memcpy(out.d, t->big_endian ? "MM\0*" : "II*\0", 4);
    tg_w32(t, out.d, 4, (uint32_t)ifd_pos[t->chain[0]]);
    if (t->has_cr2) {
        memcpy(out.d + 8, "CR\x02\0", 4);
        tg_w32(t, out.d, 12, (uint32_t)ifd_pos[t->cr2_raw_ifd]);
    }
    for (i = 0; i < t->nifds; i++) {
        size_t at = ifd_pos[i];
        size_t next = 0;
        size_t c;
        tg_sorted(t, i, order);
        tg_w16(t, out.d, at, (uint16_t)t->ifds[i].n);
        for (k = 0; k < t->ifds[i].n; k++) {
            const tg_entry *en = &t->ifds[i].e[order[k]];
            size_t e = at + 2 + 12 * k;
            uint32_t count;
            uint16_t typ = tg_encode(t, &en->val, blob_pos, ifd_pos, &count, &tmp);
            tg_w16(t, out.d, e, en->tag);
            tg_w16(t, out.d, e + 2, typ);
            tg_w32(t, out.d, e + 4, count);
            if (tmp.n <= 4) {
                if (tmp.n > 0) {
                    memcpy(out.d + e + 8, tmp.d, tmp.n);
                }
            } else {
                size_t vp = val_pos[i][k];
                tg_w32(t, out.d, e + 8, (uint32_t)vp);
                if (en->val.k != TG_IFDBYTES) {
                    memcpy(out.d + vp, tmp.d, tmp.n);
                }
            }
        }
        for (c = 0; c < t->nchain; c++) {
            if (t->chain[c] == i) {
                next = c + 1 < t->nchain ? ifd_pos[t->chain[c + 1]] : 0;
                break;
            }
        }
        tg_w32(t, out.d, at + 2 + 12 * t->ifds[i].n, (uint32_t)next);
    }
    for (i = 0; i < t->nblobs; i++) {
        if (t->blobs[i].n > 0) {
            memcpy(out.d + blob_pos[i], t->blobs[i].d, t->blobs[i].n);
        }
    }
    for (i = 0; i < t->nifds; i++) {
        free(val_pos[i]);
    }
    free(val_pos);
    free(ifd_pos);
    free(embedded);
    free(blob_pos);
    free(order);
    tg_free(&tmp);
    return out;
}

static inline void tg_set_chain(tg_tiff *t, const size_t *c, size_t n)
{
    free(t->chain);
    t->chain = tg_xalloc(n * sizeof *c);
    memcpy(t->chain, c, n * sizeof *c);
    t->nchain = n;
}

/* ((v * 10000).round() as i32, 10000) as raw u32 bits. */
static inline void tg_srat(double v, uint64_t out[2])
{
    out[0] = (uint32_t)pc_f64_to_i32(round(v * 10000.0));
    out[1] = 10000;
}

static inline void tg_urat(double v, uint64_t out[2])
{
    out[0] = pc_f64_to_u32(round(v * 10000.0));
    out[1] = 10000;
}

/* ---- DNG ---- */

typedef enum tg_storage { TG_STRIPS, TG_LJ92_TILES, TG_LJ92_STRIPS } tg_storage;

typedef struct tg_dng {
    size_t width;
    size_t height;
    size_t samples;
    uint16_t *data; /* borrowed */
    uint16_t bits;
    uint8_t cfa[4];
    tg_storage storage;
    size_t rows;           /* strips */
    size_t tile_w, tile_h; /* tiles */
    bool big_endian;
    uint32_t black[16];
    size_t nblack;
    uint16_t black_repeat[2];
    uint32_t white;
    bool has_active;
    uint32_t active[4];
    bool has_crop;
    uint32_t crop_origin[2];
    uint32_t crop_size[2];
    bool has_origin_double;
    double origin_double[2];
    bool has_size_only;
    uint32_t size_only[2];
    const uint16_t *linearization; /* borrowed */
    size_t nlinearization;
    bool has_cm1;
    uint16_t cm1_ill;
    double cm1[9];
    bool has_cm2;
    uint16_t cm2_ill;
    double cm2[9];
    bool has_fm1;
    double fm1[9];
    bool has_fm2;
    double fm2[9];
    bool has_neutral;
    double neutral[3];
    bool has_baseline;
    double baseline;
    const uint8_t *opcode_list2; /* borrowed */
    size_t nopcode_list2;
    uint16_t orientation;
    const char *make;
    const char *model;
} tg_dng;

/* A CFA DNG with default metadata around data (RGGB, 16-bit, one strip). */
static inline tg_dng tg_dng_cfa(size_t width, size_t height, uint16_t *data)
{
    tg_dng d;
    memset(&d, 0, sizeof d);
    d.width = width;
    d.height = height;
    d.samples = 1;
    d.data = data;
    d.bits = 16;
    d.cfa[0] = 0;
    d.cfa[1] = 1;
    d.cfa[2] = 1;
    d.cfa[3] = 2;
    d.storage = TG_STRIPS;
    d.rows = height;
    d.black[0] = 0;
    d.nblack = 1;
    d.black_repeat[0] = 1;
    d.black_repeat[1] = 1;
    d.white = 65535;
    d.orientation = 1;
    d.make = "Photocraft";
    d.model = "Synthetic";
    return d;
}

static inline tg_bytes tg_dng_pack(const tg_dng *s, size_t y0, size_t y1)
{
    size_t n = s->width * s->samples;
    const uint16_t *vals = s->data + y0 * n;
    size_t count = (y1 - y0) * n;
    tg_bytes out = {NULL, 0, 0};
    size_t i;
    if (s->bits == 8) {
        for (i = 0; i < count; i++) {
            tg_byte(&out, (uint8_t)vals[i]);
        }
    } else if (s->bits == 16) {
        for (i = 0; i < count; i++) {
            if (s->big_endian) {
                tg_be16(&out, vals[i]);
            } else {
                tg_le16(&out, vals[i]);
            }
        }
    } else {
        size_t r;
        for (r = 0; r < y1 - y0; r++) {
            uint64_t acc = 0;
            uint32_t have = 0;
            for (i = 0; i < n; i++) {
                acc = (acc << s->bits) | vals[r * n + i];
                have += s->bits;
                while (have >= 8) {
                    have -= 8;
                    tg_byte(&out, (uint8_t)(acc >> have));
                }
            }
            if (have > 0) {
                tg_byte(&out, (uint8_t)(acc << (8 - have)));
            }
        }
    }
    return out;
}

#define TG_E(t, v)                                                                                 \
    do {                                                                                           \
        e[ne].tag = (t);                                                                           \
        e[ne].val = (v);                                                                           \
        ne++;                                                                                      \
    } while (0)

static inline tg_bytes tg_dng_build(const tg_dng *s)
{
    tg_tiff t;
    tg_entry e[48];
    size_t ne = 0;
    uint16_t bitsv[4];
    uint32_t whites[4];
    size_t i;
    size_t raw_ifd;
    size_t thumb;
    size_t i0;
    char ucm[256];
    uint64_t *offs;
    uint32_t *lens;
    size_t nseg = 0;
    tg_bytes out;
    memset(&t, 0, sizeof t);
    t.big_endian = s->big_endian;
    for (i = 0; i < s->samples; i++) {
        bitsv[i] = s->bits;
        whites[i] = s->white;
    }
    TG_E(254, tg_v_long1(0));
    TG_E(256, tg_v_long1((uint32_t)s->width));
    TG_E(257, tg_v_long1((uint32_t)s->height));
    TG_E(258, tg_v_short(bitsv, s->samples));
    TG_E(262, tg_v_short1(s->samples == 1 ? 32803 : 34892));
    TG_E(277, tg_v_short1((uint16_t)s->samples));
    TG_E(284, tg_v_short1(1));
    TG_E(50717, tg_v_long(whites, s->samples));
    TG_E(50713, tg_v_short(s->black_repeat, 2));
    TG_E(50714, tg_v_long(s->black, s->nblack));
    if (s->samples == 1) {
        uint16_t dim[2] = {2, 2};
        uint8_t planes[3] = {0, 1, 2};
        TG_E(33421, tg_v_short(dim, 2));
        TG_E(33422, tg_v_bytes(TG_BYTE, s->cfa, 4));
        TG_E(50710, tg_v_bytes(TG_BYTE, planes, 3));
        TG_E(50711, tg_v_short1(1));
    }
    offs = tg_xalloc((s->height + 1) * (s->width + 1) * sizeof *offs);
    lens = tg_xalloc((s->height + 1) * (s->width + 1) * sizeof *lens);
    if (s->storage == TG_STRIPS || s->storage == TG_LJ92_STRIPS) {
        size_t y;
        for (y = 0; y < s->height; y += s->rows) {
            size_t y1 = y + s->rows < s->height ? y + s->rows : s->height;
            tg_bytes b;
            if (s->storage == TG_STRIPS) {
                b = tg_dng_pack(s, y, y1);
            } else {
                size_t n = s->width * s->samples;
                b = tg_lj92_encode(s->data + y * n, s->width, y1 - y, s->samples, (uint8_t)s->bits, 1, 0);
            }
            lens[nseg] = (uint32_t)b.n;
            offs[nseg++] = tg_blob(&t, b);
        }
        TG_E(259, tg_v_short1(s->storage == TG_STRIPS ? 1 : 7));
        TG_E(278, tg_v_long1((uint32_t)s->rows));
        TG_E(273, tg_v_nums(TG_BLOBS, offs, nseg));
        TG_E(279, tg_v_long(lens, nseg));
    } else {
        size_t tw = s->tile_w;
        size_t th = s->tile_h;
        size_t sp = s->samples;
        size_t ty;
        size_t tx;
        for (ty = 0; ty < s->height; ty += th) {
            for (tx = 0; tx < s->width; tx += tw) {
                uint16_t *tile = tg_xalloc(tw * th * sp * sizeof *tile);
                size_t y;
                size_t x;
                size_t c;
                tg_bytes j;
                /* Edge tiles are padded by repeating the last row / column. */
                for (y = 0; y < th; y++) {
                    for (x = 0; x < tw; x++) {
                        size_t sy = ty + y < s->height - 1 ? ty + y : s->height - 1;
                        size_t sx = tx + x < s->width - 1 ? tx + x : s->width - 1;
                        for (c = 0; c < sp; c++) {
                            tile[(y * tw + x) * sp + c] = s->data[(sy * s->width + sx) * sp + c];
                        }
                    }
                }
                /* CFA tiles as Adobe writes them: half width, 2 components. */
                if (sp == 1 && tw % 2 == 0) {
                    j = tg_lj92_encode(tile, tw / 2, th, 2, (uint8_t)s->bits, 1, 0);
                } else {
                    j = tg_lj92_encode(tile, tw, th, sp, (uint8_t)s->bits, 1, 0);
                }
                free(tile);
                lens[nseg] = (uint32_t)j.n;
                offs[nseg++] = tg_blob(&t, j);
            }
        }
        TG_E(259, tg_v_short1(7));
        TG_E(322, tg_v_long1((uint32_t)tw));
        TG_E(323, tg_v_long1((uint32_t)th));
        TG_E(324, tg_v_nums(TG_BLOBS, offs, nseg));
        TG_E(325, tg_v_long(lens, nseg));
    }
    free(offs);
    free(lens);
    if (s->has_active) {
        TG_E(50829, tg_v_long(s->active, 4));
    }
    if (s->has_crop) {
        if (s->has_origin_double) {
            uint64_t b[2];
            memcpy(&b[0], &s->origin_double[0], 8);
            memcpy(&b[1], &s->origin_double[1], 8);
            TG_E(50719, tg_v_nums(TG_DOUBLE, b, 2));
        } else {
            TG_E(50719, tg_v_long(s->crop_origin, 2));
        }
        TG_E(50720, tg_v_long(s->crop_size, 2));
    } else if (s->has_origin_double) {
        uint64_t b[2];
        memcpy(&b[0], &s->origin_double[0], 8);
        memcpy(&b[1], &s->origin_double[1], 8);
        TG_E(50719, tg_v_nums(TG_DOUBLE, b, 2));
    } else if (s->has_size_only) {
        TG_E(50720, tg_v_long(s->size_only, 2));
    }
    if (s->opcode_list2 != NULL) {
        TG_E(51009, tg_v_bytes(TG_UNDEFINED, s->opcode_list2, s->nopcode_list2));
    }
    if (s->linearization != NULL) {
        TG_E(50712, tg_v_short(s->linearization, s->nlinearization));
    }
    raw_ifd = tg_ifd_add(&t, e, ne);
    ne = 0;
    {
        tg_bytes tb = {NULL, 0, 0};
        tg_resize(&tb, 12);
        memset(tb.d, 128, 12);
        thumb = tg_blob(&t, tb);
    }
    {
        uint16_t b8[3] = {8, 8, 8};
        uint8_t ver[4] = {1, 4, 0, 0};
        TG_E(254, tg_v_long1(1));
        TG_E(256, tg_v_long1(2));
        TG_E(257, tg_v_long1(2));
        TG_E(258, tg_v_short(b8, 3));
        TG_E(259, tg_v_short1(1));
        TG_E(262, tg_v_short1(2));
        TG_E(271, tg_v_ascii(s->make));
        TG_E(272, tg_v_ascii(s->model));
        TG_E(273, tg_v_ref(TG_BLOBS, thumb));
        TG_E(274, tg_v_short1(s->orientation));
        TG_E(277, tg_v_short1(3));
        TG_E(278, tg_v_long1(2));
        TG_E(279, tg_v_long1(12));
        TG_E(330, tg_v_ref(TG_IFDS, raw_ifd));
        TG_E(50706, tg_v_bytes(TG_BYTE, ver, 4));
        (void)snprintf(ucm, sizeof ucm, "%s %s", s->make, s->model);
        TG_E(50708, tg_v_ascii(ucm));
    }
#define TG_MAT(tag, m)                                                                             \
    do {                                                                                           \
        uint64_t r_[18];                                                                           \
        int q_;                                                                                    \
        for (q_ = 0; q_ < 9; q_++) {                                                               \
            tg_srat((m)[q_], r_ + 2 * q_);                                                         \
        }                                                                                          \
        TG_E(tag, tg_v_nums(TG_SRATIONAL, r_, 9));                                                 \
    } while (0)
    if (s->has_cm1) {
        TG_MAT(50721, s->cm1);
        TG_E(50778, tg_v_short1(s->cm1_ill));
    }
    if (s->has_cm2) {
        TG_MAT(50722, s->cm2);
        TG_E(50779, tg_v_short1(s->cm2_ill));
    }
    if (s->has_fm1) {
        TG_MAT(50964, s->fm1);
    }
    if (s->has_fm2) {
        TG_MAT(50965, s->fm2);
    }
#undef TG_MAT
    if (s->has_neutral) {
        uint64_t r[6];
        for (i = 0; i < 3; i++) {
            tg_urat(s->neutral[i], r + 2 * i);
        }
        TG_E(50728, tg_v_nums(TG_RATIONAL, r, 3));
    }
    if (s->has_baseline) {
        uint64_t r[2];
        tg_srat(s->baseline, r);
        TG_E(50730, tg_v_nums(TG_SRATIONAL, r, 1));
    }
    i0 = tg_ifd_add(&t, e, ne);
    tg_set_chain(&t, &i0, 1);
    out = tg_tiff_build(&t);
    tg_tiff_free(&t);
    return out;
}

/* An OpcodeList2 with one GainMap over active-area rectangle area (top, left, bottom, right), every
 * pitch rows and columns, from a 2x2 grid of gains spanning the image. */
static inline tg_bytes tg_gain_map_opcode_list(const uint32_t area[4], uint32_t pitch, const float gains[2][2])
{
    tg_bytes p = {NULL, 0, 0};
    tg_bytes b = {NULL, 0, 0};
    uint32_t head[10];
    double dv[4] = {1.0, 1.0, 0.0, 0.0};
    int i;
    head[0] = area[0];
    head[1] = area[1];
    head[2] = area[2];
    head[3] = area[3];
    head[4] = 0;
    head[5] = 1;
    head[6] = pitch;
    head[7] = pitch;
    head[8] = 2;
    head[9] = 2;
    for (i = 0; i < 10; i++) {
        tg_be32(&p, head[i]);
    }
    for (i = 0; i < 4; i++) {
        uint64_t u;
        memcpy(&u, &dv[i], 8);
        tg_be32(&p, (uint32_t)(u >> 32));
        tg_be32(&p, (uint32_t)u);
    }
    tg_be32(&p, 1);
    for (i = 0; i < 4; i++) {
        uint32_t u;
        memcpy(&u, &gains[i / 2][i % 2], 4);
        tg_be32(&p, u);
    }
    tg_be32(&b, 1);
    tg_be32(&b, 9);
    tg_be32(&b, 0x01030000u);
    tg_be32(&b, 1);
    tg_be32(&b, (uint32_t)p.n);
    tg_put(&b, p.d, p.n);
    tg_free(&p);
    return b;
}

/* ---- CR2 ---- */

typedef struct tg_cr2 {
    size_t width;
    size_t height;
    uint16_t *data; /* borrowed */
    uint8_t precision;
    size_t components;
    size_t slices[8]; /* all but the last equal; nslices 0 = no slice tag */
    size_t nslices;
    bool has_borders;
    uint16_t borders[4]; /* left, top, right, bottom (inclusive) */
    bool has_wb;
    uint16_t wb_rggb[4];
    uint16_t orientation;
    bool has_model_id;
    uint32_t model_id;
} tg_cr2;

static inline tg_bytes tg_cr2_build(const tg_cr2 *s)
{
    size_t w = s->width;
    size_t h = s->height;
    size_t widths[8];
    size_t nw;
    uint16_t *seq = tg_xalloc(w * h * sizeof *seq);
    size_t k = 0;
    size_t x0 = 0;
    size_t i;
    size_t y;
    tg_bytes jpeg;
    tg_tiff t;
    tg_entry e[16];
    size_t ne = 0;
    size_t jb;
    size_t raw_ifd;
    size_t exif_ifd;
    size_t thumb;
    size_t ifd0;
    size_t ifd1;
    size_t ifd2;
    size_t chain[4];
    tg_entry mn[4];
    size_t nmn = 0;
    tg_bytes out;
    if (s->nslices == 0) {
        widths[0] = w;
        nw = 1;
    } else {
        memcpy(widths, s->slices, s->nslices * sizeof *widths);
        nw = s->nslices;
    }
    for (i = 0; i < nw; i++) {
        for (y = 0; y < h; y++) {
            memcpy(seq + k, s->data + y * w + x0, widths[i] * sizeof *seq);
            k += widths[i];
        }
        x0 += widths[i];
    }
    jpeg = tg_lj92_encode(seq, w / s->components, h, s->components, s->precision, 1, 0);
    free(seq);
    memset(&t, 0, sizeof t);
    {
        size_t jlen = jpeg.n;
        jb = tg_blob(&t, jpeg);
        TG_E(259, tg_v_short1(6));
        TG_E(273, tg_v_ref(TG_BLOBS, jb));
        TG_E(279, tg_v_long1((uint32_t)jlen));
    }
    if (s->nslices > 0) {
        uint16_t sl[3] = {(uint16_t)(nw - 1), (uint16_t)widths[0], (uint16_t)widths[nw - 1]};
        TG_E(50752, tg_v_short(sl, 3));
    }
    raw_ifd = tg_ifd_add(&t, e, ne);
    ne = 0;
    if (s->has_model_id) {
        mn[nmn].tag = 0x0010;
        mn[nmn++].val = tg_v_long1(s->model_id);
    }
    if (s->has_borders) {
        uint16_t si[17] = {34, (uint16_t)w, (uint16_t)h, 0, 0, s->borders[0], s->borders[1], s->borders[2],
                           s->borders[3], 0, 0, 0, 0, 0, 0, 0, 0};
        mn[nmn].tag = 0x00E0;
        mn[nmn++].val = tg_v_short(si, 17);
    }
    if (s->has_wb) {
        /* ColorData as 16-bit LE words; WB_RGGBLevelsAsShot at word offset 0x47. */
        uint8_t *cd = tg_xalloc(2 * 1273);
        for (i = 0; i < 4; i++) {
            cd[2 * (0x47 + i)] = (uint8_t)s->wb_rggb[i];
            cd[2 * (0x47 + i) + 1] = (uint8_t)(s->wb_rggb[i] >> 8);
        }
        mn[nmn].tag = 0x4001;
        mn[nmn++].val = tg_v_bytes(TG_UNDEFINED, cd, 2 * 1273);
        free(cd);
    }
    {
        uint64_t r[2] = {1, 100};
        TG_E(33434, tg_v_nums(TG_RATIONAL, r, 1));
    }
    if (nmn > 0) {
        size_t mi = tg_ifd_add(&t, mn, nmn);
        TG_E(37500, tg_v_ref(TG_IFDBYTES, mi));
    }
    exif_ifd = tg_ifd_add(&t, e, ne);
    ne = 0;
    {
        tg_bytes tb = {NULL, 0, 0};
        tg_resize(&tb, 12);
        memset(tb.d, 128, 12);
        thumb = tg_blob(&t, tb);
    }
    TG_E(256, tg_v_long1(2));
    TG_E(257, tg_v_long1(2));
    TG_E(259, tg_v_short1(1));
    TG_E(262, tg_v_short1(2));
    TG_E(271, tg_v_ascii("Canon"));
    TG_E(272, tg_v_ascii("Canon EOS Synthetic"));
    TG_E(273, tg_v_ref(TG_BLOBS, thumb));
    TG_E(274, tg_v_short1(s->orientation));
    TG_E(277, tg_v_short1(3));
    TG_E(279, tg_v_long1(12));
    TG_E(34665, tg_v_ref(TG_IFDS, exif_ifd));
    ifd0 = tg_ifd_add(&t, e, ne);
    ne = 0;
    TG_E(256, tg_v_long1(1));
    ifd1 = tg_ifd_add(&t, e, ne);
    ne = 0;
    TG_E(256, tg_v_long1(1));
    ifd2 = tg_ifd_add(&t, e, ne);
    chain[0] = ifd0;
    chain[1] = ifd1;
    chain[2] = ifd2;
    chain[3] = raw_ifd;
    tg_set_chain(&t, chain, 4);
    t.has_cr2 = true;
    t.cr2_raw_ifd = raw_ifd;
    out = tg_tiff_build(&t);
    tg_tiff_free(&t);
    return out;
}

/* ---- TIFF/EP ---- */

/* An uncompressed TIFF/EP raw: IFD0 with Make, the CFA image in a SubIFD with 16-bit samples. */
static inline tg_bytes tg_tiff_ep(const char *make, size_t width, size_t height, const uint16_t *data,
                                  const uint8_t cfa[4], uint16_t bits, const tg_entry *extra, size_t nextra)
{
    tg_tiff t;
    tg_entry e[32];
    size_t ne = 0;
    tg_bytes strip = {NULL, 0, 0};
    size_t sb;
    size_t raw_ifd;
    size_t thumb;
    size_t ifd0;
    size_t i;
    uint16_t dim[2] = {2, 2};
    tg_bytes out;
    memset(&t, 0, sizeof t);
    for (i = 0; i < width * height; i++) {
        tg_le16(&strip, data[i]);
    }
    sb = tg_blob(&t, strip);
    TG_E(254, tg_v_long1(0));
    TG_E(256, tg_v_long1((uint32_t)width));
    TG_E(257, tg_v_long1((uint32_t)height));
    TG_E(258, tg_v_short1(bits));
    TG_E(259, tg_v_short1(1));
    TG_E(262, tg_v_short1(32803));
    TG_E(273, tg_v_ref(TG_BLOBS, sb));
    TG_E(277, tg_v_short1(1));
    TG_E(278, tg_v_long1((uint32_t)height));
    TG_E(279, tg_v_long1((uint32_t)(width * height * 2)));
    TG_E(33421, tg_v_short(dim, 2));
    TG_E(33422, tg_v_bytes(TG_BYTE, cfa, 4));
    for (i = 0; i < nextra; i++) {
        e[ne++] = extra[i];
    }
    raw_ifd = tg_ifd_add(&t, e, ne);
    ne = 0;
    {
        tg_bytes tb = {NULL, 0, 0};
        tg_resize(&tb, 12);
        memset(tb.d, 128, 12);
        thumb = tg_blob(&t, tb);
    }
    TG_E(254, tg_v_long1(1));
    TG_E(256, tg_v_long1(2));
    TG_E(257, tg_v_long1(2));
    TG_E(259, tg_v_short1(1));
    TG_E(262, tg_v_short1(2));
    TG_E(271, tg_v_ascii(make));
    TG_E(273, tg_v_ref(TG_BLOBS, thumb));
    TG_E(277, tg_v_short1(3));
    TG_E(279, tg_v_long1(12));
    TG_E(330, tg_v_ref(TG_IFDS, raw_ifd));
    ifd0 = tg_ifd_add(&t, e, ne);
    tg_set_chain(&t, &ifd0, 1);
    out = tg_tiff_build(&t);
    tg_tiff_free(&t);
    return out;
}

/* A Nikon NEF with a Huffman-compressed (34713) strip: IFD0 with Make and an EXIF IFD whose maker
 * note ("Nikon\0", version 2.10, then an embedded TIFF in the given byte order) holds note_tags. */
static inline tg_bytes tg_nef_compressed(size_t width, size_t height, uint16_t bits, const uint8_t *strip,
                                         size_t nstrip, const uint8_t cfa[4], bool note_big_endian,
                                         const tg_entry *note_tags, size_t nnote)
{
    tg_tiff note;
    tg_tiff t;
    tg_entry e[16];
    size_t ne = 0;
    size_t n;
    tg_bytes nb;
    tg_bytes maker = {NULL, 0, 0};
    tg_bytes sbytes = {NULL, 0, 0};
    size_t sb;
    size_t raw_ifd;
    size_t exif;
    size_t ifd0;
    uint16_t dim[2] = {2, 2};
    tg_bytes out;
    memset(&note, 0, sizeof note);
    note.big_endian = note_big_endian;
    n = tg_ifd_add(&note, (tg_entry *)(uintptr_t)note_tags, nnote);
    tg_set_chain(&note, &n, 1);
    nb = tg_tiff_build(&note);
    /* The note's values are borrowed from the caller: don't free them with the builder. */
    free(note.ifds[0].e);
    free(note.ifds);
    free(note.chain);
    tg_put(&maker, "Nikon\0\x02\x10\0\0", 10);
    tg_put(&maker, nb.d, nb.n);
    tg_free(&nb);
    memset(&t, 0, sizeof t);
    tg_put(&sbytes, strip, nstrip);
    sb = tg_blob(&t, sbytes);
    TG_E(254, tg_v_long1(0));
    TG_E(256, tg_v_long1((uint32_t)width));
    TG_E(257, tg_v_long1((uint32_t)height));
    TG_E(258, tg_v_short1(bits));
    TG_E(259, tg_v_short1(34713));
    TG_E(262, tg_v_short1(32803));
    TG_E(273, tg_v_ref(TG_BLOBS, sb));
    TG_E(277, tg_v_short1(1));
    TG_E(278, tg_v_long1((uint32_t)height));
    TG_E(279, tg_v_long1((uint32_t)nstrip));
    TG_E(33421, tg_v_short(dim, 2));
    TG_E(33422, tg_v_bytes(TG_BYTE, cfa, 4));
    raw_ifd = tg_ifd_add(&t, e, ne);
    ne = 0;
    TG_E(37500, tg_v_bytes(TG_UNDEFINED, maker.d, maker.n));
    tg_free(&maker);
    exif = tg_ifd_add(&t, e, ne);
    ne = 0;
    TG_E(271, tg_v_ascii("NIKON CORPORATION"));
    TG_E(330, tg_v_ref(TG_IFDS, raw_ifd));
    TG_E(34665, tg_v_ref(TG_IFDS, exif));
    ifd0 = tg_ifd_add(&t, e, ne);
    tg_set_chain(&t, &ifd0, 1);
    out = tg_tiff_build(&t);
    tg_tiff_free(&t);
    return out;
}

/* ---- Sony cRAW ---- */

static inline void tg_set_bits(uint8_t b[16], uint32_t at, uint32_t nbits, uint32_t v)
{
    uint32_t i;
    for (i = 0; i < nbits && at + i < 128; i++) {
        if ((v >> i) & 1) {
            b[(at + i) / 8] = (uint8_t)(b[(at + i) / 8] | (1u << ((at + i) % 8)));
        }
    }
}

/* One cRAW block of 16 same-colour 11-bit codes. */
static inline void tg_craw_block(const uint16_t codes[16], uint8_t out[16])
{
    uint16_t max = codes[0];
    uint16_t min = codes[0];
    size_t imax = 0;
    size_t imin = 0;
    uint32_t sh = 0;
    uint32_t at = 30;
    size_t i;
    for (i = 1; i < 16; i++) {
        max = codes[i] > max ? codes[i] : max;
        min = codes[i] < min ? codes[i] : min;
    }
    while (codes[imax] != max) {
        imax++;
    }
    while (codes[imin] != min) {
        imin++;
    }
    if (imin == imax) {
        imin = (imax + 1) % 16;
    }
    while ((uint16_t)(128u << sh) <= (uint16_t)(max - min)) {
        sh++;
    }
    memset(out, 0, 16);
    tg_set_bits(out, 0, 11, max);
    tg_set_bits(out, 11, 11, min);
    tg_set_bits(out, 22, 4, (uint32_t)imax);
    tg_set_bits(out, 26, 4, (uint32_t)imin);
    for (i = 0; i < 16; i++) {
        if (i != imax && i != imin) {
            uint32_t d = (uint32_t)(uint16_t)(codes[i] - min) >> sh;
            tg_set_bits(out, at, 7, d < 127 ? d : 127);
            at += 7;
        }
    }
}

static inline tg_bytes tg_craw_encode(const uint16_t *codes, size_t width, size_t height)
{
    tg_bytes out = {NULL, 0, 0};
    size_t y;
    for (y = 0; y < height; y++) {
        size_t g;
        for (g = 0; g < width / 32; g++) {
            int parity;
            for (parity = 0; parity < 2; parity++) {
                uint16_t seq[16];
                uint8_t b[16];
                size_t i;
                for (i = 0; i < 16; i++) {
                    uint16_t c = codes[y * width + g * 32 + 2 * i + (size_t)parity];
                    seq[i] = c < 2047 ? c : 2047;
                }
                tg_craw_block(seq, b);
                tg_put(&out, b, 16);
            }
        }
    }
    return out;
}

static inline tg_bytes tg_sony_craw(size_t width, size_t height, const uint16_t *codes, const uint16_t curve[4])
{
    tg_bytes bytes = tg_craw_encode(codes, width, height);
    size_t n = bytes.n;
    tg_tiff t;
    tg_entry e[24];
    size_t ne = 0;
    size_t sb;
    size_t raw_ifd;
    size_t ifd0;
    uint16_t dim[2] = {2, 2};
    uint8_t cfa[4] = {0, 1, 1, 2};
    uint16_t black[4] = {512, 512, 512, 512};
    uint16_t wb[4] = {2048, 1024, 1024, 1536};
    tg_bytes out;
    memset(&t, 0, sizeof t);
    sb = tg_blob(&t, bytes);
    TG_E(254, tg_v_long1(0));
    TG_E(256, tg_v_long1((uint32_t)width));
    TG_E(257, tg_v_long1((uint32_t)height));
    TG_E(258, tg_v_short1(12));
    TG_E(259, tg_v_short1(32767));
    TG_E(262, tg_v_short1(32803));
    TG_E(273, tg_v_ref(TG_BLOBS, sb));
    TG_E(277, tg_v_short1(1));
    TG_E(278, tg_v_long1((uint32_t)height));
    TG_E(279, tg_v_long1((uint32_t)n));
    TG_E(33421, tg_v_short(dim, 2));
    TG_E(33422, tg_v_bytes(TG_BYTE, cfa, 4));
    TG_E(0x7000, tg_v_short1(2));
    TG_E(0x7010, tg_v_short(curve, 4));
    TG_E(0x7310, tg_v_short(black, 4));
    TG_E(0x7313, tg_v_short(wb, 4));
    TG_E(50717, tg_v_short1(16383));
    raw_ifd = tg_ifd_add(&t, e, ne);
    ne = 0;
    TG_E(271, tg_v_ascii("SONY"));
    TG_E(272, tg_v_ascii("ILCE-Synthetic"));
    TG_E(330, tg_v_ref(TG_IFDS, raw_ifd));
    ifd0 = tg_ifd_add(&t, e, ne);
    tg_set_chain(&t, &ifd0, 1);
    out = tg_tiff_build(&t);
    tg_tiff_free(&t);
    return out;
}

/* ---- Panasonic RW2 ---- */

#define TG_RW2_PAGE 0x4000u
#define TG_RW2_SPLIT 0x1FF8u

static inline tg_bytes tg_rw2_pack(const uint16_t *data, size_t n, size_t width, uint32_t bits)
{
    size_t ppb = bits == 14 ? 9 : 10;
    tg_bytes logical = {NULL, 0, 0};
    tg_bytes out = {NULL, 0, 0};
    size_t i;
    (void)width;
    for (i = 0; i + ppb <= n; i += ppb) {
        uint8_t b[16] = {0};
        size_t k;
        for (k = 0; k < ppb; k++) {
            tg_set_bits(b, bits * (uint32_t)k, bits, data[i + k] & ((1u << bits) - 1));
        }
        tg_put(&logical, b, 16);
    }
    tg_resize(&logical, (logical.n + TG_RW2_PAGE - 1) / TG_RW2_PAGE * TG_RW2_PAGE);
    tg_resize(&out, logical.n);
    for (i = 0; i < logical.n; i++) {
        size_t page = i - i % TG_RW2_PAGE;
        out.d[page + (i % TG_RW2_PAGE + TG_RW2_SPLIT) % TG_RW2_PAGE] = logical.d[i];
    }
    tg_free(&logical);
    return out;
}

/* A Panasonic RW2 (RawFormat 5): RGGB, a 2-pixel border, black 128/129/130, WB 512/256/384. */
static inline tg_bytes tg_rw2(size_t width, size_t height, const uint16_t *data, uint32_t bits)
{
    tg_tiff t;
    tg_entry e[24];
    size_t ne = 0;
    size_t raw;
    size_t ifd0;
    uint16_t white = (uint16_t)((1u << bits) - 1);
    tg_bytes out;
    memset(&t, 0, sizeof t);
    raw = tg_blob(&t, tg_rw2_pack(data, width * height, width, bits));
    TG_E(0x0002, tg_v_short1((uint16_t)width));
    TG_E(0x0003, tg_v_short1((uint16_t)height));
    TG_E(0x0004, tg_v_short1(2));
    TG_E(0x0005, tg_v_short1(2));
    TG_E(0x0006, tg_v_short1((uint16_t)(height - 2)));
    TG_E(0x0007, tg_v_short1((uint16_t)(width - 2)));
    TG_E(0x0009, tg_v_short1(1));
    TG_E(0x000A, tg_v_short1((uint16_t)bits));
    TG_E(0x000E, tg_v_short1(white));
    TG_E(0x000F, tg_v_short1(white));
    TG_E(0x0010, tg_v_short1(white));
    TG_E(0x001C, tg_v_short1(128));
    TG_E(0x001D, tg_v_short1(129));
    TG_E(0x001E, tg_v_short1(130));
    TG_E(0x0024, tg_v_short1(512));
    TG_E(0x0025, tg_v_short1(256));
    TG_E(0x0026, tg_v_short1(384));
    TG_E(0x002D, tg_v_short1(5));
    TG_E(0x010F, tg_v_ascii("Panasonic"));
    TG_E(0x0110, tg_v_ascii("DC-Synthetic"));
    TG_E(0x0112, tg_v_short1(1));
    TG_E(0x0118, tg_v_ref(TG_BLOBS, raw));
    ifd0 = tg_ifd_add(&t, e, ne);
    tg_set_chain(&t, &ifd0, 1);
    out = tg_tiff_build(&t);
    tg_tiff_free(&t);
    out.d[2] = 'U';
    out.d[3] = 0;
    return out;
}

/* ---- Olympus ORF ---- */

typedef struct tg_rel {
    uint16_t tag;
    uint16_t typ;
    uint32_t count;
    uint8_t bytes[16];
    size_t n;
} tg_rel;

/* A little-endian IFD whose value offsets are relative to a base (the IFD sits at base_offset). */
static inline tg_bytes tg_relative_ifd(const tg_rel *entries, size_t n, size_t base_offset)
{
    size_t head = 2 + 12 * n + 4;
    tg_bytes out = {NULL, 0, 0};
    tg_bytes extra = {NULL, 0, 0};
    size_t i;
    tg_le16(&out, (uint16_t)n);
    for (i = 0; i < n; i++) {
        tg_le16(&out, entries[i].tag);
        tg_le16(&out, entries[i].typ);
        tg_le32(&out, entries[i].count);
        if (entries[i].n <= 4) {
            uint8_t v[4] = {0, 0, 0, 0};
            memcpy(v, entries[i].bytes, entries[i].n);
            tg_put(&out, v, 4);
        } else {
            tg_le32(&out, (uint32_t)(base_offset + head + extra.n));
            tg_put(&extra, entries[i].bytes, entries[i].n);
        }
    }
    tg_le32(&out, 0);
    tg_put(&out, extra.d, extra.n);
    tg_free(&extra);
    return out;
}

static inline tg_rel tg_rel_shorts(uint16_t tag, const uint16_t *v, size_t n)
{
    tg_rel r;
    size_t i;
    memset(&r, 0, sizeof r);
    r.tag = tag;
    r.typ = 3;
    r.count = (uint32_t)n;
    for (i = 0; i < n; i++) {
        r.bytes[2 * i] = (uint8_t)v[i];
        r.bytes[2 * i + 1] = (uint8_t)(v[i] >> 8);
    }
    r.n = 2 * n;
    return r;
}

static inline tg_rel tg_rel_u32(uint16_t tag, uint16_t typ, uint32_t v)
{
    tg_rel r;
    memset(&r, 0, sizeof r);
    r.tag = tag;
    r.typ = typ;
    r.count = 1;
    r.bytes[0] = (uint8_t)v;
    r.bytes[1] = (uint8_t)(v >> 8);
    r.bytes[2] = (uint8_t)(v >> 16);
    r.bytes[3] = (uint8_t)(v >> 24);
    r.n = 4;
    return r;
}

/* An uncompressed Olympus ORF (IIRO): 12-bit data left-justified in 16-bit samples, a GRBG EXIF
 * CFAPattern and a new-style maker note with ImageProcessing levels and a preview JPEG. */
static inline tg_bytes tg_orf(size_t width, size_t height, const uint16_t *data)
{
    static const uint16_t WB[2] = {512, 384};
    static const uint16_t G[1] = {256};
    static const uint16_t BL[4] = {64, 64, 64, 64};
    static const uint16_t VB[2] = {12, 0};
    static const uint16_t CROP[2] = {2, 0};
    static const uint8_t JPEG[17] = {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x0B, 8, 0x00, 0x08,
                                     0x00, 0x10, 1,    1,    0x11, 0,    0xFF, 0xD9};
    tg_rel ipe[8];
    tg_rel cse[3];
    tg_rel nte[2];
    tg_bytes ip;
    tg_bytes cs;
    tg_bytes rel;
    tg_bytes note = {NULL, 0, 0};
    size_t cs_at;
    size_t cs_len = 2 + 12 * 3 + 4;
    tg_tiff t;
    tg_entry e[16];
    size_t ne = 0;
    tg_bytes strip = {NULL, 0, 0};
    size_t sb;
    size_t exif;
    size_t ifd0;
    size_t i;
    uint8_t cfa[8] = {2, 0, 2, 0, 1, 0, 2, 1};
    tg_bytes out;
    ipe[0] = tg_rel_shorts(0x0100, WB, 2);
    ipe[1] = tg_rel_shorts(0x011F, G, 1);
    ipe[2] = tg_rel_shorts(0x0600, BL, 4);
    ipe[3] = tg_rel_shorts(0x0611, VB, 2);
    ipe[4] = tg_rel_shorts(0x0612, CROP, 2);
    ipe[5] = tg_rel_shorts(0x0613, CROP, 2);
    ipe[6] = tg_rel_u32(0x0614, 4, (uint32_t)(width - 4));
    ipe[7] = tg_rel_u32(0x0615, 4, (uint32_t)(height - 4));
    ip = tg_relative_ifd(ipe, 8, 64);
    cs_at = 64 + (ip.n + 3) / 4 * 4;
    cse[0] = tg_rel_u32(0x0100, 4, 1);
    cse[1] = tg_rel_u32(0x0101, 4, (uint32_t)(cs_at + cs_len));
    cse[2] = tg_rel_u32(0x0102, 4, 17);
    cs = tg_relative_ifd(cse, 3, cs_at);
    tg_put(&note, "OLYMPUS\0II\x03\0", 12);
    nte[0] = tg_rel_u32(0x2020, 13, (uint32_t)cs_at);
    nte[1] = tg_rel_u32(0x2040, 13, 64);
    rel = tg_relative_ifd(nte, 2, 12);
    tg_put(&note, rel.d, rel.n);
    tg_free(&rel);
    tg_resize(&note, 64);
    tg_put(&note, ip.d, ip.n);
    tg_resize(&note, cs_at);
    tg_put(&note, cs.d, cs.n);
    tg_put(&note, JPEG, 17);
    tg_free(&ip);
    tg_free(&cs);
    memset(&t, 0, sizeof t);
    for (i = 0; i < width * height; i++) {
        tg_le16(&strip, (uint16_t)(data[i] << 4));
    }
    sb = tg_blob(&t, strip);
    TG_E(37500, tg_v_bytes(TG_UNDEFINED, note.d, note.n));
    TG_E(41730, tg_v_bytes(TG_UNDEFINED, cfa, 8));
    tg_free(&note);
    exif = tg_ifd_add(&t, e, ne);
    ne = 0;
    TG_E(256, tg_v_long1((uint32_t)width));
    TG_E(257, tg_v_long1((uint32_t)height));
    TG_E(258, tg_v_short1(16));
    TG_E(259, tg_v_short1(1));
    TG_E(262, tg_v_short1(1));
    TG_E(271, tg_v_ascii("OLYMPUS IMAGING CORP."));
    TG_E(272, tg_v_ascii("E-Synthetic"));
    TG_E(273, tg_v_ref(TG_BLOBS, sb));
    TG_E(277, tg_v_short1(1));
    TG_E(278, tg_v_long1((uint32_t)height));
    TG_E(279, tg_v_long1((uint32_t)(width * height * 2)));
    TG_E(34665, tg_v_ref(TG_IFDS, exif));
    ifd0 = tg_ifd_add(&t, e, ne);
    tg_set_chain(&t, &ifd0, 1);
    out = tg_tiff_build(&t);
    tg_tiff_free(&t);
    out.d[2] = 'R';
    out.d[3] = 'O';
    return out;
}

#undef TG_E

/* ---- scenes ---- */

/* A smooth, colourful synthetic scene as linear RGB in 0..1 (malloc'd, width * height). */
static inline float (*tg_scene(size_t width, size_t height))[3]
{
    float(*v)[3] = tg_xalloc(width * height * sizeof *v);
    size_t i;
    float wd = (float)(width > 0 ? width : 1);
    float hd = (float)(height > 0 ? height : 1);
    for (i = 0; i < width * height; i++) {
        float x = (float)(i % width) / wd;
        float y = (float)(i / width) / hd;
        v[i][0] = 0.1f + 0.7f * x;
        v[i][1] = 0.15f + 0.5f * (1.0f - y) * x + 0.2f * y;
        v[i][2] = 0.6f - 0.4f * y + 0.2f * x;
    }
    return v;
}

/* Mosaics linear RGB through a 2x2 CFA into black + v * (white - black) (malloc'd). */
static inline uint16_t *tg_mosaic(const float (*rgb)[3], size_t n, size_t width, const uint8_t cfa[4], uint16_t black,
                                  uint16_t white)
{
    uint16_t *out = tg_xalloc(n * sizeof *out);
    size_t i;
    for (i = 0; i < n; i++) {
        size_t x = i % width;
        size_t y = i / width;
        float v = pc_clampf(rgb[i][cfa[(y & 1) * 2 + (x & 1)]], 0.0f, 1.0f);
        out[i] = pc_f32_to_u16(roundf((float)black + v * (float)(uint16_t)(white - black)));
    }
    return out;
}

#endif
