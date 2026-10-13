/* The inline #[test]s of crates/vector/src/map.rs (non-affine maps: Distort, Perspective, Warp),
 * plus hostile input for the C port. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "vh.h"

static pc_path square(void)
{
    static const double pts[4][2] = {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}};
    return vh_poly_path(pts, 4, true);
}

static pc_point skew_up(pc_point q, void *ctx)
{
    (void)ctx;
    return pc_point_make(q.x * 2.0, q.y + q.x);
}

static pc_point shear(pc_point q, void *ctx)
{
    (void)ctx;
    return pc_point_make(q.x + q.y * 0.5, q.y);
}

static pc_point bend(pc_point q, void *ctx)
{
    (void)ctx;
    return pc_point_make(q.x, q.y - (q.x - 5.0) * (q.x - 5.0) * 0.2);
}

/* De Casteljau, as map.rs evaluates segments. */
static pc_point lerp(pc_point a, pc_point b, double t)
{
    return pc_point_make(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
}
static pc_point eval(const pc_point s[4], double t)
{
    pc_point ab = lerp(s[0], s[1], t);
    pc_point bc = lerp(s[1], s[2], t);
    pc_point cd = lerp(s[2], s[3], t);
    return lerp(lerp(ab, bc, t), lerp(bc, cd, t), t);
}

static bool pt_eq(pc_point a, pc_point b)
{
    return a.x == b.x && a.y == b.y;
}

static void test_map_points_moves_every_point_and_keeps_the_knots(void)
{
    pc_path sq = square();
    pc_path p;
    CHECK(pc_vec_map_points(&sq, skew_up, NULL, &p, NULL) == PC_OK);
    CHECK_EQ_UINT(p.subpaths[0].nknots, 4);
    CHECK(pt_eq(p.subpaths[0].knots[2].anchor, pc_point_make(20.0, 20.0)));
    CHECK(pt_eq(p.subpaths[0].knots[2].in_ctrl, p.subpaths[0].knots[2].anchor));
    pc_path_fini(&p);
    pc_path_fini(&sq);
}

static void test_map_path_keeps_lines_that_stay_straight_and_splits_bent_ones(void)
{
    pc_path sq = square();
    pc_path p;
    const pc_knot *k;
    size_t n;
    size_t i;
    size_t right = SIZE_MAX;
    /* Linear map: no splits, straight segments stay straight. */
    CHECK(pc_vec_map_path(&sq, shear, NULL, 0.1, &p, NULL) == PC_OK);
    CHECK_EQ_UINT(p.subpaths[0].nknots, 4);
    CHECK(p.subpaths[0].closed);
    pc_path_fini(&p);
    /* A bend: the top edge (y = 0) arcs, so it gains anchors that lie on the mapped curve. */
    CHECK(pc_vec_map_path(&sq, bend, NULL, 0.01, &p, NULL) == PC_OK);
    k = p.subpaths[0].knots;
    n = p.subpaths[0].nknots;
    CHECK(n > 4);
    CHECK(pt_eq(k[0].anchor, bend(pc_point_make(0.0, 0.0), NULL)));
    for (i = 0; i < pc_subpath_segment_count(&p.subpaths[0]); i++) {
        pc_point s[4];
        pc_point mid;
        pc_subpath_segment(&p.subpaths[0], i, s);
        mid = eval(s, 0.5);
        /* Every point on the top edge maps to y = -(x - 5)^2 / 5. */
        if (s[0].y <= 0.0 && s[3].y <= 0.0) {
            CHECK(fabs(mid.y + (mid.x - 5.0) * (mid.x - 5.0) * 0.2) < 0.05);
        }
    }
    /* The vertical sides are only shifted, so they stay single straight segments. */
    for (i = 0; i < n; i++) {
        if (pt_eq(k[i].anchor, bend(pc_point_make(10.0, 0.0), NULL))) {
            right = i;
            break;
        }
    }
    CHECK(right + 1 < n);
    if (right + 1 < n) {
        CHECK(pt_eq(k[right + 1].anchor, bend(pc_point_make(10.0, 10.0), NULL)));
    }
    pc_path_fini(&p);
    pc_path_fini(&sq);
}

static pc_point wild(pc_point q, void *ctx)
{
    (void)ctx;
    return pc_point_make(q.x + 40.0 * sin(q.y * 0.7), q.y + 40.0 * cos(q.x * 0.9));
}

static pc_point nan_map(pc_point q, void *ctx)
{
    (void)q;
    (void)ctx;
    return pc_point_make(NAN, INFINITY);
}

/* Not in the reference: bounded splits on hostile maps and sizes, empty and one-knot subpaths,
 * bad tolerances. */
static void test_map_path_is_bounded_on_hostile_input(void)
{
    pc_path sq = square();
    pc_path empty;
    pc_path p;
    pc_subpath one;
    static const double pt[1][2] = {{3.0, 4.0}};
    double tols[4];
    size_t i;
    tols[0] = 0.0;
    tols[1] = -1.0;
    tols[2] = NAN;
    tols[3] = 1e-300;
    for (i = 0; i < 4; i++) {
        CHECK(pc_vec_map_path(&sq, wild, NULL, tols[i], &p, NULL) == PC_OK);
        /* At most 64 pieces per segment: 4 anchors + 4 x 63 added. */
        CHECK(p.subpaths[0].nknots >= 4 && p.subpaths[0].nknots <= 4 + 4 * 63);
        pc_path_fini(&p);
    }
    CHECK(pc_vec_map_path(&sq, nan_map, NULL, 0.1, &p, NULL) == PC_OK);
    CHECK(p.subpaths[0].nknots <= 4 + 4 * 63);
    pc_path_fini(&p);
    memset(&empty, 0, sizeof empty);
    CHECK(pc_vec_map_path(&empty, wild, NULL, 0.1, &p, NULL) == PC_OK);
    CHECK_EQ_UINT(p.nsubpaths, 0);
    pc_path_fini(&p);
    one = vh_polygon(pt, 1, false);
    vh_push(&sq, one);
    sq.inverted = true;
    sq.fill_rule = PC_FILL_RULE_EVENODD;
    CHECK(pc_vec_map_path(&sq, skew_up, NULL, 0.1, &p, NULL) == PC_OK);
    CHECK_EQ_UINT(p.nsubpaths, 2);
    CHECK(p.inverted && p.fill_rule == PC_FILL_RULE_EVENODD);
    if (p.nsubpaths == 2) {
        CHECK_EQ_UINT(p.subpaths[1].nknots, 1);
        CHECK(pt_eq(p.subpaths[1].knots[0].anchor, pc_point_make(6.0, 7.0)));
    }
    pc_path_fini(&p);
    pc_path_fini(&sq);
}

static void test_map_path_splits_less_on_huge_paths(void)
{
    /* 2^15 segments: depth drops to 5 (2^15 * 2^5 = 2^20 pieces at most). */
    size_t n = (size_t)1 << 15;
    double (*pts)[2] = malloc(n * sizeof *pts);
    pc_path big;
    pc_path p;
    size_t i;
    if (pts == NULL) {
        CHECK(pts != NULL);
        return;
    }
    for (i = 0; i < n; i++) {
        double a = 6.283185307179586 * (double)i / (double)n;
        pts[i][0] = 1000.0 * cos(a);
        pts[i][1] = 1000.0 * sin(a);
    }
    big = vh_poly_path((const double (*)[2])pts, n, true);
    free(pts);
    CHECK(pc_vec_map_path(&big, wild, NULL, 1e-9, &p, NULL) == PC_OK);
    CHECK(p.subpaths[0].nknots <= ((size_t)1 << 20));
    pc_path_fini(&p);
    pc_path_fini(&big);
}

int main(int argc, char **argv)
{
    RUN(test_map_points_moves_every_point_and_keeps_the_knots);
    RUN(test_map_path_keeps_lines_that_stay_straight_and_splits_bent_ones);
    RUN(test_map_path_is_bounded_on_hostile_input);
    RUN(test_map_path_splits_less_on_huge_paths);
    return pc_t_finish();
}
