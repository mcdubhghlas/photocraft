/* Lossless JPEG decoder (ITU-T T.81 process 14: Huffman-coded, predictive, 2-16 bit samples), a
 * port of crates/raw/src/ljpeg.rs. Output is the sample sequence in raster order, components
 * interleaved. Only one interleaved scan of 1x1-sampled components is supported, which is how
 * DNG, CR2 and the TIFF-based raws use it. */
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "raw/internal.h"

/* Bits resolved by one table lookup. */
#define LOOKUP_BITS 13u
#define LOOKUP (1u << LOOKUP_BITS)

typedef struct huffman {
    /* (length << 8) | symbol for codes up to LOOKUP_BITS long, 0 if longer. */
    uint16_t lookup[LOOKUP];
    /* When a code and its extra bits fit in LOOKUP_BITS: the difference and the bits consumed
     * (fast_bits 0 = no entry). */
    int32_t fast_d[LOOKUP];
    uint8_t fast_bits[LOOKUP];
    int32_t maxcode[17];
    int32_t mincode[17];
    size_t valptr[17];
    uint8_t vals[256];
    size_t nvals;
} huffman;

static pc_status huffman_new(const uint8_t counts[16], const uint8_t *vals, size_t nvals, huffman **out,
                             pc_error *err)
{
    size_t total = 0;
    size_t i;
    uint32_t code = 0;
    size_t k = 0;
    uint32_t len;
    huffman *h;
    for (i = 0; i < 16; i++) {
        total += counts[i];
    }
    if (total == 0 || total > 256 || nvals < total) {
        return rx_malformed(err, "bad Huffman table");
    }
    h = calloc(1, sizeof *h);
    if (h == NULL) {
        return rx_nomem(err);
    }
    for (i = 0; i < 17; i++) {
        h->maxcode[i] = -1;
    }
    memcpy(h->vals, vals, total);
    h->nvals = total;
    for (len = 1; len <= 16; len++) {
        size_t n = counts[len - 1];
        if (n > 0) {
            size_t j;
            h->valptr[len] = k;
            h->mincode[len] = (int32_t)code;
            for (j = 0; j < n; j++) {
                if (code >= (1u << len)) {
                    free(h);
                    return rx_malformed(err, "Huffman code overflow");
                }
                if (len <= LOOKUP_BITS) {
                    uint32_t shift = LOOKUP_BITS - len;
                    size_t base = (size_t)code << shift;
                    uint16_t entry = (uint16_t)(len << 8 | h->vals[k]);
                    size_t s;
                    for (s = 0; s < ((size_t)1 << shift); s++) {
                        h->lookup[base + s] = entry;
                    }
                }
                code++;
                k++;
            }
            h->maxcode[len] = (int32_t)code - 1;
        }
        code <<= 1;
    }
    for (i = 0; i < LOOKUP; i++) {
        uint16_t e = h->lookup[i];
        uint32_t l = (uint32_t)e >> 8;
        uint32_t s = e & 0xFF;
        int32_t d;
        if (l == 0) {
            continue;
        }
        if (s == 0) {
            d = 0;
        } else if (s == 16) {
            d = 32768;
        } else if (s >= 1 && s <= 15 && l + s <= LOOKUP_BITS) {
            int32_t v = (int32_t)(((uint32_t)i >> (LOOKUP_BITS - l - s)) & ((1u << s) - 1));
            d = v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
        } else {
            continue;
        }
        h->fast_d[i] = d;
        h->fast_bits[i] = (uint8_t)(l + (s >= 1 && s <= 15 ? s : 0));
    }
    *out = h;
    return PC_OK;
}

/* MSB-first bit reader over entropy-coded data: removes FF 00 stuffing, stops at markers (then
 * feeds zero bits, counted in fake). */
typedef struct bits {
    const uint8_t *data;
    size_t len;
    size_t pos;
    uint64_t acc;
    uint32_t n;
    bool marker;
    size_t fake;
} bits;

static void refill(bits *b)
{
    if (!b->marker && b->len >= 8 && b->pos <= b->len - 8) {
        const uint8_t *s = b->data + b->pos;
        uint64_t chunk = (uint64_t)s[0] << 56 | (uint64_t)s[1] << 48 | (uint64_t)s[2] << 40 | (uint64_t)s[3] << 32 |
                         (uint64_t)s[4] << 24 | (uint64_t)s[5] << 16 | (uint64_t)s[6] << 8 | (uint64_t)s[7];
        uint64_t inv = ~chunk;
        if (((inv - 0x0101010101010101ull) & ~inv & 0x8080808080808080ull) == 0) {
            uint32_t k = (64 - b->n) / 8;
            if (k == 0) {
                return;
            }
            if (k == 8) {
                b->acc = chunk;
            } else {
                b->acc |= (chunk >> (64 - 8 * k)) << (64 - b->n - 8 * k);
            }
            b->n += 8 * k;
            b->pos += k;
            return;
        }
    }
    while (b->n <= 56) {
        uint8_t byte;
        if (b->marker) {
            b->fake++;
            byte = 0;
        } else if (b->pos < b->len) {
            if (b->data[b->pos] == 0xFF) {
                if (b->pos + 1 < b->len && b->data[b->pos + 1] == 0) {
                    b->pos += 2;
                    byte = 0xFF;
                } else {
                    b->marker = true;
                    b->fake++;
                    byte = 0;
                }
            } else {
                byte = b->data[b->pos++];
            }
        } else {
            b->marker = true;
            b->fake++;
            byte = 0;
        }
        b->acc |= (uint64_t)byte << (56 - b->n);
        b->n += 8;
    }
}

static uint32_t peek(const bits *b, uint32_t k) { return (uint32_t)(b->acc >> (64 - k)); }

static void consume(bits *b, uint32_t k)
{
    b->acc <<= k;
    b->n -= k;
}

static uint32_t get(bits *b, uint32_t k)
{
    uint32_t v;
    if (b->n < k) {
        refill(b);
    }
    v = peek(b, k);
    consume(b, k);
    return v;
}

static pc_status restart(bits *b, pc_error *err)
{
    b->acc = 0;
    b->n = 0;
    b->marker = false;
    b->fake = 0;
    while (b->pos < b->len) {
        if (b->data[b->pos] == 0xFF) {
            if (b->pos + 1 < b->len && b->data[b->pos + 1] >= 0xD0 && b->data[b->pos + 1] <= 0xD7) {
                b->pos += 2;
                return PC_OK;
            }
            if (b->pos + 1 < b->len && b->data[b->pos + 1] == 0xFF) {
                b->pos++;
            } else {
                return rx_malformed(err, "expected a restart marker");
            }
        } else {
            b->pos++;
        }
    }
    return rx_malformed(err, "missing restart marker");
}

static pc_status decode_sym(bits *b, const huffman *h, uint32_t *out, pc_error *err)
{
    uint16_t e;
    uint32_t len;
    if (b->n < 32) {
        refill(b);
    }
    e = h->lookup[peek(b, LOOKUP_BITS)];
    if (e != 0) {
        consume(b, (uint32_t)e >> 8);
        *out = e & 0xFF;
        return PC_OK;
    }
    for (len = LOOKUP_BITS + 1; len <= 16; len++) {
        int32_t code = (int32_t)peek(b, len);
        if (code <= h->maxcode[len]) {
            int64_t i = (int64_t)h->valptr[len] + (int64_t)code - (int64_t)h->mincode[len];
            if (i < 0 || (uint64_t)i >= h->nvals) {
                return rx_malformed(err, "bad Huffman code");
            }
            consume(b, len);
            *out = h->vals[i];
            return PC_OK;
        }
    }
    return rx_malformed(err, "bad Huffman code");
}

/* A difference value: category SSSS then SSSS extra bits (T.81 H.1.2.2). */
static pc_status diff(bits *b, const huffman *h, int32_t *out, pc_error *err)
{
    uint32_t idx;
    uint32_t s;
    pc_status st;
    if (b->n < 32) {
        refill(b);
    }
    idx = peek(b, LOOKUP_BITS);
    if (h->fast_bits[idx] != 0) {
        consume(b, h->fast_bits[idx]);
        *out = h->fast_d[idx];
        return PC_OK;
    }
    if ((st = decode_sym(b, h, &s, err)) != PC_OK) {
        return st;
    }
    if (s == 0) {
        *out = 0;
    } else if (s <= 15) {
        int32_t v = (int32_t)get(b, s);
        *out = v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
    } else if (s == 16) {
        *out = 32768;
    } else {
        return rx_malformed(err, "bad difference category");
    }
    return PC_OK;
}

typedef struct parsed {
    rx_frame frame;
    huffman *huff[4];
    int table[4]; /* per scan component: index into huff */
    uint8_t predictor;
    uint8_t point_transform;
    size_t restart;
    const uint8_t *entropy;
    size_t nentropy;
} parsed;

static void parsed_fini(parsed *p)
{
    size_t i;
    for (i = 0; i < 4; i++) {
        free(p->huff[i]);
        p->huff[i] = NULL;
    }
}

static bool be16(const uint8_t *d, size_t len, size_t at, size_t *v)
{
    if (at > len || len - at < 2) {
        return false;
    }
    *v = (size_t)d[at] << 8 | d[at + 1];
    return true;
}

static pc_status parse(const uint8_t *data, size_t dlen, parsed *p, pc_error *err)
{
    size_t pos = 2;
    bool have_frame = false;
    uint8_t comp_ids[4];
    size_t ncomp = 0;
    memset(p, 0, sizeof *p);
    if (dlen < 2 || data[0] != 0xFF || data[1] != 0xD8) {
        return rx_malformed(err, "lossless JPEG: missing SOI");
    }
    for (;;) {
        uint8_t marker;
        size_t len;
        const uint8_t *seg;
        size_t slen;
        while (pos + 1 < dlen && data[pos] == 0xFF && data[pos + 1] == 0xFF) {
            pos++;
        }
        if (pos >= dlen || data[pos] != 0xFF) {
            parsed_fini(p);
            return rx_malformed(err, "lossless JPEG: expected a marker");
        }
        if (pos + 1 >= dlen) {
            parsed_fini(p);
            return rx_malformed(err, "truncated lossless JPEG");
        }
        marker = data[pos + 1];
        pos += 2;
        if (marker == 0xD9) {
            parsed_fini(p);
            return rx_malformed(err, "lossless JPEG: no scan");
        }
        if ((marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
            continue;
        }
        if (!be16(data, dlen, pos, &len)) {
            parsed_fini(p);
            return rx_malformed(err, "truncated lossless JPEG");
        }
        if (len < 2) {
            parsed_fini(p);
            return rx_malformed(err, "lossless JPEG: bad segment length");
        }
        if (pos + len > dlen) {
            parsed_fini(p);
            return rx_malformed(err, "truncated lossless JPEG segment");
        }
        seg = data + pos + 2;
        slen = len - 2;
        if (marker == 0xC3) {
            uint8_t precision;
            size_t height;
            size_t width;
            size_t nf;
            size_t c;
            if (slen < 6) {
                parsed_fini(p);
                return rx_malformed(err, "short SOF3");
            }
            precision = seg[0];
            height = (size_t)seg[1] << 8 | seg[2];
            width = (size_t)seg[3] << 8 | seg[4];
            nf = seg[5];
            if (precision < 2 || precision > 16 || width == 0 || height == 0 || nf == 0 || nf > 4 ||
                slen < 6 + 3 * nf) {
                parsed_fini(p);
                return rx_malformed(err, "unsupported SOF3 (%u-bit, %zux%zu, %zu components)", (unsigned)precision,
                                    width, height, nf);
            }
            ncomp = 0;
            for (c = 0; c < nf; c++) {
                const uint8_t *s = seg + 6 + 3 * c;
                if (s[1] != 0x11) {
                    parsed_fini(p);
                    return rx_unsupported(err, "lossless JPEG with subsampled components");
                }
                comp_ids[ncomp++] = s[0];
            }
            p->frame.precision = precision;
            p->frame.width = width;
            p->frame.height = height;
            p->frame.components = nf;
            have_frame = true;
        } else if (marker >= 0xC0 && marker <= 0xC2) {
            parsed_fini(p);
            return rx_unsupported(err, "lossy (DCT) JPEG compressed raw data");
        } else if ((marker >= 0xC5 && marker <= 0xC7) || (marker >= 0xC9 && marker <= 0xCB) ||
                   (marker >= 0xCD && marker <= 0xCF)) {
            parsed_fini(p);
            return rx_unsupported(err, "hierarchical or arithmetic-coded JPEG");
        } else if (marker == 0xC4) {
            size_t q = 0;
            while (q < slen) {
                uint8_t tc_th = seg[q];
                size_t id = tc_th & 0x0F;
                size_t total = 0;
                size_t i;
                if (slen - q < 17) {
                    parsed_fini(p);
                    return rx_malformed(err, "short DHT");
                }
                for (i = 0; i < 16; i++) {
                    total += seg[q + 1 + i];
                }
                if (slen - q - 17 < total) {
                    parsed_fini(p);
                    return rx_malformed(err, "short DHT");
                }
                if (id < 4) {
                    huffman *h;
                    pc_status st = huffman_new(seg + q + 1, seg + q + 17, total, &h, err);
                    if (st != PC_OK) {
                        parsed_fini(p);
                        return st;
                    }
                    free(p->huff[id]);
                    p->huff[id] = h;
                }
                q += 17 + total;
            }
        } else if (marker == 0xDD) {
            if (!be16(seg, slen, 0, &p->restart)) {
                parsed_fini(p);
                return rx_malformed(err, "truncated lossless JPEG");
            }
        } else if (marker == 0xDA) {
            size_t ns;
            size_t c;
            if (!have_frame) {
                parsed_fini(p);
                return rx_malformed(err, "lossless JPEG: SOS before SOF3");
            }
            if (slen == 0) {
                parsed_fini(p);
                return rx_malformed(err, "short SOS");
            }
            ns = seg[0];
            if (ns != p->frame.components || slen < 1 + 2 * ns + 3) {
                parsed_fini(p);
                return rx_unsupported(err, "lossless JPEG with non-interleaved scans");
            }
            for (c = 0; c < ns; c++) {
                uint8_t id = seg[1 + 2 * c];
                size_t td = (size_t)(seg[2 + 2 * c] >> 4);
                if (c >= ncomp || comp_ids[c] != id) {
                    parsed_fini(p);
                    return rx_unsupported(err, "lossless JPEG scan component order");
                }
                if (td >= 4 || p->huff[td] == NULL) {
                    parsed_fini(p);
                    return rx_malformed(err, "missing Huffman table");
                }
                p->table[c] = (int)td;
            }
            p->predictor = seg[1 + 2 * ns];
            p->point_transform = seg[3 + 2 * ns] & 0x0F;
            if (p->predictor < 1 || p->predictor > 7) {
                unsigned pr = p->predictor;
                parsed_fini(p);
                return rx_malformed(err, "lossless JPEG predictor %u", pr);
            }
            if (p->point_transform >= p->frame.precision) {
                parsed_fini(p);
                return rx_malformed(err, "lossless JPEG point transform");
            }
            p->entropy = data + pos + len;
            p->nentropy = dlen - (pos + len);
            return PC_OK;
        }
        pos += len;
    }
}

pc_status rx_ljpeg_frame(const uint8_t *data, size_t len, rx_frame *out, pc_error *err)
{
    parsed p;
    pc_status st = parse(data, len, &p, err);
    if (st != PC_OK) {
        return st;
    }
    *out = p.frame;
    parsed_fini(&p);
    return PC_OK;
}

/* x >> 1 rounded towards minus infinity (Rust's arithmetic shift of an i32). */
static int32_t half_floor(int32_t x) { return (x - (x < 0 ? 1 : 0)) / 2; }

pc_status rx_ljpeg_decode(const uint8_t *data, size_t len, size_t max_samples, rx_frame *frame, uint16_t **out,
                          size_t *nout, pc_error *err)
{
    parsed p;
    rx_frame f;
    size_t total;
    size_t row_len;
    size_t rows_per_interval;
    uint16_t *o;
    bits b;
    size_t nc;
    int32_t initial;
    uint8_t pred;
    size_t interval_row = 0;
    size_t y;
    pc_status st = parse(data, len, &p, err);
    if (st != PC_OK) {
        return st;
    }
    f = p.frame;
    if (f.height > SIZE_MAX / f.width || f.width * f.height > SIZE_MAX / f.components ||
        (total = f.width * f.height * f.components) > max_samples) {
        parsed_fini(&p);
        return rx_limit(err, "lossless JPEG frame %zux%zux%zu", f.width, f.height, f.components);
    }
    /* An entropy-coded sample needs at least one bit (cheap decompression-bomb guard). */
    if ((p.nentropy > SIZE_MAX / 8 ? SIZE_MAX : p.nentropy * 8) < total / 2) {
        parsed_fini(&p);
        return rx_malformed(err, "lossless JPEG data is truncated");
    }
    row_len = f.width * f.components;
    if (p.restart != 0 && p.restart % f.width != 0) {
        parsed_fini(&p);
        return rx_unsupported(err, "lossless JPEG restart interval that is not whole rows");
    }
    rows_per_interval = p.restart == 0 ? SIZE_MAX : p.restart / f.width;
    o = pc_calloc_array(total > 0 ? total : 1, sizeof *o);
    if (o == NULL) {
        parsed_fini(&p);
        return rx_nomem(err);
    }
    memset(&b, 0, sizeof b);
    b.data = p.entropy;
    b.len = p.nentropy;
    nc = f.components;
    initial = 1 << (f.precision - p.point_transform - 1);
    pred = p.predictor;
    for (y = 0; y < f.height; y++) {
        uint16_t *cur = o + y * row_len;
        const uint16_t *prev = y > 0 ? o + (y - 1) * row_len : NULL;
        bool first_line;
        size_t c;
        if (interval_row == rows_per_interval) {
            if ((st = restart(&b, err)) != PC_OK) {
                goto fail;
            }
            interval_row = 0;
        }
        first_line = interval_row == 0;
        if (pred == 1 || first_line) {
            size_t i;
            for (c = 0; c < nc; c++) {
                int32_t px = first_line ? initial : (int32_t)prev[c];
                int32_t d;
                if ((st = diff(&b, p.huff[p.table[c]], &d, err)) != PC_OK) {
                    goto fail;
                }
                cur[c] = (uint16_t)(uint32_t)(px + d);
            }
            if (nc == 1) {
                const huffman *t = p.huff[p.table[0]];
                for (i = 1; i < row_len; i++) {
                    int32_t d;
                    if ((st = diff(&b, t, &d, err)) != PC_OK) {
                        goto fail;
                    }
                    cur[i] = (uint16_t)(uint32_t)((int32_t)cur[i - 1] + d);
                }
            } else {
                for (i = nc; i < row_len; i += nc) {
                    for (c = 0; c < nc; c++) {
                        int32_t d;
                        if ((st = diff(&b, p.huff[p.table[c]], &d, err)) != PC_OK) {
                            goto fail;
                        }
                        cur[i + c] = (uint16_t)(uint32_t)((int32_t)cur[i + c - nc] + d);
                    }
                }
            }
        } else {
            size_t x;
            for (x = 0; x < f.width; x++) {
                for (c = 0; c < nc; c++) {
                    size_t i = x * nc + c;
                    int32_t px;
                    int32_t d;
                    if (x == 0) {
                        px = (int32_t)prev[i];
                    } else {
                        int32_t ra = cur[i - nc];
                        int32_t rb = prev[i];
                        int32_t rc = prev[i - nc];
                        switch (pred) {
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
                            px = ra + half_floor(rb - rc);
                            break;
                        case 6:
                            px = rb + half_floor(ra - rc);
                            break;
                        default:
                            px = (ra + rb) >> 1;
                        }
                    }
                    if ((st = diff(&b, p.huff[p.table[c]], &d, err)) != PC_OK) {
                        goto fail;
                    }
                    cur[i] = (uint16_t)(uint32_t)(px + d);
                }
            }
        }
        interval_row++;
        if (b.fake > 64) {
            st = rx_malformed(err, "lossless JPEG data is truncated");
            goto fail;
        }
    }
    if (p.point_transform > 0) {
        size_t i;
        for (i = 0; i < total; i++) {
            o[i] = (uint16_t)(o[i] << p.point_transform);
        }
    }
    parsed_fini(&p);
    *frame = f;
    *out = o;
    *nout = total;
    return PC_OK;
fail:
    free(o);
    parsed_fini(&p);
    return st;
}
