/* Tests for src/algo/selection*.c (ports of the #[test]s in crates/algo/src/selection.rs) plus
 * never-crash checks. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "base/num.h"
#include "base/rmath.h"
#include "harness.h"

static float (*img_fn(size_t w, size_t h, void (*f)(size_t, size_t, float *)))[4]
{
    float (*v)[4] = malloc(w * h * sizeof *v);
    size_t x, y;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            f(x, y, v[y * w + x]);
        }
    }
    return v;
}

static size_t count_pos(const float *m, size_t n)
{
    size_t i, c = 0;
    for (i = 0; i < n; i++) {
        c += m[i] > 0.0f;
    }
    return c;
}

static void two_squares(size_t x, size_t y, float *p)
{
    bool red = (x >= 2 && x < 6) || (x >= 12 && x < 16);
    (void)y;
    p[0] = 1.0f;
    p[1] = red ? 0.0f : 1.0f;
    p[2] = red ? 0.0f : 1.0f;
    p[3] = 1.0f;
}

static void test_wand_contiguous_vs_global(void)
{
    float (*px)[4] = img_fn(20, 10, two_squares), *m = NULL;
    pc_rect a = {0, 0, 20, 10};
    size_t len;
    CHECK(pc_algo_selection_magic_wand((const float (*)[4])px, 200, a, 3, 3, 10.0f, true, false, &m, &len,
                                       NULL) == PC_OK);
    CHECK_EQ_UINT(count_pos(m, len), 40);
    free(m);
    (void)pc_algo_selection_magic_wand((const float (*)[4])px, 200, a, 3, 3, 10.0f, false, false, &m, &len,
                                       NULL);
    CHECK_EQ_UINT(count_pos(m, len), 80);
    free(m);
    (void)pc_algo_selection_magic_wand((const float (*)[4])px, 200, a, 3, 3, 10.0f, true, true, &m, &len,
                                       NULL);
    CHECK(m[3 * 20 + 6] > 0.0f && m[3 * 20 + 6] < 1.0f);
    free(m);
    (void)pc_algo_selection_magic_wand((const float (*)[4])px, 200, a, 50, 3, 10.0f, true, false, &m, &len,
                                       NULL);
    CHECK_EQ_UINT(count_pos(m, len), 0);
    free(m);
    /* A short pixel buffer is refused, not read past. */
    CHECK(pc_algo_selection_magic_wand((const float (*)[4])px, 5, a, 3, 3, 10.0f, true, false, &m, &len,
                                       NULL) == PC_ERR_INVALID);
    free(px);
}

static void ramp(size_t x, size_t y, float *p)
{
    (void)y;
    p[0] = (float)x * 10.0f / 255.0f;
    p[1] = p[2] = 0.0f;
    p[3] = 1.0f;
}

static void test_wand_tolerance(void)
{
    float (*px)[4] = img_fn(10, 1, ramp), *m = NULL;
    size_t len;
    (void)pc_algo_selection_magic_wand((const float (*)[4])px, 10, pc_rect_make(0, 0, 10, 1), 0, 0, 25.0f,
                                       true, false, &m, &len, NULL);
    CHECK_EQ_UINT(count_pos(m, len), 3);
    free(m);
    free(px);
}

static void test_color_range_falloff(void)
{
    const float px[4][4] = {{1, 0, 0, 1}, {0.9f, 0, 0, 1}, {0, 0, 1, 1}, {1, 0, 0, 0}};
    const float c[3] = {1, 0, 0};
    float m[4];
    pc_algo_color_range(px, 4, c, 40.0f, m);
    CHECK(m[0] == 1.0f);
    CHECK(m[1] > 0.3f && m[1] < 1.0f);
    CHECK(m[2] == 0.0f && m[3] == 0.0f);
}

static void test_color_range_several_samples_and_localized(void)
{
    const float red[4] = {1, 0, 0, 1}, blue[4] = {0, 0, 1, 1}, green[4] = {0, 1, 0, 1};
    float px[4][4], reds[4][4], m[4], m2[4];
    pc_algo_range_sample s[2];
    const float c[3] = {1, 0, 0};
    int i;
    memcpy(px[0], red, 16);
    memcpy(px[1], blue, 16);
    memcpy(px[2], green, 16);
    memcpy(px[3], red, 16);
    memset(s, 0, sizeof s);
    memcpy(s[0].color, red, 12);
    memcpy(s[1].color, blue, 12);
    pc_algo_selection_color_range_samples((const float (*)[4])px, 4, 4, s, 2, 10.0f, false, 0.0f, m);
    CHECK(m[0] == 1.0f && m[1] == 1.0f && m[2] == 0.0f && m[3] == 1.0f);
    pc_algo_selection_color_range_samples((const float (*)[4])px, 4, 4, s, 1, 40.0f, false, 0.0f, m);
    pc_algo_color_range((const float (*)[4])px, 4, c, 40.0f, m2);
    CHECK(memcmp(m, m2, sizeof m) == 0);
    s[0].has_at = true;
    pc_algo_selection_color_range_samples((const float (*)[4])px, 4, 4, s, 1, 10.0f, true, 2.0f, m);
    CHECK(m[0] == 1.0f && m[1] == 0.0f && m[2] == 0.0f && m[3] == 0.0f);
    for (i = 0; i < 4; i++) {
        memcpy(reds[i], red, 16);
    }
    pc_algo_selection_color_range_samples((const float (*)[4])reds, 4, 4, s, 1, 10.0f, true, 2.0f, m);
    CHECK(m[0] == 1.0f && m[1] == 0.5f && m[2] == 0.0f && m[3] == 0.0f);
    pc_algo_selection_color_range_samples((const float (*)[4])px, 4, 4, s, 0, 10.0f, false, 0.0f, m);
    CHECK(m[0] == 0.0f && m[1] == 0.0f && m[2] == 0.0f && m[3] == 0.0f);
}

/* Photoshop 2024's coverage around Lab (60, 25, 19.9961) (see selection.rs). */
static const float PS_COV[21][5] = {
    {61.9989f, 25.5977f, 20.5742f, 40, 0.9686f},  {64.9989f, 25.5977f, 20.5742f, 40, 0.7961f},
    {67.9988f, 25.5977f, 20.5742f, 40, 0.4824f},  {71.9997f, 25.5977f, 20.5742f, 40, 0.1098f},
    {79.9985f, 25.5977f, 20.5742f, 40, 0.0f},     {71.9997f, 25.5977f, 20.5742f, 100, 0.8118f},
    {79.9985f, 25.5977f, 20.5742f, 100, 0.4824f}, {60.0f, 13.5508f, 20.5742f, 40, 0.0432f},
    {60.0f, 19.5742f, 20.5742f, 40, 0.6706f},     {60.0f, 22.582f, 20.5742f, 40, 0.9333f},
    {60.0f, 28.6055f, 20.5742f, 40, 0.8549f},     {60.0f, 31.6211f, 20.5742f, 40, 0.5098f},
    {60.0f, 37.6445f, 20.5742f, 40, 0.0078f},     {60.0f, 13.5508f, 20.5742f, 100, 0.7647f},
    {60.0f, 37.6445f, 20.5742f, 100, 0.7137f},    {60.0f, 25.5977f, 10.5352f, 40, 0.1725f},
    {60.0f, 25.5977f, 16.5586f, 40, 0.8667f},     {60.0f, 25.5977f, 24.5898f, 40, 0.7647f},
    {60.0f, 25.5977f, 30.6133f, 40, 0.0863f},     {60.0f, 25.5977f, 10.5352f, 100, 0.8392f},
    {60.0f, 25.5977f, 30.6133f, 100, 0.8f},
};

static void test_lab_range_matches_photoshop(void)
{
    const float c0[3] = {60.0f, 25.0f, 19.9961f}, l6[3] = {66, 25, 20}, a6[3] = {60, 31, 20};
    pc_algo_lab_range r = pc_algo_lab_range_point(c0);
    float q[3], in[3] = {60.1f, 25.4f, -19.6f};
    int i;
    for (i = 0; i < 21; i++) {
        CHECK_NEAR(pc_algo_lab_range_coverage(PS_COV[i], &r, PS_COV[i][3]), PS_COV[i][4], 2.0 / 255.0);
    }
    CHECK(pc_algo_lab_range_coverage(l6, &r, 40) > pc_algo_lab_range_coverage(a6, &r, 40));
    CHECK(pc_algo_lab_range_coverage(c0, &r, 1) == 1.0f && pc_algo_lab_range_coverage(c0, &r, 10) == 1.0f &&
          pc_algo_lab_range_coverage(c0, &r, 40) == 1.0f);
    pc_algo_quantize_lab8(in, q);
    CHECK(q[0] == 153.0f / 2.55f && q[1] == 25.0f && q[2] == -20.0f);
}

static void test_lab_range_box_and_edges(void)
{
    const float cs[2][3] = {{50, 10, 0}, {60, 20, 5}}, in[3] = {55, 15, 2}, near[3] = {55, 22, 2},
                far[3] = {55, 40, 2};
    const float pc[3] = {50, 10, -5}, p1[3] = {50, 11, -5};
    const float bad[3] = {NAN, INFINITY, -INFINITY};
    pc_algo_lab_range r, p;
    float k;
    int i, j;
    CHECK(pc_algo_lab_range_spanning(cs, 2, &r));
    CHECK(r.lo[0] == 50 && r.lo[1] == 10 && r.lo[2] == 0 && r.hi[0] == 60 && r.hi[1] == 20 && r.hi[2] == 5);
    CHECK(!pc_algo_lab_range_spanning(cs, 0, &r));
    CHECK(pc_algo_lab_range_coverage(in, &r, 1) == 1.0f && pc_algo_lab_range_coverage(in, &r, 200) == 1.0f);
    k = pc_algo_lab_range_coverage(near, &r, 40);
    CHECK(k > 0.0f && k < 1.0f);
    CHECK(pc_algo_lab_range_coverage(far, &r, 40) == 0.0f);
    p = pc_algo_lab_range_point(pc);
    CHECK(pc_algo_lab_range_coverage(pc, &p, 0) == 1.0f);
    CHECK(pc_algo_lab_range_coverage(p1, &p, 0) == 0.0f);
    for (i = 0; i < 3; i++) {
        const float v[3] = {bad[i], 0, 0};
        for (j = 0; j < 2; j++) {
            k = pc_algo_lab_range_coverage(v, &p, j ? 40.0f : bad[i]);
            CHECK(k >= 0.0f && k <= 1.0f);
        }
    }
}

static const float PS_SUB[20][6] = {
    {0, 20, 5, 40, 5, 20},       {0, 20, 10, 40, 0, 10},     {0, 20, 15, 40, 0, 15},
    {0, 20, 0, 40, 2.68f, 20},   {0, 20, -2, 40, 0.68f, 20}, {0, 20, -3, 40, 2.36f, 20},
    {0, 20, -8, 40, 0.04f, 20},  {0, 20, -11, 40, 2.4f, 20}, {0, 20, -13, 40, 0.4f, 20},
    {0, 20, -14, 40, 0, 20},     {0, 20, 20, 40, 0, 17.32f}, {0, 20, 23, 40, 0, 17.64f},
    {0, 20, 33, 40, 0, 19.6f},   {0, 20, 0, 80, 5.35f, 20},  {0, 20, -12, 80, 4.05f, 20},
    {0, 20, -22, 80, 4.76f, 20}, {0, 20, -30, 80, 0, 20},    {0, 6, 4, 40, 0, 4},
    {0, 2, 0, 40, 2, 2},         {0, 2, 3, 40, 0, 0.32f},
};

static void test_subtract_reshapes_the_box_like_photoshop(void)
{
    static const float lm[4][2] = {{40.0f, 43.14f}, {34.902f, 41.17f}, {25.098f, 40.79f}, {45.098f, 45.1f}};
    pc_algo_lab_range r, want = {{56.47f, 0, 0}, {59.05f, 30.56f, 23.05f}}, p;
    const float pp[3] = {50, 10, 10}, bad[3] = {NAN, INFINITY, -INFINITY}, one[1][3] = {{50, 0, 0}};
    const float g[3] = {60, -5, 3};
    int i;
    for (i = 0; i < 20; i++) {
        pc_algo_lab_range b = {{50, PS_SUB[i][0], 0}, {50, PS_SUB[i][1], 0}};
        const float m[3] = {50, PS_SUB[i][2], 0};
        r = pc_algo_lab_range_subtract(b, m, PS_SUB[i][3]);
        CHECK_NEAR(r.lo[1], PS_SUB[i][4], 0.03);
        CHECK_NEAR(r.hi[1], PS_SUB[i][5], 0.03);
    }
    for (i = 0; i < 4; i++) {
        pc_algo_lab_range b = {{40, 0, 0}, {60, 0, 0}};
        const float m[3] = {lm[i][0], 0, 0};
        r = pc_algo_lab_range_subtract(b, m, 40);
        CHECK_NEAR(r.lo[0], lm[i][1], 0.05);
    }
    {
        pc_algo_lab_range b = {{53.58f, 0, 0}, {59.05f, 30.56f, 25.03f}};
        const float m[3] = {53.33f, -28.71f, 36.45f};
        r = pc_algo_lab_range_subtract(b, m, 40);
        for (i = 0; i < 3; i++) {
            CHECK_NEAR(r.lo[i], want.lo[i], 0.05);
            CHECK_NEAR(r.hi[i], want.hi[i], 0.05);
        }
    }
    p = pc_algo_lab_range_point(pp);
    r = pc_algo_lab_range_subtract(p, pp, 40);
    CHECK(memcmp(&r, &p, sizeof r) == 0);
    r = pc_algo_lab_range_subtract(p, bad, NAN);
    CHECK(memcmp(&r, &p, sizeof r) == 0);
    (void)pc_algo_lab_range_spanning(one, 1, &r);
    r = pc_algo_lab_range_grow(r, g);
    CHECK(r.lo[0] == 50 && r.lo[1] == -5 && r.lo[2] == 0 && r.hi[0] == 60 && r.hi[1] == 0 && r.hi[2] == 3);
}

static float pfun(float d)
{
    float t = d / (40.0f * 256.0f / 255.0f);
    return pc_rs_minf_k(floorf((1.0f - 2.0f * (t * t)) * 256.0f), 255.0f) / 255.0f;
}

static void test_eight_bit_grid_steps_are_uneven_like_photoshop(void)
{
    const float c[3] = {50, 10, -20}, u[3] = {50, 11, -20}, d[3] = {50, 9, -20}, l1[3] = {51, 10, -20},
                l0[3] = {49, 10, -20};
    const float px[2][4] = {{50, 11, -20, 1}, {50, 9, -20, 1}};
    pc_algo_lab_range r = pc_algo_lab_range_point(c);
    float up, down, m[2];
    CHECK(pc_algo_lab_range_coverage_8bit(c, &r, 1) == 1.0f &&
          pc_algo_lab_range_coverage_8bit(c, &r, 40) == 1.0f);
    up = pc_algo_lab_range_coverage_8bit(u, &r, 40);
    down = pc_algo_lab_range_coverage_8bit(d, &r, 40);
    CHECK(up == pfun(4.5f) && down == pfun(1.5f));
    CHECK(down > up);
    CHECK(pc_algo_lab_range_coverage_8bit(l1, &r, 40) == pc_algo_lab_range_coverage_8bit(l0, &r, 40));
    CHECK(pc_algo_lab_range_coverage(u, &r, 40) == pc_algo_lab_range_coverage(d, &r, 40));
    CHECK(pc_algo_selection_color_range_lab(px, 2, 2, &r, 40, true, false, 0, NULL, 0, m, NULL) == PC_OK);
    CHECK(m[0] == up && m[1] == down);
}

static void test_color_range_lab_alpha_and_localized(void)
{
    const float c[3] = {50, 0, 0}, at[1][2] = {{1, 0}};
    pc_algo_lab_range r = pc_algo_lab_range_point(c);
    float px[10][4], m[10], l[10], e[10];
    int i;
    bool ok = true;
    for (i = 0; i < 10; i++) {
        px[i][0] = 50;
        px[i][1] = px[i][2] = 0;
        px[i][3] = i == 0 ? 0.0f : 1.0f;
    }
    (void)pc_algo_selection_color_range_lab((const float (*)[4])px, 10, 10, &r, 40, false, false, 0, NULL, 0,
                                            m, NULL);
    CHECK(m[0] == 0.0f);
    for (i = 1; i < 10; i++) {
        ok &= m[i] > 0.9f;
    }
    CHECK(ok);
    (void)pc_algo_selection_color_range_lab((const float (*)[4])px, 10, 10, &r, 40, false, true, 4, at, 1, l,
                                            NULL);
    CHECK(l[1] > l[3] && l[3] > 0.0f && l[6] == 0.0f);
    (void)pc_algo_selection_color_range_lab((const float (*)[4])px, 10, 10, &r, 40, false, true, 4, at, 0, e,
                                            NULL);
    CHECK(memcmp(e, m, sizeof m) == 0);
    CHECK(pc_algo_selection_color_range_lab(NULL, 0, 0, &r, 40, false, false, 0, NULL, 0, NULL, NULL) ==
          PC_OK);
}

static void test_hue_families(void)
{
    const float px[6][4] = {{1, 0, 0, 1},       {1, 0.5f, 0, 1}, {0.5f, 0.5f, 0.5f, 1},
                            {1, 0.5f, 0.5f, 1}, {0, 0, 1, 1},    {1, 0, 0, 0}};
    const float mg[1][4] = {{1, 0, 1, 1}}, mg2[1][4] = {{1, 0, 0.5f, 1}};
    float m[6];
    pc_algo_selection_hue_range(px, 6, 0, m);
    CHECK(m[0] == 1.0f);
    CHECK_NEAR(m[1], 0.5, 1e-5);
    CHECK(m[2] == 0.0f);
    CHECK_NEAR(m[3], 0.5, 1e-5);
    CHECK(m[4] == 0.0f && m[5] == 0.0f);
    pc_algo_selection_hue_range(px, 6, 60, m);
    CHECK_NEAR(m[1], 0.5, 1e-5);
    pc_algo_selection_hue_range(px, 6, 240, m);
    CHECK(m[4] == 1.0f);
    pc_algo_selection_hue_range(mg, 1, 300, m);
    CHECK(m[0] == 1.0f);
    pc_algo_selection_hue_range(mg2, 1, 300, m);
    CHECK_NEAR(m[0], 0.5, 1e-5);
}

static void test_tonal_ranges(void)
{
    static const float lv[6] = {0, 60, 75, 128, 200, 255};
    static const float sh[6] = {1, 1, 0.5f, 0, 0, 0}, hi[6] = {0, 0, 0, 0, 1, 1}, mi[6] = {0, 0, 0, 1, 0, 0};
    float px[6][4], m[6];
    int i;
    for (i = 0; i < 6; i++) {
        px[i][0] = px[i][1] = px[i][2] = lv[i] / 255.0f;
        px[i][3] = 1;
    }
    pc_algo_selection_tone_range((const float (*)[4])px, 6, 0, 65, 20, m);
    for (i = 0; i < 6; i++) {
        CHECK_NEAR(m[i], sh[i], 1e-4);
    }
    pc_algo_selection_tone_range((const float (*)[4])px, 6, 190, 255, 20, m);
    for (i = 0; i < 6; i++) {
        CHECK_NEAR(m[i], hi[i], 1e-4);
    }
    pc_algo_selection_tone_range((const float (*)[4])px, 6, 105, 150, 20, m);
    for (i = 0; i < 6; i++) {
        CHECK_NEAR(m[i], mi[i], 1e-4);
    }
}

static float *square(size_t w, size_t h)
{
    float *m = malloc(w * h * sizeof(float));
    size_t i;
    for (i = 0; i < w * h; i++) {
        m[i] = (i % w >= 5 && i % w < 15 && i / w >= 5 && i / w < 15) ? 1.0f : 0.0f;
    }
    return m;
}

static void test_expand_contract_border(void)
{
    float *m = square(20, 20), *e = NULL, *c = NULL, *b = NULL;
    CHECK(pc_algo_selection_expand(m, 20, 20, 2, &e, NULL) == PC_OK);
    CHECK(e[10 * 20 + 3] == 1.0f && e[10 * 20 + 2] == 0.0f);
    CHECK(pc_algo_selection_contract(m, 20, 20, 2, &c, NULL) == PC_OK);
    CHECK(c[10 * 20 + 6] == 0.0f && c[10 * 20 + 7] == 1.0f);
    CHECK(pc_algo_selection_border(m, 20, 20, 4, &b, NULL) == PC_OK);
    CHECK(b[10 * 20 + 10] == 0.0f);
    CHECK(b[10 * 20 + 5] > 0.5f && b[10 * 20 + 4] > 0.5f);
    free(m);
    free(e);
    free(c);
    free(b);
}

static void test_smooth_removes_specks_and_feather_softens(void)
{
    float *m = square(20, 20), *s = NULL, *f = NULL, sum = -0.0f;
    size_t i;
    m[2 * 20 + 2] = 1.0f;
    CHECK(pc_algo_selection_smooth(m, 400, 20, 20, 1.0f, &s, NULL) == PC_OK);
    CHECK(s[2 * 20 + 2] == 0.0f && s[10 * 20 + 10] == 1.0f);
    free(m);
    m = square(20, 20);
    CHECK(pc_algo_selection_feather(m, 400, 20, 20, 4.0f, &f, NULL) == PC_OK);
    CHECK(f[10 * 20 + 5] > 0.3f && f[10 * 20 + 5] < 0.8f);
    for (i = 0; i < 400; i++) {
        sum += f[i];
    }
    CHECK_NEAR(sum, 100.0, 1.0);
    free(m);
    free(s);
    free(f);
}

static float sumf(const float *m, size_t n)
{
    float s = -0.0f;
    size_t i;
    for (i = 0; i < n; i++) {
        s += m[i];
    }
    return s;
}

static void test_polygon_area_and_aa(void)
{
    const float sq[4][2] = {{2, 2}, {12, 2}, {12, 12}, {2, 12}}, tri[3][2] = {{0, 0}, {10, 0}, {0, 10}};
    const float two[2][2] = {{0, 0}, {1, 1}};
    pc_rect a = {0, 0, 20, 20};
    float *m = NULL;
    size_t len, i;
    bool soft = false, hard = true;
    (void)pc_algo_selection_polygon(sq, 4, a, true, &m, &len, NULL);
    CHECK_NEAR(sumf(m, len), 100.0, 1e-3);
    free(m);
    (void)pc_algo_selection_polygon(tri, 3, a, true, &m, &len, NULL);
    CHECK_NEAR(sumf(m, len), 50.0, 1.0);
    for (i = 0; i < len; i++) {
        soft |= m[i] > 0.0f && m[i] < 1.0f;
    }
    CHECK(soft);
    free(m);
    (void)pc_algo_selection_polygon(tri, 3, a, false, &m, &len, NULL);
    for (i = 0; i < len; i++) {
        hard &= m[i] == 0.0f || m[i] == 1.0f;
    }
    CHECK(hard);
    free(m);
    (void)pc_algo_selection_polygon(two, 2, a, true, &m, &len, NULL);
    CHECK(sumf(m, len) == 0.0f);
    free(m);
}

static void read_mask(const pc_surface *s, bool some, pc_rect a, float *out, size_t n)
{
    float *v = NULL;
    size_t len = 0;
    (void)pc_algo_selection_mask_from_surface(some ? s : NULL, a, &v, &len, NULL);
    memcpy(out, v, (len < n ? len : n) * sizeof(float));
    free(v);
}

static void test_combine_modes(void)
{
    pc_rect a = {0, 0, 4, 1};
    const float om[4] = {1, 1, 0, 0}, nm[4] = {0, 1, 1, 0}, all[4] = {1, 1, 1, 1};
    static const float want[4][4] = {{0, 1, 1, 0}, {1, 1, 1, 0}, {1, 0, 0, 0}, {0, 1, 0, 0}};
    pc_surface old, o;
    float v[4];
    bool some;
    int mode;
    (void)pc_algo_selection_mask_to_surface(om, 4, a, &old, NULL);
    for (mode = 0; mode < 4; mode++) {
        (void)pc_algo_selection_combine(&old, nm, 4, a, (pc_algo_selection_mode)mode, &o, &some, NULL);
        read_mask(&o, some, a, v, 4);
        CHECK(memcmp(v, want[mode], sizeof v) == 0);
        pc_surface_fini(&o);
    }
    (void)pc_algo_selection_combine(&old, all, 4, a, PC_ALGO_SELECTION_SUBTRACT, &o, &some, NULL);
    CHECK(!some);
    pc_surface_fini(&o);
    CHECK(pc_algo_selection_mode_parse("add") == PC_ALGO_SELECTION_ADD);
    CHECK(strcmp(pc_algo_selection_mode_name(PC_ALGO_SELECTION_INTERSECT), "intersect") == 0);
    pc_surface_fini(&old);
}

/* splitmix64 (photo_util::Rng). */
static uint64_t sm_next(uint64_t *s)
{
    uint64_t z;
    *s += 0x9E3779B97F4A7C15ull;
    z = *s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static void old_mask_to_surface(const float *m, size_t n, pc_rect a, pc_surface *s)
{
    pc_surface_init(s, PC_PIXEL_FORMAT_GRAY8);
    if (!pc_rect_is_empty(a)) {
        (void)pc_surface_write_region(s, a, m, n, NULL);
    }
    pc_surface_prune(s);
}

static bool old_combine(const pc_surface *old, const float *nw, size_t n, pc_rect a, int mode,
                        pc_surface *out)
{
    float *prev = NULL, *o = malloc(n * sizeof(float));
    size_t len = 0, i;
    bool any = false;
    if (old != NULL) {
        (void)pc_surface_read_region(old, a, &prev, &len, NULL);
    }
    for (i = 0; i < n; i++) {
        float q = roundf(pc_clampf(nw[i], 0, 1) * 255.0f) / 255.0f, p = prev != NULL ? prev[i] : 0.0f, v;
        v = mode == 0   ? q
            : mode == 1 ? pc_rs_maxf(p, q)
            : mode == 2 ? pc_rs_maxf_k(p - q, 0)
                        : pc_rs_minf(p, q);
        o[i] = v < 0.5f / 255.0f ? 0.0f : v;
        any |= o[i] > 0.0f;
    }
    if (any) {
        old_mask_to_surface(o, n, a, out);
    }
    free(prev);
    free(o);
    return any;
}

static void test_tiled_conversions_match_per_sample_ones(void)
{
    static const pc_rect areas[4] = {
        {0, 0, 300, 20}, {-37, 250, 530, 263}, {255, -3, 258, 600}, {5, 5, 6, 6}};
    uint64_t rng = 7;
    size_t ai, i;
    int kind, mode, pi;
    for (ai = 0; ai < 4; ai++) {
        pc_rect a = areas[ai];
        size_t n = (size_t)pc_rect_width(a) * pc_rect_height(a), len = 0;
        float *om = malloc(n * sizeof(float)), *nw = malloc(n * sizeof(float)), *rd = NULL, *slow = NULL;
        pc_surface old, fast;
        for (i = 0; i < n; i++) {
            om[i] = i % 97 < 40 ? (float)(sm_next(&rng) >> 11) / 9007199254740992.0f : 0.0f;
        }
        old_mask_to_surface(om, n, a, &old);
        (void)pc_algo_selection_mask_to_surface(om, n, a, &fast, NULL);
        CHECK(pc_surface_eq(&fast, &old));
        (void)pc_algo_selection_mask_from_surface(&old, a, &rd, &len, NULL);
        (void)pc_surface_read_region(&old, a, &slow, &i, NULL);
        CHECK(len == i && memcmp(rd, slow, len * sizeof(float)) == 0);
        free(rd);
        free(slow);
        for (kind = 0; kind < 3; kind++) {
            for (i = 0; i < n; i++) {
                nw[i] = kind == 0   ? (float)(sm_next(&rng) >> 11) / 9007199254740992.0f * 1.2f - 0.1f
                        : kind == 1 ? (i % 131 < 50 ? 1.0f : 0.0f)
                                    : 0.001f;
            }
            for (mode = 0; mode < 4; mode++) {
                for (pi = 0; pi < 2; pi++) {
                    pc_surface c1, c2;
                    bool s1 = false, s2 = old_combine(pi ? &old : NULL, nw, n, a, mode, &c2);
                    (void)pc_algo_selection_combine(pi ? &old : NULL, nw, n, a, (pc_algo_selection_mode)mode,
                                                    &c1, &s1, NULL);
                    CHECK(s1 == s2);
                    if (s1 && s2) {
                        CHECK(pc_surface_eq(&c1, &c2));
                    }
                    pc_surface_fini(&c1);
                    if (s2) {
                        pc_surface_fini(&c2);
                    }
                }
            }
        }
        free(om);
        free(nw);
        pc_surface_fini(&old);
        pc_surface_fini(&fast);
    }
    {
        const float five[5] = {1, 1, 1, 1, 1};
        pc_surface o;
        bool some = false;
        (void)pc_algo_selection_combine(NULL, five, 5, pc_rect_make(0, 0, 10, 10), PC_ALGO_SELECTION_REPLACE,
                                        &o, &some, NULL);
        CHECK(some);
        pc_surface_fini(&o);
        (void)pc_algo_selection_mask_to_surface(NULL, 0, pc_rect_make(0, 0, 10, 10), &o, NULL);
        CHECK_EQ_UINT(pc_surface_tile_count(&o), 0);
        pc_surface_fini(&o);
        (void)pc_algo_selection_combine(NULL, NULL, 0, PC_RECT_EMPTY, PC_ALGO_SELECTION_REPLACE, &o, &some,
                                        NULL);
        CHECK(!some);
        pc_surface_fini(&o);
    }
}

static uint8_t (*blobs(int32_t w, int32_t h))[4]
{
    uint8_t (*v)[4] = malloc((size_t)(w * h) * 4);
    int32_t i;
    for (i = 0; i < w * h; i++) {
        int32_t x = i % w, y = i / w;
        uint8_t val = (uint8_t)((x * 7 + y * 3) % 23 + ((x / 5 + y / 4) % 3 == 0 ? 120 : 0));
        v[i][0] = val;
        v[i][1] = val / 2;
        v[i][2] = (uint8_t)(255 - val);
        v[i][3] = x % 11 == 0 ? 128 : 255;
    }
    return v;
}

static void test_wand_region_matches_reference_magic_wand(void)
{
    static const struct {
        int32_t x, y;
        float tol;
        bool cont, aa;
    } cases[5] = {{10, 5, 10, true, true},
                  {10, 5, 10, true, false},
                  {20, 20, 40, false, true},
                  {4, -2, 0, true, true},
                  {30, 10, 255, true, true}};
    const int32_t w = 37, h = 29;
    pc_rect area = {3, -2, 40, 27};
    uint8_t (*img)[4] = blobs(w, h);
    float (*f)[4] = malloc((size_t)(w * h) * sizeof *f);
    pc_algo_region r;
    int32_t i, x, y, c;
    for (i = 0; i < w * h; i++) {
        for (c = 0; c < 4; c++) {
            f[i][c] = (float)img[i][c] / 255.0f;
        }
    }
    for (i = 0; i < 5; i++) {
        float *ref = NULL;
        size_t len;
        bool ok = true;
        (void)pc_algo_selection_magic_wand((const float (*)[4])f, (size_t)(w * h), area, cases[i].x,
                                           cases[i].y, cases[i].tol, cases[i].cont, cases[i].aa, &ref, &len,
                                           NULL);
        CHECK(pc_algo_selection_wand_region((const uint8_t (*)[4])img, (size_t)(w * h), area, cases[i].x,
                                            cases[i].y, cases[i].tol, cases[i].cont, cases[i].aa, &r,
                                            NULL) == PC_OK);
        CHECK(!pc_algo_region_is_none(&r));
        for (y = area.y0; y < area.y1; y++) {
            for (x = area.x0; x < area.x1; x++) {
                float want = roundf(ref[(y - area.y0) * w + x - area.x0] * 255.0f);
                ok &= fabsf(pc_algo_region_at(&r, x, y) * 255.0f - want) <= 1.0f;
            }
        }
        CHECK(ok);
        free(ref);
        pc_algo_region_fini(&r);
    }
    (void)pc_algo_selection_wand_region((const uint8_t (*)[4])img, (size_t)(w * h), area, 0, 0, 10, true,
                                        true, &r, NULL);
    CHECK(pc_algo_region_is_none(&r));
    free(img);
    free(f);
}

static void test_combine_region_matches_combine(void)
{
    pc_rect area = {0, 0, 40, 30};
    pc_surface old;
    pc_algo_region region;
    uint8_t (*img)[4] = blobs(40, 30);
    float one = 1.0f, f4 = 0.4f, *full = malloc(1200 * sizeof(float));
    int32_t x, y, mode;
    pc_surface_init(&old, PC_PIXEL_FORMAT_GRAY8);
    (void)pc_surface_fill_rect(&old, pc_rect_make(5, 5, 25, 20), &one, 1, NULL);
    (void)pc_surface_fill_rect(&old, pc_rect_make(8, 8, 10, 10), &f4, 1, NULL);
    (void)pc_algo_selection_wand_region((const uint8_t (*)[4])img, 1200, area, 12, 12, 30, true, true,
                                        &region, NULL);
    for (y = 0; y < 30; y++) {
        for (x = 0; x < 40; x++) {
            full[y * 40 + x] = pc_algo_region_at(&region, x, y);
        }
    }
    for (mode = 0; mode < 4; mode++) {
        pc_surface a, b;
        bool sa = false, sb = false;
        (void)pc_algo_selection_combine(&old, full, 1200, area, (pc_algo_selection_mode)mode, &a, &sa, NULL);
        (void)pc_algo_selection_combine_region(&old, &region, (pc_algo_selection_mode)mode, &b, &sb, NULL);
        CHECK(sa == sb);
        if (sa && sb) {
            bool ok = true;
            for (y = 0; y < 30; y++) {
                for (x = 0; x < 40; x++) {
                    float pa[1], pb[1];
                    pc_surface_read_pixel(&a, x, y, pa);
                    pc_surface_read_pixel(&b, x, y, pb);
                    ok &= fabsf(pa[0] - pb[0]) < 1.5f / 255.0f;
                }
            }
            CHECK(ok);
        }
        pc_surface_fini(&a);
        pc_surface_fini(&b);
    }
    {
        pc_surface o;
        bool some = true;
        (void)pc_algo_selection_combine_region(NULL, NULL, PC_ALGO_SELECTION_SUBTRACT, &o, &some, NULL);
        CHECK(!some);
        pc_surface_fini(&o);
    }
    pc_algo_region_fini(&region);
    pc_surface_fini(&old);
    free(img);
    free(full);
}

/* ---- never crash ---- */

static void test_hostile_inputs(void)
{
    float *m = NULL, mask[4];
    size_t len;
    const float nanpx[1][4] = {{NAN, NAN, NAN, NAN}};
    const float pts[3][2] = {{NAN, 0}, {INFINITY, -INFINITY}, {1e30f, 1e30f}};
    pc_algo_region bad = {{0, 0, 10, 10}, NULL, 0}, r;
    pc_surface o;
    bool some;
    /* Huge areas: PC_ERR_LIMIT instead of an attempted allocation. */
    CHECK(pc_algo_selection_mask_from_surface(NULL, pc_rect_make(INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX),
                                              &m, &len, NULL) == PC_ERR_LIMIT);
    CHECK(pc_algo_selection_polygon(pts, 3, pc_rect_make(0, 0, 8, 8), true, &m, &len, NULL) == PC_OK);
    free(m);
    CHECK(pc_algo_selection_expand(mask, 0, 0, NAN, &m, NULL) == PC_OK);
    free(m);
    CHECK(pc_algo_selection_magic_wand(nanpx, 1, pc_rect_make(0, 0, 1, 1), 0, 0, NAN, true, true, &m, &len,
                                       NULL) == PC_OK);
    free(m);
    pc_algo_selection_hue_range(nanpx, 1, NAN, mask);
    pc_algo_selection_tone_range(nanpx, 1, NAN, NAN, NAN, mask);
    /* A region whose mask doesn't match its box is refused. */
    CHECK(pc_algo_selection_combine_region(NULL, &bad, PC_ALGO_SELECTION_REPLACE, &o, &some, NULL) ==
          PC_ERR_INVALID);
    CHECK(pc_algo_region_at(&bad, 3, 3) == 0.0f);
    CHECK(pc_algo_selection_wand_region(NULL, 0, pc_rect_make(0, 0, 4, 4), 1, 1, 5, true, true, &r, NULL) ==
          PC_ERR_INVALID);
}

int main(int argc, char **argv)
{
    RUN(test_wand_contiguous_vs_global);
    RUN(test_wand_tolerance);
    RUN(test_color_range_falloff);
    RUN(test_color_range_several_samples_and_localized);
    RUN(test_lab_range_matches_photoshop);
    RUN(test_lab_range_box_and_edges);
    RUN(test_subtract_reshapes_the_box_like_photoshop);
    RUN(test_eight_bit_grid_steps_are_uneven_like_photoshop);
    RUN(test_color_range_lab_alpha_and_localized);
    RUN(test_hue_families);
    RUN(test_tonal_ranges);
    RUN(test_expand_contract_border);
    RUN(test_smooth_removes_specks_and_feather_softens);
    RUN(test_polygon_area_and_aa);
    RUN(test_combine_modes);
    RUN(test_tiled_conversions_match_per_sample_ones);
    RUN(test_wand_region_matches_reference_magic_wand);
    RUN(test_combine_region_matches_combine);
    RUN(test_hostile_inputs);
    return pc_t_finish();
}
