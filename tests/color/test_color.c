/* Ports the unit tests of crates/color/src/{lib,convert,blend}.rs, plus edge cases for the C
 * API (bounds-checked samples, CMYK space scopes and cache, names). */
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "cms/builtin.h"
#include "cms/profile.h"
#include "cms/synth.h"
#include "cms/transform.h"
#include "color/blend.h"
#include "color/color.h"
#include "color/convert.h"
#include "harness.h"

#define E 1e-5

/* ---- lib.rs ---- */

static void test_pixel_format_sizes(void)
{
    CHECK_EQ_UINT(pc_pixel_format_bytes_per_pixel(PC_PIXEL_FORMAT_RGBA8), 4);
    CHECK_EQ_UINT(pc_pixel_format_bytes_per_pixel(PC_PIXEL_FORMAT_RGBA16), 8);
    CHECK_EQ_UINT(pc_pixel_format_bytes_per_pixel(PC_PIXEL_FORMAT_RGBA32F), 16);
    CHECK_EQ_UINT(pc_pixel_format_bytes_per_pixel(PC_PIXEL_FORMAT_GRAY8), 1);
    CHECK_EQ_UINT(pc_pixel_format_channels(PC_PIXEL_FORMAT_CMYKA8), 5);
    CHECK_EQ_UINT(pc_sample_type_bits(PC_SAMPLE_TYPE_U16), 16);
    CHECK_EQ_UINT(pc_color_mode_channels(PC_COLOR_MODE_MULTICHANNEL), 3);
    CHECK(pc_pixel_format_eq(PC_PIXEL_FORMAT_GRAYA8, PC_PIXEL_FORMAT_GRAYA8));
    CHECK(!pc_pixel_format_eq(PC_PIXEL_FORMAT_GRAYA8, PC_PIXEL_FORMAT_GRAY8));
}

static void test_sample_roundtrip_all_depths(void)
{
    static const float vs[4] = {0.0f, 0.25f, 0.5f, 1.0f};
    unsigned s;
    for (s = 0; s < PC_SAMPLE_TYPE_COUNT; s++) {
        uint8_t buf[16] = {0};
        float tol = s == PC_SAMPLE_TYPE_U8 ? 1.0f / 255.0f : s == PC_SAMPLE_TYPE_U16 ? 1.0f / 65535.0f : 0.0f;
        size_t i;
        for (i = 0; i < 4; i++) {
            float r;
            pc_write_sample(buf, sizeof buf, (pc_sample_type)s, i, vs[i]);
            r = pc_read_sample(buf, sizeof buf, (pc_sample_type)s, i);
            CHECK(fabsf(r - vs[i]) <= tol);
        }
    }
}

static void test_integer_samples_clamp(void)
{
    uint8_t b[2] = {7, 7};
    pc_write_sample(b, 2, PC_SAMPLE_TYPE_U8, 0, 1.7f);
    pc_write_sample(b, 2, PC_SAMPLE_TYPE_U8, 1, -0.3f);
    CHECK(b[0] == 255 && b[1] == 0);
    pc_write_sample(b, 2, PC_SAMPLE_TYPE_U8, 0, NAN); /* Rust `as u8` maps NaN to 0 */
    CHECK_EQ_UINT(b[0], 0);
}

static void test_float_samples_keep_hdr(void)
{
    uint8_t b[4];
    pc_write_sample(b, 4, PC_SAMPLE_TYPE_F32, 0, 4.5f);
    CHECK(pc_read_sample(b, 4, PC_SAMPLE_TYPE_F32, 0) == 4.5f);
}

/* The reference panics on an out-of-range index; C reads 0 and writes nothing. */
static void test_samples_out_of_range_are_harmless(void)
{
    uint8_t b[5] = {1, 2, 3, 4, 5};
    static const uint8_t same[5] = {1, 2, 3, 4, 5};
    CHECK(pc_read_sample(b, 5, PC_SAMPLE_TYPE_F32, 1) == 0.0f); /* bytes 4..8 */
    CHECK(pc_read_sample(b, 5, PC_SAMPLE_TYPE_U16, 2) == 0.0f);
    CHECK(pc_read_sample(b, 5, PC_SAMPLE_TYPE_U8, SIZE_MAX) == 0.0f);
    CHECK(pc_read_sample(b, 5, PC_SAMPLE_TYPE_F32, SIZE_MAX / 2) == 0.0f); /* index * 4 overflows */
    CHECK(pc_read_sample(NULL, 0, PC_SAMPLE_TYPE_U8, 0) == 0.0f);
    pc_write_sample(b, 5, PC_SAMPLE_TYPE_F32, 1, 1.0f);
    pc_write_sample(b, 5, PC_SAMPLE_TYPE_U16, 2, 1.0f);
    pc_write_sample(b, 5, PC_SAMPLE_TYPE_U8, 5, 1.0f);
    pc_write_sample(NULL, 5, PC_SAMPLE_TYPE_U8, 0, 1.0f);
    CHECK_EQ_MEM(b, same, 5);
    CHECK_NEAR(pc_read_sample(b, 5, PC_SAMPLE_TYPE_U8, 4), 5.0 / 255.0, 1e-7);
}

static void test_color_to_rgba8(void)
{
    pc_color w = PC_COLOR_WHITE;
    pc_color g = pc_color_gray(0.5f);
    pc_color cw = {PC_COLOR_MODE_CMYK, {0, 0, 0, 0}, 1.0f};
    pc_color ck = {PC_COLOR_MODE_CMYK, {0.75f, 0.68f, 0.67f, 0.9f}, 1.0f};
    pc_color lab = {PC_COLOR_MODE_LAB, {1.0f, 128.0f / 255.0f, 128.0f / 255.0f, 0.0f}, 0.5f};
    uint8_t q[4];
    pc_color_to_rgba8(&w, q);
    CHECK(q[0] == 255 && q[1] == 255 && q[2] == 255 && q[3] == 255);
    pc_color_to_rgba8(&g, q);
    CHECK(q[0] == 128 && q[1] == 128 && q[2] == 128 && q[3] == 255);
    pc_color_to_rgba8(&cw, q);
    CHECK(q[0] == 255 && q[1] == 255 && q[2] == 255 && q[3] == 255);
    pc_color_to_rgba8(&ck, q);
    CHECK(q[0] < 20 && q[1] < 20 && q[2] < 20);
    pc_color_to_rgba8(&lab, q); /* L 100, a = b = 0: white */
    CHECK(q[0] >= 254 && q[1] >= 254 && q[2] >= 254 && q[3] == 128);
}

static void test_names(void)
{
    pc_color_mode m = PC_COLOR_MODE_RGB;
    pc_sample_type s = PC_SAMPLE_TYPE_U8;
    pc_blend_mode b = PC_BLEND_NORMAL;
    CHECK(pc_color_mode_from_name("Multichannel", 12, &m) && m == PC_COLOR_MODE_MULTICHANNEL);
    CHECK(!pc_color_mode_from_name("rgb", 3, &m));
    CHECK(pc_sample_type_from_name("F32", 3, &s) && s == PC_SAMPLE_TYPE_F32);
    CHECK(!pc_sample_type_from_name("F3", 2, &s));
    CHECK_EQ_STR(pc_color_mode_name(PC_COLOR_MODE_CMYK), "Cmyk");
    CHECK(pc_blend_from_name("LinearDodge", 11, &b) && b == PC_BLEND_LINEAR_DODGE);
    CHECK_EQ_STR(pc_blend_label(PC_BLEND_LINEAR_DODGE), "Linear Dodge (Add)");
    CHECK_EQ_STR(pc_blend_name(PC_BLEND_PASS_THROUGH), "PassThrough");
    CHECK(!pc_blend_from_name(NULL, 0, &b));
}

/* ---- convert.rs ---- */

static void test_srgb_roundtrip(void)
{
    int i;
    for (i = 0; i <= 255; i++) {
        float v = (float)i / 255.0f;
        CHECK(fabsf(pc_linear_to_srgb(pc_srgb_to_linear(v)) - v) < 1e-5f);
    }
}

static void test_cmyk_roundtrip(void)
{
    static const float rgbs[4][3] = {{1, 0, 0}, {0.2f, 0.4f, 0.6f}, {0, 0, 0}, {1, 1, 1}};
    int i;
    int k;
    for (i = 0; i < 4; i++) {
        float c[4];
        float back[3];
        pc_rgb_to_cmyk_naive(rgbs[i], c);
        pc_cmyk_to_rgb_naive(c, back);
        for (k = 0; k < 3; k++) {
            CHECK(fabsf(back[k] - rgbs[i][k]) < 1e-5f);
        }
    }
}

static void test_managed_cmyk(void)
{
    static const float zero[4] = {0, 0, 0, 0};
    static const float k100[4] = {0, 0, 0, 1};
    static const float rgbs[3][3] = {{0.5f, 0.5f, 0.5f}, {0.6f, 0.4f, 0.3f}, {0.3f, 0.5f, 0.6f}};
    float w[3];
    float k[3];
    int i;
    int c;
    pc_cmyk_to_rgb(zero, w);
    CHECK(w[0] > 0.99f && w[1] > 0.99f && w[2] > 0.99f);
    /* 100 % K alone is a dark neutral, not pure black. */
    pc_cmyk_to_rgb(k100, k);
    CHECK(k[0] < 0.3f && fabsf(k[0] - k[2]) < 0.05f);
    for (i = 0; i < 3; i++) {
        float cm[4];
        float back[3];
        pc_rgb_to_cmyk(rgbs[i], cm);
        pc_cmyk_to_rgb(cm, back);
        for (c = 0; c < 3; c++) {
            CHECK(fabsf(back[c] - rgbs[i][c]) < 0.03f);
        }
    }
}

static bool uncoated_bytes(pc_profile *p, pc_buf *bytes)
{
    pc_cmyk_params params = pc_cmyk_params_default();
    params.description = "Test Uncoated";
    params.tvi[0] = params.tvi[1] = params.tvi[2] = 0.26;
    params.tvi[3] = 0.3;
    params.grid_a2b = 5;
    params.grid_b2a = 9;
    if (pc_synth_cmyk_profile(&params, p, NULL) != PC_OK) {
        return false;
    }
    if (pc_profile_to_bytes(p, bytes, NULL) != PC_OK) {
        pc_profile_fini(p);
        return false;
    }
    return true;
}

static void test_document_cmyk_space_scope(void)
{
    static const float ink[4] = {0.0f, 0.5f, 0.5f, 0.0f};
    static const float rgb[3] = {0.6f, 0.4f, 0.3f};
    static const uint8_t junk[3] = {1, 2, 3};
    pc_profile p;
    pc_buf bytes = PC_BUF_INIT;
    pc_buf other = PC_BUF_INIT;
    pc_cmyk_space *space;
    pc_cmyk_space *again;
    pc_cmyk_space *prev;
    pc_cmyk_space *inner;
    pc_transform *t = NULL;
    float def[3];
    float expect[3];
    float inside[3];
    float after[3];
    float cm[4];
    float back[3];
    bool differs = false;
    int i;
    if (!uncoated_bytes(&p, &bytes)) {
        PC_T_FAIL("%s", "synth");
        return;
    }
    space = pc_cmyk_space_for_profile(bytes.data, bytes.len);
    CHECK(space != NULL);
    /* Cached: equal bytes give the same object. */
    again = pc_cmyk_space_for_profile(bytes.data, bytes.len);
    CHECK(again == space);
    pc_cmyk_space_release(again);
    /* The built-in profile, non-CMYK, junk or absent bytes keep the default path. */
    CHECK_EQ_INT(pc_profile_to_bytes(pc_builtin_profile(PC_BUILTIN_COATED_CMYK), &other, NULL), PC_OK);
    CHECK(pc_cmyk_space_for_profile(other.data, other.len) == NULL);
    pc_buf_truncate(&other, 0);
    CHECK_EQ_INT(pc_profile_to_bytes(pc_builtin_profile(PC_BUILTIN_SRGB), &other, NULL), PC_OK);
    CHECK(pc_cmyk_space_for_profile(other.data, other.len) == NULL);
    CHECK(pc_cmyk_space_for_profile(junk, sizeof junk) == NULL);
    CHECK(pc_cmyk_space_for_profile(NULL, 0) == NULL);
    CHECK(pc_cmyk_space_id(space) != 0 && pc_cmyk_space_id(NULL) == 0);

    pc_cmyk_to_rgb(ink, def);
    CHECK_EQ_INT(pc_transform_new(&p, pc_builtin_profile(PC_BUILTIN_SRGB), PC_INTENT_RELATIVE, true, &t, NULL), PC_OK);
    if (t != NULL) {
        pc_transform_eval(t, ink, expect);
    }
    prev = pc_cmyk_space_enter(space);
    CHECK(prev == NULL && pc_cmyk_space_active() == space);
    pc_cmyk_to_rgb(ink, inside);
    pc_cmyk_space_leave(prev);
    for (i = 0; i < 3; i++) {
        CHECK(fabsf(inside[i] - expect[i]) < 0.01f);
        differs = differs || fabsf(inside[i] - def[i]) > 0.02f;
    }
    CHECK(differs); /* the document profile changes the colour */
    /* Restored afterwards, and nested scopes restore the outer one. */
    CHECK(pc_cmyk_space_active() == NULL);
    pc_cmyk_to_rgb(ink, after);
    CHECK_EQ_MEM(after, def, sizeof def);
    prev = pc_cmyk_space_enter(space);
    inner = pc_cmyk_space_enter(NULL);
    CHECK(pc_cmyk_space_active() == NULL);
    pc_cmyk_space_leave(inner);
    CHECK(pc_cmyk_space_active() == space);
    pc_rgb_to_cmyk(rgb, cm);
    pc_cmyk_to_rgb(cm, back);
    CHECK(fabsf(back[0] - 0.6f) < 0.04f && fabsf(back[2] - 0.3f) < 0.04f);
    pc_cmyk_space_leave(prev);
    CHECK(pc_cmyk_space_active() == NULL);
    /* The scope held its own reference: releasing ours while inside is safe. */
    prev = pc_cmyk_space_enter(space);
    pc_cmyk_space_release(space); /* the cache still holds one too */
    pc_cmyk_to_rgb(ink, inside);
    pc_cmyk_space_leave(prev);
    pc_transform_release(t);
    pc_profile_fini(&p);
    pc_buf_fini(&bytes);
    pc_buf_fini(&other);
}

typedef struct thread_arg {
    const pc_buf *bytes;
    pc_cmyk_space *got;
    float rgb[3];
} thread_arg;

static void *worker(void *v)
{
    static const float ink[4] = {0.0f, 0.5f, 0.5f, 0.0f};
    thread_arg *a = v;
    pc_cmyk_space *prev;
    a->got = pc_cmyk_space_for_profile(a->bytes->data, a->bytes->len);
    prev = pc_cmyk_space_enter(a->got);
    pc_cmyk_to_rgb(ink, a->rgb);
    pc_cmyk_space_leave(prev);
    return NULL;
}

/* The active space is per thread, and concurrent lookups share one cached object. */
static void test_spaces_are_per_thread(void)
{
    static const float ink[4] = {0.0f, 0.5f, 0.5f, 0.0f};
    pc_profile p;
    pc_buf bytes = PC_BUF_INIT;
    thread_arg args[4];
    pthread_t th[4];
    bool started[4] = {false, false, false, false};
    float def[3];
    int i;
    if (!uncoated_bytes(&p, &bytes)) {
        PC_T_FAIL("%s", "synth");
        return;
    }
    for (i = 0; i < 4; i++) {
        args[i].bytes = &bytes;
        args[i].got = NULL;
        started[i] = pthread_create(&th[i], NULL, worker, &args[i]) == 0;
    }
    pc_cmyk_to_rgb(ink, def); /* this thread stays on the built-in profile */
    for (i = 0; i < 4; i++) {
        if (started[i]) {
            (void)pthread_join(th[i], NULL);
        }
    }
    for (i = 0; i < 4; i++) {
        CHECK(started[i] && args[i].got != NULL && args[i].got == args[0].got);
        CHECK_EQ_MEM(args[i].rgb, args[0].rgb, sizeof args[0].rgb);
        CHECK(fabsf(args[i].rgb[1] - def[1]) > 0.02f || fabsf(args[i].rgb[0] - def[0]) > 0.02f);
        pc_cmyk_space_release(args[i].got);
    }
    CHECK(pc_cmyk_space_active() == NULL);
    pc_profile_fini(&p);
    pc_buf_fini(&bytes);
}

static void test_lab_roundtrip_and_anchors(void)
{
    static const float white_rgb[3] = {1, 1, 1};
    static const float black_rgb[3] = {0, 0, 0};
    static const float rgbs[3][3] = {{0.8f, 0.3f, 0.1f}, {0.1f, 0.5f, 0.9f}, {0.5f, 0.5f, 0.5f}};
    float w[3];
    float b[3];
    int i;
    int k;
    pc_srgb_to_lab(white_rgb, w);
    CHECK(fabsf(w[0] - 100.0f) < 0.1f && fabsf(w[1]) < 0.5f && fabsf(w[2]) < 0.5f);
    pc_srgb_to_lab(black_rgb, b);
    CHECK(fabsf(b[0]) < 0.1f);
    for (i = 0; i < 3; i++) {
        float lab[3];
        float back[3];
        pc_srgb_to_lab(rgbs[i], lab);
        pc_lab_to_srgb(lab, back);
        for (k = 0; k < 3; k++) {
            CHECK(fabsf(back[k] - rgbs[i][k]) < 2e-3f);
        }
    }
}

/* ---- blend.rs ---- */

static bool close(float a, float b)
{
    return fabsf(a - b) < (float)E;
}

static void test_psd_keys_roundtrip_and_unique(void)
{
    unsigned i;
    unsigned j;
    pc_blend_mode m = PC_BLEND_NORMAL;
    for (i = 0; i < PC_BLEND_PSD_COUNT; i++) {
        for (j = 0; j < i; j++) {
            CHECK(memcmp(pc_blend_psd_key((pc_blend_mode)i), pc_blend_psd_key((pc_blend_mode)j), 4) != 0);
        }
        CHECK(pc_blend_from_psd_key(pc_blend_psd_key((pc_blend_mode)i), &m) && m == (pc_blend_mode)i);
    }
    CHECK(!pc_blend_from_psd_key("zzzz", &m));
    CHECK(PC_BLEND_LAYER_MODES[0] == PC_BLEND_NORMAL && PC_BLEND_LAYER_MODES[26] == PC_BLEND_LUMINOSITY);
    for (i = 0; i < PC_BLEND_LAYER_MODE_COUNT; i++) {
        CHECK(PC_BLEND_LAYER_MODES[i] == (pc_blend_mode)(i + 1)); /* menu order = enum order */
    }
}

static void test_normal_is_source(void)
{
    CHECK(close(pc_blend_channel(PC_BLEND_NORMAL, 0.2f, 0.7f), 0.7f));
    CHECK(close(pc_blend_channel(PC_BLEND_NORMAL, 1.0f, 0.0f), 0.0f));
    CHECK(close(pc_blend_channel(PC_BLEND_NORMAL, 0.0f, 1.0f), 1.0f));
}

static void test_identities_with_white_and_black(void)
{
    static const float cbs[5] = {0.0f, 0.1f, 0.5f, 0.9f, 1.0f};
    int i;
    for (i = 0; i < 5; i++) {
        float cb = cbs[i];
        CHECK(close(pc_blend_channel(PC_BLEND_MULTIPLY, cb, 1.0f), cb));
        CHECK(close(pc_blend_channel(PC_BLEND_SCREEN, cb, 0.0f), cb));
        CHECK(close(pc_blend_channel(PC_BLEND_LINEAR_DODGE, cb, 0.0f), cb));
        CHECK(close(pc_blend_channel(PC_BLEND_DARKEN, cb, 1.0f), cb));
        CHECK(close(pc_blend_channel(PC_BLEND_LIGHTEN, cb, 0.0f), cb));
        CHECK(close(pc_blend_channel(PC_BLEND_DIFFERENCE, cb, 0.0f), cb));
        CHECK(close(pc_blend_channel(PC_BLEND_SUBTRACT, cb, 0.0f), cb));
        CHECK(close(pc_blend_channel(PC_BLEND_LINEAR_BURN, cb, 1.0f), cb));
        CHECK(close(pc_blend_channel(PC_BLEND_EXCLUSION, cb, 0.0f), cb));
        CHECK(close(pc_blend_channel(PC_BLEND_HARD_LIGHT, cb, 0.5f), cb));
        CHECK(close(pc_blend_channel(PC_BLEND_SOFT_LIGHT, cb, 0.5f), cb));
        CHECK(close(pc_blend_channel(PC_BLEND_LINEAR_LIGHT, cb, 0.5f), cb));
    }
}

static void test_known_values(void)
{
    CHECK(close(pc_blend_channel(PC_BLEND_MULTIPLY, 0.5f, 0.5f), 0.25f));
    CHECK(close(pc_blend_channel(PC_BLEND_SCREEN, 0.5f, 0.5f), 0.75f));
    CHECK(close(pc_blend_channel(PC_BLEND_OVERLAY, 0.25f, 0.5f), 0.25f));
    CHECK(close(pc_blend_channel(PC_BLEND_OVERLAY, 0.75f, 1.0f), 1.0f));
    CHECK(close(pc_blend_channel(PC_BLEND_COLOR_DODGE, 0.5f, 0.5f), 1.0f));
    CHECK(close(pc_blend_channel(PC_BLEND_COLOR_BURN, 0.5f, 0.5f), 0.0f));
    CHECK(close(pc_blend_channel(PC_BLEND_COLOR_BURN, 1.0f, 0.0f), 1.0f));
    CHECK(close(pc_blend_channel(PC_BLEND_COLOR_DODGE, 0.0f, 1.0f), 0.0f));
    CHECK(close(pc_blend_channel(PC_BLEND_DIVIDE, 0.25f, 0.5f), 0.5f));
    CHECK(close(pc_blend_channel(PC_BLEND_DIVIDE, 0.0f, 0.0f), 0.0f));
    CHECK(close(pc_blend_channel(PC_BLEND_DIVIDE, 0.3f, 0.0f), 1.0f));
    CHECK(close(pc_blend_channel(PC_BLEND_EXCLUSION, 0.5f, 0.5f), 0.5f));
    CHECK(close(pc_blend_channel(PC_BLEND_PIN_LIGHT, 0.8f, 0.2f), 0.4f));
    CHECK(close(pc_blend_channel(PC_BLEND_PIN_LIGHT, 0.1f, 0.9f), 0.8f));
    CHECK(close(pc_blend_channel(PC_BLEND_HARD_MIX, 0.6f, 0.4f), 1.0f));
    CHECK(close(pc_blend_channel(PC_BLEND_HARD_MIX, 0.6f, 0.3f), 0.0f));
}

static void test_vivid_light_source_extremes_and_interior_values(void)
{
    static const float cbs[5] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    int i;
    for (i = 0; i < 5; i++) {
        CHECK(pc_blend_channel(PC_BLEND_VIVID_LIGHT, cbs[i], 0.0f) == 0.0f);
        CHECK(pc_blend_channel(PC_BLEND_VIVID_LIGHT, cbs[i], 1.0f) == 1.0f);
    }
    CHECK(close(pc_blend_channel(PC_BLEND_VIVID_LIGHT, 0.75f, 0.25f), 0.5f));
    CHECK(close(pc_blend_channel(PC_BLEND_VIVID_LIGHT, 0.25f, 0.75f), 0.5f));
}

static void test_hard_mix_matches_photoshop_extremes_and_interior_values(void)
{
    CHECK(pc_blend_channel(PC_BLEND_HARD_MIX, 1.0f, 0.0f) == 1.0f);
    CHECK(pc_blend_channel(PC_BLEND_HARD_MIX, 0.0f, 1.0f) == 0.0f);
    CHECK(pc_blend_channel(PC_BLEND_HARD_MIX, 1.0f - 1e-5f, 0.0f) == 1.0f);
    CHECK(pc_blend_channel(PC_BLEND_HARD_MIX, 1e-5f, 1.0f) == 0.0f);
    CHECK(pc_blend_channel(PC_BLEND_HARD_MIX, 0.6f, 0.4f) == 1.0f);
    CHECK(pc_blend_channel(PC_BLEND_HARD_MIX, 0.6f, 0.3f) == 0.0f);
}

static void check_composite(pc_blend_mode m, const float bd[4], const float src[4], const float want[4])
{
    float o[4];
    pc_blend_composite(m, bd, src, 1.0f, o);
    CHECK_EQ_MEM(o, want, sizeof o);
}

static void test_vivid_light_and_hard_mix_composite_extremes(void)
{
    static const float white[4] = {1, 1, 1, 1};
    static const float black[4] = {0, 0, 0, 1};
    check_composite(PC_BLEND_VIVID_LIGHT, white, black, black);
    check_composite(PC_BLEND_VIVID_LIGHT, black, white, white);
    check_composite(PC_BLEND_HARD_MIX, black, white, black);
    check_composite(PC_BLEND_HARD_MIX, white, black, white);
}

static void test_separable_outputs_stay_in_unit_range(void)
{
    unsigned m;
    int bad = 0;
    for (m = 0; m < PC_BLEND_LAYER_MODE_COUNT; m++) {
        int a;
        int b;
        for (a = 0; a <= 20; a++) {
            for (b = 0; b <= 20; b++) {
                float cb = (float)a / 20.0f;
                float cs = (float)b / 20.0f;
                float x[3];
                float y[3];
                float o[3];
                int k;
                x[0] = cb;
                x[1] = cs;
                x[2] = 0.5f;
                y[0] = cs;
                y[1] = cb;
                y[2] = 0.25f;
                pc_blend_rgb(PC_BLEND_LAYER_MODES[m], x, y, o);
                for (k = 0; k < 3; k++) {
                    bad += !(o[k] >= -(float)E && o[k] <= 1.0f + (float)E);
                }
            }
        }
    }
    CHECK_EQ_INT(bad, 0);
}

static void test_add_and_divide_clip_at_integer_depths_but_not_in_float(void)
{
    static const float pairs[4][2] = {{0.2f, 0.3f}, {0.9f, 0.05f}, {2.0f, 0.5f}, {0.4f, 1.5f}};
    static const float b1[3] = {2.0f, 0.5f, 4.0f};
    static const float s1[3] = {0.5f, 0.25f, 0.5f};
    static const float add[3] = {2.5f, 0.75f, 4.5f};
    static const float div[3] = {4.0f, 2.0f, 8.0f};
    static const float cb[3] = {0.2f, 0.3f, 0.1f};
    static const float cs[3] = {0.3f, 0.4f, 0.4f};
    float o[3];
    float o2[3];
    unsigned m;
    CHECK(pc_blend_channel(PC_BLEND_LINEAR_DODGE, 2.0f, 0.5f) == 1.0f);
    CHECK(pc_blend_channel(PC_BLEND_LINEAR_DODGE, 0.75f, 0.5f) == 1.0f);
    CHECK(pc_blend_channel(PC_BLEND_DIVIDE, 2.0f, 0.5f) == 1.0f);
    CHECK(pc_blend_channel(PC_BLEND_DIVIDE, 0.5f, 0.25f) == 1.0f);
    CHECK(close(pc_blend_channel_hdr(PC_BLEND_LINEAR_DODGE, 2.0f, 0.5f), 2.5f));
    CHECK(close(pc_blend_channel_hdr(PC_BLEND_LINEAR_DODGE, 0.75f, 0.5f), 1.25f));
    CHECK(close(pc_blend_channel_hdr(PC_BLEND_DIVIDE, 2.0f, 0.5f), 4.0f));
    CHECK(close(pc_blend_channel_hdr(PC_BLEND_DIVIDE, 0.5f, 0.25f), 2.0f));
    pc_blend_rgb_hdr(PC_BLEND_LINEAR_DODGE, b1, s1, o);
    CHECK_EQ_MEM(o, add, sizeof o);
    pc_blend_rgb_hdr(PC_BLEND_DIVIDE, b1, s1, o);
    CHECK_EQ_MEM(o, div, sizeof o);
    CHECK(pc_blend_channel_hdr(PC_BLEND_DIVIDE, 0.0f, 0.0f) == 0.0f);
    CHECK(pc_blend_channel_hdr(PC_BLEND_DIVIDE, 3.0f, 0.0f) == 1.0f);
    CHECK(pc_blend_channel_hdr(PC_BLEND_DIVIDE, 3.0f, -1.0f) == 1.0f);
    CHECK(pc_blend_channel_hdr(PC_BLEND_DIVIDE, FLT_MAX, 1e-30f) == FLT_MAX);
    CHECK(pc_blend_channel_hdr(PC_BLEND_LINEAR_DODGE, FLT_MAX, FLT_MAX) == FLT_MAX);
    CHECK(isfinite(pc_blend_channel_hdr(PC_BLEND_LINEAR_DODGE, INFINITY, 1.0f)));
    for (m = 0; m < PC_BLEND_LAYER_MODE_COUNT; m++) {
        pc_blend_mode mode = PC_BLEND_LAYER_MODES[m];
        int i;
        /* Non-separable modes keep values above 1 in float (pc_blend_rgb_within). */
        if (!pc_blend_is_separable(mode) || mode == PC_BLEND_LINEAR_DODGE || mode == PC_BLEND_DIVIDE) {
            continue;
        }
        for (i = 0; i < 4; i++) {
            float x[3];
            float y[3];
            x[0] = pairs[i][0];
            x[1] = pairs[i][1];
            x[2] = 0.5f;
            y[0] = pairs[i][1];
            y[1] = pairs[i][0];
            y[2] = 0.25f;
            pc_blend_rgb(mode, x, y, o);
            pc_blend_rgb_hdr(mode, x, y, o2);
            CHECK_EQ_MEM(o, o2, sizeof o);
        }
    }
    pc_blend_rgb_hdr(PC_BLEND_LINEAR_DODGE, cb, cs, o);
    pc_blend_rgb(PC_BLEND_LINEAR_DODGE, cb, cs, o2);
    CHECK_EQ_MEM(o, o2, sizeof o);
    pc_blend_rgb_hdr(PC_BLEND_DIVIDE, cb, cs, o);
    pc_blend_rgb(PC_BLEND_DIVIDE, cb, cs, o2);
    CHECK_EQ_MEM(o, o2, sizeof o);
}

static void test_luminosity_and_color_swap_components(void)
{
    static const float cb[3] = {0.8f, 0.2f, 0.2f};
    static const float cs[3] = {0.1f, 0.1f, 0.9f};
    static const float gray[3] = {0.5f, 0.5f, 0.5f};
    float o[3];
    pc_blend_rgb(PC_BLEND_LUMINOSITY, cb, cs, o);
    CHECK(close(pc_blend_lum(o), pc_blend_lum(cs)));
    pc_blend_rgb(PC_BLEND_COLOR, cb, cs, o);
    CHECK(close(pc_blend_lum(o), pc_blend_lum(cb)));
    pc_blend_rgb(PC_BLEND_HUE, cb, cs, o);
    CHECK(close(pc_blend_lum(o), pc_blend_lum(cb)));
    pc_blend_rgb(PC_BLEND_SATURATION, cb, gray, o);
    CHECK(close(o[0], o[1]) && close(o[1], o[2])); /* zero-saturation source desaturates */
}

static void test_darker_lighter_color_pick_whole_pixel(void)
{
    static const float a[3] = {0.9f, 0.1f, 0.1f};
    static const float b[3] = {0.2f, 0.6f, 0.2f};
    float o[3];
    pc_blend_rgb(PC_BLEND_DARKER_COLOR, a, b, o);
    CHECK_EQ_MEM(o, pc_blend_lum(b) < pc_blend_lum(a) ? b : a, sizeof o);
    pc_blend_rgb(PC_BLEND_LIGHTER_COLOR, a, b, o);
    CHECK_EQ_MEM(o, pc_blend_lum(b) > pc_blend_lum(a) ? b : a, sizeof o);
    /* out may alias an input */
    memcpy(o, a, sizeof o);
    pc_blend_rgb(PC_BLEND_LIGHTER_COLOR, o, b, o);
    CHECK_EQ_MEM(o, pc_blend_lum(b) > pc_blend_lum(a) ? b : a, sizeof o);
}

static void test_composite_alpha_rules(void)
{
    static const float bg[4] = {0.2f, 0.4f, 0.6f, 1.0f};
    static const float clear_red[4] = {1, 0, 0, 0};
    static const float red[4] = {1, 0, 0, 1};
    static const float white[4] = {1, 1, 1, 1};
    static const float none[4] = {0, 0, 0, 0};
    static const float src[4] = {0.3f, 0.5f, 0.7f, 1.0f};
    static const float half_black[4] = {0, 0, 0, 0.5f};
    float r[4];
    pc_blend_composite(PC_BLEND_MULTIPLY, bg, clear_red, 1.0f, r);
    CHECK_EQ_MEM(r, bg, sizeof r);
    pc_blend_composite(PC_BLEND_NORMAL, bg, red, 1.0f, r);
    CHECK(close(r[0], 1.0f) && close(r[3], 1.0f));
    pc_blend_composite(PC_BLEND_NORMAL, bg, white, 0.5f, r);
    CHECK(close(r[0], 0.6f) && close(r[1], 0.7f) && close(r[2], 0.8f));
    pc_blend_composite(PC_BLEND_MULTIPLY, none, src, 1.0f, r);
    CHECK(close(r[0], 0.3f) && close(r[1], 0.5f) && close(r[2], 0.7f) && close(r[3], 1.0f));
    pc_blend_composite(PC_BLEND_SCREEN, half_black, half_black, 1.0f, r);
    CHECK(close(r[3], 0.75f));
    /* out aliasing the backdrop */
    memcpy(r, bg, sizeof r);
    pc_blend_composite(PC_BLEND_NORMAL, r, white, 0.5f, r);
    CHECK(close(r[0], 0.6f) && close(r[3], 1.0f));
}

static void test_paint_net_modes_keep_their_math_and_distinct_psd_status(void)
{
    static const float vals[4] = {0.0f, 0.1f, 0.5f, 1.0f};
    size_t i;
    unsigned b;
    unsigned sv;
    size_t count = 0;
    CHECK(close(pc_blend_channel(PC_BLEND_REFLECT, 0.5f, 0.5f), 0.5f));
    CHECK(close(pc_blend_channel(PC_BLEND_REFLECT, 0.2f, 0.0f), 0.04f));
    CHECK(close(pc_blend_channel(PC_BLEND_GLOW, 0.5f, 0.2f), 0.08f));
    CHECK(close(pc_blend_channel(PC_BLEND_NEGATION, 0.2f, 0.7f), 0.9f));
    CHECK(pc_blend_channel(PC_BLEND_REFLECT, 0.0f, 1.0f) == 1.0f);
    CHECK(pc_blend_channel(PC_BLEND_GLOW, 1.0f, 0.0f) == 1.0f);
    CHECK(pc_blend_channel(PC_BLEND_PAINT_NET_COLOR_BURN, 1.0f, 0.0f) == 0.0f);
    CHECK(pc_blend_channel(PC_BLEND_PAINT_NET_COLOR_DODGE, 0.0f, 1.0f) == 1.0f);
    for (b = 0; b <= 255; b++) {
        for (sv = 0; sv <= 255; sv++) {
            float got = pc_blend_channel(PC_BLEND_XOR, (float)b / 255.0f, (float)sv / 255.0f);
            if (!close(got, (float)(b ^ sv) / 255.0f)) {
                PC_T_FAIL("xor %u %u", b, sv);
            }
        }
    }
    for (i = 0; i < PC_BLEND_PAINT_NET_MODE_COUNT; i++) {
        pc_blend_mode m = PC_BLEND_PAINT_NET_MODES[i];
        size_t x;
        size_t y;
        CHECK(!pc_blend_has_psd_equivalent(m));
        CHECK(memcmp(pc_blend_psd_key(m), "norm", 4) == 0);
        for (x = 0; x < 4; x++) {
            for (y = 0; y < 4; y++) {
                float v = pc_blend_channel(m, vals[x], vals[y]);
                CHECK(isfinite(v) && v >= 0.0f && v <= 1.0f);
            }
        }
    }
    for (i = 0; i < PC_BLEND_PSD_COUNT; i++) {
        CHECK(pc_blend_has_psd_equivalent((pc_blend_mode)i));
    }
    count = PC_BLEND_LAYER_MODE_COUNT + PC_BLEND_PAINT_NET_MODE_COUNT;
    CHECK_EQ_UINT(count, 33);
    CHECK_EQ_UINT(PC_BLEND_ALL_LAYER_MODE_COUNT, 33);
    /* "norm" still reads as Normal; serde names round-trip. */
    {
        pc_blend_mode m = PC_BLEND_XOR;
        CHECK(pc_blend_from_psd_key("norm", &m) && m == PC_BLEND_NORMAL);
        CHECK(pc_blend_from_name("Xor", 3, &m) && m == PC_BLEND_XOR);
        CHECK(pc_blend_from_name("PaintNetColorDodge", 18, &m) && m == PC_BLEND_PAINT_NET_COLOR_DODGE);
        CHECK_EQ_STR(pc_blend_label(PC_BLEND_XOR), "XOR");
        CHECK_EQ_STR(pc_blend_label(PC_BLEND_PAINT_NET_COLOR_BURN), "Color Burn (Paint.NET)");
    }
}

/* ---- the spec's ClipColor (what clip_color was before it guarded out-of-range luminosity) ---- */

static float lum3(const float c[3])
{
    return pc_blend_lum(c);
}

static void spec_clip_color(const float c[3], float out[3])
{
    float l = lum3(c);
    float n = fminf(fminf(c[0], c[1]), c[2]);
    float x = fmaxf(fmaxf(c[0], c[1]), c[2]);
    int i;
    memcpy(out, c, 3 * sizeof *out);
    if (n < 0.0f) {
        float d = l - n;
        for (i = 0; i < 3; i++) {
            out[i] = fabsf(d) < 1e-12f ? l : l + (out[i] - l) * l / d;
        }
    }
    if (x > 1.0f) {
        float d = x - l;
        for (i = 0; i < 3; i++) {
            out[i] = fabsf(d) < 1e-12f ? l : l + (out[i] - l) * (1.0f - l) / d;
        }
    }
}

static float sat3(const float c[3])
{
    return fmaxf(fmaxf(c[0], c[1]), c[2]) - fminf(fminf(c[0], c[1]), c[2]);
}

static void set_sat3(const float c[3], float sv, float out[3])
{
    size_t idx[3] = {0, 1, 2};
    size_t i;
    float o[3] = {0.0f, 0.0f, 0.0f};
    for (i = 1; i < 3; i++) {
        size_t t = idx[i];
        size_t j = i;
        while (j > 0 && c[t] < c[idx[j - 1]]) {
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = t;
    }
    if (c[idx[2]] > c[idx[0]]) {
        o[idx[1]] = (c[idx[1]] - c[idx[0]]) * sv / (c[idx[2]] - c[idx[0]]);
        o[idx[2]] = sv;
    }
    o[idx[0]] = 0.0f;
    memcpy(out, o, sizeof o);
}

static void spec_set_lum(const float c[3], float l, float out[3])
{
    float d = l - lum3(c);
    float t[3] = {c[0] + d, c[1] + d, c[2] + d};
    spec_clip_color(t, out);
}

static void spec_blend_rgb(pc_blend_mode m, const float cb[3], const float cs[3], float out[3])
{
    float t[3];
    switch (m) {
    case PC_BLEND_HUE:
        set_sat3(cs, sat3(cb), t);
        spec_set_lum(t, lum3(cb), out);
        return;
    case PC_BLEND_SATURATION:
        set_sat3(cb, sat3(cs), t);
        spec_set_lum(t, lum3(cb), out);
        return;
    case PC_BLEND_COLOR:
        spec_set_lum(cs, lum3(cb), out);
        return;
    case PC_BLEND_LUMINOSITY:
        spec_set_lum(cb, lum3(cs), out);
        return;
    default:
        pc_blend_rgb(m, cb, cs, out);
        return;
    }
}

static const pc_blend_mode NON_SEPARABLE[4] = {PC_BLEND_HUE, PC_BLEND_SATURATION, PC_BLEND_COLOR,
                                               PC_BLEND_LUMINOSITY};

static void test_non_separable_modes_unchanged_on_integer_inputs(void)
{
    /* 8/16-bit documents only hold 0..1, where the luminosity stays in range (up to a rounding
     * step at white) and ClipColor is the plain ISO 32000 formula. */
    float edges[15] = {0.0f, 1.0f / 255.0f, 0.5f, 254.0f / 255.0f, 65534.0f / 65535.0f, 1.0f};
    float (*colors)[3];
    size_t ncolors = 0;
    size_t ne = 6;
    size_t a;
    size_t b;
    size_t c;
    uint32_t seed = 0x12345678u;
    float worst = 0.0f;
    int i;
    for (i = 0; i <= 8; i++) {
        edges[ne++] = (float)i / 8.0f;
    }
    colors = malloc((15 * 15 * 15 + 2000) * sizeof *colors);
    CHECK(colors != NULL);
    if (colors == NULL) {
        return;
    }
    for (a = 0; a < ne; a++) {
        for (b = 0; b < ne; b++) {
            for (c = 0; c < ne; c++) {
                colors[ncolors][0] = edges[a];
                colors[ncolors][1] = edges[b];
                colors[ncolors][2] = edges[c];
                ncolors++;
            }
        }
    }
#define NEXT_LEVEL(levels)                                                                                   \
    (seed = seed * 1664525u + 1013904223u, (float)((seed >> 8) % ((levels) + 1u)) / (float)(levels))
    for (i = 0; i < 2000; i++) {
        uint32_t levels = NEXT_LEVEL(1u) > 0.5f ? 255u : 65535u;
        float x = NEXT_LEVEL(levels);
        float y = NEXT_LEVEL(levels);
        float z = NEXT_LEVEL(levels);
        colors[ncolors][0] = x;
        colors[ncolors][1] = y;
        colors[ncolors][2] = z;
        ncolors++;
    }
#undef NEXT_LEVEL
    for (a = 0; a < ncolors; a++) {
        for (b = a % 7; b < ncolors; b += 7) {
            size_t m;
            for (m = 0; m < 4; m++) {
                float got[3];
                float want[3];
                size_t k;
                pc_blend_rgb(NON_SEPARABLE[m], colors[a], colors[b], got);
                spec_blend_rgb(NON_SEPARABLE[m], colors[a], colors[b], want);
                for (k = 0; k < 3; k++) {
                    worst = fmaxf(worst, fabsf(got[k] - want[k]));
                }
            }
        }
    }
    free(colors);
    /* Only white-ish colours whose luminosity rounds a step above 1 can differ, by a rounding step. */
    CHECK(worst <= 1e-6f);
}

static void test_non_separable_modes_stable_when_luminosity_is_out_of_range(void)
{
    static const float colors[10][3] = {
        {1.0f, 1.0f, 1.0f},          {1.5f, 1.5f, 1.5f},      {2.0f, 2.0f, 2.0f},    {4.0f, 4.0f, 4.0f},
        {1000.0f, 1000.0f, 1000.0f}, {2.0f, 2.0f, 1.999999f}, {4.0f, 3.9999f, 4.0f}, {3.0f, 0.25f, 0.125f},
        {1.25f, 0.875f, 3.75f},      {-0.25f, 0.5f, 1.375f},
    };
    static const float tops[2] = {1.0f, FLT_MAX};
    static const float greys[4] = {1.5f, 2.0f, 4.0f, 1000.0f};
    const float base[3] = {0.5f, 0.25f, 0.125f};
    float spec[3];
    float r[3];
    size_t t;
    size_t m;
    size_t a;
    size_t b;
    size_t g;
    /* The spec formula turns colours inside out when the luminosity is above 1. */
    {
        float two[3] = {2.0f, 2.0f, 2.0f};
        spec_blend_rgb(PC_BLEND_LUMINOSITY, base, two, spec);
        CHECK(spec[0] < spec[2]);
    }
    for (t = 0; t < 2; t++) {
        for (m = 0; m < 4; m++) {
            for (a = 0; a < 10; a++) {
                for (b = 0; b < 10; b++) {
                    float want;
                    float lo;
                    float hi;
                    pc_blend_rgb_within(NON_SEPARABLE[m], colors[a], colors[b], tops[t], r);
                    if (!(isfinite(r[0]) && isfinite(r[1]) && isfinite(r[2]))) {
                        PC_T_FAIL("mode %zu cb %zu cs %zu top %zu: not finite", m, a, b, t);
                        continue;
                    }
                    /* The target luminosity is kept, at any depth. */
                    want = NON_SEPARABLE[m] == PC_BLEND_LUMINOSITY ? lum3(colors[b]) : lum3(colors[a]);
                    if (want >= 0.0f && !(fabsf(lum3(r) - want) <= 1e-4f * fmaxf(want, 1.0f))) {
                        PC_T_FAIL("mode %zu cb %zu cs %zu top %zu: luminosity", m, a, b, t);
                    }
                    lo = fminf(fminf(r[0], r[1]), r[2]);
                    hi = fmaxf(fmaxf(r[0], r[1]), r[2]);
                    if (t == 0) {
                        /* In gamut, or (a luminosity above 1 has no in-gamut colour) its grey. */
                        if (!((lo >= 0.0f && hi <= 1.0f + (float)E) || hi - lo <= 1e-6f * hi)) {
                            PC_T_FAIL("mode %zu cb %zu cs %zu: out of gamut", m, a, b);
                        }
                    } else if (!(lo >= -1e-6f * fmaxf(hi, 1.0f))) {
                        PC_T_FAIL("mode %zu cb %zu cs %zu: negative", m, a, b);
                    }
                }
            }
        }
    }
    /* 32-bit: a grey backdrop gives a grey of its own luminosity in Hue and Saturation, a grey
     * source a grey in Color, and bright colours keep their values above 1. */
    for (g = 0; g < 4; g++) {
        float gv = greys[g];
        float grey[3] = {gv, gv, gv};
        float bright[3] = {3.0f, 0.25f, 0.125f};
        for (b = 0; b < 10; b++) {
            pc_blend_mode hs[2] = {PC_BLEND_HUE, PC_BLEND_SATURATION};
            for (m = 0; m < 2; m++) {
                size_t k;
                pc_blend_rgb_within(hs[m], grey, colors[b], FLT_MAX, r);
                for (k = 0; k < 3; k++) {
                    CHECK(fabsf(r[k] - lum3(grey)) <= 1e-5f * gv);
                }
            }
        }
        pc_blend_rgb_within(PC_BLEND_LUMINOSITY, base, grey, FLT_MAX, r);
        CHECK(r[0] > 1.0f && r[0] > r[1] && r[1] > r[2]);
        pc_blend_rgb_within(PC_BLEND_COLOR, grey, bright, FLT_MAX, r);
        CHECK(r[0] > 1.0f && r[0] > r[1] && r[1] > r[2]);
        /* Integer documents: no colour of that luminosity fits in 0..1, so it is its grey. */
        pc_blend_rgb_within(PC_BLEND_LUMINOSITY, base, grey, 1.0f, r);
        CHECK(fabsf(r[0] - lum3(grey)) <= 1e-5f * gv && fabsf(r[1] - lum3(grey)) <= 1e-5f * gv &&
              fabsf(r[2] - lum3(grey)) <= 1e-5f * gv);
    }
    /* Continuous across near-greys: the result barely moves from the grey's. */
    for (m = 0; m < 4; m++) {
        const float g2[3] = {2.0f, 2.0f, 2.0f};
        const float g4[3] = {4.0f, 4.0f, 4.0f};
        const float n2[3] = {2.0f, 2.0f, 1.999999f};
        const float n4[3] = {4.0f, 3.999999f, 4.0f};
        float ra[3];
        float rb[3];
        pc_blend_rgb_within(NON_SEPARABLE[m], g2, g4, FLT_MAX, ra);
        pc_blend_rgb_within(NON_SEPARABLE[m], n2, n4, FLT_MAX, rb);
        CHECK(fabsf(ra[0] - rb[0]) <= 1e-4f && fabsf(ra[1] - rb[1]) <= 1e-4f &&
              fabsf(ra[2] - rb[2]) <= 1e-4f);
    }
}

static void test_colors_convert_into_a_document_mode(void)
{
    pc_color blue = pc_color_rgba(0.0f, 0.0f, 1.0f, 0.5f);
    pc_color g = pc_color_in_mode(&blue, PC_COLOR_MODE_GRAYSCALE);
    pc_color same;
    static const pc_color_mode modes[2] = {PC_COLOR_MODE_CMYK, PC_COLOR_MODE_LAB};
    size_t i;
    CHECK(g.mode == PC_COLOR_MODE_GRAYSCALE && g.alpha == 0.5f);
    CHECK(fabsf(g.c[0] - 0.114f) < 1e-6f);
    /* RGB-based modes keep RGB colours; a colour already in the mode is unchanged. */
    same = pc_color_in_mode(&blue, PC_COLOR_MODE_INDEXED);
    CHECK(memcmp(&same, &blue, sizeof same) == 0);
    same = pc_color_in_mode(&g, PC_COLOR_MODE_GRAYSCALE);
    CHECK(memcmp(&same, &g, sizeof same) == 0);
    for (i = 0; i < 2; i++) {
        pc_color c = pc_color_in_mode(&blue, modes[i]);
        pc_color back = pc_color_in_mode(&c, PC_COLOR_MODE_RGB);
        float a[3];
        float b[3];
        CHECK(c.mode == modes[i]);
        CHECK(back.mode == PC_COLOR_MODE_RGB);
        pc_color_to_rgb(&back, a);
        pc_color_to_rgb(&c, b);
        CHECK_EQ_MEM(a, b, sizeof a); /* back to RGB shows the same colour */
    }
}

int main(int argc, char **argv)
{
    RUN(test_pixel_format_sizes);
    RUN(test_sample_roundtrip_all_depths);
    RUN(test_integer_samples_clamp);
    RUN(test_float_samples_keep_hdr);
    RUN(test_samples_out_of_range_are_harmless);
    RUN(test_color_to_rgba8);
    RUN(test_names);
    RUN(test_srgb_roundtrip);
    RUN(test_cmyk_roundtrip);
    RUN(test_managed_cmyk);
    RUN(test_document_cmyk_space_scope);
    RUN(test_spaces_are_per_thread);
    RUN(test_lab_roundtrip_and_anchors);
    RUN(test_psd_keys_roundtrip_and_unique);
    RUN(test_normal_is_source);
    RUN(test_identities_with_white_and_black);
    RUN(test_known_values);
    RUN(test_vivid_light_source_extremes_and_interior_values);
    RUN(test_hard_mix_matches_photoshop_extremes_and_interior_values);
    RUN(test_vivid_light_and_hard_mix_composite_extremes);
    RUN(test_separable_outputs_stay_in_unit_range);
    RUN(test_add_and_divide_clip_at_integer_depths_but_not_in_float);
    RUN(test_luminosity_and_color_swap_components);
    RUN(test_darker_lighter_color_pick_whole_pixel);
    RUN(test_composite_alpha_rules);
    RUN(test_paint_net_modes_keep_their_math_and_distinct_psd_status);
    RUN(test_non_separable_modes_unchanged_on_integer_inputs);
    RUN(test_non_separable_modes_stable_when_luminosity_is_out_of_range);
    RUN(test_colors_convert_into_a_document_mode);
    return pc_t_finish();
}
