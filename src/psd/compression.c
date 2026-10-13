#include "psd/compression.h"

#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <zlib.h>

#include "base/bytes.h"
#include "base/compress.h"
#include "base/mem.h"
#include "psd/header.h"
#include "psd/internal.h"

#define OOM_MSG "not enough memory for the decoded channel data"

size_t pc_psd_layout_row_bytes(const pc_psd_layout *l)
{
    return pc_psd_row_bytes(l->width, l->depth);
}

pc_status pc_psd_layout_total_bytes(const pc_psd_layout *l, uint64_t *out, pc_error *err)
{
    uint64_t rb = pc_psd_layout_row_bytes(l);
    uint64_t a = l->planes;
    uint64_t b = l->height;
    if (b != 0 && a > UINT64_MAX / b) {
        return psd_limit(err, "decoded size overflow");
    }
    a *= b;
    if (rb != 0 && a > UINT64_MAX / rb) {
        return psd_limit(err, "decoded size overflow");
    }
    *out = a * rb;
    return PC_OK;
}

pc_status pc_psd_layout_decoded_len(const pc_psd_layout *l, size_t *out, pc_error *err)
{
    uint64_t total;
    pc_status st = pc_psd_layout_total_bytes(l, &total, err);
    if (st != PC_OK) {
        return st;
    }
    if (total > PC_PSD_MAX_DECODED_BYTES) {
        return psd_limit(err, "decoded channel data exceeds MAX_DECODED_BYTES");
    }
    if (total > SIZE_MAX) {
        return psd_limit(err, "decoded size exceeds address space");
    }
    *out = (size_t)total;
    return PC_OK;
}

static size_t count_size(const pc_psd_layout *l)
{
    return l->version == PC_PSD_V2 ? 4 : 2;
}

static pc_status rows_of(const pc_psd_layout *l, size_t *rows, pc_error *err)
{
    if (l->height != 0 && l->planes > SIZE_MAX / l->height) {
        return psd_limit(err, "row count overflow");
    }
    *rows = l->planes * l->height;
    return PC_OK;
}

/* ---- PackBits ---- */

pc_status pc_psd_packbits_encode(const uint8_t *src, size_t n, pc_buf *out, pc_error *err)
{
    size_t i = 0;
    pc_status st = PC_OK;
    while (i < n && st == PC_OK) {
        size_t run = 1;
        size_t start;
        size_t len = 0;
        while (i + run < n && run < 128 && src[i + run] == src[i]) {
            run++;
        }
        if (run >= 2) {
            uint8_t pkt[2];
            pkt[0] = (uint8_t)(257 - run); /* (1 - run) as i8 */
            pkt[1] = src[i];
            st = pc_buf_append(out, pkt, 2, err);
            i += run;
            continue;
        }
        start = i;
        while (i < n && len < 128) {
            if (i + 2 < n && src[i] == src[i + 1] && src[i] == src[i + 2]) {
                break;
            }
            i++;
            len++;
        }
        st = pc_buf_append_byte(out, (uint8_t)(len - 1), err);
        if (st == PC_OK) {
            st = pc_buf_append(out, src + start, len, err);
        }
    }
    return st;
}

/* Decodes into dst[*pos .. *pos + expected). */
static pc_status packbits_into(const uint8_t *src, size_t n, size_t expected, uint8_t *dst, size_t *pos,
                               pc_error *err)
{
    size_t target = *pos + expected;
    size_t o = *pos;
    size_t i = 0;
    if (expected == 0 || dst == NULL) {
        return expected == 0 ? PC_OK : psd_nomem(err);
    }
    while (o < target) {
        int h;
        if (i >= n) {
            return psd_decompress(err, "PackBits row ended early");
        }
        h = (int)(int8_t)src[i++];
        if (h >= 0) {
            size_t len = (size_t)h + 1;
            if (len > n - i) {
                return psd_decompress(err, "PackBits literal truncated");
            }
            if (o + len > target) {
                return psd_decompress(err, "PackBits literal overflows row");
            }
            memcpy(dst + o, src + i, len);
            o += len;
            i += len;
        } else if (h != -128) {
            size_t len = (size_t)(1 - h);
            uint8_t b;
            if (i >= n) {
                return psd_decompress(err, "PackBits run truncated");
            }
            b = src[i++];
            if (o + len > target) {
                return psd_decompress(err, "PackBits run overflows row");
            }
            memset(dst + o, b, len);
            o += len;
        }
    }
    *pos = o;
    return PC_OK;
}

pc_status pc_psd_packbits_decode(const uint8_t *src, size_t n, size_t expected, pc_psd_bytes *out, pc_error *err)
{
    uint8_t *d = NULL;
    size_t pos = 0;
    pc_status st;
    if (expected > 0) {
        d = malloc(expected);
        if (d == NULL) {
            return psd_limit(err, OOM_MSG);
        }
    }
    st = packbits_into(src, n, expected, d, &pos, err);
    if (st != PC_OK) {
        free(d);
        return st;
    }
    pc_psd_bytes_fini(out);
    out->data = d;
    out->len = expected;
    return PC_OK;
}

/* ---- zip ---- */

pc_status pc_psd_zip_compress(const uint8_t *data, size_t n, pc_psd_bytes *out, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    pc_status st = pc_deflate(PC_Z_ZLIB, PC_Z_DEFAULT, data, n, &b, err);
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    pc_psd_bytes_fini(out);
    out->data = b.data;
    out->len = b.len;
    return PC_OK;
}

pc_status pc_psd_zip_decompress(const uint8_t *data, size_t n, size_t expected, pc_psd_bytes *out, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    pc_error e;
    pc_status st = pc_inflate(PC_Z_ZLIB, data, n, expected, &b, NULL, &e);
    if (st == PC_ERR_NOMEM) {
        return psd_limit(err, OOM_MSG);
    }
    if (st != PC_OK) {
        return psd_decompress(err, "%s", e.msg);
    }
    if (b.len < expected) {
        size_t got = b.len;
        pc_buf_fini(&b);
        return psd_decompress(err, "zlib stream produced %zu of %zu bytes", got, expected);
    }
    pc_psd_bytes_fini(out);
    out->data = b.data;
    out->len = b.len;
    return PC_OK;
}

/* Inflates a whole zlib stream (through its Adler-32 trailer). Keeps only the first `expected`
 * bytes (when `out` is non-NULL); a bounded surplus (1 MiB) tolerates padding without unbounded
 * work. Mirrors the reference's flate2::Decompress loop and its messages. */
static pc_status inflate_checked(const uint8_t *data, size_t len, uint64_t expected, pc_psd_bytes *out,
                                 pc_error *err)
{
    const uint64_t allowance = (uint64_t)1 << 20;
    uint64_t limit = expected > UINT64_MAX - allowance ? UINT64_MAX : expected + allowance;
    uint64_t want = expected == UINT64_MAX ? expected : expected + 1;
    size_t bufsize = want < 8192 ? 8192 : want > allowance ? (size_t)allowance : (size_t)want;
    uint64_t in_total = 0;
    uint64_t out_total = 0;
    pc_buf keep = PC_BUF_INIT;
    uint8_t *buf;
    z_stream z;
    pc_status st = PC_OK;
    int ret;
    if (out != NULL) {
        uint64_t hint = len > UINT64_MAX / 1032 ? UINT64_MAX : (uint64_t)len * 1032u;
        if (hint > expected) {
            hint = expected;
        }
        if (hint > SIZE_MAX || pc_buf_reserve(&keep, (size_t)hint, NULL) != PC_OK) {
            return psd_limit(err, OOM_MSG);
        }
    }
    buf = malloc(bufsize);
    if (buf == NULL) {
        pc_buf_fini(&keep);
        return psd_limit(err, OOM_MSG);
    }
    memset(&z, 0, sizeof z);
    if (inflateInit(&z) != Z_OK) {
        free(buf);
        pc_buf_fini(&keep);
        return psd_limit(err, OOM_MSG);
    }
    for (;;) {
        uint64_t before_in = in_total;
        uint64_t before_out = out_total;
        size_t rest = len - (size_t)in_total;
        uint64_t produced;
        uInt avail;
        z.next_in = (Bytef *)(uintptr_t)(data + in_total);
        z.avail_in = rest > UINT_MAX ? UINT_MAX : (uInt)rest;
        avail = z.avail_in;
        z.next_out = buf;
        z.avail_out = (uInt)bufsize;
        ret = inflate(&z, Z_NO_FLUSH);
        in_total += avail - z.avail_in;
        produced = bufsize - z.avail_out;
        out_total += produced;
        if (ret == Z_MEM_ERROR) {
            st = psd_limit(err, OOM_MSG);
            break;
        }
        if (ret == Z_NEED_DICT) {
            st = psd_decompress(err, "deflate decompression error: requires a dictionary");
            break;
        }
        if (ret != Z_OK && ret != Z_BUF_ERROR && ret != Z_STREAM_END) {
            st = psd_decompress(err, "deflate decompression error");
            break;
        }
        if (out_total > limit) {
            st = psd_limit(err, "zlib output exceeds the declared size and padding allowance");
            break;
        }
        if (out != NULL) {
            uint64_t room = expected - keep.len;
            size_t n = (size_t)(produced < room ? produced : room);
            st = pc_buf_append(&keep, buf, n, NULL);
            if (st != PC_OK) {
                st = psd_limit(err, OOM_MSG);
                break;
            }
        }
        if (ret == Z_STREAM_END) {
            if (out_total < expected) {
                st = psd_decompress(err, "zlib stream produced %llu of %llu bytes",
                                    (unsigned long long)out_total, (unsigned long long)expected);
            }
            break;
        }
        if (before_in == in_total && before_out == out_total) {
            st = psd_decompress(err, "incomplete zlib stream");
            break;
        }
    }
    (void)inflateEnd(&z);
    free(buf);
    if (st != PC_OK || out == NULL) {
        pc_buf_fini(&keep);
        return st;
    }
    pc_psd_bytes_fini(out);
    out->data = keep.data;
    out->len = keep.len;
    return PC_OK;
}

/* ---- prediction ---- */

pc_status pc_psd_predict(uint8_t *buf, size_t len, const pc_psd_layout *l, pc_error *err)
{
    size_t rb = pc_psd_layout_row_bytes(l);
    size_t w = l->width;
    size_t off;
    uint8_t *scratch = NULL;
    if (rb == 0) {
        return PC_OK;
    }
    if (l->depth != 1 && l->depth != 8 && l->depth != 16 && l->depth != 32) {
        return len >= rb ? psd_unsupported(err, "prediction for depth %u", (unsigned)l->depth) : PC_OK;
    }
    if (l->depth == 32 && len >= rb) {
        scratch = malloc(rb);
        if (scratch == NULL) {
            return psd_nomem(err);
        }
    }
    for (off = 0; off + rb <= len; off += rb) {
        uint8_t *row = buf + off;
        size_t i;
        size_t k;
        switch (l->depth) {
        case 1:
        case 8:
            for (i = rb - 1; i >= 1; i--) {
                row[i] = (uint8_t)(row[i] - row[i - 1]);
            }
            break;
        case 16:
            for (i = w - 1; i >= 1 && w > 0; i--) {
                uint16_t a = pc_load_be16(row + 2 * i);
                uint16_t b = pc_load_be16(row + 2 * i - 2);
                pc_store_be16(row + 2 * i, (uint16_t)(a - b));
            }
            break;
        default:
            for (i = 0; i < w; i++) {
                for (k = 0; k < 4; k++) {
                    scratch[k * w + i] = row[4 * i + k];
                }
            }
            for (i = rb - 1; i >= 1; i--) {
                scratch[i] = (uint8_t)(scratch[i] - scratch[i - 1]);
            }
            memcpy(row, scratch, rb);
            break;
        }
    }
    free(scratch);
    return PC_OK;
}

pc_status pc_psd_unpredict(uint8_t *buf, size_t len, const pc_psd_layout *l, pc_error *err)
{
    size_t rb = pc_psd_layout_row_bytes(l);
    size_t w = l->width;
    size_t off;
    uint8_t *scratch = NULL;
    if (rb == 0) {
        return PC_OK;
    }
    if (l->depth != 1 && l->depth != 8 && l->depth != 16 && l->depth != 32) {
        return len >= rb ? psd_unsupported(err, "prediction for depth %u", (unsigned)l->depth) : PC_OK;
    }
    if (l->depth == 32 && len >= rb) {
        scratch = malloc(rb);
        if (scratch == NULL) {
            return psd_nomem(err);
        }
    }
    for (off = 0; off + rb <= len; off += rb) {
        uint8_t *row = buf + off;
        size_t i;
        size_t k;
        switch (l->depth) {
        case 1:
        case 8:
            for (i = 1; i < rb; i++) {
                row[i] = (uint8_t)(row[i] + row[i - 1]);
            }
            break;
        case 16:
            for (i = 1; i < w; i++) {
                uint16_t a = pc_load_be16(row + 2 * i);
                uint16_t b = pc_load_be16(row + 2 * i - 2);
                pc_store_be16(row + 2 * i, (uint16_t)(a + b));
            }
            break;
        default:
            for (i = 1; i < rb; i++) {
                row[i] = (uint8_t)(row[i] + row[i - 1]);
            }
            memcpy(scratch, row, rb);
            for (i = 0; i < w; i++) {
                for (k = 0; k < 4; k++) {
                    row[4 * i + k] = scratch[k * w + i];
                }
            }
            break;
        }
    }
    free(scratch);
    return PC_OK;
}

/* ---- planes ---- */

static pc_status read_count(psd_rd *r, const pc_psd_layout *l, size_t *c, pc_error *err)
{
    pc_status st;
    if (l->version == PC_PSD_V2) {
        uint32_t v;
        st = psd_u32(r, &v, err);
        if (st == PC_OK) {
            *c = v;
        }
    } else {
        uint16_t v;
        st = psd_u16(r, &v, err);
        if (st == PC_OK) {
            *c = v;
        }
    }
    return st;
}

static pc_status decode_rle(const uint8_t *data, size_t len, const pc_psd_layout *l, size_t total, uint8_t **out,
                            pc_error *err)
{
    psd_rd r = psd_rd_make(data, len);
    size_t rows;
    size_t rb = pc_psd_layout_row_bytes(l);
    size_t *counts = NULL;
    uint8_t *d = NULL;
    size_t pos = 0;
    size_t i;
    pc_status st;
    CK(rows_of(l, &rows, err));
    CK(psd_check_count(&r, rows, count_size(l), err));
    if (rows > 0) {
        counts = pc_malloc_array(rows, sizeof *counts);
        if (counts == NULL) {
            st = psd_limit(err, OOM_MSG);
            goto fail;
        }
    }
    for (i = 0; i < rows; i++) {
        CK(read_count(&r, l, &counts[i], err));
    }
    /* PackBits expands at most 64x (2 input bytes -> 128 output bytes). */
    if ((uint64_t)total > (uint64_t)psd_remaining(&r) * 64u) {
        st = psd_decompress(err, "RLE data too short for declared size");
        goto fail;
    }
    if (total > 0) {
        d = malloc(total);
        if (d == NULL) {
            st = psd_limit(err, OOM_MSG);
            goto fail;
        }
    }
    for (i = 0; i < rows; i++) {
        const uint8_t *row;
        CK(psd_bytes(&r, counts[i], &row, err));
        CK(packbits_into(row, counts[i], rb, d, &pos, err));
    }
    free(counts);
    *out = d;
    return PC_OK;
fail:
    free(counts);
    free(d);
    return st;
}

pc_status pc_psd_decode_planes(uint16_t compression, const uint8_t *data, size_t len, const pc_psd_layout *l,
                               pc_psd_bytes *out, pc_error *err)
{
    size_t total;
    uint8_t *d = NULL;
    pc_psd_bytes z = {NULL, 0};
    pc_status st = pc_psd_layout_decoded_len(l, &total, err);
    if (st != PC_OK) {
        return st;
    }
    switch (compression) {
    case PC_PSD_RAW:
        if (len < total) {
            return psd_eof(err, len, total - len);
        }
        if (total > 0) {
            d = malloc(total);
            if (d == NULL) {
                return psd_limit(err, "not enough memory for the decoded channel data");
            }
            memcpy(d, data, total);
        }
        break;
    case PC_PSD_RLE:
        st = decode_rle(data, len, l, total, &d, err);
        if (st != PC_OK) {
            return st;
        }
        break;
    case PC_PSD_ZIP:
    case PC_PSD_ZIP_PREDICT:
        st = inflate_checked(data, len, total, &z, err);
        if (st == PC_OK && compression == PC_PSD_ZIP_PREDICT) {
            st = pc_psd_unpredict(z.data, z.len, l, err);
        }
        if (st != PC_OK) {
            pc_psd_bytes_fini(&z);
            return st;
        }
        pc_psd_bytes_fini(out);
        *out = z;
        return PC_OK;
    default:
        return psd_unsupported(err, "compression method %u", (unsigned)compression);
    }
    pc_psd_bytes_fini(out);
    out->data = d;
    out->len = total;
    return PC_OK;
}

static pc_status encode_rle(const uint8_t *dec, const pc_psd_layout *l, pc_psd_bytes *out, pc_error *err)
{
    size_t rows;
    size_t rb = pc_psd_layout_row_bytes(l);
    size_t cs = count_size(l);
    size_t tbl;
    size_t row;
    pc_buf b = PC_BUF_INIT;
    pc_status st;
    CK(rows_of(l, &rows, err));
    if (rows > SIZE_MAX / cs) {
        st = psd_limit(err, "row count overflow");
        goto fail;
    }
    tbl = rows * cs;
    if (tbl == 0) { /* no rows: nothing to encode */
        pc_psd_bytes_fini(out);
        return PC_OK;
    }
    CK(pc_buf_reserve(&b, tbl, err));
    memset(b.data, 0, tbl);
    b.len = tbl;
    for (row = 0; row < rows; row++) {
        size_t start = b.len;
        size_t n;
        CK(pc_psd_packbits_encode(rb > 0 ? dec + row * rb : dec, rb, &b, err));
        n = b.len - start;
        if (cs == 2) {
            if (n > 0xFFFFu) {
                st = psd_limit(err, "RLE row exceeds 65535 bytes (use PSB)");
                goto fail;
            }
            pc_store_be16(b.data + row * 2, (uint16_t)n);
        } else {
            if (n > 0xFFFFFFFFu) {
                st = psd_limit(err, "RLE row too large");
                goto fail;
            }
            pc_store_be32(b.data + row * 4, (uint32_t)n);
        }
    }
    pc_psd_bytes_fini(out);
    out->data = b.data;
    out->len = b.len;
    return PC_OK;
fail:
    pc_buf_fini(&b);
    return st;
}

pc_status pc_psd_encode_planes(uint16_t compression, const uint8_t *decoded, size_t len, const pc_psd_layout *l,
                               pc_psd_bytes *out, pc_error *err)
{
    size_t total;
    pc_psd_bytes tmp = {NULL, 0};
    pc_status st = pc_psd_layout_decoded_len(l, &total, err);
    if (st != PC_OK) {
        return st;
    }
    if (len != total) {
        return psd_misuse(err, "decoded buffer has %zu bytes, layout requires %zu", len, total);
    }
    switch (compression) {
    case PC_PSD_RAW:
        st = pc_psd_bytes_set(&tmp, decoded, len, err);
        break;
    case PC_PSD_RLE:
        st = encode_rle(decoded, l, &tmp, err);
        break;
    case PC_PSD_ZIP:
        st = pc_psd_zip_compress(decoded, len, &tmp, err);
        break;
    case PC_PSD_ZIP_PREDICT: {
        pc_psd_bytes copy = {NULL, 0};
        st = pc_psd_bytes_set(&copy, decoded, len, err);
        if (st == PC_OK) {
            st = pc_psd_predict(copy.data, copy.len, l, err);
        }
        if (st == PC_OK) {
            st = pc_psd_zip_compress(copy.data, copy.len, &tmp, err);
        }
        pc_psd_bytes_fini(&copy);
        break;
    }
    default:
        return psd_unsupported(err, "compression method %u", (unsigned)compression);
    }
    if (st != PC_OK) {
        pc_psd_bytes_fini(&tmp);
        return st;
    }
    pc_psd_bytes_fini(out);
    *out = tmp;
    return PC_OK;
}

pc_status pc_psd_validate_planes(uint16_t compression, const uint8_t *data, size_t len, const pc_psd_layout *l,
                                 pc_error *err)
{
    uint64_t total;
    pc_status st = pc_psd_layout_total_bytes(l, &total, err);
    if (st != PC_OK) {
        return st;
    }
    switch (compression) {
    case PC_PSD_RAW:
        if ((uint64_t)len < total) {
            uint64_t miss = total - len;
            return psd_eof(err, len, miss > SIZE_MAX ? SIZE_MAX : (size_t)miss);
        }
        return PC_OK;
    case PC_PSD_RLE: {
        psd_rd r = psd_rd_make(data, len);
        size_t rows;
        size_t i;
        uint64_t sum = 0;
        st = rows_of(l, &rows, err);
        if (st == PC_OK) {
            st = psd_check_count(&r, rows, count_size(l), err);
        }
        for (i = 0; st == PC_OK && i < rows; i++) {
            size_t c;
            st = read_count(&r, l, &c, err);
            if (st == PC_OK) {
                sum += c;
            }
        }
        if (st != PC_OK) {
            return st;
        }
        if (sum > psd_remaining(&r)) {
            uint64_t miss = sum - psd_remaining(&r);
            return psd_eof(err, len, miss > SIZE_MAX ? SIZE_MAX : (size_t)miss);
        }
        return PC_OK;
    }
    case PC_PSD_ZIP:
    case PC_PSD_ZIP_PREDICT:
        /* #2926: a stream declaring more than MAX_DECODED_BYTES is kept verbatim, not inflated:
         * decoding refuses that layout anyway, and a layered file whose merged composite is too
         * large still opens. */
        if (total > PC_PSD_MAX_DECODED_BYTES) {
            return PC_OK;
        }
        return inflate_checked(data, len, total, NULL, err);
    default:
        return PC_OK;
    }
}
