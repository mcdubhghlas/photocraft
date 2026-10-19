#include "harness.h"
#include "ui/brush_resize.h"
#include <math.h>
#include <float.h>
static void test_screen_deltas(void)
{
    float out[2];
    pc_ui_brush_resized(20, .5f, 10, 40, 2, out);
    CHECK_NEAR(out[0], 30, 0);
    CHECK_NEAR(out[1], .7f, 0);
    pc_ui_brush_resized(20, .5f, -100, -500, .5f, out);
    CHECK_NEAR(out[0], 1, 0);
    CHECK_NEAR(out[1], 0, 0);
    pc_ui_brush_resized(20, .5f, FLT_MAX, FLT_MAX, FLT_MIN, out);
    CHECK_NEAR(out[0], 5000, 0);
    CHECK_NEAR(out[1], 1, 0);
    pc_ui_brush_resized(NAN, NAN, NAN, INFINITY, 0, out);
    CHECK_NEAR(out[0], 1, 0);
    CHECK_NEAR(out[1], 0, 0);
    CHECK(pc_ui_brush_resize_applies(PC_TOOL_BRUSH));
    CHECK(pc_ui_brush_resize_applies(PC_TOOL_QUICK_SELECTION));
    CHECK(!pc_ui_brush_resize_applies(PC_TOOL_MOVE));
}
int main(int argc, char **argv)
{
    RUN(test_screen_deltas);
    return pc_t_finish();
}
