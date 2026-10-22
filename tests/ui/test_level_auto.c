#include "harness.h"
#include "ui/level_auto.h"
#include <math.h>

static void test_auto_levels_clips_tenth_percent(void)
{
    uint32_t h[256] = {0};
    /* 1000 pixels per bin across 0..99: total 100000, clip = 100 pixels = one bin each end. */
    for (size_t i = 0; i < 100; i++)
        h[i] = 1000;
    double lo = -1, hi = -1;
    pc_ui_auto_levels(h, &lo, &hi);
    /* lo advances while the running sum stays <= clip; the first bin fills the budget. */
    CHECK(lo >= 0 && lo <= 2);
    CHECK(hi >= 97 && hi <= 99);
    CHECK(hi > lo + 1);
}

static void test_auto_levels_edges(void)
{
    double lo = -1, hi = -1;
    pc_ui_auto_levels(NULL, &lo, &hi);
    CHECK(lo == 0 && hi == 255);
    uint32_t flat[256] = {0};
    pc_ui_auto_levels(flat, &lo, &hi);
    /* An empty histogram has clip 0: lo walks to 253, hi stays 255. */
    CHECK(lo == 253 && hi == 255);
    uint32_t spike[256] = {0};
    spike[128] = 42;
    pc_ui_auto_levels(spike, &lo, &hi);
    CHECK(lo == 128 && hi == 130);
}

static void test_gamma_roundtrip(void)
{
    /* gamma_pos and gamma_from_pos invert each other within the usable range. */
    for (double g = 0.25; g <= 4.0; g += 0.25) {
        double x = pc_ui_levels_gamma_pos(0, 255, g);
        double back = pc_ui_levels_gamma_from_pos(0, 255, x);
        CHECK_NEAR(back, g, 0.02);
    }
    /* Midpoint handle on gamma 1.0 sits at the centre. */
    CHECK_NEAR(pc_ui_levels_gamma_pos(0, 255, 1.0), 127.5, 0.01);
    /* Clamped to 0.01..9.99. */
    CHECK(pc_ui_levels_gamma_from_pos(0, 255, 0) >= 0.01);
    CHECK(pc_ui_levels_gamma_from_pos(0, 255, 255) <= 9.99);
    /* NaN-safe. */
    CHECK(isfinite(pc_ui_levels_gamma_pos(NAN, 255, 1)) || pc_ui_levels_gamma_pos(NAN, 255, 1) == 0);
    CHECK(pc_ui_levels_gamma_from_pos(10, 10, 10) >= 0.01);
}

static void test_slider_nearest(void)
{
    double xs[3] = {10, 128, 250};
    CHECK(pc_ui_slider_nearest(xs, 3, 12) == 0);
    CHECK(pc_ui_slider_nearest(xs, 3, 130) == 1);
    CHECK(pc_ui_slider_nearest(xs, 3, 1000) == 2);
    CHECK(pc_ui_slider_nearest(NULL, 0, 5) == SIZE_MAX);
    CHECK(pc_ui_slider_nearest(xs, 0, 5) == SIZE_MAX);
}

int main(int argc, char **argv)
{
    RUN(test_auto_levels_clips_tenth_percent);
    RUN(test_auto_levels_edges);
    RUN(test_gamma_roundtrip);
    RUN(test_slider_nearest);
    return pc_t_finish();
}
