#include "harness.h"
#include "ui/stroke_constraint.h"
#include <math.h>

static bool close2(const double a[2], const double b[2])
{
    return fabs(a[0] - b[0]) < 1e-6 && fabs(a[1] - b[1]) < 1e-6;
}

static void test_applies_connects(void)
{
    CHECK(pc_ui_stroke_constraint_applies(PC_TOOL_BRUSH));
    CHECK(pc_ui_stroke_constraint_applies(PC_TOOL_ERASER));
    CHECK(pc_ui_stroke_constraint_applies(PC_TOOL_GRADIENT));
    CHECK(!pc_ui_stroke_constraint_applies(PC_TOOL_MOVE));
    CHECK(pc_ui_stroke_constraint_connects(PC_TOOL_BRUSH));
    CHECK(!pc_ui_stroke_constraint_connects(PC_TOOL_GRADIENT)); /* gradient snaps, never connects */
    CHECK(!pc_ui_stroke_constraint_connects(PC_TOOL_MOVE));
}

static void test_snap45_gradient(void)
{
    double o[2] = {10, 10}, out[2];
    double h[2] = {50, 13};
    pc_ui_stroke_snap45(o, h, out);
    double exp_h[2] = {50, 10};
    CHECK(close2(out, exp_h)); /* horizontal */
    double v[2] = {12, -40};
    pc_ui_stroke_snap45(o, v, out);
    double exp_v[2] = {10, -40};
    CHECK(close2(out, exp_v)); /* vertical */
    double d[2] = {40, 37};
    pc_ui_stroke_snap45(o, d, out);
    CHECK(fabs(out[0] - out[1]) < 1e-9 && out[0] > 30); /* 45 degrees */
    pc_ui_stroke_snap45(o, o, out);
    CHECK(close2(out, o)); /* coincident passes through */
    double nan_in[2] = {NAN, 1};
    pc_ui_stroke_snap45(o, nan_in, out);
    CHECK(isnan(out[0])); /* non-finite passes through */
}

static void test_constrain_lock(void)
{
    double o[2] = {10, 10}, out[2];
    pc_ui_stroke_axis axis = {0};
    double p1[2] = {11, 11};
    pc_ui_stroke_constrain(PC_TOOL_BRUSH, &axis, o, o, p1, true, 1.0f, out);
    CHECK(close2(out, o)); /* inside the lock distance stays at origin */
    double p2[2] = {30, 12};
    pc_ui_stroke_constrain(PC_TOOL_BRUSH, &axis, o, o, p2, true, 1.0f, out);
    double exp2[2] = {30, 10};
    CHECK(close2(out, exp2)); /* locks horizontal */
    double p3[2] = {60, 45};
    pc_ui_stroke_constrain(PC_TOOL_BRUSH, &axis, o, o, p3, true, 1.0f, out);
    double exp3[2] = {60, 10};
    CHECK(close2(out, exp3)); /* wandering toward 45 stays on the locked horizontal */
    /* Releasing Shift frees the stroke and drops the lock. */
    double p4[2] = {70, 50};
    pc_ui_stroke_constrain(PC_TOOL_BRUSH, &axis, o, o, p4, false, 1.0f, out);
    CHECK(close2(out, p4) && !axis.active);
}

static void test_constrain_vertical_diagonal(void)
{
    double o[2] = {10, 10}, out[2];
    pc_ui_stroke_axis axis = {0};
    double pv[2] = {9, 60};
    pc_ui_stroke_constrain(PC_TOOL_BRUSH, &axis, o, o, pv, true, 1.0f, out);
    double expv[2] = {10, 60};
    CHECK(close2(out, expv)); /* vertical lock */
    pc_ui_stroke_axis axis2 = {0};
    double pd[2] = {40, 43};
    pc_ui_stroke_constrain(PC_TOOL_BRUSH, &axis2, o, o, pd, true, 1.0f, out);
    CHECK(fabs((out[0] - 10) - (out[1] - 10)) < 1e-9); /* diagonal lock */
    /* Other tools pass through untouched. */
    pc_ui_stroke_axis none = {0};
    pc_ui_stroke_constrain(PC_TOOL_MOVE, &none, o, o, pd, true, 1.0f, out);
    CHECK(close2(out, pd));
}

static void test_constrain_gradient_each_event(void)
{
    /* Gradient snaps from the start point every event, with no locked axis. */
    double o[2] = {10, 10}, out[2];
    pc_ui_stroke_axis axis = {0};
    double p[2] = {90, 22};
    pc_ui_stroke_constrain(PC_TOOL_GRADIENT, &axis, o, o, p, true, 1.0f, out);
    double exp[2] = {90, 10};
    CHECK(close2(out, exp));
    CHECK(!axis.active); /* gradient never captures an axis */
    double d[2] = {60, 52};
    pc_ui_stroke_constrain(PC_TOOL_GRADIENT, &axis, o, o, d, true, 1.0f, out);
    CHECK(fabs(out[0] - out[1]) < 1e-9); /* 45 */
}

int main(int argc, char **argv)
{
    RUN(test_applies_connects);
    RUN(test_snap45_gradient);
    RUN(test_constrain_lock);
    RUN(test_constrain_vertical_diagonal);
    RUN(test_constrain_gradient_each_event);
    return pc_t_finish();
}
