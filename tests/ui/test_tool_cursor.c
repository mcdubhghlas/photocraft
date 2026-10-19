#include <float.h>
#include <math.h>
#include <string.h>
#include "harness.h"
#include "ui/tool_cursor.h"

static uint8_t alpha(const pc_ui_cursor_bitmap *image, unsigned x, unsigned y)
{
    CHECK(x < image->side && y < image->side);
    return image->rgba[((size_t)y * image->side + x) * 4 + 3];
}
static void test_circle_and_cross(void)
{
    const float scales[] = {1, 1.25f, 1.5f, 2, 3};
    for (size_t i = 0; i < sizeof scales / sizeof *scales; i++) {
        pc_ui_cursor_shape shape = {.kind = PC_UI_CURSOR_CIRCLE, .radius = 20};
        pc_ui_cursor_bitmap bitmap = {0};
        float scale = scales[i];
        CHECK(pc_ui_cursor_rasterize(&shape, scale, 255, &bitmap, NULL) == PC_OK);
        unsigned hot = bitmap.hotspot, edge = hot + (unsigned)(20 * scale);
        CHECK(bitmap.side == hot * 2 + 1);
        CHECK(alpha(&bitmap, hot, hot) == 0);
        CHECK(alpha(&bitmap, 0, 0) == 0);
        CHECK(alpha(&bitmap, edge, hot) > 230);
        CHECK(alpha(&bitmap, hot - (unsigned)(20 * scale), hot) == alpha(&bitmap, edge, hot));
        CHECK(bitmap.rgba[((size_t)hot * bitmap.side + edge) * 4] == 242);
        shape.center = true;
        CHECK(pc_ui_cursor_rasterize(&shape, scale, 255, &bitmap, NULL) == PC_OK);
        CHECK(alpha(&bitmap, bitmap.hotspot, bitmap.hotspot) > 230);
        pc_ui_cursor_bitmap_fini(&bitmap);
    }
    pc_ui_cursor_shape cross = {.kind = PC_UI_CURSOR_CROSS, .length = 8, .gap = 2};
    pc_ui_cursor_bitmap bitmap = {0};
    CHECK(pc_ui_cursor_rasterize(&cross, 1, 255, &bitmap, NULL) == PC_OK);
    CHECK(alpha(&bitmap, bitmap.hotspot, bitmap.hotspot) == 0);
    CHECK(alpha(&bitmap, bitmap.hotspot + 5u, bitmap.hotspot) > 230);
    pc_ui_cursor_bitmap_fini(&bitmap);
}
static void test_limits_and_outlines(void)
{
    pc_ui_cursor_shape shape = {.kind = PC_UI_CURSOR_CIRCLE, .radius = 20};
    pc_ui_cursor_bitmap bitmap = {0};
    CHECK(pc_ui_cursor_rasterize(&shape, 1, 255, &bitmap, NULL) == PC_OK);
    uint8_t *original = bitmap.rgba;
    const float invalid[] = {0, -1, NAN, INFINITY, -INFINITY, FLT_MAX};
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        shape.radius = invalid[i];
        CHECK(pc_ui_cursor_rasterize(&shape, 1, 255, &bitmap, NULL) != PC_OK);
        CHECK(bitmap.rgba == original);
        shape.radius = 20;
        CHECK(pc_ui_cursor_rasterize(&shape, invalid[i], 255, &bitmap, NULL) != PC_OK);
        CHECK(bitmap.rgba == original);
    }
    shape.radius = 200;
    CHECK(pc_ui_cursor_rasterize(&shape, 1, 255, &bitmap, NULL) == PC_ERR_LIMIT);
    CHECK(pc_ui_cursor_rasterize(&shape, 1, 2047, &bitmap, NULL) == PC_OK);
    float points[][2] = {{-20, -10}, {20, -10}, {20, 10}, {-20, 10}};
    pc_paint_ring ring = {.pts = points, .n = 4};
    shape = (pc_ui_cursor_shape){.kind = PC_UI_CURSOR_OUTLINE, .rings = &ring, .count = 1, .center = true};
    CHECK(pc_ui_cursor_rasterize(&shape, 2, 255, &bitmap, NULL) == PC_OK);
    CHECK(alpha(&bitmap, bitmap.hotspot, bitmap.hotspot) > 230);
    CHECK(alpha(&bitmap, bitmap.hotspot + 40u, bitmap.hotspot) > 230);
    CHECK(alpha(&bitmap, bitmap.hotspot + 20u, bitmap.hotspot) == 0);
    original = bitmap.rgba;
    points[0][0] = NAN;
    CHECK(pc_ui_cursor_rasterize(&shape, 1, 255, &bitmap, NULL) == PC_ERR_INVALID);
    CHECK(bitmap.rgba == original);
    pc_ui_cursor_bitmap_fini(&bitmap);
}
static void test_hands_and_badges(void)
{
    pc_ui_cursor_bitmap bitmap = {0};
    uint8_t hash[4] = {0};
    for (unsigned closed = 0; closed < 2; closed++) {
        pc_ui_cursor_shape shape = {.kind = PC_UI_CURSOR_HAND, .closed = closed != 0};
        CHECK(pc_ui_cursor_rasterize(&shape, 2, 255, &bitmap, NULL) == PC_OK);
        CHECK(bitmap.side <= 68);
        CHECK(alpha(&bitmap, bitmap.hotspot, bitmap.hotspot) > 230);
        CHECK(alpha(&bitmap, 0, 0) == 0);
        bool white = false, dark = false;
        for (size_t i = 0; i < (size_t)bitmap.side * bitmap.side; i++) {
            const uint8_t *pixel = &bitmap.rgba[i * 4];
            white |= pixel[0] > 230 && pixel[3] > 230;
            dark |= pixel[0] < 25 && pixel[3] > 100;
            hash[closed] ^= pixel[3];
        }
        CHECK(white && dark);
    }
    CHECK(hash[0] != hash[1]);
    for (pc_ui_badge badge = PC_UI_BADGE_ADD; badge <= PC_UI_BADGE_INTERSECT; badge++) {
        pc_ui_cursor_shape shape = {.kind = PC_UI_CURSOR_CROSS, .length = 6, .badge = badge};
        CHECK(pc_ui_cursor_rasterize(&shape, 1, 255, &bitmap, NULL) == PC_OK);
        CHECK(alpha(&bitmap, bitmap.hotspot + 12u, bitmap.hotspot + 12u) > 230);
    }
    pc_ui_cursor_bitmap_fini(&bitmap);
}
static void test_selection_modes(void)
{
    CHECK(pc_ui_is_selection_tool(PC_TOOL_MAGNETIC_LASSO));
    CHECK(!pc_ui_is_selection_tool(PC_TOOL_BRUSH));
    CHECK_EQ_STR(pc_ui_selection_mode(PC_TOOL_QUICK_SELECTION, 0, 0, false), "add");
    CHECK_EQ_STR(pc_ui_selection_mode(PC_TOOL_QUICK_SELECTION, 0, PC_UI_MOD_ALT, false), "subtract");
    CHECK_EQ_STR(pc_ui_selection_mode(PC_TOOL_RECT_MARQUEE, 0, PC_UI_MOD_ALT, false), "replace");
    CHECK_EQ_STR(pc_ui_selection_mode(PC_TOOL_RECT_MARQUEE, 0, PC_UI_MOD_ALT, true), "subtract");
    CHECK_EQ_STR(pc_ui_selection_mode(PC_TOOL_RECT_MARQUEE, 0, PC_UI_MOD_ALT | PC_UI_MOD_SHIFT, true), "intersect");
    CHECK_EQ_STR(pc_ui_selection_mode(PC_TOOL_RECT_MARQUEE, 2, 0, false), "subtract");
    CHECK(pc_ui_badge_from_mode("add") == PC_UI_BADGE_ADD);
    CHECK(pc_ui_badge_from_mode(NULL) == PC_UI_BADGE_NONE);
}
int main(int argc, char **argv)
{
    RUN(test_circle_and_cross);
    RUN(test_limits_and_outlines);
    RUN(test_hands_and_badges);
    RUN(test_selection_modes);
    return pc_t_finish();
}
