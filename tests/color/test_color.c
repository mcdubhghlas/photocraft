/* Ports the unit tests of crates/color/src/{lib,convert,blend}.rs, plus edge cases for the C
 * API (bounds-checked samples, CMYK space scopes and cache, names). */
#include <float.h>
#include <math.h>
#include <pthread.h>
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
    for (i = 0; i < PC_BLEND_COUNT; i++) {
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
        if (mode == PC_BLEND_LINEAR_DODGE || mode == PC_BLEND_DIVIDE) {
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
    return pc_t_finish();
}
