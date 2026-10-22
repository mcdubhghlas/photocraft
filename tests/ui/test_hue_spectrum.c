#include "harness.h"
#include "ui/hue_spectrum.h"
#include <math.h>

static void test_hue_primaries(void)
{
    float rgb[3];
    pc_ui_hue_color(0, rgb);
    CHECK_NEAR(rgb[0], 1, 0.001);
    CHECK_NEAR(rgb[1], 0, 0.001);
    CHECK_NEAR(rgb[2], 0, 0.001);
    pc_ui_hue_color(120, rgb);
    CHECK_NEAR(rgb[0], 0, 0.001);
    CHECK_NEAR(rgb[1], 1, 0.001);
    CHECK_NEAR(rgb[2], 0, 0.001);
    pc_ui_hue_color(240, rgb);
    CHECK_NEAR(rgb[2], 1, 0.001);
    /* Wraps modulo 360 and is NaN-safe. */
    float wrap[3], neg[3];
    pc_ui_hue_color(360, wrap);
    pc_ui_hue_color(0, neg);
    CHECK_NEAR(wrap[0], neg[0], 0.001);
    pc_ui_hue_color(-120, neg);
    pc_ui_hue_color(240, wrap);
    CHECK_NEAR(neg[2], wrap[2], 0.001);
    pc_ui_hue_color(NAN, rgb);
    CHECK(rgb[0] == 1);
    pc_ui_hue_color(0, NULL); /* no crash */
}

static void test_hue_midpoints_in_gamut(void)
{
    for (double d = 0; d < 360; d += 11.0) {
        float rgb[3];
        pc_ui_hue_color(d, rgb);
        for (int c = 0; c < 3; c++)
            CHECK(rgb[c] >= -0.001f && rgb[c] <= 1.001f);
        /* A fully saturated hue always touches both 0 and 1 on some channel. */
        float lo = rgb[0] < rgb[1] ? (rgb[0] < rgb[2] ? rgb[0] : rgb[2]) : (rgb[1] < rgb[2] ? rgb[1] : rgb[2]);
        float hi = rgb[0] > rgb[1] ? (rgb[0] > rgb[2] ? rgb[0] : rgb[2]) : (rgb[1] > rgb[2] ? rgb[1] : rgb[2]);
        CHECK_NEAR(lo, 0, 0.001);
        CHECK_NEAR(hi, 1, 0.001);
    }
}

static void test_colorize_saturation(void)
{
    CHECK_NEAR(pc_ui_hue_colorize_saturation(0), 25, 0.001);
    CHECK_NEAR(pc_ui_hue_colorize_saturation(10), 25, 0.001);
    CHECK_NEAR(pc_ui_hue_colorize_saturation(60), 60, 0.001);
    CHECK_NEAR(pc_ui_hue_colorize_saturation(-100), 25, 0.001);
    CHECK_NEAR(pc_ui_hue_colorize_saturation(NAN), 25, 0.001);
}

static void test_spectrum_fill(void)
{
    uint8_t strip[72 * 4];
    memset(strip, 7, sizeof strip);
    pc_ui_hue_spectrum_fill(strip, 72);
    /* Column 0 is red; every column is opaque and touches both 0 and 255 on some channel. */
    CHECK_EQ_INT(strip[0], 255);
    CHECK_EQ_INT(strip[1], 0);
    CHECK_EQ_INT(strip[2], 0);
    for (size_t i = 0; i < 72; i++) {
        CHECK_EQ_INT(strip[i * 4 + 3], 255);
        uint8_t lo = 255, hi = 0;
        for (size_t c = 0; c < 3; c++) {
            uint8_t v = strip[i * 4 + c];
            if (v < lo)
                lo = v;
            if (v > hi)
                hi = v;
        }
        CHECK(lo == 0);
        CHECK(hi == 255);
    }
    /* One third of the way around the ribbon is green. */
    CHECK_EQ_INT(strip[24 * 4 + 1], 255);
    /* NULL / zero are no-ops (no crash, buffer untouched). */
    uint8_t one[4] = {1, 2, 3, 4};
    pc_ui_hue_spectrum_fill(one, 0);
    CHECK_EQ_INT(one[0], 1);
    pc_ui_hue_spectrum_fill(NULL, 72);
}

int main(int argc, char **argv)
{
    RUN(test_hue_primaries);
    RUN(test_hue_midpoints_in_gamut);
    RUN(test_colorize_saturation);
    RUN(test_spectrum_fill);
    return pc_t_finish();
}
