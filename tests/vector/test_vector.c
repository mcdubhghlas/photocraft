/* crates/vector/src/tests.rs: areas, booleans, strokes, shape rendering, gradients, masks and
 * tracing round trips. perf_6016 (ignored in the reference) runs with PHOTOCRAFT_SLOW_TESTS=1. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "harness.h"
#include "raster/pixel.h"
#include "vh.h"

#define PI 3.141592653589793

static double cov_area(const pc_path *p, pc_rect r)
{
    float *v = NULL;
    size_t n = 0;
    double a;
    if (pc_vec_path_coverage(p, r, &v, &n, NULL) != PC_OK) {
        return NAN;
    }
    a = vh_area(v, n);
    free(v);
    return a;
}

static float *cov(const pc_path *p, pc_rect r)
{
    float *v = NULL;
    size_t n = 0;
    CHECK(pc_vec_path_coverage(p, r, &v, &n, NULL) == PC_OK);
    return v;
}

static void test_circle_area_matches_analytic(void)
{
    const double rs[4] = {3.3, 10.0, 47.5, 200.25};
    size_t i;
    for (i = 0; i < 4; i++) {
        double r = rs[i];
        double c = 256.0 + 0.37;
        pc_path p = vh_ellipse(c - r, c - r, 2.0 * r, 2.0 * r);
        double got = cov_area(&p, pc_rect_make(0, 0, 512, 512));
        double want = PI * r * r;
        /* The 4-arc Bezier circle overshoots the true circle by up to 0.027% of r. */
        CHECK(fabs(got - want) / want < 0.005);
        if (r >= 10.0) {
            CHECK(fabs(got - want) / want < 0.0015);
        }
        pc_path_fini(&p);
    }
}

static void test_rect_area_exact_and_rotated(void)
{
    pc_path p = vh_rect(10.3, 20.7, 100.45, 50.2);
    pc_path rot;
    double got = cov_area(&p, pc_rect_make(0, 0, 200, 100));
    CHECK(fabs(got - 100.45 * 50.2) < 1e-2);
    /* Rotated: area preserved. */
    CHECK(pc_path_transform(&p, pc_affine_then(pc_affine_rotate(0.4), pc_affine_translate(60.0, 30.0)), &rot,
                            NULL) == PC_OK);
    got = cov_area(&rot, pc_rect_make(-100, -100, 400, 400));
    CHECK(fabs(got - 100.45 * 50.2) / (100.45 * 50.2) < 1e-4);
    pc_path_fini(&p);
    pc_path_fini(&rot);
}

static void test_coverage_is_tile_independent(void)
{
    pc_path p;
    float *full;
    const int origins[3][2] = {{0, 0}, {13, 29}, {50, 50}};
    size_t i;
    CHECK(pc_vec_shape_polygon(3.0, 5.0, 90.0, 80.0, 7, 0.45, &p, NULL) == PC_OK);
    full = cov(&p, pc_rect_make(0, 0, 100, 100));
    for (i = 0; i < 3 && full != NULL; i++) {
        int x0 = origins[i][0];
        int y0 = origins[i][1];
        pc_rect r = pc_rect_make(x0, y0, x0 + 37, y0 + 23);
        float *part = cov(&p, r);
        int x;
        int y;
        for (y = r.y0; y < r.y1 && part != NULL; y++) {
            for (x = r.x0; x < r.x1; x++) {
                CHECK(fabsf(full[y * 100 + x] - part[(y - y0) * 37 + (x - x0)]) < 1e-5f);
            }
        }
        free(part);
    }
    free(full);
    pc_path_fini(&p);
}

static void test_even_odd_vs_non_zero(void)
{
    pc_rect r = pc_rect_make(0, 0, 100, 100);
    pc_path nzp = vh_pentagram();
    pc_path eop = vh_pentagram();
    float *nz;
    float *eo;
    double r_in;
    double inner;
    eop.fill_rule = PC_FILL_RULE_EVENODD;
    nz = cov(&nzp, r);
    eo = cov(&eop, r);
    if (nz != NULL && eo != NULL) {
        double an = vh_area(nz, 10000);
        double ae = vh_area(eo, 10000);
        /* Centre: filled with non-zero (winding 2), empty with even-odd. */
        CHECK(nz[50 * 100 + 50] == 1.0f);
        CHECK(eo[50 * 100 + 50] == 0.0f);
        /* A star tip is filled with both. */
        CHECK(nz[16 * 100 + 50] == 1.0f);
        CHECK(eo[16 * 100 + 50] == 1.0f);
        CHECK(an > ae + 500.0);
        /* Non-zero area - even-odd area = the inner pentagon (circumradius R cos72 / cos36). */
        r_in = 40.0 * cos(72.0 * PI / 180.0) / cos(36.0 * PI / 180.0);
        inner = 2.5 * r_in * r_in * sin(72.0 * PI / 180.0);
        CHECK(fabs((an - ae) - inner) / inner < 0.01);
    }
    free(nz);
    free(eo);
    pc_path_fini(&nzp);
    pc_path_fini(&eop);
}

static double op_area(pc_path_op op)
{
    pc_path p = vh_two_squares(op);
    double a = cov_area(&p, pc_rect_make(0, 0, 100, 100));
    pc_path_fini(&p);
    return a;
}

static void test_boolean_ops_areas(void)
{
    pc_rect r = pc_rect_make(0, 0, 100, 100);
    pc_path p;
    pc_path d;
    pc_path in;
    double want;
    CHECK(fabs(op_area(PC_PATH_COMBINE) - (1600.0 * 2.0 - 400.0)) < 1e-2);
    CHECK(fabs(op_area(PC_PATH_SUBTRACT) - 1200.0) < 1e-2);
    CHECK(fabs(op_area(PC_PATH_INTERSECT) - 400.0) < 1e-2);
    CHECK(fabs(op_area(PC_PATH_EXCLUDE) - 2400.0) < 1e-2);
    /* Order matters: the first subpath is always combined. */
    p = vh_two_squares(PC_PATH_COMBINE);
    p.subpaths[0].op = PC_PATH_SUBTRACT;
    CHECK(fabs(cov_area(&p, r) - 2800.0) < 1e-2);
    pc_path_fini(&p);
    /* Inversion. */
    p = vh_two_squares(PC_PATH_INTERSECT);
    p.inverted = true;
    CHECK(fabs(cov_area(&p, r) - (10000.0 - 400.0)) < 1e-2);
    pc_path_fini(&p);
    /* Donut: circle minus a smaller circle (exact booleans, even with overlapping AA edges). */
    d = vh_ellipse(10.0, 10.0, 80.0, 80.0);
    in = vh_ellipse(30.0, 30.0, 40.0, 40.0);
    vh_push(&d, vh_take0(&in, PC_PATH_SUBTRACT));
    want = PI * (1600.0 - 400.0);
    CHECK(fabs(cov_area(&d, r) - want) / want < 0.002);
    pc_path_fini(&d);
}

static double stroke_area(const pc_path *path, const pc_vec_stroke_style *st)
{
    pc_vec_rasterizer r;
    float *v = NULL;
    size_t n = 0;
    double a;
    if (pc_vec_stroke_rasterizer(path, st, PC_VEC_DEFAULT_TOLERANCE, &r, NULL) != PC_OK) {
        return NAN;
    }
    if (pc_vec_rasterizer_render(&r, pc_rect_make(-50, -50, 250, 250), &v, &n, NULL) != PC_OK) {
        pc_vec_rasterizer_fini(&r);
        return NAN;
    }
    a = vh_area(v, n);
    free(v);
    pc_vec_rasterizer_fini(&r);
    return a;
}

static pc_vec_stroke_style style(double w, pc_line_cap cap, pc_line_join join)
{
    pc_vec_stroke_style s;
    pc_vec_stroke_style_default(&s);
    s.width = w;
    s.cap = cap;
    s.join = join;
    return s;
}

static void test_stroke_widths_caps_and_joins(void)
{
    static const double lp[2][2] = {{20.0, 50.0}, {120.0, 50.0}};
    static const double lj[3][2] = {{20.0, 20.0}, {120.0, 20.0}, {120.0, 120.0}};
    static const double ls[3][2] = {{0.0, 0.0}, {100.0, 5.0}, {0.0, 10.0}};
    pc_path line = vh_poly_path(lp, 2, false);
    pc_path l = vh_poly_path(lj, 3, false);
    pc_path sharp = vh_poly_path(ls, 3, false);
    const double ws[3] = {1.0, 2.5, 10.0};
    pc_vec_stroke_style st;
    double base = 2.0 * 100.0 * 10.0;
    double a100;
    double a4;
    size_t i;
    /* Butt: exactly length x width. */
    for (i = 0; i < 3; i++) {
        st = style(ws[i], PC_LINE_CAP_BUTT, PC_LINE_JOIN_MITER);
        CHECK(fabs(stroke_area(&line, &st) - 100.0 * ws[i]) < 1e-2);
    }
    /* Square adds w/2 at both ends; round adds a disc. */
    st = style(10.0, PC_LINE_CAP_SQUARE, PC_LINE_JOIN_MITER);
    CHECK(fabs(stroke_area(&line, &st) - 110.0 * 10.0) < 1e-2);
    st = style(10.0, PC_LINE_CAP_ROUND, PC_LINE_JOIN_MITER);
    CHECK(fabs(stroke_area(&line, &st) - (1000.0 + PI * 25.0)) < 0.5);
    /* Right-angle polyline: miter fills the full corner square, bevel half, round a quarter disc.
     * Two 100x10 bars share the inner 5x5 quarter; the outer 5x5 corner is the join. */
    st = style(10.0, PC_LINE_CAP_BUTT, PC_LINE_JOIN_MITER);
    CHECK(fabs(stroke_area(&l, &st) - (base - 25.0 + 25.0)) < 1e-2);
    st = style(10.0, PC_LINE_CAP_BUTT, PC_LINE_JOIN_BEVEL);
    CHECK(fabs(stroke_area(&l, &st) - (base - 25.0 + 12.5)) < 1e-2);
    st = style(10.0, PC_LINE_CAP_BUTT, PC_LINE_JOIN_ROUND);
    CHECK(fabs(stroke_area(&l, &st) - (base - 25.0 + PI * 25.0 / 4.0)) < 0.1);
    /* Miter limit: a very sharp angle falls back to bevel. */
    st = style(4.0, PC_LINE_CAP_BUTT, PC_LINE_JOIN_MITER);
    st.miter_limit = 100.0;
    a100 = stroke_area(&sharp, &st);
    st.miter_limit = 4.0;
    a4 = stroke_area(&sharp, &st);
    CHECK(a100 > a4 + 10.0);
    pc_path_fini(&line);
    pc_path_fini(&l);
    pc_path_fini(&sharp);
}

static double alpha_area(const pc_shape_layer *sh)
{
    pc_vec_compiled_shape cs;
    float (*px)[4] = NULL;
    size_t n = 0;
    double a = -0.0;
    size_t i;
    if (pc_vec_compiled_shape_new(sh, PC_VEC_DEFAULT_TOLERANCE, pc_rect_make(0, 0, 150, 150), &cs, NULL) !=
        PC_OK) {
        return NAN;
    }
    if (pc_vec_compiled_shape_render_rgba(&cs, pc_rect_make(0, 0, 150, 150), &px, &n, NULL) == PC_OK) {
        for (i = 0; i < n; i++) {
            a += (double)px[i][3];
        }
    }
    free(px);
    pc_vec_compiled_shape_fini(&cs);
    return a;
}

static void test_closed_rect_stroke_and_alignment(void)
{
    pc_path sq = vh_rect(20.0, 20.0, 100.0, 100.0);
    pc_vec_stroke_style st = style(10.0, PC_LINE_CAP_BUTT, PC_LINE_JOIN_MITER);
    const pc_stroke_align al[3] = {PC_STROKE_ALIGN_INSIDE, PC_STROKE_ALIGN_OUTSIDE, PC_STROKE_ALIGN_CENTER};
    const double want[3] = {100.0 * 100.0 - 80.0 * 80.0, 120.0 * 120.0 - 100.0 * 100.0,
                            110.0 * 110.0 - 90.0 * 90.0};
    size_t i;
    /* Centred miter stroke of a closed square: outer 110^2 minus inner 90^2. */
    CHECK(fabs(stroke_area(&sq, &st) - (110.0 * 110.0 - 90.0 * 90.0)) < 1e-2);
    for (i = 0; i < 3; i++) {
        pc_shape_layer sh;
        memset(&sh, 0, sizeof sh);
        sh.path = vh_clone(&sq);
        sh.has_stroke = true;
        pc_shape_stroke_default(&sh.stroke);
        sh.stroke.width = 10.0f;
        sh.stroke.paint = pc_fill_solid(PC_COLOR_BLACK);
        sh.stroke.align = al[i];
        CHECK(fabs(alpha_area(&sh) - want[i]) < 0.05);
        pc_shape_layer_fini(&sh);
    }
    pc_path_fini(&sq);
}

static float (*shape_rgba(const pc_shape_layer *sh, pc_rect clip))[4]
{
    pc_vec_compiled_shape c;
    float (*rgba)[4] = NULL;
    size_t n = 0;
    if (pc_vec_compiled_shape_new(sh, PC_VEC_DEFAULT_TOLERANCE, clip, &c, NULL) != PC_OK) {
        return NULL;
    }
    if (pc_vec_compiled_shape_render_rgba(&c, clip, &rgba, &n, NULL) != PC_OK) {
        rgba = NULL;
    }
    pc_vec_compiled_shape_fini(&c);
    return rgba;
}

static void test_aligned_dashes_keep_width_units_and_open_paths_stay_centered(void)
{
    pc_rect clip = pc_rect_make(0, 0, 100, 100);
    pc_path sq = vh_rect(10.0, 10.0, 80.0, 80.0);
    static const double lp[2][2] = {{10.0, 50.0}, {90.0, 50.0}};
    pc_path open = vh_poly_path(lp, 2, false);
    const pc_stroke_align al[3] = {PC_STROKE_ALIGN_INSIDE, PC_STROKE_ALIGN_CENTER, PC_STROKE_ALIGN_OUTSIDE};
    const int ys[3] = {11, 9, 7};
    const int other[3] = {7, 6, 11};
    float (*ref)[4] = NULL;
    size_t i;
    for (i = 0; i < 3; i++) {
        pc_shape_layer sh;
        float (*rgba)[4];
        float *dashes = malloc(2 * sizeof *dashes);
        memset(&sh, 0, sizeof sh);
        sh.path = vh_clone(&sq);
        sh.has_stroke = true;
        pc_shape_stroke_default(&sh.stroke);
        sh.stroke.width = 4.0f;
        sh.stroke.align = al[i];
        if (dashes != NULL) {
            dashes[0] = 2.0f;
            dashes[1] = 2.0f;
            sh.stroke.dashes = dashes;
            sh.stroke.ndashes = 2;
        }
        rgba = shape_rgba(&sh, clip);
        CHECK(rgba != NULL);
        if (rgba != NULL) {
            CHECK(rgba[ys[i] * 100 + 13][3] > 0.99f);
            CHECK(rgba[ys[i] * 100 + 21][3] < 0.01f);
            CHECK(rgba[other[i] * 100 + 13][3] < 0.01f);
        }
        free(rgba);
        pc_shape_layer_fini(&sh);
    }
    for (i = 0; i < 3; i++) {
        pc_shape_layer sh;
        float (*rgba)[4];
        memset(&sh, 0, sizeof sh);
        sh.path = vh_clone(&open);
        sh.has_stroke = true;
        pc_shape_stroke_default(&sh.stroke);
        sh.stroke.width = 4.0f;
        sh.stroke.align = al[(i + 1) % 3]; /* centre first */
        sh.stroke.cap = PC_LINE_CAP_ROUND;
        rgba = shape_rgba(&sh, clip);
        CHECK(rgba != NULL);
        if (i == 0) {
            ref = rgba;
        } else {
            CHECK(rgba != NULL && ref != NULL && memcmp(rgba, ref, 100 * 100 * sizeof *rgba) == 0);
            free(rgba);
        }
        pc_shape_layer_fini(&sh);
    }
    free(ref);
    pc_path_fini(&sq);
    pc_path_fini(&open);
}

static void test_dashes_cover_expected_fraction(void)
{
    static const double lp[2][2] = {{0.0, 50.0}, {120.0, 50.0}};
    pc_path line = vh_poly_path(lp, 2, false);
    pc_vec_stroke_style st = style(4.0, PC_LINE_CAP_BUTT, PC_LINE_JOIN_MITER);
    double d84[2] = {8.0, 4.0};
    double d0[2] = {0.0, 12.0};
    float model[2] = {2.0f, 1.0f};
    pc_shape_stroke ss;
    pc_vec_stroke_style s;
    double a;
    double one = PI * 4.0;
    /* 120 px = 10 periods of 12 px, 8 on each. */
    st.dashes = d84;
    st.ndashes = 2;
    CHECK(fabs(stroke_area(&line, &st) - 80.0 * 4.0) < 1e-2);
    /* Photoshop-style dash units (multiples of width) via stroke_style_from. */
    pc_shape_stroke_default(&ss);
    ss.width = 4.0f;
    ss.dashes = model;
    ss.ndashes = 2;
    CHECK(pc_vec_stroke_style_from(&ss, &s, NULL) == PC_OK);
    CHECK(s.ndashes == 2 && s.dashes[0] == 8.0 && s.dashes[1] == 4.0);
    pc_vec_stroke_style_fini(&s);
    /* Round caps on zero-length dashes draw dots. */
    st.cap = PC_LINE_CAP_ROUND;
    st.dashes = d0;
    a = stroke_area(&line, &st);
    CHECK(a > 9.0 * one && a < 11.5 * one);
    pc_path_fini(&line);
}

static pc_shape_layer red_square_blue_stroke(void)
{
    pc_shape_layer sh;
    memset(&sh, 0, sizeof sh);
    sh.path = vh_rect(2.0, 2.0, 10.0, 10.0);
    sh.has_fill = true;
    sh.fill = pc_fill_solid(pc_color_rgb(1.0f, 0.0f, 0.0f));
    sh.has_stroke = true;
    pc_shape_stroke_default(&sh.stroke);
    sh.stroke.width = 2.0f;
    sh.stroke.paint = pc_fill_solid(pc_color_rgb(0.0f, 0.0f, 1.0f));
    return sh;
}

static void pixel_rgba(const pc_surface *s, int32_t x, int32_t y, float out[4])
{
    float px[PC_RASTER_MAX_CHANNELS];
    pc_pixel_format f = pc_surface_format(s);
    pc_surface_read_pixel(s, x, y, px);
    pc_to_rgba(f, px, pc_pixel_format_channels(f), out);
}

static void test_render_shape_formats_and_paint(void)
{
    pc_shape_layer sh = red_square_blue_stroke();
    const pc_color_mode modes[3] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_GRAYSCALE, PC_COLOR_MODE_CMYK};
    size_t si;
    size_t mi;
    pc_surface s;
    for (si = 0; si < PC_SAMPLE_TYPE_COUNT; si++) {
        for (mi = 0; mi < 3; mi++) {
            pc_pixel_format fmt;
            float centre[4];
            float edge[4];
            fmt.mode = modes[mi];
            fmt.sample = (pc_sample_type)si;
            fmt.alpha = true;
            CHECK(pc_vec_render_shape(&sh, fmt, pc_rect_make(0, 0, 20, 20), &s, NULL) == PC_OK);
            CHECK(pc_pixel_format_eq(pc_surface_format(&s), fmt));
            CHECK(pc_rect_eq(pc_surface_content_bounds(&s), pc_rect_make(1, 1, 13, 13)));
            pixel_rgba(&s, 7, 7, centre);
            pixel_rgba(&s, 2, 7, edge);
            CHECK(fabsf(centre[3] - 1.0f) < 1e-3f);
            if (modes[mi] == PC_COLOR_MODE_RGB) {
                CHECK(centre[0] > 0.99f && centre[2] < 0.01f);
                CHECK(edge[2] > 0.99f);
            }
            pc_surface_fini(&s);
        }
    }
    /* Clip: nothing outside. */
    CHECK(pc_vec_render_shape(&sh, PC_PIXEL_FORMAT_RGBA8, pc_rect_make(0, 0, 5, 5), &s, NULL) == PC_OK);
    CHECK(pc_rect_eq(pc_surface_content_bounds(&s), pc_rect_make(1, 1, 5, 5)));
    pc_surface_fini(&s);
    pc_shape_layer_fini(&sh);
}

/* Renders a 100 x 10 rectangle filled with `fill` (taken over) in canvas. */
static float (*grad(pc_fill fill, pc_rect canvas))[4]
{
    pc_shape_layer sh;
    pc_vec_compiled_shape cs;
    float (*px)[4] = NULL;
    size_t n = 0;
    pc_rect c = pc_rect_make(0, 0, canvas.x1 < 100 ? canvas.x1 : 100, canvas.y1 < 10 ? canvas.y1 : 10);
    memset(&sh, 0, sizeof sh);
    sh.path = vh_rect(0.0, 0.0, 100.0, 10.0);
    sh.has_fill = true;
    sh.fill = fill;
    if (pc_vec_compiled_shape_new(&sh, PC_VEC_DEFAULT_TOLERANCE, canvas, &cs, NULL) == PC_OK) {
        CHECK(pc_vec_compiled_shape_render_rgba(&cs, c, &px, &n, NULL) == PC_OK);
        pc_vec_compiled_shape_fini(&cs);
    }
    pc_shape_layer_fini(&sh);
    return px;
}

static pc_fill gfill(const float *mids, size_t nmids, const float (*ops)[2], size_t nops, float ox, float oy,
                     bool reverse_stops)
{
    pc_color_stop stops[2];
    pc_fill f;
    size_t i;
    stops[0].pos = 0.0f;
    stops[0].color = PC_COLOR_BLACK;
    stops[1].pos = 1.0f;
    stops[1].color = PC_COLOR_WHITE;
    if (reverse_stops) {
        pc_color_stop t = stops[0];
        stops[0] = stops[1];
        stops[1] = t;
    }
    memset(&f, 0, sizeof f);
    CHECK(pc_fill_gradient(stops, 2, 0.0f, 1.0f, PC_GRADIENT_LINEAR, false, &f, NULL) == PC_OK);
    if (nmids > 0) {
        f.u.gradient.midpoints = malloc(nmids * sizeof(float));
        if (f.u.gradient.midpoints != NULL) {
            memcpy(f.u.gradient.midpoints, mids, nmids * sizeof(float));
            f.u.gradient.nmidpoints = nmids;
        }
    }
    if (nops > 0) {
        f.u.gradient.opacity_stops = malloc(nops * sizeof *f.u.gradient.opacity_stops);
        if (f.u.gradient.opacity_stops != NULL) {
            for (i = 0; i < nops; i++) {
                f.u.gradient.opacity_stops[i].pos = ops[i][0];
                f.u.gradient.opacity_stops[i].opacity = ops[i][1];
            }
            f.u.gradient.nopacity_stops = nops;
        }
    }
    f.u.gradient.offset[0] = ox;
    f.u.gradient.offset[1] = oy;
    return f;
}

static void test_gradient_fill_spans_bounds(void)
{
    float (*px)[4] = grad(gfill(NULL, 0, NULL, 0, 0.0f, 0.0f, false), pc_rect_make(0, 0, 100, 10));
    if (px != NULL) {
        CHECK(px[0][0] < 0.02f && px[99][0] > 0.98f && fabsf(px[50][0] - 0.505f) < 0.02f);
    }
    free(px);
}

static void test_gradient_midpoints_opacity_stops_offset_and_sorted_stops(void)
{
    /* Linear 0 deg over the path's 100 x 10 bounds: pixel x sits at t = x / 100. */
    pc_rect c = pc_rect_make(0, 0, 100, 10);
    static const float mid[1] = {0.25f};
    static const float ops[2][2] = {{0.0f, 1.0f}, {1.0f, 0.5f}};
    float (*px)[4];
    /* Midpoint 0.25: the colour is half-way at t = 0.25 (pixel 25). */
    px = grad(gfill(mid, 1, NULL, 0, 0.0f, 0.0f, false), c);
    CHECK(px != NULL && fabsf(px[25][0] - 0.5f) < 1e-3f);
    free(px);
    /* Opacity stop ramp 1 -> 0.5: alpha at t = 0.25 is 0.875. */
    px = grad(gfill(NULL, 0, ops, 2, 0.0f, 0.0f, false), c);
    CHECK(px != NULL && fabsf(px[25][3] - 0.875f) < 1e-3f);
    free(px);
    /* Centre offset half a frame right: t = 0 moves to x = 50, so pixel 75 sits at t = 0.25. */
    px = grad(gfill(NULL, 0, NULL, 0, 0.5f, 0.0f, false), c);
    CHECK(px != NULL && px[25][0] < 0.01f && fabsf(px[75][0] - 0.25f) < 1e-3f);
    free(px);
    /* Unsorted stops are sorted by location: black still left, white right. */
    px = grad(gfill(NULL, 0, NULL, 0, 0.0f, 0.0f, true), c);
    CHECK(px != NULL && px[1][0] < 0.02f && px[98][0] > 0.97f);
    free(px);
}

static void test_gradient_without_align_with_layer_uses_the_canvas_frame(void)
{
    pc_rect canvas = pc_rect_make(0, 0, 200, 10);
    pc_fill f = gfill(NULL, 0, NULL, 0, 0.0f, 0.0f, false);
    float (*px)[4];
    /* Aligned: the 100-px path bounds are the frame, pixel 55 sits at t = 0.55. */
    px = grad(f, canvas);
    CHECK(px != NULL && fabsf(px[55][0] - 0.55f) < 1e-3f);
    free(px);
    /* Not aligned: the 200-px canvas is the frame, the same pixel sits at t = 0.275. */
    f = gfill(NULL, 0, NULL, 0, 0.0f, 0.0f, false);
    f.u.gradient.align = false;
    px = grad(f, canvas);
    CHECK(px != NULL && fabsf(px[55][0] - 0.275f) < 1e-3f);
    free(px);
}

static pc_fill dither_fill(bool dither)
{
    pc_fill f = gfill(NULL, 0, NULL, 0, 0.1f, -0.1f, false);
    f.u.gradient.angle = 30.0f;
    f.u.gradient.scale = 1.3f;
    f.u.gradient.style = PC_GRADIENT_RADIAL;
    f.u.gradient.dither = dither;
    return f;
}

static void test_gradient_dither_is_deterministic_and_one_level(void)
{
    pc_rect c = pc_rect_make(0, 0, 100, 10);
    float (*a)[4] = grad(dither_fill(true), c);
    float (*b)[4] = grad(dither_fill(true), c);
    float (*plain)[4] = grad(dither_fill(false), c);
    float max = 0.0f;
    size_t changed = 0;
    size_t i;
    if (a != NULL && b != NULL && plain != NULL) {
        CHECK(memcmp(a, b, 1000 * sizeof *a) == 0); /* dither must be deterministic */
        for (i = 0; i < 1000; i++) {
            float d = fabsf(a[i][0] - plain[i][0]);
            max = d > max ? d : max;
            changed += d > 1e-6f ? 1u : 0u;
        }
        CHECK(changed > 0 && max <= 1.0f / 255.0f + 1e-6f);
    } else {
        CHECK(false);
    }
    free(a);
    free(b);
    free(plain);
}

static void test_coverage_surface_and_vector_mask_values(void)
{
    pc_path p = vh_rect(10.0, 10.0, 20.0, 20.0);
    pc_path inv = vh_clone(&p);
    pc_path empty;
    pc_vec_rasterizer r;
    pc_surface s;
    pc_vector_mask vm;
    float *v = NULL;
    size_t n = 0;
    size_t i;
    CHECK(pc_vec_fill_rasterizer(&p, PC_VEC_DEFAULT_TOLERANCE, &r, NULL) == PC_OK);
    CHECK(pc_vec_coverage_surface(&r, PC_PIXEL_FORMAT_GRAY8, pc_rect_make(0, 0, 300, 300), &s, NULL) ==
          PC_OK);
    CHECK(pc_rect_eq(pc_surface_content_bounds(&s), pc_rect_make(10, 10, 30, 30)));
    CHECK_EQ_UINT(pc_surface_tile_count(&s), 1);
    pc_surface_fini(&s);
    pc_vec_rasterizer_fini(&r);
    inv.inverted = true;
    CHECK(pc_vec_fill_rasterizer(&inv, PC_VEC_DEFAULT_TOLERANCE, &r, NULL) == PC_OK);
    CHECK(pc_vec_coverage_surface(&r, PC_PIXEL_FORMAT_GRAY8, pc_rect_make(0, 0, 40, 40), &s, NULL) == PC_OK);
    CHECK(pc_surface_sample_channel(&s, 0, 0, 0) == 1.0f);
    CHECK(pc_surface_sample_channel(&s, 15, 15, 0) == 0.0f);
    pc_surface_fini(&s);
    pc_vec_rasterizer_fini(&r);
    pc_path_fini(&inv);
    pc_vector_mask_init(&vm, &p);
    vm.density = 0.25f;
    CHECK(pc_vec_mask_values(&vm, pc_rect_make(0, 0, 40, 40), &v, &n, NULL) == PC_OK);
    CHECK(fabsf(v[0] - 0.75f) < 1e-6f && fabsf(v[15 * 40 + 15] - 1.0f) < 1e-6f);
    free(v);
    vm.enabled = false;
    CHECK(pc_vec_mask_values(&vm, pc_rect_make(0, 0, 4, 4), &v, &n, NULL) == PC_OK);
    for (i = 0; i < n; i++) {
        CHECK(v[i] == 1.0f);
    }
    free(v);
    pc_vector_mask_fini(&vm);
    /* Empty vector masks reveal all (hide all when inverted). */
    memset(&empty, 0, sizeof empty);
    pc_vector_mask_init(&vm, &empty);
    CHECK(pc_vec_mask_values(&vm, pc_rect_make(0, 0, 4, 4), &v, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 16);
    for (i = 0; i < n; i++) {
        CHECK(v[i] == 1.0f);
    }
    free(v);
    vm.path.inverted = true;
    CHECK(pc_vec_mask_values(&vm, pc_rect_make(0, 0, 4, 4), &v, &n, NULL) == PC_OK);
    for (i = 0; i < n; i++) {
        CHECK(v[i] == 0.0f);
    }
    free(v);
    pc_vector_mask_fini(&vm);
}

static bool same_values(const pc_vec_compiled_mask *c, pc_rect r)
{
    float *a = NULL;
    float *b = NULL;
    size_t na = 0;
    size_t nb = 0;
    bool ok = pc_vec_compiled_mask_render_sequential(c, r, &a, &na, NULL) == PC_OK &&
              pc_vec_compiled_mask_render(c, r, &b, &nb, NULL) == PC_OK && na == nb &&
              (na == 0 || memcmp(a, b, na * sizeof *a) == 0);
    free(a);
    free(b);
    return ok;
}

static void test_sequential_vector_masks_match_parallel_rendering(void)
{
    pc_rect rect = pc_rect_make(-3, -7, 510, 250);
    pc_path path = vh_ellipse(10.25, 8.75, 470.0, 220.0);
    pc_path hole = vh_rect(200.5, 90.25, 45.0, 50.0);
    const pc_rect empties[3] = {PC_RECT_EMPTY, {1, 1, 1, 10}, {1, 1, 10, 1}};
    const float densities[3] = {0.0f, 0.25f, 1.0f};
    pc_vec_compiled_mask c;
    pc_vector_mask m;
    pc_path tmp;
    size_t i;
    size_t d;
    vh_push(&path, vh_take0(&hole, PC_PATH_SUBTRACT));
    for (i = 0; i < 2; i++) {
        for (d = 0; d < 3; d++) {
            tmp = vh_clone(&path);
            tmp.inverted = i == 1;
            pc_vector_mask_init(&m, &tmp);
            m.density = densities[d];
            CHECK(pc_vec_compiled_mask_new(&m, &c, NULL) == PC_OK);
            CHECK(same_values(&c, rect));
            pc_vec_compiled_mask_fini(&c);
            pc_vector_mask_fini(&m);
        }
    }
    tmp = vh_clone(&path);
    pc_vector_mask_init(&m, &tmp);
    CHECK(pc_vec_compiled_mask_new(&m, &c, NULL) == PC_OK);
    for (i = 0; i < 3; i++) {
        CHECK(same_values(&c, empties[i]));
    }
    pc_vec_compiled_mask_fini(&c);
    pc_vector_mask_fini(&m);
    for (i = 0; i < 2; i++) {
        memset(&tmp, 0, sizeof tmp);
        pc_vector_mask_init(&m, &tmp);
        m.path.inverted = i == 1;
        CHECK(pc_vec_compiled_mask_new(&m, &c, NULL) == PC_OK);
        CHECK(same_values(&c, rect));
        pc_vec_compiled_mask_fini(&c);
        pc_vector_mask_fini(&m);
    }
    pc_vector_mask_init(&m, &path);
    m.enabled = false;
    CHECK(pc_vec_compiled_mask_new(&m, &c, NULL) == PC_OK);
    CHECK(same_values(&c, rect));
    CHECK(same_values(&c, PC_RECT_EMPTY));
    pc_vec_compiled_mask_fini(&c);
    pc_vector_mask_fini(&m);
}

/* Content bounds of a filled shape rendered as a layer (every depth) and as a selection (every
 * depth); they must all equal want. */
static void check_rendered_bounds(const pc_path *path, pc_rect want, const char *what)
{
    pc_rect canvas = pc_rect_make(0, 0, 1920, 1080);
    pc_shape_layer sh;
    pc_vec_rasterizer r;
    size_t si;
    memset(&sh, 0, sizeof sh);
    sh.path = vh_clone(path);
    sh.has_fill = true;
    sh.fill = pc_fill_solid(pc_color_rgb(0.0f, 0.0f, 1.0f));
    CHECK(pc_vec_fill_rasterizer(path, PC_VEC_DEFAULT_TOLERANCE, &r, NULL) == PC_OK);
    for (si = 0; si < PC_SAMPLE_TYPE_COUNT; si++) {
        pc_pixel_format fmt;
        pc_surface s;
        fmt.mode = PC_COLOR_MODE_RGB;
        fmt.sample = (pc_sample_type)si;
        fmt.alpha = true;
        CHECK(pc_vec_render_shape(&sh, fmt, canvas, &s, NULL) == PC_OK);
        if (!pc_rect_eq(pc_surface_content_bounds(&s), want)) {
            pc_rect b = pc_surface_content_bounds(&s);
            fprintf(stderr, "%s: layer sample %zu: %d,%d,%d,%d\n", what, si, b.x0, b.y0, b.x1, b.y1);
            CHECK(false);
        }
        pc_surface_fini(&s);
        fmt.mode = PC_COLOR_MODE_GRAYSCALE;
        fmt.alpha = false;
        CHECK(pc_vec_coverage_surface(&r, fmt, canvas, &s, NULL) == PC_OK);
        if (!pc_rect_eq(pc_surface_content_bounds(&s), want)) {
            pc_rect b = pc_surface_content_bounds(&s);
            fprintf(stderr, "%s: selection sample %zu: %d,%d,%d,%d\n", what, si, b.x0, b.y0, b.x1, b.y1);
            CHECK(false);
        }
        pc_surface_fini(&s);
    }
    pc_vec_rasterizer_fini(&r);
    pc_shape_layer_fini(&sh);
}

static void test_curved_shape_bounds_are_exact(void)
{
    /* f32 round-off left ~1e-7 coverage just right of curved edges, so curved shapes reported
     * bounds one pixel too wide. Rectangles never showed it. */
    pc_subpath blob;
    pc_knot kn[4];
    pc_path p;
    static const double radii[4] = {20.0, 20.0, 20.0, 20.0};
    memset(&blob, 0, sizeof blob);
    /* An asymmetric Bezier blob whose extremes lie on its anchors (axis-parallel handles). */
    kn[0] = pc_knot_smooth(pc_point_make(60.0, 0.0), pc_point_make(30.0, 0.0), pc_point_make(80.0, 0.0));
    kn[1] =
        pc_knot_smooth(pc_point_make(100.0, 45.0), pc_point_make(100.0, 20.0), pc_point_make(100.0, 70.0));
    kn[2] =
        pc_knot_smooth(pc_point_make(40.0, 100.0), pc_point_make(75.0, 100.0), pc_point_make(15.0, 100.0));
    kn[3] = pc_knot_smooth(pc_point_make(0.0, 55.0), pc_point_make(0.0, 85.0), pc_point_make(0.0, 25.0));
    blob.closed = true;
    blob.knots = malloc(sizeof kn);
    if (blob.knots != NULL) {
        memcpy(blob.knots, kn, sizeof kn);
        blob.nknots = 4;
    }
    p = vh_ellipse(0.0, 0.0, 100.0, 100.0);
    check_rendered_bounds(&p, pc_rect_make(0, 0, 100, 100), "ellipse");
    pc_path_fini(&p);
    p = vh_ellipse(660.0, 240.0, 600.0, 600.0);
    check_rendered_bounds(&p, pc_rect_make(660, 240, 1260, 840), "ellipse 600");
    pc_path_fini(&p);
    p = vh_ellipse(10.5, 10.0, 100.0, 100.0);
    check_rendered_bounds(&p, pc_rect_make(10, 10, 111, 110), "ellipse at x.5");
    pc_path_fini(&p);
    memset(&p, 0, sizeof p);
    CHECK(pc_vec_shape_rounded_rect(0.0, 0.0, 100.0, 100.0, radii, &p, NULL) == PC_OK);
    check_rendered_bounds(&p, pc_rect_make(0, 0, 100, 100), "rounded rect");
    pc_path_fini(&p);
    /* Outer vertices at x = 50 +- 50 cos 18 deg (2.45, 97.55), lowest at y = 50 + 50 sin 54 deg
     * (90.45). */
    memset(&p, 0, sizeof p);
    CHECK(pc_vec_shape_polygon(0.0, 0.0, 100.0, 100.0, 5, 0.5, &p, NULL) == PC_OK);
    check_rendered_bounds(&p, pc_rect_make(2, 0, 98, 91), "star");
    pc_path_fini(&p);
    p = vh_path(&blob, 1);
    check_rendered_bounds(&p, pc_rect_make(0, 0, 100, 100), "custom path");
    pc_path_fini(&p);
    p = vh_rect(0.0, 0.0, 100.0, 100.0);
    check_rendered_bounds(&p, pc_rect_make(0, 0, 100, 100), "rect");
    pc_path_fini(&p);
}

static void test_thin_antialiased_edges_still_count_toward_bounds(void)
{
    /* A quarter-pixel overhang and a 1-px bar straddling two columns are real coverage. */
    pc_path p = vh_rect(0.0, 0.0, 100.25, 100.0);
    float *c;
    check_rendered_bounds(&p, pc_rect_make(0, 0, 101, 100), "quarter overhang");
    pc_path_fini(&p);
    p = vh_rect(10.5, 0.0, 1.0, 50.0);
    check_rendered_bounds(&p, pc_rect_make(10, 0, 12, 50), "1-px bar");
    pc_path_fini(&p);
    p = vh_rect(0.0, 0.0, 100.01, 10.0);
    check_rendered_bounds(&p, pc_rect_make(0, 0, 101, 10), "1% sliver");
    pc_path_fini(&p);
    /* At 32-bit even a 1e-4 sliver is kept: only round-off is dropped. */
    p = vh_rect(0.0, 0.0, 100.0001, 10.0);
    c = cov(&p, pc_rect_make(0, 0, 102, 10));
    CHECK(c != NULL && c[100] > 5e-5f && c[101] == 0.0f);
    free(c);
    pc_path_fini(&p);
}

static double iou(const float *a, const float *b, size_t n)
{
    double in = 0.0;
    double un = 0.0;
    size_t i;
    for (i = 0; i < n; i++) {
        in += (double)fminf(a[i], b[i]);
        un += (double)fmaxf(a[i], b[i]);
    }
    return in / fmax(un, 1e-9);
}

static void test_trace_roundtrip_iou(void)
{
    pc_rect rect = pc_rect_make(0, 0, 160, 120);
    pc_path src = vh_ellipse(10.0, 10.0, 90.0, 90.0);
    pc_path in = vh_ellipse(35.0, 35.0, 40.0, 40.0);
    pc_path r = vh_rect(110.0, 20.0, 40.0, 80.0);
    const double tols[3] = {0.5, 1.0, 2.0};
    float *mask;
    float *hard;
    size_t n = 160 * 120;
    size_t t;
    size_t i;
    vh_push(&src, vh_take0(&in, PC_PATH_SUBTRACT));
    vh_push(&src, vh_take0(&r, PC_PATH_COMBINE));
    mask = cov(&src, rect);
    hard = malloc(n * sizeof *hard);
    if (mask == NULL || hard == NULL) {
        CHECK(false);
        free(mask);
        free(hard);
        pc_path_fini(&src);
        return;
    }
    for (t = 0; t < 3; t++) {
        pc_path traced;
        float *back;
        size_t knots = 0;
        CHECK(pc_vec_trace_mask(mask, n, rect, 0.0f, tols[t], &traced, NULL) == PC_OK);
        CHECK_EQ_UINT(traced.nsubpaths, 3);
        back = cov(&traced, rect);
        CHECK(back != NULL && iou(mask, back, n) > 0.98);
        for (i = 0; i < traced.nsubpaths; i++) {
            knots += traced.subpaths[i].nknots;
        }
        CHECK(knots < 60);
        free(back);
        pc_path_fini(&traced);
    }
    /* Hard-edged (binary) mask. */
    for (i = 0; i < n; i++) {
        hard[i] = mask[i] >= 0.5f ? 1.0f : 0.0f;
    }
    {
        pc_path traced;
        float *back;
        CHECK(pc_vec_trace_mask(hard, n, rect, 0.0f, 1.0, &traced, NULL) == PC_OK);
        back = cov(&traced, rect);
        CHECK(back != NULL && iou(hard, back, n) > 0.98);
        free(back);
        pc_path_fini(&traced);
    }
    free(mask);
    free(hard);
    pc_path_fini(&src);
}

static void test_joined_subpaths_fill_as_one_component(void)
{
    /* An outer square and an inner one wound the other way: as one component (PSD operation -1
     * after the first record) the inner one is a hole; as separate "combine" shapes it isn't. */
    static const double outer[4][2] = {{2.0, 2.0}, {18.0, 2.0}, {18.0, 18.0}, {2.0, 18.0}};
    static const double inner[4][2] = {{6.0, 6.0}, {6.0, 14.0}, {14.0, 14.0}, {14.0, 6.0}};
    static const double cutp[4][2] = {{0.0, 0.0}, {20.0, 0.0}, {20.0, 4.0}, {0.0, 4.0}};
    static const double lonep[3][2] = {{2.0, 2.0}, {8.0, 2.0}, {8.0, 8.0}};
    pc_rect r = pc_rect_make(0, 0, 20, 20);
    pc_subpath s[3];
    pc_path joined;
    pc_path separate;
    pc_path p;
    pc_path lone;
    pc_index_range *comps = NULL;
    size_t nc = 0;
    float *v;
    s[0] = vh_polygon(outer, 4, true);
    s[1] = vh_polygon(inner, 4, true);
    s[1].op = PC_PATH_JOIN;
    joined = vh_path(s, 2);
    v = cov(&joined, r);
    CHECK(v != NULL && v[10 * 20 + 10] == 0.0f); /* hole */
    CHECK(v != NULL && v[4 * 20 + 4] == 1.0f);
    CHECK(v != NULL && fabs(vh_area(v, 400) - (256.0 - 64.0)) < 1e-3);
    free(v);
    CHECK(pc_path_components(&joined, &comps, &nc, NULL) == PC_OK);
    CHECK(nc == 1 && comps[0].start == 0 && comps[0].end == 2);
    free(comps);
    separate = vh_clone(&joined);
    separate.subpaths[1].op = PC_PATH_COMBINE;
    v = cov(&separate, r);
    CHECK(v != NULL && v[10 * 20 + 10] == 1.0f);
    free(v);
    CHECK(pc_path_components(&separate, &comps, &nc, NULL) == PC_OK);
    CHECK(nc == 2 && comps[0].end == 1 && comps[1].start == 1 && comps[1].end == 2);
    free(comps);
    /* A component after a joined one starts afresh with its own operation. */
    p = vh_clone(&joined);
    s[2] = vh_polygon(cutp, 4, true);
    s[2].op = PC_PATH_SUBTRACT;
    vh_push(&p, s[2]);
    CHECK(pc_path_components(&p, &comps, &nc, NULL) == PC_OK);
    CHECK(nc == 2 && comps[0].end == 2 && comps[1].start == 2 && comps[1].end == 3);
    free(comps);
    v = cov(&p, r);
    CHECK(v != NULL && v[3 * 20 + 10] == 0.0f);
    CHECK(v != NULL && v[5 * 20 + 10] == 1.0f);
    free(v);
    /* A lone joined subpath acts as the first component. */
    lone = vh_poly_path(lonep, 3, true);
    lone.subpaths[0].op = PC_PATH_JOIN;
    v = cov(&lone, r);
    CHECK(v != NULL && v[3 * 20 + 6] == 1.0f);
    free(v);
    pc_path_fini(&joined);
    pc_path_fini(&separate);
    pc_path_fini(&p);
    pc_path_fini(&lone);
}

static double now_ms(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Performance on a 6016^2 canvas (the reference's ignored perf_6016). */
static void test_perf_6016(void)
{
    pc_rect canvas = pc_rect_make(0, 0, 6016, 6016);
    pc_subpath sp;
    pc_path path;
    pc_path e;
    pc_shape_layer sh;
    pc_vec_rasterizer r;
    pc_vec_stroke_style st;
    pc_surface s;
    float *c = NULL;
    size_t n = 0;
    size_t i;
    double t;
    double fill_ms;
    double stroke_ms;
    double shape_ms;
    const char *slow = getenv("PHOTOCRAFT_SLOW_TESTS");
    if (slow == NULL || strcmp(slow, "1") != 0) {
        return;
    }
    memset(&sp, 0, sizeof sp);
    sp.closed = true;
    sp.knots = calloc(2000, sizeof *sp.knots);
    if (sp.knots == NULL) {
        return;
    }
    sp.nknots = 2000;
    for (i = 0; i < 2000; i++) {
        double a = (double)i / 2000.0 * 2.0 * PI;
        double rr = 2400.0 + 500.0 * sin(a * 37.0) + 200.0 * cos(a * 101.0);
        double x = 3008.0 + rr * cos(a);
        double y = 3008.0 + rr * sin(a);
        sp.knots[i] = pc_knot_smooth(pc_point_make(x, y), pc_point_make(x - 6.0 * sin(a), y + 6.0 * cos(a)),
                                     pc_point_make(x + 6.0 * sin(a), y - 6.0 * cos(a)));
    }
    path = vh_path(&sp, 1);
    e = vh_ellipse(2000.0, 2000.0, 2016.0, 2016.0);
    vh_push(&path, vh_take0(&e, PC_PATH_EXCLUDE));
    memset(&sh, 0, sizeof sh);
    sh.path = vh_clone(&path);
    sh.has_fill = true;
    sh.fill = pc_fill_solid(pc_color_rgb(0.2f, 0.5f, 0.9f));
    sh.has_stroke = true;
    pc_shape_stroke_default(&sh.stroke);
    sh.stroke.width = 12.0f;
    sh.stroke.join = PC_LINE_JOIN_ROUND;
    sh.stroke.dashes = malloc(2 * sizeof(float));
    if (sh.stroke.dashes != NULL) {
        sh.stroke.dashes[0] = 3.0f;
        sh.stroke.dashes[1] = 2.0f;
        sh.stroke.ndashes = 2;
    }
    t = now_ms();
    CHECK(pc_vec_fill_rasterizer(&path, PC_VEC_DEFAULT_TOLERANCE, &r, NULL) == PC_OK);
    CHECK(pc_vec_rasterizer_render(&r, canvas, &c, &n, NULL) == PC_OK);
    fill_ms = now_ms() - t;
    free(c);
    pc_vec_rasterizer_fini(&r);
    CHECK(pc_vec_stroke_style_from(&sh.stroke, &st, NULL) == PC_OK);
    t = now_ms();
    CHECK(pc_vec_stroke_rasterizer(&path, &st, PC_VEC_DEFAULT_TOLERANCE, &r, NULL) == PC_OK);
    CHECK(pc_vec_rasterizer_render(&r, canvas, &c, &n, NULL) == PC_OK);
    stroke_ms = now_ms() - t;
    free(c);
    pc_vec_rasterizer_fini(&r);
    pc_vec_stroke_style_fini(&st);
    t = now_ms();
    CHECK(pc_vec_render_shape(&sh, PC_PIXEL_FORMAT_RGBA8, canvas, &s, NULL) == PC_OK);
    shape_ms = now_ms() - t;
    (void)fprintf(
        stderr, "6016^2: fill coverage %.0f ms, stroke coverage %.0f ms, shape->RGBA8 %.0f ms (%zu tiles)\n",
        fill_ms, stroke_ms, shape_ms, pc_surface_tile_count(&s));
    CHECK(pc_surface_tile_count(&s) > 0);
    pc_surface_fini(&s);
    pc_shape_layer_fini(&sh);
    pc_path_fini(&path);
}

int main(int argc, char **argv)
{
    RUN(test_circle_area_matches_analytic);
    RUN(test_rect_area_exact_and_rotated);
    RUN(test_coverage_is_tile_independent);
    RUN(test_even_odd_vs_non_zero);
    RUN(test_boolean_ops_areas);
    RUN(test_stroke_widths_caps_and_joins);
    RUN(test_closed_rect_stroke_and_alignment);
    RUN(test_aligned_dashes_keep_width_units_and_open_paths_stay_centered);
    RUN(test_dashes_cover_expected_fraction);
    RUN(test_render_shape_formats_and_paint);
    RUN(test_gradient_fill_spans_bounds);
    RUN(test_gradient_midpoints_opacity_stops_offset_and_sorted_stops);
    RUN(test_gradient_without_align_with_layer_uses_the_canvas_frame);
    RUN(test_gradient_dither_is_deterministic_and_one_level);
    RUN(test_coverage_surface_and_vector_mask_values);
    RUN(test_sequential_vector_masks_match_parallel_rendering);
    RUN(test_curved_shape_bounds_are_exact);
    RUN(test_thin_antialiased_edges_still_count_toward_bounds);
    RUN(test_trace_roundtrip_iou);
    RUN(test_joined_subpaths_fill_as_one_component);
    RUN(test_perf_6016);
    return pc_t_finish();
}
