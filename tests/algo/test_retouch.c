/* Dodge / Burn / Sponge / Blur / Sharpen kernels (src/algo/retouch.c): the
 * crates/algo/src/retouch.rs tests plus never-crash cases. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "harness.h"

static float db(float v, float e, pc_algo_tone_range r, bool burn)
{
    float c[3] = {v, v, v}, o[3];
    pc_algo_retouch_dodge_burn(c, e, r, burn, true, o);
    return o[0];
}

static void test_dodge_midtones_affects_midtones_most(void)
{
    float g5 = db(0.5f, 0.5f, PC_ALGO_TONE_MIDTONES, false) - 0.5f;
    float g1 = db(0.1f, 0.5f, PC_ALGO_TONE_MIDTONES, false) - 0.1f;
    float g95 = db(0.95f, 0.5f, PC_ALGO_TONE_MIDTONES, false) - 0.95f;
    float zero[3] = {0, 0, 0}, o[3];
    CHECK(g5 > 0.1f);
    CHECK(g5 > 2.0f * g1);
    CHECK(g5 > 2.0f * g95);
    pc_algo_retouch_dodge_burn(zero, 0.5f, PC_ALGO_TONE_MIDTONES, false, true, o);
    CHECK(o[0] == 0.0f && o[1] == 0.0f && o[2] == 0.0f);
    CHECK(db(0.5f, 0.5f, PC_ALGO_TONE_MIDTONES, true) < 0.45f);
}

static float d(float v, pc_algo_tone_range r)
{
    return pc_algo_retouch_tone_curve(v, 0.5f, r, false) - v;
}
static float b(float v, pc_algo_tone_range r)
{
    return v - pc_algo_retouch_tone_curve(v, 0.5f, r, true);
}

static void test_ranges_target_their_tones(void)
{
    CHECK(d(0.1f, PC_ALGO_TONE_SHADOWS) > d(0.9f, PC_ALGO_TONE_SHADOWS));
    CHECK(d(0.9f, PC_ALGO_TONE_HIGHLIGHTS) > d(0.1f, PC_ALGO_TONE_HIGHLIGHTS));
    CHECK(b(0.9f, PC_ALGO_TONE_HIGHLIGHTS) > b(0.2f, PC_ALGO_TONE_HIGHLIGHTS));
    CHECK(b(0.3f, PC_ALGO_TONE_SHADOWS) / 0.3f > b(0.9f, PC_ALGO_TONE_SHADOWS) / 0.9f);
}

static void test_protect_tones_keeps_hue(void)
{
    const float c[3] = {0.6f, 0.3f, 0.2f};
    float p[3];
    int i;
    pc_algo_retouch_dodge_burn(c, 0.8f, PC_ALGO_TONE_MIDTONES, false, true, p);
    for (i = 0; i < 3; i++) {
        CHECK(p[i] >= 0.0f && p[i] <= 1.0f);
    }
    /* Ratios (hue) preserved when nothing clips. */
    CHECK(fabsf(p[0] / p[1] - c[0] / c[1]) < 0.05f);
}

static void test_sponge_changes_saturation(void)
{
    const float c[3] = {0.8f, 0.4f, 0.3f}, sat[3] = {1.0f, 0.1f, 0.1f}, grey[3] = {0.5f, 0.5f, 0.5f};
    float dd[3], s[3], v[3], nv[3], g[3];
    int i;
    pc_algo_retouch_sponge(c, 0.5f, false, false, dd);
    CHECK(pc_algo_retouch_saturation(dd) < pc_algo_retouch_saturation(c));
    CHECK(fabsf(pc_algo_retouch_luma(dd) - pc_algo_retouch_luma(c)) < 1e-4f);
    pc_algo_retouch_sponge(c, 0.5f, true, false, s);
    CHECK(pc_algo_retouch_saturation(s) > pc_algo_retouch_saturation(c));
    for (i = 0; i < 3; i++) {
        CHECK(s[i] >= 0.0f && s[i] <= 1.0f);
    }
    /* Vibrance protects already-saturated colours when saturating. */
    pc_algo_retouch_sponge(sat, 0.5f, true, true, v);
    pc_algo_retouch_sponge(sat, 0.5f, true, false, nv);
    CHECK(fabsf(v[1] - 0.1f) <= fabsf(nv[1] - 0.1f));
    pc_algo_retouch_sponge(grey, 1.0f, false, false, g);
    CHECK(g[0] == 0.5f && g[1] == 0.5f && g[2] == 0.5f);
}

static float var(const float *v, size_t n)
{
    float m = -0.0f, s = -0.0f;
    size_t i;
    for (i = 0; i < n; i++) {
        m += v[i];
    }
    m /= (float)n;
    for (i = 0; i < n; i++) {
        s += (v[i] - m) * (v[i] - m);
    }
    return s / (float)n;
}

static void test_blur_reduces_variance_and_sharpen_increases_it(void)
{
    size_t w = 16, h = 16, i, x, y, k = 0;
    const size_t win[4] = {4, 4, 12, 12};
    float data[256], smooth[256], orig[64], so[64], *bl = NULL, *s = NULL, *sp = NULL, hi = 0.0f;
    for (i = 0; i < w * h; i++) {
        data[i] = (i % w + i / w) % 2 == 0 ? 0.2f : 0.8f;
        smooth[i] = 0.3f + 0.03f * (float)(i % w) + (i % w == 8 ? 0.2f : 0.0f);
        hi = fmaxf(hi, smooth[i]);
    }
    for (y = 4; y < 12; y++) {
        for (x = 4; x < 12; x++, k++) {
            orig[k] = data[y * w + x];
            so[k] = smooth[y * w + x];
        }
    }
    CHECK(pc_algo_retouch_local_blur(data, 256, w, h, 1, SIZE_MAX, win, 1.5f, &bl, NULL) == PC_OK);
    CHECK(var(bl, 64) < var(orig, 64) * 0.2f);
    CHECK(pc_algo_retouch_local_sharpen(smooth, 256, w, h, 1, SIZE_MAX, win, 1.0f, false, &s, NULL) == PC_OK);
    CHECK(var(s, 64) > var(so, 64));
    /* Protect detail never exceeds the local range. */
    CHECK(pc_algo_retouch_local_sharpen(smooth, 256, w, h, 1, SIZE_MAX, win, 4.0f, true, &sp, NULL) == PC_OK);
    for (i = 0; i < 64; i++) {
        CHECK(sp[i] <= hi + 1e-6f);
    }
    free(bl), free(s), free(sp);
}

static void test_blur_ignores_transparent_colour(void)
{
    /* Red opaque next to transparent green: blurred colour stays red. */
    float data[8 * 4], *bl = NULL;
    const size_t win[4] = {2, 0, 6, 1};
    size_t x;
    for (x = 0; x < 8; x++) {
        const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 0};
        memcpy(&data[x * 4], x < 4 ? red : green, sizeof red);
    }
    CHECK(pc_algo_retouch_local_blur(data, 32, 8, 1, 4, 3, win, 1.0f, &bl, NULL) == PC_OK);
    for (x = 0; x < 4; x++) {
        const float *p = &bl[x * 4];
        if (p[3] > 0.01f) {
            CHECK(p[0] > 0.99f && p[1] < 0.01f);
        }
    }
    free(bl);
}

static void test_names(void)
{
    pc_algo_tone_range r;
    CHECK_EQ_STR(pc_algo_tone_range_name(PC_ALGO_TONE_MIDTONES), "midtones");
    CHECK(pc_algo_tone_range_from_name("highlights", 10, &r) && r == PC_ALGO_TONE_HIGHLIGHTS);
    CHECK(!pc_algo_tone_range_from_name("Shadows", 7, &r));
    CHECK(pc_algo_tone_range_name((pc_algo_tone_range)7) == NULL);
}

static void test_never_crash(void)
{
    float data[4 * 4 * 2], *o = (float *)data;
    const size_t full[4] = {0, 0, 4, 4}, outside[4] = {0, 0, 5, 4}, backwards[4] = {3, 0, 1, 4},
                 empty[4] = {2, 2, 2, 9};
    float nan3[3] = {NAN, INFINITY, -1.0f}, out3[3];
    size_t i;
    for (i = 0; i < 32; i++) {
        data[i] = i % 5 == 0 ? NAN : (float)i / 32.0f;
    }
    CHECK(pc_algo_retouch_local_blur(data, 32, 4, 4, 2, SIZE_MAX, outside, 1.0f, &o, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_retouch_local_blur(data, 32, 4, 4, 2, SIZE_MAX, backwards, 1.0f, &o, NULL) ==
          PC_ERR_INVALID);
    CHECK(pc_algo_retouch_local_blur(data, 31, 4, 4, 2, SIZE_MAX, full, 1.0f, &o, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_retouch_local_blur(data, 32, 4, 4, 2, 2, full, 1.0f, &o, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_retouch_local_blur(data, 32, 4, 4, 2, SIZE_MAX, full, INFINITY, &o, NULL) == PC_ERR_LIMIT);
    CHECK(pc_algo_retouch_local_sharpen(data, 32, 4, 4, 2, SIZE_MAX, outside, 1.0f, true, &o, NULL) ==
          PC_ERR_INVALID);
    CHECK(o == (float *)data); /* untouched on failure */
    /* An empty window reads nothing (even from an empty buffer). */
    CHECK(pc_algo_retouch_local_blur(NULL, 0, 0, 0, 1, SIZE_MAX, empty, 1.0f, &o, NULL) == PC_OK);
    free(o);
    /* NaN data, NaN / negative / huge sigma and amounts. */
    CHECK(pc_algo_retouch_local_blur(data, 32, 4, 4, 2, 1, full, NAN, &o, NULL) == PC_OK);
    free(o);
    CHECK(pc_algo_retouch_local_blur(data, 32, 4, 4, 2, 1, full, -3.0f, &o, NULL) == PC_OK);
    free(o);
    CHECK(pc_algo_retouch_local_blur(data, 32, 4, 4, 2, 1, full, 900.0f, &o, NULL) == PC_OK);
    free(o);
    CHECK(pc_algo_retouch_local_sharpen(data, 32, 4, 4, 2, 0, full, NAN, true, &o, NULL) == PC_OK);
    free(o);
    pc_algo_retouch_dodge_burn(nan3, NAN, PC_ALGO_TONE_SHADOWS, true, true, out3);
    pc_algo_retouch_dodge_burn(nan3, 1e30f, (pc_algo_tone_range)9, false, false, out3);
    pc_algo_retouch_sponge(nan3, INFINITY, true, true, out3);
    (void)pc_algo_retouch_saturation(nan3);
    CHECK(!isnan(out3[0]) || isnan(out3[0])); /* reached without crashing */
}

int main(int argc, char **argv)
{
    RUN(test_dodge_midtones_affects_midtones_most);
    RUN(test_ranges_target_their_tones);
    RUN(test_protect_tones_keeps_hue);
    RUN(test_sponge_changes_saturation);
    RUN(test_blur_reduces_variance_and_sharpen_increases_it);
    RUN(test_blur_ignores_transparent_colour);
    RUN(test_names);
    RUN(test_never_crash);
    return pc_t_finish();
}
