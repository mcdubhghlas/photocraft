/* EXIF / TIFF orientation (#285): files are opened upright, and every export writes
 * Orientation = 1 so the upright pixels are never rotated twice. Port of
 * crates/codecs/tests/orientation.rs and the unit test in crates/codecs/src/orientation.rs. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <time.h>

#include "base/compress.h"
#include "codecs/orientation.h"
#include "common.h"
#include "tiffgen.h"

/* ASan and TSan abort on an impossible allocation by default; the codec must see NULL instead. */
const char *__asan_default_options(void);
const char *__asan_default_options(void) { return "allocator_may_return_null=1"; }
const char *__tsan_default_options(void);
const char *__tsan_default_options(void) { return "allocator_may_return_null=1"; }

/* ---- byte builders ---- */

static void put16(pc_buf *v, bool big, uint16_t x)
{
    uint8_t b[2];
    b[0] = (uint8_t)(big ? x >> 8 : x);
    b[1] = (uint8_t)(big ? x : x >> 8);
    (void)pc_buf_append(v, b, 2, NULL);
}

static void put32(pc_buf *v, bool big, uint32_t x)
{
    uint8_t b[4];
    int i;
    for (i = 0; i < 4; i++) {
        b[i] = (uint8_t)(x >> (8 * (big ? 3 - i : i)));
    }
    (void)pc_buf_append(v, b, 4, NULL);
}

static void put(pc_buf *v, const void *d, size_t n) { (void)pc_buf_append(v, d, n, NULL); }

static void u32b(uint32_t x, bool big, uint8_t out[4])
{
    int i;
    for (i = 0; i < 4; i++) {
        out[i] = (uint8_t)(x >> (8 * (big ? 3 - i : i)));
    }
}

/* A minimal EXIF (TIFF-structured) block: a filler tag, then Orientation. */
static pc_buf exif_blk(uint32_t o, bool big, bool lng)
{
    pc_buf v = PC_BUF_INIT;
    put(&v, big ? "MM\0*" : "II*\0", 4);
    put32(&v, big, 8);
    put16(&v, big, 2);
    /* ImageDescription-ish filler (ASCII, 4 bytes inline). */
    put16(&v, big, 0x010E);
    put16(&v, big, 2);
    put32(&v, big, 4);
    put(&v, "abc\0", 4);
    put16(&v, big, 0x0112);
    if (lng) {
        put16(&v, big, 4);
        put32(&v, big, 1);
        put32(&v, big, o);
    } else {
        put16(&v, big, 3);
        put32(&v, big, 1);
        put16(&v, big, (uint16_t)o);
        put(&v, "\0\0", 2);
    }
    put32(&v, big, 0);
    return v;
}

typedef struct entry {
    uint16_t tag;
    uint16_t ty;
    uint32_t count;
    uint8_t value[4];
} entry;

static entry ent(uint16_t tag, uint16_t ty, uint32_t count, const uint8_t value[4])
{
    entry e;
    e.tag = tag;
    e.ty = ty;
    e.count = count;
    memcpy(e.value, value, 4);
    return e;
}

/* The 4-byte value field of a SHORT. */
static entry short_ent(uint16_t tag, uint16_t ty, uint32_t count, uint16_t v, bool big)
{
    uint8_t b[4] = {0, 0, 0, 0};
    b[0] = (uint8_t)(big ? v >> 8 : v);
    b[1] = (uint8_t)(big ? v : v >> 8);
    return ent(tag, ty, count, b);
}

static entry long_ent(uint16_t tag, uint16_t ty, uint32_t count, uint32_t v, bool big)
{
    uint8_t b[4];
    u32b(v, big, b);
    return ent(tag, ty, count, b);
}

/* A TIFF-structured EXIF block: IFD0 at offset 8 holding `entries`, a zero next-IFD pointer,
 * then `tail`. */
static pc_buf ifd(bool big, const entry *es, size_t n, const void *tail, size_t tn)
{
    pc_buf v = PC_BUF_INIT;
    size_t i;
    put(&v, big ? "MM\0*" : "II*\0", 4);
    put32(&v, big, 8);
    put16(&v, big, (uint16_t)n);
    for (i = 0; i < n; i++) {
        put16(&v, big, es[i].tag);
        put16(&v, big, es[i].ty);
        put32(&v, big, es[i].count);
        put(&v, es[i].value, 4);
    }
    put32(&v, big, 0);
    if (tn > 0) {
        put(&v, tail, tn);
    }
    return v;
}

/* pc_upright_exif as an owned copy; *borrowed says nothing changed (Rust's Cow::Borrowed). */
static pc_buf ux(const uint8_t *e, size_t n, bool *borrowed)
{
    uint8_t *o = NULL;
    size_t ol = 0;
    pc_error err;
    pc_buf r = PC_BUF_INIT;
    *borrowed = false;
    if (pc_upright_exif(e, n, &o, &ol, &err) != PC_OK) {
        PC_T_FAIL("upright_exif: %s", err.msg);
        return r;
    }
    if (o == NULL) {
        *borrowed = true;
        put(&r, e, n);
    } else {
        put(&r, o, ol);
        free(o);
    }
    return r;
}

static bool ux_borrowed(const uint8_t *e, size_t n)
{
    bool b;
    pc_buf r = ux(e, n, &b);
    pc_buf_fini(&r);
    return b;
}

static bool buf_eq(const pc_buf *a, const pc_buf *b)
{
    return a->len == b->len && (a->len == 0 || memcmp(a->data, b->data, a->len) == 0);
}

/* pc_upright_xmp as an owned string; *borrowed says nothing changed. */
static char *uxmp(const char *x, bool *borrowed)
{
    char *o = NULL;
    size_t ol = 0;
    pc_error err;
    size_t n = strlen(x);
    *borrowed = false;
    if (pc_upright_xmp(x, n, &o, &ol, &err) != PC_OK) {
        PC_T_FAIL("upright_xmp: %s", err.msg);
        o = NULL;
    }
    if (o == NULL) {
        *borrowed = true;
        o = malloc(n + 1);
        memcpy(o, x, n + 1);
    }
    return o;
}

/* What a camera stores for orientation o when the scene is up (w x h): the inverse of the turn
 * a viewer applies. Returns the stored pixels (new array) and their size. */
static uint8_t *stored(const uint8_t *up, size_t w, size_t h, uint16_t o, size_t *sw, size_t *sh)
{
    uint8_t *s = calloc(w * h, 1);
    size_t x;
    size_t y;
    *sw = o >= 5 ? h : w;
    *sh = o >= 5 ? w : h;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            size_t sx;
            size_t sy;
            switch (o) {
            case 2:
                sx = w - 1 - x, sy = y; /* mirrored horizontally */
                break;
            case 3:
                sx = w - 1 - x, sy = h - 1 - y; /* rotated 180 */
                break;
            case 4:
                sx = x, sy = h - 1 - y; /* mirrored vertically */
                break;
            case 5:
                sx = y, sy = x; /* transposed */
                break;
            case 6:
                sx = y, sy = w - 1 - x; /* viewer turns it 90 clockwise */
                break;
            case 7:
                sx = h - 1 - y, sy = w - 1 - x; /* transverse */
                break;
            case 8:
                sx = h - 1 - y, sy = x; /* viewer turns it 90 counter-clockwise */
                break;
            default:
                sx = x, sy = y;
            }
            s[sy * *sw + sx] = up[y * w + x];
        }
    }
    return s;
}

/* Inserts a JPEG APP1 EXIF segment right after SOI. */
static pc_buf jpeg_with_exif(const pc_buf *jpeg, const uint8_t *exif, size_t n)
{
    pc_buf out = PC_BUF_INIT;
    uint8_t hdr[4];
    size_t seg = 6 + n;
    put(&out, jpeg->data, 2);
    hdr[0] = 0xFF;
    hdr[1] = 0xE1;
    hdr[2] = (uint8_t)((seg + 2) >> 8);
    hdr[3] = (uint8_t)(seg + 2);
    put(&out, hdr, 4);
    put(&out, "Exif\0\0", 6);
    if (n > 0) {
        put(&out, exif, n);
    }
    put(&out, jpeg->data + 2, jpeg->len - 2);
    return out;
}

/* Inserts a PNG eXIf chunk before the first IDAT. */
static pc_buf png_with_exif(const pc_buf *png, const pc_buf *exif)
{
    pc_buf out = PC_BUF_INIT;
    pc_buf body = PC_BUF_INIT;
    size_t at = 0;
    size_t i;
    for (i = 0; i + 4 <= png->len; i++) {
        if (memcmp(png->data + i, "IDAT", 4) == 0) {
            at = i - 4;
            break;
        }
    }
    CHECK(at > 0);
    put(&body, "eXIf", 4);
    put(&body, exif->data, exif->len);
    put(&out, png->data, at);
    put32(&out, true, (uint32_t)exif->len);
    put(&out, body.data, body.len);
    put32(&out, true, pc_crc32(0, body.data, body.len));
    put(&out, png->data + at, png->len - at);
    pc_buf_fini(&body);
    return out;
}

static tg_built tiff_with_orientation(const uint8_t *data, uint32_t w, uint32_t h, uint16_t o)
{
    static const size_t chain[1] = {0};
    tg_dir d = tg_strips(w, h, 8, 1, 1, h, data, (size_t)w * h);
    tg_built b;
    tg_tag1(&d, 274, TG_SHORT, o);
    b = tg_build(true, false, &d, 1, chain, 1);
    tg_dir_free(&d);
    return b;
}

static pc_cimage gray(size_t w, size_t h, const uint8_t *px)
{
    pc_cimage img;
    if (pc_cimage_from_copy((uint32_t)w, (uint32_t)h, PC_CL_GRAY, PC_CS_U8, px, w * h, &img, NULL) != PC_OK) {
        PC_T_FAIL("gray image");
        pc_cimage_init(&img);
    }
    return img;
}

static pc_cimage gray_fill(size_t w, size_t h, uint8_t v)
{
    uint8_t *px = malloc(w * h);
    pc_cimage img;
    memset(px, v, w * h);
    img = gray(w, h, px);
    free(px);
    return img;
}

/* Exact pixels: distinct values per pixel. */
static uint8_t *exact_scene(size_t w, size_t h)
{
    uint8_t *v = malloc(w * h);
    size_t i;
    for (i = 0; i < w * h; i++) {
        v[i] = (uint8_t)((uint8_t)i * 3 + 1);
    }
    return v;
}

/* 8x8-block scene so JPEG keeps each block's value: block (bx, by) is distinct. */
static uint8_t *block_scene(size_t w, size_t h)
{
    uint8_t *v = malloc(w * h);
    size_t x;
    size_t y;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            v[y * w + x] = (uint8_t)(20 + 25 * (x / 8 + (w / 8) * (y / 8)));
        }
    }
    return v;
}

static void assert_blocks(const pc_cimage *img, const uint8_t *up, size_t w, size_t h, const char *what)
{
    size_t bx;
    size_t by;
    if (img->width != w || img->height != h) {
        PC_T_FAIL("%s: %ux%u not %zux%zu", what, img->width, img->height, w, h);
        return;
    }
    for (by = 0; by < h / 8; by++) {
        for (bx = 0; bx < w / 8; bx++) {
            size_t i = (by * 8 + 4) * w + bx * 8 + 4;
            int d = (int)img->data[i] - (int)up[i];
            pc_t_checks++;
            if (d < -4 || d > 4) {
                PC_T_FAIL("%s: block (%zu,%zu) is %u not %u", what, bx, by, img->data[i], up[i]);
            }
        }
    }
}

static pc_cencode_opts q100(void)
{
    pc_cencode_opts o = pc_cencode_opts_default();
    o.jpeg_quality = 100;
    return o;
}

static pc_cdecode_opts keep(void)
{
    pc_cdecode_opts o = pc_cdecode_opts_default();
    o.keep_orientation = true;
    return o;
}

static uint16_t meta_orientation(const pc_cimage *img)
{
    if (!img->meta.exif.has) {
        PC_T_FAIL("no EXIF kept");
        return 0;
    }
    return pc_exif_orientation(img->meta.exif.data, img->meta.exif.len);
}

/* ---- tests ---- */

static void test_reads_orientation_in_every_encoding(void)
{
    uint32_t o;
    int big;
    int lng;
    pc_buf e;
    pc_buf s;
    for (o = 1; o <= 8; o++) {
        for (big = 0; big < 2; big++) {
            for (lng = 0; lng < 2; lng++) {
                pc_buf prefixed = PC_BUF_INIT;
                pc_buf fixed;
                pc_buf one = exif_blk(1, big, lng);
                bool b;
                e = exif_blk(o, big, lng);
                CHECK_EQ_UINT(pc_exif_orientation(e.data, e.len), o);
                put(&prefixed, "Exif\0\0", 6);
                put(&prefixed, e.data, e.len);
                CHECK_EQ_UINT(pc_exif_orientation(prefixed.data, prefixed.len), o);
                fixed = ux(prefixed.data, prefixed.len, &b);
                CHECK_EQ_UINT(pc_exif_orientation(fixed.data, fixed.len), 1);
                CHECK_EQ_UINT(fixed.len, prefixed.len); /* rewritten in place */
                CHECK(memcmp(fixed.data, "Exif\0\0", 6) == 0);
                /* Rewritten in the entry's own type and width: exactly exif(1, ..). */
                if (fixed.len != 6 + one.len || memcmp(fixed.data + 6, one.data, one.len) != 0) {
                    PC_T_FAIL("o=%u big=%d long=%d", o, big, lng);
                }
                pc_buf_fini(&e);
                pc_buf_fini(&prefixed);
                pc_buf_fini(&fixed);
                pc_buf_fini(&one);
            }
        }
    }
    /* Already upright, no tag, or garbage: borrowed unchanged. */
    e = exif_blk(1, false, false);
    CHECK(ux_borrowed(e.data, e.len));
    pc_buf_fini(&e);
    s = sample_exif();
    CHECK(ux_borrowed(s.data, s.len));
    pc_buf_fini(&s);
    CHECK(ux_borrowed((const uint8_t *)"nonsense", 8));
}

static void test_malformed_or_out_of_range_orientation_reads_as_1(void)
{
    static const uint32_t bads[] = {0, 9, 255, 0xFFFF, 0x10006};
    static const uint8_t empty[1] = {0};
    size_t i;
    size_t n;
    pc_buf good;
    pc_buf b;
    for (i = 0; i < sizeof bads / sizeof bads[0]; i++) {
        b = exif_blk(bads[i], false, true);
        CHECK_EQ_UINT(pc_exif_orientation(b.data, b.len), 1);
        pc_buf_fini(&b);
    }
    good = exif_blk(6, false, false);
    CHECK_EQ_UINT(pc_exif_orientation(good.data, good.len), 6);
    for (n = 0; n < good.len; n++) {
        /* Any cut, even one that keeps the Orientation entry but loses the next-IFD pointer. */
        if (pc_exif_orientation(good.data, n) != 1 || !ux_borrowed(good.data, n)) {
            PC_T_FAIL("truncated at %zu", n);
        }
    }
    /* IFD offset past the end, a huge entry count, a wrong field type. */
    pc_buf_init(&b);
    put(&b, good.data, good.len);
    memset(b.data + 4, 0xFF, 4);
    CHECK_EQ_UINT(pc_exif_orientation(b.data, b.len), 1);
    memcpy(b.data, good.data, good.len);
    memset(b.data + 8, 0xFF, 2);
    CHECK_EQ_UINT(pc_exif_orientation(b.data, b.len), 1); /* an entry count past the end is a truncated table */
    memcpy(b.data, good.data, good.len);
    b.data[24] = 2; /* ASCII */
    b.data[25] = 0;
    CHECK_EQ_UINT(pc_exif_orientation(b.data, b.len), 1);
    CHECK_EQ_UINT(pc_exif_orientation(empty, 0), 1);
    CHECK_EQ_UINT(pc_exif_orientation((const uint8_t *)"II*\0", 4), 1);
    pc_buf_fini(&b);
    pc_buf_fini(&good);
}

/* proptest random_exif_never_panics, as a seeded loop. */
static void test_random_exif_never_panics(void)
{
    rng r = {0x285};
    int iter;
    for (iter = 0; iter < 2000; iter++) {
        size_t n = (size_t)(rng_next(&r) % 256);
        bool le = (rng_next(&r) & 1) != 0;
        uint8_t *bytes = rng_bytes(&r, n);
        pc_buf b = PC_BUF_INIT;
        pc_buf up;
        pc_buf raw;
        bool br;
        uint16_t o;
        put(&b, le ? "II*\0\x08\0\0\0" : "MM\0*\0\0\0\x08", 8);
        put(&b, bytes, n);
        o = pc_exif_orientation(b.data, b.len);
        if (o < 1 || o > 8) {
            PC_T_FAIL("orientation %u", o);
        }
        up = ux(b.data, b.len, &br);
        if (pc_exif_orientation(up.data, up.len) != 1) {
            PC_T_FAIL("iteration %d: not upright after rewrite", iter);
        }
        (void)pc_exif_orientation(bytes, n);
        raw = ux(bytes, n, &br);
        pc_buf_fini(&raw);
        pc_buf_fini(&up);
        pc_buf_fini(&b);
        free(bytes);
    }
    CHECK(true);
}

/* proptest random_xmp_never_panics, as a seeded loop over an alphabet that hits the parser's
 * edges (quotes, '=', '>', whitespace, digits, multi-byte UTF-8). */
static void test_random_xmp_never_panics(void)
{
    static const char *const alpha[] = {"0", "1", "6", "9", "\"", "'", "=", ">", "<", " ", "\t", "a", "/",
                                        "\xc3\xa9", "\xe2\x82\xac", "\xf0\x9f\x98\x80", "tiff:Orientation"};
    rng r = {0x286};
    int iter;
    for (iter = 0; iter < 2000; iter++) {
        pc_buf x = PC_BUF_INIT;
        size_t ns = (size_t)(rng_next(&r) % 65);
        size_t nt = (size_t)(rng_next(&r) % 17);
        size_t i;
        char *o = NULL;
        size_t ol = 0;
        pc_error err;
        for (i = 0; i < ns; i++) {
            const char *a = alpha[rng_next(&r) % (sizeof alpha / sizeof alpha[0] - 1)];
            put(&x, a, strlen(a));
        }
        put(&x, "tiff:Orientation", 16);
        for (i = 0; i < nt; i++) {
            const char *a = alpha[rng_next(&r) % (sizeof alpha / sizeof alpha[0] - 1)];
            put(&x, a, strlen(a));
        }
        if (pc_upright_xmp((const char *)x.data, x.len, &o, &ol, &err) != PC_OK) {
            PC_T_FAIL("upright_xmp: %s", err.msg);
        }
        free(o);
        pc_buf_fini(&x);
    }
    CHECK(true);
}

static void test_xmp_orientation_is_rewritten(void)
{
    static const char *const same[] = {"tiff:Orientation=\"1\"", "no orientation", "tiff:Orientation",
                                       "tiff:Orientation=\"\"", "tiff:Orientation: 6"};
    bool b;
    char *s;
    size_t i;
    s = uxmp("<rdf:Description tiff:Orientation=\"6\" tiff:Make=\"X\"/>", &b);
    CHECK_EQ_STR(s, "<rdf:Description tiff:Orientation=\"1\" tiff:Make=\"X\"/>");
    free(s);
    s = uxmp("<tiff:Orientation>8</tiff:Orientation><tiff:Orientation>3</tiff:Orientation>", &b);
    CHECK_EQ_STR(s, "<tiff:Orientation>1</tiff:Orientation><tiff:Orientation>1</tiff:Orientation>");
    free(s);
    s = uxmp("tiff:Orientation='12'", &b);
    CHECK_EQ_STR(s, "tiff:Orientation='1'");
    free(s);
    for (i = 0; i < sizeof same / sizeof same[0]; i++) {
        s = uxmp(same[i], &b);
        if (!b) {
            PC_T_FAIL("%s", same[i]);
        }
        free(s);
    }
}

static void test_png_opens_upright_for_every_orientation(void)
{
    size_t w = 5;
    size_t h = 3;
    uint8_t *up = exact_scene(w, h);
    uint16_t o;
    for (o = 1; o <= 8; o++) {
        size_t sw;
        size_t sh;
        uint8_t *s = stored(up, w, h, o, &sw, &sh);
        pc_cimage g = gray(sw, sh, s);
        pc_buf png = enc(&g, PC_CF_PNG, NULL);
        pc_buf ex = exif_blk(o, o % 2 == 0, false);
        pc_buf file = png_with_exif(&png, &ex);
        pc_cimage img = dec(file.data, file.len);
        pc_cdecode_opts k = keep();
        pc_cimage raw = dec_with(file.data, file.len, &k);
        if (img.width != w || img.height != h || img.len != w * h || memcmp(img.data, up, w * h) != 0) {
            PC_T_FAIL("o=%u: not upright", o);
        }
        CHECK_EQ_UINT(meta_orientation(&img), 1); /* metadata says upright */
        /* Opting out keeps the stored pixels and the tag. */
        if (raw.width != sw || raw.height != sh || raw.len != w * h || memcmp(raw.data, s, w * h) != 0) {
            PC_T_FAIL("o=%u: kept orientation changed the pixels", o);
        }
        CHECK_EQ_UINT(meta_orientation(&raw), o);
        pc_cimage_fini(&raw);
        pc_cimage_fini(&img);
        pc_buf_fini(&file);
        pc_buf_fini(&ex);
        pc_buf_fini(&png);
        pc_cimage_fini(&g);
        free(s);
    }
    free(up);
}

static void test_tiff_opens_upright_for_every_orientation(void)
{
    size_t w = 4;
    size_t h = 7;
    uint8_t *up = exact_scene(w, h);
    uint16_t o;
    for (o = 1; o <= 8; o++) {
        size_t sw;
        size_t sh;
        uint8_t *s = stored(up, w, h, o, &sw, &sh);
        tg_built t = tiff_with_orientation(s, (uint32_t)sw, (uint32_t)sh, o);
        pc_cimage img = dec(t.bytes, t.len);
        if (img.width != w || img.height != h || img.len != w * h || memcmp(img.data, up, w * h) != 0) {
            PC_T_FAIL("o=%u: not upright", o);
        }
        pc_t_checks++;
        pc_cimage_fini(&img);
        tg_built_free(&t);
        free(s);
    }
    free(up);
}

static void test_webp_opens_upright(void)
{
    static const uint16_t os[] = {3, 6, 8};
    size_t w = 6;
    size_t h = 2;
    uint8_t *up = exact_scene(w, h);
    size_t k;
    for (k = 0; k < 3; k++) {
        uint16_t o = os[k];
        size_t sw;
        size_t sh;
        uint8_t *s = stored(up, w, h, o, &sw, &sh);
        pc_cimage g = gray(sw, sh, s);
        pc_buf ex = exif_blk(o, false, false);
        pc_buf one = exif_blk(1, false, false);
        pc_cencode_opts eo = pc_cencode_opts_default();
        pc_buf out;
        pc_cimage img;
        pc_cimage gimg;
        size_t i;
        bool patched = false;
        /* Rust wrote the EXIF with image_webp's encoder. Ours always writes Orientation 1 (in
         * place, same length), so the chunk is patched back to o in the file afterwards. */
        eo.webp_lossless = true;
        set_blob(&g.meta.exif, ex.data, ex.len);
        out = enc(&g, PC_CF_WEBP, &eo);
        for (i = 0; i + one.len <= out.len; i++) {
            if (memcmp(out.data + i, one.data, one.len) == 0) {
                memcpy(out.data + i, ex.data, ex.len);
                patched = true;
                break;
            }
        }
        CHECK(patched);
        /* WebP has no gray mode: it comes back as RGB. */
        img = dec(out.data, out.len);
        if (pc_cimage_convert(&img, PC_CL_GRAY, PC_CS_U8, &gimg, NULL) == PC_OK) {
            if (gimg.width != w || gimg.height != h || gimg.len != w * h || memcmp(gimg.data, up, w * h) != 0) {
                PC_T_FAIL("o=%u: not upright", o);
            }
            pc_t_checks++;
            pc_cimage_fini(&gimg);
        } else {
            PC_T_FAIL("convert");
        }
        pc_cimage_fini(&img);
        pc_buf_fini(&out);
        pc_buf_fini(&one);
        pc_buf_fini(&ex);
        pc_cimage_fini(&g);
        free(s);
    }
    free(up);
}

static void test_jpeg_opens_upright_for_every_orientation(void)
{
    size_t w = 32;
    size_t h = 16;
    uint8_t *up = block_scene(w, h);
    pc_cencode_opts q = q100();
    uint16_t o;
    for (o = 1; o <= 8; o++) {
        size_t sw;
        size_t sh;
        uint8_t *s = stored(up, w, h, o, &sw, &sh);
        pc_cimage g = gray(sw, sh, s);
        pc_buf jpeg = enc(&g, PC_CF_JPEG, &q);
        pc_buf ex = exif_blk(o, o > 4, o == 7);
        pc_buf file = jpeg_with_exif(&jpeg, ex.data, ex.len);
        pc_cimage img = dec(file.data, file.len);
        char what[16];
        (void)snprintf(what, sizeof what, "o=%u", o);
        assert_blocks(&img, up, w, h, what);
        CHECK_EQ_UINT(meta_orientation(&img), 1);
        pc_cimage_fini(&img);
        pc_buf_fini(&file);
        pc_buf_fini(&ex);
        pc_buf_fini(&jpeg);
        pc_cimage_fini(&g);
        free(s);
    }
    free(up);
}

static void test_rgb_jpeg_with_dpi_swaps_resolution_on_a_quarter_turn(void)
{
    uint8_t px[16 * 8 * 3];
    pc_cimage img;
    pc_buf jpeg;
    pc_buf ex = exif_blk(6, false, false);
    pc_buf file;
    pc_cimage back;
    memset(px, 128, sizeof px);
    CHECK_EQ_INT(pc_cimage_from_copy(16, 8, PC_CL_RGB, PC_CS_U8, px, sizeof px, &img, NULL), PC_OK);
    img.meta.has_dpi = true;
    img.meta.dpi_x = 300.0f;
    img.meta.dpi_y = 150.0f;
    jpeg = enc(&img, PC_CF_JPEG, NULL);
    file = jpeg_with_exif(&jpeg, ex.data, ex.len);
    back = dec(file.data, file.len);
    CHECK_EQ_UINT(back.width, 8);
    CHECK_EQ_UINT(back.height, 16);
    CHECK(back.meta.has_dpi);
    CHECK_NEAR(back.meta.dpi_x, 150.0, 0.0);
    CHECK_NEAR(back.meta.dpi_y, 300.0, 0.0);
    pc_cimage_fini(&back);
    pc_buf_fini(&file);
    pc_buf_fini(&ex);
    pc_buf_fini(&jpeg);
    pc_cimage_fini(&img);
}

static void test_export_writes_orientation_1_and_keeps_pixels(void)
{
    static const pc_cformat fmts[] = {PC_CF_PNG, PC_CF_JPEG, PC_CF_WEBP, PC_CF_TIFF};
    static const char xmp6[] = "<x tiff:Orientation=\"6\"/>";
    size_t w = 32;
    size_t h = 16;
    uint8_t *up = block_scene(w, h);
    size_t sw;
    size_t sh;
    uint8_t *s = stored(up, w, h, 6, &sw, &sh);
    pc_cimage g = gray(sw, sh, s);
    pc_cencode_opts q = q100();
    pc_buf jpeg = enc(&g, PC_CF_JPEG, &q);
    pc_buf ex = exif_blk(6, false, false);
    pc_buf ex_be = exif_blk(6, true, false);
    pc_buf file = jpeg_with_exif(&jpeg, ex.data, ex.len);
    pc_cimage opened = dec(file.data, file.len);
    pc_cimage stale;
    size_t k;
    assert_blocks(&opened, up, w, h, "opened");

    /* Even an image whose metadata still claims 6 (e.g. from a PSD) is written as 1. */
    CHECK_EQ_INT(pc_cimage_clone(&opened, &stale, NULL), PC_OK);
    set_blob(&stale.meta.exif, ex_be.data, ex_be.len);
    set_blob(&stale.meta.xmp, xmp6, strlen(xmp6));
    for (k = 0; k < sizeof fmts / sizeof fmts[0]; k++) {
        pc_cformat f = fmts[k];
        pc_buf bytes = enc(&stale, f, &q);
        pc_cdecode_opts ko = keep();
        pc_cimage raw = dec_with(bytes.data, bytes.len, &ko);
        pc_cimage back = dec(bytes.data, bytes.len);
        pc_t_name = pc_cformat_debug(f);
        if (raw.meta.exif.has) {
            CHECK_EQ_UINT(pc_exif_orientation(raw.meta.exif.data, raw.meta.exif.len), 1);
        }
        if (raw.meta.xmp.has) {
            CHECK(find_bytes(raw.meta.xmp.data, raw.meta.xmp.len, "tiff:Orientation=\"1\"", 20));
        }
        CHECK_EQ_UINT(pc_exif_orientation(bytes.data, bytes.len), 1); /* the file itself */
        /* Pixels as shown, both with and without applying orientation on reopen. */
        CHECK(back.width == raw.width && back.height == raw.height);
        if (f == PC_CF_JPEG) {
            assert_blocks(&back, up, w, h, "jpeg reopen");
        } else {
            pc_cimage gb;
            if (pc_cimage_convert(&back, PC_CL_GRAY, PC_CS_U8, &gb, NULL) == PC_OK) {
                CHECK(gb.len == opened.len && memcmp(gb.data, opened.data, gb.len) == 0); /* lossless */
                pc_cimage_fini(&gb);
            } else {
                PC_T_FAIL("convert");
            }
        }
        pc_cimage_fini(&back);
        pc_cimage_fini(&raw);
        pc_buf_fini(&bytes);
    }
    pc_t_name = "test_export_writes_orientation_1_and_keeps_pixels";
    pc_cimage_fini(&stale);
    pc_cimage_fini(&opened);
    pc_buf_fini(&file);
    pc_buf_fini(&ex_be);
    pc_buf_fini(&ex);
    pc_buf_fini(&jpeg);
    pc_cimage_fini(&g);
    free(s);
    free(up);
}

static void test_round_trip_open_save_reopen_is_stable(void)
{
    size_t w = 24;
    size_t h = 16;
    uint8_t *up = block_scene(w, h);
    pc_cencode_opts q = q100();
    uint16_t o;
    for (o = 1; o <= 8; o++) {
        size_t sw;
        size_t sh;
        uint8_t *s = stored(up, w, h, o, &sw, &sh);
        pc_cimage g = gray(sw, sh, s);
        pc_buf jpeg = enc(&g, PC_CF_JPEG, &q);
        pc_buf ex = exif_blk(o, false, false);
        pc_buf file = jpeg_with_exif(&jpeg, ex.data, ex.len);
        pc_cimage first = dec(file.data, file.len);
        pc_buf saved = enc(&first, PC_CF_JPEG, &q);
        pc_cimage second = dec(saved.data, saved.len);
        char what[32];
        (void)snprintf(what, sizeof what, "reopened o=%u", o);
        assert_blocks(&second, up, w, h, what);
        CHECK_EQ_UINT(meta_orientation(&second), 1);
        pc_cimage_fini(&second);
        pc_buf_fini(&saved);
        pc_cimage_fini(&first);
        pc_buf_fini(&file);
        pc_buf_fini(&ex);
        pc_buf_fini(&jpeg);
        pc_cimage_fini(&g);
        free(s);
    }
    free(up);
}

static void test_malformed_exif_in_a_jpeg_opens_as_stored(void)
{
    pc_cimage img = gray_fill(16, 8, 90);
    pc_buf jpeg = enc(&img, PC_CF_JPEG, NULL);
    pc_buf ex6 = exif_blk(6, false, false);
    rng r = {285};
    size_t i;
    for (i = 0; i < 4 + 64; i++) {
        pc_buf e = PC_BUF_INIT;
        pc_buf file;
        pc_cimage back;
        uint16_t o;
        uint32_t ww;
        uint32_t wh;
        if (i == 1) {
            put(&e, "II*\0", 4);
        } else if (i == 2) {
            put(&e, "MM\0*\xff\xff\xff\xff", 8);
        } else if (i == 3) {
            put(&e, ex6.data, 20);
        } else if (i >= 4) {
            size_t j = i - 4;
            uint8_t *rb = rng_bytes(&r, j * 7);
            put(&e, j % 2 == 0 ? "II*\0\x08\0\0\0" : "MM\0*\0\0\0\x08", 8);
            put(&e, rb, j * 7);
            free(rb);
        }
        file = jpeg_with_exif(&jpeg, e.data, e.len);
        back = dec(file.data, file.len);
        o = pc_exif_orientation(e.data, e.len);
        ww = o >= 5 ? 8 : 16;
        wh = o >= 5 ? 16 : 8;
        pc_t_checks++;
        if (back.width != ww || back.height != wh) {
            PC_T_FAIL("case %zu: %ux%u not %ux%u", i, back.width, back.height, ww, wh);
        }
        pc_cimage_fini(&back);
        pc_buf_fini(&file);
        pc_buf_fini(&e);
    }
    pc_buf_fini(&ex6);
    pc_buf_fini(&jpeg);
    pc_cimage_fini(&img);
}

static uint16_t inverse(uint16_t o) { return o == 6 ? 8 : o == 8 ? 6 : o; }

static void test_every_layout_and_depth_orients_and_inverts(void)
{
    int l;
    int s;
    for (l = 0; l < PC_CL_COUNT; l++) {
        for (s = 0; s < PC_CS_COUNT; s++) {
            pc_cimage img = test_image((pc_clayout)l, (pc_csample)s);
            uint16_t o;
            for (o = 0; o <= 9; o++) {
                pc_cimage c;
                pc_cimage turned;
                pc_cimage back;
                pc_error e;
                CHECK_EQ_INT(pc_cimage_clone(&img, &c, NULL), PC_OK);
                if (pc_cimage_oriented(&c, o, &turned, &e) != PC_OK) {
                    PC_T_FAIL("orient: %s", e.msg);
                    continue;
                }
                if (o >= 5 && o <= 8) {
                    CHECK(turned.width == img.height && turned.height == img.width);
                } else {
                    CHECK(turned.width == img.width && turned.height == img.height);
                }
                if (pc_cimage_oriented(&turned, inverse(o), &back, &e) != PC_OK) {
                    PC_T_FAIL("orient back: %s", e.msg);
                    continue;
                }
                pc_t_checks++;
                if (back.len != img.len || memcmp(back.data, img.data, img.len) != 0) {
                    PC_T_FAIL("%s %s o=%u", pc_clayout_name((pc_clayout)l), pc_csample_name((pc_csample)s), o);
                }
                pc_cimage_fini(&back);
            }
            pc_cimage_fini(&img);
        }
    }
}

static double now_ms(void)
{
    struct timespec t;
    (void)clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

/* #285 performance budget: turning a 24 MP photo upright on open adds <= 50 ms. Ignored in
 * Rust ("timing; run in release"); here it runs with PHOTOCRAFT_SLOW_TESTS=1. */
static void test_rotating_24_mp_is_fast(void)
{
    static const uint16_t os[] = {6, 3, 8};
    const char *slow = getenv("PHOTOCRAFT_SLOW_TESTS");
    uint32_t w = 6000;
    uint32_t h = 4000;
    size_t n = (size_t)w * h * 3;
    uint8_t *px;
    pc_cimage img;
    size_t i;
    size_t k;
    if (slow == NULL || strcmp(slow, "1") != 0) {
        (void)printf("(24 MP timing skipped; PHOTOCRAFT_SLOW_TESTS=1 runs it) ");
        return;
    }
    px = malloc(n);
    for (i = 0; i < n; i++) {
        px[i] = (uint8_t)i;
    }
    CHECK_EQ_INT(pc_cimage_from_raw(w, h, PC_CL_RGB, PC_CS_U8, px, n, &img, NULL), PC_OK);
    for (k = 0; k < 3; k++) {
        double best = 1e300;
        int rep;
        for (rep = 0; rep < 5; rep++) {
            pc_cimage c;
            pc_cimage r;
            double t;
            (void)pc_cimage_clone(&img, &c, NULL);
            t = now_ms();
            CHECK_EQ_INT(pc_cimage_oriented(&c, os[k], &r, NULL), PC_OK);
            t = now_ms() - t;
            best = t < best ? t : best;
            pc_cimage_fini(&r);
        }
        (void)fprintf(stderr, "orientation %u: 24 MP RGB8 in %.1f ms\n", os[k], best);
        CHECK(best <= 50.0);
    }
    pc_cimage_fini(&img);
}

static void test_duplicate_orientation_entries_first_wins(void)
{
    int big;
    for (big = 0; big < 2; big++) {
        entry e[2];
        pc_buf b;
        pc_buf want;
        pc_buf fixed;
        bool br;
#define O(v) short_ent(0x0112, 3, 1, (v), big)
#define L(v) long_ent(0x0112, 4, 1, (v), big)
#define ORI2(a, b2, expect)                                                                        \
    do {                                                                                           \
        e[0] = (a);                                                                                \
        e[1] = (b2);                                                                               \
        b = ifd(big, e, 2, NULL, 0);                                                               \
        CHECK_EQ_UINT(pc_exif_orientation(b.data, b.len), (expect));                               \
        pc_buf_fini(&b);                                                                           \
    } while (0)
        ORI2(O(6), O(3), 6);
        ORI2(O(3), O(6), 3);
        ORI2(O(1), O(8), 1);
        /* A malformed first entry is ignored as a whole: a later good one does not count. */
        ORI2(short_ent(0x0112, 3, 2, 6, big), O(8), 1);
        ORI2(L(6), O(8), 6); /* a LONG first entry counts */
        e[0] = L(6);
        e[1] = O(8);
        b = ifd(big, e, 2, NULL, 0);
        fixed = ux(b.data, b.len, &br);
        e[0] = L(1);
        e[1] = O(1);
        want = ifd(big, e, 2, NULL, 0);
        CHECK(buf_eq(&fixed, &want)); /* each in its own width */
        pc_buf_fini(&b);
        pc_buf_fini(&fixed);
        ORI2(short_ent(0x0112, 5, 1, 6, big), O(8), 1);
        /* Upright: every well-formed duplicate becomes 1, so no reader rotates twice. */
        e[0] = O(6);
        e[1] = O(3);
        b = ifd(big, e, 2, NULL, 0);
        fixed = ux(b.data, b.len, &br);
        CHECK_EQ_UINT(fixed.len, b.len);
        pc_buf_fini(&want);
        e[0] = O(1);
        e[1] = O(1);
        want = ifd(big, e, 2, NULL, 0);
        CHECK(buf_eq(&fixed, &want));
        pc_buf_fini(&b);
        pc_buf_fini(&fixed);
        /* Only a later duplicate disagrees: it is rewritten too. */
        e[0] = O(1);
        e[1] = O(8);
        b = ifd(big, e, 2, NULL, 0);
        fixed = ux(b.data, b.len, &br);
        CHECK(buf_eq(&fixed, &want));
        pc_buf_fini(&b);
        pc_buf_fini(&fixed);
        CHECK(ux_borrowed(want.data, want.len));
        pc_buf_fini(&want);
#undef ORI2
#undef L
#undef O
    }
}

static void test_strict_ifd_parsing_ignores_the_tag(void)
{
    static const uint32_t counts[] = {0, 2, 3, UINT32_MAX};
    static const uint16_t types[] = {1, 2, 5, 6, 7, 8, 9, 0, 0xFFFF};
    int big;
    for (big = 0; big < 2; big++) {
        entry o6 = short_ent(0x0112, 3, 1, 6, big);
        pc_buf good = ifd(big, &o6, 1, NULL, 0);
        size_t cut;
        uint32_t off;
        size_t i;
        CHECK_EQ_UINT(pc_exif_orientation(good.data, good.len), 6);
        /* Truncated before (or inside) the next-IFD pointer. */
        for (cut = 1; cut <= 4; cut++) {
            if (pc_exif_orientation(good.data, good.len - cut) != 1) {
                PC_T_FAIL("cut %zu", cut);
            }
        }
        /* IFD offsets inside the 8-byte header. */
        for (off = 0; off < 8; off++) {
            pc_buf b = PC_BUF_INIT;
            put(&b, good.data, good.len);
            u32b(off, big, b.data + 4);
            if (pc_exif_orientation(b.data, b.len) != 1 || !ux_borrowed(b.data, b.len)) {
                PC_T_FAIL("offset %u", off);
            }
            pc_buf_fini(&b);
        }
        /* Count must be exactly 1, type must be SHORT or LONG. */
        for (i = 0; i < sizeof counts / sizeof counts[0]; i++) {
            entry e = short_ent(0x0112, 3, counts[i], 6, big);
            pc_buf b = ifd(big, &e, 1, NULL, 0);
            if (pc_exif_orientation(b.data, b.len) != 1) {
                PC_T_FAIL("count %u", counts[i]);
            }
            pc_buf_fini(&b);
            e = long_ent(0x0112, 4, counts[i], 6, big);
            b = ifd(big, &e, 1, NULL, 0);
            if (pc_exif_orientation(b.data, b.len) != 1) {
                PC_T_FAIL("LONG count %u", counts[i]);
            }
            pc_buf_fini(&b);
        }
        for (i = 0; i < sizeof types / sizeof types[0]; i++) {
            entry e = short_ent(0x0112, types[i], 1, 6, big);
            pc_buf b = ifd(big, &e, 1, NULL, 0);
            if (pc_exif_orientation(b.data, b.len) != 1 || !ux_borrowed(b.data, b.len)) {
                PC_T_FAIL("type %u", types[i]);
            }
            pc_buf_fini(&b);
        }
        pc_t_checks += 4;
        pc_buf_fini(&good);
    }
}

static void test_upright_exif_keeps_every_other_byte(void)
{
    int big;
    for (big = 0; big < 2; big++) {
        /* IFD0: Make (12-byte ASCII stored after the IFD), Orientation 6, Software (inline
         * ASCII), ExifIFD pointer to a sub-IFD with its own entries. */
        uint32_t ifd_end = 8 + 2 + 4 * 12 + 4;
        uint32_t make_at = ifd_end;
        uint32_t sub_at = make_at + 12;
        pc_buf tail = PC_BUF_INIT;
        pc_buf e;
        pc_buf prefixed = PC_BUF_INIT;
        pc_buf fixed;
        entry es[4];
        bool br;
        size_t value = 6 + 8 + 2 + 12 + 8;
        size_t i;
        size_t nchanged = 0;
        size_t changed_at = 0;
        put(&tail, "PhotoCraftCo", 12);
        /* Sub-IFD: one DateTimeOriginal-ish entry, then next-IFD 0. */
        put16(&tail, big, 1);
        put16(&tail, big, 0x9000);
        put16(&tail, big, 7);
        put32(&tail, big, 4);
        put(&tail, "0232", 4);
        put32(&tail, big, 0);
        es[0] = long_ent(0x010F, 2, 12, make_at, big);
        es[1] = short_ent(0x0112, 3, 1, 6, big);
        es[2] = ent(0x0131, 2, 4, (const uint8_t *)"PC1\0");
        es[3] = long_ent(0x8769, 4, 1, sub_at, big);
        e = ifd(big, es, 4, tail.data, tail.len);
        put(&prefixed, "Exif\0\0", 6);
        put(&prefixed, e.data, e.len);
        fixed = ux(prefixed.data, prefixed.len, &br);
        CHECK_EQ_UINT(pc_exif_orientation(fixed.data, fixed.len), 1);
        CHECK_EQ_UINT(fixed.len, prefixed.len);
        /* Only the Orientation value (2 bytes in the second entry) differs. */
        for (i = 0; i < fixed.len && i < prefixed.len; i++) {
            if (fixed.data[i] != prefixed.data[i]) {
                nchanged++;
                changed_at = i;
            }
        }
        CHECK_EQ_UINT(nchanged, 1);
        CHECK_EQ_UINT(changed_at, big ? value + 1 : value);
        CHECK(memcmp(fixed.data, prefixed.data, value) == 0);
        CHECK(memcmp(fixed.data + value + 2, prefixed.data + value + 2, fixed.len - value - 2) == 0);
        /* The offset-based Make string and the inline Software tag are intact. */
        CHECK(memcmp(fixed.data + 6 + make_at, "PhotoCraftCo", 12) == 0);
        CHECK(find_bytes(fixed.data, fixed.len, "PC1\0", 4));
        pc_buf_fini(&fixed);
        pc_buf_fini(&prefixed);
        pc_buf_fini(&e);
        pc_buf_fini(&tail);
    }
}

static void test_limits_apply_to_the_upright_size(void)
{
    /* Stored 32x16 landscape; Orientation 6 makes it a 16x32 portrait. */
    pc_cimage g = gray_fill(32, 16, 90);
    pc_buf jpeg = enc(&g, PC_CF_JPEG, NULL);
    pc_buf ex6 = exif_blk(6, false, false);
    pc_buf ex3 = exif_blk(3, false, false);
    pc_buf tall = jpeg_with_exif(&jpeg, ex6.data, ex6.len);
    pc_buf flip = jpeg_with_exif(&jpeg, ex3.data, ex3.len);
    pc_cdecode_opts opts = pc_cdecode_opts_default();
    pc_cdecode_opts kopts;
    pc_cdecode_opts def = pc_cdecode_opts_default();
    pc_cimage img;
    pc_error e;
    uint8_t px[32 * 16];
    tg_built tiff;
    opts.limits.max_width = 32;
    opts.limits.max_height = 16;
    kopts = opts;
    kopts.keep_orientation = true;
    /* The upright portrait is too tall. */
    CHECK_EQ_INT(pc_cdecode_with(tall.data, tall.len, &opts, &img, &e), PC_ERR_LIMIT);
    /* As stored it fits, and a 180 turn keeps the size. */
    img = dec_with(tall.data, tall.len, &kopts);
    CHECK(img.width == 32 && img.height == 16);
    pc_cimage_fini(&img);
    img = dec_with(flip.data, flip.len, &opts);
    CHECK(img.width == 32 && img.height == 16);
    pc_cimage_fini(&img);
    /* The same holds for a TIFF, whose orientation lives in its own IFD0. */
    memset(px, 7, sizeof px);
    tiff = tiff_with_orientation(px, 32, 16, 8);
    CHECK_EQ_INT(pc_cdecode_with(tiff.bytes, tiff.len, &opts, &img, &e), PC_ERR_LIMIT);
    img = dec_with(tiff.bytes, tiff.len, &def);
    CHECK(img.width == 16 && img.height == 32);
    pc_cimage_fini(&img);
    tg_built_free(&tiff);
    pc_buf_fini(&flip);
    pc_buf_fini(&tall);
    pc_buf_fini(&ex3);
    pc_buf_fini(&ex6);
    pc_buf_fini(&jpeg);
    pc_cimage_fini(&g);
}

static void test_long_orientation_reads_and_rewrites_in_both_byte_orders(void)
{
    static const uint32_t bads[] = {0, 9, 0x10006, UINT32_MAX};
    int big;
    pc_cimage g = gray_fill(32, 16, 90);
    pc_buf jpeg = enc(&g, PC_CF_JPEG, NULL);
    for (big = 0; big < 2; big++) {
        uint32_t o;
        size_t i;
        entry es[2];
        pc_buf e;
        pc_buf file;
        pc_buf want;
        pc_cimage img;
        es[0] = ent(0x0131, 2, 4, (const uint8_t *)"PC1\0");
        for (o = 1; o <= 8; o++) {
            pc_buf fixed;
            bool br;
            es[1] = long_ent(0x0112, 4, 1, o, big);
            e = ifd(big, es, 2, "tail", 4);
            CHECK_EQ_UINT(pc_exif_orientation(e.data, e.len), o);
            fixed = ux(e.data, e.len, &br);
            CHECK_EQ_UINT(pc_exif_orientation(fixed.data, fixed.len), 1);
            /* Still a LONG, all 4 value bytes set to 1 in the file's byte order; nothing else moved. */
            es[1] = long_ent(0x0112, 4, 1, 1, big);
            want = ifd(big, es, 2, "tail", 4);
            if (!buf_eq(&fixed, &want)) {
                PC_T_FAIL("o=%u big=%d", o, big);
            }
            pc_buf_fini(&want);
            pc_buf_fini(&fixed);
            pc_buf_fini(&e);
        }
        /* Out of range, including values that only fit in a LONG, reads as 1 and is left alone. */
        for (i = 0; i < sizeof bads / sizeof bads[0]; i++) {
            es[1] = long_ent(0x0112, 4, 1, bads[i], big);
            e = ifd(big, &es[1], 1, NULL, 0);
            if (pc_exif_orientation(e.data, e.len) != 1) {
                PC_T_FAIL("%u", bads[i]);
            }
            pc_buf_fini(&e);
        }
        /* A LONG 6 in a JPEG opens upright. */
        es[1] = long_ent(0x0112, 4, 1, 6, big);
        e = ifd(big, &es[1], 1, NULL, 0);
        file = jpeg_with_exif(&jpeg, e.data, e.len);
        img = dec(file.data, file.len);
        CHECK(img.width == 16 && img.height == 32);
        es[1] = long_ent(0x0112, 4, 1, 1, big);
        want = ifd(big, &es[1], 1, NULL, 0);
        CHECK(img.meta.exif.has);
        if (img.meta.exif.has) {
            const uint8_t *k = img.meta.exif.data;
            size_t kn = img.meta.exif.len;
            if (kn >= 6 && memcmp(k, "Exif\0\0", 6) == 0) {
                k += 6;
                kn -= 6;
            }
            CHECK(kn == want.len && memcmp(k, want.data, kn) == 0);
        }
        pc_buf_fini(&want);
        pc_cimage_fini(&img);
        pc_buf_fini(&file);
        pc_buf_fini(&e);
    }
    pc_buf_fini(&jpeg);
    pc_cimage_fini(&g);
}

/* Unit test an_impossible_allocation_is_an_error_not_an_abort. alloc_zeroed and orient_n are
 * private, so this goes through pc_cimage_oriented: an image claiming SIZE_MAX bytes fails to
 * allocate with PC_ERR_LIMIT (and is freed), and a 3-byte image orients into a fresh buffer. */
static void test_an_impossible_allocation_is_an_error_not_an_abort(void)
{
    pc_cimage huge;
    pc_cimage out;
    pc_cimage small;
    pc_error e;
    static const uint8_t px[3] = {1, 2, 3};
    pc_cimage_init(&huge);
    huge.width = 1;
    huge.height = 1;
    huge.data = malloc(1);
    huge.len = SIZE_MAX;
    CHECK_EQ_INT(pc_cimage_oriented(&huge, 6, &out, &e), PC_ERR_LIMIT);
    CHECK_CONTAINS(e.msg, "limit exceeded: cannot allocate");
    CHECK(huge.data == NULL);
    /* orient_n of an empty image: the public API rejects 0x0 (as Rust's oriented does). */
    pc_cimage_init(&huge);
    CHECK_EQ_INT(pc_cimage_oriented(&huge, 6, &out, &e), PC_ERR_INVALID);
    /* A small allocation works: 3x1 turned to 1x3. */
    CHECK_EQ_INT(pc_cimage_from_copy(3, 1, PC_CL_GRAY, PC_CS_U8, px, 3, &small, NULL), PC_OK);
    CHECK_EQ_INT(pc_cimage_oriented(&small, 6, &out, &e), PC_OK);
    CHECK(out.width == 1 && out.height == 3 && out.len == 3);
    CHECK(out.data[0] == 1 && out.data[1] == 2 && out.data[2] == 3);
    pc_cimage_fini(&out);
}

int main(int argc, char **argv)
{
    RUN(test_reads_orientation_in_every_encoding);
    RUN(test_malformed_or_out_of_range_orientation_reads_as_1);
    RUN(test_random_exif_never_panics);
    RUN(test_random_xmp_never_panics);
    RUN(test_xmp_orientation_is_rewritten);
    RUN(test_png_opens_upright_for_every_orientation);
    RUN(test_tiff_opens_upright_for_every_orientation);
    RUN(test_webp_opens_upright);
    RUN(test_jpeg_opens_upright_for_every_orientation);
    RUN(test_rgb_jpeg_with_dpi_swaps_resolution_on_a_quarter_turn);
    RUN(test_export_writes_orientation_1_and_keeps_pixels);
    RUN(test_round_trip_open_save_reopen_is_stable);
    RUN(test_malformed_exif_in_a_jpeg_opens_as_stored);
    RUN(test_every_layout_and_depth_orients_and_inverts);
    RUN(test_rotating_24_mp_is_fast);
    RUN(test_duplicate_orientation_entries_first_wins);
    RUN(test_strict_ifd_parsing_ignores_the_tag);
    RUN(test_upright_exif_keeps_every_other_byte);
    RUN(test_limits_apply_to_the_upright_size);
    RUN(test_long_orientation_reads_and_rewrites_in_both_byte_orders);
    RUN(test_an_impossible_allocation_is_an_error_not_an_abort);
    return pc_t_finish();
}
