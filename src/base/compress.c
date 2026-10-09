#include "compress.h"

#include <limits.h>
#include <zlib.h>
#include <zstd.h>

#include "num.h"

enum { CHUNK = 64 * 1024 };

/* zlib counts in uInt; feed huge inputs in pieces. */
static uInt clamp_uint(size_t n)
{
    return n > (size_t)UINT_MAX ? UINT_MAX : (uInt)n;
}

static int window_bits(pc_zfmt fmt)
{
    switch (fmt) {
    case PC_Z_RAW: return -15;
    case PC_Z_GZIP: return 15 + 16;
    case PC_Z_ZLIB: break;
    }
    return 15;
}

pc_status pc_deflate(pc_zfmt fmt, int level, const uint8_t *data, size_t len, pc_buf *out, pc_error *err)
{
    z_stream z = {0};
    size_t fed = 0;
    size_t start = out->len;
    pc_status s = PC_OK;
    int ret;

    if (fmt == PC_Z_GZIP || level < 0 || level > 9) {
        return pc_fail(err, PC_ERR_INVALID, "unsupported deflate format or level %d", level);
    }
    ret = deflateInit2(&z, level, Z_DEFLATED, window_bits(fmt), 8, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) {
        return pc_fail(err, ret == Z_MEM_ERROR ? PC_ERR_NOMEM : PC_ERR_INVALID, "deflate init failed");
    }
    do {
        int flush;
        if (z.avail_in == 0 && fed < len) {
            z.next_in = (Bytef *)(uintptr_t)(data + fed); /* zlib's API isn't const-correct */
            z.avail_in = clamp_uint(len - fed);
            fed += z.avail_in;
        }
        flush = fed == len ? Z_FINISH : Z_NO_FLUSH;
        s = pc_buf_reserve(out, CHUNK, err);
        if (s != PC_OK) {
            goto cleanup;
        }
        z.next_out = out->data + out->len;
        z.avail_out = CHUNK;
        ret = deflate(&z, flush);
        out->len += CHUNK - z.avail_out;
        out->data[out->len] = '\0';
        if (ret == Z_STREAM_ERROR) {
            s = pc_fail(err, PC_ERR_INVALID, "deflate failed");
            goto cleanup;
        }
    } while (ret != Z_STREAM_END);
cleanup:
    (void)deflateEnd(&z);
    if (s != PC_OK) {
        pc_buf_truncate(out, start);
    }
    return s;
}

/* Shared inflate loop. Writes into *out when out != NULL, else only counts. */
static pc_status inflate_impl(pc_zfmt fmt, const uint8_t *data, size_t len, size_t max_out, pc_buf *out,
                              size_t *count, pc_decomp_info *info, pc_error *err)
{
    z_stream z = {0};
    unsigned char scratch[CHUNK];
    size_t fed = 0;
    size_t produced = 0;
    bool ended = false;
    pc_status s = PC_OK;
    int ret;

    ret = inflateInit2(&z, window_bits(fmt));
    if (ret != Z_OK) {
        return pc_fail(err, ret == Z_MEM_ERROR ? PC_ERR_NOMEM : PC_ERR_INVALID, "inflate init failed");
    }
    while (produced < max_out) {
        size_t room = max_out - produced < CHUNK ? max_out - produced : CHUNK;
        uInt before_out;
        uInt before_in;
        if (z.avail_in == 0 && fed < len) {
            z.next_in = (Bytef *)(uintptr_t)(data + fed);
            z.avail_in = clamp_uint(len - fed);
            fed += z.avail_in;
        }
        if (out != NULL) {
            s = pc_buf_reserve(out, room, err);
            if (s != PC_OK) {
                goto cleanup;
            }
            z.next_out = out->data + out->len;
        } else {
            z.next_out = scratch;
        }
        z.avail_out = (uInt)room;
        before_out = z.avail_out;
        before_in = z.avail_in;
        ret = inflate(&z, Z_NO_FLUSH);
        produced += before_out - z.avail_out;
        if (out != NULL) {
            out->len += before_out - z.avail_out;
            out->data[out->len] = '\0';
        }
        if (ret == Z_STREAM_END) {
            ended = true;
            break;
        }
        if (ret == Z_MEM_ERROR) {
            s = pc_fail(err, PC_ERR_NOMEM, "out of memory inflating");
            goto cleanup;
        }
        if (ret != Z_OK && ret != Z_BUF_ERROR) { /* Z_DATA_ERROR, Z_NEED_DICT, Z_STREAM_ERROR */
            s = pc_fail(err, PC_ERR_FORMAT, "corrupt deflate stream");
            goto cleanup;
        }
        if (z.avail_in == 0 && fed == len && before_out == z.avail_out && before_in == z.avail_in) {
            break; /* input exhausted mid-stream: truncated, not an error (like flate2) */
        }
    }
    if (info != NULL) {
        info->stream_end = ended;
        info->consumed = fed - z.avail_in;
    }
    if (count != NULL) {
        *count = produced;
    }
cleanup:
    (void)inflateEnd(&z);
    return s;
}

pc_status pc_inflate(pc_zfmt fmt, const uint8_t *data, size_t len, size_t max_out, pc_buf *out,
                     pc_decomp_info *info, pc_error *err)
{
    pc_buf tmp = PC_BUF_INIT;
    pc_status s = pc_buf_reserve(&tmp, 0, err);
    if (s == PC_OK) {
        s = inflate_impl(fmt, data, len, max_out, &tmp, NULL, info, err);
    }
    if (s != PC_OK) {
        pc_buf_fini(&tmp);
        return s;
    }
    pc_buf_fini(out);
    *out = tmp;
    return PC_OK;
}

pc_status pc_inflate_count(pc_zfmt fmt, const uint8_t *data, size_t len, size_t max_out, size_t *count,
                           pc_decomp_info *info, pc_error *err)
{
    size_t n = 0;
    pc_status s = inflate_impl(fmt, data, len, max_out, NULL, &n, info, err);
    if (s == PC_OK) {
        *count = n;
    }
    return s;
}

uint32_t pc_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    uLong c = crc;
    while (len > 0) {
        uInt n = clamp_uint(len);
        c = crc32(c, data, n);
        data += n;
        len -= n;
    }
    return (uint32_t)c;
}

pc_status pc_zstd_compress(int level, const uint8_t *data, size_t len, pc_buf *out, pc_error *err)
{
    size_t bound = ZSTD_compressBound(len);
    size_t n;
    pc_status s;
    if (ZSTD_isError(bound)) {
        return pc_fail(err, PC_ERR_LIMIT, "zstd: input too large");
    }
    s = pc_buf_reserve(out, bound, err);
    if (s != PC_OK) {
        return s;
    }
    n = ZSTD_compress(out->data + out->len, bound, data, len, level);
    if (ZSTD_isError(n)) {
        out->data[out->len] = '\0';
        return pc_fail(err, PC_ERR_INVALID, "zstd: %s", ZSTD_getErrorName(n));
    }
    out->len += n;
    out->data[out->len] = '\0';
    return PC_OK;
}

pc_status pc_zstd_decompress(const uint8_t *data, size_t len, size_t max_out, int window_log_max, pc_buf *out,
                             pc_decomp_info *info, pc_error *err)
{
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    ZSTD_inBuffer in;
    pc_buf tmp = PC_BUF_INIT;
    bool ended = false;
    pc_status s = PC_OK;
    size_t r;

    if (dctx == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "zstd: out of memory");
    }
    r = ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, window_log_max > 0 ? window_log_max : 27);
    if (ZSTD_isError(r)) {
        s = pc_fail(err, PC_ERR_INVALID, "zstd: %s", ZSTD_getErrorName(r));
        goto cleanup;
    }
    s = pc_buf_reserve(&tmp, 0, err);
    if (s != PC_OK) {
        goto cleanup;
    }
    in.src = data;
    in.size = len;
    in.pos = 0;
    while (tmp.len < max_out) {
        size_t room = max_out - tmp.len < CHUNK ? max_out - tmp.len : CHUNK;
        ZSTD_outBuffer o;
        size_t in_before = in.pos;
        s = pc_buf_reserve(&tmp, room, err);
        if (s != PC_OK) {
            goto cleanup;
        }
        o.dst = tmp.data + tmp.len;
        o.size = room;
        o.pos = 0;
        r = ZSTD_decompressStream(dctx, &o, &in);
        tmp.len += o.pos;
        tmp.data[tmp.len] = '\0';
        if (ZSTD_isError(r)) {
            s = pc_fail(err, PC_ERR_FORMAT, "zstd: %s", ZSTD_getErrorName(r));
            goto cleanup;
        }
        if (r == 0) {
            ended = true; /* one frame fully decoded and flushed */
            break;
        }
        if (in.pos == in.size && o.pos < o.size && in.pos == in_before) {
            s = pc_fail(err, PC_ERR_FORMAT, "zstd: truncated frame");
            goto cleanup;
        }
    }
    if (info != NULL) {
        info->stream_end = ended;
        info->consumed = in.pos;
    }
    pc_buf_fini(out);
    *out = tmp;
    tmp.data = NULL;
cleanup:
    pc_buf_fini(&tmp);
    ZSTD_freeDCtx(dctx);
    return s;
}
