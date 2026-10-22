/* Headless unit tests for the Brush Settings pane maths (src/ui/brush_tip.c): the angle/roundness
 * ellipse control, the logarithmic size slider, the tip-grid thumbnail scale, and the texture
 * Brightness/Contrast range mappings. Mirrors crates/ui-egui/src/brush_sections.rs's tests. */
#include "harness.h"

#include "ui/brush_tip.h"

#include <math.h>

static void test_ellipse_angle_drag(void)
{
    float angle = 0, round = 1;
    /* Pointing straight up (screen y down) is +90deg counter-clockwise. */
    pc_ui_ellipse_drag(PC_UI_ELLIPSE_ANGLE, 0, -20, &angle, &round);
    CHECK_NEAR(angle, 90, 1e-4);
    CHECK_NEAR(round, 1, 0);
    /* Pointing left is +-180deg. */
    angle = 0;
    pc_ui_ellipse_drag(PC_UI_ELLIPSE_ANGLE, -20, 0, &angle, &round);
    CHECK_NEAR(fabsf(angle), 180, 1e-4);
    /* A press within the 1 px dead zone leaves the angle alone. */
    angle = 30;
    round = 0.5f;
    pc_ui_ellipse_drag(PC_UI_ELLIPSE_ANGLE, 0.2f, 0.1f, &angle, &round);
    CHECK_NEAR(angle, 30, 0);
    CHECK_NEAR(round, 0.5f, 0);
}

static void test_ellipse_roundness_drag(void)
{
    /* At angle 0 the minor axis is vertical: half the radius up is 50 % roundness. */
    float angle = 0, round = 1;
    pc_ui_ellipse_drag(PC_UI_ELLIPSE_ROUNDNESS, 0, -PC_UI_ELLIPSE_R / 2, &angle, &round);
    CHECK_NEAR(round, 0.5f, 1e-4);
    CHECK_NEAR(angle, 0, 0);
    /* Rotated 90deg the minor axis is horizontal; a far drag clamps to 1. */
    angle = 90;
    round = 0.2f;
    pc_ui_ellipse_drag(PC_UI_ELLIPSE_ROUNDNESS, 500, 0, &angle, &round);
    CHECK_NEAR(round, 1, 0);
    /* Clamped to a 0.01 floor, never zero. */
    angle = 0;
    round = 0.5f;
    pc_ui_ellipse_drag(PC_UI_ELLIPSE_ROUNDNESS, 0, 0, &angle, &round);
    CHECK_NEAR(round, 0.01f, 0);
}

static void test_ellipse_handles_and_grab(void)
{
    float h[2][2];
    /* Handles sit on the minor axis; at angle 0, roundness 0.5 the first is half-radius up. */
    pc_ui_ellipse_handles(0, 0.5f, h);
    CHECK_NEAR(h[0][0], 0, 1e-4);
    CHECK_NEAR(h[0][1], -PC_UI_ELLIPSE_R / 2, 1e-4);
    CHECK_NEAR(h[1][0], 0, 1e-4);
    CHECK_NEAR(h[1][1], PC_UI_ELLIPSE_R / 2, 1e-4);
    /* A press on the handle grabs roundness; far off the minor axis grabs the angle. */
    CHECK(pc_ui_ellipse_grab_at(h[0][0], h[0][1], 0, 0.5f) == PC_UI_ELLIPSE_ROUNDNESS);
    CHECK(pc_ui_ellipse_grab_at(PC_UI_ELLIPSE_R, 0, 0, 0.5f) == PC_UI_ELLIPSE_ANGLE);
    /* Rotated 90deg the minor axis is horizontal. */
    pc_ui_ellipse_handles(90, 1, h);
    CHECK_NEAR(fabsf(h[0][0]), PC_UI_ELLIPSE_R, 1e-3);
    CHECK_NEAR(h[0][1], 0, 1e-3);
}

static void test_size_slider_roundtrip(void)
{
    float max = 5000;
    /* Slider position is the natural log of the size. */
    CHECK_NEAR(pc_ui_brush_size_slider(1, max), 0, 1e-5);
    CHECK_NEAR(pc_ui_brush_size_slider(100, max), logf(100), 1e-4);
    /* Round-trip at several sizes recovers the whole-pixel size. */
    float sizes[] = {1, 5, 30, 100, 1000, 5000};
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        float back = pc_ui_brush_slider_size(pc_ui_brush_size_slider(sizes[i], max), max);
        CHECK_NEAR(back, sizes[i], 1);
    }
    /* Clamped to 1..max at both ends. */
    CHECK_NEAR(pc_ui_brush_slider_size(-10, max), 1, 0);
    CHECK_NEAR(pc_ui_brush_slider_size(1e9f, max), max, 0);
    CHECK_NEAR(pc_ui_brush_size_slider(0.1f, max), 0, 1e-5);
}

static void test_thumb_scale(void)
{
    CHECK_NEAR(pc_ui_brush_thumb_scale(40), 1, 0);
    CHECK_NEAR(pc_ui_brush_thumb_scale(80), 1, 0);
    CHECK_NEAR(pc_ui_brush_thumb_scale(20), 0.5f, 1e-5);
    CHECK_NEAR(pc_ui_brush_thumb_scale(1), 0.30f, 0); /* 1/40 < 0.3 -> floor */
}

static void test_texture_range_mappings(void)
{
    /* Brightness +-150 <-> +-1. */
    CHECK_NEAR(pc_ui_texture_brightness_to_ps(1), 150, 0);
    CHECK_NEAR(pc_ui_texture_brightness_to_ps(-1), -150, 0);
    CHECK_NEAR(pc_ui_texture_brightness_from_ps(75), 0.5f, 1e-6);
    /* Contrast: negative through 50, positive through 100. */
    CHECK_NEAR(pc_ui_texture_contrast_to_ps(-1), -50, 0);
    CHECK_NEAR(pc_ui_texture_contrast_to_ps(1), 100, 0);
    CHECK_NEAR(pc_ui_texture_contrast_from_ps(-25), -0.5f, 1e-6);
    CHECK_NEAR(pc_ui_texture_contrast_from_ps(50), 0.5f, 1e-6);
    /* Round-trip through both directions. */
    CHECK_NEAR(pc_ui_texture_brightness_from_ps(pc_ui_texture_brightness_to_ps(0.4f)), 0.4f, 1e-6);
    CHECK_NEAR(pc_ui_texture_contrast_from_ps(pc_ui_texture_contrast_to_ps(-0.6f)), -0.6f, 1e-6);
}

static void test_hostile_inputs(void)
{
    float nan = nanf(""), inf = INFINITY;
    /* Non-finite coordinates never change the ellipse values. */
    float angle = 42, round = 0.3f;
    pc_ui_ellipse_drag(PC_UI_ELLIPSE_ANGLE, nan, inf, &angle, &round);
    pc_ui_ellipse_drag(PC_UI_ELLIPSE_ROUNDNESS, nan, nan, &angle, &round);
    CHECK_NEAR(angle, 42, 0);
    CHECK_NEAR(round, 0.3f, 0);
    /* NULL out-pointers are ignored (no crash). */
    pc_ui_ellipse_drag(PC_UI_ELLIPSE_ANGLE, 10, 10, NULL, NULL);
    pc_ui_ellipse_handles(0, 0.5f, NULL);
    (void)pc_ui_ellipse_grab_at(nan, nan, 0, 0.5f);
    /* Degenerate maxima and non-finite sizes stay bounded. */
    CHECK_NEAR(pc_ui_brush_slider_size(5, nan), 1, 0);
    CHECK_NEAR(pc_ui_brush_slider_size(5, 0), 1, 0);
    CHECK(pc_ui_brush_thumb_scale(nan) == 0.30f);
    CHECK(pc_ui_brush_size_slider(nan, 5000) == 0.0f);
    CHECK(isfinite(pc_ui_texture_brightness_to_ps(nan)));
    CHECK(isfinite(pc_ui_texture_contrast_to_ps(inf)));
}

int main(int argc, char **argv)
{
    RUN(test_ellipse_angle_drag);
    RUN(test_ellipse_roundness_drag);
    RUN(test_ellipse_handles_and_grab);
    RUN(test_size_slider_roundtrip);
    RUN(test_thumb_scale);
    RUN(test_texture_range_mappings);
    RUN(test_hostile_inputs);
    return pc_t_finish();
}
