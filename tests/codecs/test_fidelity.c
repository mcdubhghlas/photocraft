/* pc_cfidelity_warnings cases, and consistency with what pc_cencode does (port of
 * crates/codecs/tests/fidelity.rs).
 *
 * OpenEXR may be compiled out (PC_NO_EXR); the EXR cases then expect only WriteUnsupported. */
#include "codecs/internal.h"
#include "common.h"

#define MAXW 32

typedef struct warns_t {
    pc_cfidelity w[MAXW];
    size_t n;
} warns_t;

static warns_t warns_with(const pc_cimage *img, pc_cformat f, const pc_cencode_opts *o)
{
    warns_t r;
    r.n = o != NULL ? pc_cfidelity_warnings_with(img, f, o, r.w, MAXW) : pc_cfidelity_warnings(img, f, r.w, MAXW);
    if (r.n > MAXW) {
        PC_T_FAIL("%zu warnings, more than %d", r.n, MAXW);
        r.n = MAXW;
    }
    return r;
}

static warns_t warns(const pc_cimage *img, pc_cformat f) { return warns_with(img, f, NULL); }

static pc_cfidelity W(pc_cfidelity_kind k)
{
    pc_cfidelity w;
    memset(&w, 0, sizeof w);
    w.kind = k;
    return w;
}

static pc_cfidelity depth_reduced(pc_csample from, pc_csample to)
{
    pc_cfidelity w = W(PC_FW_DEPTH_REDUCED);
    w.from = from;
    w.to = to;
    return w;
}

static pc_cfidelity cmyk_converted(pc_clayout to)
{
    pc_cfidelity w = W(PC_FW_CMYK_CONVERTED);
    w.to_layout = to;
    return w;
}

static bool contains(const warns_t *ws, pc_cfidelity w)
{
    size_t i;
    for (i = 0; i < ws->n; i++) {
        if (pc_cfidelity_eq(&ws->w[i], &w)) {
            return true;
        }
    }
    return false;
}

static bool any_kind(const warns_t *ws, pc_cfidelity_kind k)
{
    size_t i;
    for (i = 0; i < ws->n; i++) {
        if (ws->w[i].kind == k) {
            return true;
        }
    }
    return false;
}

static bool contains_kind(const warns_t *ws, pc_cfidelity_kind k) { return contains(ws, W(k)); }

/* Without OpenEXR, every EXR export reports exactly WriteUnsupported { OpenExr }. */
static bool exr_unsupported_only(const warns_t *ws)
{
    pc_cfidelity u = W(PC_FW_WRITE_UNSUPPORTED);
    u.format = PC_CF_EXR;
    return ws->n == 1 && pc_cfidelity_eq(&ws->w[0], &u);
}

static void test_png_u8_rgb_is_clean(void)
{
    pc_cimage img = test_image(PC_CL_RGB, PC_CS_U8);
    warns_t w = warns(&img, PC_CF_PNG);
    CHECK_EQ_UINT(w.n, 0);
    pc_cimage_fini(&img);
}

static void test_png_u16_rgba_is_clean(void)
{
    pc_cimage img = test_image(PC_CL_RGBA, PC_CS_U16);
    warns_t w = warns(&img, PC_CF_PNG);
    CHECK_EQ_UINT(w.n, 0);
    pc_cimage_fini(&img);
}

static void test_tiff_is_clean_for_all_integer_and_f32_layouts(void)
{
    static const pc_csample S[] = {PC_CS_U8, PC_CS_U16, PC_CS_F32, PC_CS_F16};
    int l;
    size_t s;
    for (l = 0; l < PC_CL_COUNT; l++) {
        for (s = 0; s < 4; s++) {
            pc_cimage img = test_image((pc_clayout)l, S[s]);
            warns_t w = warns(&img, PC_CF_TIFF);
            if (w.n != 0) {
                PC_T_FAIL("%s %s: %zu warnings", pc_clayout_name((pc_clayout)l), pc_csample_name(S[s]), w.n);
            }
            pc_t_checks++;
            pc_cimage_fini(&img);
        }
    }
}

static void test_sixteen_bit_to_jpeg_reduces(void)
{
    pc_cimage img = test_image(PC_CL_RGB, PC_CS_U16);
    warns_t w = warns(&img, PC_CF_JPEG);
    CHECK(contains(&w, depth_reduced(PC_CS_U16, PC_CS_U8)));
    CHECK(contains_kind(&w, PC_FW_LOSSY_COMPRESSION));
    pc_cimage_fini(&img);
}

static void test_sixteen_bit_message_text(void)
{
    pc_cfidelity w = depth_reduced(PC_CS_U16, PC_CS_U8);
    char t[128];
    pc_cfidelity_text(&w, t, sizeof t);
    CHECK_EQ_STR(t, "16-bit will be reduced to 8-bit");
}

static void test_alpha_to_jpeg_discarded(void)
{
    pc_cimage img = test_image(PC_CL_RGBA, PC_CS_U8);
    warns_t w = warns(&img, PC_CF_JPEG);
    CHECK(contains_kind(&w, PC_FW_ALPHA_DISCARDED));
    pc_cimage_fini(&img);
}

static void test_opaque_alpha_not_reported(void)
{
    pc_cimage img = test_image(PC_CL_RGBA, PC_CS_U8);
    warns_t w;
    size_t i;
    for (i = 3; i < img.len; i += 4) {
        img.data[i] = 255;
    }
    w = warns(&img, PC_CF_JPEG);
    CHECK(!contains_kind(&w, PC_FW_ALPHA_DISCARDED));
    pc_cimage_fini(&img);
}

static void test_alpha_to_hdr_discarded(void)
{
    pc_cimage img = test_image(PC_CL_RGBA, PC_CS_F32);
    warns_t w = warns(&img, PC_CF_HDR);
    CHECK(contains_kind(&w, PC_FW_ALPHA_DISCARDED));
    pc_cimage_fini(&img);
}

static void test_cmyk_to_png_converted(void)
{
    pc_cimage img = test_image(PC_CL_CMYK, PC_CS_U8);
    warns_t w = warns(&img, PC_CF_PNG);
    CHECK(contains(&w, cmyk_converted(PC_CL_RGB)));
    pc_cimage_fini(&img);
}

static void test_cmyka_to_webp_converted_keeps_alpha(void)
{
    pc_cimage img = test_image(PC_CL_CMYKA, PC_CS_U8);
    warns_t w = warns(&img, PC_CF_WEBP);
    CHECK(contains(&w, cmyk_converted(PC_CL_RGBA)));
    CHECK(!contains_kind(&w, PC_FW_ALPHA_DISCARDED));
    pc_cimage_fini(&img);
}

static void test_cmyk_to_jpeg_and_tiff_not_converted(void)
{
    static const pc_cformat FS[] = {PC_CF_JPEG, PC_CF_TIFF, PC_CF_PNM};
    size_t i;
    for (i = 0; i < 3; i++) {
        pc_cimage img = test_image(PC_CL_CMYK, PC_CS_U8);
        warns_t w = warns(&img, FS[i]);
        pc_t_name = pc_cformat_debug(FS[i]);
        CHECK(!any_kind(&w, PC_FW_CMYK_CONVERTED));
        pc_cimage_fini(&img);
    }
}

static pc_cimage with_icc(pc_cimage img)
{
    pc_buf icc = sample_icc(100);
    set_blob(&img.icc, icc.data, icc.len);
    pc_buf_fini(&icc);
    return img;
}

static void test_icc_dropped_for_bmp(void)
{
    pc_cimage img = with_icc(test_image(PC_CL_RGB, PC_CS_U8));
    warns_t a = warns(&img, PC_CF_BMP);
    warns_t b = warns(&img, PC_CF_PNG);
    CHECK(contains_kind(&a, PC_FW_ICC_DROPPED));
    CHECK(!contains_kind(&b, PC_FW_ICC_DROPPED));
    pc_cimage_fini(&img);
}

static void test_icc_warning_suppressed_when_not_embedding(void)
{
    pc_cimage img = with_icc(test_image(PC_CL_RGB, PC_CS_U8));
    pc_cencode_opts o = pc_cencode_opts_default();
    warns_t w;
    o.embed_icc = false;
    w = warns_with(&img, PC_CF_BMP, &o);
    CHECK(!contains_kind(&w, PC_FW_ICC_DROPPED));
    pc_cimage_fini(&img);
}

/* exif, xmp "x", 72 dpi, one text entry. */
static pc_cimage with_meta(pc_cimage img)
{
    pc_buf exif = sample_exif();
    set_blob(&img.meta.exif, exif.data, exif.len);
    set_blob(&img.meta.xmp, "x", 1);
    img.meta.has_dpi = true;
    img.meta.dpi_x = 72.0f;
    img.meta.dpi_y = 72.0f;
    CHECK(pc_cmeta_add_text(&img.meta, "a", 1, "b", 1, NULL) == PC_OK);
    pc_buf_fini(&exif);
    return img;
}

static void test_exif_xmp_dpi_text_dropped_for_qoi(void)
{
    static const pc_cfidelity_kind K[] = {PC_FW_EXIF_DROPPED, PC_FW_XMP_DROPPED, PC_FW_DPI_DROPPED,
                                          PC_FW_TEXT_DROPPED};
    pc_cimage img = with_meta(test_image(PC_CL_RGB, PC_CS_U8));
    warns_t w = warns(&img, PC_CF_QOI);
    size_t i;
    for (i = 0; i < 4; i++) {
        if (!contains_kind(&w, K[i])) {
            PC_T_FAIL("missing kind %d", (int)K[i]);
        }
        pc_t_checks++;
    }
    pc_cimage_fini(&img);
}

static void test_webp_drops_dpi_and_text_only(void)
{
    pc_cimage img = with_meta(test_image(PC_CL_RGB, PC_CS_U8));
    warns_t w = warns(&img, PC_CF_WEBP);
    CHECK_EQ_UINT(w.n, 2);
    CHECK(w.n == 2 && w.w[0].kind == PC_FW_DPI_DROPPED && w.w[1].kind == PC_FW_TEXT_DROPPED);
    pc_cimage_fini(&img);
}

static void test_float_to_png_reduces_and_clips(void)
{
    float vals[12];
    pc_cimage img;
    warns_t w;
    int i;
    for (i = 0; i < 12; i++) {
        vals[i] = (float)i * 0.3f - 0.5f;
    }
    if (pc_cimage_from_copy(2, 2, PC_CL_RGB, PC_CS_F32, vals, sizeof vals, &img, NULL) != PC_OK) {
        PC_T_FAIL("from_copy");
        return;
    }
    w = warns(&img, PC_CF_PNG);
    CHECK(contains(&w, depth_reduced(PC_CS_F32, PC_CS_U16)));
    CHECK(contains_kind(&w, PC_FW_RANGE_CLIPPED));
    pc_cimage_fini(&img);
}

static void test_in_range_float_not_clipped(void)
{
    pc_cimage img = test_image(PC_CL_RGB, PC_CS_F32);
    warns_t w = warns(&img, PC_CF_PNG);
    CHECK(!contains_kind(&w, PC_FW_RANGE_CLIPPED));
    pc_cimage_fini(&img);
}

static void test_float_to_exr_and_tiff_is_clean(void)
{
    static const pc_cformat FS[] = {PC_CF_EXR, PC_CF_TIFF, PC_CF_PNM};
    size_t i;
    for (i = 0; i < 3; i++) {
        pc_cimage img = test_image(PC_CL_RGB, PC_CS_F32);
        warns_t w = warns(&img, FS[i]);
        pc_t_name = pc_cformat_debug(FS[i]);
        if (FS[i] == PC_CF_EXR && !cx_exr_available()) {
            CHECK(exr_unsupported_only(&w));
        } else {
            CHECK_EQ_UINT(w.n, 0);
        }
        pc_cimage_fini(&img);
    }
}

static void test_f16_to_exr_is_clean(void)
{
    pc_cimage img = test_image(PC_CL_RGBA, PC_CS_F16);
    warns_t w = warns(&img, PC_CF_EXR);
    if (cx_exr_available()) {
        CHECK_EQ_UINT(w.n, 0);
    } else {
        CHECK(exr_unsupported_only(&w));
    }
    pc_cimage_fini(&img);
}

static void test_integer_to_exr_is_lossless(void)
{
    static const pc_csample S[] = {PC_CS_U8, PC_CS_U16};
    size_t i;
    for (i = 0; i < 2; i++) {
        pc_cimage img = test_image(PC_CL_RGB, S[i]);
        warns_t w = warns(&img, PC_CF_EXR);
        pc_t_name = pc_csample_name(S[i]);
        if (cx_exr_available()) {
            CHECK_EQ_UINT(w.n, 0);
        } else {
            CHECK(exr_unsupported_only(&w));
        }
        pc_cimage_fini(&img);
    }
}

static void test_float_pnm_drops_alpha(void)
{
    pc_cimage img = test_image(PC_CL_RGBA, PC_CS_F32);
    warns_t w = warns(&img, PC_CF_PNM);
    CHECK(contains_kind(&w, PC_FW_ALPHA_DISCARDED));
    pc_cimage_fini(&img);
}

static void test_gif_quantizes_and_binarizes(void)
{
    pc_cimage img = test_image(PC_CL_RGBA, PC_CS_U8);
    warns_t w = warns(&img, PC_CF_GIF);
    CHECK(contains_kind(&w, PC_FW_PALETTE_QUANTIZED));
    CHECK(contains_kind(&w, PC_FW_ALPHA_BINARIZED));
    CHECK(!contains_kind(&w, PC_FW_LOSSY_COMPRESSION));
    pc_cimage_fini(&img);
}

static void test_hdr_is_lossy(void)
{
    pc_cimage img = test_image(PC_CL_RGB, PC_CS_F32);
    warns_t w = warns(&img, PC_CF_HDR);
    CHECK(contains_kind(&w, PC_FW_LOSSY_COMPRESSION));
    pc_cimage_fini(&img);
}

static void test_ico_oversize_is_fatal(void)
{
    pc_cimage img;
    warns_t w;
    pc_cfidelity d = W(PC_FW_DIMENSIONS_EXCEEDED);
    bool fatal = false;
    size_t i;
    if (pc_cimage_new(512, 16, PC_CL_RGBA, PC_CS_U8, &img, NULL) != PC_OK) {
        PC_T_FAIL("new");
        return;
    }
    w = warns(&img, PC_CF_ICO);
    for (i = 0; i < w.n; i++) {
        fatal = fatal || pc_cfidelity_is_fatal(&w.w[i]);
    }
    CHECK(fatal);
    d.max_width = 256;
    d.max_height = 256;
    CHECK(contains(&w, d));
    pc_cimage_fini(&img);
}

static void test_avif_default_build_write_unsupported(void)
{
    /* This build has no AVIF encoder (the reference's default, no "avif" feature). */
    pc_cimage img = test_image(PC_CL_RGB, PC_CS_U8);
    warns_t w = warns(&img, PC_CF_AVIF);
    pc_cfidelity u = W(PC_FW_WRITE_UNSUPPORTED);
    u.format = PC_CF_AVIF;
    CHECK_EQ_UINT(w.n, 1);
    CHECK(w.n == 1 && pc_cfidelity_eq(&w.w[0], &u));
    CHECK(w.n >= 1 && pc_cfidelity_is_fatal(&w.w[0]));
    pc_cimage_fini(&img);
}

static void test_every_warning_has_a_message(void)
{
    pc_cfidelity all[15];
    size_t i;
    for (i = 0; i < 15; i++) {
        all[i] = W(PC_FW_WRITE_UNSUPPORTED);
    }
    all[0].format = PC_CF_AVIF;
    all[1] = depth_reduced(PC_CS_F32, PC_CS_F16);
    all[2] = W(PC_FW_RANGE_CLIPPED);
    all[3] = W(PC_FW_ALPHA_DISCARDED);
    all[4] = W(PC_FW_ALPHA_BINARIZED);
    all[5] = cmyk_converted(PC_CL_RGB);
    all[6] = W(PC_FW_COLOR_TO_GRAY);
    all[7] = W(PC_FW_PALETTE_QUANTIZED);
    all[8] = W(PC_FW_LOSSY_COMPRESSION);
    all[9] = W(PC_FW_ICC_DROPPED);
    all[10] = W(PC_FW_EXIF_DROPPED);
    all[11] = W(PC_FW_XMP_DROPPED);
    all[12] = W(PC_FW_DPI_DROPPED);
    all[13] = W(PC_FW_TEXT_DROPPED);
    all[14] = W(PC_FW_DIMENSIONS_EXCEEDED);
    all[14].max_width = 1;
    all[14].max_height = 1;
    for (i = 0; i < 15; i++) {
        char t[256];
        t[0] = '\0';
        pc_cfidelity_text(&all[i], t, sizeof t);
        if (strlen(t) <= 5) {
            PC_T_FAIL("warning %zu: \"%s\"", i, t);
        }
        pc_t_checks++;
    }
}

/* The warnings must agree with what actually comes back from the file. */
static void test_warnings_match_actual_roundtrip_behaviour(void)
{
    int f;
    int l;
    int s;
    for (f = 0; f < PC_CF_COUNT; f++) {
        if (!rw((pc_cformat)f)) {
            continue;
        }
        for (l = 0; l < PC_CL_COUNT; l++) {
            for (s = 0; s < PC_CS_COUNT; s++) {
                pc_cimage img = test_image((pc_clayout)l, (pc_csample)s);
                warns_t w = warns(&img, (pc_cformat)f);
                pc_buf b = enc(&img, (pc_cformat)f, NULL);
                pc_cimage back = dec(b.data, b.len);
                bool depth_reduced_w = any_kind(&w, PC_FW_DEPTH_REDUCED);
                bool lost_depth = back.sample != (pc_csample)s && back.sample != PC_CS_F32 && s != PC_CS_U8;
                bool alpha_lost = pc_clayout_has_alpha((pc_clayout)l) && !pc_clayout_has_alpha(back.layout);
                bool cmyk_lost = pc_clayout_is_cmyk((pc_clayout)l) && !pc_clayout_is_cmyk(back.layout);
                char name[96];
                (void)snprintf(name, sizeof name, "%s %s %s", pc_cformat_debug((pc_cformat)f),
                               pc_clayout_name((pc_clayout)l), pc_csample_name((pc_csample)s));
                pc_t_name = name;
                if (depth_reduced_w != lost_depth) {
                    PC_T_FAIL("depth_reduced=%d but decoded as %s", depth_reduced_w, pc_csample_name(back.sample));
                }
                pc_t_checks++;
                CHECK(contains_kind(&w, PC_FW_ALPHA_DISCARDED) == alpha_lost);
                CHECK(any_kind(&w, PC_FW_CMYK_CONVERTED) == cmyk_lost);
                pc_t_name = "test_warnings_match_actual_roundtrip_behaviour";
                pc_cimage_fini(&back);
                pc_buf_fini(&b);
                pc_cimage_fini(&img);
            }
        }
    }
}

int main(int argc, char **argv)
{
    RUN(test_png_u8_rgb_is_clean);
    RUN(test_png_u16_rgba_is_clean);
    RUN(test_tiff_is_clean_for_all_integer_and_f32_layouts);
    RUN(test_sixteen_bit_to_jpeg_reduces);
    RUN(test_sixteen_bit_message_text);
    RUN(test_alpha_to_jpeg_discarded);
    RUN(test_opaque_alpha_not_reported);
    RUN(test_alpha_to_hdr_discarded);
    RUN(test_cmyk_to_png_converted);
    RUN(test_cmyka_to_webp_converted_keeps_alpha);
    RUN(test_cmyk_to_jpeg_and_tiff_not_converted);
    RUN(test_icc_dropped_for_bmp);
    RUN(test_icc_warning_suppressed_when_not_embedding);
    RUN(test_exif_xmp_dpi_text_dropped_for_qoi);
    RUN(test_webp_drops_dpi_and_text_only);
    RUN(test_float_to_png_reduces_and_clips);
    RUN(test_in_range_float_not_clipped);
    RUN(test_float_to_exr_and_tiff_is_clean);
    RUN(test_f16_to_exr_is_clean);
    RUN(test_integer_to_exr_is_lossless);
    RUN(test_float_pnm_drops_alpha);
    RUN(test_gif_quantizes_and_binarizes);
    RUN(test_hdr_is_lossy);
    RUN(test_ico_oversize_is_fatal);
    RUN(test_avif_default_build_write_unsupported);
    RUN(test_every_warning_has_a_message);
    RUN(test_warnings_match_actual_roundtrip_behaviour);
    return pc_t_finish();
}
