#include "harness.h"
#include "ui/crop.h"
#include "ui/crop_overlay.h"
#include "ui/crop_size.h"
#include <math.h>
#include <string.h>
static pc_ui_crop_frame frame(void)
{
    return (pc_ui_crop_frame){.present = true, .rect = {0, 0, 100, 80}};
}
static void test_draw_ratio_center_and_space(void)
{
    pc_ui_crop_frame f = frame();
    pc_ui_crop_drag drag;
    const double start[2] = {20, 20}, end[2] = {80, 60}, reposition[2] = {90, 65};
    CHECK(pc_ui_crop_begin(&drag, &f, start, 4, true, false));
    CHECK(drag.draw);
    CHECK(pc_ui_crop_motion(&f, &drag, end, 0, false, false, false));
    CHECK(f.rect[0] == 20 && f.rect[1] == 20 && f.rect[2] == 80 && f.rect[3] == 60);
    CHECK(pc_ui_crop_motion(&f, &drag, reposition, 0, false, false, true));
    CHECK(f.rect[0] == 30 && f.rect[1] == 25 && f.rect[2] == 90 && f.rect[3] == 65);
    f = frame();
    CHECK(pc_ui_crop_begin(&drag, &f, start, 4, true, false));
    CHECK(pc_ui_crop_motion(&f, &drag, end, 0, true, true, false));
    CHECK(f.rect[0] == -40 && f.rect[1] == -40 && f.rect[2] == 80 && f.rect[3] == 80);
    f = frame();
    CHECK(pc_ui_crop_begin(&drag, &f, start, 4, true, false));
    CHECK(pc_ui_crop_motion(&f, &drag, end, 2, false, false, false));
    CHECK_NEAR((f.rect[2] - f.rect[0]) / (f.rect[3] - f.rect[1]), 2, 1e-9);
}
static void test_move_image_and_classic_handles(void)
{
    const double start[2] = {40, 30}, end[2] = {50, 35};
    pc_ui_crop_drag drag;
    pc_ui_crop_frame f = frame();
    CHECK(pc_ui_crop_begin(&drag, &f, start, 4, false, true));
    CHECK(pc_ui_crop_motion(&f, &drag, end, 0, false, false, false));
    CHECK_NEAR(f.rect[0], 10, 1e-8);
    CHECK_NEAR(f.rect[1], 5, 1e-8);
    f = frame();
    CHECK(pc_ui_crop_begin(&drag, &f, start, 4, false, false));
    CHECK(drag.image);
    CHECK(pc_ui_crop_motion(&f, &drag, end, 0, false, false, false));
    CHECK_NEAR(f.rect[0], -10, 1e-8);
    CHECK_NEAR(f.rect[1], -5, 1e-8);
    const double corner[2] = {100, 80}, resize[2] = {140, 90};
    f = frame();
    CHECK(pc_ui_crop_begin(&drag, &f, corner, 8, false, true));
    CHECK(pc_ui_crop_motion(&f, &drag, resize, 0, false, false, false));
    CHECK_NEAR(f.rect[2], 140, 1e-8);
    CHECK_NEAR(f.rect[3], 90, 1e-8);
    f = frame();
    CHECK(pc_ui_crop_begin(&drag, &f, corner, 8, false, true));
    CHECK(pc_ui_crop_motion(&f, &drag, resize, 0, true, false, false));
    CHECK_NEAR(f.rect[2] / f.rect[3], 1.25, 1e-8);
    f = frame();
    CHECK(pc_ui_crop_begin(&drag, &f, corner, 8, false, true));
    CHECK(pc_ui_crop_motion(&f, &drag, resize, 2, false, false, false));
    CHECK_NEAR((f.rect[2] - f.rect[0]) / (f.rect[3] - f.rect[1]), 2, 1e-8);
}
static void test_rotation_swap_and_hostile_points(void)
{
    pc_ui_crop_frame f = frame();
    pc_ui_crop_drag drag;
    const double start[2] = {150, 40}, end[2] = {50, 140};
    CHECK(pc_ui_crop_begin(&drag, &f, start, 4, false, true));
    CHECK(pc_ui_crop_motion(&f, &drag, end, 0, false, false, false));
    CHECK_NEAR(f.angle, 90, 1e-8);
    CHECK_NEAR(f.rect[0], 0, 1e-8);
    CHECK_NEAR(f.rect[2], 100, 1e-8);
    f = frame();
    CHECK(pc_ui_crop_begin(&drag, &f, start, 4, false, false));
    CHECK(pc_ui_crop_motion(&f, &drag, end, 0, false, false, false));
    CHECK_NEAR(f.angle, -90, 1e-8);
    CHECK(pc_ui_crop_swap(&f));
    CHECK_NEAR(f.rect[2] - f.rect[0], 80, 1e-8);
    CHECK_NEAR(f.rect[3] - f.rect[1], 100, 1e-8);
    pc_ui_crop_frame before = f;
    const double bad[2] = {NAN, 0};
    CHECK(!pc_ui_crop_motion(&f, &drag, bad, 0, false, false, false));
    CHECK(!memcmp(&f, &before, sizeof f));
    CHECK(!pc_ui_crop_begin(&drag, &f, start, 0, false, false));
    CHECK(!pc_ui_crop_motion(&f, &drag, end, INFINITY, false, false, false));
    CHECK(!memcmp(&f, &before, sizeof f));
}
static void test_crop_overlays_bounded_and_oriented(void)
{
    float lines[PC_UI_CROP_GUIDE_MAX][4], other[PC_UI_CROP_GUIDE_MAX][4];
    for (pc_ui_crop_overlay kind = PC_UI_CROP_THIRDS; kind <= PC_UI_CROP_GOLDEN_SPIRAL; kind++) {
        size_t n = pc_ui_crop_guides(kind, 0, 320, 240, lines);
        CHECK(n > 0 && n <= PC_UI_CROP_GUIDE_MAX);
        for (size_t i = 0; i < n; i++)
            for (size_t a = 0; a < 4; a++)
                CHECK(isfinite(lines[i][a]) && lines[i][a] >= 0 && lines[i][a] <= 1);
        CHECK(pc_ui_crop_guides(kind, 0, NAN, 240, other) == 0);
        CHECK(pc_ui_crop_guides(kind, 0, 0, 240, other) == 0);
        CHECK(pc_ui_crop_guides(kind, 0, FLT_MAX, FLT_MAX, other) <= PC_UI_CROP_GUIDE_MAX);
    }
    CHECK(pc_ui_crop_guides(PC_UI_CROP_GRID, 0, FLT_MAX, FLT_MAX, lines) == 190);
    size_t n = pc_ui_crop_guides(PC_UI_CROP_TRIANGLE, 0, 320, 240, lines);
    CHECK(pc_ui_crop_guides(PC_UI_CROP_TRIANGLE, 1, 320, 240, other) == n);
    for (size_t i = 0; i < n; i++)
        for (size_t a = 0; a < 4; a++)
            CHECK_NEAR(other[i][a], a % 2 ? lines[i][a] : 1 - lines[i][a], 1e-7);
}
static void test_crop_size_units_decimal_comma_and_full_validation(void)
{
    const char *text[] = {"4 in", "2.54cm", "25,4 mm", "72 pt", "6 pc", "1024px", "12", " 1 inch  "};
    const double expected[] = {1200, 300, 300, 300, 300, 1024, 12, 300};
    for (size_t i = 0; i < sizeof text / sizeof text[0]; i++) {
        double px = 0;
        CHECK(pc_ui_crop_pixels(text[i], 300, &px));
        CHECK_NEAR(px, expected[i], 1e-6);
    }
    const char *bad[] = {"", "0", "-1 px", "NaN", "inf", "1e3px", "2 %%", "4cmjunk", "5.2.3", "1000000001px"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        double px = 42;
        CHECK(!pc_ui_crop_pixels(bad[i], 300, &px));
        CHECK(px == 42);
    }
    double ppi = 0;
    CHECK(pc_ui_crop_resolution("100", "px/cm", &ppi));
    CHECK_NEAR(ppi, 254, 1e-8);
    CHECK(!pc_ui_crop_resolution("100junk", "px/in", &ppi));
    CHECK(!pc_ui_crop_resolution("100", "bad", &ppi));
}
static void test_handle_keeps_press_offset_and_stationary_frame(void)
{
    pc_ui_crop_frame f = frame(), before = f;
    pc_ui_crop_drag drag;
    double p[2] = {104, 78};
    CHECK(pc_ui_crop_begin(&drag, &f, p, 8, false, true));
    CHECK(pc_ui_crop_motion(&f, &drag, p, 0, false, false, false));
    for (size_t i = 0; i < 4; i++)
        CHECK_NEAR(f.rect[i], before.rect[i], 1e-7);
    p[0] += 20;
    p[1] += 20;
    CHECK(pc_ui_crop_motion(&f, &drag, p, 0, false, false, false));
    CHECK_NEAR(f.rect[2], 120, 1e-7);
    CHECK_NEAR(f.rect[3], 100, 1e-7);
}
int main(int argc, char **argv)
{
    RUN(test_crop_overlays_bounded_and_oriented);
    RUN(test_crop_size_units_decimal_comma_and_full_validation);
    RUN(test_handle_keeps_press_offset_and_stationary_frame);
    RUN(test_draw_ratio_center_and_space);
    RUN(test_move_image_and_classic_handles);
    RUN(test_rotation_swap_and_hostile_points);
    return pc_t_finish();
}
