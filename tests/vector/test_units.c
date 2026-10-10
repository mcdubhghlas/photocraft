/* The inline #[test]s of crates/vector/src/{flatten,raster,stroke,shapes,edit,trace}.rs. */
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
    return pc_t_finish();
}
