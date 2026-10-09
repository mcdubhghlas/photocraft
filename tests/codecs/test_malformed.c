/* Malformed input must produce errors, never crashes or huge allocations (port of
 * crates/codecs/tests/malformed.rs). The proptest cases become seeded loops over the same input
 * spaces. */
#include "base/file.h"
#include "common.h"

typedef struct sample {
    pc_cformat f;
    pc_buf b;
} sample;

typedef struct samples {
    sample v[64];
    size_t n;
} samples;

static pc_cdecode_opts tight(void)
{
    pc_cdecode_opts o = pc_cdecode_opts_default();
    o.limits.max_width = 4096;
    o.limits.max_height = 4096;
    o.limits.max_pixels = 1u << 22;
    o.limits.max_alloc = 64u << 20;
    return o;
}

static void push(samples *s, pc_cformat f, pc_buf b)
{
    if (s->n < sizeof s->v / sizeof s->v[0]) {
        s->v[s->n].f = f;
        s->v[s->n].b = b;
        s->n++;
    } else {
        PC_T_FAIL("too many samples");
        pc_buf_fini(&b);
    }
}

static void push_bytes(samples *s, pc_cformat f, const void *d, size_t n)
{
    pc_buf b = PC_BUF_INIT;
    (void)pc_buf_append(&b, d, n, NULL);
    push(s, f, b);
}

static samples make_samples(void)
{
    static const pc_clayout LS[3] = {PC_CL_RGBA, PC_CL_GRAY, PC_CL_RGB};
    static const pc_csample SS[3] = {PC_CS_U8, PC_CS_U16, PC_CS_F32};
    static const char *const HEIC[2] = {"corpus/heif/heic-rs/rgb-strips-96.heic", "corpus/heif/heic-rs/checker-1024.heic"};
    static const char PNM[] = "P3\n2 2\n255\n0 1 2 3 4 5 6 7 8 9 10 11\n";
    samples s;
    int f;
    size_t k;
    pc_cimage img;
    pc_cencode_opts o = pc_cencode_opts_default();
    s.n = 0;
    for (f = 0; f < PC_CF_COUNT; f++) {
        if (!rw((pc_cformat)f)) {
            continue;
        }
        for (k = 0; k < 3; k++) {
            pc_buf icc = sample_icc(300);
            pc_buf exif = sample_exif();
            img = synth(19, 13, LS[k], SS[k], 11, 0.3f);
            set_blob(&img.icc, icc.data, icc.len);
            set_blob(&img.meta.exif, exif.data, exif.len);
            set_blob(&img.meta.xmp, SAMPLE_XMP, strlen(SAMPLE_XMP));
            push(&s, (pc_cformat)f, enc(&img, (pc_cformat)f, NULL));
            pc_buf_fini(&icc);
            pc_buf_fini(&exif);
            pc_cimage_fini(&img);
        }
    }
    /* Read-only formats: Apple-encoded HEIC from corpus/heif (`make corpus`), a single picture
     * and a grid of tiles. Rust gates these on the corpus feature; we take them when present. */
    if (pc_cformat_caps(PC_CF_HEIF).read) {
        for (k = 0; k < 2; k++) {
            pc_buf b = PC_BUF_INIT;
            if (pc_file_read(HEIC[k], 64u << 20, &b, NULL) == PC_OK) {
                push(&s, PC_CF_HEIF, b);
            }
        }
    }
    /* Extra variants: interlaced PNG, ASCII PNM, CMYK JPEG. */
    img = synth(19, 13, PC_CL_RGB, PC_CS_U8, 1, 0.3f);
    o.png_interlaced = true;
    push(&s, PC_CF_PNG, enc(&img, PC_CF_PNG, &o));
    pc_cimage_fini(&img);
    push_bytes(&s, PC_CF_PNM, PNM, sizeof PNM - 1);
    img = synth(19, 13, PC_CL_CMYK, PC_CS_U8, 1, 0.3f);
    push(&s, PC_CF_JPEG, enc(&img, PC_CF_JPEG, NULL));
    pc_cimage_fini(&img);
    return s;
}

static void samples_fini(samples *s)
{
    size_t i;
    for (i = 0; i < s->n; i++) {
        pc_buf_fini(&s->v[i].b);
    }
    s->n = 0;
}

static void no_panic(pc_cformat f, const uint8_t *b, size_t n)
{
    pc_cdecode_opts o = tight();
    pc_cimage img;
    if (pc_cdecode_with(b, n, &o, &img, NULL) == PC_OK) {
        pc_cimage_fini(&img);
    }
    if (pc_cdecode_as_with(f, b, n, &o, &img, NULL) == PC_OK) {
        pc_cimage_fini(&img);
    }
}

/* A heap copy of n bytes, so ASan sees reads past the cut. */
static uint8_t *dup_bytes(const uint8_t *b, size_t n)
{
    uint8_t *c = malloc(n > 0 ? n : 1);
    if (c != NULL && n > 0) {
        memcpy(c, b, n);
    }
    return c;
}

static void test_truncated_at_many_offsets(void)
{
    samples s = make_samples();
    size_t i;
    for (i = 0; i < s.n; i++) {
        size_t n = s.v[i].b.len;
        size_t step = n / 97 > 0 ? n / 97 : 1;
        size_t cut;
        for (cut = 0; cut < n && cut < 80; cut++) {
            uint8_t *c = dup_bytes(s.v[i].b.data, cut);
            no_panic(s.v[i].f, c, cut);
            free(c);
        }
        for (cut = 0; cut < n; cut += step) {
            uint8_t *c = dup_bytes(s.v[i].b.data, cut);
            no_panic(s.v[i].f, c, cut);
            free(c);
        }
        for (cut = n > 40 ? n - 40 : 0; cut < n; cut++) {
            uint8_t *c = dup_bytes(s.v[i].b.data, cut);
            no_panic(s.v[i].f, c, cut);
            free(c);
        }
    }
    CHECK(s.n > 0);
    samples_fini(&s);
}

static void test_truncated_files_mostly_error(void)
{
    /* Chopping off the second half must not silently succeed for formats with length-checked
     * rasters. */
    samples s = make_samples();
    pc_cdecode_opts o = tight();
    size_t i;
    for (i = 0; i < s.n; i++) {
        pc_cformat f = s.v[i].f;
        size_t half = s.v[i].b.len / 2;
        pc_cimage img;
        pc_error e;
        if (f == PC_CF_PNG || f == PC_CF_PNM || f == PC_CF_QOI || f == PC_CF_EXR || f == PC_CF_TIFF ||
            f == PC_CF_BMP) {
            bool err = pc_cdecode_as_with(f, s.v[i].b.data, half, &o, &img, NULL) != PC_OK;
            if (!err) {
                PC_T_FAIL("%s decoded half a file", pc_cformat_debug(f));
                pc_cimage_fini(&img);
            }
            CHECK(err);
        }
        /* JPEG decodes leniently (the missing part grey) but never silently (#518). */
        if (f == PC_CF_JPEG) {
            if (pc_cdecode_as_with(f, s.v[i].b.data, half, &o, &img, &e) == PC_OK) {
                CHECK_EQ_UINT(img.nwarnings, 1);
                CHECK(img.nwarnings == 1 && img.warnings[0].kind == PC_CW_TRUNCATED &&
                      img.warnings[0].format == (int)PC_CF_JPEG);
                pc_cimage_fini(&img);
            } else {
                CHECK_CONTAINS(e.msg, "before any image data");
            }
        }
    }
    samples_fini(&s);
}

static void test_bit_flips_never_panic(void)
{
    samples s = make_samples();
    rng r = {42};
    size_t i;
    for (i = 0; i < s.n; i++) {
        int t;
        for (t = 0; t < 150; t++) {
            uint8_t *b = dup_bytes(s.v[i].b.data, s.v[i].b.len);
            size_t n = s.v[i].b.len;
            size_t flips = 1 + (size_t)(rng_next(&r) % 4);
            size_t k;
            for (k = 0; k < flips && n > 0; k++) {
                size_t at = (size_t)(rng_next(&r) % n);
                b[at] ^= (uint8_t)(1u << (rng_next(&r) % 8));
            }
            no_panic(s.v[i].f, b, n);
            free(b);
        }
    }
    CHECK(s.n > 0);
    samples_fini(&s);
}

static void test_byte_overwrites_in_header_never_panic(void)
{
    static const uint8_t VALS[3] = {0x00, 0x7F, 0xFF};
    samples s = make_samples();
    size_t i;
    for (i = 0; i < s.n; i++) {
        size_t n = s.v[i].b.len;
        size_t at;
        for (at = 0; at < n && at < 64; at++) {
            size_t k;
            for (k = 0; k < 3; k++) {
                uint8_t *b = dup_bytes(s.v[i].b.data, n);
                b[at] = VALS[k];
                no_panic(s.v[i].f, b, n);
                free(b);
            }
        }
    }
    CHECK(s.n > 0);
    samples_fini(&s);
}

static void test_empty_input_errors_for_every_format(void)
{
    int f;
    for (f = 0; f < PC_CF_COUNT; f++) {
        pc_cimage img;
        bool err = pc_cdecode_as((pc_cformat)f, NULL, 0, &img, NULL) != PC_OK;
        if (!err) {
            PC_T_FAIL("%s", pc_cformat_debug((pc_cformat)f));
            pc_cimage_fini(&img);
        }
        CHECK(err);
    }
}

typedef struct bytes {
    const char *d;
    size_t n;
} bytes;
#define B(s) {s, sizeof(s) - 1}

static void expect_decode_err(const char *d, size_t n)
{
    pc_cimage img;
    bool err = pc_cdecode((const uint8_t *)d, n, &img, NULL) != PC_OK;
    if (!err) {
        PC_T_FAIL("decoded %.*s", (int)n, d);
        pc_cimage_fini(&img);
    }
    CHECK(err);
}

static void test_pnm_specific_malformations(void)
{
    static const bytes CASES[] = {
        B("P6\n"),
        B("P6\n2 2\n"),
        B("P6\n2 2\n255"),
        B("P6\n2 2\n0\n"),
        B("P6\n2 2\n70000\n"),
        B("P6\n-2 2\n255\n"),
        B("P6\n2 2\n255\n\x01\x02"),
        B("P5\n2 2\n65535\n\x00\x00\x00"),
        B("P2\n2 2\n10\n1 2 3 11\n"),
        B("P1\n2 2\n0 1 2 0\n"),
        B("P4\n9 2\n\x00"),
        B("P7\nWIDTH 2\nHEIGHT 2\nDEPTH 9\nMAXVAL 255\nENDHDR\n"),
        B("P7\nWIDTH 2\nHEIGHT 2\nMAXVAL 255\nENDHDR\n"),
        B("P7\nWIDTH 2\nBOGUS 3\nENDHDR\n"),
        B("PF\n2 2\n0.0\n"),
        B("PF\n2 2\nnan\n"),
        B("PF\n2 2\n-1.0\n\0\0\0\0"),
        B("P6 4294967295 4294967295 255\n"),
    };
    size_t i;
    for (i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        expect_decode_err(CASES[i].d, CASES[i].n);
    }
}

static pc_cimage dec_s(const char *d, size_t n) { return dec((const uint8_t *)d, n); }
#define DEC(s) dec_s(s, sizeof(s) - 1)

static void check_data(const pc_cimage *img, const uint8_t *want, size_t n)
{
    CHECK_EQ_UINT(img->len, n);
    if (img->len == n) {
        CHECK_EQ_MEM(img->data, want, n);
    }
}

static void test_pnm_valid_variants(void)
{
    static const uint8_t ALT[6] = {0, 255, 0, 255, 0, 255};
    pc_cimage img;
    uint8_t pfm[12 + 24];
    size_t n = 0;
    size_t i;
    static const float V[6] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    static const float WANT[6] = {4.0f, 5.0f, 6.0f, 1.0f, 2.0f, 3.0f};

    img = DEC("P1\n# comment\n3 2\n1 0 1\n0 1 0\n");
    check_data(&img, ALT, 6);
    pc_cimage_fini(&img);
    img = DEC("P1\n3 2\n101010");
    check_data(&img, ALT, 6);
    pc_cimage_fini(&img);
    img = DEC("P4\n3 2\n\xA0\x40");
    check_data(&img, ALT, 6);
    pc_cimage_fini(&img);
    img = DEC("P2\n2 1\n15\n0 15\n");
    check_data(&img, (const uint8_t[]){0, 255}, 2);
    pc_cimage_fini(&img);
    img = DEC("P2\n2 1\n1023\n0 1023\n");
    CHECK(img.sample == PC_CS_U16);
    CHECK_EQ_UINT(img.len, 4);
    if (img.len == 4) {
        CHECK_EQ_UINT(ne16(img.data, 0), 0);
        CHECK_EQ_UINT(ne16(img.data, 1), 65535);
    }
    pc_cimage_fini(&img);
    img = DEC("P3\n1 1\n255\n1 2 3\n");
    CHECK(img.layout == PC_CL_RGB);
    check_data(&img, (const uint8_t[]){1, 2, 3}, 3);
    pc_cimage_fini(&img);
    img = DEC("P7\nWIDTH 1\nHEIGHT 1\nDEPTH 2\nMAXVAL 255\nTUPLTYPE GRAYSCALE_ALPHA\nENDHDR\n\x10\x20");
    CHECK(img.layout == PC_CL_GRAYA);
    check_data(&img, (const uint8_t[]){0x10, 0x20}, 2);
    pc_cimage_fini(&img);

    memcpy(pfm, "PF\n1 2\n1.0\n", 11); /* big-endian, bottom-up */
    n = 11;
    for (i = 0; i < 6; i++) {
        uint32_t u;
        memcpy(&u, &V[i], 4);
        pfm[n++] = (uint8_t)(u >> 24);
        pfm[n++] = (uint8_t)(u >> 16);
        pfm[n++] = (uint8_t)(u >> 8);
        pfm[n++] = (uint8_t)u;
    }
    img = dec(pfm, n);
    CHECK(img.sample == PC_CS_F32);
    CHECK_EQ_UINT(img.len, 24);
    if (img.len == 24 && img.sample == PC_CS_F32) {
        for (i = 0; i < 6; i++) {
            CHECK(nef32(img.data, i) == WANT[i]);
        }
    }
    pc_cimage_fini(&img);
}

static void test_jpeg_marker_garbage(void)
{
    static const bytes CASES[] = {
        B("\xFF\xD8\xFF"),
        B("\xFF\xD8\xFF\xE0\x00"),
        B("\xFF\xD8\xFF\xE0\x00\x01"),
        B("\xFF\xD8\xFF\xE2\x00\x10ICC_PROFILE\x00\x01\x01"),
        B("\xFF\xD8\xFF\xC0\x00\x0B\x08\xFF\xFF\xFF\xFF\x03\x00\x00\x00"),
        B("\xFF\xD8\xFF\xD9"),
    };
    size_t i;
    for (i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        expect_decode_err(CASES[i].d, CASES[i].n);
    }
}

/* ---- proptest ports: 400 seeded cases each ---- */

enum { CASES = 400 };

static void test_random_bytes_never_panic(void)
{
    rng r = {0x5EED1};
    pc_cdecode_opts o = tight();
    int t;
    for (t = 0; t < CASES; t++) {
        size_t n = (size_t)(rng_next(&r) % 2048);
        uint8_t *d = rng_bytes(&r, n);
        pc_cimage img;
        if (pc_cdecode_with(d, n, &o, &img, NULL) == PC_OK) {
            pc_cimage_fini(&img);
        }
        free(d);
    }
    CHECK(true);
}

static void test_random_bytes_with_magic_never_panic(void)
{
    static const bytes MAGICS[14] = {
        B("\x89PNG\r\n\x1a\n"),
        B("\xFF\xD8\xFF"),
        B("II*\0"),
        B("RIFF\0\0\0\0WEBP"),
        B("GIF89a"),
        B("BM"),
        B("\0\0\x01\0\x01\0"),
        B("P6\n"),
        B("qoif"),
        B("\x76\x2F\x31\x01"),
        B("#?RADIANCE\n"),
        B("P7\n"),
        B("PF\n"),
        B("\0\0\0\x18" "ftypheic\0\0\0\0mif1heic"),
    };
    rng r = {0x5EED2};
    pc_cdecode_opts o = tight();
    int t;
    for (t = 0; t < CASES; t++) {
        const bytes *m = &MAGICS[rng_next(&r) % 14];
        size_t n = (size_t)(rng_next(&r) % 1024);
        uint8_t *d = rng_bytes(&r, n);
        uint8_t *b = malloc(m->n + n);
        pc_cimage img;
        memcpy(b, m->d, m->n);
        if (n > 0) {
            memcpy(b + m->n, d, n);
        }
        if (pc_cdecode_with(b, m->n + n, &o, &img, NULL) == PC_OK) {
            pc_cimage_fini(&img);
        }
        free(b);
        free(d);
    }
    CHECK(true);
}

static void test_random_bytes_each_format_never_panic(void)
{
    rng r = {0x5EED3};
    pc_cdecode_opts o = tight();
    int t;
    for (t = 0; t < CASES; t++) {
        pc_cformat f = (pc_cformat)(rng_next(&r) % PC_CF_COUNT);
        size_t n = (size_t)(rng_next(&r) % 512);
        uint8_t *d = rng_bytes(&r, n);
        pc_cimage img;
        if (pc_cdecode_as_with(f, d, n, &o, &img, NULL) == PC_OK) {
            pc_cimage_fini(&img);
        }
        free(d);
    }
    CHECK(true);
}

static void test_mutated_valid_files_never_panic(void)
{
    samples s = make_samples();
    rng r = {0x5EED4};
    pc_cdecode_opts o = tight();
    int t;
    for (t = 0; t < CASES && s.n > 0; t++) {
        size_t which = (size_t)(rng_next(&r) % 40);
        size_t pos = (size_t)rng_next(&r);
        uint8_t val = (uint8_t)rng_next(&r);
        size_t cut = (size_t)rng_next(&r);
        const sample *sm = &s.v[which % s.n];
        size_t n = sm->b.len;
        size_t p = pos % n;
        size_t c = cut % (n + 1);
        size_t len = c > p + 1 ? c : p + 1;
        uint8_t *b;
        pc_cimage img;
        len = len < n ? len : n;
        b = dup_bytes(sm->b.data, len);
        b[p] = val;
        if (pc_cdecode_as_with(sm->f, b, len, &o, &img, NULL) == PC_OK) {
            pc_cimage_fini(&img);
        }
        free(b);
    }
    CHECK(s.n > 0);
    samples_fini(&s);
}

/* A little-endian TIFF with one 1x1 8-bit grey strip and the given extra IFD entries
 * (tag, type, count, value), sorted into place. */
typedef struct tiff_entry {
    uint16_t tag;
    uint16_t ty;
    uint32_t count;
    uint32_t value;
} tiff_entry;

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

static pc_buf tiny_tiff(const tiff_entry *extra, size_t nextra)
{
    enum { SHORT = 3, LONG = 4 };
    tiff_entry e[16] = {
        {256, SHORT, 1, 1}, /* ImageWidth */
        {257, SHORT, 1, 1}, /* ImageLength */
        {258, SHORT, 1, 8}, /* BitsPerSample */
        {259, SHORT, 1, 1}, /* Compression: none */
        {262, SHORT, 1, 1}, /* Photometric: BlackIsZero */
        {273, LONG, 1, 0},  /* StripOffsets (patched below) */
        {277, SHORT, 1, 1}, /* SamplesPerPixel */
        {278, SHORT, 1, 1}, /* RowsPerStrip */
        {279, LONG, 1, 1},  /* StripByteCounts */
    };
    size_t n = 9;
    size_t i;
    size_t j;
    uint32_t strip;
    pc_buf b = PC_BUF_INIT;
    for (i = 0; i < nextra && n < 16; i++) {
        e[n++] = extra[i];
    }
    for (i = 1; i < n; i++) { /* stable insertion sort by tag */
        tiff_entry x = e[i];
        for (j = i; j > 0 && e[j - 1].tag > x.tag; j--) {
            e[j] = e[j - 1];
        }
        e[j] = x;
    }
    strip = (uint32_t)(8 + 2 + 12 * n + 4);
    (void)pc_buf_append(&b, "II*\0", 4, NULL);
    put32(&b, 8);
    put16(&b, (uint16_t)n);
    for (i = 0; i < n; i++) {
        put16(&b, e[i].tag);
        put16(&b, e[i].ty);
        put32(&b, e[i].count);
        put32(&b, e[i].tag == 273 ? strip : e[i].value);
    }
    put32(&b, 0);
    (void)pc_buf_append_byte(&b, 0x80, NULL);
    return b;
}

static void test_tiff_fixture_decodes(void)
{
    pc_buf b = tiny_tiff(NULL, 0);
    pc_cdecode_opts o = tight();
    pc_cimage img;
    pc_error e;
    if (pc_cdecode_as_with(PC_CF_TIFF, b.data, b.len, &o, &img, &e) == PC_OK) {
        CHECK_EQ_UINT(img.width, 1);
        CHECK_EQ_UINT(img.height, 1);
        pc_cimage_fini(&img);
    } else {
        PC_T_FAIL("decode: %s", e.msg);
    }
    pc_buf_fini(&b);
}

/* Regression: an empty SampleFormat tag made the tiff 0.10 decoder index an empty list. Found by
 * mutation fuzzing. */
static void test_tiff_empty_sample_format_is_an_error(void)
{
    static const tiff_entry SF = {339, 3, 0, 0};
    pc_buf b = tiny_tiff(&SF, 1);
    pc_cdecode_opts o = tight();
    pc_cimage img;
    bool e1 = pc_cdecode_as_with(PC_CF_TIFF, b.data, b.len, &o, &img, NULL) != PC_OK;
    bool e2;
    if (!e1) {
        pc_cimage_fini(&img);
    }
    e2 = pc_cdecode_with(b.data, b.len, &o, &img, NULL) != PC_OK;
    if (!e2) {
        pc_cimage_fini(&img);
    }
    CHECK(e1);
    CHECK(e2);
    pc_buf_fini(&b);
}

int main(int argc, char **argv)
{
    RUN(test_truncated_at_many_offsets);
    RUN(test_truncated_files_mostly_error);
    RUN(test_bit_flips_never_panic);
    RUN(test_byte_overwrites_in_header_never_panic);
    RUN(test_empty_input_errors_for_every_format);
    RUN(test_pnm_specific_malformations);
    RUN(test_pnm_valid_variants);
    RUN(test_jpeg_marker_garbage);
    RUN(test_random_bytes_never_panic);
    RUN(test_random_bytes_with_magic_never_panic);
    RUN(test_random_bytes_each_format_never_panic);
    RUN(test_mutated_valid_files_never_panic);
    RUN(test_tiff_fixture_decodes);
    RUN(test_tiff_empty_sample_format_is_an_error);
    return pc_t_finish();
}
