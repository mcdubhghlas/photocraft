/* The inline #[test]s of crates/vector/src/{flatten,raster,stroke,shapes,edit,trace}.rs (map.rs:
 * test_map.c). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "vh.h"

/* ---- flatten.rs ---- */

static void test_lines_are_not_subdivided(void)
{
    static const double pts[3][2] = {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}};
    pc_subpath s = vh_polygon(pts, 3, true);
    pc_vec_polyline p;
    size_t i;
    CHECK(pc_vec_flatten_subpath(&s, 0.1, &p, NULL) == PC_OK);
    CHECK_EQ_UINT(p.len, 3);
    for (i = 0; i < p.len; i++) {
        CHECK(p.knot[i]);
    }
    pc_vec_polyline_fini(&p);
    pc_subpath_fini(&s);
}

static void test_circle_flattening_respects_tolerance(void)
{
    const double tols[3] = {1.0, 0.25, 0.01};
    size_t t;
    for (t = 0; t < 3; t++) {
        double tol = tols[t];
        pc_path path = vh_ellipse(0.0, 0.0, 200.0, 200.0);
        pc_vec_polyline p;
        size_t i;
        CHECK(pc_vec_flatten_subpath(&path.subpaths[0], tol, &p, NULL) == PC_OK);
        /* Kappa circles deviate from the true circle by ~0.027% of r; chords add <= tol. */
        for (i = 0; i + 1 < p.len; i++) {
            double mx = (p.pts[i][0] + p.pts[i + 1][0]) / 2.0 - 100.0;
            double my = (p.pts[i][1] + p.pts[i + 1][1]) / 2.0 - 100.0;
            double r = sqrt(mx * mx + my * my);
            CHECK(100.0 - r <= tol + 0.03);
        }
        CHECK(p.len < 4 * (1 + (size_t)sqrt(0.75 * 100.0 / fmin(tol, 0.15)) + 1));
        pc_vec_polyline_fini(&p);
        pc_path_fini(&path);
    }
}

/* ---- raster.rs ---- */

static pc_vec_poly square(double (*buf)[2], double x, double y, double s)
{
    pc_vec_poly p;
    buf[0][0] = x;
    buf[0][1] = y;
    buf[1][0] = x + s;
    buf[1][1] = y;
    buf[2][0] = x + s;
    buf[2][1] = y + s;
    buf[3][0] = x;
    buf[3][1] = y + s;
    p.pts = buf;
    p.len = 4;
    return p;
}

static void test_axis_aligned_square_exact(void)
{
    double b[4][2];
    pc_vec_poly sq = square(b, 2.25, 3.5, 4.0);
    pc_vec_rasterizer r;
    float *c = NULL;
    size_t n = 0;
    pc_vec_rasterizer_init(&r, false);
    CHECK(pc_vec_rasterizer_add_component(&r, &sq, 1, PC_PATH_COMBINE, PC_FILL_RULE_NONZERO, NULL) == PC_OK);
    CHECK(pc_vec_rasterizer_render(&r, pc_rect_make(0, 0, 10, 10), &c, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 100);
    CHECK(fabs(vh_area(c, n) - 16.0) < 1e-4);
    CHECK(fabsf(c[3 * 10 + 2] - 0.75f * 0.5f) < 1e-5f);
    CHECK(fabsf(c[5 * 10 + 4] - 1.0f) < 1e-6f);
    CHECK(c[0] == 0.0f);
    free(c);
    pc_vec_rasterizer_fini(&r);
}

static void test_clipped_rect_matches_full_render(void)
{
    double t[3][2] = {{1.3, 0.7}, {17.9, 5.2}, {8.1, 19.6}};
    pc_vec_poly tri;
    pc_vec_rasterizer r;
    float *full = NULL;
    float *part = NULL;
    size_t nf = 0;
    size_t np = 0;
    int x;
    int y;
    double area;
    tri.pts = t;
    tri.len = 3;
    pc_vec_rasterizer_init(&r, false);
    CHECK(pc_vec_rasterizer_add_component(&r, &tri, 1, PC_PATH_COMBINE, PC_FILL_RULE_NONZERO, NULL) == PC_OK);
    CHECK(pc_vec_rasterizer_render(&r, pc_rect_make(0, 0, 20, 20), &full, &nf, NULL) == PC_OK);
    CHECK(pc_vec_rasterizer_render(&r, pc_rect_make(7, 4, 13, 15), &part, &np, NULL) == PC_OK);
    for (y = 4; y < 15; y++) {
        for (x = 7; x < 13; x++) {
            float a = full[y * 20 + x];
            float b = part[(y - 4) * 6 + (x - 7)];
            CHECK(fabsf(a - b) < 1e-5f);
        }
    }
    /* Triangle area by the shoelace formula. */
    area = 0.5 * fabs((17.9 - 1.3) * (19.6 - 0.7) - (8.1 - 1.3) * (5.2 - 0.7));
    CHECK(fabs(vh_area(full, nf) - area) < 1e-3);
    free(full);
    free(part);
    pc_vec_rasterizer_fini(&r);
}

static void test_inverted_empty_is_full(void)
{
    pc_vec_rasterizer r;
    float *c = NULL;
    size_t n = 0;
    size_t i;
    pc_vec_rasterizer_init(&r, true);
    CHECK(pc_vec_rasterizer_render(&r, pc_rect_make(0, 0, 4, 4), &c, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 16);
    for (i = 0; i < n; i++) {
        CHECK(c[i] == 1.0f);
    }
    free(c);
    pc_vec_rasterizer_fini(&r);
}

static void test_pixel_bounds_clip_upper_edge_at_i32_max(void)
{
    double b[4][2];
    double max = (double)INT32_MAX;
    pc_vec_poly sq = square(b, max - 2.0, max - 2.0, 4.0);
    pc_vec_rasterizer r;
    pc_rect pb;
    pc_surface s;
    pc_vec_rasterizer_init(&r, false);
    CHECK(pc_vec_rasterizer_add_component(&r, &sq, 1, PC_PATH_COMBINE, PC_FILL_RULE_NONZERO, NULL) == PC_OK);
    CHECK(pc_vec_rasterizer_pixel_bounds(&r, &pb));
    CHECK(pc_rect_eq(pb, pc_rect_make(INT32_MAX - 2, INT32_MAX - 2, INT32_MAX, INT32_MAX)));
    /* Raster work clipped to a normal document area stays empty and does not use the extreme
     * bounds as an allocation size. */
    CHECK(pc_vec_coverage_surface(&r, PC_PIXEL_FORMAT_GRAY8, pc_rect_make(0, 0, 8, 8), &s, NULL) == PC_OK);
    CHECK_EQ_UINT(pc_surface_tile_count(&s), 0);
    pc_surface_fini(&s);
    pc_vec_rasterizer_fini(&r);
}

static void test_pixel_bounds_beyond_i32_max_are_empty(void)
{
    double b[4][2];
    double max = (double)INT32_MAX;
    pc_vec_poly sq = square(b, max + 10.0, max + 10.0, 4.0);
    pc_vec_rasterizer r;
    pc_rect pb;
    pc_vec_rasterizer_init(&r, false);
    CHECK(pc_vec_rasterizer_add_component(&r, &sq, 1, PC_PATH_COMBINE, PC_FILL_RULE_NONZERO, NULL) == PC_OK);
    CHECK(!pc_vec_rasterizer_pixel_bounds(&r, &pb));
    pc_vec_rasterizer_fini(&r);
}

static void test_pixel_bounds_keep_normal_boundary_rounding(void)
{
    double b[4][2];
    pc_vec_poly sq = square(b, 1.0, 2.0, 3.0);
    pc_vec_rasterizer r;
    pc_rect pb;
    pc_vec_rasterizer_init(&r, false);
    CHECK(pc_vec_rasterizer_add_component(&r, &sq, 1, PC_PATH_COMBINE, PC_FILL_RULE_NONZERO, NULL) == PC_OK);
    CHECK(pc_vec_rasterizer_pixel_bounds(&r, &pb));
    CHECK(pc_rect_eq(pb, pc_rect_make(1, 2, 5, 6)));
    pc_vec_rasterizer_fini(&r);
}

/* ---- stroke.rs ---- */

static pc_vec_polyline line_of(double (*pts)[2], bool *knots, size_t n, bool closed)
{
    pc_vec_polyline p;
    size_t i;
    for (i = 0; i < n; i++) {
        knots[i] = true;
    }
    p.pts = pts;
    p.knot = knots;
    p.len = n;
    p.cap = n;
    p.closed = closed;
    return p;
}

static void test_dashes_split_by_length(void)
{
    double pts[2][2] = {{0.0, 0.0}, {10.0, 0.0}};
    bool k[2];
    pc_vec_polyline pl = line_of(pts, k, 2, false);
    static const double pat[2] = {2.0, 1.0};
    pc_vec_polyline *d = NULL;
    size_t n = 0;
    CHECK(pc_vec_dash_polyline(&pl, pat, 2, 0.0, &d, &n, NULL) == PC_OK);
    /* on [0,2] [3,5] [6,8] [9,10] */
    CHECK_EQ_UINT(n, 4);
    if (n == 4) {
        CHECK(fabs(d[1].pts[0][0] - 3.0) < 1e-9 && fabs(d[1].pts[d[1].len - 1][0] - 5.0) < 1e-9);
        CHECK(fabs(d[3].pts[d[3].len - 1][0] - 10.0) < 1e-9);
    }
    pc_vec_polylines_free(d, n);
    CHECK(pc_vec_dash_polyline(&pl, pat, 2, 1.0, &d, &n, NULL) == PC_OK);
    CHECK(n > 0 && fabs(d[0].pts[d[0].len - 1][0] - 1.0) < 1e-9);
    pc_vec_polylines_free(d, n);
}

static double signed_area(const pc_vec_poly *p)
{
    double s = -0.0;
    size_t i;
    for (i = 0; i < p->len; i++) {
        const double *a = p->pts[i];
        const double *b = p->pts[(i + 1) % p->len];
        s += a[0] * b[1] - a[1] * b[0];
    }
    return s * 0.5;
}

static void test_pieces_are_positive(void)
{
    double pts[3][2] = {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}};
    bool k[3];
    pc_vec_polyline pl = line_of(pts, k, 3, false);
    const pc_line_join joins[3] = {PC_LINE_JOIN_MITER, PC_LINE_JOIN_ROUND, PC_LINE_JOIN_BEVEL};
    const pc_line_cap caps[3] = {PC_LINE_CAP_BUTT, PC_LINE_CAP_ROUND, PC_LINE_CAP_SQUARE};
    size_t j;
    size_t c;
    for (j = 0; j < 3; j++) {
        for (c = 0; c < 3; c++) {
            pc_vec_stroke_style st;
            pc_vec_polys polys = {NULL, 0, 0};
            size_t i;
            pc_vec_stroke_style_default(&st);
            st.width = 2.0;
            st.join = joins[j];
            st.cap = caps[c];
            CHECK(pc_vec_stroke_polygons(&pl, 1, &st, 0.1, &polys, NULL) == PC_OK);
            CHECK(polys.len > 0);
            for (i = 0; i < polys.len; i++) {
                CHECK(signed_area(&polys.v[i]) > 0.0);
            }
            pc_vec_polys_fini(&polys);
        }
    }
}

/* ---- shapes.rs ---- */

static void test_rounded_rect_knots(void)
{
    static const double r10[4] = {10.0, 10.0, 10.0, 10.0};
    static const double r0[4] = {0.0, 0.0, 0.0, 0.0};
    static const double mixed[4] = {10.0, 0.0, 10.0, 0.0};
    pc_path p;
    double c[4];
    int i;
    CHECK(pc_vec_shape_rounded_rect(0.0, 0.0, 100.0, 50.0, r10, &p, NULL) == PC_OK);
    CHECK_EQ_UINT(p.subpaths[0].nknots, 8);
    pc_path_fini(&p);
    CHECK(pc_vec_shape_rounded_rect(0.0, 0.0, 100.0, 50.0, r0, &p, NULL) == PC_OK);
    CHECK_EQ_UINT(p.subpaths[0].nknots, 4);
    pc_path_fini(&p);
    CHECK(pc_vec_shape_rounded_rect(0.0, 0.0, 100.0, 50.0, mixed, &p, NULL) == PC_OK);
    CHECK_EQ_UINT(p.subpaths[0].nknots, 6);
    pc_path_fini(&p);
    pc_vec_clamp_radii(10.0, 10.0, r10, c);
    for (i = 0; i < 4; i++) {
        CHECK(c[i] == 5.0);
    }
}

static void test_star_vertices(void)
{
    pc_path p;
    pc_point top;
    CHECK(pc_vec_shape_polygon(0.0, 0.0, 10.0, 10.0, 5, 0.5, &p, NULL) == PC_OK);
    CHECK_EQ_UINT(p.subpaths[0].nknots, 10);
    pc_path_fini(&p);
    CHECK(pc_vec_shape_polygon(0.0, 0.0, 10.0, 10.0, 6, 1.0, &p, NULL) == PC_OK);
    CHECK_EQ_UINT(p.subpaths[0].nknots, 6);
    pc_path_fini(&p);
    CHECK(pc_vec_shape_polygon(0.0, 0.0, 10.0, 10.0, 3, 1.0, &p, NULL) == PC_OK);
    top = p.subpaths[0].knots[0].anchor;
    CHECK(fabs(top.x - 5.0) < 1e-9 && fabs(top.y) < 1e-9);
    pc_path_fini(&p);
}

/* ---- edit.rs ---- */

static pc_vec_knot_ref kr(size_t s, size_t k)
{
    pc_vec_knot_ref r;
    r.sub = s;
    r.knot = k;
    return r;
}

static bool pt_eq(pc_point a, double x, double y)
{
    return a.x == x && a.y == y;
}
static double dist(pc_point a, pc_point b)
{
    return hypot(a.x - b.x, a.y - b.y);
}

static void test_anchors_move_with_their_handles_once_each(void)
{
    pc_path p = vh_curve();
    pc_vec_knot_ref l[2];
    static const double d[2] = {5.0, -2.0};
    pc_knot k;
    l[0] = kr(0, 1);
    l[1] = kr(0, 1);
    CHECK(pc_vec_move_anchors(&p, l, 2, d, NULL) == PC_OK);
    k = p.subpaths[0].knots[1];
    CHECK(pt_eq(k.anchor, 55.0, -2.0) && pt_eq(k.in_ctrl, 35.0, -2.0) && pt_eq(k.out_ctrl, 75.0, -2.0));
    CHECK(pt_eq(p.subpaths[0].knots[0].anchor, 0.0, 50.0)); /* other anchors stay */
    pc_path_fini(&p);
}

static void test_bad_indices_are_errors_and_change_nothing(void)
{
    pc_path p = vh_tri();
    pc_path t = vh_tri();
    pc_path open = vh_curve();
    pc_vec_knot_ref l[2];
    static const double d[2] = {1.0, 1.0};
    pc_error e;
    l[0] = kr(0, 0);
    l[1] = kr(0, 9);
    CHECK(pc_vec_move_anchors(&p, l, 2, d, &e) == PC_ERR_INVALID);
    CHECK_EQ_STR(e.msg, "no knot 9 in subpath 0");
    CHECK(pc_path_eq(&p, &t)); /* validated before any knot moved */
    CHECK(pc_vec_move_handle(&p, kr(3, 0), PC_VEC_HANDLE_IN, pc_point_make(0.0, 0.0), false, &e) != PC_OK);
    CHECK(pc_vec_bend_segment(&p, kr(0, 7), 0.5, d, &e) != PC_OK);
    CHECK_EQ_STR(e.msg, "no segment leaves knot 7 of subpath 0");
    CHECK(pc_vec_convert_point(&p, kr(SIZE_MAX, SIZE_MAX), NULL, &e) != PC_OK);
    /* An open subpath's last knot starts no segment. */
    CHECK(pc_vec_bend_segment(&open, kr(0, 2), 0.5, d, NULL) != PC_OK);
    CHECK(pc_vec_bend_segment(&open, kr(0, SIZE_MAX), 0.5, d, NULL) != PC_OK);
    CHECK(pc_vec_bend_segment(&open, kr(5, 0), 0.5, d, &e) != PC_OK);
    CHECK_EQ_STR(e.msg, "no subpath 5");
    pc_path_fini(&p);
    pc_path_fini(&t);
    pc_path_fini(&open);
}

static void test_smooth_handles_stay_collinear_and_keep_their_length(void)
{
    pc_path p = vh_curve();
    pc_knot k;
    CHECK(pc_vec_move_handle(&p, kr(0, 1), PC_VEC_HANDLE_OUT, pc_point_make(50.0, 30.0), false, NULL) ==
          PC_OK);
    k = p.subpaths[0].knots[1];
    CHECK(pt_eq(k.out_ctrl, 50.0, 30.0));
    CHECK(dist(k.in_ctrl, pc_point_make(50.0, -20.0)) < 1e-9); /* opposite turned, length 20 kept */
    CHECK(k.smooth);
    pc_path_fini(&p);
}

static void test_independent_handles_break_the_knot(void)
{
    pc_path p = vh_curve();
    pc_knot k;
    CHECK(pc_vec_move_handle(&p, kr(0, 1), PC_VEC_HANDLE_IN, pc_point_make(40.0, 20.0), true, NULL) == PC_OK);
    k = p.subpaths[0].knots[1];
    CHECK(pt_eq(k.in_ctrl, 40.0, 20.0) && pt_eq(k.out_ctrl, 70.0, 0.0) && !k.smooth);
    pc_path_fini(&p);
}

static void test_convert_point_retracts_or_pulls_mirrored_handles(void)
{
    pc_path p = vh_curve();
    pc_knot k;
    pc_knot want = pc_knot_corner(50.0, 0.0);
    pc_point o = pc_point_make(10.0, 60.0);
    CHECK(pc_vec_convert_point(&p, kr(0, 1), NULL, NULL) == PC_OK);
    CHECK(pc_knot_eq(&p.subpaths[0].knots[1], &want));
    CHECK(pc_vec_convert_point(&p, kr(0, 0), &o, NULL) == PC_OK);
    k = p.subpaths[0].knots[0];
    CHECK(pt_eq(k.in_ctrl, -10.0, 40.0) && pt_eq(k.out_ctrl, 10.0, 60.0) && k.smooth);
    pc_path_fini(&p);
}

static void test_straight_segments_move_with_both_anchors(void)
{
    pc_path p = vh_tri();
    static const double d[2] = {0.0, 10.0};
    /* The closing segment (2 -> 0) wraps. */
    CHECK(pc_vec_bend_segment(&p, kr(0, 2), 0.5, d, NULL) == PC_OK);
    CHECK(pt_eq(p.subpaths[0].knots[2].anchor, 50.0, 90.0));
    CHECK(pt_eq(p.subpaths[0].knots[0].anchor, 0.0, 10.0));
    CHECK(pt_eq(p.subpaths[0].knots[1].anchor, 100.0, 0.0));
    pc_path_fini(&p);
}

static pc_point seg_eval(const pc_path *p, size_t i, double t)
{
    pc_point s[4];
    double u = 1.0 - t;
    double a;
    double b;
    double c;
    double d;
    pc_subpath_segment(&p->subpaths[0], i, s);
    a = u * u * u;
    b = 3.0 * u * u * t;
    c = 3.0 * u * t * t;
    d = t * t * t;
    return pc_point_make(a * s[0].x + b * s[1].x + c * s[2].x + d * s[3].x,
                         a * s[0].y + b * s[1].y + c * s[2].y + d * s[3].y);
}

static void test_curves_bend_through_the_dragged_point(void)
{
    const double ts[3] = {0.2, 0.5, 0.8};
    size_t i;
    for (i = 0; i < 3; i++) {
        pc_path p = vh_curve();
        static const double d[2] = {3.0, 12.0};
        pc_point before = seg_eval(&p, 1, ts[i]);
        pc_point after;
        pc_knot k;
        double vi[2];
        double vo[2];
        CHECK(pc_vec_bend_segment(&p, kr(0, 1), ts[i], d, NULL) == PC_OK);
        after = seg_eval(&p, 1, ts[i]);
        CHECK(dist(after, pc_point_make(before.x + 3.0, before.y + 12.0)) < 1e-9);
        k = p.subpaths[0].knots[1];
        vi[0] = k.in_ctrl.x - 50.0;
        vi[1] = k.in_ctrl.y;
        vo[0] = k.out_ctrl.x - 50.0;
        vo[1] = k.out_ctrl.y;
        /* The smooth knot stayed smooth. */
        CHECK(fabs(vi[0] * vo[1] - vi[1] * vo[0]) < 1e-9 && vi[0] * vo[0] + vi[1] * vo[1] < 0.0);
        pc_path_fini(&p);
    }
}

static void test_hits_prefer_handles_then_anchors_then_segments(void)
{
    pc_path p = vh_curve();
    pc_path t = vh_tri();
    pc_vec_handle_ref shown;
    pc_vec_hit h;
    shown.knot = kr(0, 1);
    shown.handle = PC_VEC_HANDLE_OUT;
    CHECK(pc_vec_hit_test(&p, pc_point_make(71.0, 1.0), 3.0, &shown, 1, &h));
    CHECK(h.kind == PC_VEC_HIT_HANDLE && h.knot.sub == 0 && h.knot.knot == 1 &&
          h.handle == PC_VEC_HANDLE_OUT);
    CHECK(!pc_vec_hit_test(&p, pc_point_make(71.0, 1.0), 3.0, NULL, 0, &h)); /* hidden handles aren't hit */
    CHECK(pc_vec_hit_test(&p, pc_point_make(49.0, 1.0), 3.0, &shown, 1, &h));
    CHECK(h.kind == PC_VEC_HIT_ANCHOR && h.knot.sub == 0 && h.knot.knot == 1);
    CHECK(pc_vec_hit_test(&t, pc_point_make(50.0, 1.0), 3.0, NULL, 0, &h));
    CHECK(h.kind == PC_VEC_HIT_SEGMENT && h.knot.sub == 0 && h.knot.knot == 0);
    CHECK(fabs(h.t - 0.5) < 0.02);
    CHECK(!pc_vec_hit_test(&t, pc_point_make(50.0, 40.0), 3.0, NULL, 0, &h));
    pc_path_fini(&p);
    pc_path_fini(&t);
}

static void test_marquee_and_subpath_selections(void)
{
    pc_path p = vh_tri();
    static const double box[4] = {110.0, -5.0, 40.0, 5.0};
    pc_vec_knot_ref *v = NULL;
    size_t n = 0;
    CHECK(pc_vec_anchors_in(&p, box, &v, &n, NULL) == PC_OK);
    CHECK(n == 1 && v[0].sub == 0 && v[0].knot == 1);
    free(v);
    CHECK(pc_vec_subpath_anchors(&p, 0, &v, &n, NULL) == PC_OK);
    CHECK(n == 3 && v[0].knot == 0 && v[1].knot == 1 && v[2].knot == 2);
    free(v);
    CHECK(pc_vec_subpath_anchors(&p, 4, &v, &n, NULL) == PC_OK);
    CHECK(n == 0 && v == NULL);
    pc_path_fini(&p);
}

static void test_handle_names(void)
{
    pc_vec_handle h;
    CHECK(pc_vec_handle_parse("in", 2, &h) && h == PC_VEC_HANDLE_IN);
    CHECK(pc_vec_handle_parse("out", 3, &h) && h == PC_VEC_HANDLE_OUT);
    CHECK(!pc_vec_handle_parse("In", 2, &h));
    CHECK(!pc_vec_handle_parse("outer", 5, &h));
    CHECK_EQ_STR(pc_vec_handle_name(PC_VEC_HANDLE_IN), "in");
    CHECK_EQ_STR(pc_vec_handle_name(PC_VEC_HANDLE_OUT), "out");
}

/* ---- trace.rs ---- */

static void test_square_contour_orientation_and_count(void)
{
    float v[6 * 5];
    pc_vec_polys c = {NULL, 0, 0};
    int x;
    int y;
    memset(v, 0, sizeof v);
    for (y = 1; y < 4; y++) {
        for (x = 1; x < 5; x++) {
            v[y * 6 + x] = 1.0f;
        }
    }
    CHECK(pc_vec_contours(v, 30, pc_rect_make(0, 0, 6, 5), 0.0f, &c, NULL) == PC_OK);
    CHECK_EQ_UINT(c.len, 1);
    if (c.len == 1) {
        /* Inside on the left in y-down screen space = counter-clockwise = negative area. */
        double a = signed_area(&c.v[0]);
        CHECK(a < 0.0);
        /* Pixels 1..5 x 1..4: a 4x3 area with 45 deg chamfered corners, 12 - 4 * 0.125. */
        CHECK(fabs(-a - 11.5) < 1e-9);
    }
    pc_vec_polys_fini(&c);
}

static void test_traced_rect_has_corners(void)
{
    float *v = calloc(40 * 30, sizeof *v);
    pc_path p;
    size_t corners = 0;
    size_t i;
    int x;
    int y;
    if (v == NULL) {
        CHECK(v != NULL);
        return;
    }
    for (y = 5; y < 25; y++) {
        for (x = 5; x < 35; x++) {
            v[y * 40 + x] = 1.0f;
        }
    }
    CHECK(pc_vec_trace_mask(v, 1200, pc_rect_make(0, 0, 40, 30), 0.0f, 1.0, &p, NULL) == PC_OK);
    CHECK_EQ_UINT(p.nsubpaths, 1);
    if (p.nsubpaths == 1) {
        size_t n = p.subpaths[0].nknots;
        CHECK(n >= 4 && n <= 12);
        for (i = 0; i < n; i++) {
            corners += p.subpaths[0].knots[i].smooth ? 0u : 1u;
        }
        CHECK(corners >= 4);
    }
    pc_path_fini(&p);
    free(v);
}

/* ---- stroke.rs: closed dashes, validation, non-uniform patterns ---- */

static void test_closed_dashes_have_a_join_at_the_seam(void)
{
    double pts[4][2] = {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}};
    bool k[4];
    pc_vec_polyline sq = line_of(pts, k, 4, true);
    static const double pat[2] = {8.0, 4.0};
    static const double full[2] = {80.0, 2.0};
    pc_vec_polyline *d = NULL;
    size_t n = 0;
    CHECK(pc_vec_dash_polyline(&sq, pat, 2, 2.0, &d, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 3);
    if (n == 3) {
        const pc_vec_polyline *c = &d[0];
        CHECK_EQ_UINT(c->len, 3);
        if (c->len == 3) {
            CHECK(c->pts[0][0] == 0.0 && c->pts[0][1] == 6.0);
            CHECK(c->pts[1][0] == 0.0 && c->pts[1][1] == 0.0);
            CHECK(c->pts[2][0] == 6.0 && c->pts[2][1] == 0.0);
            CHECK(c->knot[0] && c->knot[1] && c->knot[2]);
        }
    }
    pc_vec_polylines_free(d, n);
    CHECK(pc_vec_dash_polyline(&sq, full, 2, 0.0, &d, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 1);
    CHECK(n == 1 && d[0].closed);
    pc_vec_polylines_free(d, n);
}

static void test_invalid_and_too_dense_dashes_finish_with_bounded_work(void)
{
    double pts[2][2] = {{0.0, 0.0}, {100.0, 0.0}};
    bool k[2];
    pc_vec_polyline pl = line_of(pts, k, 2, false);
    double pats[6][33];
    size_t lens[6] = {2, 2, 2, 2, 33, 2};
    static const double two_one[2] = {2.0, 1.0};
    pc_vec_polyline *d = NULL;
    size_t n = 0;
    size_t i;
    memset(pats, 0, sizeof pats);
    pats[1][0] = INFINITY;
    pats[1][1] = 1.0;
    pats[2][0] = NAN;
    pats[2][1] = 1.0;
    pats[3][0] = -1.0;
    pats[3][1] = 2.0;
    for (i = 0; i < 33; i++) {
        pats[4][i] = 1.0;
    }
    pats[5][0] = 1e-8;
    pats[5][1] = 1e-8;
    for (i = 0; i < 6; i++) {
        n = 99;
        CHECK(pc_vec_dash_polyline(&pl, pats[i], lens[i], 0.0, &d, &n, NULL) == PC_OK);
        CHECK(n == 0 && d == NULL);
        pc_vec_polylines_free(d, n);
    }
    CHECK(pc_vec_dash_polyline(&pl, two_one, 2, INFINITY, &d, &n, NULL) == PC_OK);
    CHECK(n == 0);
    pc_vec_polylines_free(d, n);
    pts[0][0] = NAN;
    pts[1][0] = 1.0;
    CHECK(pc_vec_dash_polyline(&pl, two_one, 2, 0.0, &d, &n, NULL) == PC_OK);
    CHECK(n == 0);
    pc_vec_polylines_free(d, n);
}

static void test_nonuniform_patterns_and_negative_offsets_repeat_by_arc_length(void)
{
    double pts[2][2] = {{0.0, 0.0}, {30.0, 0.0}};
    bool k[2];
    pc_vec_polyline pl = line_of(pts, k, 2, false);
    static const double pat[4] = {4.0, 2.0, 1.0, 3.0};
    static const double want[6][2] = {{0.0, 4.0},   {6.0, 7.0},   {10.0, 14.0},
                                      {16.0, 17.0}, {20.0, 24.0}, {26.0, 27.0}};
    pc_vec_polyline *d = NULL;
    size_t n = 0;
    size_t i;
    CHECK(pc_vec_dash_polyline(&pl, pat, 4, -10.0, &d, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 6);
    for (i = 0; i < n && n == 6; i++) {
        CHECK(d[i].pts[0][0] == want[i][0] && d[i].pts[d[i].len - 1][0] == want[i][1]);
    }
    pc_vec_polylines_free(d, n);
}

/* ---- edit.rs: Add / Delete Anchor Point, Curvature Pen ---- */

static pc_knot at(const pc_path *p, size_t s, size_t k)
{
    return p->subpaths[s].knots[k];
}

static void seg_of(const pc_path *p, size_t s, size_t i, pc_point out[4])
{
    pc_subpath_segment(&p->subpaths[s], i, out);
}

static bool seg_eq(const pc_point a[4], const pc_point b[4])
{
    size_t i;
    for (i = 0; i < 4; i++) {
        if (a[i].x != b[i].x || a[i].y != b[i].y) {
            return false;
        }
    }
    return true;
}

static void test_adding_to_a_curve_splits_it_without_changing_its_shape(void)
{
    static const double ts[3] = {0.5, 0.13, 0.9};
    size_t ti;
    for (ti = 0; ti < 3; ti++) {
        double t = ts[ti];
        pc_path p = vh_curve();
        pc_path orig = vh_curve();
        pc_point seg[4];
        pc_point halves[3][4];
        pc_point next[4];
        pc_vec_knot_ref r = {9, 9};
        pc_knot k;
        int i;
        seg_of(&orig, 0, 0, seg);
        CHECK(pc_vec_add_anchor(&p, kr(0, 0), t, &r, NULL) == PC_OK);
        CHECK(r.sub == 0 && r.knot == 1);
        CHECK_EQ_UINT(p.subpaths[0].nknots, 4);
        if (p.subpaths[0].nknots != 4) {
            pc_path_fini(&p);
            pc_path_fini(&orig);
            continue;
        }
        k = at(&p, 0, 1);
        CHECK(k.smooth); /* a split curve gets a smooth anchor */
        CHECK(dist(k.anchor, pc_vec_eval(seg, t)) < 1e-9);
        /* 100 evenly spaced parameters of the old segment land on the two halves. */
        seg_of(&p, 0, 0, halves[0]);
        seg_of(&p, 0, 1, halves[1]);
        seg_of(&p, 0, 2, halves[2]);
        for (i = 0; i <= 100; i++) {
            double u = (double)i / 100.0;
            pc_point q = u <= t ? pc_vec_eval(halves[0], u / t) : pc_vec_eval(halves[1], (u - t) / (1.0 - t));
            CHECK(dist(q, pc_vec_eval(seg, u)) < 0.01);
        }
        seg_of(&orig, 0, 1, next);
        CHECK(seg_eq(halves[2], next)); /* the next segment is untouched */
        pc_path_fini(&p);
        pc_path_fini(&orig);
    }
}

static void test_adding_to_a_straight_segment_uses_the_hit_position(void)
{
    static const double a[2][2] = {{0.0, 0.0}, {100.0, 0.0}};
    static const double b[2][2] = {{0.0, 0.0}, {0.0, 100.0}};
    static const double c[2][2] = {{0.0, 0.0}, {100.0, 100.0}};
    static const double d[3][2] = {{0.0, 0.0}, {100.0, 0.0}, {100.0, 100.0}};
    const double (*cases[4])[2] = {a, b, c, d};
    static const size_t ns[4] = {2, 2, 2, 3};
    static const bool closed[4] = {false, false, false, true};
    static const double ts[2] = {0.25, 0.75};
    size_t ci;
    size_t ti;
    for (ci = 0; ci < 4; ci++) {
        for (ti = 0; ti < 2; ti++) {
            pc_path p = vh_poly_path(cases[ci], ns[ci], closed[ci]);
            pc_point seg[4];
            pc_point clicked;
            pc_vec_hit h;
            pc_vec_knot_ref added = {0, 0};
            pc_knot ins;
            pc_knot want;
            seg_of(&p, 0, closed[ci] ? 2 : 0, seg);
            clicked = pc_vec_eval(seg, ts[ti]);
            CHECK(pc_vec_hit_test(&p, clicked, 1.0, NULL, 0, &h));
            CHECK(h.kind == PC_VEC_HIT_SEGMENT);
            if (h.kind == PC_VEC_HIT_SEGMENT && pc_vec_add_anchor(&p, h.knot, h.t, &added, NULL) == PC_OK) {
                ins = at(&p, added.sub, added.knot);
                want = pc_knot_corner(clicked.x, clicked.y);
                CHECK(dist(ins.anchor, clicked) < 1e-9);
                CHECK(pc_knot_eq(&ins, &want)); /* straight segments retain retracted handles */
            } else {
                CHECK(false);
            }
            pc_path_fini(&p);
        }
    }
}

static void test_adding_to_a_straight_segment_makes_a_corner_on_the_line(void)
{
    pc_path p = vh_tri();
    pc_vec_knot_ref r = {0, 0};
    pc_knot k;
    pc_knot want;
    /* The closing segment (2 -> 0) wraps: the new anchor goes last. */
    CHECK(pc_vec_add_anchor(&p, kr(0, 2), 0.5, &r, NULL) == PC_OK);
    CHECK(r.sub == 0 && r.knot == 3);
    k = at(&p, 0, 3);
    want = pc_knot_corner(25.0, 40.0);
    CHECK(pc_knot_eq(&k, &want));
    CHECK(pc_vec_add_anchor(&p, kr(0, 0), 0.25, &r, NULL) == PC_OK);
    CHECK(r.sub == 0 && r.knot == 1);
    k = at(&p, 0, 1);
    want = pc_knot_corner(15.625, 0.0);
    CHECK(pc_knot_eq(&k, &want));
    CHECK_EQ_UINT(p.subpaths[0].nknots, 5);
    CHECK(p.subpaths[0].closed);
    pc_path_fini(&p);
}

static void test_deleting_joins_the_neighbours_with_their_handles(void)
{
    static const double sqp[4][2] = {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}};
    static const double cp[3][2] = {{0.0, 50.0}, {50.0, 0.0}, {100.0, 50.0}};
    pc_path sq = vh_poly_path(sqp, 4, true);
    pc_path c = vh_poly_path(cp, 3, false);
    bool removed = true;
    pc_point seg[4];
    pc_point want[4];
    CHECK(pc_vec_delete_anchor(&sq, kr(0, 2), &removed, NULL) == PC_OK);
    CHECK(!removed);
    CHECK(sq.subpaths[0].nknots == 3 && sq.subpaths[0].closed);
    c.subpaths[0].knots[0].out_ctrl = pc_point_make(10.0, 20.0);
    c.subpaths[0].knots[2].in_ctrl = pc_point_make(90.0, 20.0);
    CHECK(pc_vec_delete_anchor(&c, kr(0, 1), &removed, NULL) == PC_OK);
    CHECK_EQ_UINT(pc_subpath_segment_count(&c.subpaths[0]), 1);
    seg_of(&c, 0, 0, seg);
    want[0] = pc_point_make(0.0, 50.0);
    want[1] = pc_point_make(10.0, 20.0);
    want[2] = pc_point_make(90.0, 20.0);
    want[3] = pc_point_make(100.0, 50.0);
    CHECK(seg_eq(seg, want));
    pc_path_fini(&sq);
    pc_path_fini(&c);
}

static void test_deleting_a_subpaths_last_anchor_removes_it_but_never_the_last_anchor(void)
{
    static const double one_pt[1][2] = {{5.0, 5.0}};
    pc_path p = vh_tri();
    pc_path t = vh_tri();
    pc_path one = vh_poly_path(one_pt, 1, false);
    pc_path before = vh_clone(&one);
    bool removed = false;
    pc_error e;
    vh_push(&p, vh_polygon(one_pt, 1, false));
    CHECK(pc_vec_delete_anchor(&p, kr(1, 0), &removed, NULL) == PC_OK);
    CHECK(removed);
    CHECK(pc_path_eq(&p, &t));
    CHECK(pc_vec_delete_anchor(&one, kr(0, 0), &removed, &e) == PC_ERR_INVALID);
    CHECK_EQ_STR(e.msg, "can't delete the path's only anchor point; delete the path instead");
    CHECK(pc_path_eq(&one, &before));
    pc_path_fini(&p);
    pc_path_fini(&t);
    pc_path_fini(&one);
    pc_path_fini(&before);
}

static void test_bad_anchor_edits_are_errors_and_change_nothing(void)
{
    pc_path p = vh_curve();
    pc_path c = vh_curve();
    double ts[6];
    pc_vec_knot_ref r;
    bool removed;
    pc_error e;
    size_t i;
    ts[0] = 0.0;
    ts[1] = 1.0;
    ts[2] = -0.5;
    ts[3] = 2.0;
    ts[4] = NAN;
    ts[5] = INFINITY;
    for (i = 0; i < 6; i++) {
        CHECK(pc_vec_add_anchor(&p, kr(0, 0), ts[i], &r, &e) == PC_ERR_INVALID);
        CHECK_EQ_STR(e.msg, "`t` must be strictly between 0 and 1");
    }
    /* An open subpath's last knot starts no segment. */
    CHECK(pc_vec_add_anchor(&p, kr(0, 2), 0.5, &r, &e) != PC_OK);
    CHECK_EQ_STR(e.msg, "no segment leaves knot 2 of subpath 0");
    CHECK(pc_vec_add_anchor(&p, kr(0, SIZE_MAX), 0.5, &r, NULL) != PC_OK);
    CHECK(pc_vec_add_anchor(&p, kr(SIZE_MAX, 0), 0.5, &r, NULL) != PC_OK);
    CHECK(pc_vec_delete_anchor(&p, kr(0, 3), &removed, &e) != PC_OK);
    CHECK_EQ_STR(e.msg, "no knot 3 in subpath 0");
    CHECK(pc_vec_delete_anchor(&p, kr(1, 0), &removed, NULL) != PC_OK);
    CHECK(pc_path_eq(&p, &c));
    pc_path_fini(&p);
    pc_path_fini(&c);
}

static void test_curvature_knots_are_smooth_and_finite(void)
{
    static const double pts[4][2] = {{0.0, 0.0}, {30.0, 0.0}, {60.0, 30.0}, {60.0, 30.0}};
    static const bool corners[2] = {false, true};
    double out[4][3][2];
    size_t i;
    size_t j;
    /* An inner anchor's handles are collinear and a third of the way to each neighbour. */
    pc_vec_curvature_knots(pts, 3, NULL, 0, false, out);
    CHECK(fabs(hypot(out[1][1][0] - 30.0, out[1][1][1]) - 10.0) < 1e-9);
    CHECK(fabs(hypot(out[1][2][0] - 30.0, out[1][2][1]) - hypot(30.0, 30.0) / 3.0) < 1e-9);
    CHECK(fabs((out[1][1][0] - 30.0) * out[1][2][1] - out[1][1][1] * (out[1][2][0] - 30.0)) < 1e-9);
    /* A corner keeps retracted handles; coincident points never give NaN. */
    pc_vec_curvature_knots(pts, 4, corners, 2, true, out);
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 3; j++) {
            CHECK(isfinite(out[i][j][0]) && isfinite(out[i][j][1]));
        }
    }
    CHECK(out[1][1][0] == 30.0 && out[1][2][0] == 30.0 && out[1][1][1] == 0.0);
    pc_vec_curvature_knots(pts, 0, NULL, 0, false, NULL);
}

/* ---- trace.rs: fit_trace (the Freeform Pen) ---- */

/* Largest distance from a sample to the fitted subpath (each segment densely sampled). */
static double max_deviation(const pc_subpath *sp, const double (*samples)[2], size_t n)
{
    size_t nseg = pc_subpath_segment_count(sp);
    double worst = 0.0;
    size_t i;
    for (i = 0; i < n; i++) {
        double best = INFINITY;
        size_t s;
        for (s = 0; s < nseg; s++) {
            pc_point seg[4];
            int k;
            pc_subpath_segment(sp, s, seg);
            for (k = 0; k <= 400; k++) {
                pc_point c = pc_vec_eval(seg, (double)k / 400.0);
                double d = hypot(c.x - samples[i][0], c.y - samples[i][1]);
                if (d < best) {
                    best = d;
                }
            }
        }
        if (best > worst) {
            worst = best;
        }
    }
    return worst;
}

static void test_fit_trace_follows_fixtures_with_few_anchors(void)
{
    const double tol = 2.0;
    static double line[101][2];
    static double zig[151][2];
    static double circle[180][2];
    static double sine[201][2];
    static const double turns[6][2] = {{0.0, 0.0},    {60.0, 0.0},    {60.0, 60.0},
                                       {120.0, 60.0}, {120.0, 120.0}, {180.0, 120.0}};
    pc_subpath sp;
    bool ok = false;
    size_t i;
    size_t n = 0;
    /* Straight drag with +-0.5 px of hand jitter, 3 px apart. */
    for (i = 0; i <= 100; i++) {
        line[i][0] = 10.0 + 3.0 * (double)i;
        line[i][1] = 50.0 + (i % 2 == 0 ? 0.5 : -0.5);
    }
    CHECK(pc_vec_fit_trace((const double (*)[2])line, 101, tol, false, &sp, &ok, NULL) == PC_OK && ok);
    if (ok) {
        CHECK(!sp.closed);
        CHECK_EQ_UINT(sp.nknots, 2); /* a straight drag is one segment */
        CHECK(max_deviation(&sp, (const double (*)[2])line, 101) <= tol + 0.05);
        pc_subpath_fini(&sp);
    }
    /* Zig-zag: five 60 px legs at right angles, sampled every 2 px. */
    for (i = 0; i < 5; i++) {
        int k;
        for (k = 0; k < 30; k++) {
            zig[n][0] = turns[i][0] + (turns[i + 1][0] - turns[i][0]) * (double)k / 30.0;
            zig[n][1] = turns[i][1] + (turns[i + 1][1] - turns[i][1]) * (double)k / 30.0;
            n++;
        }
    }
    zig[n][0] = turns[5][0];
    zig[n][1] = turns[5][1];
    n++;
    ok = false;
    CHECK(pc_vec_fit_trace((const double (*)[2])zig, n, tol, false, &sp, &ok, NULL) == PC_OK && ok);
    if (ok) {
        CHECK_EQ_UINT(sp.nknots, 6); /* the corners are kept as anchors, nothing between */
        for (i = 0; i < sp.nknots && sp.nknots == 6; i++) {
            CHECK(sp.knots[i].anchor.x == turns[i][0] && sp.knots[i].anchor.y == turns[i][1]);
            if (i >= 1 && i < 5) {
                CHECK(!sp.knots[i].smooth);
            }
        }
        CHECK(max_deviation(&sp, (const double (*)[2])zig, n) <= tol + 0.05);
        pc_subpath_fini(&sp);
    }
    /* Circle, radius 80, 180 samples, closed. */
    for (i = 0; i < 180; i++) {
        double a = (double)i * (3.141592653589793 / 180.0) * 2.0;
        circle[i][0] = 100.0 + 80.0 * cos(a);
        circle[i][1] = 100.0 + 80.0 * sin(a);
    }
    ok = false;
    CHECK(pc_vec_fit_trace((const double (*)[2])circle, 180, tol, true, &sp, &ok, NULL) == PC_OK && ok);
    if (ok) {
        CHECK(sp.closed);
        CHECK(sp.nknots >= 2 && sp.nknots <= 8);
        for (i = 0; i < sp.nknots; i++) {
            CHECK(sp.knots[i].smooth); /* a circle has no corners */
        }
        CHECK(max_deviation(&sp, (const double (*)[2])circle, 180) <= tol + 0.05);
        pc_subpath_fini(&sp);
    }
    /* Two periods of a sine wave (amplitude 40, wavelength 200) with the same jitter: at most two
     * segments per half period. */
    for (i = 0; i <= 200; i++) {
        double x = (double)i * 2.0;
        sine[i][0] = x;
        sine[i][1] = 100.0 + 40.0 * sin(x / 200.0 * 6.283185307179586) + 0.5 - (double)(i % 2);
    }
    ok = false;
    CHECK(pc_vec_fit_trace((const double (*)[2])sine, 201, tol, false, &sp, &ok, NULL) == PC_OK && ok);
    if (ok) {
        CHECK(sp.nknots >= 3 && sp.nknots <= 9);
        for (i = 1; i + 1 < sp.nknots; i++) {
            CHECK(sp.knots[i].smooth);
        }
        CHECK(max_deviation(&sp, (const double (*)[2])sine, 201) <= tol + 0.05);
        pc_subpath_fini(&sp);
    }
}

static void test_fit_trace_survives_hostile_input(void)
{
    static double same[50][2];
    double bad[3][2];
    double (*lng)[2] = malloc(200000 * sizeof *lng);
    pc_subpath sp;
    bool ok = true;
    size_t i;
    CHECK(pc_vec_fit_trace(NULL, 0, 2.0, false, &sp, &ok, NULL) == PC_OK && !ok);
    for (i = 0; i < 50; i++) {
        same[i][0] = 1.0;
        same[i][1] = 1.0;
    }
    ok = true;
    CHECK(pc_vec_fit_trace((const double (*)[2])same, 50, 2.0, false, &sp, &ok, NULL) == PC_OK && !ok);
    bad[0][0] = NAN;
    bad[0][1] = 0.0;
    bad[1][0] = INFINITY;
    bad[1][1] = 1.0;
    bad[2][0] = 1e300;
    bad[2][1] = 2.0;
    ok = true;
    CHECK(pc_vec_fit_trace((const double (*)[2])bad, 3, NAN, true, &sp, &ok, NULL) == PC_OK && !ok);
    if (lng == NULL) {
        CHECK(lng != NULL);
        return;
    }
    for (i = 0; i < 200000; i++) {
        lng[i][0] = (double)i * 0.01;
        lng[i][1] = sin((double)i * 0.001) * 40.0;
    }
    ok = false;
    CHECK(pc_vec_fit_trace((const double (*)[2])lng, 200000, -5.0, false, &sp, &ok, NULL) == PC_OK && ok);
    if (ok) {
        CHECK(sp.nknots >= 2);
        for (i = 0; i < sp.nknots; i++) {
            CHECK(isfinite(sp.knots[i].anchor.x) && isfinite(sp.knots[i].out_ctrl.y));
        }
        pc_subpath_fini(&sp);
    }
    free(lng);
}

int main(int argc, char **argv)
{
    RUN(test_lines_are_not_subdivided);
    RUN(test_circle_flattening_respects_tolerance);
    RUN(test_axis_aligned_square_exact);
    RUN(test_clipped_rect_matches_full_render);
    RUN(test_inverted_empty_is_full);
    RUN(test_pixel_bounds_clip_upper_edge_at_i32_max);
    RUN(test_pixel_bounds_beyond_i32_max_are_empty);
    RUN(test_pixel_bounds_keep_normal_boundary_rounding);
    RUN(test_dashes_split_by_length);
    RUN(test_pieces_are_positive);
    RUN(test_rounded_rect_knots);
    RUN(test_star_vertices);
    RUN(test_anchors_move_with_their_handles_once_each);
    RUN(test_bad_indices_are_errors_and_change_nothing);
    RUN(test_smooth_handles_stay_collinear_and_keep_their_length);
    RUN(test_independent_handles_break_the_knot);
    RUN(test_convert_point_retracts_or_pulls_mirrored_handles);
    RUN(test_straight_segments_move_with_both_anchors);
    RUN(test_curves_bend_through_the_dragged_point);
    RUN(test_hits_prefer_handles_then_anchors_then_segments);
    RUN(test_marquee_and_subpath_selections);
    RUN(test_handle_names);
    RUN(test_square_contour_orientation_and_count);
    RUN(test_traced_rect_has_corners);
    RUN(test_closed_dashes_have_a_join_at_the_seam);
    RUN(test_invalid_and_too_dense_dashes_finish_with_bounded_work);
    RUN(test_nonuniform_patterns_and_negative_offsets_repeat_by_arc_length);
    RUN(test_adding_to_a_curve_splits_it_without_changing_its_shape);
    RUN(test_adding_to_a_straight_segment_uses_the_hit_position);
    RUN(test_adding_to_a_straight_segment_makes_a_corner_on_the_line);
    RUN(test_deleting_joins_the_neighbours_with_their_handles);
    RUN(test_deleting_a_subpaths_last_anchor_removes_it_but_never_the_last_anchor);
    RUN(test_bad_anchor_edits_are_errors_and_change_nothing);
    RUN(test_curvature_knots_are_smooth_and_finite);
    RUN(test_fit_trace_follows_fixtures_with_few_anchors);
    RUN(test_fit_trace_survives_hostile_input);
    return pc_t_finish();
}
