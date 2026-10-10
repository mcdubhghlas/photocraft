/* Perspective Warp (port of the tests in crates/algo/src/perspective.rs) plus never-crash
 * checks. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "harness.h"

static bool close2(double ax, double ay, double bx, double by, double tol)
{
    return fabs(ax - bx) <= tol && fabs(ay - by) <= tol;
}

static void test_identity_and_a_known_homography(void)
{
    const double q[4][2] = {{10, 10}, {90, 12}, {85, 70}, {12, 75}};
    const double pts[4][2] = {{20, 20}, {50, 40}, {0, 0}, {120, 90}};
    const double src[4][2] = {{0, 0}, {100, 0}, {100, 100}, {0, 100}};
    const double dst[4][2] = {{20, 0}, {80, 0}, {100, 100}, {0, 100}};
    const double r[4] = {0, 0, 100, 100};
    pc_algo_persp_plane pl = pc_algo_persp_plane_identity(q);
    pc_algo_persp_map *m = NULL;
    pc_algo_homography h;
    double x, y, hx, hy;
    int k;
    CHECK(pc_algo_persp_map_new(&pl, 1, &m, NULL) == PC_OK);
    for (k = 0; k < 4; k++) {
        pc_algo_persp_map_apply(m, pts[k][0], pts[k][1], &x, &y);
        CHECK(close2(x, y, pts[k][0], pts[k][1], 1e-9));
    }
    pc_algo_persp_map_free(m);
    memcpy(pl.src, src, sizeof src);
    memcpy(pl.dst, dst, sizeof dst);
    CHECK(pc_algo_persp_map_new(&pl, 1, &m, NULL) == PC_OK);
    for (k = 0; k < 4; k++) {
        pc_algo_persp_map_apply(m, src[k][0], src[k][1], &x, &y);
        CHECK(close2(x, y, dst[k][0], dst[k][1], 1e-9));
    }
    CHECK(pc_algo_homography_rect_to_quad(r, dst, &h));
    pc_algo_homography_apply(&h, 50.0, 50.0, &hx, &hy);
    pc_algo_persp_map_apply(m, 50.0, 50.0, &x, &y);
    CHECK(close2(x, y, hx, hy, 1e-9));
    /* The diagonals' crossing stays the crossing. */
    CHECK(fabs(x - 50.0) < 1e-9 && y < 50.0);
    pc_algo_persp_map_free(m);
}

static void test_shared_edges_stay_continuous(void)
{
    pc_algo_persp_plane p[2] = {
        {{{0, 0}, {50, 0}, {50, 100}, {0, 100}}, {{0, 10}, {50, 0}, {55, 100}, {0, 90}}},
        {{{50.2, 0}, {100, 0}, {100, 100}, {50, 100.2}}, {{50, 0}, {110, 20}, {100, 80}, {55, 100}}},
    };
    const double ts[5] = {0.05, 0.25, 0.5, 0.77, 0.95};
    const double zero[4][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
    pc_algo_persp_plane one = pc_algo_persp_plane_identity(zero);
    pc_algo_persp_map *m = NULL;
    size_t groups[4], ng = 0;
    int k;
    CHECK(pc_algo_persp_map_new(p, 2, &m, NULL) == PC_OK);
    /* After linking, the shared edge runs from (50.1, 0) to (50, 100.1). */
    for (k = 0; k < 5; k++) {
        double t = ts[k], x = 50.1 - 0.1 * t, y = 100.1 * t, lx, ly, rx, ry;
        pc_algo_persp_map_apply(m, x - 1e-6, y, &lx, &ly);
        pc_algo_persp_map_apply(m, x + 1e-6, y, &rx, &ry);
        CHECK(close2(lx, ly, rx, ry, 1e-4));
    }
    pc_algo_persp_map_free(m);
    pc_algo_persp_linked_corners(&one, 1, groups, &ng);
    CHECK_EQ_UINT(ng, 1);
}

static void test_straighten_levels_and_plumbs_edges(void)
{
    pc_algo_persp_plane p = {{{0, 0}, {100, 0}, {100, 100}, {0, 100}},
                             {{0, 3}, {100, -2}, {96, 100}, {4, 104}}};
    pc_algo_persp_straighten mode;
    CHECK(pc_algo_persp_straighten_planes(&p, 1, PC_ALGO_PERSP_VERTICAL, NULL) == PC_OK);
    CHECK(p.dst[1][0] == p.dst[2][0]);
    CHECK(p.dst[0][0] == p.dst[3][0]);
    CHECK(p.dst[0][1] != p.dst[1][1]);
    CHECK(pc_algo_persp_straighten_planes(&p, 1, PC_ALGO_PERSP_AUTO, NULL) == PC_OK);
    CHECK(p.dst[0][1] == p.dst[1][1]);
    CHECK(p.dst[2][1] == p.dst[3][1]);
    CHECK(pc_algo_persp_straighten_parse("Level", 5, &mode) && mode == PC_ALGO_PERSP_HORIZONTAL);
    CHECK(pc_algo_persp_straighten_parse("BOTH", 4, &mode) && mode == PC_ALGO_PERSP_AUTO);
    CHECK(!pc_algo_persp_straighten_parse("diag", 4, &mode));
}

static void test_degenerate_and_hostile_planes(void)
{
    const double flat[4][2] = {{0, 0}, {10, 0}, {20, 0}, {30, 0}};
    pc_algo_persp_plane p = pc_algo_persp_plane_identity(flat), q;
    pc_algo_persp_map *m = (pc_algo_persp_map *)1;
    double out[2], x, y;
    int k;
    CHECK(pc_algo_persp_map_new(&p, 1, &m, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_persp_map_new(NULL, 0, &m, NULL) == PC_ERR_INVALID);
    CHECK(m == (pc_algo_persp_map *)1);
    pc_algo_persp_plane_point(&p, 0.5, 0.5, true, out);
    CHECK(out[0] == 0.0 && out[1] == 0.0);
    /* NaN / infinite corners never crash. */
    for (k = 0; k < 3; k++) {
        double v = k == 0 ? NAN : (k == 1 ? INFINITY : 1e308);
        q = (pc_algo_persp_plane){{{0, 0}, {100, 0}, {100, 100}, {0, 100}},
                                  {{v, 0}, {100, v}, {100, 100}, {0, 100}}};
        if (pc_algo_persp_map_new(&q, 1, &m, NULL) == PC_OK) {
            pc_algo_persp_map_apply(m, 50.0, 50.0, &x, &y);
            pc_algo_persp_map_apply(m, -1e300, NAN, &x, &y);
            pc_algo_persp_map_free(m);
        }
        CHECK(pc_algo_persp_straighten_planes(&q, 1, PC_ALGO_PERSP_AUTO, NULL) == PC_OK);
    }
    CHECK(pc_algo_persp_is_identity(&p, 1));
    CHECK(pc_algo_persp_straighten_planes(NULL, 0, PC_ALGO_PERSP_AUTO, NULL) == PC_OK);
}

int main(int argc, char **argv)
{
    RUN(test_identity_and_a_known_homography);
    RUN(test_shared_edges_stay_continuous);
    RUN(test_straighten_levels_and_plumbs_edges);
    RUN(test_degenerate_and_hostile_planes);
    return pc_t_finish();
}
