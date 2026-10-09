#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <zstd.h>

#include "base/compress.h"
#include "base/mem.h"
#include "harness.h"

enum { N = 300000 };

static uint8_t *make_data(void)
{
    uint8_t *d = pc_malloc(N);
    size_t i;
    if (d != NULL) {
        for (i = 0; i < N; i++) {
            d[i] = (uint8_t)((i * i) % 251 ^ (i >> 7));
        }
    }
    return d;
}

static void test_crc32(void)
{
    CHECK_EQ_UINT(pc_crc32(0, (const uint8_t *)"123456789", 9), 0xCBF43926u);
    CHECK_EQ_UINT(pc_crc32(pc_crc32(0, (const uint8_t *)"1234", 4), (const uint8_t *)"56789", 5), 0xCBF43926u);
    CHECK_EQ_UINT(pc_crc32(0, NULL, 0), 0);
}

static void test_deflate_roundtrip(void)
{
    static const pc_zfmt fmts[] = {PC_Z_ZLIB, PC_Z_RAW};
    static const int levels[] = {0, PC_Z_FAST, PC_Z_DEFAULT, PC_Z_BEST};
    uint8_t *data = make_data();
    size_t f;
    size_t l;
    CHECK(data != NULL);
    for (f = 0; data != NULL && f < 2; f++) {
        for (l = 0; l < 4; l++) {
            pc_buf z = PC_BUF_INIT;
            pc_buf back = PC_BUF_INIT;
            pc_decomp_info info;
            size_t count = 0;
            CHECK_EQ_INT(pc_deflate(fmts[f], levels[l], data, N, &z, NULL), PC_OK);
            CHECK_EQ_INT(pc_inflate(fmts[f], z.data, z.len, N + 10, &back, &info, NULL), PC_OK);
            CHECK_EQ_UINT(back.len, N);
            CHECK(back.len == N && memcmp(back.data, data, N) == 0);
            CHECK(info.stream_end);
            CHECK_EQ_UINT(info.consumed, z.len);
            CHECK_EQ_INT(pc_inflate_count(fmts[f], z.data, z.len, SIZE_MAX, &count, NULL, NULL), PC_OK);
            CHECK_EQ_UINT(count, N);
            pc_buf_fini(&z);
            pc_buf_fini(&back);
        }
    }
    free(data);
}

static void test_inflate_take_semantics(void)
{
    uint8_t *data = make_data();
    pc_buf z = PC_BUF_INIT;
    pc_buf out = PC_BUF_INIT;
    pc_decomp_info info;
    pc_error e;
    CHECK(data != NULL);
    if (data == NULL) {
        return;
    }
    CHECK_EQ_INT(pc_deflate(PC_Z_ZLIB, PC_Z_DEFAULT, data, N, &z, NULL), PC_OK);

    /* take(n): stops at the limit, no error. */
    CHECK_EQ_INT(pc_inflate(PC_Z_ZLIB, z.data, z.len, 1000, &out, &info, NULL), PC_OK);
    CHECK_EQ_UINT(out.len, 1000);
    CHECK(!info.stream_end);
    CHECK(memcmp(out.data, data, 1000) == 0);
    CHECK_EQ_INT(pc_inflate(PC_Z_ZLIB, z.data, z.len, 0, &out, &info, NULL), PC_OK);
    CHECK_EQ_UINT(out.len, 0);

    /* Truncated input: a short result, not an error (flate2 read semantics). */
    CHECK_EQ_INT(pc_inflate(PC_Z_ZLIB, z.data, z.len / 2, N, &out, &info, NULL), PC_OK);
    CHECK(out.len > 0 && out.len < N);
    CHECK(!info.stream_end);

    /* Trailing garbage after the stream is ignored. */
    {
        pc_buf zz = PC_BUF_INIT;
        CHECK_EQ_INT(pc_buf_append(&zz, z.data, z.len, NULL), PC_OK);
        CHECK_EQ_INT(pc_buf_append_str(&zz, "GARBAGE", NULL), PC_OK);
        CHECK_EQ_INT(pc_inflate(PC_Z_ZLIB, zz.data, zz.len, N, &out, &info, NULL), PC_OK);
        CHECK_EQ_UINT(out.len, N);
        CHECK(info.stream_end);
        CHECK_EQ_UINT(info.consumed, z.len);
        pc_buf_fini(&zz);
    }

    /* Corrupt data and a bad Adler-32 are errors; the output buffer is left alone. */
    CHECK_EQ_INT(pc_buf_append_str(&out, "", NULL), PC_OK);
    pc_buf_truncate(&out, 0);
    CHECK_EQ_INT(pc_buf_append_str(&out, "keep", NULL), PC_OK);
    z.data[z.len - 1] ^= 0xFF;
    CHECK_EQ_INT(pc_inflate(PC_Z_ZLIB, z.data, z.len, N + 1, &out, &info, &e), PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "corrupt deflate stream");
    CHECK_EQ_STR(pc_buf_cstr(&out), "keep");
    CHECK_EQ_INT(pc_inflate(PC_Z_ZLIB, (const uint8_t *)"\x00\x01garbage", 9, 100, &out, NULL, &e), PC_ERR_FORMAT);
    CHECK_EQ_INT(pc_inflate(PC_Z_ZLIB, NULL, 0, 100, &out, &info, &e), PC_OK);
    CHECK_EQ_UINT(out.len, 0);
    CHECK(!info.stream_end);
    CHECK_EQ_INT(pc_deflate(PC_Z_GZIP, 6, data, 10, &out, &e), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_deflate(PC_Z_ZLIB, 10, data, 10, &out, &e), PC_ERR_INVALID);
    pc_buf_fini(&z);
    pc_buf_fini(&out);
    free(data);
}

static void test_gzip_decode(void)
{
    static const char text[] = "the words list ships gzip-compressed";
    uint8_t gz[256];
    z_stream s;
    pc_buf out = PC_BUF_INIT;
    memset(&s, 0, sizeof s);
    CHECK_EQ_INT(deflateInit2(&s, 9, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY), Z_OK);
    s.next_in = (Bytef *)(uintptr_t)text;
    s.avail_in = (uInt)(sizeof text - 1);
    s.next_out = gz;
    s.avail_out = sizeof gz;
    CHECK_EQ_INT(deflate(&s, Z_FINISH), Z_STREAM_END);
    CHECK_EQ_INT(pc_inflate(PC_Z_GZIP, gz, s.total_out, 1000, &out, NULL, NULL), PC_OK);
    CHECK_EQ_STR(pc_buf_cstr(&out), text);
    (void)deflateEnd(&s);
    pc_buf_fini(&out);
}

static void test_zstd(void)
{
    uint8_t *data = make_data();
    pc_buf z = PC_BUF_INIT;
    pc_buf two = PC_BUF_INIT;
    pc_buf out = PC_BUF_INIT;
    pc_decomp_info info;
    pc_error e;
    CHECK(data != NULL);
    if (data == NULL) {
        return;
    }
    CHECK_EQ_INT(pc_zstd_compress(1, data, N, &z, NULL), PC_OK);
    CHECK(z.len < N);
    CHECK_EQ_INT(pc_zstd_decompress(z.data, z.len, N + 1, 0, &out, &info, NULL), PC_OK);
    CHECK_EQ_UINT(out.len, N);
    CHECK(out.len == N && memcmp(out.data, data, N) == 0);
    CHECK(info.stream_end);
    CHECK_EQ_UINT(info.consumed, z.len);

    /* Two frames back to back: one call decodes only the first (ruzstd StreamingDecoder). */
    CHECK_EQ_INT(pc_buf_append(&two, z.data, z.len, NULL), PC_OK);
    CHECK_EQ_INT(pc_zstd_compress(3, (const uint8_t *)"second", 6, &two, NULL), PC_OK);
    CHECK_EQ_INT(pc_zstd_decompress(two.data, two.len, SIZE_MAX, 0, &out, &info, NULL), PC_OK);
    CHECK_EQ_UINT(out.len, N);
    CHECK_EQ_UINT(info.consumed, z.len);
    CHECK_EQ_INT(pc_zstd_decompress(two.data + info.consumed, two.len - info.consumed, 100, 0, &out, &info, NULL),
                 PC_OK);
    CHECK_EQ_STR(pc_buf_cstr(&out), "second");

    /* take(n) stops early without error; a truncated frame is an error. */
    CHECK_EQ_INT(pc_zstd_decompress(z.data, z.len, 500, 0, &out, &info, NULL), PC_OK);
    CHECK_EQ_UINT(out.len, 500);
    CHECK(!info.stream_end);
    CHECK_EQ_INT(pc_buf_append_str(&out, "", NULL), PC_OK);
    pc_buf_truncate(&out, 0);
    CHECK_EQ_INT(pc_buf_append_str(&out, "keep", NULL), PC_OK);
    CHECK_EQ_INT(pc_zstd_decompress(z.data, z.len - 5, N + 1, 0, &out, &info, &e), PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "zstd: truncated frame");
    CHECK_EQ_STR(pc_buf_cstr(&out), "keep");
    CHECK_EQ_INT(pc_zstd_decompress(NULL, 0, 10, 0, &out, &info, &e), PC_ERR_FORMAT);
    CHECK_EQ_INT(pc_zstd_decompress((const uint8_t *)"not zstd at all", 15, 10, 0, &out, &info, &e), PC_ERR_FORMAT);
    CHECK_CONTAINS(e.msg, "zstd: ");
    CHECK_EQ_STR(pc_buf_cstr(&out), "keep");
    pc_buf_fini(&z);
    pc_buf_fini(&two);
    pc_buf_fini(&out);
    free(data);
}

static void test_zstd_window_limit(void)
{
    enum { BIG = 4 << 20 };
    uint8_t *data = pc_calloc_array(BIG, 1);
    ZSTD_CCtx *c = ZSTD_createCCtx();
    pc_buf z = PC_BUF_INIT;
    pc_buf out = PC_BUF_INIT;
    pc_error e;
    size_t i;
    size_t n;
    CHECK(data != NULL && c != NULL);
    if (data == NULL || c == NULL) {
        free(data);
        ZSTD_freeCCtx(c);
        return;
    }
    for (i = 0; i < BIG; i++) {
        data[i] = (uint8_t)(i * 2654435761u >> 24);
    }
    (void)ZSTD_CCtx_setParameter(c, ZSTD_c_windowLog, 22);
    (void)ZSTD_CCtx_setParameter(c, ZSTD_c_contentSizeFlag, 0);
    CHECK_EQ_INT(pc_buf_reserve(&z, ZSTD_compressBound(BIG), NULL), PC_OK);
    n = ZSTD_compress2(c, z.data, ZSTD_compressBound(BIG), data, BIG);
    CHECK(!ZSTD_isError(n));
    z.len = n;
    CHECK_EQ_INT(pc_zstd_decompress(z.data, z.len, BIG, 22, &out, NULL, NULL), PC_OK);
    CHECK_EQ_UINT(out.len, BIG);
    CHECK_EQ_INT(pc_zstd_decompress(z.data, z.len, BIG, 20, &out, NULL, &e), PC_ERR_FORMAT);
    CHECK_CONTAINS(e.msg, "zstd: ");
    ZSTD_freeCCtx(c);
    pc_buf_fini(&z);
    pc_buf_fini(&out);
    free(data);
}

int main(int argc, char **argv)
{
    RUN(test_crc32);
    RUN(test_deflate_roundtrip);
    RUN(test_inflate_take_semantics);
    RUN(test_gzip_decode);
    RUN(test_zstd);
    RUN(test_zstd_window_limit);
    return pc_t_finish();
}
