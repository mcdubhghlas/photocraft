/* Round-trip every writable format x every sample type x every layout (port of
 * crates/codecs/tests/roundtrip.rs). Lossless formats reproduce the planned conversion exactly;
 * lossy ones stay above a PSNR threshold. */
#include "common.h"

static double psnr_threshold(pc_cformat f)
{
    switch (f) {
    case PC_CF_JPEG:
        return 30.0;
    case PC_CF_GIF:
        return 20.0;
    case PC_CF_HDR:
        return 40.0;
    case PC_CF_AVIF:
        return 28.0;
    default:
        return INFINITY;
    }
}

static bool caps_has_layout(const pc_ccaps *c, pc_clayout l)
{
    size_t i;
    for (i = 0; i < c->nlayouts; i++) {
        if (c->layouts[i] == l) {
            return true;
        }
    }
    return false;
}

static bool caps_has_depth(const pc_ccaps *c, pc_csample s)
{
    size_t i;
    for (i = 0; i < c->ndepths; i++) {
        if (c->depths[i] == s) {
            return true;
        }
    }
    return false;
}

static void check_roundtrip(pc_cformat f, pc_clayout l, pc_csample s)
{
    pc_ccaps c = pc_cformat_caps(f);
    pc_cimage img;
    pc_buf b;
    pc_cimage back;
    pc_cimage expected;
    pc_cformat det;
    bool float_pnm = f == PC_CF_PNM && pc_csample_is_float(s);
    char name[96];
    if (!c.write) {
        return;
    }
    (void)snprintf(name, sizeof name, "%s %s %s", pc_cformat_debug(f), pc_clayout_name(l), pc_csample_name(s));
    pc_t_name = name;
    if (f == PC_CF_GIF) {
        img = synth(37, 23, l, s, 7, 0.0f);
    } else if (c.lossy) {
        img = synth(37, 23, l, s, 7, 0.02f);
    } else {
        img = test_image(l, s);
    }
    b = enc(&img, f, NULL);
    CHECK(pc_cformat_detect(b.data, b.len, &det) && det == f);
    if (!c.read) {
        pc_buf_fini(&b);
        pc_cimage_fini(&img);
        return;
    }
    back = dec(b.data, b.len);
    CHECK(back.width == img.width && back.height == img.height);
    if (caps_has_layout(&c, l) && caps_has_depth(&c, s) && !float_pnm) {
        CHECK(back.layout == l);
        CHECK(back.sample == s);
    }
    if (caps_has_depth(&c, s) && !float_pnm) {
        CHECK(back.sample == s);
    }
    if (s == PC_CS_U16 && caps_has_depth(&c, PC_CS_U16)) {
        CHECK(back.sample == PC_CS_U16 || back.sample == PC_CS_F32);
    }
    if (back.width == img.width && back.height == img.height &&
        pc_cimage_convert(&img, back.layout, back.sample, &expected, NULL) == PC_OK) {
        if (f == PC_CF_GIF && pc_clayout_has_alpha(l)) {
            /* GIF alpha is binary: compare colour only on opaque pixels. */
            size_t ch = pc_clayout_channels(back.layout);
            float *e = NULL;
            float *bk = NULL;
            size_t ne;
            size_t nb;
            size_t p;
            pc_cimage cmp;
            (void)pc_cimage_to_normalized(&expected, &e, &ne, NULL);
            (void)pc_cimage_to_normalized(&back, &bk, &nb, NULL);
            for (p = 0; p + ch <= ne; p += ch) {
                if (bk[p + ch - 1] == 0.0f) {
                    memcpy(e + p, bk + p, ch * sizeof *e);
                }
                e[p + ch - 1] = bk[p + ch - 1];
            }
            if (pc_cimage_from_normalized(img.width, img.height, back.layout, back.sample, e, ne, &cmp, NULL) ==
                PC_OK) {
                pc_cimage_fini(&expected);
                expected = cmp;
            }
            free(e);
            free(bk);
        }
        if (c.lossy) {
            double p = psnr(&expected, &back);
            if (!(p >= psnr_threshold(f))) {
                PC_T_FAIL("PSNR %.2f dB below %.0f", p, psnr_threshold(f));
            }
        } else if (expected.len != back.len || memcmp(expected.data, back.data, back.len) != 0) {
            PC_T_FAIL("lossless round-trip mismatch (max diff %g)", (double)max_abs_diff(&expected, &back));
        }
        pc_cimage_fini(&expected);
    }
    pc_cimage_fini(&back);
    pc_buf_fini(&b);
    pc_cimage_fini(&img);
}

static void test_matrix(void)
{
    int f;
    int l;
    int s;
    for (f = 0; f < PC_CF_COUNT; f++) {
        for (l = 0; l < PC_CL_COUNT; l++) {
            for (s = 0; s < PC_CS_COUNT; s++) {
                check_roundtrip((pc_cformat)f, (pc_clayout)l, (pc_csample)s);
            }
        }
    }
}

static void test_hdr_values_survive_float_formats(void)
{
    float vals[16 * 8 * 3];
    pc_cimage img;
    static const pc_cformat FS[3] = {PC_CF_TIFF, PC_CF_EXR, PC_CF_PNM};
    int i;
    for (i = 0; i < 16 * 8 * 3; i++) {
        vals[i] = ((float)i - 40.0f) * 0.37f;
    }
    CHECK_EQ_INT(pc_cimage_from_copy(16, 8, PC_CL_RGB, PC_CS_F32, vals, sizeof vals, &img, NULL), PC_OK);
    for (i = 0; i < 3; i++) {
        pc_buf b;
#ifdef PC_NO_EXR
        if (FS[i] == PC_CF_EXR) {
            continue; /* Rust always builds OpenEXR; here it is optional */
        }
#endif
        b = enc(&img, FS[i], NULL);
        pc_cimage back = dec(b.data, b.len);
        CHECK(back.sample == PC_CS_F32 && back.len == sizeof vals && memcmp(back.data, vals, sizeof vals) == 0);
        pc_cimage_fini(&back);
        pc_buf_fini(&b);
    }
    pc_cimage_fini(&img);
}

static void test_hdr_format_keeps_bright_values_approximately(void)
{
    float vals[16 * 8 * 3];
    pc_cimage img;
    pc_buf b;
    pc_cimage back;
    int i;
    for (i = 0; i < 16 * 8 * 3; i++) {
        vals[i] = 0.01f + (float)i * 0.25f;
    }
    CHECK_EQ_INT(pc_cimage_from_copy(16, 8, PC_CL_RGB, PC_CS_F32, vals, sizeof vals, &img, NULL), PC_OK);
    b = enc(&img, PC_CF_HDR, NULL);
    back = dec(b.data, b.len);
    CHECK(back.sample == PC_CS_F32 && back.len == sizeof vals);
    if (back.len == sizeof vals) {
        const float *got = (const float *)(const void *)back.data;
        for (i = 0; i < 16 * 8; i++) {
            float m = fmaxf(vals[3 * i], fmaxf(vals[3 * i + 1], vals[3 * i + 2]));
            int k;
            for (k = 0; k < 3; k++) {
                if (!(fabsf(vals[3 * i + k] - got[3 * i + k]) <= m * 0.01f)) {
                    PC_T_FAIL("%g vs %g", (double)vals[3 * i + k], (double)got[3 * i + k]);
                }
            }
        }
    }
    pc_cimage_fini(&back);
    pc_buf_fini(&b);
    pc_cimage_fini(&img);
}

static void test_f16_exr_is_bit_exact(void)
{
#ifndef PC_NO_EXR
    uint16_t vals[20 * 10 * 4];
    pc_cimage img;
    int c;
    int i;
    for (i = 0; i < 20 * 10 * 4; i++) {
        vals[i] = pc_f16_from_f32((float)i * 0.013f - 1.0f);
    }
    CHECK_EQ_INT(pc_cimage_from_copy(20, 10, PC_CL_RGBA, PC_CS_F16, vals, sizeof vals, &img, NULL), PC_OK);
    for (c = 0; c < 5; c++) {
        pc_cencode_opts o = pc_cencode_opts_default();
        pc_buf b;
        pc_cimage back;
        o.exr_compression = (pc_exr_compression)c;
        b = enc(&img, PC_CF_EXR, &o);
        back = dec(b.data, b.len);
        CHECK(back.sample == PC_CS_F16 && back.len == sizeof vals && memcmp(back.data, vals, sizeof vals) == 0);
        pc_cimage_fini(&back);
        pc_buf_fini(&b);
    }
    pc_cimage_fini(&img);
#endif
}

static void test_tiff_all_compressions_lossless(void)
{
    static const pc_csample S[3] = {PC_CS_U8, PC_CS_U16, PC_CS_F32};
    int l;
    int s;
    int c;
    for (l = 0; l < PC_CL_COUNT; l++) {
        for (s = 0; s < 3; s++) {
            pc_cimage img = test_image((pc_clayout)l, S[s]);
            for (c = 0; c < 4; c++) {
                pc_cencode_opts o = pc_cencode_opts_default();
                pc_buf b;
                pc_cimage back;
                o.tiff_compression = (pc_tiff_compression)c;
                b = enc(&img, PC_CF_TIFF, &o);
                back = dec(b.data, b.len);
                CHECK(back.width == img.width && back.layout == img.layout && back.sample == img.sample &&
                      back.len == img.len && memcmp(back.data, img.data, img.len) == 0 && back.nwarnings == 0 &&
                      !back.icc.has);
                pc_cimage_fini(&back);
                pc_buf_fini(&b);
            }
            pc_cimage_fini(&img);
        }
    }
}

static void test_png_all_compressions_lossless(void)
{
    pc_cimage img = test_image(PC_CL_RGBA, PC_CS_U16);
    int c;
    for (c = 0; c < 4; c++) {
        pc_cencode_opts o = pc_cencode_opts_default();
        pc_buf b;
        pc_cimage back;
        o.png_compression = (pc_png_compression)c;
        b = enc(&img, PC_CF_PNG, &o);
        back = dec(b.data, b.len);
        CHECK(back.len == img.len && memcmp(back.data, img.data, img.len) == 0);
        pc_cimage_fini(&back);
        pc_buf_fini(&b);
    }
    pc_cimage_fini(&img);
}

static void test_jpeg_quality_affects_size_and_error(void)
{
    pc_cimage img = test_image(PC_CL_RGB, PC_CS_U8);
    pc_cencode_opts lo = pc_cencode_opts_default();
    pc_cencode_opts hi = pc_cencode_opts_default();
    pc_buf a;
    pc_buf b;
    pc_cimage da;
    pc_cimage db;
    lo.jpeg_quality = 20;
    hi.jpeg_quality = 98;
    a = enc(&img, PC_CF_JPEG, &lo);
    b = enc(&img, PC_CF_JPEG, &hi);
    CHECK(a.len < b.len);
    da = dec(a.data, a.len);
    db = dec(b.data, b.len);
    CHECK(psnr(&img, &db) > psnr(&img, &da));
    pc_cimage_fini(&da);
    pc_cimage_fini(&db);
    pc_buf_fini(&a);
    pc_buf_fini(&b);
    pc_cimage_fini(&img);
}

static void test_jpeg_444_beats_420_on_chroma_detail(void)
{
    pc_cimage img = synth(64, 64, PC_CL_RGB, PC_CS_U8, 3, 0.5f);
    pc_cencode_opts a = pc_cencode_opts_default();
    pc_cencode_opts b = pc_cencode_opts_default();
    pc_buf ba;
    pc_buf bb;
    pc_cimage da;
    pc_cimage db;
    a.jpeg_chroma_subsampling = false;
    a.jpeg_quality = 95;
    b.jpeg_chroma_subsampling = true;
    b.jpeg_quality = 95;
    ba = enc(&img, PC_CF_JPEG, &a);
    bb = enc(&img, PC_CF_JPEG, &b);
    da = dec(ba.data, ba.len);
    db = dec(bb.data, bb.len);
    CHECK(psnr(&img, &da) > psnr(&img, &db));
    pc_cimage_fini(&da);
    pc_cimage_fini(&db);
    pc_buf_fini(&ba);
    pc_buf_fini(&bb);
    pc_cimage_fini(&img);
}

static void test_webp_lossy_request_is_a_lossy_file(void)
{
    pc_cimage img = test_image(PC_CL_RGB, PC_CS_U8);
    pc_cencode_opts o = pc_cencode_opts_default();
    pc_buf b;
    pc_cimage back;
    pc_cfidelity w[16];
    size_t nw;
    size_t i;
    bool lossy = false;
    o.webp_lossless = false;
    b = enc(&img, PC_CF_WEBP, &o);
    CHECK(b.len > 16 && memcmp(b.data + 12, "VP8 ", 4) == 0);
    back = dec(b.data, b.len);
    CHECK(back.width == img.width && back.height == img.height);
    nw = pc_cfidelity_warnings_with(&img, PC_CF_WEBP, &o, w, 16);
    for (i = 0; i < nw && i < 16; i++) {
        lossy = lossy || w[i].kind == PC_FW_LOSSY_COMPRESSION;
    }
    CHECK(lossy);
    pc_cimage_fini(&back);
    pc_buf_fini(&b);
    pc_cimage_fini(&img);
}

static void test_one_pixel_images_roundtrip(void)
{
    int f;
    for (f = 0; f < PC_CF_COUNT; f++) {
        pc_cimage img;
        pc_buf b;
        pc_cimage back;
        if (!rw((pc_cformat)f)) {
            continue;
        }
        pc_t_name = pc_cformat_debug((pc_cformat)f);
        img = synth(1, 1, PC_CL_RGB, PC_CS_U8, 1, 0.0f);
        b = enc(&img, (pc_cformat)f, NULL);
        back = dec(b.data, b.len);
        CHECK(back.width == 1 && back.height == 1);
        pc_cimage_fini(&back);
        pc_buf_fini(&b);
        pc_cimage_fini(&img);
    }
}

static void test_ico_rejects_oversize(void)
{
    pc_cimage img;
    pc_cencode_opts o = pc_cencode_opts_default();
    pc_buf b = PC_BUF_INIT;
    pc_error e;
    CHECK_EQ_INT(pc_cimage_new(300, 10, PC_CL_RGBA, PC_CS_U8, &img, NULL), PC_OK);
    CHECK(pc_cencode(&img, PC_CF_ICO, &o, &b, &e) == PC_ERR_UNSUPPORTED);
    CHECK_CONTAINS(e.msg, "encode failed");
    pc_cimage_fini(&img);
    CHECK_EQ_INT(pc_cimage_new(256, 256, PC_CL_RGBA, PC_CS_U8, &img, NULL), PC_OK);
    CHECK_EQ_INT(pc_cencode(&img, PC_CF_ICO, &o, &b, NULL), PC_OK);
    pc_buf_fini(&b);
    pc_cimage_fini(&img);
}

static void test_empty_image_rejected(void)
{
    pc_cimage img;
    pc_cencode_opts o = pc_cencode_opts_default();
    int f;
    CHECK_EQ_INT(pc_cimage_new(0, 5, PC_CL_RGB, PC_CS_U8, &img, NULL), PC_OK);
    for (f = 0; f < PC_CF_COUNT; f++) {
        pc_buf b = PC_BUF_INIT;
        if (writable((pc_cformat)f)) {
            CHECK(pc_cencode(&img, (pc_cformat)f, &o, &b, NULL) != PC_OK);
        }
        pc_buf_fini(&b);
    }
    pc_cimage_fini(&img);
}

static void test_larger_image_png_tiff_exact(void)
{
    pc_cimage img = synth(513, 257, PC_CL_RGBA, PC_CS_U16, 99, 1.0f);
    static const pc_cformat FS[3] = {PC_CF_PNG, PC_CF_TIFF, PC_CF_PNM};
    int i;
    for (i = 0; i < 3; i++) {
        pc_buf b = enc(&img, FS[i], NULL);
        pc_cimage back = dec(b.data, b.len);
        CHECK(back.len == img.len && memcmp(back.data, img.data, img.len) == 0);
        pc_cimage_fini(&back);
        pc_buf_fini(&b);
    }
    pc_cimage_fini(&img);
}

int main(int argc, char **argv)
{
    RUN(test_matrix);
    RUN(test_hdr_values_survive_float_formats);
    RUN(test_hdr_format_keeps_bright_values_approximately);
    RUN(test_f16_exr_is_bit_exact);
    RUN(test_tiff_all_compressions_lossless);
    RUN(test_png_all_compressions_lossless);
    RUN(test_jpeg_quality_affects_size_and_error);
    RUN(test_jpeg_444_beats_420_on_chroma_detail);
    RUN(test_webp_lossy_request_is_a_lossy_file);
    RUN(test_one_pixel_images_roundtrip);
    RUN(test_ico_rejects_oversize);
    RUN(test_empty_image_rejected);
    RUN(test_larger_image_png_tiff_exact);
    return pc_t_finish();
}
