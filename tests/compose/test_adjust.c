/* Adjustment evaluation (crates/compose/src/adjust.rs lookup_tests and tone_tests, and the
 * function-level adjustment tests of tests.rs: threshold / posterize / levels / curves,
 * Hue/Saturation, modern Brightness/Contrast, Levels, Exposure, Photo Filter, Vibrance), plus
 * gradient_fill.rs and pattern.rs unit tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ch.h"
#include "compose/adjust.h"
#include "compose/gradient_fill.h"
#include "compose/pattern.h"

#define LUT PC_COMPOSE_LUT_SIZE

static const pc_compose_transfer SRGB = {PC_COMPOSE_TRANSFER_SRGB, 0.0f};

/* A 1 x 1 buffer of colour c. */
static pc_compose_buffer px1(float r, float g, float b, float a)
{
    pc_compose_buffer buf;
    const float c[4] = {r, g, b, a};
    if (pc_compose_buffer_filled(pc_rect_make(0, 0, 1, 1), c, &buf, NULL) != PC_OK) {
        abort();
    }
    return buf;
}

static pc_adjustment adj_of(pc_adjustment_kind k)
{
    pc_adjustment a;
    memset(&a, 0, sizeof a);
    a.kind = k;
    return a;
}

/* An 8 x 8 ramp: red along x, green along y, blue 0.3. */
static pc_compose_buffer ramp(void)
{
    pc_compose_buffer b;
    size_t i;
    if (pc_compose_buffer_transparent(pc_rect_make(0, 0, 8, 8), &b, NULL) != PC_OK) {
        abort();
    }
    for (i = 0; i < 64; i++) {
        b.px[i][0] = (float)(i % 8) / 7.0f;
        b.px[i][1] = (float)(i / 8) / 7.0f;
        b.px[i][2] = 0.3f;
        b.px[i][3] = 1.0f;
    }
    return b;
}

static float *identity_lut3d(size_t n, size_t *len)
{
    float m = (float)(n - 1);
    size_t i;
    float *t = malloc(n * n * n * 3 * sizeof *t);
    if (t == NULL) {
        abort();
    }
    for (i = 0; i < n * n * n; i++) {
        t[i * 3] = (float)(i % n) / m;
        t[i * 3 + 1] = (float)((i / n) % n) / m;
        t[i * 3 + 2] = (float)(i / (n * n)) / m;
    }
    *len = n * n * n * 3;
    return t;
}

/* ---- lookup_tests ---- */

static void test_identity_lut_is_identity_both_interpolations(void)
{
    static const float cs[3][3] = {{0.1f, 0.5f, 0.9f}, {0.33f, 0.77f, 0.0f}, {1, 1, 1}};
    size_t len;
    float *t = identity_lut3d(5, &len);
    size_t i;
    int tet;
    int k;
    for (i = 0; i < 3; i++) {
        for (tet = 0; tet < 2; tet++) {
            float o[3];
            pc_compose_lut3d_sample(t, len, 5, cs[i], tet != 0, o);
            for (k = 0; k < 3; k++) {
                CHECK(fabsf(o[k] - cs[i][k]) < 1e-5f);
            }
        }
    }
    /* A short table or a degenerate size gives the input back (the reference would panic). */
    {
        float o[3];
        pc_compose_lut3d_sample(t, len - 1, 5, cs[0], false, o);
        CHECK(memcmp(o, cs[0], sizeof o) == 0);
        pc_compose_lut3d_sample(t, len, 1, cs[0], true, o);
        CHECK(memcmp(o, cs[0], sizeof o) == 0);
    }
    free(t);
}

static void test_selective_color_zero_is_identity_and_ranges_are_local(void)
{
    static const float cs[3][3] = {{0.9f, 0.1f, 0.1f}, {0.5f, 0.5f, 0.5f}, {0.2f, 0.4f, 0.8f}};
    float zero[9][4];
    float a[9][4];
    float o[3];
    const float red[3] = {0.9f, 0.1f, 0.1f};
    const float blue[3] = {0.1f, 0.1f, 0.9f};
    const float white[3] = {1, 1, 1};
    size_t i;
    memset(zero, 0, sizeof zero);
    for (i = 0; i < 3; i++) {
        pc_compose_selective_color(cs[i], true, (const float (*)[4])zero, o);
        CHECK(memcmp(o, cs[i], sizeof o) == 0);
    }
    /* +100 % cyan on reds only: red loses red, blue is untouched. */
    memcpy(a, zero, sizeof a);
    a[0][0] = 100.0f;
    pc_compose_selective_color(red, false, (const float (*)[4])a, o);
    CHECK(o[0] < 0.2f);
    pc_compose_selective_color(blue, false, (const float (*)[4])a, o);
    CHECK(memcmp(o, blue, sizeof o) == 0);
    /* Relative mode can't tint pure white; absolute can (via whites). */
    memcpy(a, zero, sizeof a);
    a[6][2] = 50.0f;
    pc_compose_selective_color(white, true, (const float (*)[4])a, o);
    CHECK(memcmp(o, white, sizeof o) == 0);
    pc_compose_selective_color(white, false, (const float (*)[4])a, o);
    CHECK(o[2] < 0.6f);
}

static void test_dither_offsets_are_balanced(void)
{
    float sum = 0.0f;
    int32_t x;
    int32_t y;
    for (y = 0; y < 4; y++) {
        for (x = 0; x < 4; x++) {
            sum += pc_compose_bayer4(x, y);
        }
    }
    CHECK(fabsf(sum) < 1e-5f);
    CHECK(pc_compose_bayer4(-4, -4) == pc_compose_bayer4(0, 0));
}

/* ---- tone_tests ---- */

static void test_color_lookup_with_an_overflowing_stored_size_is_the_identity(void)
{
    /* A stored document's size is untrusted: (2^22)^3 x 3 overflows. */
    static const uint32_t sizes[2] = {1u << 22, UINT32_MAX};
    float vals[24];
    size_t i;
    for (i = 0; i < 24; i++) {
        vals[i] = 0.5f;
    }
    for (i = 0; i < 2; i++) {
        pc_adjustment a = adj_of(PC_ADJ_COLOR_LOOKUP);
        pc_compose_buffer b = ramp();
        pc_compose_buffer r = ramp();
        CHECK(pc_blob_new(vals, sizeof vals, &a.u.color_lookup.lut, NULL) == PC_OK);
        a.u.color_lookup.size = sizes[i];
        CHECK(pc_compose_adjust_apply(&a, &b, NULL) == PC_OK);
        CHECK(pc_compose_buffer_eq(&b, &r));
        pc_adjustment_fini(&a);
        pc_compose_buffer_fini(&b);
        pc_compose_buffer_fini(&r);
    }
}

static void test_identity_tone_in_every_space_is_identity(void)
{
    static const pc_tone_space spaces[3] = {PC_TONE_RGB, PC_TONE_CMYK, PC_TONE_LAB};
    size_t s;
    size_t i;
    int k;
    for (s = 0; s < 3; s++) {
        pc_adjustment a;
        pc_compose_buffer b = ramp();
        pc_compose_buffer r = ramp();
        bool ok = true;
        CHECK(pc_adjustment_identity_curves(&a, NULL) == PC_OK);
        a.u.curves.space = spaces[s];
        CHECK(pc_compose_adjust_apply(&a, &b, NULL) == PC_OK);
        for (i = 0; i < b.len; i++) {
            for (k = 0; k < 3; k++) {
                ok = ok && fabsf(b.px[i][k] - r.px[i][k]) < 0.02f;
            }
        }
        CHECK(ok);
        pc_adjustment_fini(&a);
        pc_compose_buffer_fini(&b);
        pc_compose_buffer_fini(&r);
    }
}

static pc_curve line(float a, float b)
{
    pc_curve c;
    c.pts = malloc(2 * sizeof *c.pts);
    if (c.pts == NULL) {
        abort();
    }
    c.pts[0].input = 0.0f;
    c.pts[0].output = a;
    c.pts[1].input = 1.0f;
    c.pts[1].output = b;
    c.len = 2;
    return c;
}

static void test_cmyk_black_curve_darkens_and_lab_lightness_lifts(void)
{
    pc_adjustment a = adj_of(PC_ADJ_CURVES);
    pc_compose_buffer b = px1(0.8f, 0.8f, 0.8f, 1);
    float *p;
    /* Black brightness 1 -> 0.5: more black ink everywhere. */
    a.u.curves.master = line(0, 1);
    a.u.curves.per_channel[0] = line(0, 1);
    a.u.curves.per_channel[1] = line(0, 1);
    a.u.curves.per_channel[2] = line(0, 1);
    a.u.curves.space = PC_TONE_CMYK;
    a.u.curves.black = line(0, 0.5f);
    CHECK(pc_compose_adjust_apply(&a, &b, NULL) == PC_OK);
    CHECK(b.px[0][0] < 0.7f);
    pc_adjustment_fini(&a);
    pc_compose_buffer_fini(&b);
    /* Lab lightness 0..1 -> 0.2..1 lifts a dark grey; a/b untouched keeps it grey. */
    a = adj_of(PC_ADJ_CURVES);
    a.u.curves.master = line(1, 0); /* ignored in Lab */
    a.u.curves.per_channel[0] = line(0.2f, 1);
    a.u.curves.per_channel[1] = line(0, 1);
    a.u.curves.per_channel[2] = line(0, 1);
    a.u.curves.space = PC_TONE_LAB;
    b = px1(0.2f, 0.2f, 0.2f, 1);
    CHECK(pc_compose_adjust_apply(&a, &b, NULL) == PC_OK);
    p = b.px[0];
    CHECK(p[0] > 0.3f && fabsf(p[0] - p[2]) < 0.02f);
    pc_adjustment_fini(&a);
    pc_compose_buffer_fini(&b);
}

static void test_hue_ranges_only_touch_their_colours(void)
{
    pc_adjustment a = adj_of(PC_ADJ_HUE_SATURATION);
    pc_compose_buffer b;
    const float c[3][4] = {{0.9f, 0.1f, 0.1f, 1}, {0.1f, 0.1f, 0.9f, 1}, {0.5f, 0.5f, 0.5f, 1}};
    pc_hue_range_defaults(a.u.hue_saturation.ranges);
    a.u.hue_saturation.ranges[0].saturation = -100.0f; /* desaturate reds */
    CHECK(pc_compose_buffer_transparent(pc_rect_make(0, 0, 3, 1), &b, NULL) == PC_OK);
    memcpy(b.px, c, sizeof c);
    CHECK(pc_compose_adjust_apply(&a, &b, NULL) == PC_OK);
    CHECK(fabsf(b.px[0][0] - b.px[0][1]) < 0.05f);
    CHECK(fabsf(b.px[1][2] - 0.9f) < 1e-4f && fabsf(b.px[1][0] - 0.1f) < 1e-4f);
    CHECK(memcmp(b.px[2], c[2], sizeof c[2]) == 0);
    pc_compose_buffer_fini(&b);
}

static void test_black_white_weights(void)
{
    const float d[6] = {40, 60, 40, 60, 20, 80};
    float w[6];
    const float grey[3] = {0.5f, 0.5f, 0.5f};
    const float red[3] = {1, 0, 0};
    const float yellow[3] = {1, 1, 0};
    CHECK(fabsf(pc_compose_black_white_gray(grey, d) - 0.5f) < 1e-6f);
    CHECK(fabsf(pc_compose_black_white_gray(red, d) - 0.4f) < 1e-6f);
    CHECK(fabsf(pc_compose_black_white_gray(yellow, d) - 0.6f) < 1e-6f);
    memcpy(w, d, sizeof w);
    w[0] = 100.0f;
    CHECK(pc_compose_black_white_gray(red, w) > pc_compose_black_white_gray(red, d));
}

static void test_gradient_map_dither_stays_close(void)
{
    pc_gradient_map_stop stops[2] = {{0.0f, {0, 0, 0}}, {1.0f, {1, 1, 1}}};
    pc_adjustment a = adj_of(PC_ADJ_GRADIENT_MAP);
    pc_compose_buffer x = ramp();
    pc_compose_buffer y = ramp();
    bool close = true;
    bool differ = false;
    size_t i;
    a.u.gradient_map.stops = stops;
    a.u.gradient_map.nstops = 2;
    CHECK(pc_compose_adjust_apply(&a, &x, NULL) == PC_OK);
    a.u.gradient_map.dither = true;
    CHECK(pc_compose_adjust_apply(&a, &y, NULL) == PC_OK);
    for (i = 0; i < x.len; i++) {
        close = close && fabsf(x.px[i][0] - y.px[i][0]) <= 0.5f / 255.0f + 1e-6f;
        differ = differ || x.px[i][0] != y.px[i][0];
    }
    CHECK(close && differ);
    pc_compose_buffer_fini(&x);
    pc_compose_buffer_fini(&y);
}

/* ---- tests.rs: functions ---- */

static void test_threshold_posterize_levels_curves(void)
{
    pc_adjustment a = adj_of(PC_ADJ_THRESHOLD);
    pc_compose_buffer b = px1(0.3f, 0.3f, 0.3f, 1);
    const float black[4] = {0, 0, 0, 1};
    const float post[4] = {0, 1, 1, 1};
    pc_levels_channel lv = pc_levels_channel_default();
    pc_curve_point id[2] = {{0, 0}, {1, 1}};
    pc_curve_point sp[4] = {{0, 0}, {0.25f, 0.15f}, {0.75f, 0.85f}, {1, 1}};
    float *lut = malloc(LUT * sizeof *lut);
    size_t i;
    bool ok = true;
    if (lut == NULL) {
        abort();
    }
    a.u.threshold.level = 0.5f;
    CHECK(pc_compose_adjust_apply(&a, &b, NULL) == PC_OK);
    CHECK(ch_close4(b.px[0], black));
    pc_compose_buffer_fini(&b);
    a = adj_of(PC_ADJ_POSTERIZE);
    a.u.posterize.levels = 2;
    b = px1(0.3f, 0.6f, 0.9f, 1);
    CHECK(pc_compose_adjust_apply(&a, &b, NULL) == PC_OK);
    CHECK(ch_close4(b.px[0], post));
    pc_compose_buffer_fini(&b);
    lv.in_black = 0.2f;
    lv.in_white = 0.8f;
    CHECK(fabsf(pc_compose_levels(&lv, 0.5f) - 0.5f) < 1e-6f);
    CHECK(fabsf(pc_compose_levels(&lv, 0.2f)) < 1e-6f);
    CHECK(fabsf(pc_compose_levels(&lv, 0.9f) - 1.0f) < 1e-6f);
    CHECK(pc_compose_curve_lut(id, 2, lut, NULL) == PC_OK);
    for (i = 0; i < LUT; i += 97) {
        ok = ok && fabsf(lut[i] - (float)i / (float)(LUT - 1)) < 1e-4f;
    }
    CHECK(ok);
    CHECK(pc_compose_curve_lut(sp, 4, lut, NULL) == PC_OK);
    ok = true;
    for (i = 1; i < LUT; i++) {
        ok = ok && lut[i] >= lut[i - 1] - 1e-6f; /* monotone */
    }
    CHECK(ok);
    CHECK(lut[LUT / 4] < 0.25f && lut[3 * LUT / 4] > 0.75f);
    free(lut);
}

static void test_hue_saturation_roundtrips_and_desaturates(void)
{
    const float c[3] = {0.8f, 0.3f, 0.1f};
    float h;
    float s;
    float l;
    float back[3];
    int i;
    pc_adjustment a;
    pc_compose_buffer b;
    const float green[4] = {0, 1, 0, 1};
    pc_compose_rgb_to_hsl(c, &h, &s, &l);
    pc_compose_hsl_to_rgb(h, s, l, back);
    for (i = 0; i < 3; i++) {
        CHECK(fabsf(back[i] - c[i]) < 1e-5f);
    }
    pc_adjustment_default_hue_saturation(&a);
    a.u.hue_saturation.saturation = -100.0f;
    b = px1(0.8f, 0.3f, 0.1f, 1);
    CHECK(pc_compose_adjust_apply(&a, &b, NULL) == PC_OK);
    CHECK(fabsf(b.px[0][0] - b.px[0][1]) < 1e-5f && fabsf(b.px[0][1] - b.px[0][2]) < 1e-5f);
    pc_compose_buffer_fini(&b);
    pc_adjustment_default_hue_saturation(&a);
    a.u.hue_saturation.hue = 120.0f;
    b = px1(1, 0, 0, 1);
    CHECK(pc_compose_adjust_apply(&a, &b, NULL) == PC_OK);
    CHECK(ch_close4(b.px[0], green));
    pc_compose_buffer_fini(&b);
}

static void test_modern_contrast_matches_photoshop(void)
{
    /* Ground truth from Photoshop 2026 (27.10.0): a 0..255 ramp through modern contrast. */
    static const float cs[4] = {50, -50, 100, -25};
    static const uint8_t out[4][10] = {{0, 11, 23, 52, 87, 128, 169, 204, 233, 255},
                                       {0, 21, 41, 76, 105, 128, 151, 180, 215, 255},
                                       {0, 5, 14, 40, 78, 128, 178, 216, 242, 255},
                                       {0, 19, 37, 70, 101, 128, 155, 186, 220, 255}};
    static const int xs[10] = {0, 16, 32, 64, 96, 128, 160, 192, 224, 255};
    size_t c;
    size_t i;
    for (c = 0; c < 4; c++) {
        for (i = 0; i < 10; i++) {
            float got = pc_compose_modern_contrast((float)xs[i] / 255.0f, cs[c]) * 255.0f;
            CHECK(fabsf(got - (float)out[c][i]) <= 2.0f);
        }
    }
}

static void test_modern_brightness_matches_photoshop(void)
{
    static const float bs[2] = {50, -50};
    static const uint8_t out[2][10] = {{0, 22, 44, 88, 132, 171, 203, 228, 246, 255},
                                       {0, 12, 23, 47, 70, 93, 118, 148, 186, 255}};
    static const int xs[10] = {0, 16, 32, 64, 96, 128, 160, 192, 224, 255};
    size_t b;
    size_t i;
    for (b = 0; b < 2; b++) {
        for (i = 0; i < 10; i++) {
            float got = pc_compose_modern_brightness((float)xs[i] / 255.0f, bs[b]) * 255.0f;
            /* The brightness roll-off is a spline; the fit is approximate at the top. */
            CHECK(fabsf(got - (float)out[b][i]) <= 6.0f);
        }
    }
    CHECK(pc_compose_modern_brightness(0.0f, 120.0f) == 0.0f);
    CHECK(pc_compose_modern_brightness(1.0f, -120.0f) == 1.0f);
    CHECK(pc_compose_modern_brightness(0.37f, 0.0f) == 0.37f);
    CHECK(pc_compose_modern_contrast(0.37f, 0.0f) == 0.37f);
}

static pc_levels_channel ident_g(float g)
{
    pc_levels_channel c = {0.0f, 1.0f, g, 0.0f, 1.0f};
    return c;
}

static void test_levels_matches_photoshop(void)
{
    static const int g2[8][2] = {{1, 4},    {8, 30},    {16, 55},   {32, 90},
                                 {64, 128}, {128, 181}, {192, 221}, {224, 239}};
    static const int xs[6] = {0, 32, 64, 128, 192, 255};
    pc_levels_channel c2 = ident_g(2.0f);
    pc_levels_channel c05 = ident_g(0.5f);
    pc_levels_channel c25 = ident_g(2.5f);
    pc_levels_channel c1 = ident_g(1.0f);
    size_t i;
    for (i = 0; i < 8; i++) {
        float got = roundf(pc_compose_levels(&c2, (float)g2[i][0] / 255.0f) * 255.0f);
        CHECK(fabsf(got - (float)g2[i][1]) <= 4.0f);
    }
    for (i = 0; i < 6; i++) {
        float x = (float)xs[i] / 255.0f;
        float got = roundf(pc_compose_levels(&c05, x) * 255.0f);
        CHECK(got == roundf(x * x * 255.0f));
    }
    CHECK(pc_compose_levels(&c25, 0.0f) == 0.0f);
    CHECK(roundf(pc_compose_levels(&c25, 1.0f) * 255.0f) == 255.0f);
    CHECK(fabsf(pc_compose_levels(&c25, 1.0f) - 1.0f) < 5e-4f);
    CHECK(fabsf(pc_compose_levels(&c1, 0.37f) - 0.37f) < 1e-6f);
}

static void test_levels_work_on_whole_levels(void)
{
    /* psd-tools levels_grayscale: 44..214, gamma 1.78; the stretch is rounded to whole levels
     * before the gamma, so the shadows step unevenly. */
    static const float cases[9][2] = {{44, 0},  {45, 7},  {46, 10},   {47, 17},  {48, 20},
                                      {52, 37}, {69, 88}, {100, 137}, {214, 255}};
    pc_levels_channel ch = {44.0f / 255.0f, 214.0f / 255.0f, 1.78f, 0.0f, 1.0f};
    pc_adjustment a = adj_of(PC_ADJ_LEVELS);
    float *luts = malloc(4 * LUT * sizeof *luts);
    size_t i;
    const float qs[2] = {32768.0f, 0.0f};
    const float bad[3] = {0.0f, NAN, -3.0f};
    if (luts == NULL) {
        abort();
    }
    a.u.levels.master = pc_levels_channel_default();
    a.u.levels.per_channel[0] = a.u.levels.per_channel[1] = a.u.levels.per_channel[2] = ch;
    a.u.levels.black = pc_levels_channel_default();
    CHECK(pc_compose_tone_luts_q(&a, true, 255.0f, luts, NULL) == PC_OK);
    for (i = 0; i < 9; i++) {
        float x = cases[i][0] / 255.0f * 4095.0f;
        size_t k = (size_t)floorf(x);
        float f = x - floorf(x);
        float y = luts[k] * (1.0f - f) + luts[k + 1 < 4095 ? k + 1 : 4095] * f;
        CHECK(fabsf(roundf(y * 255.0f) - cases[i][1]) <= 1.0f);
    }
    /* 16-bit and float stay smooth. */
    for (i = 0; i < 2; i++) {
        float lo = pc_compose_levels_q(&ch, 0.2f, i == 0, qs[i]);
        float hi = pc_compose_levels_q(&ch, 0.2f + 1.0f / 2048.0f, i == 0, qs[i]);
        CHECK(hi > lo && hi - lo < 2.0f / 255.0f);
    }
    /* Hostile quantum values fall back to the continuous curve. */
    for (i = 0; i < 3; i++) {
        CHECK(pc_compose_levels_q(&ch, 0.4f, true, bad[i]) == pc_compose_levels(&ch, 0.4f));
    }
    free(luts);
}

static void test_levels_dont_clip_in_32_bit(void)
{
    /* The oracle corpus' rgb32 levels.psd: input 15..230, gamma 1.3, output 10..245. */
    static const float cases[6][2] = {{0, 0},         {0.0497f, 3},   {0.0976f, 32},
                                      {0.4508f, 140}, {0.9473f, 255}, {1, 255}};
    pc_adjustment a = adj_of(PC_ADJ_LEVELS);
    size_t i;
    a.u.levels.master =
        (pc_levels_channel){15.0f / 255.0f, 230.0f / 255.0f, 1.3f, 10.0f / 255.0f, 245.0f / 255.0f};
    a.u.levels.per_channel[0] = a.u.levels.per_channel[1] = a.u.levels.per_channel[2] =
        pc_levels_channel_default();
    a.u.levels.black = pc_levels_channel_default();
    for (i = 0; i < 8; i++) {
        bool f32 = i < 6;
        float v = f32 ? cases[i][0] : (i == 6 ? 0.0f : 1.0f);
        float want = f32 ? cases[i][1] : (i == 6 ? 10.0f : 245.0f);
        pc_compose_buffer b = px1(v, v, v, 1);
        CHECK(pc_compose_adjust_apply_depth(&a, &b, SRGB, true, f32 ? PC_SAMPLE_TYPE_F32 : PC_SAMPLE_TYPE_U8,
                                            NULL) == PC_OK);
        CHECK(roundf(b.px[0][0] * 255.0f) == want);
        pc_compose_buffer_fini(&b);
    }
}

static void test_exposure_offset_uses_gamma_2_2_in_rgb(void)
{
    pc_adjustment a = adj_of(PC_ADJ_EXPOSURE);
    pc_compose_buffer b = px1(76.0f / 255.0f, 76.0f / 255.0f, 76.0f / 255.0f, 1);
    pc_compose_transfer g = {PC_COMPOSE_TRANSFER_GAMMA, 1.732f};
    pc_compose_transfer e;
    a.u.exposure.offset = 0.1738f;
    a.u.exposure.gamma = 1.0f;
    CHECK(pc_compose_adjust_apply_with(&a, &b, SRGB, NULL) == PC_OK);
    CHECK(roundf(b.px[0][0] * 255.0f) == 134.0f);
    e = pc_compose_transfer_for_exposure(g);
    CHECK(e.kind == PC_COMPOSE_TRANSFER_GAMMA && e.gamma == 1.732f);
    e = pc_compose_transfer_for_document(PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_F32);
    CHECK(e.kind == PC_COMPOSE_TRANSFER_GAMMA && e.gamma == 1.0f);
    e = pc_compose_transfer_for_document(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32);
    CHECK(e.kind == PC_COMPOSE_TRANSFER_GAMMA && e.gamma == 1.0f);
    pc_compose_buffer_fini(&b);
}

static void filter(const float color[3], pc_sample_type depth, const float v[3], float out[3])
{
    pc_adjustment a = adj_of(PC_ADJ_PHOTO_FILTER);
    pc_compose_buffer b = px1(v[0], v[1], v[2], 1);
    int i;
    memcpy(a.u.photo_filter.color, color, 3 * sizeof(float));
    a.u.photo_filter.density = 0.4f;
    a.u.photo_filter.preserve_luminosity = true;
    CHECK(pc_compose_adjust_apply_depth(&a, &b, pc_compose_transfer_for_document(PC_COLOR_MODE_RGB, depth),
                                        true, depth, NULL) == PC_OK);
    for (i = 0; i < 3; i++) {
        out[i] = b.px[0][i] * 255.0f;
    }
    pc_compose_buffer_fini(&b);
}

static bool near3(const float got[3], const float want[3], float tol)
{
    int i;
    for (i = 0; i < 3; i++) {
        if (!(fabsf(got[i] - want[i]) <= tol)) {
            (void)fprintf(stderr, "  got %g %g %g, want %g %g %g\n", (double)got[0], (double)got[1],
                          (double)got[2], (double)want[0], (double)want[1], (double)want[2]);
            return false;
        }
    }
    return true;
}

static void test_photo_filter_matches_photoshop(void)
{
    /* The photoshop corpus photo-filter.psd (Warming Filter (85), density 40 %, Preserve
     * Luminosity): pure blue gains red, white stays white (#442). */
    static const float cases[8][2][3] = {{{1, 0, 0}, {239, 7, 7}},
                                         {{0, 1, 0}, {69, 220, 0}},
                                         {{0, 0, 1}, {46, 0, 130}},
                                         {{1, 1, 0}, {255, 240, 80}},
                                         {{0.5019f, 0.5019f, 0.5019f}, {140, 124, 116}},
                                         {{0.949f, 0.949f, 0.949f}, {255, 238, 229}},
                                         {{1, 1, 1}, {255, 255, 255}},
                                         {{0, 0, 0}, {0, 0, 0}}};
    static const float c32[3][2][3] = {
        {{0.2159f, 0.2159f, 0.2159f}, {66, 53, 40}}, {{1, 1, 0}, {255, 251, 0}}, {{1, 0, 0}, {255, 0, 0}}};
    const float warming[3] = {236.0f / 255.0f, 138.0f / 255.0f, 0.0f};
    const float warming32[3] = {0.9664456f, 0.7618063f, 0.0f};
    const pc_sample_type depths[2] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16};
    size_t d;
    size_t i;
    float got[3];
    for (d = 0; d < 2; d++) {
        for (i = 0; i < 8; i++) {
            filter(warming, depths[d], cases[i][0], got);
            CHECK(near3(got, cases[i][1], 1.0f));
        }
    }
    for (i = 0; i < 3; i++) {
        filter(warming32, PC_SAMPLE_TYPE_F32, c32[i][0], got);
        CHECK(near3(got, c32[i][1], 2.5f));
    }
}

static void vibrance_255(const float c[3], float vibrance, float saturation, float out[3])
{
    pc_adjustment a = adj_of(PC_ADJ_VIBRANCE);
    pc_compose_buffer b = px1(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, 1);
    int i;
    a.u.vibrance.vibrance = vibrance;
    a.u.vibrance.saturation = saturation;
    CHECK(pc_compose_adjust_apply(&a, &b, NULL) == PC_OK);
    for (i = 0; i < 3; i++) {
        out[i] = b.px[0][i] * 255.0f;
    }
    pc_compose_buffer_fini(&b);
}

static void test_vibrance_saturation_matches_photoshop(void)
{
    /* Photoshop 25.4: -100 greys to 0.288 R + 0.712 G in linear light. */
    static const float greys[5][4] = {
        {251, 201, 0, 217}, {255, 0, 0, 146}, {0, 255, 0, 219.4f}, {0, 0, 255, 0}, {255, 255, 0, 255}};
    static const float sats[5][7] = {{255, 128, 0, -50, 220.7f, 155.2f, 129.4f},
                                     {224, 176, 144, -50, 208.6f, 183.9f, 169.9f},
                                     {64, 128, 192, 50, 0.0f, 134.3f, 218.7f},
                                     {224, 176, 144, 100, 251.3f, 158.5f, 51.7f},
                                     {32, 96, 64, 100, 0.0f, 106.8f, 32.8f}};
    size_t i;
    float got[3];
    for (i = 0; i < 5; i++) {
        const float want[3] = {greys[i][3], greys[i][3], greys[i][3]};
        vibrance_255(greys[i], 0.0f, -100.0f, got);
        CHECK(near3(got, want, 0.5f));
    }
    for (i = 0; i < 5; i++) {
        vibrance_255(sats[i], 0.0f, sats[i][3], got);
        CHECK(near3(got, sats[i] + 4, 0.5f));
    }
}

static void test_negative_vibrance_matches_photoshop(void)
{
    static const float cases[7][7] = {{255, 0, 0, -100, 255, 137, 137},
                                      {128, 0, 0, -100, 128, 65.7f, 65.7f},
                                      {224, 176, 144, -100, 213.2f, 197.4f, 188.8f},
                                      {64, 128, 192, -100, 124.4f, 149, 185},
                                      {255, 128, 0, -50, 255, 152, 99.1f},
                                      {251, 201, 0, -50, 251, 208.1f, 97.4f},
                                      {32, 96, 64, -50, 50.1f, 91.7f, 68.6f}};
    const float c[3] = {64, 128, 192};
    const float want[3] = {86.5f, 117.8f, 158.4f};
    size_t i;
    float got[3];
    for (i = 0; i < 7; i++) {
        vibrance_255(cases[i], cases[i][3], 0.0f, got);
        CHECK(near3(got, cases[i] + 4, 0.6f));
    }
    /* Vibrance applies first, then Saturation. */
    vibrance_255(c, 50.0f, -50.0f, got);
    CHECK(near3(got, want, 3.0f));
}

static void test_positive_vibrance_follows_photoshop_closely(void)
{
    static const float cases[5][8] = {{255, 128, 0, 50, 255, 128, 0, 0.5f},
                                      {144, 160, 176, 100, 121.2f, 152.6f, 180.5f, 1.5f},
                                      {64, 128, 192, 50, 50.6f, 125.1f, 192.8f, 4.0f},
                                      {224, 176, 144, 50, 224.9f, 173.4f, 138.0f, 3.0f},
                                      {224, 176, 144, 100, 226.6f, 166.8f, 122.4f, 5.0f}};
    size_t i;
    float got[3];
    for (i = 0; i < 5; i++) {
        vibrance_255(cases[i], cases[i][3], 0.0f, got);
        CHECK(near3(got, cases[i] + 4, cases[i][7]));
    }
}

/* ---- gradient_fill.rs ---- */

static const pc_gradient_style STYLES[5] = {PC_GRADIENT_LINEAR, PC_GRADIENT_RADIAL, PC_GRADIENT_ANGLE,
                                            PC_GRADIENT_REFLECTED, PC_GRADIENT_DIAMOND};

static void test_handles_round_trip(void)
{
    static const float drags[3][2][2] = {{{30, 40}, {180, 10}}, {{100, 50}, {100, 90}}, {{5, 5}, {-40, 70}}};
    pc_rect frame = pc_rect_make(10, -4, 210, 96);
    size_t s;
    size_t i;
    for (s = 0; s < 5; s++) {
        for (i = 0; i < 3; i++) {
            float a;
            float sc;
            float o[2];
            float f2[2];
            float t2[2];
            pc_compose_gradient_from_handles(STYLES[s], drags[i][0], drags[i][1], frame, 0.0f, &a, &sc, o);
            pc_compose_gradient_handles(STYLES[s], a, sc, o, frame, f2, t2);
            CHECK(fabsf(drags[i][0][0] - f2[0]) < 1e-2f && fabsf(drags[i][0][1] - f2[1]) < 1e-2f);
            CHECK(fabsf(drags[i][1][0] - t2[0]) < 1e-2f && fabsf(drags[i][1][1] - t2[1]) < 1e-2f);
        }
    }
}

static void test_handles_map_to_the_ends_of_the_ramp(void)
{
    pc_rect frame = pc_rect_make(0, 0, 100, 60);
    const float f1[2] = {20, 30};
    const float t1[2] = {80, 30};
    const float f2[2] = {40, 20};
    const float t2[2] = {40, 50};
    float a;
    float s;
    float o[2];
    pc_compose_gradient_from_handles(PC_GRADIENT_LINEAR, f1, t1, frame, 0.0f, &a, &s, o);
    /* Linear gradients sample each pixel's top-left corner: the pixel whose corner is the handle
     * is where t is 0 or 1. */
#define T(x) pc_compose_gradient_t(PC_GRADIENT_LINEAR, a, s, false, o, frame, (x) + 0.5f, 30.5f)
    CHECK(fabsf(T(20.0f)) < 1e-4f && fabsf(T(80.0f) - 1.0f) < 1e-4f && fabsf(T(50.0f) - 0.5f) < 1e-4f);
#undef T
    pc_compose_gradient_from_handles(PC_GRADIENT_RADIAL, f2, t2, frame, 0.0f, &a, &s, o);
#define R(x, y) pc_compose_gradient_t(PC_GRADIENT_RADIAL, a, s, false, o, frame, x, y)
    CHECK(R(40.0f, 20.0f) < 1e-4f && fabsf(R(70.0f, 20.0f) - 1.0f) < 1e-3f &&
          fabsf(R(55.0f, 20.0f) - 0.5f) < 1e-3f);
#undef R
}

static void test_ramp_midpoints_and_opacity(void)
{
    pc_color_stop stops[2] = {{1.0f, PC_COLOR_WHITE}, {0.0f, PC_COLOR_BLACK}};
    pc_opacity_stop ops[2] = {{0.0f, 1.0f}, {1.0f, 0.0f}};
    float mids[1] = {0.25f};
    pc_fill f;
    pc_fill solid = pc_fill_solid(PC_COLOR_BLACK);
    pc_compose_ramp r;
    bool found = false;
    float c[4];
    const float end[4] = {1, 1, 1, 0};
    memset(&f, 0, sizeof f);
    f.kind = PC_FILL_GRADIENT;
    f.u.gradient.stops = stops;
    f.u.gradient.nstops = 2;
    f.u.gradient.scale = 1.0f;
    f.u.gradient.opacity_stops = ops;
    f.u.gradient.nopacity_stops = 2;
    f.u.gradient.midpoints = mids;
    f.u.gradient.nmidpoints = 1;
    f.u.gradient.align = true;
    CHECK(pc_compose_ramp_init(&f, &found, &r, NULL) == PC_OK && found);
    /* Unsorted stops are sorted; the midpoint puts 50 % gray at t = 0.25. */
    pc_compose_ramp_sample(&r, 0.25f, c);
    CHECK(fabsf(c[0] - 0.5f) < 1e-5f);
    CHECK(fabsf(c[3] - 0.75f) < 1e-5f);
    pc_compose_ramp_sample(&r, 1.0f, c);
    CHECK(memcmp(c, end, sizeof c) == 0);
    CHECK(pc_compose_midpoint_remap(0.3f, 0.5f) == 0.3f);
    pc_compose_ramp_fini(&r);
    found = true;
    CHECK(pc_compose_ramp_init(&solid, &found, &r, NULL) == PC_OK && !found);
}

static void test_render_handles_empty_and_degenerate_rects(void)
{
    pc_fill f;
    pc_compose_buffer b;
    CHECK(pc_fill_gradient(NULL, 0, 0.0f, 0.0f, PC_GRADIENT_ANGLE, true, &f, NULL) == PC_OK);
    CHECK(pc_compose_gradient_fill_render(&f, pc_rect_make(0, 0, 0, 5), pc_rect_make(0, 0, 0, 0), &b, NULL) ==
          PC_OK);
    CHECK(b.len == 0);
    pc_compose_buffer_fini(&b);
    CHECK(pc_compose_gradient_fill_render(&f, pc_rect_make(0, 0, 3, 2), pc_rect_make(0, 0, 0, 0), &b, NULL) ==
          PC_OK);
    CHECK(b.len == 6);
    pc_compose_buffer_fini(&b);
    pc_fill_fini(&f);
}

/* ---- pattern.rs ---- */

static pc_pattern checker(void)
{
    pc_surface s;
    pc_pattern p;
    const float red[4] = {1, 0, 0, 1};
    const float blue[4] = {0, 0, 1, 1};
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(0, 0, 4, 4), red, 4, NULL) == PC_OK);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(0, 0, 2, 2), blue, 4, NULL) == PC_OK);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(2, 2, 4, 4), blue, 4, NULL) == PC_OK);
    if (pc_pattern_new("checker", 7, &s, 4, 4, &p, NULL) != PC_OK) {
        abort();
    }
    return p;
}

static void test_identity_placement_tiles_exactly(void)
{
    pc_pattern pat = checker();
    pc_compose_patterns *prepared = NULL;
    pc_compose_patterns *uncached = NULL;
    pc_compose_pattern_tile *t = NULL;
    pc_compose_pattern_tile *again = NULL;
    pc_compose_pattern_tile *fallback = NULL;
    pc_compose_placement p;
    pc_compose_buffer a;
    pc_compose_buffer b;
    const float zero[2] = {0, 0};
    const float blue[4] = {0, 0, 1, 1};
    const float red[4] = {1, 0, 0, 1};
    CHECK(pc_compose_patterns_new(&pat, 1, PC_COMPOSE_PREPARED_PATTERN_BYTES, &prepared, NULL) == PC_OK);
    CHECK(pc_compose_patterns_get(prepared, pat.id.ptr, pat.id.len, "", 0, &t, NULL) == PC_OK && t != NULL);
    CHECK(pc_compose_patterns_get(prepared, pat.id.ptr, pat.id.len, "", 0, &again, NULL) == PC_OK);
    CHECK(t == again); /* the cached tile */
    CHECK(pc_compose_patterns_new(&pat, 1, 0, &uncached, NULL) == PC_OK);
    CHECK(pc_compose_patterns_get(uncached, pat.id.ptr, pat.id.len, "", 0, &fallback, NULL) == PC_OK);
    CHECK(fallback != NULL && fallback != t);
    p = pc_compose_placement_new(pc_rect_make(0, 0, 10, 10), false, zero, 1.0f, 0.0f);
    CHECK(pc_compose_pattern_render(t, &p, pc_rect_make(-4, 0, 8, 1), &a, NULL) == PC_OK);
    CHECK(pc_compose_pattern_render(fallback, &p, pc_rect_make(-4, 0, 8, 1), &b, NULL) == PC_OK);
    CHECK(pc_compose_buffer_eq(&a, &b));
    CHECK(memcmp(a.px[0], blue, sizeof blue) == 0); /* x = -4 wraps to 0 */
    CHECK(memcmp(a.px[6], red, sizeof red) == 0);   /* x = 2 */
    CHECK(memcmp(a.px[8], blue, sizeof blue) == 0); /* x = 4 */
    /* A missing pattern is NULL, and a fill with it is transparent. */
    {
        pc_compose_pattern_tile *none = (pc_compose_pattern_tile *)1;
        CHECK(pc_compose_patterns_get(prepared, "nope", 4, "nope", 4, &none, NULL) == PC_OK && none == NULL);
        CHECK(pc_compose_patterns_get(NULL, "nope", 4, "", 0, &none, NULL) == PC_OK && none == NULL);
    }
    pc_compose_buffer_fini(&a);
    pc_compose_buffer_fini(&b);
    pc_compose_pattern_tile_release(t);
    pc_compose_pattern_tile_release(again);
    pc_compose_pattern_tile_release(fallback);
    pc_compose_patterns_free(prepared);
    pc_compose_patterns_free(uncached);
    pc_pattern_fini(&pat);
}

static void test_link_phase_scale_and_angle_move_the_origin(void)
{
    pc_pattern pat = checker();
    pc_compose_pattern_tile *t = NULL;
    pc_compose_placement p;
    pc_compose_buffer b;
    const float zero[2] = {0, 0};
    const float phase[2] = {2, 0};
    const float blue[4] = {0, 0, 1, 1};
    const float red[4] = {1, 0, 0, 1};
    double u;
    double v;
    CHECK(pc_compose_pattern_tile_new(&pat, &t, NULL) == PC_OK && t != NULL);
    p = pc_compose_placement_new(pc_rect_make(2, 0, 10, 10), true, zero, 1.0f, 0.0f);
    CHECK(pc_compose_pattern_render(t, &p, pc_rect_make(2, 0, 3, 1), &b, NULL) == PC_OK);
    CHECK(memcmp(b.px[0], blue, sizeof blue) == 0);
    pc_compose_buffer_fini(&b);
    p = pc_compose_placement_new(PC_RECT_EMPTY, false, phase, 1.0f, 0.0f);
    CHECK(pc_compose_pattern_render(t, &p, pc_rect_make(2, 0, 3, 1), &b, NULL) == PC_OK);
    CHECK(memcmp(b.px[0], blue, sizeof blue) == 0);
    pc_compose_buffer_fini(&b);
    p = pc_compose_placement_new(PC_RECT_EMPTY, false, zero, 2.0f, 0.0f);
    CHECK(pc_compose_pattern_render(t, &p, pc_rect_make(0, 1, 8, 2), &b, NULL) == PC_OK);
    CHECK(memcmp(b.px[2], blue, sizeof blue) == 0);
    CHECK(memcmp(b.px[6], red, sizeof red) == 0);
    pc_compose_buffer_fini(&b);
    /* 90 degrees: x and y swap roles. */
    p = pc_compose_placement_new(PC_RECT_EMPTY, false, zero, 1.0f, 90.0f);
    pc_compose_placement_map(&p, 1.0, 3.0, &u, &v);
    CHECK(fabs(u - -3.0) < 1e-9 && fabs(v - 1.0) < 1e-9);
    pc_compose_pattern_tile_release(t);
    pc_pattern_fini(&pat);
}

int main(int argc, char **argv)
{
    RUN(test_identity_lut_is_identity_both_interpolations);
    RUN(test_selective_color_zero_is_identity_and_ranges_are_local);
    RUN(test_dither_offsets_are_balanced);
    RUN(test_color_lookup_with_an_overflowing_stored_size_is_the_identity);
    RUN(test_identity_tone_in_every_space_is_identity);
    RUN(test_cmyk_black_curve_darkens_and_lab_lightness_lifts);
    RUN(test_hue_ranges_only_touch_their_colours);
    RUN(test_black_white_weights);
    RUN(test_gradient_map_dither_stays_close);
    RUN(test_threshold_posterize_levels_curves);
    RUN(test_hue_saturation_roundtrips_and_desaturates);
    RUN(test_modern_contrast_matches_photoshop);
    RUN(test_modern_brightness_matches_photoshop);
    RUN(test_levels_matches_photoshop);
    RUN(test_levels_work_on_whole_levels);
    RUN(test_levels_dont_clip_in_32_bit);
    RUN(test_exposure_offset_uses_gamma_2_2_in_rgb);
    RUN(test_photo_filter_matches_photoshop);
    RUN(test_vibrance_saturation_matches_photoshop);
    RUN(test_negative_vibrance_matches_photoshop);
    RUN(test_positive_vibrance_follows_photoshop_closely);
    RUN(test_handles_round_trip);
    RUN(test_handles_map_to_the_ends_of_the_ramp);
    RUN(test_ramp_midpoints_and_opacity);
    RUN(test_render_handles_empty_and_degenerate_rects);
    RUN(test_identity_placement_tiles_exactly);
    RUN(test_link_phase_scale_and_angle_move_the_origin);
    return pc_t_finish();
}
