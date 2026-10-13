/* Vanishing Point (port of the tests in crates/algo/src/vanishing.rs) plus never-crash checks. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "harness.h"

static void project(double f, const double c[2], const double p[3], double out[2])
{
    out[0] = c[0] + f * p[0] / p[2];
    out[1] = c[1] + f * p[1] / p[2];
}

/* A 2 x 1 rectangle on a tilted plane, rotated about the vertical axis too, seen with focal f. */
static pc_algo_vp_plane floor_quad(double f, const double c[2])
{
    static const double pts[4][3] = {{-1.0, -0.5, 0.0}, {1.0, -0.5, 0.0}, {1.0, 0.5, 0.0}, {-1.0, 0.5, 0.0}};
    const double rx = 0.6, ry = 0.4;
    pc_algo_vp_plane pl;
    int k;
    for (k = 0; k < 4; k++) {
        const double *p = pts[k];
        double s = sin(rx), co = cos(rx), q[3] = {p[0], co * p[1] - s * p[2], s * p[1] + co * p[2]};
        double s2 = sin(ry), c2 = cos(ry), r[3] = {c2 * q[0] + s2 * q[2], q[1], -s2 * q[0] + c2 * q[2]};
        r[2] += 4.0;
        project(f, c, r, pl.corners[k]);
    }
    return pl;
}

static void test_focal_and_metric_aspect_from_one_plane(void)
{
    const double c[2] = {400.0, 300.0};
    pc_algo_vp_plane pl = floor_quad(500.0, c);
    pc_algo_vp_lifted l;
    double f = 0.0, lu, lv;
    CHECK(pc_algo_vp_plane_focal(&pl, c, &f));
    CHECK(fabs(f - 500.0) < 1.0);
    pc_algo_vp_plane_lift(&pl, f, c, &l);
    pc_algo_vp_lifted_lengths(&l, &lu, &lv);
    CHECK(fabs(lu / lv - 2.0) < 0.01);
}

static void cross(const double *a, const double *b, double *o)
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

static void normal_of(const pc_algo_vp_lifted *l, double n[3])
{
    double e1[3], e2[3], m;
    int i;
    for (i = 0; i < 3; i++) {
        e1[i] = l->pts[1][i] - l->pts[0][i];
        e2[i] = l->pts[3][i] - l->pts[0][i];
    }
    cross(e1, e2, n);
    m = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (i = 0; i < 3; i++) {
        n[i] /= m;
    }
}

static void test_tear_off_is_perpendicular_and_shares_the_edge(void)
{
    const double c[2] = {400.0, 300.0};
    pc_algo_vp_plane first = floor_quad(500.0, c);
    pc_algo_vp_scene sc;
    pc_algo_vp_lifted a, b;
    double n1[3], n2[3];
    size_t k = 0;
    CHECK(pc_algo_vp_scene_init(&sc, &first, c, false, 0.0, 800.0, NULL) == PC_OK);
    CHECK(pc_algo_vp_scene_tear_off(&sc, 0, PC_ALGO_VP_TOP, 90.0, 1.0, &k, NULL) == PC_OK);
    CHECK(fabs(sc.planes[k].corners[3][0] - sc.planes[0].corners[0][0]) < 1e-6 &&
          fabs(sc.planes[k].corners[2][0] - sc.planes[0].corners[1][0]) < 1e-6);
    pc_algo_vp_scene_lifted(&sc, 0, &a);
    pc_algo_vp_scene_lifted(&sc, k, &b);
    normal_of(&a, n1);
    normal_of(&b, n2);
    CHECK(fabs(n1[0] * n2[0] + n1[1] * n2[1] + n1[2] * n2[2]) < 0.02);
    pc_algo_vp_scene_fini(&sc);
}

static void test_paste_and_clone_in_perspective(void)
{
    const double c[2] = {400.0, 300.0};
    pc_algo_vp_plane first = floor_quad(500.0, c);
    pc_algo_vp_scene sc;
    pc_surface img, out, s;
    pc_algo_homography h;
    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f}, grey[4] = {0.2f, 0.2f, 0.2f, 1.0f};
    const double at[2] = {0.25, 0.25};
    double x, y, sx, sy, dx, dy, src[2], dab[1][2];
    float px[4];
    size_t n = 0;
    CHECK(pc_algo_vp_scene_init(&sc, &first, c, false, 0.0, 800.0, NULL) == PC_OK);
    pc_surface_init(&img, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_surface_fill_rect(&img, pc_rect_make(0, 0, 100, 50), red, 4, NULL) == PC_OK);
    CHECK(pc_algo_vp_paste(&sc, 0, &img, pc_rect_make(0, 0, 100, 50), at, 0.5, &out, NULL) == PC_OK);
    /* The pasted rectangle lands inside the plane, around plane (0.5, 0.5). */
    CHECK(pc_algo_vp_plane_homography(&sc.planes[0], &h));
    pc_algo_homography_apply(&h, 0.5, 0.5, &x, &y);
    pc_surface_rgba(&out, (int32_t)x, (int32_t)y, px);
    CHECK(px[3] > 0.9f);
    pc_algo_homography_apply(&h, 0.1, 0.1, &x, &y);
    pc_surface_rgba(&out, (int32_t)x, (int32_t)y, px);
    CHECK(px[3] < 0.1f);
    /* Clone: paint a red dot near the far edge from a red source near the near edge. */
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(0, 0, 800, 600), grey, 4, NULL) == PC_OK);
    pc_algo_homography_apply(&h, 0.5, 0.8, &sx, &sy);
    CHECK(pc_surface_fill_rect(
              &s, pc_rect_make((int32_t)sx - 6, (int32_t)sy - 6, (int32_t)sx + 6, (int32_t)sy + 6), red, 4,
              NULL) == PC_OK);
    pc_algo_homography_apply(&h, 0.5, 0.3, &dx, &dy);
    src[0] = sx, src[1] = sy, dab[0][0] = dx, dab[0][1] = dy;
    CHECK(pc_algo_vp_clone_stroke(&sc, &s, src, (const double (*)[2])dab, 1, 10.0, 0.8, 1.0f, &n, NULL) ==
          PC_OK);
    CHECK_EQ_UINT(n, 1);
    pc_surface_rgba(&s, (int32_t)dx, (int32_t)dy, px);
    CHECK(px[0] > 0.8f);
    pc_surface_fini(&img), pc_surface_fini(&out), pc_surface_fini(&s);
    pc_algo_vp_scene_fini(&sc);
}

static void test_hostile_scenes_never_crash(void)
{
    const double c[2] = {50.0, 50.0}, at[2] = {NAN, INFINITY};
    pc_algo_vp_plane deg = {{{0, 0}, {0, 0}, {0, 0}, {0, 0}}},
                     nanp = {{{NAN, 0}, {10, 0}, {10, 10}, {0, 10}}};
    pc_algo_vp_scene sc;
    pc_surface img, out;
    double pts[3][2] = {{5, 5}, {NAN, 1}, {1e300, -1e300}}, src[2] = {5, 5}, f;
    size_t k = 0, n = 0;
    int e;
    CHECK(!pc_algo_vp_plane_focal(&deg, c, &f));
    pc_surface_init(&img, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_surface_fill_rect(&img, pc_rect_make(0, 0, 10, 10), (const float[4]){1, 1, 1, 1}, 4, NULL) ==
          PC_OK);
    CHECK(pc_algo_vp_scene_init(&sc, &deg, c, true, NAN, 800.0, NULL) == PC_OK);
    CHECK(sc.focal == 800.0);
    if (pc_algo_vp_paste(&sc, 0, &img, pc_rect_make(0, 0, 10, 10), at, 0.5, &out, NULL) == PC_OK) {
        pc_surface_fini(&out);
    }
    CHECK(pc_algo_vp_paste(&sc, 0, &img, PC_RECT_EMPTY, at, 0.5, &out, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_vp_paste(&sc, 7, &img, pc_rect_make(0, 0, 10, 10), at, 0.5, &out, NULL) == PC_ERR_INVALID);
    for (e = 0; e < 4; e++) {
        (void)pc_algo_vp_scene_tear_off(&sc, 0, (pc_algo_vp_edge)e, NAN, -1.0, &k, NULL);
    }
    CHECK(pc_algo_vp_scene_tear_off(&sc, 99, PC_ALGO_VP_TOP, 90.0, 1.0, &k, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_vp_clone_stroke(&sc, &img, src, (const double (*)[2])pts, 3, 4.0, NAN, 1.0f, &n, NULL) ==
          PC_OK);
    pc_algo_vp_scene_fini(&sc);
    CHECK(pc_algo_vp_scene_init(&sc, &nanp, c, false, 0.0, 300.0, NULL) == PC_OK);
    if (pc_algo_vp_paste(&sc, 0, &img, pc_rect_make(0, 0, 10, 10), at, 0.5, &out, NULL) == PC_OK) {
        pc_surface_fini(&out);
    }
    CHECK(pc_algo_vp_clone_stroke(&sc, &img, src, (const double (*)[2])pts, 3, 1e300, 0.5, 1.0f, &n, NULL) ==
          PC_OK);
    CHECK(pc_algo_vp_clone_stroke(&sc, &img, src, NULL, 0, 4.0, 0.5, 1.0f, &n, NULL) == PC_OK && n == 0);
    pc_algo_vp_scene_fini(&sc);
    pc_surface_fini(&img);
}

static void test_plane_check_and_corner_order(void)
{
    const double c[2] = {400.0, 300.0};
    pc_algo_vp_plane fq = floor_quad(500.0, c), sq = {{{10, 10}, {110, 10}, {110, 110}, {10, 110}}};
    pc_algo_vp_plane bowtie = {{{10, 10}, {110, 110}, {110, 10}, {10, 110}}};
    pc_algo_vp_plane dent = {{{10, 10}, {110, 10}, {40, 40}, {10, 110}}};
    pc_algo_vp_plane sliver = {{{10, 10}, {300, 10}, {300, 40}, {200, 40}}};
    pc_algo_vp_plane nan = {{{NAN, 10}, {110, 10}, {110, 110}, {10, 110}}};
    pc_algo_vp_plane ccw = {{{110, 110}, {110, 10}, {10, 10}, {10, 110}}}, n;
    int k;
    CHECK(pc_algo_vp_plane_check_at(&fq, c) == PC_ALGO_VP_VALID);
    CHECK(pc_algo_vp_plane_check_at(&sq, c) == PC_ALGO_VP_VALID); /* fronto-parallel */
    CHECK(pc_algo_vp_plane_check_at(&bowtie, c) == PC_ALGO_VP_INVALID);
    CHECK(pc_algo_vp_plane_check_at(&dent, c) == PC_ALGO_VP_INVALID); /* concave */
    CHECK(pc_algo_vp_plane_check_at(&sliver, c) == PC_ALGO_VP_QUESTIONABLE);
    CHECK(pc_algo_vp_plane_check_at(&nan, c) == PC_ALGO_VP_INVALID);
    CHECK(strcmp(pc_algo_vp_plane_check_name(PC_ALGO_VP_QUESTIONABLE), "questionable") == 0);
    /* Clicked counter-clockwise from the bottom-right: clockwise from the top-left. */
    n = pc_algo_vp_plane_normalized(&ccw);
    for (k = 0; k < 4; k++) {
        CHECK(n.corners[k][0] == sq.corners[k][0] && n.corners[k][1] == sq.corners[k][1]);
    }
}

int main(int argc, char **argv)
{
    RUN(test_plane_check_and_corner_order);
    RUN(test_focal_and_metric_aspect_from_one_plane);
    RUN(test_tear_off_is_perpendicular_and_shares_the_edge);
    RUN(test_paste_and_clone_in_perspective);
    RUN(test_hostile_scenes_never_crash);
    return pc_t_finish();
}
