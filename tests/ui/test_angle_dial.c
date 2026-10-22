#include "harness.h"
#include "ui/angle_dial.h"
#include <math.h>

static void test_from_point_directions(void)
{
    double out = -999;
    /* Pointer to the right of centre: 0 deg. */
    CHECK(pc_ui_angle_from_point(50, 50, 70, 50, -180, 180, &out));
    CHECK_NEAR(out, 0, 0.001);
    /* Up (screen y smaller): +90 deg (y up). */
    CHECK(pc_ui_angle_from_point(50, 50, 50, 35, -180, 180, &out));
    CHECK_NEAR(out, 90, 0.001);
    /* Down: -90. */
    CHECK(pc_ui_angle_from_point(50, 50, 50, 65, -180, 180, &out));
    CHECK_NEAR(out, -90, 0.001);
}

static void test_from_point_clamp_and_deadzone(void)
{
    double out = 7;
    /* Within 1px of centre: no change, returns false. */
    CHECK(!pc_ui_angle_from_point(50, 50, 50.5, 50.0, -180, 180, &out));
    CHECK(out == 7);
    /* Narrow range clamps: pointing up-left (~172 deg) stops at 90. */
    CHECK(pc_ui_angle_from_point(50, 50, 35, 48, -90, 90, &out));
    CHECK(out == 90);
    /* NaN-safe. */
    CHECK(!pc_ui_angle_from_point(50, 50, NAN, 50, -180, 180, &out));
}

static void test_step_wrap_vs_clamp(void)
{
    /* Half-turn range wraps. */
    CHECK_NEAR(pc_ui_angle_step(180, 1, -180, 180), -179, 0.001);
    CHECK_NEAR(pc_ui_angle_step(-180, -1, -180, 180), 179, 0.001);
    /* Wide range steps on and clamps. */
    CHECK_NEAR(pc_ui_angle_step(270, 1, -360, 360), 271, 0.001);
    CHECK_NEAR(pc_ui_angle_step(360, 5, -360, 360), 360, 0.001);
}

static void test_tip(void)
{
    double tx = 0, ty = 0;
    pc_ui_angle_tip(0, 10, 10, 5, &tx, &ty);
    CHECK_NEAR(tx, 15, 0.001);
    CHECK_NEAR(ty, 10, 0.001);
    pc_ui_angle_tip(90, 10, 10, 5, &tx, &ty);
    CHECK_NEAR(tx, 10, 0.001);
    CHECK_NEAR(ty, 5, 0.001); /* y up => tip above centre */
}

int main(int argc, char **argv)
{
    RUN(test_from_point_directions);
    RUN(test_from_point_clamp_and_deadzone);
    RUN(test_step_wrap_vs_clamp);
    RUN(test_tip);
    return pc_t_finish();
}
