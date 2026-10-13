/* Ports crates/codecs/tests/{gif_frames,pcx}.rs, the unit tests of crates/codecs/src/codecs/fits.rs
 * and the fuzz target crates/codecs/fuzz/fuzz_targets/decode_gif.rs (as a never-crash sweep), plus
 * hostile-input sweeps (every prefix, every byte mutated) for the new FITS, PCX and GIF-frames
 * decoders. The `image`-crate GIF encoder of the Rust tests is replaced by the in-tree animated GIF
 * writer (web.h): the decoder sees the same frames, delays and full-screen palettes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "codecs/codecs.h"
#include "codecs/web.h"
#include "harness.h"

/* ---- gif_frames.rs ---- */

static const uint8_t RED[4] = {255, 0, 0, 255};
static const uint8_t GREEN[4] = {0, 255, 0, 255};
static const uint8_t BLUE[4] = {0, 0, 255, 255};
static const uint8_t CLEAR[4] = {0, 0, 0, 0};

/* Solid full-screen frames, delay_ms apart. */
static pc_buf solid_gif(const uint8_t *const *colours, size_t n, uint32_t w, uint32_t h, uint32_t delay_ms)
{
    pc_buf out = PC_BUF_INIT;
    pc_gif_frame fr[3];
    uint8_t pal[3][1][3];
    uint8_t *idx = calloc((size_t)w * h, 1);
    size_t i;
    for (i = 0; i < n && i < 3; i++) {
        memcpy(pal[i][0], colours[i], 3);
        fr[i].indices = idx;
        fr[i].n = (size_t)w * h;
        fr[i].palette = (const uint8_t (*)[3])pal[i];
        fr[i].npal = 1;
        fr[i].transparent = -1;
        fr[i].delay_cs = (uint16_t)(delay_ms / 10);
    }
    CHECK_EQ_INT(pc_web_encode_gif_animated(w, h, fr, n, false, &out, NULL), PC_OK);
    free(idx);
    return out;
}

/* A palette-index image LZW-packed with a 2-bit minimum code size (code size stays 3 bits). */
static void lzw2(const uint8_t *idx, size_t n, pc_buf *out)
{
    uint32_t acc = 0;
    unsigned nbits = 0;
    size_t i;
    for (i = 0; i < n + 2; i++) {
        uint32_t c = i == 0 ? 4u : i == n + 1 ? 5u : idx[i - 1];
        acc |= c << nbits;
        nbits += 3;
        while (nbits >= 8) {
            (void)pc_buf_append_byte(out, (uint8_t)(acc & 0xFF), NULL);
            acc >>= 8;
            nbits -= 8;
        }
    }
    if (nbits > 0) {
        (void)pc_buf_append_byte(out, (uint8_t)acc, NULL);
    }
}

typedef struct part {
    uint16_t x, y, w, h;
    uint8_t disposal;
    const uint8_t *idx;
} part;

static void le16(pc_buf *b, uint16_t v)
{
    uint8_t x[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    (void)pc_buf_append(b, x, 2, NULL);
}

/* A hand-built GIF89a: palette red, green, blue, black; frames 10 cs apart. */
static pc_buf raw_gif(uint16_t w, uint16_t h, const part *parts, size_t np)
{
    static const uint8_t pal[12] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0};
    pc_buf b = PC_BUF_INIT;
    size_t i;
    (void)pc_buf_append(&b, "GIF89a", 6, NULL);
    le16(&b, w);
    le16(&b, h);
    (void)pc_buf_append(&b, "\x81\0\0", 3, NULL);
    (void)pc_buf_append(&b, pal, 12, NULL);
    for (i = 0; i < np; i++) {
        uint8_t gce[8] = {0x21, 0xF9, 4, (uint8_t)(parts[i].disposal << 2), 10, 0, 0, 0};
        pc_buf data = PC_BUF_INIT;
        (void)pc_buf_append(&b, gce, 8, NULL);
        (void)pc_buf_append_byte(&b, 0x2C, NULL);
        le16(&b, parts[i].x);
        le16(&b, parts[i].y);
        le16(&b, parts[i].w);
        le16(&b, parts[i].h);
        (void)pc_buf_append(&b, "\0\2", 2, NULL);
        lzw2(parts[i].idx, (size_t)parts[i].w * parts[i].h, &data);
        (void)pc_buf_append_byte(&b, (uint8_t)data.len, NULL);
        (void)pc_buf_append(&b, data.data, data.len, NULL);
        (void)pc_buf_append_byte(&b, 0, NULL);
        pc_buf_fini(&data);
    }
    (void)pc_buf_append_byte(&b, 0x3B, NULL);
    return b;
}

static pc_status frames(const pc_buf *b, const pc_cdecode_opts *o, pc_canim_frame **f, size_t *n)
{
    pc_cdecode_opts d = pc_cdecode_opts_default();
    return pc_cdecode_gif_frames(b->data, b->len, o != NULL ? o : &d, f, n, NULL);
}

/* Frame i's RGBA8 equals the pixels in want (npx RGBA quads). */
static bool frame_is(const pc_canim_frame *f, const uint8_t *const *want, size_t npx)
{
    uint8_t *rgba = NULL;
    size_t n = 0;
    bool ok = pc_cimage_to_rgba8(&f->image, &rgba, &n, NULL) == PC_OK && n == npx * 4;
    size_t i;
    for (i = 0; ok && i < npx; i++) {
        ok = memcmp(rgba + 4 * i, want[i], 4) == 0;
    }
    free(rgba);
    return ok;
}

static void test_a_still_gif_gives_one_frame_equal_to_decode(void)
{
    const uint8_t *c[1] = {RED};
    pc_buf g = solid_gif(c, 1, 3, 2, 100);
    pc_canim_frame *f = NULL;
    size_t n = 0;
    pc_cimage img;
    CHECK_EQ_INT(frames(&g, NULL, &f, &n), PC_OK);
    CHECK_EQ_UINT(n, 1);
    CHECK_EQ_INT(pc_cdecode(g.data, g.len, &img, NULL), PC_OK);
    if (n == 1) {
        CHECK(f[0].image.width == 3 && f[0].image.height == 2);
        CHECK(pc_cimage_eq(&f[0].image, &img) ||
              (f[0].image.len == img.len && memcmp(f[0].image.data, img.data, img.len) == 0));
        CHECK_EQ_UINT(f[0].image.nwarnings, 0);
    }
    pc_cimage_fini(&img);
    pc_canim_frames_free(f, n);
    pc_buf_fini(&g);
}

static void test_every_frame_comes_back_in_order_with_its_delay(void)
{
    const uint8_t *c[3] = {RED, GREEN, BLUE};
    pc_buf g = solid_gif(c, 3, 2, 2, 120);
    pc_canim_frame *f = NULL;
    size_t n = 0;
    size_t i;
    CHECK_EQ_INT(frames(&g, NULL, &f, &n), PC_OK);
    CHECK_EQ_UINT(n, 3);
    for (i = 0; i < n && i < 3; i++) {
        const uint8_t *w[4] = {c[i], c[i], c[i], c[i]};
        CHECK(frame_is(&f[i], w, 4));
        CHECK_EQ_UINT(f[i].delay_ms, 120);
    }
    pc_canim_frames_free(f, n);
    pc_buf_fini(&g);
}

static void test_disposal_methods(void)
{
    static const uint8_t i00[2] = {0, 0};
    static const uint8_t i11[2] = {1, 1};
    static const uint8_t i2[1] = {2};
    static const uint8_t i1[1] = {1};
    pc_canim_frame *f = NULL;
    size_t n = 0;
    /* A smaller frame is drawn over the previous one. */
    {
        part p[2] = {{0, 0, 2, 1, 1, i00}, {1, 0, 1, 1, 1, i2}};
        pc_buf g = raw_gif(2, 1, p, 2);
        const uint8_t *w0[2] = {RED, RED};
        const uint8_t *w1[2] = {RED, BLUE};
        CHECK_EQ_INT(frames(&g, NULL, &f, &n), PC_OK);
        CHECK(n == 2 && frame_is(&f[0], w0, 2) && frame_is(&f[1], w1, 2) && f[1].delay_ms == 100);
        pc_canim_frames_free(f, n);
        pc_buf_fini(&g);
    }
    /* Disposal to background clears the previous frame. */
    {
        part p[2] = {{0, 0, 2, 1, 2, i11}, {1, 0, 1, 1, 1, i2}};
        pc_buf g = raw_gif(2, 1, p, 2);
        const uint8_t *w0[2] = {GREEN, GREEN};
        const uint8_t *w1[2] = {CLEAR, BLUE};
        CHECK_EQ_INT(frames(&g, NULL, &f, &n), PC_OK);
        CHECK(n == 2 && frame_is(&f[0], w0, 2) && frame_is(&f[1], w1, 2));
        pc_canim_frames_free(f, n);
        pc_buf_fini(&g);
    }
    /* Disposal to previous restores what was there. */
    {
        part p[3] = {{0, 0, 2, 1, 1, i00}, {0, 0, 1, 1, 3, i1}, {1, 0, 1, 1, 1, i2}};
        pc_buf g = raw_gif(2, 1, p, 3);
        const uint8_t *w1[2] = {GREEN, RED};
        const uint8_t *w2[2] = {RED, BLUE};
        CHECK_EQ_INT(frames(&g, NULL, &f, &n), PC_OK);
        CHECK(n == 3 && frame_is(&f[1], w1, 2) && frame_is(&f[2], w2, 2));
        pc_canim_frames_free(f, n);
        pc_buf_fini(&g);
    }
}

static void test_a_damaged_later_frame_keeps_the_frames_before_it(void)
{
    const uint8_t *c[3] = {RED, GREEN, BLUE};
    pc_buf g = solid_gif(c, 3, 8, 8, 100);
    pc_buf cut = PC_BUF_INIT;
    pc_canim_frame *f = NULL;
    size_t n = 0;
    size_t i;
    (void)pc_buf_append(&cut, g.data, g.len - 12, NULL);
    CHECK_EQ_INT(frames(&cut, NULL, &f, &n), PC_OK);
    CHECK(n >= 1 && n < 3);
    if (n >= 1) {
        CHECK(f[0].image.nwarnings == 1 && f[0].image.warnings[0].kind == PC_CW_TRUNCATED &&
              f[0].image.warnings[0].format == PC_CF_GIF);
        for (i = 1; i < n; i++) {
            CHECK_EQ_UINT(f[i].image.nwarnings, 0);
        }
    }
    pc_canim_frames_free(f, n);
    pc_buf_fini(&cut);
    pc_buf_fini(&g);
}

static void test_the_first_frame_must_decode(void)
{
    pc_cdecode_opts o = pc_cdecode_opts_default();
    pc_canim_frame *f = NULL;
    size_t n = 0;
    uint8_t ff[64];
    memset(ff, 0xFF, sizeof ff);
    CHECK(pc_cdecode_gif_frames((const uint8_t *)"", 0, &o, &f, &n, NULL) != PC_OK);
    CHECK(pc_cdecode_gif_frames((const uint8_t *)"GIF89a", 6, &o, &f, &n, NULL) != PC_OK);
    CHECK(pc_cdecode_gif_frames(ff, sizeof ff, &o, &f, &n, NULL) != PC_OK);
}

/* every_prefix_of_a_gif_returns + the decode_gif fuzz target: prefixes and byte mutations. */
static void test_every_prefix_and_mutation_returns(void)
{
    const uint8_t *c[3] = {RED, GREEN, BLUE};
    pc_buf g = solid_gif(c, 3, 5, 4, 100);
    pc_cdecode_opts o = pc_cdecode_opts_default();
    size_t i;
    for (i = 0; i <= g.len; i++) {
        pc_canim_frame *f = NULL;
        size_t n = 0;
        pc_cimage img;
        if (pc_cdecode_gif_frames(g.data, i, &o, &f, &n, NULL) == PC_OK) {
            pc_canim_frames_free(f, n);
        }
        if (pc_cdecode_as(PC_CF_GIF, g.data, i, &img, NULL) == PC_OK) {
            pc_cimage_fini(&img);
        }
    }
    for (i = 0; i < g.len; i++) {
        pc_canim_frame *f = NULL;
        size_t n = 0;
        g.data[i] ^= 0x5a;
        if (pc_cdecode_gif_frames(g.data, g.len, &o, &f, &n, NULL) == PC_OK) {
            size_t k;
            CHECK(n >= 1 && n <= PC_MAX_GIF_FRAMES);
            for (k = 0; k < n; k++) {
                CHECK(f[k].image.width > 0 && f[k].image.layout == PC_CL_RGBA);
            }
            pc_canim_frames_free(f, n);
        }
        g.data[i] ^= 0x5a;
    }
    pc_buf_fini(&g);
}

static void test_limits_apply(void)
{
    const uint8_t *c[3] = {RED, GREEN, BLUE};
    pc_buf one = solid_gif(c, 1, 4, 4, 100);
    pc_buf two = solid_gif(c, 2, 4, 4, 100);
    pc_buf three = solid_gif(c, 3, 4, 4, 100);
    pc_cdecode_opts o = pc_cdecode_opts_default();
    pc_canim_frame *f = NULL;
    size_t n = 0;
    o.limits.max_width = 3;
    CHECK_EQ_INT(frames(&one, &o, &f, &n), PC_ERR_LIMIT);
    /* One 4x4 RGBA frame is 64 bytes: two frames fit in 128, three do not. */
    o = pc_cdecode_opts_default();
    o.limits.max_alloc = 128;
    CHECK_EQ_INT(frames(&two, &o, &f, &n), PC_OK);
    CHECK_EQ_UINT(n, 2);
    pc_canim_frames_free(f, n);
    CHECK_EQ_INT(frames(&three, &o, &f, &n), PC_ERR_LIMIT);
    pc_buf_fini(&one);
    pc_buf_fini(&two);
    pc_buf_fini(&three);
}

/* ---- pcx.rs ---- */

static pc_buf pcx_header(uint8_t bits, uint8_t planes, uint16_t w, uint16_t h, uint16_t stride)
{
    pc_buf b = PC_BUF_INIT;
    uint8_t x[128];
    memset(x, 0, sizeof x);
    x[0] = 0x0a;
    x[1] = 5;
    x[2] = 1;
    x[3] = bits;
    x[8] = (uint8_t)(w - 1);
    x[9] = (uint8_t)((w - 1) >> 8);
    x[10] = (uint8_t)(h - 1);
    x[11] = (uint8_t)((h - 1) >> 8);
    x[12] = 300 & 0xff;
    x[13] = 300 >> 8;
    x[14] = 150;
    x[65] = planes;
    x[66] = (uint8_t)stride;
    x[67] = (uint8_t)(stride >> 8);
    x[68] = 1;
    (void)pc_buf_append(&b, x, sizeof x, NULL);
    return b;
}

static void rle_row(pc_buf *b, const uint8_t *row, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (row[i] >= 0xc0) {
            (void)pc_buf_append_byte(b, 0xc1, NULL);
        }
        (void)pc_buf_append_byte(b, row[i], NULL);
    }
}

static void test_pcx_is_readable_but_not_writable(void)
{
    pc_cformat f;
    pc_cimage img;
    pc_buf out = PC_BUF_INIT;
    static const uint8_t px[3] = {0, 0, 0};
    CHECK(pc_cformat_from_extension("archive.PCX", &f) && f == PC_CF_PCX);
    CHECK(pc_cformat_caps(PC_CF_PCX).read && !pc_cformat_caps(PC_CF_PCX).write);
    CHECK_EQ_INT(pc_cimage_from_copy(1, 1, PC_CL_RGB, PC_CS_U8, px, 3, &img, NULL), PC_OK);
    CHECK_EQ_INT(pc_cencode(&img, PC_CF_PCX, NULL, &out, NULL), PC_ERR_UNSUPPORTED);
    pc_cimage_fini(&img);
    pc_buf_fini(&out);
}

static void test_pcx_decodes(void)
{
    static const uint8_t r[2] = {255, 0};
    static const uint8_t gg[2] = {0, 255};
    static const uint8_t z[2] = {0, 0};
    static const uint8_t rgb[6] = {255, 0, 0, 0, 255, 0};
    pc_buf b = pcx_header(8, 3, 2, 1, 2);
    pc_cimage img;
    pc_cformat f;
    rle_row(&b, r, 2);
    rle_row(&b, gg, 2);
    rle_row(&b, z, 2);
    CHECK(pc_cformat_detect(b.data, b.len, &f) && f == PC_CF_PCX);
    CHECK_EQ_INT(pc_cdecode(b.data, b.len, &img, NULL), PC_OK);
    CHECK(img.layout == PC_CL_RGB && img.sample == PC_CS_U8 && img.width == 2 && img.height == 1);
    CHECK(img.len == 6 && memcmp(img.data, rgb, 6) == 0);
    CHECK(img.meta.has_dpi && img.meta.dpi_x == 300.0f && img.meta.dpi_y == 150.0f);
    pc_cimage_fini(&img);
    pc_buf_fini(&b);
    /* 256-colour VGA palette */
    {
        static const uint8_t idx[2] = {1, 2};
        uint8_t pal[768];
        b = pcx_header(8, 1, 2, 1, 2);
        rle_row(&b, idx, 2);
        (void)pc_buf_append_byte(&b, 0x0c, NULL);
        memset(pal, 0, sizeof pal);
        pal[3] = 255;
        pal[7] = 255;
        (void)pc_buf_append(&b, pal, sizeof pal, NULL);
        CHECK_EQ_INT(pc_cdecode(b.data, b.len, &img, NULL), PC_OK);
        CHECK(img.len == 6 && memcmp(img.data, rgb, 6) == 0);
        pc_cimage_fini(&img);
        pc_buf_fini(&b);
    }
    /* 4-bit and monochrome palettes */
    {
        static const uint8_t four_row[2] = {0x12, 0};
        static const uint8_t one_row[2] = {0x80, 0};
        static const uint8_t want4[6] = {255, 0, 0, 0, 0, 255};
        static const uint8_t want1[6] = {255, 0, 0, 0, 0, 0};
        b = pcx_header(4, 1, 2, 1, 2);
        b.data[16 + 3] = 255;
        b.data[16 + 8] = 255;
        rle_row(&b, four_row, 2);
        CHECK_EQ_INT(pc_cdecode(b.data, b.len, &img, NULL), PC_OK);
        CHECK(img.len == 6 && memcmp(img.data, want4, 6) == 0);
        pc_cimage_fini(&img);
        pc_buf_fini(&b);
        b = pcx_header(1, 1, 2, 1, 2);
        b.data[16 + 3] = 255;
        rle_row(&b, one_row, 2);
        CHECK_EQ_INT(pc_cdecode(b.data, b.len, &img, NULL), PC_OK);
        CHECK(img.len == 6 && memcmp(img.data, want1, 6) == 0);
        pc_cimage_fini(&img);
        pc_buf_fini(&b);
    }
}

static void test_pcx_rejects_truncated_and_overshooting_rle(void)
{
    pc_buf b = pcx_header(8, 3, 2, 1, 2);
    pc_cimage img;
    pc_cformat f;
    CHECK(pc_cdecode_as(PC_CF_PCX, b.data, b.len, &img, NULL) != PC_OK);
    (void)pc_buf_append(&b, "\xc5\xff", 2, NULL);
    CHECK(pc_cdecode_as(PC_CF_PCX, b.data, b.len, &img, NULL) != PC_OK);
    pc_buf_fini(&b);
    b = pcx_header(8, 3, 2, 1, 2);
    (void)pc_buf_append_byte(&b, 0xc1, NULL);
    CHECK(pc_cdecode_as(PC_CF_PCX, b.data, b.len, &img, NULL) != PC_OK);
    pc_buf_fini(&b);
    b = pcx_header(8, 3, 2, 1, 2);
    b.data[66] = 0;
    b.data[67] = 0;
    CHECK(!(pc_cformat_detect(b.data, b.len, &f) && f == PC_CF_PCX));
    pc_buf_fini(&b);
}

static void test_pcx_refuses_headers_over_budget(void)
{
    pc_buf b = pcx_header(8, 3, 3000, 3000, 3000);
    pc_cdecode_opts o = pc_cdecode_opts_default();
    pc_cimage img;
    pc_cformat f;
    o.limits.max_alloc = 1024;
    CHECK_EQ_INT(pc_cdecode_as_with(PC_CF_PCX, b.data, b.len, &o, &img, NULL), PC_ERR_LIMIT);
    b.data[2] = 0;
    CHECK(!(pc_cformat_detect(b.data, b.len, &f) && f == PC_CF_PCX));
    pc_buf_fini(&b);
}

static void sweep_decoder(pc_cformat fmt, pc_buf *b)
{
    size_t i;
    pc_cimage img;
    for (i = 0; i <= b->len; i++) {
        if (pc_cdecode_as(fmt, b->data, i, &img, NULL) == PC_OK) {
            pc_cimage_fini(&img);
        }
    }
    for (i = 0; i < b->len; i++) {
        uint8_t keep = b->data[i];
        int k;
        for (k = 0; k < 3; k++) {
            b->data[i] = (uint8_t)(k == 0 ? keep ^ 0x5a : k == 1 ? 0xff : 0x00);
            if (pc_cdecode_as(fmt, b->data, b->len, &img, NULL) == PC_OK) {
                CHECK(img.len == pc_cimage_sample_count(&img) * (img.sample == PC_CS_U8    ? 1u
                                                                 : img.sample == PC_CS_F32 ? 4u
                                                                                           : 2u));
                pc_cimage_fini(&img);
            }
        }
        b->data[i] = keep;
    }
}

static void test_pcx_hostile_sweep(void)
{
    static const uint8_t idx[4] = {1, 2, 0xc3, 0};
    uint8_t pal[768];
    pc_buf b = pcx_header(8, 1, 4, 1, 4);
    rle_row(&b, idx, 4);
    (void)pc_buf_append_byte(&b, 0x0c, NULL);
    memset(pal, 7, sizeof pal);
    (void)pc_buf_append(&b, pal, sizeof pal, NULL);
    sweep_decoder(PC_CF_PCX, &b);
    pc_buf_fini(&b);
}

/* ---- fits.rs ---- */

/* A FITS header block: each card padded to 80 characters, END, padded to 2880. */
static void fits_header(pc_buf *b, const char *const *cards, size_t n)
{
    size_t i;
    size_t start = b->len;
    for (i = 0; i <= n; i++) {
        const char *c = i < n ? cards[i] : "END";
        size_t l = strlen(c);
        size_t k;
        (void)pc_buf_append(b, c, l, NULL);
        for (k = l; k < 80; k++) {
            (void)pc_buf_append_byte(b, ' ', NULL);
        }
    }
    while ((b->len - start) % 2880 != 0) {
        (void)pc_buf_append_byte(b, ' ', NULL);
    }
}

static void pad_block(pc_buf *b)
{
    while (b->len % 2880 != 0) {
        (void)pc_buf_append_byte(b, 0, NULL);
    }
}

static bool has_text(const pc_cimage *img, const char *k, const char *v)
{
    size_t i;
    for (i = 0; i < img->meta.ntext; i++) {
        if (strcmp(img->meta.text[i].key, k) == 0 && (v == NULL || strcmp(img->meta.text[i].value, v) == 0)) {
            return true;
        }
    }
    return false;
}

#define NC(a) (sizeof(a) / sizeof(a)[0])

static void test_fits_camera_u16_with_bzero_reads_exactly_and_bottom_up(void)
{
    static const char *const cards[] = {
        "SIMPLE  =                    T", "BITPIX  =                   16",
        "NAXIS   =                    2", "NAXIS1  =                    2",
        "NAXIS2  =                    2", "BZERO   =                32768",
        "OBJECT  = 'M 31    '",           "EXPTIME =                300.0 / seconds"};
    static const uint16_t vals[4] = {0, 1000, 40000, 65535};
    static const uint16_t want[4] = {40000, 65535, 0, 1000};
    pc_buf b = PC_BUF_INIT;
    pc_cimage img;
    size_t i;
    fits_header(&b, cards, NC(cards));
    for (i = 0; i < 4; i++) {
        uint16_t s = (uint16_t)(vals[i] ^ 0x8000u);
        uint8_t x[2] = {(uint8_t)(s >> 8), (uint8_t)s};
        (void)pc_buf_append(&b, x, 2, NULL);
    }
    pad_block(&b);
    CHECK_EQ_INT(pc_cdecode_as(PC_CF_FITS, b.data, b.len, &img, NULL), PC_OK);
    CHECK(img.sample == PC_CS_U16);
    for (i = 0; i < 4 && img.len == 8; i++) {
        uint16_t v;
        memcpy(&v, img.data + 2 * i, 2);
        CHECK_EQ_UINT(v, want[i]);
    }
    CHECK(has_text(&img, "OBJECT", "M 31"));
    CHECK(has_text(&img, "EXPTIME", "300.0"));
    CHECK(!has_text(&img, "BZERO", NULL) && !has_text(&img, "NAXIS1", NULL));
    pc_cimage_fini(&img);
    pc_buf_fini(&b);
}

static void test_fits_small_cases(void)
{
    pc_cimage img;
    pc_buf b = PC_BUF_INIT;
    /* ROWORDER top-down is honoured */
    {
        static const char *const c[] = {"SIMPLE  = T", "BITPIX  = 8", "NAXIS   = 2",
                                        "NAXIS1  = 1", "NAXIS2  = 2", "ROWORDER= 'TOP-DOWN'"};
        fits_header(&b, c, NC(c));
        (void)pc_buf_append(&b, "\x0a\x14", 2, NULL);
        CHECK_EQ_INT(pc_cdecode_as(PC_CF_FITS, b.data, b.len, &img, NULL), PC_OK);
        CHECK(img.len == 2 && img.data[0] == 10 && img.data[1] == 20);
        pc_cimage_fini(&img);
        pc_buf_fini(&b);
    }
    /* Stacked float in ADU is scaled by 65535; NaN reads as 0 */
    {
        static const char *const c[] = {"SIMPLE  = T", "BITPIX  = -32", "NAXIS   = 2", "NAXIS1  = 2",
                                        "NAXIS2  = 1"};
        static const uint8_t v[8] = {0x47, 0x7f, 0xff, 0x00, 0x7f, 0xc0, 0, 0};
        fits_header(&b, c, NC(c));
        (void)pc_buf_append(&b, v, 8, NULL);
        CHECK_EQ_INT(pc_cdecode_as(PC_CF_FITS, b.data, b.len, &img, NULL), PC_OK);
        if (img.len == 8) {
            float f[2];
            memcpy(f, img.data, 8);
            CHECK(f[0] == 1.0f && f[1] == 0.0f);
        }
        pc_cimage_fini(&img);
        pc_buf_fini(&b);
    }
    /* Signed negative data falls back to float */
    {
        static const char *const c[] = {"SIMPLE  = T", "BITPIX  = 16", "NAXIS   = 2", "NAXIS1  = 2",
                                        "NAXIS2  = 1"};
        fits_header(&b, c, NC(c));
        (void)pc_buf_append(&b, "\xff\x9c\x03\xe8", 4, NULL);
        CHECK_EQ_INT(pc_cdecode_as(PC_CF_FITS, b.data, b.len, &img, NULL), PC_OK);
        CHECK(img.sample == PC_CS_F32);
        pc_cimage_fini(&img);
        pc_buf_fini(&b);
    }
    /* Image extension after an empty primary */
    {
        static const char *const p[] = {"SIMPLE  = T", "BITPIX  = 8", "NAXIS   = 0", "EXTEND  = T",
                                        "TELESCOP= 'Seestar S50'"};
        static const char *const x[] = {"XTENSION= 'IMAGE   '", "BITPIX  = 8", "NAXIS   = 2", "NAXIS1  = 1",
                                        "NAXIS2  = 1",          "PCOUNT  = 0", "GCOUNT  = 1"};
        fits_header(&b, p, NC(p));
        fits_header(&b, x, NC(x));
        (void)pc_buf_append_byte(&b, 42, NULL);
        CHECK_EQ_INT(pc_cdecode_as(PC_CF_FITS, b.data, b.len, &img, NULL), PC_OK);
        CHECK(img.len == 1 && img.data[0] == 42);
        CHECK(has_text(&img, "TELESCOP", "Seestar S50"));
        pc_cimage_fini(&img);
        pc_buf_fini(&b);
    }
    /* fpack is refused as unsupported */
    {
        static const char *const p[] = {"SIMPLE  = T", "BITPIX  = 8", "NAXIS   = 0"};
        static const char *const x1[] = {"XTENSION= 'BINTABLE'", "BITPIX  = 8", "NAXIS   = 2",
                                         "NAXIS1  = 0",          "NAXIS2  = 0", "ZIMAGE  = T"};
        static const char *const x2[] = {"XTENSION= 'BINTABLE'", "BITPIX  = 8", "NAXIS   = 1", "NAXIS1  = 0"};
        fits_header(&b, p, NC(p));
        fits_header(&b, x1, NC(x1));
        fits_header(&b, x2, NC(x2));
        CHECK_EQ_INT(pc_cdecode_as(PC_CF_FITS, b.data, b.len, &img, NULL), PC_ERR_UNSUPPORTED);
        pc_buf_fini(&b);
    }
    /* Extra planes warn */
    {
        static const char *const c[] = {"SIMPLE  = T", "BITPIX  = 8", "NAXIS   = 3",
                                        "NAXIS1  = 1", "NAXIS2  = 1", "NAXIS3  = 4"};
        fits_header(&b, c, NC(c));
        (void)pc_buf_append(&b, "\1\2\3\4", 4, NULL);
        CHECK_EQ_INT(pc_cdecode_as(PC_CF_FITS, b.data, b.len, &img, NULL), PC_OK);
        CHECK(img.layout == PC_CL_GRAY);
        CHECK(img.nwarnings == 1 && img.warnings[0].kind == PC_CW_MORE_PAGES && img.warnings[0].has_total &&
              img.warnings[0].total == 4);
        pc_cimage_fini(&img);
        pc_buf_fini(&b);
    }
}

static void test_fits_hostile_headers_error_without_crashing(void)
{
    static const char *const cases[][4] = {
        {"NAXIS   = 2", "NAXIS1  = 4294967296", "NAXIS2  = 1", NULL},
        {"NAXIS   = 2", "NAXIS1  = -5", "NAXIS2  = 1", NULL},
        {"NAXIS   = 2", "NAXIS1  = 2000000", "NAXIS2  = 2000000", NULL},
        {"NAXIS   = 3", "NAXIS1  = 65536", "NAXIS2  = 65536", "NAXIS3  = 9223372036854775807"},
        {"NAXIS   = 1000", NULL, NULL, NULL},
        {"NAXIS   = -1", NULL, NULL, NULL},
        {"NAXIS   = 2", "NAXIS1  = 0", "NAXIS2  = 0", NULL},
        {"NAXIS   = 2", "NAXIS1  = 1.5D300", "NAXIS2  = 1", NULL},
        {"NAXIS   = 2", NULL, NULL, NULL},
        {"NAXIS   = 2", "NAXIS1  = 100", "NAXIS2  = 100", NULL},
    };
    static const char *const bitpix[5] = {"BITPIX  = 0", "BITPIX  = 7", "BITPIX  = -16", "BITPIX  = 128",
                                          "BITPIX  = 'X'"};
    pc_cimage img;
    pc_buf b = PC_BUF_INIT;
    size_t i;
    size_t k;
    for (i = 0; i < NC(cases); i++) {
        const char *cards[6] = {"SIMPLE  = T", "BITPIX  = 16"};
        size_t n = 2;
        for (k = 0; k < 4 && cases[i][k] != NULL; k++) {
            cards[n++] = cases[i][k];
        }
        fits_header(&b, cards, n);
        CHECK(pc_cdecode_as(PC_CF_FITS, b.data, b.len, &img, NULL) != PC_OK);
        pc_buf_fini(&b);
    }
    for (i = 0; i < 5; i++) {
        const char *cards[5] = {"SIMPLE  = T", bitpix[i], "NAXIS   = 2", "NAXIS1  = 1", "NAXIS2  = 1"};
        fits_header(&b, cards, 5);
        CHECK(pc_cdecode_as(PC_CF_FITS, b.data, b.len, &img, NULL) != PC_OK);
        pc_buf_fini(&b);
    }
    /* No END card: the header runs off the end of the file. */
    (void)pc_buf_append(&b, "SIMPLE  = T", 11, NULL);
    while (b.len < 240) {
        (void)pc_buf_append_byte(&b, ' ', NULL);
    }
    CHECK(pc_cdecode_as(PC_CF_FITS, b.data, b.len, &img, NULL) != PC_OK);
    pc_buf_fini(&b);
    /* An empty primary followed by many empty extensions: a bounded walk, then "no image". */
    {
        static const char *const p[] = {"SIMPLE  = T", "BITPIX  = 8", "NAXIS   = 0"};
        static const char *const x[] = {"XTENSION= 'IMAGE   '", "BITPIX  = 8", "NAXIS   = 0"};
        fits_header(&b, p, 3);
        for (i = 0; i < 300; i++) {
            fits_header(&b, x, 3);
        }
        CHECK(pc_cdecode_as(PC_CF_FITS, b.data, b.len, &img, NULL) != PC_OK);
        pc_buf_fini(&b);
    }
    /* An extension whose declared data size overflows. */
    {
        static const char *const p[] = {"SIMPLE  = T", "BITPIX  = 8", "NAXIS   = 0"};
        static const char *const x[] = {
            "XTENSION= 'BINTABLE'", "BITPIX  = 64", "NAXIS   = 2", "NAXIS1  = 9223372036854775807",
            "NAXIS2  = 4",          "PCOUNT  = 0",  "GCOUNT  = 1"};
        fits_header(&b, p, 3);
        fits_header(&b, x, NC(x));
        CHECK(pc_cdecode_as(PC_CF_FITS, b.data, b.len, &img, NULL) != PC_OK);
        pc_buf_fini(&b);
    }
}

/* strings_with_quotes_and_hierarch_round_trip, through the public writer and reader. */
static void test_fits_text_round_trip_and_sweep(void)
{
    static const uint16_t px[6] = {0, 1000, 20000, 40000, 65535, 7};
    pc_cimage img;
    pc_cimage back;
    pc_buf out = PC_BUF_INIT;
    CHECK_EQ_INT(pc_cimage_from_copy(3, 2, PC_CL_GRAY, PC_CS_U16, px, sizeof px, &img, NULL), PC_OK);
    CHECK_EQ_INT(pc_cmeta_add_text(&img.meta, "OBSERVER", 8, "O'Brien", 7, NULL), PC_OK);
    CHECK_EQ_INT(pc_cmeta_add_text(&img.meta, "Software", 8, "photocraft 1.0", 14, NULL), PC_OK);
    CHECK_EQ_INT(pc_cmeta_add_text(&img.meta, "HISTORY", 7, "stacked", 7, NULL), PC_OK);
    CHECK_EQ_INT(pc_cencode(&img, PC_CF_FITS, NULL, &out, NULL), PC_OK);
    CHECK_EQ_INT(pc_cdecode_as(PC_CF_FITS, out.data, out.len, &back, NULL), PC_OK);
    CHECK(back.width == 3 && back.height == 2 && back.sample == PC_CS_U16);
    CHECK(back.len == img.len && memcmp(back.data, img.data, img.len) == 0);
    CHECK(has_text(&back, "OBSERVER", "O'Brien"));
    CHECK(has_text(&back, "Software", "photocraft 1.0"));
    CHECK(has_text(&back, "HISTORY", "stacked"));
    pc_cimage_fini(&back);
    /* Truncation and mutation of a valid file never crash (only the header block is swept). */
    {
        pc_buf head = PC_BUF_INIT;
        (void)pc_buf_append(&head, out.data, out.len < 2880 + 64 ? out.len : 2880 + 64, NULL);
        sweep_decoder(PC_CF_FITS, &head);
        pc_buf_fini(&head);
    }
    pc_cimage_fini(&img);
    pc_buf_fini(&out);
}

int main(int argc, char **argv)
{
    RUN(test_a_still_gif_gives_one_frame_equal_to_decode);
    RUN(test_every_frame_comes_back_in_order_with_its_delay);
    RUN(test_disposal_methods);
    RUN(test_a_damaged_later_frame_keeps_the_frames_before_it);
    RUN(test_the_first_frame_must_decode);
    RUN(test_every_prefix_and_mutation_returns);
    RUN(test_limits_apply);
    RUN(test_pcx_is_readable_but_not_writable);
    RUN(test_pcx_decodes);
    RUN(test_pcx_rejects_truncated_and_overshooting_rle);
    RUN(test_pcx_refuses_headers_over_budget);
    RUN(test_pcx_hostile_sweep);
    RUN(test_fits_camera_u16_with_bzero_reads_exactly_and_bottom_up);
    RUN(test_fits_small_cases);
    RUN(test_fits_hostile_headers_error_without_crashing);
    RUN(test_fits_text_round_trip_and_sweep);
    return pc_t_finish();
}
