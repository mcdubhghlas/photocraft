/* Decompression-bomb guards (port of crates/codecs/tests/limits.rs). */
#include "base/compress.h"
#include "common.h"

static pc_cdecode_opts opts(pc_climits l)
{
    pc_cdecode_opts o = pc_cdecode_opts_default();
    o.limits = l;
    return o;
}

/* Decodes and reports the status (PC_OK images are freed). */
static pc_status try_dec(const uint8_t *b, size_t n, const pc_cdecode_opts *o)
{
    pc_cimage img;
    pc_status s = o != NULL ? pc_cdecode_with(b, n, o, &img, NULL) : pc_cdecode(b, n, &img, NULL);
    if (s == PC_OK) {
        pc_cimage_fini(&img);
    }
    return s;
}

static bool is_limit(pc_status s) { return s == PC_ERR_LIMIT; }

static pc_buf encoded(pc_cformat f)
{
    pc_cimage img = synth(64, 48, PC_CL_RGBA, PC_CS_U8, 1, 0.1f);
    pc_buf b = enc(&img, f, NULL);
    pc_cimage_fini(&img);
    return b;
}

static void test_max_pixels_enforced_for_every_format(void)
{
    int f;
    for (f = 0; f < PC_CF_COUNT; f++) {
        pc_buf b;
        pc_climits l = pc_climits_default();
        pc_cdecode_opts o;
        if (!rw((pc_cformat)f)) {
            continue;
        }
        pc_t_name = pc_cformat_debug((pc_cformat)f);
        b = encoded((pc_cformat)f);
        l.max_pixels = 1000;
        o = opts(l);
        CHECK(is_limit(try_dec(b.data, b.len, &o)));
        l.max_pixels = 64 * 48; /* the exact limit passes */
        o = opts(l);
        CHECK(try_dec(b.data, b.len, &o) == PC_OK);
        pc_buf_fini(&b);
    }
}

static void test_max_width_enforced_for_every_format(void)
{
    int f;
    for (f = 0; f < PC_CF_COUNT; f++) {
        pc_buf b;
        pc_climits l = pc_climits_default();
        pc_cdecode_opts o;
        if (!rw((pc_cformat)f)) {
            continue;
        }
        pc_t_name = pc_cformat_debug((pc_cformat)f);
        b = encoded((pc_cformat)f);
        l.max_width = 63;
        o = opts(l);
        CHECK(is_limit(try_dec(b.data, b.len, &o)));
        pc_buf_fini(&b);
    }
}

static void test_max_height_enforced_for_every_format(void)
{
    int f;
    for (f = 0; f < PC_CF_COUNT; f++) {
        pc_buf b;
        pc_climits l = pc_climits_default();
        pc_cdecode_opts o;
        if (!rw((pc_cformat)f)) {
            continue;
        }
        pc_t_name = pc_cformat_debug((pc_cformat)f);
        b = encoded((pc_cformat)f);
        l.max_height = 47;
        o = opts(l);
        CHECK(is_limit(try_dec(b.data, b.len, &o)));
        pc_buf_fini(&b);
    }
}

static void test_max_alloc_enforced_for_every_format(void)
{
    int f;
    for (f = 0; f < PC_CF_COUNT; f++) {
        pc_buf b;
        pc_climits l = pc_climits_default();
        pc_cdecode_opts o;
        if (!rw((pc_cformat)f)) {
            continue;
        }
        pc_t_name = pc_cformat_debug((pc_cformat)f);
        b = encoded((pc_cformat)f);
        l.max_alloc = 64 * 48;
        o = opts(l);
        CHECK(try_dec(b.data, b.len, &o) != PC_OK);
        pc_buf_fini(&b);
    }
}

static void test_limits_none_accepts(void)
{
    int f;
    pc_cdecode_opts o = opts(pc_climits_none());
    for (f = 0; f < PC_CF_COUNT; f++) {
        pc_buf b;
        if (!rw((pc_cformat)f)) {
            continue;
        }
        pc_t_name = pc_cformat_debug((pc_cformat)f);
        b = encoded((pc_cformat)f);
        CHECK(try_dec(b.data, b.len, &o) == PC_OK);
        pc_buf_fini(&b);
    }
}

/* ---- PNG fixtures ---- */

static void put_be32(pc_buf *b, uint32_t v)
{
    uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    (void)pc_buf_append(b, x, 4, NULL);
}

/* Appends one chunk with a correct CRC. */
static void png_chunk(pc_buf *out, const char *ty, const void *data, size_t n)
{
    uint32_t crc = pc_crc32(0, (const uint8_t *)ty, 4);
    crc = pc_crc32(crc, (const uint8_t *)data, n);
    put_be32(out, (uint32_t)n);
    (void)pc_buf_append(out, ty, 4, NULL);
    (void)pc_buf_append(out, data, n, NULL);
    put_be32(out, crc);
}

static void test_png_bomb_header_rejected_before_alloc(void)
{
    static const uint8_t IHDR[13] = {0, 1, 0x86, 0xA0, 0, 1, 0x86, 0xA0, 16, 6, 0, 0, 0}; /* 100000 x 100000 */
    static const uint8_t IDAT[8] = {0x78, 0x9C, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
    pc_buf b = PC_BUF_INIT;
    (void)pc_buf_append(&b, "\x89PNG\r\n\x1a\n", 8, NULL);
    png_chunk(&b, "IHDR", IHDR, sizeof IHDR);
    png_chunk(&b, "IDAT", IDAT, sizeof IDAT);
    png_chunk(&b, "IEND", "", 0);
    CHECK(is_limit(try_dec(b.data, b.len, NULL)));
    pc_buf_fini(&b);
}

enum { PNG_TEXT_BUDGET = 64 << 10 };

static pc_status png_text_decode(const pc_buf *bytes, size_t max_alloc, pc_cimage *out)
{
    pc_climits l = pc_climits_default();
    pc_cdecode_opts o;
    pc_cimage img;
    pc_status s;
    l.max_alloc = max_alloc;
    o = opts(l);
    s = pc_cdecode_with(bytes->data, bytes->len, &o, &img, NULL);
    if (s == PC_OK) {
        if (out != NULL) {
            *out = img;
        } else {
            pc_cimage_fini(&img);
        }
    }
    return s;
}

static pc_buf zlib(const void *text, size_t n)
{
    pc_buf b = PC_BUF_INIT;
    if (pc_deflate(PC_Z_ZLIB, PC_Z_FAST, (const uint8_t *)text, n, &b, NULL) != PC_OK) {
        PC_T_FAIL("deflate");
    }
    return b;
}

/* Appends a text chunk; zTXt and iTXt payloads supplied here are already compressed. */
static void png_text_stream(pc_buf *out, const char *ty, const void *kw, size_t kwn, const void *payload, size_t n)
{
    pc_buf d = PC_BUF_INIT;
    (void)pc_buf_append(&d, kw, kwn, NULL);
    (void)pc_buf_append_byte(&d, 0, NULL);
    if (strcmp(ty, "zTXt") == 0) {
        (void)pc_buf_append_byte(&d, 0, NULL);
    } else if (strcmp(ty, "iTXt") == 0) {
        (void)pc_buf_append(&d, "\x01\0\0\0", 4, NULL);
    }
    (void)pc_buf_append(&d, payload, n, NULL);
    png_chunk(out, ty, d.data, d.len);
    pc_buf_fini(&d);
}

static void png_text(pc_buf *out, const char *ty, const void *kw, size_t kwn, const void *text, size_t n)
{
    if (strcmp(ty, "tEXt") == 0) {
        png_text_stream(out, ty, kw, kwn, text, n);
    } else {
        pc_buf z = zlib(text, n);
        png_text_stream(out, ty, kw, kwn, z.data, z.len);
        pc_buf_fini(&z);
    }
}

static uint32_t rd_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* A 1x1 8-bit grey PNG (our encoder; Rust used the png crate) with `before` spliced in before the
 * first IDAT and `after` before IEND. Either may be NULL. */
static pc_buf png_with_text(const pc_buf *before, const pc_buf *after)
{
    static const uint8_t PIX = 127;
    pc_cimage img;
    pc_buf image = PC_BUF_INIT;
    pc_buf out = PC_BUF_INIT;
    size_t at = 8;
    size_t idat = 0;
    bool have_idat = false;
    size_t iend = 0;
    if (pc_cimage_from_copy(1, 1, PC_CL_GRAY, PC_CS_U8, &PIX, 1, &img, NULL) != PC_OK) {
        PC_T_FAIL("image");
        return out;
    }
    image = enc(&img, PC_CF_PNG, NULL);
    pc_cimage_fini(&img);
    while (at + 8 <= image.len) {
        size_t len = rd_be32(image.data + at);
        const uint8_t *ty = image.data + at + 4;
        if (memcmp(ty, "IDAT", 4) == 0 && !have_idat) {
            idat = at;
            have_idat = true;
        }
        if (memcmp(ty, "IEND", 4) == 0) {
            iend = at;
            break;
        }
        at += len + 12;
    }
    if (!have_idat || iend == 0) {
        PC_T_FAIL("PNG fixture has no IDAT/IEND");
        pc_buf_fini(&image);
        return out;
    }
    (void)pc_buf_append(&out, image.data, idat, NULL);
    if (before != NULL) {
        (void)pc_buf_append(&out, before->data, before->len, NULL);
    }
    (void)pc_buf_append(&out, image.data + idat, iend - idat, NULL);
    if (after != NULL) {
        (void)pc_buf_append(&out, after->data, after->len, NULL);
    }
    (void)pc_buf_append(&out, image.data + iend, image.len - iend, NULL);
    pc_buf_fini(&image);
    return out;
}

static uint8_t *filled(uint8_t v, size_t n)
{
    uint8_t *p = malloc(n > 0 ? n : 1);
    memset(p, v, n);
    return p;
}

/* meta.text == [(key, value)] */
static void check_one_text(const pc_cimage *img, const char *key, const void *value, size_t n)
{
    CHECK_EQ_UINT(img->meta.ntext, 1);
    if (img->meta.ntext == 1) {
        CHECK_EQ_STR(img->meta.text[0].key, key);
        CHECK_EQ_UINT(img->meta.text[0].value_len, n);
        if (img->meta.text[0].value_len == n) {
            CHECK_EQ_MEM(img->meta.text[0].value, value, n);
        }
    }
}

static const char *const TYS[3] = {"tEXt", "zTXt", "iTXt"};

static void test_png_compressed_text_bombs_obey_limits_before_and_after_idat(void)
{
    uint8_t *bomb = filled('A', PNG_TEXT_BUDGET * 8);
    size_t t;
    int after_idat;
    for (t = 1; t < 3; t++) {
        for (after_idat = 0; after_idat < 2; after_idat++) {
            pc_buf chunk = PC_BUF_INIT;
            pc_buf bytes;
            png_text(&chunk, TYS[t], "Comment", 7, bomb, PNG_TEXT_BUDGET * 8);
            bytes = after_idat ? png_with_text(NULL, &chunk) : png_with_text(&chunk, NULL);
            /* The fixture must fit comfortably within the compressed-input budget. */
            CHECK(bytes.len < PNG_TEXT_BUDGET / 2);
            if (!is_limit(png_text_decode(&bytes, PNG_TEXT_BUDGET, NULL))) {
                PC_T_FAIL("%s, after IDAT=%d", TYS[t], after_idat);
            }
            pc_buf_fini(&bytes);
            pc_buf_fini(&chunk);
        }
    }
    free(bomb);
}

static void test_png_text_chunks_share_an_aggregate_budget(void)
{
    static const size_t LENS[2] = {(PNG_TEXT_BUDGET - 3) / 3, PNG_TEXT_BUDGET / 3};
    size_t k;
    for (k = 0; k < 2; k++) {
        size_t text_len = LENS[k];
        uint8_t *text = filled('A', text_len);
        pc_buf before = PC_BUF_INIT;
        pc_buf after = PC_BUF_INIT;
        pc_buf bytes;
        pc_cimage img;
        pc_status s;
        png_text(&before, "tEXt", "A", 1, text, text_len);
        png_text(&before, "zTXt", "B", 1, text, text_len);
        png_text(&after, "iTXt", "C", 1, text, text_len);
        bytes = png_with_text(&before, &after);
        s = png_text_decode(&bytes, PNG_TEXT_BUDGET, &img);
        if (3 * (text_len + 1) <= PNG_TEXT_BUDGET) {
            CHECK(s == PC_OK);
            if (s == PC_OK) {
                size_t i;
                CHECK_EQ_UINT(img.meta.ntext, 3);
                for (i = 0; i < img.meta.ntext; i++) {
                    CHECK_EQ_UINT(img.meta.text[i].value_len, text_len);
                }
                pc_cimage_fini(&img);
            }
        } else {
            /* Chunks fitting individually must still share a budget. */
            CHECK(is_limit(s));
            if (s == PC_OK) {
                pc_cimage_fini(&img);
            }
        }
        pc_buf_fini(&bytes);
        pc_buf_fini(&before);
        pc_buf_fini(&after);
        free(text);
    }
}

static void test_png_text_accepts_the_exact_utf8_budget(void)
{
    size_t t;
    for (t = 0; t < 3; t++) {
        uint8_t *text = filled('A', PNG_TEXT_BUDGET - 1);
        pc_buf chunk = PC_BUF_INIT;
        pc_buf bytes;
        pc_cimage img;
        png_text(&chunk, TYS[t], "K", 1, text, PNG_TEXT_BUDGET - 1);
        bytes = png_with_text(&chunk, NULL);
        if (png_text_decode(&bytes, PNG_TEXT_BUDGET, &img) == PC_OK) {
            check_one_text(&img, "K", text, PNG_TEXT_BUDGET - 1);
            pc_cimage_fini(&img);
        } else {
            PC_T_FAIL("%s: decode failed", TYS[t]);
        }
        /* Keyword and text both count. */
        if (!is_limit(png_text_decode(&bytes, PNG_TEXT_BUDGET - 1, NULL))) {
            PC_T_FAIL("%s: keyword and text both count", TYS[t]);
        }
        pc_buf_fini(&bytes);
        pc_buf_fini(&chunk);
        free(text);
    }
}

typedef struct bytes {
    const char *d;
    size_t n;
} lit;
#define B(s) {s, sizeof(s) - 1}

static void test_png_plain_itxt_obeys_the_exact_budget_and_validates_fields(void)
{
    static const lit INVALID[] = {
        B("K\0\0\0\0\0\xFF"),     /* Invalid text UTF-8. */
        B("K\0\0\0\xFF\0\0text"), /* Non-ASCII language tag. */
        B("K\0\0\0\0\xFF\0text"), /* Invalid translated-keyword UTF-8. */
        B("K\0\x02\0\0\0text"),   /* Unknown compression flag. */
        B("K\0\x01\x01\0\0text"), /* Unknown compression method for compressed text. */
        B("K\0\0"),               /* Missing compression method. */
        B("K\0\0\0\0text"),       /* Missing translated-keyword separator. */
    };
    uint8_t *a = filled('A', PNG_TEXT_BUDGET - 1);
    pc_buf payload = PC_BUF_INIT;
    pc_buf chunk = PC_BUF_INIT;
    pc_buf bytes;
    pc_cimage img;
    size_t i;
    (void)pc_buf_append(&payload, "K\0\0\0en\0Keyword\0", 15, NULL);
    (void)pc_buf_append(&payload, a, PNG_TEXT_BUDGET - 1, NULL);
    png_chunk(&chunk, "iTXt", payload.data, payload.len);
    bytes = png_with_text(NULL, &chunk);
    if (png_text_decode(&bytes, PNG_TEXT_BUDGET, &img) == PC_OK) {
        check_one_text(&img, "K", a, PNG_TEXT_BUDGET - 1);
        pc_cimage_fini(&img);
    } else {
        PC_T_FAIL("decode failed");
    }
    CHECK(is_limit(png_text_decode(&bytes, PNG_TEXT_BUDGET - 1, NULL)));
    pc_buf_fini(&bytes);
    pc_buf_fini(&chunk);
    pc_buf_fini(&payload);
    free(a);

    for (i = 0; i < sizeof INVALID / sizeof INVALID[0]; i++) {
        pc_buf before = PC_BUF_INIT;
        pc_buf after = PC_BUF_INIT;
        png_chunk(&before, "iTXt", INVALID[i].d, INVALID[i].n);
        png_text(&after, "tEXt", "Good", 4, "retained", 8);
        bytes = png_with_text(&before, &after);
        if (png_text_decode(&bytes, PNG_TEXT_BUDGET, &img) == PC_OK) {
            check_one_text(&img, "Good", "retained", 8);
            pc_cimage_fini(&img);
        } else {
            PC_T_FAIL("invalid iTXt case %zu: decode failed", i);
        }
        pc_buf_fini(&bytes);
        pc_buf_fini(&before);
        pc_buf_fini(&after);
    }
}

static void test_png_latin1_expansion_counts_keyword_and_text_utf8_bytes(void)
{
    static const uint8_t KW = 0xE9;
    size_t text_len = (PNG_TEXT_BUDGET - 2) / 2;
    uint8_t *latin = filled(0xE9, text_len);
    uint8_t *utf8 = malloc(2 * text_len);
    size_t i;
    size_t t;
    for (i = 0; i < text_len; i++) {
        utf8[2 * i] = 0xC3;
        utf8[2 * i + 1] = 0xA9;
    }
    for (t = 0; t < 3; t++) {
        bool itxt = t == 2;
        pc_buf chunk = PC_BUF_INIT;
        pc_buf bytes;
        pc_cimage img;
        png_text(&chunk, TYS[t], &KW, 1, itxt ? utf8 : latin, itxt ? 2 * text_len : text_len);
        bytes = png_with_text(NULL, &chunk);
        if (png_text_decode(&bytes, PNG_TEXT_BUDGET, &img) == PC_OK) {
            check_one_text(&img, "\xC3\xA9", utf8, 2 * text_len);
            pc_cimage_fini(&img);
        } else {
            PC_T_FAIL("%s: decode failed", TYS[t]);
        }
        /* Latin-1 expands when stored as UTF-8. */
        if (!is_limit(png_text_decode(&bytes, PNG_TEXT_BUDGET - 1, NULL))) {
            PC_T_FAIL("%s: Latin-1 expands when stored as UTF-8", TYS[t]);
        }
        pc_buf_fini(&bytes);
        pc_buf_fini(&chunk);
    }
    free(latin);
    free(utf8);
}

static void test_png_duplicate_xmp_chunks_each_consume_budget(void)
{
    static const char KEYWORD[] = "XML:com.adobe.xmp";
    size_t kwn = sizeof KEYWORD - 1;
    size_t text_len = (PNG_TEXT_BUDGET - 2 * kwn) / 2;
    size_t extra;
    for (extra = 0; extra < 2; extra++) {
        uint8_t *first_text = filled('A', text_len + extra);
        uint8_t *last_text = filled('B', text_len);
        pc_buf first = PC_BUF_INIT;
        pc_buf last = PC_BUF_INIT;
        pc_buf bytes;
        pc_cimage img;
        pc_status s;
        png_text(&first, "iTXt", KEYWORD, kwn, first_text, text_len + extra);
        png_text(&last, "iTXt", KEYWORD, kwn, last_text, text_len);
        bytes = png_with_text(&first, &last);
        s = png_text_decode(&bytes, PNG_TEXT_BUDGET, &img);
        if (extra == 0) {
            CHECK(s == PC_OK);
            if (s == PC_OK) {
                CHECK(img.meta.xmp.has);
                CHECK_EQ_UINT(img.meta.xmp.len, text_len);
                if (img.meta.xmp.len == text_len) {
                    CHECK_EQ_MEM(img.meta.xmp.data, last_text, text_len);
                }
                CHECK_EQ_UINT(img.meta.ntext, 0);
                pc_cimage_fini(&img);
            }
        } else {
            /* Replacing an XMP packet must not reset its budget charge. */
            CHECK(is_limit(s));
            if (s == PC_OK) {
                pc_cimage_fini(&img);
            }
        }
        pc_buf_fini(&bytes);
        pc_buf_fini(&first);
        pc_buf_fini(&last);
        free(first_text);
        free(last_text);
    }
}

static void test_png_corrupt_text_is_still_skipped(void)
{
    pc_buf valid = zlib("discard", 7);
    pc_buf bad_header = PC_BUF_INIT;
    pc_buf truncated = PC_BUF_INIT;
    const pc_buf *streams[2];
    pc_buf chunk = PC_BUF_INIT;
    pc_buf bytes;
    pc_cimage img;
    size_t t;
    size_t k;
    (void)pc_buf_append(&bad_header, valid.data, valid.len, NULL);
    if (bad_header.len > 0) {
        bad_header.data[0] = 0;
    }
    (void)pc_buf_append(&truncated, valid.data, valid.len >= 4 ? valid.len - 4 : 0, NULL);
    streams[0] = &bad_header;
    streams[1] = &truncated;
    for (t = 1; t < 3; t++) {
        for (k = 0; k < 2; k++) {
            pc_buf before = PC_BUF_INIT;
            pc_buf after = PC_BUF_INIT;
            png_text_stream(&before, TYS[t], "Bad", 3, streams[k]->data, streams[k]->len);
            png_text(&after, "tEXt", "Good", 4, "retained", 8);
            bytes = png_with_text(&before, &after);
            if (png_text_decode(&bytes, PNG_TEXT_BUDGET, &img) == PC_OK) {
                check_one_text(&img, "Good", "retained", 8);
                pc_cimage_fini(&img);
            } else {
                PC_T_FAIL("%s stream %zu: decode failed", TYS[t], k);
            }
            pc_buf_fini(&bytes);
            pc_buf_fini(&before);
            pc_buf_fini(&after);
        }
    }
    png_text(&chunk, "iTXt", "Bad", 3, "\xFF", 1);
    bytes = png_with_text(NULL, &chunk);
    if (png_text_decode(&bytes, PNG_TEXT_BUDGET, &img) == PC_OK) {
        CHECK_EQ_UINT(img.meta.ntext, 0);
        pc_cimage_fini(&img);
    } else {
        PC_T_FAIL("invalid UTF-8 iTXt: decode failed");
    }
    pc_buf_fini(&bytes);
    pc_buf_fini(&chunk);
    pc_buf_fini(&valid);
    pc_buf_fini(&bad_header);
    pc_buf_fini(&truncated);
}

static void test_png_text_with_bad_crc_and_text_after_iend_are_ignored(void)
{
    uint8_t *bomb = filled('A', PNG_TEXT_BUDGET * 8);
    size_t t;
    for (t = 1; t < 3; t++) {
        pc_buf valid = PC_BUF_INIT;
        pc_buf bad_crc = PC_BUF_INIT;
        pc_buf bytes;
        pc_cimage img;
        int after_idat;
        png_text(&valid, TYS[t], "Comment", 7, bomb, PNG_TEXT_BUDGET * 8);
        (void)pc_buf_append(&bad_crc, valid.data, valid.len, NULL);
        bad_crc.data[bad_crc.len - 1] ^= 1;
        for (after_idat = 0; after_idat < 2; after_idat++) {
            bytes = after_idat ? png_with_text(NULL, &bad_crc) : png_with_text(&bad_crc, NULL);
            if (png_text_decode(&bytes, PNG_TEXT_BUDGET, &img) == PC_OK) {
                CHECK_EQ_UINT(img.meta.ntext, 0);
                pc_cimage_fini(&img);
            } else {
                PC_T_FAIL("%s bad CRC, after IDAT=%d: decode failed", TYS[t], after_idat);
            }
            pc_buf_fini(&bytes);
        }
        bytes = png_with_text(NULL, NULL);
        (void)pc_buf_append(&bytes, valid.data, valid.len, NULL);
        if (png_text_decode(&bytes, PNG_TEXT_BUDGET, &img) == PC_OK) {
            CHECK_EQ_UINT(img.meta.ntext, 0);
            pc_cimage_fini(&img);
        } else {
            PC_T_FAIL("%s after IEND: decode failed", TYS[t]);
        }
        pc_buf_fini(&bytes);
        pc_buf_fini(&valid);
        pc_buf_fini(&bad_crc);
    }
    free(bomb);
}

static void test_png_malformed_chunk_tails_remain_tolerated_after_pixels(void)
{
    static const lit TAILS[] = {
        {"", 0},
        B("\x00\x00\x00"),
        B("\x00\x00\x00\x10iTXtK\0\0"),
        B("\xFF\xFF\xFF\xFFiTXt"),
    };
    pc_buf before = PC_BUF_INIT;
    pc_buf after = PC_BUF_INIT;
    pc_buf base;
    size_t iend;
    size_t i;
    png_text(&before, "tEXt", "Good", 4, "retained", 8);
    png_chunk(&after, "vpAg", "", 0);
    base = png_with_text(&before, &after);
    iend = base.len - 12;
    for (i = 0; i < sizeof TAILS / sizeof TAILS[0]; i++) {
        pc_buf bytes = PC_BUF_INIT;
        pc_cimage img;
        (void)pc_buf_append(&bytes, base.data, iend, NULL);
        (void)pc_buf_append(&bytes, TAILS[i].d, TAILS[i].n, NULL);
        if (png_text_decode(&bytes, PNG_TEXT_BUDGET, &img) == PC_OK) {
            CHECK_EQ_UINT(img.width, 1);
            CHECK_EQ_UINT(img.height, 1);
            check_one_text(&img, "Good", "retained", 8);
            pc_cimage_fini(&img);
        } else {
            PC_T_FAIL("tail %zu: decode failed", i);
        }
        pc_buf_fini(&bytes);
    }
    pc_buf_fini(&base);
    pc_buf_fini(&before);
    pc_buf_fini(&after);
}

static void test_png_invalid_optional_text_does_not_spend_an_exhausted_budget(void)
{
    uint8_t *a = filled('A', PNG_TEXT_BUDGET - 1);
    pc_buf full = PC_BUF_INIT;
    pc_buf invalid = PC_BUF_INIT;
    pc_buf bytes;
    pc_cimage img;
    png_text(&full, "tEXt", "K", 1, a, PNG_TEXT_BUDGET - 1);
    png_chunk(&invalid, "iTXt", "Bad\0\0\0\0\0\xFF", 9);
    png_text_stream(&invalid, "zTXt", "Bad", 3, "invalid", 7);
    bytes = png_with_text(&full, &invalid);
    if (png_text_decode(&bytes, PNG_TEXT_BUDGET, &img) == PC_OK) {
        CHECK_EQ_UINT(img.meta.ntext, 1);
        if (img.meta.ntext >= 1) {
            CHECK_EQ_UINT(img.meta.text[0].value_len, PNG_TEXT_BUDGET - 1);
        }
        pc_cimage_fini(&img);
    } else {
        PC_T_FAIL("decode failed");
    }
    pc_buf_fini(&bytes);
    pc_buf_fini(&full);
    pc_buf_fini(&invalid);
    free(a);
}

/* ---- PNM ---- */

static pc_status try_str(const char *s, const pc_cdecode_opts *o) { return try_dec((const uint8_t *)s, strlen(s), o); }

static void test_pnm_bomb_header_rejected(void)
{
    static const char PPM[] = "P6\n200000 200000\n255\n\0\0\0";
    CHECK(is_limit(try_dec((const uint8_t *)PPM, sizeof PPM - 1, NULL)));
    CHECK(is_limit(
        try_str("P7\nWIDTH 99999\nHEIGHT 99999\nDEPTH 4\nMAXVAL 65535\nTUPLTYPE RGB_ALPHA\nENDHDR\n", NULL)));
    CHECK(is_limit(try_str("PF\n100000 100000\n-1.0\n", NULL)));
}

static void test_pnm_header_alone_reserves_nothing_without_limits(void)
{
    /* #1110: ASCII PGM/PPM reserved four bytes per declared sample before reading any. A
     * 2^31 x 2^31 header asked for more than isize::MAX, and PBM reserved 2^62 bytes before
     * finding the raster missing (an abort). */
    static const char *const CASES[4] = {
        "P1\n2147483648 2147483648\n",
        "P2\n2147483648 2147483648\n255\n",
        "P3\n2147483648 2147483648\n255\n",
        "P4\n2147483648 2147483648\n",
    };
    pc_cdecode_opts none = opts(pc_climits_none());
    size_t i;
    for (i = 0; i < 4; i++) {
        if (try_str(CASES[i], &none) == PC_OK) {
            PC_T_FAIL("decoded %s", CASES[i]);
        }
        pc_t_checks++;
    }
}

static void test_pnm_header_sizes_that_overflow_usize_are_errors_without_limits(void)
{
    static const char *const CASES[5] = {
        "P3\n4294967295 4294967295\n255\n",
        "P6\n4294967295 4294967295\n255\n",
        "P5\n4294967295 4294967295\n65535\n",
        "PF\n4294967295 4294967295\n-1.0\n",
        "P7\nWIDTH 4294967295\nHEIGHT 4294967295\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n",
    };
    pc_cdecode_opts none = opts(pc_climits_none());
    size_t i;
    for (i = 0; i < 5; i++) {
        if (try_str(CASES[i], &none) == PC_OK) {
            PC_T_FAIL("decoded %s", CASES[i]);
        }
        pc_t_checks++;
    }
}

static void check_dec(const char *s, const uint8_t *want, size_t n)
{
    pc_cimage img = dec((const uint8_t *)s, strlen(s));
    CHECK_EQ_UINT(img.len, n);
    if (img.len == n) {
        CHECK_EQ_MEM(img.data, want, n);
    }
    pc_cimage_fini(&img);
}

static void test_ascii_pnm_with_the_tightest_layout_still_decodes(void)
{
    /* The capacity bound for #1110 must not cut off files with no slack: PBM digits need no
     * separator, and the last ASCII sample needs no trailing whitespace. */
    check_dec("P1\n3 1\n010", (const uint8_t[]){255, 0, 255}, 3);
    check_dec("P2\n3 1\n255\n1 2 3", (const uint8_t[]){1, 2, 3}, 3);
    check_dec("P3\n1 1\n255\n9 8 7", (const uint8_t[]){9, 8, 7}, 3);
}

/* ---- JPEG, TIFF ---- */

static void test_jpeg_bomb_header_rejected(void)
{
    /* SOI + SOF0 claiming 65535x65535x3 with no scan data. */
    static const uint8_t B_[23] = {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x11, 8, 0xFF, 0xFF, 0xFF, 0xFF, 3,
                                   1,    0x11, 0,    2,    0x11, 0,    3, 0x11, 0,    0xFF, 0xD9};
    pc_climits l = pc_climits_default();
    pc_cdecode_opts o;
    l.max_pixels = 1u << 24;
    o = opts(l);
    CHECK(is_limit(try_dec(B_, sizeof B_, &o)));
}

static void put16(pc_buf *b, uint16_t v)
{
    uint8_t x[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    (void)pc_buf_append(b, x, 2, NULL);
}

static void put32(pc_buf *b, uint32_t v)
{
    uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    (void)pc_buf_append(b, x, 4, NULL);
}

static void test_tiff_bomb_header_rejected(void)
{
    /* Minimal little-endian TIFF claiming 1_000_000 x 1_000_000 8-bit gray. */
    static const uint32_t E[6][4] = {{256, 4, 1, 1000000}, {257, 4, 1, 1000000}, {258, 3, 1, 8},
                                     {262, 3, 1, 1},       {273, 4, 1, 0},       {279, 4, 1, 0}};
    pc_buf b = PC_BUF_INIT;
    size_t i;
    (void)pc_buf_append(&b, "II*\0\x08\0\0\0", 8, NULL);
    put16(&b, 6);
    for (i = 0; i < 6; i++) {
        put16(&b, (uint16_t)E[i][0]);
        put16(&b, (uint16_t)E[i][1]);
        put32(&b, E[i][2]);
        put32(&b, E[i][3]);
    }
    put32(&b, 0);
    CHECK(is_limit(try_dec(b.data, b.len, NULL)));
    pc_buf_fini(&b);
}

static void test_limits_check_api(void)
{
    pc_climits l = pc_climits_default();
    CHECK(pc_climits_check(&l, 100, 100, PC_CL_RGBA, PC_CS_F32, NULL) == PC_OK);
    CHECK(pc_climits_check(&l, 0, 1, PC_CL_GRAY, PC_CS_U8, NULL) == PC_ERR_INVALID);
    CHECK(pc_climits_check(&l, UINT32_MAX, 1, PC_CL_GRAY, PC_CS_U8, NULL) == PC_ERR_LIMIT);
    l.max_alloc = 399;
    CHECK(pc_climits_check(&l, 10, 10, PC_CL_RGBA, PC_CS_U8, NULL) != PC_OK);
    CHECK(pc_climits_check(&l, 10, 10, PC_CL_RGB, PC_CS_U8, NULL) == PC_OK);
}

int main(int argc, char **argv)
{
    RUN(test_max_pixels_enforced_for_every_format);
    RUN(test_max_width_enforced_for_every_format);
    RUN(test_max_height_enforced_for_every_format);
    RUN(test_max_alloc_enforced_for_every_format);
    RUN(test_limits_none_accepts);
    RUN(test_png_bomb_header_rejected_before_alloc);
    RUN(test_png_compressed_text_bombs_obey_limits_before_and_after_idat);
    RUN(test_png_text_chunks_share_an_aggregate_budget);
    RUN(test_png_text_accepts_the_exact_utf8_budget);
    RUN(test_png_plain_itxt_obeys_the_exact_budget_and_validates_fields);
    RUN(test_png_latin1_expansion_counts_keyword_and_text_utf8_bytes);
    RUN(test_png_duplicate_xmp_chunks_each_consume_budget);
    RUN(test_png_corrupt_text_is_still_skipped);
    RUN(test_png_text_with_bad_crc_and_text_after_iend_are_ignored);
    RUN(test_png_malformed_chunk_tails_remain_tolerated_after_pixels);
    RUN(test_png_invalid_optional_text_does_not_spend_an_exhausted_budget);
    RUN(test_pnm_bomb_header_rejected);
    RUN(test_pnm_header_alone_reserves_nothing_without_limits);
    RUN(test_pnm_header_sizes_that_overflow_usize_are_errors_without_limits);
    RUN(test_ascii_pnm_with_the_tightest_layout_still_decodes);
    RUN(test_jpeg_bomb_header_rejected);
    RUN(test_tiff_bomb_header_rejected);
    RUN(test_limits_check_api);
    return pc_t_finish();
}
