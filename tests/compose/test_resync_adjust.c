/* Tests of the compose resync (R9) for adjustments, blending, fills and patterns: the upstream
 * tests added after C1-C3 in adjust.rs (Photoshop 27.11 measurements: legacy Brightness/Contrast,
 * Color Balance, Hue/Saturation, range Lightness, Black & White tint, absolute Selective Color,
 * Lab-document adjustments, channel-before-master tone order, float HDR adjustments), psblend.rs
 * (special-eight Fill, nested HDR scope), pattern.rs (unpaintable patterns), gradient_fill.rs
 * (native CMYK ramps, quantised dither), proxy.rs (empty vector masks) and tests.rs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ch.h"
#include "color/convert.h"
#include "compose/adjust.h"
#include "compose/gradient_fill.h"
#include "compose/pattern.h"

#define LUT PC_COMPOSE_LUT_SIZE

static const pc_compose_transfer SRGB = {PC_COMPOSE_TRANSFER_SRGB, 0.0f};
static const pc_compose_transfer LINEAR = {PC_COMPOSE_TRANSFER_GAMMA, 1.0f};

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

/* adj (borrowed) applied as apply() does: sRGB, unrounded. */
static void apply1(const pc_adjustment *a, float c[3])
{
    pc_compose_buffer b = px1(c[0], c[1], c[2], 1.0f);
    CHECK(pc_compose_adjust_apply(a, &b, NULL) == PC_OK);
    memcpy(c, b.px[0], 3 * sizeof(float));
    pc_compose_buffer_fini(&b);
}

/* ---- psblend ---- */

static void test_special_eight_fill_matches_photoshop(void)
{
    static const pc_blend_mode modes[8] = {PC_BLEND_COLOR_BURN,   PC_BLEND_LINEAR_BURN, PC_BLEND_COLOR_DODGE,
                                           PC_BLEND_LINEAR_DODGE, PC_BLEND_VIVID_LIGHT, PC_BLEND_LINEAR_LIGHT,
                                           PC_BLEND_HARD_MIX,     PC_BLEND_DIFFERENCE};
    static const unsigned char measured[8][3] = {{176, 132, 88},  {122, 112, 82},  {249, 232, 176},
                                                 {250, 240, 210}, {193, 188, 157}, {171, 202, 192},
                                                 {245, 225, 165}, {150, 60, 10}};
    static const unsigned char q25[8][3] = {{51, 94, 159},  {49, 79, 124},   {76, 132, 186}, {113, 143, 188},
                                            {71, 109, 162}, {100, 100, 130}, {65, 105, 166}, {7, 97, 172}};
    static const unsigned char q75[8][3] = {{30, 0, 33},   {26, 0, 11},   {156, 163, 197}, {217, 187, 202},
                                            {117, 81, 80}, {183, 63, 33}, {102, 0, 47},    {97, 53, 158}};
    const float b[4] = {200 / 255.0f, 150 / 255.0f, 100 / 255.0f, 1};
    const float t[4] = {100 / 255.0f, 180 / 255.0f, 220 / 255.0f, 1};
    const float b2[4] = {60 / 255.0f, 120 / 255.0f, 180 / 255.0f, 1};
    const float t2[4] = {210 / 255.0f, 90 / 255.0f, 30 / 255.0f, 1};
    float got[4];
    float want[4];
    int m;
    int i;
    for (m = 0; m < 8; m++) {
        float tol = modes[m] == PC_BLEND_HARD_MIX ? 3.5f : 1.5f;
        pc_psblend_composite_fill(modes[m], b, t, 1.0f, 128.0f / 255.0f, 1.0f, got);
        for (i = 0; i < 3; i++) {
            CHECK(fabsf(got[i] * 255.0f - measured[m][i]) <= 1.5f);
        }
        /* At 100 % Fill the plain blend; at 0 % the backdrop. */
        pc_psblend_composite_fill(modes[m], b, t, 1.0f, 1.0f, 1.0f, got);
        pc_psblend_composite(modes[m], b, t, 1.0f, want);
        CHECK(memcmp(got, want, sizeof got) == 0);
        pc_psblend_composite_fill(modes[m], b, t, 1.0f, 0.0f, 1.0f, got);
        for (i = 0; i < 3; i++) {
            CHECK(fabsf(got[i] - b[i]) < 1e-5f);
        }
        pc_psblend_composite_fill(modes[m], b2, t2, 1.0f, 64.0f / 255.0f, 1.0f, got);
        for (i = 0; i < 3; i++) {
            CHECK(fabsf(got[i] * 255.0f - q25[m][i]) <= tol);
        }
        pc_psblend_composite_fill(modes[m], b2, t2, 1.0f, 191.0f / 255.0f, 1.0f, got);
        for (i = 0; i < 3; i++) {
            CHECK(fabsf(got[i] * 255.0f - q75[m][i]) <= tol);
        }
    }
    /* Other modes keep Fill as coverage, like Opacity. */
    pc_psblend_composite_fill(PC_BLEND_MULTIPLY, b, t, 0.8f, 0.5f, 1.0f, got);
    pc_psblend_composite(PC_BLEND_MULTIPLY, b, t, 0.4f, want);
    CHECK(memcmp(got, want, sizeof got) == 0);
    /* Over transparency Fill is still coverage for the special eight. */
    {
        const float none[4] = {0, 0, 0, 0};
        pc_psblend_composite_fill(PC_BLEND_LINEAR_DODGE, none, t, 1.0f, 0.5f, 1.0f, got);
        CHECK(fabsf(got[3] - 0.5f) < 1e-6f && fabsf(got[0] - t[0]) < 1e-6f);
    }
}

static void test_hdr_nests_and_restores(void)
{
    const float grey[3] = {0.5f, 0.5f, 0.5f};
    const float red[3] = {1, 0, 0};
    float o[3];
    bool outer = pc_psblend_set_hdr(true);
    pc_psblend_rgb(PC_BLEND_COLOR, grey, red, o);
    CHECK(o[0] > 1.1f); /* a 32-bit document keeps Color's red above 1 */
    {
        bool inner = pc_psblend_set_hdr(false);
        pc_psblend_rgb(PC_BLEND_COLOR, grey, red, o);
        CHECK(o[0] <= 1.0f);
        CHECK(inner);
        (void)pc_psblend_set_hdr(inner);
    }
    CHECK(pc_psblend_hdr());
    (void)pc_psblend_set_hdr(outer);
    CHECK(!pc_psblend_hdr());
}

static void test_rendering_restores_the_lab_mix_flag_it_found(void)
{
    /* #1112: a render clears nothing it didn't set. */
    int mode;
    for (mode = 0; mode < 2; mode++) {
        pc_document d;
        int tile;
        CHECK(pc_document_with_background(&d, "t", pc_size_make(8, 8),
                                          mode == 0 ? PC_COLOR_MODE_LAB : PC_COLOR_MODE_RGB,
                                          PC_SAMPLE_TYPE_U8, PC_COLOR_WHITE, NULL) == PC_OK);
        for (tile = 0; tile < 2; tile++) {
            int outer;
            for (outer = 0; outer < 2; outer++) {
                pc_compose_buffer b;
                (void)pc_psblend_set_lab_mix(outer == 1);
                CHECK(pc_compose_render_tiled(&d, pc_document_bounds(&d), tile == 0 ? 256 : 4, NULL, &b,
                                              NULL) == PC_OK);
                pc_compose_buffer_fini(&b);
                CHECK(pc_psblend_lab_mix() == (outer == 1));
                (void)pc_psblend_set_lab_mix(false);
            }
        }
        pc_document_fini(&d);
    }
}

/* ---- adjustments (Photoshop 27.11 measurements) ---- */

static const unsigned char RAMP17[17] = {0,   16,  32,  48,  64,  80,  96,  112, 128,
                                         144, 160, 176, 192, 208, 224, 240, 255};

static void test_legacy_brightness_contrast_matches_photoshop(void)
{
    static const struct {
        float b;
        float c;
        unsigned char want[17];
    } rows[7] = {
        {40, 0, {40, 56, 72, 88, 104, 120, 136, 152, 168, 184, 200, 216, 232, 248, 255, 255, 255}},
        {-40, 0, {0, 0, 0, 8, 24, 40, 56, 72, 88, 104, 120, 136, 152, 168, 184, 200, 215}},
        {0, 30, {0, 0, 0, 14, 37, 60, 82, 105, 128, 151, 174, 196, 219, 242, 255, 255, 255}},
        {0, -30, {38, 49, 61, 72, 83, 94, 106, 117, 128, 139, 150, 162, 173, 184, 195, 207, 217}},
        {40, 30, {3, 25, 48, 71, 94, 117, 139, 162, 185, 208, 231, 253, 255, 255, 255, 255, 255}},
        {100, 0, {100, 116, 132, 148, 164, 180, 196, 212, 228, 244, 255, 255, 255, 255, 255, 255, 255}},
        {0, 80, {0, 0, 0, 0, 0, 0, 0, 48, 128, 208, 255, 255, 255, 255, 255, 255, 255}},
    };
    int r;
    int i;
    for (r = 0; r < 7; r++) {
        pc_adjustment a = adj_of(PC_ADJ_BRIGHTNESS_CONTRAST);
        a.u.brightness_contrast.brightness = rows[r].b;
        a.u.brightness_contrast.contrast = rows[r].c;
        a.u.brightness_contrast.legacy = true;
        for (i = 0; i < 17; i++) {
            float c[3] = {RAMP17[i] / 255.0f, RAMP17[i] / 255.0f, RAMP17[i] / 255.0f};
            apply1(&a, c);
            CHECK(abs((int)roundf(c[0] * 255.0f) - rows[r].want[i]) <= 1);
        }
    }
}

static void test_color_balance_matches_photoshop(void)
{
    static const int tone[6] = {1, 0, 2, 0, 1, 2};
    static const float amount[6] = {50, -100, 100, 50, 100, -100};
    static const bool preserve[6] = {false, false, false, true, true, true};
    static const unsigned char reds[6][17] = {
        {0, 36, 59, 78, 96, 112, 128, 142, 156, 170, 183, 196, 209, 221, 233, 244, 255},
        {0, 0, 0, 0, 0, 0, 0, 7, 23, 43, 67, 93, 122, 154, 186, 221, 255},
        {0, 51, 84, 111, 136, 160, 182, 202, 223, 242, 255, 255, 255, 255, 255, 255, 255},
        {0, 16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240, 255},
        {0, 36, 59, 78, 96, 112, 128, 142, 156, 170, 183, 196, 209, 221, 233, 244, 255},
        {0, 16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240, 255}};
    static const unsigned char greens[6][17] = {
        {0, 16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240, 255},
        {0, 16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240, 255},
        {0, 16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240, 255},
        {0, 0, 0, 0, 17, 37, 57, 77, 97, 117, 137, 157, 177, 197, 216, 236, 255},
        {0, 5, 14, 24, 36, 50, 64, 80, 97, 114, 132, 151, 171, 191, 212, 234, 255},
        {0, 26, 53, 79, 105, 132, 158, 184, 211, 237, 255, 255, 255, 255, 255, 255, 255}};
    int r;
    int i;
    for (r = 0; r < 6; r++) {
        float tones[3][3] = {{0}};
        tones[tone[r]][0] = amount[r];
        for (i = 0; i < 17; i++) {
            float v = RAMP17[i] / 255.0f;
            int red = (int)roundf(
                pc_compose_color_balance_channel((const float (*)[3])tones, preserve[r], 0, v) * 255.0f);
            int green = (int)roundf(
                pc_compose_color_balance_channel((const float (*)[3])tones, preserve[r], 1, v) * 255.0f);
            CHECK(abs(red - reds[r][i]) <= 1);
            CHECK(abs(green - greens[r][i]) <= 1);
        }
    }
    /* Two sliders in one tone, Preserve Luminosity on. */
    {
        float t1[3][3] = {{50, 0, 50}, {0}, {0}};
        float t2[3][3] = {{0}, {0}, {-50, 50, 0}};
        int want1[3] = {128, 97, 128};
        int want2[3] = {128, 211, 159};
        for (i = 0; i < 3; i++) {
            int g1 = (int)roundf(
                pc_compose_color_balance_channel((const float (*)[3])t1, true, i, 128 / 255.0f) * 255.0f);
            int g2 = (int)roundf(
                pc_compose_color_balance_channel((const float (*)[3])t2, true, i, 128 / 255.0f) * 255.0f);
            CHECK(g1 == want1[i]);
            CHECK(abs(g2 - want2[i]) <= 1);
        }
    }
}

static void test_hue_saturation_and_range_lightness_match_photoshop(void)
{
    static const struct {
        unsigned char in[3];
        float hue;
        float sat;
        float light;
        bool colorize;
        unsigned char want[3];
    } cases[8] = {
        {{204, 102, 102}, 0, 50, 0, false, {254, 51, 51}},
        {{204, 159, 102}, 0, 50, 0, false, {254, 164, 51}},
        {{204, 102, 102}, 0, -50, 0, false, {178, 127, 127}},
        {{0, 0, 0}, 120, 100, 30, true, {0, 152, 0}},
        {{28, 28, 28}, 120, 100, 30, true, {0, 192, 0}},
        {{160, 160, 160}, 30, 25, -40, true, {120, 96, 73}},
        {{252, 252, 252}, 30, 25, -40, true, {177, 151, 126}},
        {{255, 143, 0}, 30, 25, -40, true, {95, 76, 58}},
    };
    static const struct {
        unsigned char in[3];
        float l;
        unsigned char want[3];
    } ranges[5] = {{{0, 29, 115}, 0.5f, {57, 71, 115}},
                   {{0, 64, 255}, 0.5f, {125, 157, 255}},
                   {{102, 128, 204}, 0.5f, {152, 165, 204}},
                   {{102, 128, 204}, -0.5f, {102, 115, 154}},
                   {{0, 18, 115}, -0.5f, {0, 9, 58}}};
    int k;
    int i;
    for (k = 0; k < 8; k++) {
        pc_adjustment a = adj_of(PC_ADJ_HUE_SATURATION);
        float c[3];
        a.u.hue_saturation.hue = cases[k].hue;
        a.u.hue_saturation.saturation = cases[k].sat;
        a.u.hue_saturation.lightness = cases[k].light;
        a.u.hue_saturation.colorize = cases[k].colorize;
        pc_hue_range_defaults(a.u.hue_saturation.ranges);
        for (i = 0; i < 3; i++) {
            c[i] = cases[k].in[i] / 255.0f;
        }
        apply1(&a, c);
        for (i = 0; i < 3; i++) {
            CHECK(fabsf(c[i] * 255.0f - cases[k].want[i]) <= 2.0f);
        }
    }
    for (k = 0; k < 5; k++) {
        float c[3];
        float o[3];
        for (i = 0; i < 3; i++) {
            c[i] = ranges[k].in[i] / 255.0f;
        }
        pc_compose_range_lightness(c, ranges[k].l, o);
        for (i = 0; i < 3; i++) {
            CHECK(fabsf(o[i] * 255.0f - ranges[k].want[i]) <= 2.5f);
        }
    }
}

static void test_colorize_at_zero_saturation_is_grey(void)
{
    /* #2326: Colorize at 0 % saturation leaves only the lightness; low saturations still tint. */
    static const float px[3][3] = {{0.9f, 0.2f, 0.1f}, {0.1f, 0.6f, 0.8f}, {0.5f, 0.5f, 0.5f}};
    static const float lights[3] = {0, 30, -30};
    int l;
    int p;
    float t10;
    float t25;
    for (l = 0; l < 3; l++) {
        for (p = 0; p < 3; p++) {
            float o[3];
            pc_compose_hue_saturation(px[p], 200.0f, 0.0f, lights[l] / 100.0f, true, o);
            CHECK(fabsf(o[0] - o[1]) < 1e-4f && fabsf(o[1] - o[2]) < 1e-4f);
        }
    }
    {
        const float g[3] = {0.5f, 0.5f, 0.5f};
        float o[3];
        pc_compose_hue_saturation(g, 0.0f, 0.10f, 0.0f, true, o);
        t10 = o[0] - o[2];
        pc_compose_hue_saturation(g, 0.0f, 0.25f, 0.0f, true, o);
        t25 = o[0] - o[2];
    }
    CHECK(t10 > 0.05f && t10 < t25);
}

static void test_black_white_tint_matches_photoshop(void)
{
    static const struct {
        unsigned char tint[3];
        unsigned char grey;
        unsigned char want[3];
    } cases[6] = {{{255, 0, 0}, 24, {81, 1, 1}},         {{255, 0, 0}, 120, {255, 64, 64}},
                  {{255, 0, 0}, 240, {255, 235, 235}},   {{0xe1, 0xd3, 0xb3}, 120, {133, 119, 87}},
                  {{0xe1, 0xd3, 0xb3}, 24, {33, 24, 1}}, {{0x40, 0x20, 0x10}, 120, {144, 112, 96}}};
    static const float weights[6] = {40, 60, 40, 60, 20, 80};
    int k;
    int i;
    for (k = 0; k < 6; k++) {
        pc_adjustment a = adj_of(PC_ADJ_BLACK_WHITE);
        float c[3];
        memcpy(a.u.black_white.weights, weights, sizeof weights);
        a.u.black_white.has_tint = true;
        for (i = 0; i < 3; i++) {
            a.u.black_white.tint[i] = cases[k].tint[i] / 255.0f;
            c[i] = cases[k].grey / 255.0f;
        }
        apply1(&a, c);
        for (i = 0; i < 3; i++) {
            CHECK(fabsf(c[i] * 255.0f - cases[k].want[i]) <= 2.5f);
        }
    }
}

static void test_absolute_selective_color_matches_photoshop(void)
{
    static const struct {
        int range;
        float cyan;
        unsigned char grey;
        unsigned char want;
    } cases[8] = {{7, 50, 64, 32},    {7, 50, 192, 129},  {7, -50, 192, 223}, {7, -50, 64, 128},
                  {6, -50, 192, 224}, {6, -50, 240, 253}, {8, 50, 64, 32},    {8, 50, 16, 2}};
    int k;
    for (k = 0; k < 8; k++) {
        float adj[9][4] = {{0}};
        float g = cases[k].grey / 255.0f;
        const float c[3] = {g, g, g};
        float o[3];
        adj[cases[k].range][0] = cases[k].cyan;
        pc_compose_selective_color(c, false, (const float (*)[4])adj, o);
        CHECK(fabsf(o[0] * 255.0f - cases[k].want) <= 1.5f);
    }
}

static pc_curve line_curve(float a, float b)
{
    pc_curve c;
    c.pts = malloc(2 * sizeof *c.pts);
    if (c.pts == NULL) {
        abort();
    }
    c.len = 2;
    c.pts[0].input = 0.0f;
    c.pts[0].output = a;
    c.pts[1].input = 1.0f;
    c.pts[1].output = b;
    return c;
}

static void test_levels_and_curves_apply_each_channel_before_the_master(void)
{
    /* channel(x) = 1/2 + x/2, then master(x) = x/2: 0 maps to 0.25 (#975). */
    pc_adjustment lv = adj_of(PC_ADJ_LEVELS);
    pc_adjustment cv = adj_of(PC_ADJ_CURVES);
    float *ll = malloc(4 * LUT * sizeof *ll);
    float *cl = malloc(4 * LUT * sizeof *cl);
    int ch;
    int d;
    if (ll == NULL || cl == NULL) {
        abort();
    }
    lv.u.levels.master = pc_levels_channel_default();
    lv.u.levels.master.out_white = 0.5f;
    for (ch = 0; ch < 3; ch++) {
        lv.u.levels.per_channel[ch] = pc_levels_channel_default();
        lv.u.levels.per_channel[ch].out_black = 0.5f;
        cv.u.curves.per_channel[ch] = line_curve(0.5f, 1.0f);
    }
    lv.u.levels.black = pc_levels_channel_default();
    cv.u.curves.master = line_curve(0.0f, 0.5f);
    for (d = 0; d < 4; d++) {
        static const pc_sample_type ds[4] = {PC_SAMPLE_TYPE_F32, PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16,
                                             PC_SAMPLE_TYPE_F32};
        bool has = d > 0;
        size_t idx[5] = {0, LUT / 4, LUT / 2, 3 * LUT / 4, LUT - 1};
        int k;
        CHECK(pc_compose_tone_luts_depth(&lv, has, ds[d], ll, NULL) == PC_OK);
        CHECK(pc_compose_tone_luts_depth(&cv, has, ds[d], cl, NULL) == PC_OK);
        for (ch = 0; ch < 3; ch++) {
            for (k = 0; k < 5; k++) {
                CHECK(fabsf(ll[(size_t)ch * LUT + idx[k]] - cl[(size_t)ch * LUT + idx[k]]) < 0.01f);
            }
        }
        CHECK(fabsf(cl[0] - 0.25f) < 0.01f);
        for (k = 0; k < 2; k++) {
            pc_compose_buffer b = px1(0, 0, 0, 1);
            CHECK(pc_compose_adjust_apply_depth(k == 0 ? &lv : &cv, &b, SRGB, has, ds[d], NULL) == PC_OK);
            for (ch = 0; ch < 3; ch++) {
                CHECK(fabsf(b.px[0][ch] - 0.25f) < 0.01f);
            }
            pc_compose_buffer_fini(&b);
        }
    }
    free(ll);
    free(cl);
    pc_adjustment_fini(&cv);
}

static void test_pencil_curves_and_contour_corners(void)
{
    /* A pencil map is straight segments; corners split a contour into its own splines. */
    pc_curve_point pts[3] = {{0.0f, 0.0f}, {0.5f, 1.0f}, {1.0f, 0.0f}};
    bool corners[3] = {false, true, false};
    float *a = malloc(LUT * sizeof *a);
    float *b = malloc(LUT * sizeof *b);
    if (a == NULL || b == NULL) {
        abort();
    }
    CHECK(pc_compose_curve_map_lut(pts, 3, a, NULL) == PC_OK);
    CHECK(fabsf(a[LUT / 4] - 0.5f) < 1e-3f && fabsf(a[3 * LUT / 4] - 0.5f) < 1e-3f);
    CHECK(pc_compose_curve_lut_mode(pts, 3, true, b, NULL) == PC_OK && memcmp(a, b, LUT * sizeof *a) == 0);
    /* All-corner points make a polyline: the same tent. */
    CHECK(pc_compose_contour_curve_lut(pts, 3, corners, 3, b, NULL) == PC_OK);
    CHECK(fabsf(b[LUT / 4] - 0.5f) < 1e-3f && fabsf(b[3 * LUT / 4] - 0.5f) < 1e-3f);
    /* No corner flags: the plain spline (overshoots past the straight line). */
    CHECK(pc_compose_contour_curve_lut(pts, 3, NULL, 0, b, NULL) == PC_OK);
    CHECK(pc_compose_curve_lut(pts, 3, a, NULL) == PC_OK && memcmp(a, b, LUT * sizeof *a) == 0);
    CHECK(b[LUT / 4] > 0.55f);
    /* Hostile maps: non-finite points are dropped, no points is the identity. */
    pts[1].input = NAN;
    CHECK(pc_compose_curve_map_lut(pts, 3, a, NULL) == PC_OK);
    CHECK(pc_compose_curve_map_lut(pts, 0, a, NULL) == PC_OK && a[LUT - 1] == 1.0f && a[0] == 0.0f);
    free(a);
    free(b);
}

/* ---- float documents ---- */

static void test_float_adjustments_preserve_hdr_range(void)
{
    pc_adjustment ex = adj_of(PC_ADJ_EXPOSURE);
    pc_adjustment lv = adj_of(PC_ADJ_LEVELS);
    pc_compose_buffer b;
    int i;
    ex.u.exposure.exposure = 1.0f;
    ex.u.exposure.gamma = 1.0f;
    b = px1(4.0f, 2.0f, 0.5f, 0.75f);
    CHECK(pc_compose_adjust_apply_depth(&ex, &b, LINEAR, true, PC_SAMPLE_TYPE_F32, NULL) == PC_OK);
    CHECK(b.px[0][0] == 8.0f && b.px[0][1] == 4.0f && b.px[0][2] == 1.0f && b.px[0][3] == 0.75f);
    pc_compose_buffer_fini(&b);
    /* Identity Levels leave samples outside 0..1 alone; channel before master outside the range. */
    lv.u.levels.master = pc_levels_channel_default();
    for (i = 0; i < 3; i++) {
        lv.u.levels.per_channel[i] = pc_levels_channel_default();
    }
    lv.u.levels.black = pc_levels_channel_default();
    b = px1(-0.1f, 4.0f, 0.5f, 0.75f);
    CHECK(pc_compose_adjust_apply_depth(&lv, &b, LINEAR, true, PC_SAMPLE_TYPE_F32, NULL) == PC_OK);
    CHECK(b.px[0][0] == -0.1f && b.px[0][1] == 4.0f && b.px[0][2] == 0.5f);
    pc_compose_buffer_fini(&b);
    for (i = 0; i < 3; i++) {
        lv.u.levels.per_channel[i].in_black = 0.5f;
        lv.u.levels.per_channel[i].in_white = 1.5f;
    }
    lv.u.levels.master.out_black = -0.1f;
    lv.u.levels.master.out_white = 1.5f;
    b = px1(4.0f, 2.0f, -0.1f, 1.0f);
    CHECK(pc_compose_adjust_apply_depth(&lv, &b, LINEAR, true, PC_SAMPLE_TYPE_F32, NULL) == PC_OK);
    CHECK(fabsf(b.px[0][0] - 5.5f) < 1e-6f && fabsf(b.px[0][1] - 2.3f) < 1e-6f &&
          fabsf(b.px[0][2] + 1.06f) < 1e-6f);
    pc_compose_buffer_fini(&b);
}

static void test_float_exposure_round_trips_and_keeps_signed_gamma(void)
{
    pc_adjustment ex = adj_of(PC_ADJ_EXPOSURE);
    pc_compose_buffer b = px1(4.0f, -2.0f, 0.5f, 0.75f);
    const float want[4] = {-sqrtf(1.25f), -0.5f, sqrtf(7.75f), 0.75f};
    int i;
    ex.u.exposure.gamma = 1.0f;
    ex.u.exposure.exposure = 1.0f;
    CHECK(pc_compose_adjust_apply_depth(&ex, &b, LINEAR, true, PC_SAMPLE_TYPE_F32, NULL) == PC_OK);
    ex.u.exposure.exposure = -1.0f;
    CHECK(pc_compose_adjust_apply_depth(&ex, &b, LINEAR, true, PC_SAMPLE_TYPE_F32, NULL) == PC_OK);
    CHECK(b.px[0][0] == 4.0f && b.px[0][1] == -2.0f && b.px[0][2] == 0.5f);
    pc_compose_buffer_fini(&b);
    b = px1(-0.5f, 0.0f, 4.0f, 0.75f);
    ex.u.exposure.exposure = 1.0f;
    ex.u.exposure.offset = -0.25f;
    ex.u.exposure.gamma = 2.0f;
    CHECK(pc_compose_adjust_apply_depth(&ex, &b, LINEAR, true, PC_SAMPLE_TYPE_F32, NULL) == PC_OK);
    for (i = 0; i < 4; i++) {
        CHECK(fabsf(b.px[0][i] - want[i]) < 1e-6f);
    }
    pc_compose_buffer_fini(&b);
}

/* ---- Lab documents ---- */

static void lab_apply(const pc_adjustment *a, const float lab[3], float out_lab[3])
{
    float rgb[3];
    pc_compose_buffer b;
    pc_lab_to_srgb(lab, rgb);
    b = px1(rgb[0], rgb[1], rgb[2], 1.0f);
    CHECK(pc_compose_adjust_apply_in(a, &b, SRGB, true, PC_SAMPLE_TYPE_U8, PC_COLOR_MODE_LAB, NULL) == PC_OK);
    pc_srgb_to_lab(b.px[0], out_lab);
    pc_compose_buffer_fini(&b);
}

static bool near3(const float a[3], const float b[3], float tol)
{
    return fabsf(a[0] - b[0]) <= tol && fabsf(a[1] - b[1]) <= tol && fabsf(a[2] - b[2]) <= tol;
}

static void test_lab_documents_evaluate_on_l_a_and_b(void)
{
    pc_adjustment inv = adj_of(PC_ADJ_INVERT);
    pc_adjustment th = adj_of(PC_ADJ_THRESHOLD);
    pc_adjustment bc = adj_of(PC_ADJ_BRIGHTNESS_CONTRAST);
    pc_adjustment gm = adj_of(PC_ADJ_GRADIENT_MAP);
    pc_gradient_map_stop stops[2] = {{0.0f, {1, 0, 0}}, {1.0f, {0, 0, 1}}};
    const float c1[3] = {30, 12, -18};
    float o[3];
    int legacy;
    lab_apply(&inv, c1, o);
    CHECK(near3(o, (const float[3]){70, -13, 17}, 0.6f));
    /* Threshold splits on L with a neutral result. */
    th.u.threshold.level = 0.5f;
    lab_apply(&th, (const float[3]){52, 40, 30}, o);
    CHECK(near3(o, (const float[3]){100, 0, 0}, 0.5f));
    lab_apply(&th, (const float[3]){48, -40, 30}, o);
    CHECK(near3(o, (const float[3]){0, 0, 0}, 0.5f));
    /* Brightness/Contrast moves L only. */
    for (legacy = 0; legacy < 2; legacy++) {
        bc.u.brightness_contrast.brightness = 40;
        bc.u.brightness_contrast.contrast = 30;
        bc.u.brightness_contrast.legacy = legacy == 1;
        lab_apply(&bc, (const float[3]){45, 15, -20}, o);
        CHECK(o[0] > 50.0f && fabsf(o[1] - 15) < 0.5f && fabsf(o[2] + 20) < 0.5f);
    }
    /* Gradient Map follows L: a grey and a green of the same L map alike. */
    gm.u.gradient_map.stops = stops;
    gm.u.gradient_map.nstops = 2;
    {
        float g[3];
        float want[3];
        const float rgb[3] = {0.4f, 0.0f, 0.6f};
        lab_apply(&gm, (const float[3]){60, 0, 0}, g);
        lab_apply(&gm, (const float[3]){60, -50, 40}, o);
        CHECK(near3(g, o, 1e-3f));
        pc_srgb_to_lab(rgb, want);
        CHECK(near3(g, want, 0.05f));
    }
    /* Other modes and adjustments keep the screen colour path; odd values never crash. */
    {
        pc_compose_buffer a = px1(0.2f, 0.5f, 0.7f, 1);
        pc_compose_buffer b = px1(0.2f, 0.5f, 0.7f, 1);
        pc_compose_buffer odd = px1(NAN, INFINITY, -3.0f, 1);
        CHECK(pc_compose_adjust_apply_in(&inv, &a, SRGB, false, PC_SAMPLE_TYPE_F32, PC_COLOR_MODE_RGB,
                                         NULL) == PC_OK);
        CHECK(pc_compose_adjust_apply_depth(&inv, &b, SRGB, false, PC_SAMPLE_TYPE_F32, NULL) == PC_OK);
        CHECK(memcmp(a.px, b.px, sizeof(float[4])) == 0);
        CHECK(!pc_compose_lab_channel_adjustment(&(pc_adjustment){.kind = PC_ADJ_HUE_SATURATION}));
        gm.u.gradient_map.nstops = 0;
        gm.u.gradient_map.dither = true;
        CHECK(pc_compose_adjust_apply_in(&gm, &odd, SRGB, false, PC_SAMPLE_TYPE_F32, PC_COLOR_MODE_LAB,
                                         NULL) == PC_OK);
        CHECK(pc_compose_adjust_apply_in(&th, &odd, SRGB, false, PC_SAMPLE_TYPE_F32, PC_COLOR_MODE_LAB,
                                         NULL) == PC_OK);
        pc_compose_buffer_fini(&a);
        pc_compose_buffer_fini(&b);
        pc_compose_buffer_fini(&odd);
    }
}

static void test_lab_document_adjustment_layers_render_at_every_depth(void)
{
    const float lab[3] = {30, 12, -18};
    float rgb[3];
    int d;
    pc_lab_to_srgb(lab, rgb);
    for (d = 0; d < 3; d++) {
        static const pc_sample_type ds[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
        pc_document doc;
        pc_layer l;
        pc_layer_content c;
        float p[4];
        float o[3];
        CHECK(pc_document_with_background(&doc, "t", pc_size_make(4, 4), PC_COLOR_MODE_LAB, ds[d],
                                          pc_color_rgb(rgb[0], rgb[1], rgb[2]), NULL) == PC_OK);
        memset(&c, 0, sizeof c);
        c.kind = PC_LAYER_ADJUSTMENT;
        c.u.adjustment.kind = PC_ADJ_INVERT;
        CHECK(pc_layer_init(&l, "adj", &c, NULL) == PC_OK);
        ch_push(&doc, l);
        ch_px(&doc, 1, 1, p);
        pc_srgb_to_lab(p, o);
        CHECK(near3(o, (const float[3]){70, -13, 17}, 1.0f));
        pc_document_fini(&doc);
    }
}

/* ---- fills and patterns ---- */

static void test_native_cmyk_ramp_interpolates_black_and_alpha_separately(void)
{
    pc_color_stop stops[2];
    pc_fill f;
    pc_compose_ramp r;
    bool found = false;
    float v[5];
    float d[5];
    const float want[5] = {0.2f, 0.4f, 0.6f, 0.8f, 0.5625f};
    float mids[1] = {0.25f};
    pc_opacity_stop ops[2] = {{1.0f, 0.0f}, {0.0f, 1.0f}};
    float noise;
    int i;
    stops[0].pos = 1.0f;
    stops[0].color = (pc_color){PC_COLOR_MODE_CMYK, {0.4f, 0.6f, 0.8f, 1.0f}, 1.0f};
    stops[1].pos = 0.0f;
    stops[1].color = (pc_color){PC_COLOR_MODE_CMYK, {0.0f, 0.2f, 0.4f, 0.6f}, 0.5f};
    CHECK(pc_fill_gradient(stops, 2, 0.0f, 1.0f, PC_GRADIENT_LINEAR, false, &f, NULL) == PC_OK);
    free(f.u.gradient.midpoints);
    free(f.u.gradient.opacity_stops);
    f.u.gradient.midpoints = mids;
    f.u.gradient.nmidpoints = 1;
    f.u.gradient.opacity_stops = ops;
    f.u.gradient.nopacity_stops = 2;
    CHECK(pc_compose_ramp_init_cmyk(&f, &found, &r, NULL) == PC_OK && found && r.nch == 5);
    pc_compose_ramp_sample(&r, 0.25f, v);
    for (i = 0; i < 5; i++) {
        CHECK(fabsf(v[i] - want[i]) < 1e-6f);
    }
    memcpy(d, v, sizeof d);
    pc_compose_gradient_dither_to(d, 4, 3, 7, false, 0.0f);
    CHECK(d[4] == want[4]);
    noise = (pc_dither_noise(3, 7) - 0.5f) / 255.0f;
    for (i = 0; i < 4; i++) {
        CHECK(fabsf(d[i] - want[i] - noise) < 1e-6f);
    }
    pc_compose_ramp_fini(&r);
    f.u.gradient.midpoints = NULL;
    f.u.gradient.nmidpoints = 0;
    f.u.gradient.opacity_stops = NULL;
    f.u.gradient.nopacity_stops = 0;
    pc_fill_fini(&f);
}

static void test_dithered_gradient_fill_is_rounded_to_the_document_depth(void)
{
    pc_color_stop stops[2] = {{0.0f, {PC_COLOR_MODE_RGB, {0.1f, 0.4f, 0.0f, 0}, 1}},
                              {1.0f, {PC_COLOR_MODE_RGB, {0.9f, 0.2f, 1.0f, 0}, 1}}};
    pc_fill f;
    pc_rect rect = pc_rect_make(0, 0, 64, 8);
    pc_compose_buffer a;
    pc_compose_buffer b;
    int d;
    CHECK(pc_fill_gradient(stops, 2, 0.0f, 1.0f, PC_GRADIENT_LINEAR, false, &f, NULL) == PC_OK);
    f.u.gradient.dither = true;
    for (d = 0; d < 2; d++) {
        float q = d == 0 ? 255.0f : 32768.0f;
        size_t i;
        int c;
        bool on = true;
        CHECK(pc_compose_gradient_fill_render_quantized(&f, rect, rect, true, q, &a, NULL) == PC_OK);
        for (i = 0; i < a.len; i++) {
            for (c = 0; c < 3; c++) {
                on = on && roundf(a.px[i][c] * q) / q == a.px[i][c];
            }
        }
        CHECK(on);
        pc_compose_buffer_fini(&a);
    }
    CHECK(pc_compose_gradient_fill_render_quantized(&f, rect, rect, false, 0.0f, &a, NULL) == PC_OK);
    CHECK(pc_compose_gradient_fill_render(&f, rect, rect, &b, NULL) == PC_OK);
    CHECK(pc_compose_buffer_eq(&a, &b));
    pc_compose_buffer_fini(&a);
    pc_compose_buffer_fini(&b);
    pc_fill_fini(&f);
}

static void test_dithered_gradient_fills_under_color_dodge_keep_black_channels(void)
{
    /* #2755: on an 8-bit document the dithered pixels sit on levels, so a channel at 0 stays 0. */
    pc_document d = ch_doc_white(128, 32);
    pc_color_stop s1[2] = {{0.0f, {PC_COLOR_MODE_RGB, {1, 0, 0, 0}, 1}},
                           {1.0f, {PC_COLOR_MODE_RGB, {0, 0, 1, 0}, 1}}};
    pc_color_stop s2[2] = {{0.0f, {PC_COLOR_MODE_RGB, {0.88f, 0.88f, 0.88f, 0}, 1}},
                           {1.0f, {PC_COLOR_MODE_RGB, {0.97f, 0.97f, 0.97f, 0}, 1}}};
    pc_layer_content c;
    pc_layer l;
    pc_compose_buffer b;
    size_t i;
    bool ok = true;
    c.kind = PC_LAYER_FILL;
    CHECK(pc_fill_gradient(s1, 2, 0.0f, 1.0f, PC_GRADIENT_LINEAR, false, &c.u.fill, NULL) == PC_OK);
    c.u.fill.u.gradient.dither = true;
    CHECK(pc_layer_init(&l, "bottom", &c, NULL) == PC_OK);
    ch_push(&d, l);
    c.kind = PC_LAYER_FILL;
    CHECK(pc_fill_gradient(s2, 2, 90.0f, 1.0f, PC_GRADIENT_LINEAR, false, &c.u.fill, NULL) == PC_OK);
    c.u.fill.u.gradient.dither = true;
    CHECK(pc_layer_init(&l, "top", &c, NULL) == PC_OK);
    l.blend = PC_BLEND_COLOR_DODGE;
    ch_push(&d, l);
    CHECK(pc_compose_flatten(&d, &b, NULL) == PC_OK);
    for (i = 0; i < b.len; i++) {
        ok = ok && b.px[i][1] == 0.0f;
    }
    CHECK(ok);
    pc_compose_buffer_fini(&b);
    pc_document_fini(&d);
}

static void test_patterns_too_large_to_paint_are_skipped_without_allocating(void)
{
    pc_pattern p;
    pc_compose_pattern_tile *t = (pc_compose_pattern_tile *)1;
    pc_compose_patterns *pp = NULL;
    float (*px)[4] = (float (*)[4])1;
    size_t n = 7;
    memset(&p, 0, sizeof p);
    CHECK(pc_str_set(&p.id, "big", 3, NULL) == PC_OK);
    p.width = 9000;
    p.height = 8000;
    pc_surface_init(&p.surface, PC_PIXEL_FORMAT_RGBA8);
    CHECK(!pc_pattern_is_paintable(&p));
    CHECK(pc_compose_pattern_tile_new(&p, &t, NULL) == PC_OK && t == NULL);
    CHECK(pc_compose_pattern_rgba(&p, PC_COLOR_MODE_RGB, &px, &n, NULL) == PC_OK && px == NULL && n == 0);
    CHECK(pc_compose_patterns_new(&p, 1, 0, &pp, NULL) == PC_OK);
    t = (pc_compose_pattern_tile *)1;
    CHECK(pc_compose_patterns_get(pp, "big", 3, "", 0, &t, NULL) == PC_OK && t == NULL);
    pc_compose_patterns_free(pp);
    pc_pattern_fini(&p);
}

static void test_patterns_paint_in_the_document_mode(void)
{
    /* An RGB red pattern in a Grayscale document paints grey. */
    pc_pattern p;
    float (*px)[4] = NULL;
    size_t n = 0;
    const float red[4] = {1, 0, 0, 1};
    memset(&p, 0, sizeof p);
    p.width = 2;
    p.height = 2;
    pc_surface_init(&p.surface, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_surface_fill_rect(&p.surface, pc_rect_make(0, 0, 2, 2), red, 4, NULL) == PC_OK);
    CHECK(pc_compose_pattern_rgba(&p, PC_COLOR_MODE_GRAYSCALE, &px, &n, NULL) == PC_OK && n == 4);
    CHECK(px != NULL && fabsf(px[0][0] - px[0][1]) < 1e-6f && fabsf(px[0][1] - px[0][2]) < 1e-6f);
    free(px);
    CHECK(pc_compose_pattern_rgba(&p, PC_COLOR_MODE_RGB, &px, &n, NULL) == PC_OK && n == 4);
    CHECK(px != NULL && px[0][0] == 1.0f && px[0][1] == 0.0f);
    free(px);
    pc_pattern_fini(&p);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_special_eight_fill_matches_photoshop);
    RUN(test_hdr_nests_and_restores);
    RUN(test_rendering_restores_the_lab_mix_flag_it_found);
    RUN(test_legacy_brightness_contrast_matches_photoshop);
    RUN(test_color_balance_matches_photoshop);
    RUN(test_hue_saturation_and_range_lightness_match_photoshop);
    RUN(test_colorize_at_zero_saturation_is_grey);
    RUN(test_black_white_tint_matches_photoshop);
    RUN(test_absolute_selective_color_matches_photoshop);
    RUN(test_levels_and_curves_apply_each_channel_before_the_master);
    RUN(test_pencil_curves_and_contour_corners);
    RUN(test_float_adjustments_preserve_hdr_range);
    RUN(test_float_exposure_round_trips_and_keeps_signed_gamma);
    RUN(test_lab_documents_evaluate_on_l_a_and_b);
    RUN(test_lab_document_adjustment_layers_render_at_every_depth);
    RUN(test_native_cmyk_ramp_interpolates_black_and_alpha_separately);
    RUN(test_dithered_gradient_fill_is_rounded_to_the_document_depth);
    RUN(test_dithered_gradient_fills_under_color_dodge_keep_black_channels);
    RUN(test_patterns_too_large_to_paint_are_skipped_without_allocating);
    RUN(test_patterns_paint_in_the_document_mode);
    return pc_t_finish();
}
