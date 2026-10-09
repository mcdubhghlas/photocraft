/* Lossy WebP: port of crates/codecs/tests/webp_lossy.rs. Files decode, look like the input (PSNR
 * by quality), get smaller as quality drops and are far smaller than lossless on photographic
 * content; alpha and metadata travel in the container; odd sizes and the limits behave. The Rust
 * test checked its own VP8 encoder against image-webp; here libwebp both encodes and decodes. */
#include "common.h"

static pc_cencode_opts lossy(uint8_t q)
{
    pc_cencode_opts o = pc_cencode_opts_default();
    o.webp_lossless = false;
    o.webp_quality = q;
    return o;
}

/* A smooth photograph-like image: gradients with a little grain. */
static pc_cimage photo(uint32_t w, uint32_t h, uint64_t seed) { return synth(w, h, PC_CL_RGB, PC_CS_U8, seed, 0.03f); }

static void test_decodes_with_the_right_size_and_looks_like_the_input(void)
{
    static const uint8_t Q[5] = {100, 90, 75, 50, 20};
    static const double MIN_PSNR[5] = {40.0, 36.0, 33.0, 30.0, 26.0};
    pc_cimage img = photo(97, 61, 3);
    size_t i;
    for (i = 0; i < 5; i++) {
        pc_cencode_opts o = lossy(Q[i]);
        pc_buf bytes = enc(&img, PC_CF_WEBP, &o);
        pc_cimage back = dec(bytes.data, bytes.len);
        double p;
        CHECK_EQ_UINT(back.width, 97);
        CHECK_EQ_UINT(back.height, 61);
        CHECK_EQ_INT(back.layout, PC_CL_RGB);
        p = psnr(&img, &back);
        if (p < MIN_PSNR[i]) {
            PC_T_FAIL("quality %u: PSNR %.1f dB below %.1f", (unsigned)Q[i], p, MIN_PSNR[i]);
        }
        pc_cimage_fini(&back);
        pc_buf_fini(&bytes);
    }
    pc_cimage_fini(&img);
}

static void test_smaller_at_lower_quality_and_much_smaller_than_lossless_on_photos(void)
{
    static const uint8_t Q[5] = {95, 80, 60, 40, 20};
    pc_cimage img = synth(256, 192, PC_CL_RGB, PC_CS_U8, 9, 0.08f);
    pc_buf ll = enc(&img, PC_CF_WEBP, NULL);
    size_t lossless = ll.len;
    size_t sizes[5];
    size_t i;
    for (i = 0; i < 5; i++) {
        pc_cencode_opts o = lossy(Q[i]);
        pc_buf b = enc(&img, PC_CF_WEBP, &o);
        sizes[i] = b.len;
        pc_buf_fini(&b);
    }
    for (i = 1; i < 5; i++) {
        if (sizes[i] > sizes[i - 1]) {
            PC_T_FAIL("sizes must not grow as quality drops: q%u %zu > q%u %zu", (unsigned)Q[i], sizes[i],
                      (unsigned)Q[i - 1], sizes[i - 1]);
        }
    }
    if (!(sizes[1] * 4 < lossless)) {
        PC_T_FAIL("q80 %zu vs lossless %zu", sizes[1], lossless);
    }
    if (!(sizes[4] * 10 < lossless)) {
        PC_T_FAIL("q20 %zu vs lossless %zu", sizes[4], lossless);
    }
    pc_t_checks += 6;
    pc_buf_fini(&ll);
    pc_cimage_fini(&img);
}

static void test_flat_areas_cost_almost_nothing(void)
{
    /* A single colour: every macroblock is skipped; the file is dominated by the header. */
    size_t n = 640 * 480 * 3;
    uint8_t *px = malloc(n);
    pc_cimage img;
    pc_cencode_opts o = lossy(80);
    pc_buf bytes;
    pc_cimage back;
    memset(px, 200, n);
    CHECK(pc_cimage_from_raw(640, 480, PC_CL_RGB, PC_CS_U8, px, n, &img, NULL) == PC_OK);
    bytes = enc(&img, PC_CF_WEBP, &o);
    if (bytes.len >= 1200) {
        PC_T_FAIL("%zu bytes", bytes.len);
    }
    back = dec(bytes.data, bytes.len);
    /* flat colour reproduced within rounding */
    CHECK(max_abs_diff(&img, &back) <= 3.0f / 255.0f);
    pc_cimage_fini(&back);
    pc_buf_fini(&bytes);
    pc_cimage_fini(&img);
}

static void test_alpha_is_kept_exactly_and_metadata_travels(void)
{
    pc_cimage img = synth(40, 30, PC_CL_RGBA, PC_CS_U8, 5, 0.05f);
    pc_buf icc = sample_icc(400);
    pc_buf exif = sample_exif();
    pc_cencode_opts o = lossy(85);
    pc_buf bytes;
    pc_cimage back;
    pc_cimage opaque;
    size_t i;
    bool alpha_same = true;
    set_blob(&img.icc, icc.data, icc.len);
    set_blob(&img.meta.exif, exif.data, exif.len);
    set_blob(&img.meta.xmp, SAMPLE_XMP, strlen(SAMPLE_XMP));
    bytes = enc(&img, PC_CF_WEBP, &o);
    CHECK(bytes.len >= 16 && memcmp(bytes.data + 12, "VP8X", 4) == 0); /* extended container */
    back = dec(bytes.data, bytes.len);
    CHECK_EQ_INT(back.layout, PC_CL_RGBA);
    for (i = 0; i < 40 * 30 && back.len == img.len; i++) {
        alpha_same = alpha_same && img.data[4 * i + 3] == back.data[4 * i + 3];
    }
    CHECK(back.len == img.len && alpha_same); /* alpha is stored losslessly */
    CHECK(psnr(&img, &back) > 30.0);
    CHECK(pc_blob_eq(&back.icc, &img.icc));
    CHECK(back.meta.xmp.has);
    CHECK(back.meta.exif.has);
    pc_cimage_fini(&back);
    pc_buf_fini(&bytes);
    /* Fully opaque alpha is dropped rather than stored. */
    opaque = synth(40, 30, PC_CL_RGBA, PC_CS_U8, 5, 0.0f);
    for (i = 0; i < 40 * 30; i++) {
        opaque.data[4 * i + 3] = 255;
    }
    o.embed_icc = false;
    o.embed_metadata = false;
    bytes = enc(&opaque, PC_CF_WEBP, &o);
    CHECK(bytes.len >= 16 && memcmp(bytes.data + 12, "VP8 ", 4) == 0);
    back = dec(bytes.data, bytes.len);
    CHECK_EQ_INT(back.layout, PC_CL_RGB);
    pc_cimage_fini(&back);
    pc_buf_fini(&bytes);
    pc_cimage_fini(&opaque);
    pc_buf_fini(&exif);
    pc_buf_fini(&icc);
    pc_cimage_fini(&img);
}

static void test_gray_and_odd_sizes_and_tiny_images(void)
{
    static const uint32_t SIZES[7][2] = {{1, 1}, {2, 3}, {17, 9}, {33, 1}, {1, 47}, {16, 16}, {31, 15}};
    static const pc_clayout LAYOUTS[4] = {PC_CL_GRAY, PC_CL_GRAYA, PC_CL_RGB, PC_CL_RGBA};
    size_t si;
    size_t li;
    for (si = 0; si < 7; si++) {
        uint32_t w = SIZES[si][0];
        uint32_t h = SIZES[si][1];
        for (li = 0; li < 4; li++) {
            /* Little noise: at these sizes the 2 x 2 chroma average alone would otherwise dominate. */
            pc_cimage img = synth(w, h, LAYOUTS[li], PC_CS_U8, (uint64_t)(w * 100 + h), 0.02f);
            pc_cencode_opts o = lossy(80);
            pc_buf bytes = enc(&img, PC_CF_WEBP, &o);
            pc_cimage back = dec(bytes.data, bytes.len);
            CHECK_EQ_UINT(back.width, w);
            CHECK_EQ_UINT(back.height, h);
            /* Below 8 px the 2 x 2 chroma average of random saturated pixels is the whole error. */
            if (w >= 8 && h >= 8) {
                pc_cimage conv;
                if (pc_cimage_convert(&img, back.layout, PC_CS_U8, &conv, NULL) == PC_OK) {
                    double p = psnr(&conv, &back);
                    if (!(p > 24.0)) {
                        PC_T_FAIL("%ux%u %s: %.1f dB", (unsigned)w, (unsigned)h, pc_clayout_name(LAYOUTS[li]), p);
                    }
                    pc_cimage_fini(&conv);
                } else {
                    PC_T_FAIL("convert");
                }
            }
            pc_cimage_fini(&back);
            pc_buf_fini(&bytes);
            pc_cimage_fini(&img);
        }
    }
}

static void test_sixteen_bit_input_is_quantised_to_eight(void)
{
    pc_cimage img = synth(24, 24, PC_CL_RGB, PC_CS_U16, 2, 0.02f);
    pc_cencode_opts o = lossy(90);
    pc_buf bytes = enc(&img, PC_CF_WEBP, &o);
    pc_cimage back = dec(bytes.data, bytes.len);
    pc_cimage conv;
    CHECK(pc_cimage_convert(&img, PC_CL_RGB, PC_CS_U8, &conv, NULL) == PC_OK);
    CHECK(psnr(&conv, &back) > 34.0);
    pc_cimage_fini(&conv);
    pc_cimage_fini(&back);
    pc_buf_fini(&bytes);
    pc_cimage_fini(&img);
}

static void test_dimension_limit_is_an_error_not_a_panic(void)
{
    pc_cimage img;
    pc_cencode_opts o = lossy(50);
    pc_buf bytes = PC_BUF_INIT;
    pc_error e;
    pc_cimage back;
    CHECK(pc_cimage_new(16384, 1, PC_CL_RGB, PC_CS_U8, &img, NULL) == PC_OK);
    memset(&e, 0, sizeof e);
    CHECK_EQ_INT(pc_cencode(&img, PC_CF_WEBP, &o, &bytes, &e), PC_ERR_UNSUPPORTED);
    CHECK_CONTAINS(e.msg, "encode failed"); /* CodecError::Encode */
    pc_cimage_fini(&img);
    CHECK(pc_cimage_new(16383, 1, PC_CL_RGB, PC_CS_U8, &img, NULL) == PC_OK);
    pc_buf_fini(&bytes);
    bytes = enc(&img, PC_CF_WEBP, &o);
    back = dec(bytes.data, bytes.len);
    CHECK_EQ_UINT(back.width, 16383);
    CHECK_EQ_UINT(back.height, 1);
    pc_cimage_fini(&back);
    pc_buf_fini(&bytes);
    pc_cimage_fini(&img);
}

static void test_hard_edges_and_text_like_content_keep_their_structure(void)
{
    /* Black text-like strokes on white: sub-block modes keep edges sharp enough that the
     * thresholded result matches the source almost everywhere. */
    uint32_t w = 128;
    uint32_t h = 64;
    size_t n = (size_t)w * h * 3;
    uint8_t *data = malloc(n);
    pc_cimage img;
    pc_cencode_opts o = lossy(90);
    pc_buf bytes;
    pc_cimage back;
    uint32_t x;
    uint32_t y;
    size_t i;
    size_t wrong = 0;
    size_t total = (size_t)w * h;
    double p;
    memset(data, 255, n);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            bool stroke = (x % 11 < 2 && y > 8 && y < 56) || (y % 13 < 2 && x > 4 && x < 124);
            if (stroke) {
                memset(data + ((size_t)y * w + x) * 3, 0, 3);
            }
        }
    }
    CHECK(pc_cimage_from_raw(w, h, PC_CL_RGB, PC_CS_U8, data, n, &img, NULL) == PC_OK);
    bytes = enc(&img, PC_CF_WEBP, &o);
    back = dec(bytes.data, bytes.len);
    for (i = 0; i < total && back.len == img.len; i++) {
        wrong += (img.data[3 * i] < 128) != (back.data[3 * i] < 128);
    }
    CHECK(back.len == img.len);
    if (!(wrong * 100 < total * 2)) {
        PC_T_FAIL("%zu of %zu pixels flipped side of mid-gray", wrong, total);
    }
    p = psnr(&img, &back);
    if (!(p > 30.0)) {
        PC_T_FAIL("%.1f", p);
    }
    pc_t_checks += 2;
    pc_cimage_fini(&back);
    pc_buf_fini(&bytes);
    pc_cimage_fini(&img);
}

int main(int argc, char **argv)
{
    RUN(test_decodes_with_the_right_size_and_looks_like_the_input);
    RUN(test_smaller_at_lower_quality_and_much_smaller_than_lossless_on_photos);
    RUN(test_flat_areas_cost_almost_nothing);
    RUN(test_alpha_is_kept_exactly_and_metadata_travels);
    RUN(test_gray_and_odd_sizes_and_tiny_images);
    RUN(test_sixteen_bit_input_is_quantised_to_eight);
    RUN(test_dimension_limit_is_an_error_not_a_panic);
    RUN(test_hard_edges_and_text_like_content_keep_their_structure);
    return pc_t_finish();
}
