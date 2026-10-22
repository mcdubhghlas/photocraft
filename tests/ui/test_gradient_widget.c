/* Pure gradient on-canvas widget model: endpoint/stop/line/midpoint hit-test, line projection,
 * 45-degree snap, and the 5 styles' decoration geometry. Headless; hostile-input safe (rule 9). */
#include "harness.h"
#include "ui/gradient_widget.h"
#include <math.h>

/* A horizontal line from (10,20) to (110,20) with three colour stops and two midpoints. */
static pc_ui_grad_widget horizontal(void)
{
    pc_ui_grad_widget w = {0};
    w.from[0] = 10;
    w.from[1] = 20;
    w.to[0] = 110;
    w.to[1] = 20;
    w.reverse = false;
    w.stops[0] = 0.0;
    w.stops[1] = 0.5;
    w.stops[2] = 1.0;
    w.nstops = 3;
    w.mids[0] = 0.5;
    w.mids[1] = 0.5;
    w.nmids = 2;
    return w;
}

static void test_at_and_project(void)
{
    pc_ui_grad_widget w = horizontal();
    double at[2];
    pc_ui_grad_widget_at(&w, 0.0, at);
    CHECK_NEAR(at[0], 10, 1e-9);
    pc_ui_grad_widget_at(&w, 1.0, at);
    CHECK_NEAR(at[0], 110, 1e-9);
    pc_ui_grad_widget_at(&w, 0.25, at);
    CHECK_NEAR(at[0], 35, 1e-9);
    CHECK_NEAR(at[1], 20, 1e-9);
    /* Reverse flips the mapping. */
    w.reverse = true;
    pc_ui_grad_widget_at(&w, 0.0, at);
    CHECK_NEAR(at[0], 110, 1e-9);
    pc_ui_grad_widget_at(&w, 0.25, at);
    CHECK_NEAR(at[0], 85, 1e-9);
    w.reverse = false;
    /* Projection: a point above the midpoint. */
    double u = -1, perp = -1;
    pc_ui_grad_widget_project(&w, 60, 5, &u, &perp);
    CHECK_NEAR(u, 0.5, 1e-9);
    CHECK_NEAR(perp, 15, 1e-9);
    /* Past the end clamps u. */
    pc_ui_grad_widget_project(&w, 200, 20, &u, &perp);
    CHECK_NEAR(u, 1.0, 1e-9);
}

static void test_hit_order(void)
{
    pc_ui_grad_widget w = horizontal();
    double tol = 6;
    /* End beats start beats line. */
    pc_ui_grad_grab g = pc_ui_grad_widget_hit(&w, 110, 20, tol);
    CHECK(g.kind == PC_UI_GRAD_GRAB_END);
    g = pc_ui_grad_widget_hit(&w, 10, 20, tol);
    CHECK(g.kind == PC_UI_GRAD_GRAB_START);
    /* The middle colour stop (loc 0.5 -> x=60). */
    g = pc_ui_grad_widget_hit(&w, 60, 20, tol);
    CHECK(g.kind == PC_UI_GRAD_GRAB_STOP);
    CHECK_EQ_INT(g.index, 1);
    /* A segment midpoint: segment 0 midpoint at loc 0.25 -> x=35. */
    g = pc_ui_grad_widget_hit(&w, 35, 20, tol);
    CHECK(g.kind == PC_UI_GRAD_GRAB_MID);
    CHECK_EQ_INT(g.index, 0);
    /* The line body away from any marker (midpoints sit at x=35 and x=85). */
    g = pc_ui_grad_widget_hit(&w, 95, 20, tol);
    CHECK(g.kind == PC_UI_GRAD_GRAB_LINE);
    CHECK_NEAR(g.t, 0.85, 1e-6);
    /* A miss. */
    g = pc_ui_grad_widget_hit(&w, 60, 200, tol);
    CHECK(g.kind == PC_UI_GRAD_GRAB_NONE);
}

static void test_hit_line_t_reversed(void)
{
    pc_ui_grad_widget w = horizontal();
    w.reverse = true;
    /* At x=95 the fraction u=0.85; the gradient location is reversed to 0.15. The midpoints move
     * with reverse, so x=95 is clear of them here. */
    pc_ui_grad_grab g = pc_ui_grad_widget_hit(&w, 95, 25, 6);
    CHECK(g.kind == PC_UI_GRAD_GRAB_LINE);
    CHECK_NEAR(g.t, 0.15, 1e-6);
}

static void test_snap45(void)
{
    double anchor[2] = {0, 0};
    double p[2] = {10, 1}; /* nearly horizontal */
    double out[2];
    pc_ui_grad_snap45(anchor, p, true, out);
    CHECK_NEAR(out[1], 0, 1e-6); /* snapped to 0 degrees, length preserved */
    CHECK_NEAR(hypot(out[0], out[1]), hypot(p[0], p[1]), 1e-9);
    double q[2] = {9, 10}; /* nearly 45 */
    pc_ui_grad_snap45(anchor, q, true, out);
    CHECK_NEAR(out[0], out[1], 1e-6);
    /* No shift copies the point through. */
    pc_ui_grad_snap45(anchor, q, false, out);
    CHECK_NEAR(out[0], 9, 1e-9);
    CHECK_NEAR(out[1], 10, 1e-9);
}

static void test_decorations(void)
{
    pc_ui_grad_widget w = horizontal(); /* from (10,20) to (110,20), length 100 */
    double center[2], radius = 0;
    CHECK(pc_ui_grad_decor_circle(&w, center, &radius));
    CHECK_NEAR(center[0], 10, 1e-9);
    CHECK_NEAR(radius, 100, 1e-9);
    double corners[4][2];
    CHECK(pc_ui_grad_decor_diamond(&w, corners));
    CHECK_NEAR(corners[0][0], 110, 1e-9); /* +d is the `to` tip */
    CHECK_NEAR(corners[0][1], 20, 1e-9);
    CHECK_NEAR(corners[1][0], 10, 1e-9); /* +n: perpendicular (0,100) */
    CHECK_NEAR(corners[1][1], 120, 1e-9);
    CHECK_NEAR(corners[2][0], -90, 1e-9); /* -d */
    CHECK_NEAR(corners[3][1], -80, 1e-9); /* -n */
    double mirror[2];
    CHECK(pc_ui_grad_decor_reflected(&w, mirror));
    CHECK_NEAR(mirror[0], -90, 1e-9); /* 2*from - to */
    CHECK_NEAR(mirror[1], 20, 1e-9);
    /* Degenerate line: no decoration. */
    pc_ui_grad_widget z = {0};
    z.to[0] = z.from[0];
    z.to[1] = z.from[1];
    CHECK(!pc_ui_grad_decor_circle(&z, center, &radius));
    CHECK(!pc_ui_grad_decor_diamond(&z, corners));
    CHECK(!pc_ui_grad_decor_reflected(&z, mirror));
}

static void test_hostile_input(void)
{
    /* NULL and NaN must never crash and must report a miss. */
    pc_ui_grad_grab g = pc_ui_grad_widget_hit(NULL, 0, 0, 6);
    CHECK(g.kind == PC_UI_GRAD_GRAB_NONE);
    pc_ui_grad_widget w = horizontal();
    g = pc_ui_grad_widget_hit(&w, NAN, 0, 6);
    CHECK(g.kind == PC_UI_GRAD_GRAB_NONE);
    g = pc_ui_grad_widget_hit(&w, 0, 0, -5);
    CHECK(g.kind == PC_UI_GRAD_GRAB_NONE);
    double out[2] = {123, 456};
    pc_ui_grad_widget_at(NULL, 0.5, out);
    CHECK_NEAR(out[0], 0, 1e-9);
    pc_ui_grad_snap45(NULL, w.to, true, out); /* NULL anchor copies through */
    CHECK_NEAR(out[0], 110, 1e-9);
    /* Huge coordinates stay finite and bounded. */
    w.to[0] = 1e12;
    double u = 0, perp = 0;
    pc_ui_grad_widget_project(&w, 1e6, 1e6, &u, &perp);
    CHECK(u >= 0 && u <= 1 && isfinite(perp));
    /* Over-large counts are capped, not over-read. */
    w = horizontal();
    w.nstops = (size_t)-1;
    g = pc_ui_grad_widget_hit(&w, 60, 20, 6);
    CHECK(g.kind == PC_UI_GRAD_GRAB_STOP);
}

int main(int argc, char **argv)
{
    RUN(test_at_and_project);
    RUN(test_hit_order);
    RUN(test_hit_line_t_reversed);
    RUN(test_snap45);
    RUN(test_decorations);
    RUN(test_hostile_input);
    return pc_t_finish();
}
