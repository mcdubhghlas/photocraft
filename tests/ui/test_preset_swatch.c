#include "harness.h"
#include "ui/preset_swatch.h"

static void test_stop_color_tokens(void)
{
    const float fg[4] = {0.1f, 0.2f, 0.3f, 1.0f};
    const float bg[4] = {0.7f, 0.8f, 0.9f, 1.0f};
    float c[3] = {-1, -1, -1};

    /* literal hex, with and without the leading '#' */
    CHECK(pc_ui_preset_stop_color("#ff0000", fg, bg, c));
    CHECK_NEAR(c[0], 1.0f, 1e-4f);
    CHECK_NEAR(c[1], 0.0f, 1e-4f);
    CHECK_NEAR(c[2], 0.0f, 1e-4f);
    CHECK(pc_ui_preset_stop_color("00ff00", fg, bg, c));
    CHECK_NEAR(c[1], 1.0f, 1e-4f);
    CHECK(pc_ui_preset_stop_color("#0000FF", fg, bg, c)); /* upper-case nibbles */
    CHECK_NEAR(c[2], 1.0f, 1e-4f);
    CHECK(pc_ui_preset_stop_color("#808080", fg, bg, c));
    CHECK_NEAR(c[0], 128.0f / 255.0f, 1e-4f);

    /* named tokens copy the matching slot */
    CHECK(pc_ui_preset_stop_color("foreground", fg, bg, c));
    CHECK_NEAR(c[0], 0.1f, 1e-6f);
    CHECK_NEAR(c[2], 0.3f, 1e-6f);
    CHECK(pc_ui_preset_stop_color("background", fg, bg, c));
    CHECK_NEAR(c[1], 0.8f, 1e-6f);

    /* malformed / hostile input is rejected, never crashes */
    CHECK(!pc_ui_preset_stop_color(NULL, fg, bg, c));
    CHECK(!pc_ui_preset_stop_color("", fg, bg, c));
    CHECK(!pc_ui_preset_stop_color("#fff", fg, bg, c));       /* 3 digits unsupported */
    CHECK(!pc_ui_preset_stop_color("#gg0000", fg, bg, c));    /* non-hex */
    CHECK(!pc_ui_preset_stop_color("#ff00000", fg, bg, c));   /* 7 digits */
    CHECK(!pc_ui_preset_stop_color("foreground", NULL, bg, c)); /* no fg available */
    CHECK(!pc_ui_preset_stop_color("background", fg, NULL, c)); /* no bg available */
    CHECK(!pc_ui_preset_stop_color("red", fg, bg, c));       /* unknown name */
}

static void test_gradient_sample(void)
{
    /* black -> white across [0,1] */
    const float locs2[2] = {0.0f, 1.0f};
    const float rgb2[2][3] = {{0, 0, 0}, {1, 1, 1}};
    float c[3];
    pc_ui_preset_gradient_sample(locs2, rgb2, 2, 0.0f, c);
    CHECK_NEAR(c[0], 0.0f, 1e-4f);
    pc_ui_preset_gradient_sample(locs2, rgb2, 2, 1.0f, c);
    CHECK_NEAR(c[0], 1.0f, 1e-4f);
    pc_ui_preset_gradient_sample(locs2, rgb2, 2, 0.5f, c);
    CHECK_NEAR(c[0], 0.5f, 1e-4f);
    CHECK_NEAR(c[1], 0.5f, 1e-4f);
    CHECK_NEAR(c[2], 0.5f, 1e-4f);

    /* ends clamp beyond the stop range */
    pc_ui_preset_gradient_sample(locs2, rgb2, 2, -0.5f, c);
    CHECK_NEAR(c[0], 0.0f, 1e-4f);
    pc_ui_preset_gradient_sample(locs2, rgb2, 2, 2.0f, c);
    CHECK_NEAR(c[0], 1.0f, 1e-4f);

    /* three stops; interpolation inside the first and second span */
    const float locs3[3] = {0.0f, 0.5f, 1.0f};
    const float rgb3[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    pc_ui_preset_gradient_sample(locs3, rgb3, 3, 0.25f, c); /* halfway red->green */
    CHECK_NEAR(c[0], 0.5f, 1e-4f);
    CHECK_NEAR(c[1], 0.5f, 1e-4f);
    CHECK_NEAR(c[2], 0.0f, 1e-4f);
    pc_ui_preset_gradient_sample(locs3, rgb3, 3, 0.75f, c); /* halfway green->blue */
    CHECK_NEAR(c[1], 0.5f, 1e-4f);
    CHECK_NEAR(c[2], 0.5f, 1e-4f);

    /* unsorted input is handled (same result as the sorted array) */
    const float locsU[3] = {1.0f, 0.0f, 0.5f};
    const float rgbU[3][3] = {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}};
    pc_ui_preset_gradient_sample(locsU, rgbU, 3, 0.25f, c);
    CHECK_NEAR(c[0], 0.5f, 1e-4f);
    CHECK_NEAR(c[1], 0.5f, 1e-4f);

    /* a single stop is a flat colour everywhere */
    const float locs1[1] = {0.3f};
    const float rgb1[1][3] = {{0.2f, 0.4f, 0.6f}};
    pc_ui_preset_gradient_sample(locs1, rgb1, 1, 0.0f, c);
    CHECK_NEAR(c[1], 0.4f, 1e-6f);
    pc_ui_preset_gradient_sample(locs1, rgb1, 1, 0.9f, c);
    CHECK_NEAR(c[2], 0.6f, 1e-6f);

    /* duplicate locations do not divide by zero */
    const float locsD[3] = {0.0f, 0.5f, 0.5f};
    const float rgbD[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    pc_ui_preset_gradient_sample(locsD, rgbD, 3, 0.5f, c);
    CHECK(c[0] >= 0.0f && c[0] <= 1.0f);
    CHECK(c[1] >= 0.0f && c[1] <= 1.0f);

    /* degenerate args are no-ops (sentinel untouched) */
    float keep[3] = {9, 9, 9};
    pc_ui_preset_gradient_sample(locs2, rgb2, 0, 0.5f, keep);
    CHECK_NEAR(keep[0], 9.0f, 1e-6f);
    pc_ui_preset_gradient_sample(NULL, rgb2, 2, 0.5f, keep);
    CHECK_NEAR(keep[0], 9.0f, 1e-6f);
}

int main(int argc, char **argv)
{
    RUN(test_stop_color_tokens);
    RUN(test_gradient_sample);
    return pc_t_finish();
}
