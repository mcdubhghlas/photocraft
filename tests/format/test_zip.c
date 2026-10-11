/* The ZIP container (crates/format/src/zip.rs tests, plus hostile-input checks). Byte-level parity
 * with the reference is in test_parity.c. */
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/compress.h"
#include "format/format.h"
#include "harness.h"

static void build(pc_buf *out, const char *const *names, const char *const *data, size_t n)
{
    pc_zip_writer w;
    pc_zip_writer_init(&w);
    for (size_t i = 0; i < n; i++) {
        CHECK(pc_zip_writer_add(&w, names[i], strlen(names[i]), (const uint8_t *)data[i], strlen(data[i]),
                                NULL) == PC_OK);
    }
    CHECK(pc_zip_writer_finish(&w, out, NULL) == PC_OK);
    pc_zip_writer_fini(&w);
}

static void test_crc_known_value(void)
{
    CHECK_EQ_UINT(pc_zip_crc32((const uint8_t *)"123456789", 9), 0xCBF43926u);
    CHECK_EQ_UINT(pc_zip_crc32(NULL, 0), 0u);
}

static void test_roundtrip(void)
{
    const uint8_t four[4] = {0, 1, 2, 3};
    pc_zip_writer w;
    pc_zip_reader r;
    pc_buf bytes = PC_BUF_INIT;
    pc_buf d = PC_BUF_INIT;
    pc_error e;
    pc_zip_writer_init(&w);
    CHECK(pc_zip_writer_add(&w, "a.txt", 5, (const uint8_t *)"hello", 5, NULL) == PC_OK);
    CHECK(pc_zip_writer_add(&w, "dir/b.bin", 9, four, 4, NULL) == PC_OK);
    CHECK(pc_zip_writer_add(&w, "empty", 5, NULL, 0, NULL) == PC_OK);
    CHECK(pc_zip_writer_finish(&w, &bytes, NULL) == PC_OK);
    CHECK_EQ_UINT(w.count, 0); /* reusable */
    pc_zip_writer_fini(&w);
    CHECK(pc_zip_reader_open(&r, bytes.data, bytes.len, NULL) == PC_OK);
    CHECK_EQ_UINT(r.count, 3);
    CHECK(pc_zip_read_by_name(&r, "a.txt", 100, &d, NULL) == PC_OK);
    CHECK(d.len == 5 && memcmp(d.data, "hello", 5) == 0);
    CHECK(pc_zip_read_by_name(&r, "dir/b.bin", 100, &d, NULL) == PC_OK);
    CHECK(d.len == 4 && memcmp(d.data, four, 4) == 0);
    CHECK(pc_zip_read_by_name(&r, "empty", 0, &d, NULL) == PC_OK);
    CHECK_EQ_UINT(d.len, 0);
    CHECK(pc_zip_read_by_name(&r, "a.txt", 3, &d, &e) == PC_ERR_LIMIT);
    CHECK_EQ_STR(e.msg, "limit exceeded: zip entry `a.txt` is 5 bytes (max 3)");
    CHECK(pc_format_error_kind(PC_ERR_LIMIT, &e) == PC_FORMAT_ERR_LIMIT);
    CHECK(pc_zip_read_by_name(&r, "nope", 100, &d, &e) == PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "corrupt .pcraft bundle: missing `nope`");
    pc_zip_reader_close(&r);
    pc_buf_fini(&bytes);
    pc_buf_fini(&d);
}

static void test_crc_mismatch_detected(void)
{
    static const char *const n[1] = {"a"};
    static const char *const v[1] = {"hello"};
    pc_buf bytes = PC_BUF_INIT;
    pc_buf d = PC_BUF_INIT;
    pc_zip_reader r;
    pc_error e;
    uint8_t *pos;
    build(&bytes, n, v, 1);
    pos = memchr(bytes.data, 'h', bytes.len);
    CHECK(pos != NULL);
    if (pos != NULL) {
        *pos = 'j';
    }
    CHECK(pc_zip_reader_open(&r, bytes.data, bytes.len, NULL) == PC_OK);
    CHECK(pc_zip_read_by_name(&r, "a", 100, &d, &e) == PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "corrupt .pcraft bundle: zip entry `a` failed its CRC check");
    pc_zip_reader_close(&r);
    pc_buf_fini(&bytes);
    pc_buf_fini(&d);
}

/* The reference's u16_at/u32_at reject offsets that overflow; here every offset read from the
 * archive is checked the same way (a local header offset of 0xffffffff, as in the parity vectors,
 * and offsets near the end). */
static void test_offset_access_rejects_overflow(void)
{
    static const char *const n[1] = {"a"};
    static const char *const v[1] = {"x"};
    pc_buf bytes = PC_BUF_INIT;
    pc_zip_reader r;
    pc_error e;
    size_t cd;
    build(&bytes, n, v, 1);
    cd = pc_load_le32(bytes.data + bytes.len - 6);
    pc_store_le32(bytes.data + cd + 42, 0xffffffffu);
    CHECK(pc_zip_reader_open(&r, bytes.data, bytes.len, &e) == PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "corrupt .pcraft bundle: truncated zip");
    pc_store_le32(bytes.data + cd + 42, (uint32_t)(bytes.len - 4)); /* sig would run off the end */
    CHECK(pc_zip_reader_open(&r, bytes.data, bytes.len, &e) == PC_ERR_FORMAT);
    pc_buf_fini(&bytes);
}

/* The end record is found within 65535 + 22 bytes of the end (the longest comment), not further. */
static void test_eocd_search_window(void)
{
    static const char *const n[1] = {"manifest.json"};
    static const char *const v[1] = {"{}"};
    pc_buf bytes = PC_BUF_INIT;
    pc_buf big = PC_BUF_INIT;
    pc_zip_reader r;
    pc_error e;
    build(&bytes, n, v, 1);
    /* A maximal comment: still found. */
    (void)pc_buf_append(&big, bytes.data, bytes.len, NULL);
    pc_store_le16(big.data + big.len - 2, 65535);
    for (size_t i = 0; i < 65535; i++) {
        (void)pc_buf_append_byte(&big, 'c', NULL);
    }
    CHECK(pc_zip_reader_open(&r, big.data, big.len, NULL) == PC_OK);
    CHECK_EQ_UINT(r.count, 1);
    pc_zip_reader_close(&r);
    CHECK(pc_format_is_pcraft(big.data, big.len));
    /* One byte more and the record is out of reach. */
    (void)pc_buf_append_byte(&big, 'c', NULL);
    CHECK(pc_zip_reader_open(&r, big.data, big.len, &e) == PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "corrupt .pcraft bundle: zip end-of-central-directory not found");
    CHECK(pc_format_is_pcraft(big.data, big.len)); /* the first local header still names it */
    pc_buf_fini(&bytes);
    pc_buf_fini(&big);
}

static void test_writer_refusals_leave_it_unchanged(void)
{
    pc_zip_writer w;
    pc_buf a = PC_BUF_INIT;
    pc_buf b = PC_BUF_INIT;
    pc_error e;
    char *name = malloc(65536);
    memset(name, 'n', 65536);
    pc_zip_writer_init(&w);
    CHECK(pc_zip_writer_add(&w, "x", 1, (const uint8_t *)"1", 1, NULL) == PC_OK);
    CHECK(pc_zip_writer_add(&w, name, 65536, NULL, 0, &e) == PC_ERR_UNSUPPORTED);
    CHECK_EQ_STR(e.msg, "unsupported .pcraft feature: zip entry name over 65535 bytes");
    CHECK(pc_zip_writer_add(&w, name, 65535, NULL, 0, NULL) == PC_OK); /* the longest name fits */
    CHECK(pc_zip_writer_finish(&w, &a, NULL) == PC_OK);
    pc_zip_writer_fini(&w);
    pc_zip_writer_init(&w);
    CHECK(pc_zip_writer_add(&w, "x", 1, (const uint8_t *)"1", 1, NULL) == PC_OK);
    CHECK(pc_zip_writer_add(&w, name, 65535, NULL, 0, NULL) == PC_OK);
    CHECK(pc_zip_writer_finish(&w, &b, NULL) == PC_OK);
    CHECK(a.len == b.len && memcmp(a.data, b.data, a.len) == 0);
    {
        pc_zip_reader r;
        CHECK(pc_zip_reader_open(&r, a.data, a.len, NULL) == PC_OK);
        CHECK_EQ_UINT(r.count, 2);
        CHECK_EQ_UINT(r.entries[1].name.len, 65535);
        pc_zip_reader_close(&r);
    }
    pc_zip_writer_fini(&w);
    free(name);
    pc_buf_fini(&a);
    pc_buf_fini(&b);
}

/* Hand-built DEFLATE entry. */
static void deflate_archive(pc_buf *out, const char *name, const uint8_t *plain, size_t n, uint32_t declared)
{
    pc_buf comp = PC_BUF_INIT;
    uint8_t h[46];
    size_t nl = strlen(name);
    size_t cd;
    CHECK(pc_deflate(PC_Z_RAW, 6, plain, n, &comp, NULL) == PC_OK);
    pc_buf_truncate(out, 0);
    memset(h, 0, sizeof h);
    pc_store_le32(h, 0x04034b50u);
    pc_store_le16(h + 4, 20);
    pc_store_le16(h + 8, 8);
    pc_store_le32(h + 14, pc_zip_crc32(plain, n));
    pc_store_le32(h + 18, (uint32_t)comp.len);
    pc_store_le32(h + 22, declared);
    pc_store_le16(h + 26, (uint16_t)nl);
    (void)pc_buf_append(out, h, 30, NULL);
    (void)pc_buf_append(out, name, nl, NULL);
    (void)pc_buf_append(out, comp.data, comp.len, NULL);
    cd = out->len;
    memset(h, 0, sizeof h);
    pc_store_le32(h, 0x02014b50u);
    pc_store_le16(h + 4, 20);
    pc_store_le16(h + 6, 20);
    pc_store_le16(h + 10, 8);
    pc_store_le32(h + 16, pc_zip_crc32(plain, n));
    pc_store_le32(h + 20, (uint32_t)comp.len);
    pc_store_le32(h + 24, declared);
    pc_store_le16(h + 28, (uint16_t)nl);
    (void)pc_buf_append(out, h, 46, NULL);
    (void)pc_buf_append(out, name, nl, NULL);
    memset(h, 0, 22);
    pc_store_le32(h, 0x06054b50u);
    pc_store_le16(h + 8, 1);
    pc_store_le16(h + 10, 1);
    pc_store_le32(h + 12, (uint32_t)(46 + nl));
    pc_store_le32(h + 16, (uint32_t)cd);
    (void)pc_buf_append(out, h, 22, NULL);
    pc_buf_fini(&comp);
}

/* A DEFLATE bomb: the declared size bounds the work, and output past max is never kept. */
static void test_deflate_bomb_is_bounded(void)
{
    size_t n = (size_t)8 << 20;
    uint8_t *zeros = calloc(n, 1);
    pc_buf z = PC_BUF_INIT;
    pc_buf d = PC_BUF_INIT;
    pc_zip_reader r;
    pc_error e;
    deflate_archive(&z, "bomb", zeros, n, 10); /* claims 10 bytes */
    CHECK(z.len < 20000);
    CHECK(pc_zip_reader_open(&r, z.data, z.len, NULL) == PC_OK);
    CHECK(pc_zip_read(&r, &r.entries[0], 1000, &d, &e) == PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "corrupt .pcraft bundle: zip entry `bomb` has wrong size");
    pc_zip_reader_close(&r);
    deflate_archive(&z, "bomb", zeros, n, (uint32_t)n);
    CHECK(pc_zip_reader_open(&r, z.data, z.len, NULL) == PC_OK);
    CHECK(pc_zip_read(&r, &r.entries[0], 1000, &d, &e) == PC_ERR_LIMIT);
    /* The whole thing when allowed; SIZE_MAX as max works (the reference's u64 overflow there
     * would read nothing in release builds). */
    CHECK(pc_zip_read(&r, &r.entries[0], SIZE_MAX, &d, NULL) == PC_OK);
    CHECK_EQ_UINT(d.len, n);
    pc_zip_reader_close(&r);
    free(zeros);
    pc_buf_fini(&z);
    pc_buf_fini(&d);
}

static void test_names_are_lossy_and_length_carrying(void)
{
    static const char *const n[2] = {"a", "manifest.json"};
    static const char *const v[2] = {"1", "{}"};
    pc_buf bytes = PC_BUF_INIT;
    pc_zip_reader r;
    pc_zip_writer w;
    pc_zip_writer_init(&w);
    CHECK(pc_zip_writer_add(&w, "x\0y", 3, (const uint8_t *)"z", 1, NULL) == PC_OK);
    CHECK(pc_zip_writer_add(&w, "\xff", 1, (const uint8_t *)"q", 1, NULL) == PC_OK);
    CHECK(pc_zip_writer_finish(&w, &bytes, NULL) == PC_OK);
    pc_zip_writer_fini(&w);
    CHECK(pc_zip_reader_open(&r, bytes.data, bytes.len, NULL) == PC_OK);
    CHECK_EQ_UINT(r.entries[0].name.len, 3);
    CHECK(pc_zip_find(&r, "x", 1) == NULL);
    CHECK(pc_zip_find(&r, "x\0y", 3) == &r.entries[0]);
    CHECK(pc_zip_find(&r, "\xef\xbf\xbd", 3) == &r.entries[1]); /* U+FFFD */
    pc_zip_reader_close(&r);
    build(&bytes, n, v, 2);
    CHECK(pc_format_is_pcraft(bytes.data, bytes.len));
    pc_buf_fini(&bytes);
}

/* Out-params are written only on success. */
static void test_failures_leave_outputs(void)
{
    pc_zip_reader r;
    pc_buf d = PC_BUF_INIT;
    static const char *const n[1] = {"a"};
    static const char *const v[1] = {"hello"};
    pc_buf bytes = PC_BUF_INIT;
    size_t sentinel;
    memset(&sentinel, 0x5a, sizeof sentinel);
    memset(&r, 0x5a, sizeof r);
    CHECK(pc_zip_reader_open(&r, (const uint8_t *)"short", 5, NULL) == PC_ERR_FORMAT);
    CHECK(r.count == sentinel);
    build(&bytes, n, v, 1);
    CHECK(pc_zip_reader_open(&r, bytes.data, bytes.len, NULL) == PC_OK);
    (void)pc_buf_append_str(&d, "keep", NULL);
    CHECK(pc_zip_read_by_name(&r, "a", 2, &d, NULL) == PC_ERR_LIMIT);
    CHECK_EQ_STR(pc_buf_cstr(&d), "keep");
    pc_zip_reader_close(&r);
    pc_buf_fini(&d);
    pc_buf_fini(&bytes);
}

static uint64_t rng_next(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Random bytes and random damage to a real archive never crash (run under ASan/UBSan). */
static void test_hostile_input_never_crashes(void)
{
    static const char *const n[3] = {"manifest.json", "tiles/a.zst", "blobs/b.zst"};
    static const char *const v[3] = {"{\"format_version\":1}", "tile data here", "blob"};
    pc_buf good = PC_BUF_INIT;
    pc_buf b = PC_BUF_INIT;
    pc_buf d = PC_BUF_INIT;
    pc_buf deflated = PC_BUF_INIT;
    uint64_t s = 42;
    size_t opened = 0;
    build(&good, n, v, 3);
    deflate_archive(&deflated, "manifest.json", (const uint8_t *)"hello hello hello hello", 23, 23);
    for (int iter = 0; iter < 6000; iter++) {
        const pc_buf *src = iter % 3 == 0 ? &deflated : &good;
        pc_zip_reader r;
        pc_buf_truncate(&b, 0);
        if (iter % 5 == 0) {
            size_t len = (size_t)(rng_next(&s) % 4096);
            for (size_t i = 0; i < len; i++) {
                (void)pc_buf_append_byte(&b, (uint8_t)rng_next(&s), NULL);
            }
            if (len >= 4 && iter % 2 == 0) {
                memcpy(b.data, "PK\x03\x04", 4);
            }
        } else {
            size_t flips = 1 + (size_t)(rng_next(&s) % 4);
            (void)pc_buf_append(&b, src->data, src->len, NULL);
            for (size_t f = 0; f < flips; f++) {
                b.data[rng_next(&s) % b.len] ^= (uint8_t)(1 + rng_next(&s) % 255);
            }
            if (iter % 7 == 0) {
                pc_buf_truncate(&b, (size_t)(rng_next(&s) % b.len));
            }
        }
        (void)pc_format_is_pcraft(b.data, b.len);
        if (pc_zip_reader_open(&r, b.data, b.len, NULL) == PC_OK) {
            opened++;
            for (size_t i = 0; i < r.count; i++) {
                (void)pc_zip_read(&r, &r.entries[i], 1 << 20, &d, NULL);
                (void)pc_zip_read(&r, &r.entries[i], 0, &d, NULL);
            }
            pc_zip_reader_close(&r);
        }
    }
    CHECK(opened > 100);
    pc_buf_fini(&good);
    pc_buf_fini(&b);
    pc_buf_fini(&d);
    pc_buf_fini(&deflated);
}

int main(int argc, char **argv)
{
    RUN(test_crc_known_value);
    RUN(test_roundtrip);
    RUN(test_crc_mismatch_detected);
    RUN(test_offset_access_rejects_overflow);
    RUN(test_eocd_search_window);
    RUN(test_writer_refusals_leave_it_unchanged);
    RUN(test_deflate_bomb_is_bounded);
    RUN(test_names_are_lossy_and_length_carrying);
    RUN(test_failures_leave_outputs);
    RUN(test_hostile_input_never_crashes);
    return pc_t_finish();
}
