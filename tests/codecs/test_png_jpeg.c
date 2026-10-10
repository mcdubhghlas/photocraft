/* Format-specific behaviour: Adam7 PNG, JPEG colour models, oracle cross-checks and the PngSuite
 * corpus (port of crates/codecs/tests/png_jpeg.rs), plus the unit tests of
 * crates/codecs/src/codecs/png.rs. The Rust oracles (the `image`, `png` and `jpeg_encoder`
 * crates) are replaced by libpng and libjpeg-turbo used directly, and by our own decoder for TIFF
 * (no libtiff here). */
#include <dirent.h>
#include <setjmp.h>
#include <stdio.h>
#include <sys/stat.h>

#include <jpeglib.h>
#include <png.h>
#include <zlib.h>

#include "common.h"

static bool host_little(void)
{
    const uint16_t one = 1;
    uint8_t b;
    memcpy(&b, &one, 1);
    return b == 1;
}

/* ---- libpng oracle: any PNG to interleaved native-endian RGBA16 ---- */

typedef struct png_src {
    const uint8_t *b;
    size_t n;
    size_t at;
} png_src;

static void png_src_read(png_structp p, png_bytep out, size_t len)
{
    png_src *s = png_get_io_ptr(p);
    if (len > s->n - s->at) {
        png_error(p, "unexpected end of data");
    }
    memcpy(out, s->b + s->at, len);
    s->at += len;
}

static void png_quiet(png_structp p, png_const_charp msg)
{
    (void)p;
    (void)msg;
}

static void png_quiet_err(png_structp p, png_const_charp msg)
{
    (void)msg;
    png_longjmp(p, 1);
}

/* Expands palette/low-bit/gray/tRNS like the image crate's into_rgba16 (8-bit samples * 257). */
static bool png_oracle_rgba16(const uint8_t *b, size_t n, uint16_t **out, uint32_t *w, uint32_t *h)
{
    png_src src = {b, n, 0};
    png_structp p = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, png_quiet_err, png_quiet);
    png_infop info = NULL;
    uint16_t *volatile px = NULL;
    png_bytep *volatile rows = NULL;
    png_uint_32 pw;
    png_uint_32 ph;
    png_uint_32 y;
    if (p == NULL) {
        return false;
    }
    info = png_create_info_struct(p);
    if (info == NULL || setjmp(png_jmpbuf(p)) != 0) {
        free(px);
        free(rows);
        png_destroy_read_struct(&p, &info, NULL);
        return false;
    }
    png_set_read_fn(p, &src, png_src_read);
    png_read_info(p, info);
    png_set_expand(p);
    png_set_expand_16(p);
    png_set_gray_to_rgb(p);
    png_set_add_alpha(p, 0xFFFF, PNG_FILLER_AFTER);
    if (host_little()) {
        png_set_swap(p);
    }
    (void)png_set_interlace_handling(p);
    png_read_update_info(p, info);
    pw = png_get_image_width(p, info);
    ph = png_get_image_height(p, info);
    if (png_get_rowbytes(p, info) != (size_t)pw * 8 || (uint64_t)pw * ph > ((uint64_t)1 << 26)) {
        png_error(p, "unexpected layout");
    }
    px = malloc((size_t)pw * ph * 8 + 8);
    rows = malloc(((size_t)ph + 1) * sizeof *rows);
    if (px == NULL || rows == NULL) {
        png_error(p, "out of memory");
    }
    for (y = 0; y < ph; y++) {
        rows[y] = (png_bytep)(px + (size_t)y * pw * 4);
    }
    png_read_image(p, rows);
    free(rows);
    png_destroy_read_struct(&p, &info, NULL);
    *out = px;
    *w = pw;
    *h = ph;
    return true;
}

static bool rgba16_eq(const uint16_t *a, const uint16_t *b, size_t n) { return memcmp(a, b, n * 2) == 0; }

/* ---- a tiny PNG writer (replaces the png crate for files our encoder never writes) ---- */

static void put_be32(pc_buf *o, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    (void)pc_buf_append(o, b, 4, NULL);
}

static void put_chunk(pc_buf *o, const char *type, const uint8_t *d, size_t n)
{
    uLong crc = crc32(0L, (const Bytef *)type, 4);
    put_be32(o, (uint32_t)n);
    (void)pc_buf_append(o, type, 4, NULL);
    if (n > 0) {
        (void)pc_buf_append(o, d, n, NULL);
        crc = crc32(crc, d, (uInt)n);
    }
    put_be32(o, (uint32_t)crc);
}

/* One-row-or-more PNG from unfiltered scanlines (each row gets filter byte 0). */
static pc_buf mini_png(uint32_t w, uint32_t h, uint8_t depth, uint8_t color, const uint8_t *rows, size_t row_len,
                       const uint8_t *plte, size_t nplte, const uint8_t *trns, size_t ntrns)
{
    pc_buf o = PC_BUF_INIT;
    uint8_t ihdr[13];
    size_t raw_len = (row_len + 1) * h;
    uint8_t *raw = malloc(raw_len);
    uLongf zlen = compressBound((uLong)raw_len);
    uint8_t *z = malloc(zlen);
    uint32_t y;
    for (y = 0; y < h; y++) {
        raw[y * (row_len + 1)] = 0;
        memcpy(raw + y * (row_len + 1) + 1, rows + y * row_len, row_len);
    }
    (void)compress(z, &zlen, raw, (uLong)raw_len);
    (void)pc_buf_append(&o, "\x89PNG\r\n\x1a\n", 8, NULL);
    ihdr[0] = (uint8_t)(w >> 24);
    ihdr[1] = (uint8_t)(w >> 16);
    ihdr[2] = (uint8_t)(w >> 8);
    ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)(h >> 24);
    ihdr[5] = (uint8_t)(h >> 16);
    ihdr[6] = (uint8_t)(h >> 8);
    ihdr[7] = (uint8_t)h;
    ihdr[8] = depth;
    ihdr[9] = color;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    put_chunk(&o, "IHDR", ihdr, 13);
    if (nplte > 0) {
        put_chunk(&o, "PLTE", plte, nplte);
    }
    if (ntrns > 0) {
        put_chunk(&o, "tRNS", trns, ntrns);
    }
    put_chunk(&o, "IDAT", z, zlen);
    put_chunk(&o, "IEND", NULL, 0);
    free(raw);
    free(z);
    return o;
}

/* ---------------------------------------------------------------------------
 * PNG Adam7
 * --------------------------------------------------------------------------- */

static pc_buf interlaced(const pc_cimage *img)
{
    pc_cencode_opts o = pc_cencode_opts_default();
    o.png_interlaced = true;
    return enc(img, PC_CF_PNG, &o);
}

static void check_adam7(uint32_t w, uint32_t h, pc_clayout l, pc_csample s)
{
    pc_cimage img = synth(w, h, l, s, (uint64_t)w * 31 + h, 0.5f);
    pc_buf a = interlaced(&img);
    pc_buf b = enc(&img, PC_CF_PNG, NULL);
    pc_cimage da;
    pc_cimage db;
    pc_cimage ours;
    uint16_t *o = NULL;
    uint32_t ow = 0;
    uint32_t oh = 0;
    char name[64];
    (void)snprintf(name, sizeof name, "%ux%u %s %s", w, h, pc_clayout_name(l), pc_csample_name(s));
    pc_t_name = name;
    CHECK(a.len > 28 && a.data[28] == 1); /* IHDR interlace flag */
    CHECK(b.len > 28 && b.data[28] == 0);
    da = dec(a.data, a.len);
    db = dec(b.data, b.len);
    CHECK(pc_cimage_eq(&da, &db)); /* interlaced != progressive */
    CHECK(da.len == img.len && memcmp(da.data, img.data, img.len) == 0);
    /* Oracle: libpng agrees. */
    CHECK(png_oracle_rgba16(a.data, a.len, &o, &ow, &oh));
    if (pc_cimage_convert(&da, PC_CL_RGBA, PC_CS_U16, &ours, NULL) == PC_OK) {
        CHECK(o != NULL && ow == w && oh == h && ours.len == (size_t)w * h * 8 && memcmp(o, ours.data, ours.len) == 0);
        pc_cimage_fini(&ours);
    } else {
        PC_T_FAIL("convert");
    }
    free(o);
    pc_cimage_fini(&da);
    pc_cimage_fini(&db);
    pc_buf_fini(&a);
    pc_buf_fini(&b);
    pc_cimage_fini(&img);
}

static void test_adam7_small_sizes_all_layouts(void)
{
    static const uint32_t S[9][2] = {{1, 1}, {2, 2}, {3, 3}, {5, 1}, {1, 9}, {7, 5}, {8, 8}, {9, 17}, {33, 31}};
    static const pc_clayout L[4] = {PC_CL_GRAY, PC_CL_GRAYA, PC_CL_RGB, PC_CL_RGBA};
    int i;
    int k;
    for (i = 0; i < 9; i++) {
        for (k = 0; k < 4; k++) {
            check_adam7(S[i][0], S[i][1], L[k], PC_CS_U8);
        }
    }
}

static void test_adam7_16bit(void)
{
    static const uint32_t S[4][2] = {{1, 1}, {4, 3}, {13, 11}, {64, 40}};
    static const pc_clayout L[2] = {PC_CL_GRAY, PC_CL_RGBA};
    int i;
    int k;
    for (i = 0; i < 4; i++) {
        for (k = 0; k < 2; k++) {
            check_adam7(S[i][0], S[i][1], L[k], PC_CS_U16);
        }
    }
}

static void test_adam7_with_metadata(void)
{
    pc_cimage img = test_image(PC_CL_RGB, PC_CS_U8);
    pc_buf icc = sample_icc(400);
    pc_buf b;
    pc_cimage back;
    set_blob(&img.icc, icc.data, icc.len);
    pc_buf_fini(&icc);
    (void)pc_cmeta_add_text(&img.meta, "Comment", 7, "interlaced", 10, NULL);
    b = interlaced(&img);
    back = dec(b.data, b.len);
    CHECK(pc_meta_blob_eq(&back.icc, &img.icc));
    CHECK(back.meta.ntext == 1 && back.meta.text[0].key_len == 7 && memcmp(back.meta.text[0].key, "Comment", 7) == 0 &&
          back.meta.text[0].value_len == 10 && memcmp(back.meta.text[0].value, "interlaced", 10) == 0);
    pc_cimage_fini(&back);
    pc_buf_fini(&b);
    pc_cimage_fini(&img);
}

static void test_adam7_compressions(void)
{
    static const pc_png_compression C[3] = {PC_PNG_NONE, PC_PNG_FAST, PC_PNG_BEST};
    pc_cimage img = test_image(PC_CL_RGBA, PC_CS_U8);
    int i;
    for (i = 0; i < 3; i++) {
        pc_cencode_opts o = pc_cencode_opts_default();
        pc_buf b;
        pc_cimage back;
        o.png_interlaced = true;
        o.png_compression = C[i];
        b = enc(&img, PC_CF_PNG, &o);
        back = dec(b.data, b.len);
        CHECK(back.len == img.len && memcmp(back.data, img.data, img.len) == 0);
        pc_cimage_fini(&back);
        pc_buf_fini(&b);
    }
    pc_cimage_fini(&img);
}

static void test_png_palette_and_low_bit_depth_expand(void)
{
    /* A 2-bit gray PNG and a palette PNG with tRNS (written by mini_png instead of the png crate). */
    static const uint8_t ROW[1] = {0x1B}; /* 0b00_01_10_11 */
    static const uint8_t GRAY[4] = {0, 85, 170, 255};
    static const uint8_t PLTE[6] = {255, 0, 0, 0, 0, 255};
    static const uint8_t TRNS[1] = {128};
    static const uint8_t IDX[2] = {0, 1};
    static const uint8_t RGBA[8] = {255, 0, 0, 128, 0, 0, 255, 255};
    pc_buf b = mini_png(4, 1, 2, 0, ROW, 1, NULL, 0, NULL, 0);
    pc_cimage img = dec(b.data, b.len);
    CHECK(img.layout == PC_CL_GRAY && img.len == 4 && memcmp(img.data, GRAY, 4) == 0);
    pc_cimage_fini(&img);
    pc_buf_fini(&b);

    b = mini_png(2, 1, 8, 3, IDX, 2, PLTE, 6, TRNS, 1);
    img = dec(b.data, b.len);
    CHECK(img.layout == PC_CL_RGBA && img.len == 8 && memcmp(img.data, RGBA, 8) == 0);
    pc_cimage_fini(&img);
    pc_buf_fini(&b);
}

/* ---------------------------------------------------------------------------
 * JPEG colour models
 * --------------------------------------------------------------------------- */

typedef struct jerr {
    struct jpeg_error_mgr mgr;
    jmp_buf jb;
} jerr;

static void jerr_exit(j_common_ptr c)
{
    jerr *e = (jerr *)(void *)c->err;
    longjmp(e->jb, 1);
}

static void jerr_silent(j_common_ptr c, int level)
{
    (void)c;
    (void)level;
}

/* libjpeg-turbo stands in for the jpeg_encoder crate. 4:4:4 sampling; `ycck` stores CMYK as YCCK
 * with libjpeg's Adobe convention (inverted samples, as jpeg_encoder's CmykAsYcck produces). */
static pc_buf jpeg_write(const uint8_t *px, uint32_t w, uint32_t h, J_COLOR_SPACE in, int comps, bool ycck,
                         bool progressive, int quality)
{
    struct jpeg_compress_struct c;
    jerr e;
    unsigned char *mem = NULL;
    unsigned long mlen = 0;
    pc_buf out = PC_BUF_INIT;
    uint32_t y;
    c.err = jpeg_std_error(&e.mgr);
    e.mgr.error_exit = jerr_exit;
    e.mgr.emit_message = jerr_silent;
    if (setjmp(e.jb) != 0) {
        jpeg_destroy_compress(&c);
        PC_T_FAIL("libjpeg encode failed");
        return out;
    }
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &mem, &mlen);
    c.image_width = w;
    c.image_height = h;
    c.input_components = comps;
    c.in_color_space = in;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, quality, TRUE);
    if (ycck) {
        int i;
        jpeg_set_colorspace(&c, JCS_YCCK);
        for (i = 0; i < c.num_components; i++) {
            c.comp_info[i].h_samp_factor = 1;
            c.comp_info[i].v_samp_factor = 1;
        }
    }
    if (progressive) {
        jpeg_simple_progression(&c);
    }
    jpeg_start_compress(&c, TRUE);
    for (y = 0; y < h; y++) {
        JSAMPROW row = (JSAMPROW)(uintptr_t)(px + (size_t)y * w * (size_t)comps);
        (void)jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    (void)pc_buf_append(&out, mem, mlen, NULL);
    free(mem);
    return out;
}

/* Decodes to 8-bit RGB with libjpeg-turbo's defaults (replaces the image crate's into_rgb8). */
static bool jpeg_oracle_rgb8(const uint8_t *b, size_t n, uint8_t **out, size_t *len)
{
    struct jpeg_decompress_struct d;
    jerr e;
    uint8_t *volatile px = NULL;
    d.err = jpeg_std_error(&e.mgr);
    e.mgr.error_exit = jerr_exit;
    e.mgr.emit_message = jerr_silent;
    if (setjmp(e.jb) != 0) {
        free(px);
        jpeg_destroy_decompress(&d);
        return false;
    }
    jpeg_create_decompress(&d);
    jpeg_mem_src(&d, b, (unsigned long)n);
    (void)jpeg_read_header(&d, TRUE);
    d.out_color_space = JCS_RGB;
    jpeg_start_decompress(&d);
    px = malloc((size_t)d.output_width * d.output_height * 3 + 1);
    if (px == NULL) {
        jpeg_destroy_decompress(&d);
        return false;
    }
    while (d.output_scanline < d.output_height) {
        JSAMPROW row = px + (size_t)d.output_scanline * d.output_width * 3;
        (void)jpeg_read_scanlines(&d, &row, 1);
    }
    *len = (size_t)d.output_width * d.output_height * 3;
    jpeg_finish_decompress(&d);
    jpeg_destroy_decompress(&d);
    *out = px;
    return true;
}

static void test_jpeg_grayscale_decodes_as_gray(void)
{
    pc_cimage img = synth(40, 30, PC_CL_GRAY, PC_CS_U8, 2, 0.02f);
    pc_buf b = enc(&img, PC_CF_JPEG, NULL);
    pc_cimage back = dec(b.data, b.len);
    CHECK(back.layout == PC_CL_GRAY);
    CHECK(psnr(&img, &back) > 35.0);
    pc_cimage_fini(&back);
    pc_buf_fini(&b);
    pc_cimage_fini(&img);
}

static void test_jpeg_cmyk_roundtrip_with_adobe_marker(void)
{
    pc_cimage img = synth(40, 30, PC_CL_CMYK, PC_CS_U8, 3, 0.02f);
    pc_cencode_opts o = pc_cencode_opts_default();
    pc_buf b;
    pc_cimage back;
    double p;
    o.jpeg_quality = 95;
    b = enc(&img, PC_CF_JPEG, &o);
    CHECK(find_bytes(b.data, b.len, "Adobe", 5)); /* APP14 Adobe marker */
    back = dec(b.data, b.len);
    CHECK(back.layout == PC_CL_CMYK);
    p = psnr(&img, &back);
    if (!(p > 35.0)) {
        PC_T_FAIL("psnr %g", p);
    }
    pc_cimage_fini(&back);
    pc_buf_fini(&b);
    pc_cimage_fini(&img);
}

static void test_jpeg_ycck_decodes_to_cmyk(void)
{
    pc_cimage img = synth(40, 32, PC_CL_CMYK, PC_CS_U8, 4, 0.02f);
    uint8_t *inv = malloc(img.len);
    pc_buf b;
    pc_cimage back;
    size_t i;
    double p;
    /* libjpeg takes Adobe-inverted CMYK, which is what a YCCK file decodes to. */
    for (i = 0; i < img.len; i++) {
        inv[i] = (uint8_t)(255 - img.data[i]);
    }
    b = jpeg_write(inv, 40, 32, JCS_CMYK, 4, true, false, 95);
    back = dec(b.data, b.len);
    CHECK(back.layout == PC_CL_CMYK);
    p = psnr(&img, &back);
    if (!(p > 32.0)) {
        PC_T_FAIL("psnr %g", p);
    }
    free(inv);
    pc_cimage_fini(&back);
    pc_buf_fini(&b);
    pc_cimage_fini(&img);
}

static void test_jpeg_cmyk_flat_colour_exact_ish(void)
{
    static const uint8_t PX[4] = {10, 100, 200, 50};
    uint8_t v[16 * 16 * 4];
    pc_cimage img;
    pc_buf b;
    pc_cimage back;
    size_t i;
    for (i = 0; i < sizeof v; i++) {
        v[i] = PX[i % 4];
    }
    CHECK_EQ_INT(pc_cimage_from_copy(16, 16, PC_CL_CMYK, PC_CS_U8, v, sizeof v, &img, NULL), PC_OK);
    b = enc(&img, PC_CF_JPEG, NULL);
    back = dec(b.data, b.len);
    CHECK(back.layout == PC_CL_CMYK && back.len == sizeof v);
    for (i = 0; i < back.len; i++) {
        if (abs((int)back.data[i] - (int)PX[i % 4]) > 2) {
            PC_T_FAIL("sample %zu: %u vs %u", i, back.data[i], PX[i % 4]);
            break;
        }
    }
    pc_cimage_fini(&back);
    pc_buf_fini(&b);
    pc_cimage_fini(&img);
}

static void test_jpeg_rgb_matches_oracle_decoder(void)
{
    pc_cimage img = test_image(PC_CL_RGB, PC_CS_U8);
    pc_buf b = enc(&img, PC_CF_JPEG, NULL);
    pc_cimage ours = dec(b.data, b.len);
    uint8_t *theirs = NULL;
    size_t n = 0;
    size_t i;
    int max = 0;
    CHECK(jpeg_oracle_rgb8(b.data, b.len, &theirs, &n));
    CHECK(n == ours.len);
    for (i = 0; i < n && i < ours.len; i++) {
        int d = abs((int)ours.data[i] - (int)theirs[i]);
        max = d > max ? d : max;
    }
    CHECK_EQ_INT(max <= 1, 1);
    free(theirs);
    pc_cimage_fini(&ours);
    pc_buf_fini(&b);
    pc_cimage_fini(&img);
}

static void test_jpeg_progressive_from_other_encoder_decodes(void)
{
    pc_cimage img = test_image(PC_CL_RGB, PC_CS_U8);
    pc_buf b = jpeg_write(img.data, img.width, img.height, JCS_RGB, 3, false, true, 90);
    pc_cimage back = dec(b.data, b.len);
    CHECK(psnr(&img, &back) > 25.0);
    pc_cimage_fini(&back);
    pc_buf_fini(&b);
    pc_cimage_fini(&img);
}

/* ---------------------------------------------------------------------------
 * Oracle: files we write are readable by another decoder
 * --------------------------------------------------------------------------- */

static void test_png_output_matches_oracle_all_layouts(void)
{
    static const pc_clayout L[4] = {PC_CL_GRAY, PC_CL_GRAYA, PC_CL_RGB, PC_CL_RGBA};
    static const pc_csample S[2] = {PC_CS_U8, PC_CS_U16};
    int i;
    int k;
    for (i = 0; i < 4; i++) {
        for (k = 0; k < 2; k++) {
            pc_cimage img = test_image(L[i], S[k]);
            pc_buf b = enc(&img, PC_CF_PNG, NULL);
            uint16_t *o = NULL;
            uint16_t *want = NULL;
            size_t n = 0;
            uint32_t w = 0;
            uint32_t h = 0;
            CHECK(png_oracle_rgba16(b.data, b.len, &o, &w, &h));
            CHECK_EQ_INT(pc_cimage_to_rgba16(&img, &want, &n, NULL), PC_OK);
            if (!(o != NULL && want != NULL && n == (size_t)w * h * 4 && rgba16_eq(o, want, n))) {
                PC_T_FAIL("%s %s", pc_clayout_name(L[i]), pc_csample_name(S[k]));
            }
            free(o);
            free(want);
            pc_buf_fini(&b);
            pc_cimage_fini(&img);
        }
    }
}

static void test_tiff_output_matches_oracle(void)
{
    static const pc_clayout L[3] = {PC_CL_GRAY, PC_CL_RGB, PC_CL_RGBA};
    static const pc_csample S[2] = {PC_CS_U8, PC_CS_U16};
    int i;
    int k;
    for (i = 0; i < 3; i++) {
        for (k = 0; k < 2; k++) {
            pc_cimage img = test_image(L[i], S[k]);
            pc_buf b = enc(&img, PC_CF_TIFF, NULL);
            /* No libtiff here: our own decoder stands in for the image crate. */
            pc_cimage back = dec(b.data, b.len);
            uint16_t *o = NULL;
            uint16_t *want = NULL;
            size_t no = 0;
            size_t n = 0;
            CHECK_EQ_INT(pc_cimage_to_rgba16(&back, &o, &no, NULL), PC_OK);
            CHECK_EQ_INT(pc_cimage_to_rgba16(&img, &want, &n, NULL), PC_OK);
            if (!(o != NULL && want != NULL && n == no && rgba16_eq(o, want, n))) {
                PC_T_FAIL("%s %s", pc_clayout_name(L[i]), pc_csample_name(S[k]));
            }
            free(o);
            free(want);
            pc_cimage_fini(&back);
            pc_buf_fini(&b);
            pc_cimage_fini(&img);
        }
    }
}

static void test_tiff_f32_output_matches_oracle(void)
{
    pc_cimage img = test_image(PC_CL_RGBA, PC_CS_F32);
    pc_cencode_opts opt = pc_cencode_opts_default();
    pc_buf b;
    pc_cimage back;
    float *o = NULL;
    size_t n = 0;
    opt.tiff_compression = PC_TIFF_NONE;
    b = enc(&img, PC_CF_TIFF, &opt);
    /* No libtiff here: our own decoder stands in for the image crate. */
    back = dec(b.data, b.len);
    CHECK_EQ_INT(pc_cimage_to_rgba_f32(&back, &o, &n, NULL), PC_OK);
    CHECK(o != NULL && n * 4 == img.len && memcmp(o, img.data, img.len) == 0);
    free(o);
    pc_cimage_fini(&back);
    pc_buf_fini(&b);
    pc_cimage_fini(&img);
}

/* ---------------------------------------------------------------------------
 * PngSuite corpus (the .png files in corpus/pngsuite, fetched by `make corpus`; skipped when absent)
 * --------------------------------------------------------------------------- */

static uint8_t *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *v = NULL;
    long len;
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0) {
        v = malloc((size_t)len + 1);
        if (v != NULL && fread(v, 1, (size_t)len, f) != (size_t)len) {
            free(v);
            v = NULL;
        }
        *n = (size_t)len;
    }
    (void)fclose(f);
    return v;
}

static void test_pngsuite_corpus_matches_oracle(void)
{
    const char *dir = "corpus/pngsuite";
    DIR *d = opendir(dir);
    struct dirent *ent;
    int checked = 0;
    if (d == NULL) {
        (void)printf("(%s absent: run `make corpus`; skipped) ", dir);
        return;
    }
    while ((ent = readdir(d)) != NULL) {
        size_t nl = strlen(ent->d_name);
        char path[512];
        uint8_t *bytes;
        size_t n = 0;
        uint16_t *o = NULL;
        uint32_t ow = 0;
        uint32_t oh = 0;
        bool oracle_ok;
        pc_cimage ours;
        pc_error e;
        if (nl < 4 || strcmp(ent->d_name + nl - 4, ".png") != 0) {
            continue;
        }
        (void)snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);
        bytes = read_file(path, &n);
        if (bytes == NULL) {
            PC_T_FAIL("%s: unreadable", path);
            continue;
        }
        oracle_ok = png_oracle_rgba16(bytes, n, &o, &ow, &oh);
        if (pc_cdecode(bytes, n, &ours, &e) == PC_OK) {
            if (oracle_ok) {
                uint16_t *a = NULL;
                size_t na = 0;
                if (!(pc_cimage_to_rgba16(&ours, &a, &na, NULL) == PC_OK && ours.width == ow && ours.height == oh &&
                      na == (size_t)ow * oh * 4 && rgba16_eq(a, o, na))) {
                    PC_T_FAIL("%s: differs from libpng", path);
                }
                free(a);
                checked++;
            }
            pc_cimage_fini(&ours);
        } else if (oracle_ok) {
            PC_T_FAIL("%s: we failed but oracle decoded: %s", path, e.msg);
        }
        free(o);
        free(bytes);
    }
    (void)closedir(d);
    (void)printf("(pngsuite: %d files compared) ", checked);
    CHECK(checked > 100); /* else: run `make corpus` */
}

/* ---------------------------------------------------------------------------
 * Unit tests of crates/codecs/src/codecs/png.rs
 * --------------------------------------------------------------------------- */

static void test_png8_roundtrips_through_the_decoder(void)
{
    static const uint8_t PAL[3][3] = {{255, 0, 0}, {0, 0, 255}, {0, 0, 0}};
    static const uint8_t IDX[6] = {0, 1, 2, 1, 0, 2};
    static const uint8_t NINE[6] = {9, 9, 9, 9, 9, 9};
    static const uint8_t FIRST[8] = {255, 0, 0, 255, 0, 0, 255, 255};
    pc_buf b = PC_BUF_INIT;
    pc_buf bad = PC_BUF_INIT;
    pc_cimage img;
    pc_cimage px;
    CHECK_EQ_INT(pc_cencode_png_indexed(3, 2, IDX, 6, PAL, 3, 2, &b, NULL), PC_OK);
    img = dec(b.data, b.len);
    CHECK(img.width == 3 && img.height == 2);
    CHECK_EQ_INT(pc_cimage_convert(&img, PC_CL_RGBA, PC_CS_U8, &px, NULL), PC_OK);
    CHECK(px.len >= 12 && memcmp(px.data, FIRST, 8) == 0);
    CHECK(px.len >= 12 && px.data[11] == 0); /* transparent entry */
    CHECK(pc_cencode_png_indexed(3, 2, NINE, 6, PAL, 3, -1, &bad, NULL) != PC_OK);
    CHECK(pc_cencode_png_indexed(3, 3, IDX, 6, PAL, 3, -1, &bad, NULL) != PC_OK);
    pc_cimage_fini(&px);
    pc_cimage_fini(&img);
    pc_buf_fini(&bad);
    pc_buf_fini(&b);
}

/* The C encoder combines band checksums with zlib's adler32_combine (Rust had its own). */
static void test_adler_combine_matches_one_pass(void)
{
    size_t na = 100000;
    size_t nb = 70001;
    uint8_t *whole = malloc(na + nb);
    uint32_t i;
    uLong a;
    uLong b;
    for (i = 0; i < na; i++) {
        whole[i] = (uint8_t)(i * 7 + i / 13);
    }
    for (i = 0; i < nb; i++) {
        whole[na + i] = (uint8_t)(i * 31);
    }
    a = adler32(1L, whole, (uInt)na);
    b = adler32(1L, whole + na, (uInt)nb);
    CHECK_EQ_UINT(adler32_combine(a, b, (z_off_t)nb), adler32(1L, whole, (uInt)(na + nb)));
    free(whole);
}

/* Rust tests its static deflate_band directly at miniz_oxide block boundaries. The C deflate_raw
 * is static and zlib's blocks fall elsewhere, so the same data goes through the banded (parallel,
 * sync-flushed) encoder instead: images of at least 4 MB built from the same generator. */
static void test_bands_that_end_just_past_a_block_keep_all_their_data(void)
{
    static const uint32_t K[4] = {20, 20, 24, 28};
    static const size_t LEN[4] = {136916, 345044, 195796, 189142};
    static const pc_png_compression C[2] = {PC_PNG_FAST, PC_PNG_DEFAULT};
    int t;
    for (t = 0; t < 4; t++) {
        uint32_t w = (uint32_t)LEN[t] / 64; /* one band boundary per run of rows */
        uint32_t h = (uint32_t)((((size_t)5 << 20) + w - 1) / w);
        size_t n = (size_t)w * h;
        uint8_t *data = malloc(n);
        uint32_t x = 0x9E3779B9u ^ K[t];
        size_t i;
        pc_cimage img;
        int c;
        for (i = 0; i < n; i++) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            data[i] = (uint8_t)((x >> 8) % K[t]);
        }
        CHECK_EQ_INT(pc_cimage_from_raw(w, h, PC_CL_GRAY, PC_CS_U8, data, n, &img, NULL), PC_OK);
        for (c = 0; c < 2; c++) {
            pc_cencode_opts o = pc_cencode_opts_default();
            pc_buf b;
            pc_cimage back;
            o.png_compression = C[c];
            b = enc(&img, PC_CF_PNG, &o);
            back = dec(b.data, b.len);
            if (!(back.len == img.len && memcmp(back.data, img.data, img.len) == 0)) {
                PC_T_FAIL("k %u len %zu level %d: %zu of %zu bytes", K[t], LEN[t], c, back.len, img.len);
            }
            pc_cimage_fini(&back);
            pc_buf_fini(&b);
        }
        pc_cimage_fini(&img);
    }
}

static uint32_t rotl32(uint32_t v, uint32_t r) { return r == 0 ? v : (v << r) | (v >> (32 - r)); }

static void test_large_images_roundtrip_through_the_parallel_encoder(void)
{
    static const pc_clayout L[2] = {PC_CL_RGB, PC_CL_RGBA};
    static const pc_csample S[2] = {PC_CS_U8, PC_CS_U16};
    static const size_t CH[2] = {3, 4};
    static const size_t BY[2] = {1, 2};
    static const pc_png_compression C[2] = {PC_PNG_FAST, PC_PNG_DEFAULT};
    int t;
    for (t = 0; t < 2; t++) {
        uint32_t w = 1531;
        uint32_t h = 977;
        size_t n = (size_t)w * h * CH[t] * BY[t];
        uint8_t *px = malloc(n);
        size_t i;
        pc_cimage img;
        int c;
        for (i = 0; i < n; i++) {
            uint8_t v = (uint8_t)rotl32((uint32_t)(i / 3) * 2654435761u, (uint32_t)i % 7);
            px[i] = (uint8_t)(v / 3 + (uint8_t)(i % 97));
        }
        CHECK_EQ_INT(pc_cimage_from_raw(w, h, L[t], S[t], px, n, &img, NULL), PC_OK);
        CHECK(n >= ((size_t)4 << 20) || S[t] == PC_CS_U8); /* PARALLEL_MIN_BYTES: idat_stream's 4 MB */
        for (c = 0; c < 2; c++) {
            pc_cencode_opts o = pc_cencode_opts_default();
            pc_buf b;
            pc_cimage back;
            pc_cimage conv;
            uint16_t *orc = NULL;
            uint32_t ow = 0;
            uint32_t oh = 0;
            o.png_compression = C[c];
            b = enc(&img, PC_CF_PNG, &o);
            back = dec(b.data, b.len);
            CHECK(back.width == w && back.height == h);
            if (pc_cimage_convert(&back, L[t], S[t], &conv, NULL) == PC_OK) {
                if (!(conv.len == img.len && memcmp(conv.data, img.data, img.len) == 0)) {
                    PC_T_FAIL("%s %s level %d", pc_clayout_name(L[t]), pc_csample_name(S[t]), c);
                }
                pc_cimage_fini(&conv);
            } else {
                PC_T_FAIL("convert");
            }
            /* The oracle decoder (libpng) agrees. */
            CHECK(png_oracle_rgba16(b.data, b.len, &orc, &ow, &oh));
            CHECK(ow == w && oh == h);
            free(orc);
            pc_cimage_fini(&back);
            pc_buf_fini(&b);
        }
        pc_cimage_fini(&img);
    }
}

int main(int argc, char **argv)
{
    RUN(test_adam7_small_sizes_all_layouts);
    RUN(test_adam7_16bit);
    RUN(test_adam7_with_metadata);
    RUN(test_adam7_compressions);
    RUN(test_png_palette_and_low_bit_depth_expand);
    RUN(test_jpeg_grayscale_decodes_as_gray);
    RUN(test_jpeg_cmyk_roundtrip_with_adobe_marker);
    RUN(test_jpeg_ycck_decodes_to_cmyk);
    RUN(test_jpeg_cmyk_flat_colour_exact_ish);
    RUN(test_jpeg_rgb_matches_oracle_decoder);
    RUN(test_jpeg_progressive_from_other_encoder_decodes);
    RUN(test_png_output_matches_oracle_all_layouts);
    RUN(test_tiff_output_matches_oracle);
    RUN(test_tiff_f32_output_matches_oracle);
    RUN(test_pngsuite_corpus_matches_oracle);
    RUN(test_png8_roundtrips_through_the_decoder);
    RUN(test_adler_combine_matches_one_pass);
    RUN(test_bands_that_end_just_past_a_block_keep_all_their_data);
    RUN(test_large_images_roundtrip_through_the_parallel_encoder);
    return pc_t_finish();
}
