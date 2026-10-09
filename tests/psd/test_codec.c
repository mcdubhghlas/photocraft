/* Ports the unit tests of crates/psd/src/{io,header,compression,resources,blend,descriptor,
 * metadata,hdr,path,slices,filter_effects}.rs (proptests become seeded random loops). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "harness.h"
#include "psd/compression.h"
#include "psd/descriptor.h"
#include "psd/filter_effects.h"
#include "psd/header.h"
#include "psd/internal.h"
#include "psd/misc.h"
#include "psd/resources.h"
#include "psd/slices.h"

static uint64_t rng_state = 0x9E3779B97F4A7C15u;
static uint64_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* ---- io.rs ---- */

static void test_reader_reads_big_endian(void)
{
    static const uint8_t d[8] = {0x00, 0x01, 0xff, 0xfe, 0x00, 0x00, 0x00, 0x02};
    psd_rd r = psd_rd_make(d, 8);
    uint16_t u;
    int16_t i;
    uint32_t w;
    uint8_t b;
    pc_error e;
    CHECK(psd_u16(&r, &u, NULL) == PC_OK && u == 1);
    CHECK(psd_i16(&r, &i, NULL) == PC_OK && i == -2);
    CHECK(psd_u32(&r, &w, NULL) == PC_OK && w == 2);
    CHECK(psd_empty(&r));
    CHECK_EQ_INT(psd_u8(&r, &b, &e), PC_ERR_FORMAT);
    CHECK_CONTAINS(e.msg, "unexpected end of data");
}

static void test_reader_eof_reports_needed(void)
{
    static const uint8_t d[2] = {1, 2};
    psd_rd r = psd_rd_make(d, 2);
    uint32_t v;
    pc_error e;
    CHECK(psd_u32(&r, &v, &e) != PC_OK);
    CHECK_EQ_STR(e.msg, "unexpected end of data at offset 0 (needed 2 more bytes)");
}

static void test_check_count_rejects_huge(void)
{
    static const uint8_t d[8];
    psd_rd r = psd_rd_make(d, 8);
    CHECK(psd_check_count(&r, UINT64_MAX, 4, NULL) != PC_OK);
    CHECK(psd_check_count(&r, 2, 4, NULL) == PC_OK);
    CHECK(psd_check_count(&r, 3, 4, NULL) != PC_OK);
}

static void test_len_patch_roundtrip(void)
{
    static const uint8_t a[7] = {0, 0, 0, 3, 1, 2, 3};
    static const uint8_t b[9] = {0, 0, 0, 0, 0, 0, 0, 1, 9};
    static const uint8_t three[3] = {1, 2, 3};
    psd_w w;
    pc_psd_bytes out = {NULL, 0};
    size_t at;
    psd_w_init(&w, NULL);
    at = psd_begin_len(&w, false);
    psd_put(&w, three, 3);
    psd_end_len(&w, at, false);
    CHECK_EQ_INT(psd_w_finish(&w, &out), PC_OK);
    CHECK(out.len == 7 && memcmp(out.data, a, 7) == 0);
    pc_psd_bytes_fini(&out);
    psd_w_init(&w, NULL);
    at = psd_begin_len(&w, true);
    psd_put_u8(&w, 9);
    psd_end_len(&w, at, true);
    CHECK_EQ_INT(psd_w_finish(&w, &out), PC_OK);
    CHECK(out.len == 9 && memcmp(out.data, b, 9) == 0);
    pc_psd_bytes_fini(&out);
}

static void test_pascal_padding(void)
{
    static const struct {
        const char *name;
        size_t align;
        size_t total;
    } cases[6] = {{"", 2, 2}, {"a", 2, 2}, {"ab", 2, 4}, {"", 4, 4}, {"abc", 4, 4}, {"abcd", 4, 8}};
    int i;
    for (i = 0; i < 6; i++) {
        psd_w w;
        pc_psd_bytes out = {NULL, 0};
        pc_psd_bytes back = {NULL, 0};
        psd_rd r;
        size_t n = strlen(cases[i].name);
        psd_w_init(&w, NULL);
        psd_write_pascal(&w, (const uint8_t *)cases[i].name, n, cases[i].align);
        CHECK_EQ_INT(psd_w_finish(&w, &out), PC_OK);
        CHECK_EQ_UINT(out.len, cases[i].total);
        r = psd_rd_make(out.data, out.len);
        CHECK_EQ_INT(psd_read_pascal(&r, cases[i].align, &back, NULL), PC_OK);
        CHECK(back.len == n && (n == 0 || memcmp(back.data, cases[i].name, n) == 0));
        CHECK(psd_empty(&r));
        pc_psd_bytes_fini(&out);
        pc_psd_bytes_fini(&back);
    }
}

static void test_legacy_names(void)
{
    static const uint8_t latin[2] = {0x41, 0xe9};
    char *s = NULL;
    pc_psd_bytes b = {NULL, 0};
    CHECK_EQ_INT(psd_decode_legacy_name((const uint8_t *)"Layer 1", 7, &s, NULL), PC_OK);
    CHECK_EQ_STR(s, "Layer 1");
    free(s);
    CHECK_EQ_INT(psd_decode_legacy_name(latin, 2, &s, NULL), PC_OK);
    CHECK_EQ_STR(s, "A\xC3\xA9");
    free(s);
    CHECK_EQ_INT(psd_encode_legacy_name("h\xC3\xA9llo", &b, NULL), PC_OK);
    CHECK(b.len == 5 && memcmp(b.data, "h?llo", 5) == 0);
    pc_psd_bytes_fini(&b);
}

static void test_unicode_units_roundtrip(void)
{
    static const uint16_t u[3] = {0x48, 0x69, 0};
    static const uint8_t bad[4] = {0xff, 0xff, 0xff, 0xff};
    psd_w w;
    pc_psd_bytes out = {NULL, 0};
    psd_rd r;
    uint16_t *back = NULL;
    size_t n = 0;
    psd_w_init(&w, NULL);
    psd_write_units(&w, u, 3);
    CHECK_EQ_INT(psd_w_finish(&w, &out), PC_OK);
    r = psd_rd_make(out.data, out.len);
    CHECK_EQ_INT(psd_read_units(&r, &back, &n, NULL), PC_OK);
    CHECK(n == 3 && memcmp(back, u, sizeof u) == 0);
    free(back);
    pc_psd_bytes_fini(&out);
    r = psd_rd_make(bad, 4);
    CHECK(psd_read_units(&r, &back, &n, NULL) != PC_OK);
}

/* ---- header.rs ---- */

static void hdr_bytes(const pc_psd_header *h, uint8_t out[26])
{
    pc_psd_header_write(h, out);
}

static void test_header_roundtrip(void)
{
    pc_psd_header h = pc_psd_header_make(PC_PSD_V2, 1000, 20, 4, 16, PC_PSD_MODE_CMYK);
    pc_psd_header back;
    uint8_t b[26];
    hdr_bytes(&h, b);
    CHECK_EQ_INT(pc_psd_header_parse(b, 26, &back, NULL), PC_OK);
    CHECK(pc_psd_header_eq(&h, &back));
}

static void test_header_errors(void)
{
    pc_psd_header h = pc_psd_header_make(PC_PSD_V1, 1, 1, 1, 8, PC_PSD_MODE_GRAYSCALE);
    pc_psd_header back;
    uint8_t b[26];
    pc_error e;
    static const uint32_t bad[7][5] = {{1, 300001, 1, 1, 8}, {1, 0, 1, 1, 8},  {1, 1, 0, 1, 8}, {2, 0, 0, 1, 8},
                                       {1, 1, 1, 0, 8},      {1, 1, 1, 57, 8}, {1, 1, 1, 3, 12}};
    int i;
    hdr_bytes(&h, b);
    b[0] = 'X';
    CHECK_EQ_INT(pc_psd_header_parse(b, 26, &back, &e), PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "invalid signature [88, 66, 80, 83], expected 8BPS");
    hdr_bytes(&h, b);
    b[5] = 3;
    CHECK_EQ_INT(pc_psd_header_parse(b, 26, &back, &e), PC_ERR_UNSUPPORTED);
    CHECK_EQ_STR(e.msg, "unsupported file version 3");
    for (i = 0; i < 7; i++) {
        pc_psd_header x = pc_psd_header_make((pc_psd_version)bad[i][0], bad[i][1], bad[i][2], (uint16_t)bad[i][3],
                                             (uint16_t)bad[i][4], PC_PSD_MODE_RGB);
        hdr_bytes(&x, b);
        CHECK(pc_psd_header_parse(b, 26, &back, NULL) != PC_OK);
    }
    CHECK_EQ_UINT(pc_psd_mode_color_channels(PC_PSD_MODE_RGB), 3);
}

static void test_row_bytes_values(void)
{
    CHECK_EQ_UINT(pc_psd_row_bytes(9, 1), 2);
    CHECK_EQ_UINT(pc_psd_row_bytes(8, 1), 1);
    CHECK_EQ_UINT(pc_psd_row_bytes(0, 1), 0);
    CHECK_EQ_UINT(pc_psd_row_bytes(3, 8), 3);
    CHECK_EQ_UINT(pc_psd_row_bytes(3, 16), 6);
    CHECK_EQ_UINT(pc_psd_row_bytes(3, 32), 12);
}

/* ---- blend.rs ---- */

static void test_blend_keys(void)
{
    int i;
    int j;
    static const uint8_t mul[4] = {'m', 'u', 'l', ' '};
    static const uint8_t zzzz[4] = {'z', 'z', 'z', 'z'};
    for (i = 0; i < PC_PSD_BLEND_COUNT; i++) {
        CHECK_EQ_INT(pc_psd_blend_index((const uint8_t *)PC_PSD_BLEND_KEYS[i]), i);
        for (j = 0; j < i; j++) {
            CHECK(memcmp(PC_PSD_BLEND_KEYS[i], PC_PSD_BLEND_KEYS[j], 4) != 0);
        }
    }
    CHECK_EQ_INT(pc_psd_blend_index(mul), 4);
    CHECK_EQ_INT(pc_psd_blend_index(zzzz), -1);
}

/* ---- compression.rs ---- */

static pc_psd_layout lay(size_t planes, size_t w, size_t h, uint16_t depth, pc_psd_version v)
{
    pc_psd_layout l;
    l.planes = planes;
    l.width = w;
    l.height = h;
    l.depth = depth;
    l.version = v;
    return l;
}

static pc_buf pb_enc(const uint8_t *src, size_t n)
{
    pc_buf b = PC_BUF_INIT;
    pc_psd_bytes dec = {NULL, 0};
    CHECK_EQ_INT(pc_psd_packbits_encode(src, n, &b, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_packbits_decode(b.data, b.len, n, &dec, NULL), PC_OK);
    CHECK(dec.len == n && (n == 0 || memcmp(dec.data, src, n) == 0));
    pc_psd_bytes_fini(&dec);
    return b;
}

static bool buf_is(const pc_buf *b, const uint8_t *want, size_t n)
{
    return b->len == n && (n == 0 || memcmp(b->data, want, n) == 0);
}

static void test_packbits_vectors(void)
{
    uint8_t src[300];
    pc_buf b;
    static const uint8_t one[1] = {7};
    static const uint8_t two[2] = {5, 5};
    static const uint8_t w1[2] = {0, 7};
    static const uint8_t w2[2] = {0xff, 5};
    static const uint8_t w128[2] = {0x81, 9};
    static const uint8_t w129[4] = {0x81, 9, 0, 9};
    static const uint8_t w256[4] = {0x81, 1, 0x81, 1};
    static const uint8_t lr[5] = {1, 2, 3, 3, 3};
    static const uint8_t wlr[5] = {1, 1, 2, 0xfe, 3};
    size_t i;
    size_t n;
    b = pb_enc(NULL, 0);
    CHECK_EQ_UINT(b.len, 0);
    pc_buf_fini(&b);
    b = pb_enc(one, 1);
    CHECK(buf_is(&b, w1, 2));
    pc_buf_fini(&b);
    b = pb_enc(two, 2);
    CHECK(buf_is(&b, w2, 2));
    pc_buf_fini(&b);
    memset(src, 9, sizeof src);
    b = pb_enc(src, 128);
    CHECK(buf_is(&b, w128, 2));
    pc_buf_fini(&b);
    b = pb_enc(src, 129);
    CHECK(buf_is(&b, w129, 4));
    pc_buf_fini(&b);
    memset(src, 1, sizeof src);
    b = pb_enc(src, 130);
    pc_buf_fini(&b);
    b = pb_enc(src, 256);
    CHECK(buf_is(&b, w256, 4));
    pc_buf_fini(&b);
    for (i = 0; i < 129; i++) {
        src[i] = (uint8_t)i;
    }
    b = pb_enc(src, 128);
    CHECK(b.data[0] == 127 && b.len == 129);
    pc_buf_fini(&b);
    b = pb_enc(src, 129);
    CHECK(b.data[0] == 127 && b.data[129] == 0 && b.len == 131);
    pc_buf_fini(&b);
    b = pb_enc(lr, 5);
    CHECK(buf_is(&b, wlr, 5));
    pc_buf_fini(&b);
    /* never emits the -128 no-op */
    for (n = 0; n < 300; n++) {
        size_t at = 0;
        for (i = 0; i < n; i++) {
            src[i] = (uint8_t)((i / 3) % 5);
        }
        b = pb_enc(src, n);
        while (at < b.len) {
            int h = (int)(int8_t)b.data[at];
            CHECK(h != -128);
            at += h >= 0 ? (size_t)h + 2 : 2;
        }
        pc_buf_fini(&b);
    }
}

static void test_packbits_decode_edges(void)
{
    static const uint8_t noop[3] = {0x80, 0, 42};
    static const uint8_t lt[3] = {2, 1, 2};
    static const uint8_t rt[1] = {0xfe};
    static const uint8_t ro[2] = {0xfd, 1};
    static const uint8_t lo[5] = {3, 1, 2, 3, 4};
    static const uint8_t z[2] = {0xff, 1};
    pc_psd_bytes d = {NULL, 0};
    CHECK_EQ_INT(pc_psd_packbits_decode(noop, 3, 1, &d, NULL), PC_OK);
    CHECK(d.len == 1 && d.data[0] == 42);
    pc_psd_bytes_fini(&d);
    CHECK(pc_psd_packbits_decode(NULL, 0, 1, &d, NULL) != PC_OK);
    CHECK(pc_psd_packbits_decode(lt, 3, 3, &d, NULL) != PC_OK);
    CHECK(pc_psd_packbits_decode(rt, 1, 3, &d, NULL) != PC_OK);
    CHECK(pc_psd_packbits_decode(ro, 2, 3, &d, NULL) != PC_OK);
    CHECK(pc_psd_packbits_decode(lo, 5, 3, &d, NULL) != PC_OK);
    CHECK_EQ_INT(pc_psd_packbits_decode(z, 2, 0, &d, NULL), PC_OK);
    CHECK_EQ_UINT(d.len, 0);
}

static pc_psd_bytes sample_data(const pc_psd_layout *l)
{
    size_t n = 0;
    size_t i;
    pc_psd_bytes b = {NULL, 0};
    CHECK_EQ_INT(pc_psd_layout_decoded_len(l, &n, NULL), PC_OK);
    if (n > 0) {
        b.data = malloc(n);
        b.len = n;
        for (i = 0; b.data != NULL && i < n; i++) {
            b.data[i] = (uint8_t)((i * 7 + i / 5) % 251);
        }
    }
    return b;
}

static void test_all_codecs_roundtrip_all_depths(void)
{
    static const size_t sizes[6][2] = {{0, 0}, {1, 1}, {3, 2}, {17, 5}, {0, 3}, {4, 0}};
    static const uint16_t depths[4] = {1, 8, 16, 32};
    int v;
    int d;
    int s;
    int planes;
    uint16_t c;
    int bad = 0;
    for (v = 1; v <= 2; v++) {
        for (d = 0; d < 4; d++) {
            for (s = 0; s < 6; s++) {
                for (planes = 1; planes <= 3; planes += 2) {
                    pc_psd_layout l = lay((size_t)planes, sizes[s][0], sizes[s][1], depths[d], (pc_psd_version)v);
                    pc_psd_bytes data = sample_data(&l);
                    for (c = 0; c < 4; c++) {
                        pc_psd_bytes enc = {NULL, 0};
                        pc_psd_bytes dec = {NULL, 0};
                        bad += pc_psd_encode_planes(c, data.data, data.len, &l, &enc, NULL) != PC_OK;
                        bad += pc_psd_validate_planes(c, enc.data, enc.len, &l, NULL) != PC_OK;
                        bad += pc_psd_decode_planes(c, enc.data, enc.len, &l, &dec, NULL) != PC_OK;
                        bad += !pc_psd_bytes_eq(&dec, &data);
                        pc_psd_bytes_fini(&enc);
                        pc_psd_bytes_fini(&dec);
                    }
                    pc_psd_bytes_fini(&data);
                }
            }
        }
    }
    CHECK_EQ_INT(bad, 0);
}

static void test_rle_count_width_and_truncation(void)
{
    static const uint8_t zeros[4];
    static const uint8_t wpsd[8] = {0, 2, 0, 2, 0xff, 0, 0xff, 0};
    static const uint8_t wpsb[12] = {0, 0, 0, 2, 0, 0, 0, 2, 0xff, 0, 0xff, 0};
    pc_psd_layout l1 = lay(1, 2, 2, 8, PC_PSD_V1);
    pc_psd_layout l2 = lay(1, 2, 2, 8, PC_PSD_V2);
    pc_psd_layout l3 = lay(1, 10, 10, 8, PC_PSD_V1);
    pc_psd_bytes a = {NULL, 0};
    pc_psd_bytes b = {NULL, 0};
    pc_psd_bytes d = {NULL, 0};
    pc_psd_bytes data;
    size_t cut;
    int bad = 0;
    CHECK_EQ_INT(pc_psd_encode_planes(PC_PSD_RLE, zeros, 4, &l1, &a, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_encode_planes(PC_PSD_RLE, zeros, 4, &l2, &b, NULL), PC_OK);
    CHECK(a.len == 8 && memcmp(a.data, wpsd, 8) == 0);
    CHECK(b.len == 12 && memcmp(b.data, wpsb, 12) == 0);
    CHECK(pc_psd_decode_planes(PC_PSD_RLE, a.data, a.len, &l2, &d, NULL) != PC_OK);
    pc_psd_bytes_fini(&a);
    pc_psd_bytes_fini(&b);
    data = sample_data(&l3);
    CHECK_EQ_INT(pc_psd_encode_planes(PC_PSD_RLE, data.data, data.len, &l3, &a, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_encode_planes(PC_PSD_ZIP, data.data, data.len, &l3, &b, NULL), PC_OK);
    for (cut = 0; cut < a.len; cut++) {
        bad += pc_psd_decode_planes(PC_PSD_RLE, a.data, cut, &l3, &d, NULL) == PC_OK;
        bad += pc_psd_validate_planes(PC_PSD_RLE, a.data, cut, &l3, NULL) == PC_OK;
    }
    for (cut = 0; cut + 4 < b.len; cut++) {
        bad += pc_psd_decode_planes(PC_PSD_ZIP, b.data, cut, &l3, &d, NULL) == PC_OK;
        bad += pc_psd_validate_planes(PC_PSD_ZIP, b.data, cut, &l3, NULL) == PC_OK;
    }
    CHECK_EQ_INT(bad, 0);
    pc_psd_bytes_fini(&a);
    pc_psd_bytes_fini(&b);
    pc_psd_bytes_fini(&data);
}

static void test_codec_errors_and_limits(void)
{
    static const uint8_t z18[18];
    static const uint8_t one[1];
    pc_psd_layout l = lay(2, 3, 3, 8, PC_PSD_V1);
    pc_psd_layout l1 = lay(1, 1, 1, 8, PC_PSD_V1);
    pc_psd_layout l2 = lay(1, 2, 2, 8, PC_PSD_V1);
    pc_psd_layout huge = lay(56, 300000, 300000, 32, PC_PSD_V2);
    pc_psd_bytes d = {NULL, 0};
    size_t n;
    pc_error e;
    CHECK(pc_psd_decode_planes(PC_PSD_RAW, z18, 17, &l, &d, NULL) != PC_OK);
    CHECK(pc_psd_decode_planes(PC_PSD_RAW, z18, 18, &l, &d, NULL) == PC_OK);
    pc_psd_bytes_fini(&d);
    CHECK_EQ_INT(pc_psd_decode_planes(7, one, 1, &l1, &d, &e), PC_ERR_UNSUPPORTED);
    CHECK_EQ_STR(e.msg, "unsupported: compression method 7");
    CHECK(pc_psd_encode_planes(7, one, 1, &l1, &d, NULL) != PC_OK);
    CHECK_EQ_INT(pc_psd_encode_planes(PC_PSD_RAW, z18, 3, &l2, &d, NULL), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_psd_layout_decoded_len(&huge, &n, &e), PC_ERR_LIMIT);
    CHECK(pc_psd_decode_planes(PC_PSD_RLE, z18, 16, &huge, &d, NULL) != PC_OK);
    if (SIZE_MAX > 0xFFFFFFFFu) { /* real PSBs go past 2 GiB in one decode */
        pc_psd_layout a = lay(3, 30000, 30000, 8, PC_PSD_V2);
        pc_psd_layout b = lay(1, 33000, 33000, 16, PC_PSD_V2);
        pc_psd_layout c = lay(4, 40000, 40000, 16, PC_PSD_V2);
        CHECK(pc_psd_layout_decoded_len(&a, &n, NULL) == PC_OK && n == 2700000000u);
        CHECK(pc_psd_layout_decoded_len(&b, &n, NULL) == PC_OK && n == 2178000000u);
        CHECK(pc_psd_layout_decoded_len(&c, &n, NULL) != PC_OK);
    }
}

static void test_zip_edges(void)
{
    uint8_t seven[64];
    pc_psd_bytes z = {NULL, 0};
    pc_psd_bytes d = {NULL, 0};
    pc_psd_bytes big = {NULL, 0};
    uint8_t *src;
    size_t i;
    pc_psd_layout l = lay(1, 1u << 16, 1u << 15, 8, PC_PSD_V2);
    pc_psd_layout l2 = lay(4, 1u << 20, 1u << 20, 32, PC_PSD_V2);
    static const uint8_t ff[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                   0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    static const uint8_t junk[4] = {1, 2, 3, 4};
    memset(seven, 7, sizeof seven);
    CHECK_EQ_INT(pc_psd_zip_compress(seven, 64, &z, NULL), PC_OK);
    CHECK(pc_psd_zip_decompress(z.data, z.len, (size_t)1 << 40, &d, NULL) != PC_OK);
    CHECK(pc_psd_zip_decompress(z.data, z.len, SIZE_MAX / 2, &d, NULL) != PC_OK);
    CHECK(pc_psd_decode_planes(PC_PSD_ZIP, z.data, z.len, &l, &d, NULL) != PC_OK);
    CHECK(pc_psd_decode_planes(PC_PSD_ZIP_PREDICT, z.data, z.len, &l, &d, NULL) != PC_OK);
    CHECK(pc_psd_decode_planes(PC_PSD_ZIP, z.data, z.len, &l2, &d, NULL) != PC_OK);
    CHECK(pc_psd_zip_decompress(ff, 16, (size_t)1 << 30, &d, NULL) != PC_OK);
    CHECK(pc_psd_zip_decompress(junk, 4, 4, &d, NULL) != PC_OK);
    pc_psd_bytes_fini(&z);
    src = malloc(3000000);
    if (src == NULL) {
        return;
    }
    for (i = 0; i < 3000000; i++) {
        src[i] = (uint8_t)(i % 251);
    }
    CHECK_EQ_INT(pc_psd_zip_compress(src, 3000000, &z, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_zip_decompress(z.data, z.len, 3000000, &d, NULL), PC_OK);
    CHECK(d.len == 3000000 && memcmp(d.data, src, 3000000) == 0);
    CHECK_EQ_INT(pc_psd_zip_decompress(z.data, z.len, 1000, &d, NULL), PC_OK);
    CHECK(d.len == 1000 && memcmp(d.data, src, 1000) == 0);
    CHECK(pc_psd_zip_decompress(z.data, z.len, 3000001, &d, NULL) != PC_OK);
    CHECK_EQ_INT(pc_psd_zip_decompress(z.data, z.len, 0, &d, NULL), PC_OK);
    CHECK_EQ_UINT(d.len, 0);
    pc_psd_bytes_fini(&z);
    memset(src, 0, 1 << 20);
    CHECK_EQ_INT(pc_psd_zip_compress(src, 1 << 20, &big, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_zip_decompress(big.data, big.len, 10, &d, NULL), PC_OK); /* bomb bounded */
    CHECK_EQ_UINT(d.len, 10);
    pc_psd_bytes_fini(&big);
    pc_psd_bytes_fini(&d);
    free(src);
}

static void test_prediction_vectors(void)
{
    uint8_t v8[4] = {10, 12, 11, 255};
    static const uint8_t p8[4] = {10, 2, 255, 244};
    static const uint8_t o8[4] = {10, 12, 11, 255};
    uint8_t v16[6] = {0x01, 0x00, 0x01, 0x05, 0x00, 0x00};
    static const uint8_t p16[6] = {0x01, 0x00, 0x00, 0x05, 0xfe, 0xfb};
    static const uint8_t o16[6] = {0x01, 0x00, 0x01, 0x05, 0x00, 0x00};
    uint8_t v32[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22, 0x33, 0x44};
    static const uint8_t o32[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22, 0x33, 0x44};
    uint8_t expect[8] = {0xAA, 0x11, 0xBB, 0x22, 0xCC, 0x33, 0xDD, 0x44};
    pc_psd_layout l8 = lay(1, 4, 1, 8, PC_PSD_V1);
    pc_psd_layout l16 = lay(1, 3, 1, 16, PC_PSD_V1);
    pc_psd_layout l32 = lay(1, 2, 1, 32, PC_PSD_V1);
    pc_psd_layout lf = lay(1, 5, 2, 32, PC_PSD_V1);
    uint8_t floats[40];
    pc_psd_bytes enc = {NULL, 0};
    pc_psd_bytes dec = {NULL, 0};
    int i;
    CHECK_EQ_INT(pc_psd_predict(v8, 4, &l8, NULL), PC_OK);
    CHECK_EQ_MEM(v8, p8, 4);
    CHECK_EQ_INT(pc_psd_unpredict(v8, 4, &l8, NULL), PC_OK);
    CHECK_EQ_MEM(v8, o8, 4);
    CHECK_EQ_INT(pc_psd_predict(v16, 6, &l16, NULL), PC_OK);
    CHECK_EQ_MEM(v16, p16, 6);
    CHECK_EQ_INT(pc_psd_unpredict(v16, 6, &l16, NULL), PC_OK);
    CHECK_EQ_MEM(v16, o16, 6);
    for (i = 7; i >= 1; i--) {
        expect[i] = (uint8_t)(expect[i] - expect[i - 1]);
    }
    CHECK_EQ_INT(pc_psd_predict(v32, 8, &l32, NULL), PC_OK);
    CHECK_EQ_MEM(v32, expect, 8);
    CHECK_EQ_INT(pc_psd_unpredict(v32, 8, &l32, NULL), PC_OK);
    CHECK_EQ_MEM(v32, o32, 8);
    for (i = 0; i < 10; i++) {
        pc_store_be32(floats + 4 * i, pc_f32_to_bits((float)i * 0.37f - 1.0f));
    }
    CHECK_EQ_INT(pc_psd_encode_planes(PC_PSD_ZIP_PREDICT, floats, 40, &lf, &enc, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_decode_planes(PC_PSD_ZIP_PREDICT, enc.data, enc.len, &lf, &dec, NULL), PC_OK);
    CHECK(dec.len == 40 && memcmp(dec.data, floats, 40) == 0);
    pc_psd_bytes_fini(&enc);
    pc_psd_bytes_fini(&dec);
}

/* proptests.rs: packbits and plane codecs on random data. */
static void test_codec_properties(void)
{
    static const uint16_t depths[4] = {1, 8, 16, 32};
    int k;
    int bad = 0;
    for (k = 0; k < 300; k++) {
        uint8_t data[600];
        size_t n = rnd() % 600;
        size_t i;
        pc_buf enc = PC_BUF_INIT;
        pc_psd_bytes dec = {NULL, 0};
        for (i = 0; i < n; i++) {
            data[i] = (uint8_t)(rnd() % 4 == 0 ? rnd() : data[i > 0 ? i - 1 : 0]);
        }
        bad += pc_psd_packbits_encode(data, n, &enc, NULL) != PC_OK;
        bad += pc_psd_packbits_decode(enc.data, enc.len, n, &dec, NULL) != PC_OK;
        bad += dec.len != n || (n > 0 && memcmp(dec.data, data, n) != 0);
        bad += enc.len > n + (n + 127) / 128;
        pc_buf_fini(&enc);
        pc_psd_bytes_fini(&dec);
        /* garbage never crashes */
        for (i = 0; i < 64; i++) {
            data[i] = (uint8_t)rnd();
        }
        (void)pc_psd_packbits_decode(data, rnd() % 64, rnd() % 512, &dec, NULL);
        pc_psd_bytes_fini(&dec);
    }
    for (k = 0; k < 200; k++) {
        pc_psd_layout l = lay(1 + rnd() % 3, rnd() % 40, rnd() % 12, depths[rnd() % 4], rnd() % 2 ? PC_PSD_V1 : PC_PSD_V2);
        uint16_t c = (uint16_t)(rnd() % 4);
        uint64_t x = rnd();
        size_t n = 0;
        size_t i;
        pc_psd_bytes data = {NULL, 0};
        pc_psd_bytes enc = {NULL, 0};
        pc_psd_bytes dec = {NULL, 0};
        uint8_t garbage[128];
        (void)pc_psd_layout_decoded_len(&l, &n, NULL);
        data.data = n > 0 ? malloc(n) : NULL;
        data.len = n;
        for (i = 0; i < n; i++) {
            x = x * 6364136223846793005u + 1;
            data.data[i] = x >> 62 == 0 ? 0 : (uint8_t)(x >> 33);
        }
        bad += pc_psd_encode_planes(c, data.data, n, &l, &enc, NULL) != PC_OK;
        bad += pc_psd_decode_planes(c, enc.data, enc.len, &l, &dec, NULL) != PC_OK;
        bad += !pc_psd_bytes_eq(&dec, &data);
        pc_psd_bytes_fini(&enc);
        pc_psd_bytes_fini(&dec);
        pc_psd_bytes_fini(&data);
        for (i = 0; i < 128; i++) {
            garbage[i] = (uint8_t)rnd();
        }
        l.planes = 1;
        (void)pc_psd_decode_planes((uint16_t)(rnd() % 5), garbage, rnd() % 128, &l, &dec, NULL);
        pc_psd_bytes_fini(&dec);
    }
    CHECK_EQ_INT(bad, 0);
}

/* ---- resources.rs ---- */

static pc_psd_bytes res_section(const pc_psd_resource *res, size_t n)
{
    psd_w w;
    pc_psd_bytes out = {NULL, 0};
    psd_w_init(&w, NULL);
    psd_write_resources(&w, res, n);
    CHECK_EQ_INT(psd_w_finish(&w, &out), PC_OK);
    return out;
}

static void test_resources_section(void)
{
    static const uint8_t three[3] = {1, 2, 3};
    static const uint8_t nine[4] = {9, 9, 9, 9};
    static const uint8_t zero4[4];
    pc_psd_resource r[2];
    pc_psd_bytes out;
    pc_psd_resource *back = NULL;
    size_t nb = 0;
    psd_rd rd;
    size_t cut;
    int bad = 0;
    out = res_section(NULL, 0);
    CHECK(out.len == 4 && memcmp(out.data, zero4, 4) == 0);
    pc_psd_bytes_fini(&out);
    CHECK_EQ_INT(pc_psd_resource_init(&r[0], 1000, three, 3, NULL), PC_OK);
    out = res_section(r, 1);
    CHECK_EQ_UINT(out.len, 4 + 4 + 2 + 2 + 4 + 4); /* odd data is padded */
    CHECK_EQ_UINT(out.data[out.len - 1], 0);
    rd = psd_rd_make(out.data, out.len);
    CHECK_EQ_INT(psd_read_resources(&rd, &back, &nb, NULL), PC_OK);
    CHECK(nb == 1 && pc_psd_resource_eq(&back[0], &r[0]));
    pc_psd_resources_free(back, nb);
    out.data[4] = 'X';
    rd = psd_rd_make(out.data, out.len);
    CHECK(psd_read_resources(&rd, &back, &nb, NULL) != PC_OK);
    pc_psd_bytes_fini(&out);
    pc_psd_resource_fini(&r[0]);
    /* named resources and other signatures */
    CHECK_EQ_INT(pc_psd_resource_init(&r[0], 2000, nine, 4, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_bytes_set(&r[0].name, "path", 4, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_resource_init(&r[1], 2001, NULL, 0, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_bytes_set(&r[1].name, "abc", 3, NULL), PC_OK);
    memcpy(r[1].signature, "MeSa", 4);
    out = res_section(r, 2);
    rd = psd_rd_make(out.data, out.len);
    CHECK_EQ_INT(psd_read_resources(&rd, &back, &nb, NULL), PC_OK);
    CHECK(nb == 2 && pc_psd_resource_eq(&back[0], &r[0]) && pc_psd_resource_eq(&back[1], &r[1]));
    pc_psd_resources_free(back, nb);
    for (cut = 0; cut < out.len; cut++) {
        rd = psd_rd_make(out.data, cut);
        if (psd_read_resources(&rd, &back, &nb, NULL) == PC_OK) {
            bad++;
            pc_psd_resources_free(back, nb);
        }
    }
    CHECK_EQ_INT(bad, 0);
    pc_psd_bytes_fini(&out);
    pc_psd_resource_fini(&r[0]);
    pc_psd_resource_fini(&r[1]);
}

static void test_typed_resources(void)
{
    pc_psd_resolution ri = pc_psd_resolution_from_dpi(300.0);
    pc_psd_resolution back;
    uint8_t b[16];
    pc_psd_resource r;
    pc_psd_resource vr;
    pc_psd_version_info vi;
    pc_psd_bytes again = {NULL, 0};
    uint16_t *ids = NULL;
    size_t n = 0;
    int32_t v;
    uint16_t u;
    size_t xl;
    static const uint8_t ang[4] = {0, 0, 0, 120};
    static const uint8_t ls[2] = {0, 2};
    static const uint8_t gi[4] = {0, 1, 0, 2};
    pc_psd_resolution_write(&ri, b);
    CHECK_EQ_INT(pc_psd_resolution_parse(b, 16, &back, NULL), PC_OK);
    CHECK(memcmp(&back, &ri, sizeof ri) == 0);
    CHECK_NEAR(pc_psd_resolution_h(&back), 300.0, 1e-9);
    CHECK_NEAR(pc_psd_resolution_v(&back), 300.0, 1e-9);
    CHECK_EQ_INT(pc_psd_resource_init(&r, PC_PSD_RES_GLOBAL_ANGLE, ang, 4, NULL), PC_OK);
    CHECK(pc_psd_res_i32(&r, &v, NULL) == PC_OK && v == 120);
    pc_psd_resource_fini(&r);
    CHECK_EQ_INT(pc_psd_resource_init(&r, PC_PSD_RES_LAYER_STATE, ls, 2, NULL), PC_OK);
    CHECK(pc_psd_res_layer_state(&r, &u, NULL) == PC_OK && u == 2);
    pc_psd_resource_fini(&r);
    CHECK_EQ_INT(pc_psd_resource_init(&r, PC_PSD_RES_LAYER_GROUP_INFO, gi, 4, NULL), PC_OK);
    CHECK(pc_psd_res_group_ids(&r, &ids, &n, NULL) == PC_OK && n == 2 && ids[0] == 1 && ids[1] == 2);
    free(ids);
    pc_psd_resource_fini(&r);
    CHECK_EQ_INT(pc_psd_resource_init(&r, PC_PSD_RES_LAYER_GROUP_INFO, gi, 1, NULL), PC_OK);
    CHECK(pc_psd_res_group_ids(&r, &ids, &n, NULL) != PC_OK);
    pc_psd_resource_fini(&r);
    CHECK_EQ_INT(pc_psd_resource_init(&r, PC_PSD_RES_XMP, "<x/>", 4, NULL), PC_OK);
    CHECK(pc_psd_res_xmp(&r, &xl) != NULL && xl == 4);
    pc_psd_resource_fini(&r);
    CHECK(!pc_psd_res_is_known(4242) && pc_psd_res_is_known(PC_PSD_RES_EXIF));
    CHECK_EQ_INT(pc_psd_version_info_resource(false, &vr, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_version_info_parse(vr.data.data, vr.data.len, &vi, NULL), PC_OK);
    CHECK(!vi.has_real_merged_data);
    CHECK_EQ_INT(pc_psd_version_info_to_bytes(&vi, &again, NULL), PC_OK);
    CHECK(pc_psd_bytes_eq(&again, &vr.data));
    pc_psd_bytes_fini(&again);
    pc_psd_version_info_fini(&vi);
    pc_psd_resource_fini(&vr);
}

/* ---- descriptor.rs ---- */

static pc_psd_value v_class(const char *name, const char *id)
{
    pc_psd_value v;
    memset(&v, 0, sizeof v);
    v.kind = PC_PSD_V_CLASS;
    CHECK_EQ_INT(pc_psd_ustr_from_utf8(name, false, &v.cls.name, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_id_make(id, &v.cls.class_id, NULL), PC_OK);
    return v;
}

static void add(pc_psd_desc *d, const char *key, pc_psd_value v)
{
    CHECK_EQ_INT(pc_psd_desc_add(d, key, &v, NULL), PC_OK);
}

static pc_psd_ref_item ref_class(pc_psd_ref_kind kind, const char *cls)
{
    pc_psd_ref_item it;
    memset(&it, 0, sizeof it);
    it.kind = kind;
    CHECK_EQ_INT(pc_psd_id_make(cls, &it.cls.class_id, NULL), PC_OK);
    return it;
}

/* descriptor.rs tests::sample(): every value type. */
static pc_psd_desc desc_sample(void)
{
    pc_psd_desc d;
    pc_psd_desc inner;
    pc_psd_value v;
    pc_psd_value list;
    static const uint8_t alis[3] = {1, 2, 3};
    static const uint8_t tdta[2] = {0xde, 0xad};
    static const uint8_t pth[2] = {9, 9};
    pc_psd_desc_init(&d, "null", NULL);
    CHECK_EQ_INT(pc_psd_value_text("Hello \xF0\x9F\x98\x80", true, &v, NULL), PC_OK);
    add(&d, "Nm  ", v);
    memset(&v, 0, sizeof v);
    v.kind = PC_PSD_V_UNIT_FLOAT;
    memcpy(v.unit, "#Prc", 4);
    v.d = 75.5;
    add(&d, "Opct", v);
    add(&d, "enab", pc_psd_value_bool(true));
    add(&d, "long", pc_psd_value_int(-5));
    memset(&v, 0, sizeof v);
    v.kind = PC_PSD_V_LARGE_INTEGER;
    v.l = (int64_t)1 << 40;
    add(&d, "comp", v);
    add(&d, "doub", pc_psd_value_double(0.25));
    CHECK_EQ_INT(pc_psd_value_enum("BlnM", "Mltp", &v, NULL), PC_OK);
    add(&d, "Md  ", v);
    pc_psd_desc_init(&inner, "classFloatRect", NULL);
    add(&inner, "Top ", pc_psd_value_double(1.0));
    CHECK_EQ_INT(pc_psd_value_object(&inner, &v, NULL), PC_OK);
    add(&d, "artboardRect", v);
    memset(&list, 0, sizeof list);
    list.kind = PC_PSD_V_LIST;
    v = pc_psd_value_int(1);
    CHECK_EQ_INT(pc_psd_value_list_push(&list, &v, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_value_text("x", false, &v, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_value_list_push(&list, &v, NULL), PC_OK);
    add(&d, "list", list);
    memset(&v, 0, sizeof v);
    v.kind = PC_PSD_V_REFERENCE;
    v.nrefs = 7;
    v.refs = calloc(7, sizeof *v.refs);
    v.refs[0] = ref_class(PC_PSD_REF_PROPERTY, "Lyr ");
    pc_psd_id_make("Nm  ", &v.refs[0].key, NULL);
    v.refs[1] = ref_class(PC_PSD_REF_CLASS, "Dcmn");
    v.refs[2] = ref_class(PC_PSD_REF_ENUMERATED, "Lyr ");
    pc_psd_id_make("Ordn", &v.refs[2].key, NULL);
    pc_psd_id_make("Trgt", &v.refs[2].value, NULL);
    v.refs[3] = ref_class(PC_PSD_REF_OFFSET, "Lyr ");
    v.refs[3].offset = -1;
    v.refs[4].kind = PC_PSD_REF_IDENTIFIER;
    v.refs[4].number = 7;
    v.refs[5].kind = PC_PSD_REF_INDEX;
    v.refs[5].number = 2;
    v.refs[6] = ref_class(PC_PSD_REF_NAME, "Lyr ");
    pc_psd_ustr_from_utf8("Bg", false, &v.refs[6].name, NULL);
    add(&d, "null", v);
    add(&d, "type", v_class("c", "Clr "));
    v = v_class("c", "RGBC");
    v.kind = PC_PSD_V_GLOBAL_CLASS;
    add(&d, "glbc", v);
    pc_psd_desc_init(&inner, "null", NULL);
    CHECK_EQ_INT(pc_psd_value_object(&inner, &v, NULL), PC_OK);
    v.kind = PC_PSD_V_GLOBAL_OBJECT;
    add(&d, "glbo", v);
    memset(&v, 0, sizeof v);
    v.kind = PC_PSD_V_ALIAS;
    pc_psd_bytes_set(&v.raw, alis, 3, NULL);
    add(&d, "alis", v);
    memset(&v, 0, sizeof v);
    v.kind = PC_PSD_V_RAW_DATA;
    pc_psd_bytes_set(&v.raw, tdta, 2, NULL);
    add(&d, "tdta", v);
    memset(&v, 0, sizeof v);
    v.kind = PC_PSD_V_PATH;
    pc_psd_bytes_set(&v.raw, pth, 2, NULL);
    add(&d, "pth ", v);
    pc_psd_desc_init(&inner, "rationalPoint", NULL);
    memset(&v, 0, sizeof v);
    v.kind = PC_PSD_V_UNIT_FLOATS;
    memcpy(v.unit, "#Pxl", 4);
    v.nfloats = 2;
    v.floats = malloc(2 * sizeof(double));
    v.floats[0] = 1.0;
    v.floats[1] = 2.0;
    add(&inner, "Hrzn", v);
    CHECK_EQ_INT(pc_psd_value_object(&inner, &v, NULL), PC_OK);
    v.kind = PC_PSD_V_OBJECT_ARRAY;
    v.prefix = 16;
    add(&d, "ObAr", v);
    return d;
}

static void test_descriptor_roundtrip_all_types(void)
{
    pc_psd_desc d = desc_sample();
    pc_psd_desc p;
    pc_psd_desc c;
    pc_psd_bytes b = {NULL, 0};
    pc_psd_bytes b2 = {NULL, 0};
    size_t used = 0;
    size_t cut;
    int bad = 0;
    pc_error e;
    CHECK_EQ_INT(pc_psd_desc_to_bytes(&d, &b, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_desc_parse(b.data, b.len, &p, NULL), PC_OK);
    CHECK(pc_psd_desc_eq(&p, &d));
    CHECK_EQ_INT(pc_psd_desc_to_bytes(&p, &b2, NULL), PC_OK);
    CHECK(pc_psd_bytes_eq(&b, &b2));
    CHECK_EQ_INT(pc_psd_desc_clone(&d, &c, NULL), PC_OK);
    CHECK(pc_psd_desc_eq(&c, &d));
    pc_psd_desc_fini(&c);
    pc_psd_desc_fini(&p);
    pc_psd_bytes_fini(&b2);
    CHECK(pc_psd_desc_get(&d, "long") != NULL && pc_psd_desc_get(&d, "long")->i == -5);
    CHECK(pc_psd_desc_get(&d, "nope") == NULL);
    for (cut = 0; cut < b.len; cut++) {
        if (pc_psd_desc_parse(b.data, cut, &p, NULL) == PC_OK) {
            bad++;
            pc_psd_desc_fini(&p);
        }
    }
    CHECK_EQ_INT(bad, 0);
    b.data = realloc(b.data, b.len + 1);
    b.data[b.len++] = 0;
    CHECK_EQ_INT(pc_psd_desc_parse(b.data, b.len, &p, &e), PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "invalid data: trailing bytes after descriptor");
    pc_psd_bytes_fini(&b);
    /* versioned */
    CHECK_EQ_INT(pc_psd_vdesc_to_bytes(&d, &b, NULL), PC_OK);
    CHECK(b.data[3] == 16);
    CHECK_EQ_INT(pc_psd_vdesc_parse(b.data, b.len, &p, NULL), PC_OK);
    CHECK(pc_psd_desc_eq(&p, &d));
    pc_psd_desc_fini(&p);
    b.data = realloc(b.data, b.len + 3);
    memset(b.data + b.len, 1, 3);
    CHECK_EQ_INT(pc_psd_vdesc_parse_prefix(b.data, b.len + 3, &p, &used, NULL), PC_OK);
    CHECK_EQ_UINT(used, b.len);
    pc_psd_desc_fini(&p);
    b.data[3] = 15;
    CHECK_EQ_INT(pc_psd_vdesc_parse(b.data, b.len, &p, &e), PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "invalid data: descriptor version 15, expected 16");
    pc_psd_bytes_fini(&b);
    pc_psd_desc_fini(&d);
}

static void test_descriptor_ids_and_errors(void)
{
    static const uint8_t explicit4[8] = {0, 0, 0, 4, 'a', 'b', 'c', 'd'};
    pc_psd_desc d;
    pc_psd_desc p;
    pc_psd_bytes b = {NULL, 0};
    pc_psd_ustr s;
    char *t = NULL;
    pc_psd_id id;
    pc_error e;
    uint16_t lone[2] = {0xd800, 0x41};
    int i;
    psd_w w;
    psd_rd r;
    /* A 4-byte explicit-length id stays explicit. */
    pc_psd_desc_init(&d, "null", NULL);
    pc_psd_desc_to_bytes(&d, &b, NULL);
    CHECK(b.len == 4 + 8 + 4 && memcmp(b.data + 4, "\0\0\0\0null", 8) == 0);
    pc_psd_bytes_fini(&b);
    pc_psd_id_fini(&d.class_id);
    r = psd_rd_make(explicit4, 8);
    (void)r;
    memset(&id, 0, sizeof id);
    CHECK_EQ_INT(pc_psd_bytes_set(&id.str, "abcd", 4, NULL), PC_OK);
    d.class_id = id;
    psd_w_init(&w, NULL);
    psd_write_desc(&w, &d);
    CHECK_EQ_INT(psd_w_finish(&w, &b), PC_OK);
    CHECK(memcmp(b.data + 4, explicit4, 8) == 0);
    CHECK_EQ_INT(pc_psd_desc_parse(b.data, b.len, &p, NULL), PC_OK);
    CHECK(!p.class_id.is_code && pc_psd_desc_eq(&p, &d));
    pc_psd_desc_fini(&p);
    pc_psd_bytes_fini(&b);
    /* unknown OSType */
    pc_psd_desc_fini(&d);
    pc_psd_desc_init(&d, "null", NULL);
    pc_psd_desc_to_bytes(&d, &b, NULL);
    b.data = realloc(b.data, b.len + 12);
    b.data[b.len - 1] = 1;
    memcpy(b.data + b.len, "\0\0\0\0key ????", 12);
    b.len += 12;
    CHECK_EQ_INT(pc_psd_desc_parse(b.data, b.len, &p, &e), PC_ERR_UNSUPPORTED);
    CHECK_EQ_STR(e.msg, "unsupported: descriptor OSType \"????\"");
    pc_psd_bytes_fini(&b);
    /* deep nesting is limited */
    for (i = 0; i < PC_PSD_DESC_MAX_DEPTH + 5; i++) {
        pc_psd_desc outer;
        pc_psd_value v;
        pc_psd_desc_init(&outer, "null", NULL);
        pc_psd_value_object(&d, &v, NULL);
        add(&outer, "in  ", v);
        d = outer;
    }
    pc_psd_desc_to_bytes(&d, &b, NULL);
    CHECK_EQ_INT(pc_psd_desc_parse(b.data, b.len, &p, &e), PC_ERR_LIMIT);
    CHECK_EQ_STR(e.msg, "limit exceeded: descriptor nesting too deep");
    pc_psd_bytes_fini(&b);
    pc_psd_desc_fini(&d);
    /* huge counts rejected without allocating */
    psd_w_init(&w, NULL);
    psd_put_u32(&w, 0);
    psd_put_u32(&w, 0);
    psd_put(&w, "null", 4);
    psd_put_u32(&w, 0xFFFFFFFFu);
    psd_w_finish(&w, &b);
    CHECK(pc_psd_desc_parse(b.data, b.len, &p, NULL) != PC_OK);
    pc_psd_bytes_fini(&b);
    /* lossy strings */
    CHECK_EQ_INT(pc_psd_ustr_from_utf8("ab", true, &s, NULL), PC_OK);
    CHECK(s.n == 3);
    CHECK_EQ_INT(pc_psd_ustr_to_utf8(&s, &t, NULL), PC_OK);
    CHECK_EQ_STR(t, "ab");
    free(t);
    pc_psd_ustr_fini(&s);
    s.u = lone;
    s.n = 2;
    CHECK_EQ_INT(pc_psd_ustr_to_utf8(&s, &t, NULL), PC_OK);
    CHECK_EQ_STR(t, "\xEF\xBF\xBD" "A");
    free(t);
}

/* descriptor proptests: random byte soup never crashes; mutated real descriptors either fail or
 * re-serialise identically. */
static void test_descriptor_fuzz(void)
{
    pc_psd_desc d = desc_sample();
    pc_psd_bytes b = {NULL, 0};
    int k;
    int bad = 0;
    pc_psd_desc_to_bytes(&d, &b, NULL);
    for (k = 0; k < 3000; k++) {
        uint8_t soup[256];
        size_t n = rnd() % 256;
        size_t i;
        pc_psd_desc p;
        uint8_t *m = malloc(b.len);
        for (i = 0; i < n; i++) {
            soup[i] = (uint8_t)rnd();
        }
        if (pc_psd_desc_parse(soup, n, &p, NULL) == PC_OK) {
            pc_psd_desc_fini(&p);
        }
        if (pc_psd_vdesc_parse(soup, n, &p, NULL) == PC_OK) {
            pc_psd_desc_fini(&p);
        }
        memcpy(m, b.data, b.len);
        for (i = 0; i < 1 + rnd() % 4; i++) {
            m[rnd() % b.len] = (uint8_t)rnd();
        }
        if (pc_psd_desc_parse(m, b.len, &p, NULL) == PC_OK) {
            /* What parses writes back and parses to the same model (a bool byte of 2 becomes 1). */
            pc_psd_bytes again = {NULL, 0};
            pc_psd_desc q;
            bad += pc_psd_desc_to_bytes(&p, &again, NULL) != PC_OK;
            if (pc_psd_desc_parse(again.data, again.len, &q, NULL) == PC_OK) {
                bad += !pc_psd_desc_eq(&p, &q);
                pc_psd_desc_fini(&q);
            } else {
                bad++;
            }
            pc_psd_bytes_fini(&again);
            pc_psd_desc_fini(&p);
        }
        free(m);
    }
    CHECK_EQ_INT(bad, 0);
    pc_psd_bytes_fini(&b);
    pc_psd_desc_fini(&d);
}

/* ---- metadata.rs, hdr.rs ---- */

static void test_metadata(void)
{
    static const uint8_t three[3] = {1, 2, 3};
    static const uint8_t nine[4] = {9, 9, 9, 9};
    static const uint8_t two[2];
    static const uint8_t bad[4] = {0, 0, 0, 9};
    pc_psd_meta_item items[2];
    pc_psd_meta_item *back = NULL;
    size_t n = 0;
    pc_psd_bytes b = {NULL, 0};
    pc_psd_bytes again = {NULL, 0};
    CHECK_EQ_INT(pc_psd_meta_item_make("cmls", three, 3, &items[0], NULL), PC_OK);
    CHECK_EQ_UINT(items[0].data.len, 4);
    CHECK_EQ_INT(pc_psd_meta_item_make("cust", nine, 4, &items[1], NULL), PC_OK);
    items[1].copy = true;
    CHECK_EQ_INT(pc_psd_write_shmd(items, 2, &b, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_parse_shmd(b.data, b.len, &back, &n, NULL), PC_OK);
    CHECK(n == 2 && pc_psd_meta_item_eq(&back[0], &items[0]) && pc_psd_meta_item_eq(&back[1], &items[1]));
    CHECK_EQ_INT(pc_psd_write_shmd(back, n, &again, NULL), PC_OK);
    CHECK(pc_psd_bytes_eq(&again, &b));
    pc_psd_meta_items_free(back, n);
    pc_psd_bytes_fini(&b);
    pc_psd_bytes_fini(&again);
    CHECK(pc_psd_parse_shmd(NULL, 0, &back, &n, NULL) != PC_OK);
    CHECK(pc_psd_parse_shmd(bad, 4, &back, &n, NULL) != PC_OK);
    pc_psd_bytes_fini(&items[0].data);
    CHECK_EQ_INT(pc_psd_meta_item_make("cmls", two, 2, &items[0], NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_write_shmd(items, 1, &b, NULL), PC_OK);
    b.data = realloc(b.data, b.len + 1);
    b.data[b.len] = 0;
    CHECK(pc_psd_parse_shmd(b.data, b.len + 1, &back, &n, NULL) != PC_OK);
    CHECK(pc_psd_parse_shmd(b.data, b.len - 2, &back, &n, NULL) != PC_OK);
    pc_psd_bytes_fini(&b);
    pc_psd_bytes_fini(&items[0].data);
    pc_psd_bytes_fini(&items[1].data);
}

static void test_hdr_default_record(void)
{
    pc_psd_bytes d = {NULL, 0};
    pc_psd_bytes e = {NULL, 0};
    CHECK_EQ_INT(pc_psd_default_toning_data(&d, NULL), PC_OK);
    CHECK_EQ_UINT(d.len, 110);
    CHECK(pc_psd_is_toning_data(d.data, d.len));
    CHECK(memcmp(d.data + 72, "hdra", 4) == 0);
    CHECK(pc_load_be32(d.data + 4) == 3 && pc_load_be32(d.data + 76) == 6);
    CHECK_EQ_INT(pc_psd_color_mode_data_for_depth(8, &e, NULL), PC_OK);
    CHECK_EQ_UINT(e.len, 0);
    CHECK_EQ_INT(pc_psd_color_mode_data_for_depth(32, &e, NULL), PC_OK);
    CHECK(pc_psd_bytes_eq(&d, &e));
    pc_psd_bytes_fini(&d);
    pc_psd_bytes_fini(&e);
}

/* ---- path.rs ---- */

static void sample_subpaths(pc_psd_subpath s[2], pc_psd_knot k[4])
{
    static const double v[4][7] = {{0, .25, .25, .25, .25, .25, .25},
                                   {1, .625, .125, .75, .25, .875, .375},
                                   {0, .5, .75, .5, .75, .5, .75},
                                   {0, -.5, 1.5, 0.0, 1.0, .0625, .9375}};
    int i;
    for (i = 0; i < 4; i++) {
        k[i].linked = v[i][0] != 0;
        k[i].pre[0] = v[i][1];
        k[i].pre[1] = v[i][2];
        k[i].anchor[0] = v[i][3];
        k[i].anchor[1] = v[i][4];
        k[i].post[0] = v[i][5];
        k[i].post[1] = v[i][6];
    }
    s[0].closed = true;
    s[0].operation = 1;
    s[0].knots = k;
    s[0].n = 3;
    s[1].closed = false;
    s[1].operation = 2;
    s[1].knots = k + 3;
    s[1].n = 1;
}

static bool subpaths_eq(const pc_psd_subpath *a, const pc_psd_subpath *b, size_t n)
{
    size_t i;
    size_t k;
    for (i = 0; i < n; i++) {
        if (a[i].closed != b[i].closed || a[i].operation != b[i].operation || a[i].n != b[i].n) {
            return false;
        }
        for (k = 0; k < a[i].n; k++) {
            const pc_psd_knot *x = &a[i].knots[k];
            const pc_psd_knot *y = &b[i].knots[k];
            if (x->linked != y->linked || x->pre[0] != y->pre[0] || x->pre[1] != y->pre[1] ||
                x->anchor[0] != y->anchor[0] || x->anchor[1] != y->anchor[1] || x->post[0] != y->post[0] ||
                x->post[1] != y->post[1]) {
                return false;
            }
        }
    }
    return true;
}

static void test_paths(void)
{
    pc_psd_subpath s[2];
    pc_psd_knot k[4];
    pc_psd_path p;
    pc_psd_path q;
    pc_psd_bytes b = {NULL, 0};
    pc_psd_bytes b2 = {NULL, 0};
    pc_psd_subpath *got = NULL;
    size_t n = 0;
    pc_psd_vector_mask vm;
    pc_psd_vector_mask vm2;
    static const uint8_t four[4];
    size_t i;
    size_t j;
    sample_subpaths(s, k);
    CHECK_EQ_INT(pc_psd_path_from_subpaths(s, 2, true, &p, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_path_to_bytes(&p, &b, NULL), PC_OK);
    CHECK_EQ_UINT(b.len, (2 + 2 + 4) * 26u);
    CHECK_EQ_INT(pc_psd_path_parse(b.data, b.len, &q, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_path_to_bytes(&q, &b2, NULL), PC_OK);
    CHECK(pc_psd_bytes_eq(&b, &b2));
    CHECK_EQ_INT(pc_psd_path_subpaths(&q, &got, &n, NULL), PC_OK);
    CHECK(n == 2 && subpaths_eq(got, s, 2));
    CHECK(pc_psd_path_initial_fill(&q));
    pc_psd_subpaths_free(got, n);
    pc_psd_bytes_fini(&b2);
    /* vector mask block */
    vm.version = 3;
    vm.flags = 1;
    vm.path = p;
    CHECK_EQ_INT(pc_psd_vector_mask_to_bytes(&vm, &b2, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_vector_mask_parse(b2.data, b2.len, &vm2, NULL), PC_OK);
    CHECK(vm2.version == 3 && vm2.flags == 1 && vm2.path.n == p.n &&
          memcmp(vm2.path.records, p.records, p.n * sizeof *p.records) == 0);
    pc_psd_path_fini(&vm2.path);
    CHECK(pc_psd_vector_mask_parse(four, 4, &vm2, NULL) != PC_OK);
    pc_psd_bytes_fini(&b2);
    /* fixed point */
    CHECK_EQ_INT(pc_psd_f64_to_fixed(1.0), 1 << 24);
    CHECK(pc_psd_fixed_to_f64(-(1 << 23)) == -0.5);
    CHECK_EQ_INT(pc_psd_f64_to_fixed(1e12), INT32_MAX);
    /* lenient knots without length records */
    for (i = 0, j = 0; i < q.n; i++) {
        if (q.records[i].selector != PC_PSD_PATH_CLOSED_LENGTH && q.records[i].selector != PC_PSD_PATH_OPEN_LENGTH) {
            q.records[j++] = q.records[i];
        }
    }
    q.n = j;
    CHECK_EQ_INT(pc_psd_path_subpaths(&q, &got, &n, NULL), PC_OK);
    CHECK(n == 2 && got[0].n == 3);
    pc_psd_subpaths_free(got, n);
    pc_psd_path_fini(&p);
    pc_psd_path_fini(&q);
    pc_psd_bytes_fini(&b);
}

/* ---- slices.rs ---- */

static void set(char **f, const char *s)
{
    free(*f);
    *f = strdup(s);
}

static pc_psd_slices slices_sample(void)
{
    pc_psd_slices s;
    int i;
    memset(&s, 0, sizeof s);
    s.version = 6;
    s.bounds[2] = 80;
    s.bounds[3] = 100;
    s.group_name = strdup("site");
    s.n = 3;
    s.slices = calloc(3, sizeof *s.slices);
    for (i = 0; i < 3; i++) {
        CHECK_EQ_INT(pc_psd_slice_init(&s.slices[i], NULL), PC_OK);
    }
    s.slices[0].kind = 1;
    s.slices[0].rect[2] = 100;
    s.slices[0].rect[3] = 10;
    s.slices[1].id = 1;
    s.slices[1].origin = 2;
    set(&s.slices[1].name, "logo");
    s.slices[1].kind = 1;
    s.slices[1].rect[0] = 20;
    s.slices[1].rect[1] = 10;
    s.slices[1].rect[2] = 60;
    s.slices[1].rect[3] = 30;
    set(&s.slices[1].url, "https://example.org/");
    set(&s.slices[1].target, "_blank");
    set(&s.slices[1].message, "hi");
    set(&s.slices[1].alt, "Logo");
    s.slices[1].cell_text_is_html = true;
    set(&s.slices[1].cell_text, "<b>x</b>");
    s.slices[1].horizontal_align = 1;
    s.slices[1].vertical_align = 2;
    s.slices[1].color[0] = 255;
    s.slices[1].color[1] = 10;
    s.slices[1].color[2] = 20;
    s.slices[1].color[3] = 30;
    s.slices[2].id = 2;
    s.slices[2].origin = 1;
    s.slices[2].has_layer_id = true;
    s.slices[2].layer_id = 7;
    s.slices[2].rect[0] = 5;
    s.slices[2].rect[1] = 40;
    s.slices[2].rect[2] = 25;
    s.slices[2].rect[3] = 70;
    for (i = 0; i < 4; i++) {
        s.slices[2].outsets[i] = i + 1;
    }
    return s;
}

static void test_slices(void)
{
    pc_psd_slices s = slices_sample();
    pc_psd_slices back;
    pc_psd_bytes b = {NULL, 0};
    pc_psd_bytes b2 = {NULL, 0};
    uint8_t huge[28];
    uint8_t nine[4] = {0, 0, 0, 9};
    static const size_t cuts[5] = {0, 3, 10, 30, 120};
    int i;
    CHECK_EQ_INT(pc_psd_slices_to_bytes(&s, &b, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_slices_parse(b.data, b.len, &back, NULL), PC_OK);
    CHECK(pc_psd_slices_eq(&back, &s));
    CHECK_EQ_INT(pc_psd_slices_to_bytes(&back, &b2, NULL), PC_OK);
    CHECK(pc_psd_bytes_eq(&b, &b2));
    pc_psd_slices_fini(&back);
    pc_psd_bytes_fini(&b2);
    for (i = 0; i < 5; i++) {
        CHECK(pc_psd_slices_parse(b.data, cuts[i], &back, NULL) != PC_OK);
    }
    memset(huge, 0, sizeof huge);
    pc_store_be32(huge, 6);
    pc_store_be32(huge + 24, 0xFFFFFFFFu);
    CHECK(pc_psd_slices_parse(huge, 28, &back, NULL) != PC_OK);
    CHECK(pc_psd_slices_parse(nine, 4, &back, NULL) != PC_OK);
    pc_psd_bytes_fini(&b);
    /* v7 descriptor form */
    CHECK_EQ_INT(pc_psd_slices_to_descriptor_bytes(&s, &b, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_slices_parse(b.data, b.len, &back, NULL), PC_OK);
    CHECK(back.version == 7 && strcmp(back.group_name, "site") == 0 && back.n == 3);
    CHECK(memcmp(back.bounds, s.bounds, sizeof s.bounds) == 0);
    CHECK(memcmp(back.slices[1].rect, s.slices[1].rect, sizeof s.slices[1].rect) == 0);
    CHECK_EQ_STR(back.slices[1].alt, "Logo");
    CHECK_EQ_STR(back.slices[1].target, "_blank");
    CHECK(back.slices[2].origin == 1 && back.slices[2].has_layer_id && back.slices[2].layer_id == 7);
    CHECK(back.slices[2].kind == 0 && memcmp(back.slices[2].outsets, s.slices[2].outsets, 16) == 0);
    pc_psd_slices_fini(&back);
    pc_psd_bytes_fini(&b);
    pc_psd_slices_fini(&s);
}

/* ---- filter_effects.rs ---- */

static pc_psd_fx_item fx_item(const char *id, bool mask)
{
    pc_psd_fx_item it;
    uint8_t ten[16];
    uint8_t full[16];
    uint8_t half[16];
    memset(ten, 10, 16);
    memset(full, 255, 16);
    memset(half, 128, 16);
    memset(&it, 0, sizeof it);
    it.id = strdup(id);
    it.version = 1;
    it.rect.top = 1;
    it.rect.left = 2;
    it.rect.bottom = 5;
    it.rect.right = 6;
    it.depth = 8;
    it.max_channels = 24;
    it.nslots = 26;
    it.slots = calloc(26, sizeof *it.slots);
    it.slot_written = calloc(26, sizeof *it.slot_written);
    pc_psd_fx_plane_encode(PC_PSD_RLE, ten, 16, 4, 4, 8, &it.slots[0], NULL);
    pc_psd_fx_plane_encode(PC_PSD_RLE, full, 16, 4, 4, 8, &it.slots[25], NULL);
    it.slot_written[0] = true;
    it.slot_written[25] = true;
    if (mask) {
        it.has_mask = true;
        it.mask_rect = it.rect;
        pc_psd_fx_plane_encode(PC_PSD_RLE, half, 16, 4, 4, 8, &it.mask, NULL);
    }
    return it;
}

static void test_filter_effects(void)
{
    pc_psd_fx_item items[2];
    pc_psd_filter_effects fx;
    pc_psd_filter_effects back;
    pc_psd_filter_effects empty;
    pc_psd_bytes b = {NULL, 0};
    pc_psd_bytes eb = {NULL, 0};
    pc_psd_bytes d = {NULL, 0};
    uint8_t *padded;
    size_t n;
    int k;
    int bad = 0;
    items[0] = fx_item("a-1", true);
    items[1] = fx_item("b-2", false);
    fx.version = 3;
    fx.items = items;
    fx.n = 2;
    CHECK_EQ_INT(pc_psd_filter_effects_to_bytes(&fx, &b, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_filter_effects_parse(b.data, b.len, &back, NULL), PC_OK);
    CHECK(back.n == 2 && pc_psd_fx_item_eq(&back.items[0], &items[0]) && pc_psd_fx_item_eq(&back.items[1], &items[1]));
    CHECK_EQ_INT(pc_psd_fx_plane_decode(&back.items[0].mask, 4, 4, 8, &d, NULL), PC_OK);
    CHECK(d.len == 16 && d.data[0] == 128);
    pc_psd_bytes_fini(&d);
    pc_psd_filter_effects_fini(&back);
    empty.version = 3;
    empty.items = NULL;
    empty.n = 0;
    CHECK_EQ_INT(pc_psd_filter_effects_to_bytes(&empty, &eb, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_filter_effects_parse(eb.data, eb.len, &back, NULL), PC_OK);
    CHECK(back.n == 0);
    pc_psd_filter_effects_fini(&back);
    pc_psd_bytes_fini(&eb);
    n = b.len;
    padded = calloc(n + 3, 1);
    memcpy(padded, b.data, n);
    CHECK_EQ_INT(pc_psd_filter_effects_parse(padded, n + 2, &back, NULL), PC_OK);
    pc_psd_filter_effects_fini(&back);
    padded[n + 2] = 7;
    CHECK(pc_psd_filter_effects_parse(padded, n + 3, &back, NULL) != PC_OK);
    free(padded);
    /* mutations never crash */
    for (k = 0; k < 2000; k++) {
        uint8_t *m = malloc(b.len);
        int j;
        memcpy(m, b.data, b.len);
        for (j = 0; j < 1 + (int)(rnd() % 4); j++) {
            m[rnd() % b.len] = (uint8_t)rnd();
        }
        if (pc_psd_filter_effects_parse(m, b.len, &back, NULL) == PC_OK) {
            size_t i;
            pc_psd_bytes again = {NULL, 0};
            for (i = 0; i < back.n; i++) {
                size_t s;
                if (back.items[i].has_mask) {
                    if (pc_psd_fx_plane_decode(&back.items[i].mask, 4, 4, 8, &d, NULL) == PC_OK) {
                        pc_psd_bytes_fini(&d);
                    }
                }
                for (s = 0; s < back.items[i].nslots; s++) {
                    if (back.items[i].slot_written[s] &&
                        pc_psd_fx_plane_decode(&back.items[i].slots[s], 4, 4, 8, &d, NULL) == PC_OK) {
                        pc_psd_bytes_fini(&d);
                    }
                }
            }
            bad += pc_psd_filter_effects_to_bytes(&back, &again, NULL) != PC_OK;
            pc_psd_bytes_fini(&again);
            pc_psd_filter_effects_fini(&back);
        }
        free(m);
    }
    CHECK_EQ_INT(bad, 0);
    /* malformed: truncations inside the section, bad version, huge channel count */
    for (n = 0; n + 4 < b.len / 2; n++) {
        bad += pc_psd_filter_effects_parse(b.data, n, &back, NULL) == PC_OK;
    }
    CHECK_EQ_INT(bad, 0);
    b.data[3] = 9;
    CHECK(pc_psd_filter_effects_parse(b.data, b.len, &back, NULL) != PC_OK);
    b.data[3] = 3;
    pc_store_be32(b.data + 4 + 8 + 2 + 3 + 4 + 8 + 16 + 4, 0xFFFFFFFFu); /* "a-1": pascal is 4 bytes */
    CHECK(pc_psd_filter_effects_parse(b.data, b.len, &back, NULL) != PC_OK);
    pc_psd_bytes_fini(&b);
    pc_psd_fx_item_fini(&items[0]);
    pc_psd_fx_item_fini(&items[1]);
}

int main(int argc, char **argv)
{
    RUN(test_reader_reads_big_endian);
    RUN(test_reader_eof_reports_needed);
    RUN(test_check_count_rejects_huge);
    RUN(test_len_patch_roundtrip);
    RUN(test_pascal_padding);
    RUN(test_legacy_names);
    RUN(test_unicode_units_roundtrip);
    RUN(test_header_roundtrip);
    RUN(test_header_errors);
    RUN(test_row_bytes_values);
    RUN(test_blend_keys);
    RUN(test_packbits_vectors);
    RUN(test_packbits_decode_edges);
    RUN(test_all_codecs_roundtrip_all_depths);
    RUN(test_rle_count_width_and_truncation);
    RUN(test_codec_errors_and_limits);
    RUN(test_zip_edges);
    RUN(test_prediction_vectors);
    RUN(test_codec_properties);
    RUN(test_resources_section);
    RUN(test_typed_resources);
    RUN(test_descriptor_roundtrip_all_types);
    RUN(test_descriptor_ids_and_errors);
    RUN(test_descriptor_fuzz);
    RUN(test_metadata);
    RUN(test_hdr_default_record);
    RUN(test_paths);
    RUN(test_slices);
    RUN(test_filter_effects);
    return pc_t_finish();
}
