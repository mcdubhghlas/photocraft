/* The codecs' flat image: port of the unit tests in crates/codecs/src/image.rs. */
#include "codecs/internal.h"
#include "common.h"

static void test_adding_alpha_fast_path_matches_the_general_conversion(void)
{
    static const pc_clayout layouts[] = {PC_CL_GRAY, PC_CL_RGB, PC_CL_CMYK};
    int s;
    size_t k;
    for (s = 0; s < PC_CS_COUNT; s++) {
        for (k = 0; k < 3; k++) {
            pc_clayout l = layouts[k];
            pc_clayout la = pc_clayout_with_alpha(l);
            size_t n = 7 * 3 * pc_clayout_channels(l);
            size_t na = 7 * 3 * pc_clayout_channels(la);
            float *vals = malloc(n * sizeof *vals);
            float *norm = NULL;
            float *conv = malloc(na * sizeof *conv);
            uint8_t *slow = malloc(na * pc_csample_bytes((pc_csample)s));
            size_t nn = 0;
            size_t i;
            pc_cimage img;
            pc_cimage fast;
            for (i = 0; i < n; i++) {
                vals[i] = fmodf((float)i * 0.137f, 1.0f);
            }
            CHECK_EQ_INT(pc_cimage_from_normalized(7, 3, l, (pc_csample)s, vals, n, &img, NULL), PC_OK);
            CHECK_EQ_INT(pc_cimage_convert(&img, la, (pc_csample)s, &fast, NULL), PC_OK);
            CHECK_EQ_INT(pc_cimage_to_normalized(&img, &norm, &nn, NULL), PC_OK);
            cx_convert_layout(norm, 7 * 3, l, la, conv);
            cx_quantize(conv, na, (pc_csample)s, slow);
            pc_t_checks++;
            if (fast.len != na * pc_csample_bytes((pc_csample)s) || memcmp(fast.data, slow, fast.len) != 0) {
                PC_T_FAIL("%s %s", pc_clayout_name(l), pc_csample_name((pc_csample)s));
            }
            pc_cimage_fini(&fast);
            pc_cimage_fini(&img);
            free(slow);
            free(conv);
            free(norm);
            free(vals);
        }
    }
}

static void test_u8_u16_roundtrip_exact(void)
{
    uint8_t v[256];
    pc_cimage img;
    pc_cimage a;
    pc_cimage back;
    int i;
    for (i = 0; i < 256; i++) {
        v[i] = (uint8_t)i;
    }
    CHECK_EQ_INT(pc_cimage_from_copy(256, 1, PC_CL_GRAY, PC_CS_U8, v, 256, &img, NULL), PC_OK);
    CHECK_EQ_INT(pc_cimage_convert(&img, PC_CL_GRAY, PC_CS_U16, &a, NULL), PC_OK);
    CHECK_EQ_INT(pc_cimage_convert(&a, PC_CL_GRAY, PC_CS_U8, &back, NULL), PC_OK);
    CHECK(back.len == 256 && memcmp(back.data, v, 256) == 0);
    pc_cimage_fini(&back);
    pc_cimage_fini(&a);
    pc_cimage_fini(&img);
}

static void test_u16_f32_roundtrip_exact(void)
{
    size_t n = 65535 / 7 + 1;
    uint16_t *v = malloc(n * sizeof *v);
    pc_cimage img;
    pc_cimage a;
    pc_cimage back;
    size_t i;
    size_t bad = 0;
    for (i = 0; i < n; i++) {
        v[i] = (uint16_t)(i * 7);
    }
    CHECK_EQ_INT(pc_cimage_from_copy((uint32_t)n, 1, PC_CL_GRAY, PC_CS_U16, v, n * 2, &img, NULL), PC_OK);
    CHECK_EQ_INT(pc_cimage_convert(&img, PC_CL_GRAY, PC_CS_F32, &a, NULL), PC_OK);
    CHECK_EQ_INT(pc_cimage_convert(&a, PC_CL_GRAY, PC_CS_U16, &back, NULL), PC_OK);
    CHECK_EQ_UINT(back.len, n * 2);
    for (i = 0; i < n && back.len == n * 2; i++) {
        bad += ne16(back.data, i) != v[i];
    }
    CHECK_EQ_UINT(bad, 0);
    pc_cimage_fini(&back);
    pc_cimage_fini(&a);
    pc_cimage_fini(&img);
    free(v);
}

static void test_icc_dropped_on_model_change(void)
{
    static const uint8_t icc[3] = {1, 2, 3};
    pc_cimage img;
    pc_cimage c;
    CHECK_EQ_INT(pc_cimage_new(2, 2, PC_CL_CMYK, PC_CS_U8, &img, NULL), PC_OK);
    set_blob(&img.icc, icc, 3);
    CHECK_EQ_INT(pc_cimage_convert(&img, PC_CL_RGB, PC_CS_U8, &c, NULL), PC_OK);
    CHECK(!c.icc.has);
    pc_cimage_fini(&c);
    CHECK_EQ_INT(pc_cimage_convert(&img, PC_CL_CMYKA, PC_CS_U16, &c, NULL), PC_OK);
    CHECK(c.icc.has);
    pc_cimage_fini(&c);
    pc_cimage_fini(&img);
}

static void test_from_raw_rejects_bad_len(void)
{
    uint8_t *d = calloc(11, 1);
    pc_cimage img;
    pc_error e;
    CHECK_EQ_INT(pc_cimage_from_raw(2, 2, PC_CL_RGB, PC_CS_U8, d, 11, &img, &e), PC_ERR_INVALID);
    CHECK_CONTAINS(e.msg, "buffer has 11 bytes, expected 12");
}

static void test_rgba8_of_gray(void)
{
    static const uint8_t px[2] = {10, 20};
    static const uint8_t want[4] = {10, 10, 10, 20};
    pc_cimage img;
    uint8_t *out = NULL;
    size_t n = 0;
    CHECK_EQ_INT(pc_cimage_from_copy(1, 1, PC_CL_GRAYA, PC_CS_U8, px, 2, &img, NULL), PC_OK);
    CHECK_EQ_INT(pc_cimage_to_rgba8(&img, &out, &n, NULL), PC_OK);
    CHECK(n == 4 && memcmp(out, want, 4) == 0);
    free(out);
    pc_cimage_fini(&img);
}

static void test_cmyk_rgb_basic(void)
{
    static const uint8_t k[4] = {0, 0, 0, 255};
    static const uint8_t black[4] = {0, 0, 0, 255};
    static const uint8_t red[3] = {255, 0, 0};
    static const uint8_t cmyk_red[4] = {0, 255, 255, 0};
    pc_cimage img;
    pc_cimage c;
    uint8_t *out = NULL;
    size_t n = 0;
    CHECK_EQ_INT(pc_cimage_from_copy(1, 1, PC_CL_CMYK, PC_CS_U8, k, 4, &img, NULL), PC_OK);
    CHECK_EQ_INT(pc_cimage_to_rgba8(&img, &out, &n, NULL), PC_OK);
    CHECK(n == 4 && memcmp(out, black, 4) == 0);
    free(out);
    pc_cimage_fini(&img);
    CHECK_EQ_INT(pc_cimage_from_copy(1, 1, PC_CL_RGB, PC_CS_U8, red, 3, &img, NULL), PC_OK);
    CHECK_EQ_INT(pc_cimage_convert(&img, PC_CL_CMYK, PC_CS_U8, &c, NULL), PC_OK);
    CHECK(c.len == 4 && memcmp(c.data, cmyk_red, 4) == 0);
    pc_cimage_fini(&c);
    pc_cimage_fini(&img);
}

int main(int argc, char **argv)
{
    RUN(test_adding_alpha_fast_path_matches_the_general_conversion);
    RUN(test_u8_u16_roundtrip_exact);
    RUN(test_u16_f32_roundtrip_exact);
    RUN(test_icc_dropped_on_model_change);
    RUN(test_from_raw_rejects_bad_len);
    RUN(test_rgba8_of_gray);
    RUN(test_cmyk_rgb_basic);
    return pc_t_finish();
}
