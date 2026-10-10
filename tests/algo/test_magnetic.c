/* Tests for src/algo/magnetic.c (ports of crates/algo/src/magnetic/tests.rs). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "harness.h"

/* A procedural image as a fetch that counts the pixels it reads. */
typedef struct pimg {
    int kind;
    size_t read;
} pimg;

static uint8_t value_at(int kind, int32_t x, int32_t y)
{
    static const uint8_t lv3[3] = {0, 40, 255}, st3[3] = {60, 100, 255};
    switch (kind) {
    case 0:
        return x < 20 ? 0 : 255;
    case 1:
        return x < 20 ? 100 : 113;
    case 2:
        return lv3[(x >= 12) + (x >= 18)];
    case 3:
        return hypot((double)x + 0.5 - 50.0, (double)y + 0.5 - 50.0) < 30.0 ? 255 : 0;
    case 4:
        return 128;
    case 5:
        return st3[(y >= 20) + (y >= 40)];
    case 6:
        return y < 30 ? 20 : 200;
    case 7:
        return x < 3000 ? 0 : 255;
    default:
        return (uint8_t)((x ^ y) & 255);
    }
}

static bool fetch(void *ctx, pc_rect r, uint8_t (*out)[4], size_t n)
{
    pimg *p = ctx;
    size_t i = 0;
    int32_t x, y;
    p->read += (size_t)pc_rect_width(r) * pc_rect_height(r);
    for (y = r.y0; y < r.y1; y++) {
        for (x = r.x0; x < r.x1 && i < n; x++, i++) {
            out[i][0] = out[i][1] = out[i][2] = value_at(p->kind, x, y);
            out[i][3] = 255;
        }
    }
    return true;
}

static bool fetch_bad(void *ctx, pc_rect r, uint8_t (*out)[4], size_t n)
{
    (void)ctx;
    (void)r;
    (void)out;
    (void)n;
    return false;
}

static double dist2_seg(const double p[2], const double a[2], const double b[2])
{
    double vx = b[0] - a[0], vy = b[1] - a[1], l2 = vx * vx + vy * vy, t = 0.0, dx, dy;
    if (l2 > 0.0) {
        t = ((p[0] - a[0]) * vx + (p[1] - a[1]) * vy) / l2;
        t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    }
    dx = p[0] - (a[0] + t * vx);
    dy = p[1] - (a[1] + t * vy);
    return dx * dx + dy * dy;
}

static double off_polyline(const double p[2], const double (*g)[2], size_t n)
{
    double best = INFINITY;
    size_t i;
    for (i = 0; i + 1 < n; i++) {
        double d = sqrt(dist2_seg(p, g[i], g[i + 1]));
        best = d < best ? d : best;
    }
    return best;
}

static pc_algo_magnetic_tracer *tracer(int32_t w, int32_t h)
{
    pc_algo_magnetic_tracer *t = NULL;
    (void)pc_algo_magnetic_tracer_new(pc_rect_make(0, 0, w, h), &t, NULL);
    return t;
}

static bool snap(pc_algo_magnetic_tracer *t, pimg *p, double x, double y, double w, float c, double out[2])
{
    double q[2] = {x, y};
    bool some = false;
    (void)pc_algo_magnetic_snap(t, fetch, p, q, pc_algo_magnetic_settings_new(w, c), out, &some, NULL);
    return some;
}

static size_t trace(pc_algo_magnetic_tracer *t, pc_algo_magnetic_fetch f, pimg *p, const double a[2],
                    const double b[2], const double (*g)[2], size_t ng, pc_algo_magnetic_settings s,
                    double (**out)[2])
{
    size_t n = 0;
    *out = NULL;
    CHECK(pc_algo_magnetic_trace(t, f, p, a, b, g, ng, s, out, &n, NULL) == PC_OK);
    return n;
}

static void test_snaps_to_the_edge_centre_within_the_width(void)
{
    pimg im = {0, 0};
    pc_algo_magnetic_tracer *t = tracer(64, 32);
    double p[2];
    CHECK(snap(t, &im, 14.3, 10.5, 10.0, 0.1f, p));
    CHECK(fabs(p[0] - 20.0) < 0.01 && fabs(p[1] - 10.5) < 0.01);
    CHECK(snap(t, &im, 27.0, 3.2, 10.0, 0.1f, p));
    CHECK(fabs(p[0] - 20.0) < 0.01);
    CHECK(snap(t, &im, 14.3, 10.5, 3.0, 0.1f, p));
    CHECK(p[0] == 14.3 && p[1] == 10.5);
    pc_algo_magnetic_tracer_free(t);
}

static void test_contrast_sets_which_steps_are_edges(void)
{
    pimg im = {1, 0};
    pc_algo_magnetic_tracer *t = tracer(64, 32);
    double p[2];
    CHECK(snap(t, &im, 14.3, 10.5, 10.0, 0.10f, p) && p[0] == 14.3 && p[1] == 10.5);
    CHECK(snap(t, &im, 14.3, 10.5, 10.0, 0.03f, p) && fabs(p[0] - 20.0) < 0.01);
    pc_algo_magnetic_tracer_free(t);
}

static void test_snaps_to_the_most_prominent_edge(void)
{
    pimg im = {2, 0};
    pc_algo_magnetic_tracer *t = tracer(40, 20);
    double p[2];
    CHECK(snap(t, &im, 13.0, 10.0, 10.0, 0.1f, p) && fabs(p[0] - 18.0) < 0.01);
    pc_algo_magnetic_tracer_free(t);
}

static void test_trace_follows_a_curved_edge(void)
{
    pimg im = {3, 0};
    pc_algo_magnetic_tracer *t = tracer(100, 100);
    pc_algo_magnetic_settings s = pc_algo_magnetic_settings_default();
    double a[2], b[2], (*path)[2];
    size_t n, i;
    bool ok = true;
    (void)snap(t, &im, 50.0, 21.0, s.width, s.contrast, a);
    (void)snap(t, &im, 79.0, 50.0, s.width, s.contrast, b);
    n = trace(t, fetch, &im, a, b, NULL, 0, s, &path);
    CHECK(n > 4);
    CHECK(n > 0 && path[0][0] == a[0] && path[0][1] == a[1] && path[n - 1][0] == b[0] &&
          path[n - 1][1] == b[1]);
    for (i = 0; i < n; i++) {
        ok &= fabs(hypot(path[i][0] - 50.0, path[i][1] - 50.0) - 30.0) < 1.0;
    }
    CHECK(ok);
    free(path);
    pc_algo_magnetic_tracer_free(t);
}

static void test_without_edges_the_border_follows_the_pointer(void)
{
    pimg im = {4, 0};
    pc_algo_magnetic_tracer *t = tracer(120, 80);
    pc_algo_magnetic_settings s = pc_algo_magnetic_settings_default();
    double a[2] = {10, 40}, b[2] = {110, 40}, guide[9][2], g[11][2], (*path)[2], probe[2] = {60, 25};
    double c0[2] = {10, 10}, c1[2] = {100, 75}, c2[2] = {110, 73};
    const double sharp[1][2] = {{30, 70}}, sg[3][2] = {{10, 10}, {30, 70}, {100, 75}};
    size_t n, i;
    bool ok = true;
    for (i = 0; i < 9; i++) {
        guide[i][0] = 10.0 + (double)(i + 1) * 10.0;
        guide[i][1] = 40.0 - 15.0 * sin((double)(i + 1) * 3.14159265358979323846 / 10.0);
        g[i + 1][0] = guide[i][0];
        g[i + 1][1] = guide[i][1];
    }
    g[0][0] = a[0];
    g[0][1] = a[1];
    g[10][0] = b[0];
    g[10][1] = b[1];
    n = trace(t, fetch, &im, a, b, (const double (*)[2])guide, 9, s, &path);
    for (i = 0; i < n; i++) {
        ok &= off_polyline(path[i], (const double (*)[2])g, 11) < 2.0;
    }
    CHECK(ok);
    CHECK(off_polyline(probe, (const double (*)[2])path, n) < 2.0);
    free(path);
    n = trace(t, fetch, &im, c0, c1, sharp, 1, s, &path);
    ok = true;
    for (i = 0; i < n; i++) {
        ok &= off_polyline(path[i], sg, 3) <= s.width;
    }
    CHECK(ok);
    free(path);
    n = trace(t, fetch, &im, c0, c2, NULL, 0, s, &path);
    CHECK(n == 2 && path[0][0] == 10 && path[0][1] == 10 && path[1][0] == 110 && path[1][1] == 73);
    free(path);
    pc_algo_magnetic_tracer_free(t);
}

static void test_the_corridor_keeps_the_border_off_edges_the_pointer_never_went_near(void)
{
    pimg im = {5, 0};
    pc_algo_magnetic_tracer *t = tracer(120, 60);
    double a[2] = {10, 22}, b[2] = {110, 18}, (*path)[2];
    const double g[1][2] = {{60, 23}};
    size_t n, i;
    bool ok = true, ok2 = true;
    n = trace(t, fetch, &im, a, b, g, 1, pc_algo_magnetic_settings_new(10.0, 0.1f), &path);
    for (i = 0; i < n; i++) {
        ok &= fabs(path[i][1] - 20.0) <= 2.0 + 1e-9;
        if (path[i][0] > 25.0 && path[i][0] < 95.0) {
            ok2 &= fabs(path[i][1] - 20.0) < 0.01;
        }
    }
    CHECK(ok);
    CHECK(ok2);
    free(path);
    pc_algo_magnetic_tracer_free(t);
}

static void test_long_guides_are_traced_in_pieces_along_the_edge(void)
{
    pimg im = {6, 0};
    pc_algo_magnetic_tracer *t = tracer(1000, 60);
    double guide[19][2], a[2] = {5, 30}, b[2] = {995, 30}, (*path)[2];
    size_t n, i;
    bool ok = true;
    for (i = 0; i < 19; i++) {
        guide[i][0] = (double)(i + 1) * 50.0;
        guide[i][1] = (i + 1) % 2 == 0 ? 26.0 : 34.0;
    }
    n = trace(t, fetch, &im, a, b, (const double (*)[2])guide, 19, pc_algo_magnetic_settings_default(),
              &path);
    for (i = 0; i < n; i++) {
        ok &= fabs(path[i][1] - 30.0) < 0.01;
    }
    CHECK(ok);
    CHECK_NEAR(pc_algo_magnetic_length((const double (*)[2])path, n), 990.0, 1.0);
    CHECK(n < 20);
    free(path);
    pc_algo_magnetic_tracer_free(t);
}

static void test_reads_only_the_pixels_around_the_border(void)
{
    pimg im = {7, 0};
    pc_algo_magnetic_tracer *t = tracer(6000, 4000);
    pc_algo_magnetic_settings s = pc_algo_magnetic_settings_default();
    double a[2], b[2], (*path)[2];
    size_t n, i, first;
    bool ok = true;
    (void)snap(t, &im, 2996.0, 2000.0, s.width, s.contrast, a);
    (void)snap(t, &im, 3004.0, 2100.0, s.width, s.contrast, b);
    n = trace(t, fetch, &im, a, b, NULL, 0, s, &path);
    for (i = 0; i < n; i++) {
        ok &= fabs(path[i][0] - 3000.0) < 0.01;
    }
    CHECK(ok);
    free(path);
    first = im.read;
    CHECK(first <= 6 * 128 * 128);
    CHECK_EQ_UINT(pc_algo_magnetic_cached_pixels(t), first);
    n = trace(t, fetch, &im, a, b, NULL, 0, s, &path);
    free(path);
    CHECK_EQ_UINT(im.read, first);
    pc_algo_magnetic_tracer_free(t);
}

static void test_hostile_input_never_panics(void)
{
    pimg im = {8, 0};
    pc_algo_magnetic_settings s = pc_algo_magnetic_settings_default(),
                              bad[3] = {{NAN, NAN}, {1e300, -4.0f}, {-3.0, 1e30f}};
    pc_algo_magnetic_tracer *e = NULL, *t;
    double p[2], q[2] = {1, 1}, r[2] = {5, 5}, (*path)[2], nan1[2] = {NAN, 1.0}, inf1[2] = {INFINITY, 1.0};
    double fa[2] = {-1e300, -1e9}, fb[2] = {1e9, 1e300}, g[2][2] = {{NAN, 3.0}, {1e12, -5.0}};
    double c20[2] = {20, 20}, c2[2] = {2, 2}, c45[2] = {45, 30}, c150[2] = {150, 150}, c10[2] = {10, 10};
    double c290[2] = {290, 280}, c7[2] = {7, 7};
    const double g35[1][2] = {{10, 35}};
    bool some = true, ok = true;
    size_t n, i, k;
    (void)pc_algo_magnetic_tracer_new(PC_RECT_EMPTY, &e, NULL);
    CHECK(pc_algo_magnetic_snap(e, fetch, &im, q, s, p, &some, NULL) == PC_OK && !some);
    CHECK(trace(e, fetch, &im, q, r, NULL, 0, s, &path) == 0);
    free(path);
    pc_algo_magnetic_tracer_free(e);
    t = tracer(50, 40);
    CHECK(pc_algo_magnetic_snap(t, fetch, &im, nan1, s, p, &some, NULL) == PC_OK && !some);
    CHECK(trace(t, fetch, &im, inf1, r, NULL, 0, s, &path) == 0);
    free(path);
    n = trace(t, fetch, &im, fa, fb, (const double (*)[2])g, 2, s, &path);
    for (i = 0; i < n; i++) {
        ok &= path[i][0] >= 0.0 && path[i][0] <= 50.0 && path[i][1] >= 0.0 && path[i][1] <= 40.0;
    }
    CHECK(ok);
    free(path);
    for (k = 0; k < 3; k++) {
        CHECK(pc_algo_magnetic_snap(t, fetch, &im, c20, bad[k], p, &some, NULL) == PC_OK && some);
        CHECK(trace(t, fetch, &im, c2, c45, g35, 1, bad[k], &path) > 0);
        free(path);
    }
    pc_algo_magnetic_tracer_free(t);
    t = tracer(300, 300);
    CHECK(pc_algo_magnetic_snap(t, fetch_bad, NULL, c150, s, p, &some, NULL) == PC_OK && some &&
          p[0] == 150 && p[1] == 150);
    n = trace(t, fetch_bad, NULL, c10, c290, NULL, 0, s, &path);
    CHECK(n >= 2 && path[0][0] == 10 && path[0][1] == 10 && path[n - 1][0] == 290 && path[n - 1][1] == 280);
    free(path);
    n = trace(t, fetch_bad, NULL, c7, c7, NULL, 0, s, &path);
    CHECK(n == 2 && path[0][0] == 7 && path[1][1] == 7);
    free(path);
    pc_algo_magnetic_tracer_free(t);
}

static void test_split_and_simplify(void)
{
    const double g[3][2] = {{0, 0}, {600, 0}, {600, 100}}, sp[4][2] = {{0, 0}, {1, 0.1}, {2, 0}, {3, 5}};
    double (*pts)[2] = NULL, (*o)[2] = NULL, total = 0.0;
    size_t *starts = NULL, np = 0, n = 0, k;
    CHECK(pc_algo_magnetic_split(g, 3, 256.0, &pts, &starts, &np, NULL) == PC_OK);
    CHECK_EQ_UINT(np, 3);
    CHECK(starts[1] - starts[0] == 2 && pts[0][0] == 0 && pts[1][0] == 256 && pts[1][1] == 0);
    CHECK(pts[starts[3] - 1][0] == 600 && pts[starts[3] - 1][1] == 100);
    for (k = 0; k < np; k++) {
        total += pc_algo_magnetic_length((const double (*)[2])(pts + starts[k]), starts[k + 1] - starts[k]);
    }
    CHECK_NEAR(total, 700.0, 1e-9);
    free(pts);
    free(starts);
    CHECK(pc_algo_magnetic_simplify(sp, 4, 0.3, &o, &n, NULL) == PC_OK);
    CHECK(n == 3 && o[0][0] == 0 && o[1][0] == 2 && o[2][0] == 3 && o[2][1] == 5);
    free(o);
    CHECK(pc_algo_magnetic_split(NULL, 0, 256.0, &pts, &starts, &np, NULL) == PC_OK && np == 0);
}

int main(int argc, char **argv)
{
    RUN(test_snaps_to_the_edge_centre_within_the_width);
    RUN(test_contrast_sets_which_steps_are_edges);
    RUN(test_snaps_to_the_most_prominent_edge);
    RUN(test_trace_follows_a_curved_edge);
    RUN(test_without_edges_the_border_follows_the_pointer);
    RUN(test_the_corridor_keeps_the_border_off_edges_the_pointer_never_went_near);
    RUN(test_long_guides_are_traced_in_pieces_along_the_edge);
    RUN(test_reads_only_the_pixels_around_the_border);
    RUN(test_hostile_input_never_panics);
    RUN(test_split_and_simplify);
    return pc_t_finish();
}
