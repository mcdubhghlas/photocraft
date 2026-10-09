/* TIFF and BigTIFF (port of crates/codecs/src/codecs/tiff.rs; REWRITE_PLAN.md D25).
 *
 * Reading: tiff_ifd walks the directories; this file decodes one directory's pixels with the
 * chunk codecs the reference supported (none, LZW, Deflate, PackBits; horizontal and
 * floating-point predictors): 1/2/4/8/16/32/64-bit integers and 16/32/64-bit floats, gray / RGB /
 * CMYK / palette with optional alpha (unassociated, or associated and un-premultiplied) and extra
 * samples, WhiteIsZero, strips or tiles, planar configuration 1 or 2. Decoding is banded: the
 * output is allocated once and each band of rows (one strip, or one row of tiles, across every
 * plane) is decoded straight into it, in parallel. A chunk cut off by the end of the file (or
 * whose data ends early) stays empty and adds a Truncated warning. The colour type follows the
 * reference's tiff crate: photometric samples = samples - extra samples, alpha when the first
 * extra sample says so, trailing extra samples dropped.
 *
 * Writing: native byte order (as the reference), one IFD after the strips; ICC (34675), XMP
 * (700), DPI, ASCII text tags and Photoshop's 34377 / 37724 payloads; BigTIFF on request or when
 * the file could pass 4 GiB. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "base/compress.h"
#include "base/half.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "base/utf8.h"
#include "codecs/internal.h"
#include "codecs/orientation.h"
#include "codecs/tiff_ifd.h"

#define F PC_CF_TIFF
#define TAG_XMP 700
#define TAG_ICC 34675
#define TAG_PHOTOSHOP 34377
#define TAG_IMAGE_SOURCE_DATA 37724
#define TAG_DNG_VERSION 50706
#define PHOTOMETRIC_CFA 32803
#define PHOTOMETRIC_LINEAR_RAW 34892

static bool host_little(void)
{
    const uint16_t x = 1;
    uint8_t b;
    memcpy(&b, &x, 1);
    return b == 1;
}

bool pc_ctiff_writes_little_endian(void) { return host_little(); }

/* ---- Photoshop's private tags ---- */

void pc_ctiff_photoshop_tags(const uint8_t *b, size_t n, const uint8_t **res, size_t *res_len,
                             const uint8_t **layers, size_t *layers_len)
{
    tiff_file f;
    uint64_t ifd;
    uint64_t cnt;
    uint64_t entry;
    uint64_t first;
    uint64_t inl;
    uint64_t i;
    const uint8_t *r = NULL;
    const uint8_t *l = NULL;
    size_t rl = 0;
    size_t ll = 0;
    *res = NULL;
    *layers = NULL;
    *res_len = 0;
    *layers_len = 0;
    if (n < 2 || !((b[0] == 'I' && b[1] == 'I') || (b[0] == 'M' && b[1] == 'M'))) {
        return;
    }
    f.b = b;
    f.n = n;
    f.le = b[0] == 'I';
    {
        uint16_t v;
        if (!tiff_u16_at(&f, 2, &v)) {
            return;
        }
        if (v == 42) {
            uint32_t o;
            if (!tiff_u32_at(&f, 4, &o)) {
                return;
            }
            f.big = false;
            ifd = o;
        } else if (v == 43) {
            if (!tiff_u64_at(&f, 8, &ifd)) {
                return;
            }
            f.big = true;
        } else {
            return;
        }
    }
    if (f.big) {
        if (!tiff_u64_at(&f, ifd, &cnt) || ifd > UINT64_MAX - 8) {
            return;
        }
        entry = 20;
        first = ifd + 8;
        inl = 8;
    } else {
        uint16_t c;
        if (!tiff_u16_at(&f, ifd, &c) || ifd > UINT64_MAX - 2) {
            return;
        }
        cnt = c;
        entry = 12;
        first = ifd + 2;
        inl = 4;
    }
    if (cnt > 4096) {
        cnt = 4096;
    }
    for (i = 0; i < cnt; i++) {
        uint64_t e = first + i * entry;
        uint16_t tag;
        uint16_t ty;
        uint64_t count;
        uint64_t start;
        uint64_t value_at;
        if (e < first || !tiff_u16_at(&f, e, &tag)) {
            return;
        }
        if (tag != TAG_PHOTOSHOP && tag != TAG_IMAGE_SOURCE_DATA) {
            continue;
        }
        /* BYTE, ASCII, SBYTE or UNDEFINED: one byte per element. */
        if (!tiff_u16_at(&f, e + 2, &ty)) {
            return;
        }
        if (!(ty == 1 || ty == 2 || ty == 6 || ty == 7)) {
            continue;
        }
        if (f.big) {
            if (!tiff_u64_at(&f, e + 4, &count)) {
                return;
            }
        } else {
            uint32_t c;
            if (!tiff_u32_at(&f, e + 4, &c)) {
                return;
            }
            count = c;
        }
        value_at = e + (f.big ? 12 : 8);
        if (count <= inl) {
            start = value_at;
        } else if (f.big) {
            if (!tiff_u64_at(&f, value_at, &start)) {
                return;
            }
        } else {
            uint32_t o;
            if (!tiff_u32_at(&f, value_at, &o)) {
                return;
            }
            start = o;
        }
        if (start > n || count > n - start) {
            return; /* anything malformed reads as absent */
        }
        if (tag == TAG_PHOTOSHOP) {
            r = b + start;
            rl = (size_t)count;
        } else {
            l = b + start;
            ll = (size_t)count;
        }
    }
    *res = r;
    *res_len = rl;
    *layers = l;
    *layers_len = ll;
}

/* ---- camera raw recognition ---- */

/* TIFF-structured camera raw files (TIFF/EP, DNG, CR2, NEF, ARW, ...): a CR2 signature, a
 * DNGVersion tag, or a CFA / LinearRaw image in IFD0, its SubIFDs or the next few IFDs. */
static bool camera_raw(const uint8_t *b, size_t n)
{
    tiff_file f;
    uint64_t queue[64];
    size_t nq = 0;
    int visited = 0;
    uint32_t o;
    if (n < 4) {
        return false;
    }
    if (memcmp(b, "II*\0", 4) == 0) {
        f.le = true;
    } else if (memcmp(b, "MM\0*", 4) == 0) {
        f.le = false;
    } else {
        return false;
    }
    f.b = b;
    f.n = n;
    f.big = false;
    if (n >= 11 && memcmp(b + 8, "CR\x02", 3) == 0) {
        return true;
    }
    if (tiff_u32_at(&f, 4, &o)) {
        queue[nq++] = o;
    }
    while (nq > 0) {
        uint64_t ifd = queue[--nq];
        uint16_t cnt;
        uint16_t i;
        uint32_t next;
        if (++visited > 16) {
            break;
        }
        if (!tiff_u16_at(&f, ifd, &cnt)) {
            continue;
        }
        for (i = 0; i < cnt; i++) {
            uint64_t e = ifd + 2 + 12 * (uint64_t)i;
            uint16_t tag;
            uint32_t count;
            uint16_t ty;
            uint64_t value = 0;
            bool has_value = false;
            if (!tiff_u16_at(&f, e, &tag) || !tiff_u32_at(&f, e + 4, &count)) {
                break;
            }
            if (tiff_u16_at(&f, e + 2, &ty)) {
                if (ty == 3) {
                    uint16_t v;
                    has_value = tiff_u16_at(&f, e + 8, &v);
                    value = v;
                } else if (ty == 4 || ty == 13) {
                    uint32_t v;
                    has_value = tiff_u32_at(&f, e + 8, &v);
                    value = v;
                }
            }
            if (tag == TAG_DNG_VERSION) {
                return true;
            }
            if (tag == TAG_PHOTOMETRIC && has_value && (value == PHOTOMETRIC_CFA || value == PHOTOMETRIC_LINEAR_RAW)) {
                return true;
            }
            if (tag == TAG_SUB_IFDS && count == 1) {
                if (has_value && nq < 64) {
                    queue[nq++] = value;
                }
            } else if (tag == TAG_SUB_IFDS) {
                /* More than one offset: they are stored at the entry's offset. */
                uint32_t at = 0;
                uint32_t k;
                (void)tiff_u32_at(&f, e + 8, &at);
                for (k = 0; k < count && k < 8; k++) {
                    uint32_t v;
                    if (tiff_u32_at(&f, (uint64_t)at + 4 * (uint64_t)k, &v) && nq < 64) {
                        queue[nq++] = v;
                    }
                }
            }
        }
        if (tiff_u32_at(&f, ifd + 2 + 12 * (uint64_t)cnt, &next) && next != 0 && nq < 64) {
            queue[nq++] = next;
        }
    }
    return false;
}

/* ---- chunk codecs ---- */

/* Decoded chunk status. */
enum { CH_OK, CH_MISSING, CH_ERROR };

/* TIFF LZW (MSB first, 9-12 bit codes, early change). Fills up to need bytes. */
static int lzw_decode(const uint8_t *in, size_t n, uint8_t *out, size_t need, size_t *got)
{
    uint16_t prefix[4096];
    uint8_t suffix[4096];
    uint16_t length[4096];
    uint8_t stack[4097];
    size_t pos = 0;
    uint32_t acc = 0;
    int nbits = 0;
    int width = 9;
    int next = 258;
    int prev = -1;
    size_t o = 0;
    int i;
    for (i = 0; i < 256; i++) {
        prefix[i] = 0;
        suffix[i] = (uint8_t)i;
        length[i] = 1;
    }
    while (o < need) {
        int code;
        int c;
        int len;
        int k;
        while (nbits < width) {
            if (pos >= n) {
                *got = o;
                return CH_MISSING; /* the data ends early */
            }
            acc = (acc << 8) | in[pos++];
            nbits += 8;
        }
        code = (int)((acc >> (nbits - width)) & ((1u << width) - 1));
        nbits -= width;
        if (code == 257) {
            break;
        }
        if (code == 256) {
            width = 9;
            next = 258;
            prev = -1;
            continue;
        }
        if (prev < 0) {
            if (code > 255) {
                return CH_ERROR;
            }
            out[o++] = (uint8_t)code;
            prev = code;
            continue;
        }
        if (code > next || code == 256 || code == 257 || (code == next && next >= 4096)) {
            return CH_ERROR;
        }
        /* The string of code; for code == next, prev's string plus its first byte. */
        c = code < next ? code : prev;
        len = length[c];
        for (k = len; k > 0; k--) {
            stack[k - 1] = suffix[c];
            c = prefix[c];
        }
        if (code == next) {
            stack[len++] = stack[0];
        }
        if (next < 4096) {
            prefix[next] = (uint16_t)prev;
            suffix[next] = stack[0];
            length[next] = (uint16_t)(length[prev] + 1);
            next++;
            /* Early change: the width grows one code before the table needs it. */
            if (next + 1 >= (1 << width) && width < 12) {
                width++;
            }
        }
        for (k = 0; k < len && o < need; k++) {
            out[o++] = stack[k];
        }
        prev = code;
    }
    *got = o;
    return o >= need ? CH_OK : CH_MISSING;
}

static int packbits_decode(const uint8_t *in, size_t n, uint8_t *out, size_t need)
{
    size_t i = 0;
    size_t o = 0;
    while (o < need) {
        int8_t c;
        if (i >= n) {
            return CH_MISSING;
        }
        c = (int8_t)in[i++];
        if (c >= 0) {
            size_t k = (size_t)c + 1;
            if (n - i < k) {
                return CH_MISSING;
            }
            if (k > need - o) {
                k = need - o;
            }
            memcpy(out + o, in + i, k);
            o += k;
            i += (size_t)c + 1;
        } else if (c != -128) {
            size_t k = (size_t)(1 - c);
            if (i >= n) {
                return CH_MISSING;
            }
            if (k > need - o) {
                k = need - o;
            }
            memset(out + o, in[i], k);
            o += k;
            i++;
        }
    }
    return CH_OK;
}

static int inflate_into(const uint8_t *in, size_t n, uint8_t *out, size_t need)
{
    z_stream z;
    int r;
    int st = CH_OK;
    memset(&z, 0, sizeof z);
    if (inflateInit(&z) != Z_OK) {
        return CH_ERROR;
    }
    z.next_in = (Bytef *)(uintptr_t)in;
    z.avail_in = (uInt)(n > UINT32_MAX ? UINT32_MAX : n);
    z.next_out = out;
    while (need - (size_t)((uint8_t *)z.next_out - out) > 0) {
        size_t left = need - (size_t)((uint8_t *)z.next_out - out);
        z.avail_out = (uInt)(left > UINT32_MAX ? UINT32_MAX : left);
        r = inflate(&z, Z_NO_FLUSH);
        if (r == Z_STREAM_END) {
            if ((size_t)((uint8_t *)z.next_out - out) < need) {
                st = CH_MISSING;
            }
            break;
        }
        if (r == Z_BUF_ERROR || (r == Z_OK && z.avail_in == 0 && z.avail_out > 0)) {
            st = CH_ERROR; /* the stream breaks off inside the chunk */
            break;
        }
        if (r != Z_OK) {
            st = CH_ERROR;
            break;
        }
    }
    inflateEnd(&z);
    return st;
}

/* ---- directory decode ---- */

typedef enum src_kind { S_BITS, S_U8, S_U16, S_U32, S_U64, S_F16, S_F32, S_F64 } src_kind;

static size_t src_bytes(src_kind s)
{
    switch (s) {
    case S_BITS:
    case S_U8:
        return 1;
    case S_U16:
    case S_F16:
        return 2;
    case S_U32:
    case S_F32:
        return 4;
    default:
        return 8;
    }
}

static pc_csample src_out(src_kind s)
{
    switch (s) {
    case S_BITS:
    case S_U8:
        return PC_CS_U8;
    case S_U16:
    case S_U32:
    case S_U64:
        return PC_CS_U16;
    case S_F16:
        return PC_CS_F16;
    default:
        return PC_CS_F32;
    }
}

static bool src_identity(src_kind s) { return s == S_U8 || s == S_U16 || s == S_F16 || s == S_F32; }

typedef struct lay {
    const tiff_file *f;
    const tiff_entry *offsets;
    const tiff_entry *counts;
    size_t w;
    size_t h;
    src_kind src;
    unsigned bits;
    pc_csample out;
    pc_clayout out_layout;
    size_t chunk_spp; /* colour-type samples per pixel (1 for planar) */
    size_t raw_spp;   /* samples per pixel stored in a chunk (spp, or 1 for planar) */
    bool planar;
    size_t planes;
    size_t cw;
    size_t ch;
    bool tiled;
    size_t across;
    size_t chunks_per_plane;
    bool invert;
    size_t alpha_sample;
    bool premultiplied;
    uint16_t (*palette)[3];
    size_t scratch_cap;
    uint16_t compression;
    uint16_t predictor;
} lay;

static size_t out_channels(const lay *l) { return pc_clayout_channels(l->out_layout); }

static size_t source_sample(const lay *l, size_t k)
{
    return pc_clayout_has_alpha(l->out_layout) && k + 1 == out_channels(l) ? l->alpha_sample : k;
}

static size_t out_row(const lay *l) { return l->w * out_channels(l) * pc_csample_bytes(l->out); }

static size_t raw_row(const lay *l) { return (l->cw * l->raw_spp * l->bits + 7) / 8; }

/* Undoes the predictor and converts multi-byte samples to native byte order, in place. */
static int finish_raw(const lay *l, uint8_t *raw, size_t rows)
{
    size_t row = raw_row(l);
    size_t bps = src_bytes(l->src);
    size_t nsamp = l->cw * l->raw_spp;
    size_t r;
    bool swap = l->src != S_BITS && bps > 1 && l->f->le != host_little();
    if (l->predictor == 3) {
        uint8_t *tmp;
        if (!(l->src == S_F16 || l->src == S_F32 || l->src == S_F64)) {
            return CH_ERROR;
        }
        tmp = calloc(row > 0 ? row : 1, 1);
        if (tmp == NULL) {
            return CH_ERROR;
        }
        for (r = 0; r < rows; r++) {
            uint8_t *p = raw + r * row;
            size_t i;
            size_t s;
            size_t k;
            for (i = 1; i < row; i++) {
                p[i] = (uint8_t)(p[i] + p[i - 1]);
            }
            memcpy(tmp, p, row);
            for (s = 0; s < nsamp; s++) {
                for (k = 0; k < bps; k++) {
                    /* plane k holds byte k, most significant first */
                    uint8_t v = tmp[k * nsamp + s];
                    p[s * bps + (host_little() ? bps - 1 - k : k)] = v;
                }
            }
        }
        free(tmp);
        return CH_OK;
    }
    for (r = 0; r < rows; r++) {
        uint8_t *p = raw + r * row;
        size_t i;
        if (swap) {
            for (i = 0; i < nsamp; i++) {
                uint8_t *q = p + i * bps;
                size_t a = 0;
                size_t b = bps - 1;
                while (a < b) {
                    uint8_t t = q[a];
                    q[a] = q[b];
                    q[b] = t;
                    a++;
                    b--;
                }
            }
        }
        if (l->predictor == 2) {
            size_t spp = l->raw_spp;
            if (l->src == S_BITS || pc_csample_is_float(src_out(l->src)) || l->src == S_F64) {
                return CH_ERROR;
            }
            for (i = spp; i < nsamp; i++) {
                switch (bps) {
                case 1:
                    p[i] = (uint8_t)(p[i] + p[i - spp]);
                    break;
                case 2: {
                    uint16_t a;
                    uint16_t b;
                    memcpy(&a, p + 2 * i, 2);
                    memcpy(&b, p + 2 * (i - spp), 2);
                    a = (uint16_t)(a + b);
                    memcpy(p + 2 * i, &a, 2);
                    break;
                }
                case 4: {
                    uint32_t a;
                    uint32_t b;
                    memcpy(&a, p + 4 * i, 4);
                    memcpy(&b, p + 4 * (i - spp), 4);
                    a += b;
                    memcpy(p + 4 * i, &a, 4);
                    break;
                }
                default: {
                    uint64_t a;
                    uint64_t b;
                    memcpy(&a, p + 8 * i, 8);
                    memcpy(&b, p + 8 * (i - spp), 8);
                    a += b;
                    memcpy(p + 8 * i, &a, 8);
                }
                }
            }
        }
    }
    return CH_OK;
}

/* Decodes chunk idx into raw (rows full-width chunk rows). */
static int decode_chunk(const lay *l, size_t idx, uint8_t *raw, size_t rows, pc_error *err, pc_status *st)
{
    uint64_t off;
    uint64_t cnt;
    size_t need = raw_row(l) * rows;
    const uint8_t *in;
    int r;
    if (!tiff_uint(l->f, l->offsets, idx, &off) || !tiff_uint(l->f, l->counts, idx, &cnt)) {
        *st = cx_malformed(err, F, "strip or tile table unreadable");
        return CH_ERROR;
    }
    if (off > l->f->n) {
        return CH_MISSING;
    }
    in = l->f->b + off;
    /* Like the reference's tiff crate: uncompressed data ignores the byte count and reads on, and a
     * count past the end of the file is cut there (the decoder reports a short stream as missing). */
    if (cnt > l->f->n - off) {
        cnt = l->f->n - off;
    }
    switch (l->compression) {
    case 1:
        if (l->f->n - off < need) {
            return CH_MISSING;
        }
        memcpy(raw, in, need);
        r = CH_OK;
        break;
    case 5: {
        size_t got = 0;
        r = lzw_decode(in, (size_t)cnt, raw, need, &got);
        break;
    }
    case 8:
    case 32946:
        r = inflate_into(in, (size_t)cnt, raw, need);
        break;
    default: /* 32773 */
        r = packbits_decode(in, (size_t)cnt, raw, need);
    }
    if (r == CH_ERROR) {
        *st = cx_malformed(err, F, "corrupt compressed image data");
        return CH_ERROR;
    }
    if (r == CH_MISSING) {
        return CH_MISSING;
    }
    if (finish_raw(l, raw, rows) != CH_OK) {
        *st = cx_unsupported(err, F, "predictor %u with %u-bit samples", (unsigned)l->predictor, l->bits);
        return CH_ERROR;
    }
    return CH_OK;
}

/* The value of pixel i in a row of one-sample indices (palette and sub-byte gray). */
static size_t sample_index(src_kind s, unsigned bits, const uint8_t *row, size_t i)
{
    if (s == S_BITS) {
        size_t bit = i * bits;
        uint8_t byte = row[bit / 8];
        return (size_t)(((unsigned)byte >> (8 - bits - bit % 8)) & ((1u << bits) - 1));
    }
    if (s == S_U8) {
        return row[i];
    }
    if (s == S_U16) {
        return cx_ne16(row, i);
    }
    return 0;
}

/* Copies sample si of each of n pixels (sspp samples each) to output channel di (dch
 * channels), converting the sample type. */
static void transfer(const lay *l, const uint8_t *row, size_t sspp, size_t si, uint8_t *dst, size_t dch, size_t di,
                     size_t n)
{
    size_t ob = pc_csample_bytes(l->out);
    size_t sb = src_bytes(l->src);
    size_t i;
    if (l->src == S_BITS) {
        uint32_t max = (1u << l->bits) - 1;
        for (i = 0; i < n; i++) {
            dst[i * dch * ob + di] = (uint8_t)(sample_index(l->src, l->bits, row, i) * 255 / max);
        }
        return;
    }
    for (i = 0; i < n; i++) {
        const uint8_t *s = row + (i * sspp + si) * sb;
        uint8_t *d = dst + (i * dch + di) * ob;
        switch (l->src) {
        case S_U32: {
            uint32_t v;
            uint16_t o;
            memcpy(&v, s, 4);
            o = (uint16_t)(v >> 16);
            memcpy(d, &o, 2);
            break;
        }
        case S_U64: {
            uint64_t v;
            uint16_t o;
            memcpy(&v, s, 8);
            o = (uint16_t)(v >> 48);
            memcpy(d, &o, 2);
            break;
        }
        case S_F64: {
            double v;
            float o;
            memcpy(&v, s, 8);
            o = pc_f64_to_f32(v);
            memcpy(d, &o, 4);
            break;
        }
        default:
            memcpy(d, s, sb);
        }
    }
}

/* Converts n pixels of one decoded chunk row into output pixels (plane for planar data). */
static void convert_row(const lay *l, const uint8_t *src, uint8_t *dst, size_t n, size_t plane)
{
    size_t och = out_channels(l);
    size_t k;
    if (l->palette != NULL) {
        size_t ob = pc_csample_bytes(l->out);
        size_t i;
        size_t entries = (size_t)1 << l->bits;
        for (i = 0; i < n; i++) {
            size_t index = sample_index(l->src, l->bits, src, i);
            uint8_t *d = dst + i * och * ob;
            if (index >= entries) {
                continue;
            }
            for (k = 0; k < 3; k++) {
                uint16_t v = l->palette[index][k];
                if (ob == 1) {
                    d[k] = (uint8_t)(v >> 8);
                } else {
                    memcpy(d + 2 * k, &v, 2);
                }
            }
        }
        return;
    }
    if (!l->planar && src_identity(l->src) && l->raw_spp == och) {
        memcpy(dst, src, n * och * pc_csample_bytes(l->out));
        return;
    }
    if (l->planar) {
        transfer(l, src, 1, 0, dst, och, plane, n);
    } else {
        for (k = 0; k < och; k++) {
            transfer(l, src, l->raw_spp, source_sample(l, k), dst, och, k, n);
        }
    }
}

/* WhiteIsZero inversion of the gray channel and associated alpha made straight. */
static void finish_band(const lay *l, uint8_t *band, size_t len)
{
    size_t ch = out_channels(l);
    size_t ob = pc_csample_bytes(l->out);
    size_t color = pc_clayout_color_channels(l->out_layout);
    size_t px = ch * ob;
    size_t p;
    if (!l->invert && !l->premultiplied) {
        return;
    }
    for (p = 0; p + px <= len; p += px) {
        uint8_t *q = band + p;
        size_t i;
        if (l->out == PC_CS_U8) {
            if (l->invert) {
                q[0] = (uint8_t)(255 - q[0]);
            }
            if (l->premultiplied) {
                uint32_t a = q[ch - 1];
                for (i = 0; i < color; i++) {
                    uint32_t v = a == 0 ? 0 : ((uint32_t)q[i] * 255 + a / 2) / a;
                    q[i] = (uint8_t)(v > 255 ? 255 : v);
                }
            }
        } else if (l->out == PC_CS_U16) {
            uint16_t a16 = cx_ne16(q, ch - 1);
            uint32_t a = a16;
            for (i = 0; i < color; i++) {
                uint32_t v = cx_ne16(q, i);
                uint16_t o;
                if (l->invert && i == 0) {
                    v = 65535 - v;
                }
                if (l->premultiplied) {
                    v = a == 0 ? 0 : (v * 65535 + a / 2) / a;
                    if (v > 65535) {
                        v = 65535;
                    }
                }
                o = (uint16_t)v;
                memcpy(q + 2 * i, &o, 2);
            }
        } else {
            float a;
            if (ob == 2) {
                a = pc_f16_to_f32(cx_ne16(q, ch - 1));
            } else {
                memcpy(&a, q + 4 * (ch - 1), 4);
            }
            for (i = 0; i < color; i++) {
                float v;
                if (ob == 2) {
                    v = pc_f16_to_f32(cx_ne16(q, i));
                } else {
                    memcpy(&v, q + 4 * i, 4);
                }
                if (l->invert && i == 0) {
                    v = 1.0f - v;
                }
                if (l->premultiplied && a > 0.0f) {
                    v /= a;
                }
                if (ob == 2) {
                    uint16_t h = pc_f16_from_f32(v);
                    memcpy(q + 2 * i, &h, 2);
                } else {
                    memcpy(q + 4 * i, &v, 4);
                }
            }
        }
    }
}

typedef struct band_ctx {
    const lay *l;
    uint8_t *data;
    size_t band_bytes;
    size_t len;
    size_t *decoded; /* per band */
    size_t *missing;
    pc_error *errs; /* per band */
} band_ctx;

static pc_status band_fn(void *vctx, size_t b, size_t b0, size_t b1)
{
    band_ctx *c = vctx;
    const lay *l = c->l;
    size_t start = b * c->band_bytes;
    size_t len = start + c->band_bytes <= c->len ? c->band_bytes : c->len - start;
    uint8_t *band = c->data + start;
    size_t orow = out_row(l);
    size_t rows = len / orow;
    size_t px = out_channels(l) * pc_csample_bytes(l->out);
    size_t rrow = raw_row(l);
    uint8_t *raw;
    size_t plane;
    pc_status st = PC_OK;
    (void)b0;
    (void)b1;
    if (rrow != 0 && rows > c->l->scratch_cap / rrow) {
        return cx_limit(&c->errs[b], "a %zu byte strip or tile", rrow * rows);
    }
    raw = calloc(rrow * rows > 0 ? rrow * rows : 1, 1);
    if (raw == NULL) {
        return cx_limit(&c->errs[b], "a %zu byte strip or tile", rrow * rows);
    }
    for (plane = 0; plane < l->planes && st == PC_OK; plane++) {
        size_t file_plane = l->planar ? source_sample(l, plane) : 0;
        size_t col;
        for (col = 0; col < l->across; col++) {
            size_t chunk = file_plane * l->chunks_per_plane + b * l->across + col;
            size_t x0 = col * l->cw;
            size_t n = l->cw < l->w - x0 ? l->cw : l->w - x0;
            size_t r;
            int res = decode_chunk(l, chunk, raw, rows, &c->errs[b], &st);
            if (res == CH_ERROR) {
                break;
            }
            if (res == CH_MISSING) {
                c->missing[b]++;
                continue;
            }
            c->decoded[b]++;
            for (r = 0; r < rows; r++) {
                convert_row(l, raw + r * rrow, band + r * orow + x0 * px, n, plane);
            }
        }
    }
    free(raw);
    if (st == PC_OK) {
        finish_band(l, band, len);
    }
    return st;
}

static const struct {
    uint16_t tag;
    const char *key;
} TEXT_TAGS[] = {{270, "Description"}, {271, "Make"},   {272, "Model"},       {305, "Software"},
                 {306, "DateTime"},    {315, "Artist"}, {33432, "Copyright"}};

/* A text tag as UTF-8 without trailing NULs; false when absent, invalid or empty. */
static bool text_tag(const tiff_file *f, const tiff_dir *d, uint16_t tag, const uint8_t **s, size_t *n)
{
    size_t len;
    const uint8_t *b = tiff_tag_bytes(f, d, tag, &len);
    size_t i = 0;
    uint32_t cp;
    if (b == NULL) {
        return false;
    }
    while (i < len) {
        if (!pc_utf8_next(b, len, &i, &cp)) {
            return false;
        }
    }
    while (len > 0 && b[len - 1] == 0) {
        len--;
    }
    if (len == 0) {
        return false;
    }
    *s = b;
    *n = len;
    return true;
}

static pc_status read_metadata(const tiff_file *f, const tiff_dir *d, pc_cimage *img, pc_error *err)
{
    size_t len;
    const uint8_t *b;
    const uint8_t *s;
    size_t sn;
    double x;
    double y;
    uint64_t unit = 2;
    size_t i;
    pc_status st;
    const uint8_t *res;
    const uint8_t *lyr;
    size_t rl;
    size_t ll;
    b = tiff_tag_bytes(f, d, TAG_ICC, &len);
    if (b != NULL && len > 0 && (st = pc_blob_set(&img->icc, b, len, err)) != PC_OK) {
        return st;
    }
    if (text_tag(f, d, TAG_XMP, &s, &sn) && (st = pc_blob_set(&img->meta.xmp, s, sn, err)) != PC_OK) {
        return st;
    }
    (void)tiff_tag_uint(f, d, 296, &unit);
    if (tiff_tag_f64(f, d, 282, &x) && tiff_tag_f64(f, d, 283, &y) && x > 0.0 && y > 0.0 && isfinite(x) &&
        isfinite(y)) {
        if (unit == 2) {
            img->meta.has_dpi = true;
            img->meta.dpi_x = pc_f64_to_f32(x);
            img->meta.dpi_y = pc_f64_to_f32(y);
        } else if (unit == 3) {
            img->meta.has_dpi = true;
            img->meta.dpi_x = pc_f64_to_f32(x * 2.54);
            img->meta.dpi_y = pc_f64_to_f32(y * 2.54);
        }
    }
    for (i = 0; i < sizeof TEXT_TAGS / sizeof TEXT_TAGS[0]; i++) {
        if (text_tag(f, d, TEXT_TAGS[i].tag, &s, &sn) &&
            (st = pc_cmeta_add_text(&img->meta, TEXT_TAGS[i].key, strlen(TEXT_TAGS[i].key), (const char *)s, sn,
                                    err)) != PC_OK) {
            return st;
        }
    }
    /* Photoshop keeps a layered TIFF's layers and resources in IFD0; other pages read their own. */
    if (d->at == f->first_ifd) {
        pc_ctiff_photoshop_tags(f->b, f->n, &res, &rl, &lyr, &ll);
    } else {
        res = tiff_tag_bytes(f, d, TAG_PHOTOSHOP, &rl);
        lyr = tiff_tag_bytes(f, d, TAG_IMAGE_SOURCE_DATA, &ll);
    }
    if (res != NULL && (st = pc_blob_set(&img->meta.photoshop_resources, res, rl, err)) != PC_OK) {
        return st;
    }
    if (lyr != NULL && (st = pc_blob_set(&img->meta.photoshop_layers, lyr, ll, err)) != PC_OK) {
        return st;
    }
    return PC_OK;
}

static const uint16_t STRUCTURAL[] = {256, 257, 258, 259, 262, 273, 277, 278, 279, 284,
                                      317, 322, 323, 324, 325, 338, 339, 347, 530};

static void fmt_opt(char *out, size_t cap, const tiff_entry *e)
{
    if (e == NULL) {
        (void)snprintf(out, cap, "None");
    } else {
        (void)snprintf(out, cap, "Some(%llu)", (unsigned long long)e->count);
    }
}

static pc_status need_uint(const tiff_file *f, const tiff_dir *d, uint16_t tag, const char *name, uint64_t *v,
                           pc_error *err)
{
    if (!tiff_tag_uint(f, d, tag, v)) {
        return cx_malformed(err, F, "missing or unreadable %s", name);
    }
    return PC_OK;
}

/* The reference's tiff crate checks on opening a directory (its errors, in its kinds). */
static pc_status crate_checks(const tiff_file *f, const tiff_dir *d, uint64_t spp, uint64_t nextra, pc_error *err)
{
    const tiff_entry *e;
    uint64_t v;
    size_t i;
    e = tiff_dir_get(d, TAG_SAMPLE_FORMAT);
    if (e != NULL) {
        uint64_t first;
        if (e->count == 0 || !tiff_uint(f, e, 0, &first)) {
            return cx_malformed(err, F, "invalid SampleFormat");
        }
        for (i = 1; i < e->count && i < 64; i++) {
            if (!tiff_uint(f, e, i, &v) || v != first) {
                return cx_unsupported(err, F, "mixed sample formats");
            }
        }
    }
    e = tiff_dir_get(d, TAG_BITS);
    if (e != NULL && e->count != spp && e->count != 1) {
        return cx_malformed(err, F, "inconsistent sizes encountered");
    }
    if (e != NULL && tiff_uint(f, e, 0, &v) && v == 0) {
        return cx_unsupported(err, F, "zero bits per sample");
    }
    if (nextra > spp) {
        return cx_malformed(err, F, "inconsistent sizes encountered");
    }
    if (tiff_tag_uint(f, d, TAG_PREDICTOR, &v) && (v < 1 || v > 3)) {
        return cx_malformed(err, F, "unknown predictor %llu", (unsigned long long)v);
    }
    if (tiff_tag_uint(f, d, TAG_PLANAR, &v) && (v < 1 || v > 2)) {
        return cx_malformed(err, F, "unknown planar configuration %llu", (unsigned long long)v);
    }
    if ((tiff_dir_get(d, TAG_STRIP_OFFSETS) != NULL || tiff_dir_get(d, TAG_STRIP_COUNTS) != NULL) &&
        (tiff_dir_get(d, TAG_TILE_OFFSETS) != NULL || tiff_dir_get(d, TAG_TILE_COUNTS) != NULL)) {
        return cx_malformed(err, F, "strip and tile tags conflict");
    }
    return PC_OK;
}

/* The crate's colour type for a (patched) photometric interpretation. */
typedef enum ct_kind { CT_GRAY, CT_MULTIBAND, CT_RGB, CT_RGBA, CT_CMYK, CT_CMYKA } ct_kind;

static pc_status color_type(uint64_t photometric, uint64_t spp, size_t nextra, bool first_extra_alpha, unsigned bits,
                            ct_kind *out, pc_error *err)
{
    uint64_t ps = spp - nextra;
    switch (photometric) {
    case 0:
    case 1:
        *out = spp == 1 ? CT_GRAY : CT_MULTIBAND;
        return PC_OK;
    case 2:
        if (ps == 3) {
            *out = first_extra_alpha ? CT_RGBA : CT_RGB;
            return PC_OK;
        }
        if (ps == 4) {
            *out = CT_RGBA;
            return PC_OK;
        }
        break;
    case 5:
        if (ps == 4) {
            *out = first_extra_alpha ? CT_CMYKA : CT_CMYK;
            return PC_OK;
        }
        if (ps == 5) {
            *out = CT_CMYKA;
            return PC_OK;
        }
        break;
    case 6:
        if (ps == 3) {
            return cx_unsupported(err, F, "colour type YCbCr(%u)", bits);
        }
        break;
    case 9:
        if (ps == 3) {
            return cx_unsupported(err, F, "colour type Lab(%u)", bits);
        }
        break;
    case 3:
    case 4:
    case 8:
    case 10:
        break;
    default:
        return cx_unsupported(err, F, "unknown photometric interpretation %llu", (unsigned long long)photometric);
    }
    return cx_unsupported(err, F, "photometric interpretation %llu with %llu samples of %u bits",
                          (unsigned long long)photometric, (unsigned long long)spp, bits);
}

static pc_status decode_dir(const tiff_file *f, const tiff_dir *d, const pc_climits *lim, pc_cimage *out,
                            pc_error *err)
{
    lay l;
    uint64_t w64;
    uint64_t h64;
    uint64_t spp = 1;
    uint64_t *bits_all = NULL;
    size_t nbits = 0;
    uint64_t bits = 1;
    uint64_t format = 1;
    uint64_t photometric;
    uint64_t *extra = NULL;
    size_t nextra = 0;
    uint64_t cw;
    uint64_t ch;
    uint64_t planes_in_file;
    uint64_t expected;
    uint64_t v;
    uint64_t comp = 1;
    bool has_alpha_at = false;
    size_t alpha_at = 0;
    size_t color;
    bool all_samples;
    ct_kind ct = CT_GRAY;
    uint64_t eff_photometric;
    size_t chunk_spp;
    bool gray_alpha;
    size_t i;
    uint8_t *data = NULL;
    size_t len;
    band_ctx bc;
    size_t nb;
    size_t decoded = 0;
    size_t missing = 0;
    pc_status st;
    memset(&l, 0, sizeof l);
    memset(&bc, 0, sizeof bc);
    for (i = 0; i < d->n; i++) {
        size_t k;
        size_t dl;
        for (k = 0; k < sizeof STRUCTURAL / sizeof STRUCTURAL[0]; k++) {
            if (d->entries[i].tag == STRUCTURAL[k] && tiff_data(f, &d->entries[i], &dl) == NULL) {
                return cx_malformed(err, F, "tag %u points past the end of the file", (unsigned)d->entries[i].tag);
            }
        }
    }
    if ((st = need_uint(f, d, TAG_WIDTH, "ImageWidth", &w64, err)) != PC_OK) {
        return st;
    }
    if (w64 > UINT32_MAX) {
        return cx_malformed(err, F, "ImageWidth out of range");
    }
    if ((st = need_uint(f, d, TAG_HEIGHT, "ImageLength", &h64, err)) != PC_OK) {
        return st;
    }
    if (h64 > UINT32_MAX) {
        return cx_malformed(err, F, "ImageLength out of range");
    }
    if (w64 == 0 || h64 == 0) {
        return cx_invalid(err, "zero-sized image %llux%llu", (unsigned long long)w64, (unsigned long long)h64);
    }
    (void)tiff_tag_uint(f, d, TAG_SAMPLES, &spp);
    if (spp == 0 || spp > 64) {
        return cx_unsupported(err, F, "%llu samples per pixel", (unsigned long long)spp);
    }
    if (tiff_tag_uints(f, d, TAG_BITS, spp, &bits_all, &nbits) && nbits > 0) {
        bits = bits_all[0];
        for (i = 1; i < nbits; i++) {
            if (bits_all[i] != bits) {
                char list[512];
                size_t k;
                size_t o = 0;
                list[0] = '\0';
                for (k = 0; k < nbits && o + 24 < sizeof list; k++) {
                    o += (size_t)snprintf(list + o, sizeof list - o, "%s%llu", k ? ", " : "",
                                          (unsigned long long)bits_all[k]);
                }
                free(bits_all);
                return cx_unsupported(err, F, "mixed bits per sample [%s]", list);
            }
        }
    }
    free(bits_all);
    (void)tiff_tag_uint(f, d, TAG_SAMPLE_FORMAT, &format);
    if (!tiff_tag_uint(f, d, TAG_PHOTOMETRIC, &photometric)) {
        return cx_unsupported(err, F, "missing PhotometricInterpretation");
    }
    l.f = f;
    l.planar = tiff_tag_uint(f, d, TAG_PLANAR, &v) && v == 2;
    if (!tiff_tag_uints(f, d, TAG_EXTRA_SAMPLES, 64, &extra, &nextra)) {
        extra = NULL;
        nextra = 0;
    }
    l.tiled = tiff_dir_get(d, TAG_TILE_OFFSETS) != NULL;
    if (l.tiled) {
        if ((st = need_uint(f, d, TAG_TILE_WIDTH, "TileWidth", &cw, err)) != PC_OK ||
            (st = need_uint(f, d, TAG_TILE_LENGTH, "TileLength", &ch, err)) != PC_OK) {
            goto fail;
        }
    } else {
        cw = w64;
        ch = h64;
        (void)tiff_tag_uint(f, d, TAG_ROWS_PER_STRIP, &ch);
        if (ch > h64) {
            ch = h64;
        }
    }
    if (cw == 0 || ch == 0) {
        st = cx_malformed(err, F, "zero tile or strip size");
        goto fail;
    }
    planes_in_file = l.planar ? spp : 1;
    {
        uint64_t across = (w64 + cw - 1) / cw;
        uint64_t down = (h64 + ch - 1) / ch;
        if ((down != 0 && across > UINT64_MAX / down) || across * down > UINT64_MAX / planes_in_file ||
            (expected = across * down * planes_in_file) > UINT32_MAX) {
            st = cx_malformed(err, F, "too many strips or tiles");
            goto fail;
        }
    }
    l.offsets = tiff_dir_get(d, l.tiled ? TAG_TILE_OFFSETS : TAG_STRIP_OFFSETS);
    l.counts = tiff_dir_get(d, l.tiled ? TAG_TILE_COUNTS : TAG_STRIP_COUNTS);
    if (l.offsets == NULL || l.counts == NULL || l.offsets->count != expected || l.counts->count != expected) {
        char a[32];
        char b[32];
        fmt_opt(a, sizeof a, l.offsets);
        fmt_opt(b, sizeof b, l.counts);
        st = cx_malformed(err, F, "%llu strips or tiles expected, the tables list (%s, %s)", (unsigned long long)expected,
                          a, b);
        goto fail;
    }
    if (format == 1 && (bits == 1 || bits == 2 || bits == 4)) {
        l.src = S_BITS;
    } else if (format == 1 && bits == 8) {
        l.src = S_U8;
    } else if (format == 1 && bits == 16) {
        l.src = S_U16;
    } else if (format == 1 && bits == 32) {
        l.src = S_U32;
    } else if (format == 1 && bits == 64) {
        l.src = S_U64;
    } else if (format == 3 && bits == 16) {
        l.src = S_F16;
    } else if (format == 3 && bits == 32) {
        l.src = S_F32;
    } else if (format == 3 && bits == 64) {
        l.src = S_F64;
    } else if (format == 2) {
        st = cx_unsupported(err, F, "signed integer samples");
        goto fail;
    } else {
        st = cx_unsupported(err, F, "%llu-bit samples (sample format %llu)", (unsigned long long)bits,
                            (unsigned long long)format);
        goto fail;
    }
    l.bits = (unsigned)bits;
    /* WhiteIsZero and palette images: the stored samples are read and interpreted here. */
    l.invert = photometric == 0;
    if (photometric == 3) {
        const tiff_entry *map = tiff_dir_get(d, TAG_COLOR_MAP);
        size_t n;
        if (spp != 1 || !(l.src == S_BITS || l.src == S_U8 || l.src == S_U16)) {
            st = cx_unsupported(err, F, "palette image with %llu samples of %llu bits", (unsigned long long)spp,
                                (unsigned long long)bits);
            goto fail;
        }
        n = (size_t)1 << bits;
        if (map == NULL || map->ty != 3 || map->count != 3 * (uint64_t)n) {
            st = cx_malformed(err, F, "palette image without a valid ColorMap");
            goto fail;
        }
        l.palette = pc_malloc_array(n, sizeof *l.palette);
        if (l.palette == NULL) {
            st = cx_nomem(err);
            goto fail;
        }
        for (i = 0; i < n; i++) {
            uint64_t r;
            uint64_t g;
            uint64_t bl;
            if (!tiff_uint(f, map, i, &r) || !tiff_uint(f, map, n + i, &g) || !tiff_uint(f, map, 2 * n + i, &bl)) {
                st = cx_malformed(err, F, "palette ColorMap cut off");
                goto fail;
            }
            l.palette[i][0] = (uint16_t)r;
            l.palette[i][1] = (uint16_t)g;
            l.palette[i][2] = (uint16_t)bl;
        }
    }
    /* Any extra sample may be the alpha; an alpha position past the samples is ignored. */
    color = photometric == 2 ? 3 : photometric == 5 ? 4 : 1;
    for (i = 0; i < nextra; i++) {
        if (extra[i] == 1 || extra[i] == 2) {
            if (color + i < spp) {
                has_alpha_at = true;
                alpha_at = i;
            }
            break;
        }
    }
    all_samples = l.palette == NULL && (photometric == 2 || photometric == 5) && has_alpha_at && alpha_at > 0;
    if ((st = crate_checks(f, d, spp, nextra, err)) != PC_OK) {
        goto fail;
    }
    (void)tiff_tag_uint(f, d, TAG_COMPRESSION, &comp);
    if (!(comp == 1 || comp == 5 || comp == 8 || comp == 32946 || comp == 32773)) {
        st = cx_unsupported(err, F, "compression method %llu", (unsigned long long)comp);
        goto fail;
    }
    l.compression = (uint16_t)comp;
    l.predictor = tiff_tag_uint(f, d, TAG_PREDICTOR, &v) ? (uint16_t)v : 1;
    eff_photometric = (l.invert || l.palette != NULL || all_samples) ? 1 : photometric;
    if ((st = color_type(eff_photometric, spp, nextra, nextra > 0 && (extra[0] == 1 || extra[0] == 2), l.bits, &ct,
                         err)) != PC_OK) {
        goto fail;
    }
    gray_alpha = has_alpha_at || (nextra == 0 && spp == 2);
    if (l.palette != NULL) {
        l.out_layout = PC_CL_RGB;
        chunk_spp = 1;
    } else if (ct == CT_GRAY) {
        l.out_layout = PC_CL_GRAY;
        chunk_spp = 1;
    } else if (ct == CT_MULTIBAND && all_samples) {
        l.out_layout = photometric == 2 ? PC_CL_RGBA : PC_CL_CMYKA;
        chunk_spp = (size_t)spp;
    } else if (ct == CT_MULTIBAND && photometric <= 1) {
        l.out_layout = gray_alpha ? PC_CL_GRAYA : PC_CL_GRAY;
        chunk_spp = (size_t)spp;
    } else if (ct == CT_RGB) {
        l.out_layout = PC_CL_RGB;
        chunk_spp = 3;
    } else if (ct == CT_RGBA) {
        l.out_layout = PC_CL_RGBA;
        chunk_spp = 4;
    } else if (ct == CT_CMYK) {
        l.out_layout = PC_CL_CMYK;
        chunk_spp = 4;
    } else if (ct == CT_CMYKA) {
        l.out_layout = PC_CL_CMYKA;
        chunk_spp = 5;
    } else {
        st = cx_unsupported(err, F, "colour type Multiband");
        goto fail;
    }
    if (l.planar) {
        chunk_spp = 1;
    }
    l.chunk_spp = chunk_spp;
    l.raw_spp = l.planar ? 1 : (size_t)spp;
    /* The sample (chunky) or plane (planar) that holds the alpha: the colour samples come first. */
    l.alpha_sample = pc_clayout_color_channels(l.out_layout) + (has_alpha_at ? alpha_at : 0);
    l.premultiplied = pc_clayout_has_alpha(l.out_layout) && has_alpha_at && extra[alpha_at] == 1;
    if (pc_clayout_has_alpha(l.out_layout) &&
        (l.alpha_sample >= spp || (!l.planar && l.alpha_sample >= chunk_spp))) {
        st = cx_malformed(err, F, "the alpha sample is past the samples of a pixel");
        goto fail;
    }
    if (l.src == S_BITS && (pc_clayout_channels(l.out_layout) > 1 || spp != 1) && l.palette == NULL) {
        st = cx_unsupported(err, F, "%llu-bit samples with %llu samples per pixel", (unsigned long long)bits,
                            (unsigned long long)spp);
        goto fail;
    }
    if ((!l.planar && l.palette == NULL && chunk_spp < pc_clayout_channels(l.out_layout)) ||
        (l.planar && pc_clayout_channels(l.out_layout) > spp)) {
        st = cx_malformed(err, F, "fewer samples than the colour type needs");
        goto fail;
    }
    l.out = l.palette != NULL ? (l.src == S_U16 ? PC_CS_U16 : PC_CS_U8) : src_out(l.src);
    if ((st = pc_climits_check(lim, (uint32_t)w64, (uint32_t)h64, l.out_layout, l.out, err)) != PC_OK) {
        goto fail;
    }
    /* Decompression-bomb guard: refuse a file far too small for its declared size. */
    {
        uint64_t stored = w64 * h64;
        uint64_t ratio = comp == 1 ? 4 : comp == 32773 ? 130 : 4096;
        uint64_t cap;
        stored = spp != 0 && stored > UINT64_MAX / spp ? UINT64_MAX : stored * spp;
        stored = bits != 0 && stored > UINT64_MAX / bits ? UINT64_MAX : stored * bits;
        stored = stored / 8 + (stored % 8 != 0);
        cap = (uint64_t)f->n > (UINT64_MAX - (1u << 20)) / ratio ? UINT64_MAX : (uint64_t)f->n * ratio + (1u << 20);
        if (stored > cap) {
            st = cx_malformed(err, F, "a %zu byte file cannot hold a %llux%llu image", f->n, (unsigned long long)w64,
                              (unsigned long long)h64);
            goto fail;
        }
    }
    l.w = (size_t)w64;
    l.h = (size_t)h64;
    if (cw > SIZE_MAX || ch > SIZE_MAX) {
        st = cx_malformed(err, F, "tile too large");
        goto fail;
    }
    l.cw = (size_t)cw;
    l.ch = (size_t)ch;
    l.across = (l.w + l.cw - 1) / l.cw;
    l.chunks_per_plane = l.across * ((l.h + l.ch - 1) / l.ch);
    l.planes = l.planar ? pc_clayout_channels(l.out_layout) : 1;
    if (l.tiled && l.across > 1 && ((uint64_t)l.cw * l.bits * chunk_spp) % 8 != 0) {
        st = cx_unsupported(err, F, "misaligned tile boundaries");
        goto fail;
    }
    {
        /* One chunk's decoded bytes, clipped to the image, bound the scratch buffer. */
        uint64_t cb = (uint64_t)(l.cw < l.w ? l.cw : l.w) * (l.ch < l.h ? l.ch : l.h);
        uint64_t sc;
        cb = cb > UINT64_MAX / chunk_spp ? UINT64_MAX : cb * chunk_spp;
        cb = cb > UINT64_MAX / src_bytes(l.src) ? UINT64_MAX : cb * src_bytes(l.src);
        sc = cb > UINT64_MAX / 2 ? UINT64_MAX : cb * 2;
        if (sc < ((uint64_t)64 << 20)) {
            sc = (uint64_t)64 << 20;
        }
        if (sc > cx_alloc_usize(lim)) {
            sc = cx_alloc_usize(lim);
        }
        l.scratch_cap = sc > SIZE_MAX ? SIZE_MAX : (size_t)sc;
    }
    if (!pc_size_mul(out_row(&l), l.h, &len)) {
        st = cx_limit(err, "image size");
        goto fail;
    }
    data = calloc(len > 0 ? len : 1, 1);
    if (data == NULL) {
        st = cx_limit(err, "cannot allocate %zu bytes for the image", len);
        goto fail;
    }
    bc.l = &l;
    bc.data = data;
    bc.len = len;
    bc.band_bytes = out_row(&l) * (l.ch < l.h ? l.ch : l.h);
    nb = (len + bc.band_bytes - 1) / bc.band_bytes;
    bc.decoded = calloc(nb, sizeof *bc.decoded);
    bc.missing = calloc(nb, sizeof *bc.missing);
    bc.errs = calloc(nb, sizeof *bc.errs);
    if (bc.decoded == NULL || bc.missing == NULL || bc.errs == NULL) {
        st = cx_nomem(err);
        goto fail;
    }
    st = pc_pool_for(pc_pool_global(), nb, 1, band_fn, &bc);
    if (st != PC_OK) {
        for (i = 0; i < nb; i++) {
            if (bc.errs[i].code != PC_OK) {
                if (err != NULL) {
                    *err = bc.errs[i];
                }
                st = bc.errs[i].code;
                break;
            }
        }
        goto fail;
    }
    for (i = 0; i < nb; i++) {
        decoded += bc.decoded[i];
        missing += bc.missing[i];
    }
    if (decoded == 0 && missing > 0) {
        st = cx_malformed(err, F, "the image data is missing (cut off or past the end of the file)");
        goto fail;
    }
    st = pc_cimage_from_raw((uint32_t)w64, (uint32_t)h64, l.out_layout, l.out, data, len, out, err);
    data = NULL;
    if (st == PC_OK && missing > 0) {
        pc_cwarning wn;
        memset(&wn, 0, sizeof wn);
        wn.kind = PC_CW_TRUNCATED;
        wn.format = F;
        st = pc_cimage_add_warning(out, wn, err);
        if (st != PC_OK) {
            pc_cimage_fini(out);
        }
    }
fail:
    free(data);
    free(bc.decoded);
    free(bc.missing);
    free(bc.errs);
    free(l.palette);
    free(extra);
    return st;
}

pc_status cx_tiff_decode_page(const uint8_t *b, size_t n, bool has_page, size_t page, const pc_climits *lim,
                              pc_cimage *out, pc_error *err)
{
    tiff_file f;
    pc_tiff_info info;
    size_t index;
    tiff_dir d;
    pc_cimage img;
    size_t pages;
    pc_status st;
    if (camera_raw(b, n)) {
        return cx_unsupported(err, F,
                              "camera raw files (such as CR2, NEF, ARW or DNG) are not flat images; decode them with "
                              "photocraft-raw");
    }
    if (!tiff_file_parse(b, n, &f)) {
        return cx_malformed(err, F, "not a TIFF or BigTIFF header");
    }
    if ((st = tiff_info_of(&f, &info, err)) != PC_OK) {
        return st;
    }
    if (has_page) {
        index = page;
    } else if (!pc_tiff_info_default_page(&info, &index)) {
        pc_tiff_info_fini(&info);
        return cx_malformed(err, F, "no image directory");
    }
    if (index >= info.npages) {
        st = cx_invalid(err, "TIFF page %zu does not exist (the file has %zu directories)", index, info.npages);
        pc_tiff_info_fini(&info);
        return st;
    }
    if (!tiff_dir_read(&f, info.pages[index].ifd_offset, &d)) {
        pc_tiff_info_fini(&info);
        return cx_malformed(err, F, "image directory cut off");
    }
    pc_cimage_init(&img);
    st = decode_dir(&f, &d, lim, &img, err);
    if (st == PC_OK) {
        st = read_metadata(&f, &d, &img, err);
    }
    pages = pc_tiff_info_page_count(&info);
    if (st == PC_OK && pages > 1) {
        pc_cwarning w;
        memset(&w, 0, sizeof w);
        w.kind = PC_CW_MORE_PAGES;
        w.has_total = info.complete;
        w.total = pages > UINT32_MAX ? UINT32_MAX : (uint32_t)pages;
        st = pc_cimage_add_warning(&img, w, err);
    }
    tiff_dir_fini(&d);
    pc_tiff_info_fini(&info);
    if (st != PC_OK) {
        pc_cimage_fini(&img);
        return st;
    }
    *out = img;
    return PC_OK;
}

uint16_t cx_tiff_orientation(const uint8_t *b, size_t n, bool has_page, size_t page)
{
    tiff_file f;
    pc_tiff_info info;
    size_t index;
    tiff_dir d;
    uint16_t o = 1;
    if (!tiff_file_parse(b, n, &f) || tiff_info_of(&f, &info, NULL) != PC_OK) {
        return 1;
    }
    if (has_page) {
        index = page;
    } else if (!pc_tiff_info_default_page(&info, &index)) {
        pc_tiff_info_fini(&info);
        return 1;
    }
    if (index < info.npages && tiff_dir_read(&f, info.pages[index].ifd_offset, &d)) {
        o = tiff_orientation_of(&f, &d);
        tiff_dir_fini(&d);
    }
    pc_tiff_info_fini(&info);
    return o;
}

/* ---- encoding ---- */

/* TIFF LZW encoder (MSB first, early change, clear at 4094), appending to out. */
static pc_status lzw_encode(const uint8_t *in, size_t n, pc_buf *out)
{
    /* open-addressing table of (prefix << 8 | byte) -> code */
    enum { HSIZE = 9001 };
    static const uint32_t EMPTY = 0xFFFFFFFFu;
    uint32_t *keys = malloc(HSIZE * sizeof *keys);
    uint16_t *vals = malloc(HSIZE * sizeof *vals);
    uint64_t acc = 0;
    int nbits = 0;
    int width = 9;
    int next = 258;
    int prefix;
    size_t i;
    pc_status st = PC_OK;
    uint8_t buf[4096];
    size_t bl = 0;
#define PUT(code)                                                                                  \
    do {                                                                                           \
        acc = (acc << width) | (uint64_t)(code);                                                   \
        nbits += width;                                                                            \
        while (nbits >= 8) {                                                                       \
            buf[bl++] = (uint8_t)(acc >> (nbits - 8));                                             \
            nbits -= 8;                                                                            \
            if (bl == sizeof buf) {                                                                \
                if (st == PC_OK) {                                                                 \
                    st = pc_buf_append(out, buf, bl, NULL);                                        \
                }                                                                                  \
                bl = 0;                                                                            \
            }                                                                                      \
        }                                                                                          \
    } while (0)
    if (keys == NULL || vals == NULL) {
        free(keys);
        free(vals);
        return PC_ERR_NOMEM;
    }
    for (i = 0; i < HSIZE; i++) {
        keys[i] = EMPTY;
    }
    PUT(256);
    if (n == 0) {
        PUT(257);
    } else {
        prefix = in[0];
        for (i = 1; i < n; i++) {
            uint32_t key = ((uint32_t)prefix << 8) | in[i];
            size_t h = (size_t)(key % HSIZE);
            while (keys[h] != EMPTY && keys[h] != key) {
                h = h + 1 == HSIZE ? 0 : h + 1;
            }
            if (keys[h] == key) {
                prefix = vals[h];
                continue;
            }
            PUT((uint32_t)prefix);
            keys[h] = key;
            vals[h] = (uint16_t)next;
            next++;
            if (next == 4094) {
                PUT(256);
                for (h = 0; h < HSIZE; h++) {
                    keys[h] = EMPTY;
                }
                width = 9;
                next = 258;
            } else if (next >= (1 << width) && width < 12) {
                width++;
            }
            prefix = in[i];
        }
        PUT((uint32_t)prefix);
        next++;
        if (next == 4094) {
            PUT(256);
            width = 9;
        } else if (next >= (1 << width) && width < 12) {
            width++;
        }
        PUT(257);
    }
    if (nbits > 0) {
        buf[bl++] = (uint8_t)(acc << (8 - nbits));
    }
    if (st == PC_OK && bl > 0) {
        st = pc_buf_append(out, buf, bl, NULL);
    }
#undef PUT
    free(keys);
    free(vals);
    return st;
}

static pc_status packbits_encode(const uint8_t *in, size_t n, pc_buf *out)
{
    size_t i = 0;
    pc_status st = PC_OK;
    while (i < n && st == PC_OK) {
        size_t run = 1;
        while (i + run < n && run < 128 && in[i + run] == in[i]) {
            run++;
        }
        if (run >= 2) {
            uint8_t h[2] = {(uint8_t)(1 - (int)run), in[i]};
            st = pc_buf_append(out, h, 2, NULL);
            i += run;
        } else {
            size_t start = i;
            uint8_t c;
            i++;
            while (i < n && i - start < 128 && !(i + 1 < n && in[i] == in[i + 1])) {
                i++;
            }
            c = (uint8_t)(i - start - 1);
            st = pc_buf_append(out, &c, 1, NULL);
            if (st == PC_OK) {
                st = pc_buf_append(out, in + start, i - start, NULL);
            }
        }
    }
    return st;
}

typedef struct strip_job {
    const uint8_t *data;
    size_t row;
    size_t h;
    size_t rps;
    size_t spp;
    size_t bps;
    bool predict;
    pc_tiff_compression comp;
    pc_buf *parts;
} strip_job;

static pc_status strip_fn(void *ctx, size_t idx, size_t b0, size_t b1)
{
    strip_job *j = ctx;
    size_t y0 = idx * j->rps;
    size_t rows = y0 + j->rps <= j->h ? j->rps : j->h - y0;
    size_t len = rows * j->row;
    uint8_t *tmp = NULL;
    const uint8_t *src = j->data + y0 * j->row;
    pc_status st = PC_OK;
    (void)b0;
    (void)b1;
    if (j->predict) {
        size_t r;
        tmp = malloc(len > 0 ? len : 1);
        if (tmp == NULL) {
            return PC_ERR_NOMEM;
        }
        memcpy(tmp, src, len);
        for (r = 0; r < rows; r++) {
            uint8_t *p = tmp + r * j->row;
            size_t nsamp = j->row / j->bps;
            size_t i;
            for (i = nsamp; i-- > j->spp;) {
                if (j->bps == 1) {
                    p[i] = (uint8_t)(p[i] - p[i - j->spp]);
                } else {
                    uint16_t a;
                    uint16_t b;
                    memcpy(&a, p + 2 * i, 2);
                    memcpy(&b, p + 2 * (i - j->spp), 2);
                    a = (uint16_t)(a - b);
                    memcpy(p + 2 * i, &a, 2);
                }
            }
        }
        src = tmp;
    }
    switch (j->comp) {
    case PC_TIFF_NONE:
        st = pc_buf_append(&j->parts[idx], src, len, NULL);
        break;
    case PC_TIFF_LZW:
        st = lzw_encode(src, len, &j->parts[idx]);
        break;
    case PC_TIFF_DEFLATE:
        st = pc_deflate(PC_Z_ZLIB, 6, src, len, &j->parts[idx], NULL);
        break;
    default: {
        size_t r;
        for (r = 0; r < rows && st == PC_OK; r++) {
            st = packbits_encode(src + r * j->row, j->row, &j->parts[idx]);
        }
    }
    }
    free(tmp);
    return st;
}

typedef struct tag_out {
    uint16_t tag;
    uint16_t ty;
    uint64_t count;
    const uint8_t *data; /* in file byte order */
    size_t len;
} tag_out;

static int tag_cmp(const void *a, const void *b)
{
    const tag_out *x = a;
    const tag_out *y = b;
    return (int)x->tag - (int)y->tag;
}

/* Native-order value bytes (the file is written in native byte order). */
typedef struct vals {
    uint8_t b[64];
    size_t n;
} vals;

static void v16(vals *v, uint16_t x)
{
    memcpy(v->b + v->n, &x, 2);
    v->n += 2;
}

static void v32(vals *v, uint32_t x)
{
    memcpy(v->b + v->n, &x, 4);
    v->n += 4;
}

static bool upcase_eq(const char *a, size_t an, const char *b)
{
    size_t i;
    if (an != strlen(b)) {
        return false;
    }
    for (i = 0; i < an; i++) {
        char x = a[i] >= 'a' && a[i] <= 'z' ? (char)(a[i] - 32) : a[i];
        char y = b[i] >= 'a' && b[i] <= 'z' ? (char)(b[i] - 32) : b[i];
        if (x != y) {
            return false;
        }
    }
    return true;
}

static pc_status write_tiff(const pc_cimage *img, const pc_cencode_opts *o, bool big, const uint8_t *xmp,
                            size_t xmp_len, pc_buf *out, bool *overflow, pc_error *err)
{
    size_t ch = pc_clayout_channels(img->layout);
    size_t bps = pc_csample_bytes(img->sample);
    size_t row = (size_t)img->width * ch * bps;
    size_t rps = row > 0 ? (1000000 + row - 1) / row : 1;
    size_t nstrips;
    strip_job j;
    pc_buf *parts;
    uint64_t *offs;
    uint64_t *cnts;
    tag_out tags[32];
    size_t nt = 0;
    vals vbits;
    vals vfmt;
    vals vsmall[12];
    size_t ns = 0;
    uint8_t res[16];
    size_t i;
    size_t k;
    pc_status st = PC_OK;
    bool predict = !pc_csample_is_float(img->sample) &&
                   (o->tiff_compression == PC_TIFF_LZW || o->tiff_compression == PC_TIFF_DEFLATE);
    uint16_t compression = o->tiff_compression == PC_TIFF_NONE    ? 1
                           : o->tiff_compression == PC_TIFF_LZW   ? 5
                           : o->tiff_compression == PC_TIFF_DEFLATE ? 8
                                                                    : 32773;
    uint16_t photometric = pc_clayout_is_gray(img->layout) ? 1 : pc_clayout_is_rgb(img->layout) ? 2 : 5;
    uint8_t *offbuf = NULL;
    uint8_t *cntbuf = NULL;
    uint64_t ifd_at;
    *overflow = false;
    if (rps > img->height) {
        rps = img->height;
    }
    if (rps == 0) {
        rps = 1;
    }
    nstrips = (img->height + rps - 1) / rps;
    parts = calloc(nstrips, sizeof *parts);
    offs = calloc(nstrips, sizeof *offs);
    cnts = calloc(nstrips, sizeof *cnts);
    if (parts == NULL || offs == NULL || cnts == NULL) {
        free(parts);
        free(offs);
        free(cnts);
        return cx_nomem(err);
    }
    j.data = img->data;
    j.row = row;
    j.h = img->height;
    j.rps = rps;
    j.spp = ch;
    j.bps = bps;
    j.predict = predict;
    j.comp = o->tiff_compression;
    j.parts = parts;
    st = pc_pool_for(nstrips > 1 ? pc_pool_global() : NULL, nstrips, 1, strip_fn, &j);
    if (st != PC_OK) {
        st = cx_encode(err, F, "strip compression failed");
        goto done;
    }
    /* Layout: header, strips, out-of-line tag values, the IFD. */
    pc_buf_truncate(out, 0);
    {
        cx_w w = {out, PC_OK, err};
        bool le = host_little();
        cx_w_put(&w, le ? "II" : "MM", 2);
        if (big) {
            uint16_t v = 43;
            uint16_t s8 = 8;
            uint16_t z = 0;
            uint64_t ph = 0;
            cx_w_put(&w, &v, 2);
            cx_w_put(&w, &s8, 2);
            cx_w_put(&w, &z, 2);
            cx_w_put(&w, &ph, 8);
        } else {
            uint16_t v = 42;
            uint32_t ph = 0;
            cx_w_put(&w, &v, 2);
            cx_w_put(&w, &ph, 4);
        }
        for (i = 0; i < nstrips; i++) {
            offs[i] = out->len;
            cnts[i] = parts[i].len;
            cx_w_put(&w, parts[i].data, parts[i].len);
            pc_buf_fini(&parts[i]);
            if (out->len & 1) {
                cx_w_byte(&w, 0);
            }
        }
        if (w.st != PC_OK) {
            st = w.st;
            goto done;
        }
    }
    if (!big && out->len > UINT32_MAX) {
        *overflow = true;
        goto done;
    }
    /* tag values */
    memset(&vbits, 0, sizeof vbits);
    memset(&vfmt, 0, sizeof vfmt);
    for (i = 0; i < ch; i++) {
        v16(&vbits, (uint16_t)(bps * 8));
        v16(&vfmt, pc_csample_is_float(img->sample) ? 3 : 1);
    }
    offbuf = malloc(nstrips * (big ? 8 : 4));
    cntbuf = malloc(nstrips * (big ? 8 : 4));
    if (offbuf == NULL || cntbuf == NULL) {
        st = cx_nomem(err);
        goto done;
    }
    for (i = 0; i < nstrips; i++) {
        if (big) {
            memcpy(offbuf + 8 * i, &offs[i], 8);
            memcpy(cntbuf + 8 * i, &cnts[i], 8);
        } else {
            uint32_t a = (uint32_t)offs[i];
            uint32_t c = (uint32_t)cnts[i];
            if (offs[i] > UINT32_MAX || cnts[i] > UINT32_MAX) {
                *overflow = true;
                goto done;
            }
            memcpy(offbuf + 4 * i, &a, 4);
            memcpy(cntbuf + 4 * i, &c, 4);
        }
    }
#define SMALL(tg, typ, cnt, fill)                                                                  \
    do {                                                                                           \
        vals *vv = &vsmall[ns++];                                                                  \
        vv->n = 0;                                                                                 \
        fill;                                                                                      \
        tags[nt].tag = (tg);                                                                       \
        tags[nt].ty = (typ);                                                                       \
        tags[nt].count = (cnt);                                                                    \
        tags[nt].data = vv->b;                                                                     \
        tags[nt].len = vv->n;                                                                      \
        nt++;                                                                                      \
    } while (0)
    SMALL(256, 4, 1, v32(vv, img->width));
    SMALL(257, 4, 1, v32(vv, img->height));
    tags[nt].tag = 258;
    tags[nt].ty = 3;
    tags[nt].count = ch;
    tags[nt].data = vbits.b;
    tags[nt].len = vbits.n;
    nt++;
    SMALL(259, 3, 1, v16(vv, compression));
    SMALL(262, 3, 1, v16(vv, photometric));
    tags[nt].tag = 273;
    tags[nt].ty = big ? 16 : 4;
    tags[nt].count = nstrips;
    tags[nt].data = offbuf;
    tags[nt].len = nstrips * (big ? 8 : 4);
    nt++;
    SMALL(277, 3, 1, v16(vv, (uint16_t)ch));
    SMALL(278, 4, 1, v32(vv, (uint32_t)rps));
    tags[nt].tag = 279;
    tags[nt].ty = big ? 16 : 4;
    tags[nt].count = nstrips;
    tags[nt].data = cntbuf;
    tags[nt].len = nstrips * (big ? 8 : 4);
    nt++;
    {
        bool dpi = o->embed_metadata && img->meta.has_dpi && img->meta.dpi_x > 0.0f && img->meta.dpi_y > 0.0f;
        uint32_t xn = dpi ? pc_f32_to_u32(roundf(img->meta.dpi_x * 1000.0f)) : 1;
        uint32_t yn = dpi ? pc_f32_to_u32(roundf(img->meta.dpi_y * 1000.0f)) : 1;
        uint32_t dd = dpi ? 1000 : 1;
        memcpy(res, &xn, 4);
        memcpy(res + 4, &dd, 4);
        memcpy(res + 8, &yn, 4);
        memcpy(res + 12, &dd, 4);
        tags[nt].tag = 282;
        tags[nt].ty = 5;
        tags[nt].count = 1;
        tags[nt].data = res;
        tags[nt].len = 8;
        nt++;
        tags[nt].tag = 283;
        tags[nt].ty = 5;
        tags[nt].count = 1;
        tags[nt].data = res + 8;
        tags[nt].len = 8;
        nt++;
        SMALL(296, 3, 1, v16(vv, dpi ? 2 : 1));
    }
    SMALL(284, 3, 1, v16(vv, 1));
    if (predict) {
        SMALL(317, 3, 1, v16(vv, 2));
    }
    if (pc_clayout_has_alpha(img->layout)) {
        SMALL(338, 3, 1, v16(vv, 2)); /* unassociated alpha */
    }
    tags[nt].tag = 339;
    tags[nt].ty = 3;
    tags[nt].count = ch;
    tags[nt].data = vfmt.b;
    tags[nt].len = vfmt.n;
    nt++;
#undef SMALL
    if (o->embed_icc && img->icc.has && img->icc.len > 0) {
        tags[nt].tag = TAG_ICC;
        tags[nt].ty = 7;
        tags[nt].count = img->icc.len;
        tags[nt].data = img->icc.data;
        tags[nt].len = img->icc.len;
        nt++;
    }
    if (xmp != NULL) {
        tags[nt].tag = TAG_XMP;
        tags[nt].ty = 1;
        tags[nt].count = xmp_len;
        tags[nt].data = xmp;
        tags[nt].len = xmp_len;
        nt++;
    }
    if (o->embed_metadata) {
        for (k = 0; k < sizeof TEXT_TAGS / sizeof TEXT_TAGS[0] && nt < 30; k++) {
            for (i = 0; i < img->meta.ntext; i++) {
                const pc_ctext *t = &img->meta.text[i];
                size_t q;
                bool ascii = true;
                if (!upcase_eq(t->key, t->key_len, TEXT_TAGS[k].key)) {
                    continue;
                }
                for (q = 0; q < t->value_len; q++) {
                    if ((uint8_t)t->value[q] >= 128 || t->value[q] == 0) {
                        ascii = false;
                    }
                }
                if (!ascii) {
                    continue;
                }
                tags[nt].tag = TEXT_TAGS[k].tag;
                tags[nt].ty = 2;
                tags[nt].count = t->value_len + 1; /* with its terminating NUL */
                tags[nt].data = (const uint8_t *)t->value;
                tags[nt].len = t->value_len + 1;
                nt++;
                break;
            }
        }
    }
    /* Document content, not metadata: written whenever present. */
    if (img->meta.photoshop_resources.has && img->meta.photoshop_resources.len > 0) {
        tags[nt].tag = TAG_PHOTOSHOP;
        tags[nt].ty = 1;
        tags[nt].count = img->meta.photoshop_resources.len;
        tags[nt].data = img->meta.photoshop_resources.data;
        tags[nt].len = img->meta.photoshop_resources.len;
        nt++;
    }
    if (img->meta.photoshop_layers.has && img->meta.photoshop_layers.len > 0) {
        tags[nt].tag = TAG_IMAGE_SOURCE_DATA;
        tags[nt].ty = 7;
        tags[nt].count = img->meta.photoshop_layers.len;
        tags[nt].data = img->meta.photoshop_layers.data;
        tags[nt].len = img->meta.photoshop_layers.len;
        nt++;
    }
    qsort(tags, nt, sizeof tags[0], tag_cmp);
    {
        cx_w w = {out, PC_OK, err};
        size_t inl = big ? 8 : 4;
        uint64_t at[32];
        /* out-of-line values, word aligned */
        for (i = 0; i < nt; i++) {
            if (tags[i].len > inl) {
                if (out->len & 1) {
                    cx_w_byte(&w, 0);
                }
                at[i] = out->len;
                cx_w_put(&w, tags[i].data, tags[i].len);
            }
        }
        if (out->len & 1) {
            cx_w_byte(&w, 0);
        }
        ifd_at = out->len;
        if (big) {
            uint64_t c = nt;
            cx_w_put(&w, &c, 8);
        } else {
            uint16_t c = (uint16_t)nt;
            cx_w_put(&w, &c, 2);
        }
        for (i = 0; i < nt; i++) {
            cx_w_put(&w, &tags[i].tag, 2);
            cx_w_put(&w, &tags[i].ty, 2);
            if (big) {
                cx_w_put(&w, &tags[i].count, 8);
            } else {
                uint32_t c = (uint32_t)tags[i].count;
                cx_w_put(&w, &c, 4);
            }
            if (tags[i].len <= inl) {
                uint8_t field[8] = {0};
                memcpy(field, tags[i].data, tags[i].len);
                cx_w_put(&w, field, inl);
            } else if (big) {
                cx_w_put(&w, &at[i], 8);
            } else {
                uint32_t a32 = (uint32_t)at[i];
                if (at[i] > UINT32_MAX) {
                    *overflow = true;
                }
                cx_w_put(&w, &a32, 4);
            }
        }
        if (big) {
            uint64_t z = 0;
            cx_w_put(&w, &z, 8);
        } else {
            uint32_t z = 0;
            cx_w_put(&w, &z, 4);
        }
        if (w.st != PC_OK) {
            st = w.st;
            goto done;
        }
        if (!big && ifd_at > UINT32_MAX) {
            *overflow = true;
        }
        if (*overflow) {
            goto done;
        }
        if (big) {
            memcpy(out->data + 8, &ifd_at, 8);
        } else {
            uint32_t a32 = (uint32_t)ifd_at;
            memcpy(out->data + 4, &a32, 4);
        }
    }
done:
    for (i = 0; i < nstrips; i++) {
        pc_buf_fini(&parts[i]);
    }
    free(parts);
    free(offs);
    free(cnts);
    free(offbuf);
    free(cntbuf);
    return st;
}

pc_status cx_tiff_encode(const pc_cimage *src, cx_plan p, const pc_cencode_opts *o, pc_buf *out, pc_error *err)
{
    pc_cimage tmp;
    const pc_cimage *img;
    char *xmp = NULL;
    size_t xmp_len = 0;
    const uint8_t *xp = NULL;
    bool big;
    bool overflow = false;
    uint64_t est;
    pc_buf b = PC_BUF_INIT;
    pc_status st = pc_cimage_converted(src, p.layout, p.sample, &tmp, &img, err);
    if (st != PC_OK) {
        return st;
    }
    /* TIFF writes no Orientation tag (= 1): keep the XMP from contradicting it. */
    if (o->embed_metadata && img->meta.xmp.has) {
        st = pc_upright_xmp((const char *)img->meta.xmp.data, img->meta.xmp.len, &xmp, &xmp_len, err);
        if (st != PC_OK) {
            pc_cimage_fini(&tmp);
            return st;
        }
        xp = xmp != NULL ? (const uint8_t *)xmp : img->meta.xmp.data;
        if (xmp == NULL) {
            xmp_len = img->meta.xmp.len;
        }
    }
    est = (uint64_t)img->len + (img->icc.has ? img->icc.len : 0) + xmp_len +
          (img->meta.photoshop_resources.has ? img->meta.photoshop_resources.len : 0) +
          (img->meta.photoshop_layers.has ? img->meta.photoshop_layers.len : 0) + (img->len / (1 << 20) + 1) * 16 +
          (1 << 20);
    big = o->tiff_bigtiff || est > UINT32_MAX;
    st = write_tiff(img, o, big, xp, xmp_len, &b, &overflow, err);
    if (st == PC_OK && overflow && !big) {
        st = write_tiff(img, o, true, xp, xmp_len, &b, &overflow, err);
    }
    free(xmp);
    pc_cimage_fini(&tmp);
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    pc_buf_fini(out);
    *out = b;
    return PC_OK;
}
