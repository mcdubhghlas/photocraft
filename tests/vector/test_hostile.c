/* Never-crash tests for src/vector (AGENTS.md "Never crash"): non-finite coordinates, empty and
 * degenerate paths, absurd stroke widths / tolerances / dash patterns, huge render requests and
 * hostile trace inputs must give a result or an error, with bounded work and memory. Run under
 * ASan/UBSan by `make sanitize`. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "raster/pixel.h"
#include "vh.h"

static const double NASTY[] = {0.0,   -0.0,   1.0,      -1.0,      1e-300, -1e-300,
                               1e300, -1e300, INFINITY, -INFINITY, NAN,    3e9};
#define NNASTY (sizeof NASTY / sizeof NASTY[0])

/* A path whose coordinates mix finite and hostile values (seeded). */
static pc_path nasty_path(vh_rng *rng)
{
    pc_path p;
    size_t s;
    size_t nsub = 1 + (size_t)vh_below(rng, 3);
    memset(&p, 0, sizeof p);
    for (s = 0; s < nsub; s++) {
        pc_subpath sp;
        size_t k;
        size_t nk = (size_t)vh_below(rng, 6);
        memset(&sp, 0, sizeof sp);
        sp.closed = vh_below(rng, 2) == 0;
        sp.op = (pc_path_op)vh_below(rng, 5);
        sp.knots = nk > 0 ? calloc(nk, sizeof *sp.knots) : NULL;
        sp.nknots = sp.knots != NULL ? nk : 0;
        for (k = 0; k < sp.nknots; k++) {
            double v[6];
            int i;
            for (i = 0; i < 6; i++) {
                v[i] = vh_below(rng, 3) == 0 ? NASTY[vh_below(rng, NNASTY)] : vh_f(rng) * 80.0 - 10.0;
            }
            sp.knots[k].anchor = pc_point_make(v[0], v[1]);
            sp.knots[k].in_ctrl = pc_point_make(v[2], v[3]);
            sp.knots[k].out_ctrl = pc_point_make(v[4], v[5]);
            sp.knots[k].smooth = vh_below(rng, 2) == 0;
        }
        vh_push(&p, sp);
    }
    p.fill_rule = vh_below(rng, 2) == 0 ? PC_FILL_RULE_NONZERO : PC_FILL_RULE_EVENODD;
    p.inverted = vh_below(rng, 4) == 0;
    return p;
}

static void exercise_path(const pc_path *p, double tol)
{
    pc_vec_polyline *lines = NULL;
    size_t nl = 0;
    pc_vec_rasterizer r;
    float *c = NULL;
    size_t n = 0;
    pc_rect b;
    pc_status st = pc_vec_flatten_path(p, tol, &lines, &nl, NULL);
    CHECK(st == PC_OK || st == PC_ERR_LIMIT);
    pc_vec_polylines_free(lines, nl);
    st = pc_vec_fill_rasterizer(p, tol, &r, NULL);
    CHECK(st == PC_OK || st == PC_ERR_LIMIT);
    if (st == PC_OK) {
        (void)pc_vec_rasterizer_pixel_bounds(&r, &b);
        CHECK(pc_vec_rasterizer_render(&r, pc_rect_make(-3, -3, 20, 9), &c, &n, NULL) == PC_OK);
        free(c);
        pc_vec_rasterizer_fini(&r);
    }
}

static void test_nonfinite_coordinates_everywhere(void)
{
    vh_rng rng = {0xbad5eedu};
    const double tols[5] = {0.01, 1.0, 0.0, NAN, -1.0};
    size_t i;
    for (i = 0; i < 120; i++) {
        pc_path p = nasty_path(&rng);
        pc_vec_stroke_style st;
        pc_vec_rasterizer r;
        pc_shape_layer sh;
        pc_vec_hit hit;
        pc_status s;
        exercise_path(&p, tols[i % 5]);
        pc_vec_stroke_style_default(&st);
        st.width = 1.0 + (double)(i % 7);
        st.cap = (pc_line_cap)(i % 3);
        st.join = (pc_line_join)((i / 3) % 3);
        s = pc_vec_stroke_rasterizer(&p, &st, 0.05, &r, NULL);
        CHECK(s == PC_OK || s == PC_ERR_LIMIT);
        if (s == PC_OK) {
            float *c = NULL;
            size_t n = 0;
            CHECK(pc_vec_rasterizer_render(&r, pc_rect_make(-5, -5, 20, 5), &c, &n, NULL) == PC_OK);
            free(c);
            pc_vec_rasterizer_fini(&r);
        }
        (void)pc_vec_hit_test(&p, pc_point_make(10.0, 10.0), 5.0, NULL, 0, &hit);
        memset(&sh, 0, sizeof sh);
        sh.path = p;
        sh.has_fill = true;
        sh.fill = pc_fill_solid(pc_color_rgb(0.5f, 0.2f, 0.1f));
        sh.has_stroke = i % 2 == 0;
        pc_shape_stroke_default(&sh.stroke);
        sh.stroke.align = (pc_stroke_align)(i % 3);
        {
            pc_surface surf;
            s = pc_vec_render_shape(&sh, PC_PIXEL_FORMAT_RGBA8, pc_rect_make(0, 0, 24, 10), &surf, NULL);
            CHECK(s == PC_OK || s == PC_ERR_LIMIT);
            if (s == PC_OK) {
                pc_surface_fini(&surf);
            }
        }
        pc_shape_layer_fini(&sh);
    }
}

static void test_empty_and_degenerate_paths(void)
{
    pc_path empty;
    pc_path p;
    pc_subpath sp;
    pc_vec_rasterizer r;
    pc_vec_stroke_style st;
    pc_vec_compiled_mask m;
    pc_vector_mask vm;
    float *c = NULL;
    size_t n = 0;
    pc_rect b;
    size_t i;
    memset(&empty, 0, sizeof empty);
    exercise_path(&empty, 0.01);
    CHECK(pc_vec_fill_rasterizer(&empty, 0.01, &r, NULL) == PC_OK);
    CHECK(!pc_vec_rasterizer_pixel_bounds(&r, &b));
    pc_vec_rasterizer_fini(&r);
    /* Subpaths without knots, with one knot, with coincident knots and handles. */
    memset(&sp, 0, sizeof sp);
    p = vh_path(&sp, 1);
    exercise_path(&p, 0.01);
    pc_path_fini(&p);
    for (n = 1; n < 5; n++) {
        double pts[4][2] = {{7.0, 7.0}, {7.0, 7.0}, {7.0, 7.0}, {7.0, 7.0}};
        for (i = 0; i < 2; i++) {
            pc_vec_stroke_style_default(&st);
            p = vh_poly_path((const double (*)[2])pts, n, i == 0);
            exercise_path(&p, 0.01);
            st.cap = PC_LINE_CAP_ROUND;
            CHECK(pc_vec_stroke_rasterizer(&p, &st, 0.01, &r, NULL) == PC_OK);
            pc_vec_rasterizer_fini(&r);
            pc_path_fini(&p);
        }
    }
    n = 0;
    /* Zero-size shapes. */
    p = vh_ellipse(5.0, 5.0, 0.0, 0.0);
    exercise_path(&p, 0.01);
    pc_path_fini(&p);
    /* Empty masks and mask compile of empty paths. */
    memset(&empty, 0, sizeof empty);
    pc_vector_mask_init(&vm, &empty);
    CHECK(pc_vec_compiled_mask_new(&vm, &m, NULL) == PC_OK);
    CHECK(pc_vec_compiled_mask_render(&m, PC_RECT_EMPTY, &c, &n, NULL) == PC_OK);
    CHECK(c == NULL && n == 0);
    pc_vec_compiled_mask_fini(&m);
    pc_vector_mask_fini(&vm);
    /* Rasteriser components with polygons of 0 and 1 points. */
    {
        double one[1][2] = {{1.0, 1.0}};
        pc_vec_poly polys[2];
        polys[0].pts = NULL;
        polys[0].len = 0;
        polys[1].pts = one;
        polys[1].len = 1;
        pc_vec_rasterizer_init(&r, false);
        CHECK(pc_vec_rasterizer_add_component(&r, polys, 2, PC_PATH_SUBTRACT, PC_FILL_RULE_EVENODD, NULL) ==
              PC_OK);
        CHECK(pc_vec_rasterizer_add_component(&r, NULL, 0, PC_PATH_INTERSECT, PC_FILL_RULE_NONZERO, NULL) ==
              PC_OK);
        CHECK(pc_vec_rasterizer_render(&r, pc_rect_make(0, 0, 4, 4), &c, &n, NULL) == PC_OK);
        free(c);
        pc_vec_rasterizer_fini(&r);
    }
}

static void test_absurd_stroke_parameters(void)
{
    static const double widths[] = {0.0, -1.0, 1e-300, 1e-9, 1e6, 1e300, INFINITY, NAN};
    static const double limits[] = {NAN, INFINITY, 0.0, -5.0, 1.0};
    static const double tols[] = {0.0, -1.0, NAN, INFINITY, 1e-300};
    static const double offs[] = {NAN, INFINITY, -1e300, 0.0};
    double pat_nan[3] = {NAN, NAN, NAN};
    double pat_inf[2] = {INFINITY, 1.0};
    double pat_neg[2] = {-1.0, -2.0};
    double pat_big[1000];
    double *pats[4] = {pat_nan, pat_inf, pat_neg, pat_big};
    size_t npats[4] = {3, 2, 2, 1000};
    pc_path p = vh_pentagram();
    pc_path e = vh_ellipse(0.0, 0.0, 50.0, 30.0);
    size_t i;
    for (i = 0; i < 1000; i++) {
        pat_big[i] = 0.5 + (double)(i % 7);
    }
    vh_push(&p, vh_take0(&e, PC_PATH_COMBINE));
    for (i = 0; i < 24; i++) {
        pc_vec_stroke_style st;
        pc_vec_rasterizer r;
        pc_status s;
        pc_vec_stroke_style_default(&st);
        st.width = widths[i % 8];
        st.miter_limit = limits[(i / 8) % 5];
        st.join = (pc_line_join)(i % 3);
        st.cap = (pc_line_cap)((i / 3) % 3);
        st.dashes = pats[i % 4];
        st.ndashes = i % 5 == 0 ? 0 : npats[i % 4];
        st.dash_offset = offs[i % 4];
        s = pc_vec_stroke_rasterizer(&p, &st, tols[(i / 7) % 5], &r, NULL);
        CHECK(s == PC_OK || s == PC_ERR_LIMIT);
        if (s == PC_OK) {
            float *c = NULL;
            size_t n = 0;
            CHECK(pc_vec_rasterizer_render(&r, pc_rect_make(10, 10, 40, 18), &c, &n, NULL) == PC_OK);
            for (n = 0; c != NULL && n < 30 * 8; n++) {
                if (!(c[n] >= 0.0f && c[n] <= 1.0f) && !isnan(c[n])) {
                    CHECK(false);
                    break;
                }
            }
            free(c);
            pc_vec_rasterizer_fini(&r);
        }
    }
    pc_path_fini(&p);
}

static void test_tiny_dashes_are_bounded(void)
{
    double pts[2][2] = {{0.0, 0.0}, {1e6, 0.0}};
    bool k[2] = {true, true};
    pc_vec_polyline pl = {pts, k, 2, 2, false};
    double pat[2] = {1e-3, 1e-3};
    pc_vec_polyline *d = NULL;
    size_t n = 0;
    pc_error e;
    pc_vec_polys out = {NULL, 0, 0};
    pc_vec_stroke_style st;
    /* 5e8 dashes requested: past the reference's 100000-step budget the polyline gets no dashes
     * at all (and so no stroke), with bounded work. */
    CHECK(pc_vec_dash_polyline(&pl, pat, 2, 0.0, &d, &n, &e) == PC_OK);
    CHECK(d == NULL && n == 0);
    pc_vec_stroke_style_default(&st);
    st.cap = PC_LINE_CAP_ROUND;
    st.dashes = pat;
    st.ndashes = 2;
    CHECK(pc_vec_stroke_polygons(&pl, 1, &st, 0.01, &out, NULL) == PC_OK);
    CHECK(out.len == 0);
    pc_vec_polys_fini(&out);
    /* Steps too small to advance the position along the segment end the dashing too. */
    pat[0] = 1e-9;
    pat[1] = 1e-9;
    pts[1][0] = 1.0;
    CHECK(pc_vec_dash_polyline(&pl, pat, 2, 0.0, &d, &n, NULL) == PC_OK);
    CHECK(d == NULL && n == 0);
    pts[0][0] = 1e12;
    pts[1][0] = 1e12 + 1.0;
    pat[0] = 1e-6;
    pat[1] = 1e-6;
    CHECK(pc_vec_dash_polyline(&pl, pat, 2, 0.0, &d, &n, NULL) == PC_OK);
    CHECK(d == NULL && n == 0);
}

static void test_flatten_point_cap(void)
{
    /* 300 huge curves at a tiny tolerance would be 300 x 65536 points: refused cleanly. */
    pc_subpath sp;
    pc_path p;
    pc_vec_polyline *lines = NULL;
    size_t nl = 0;
    size_t i;
    pc_vec_rasterizer r;
    memset(&sp, 0, sizeof sp);
    sp.knots = calloc(300, sizeof *sp.knots);
    if (sp.knots == NULL) {
        return;
    }
    sp.nknots = 300;
    for (i = 0; i < 300; i++) {
        double x = (double)i * 1e6;
        sp.knots[i] =
            pc_knot_smooth(pc_point_make(x, 0.0), pc_point_make(x - 4e5, 9e5), pc_point_make(x + 4e5, -9e5));
    }
    p = vh_path(&sp, 1);
    CHECK(pc_vec_flatten_path(&p, 1e-9, &lines, &nl, NULL) == PC_ERR_LIMIT);
    CHECK(lines == NULL && nl == 0);
    CHECK(pc_vec_fill_rasterizer(&p, 1e-9, &r, NULL) == PC_ERR_LIMIT);
    pc_path_fini(&p);
}

static void test_huge_render_requests_fail_cleanly(void)
{
    pc_rect huge = pc_rect_make(INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX);
    pc_path p = vh_rect(1.0, 1.0, 5.0, 5.0);
    pc_path inv = vh_rect(1.0, 1.0, 5.0, 5.0);
    pc_vec_rasterizer r;
    pc_vec_compiled_shape cs;
    pc_shape_layer sh;
    pc_vector_mask vm;
    pc_surface s;
    float *c = NULL;
    size_t n = 0;
    float (*px)[4] = NULL;
    float small[4];
    pc_error e;
    CHECK(pc_vec_fill_rasterizer(&p, 0.01, &r, NULL) == PC_OK);
    CHECK(pc_vec_rasterizer_render(&r, huge, &c, &n, &e) == PC_ERR_LIMIT);
    CHECK(c == NULL);
    CHECK(pc_vec_rasterizer_render_into(&r, pc_rect_make(0, 0, 4, 4), small, 4, NULL) == PC_ERR_INVALID);
    CHECK(pc_vec_rasterizer_render_into(&r, pc_rect_make(0, 0, 2, 2), small, 4, NULL) == PC_OK);
    /* Non-inverted coverage over a huge clip only touches the path's bounds. */
    CHECK(pc_vec_coverage_surface(&r, PC_PIXEL_FORMAT_GRAY8, huge, &s, NULL) == PC_OK);
    CHECK(pc_rect_eq(pc_surface_content_bounds(&s), pc_rect_make(1, 1, 6, 6)));
    pc_surface_fini(&s);
    pc_vec_rasterizer_fini(&r);
    inv.inverted = true;
    CHECK(pc_vec_fill_rasterizer(&inv, 0.01, &r, NULL) == PC_OK);
    CHECK(pc_vec_coverage_surface(&r, PC_PIXEL_FORMAT_GRAY8, huge, &s, &e) == PC_ERR_LIMIT);
    pc_vec_rasterizer_fini(&r);
    /* An inverted filled shape's bounds are effectively infinite. */
    memset(&sh, 0, sizeof sh);
    sh.path = inv;
    sh.has_fill = true;
    sh.fill = pc_fill_solid(PC_COLOR_BLACK);
    CHECK(pc_vec_compiled_shape_new(&sh, 0.01, huge, &cs, NULL) == PC_OK);
    CHECK(pc_vec_compiled_shape_render(&cs, PC_PIXEL_FORMAT_RGBA8, huge, &s, NULL) == PC_ERR_LIMIT);
    CHECK(pc_vec_compiled_shape_render_rgba(&cs, huge, &px, &n, NULL) == PC_ERR_LIMIT);
    CHECK(pc_vec_compiled_shape_render(&cs, PC_PIXEL_FORMAT_RGBA8, pc_rect_make(0, 0, 300, 20), &s, NULL) ==
          PC_OK);
    CHECK(pc_rect_eq(pc_surface_content_bounds(&s), pc_rect_make(0, 0, 300, 20)));
    pc_surface_fini(&s);
    pc_vec_compiled_shape_fini(&cs);
    pc_shape_layer_fini(&sh);
    pc_vector_mask_init(&vm, &p);
    vm.enabled = false;
    CHECK(pc_vec_mask_values(&vm, huge, &c, &n, NULL) == PC_ERR_LIMIT);
    pc_vector_mask_fini(&vm);
}

static void test_extreme_coordinates_saturate(void)
{
    double far[3][2] = {{-1e12, -1e12}, {1e12, -1e12}, {0.0, 1e12}};
    double edge[4][2] = {{2147483640.0, 0.0}, {2147483647.0, 0.0}, {2147483647.0, 3.0}, {2147483640.0, 3.0}};
    pc_vec_poly poly;
    pc_vec_rasterizer r;
    pc_rect b;
    pc_surface s;
    poly.pts = far;
    poly.len = 3;
    pc_vec_rasterizer_init(&r, false);
    CHECK(pc_vec_rasterizer_add_component(&r, &poly, 1, PC_PATH_COMBINE, PC_FILL_RULE_NONZERO, NULL) ==
          PC_OK);
    CHECK(pc_vec_rasterizer_pixel_bounds(&r, &b));
    CHECK(b.x0 == INT32_MIN && b.y0 == INT32_MIN && b.x1 == INT32_MAX && b.y1 == INT32_MAX);
    CHECK(pc_vec_coverage_surface(&r, PC_PIXEL_FORMAT_GRAY8, pc_rect_make(-300, -300, 300, 300), &s, NULL) ==
          PC_OK);
    CHECK(pc_surface_tile_count(&s) > 0);
    pc_surface_fini(&s);
    pc_vec_rasterizer_fini(&r);
    /* Bands and tiles next to INT32_MAX do not wrap around. */
    poly.pts = edge;
    poly.len = 4;
    pc_vec_rasterizer_init(&r, false);
    CHECK(pc_vec_rasterizer_add_component(&r, &poly, 1, PC_PATH_COMBINE, PC_FILL_RULE_NONZERO, NULL) ==
          PC_OK);
    CHECK(pc_vec_coverage_surface(&r, PC_PIXEL_FORMAT_GRAY8,
                                  pc_rect_make(INT32_MAX - 300, INT32_MAX - 300, INT32_MAX, INT32_MAX), &s,
                                  NULL) == PC_OK);
    pc_surface_fini(&s);
    CHECK(pc_vec_coverage_surface(&r, PC_PIXEL_FORMAT_GRAY8, pc_rect_make(INT32_MAX - 10, -2, INT32_MAX, 10),
                                  &s, NULL) == PC_OK);
    CHECK(pc_surface_tile_count(&s) > 0);
    pc_surface_fini(&s);
    pc_vec_rasterizer_fini(&r);
}

static void test_crossing_heavy_polygons_stay_bounded(void)
{
    /* Many crossings per row exercise band()'s depth-capped splitting. */
    vh_rng rng = {0x5ca7u};
    size_t t;
    for (t = 0; t < 6; t++) {
        size_t m = 50 + 60 * t;
        double (*pts)[2] = malloc(m * sizeof *pts);
        pc_vec_poly poly;
        pc_vec_rasterizer r;
        float *a = NULL;
        float *b = NULL;
        size_t n = 0;
        size_t i;
        if (pts == NULL) {
            return;
        }
        for (i = 0; i < m; i++) {
            pts[i][0] = vh_f(&rng) * 60.0;
            pts[i][1] = vh_f(&rng) * 60.0;
        }
        poly.pts = pts;
        poly.len = m;
        pc_vec_rasterizer_init(&r, t % 2 == 1);
        CHECK(pc_vec_rasterizer_add_component(&r, &poly, 1, PC_PATH_COMBINE,
                                              t % 3 == 0 ? PC_FILL_RULE_EVENODD : PC_FILL_RULE_NONZERO,
                                              NULL) == PC_OK);
        CHECK(pc_vec_rasterizer_render(&r, pc_rect_make(0, 0, 60, 60), &a, &n, NULL) == PC_OK);
        for (i = 0; a != NULL && i < n; i++) {
            if (!(a[i] >= 0.0f && a[i] <= 1.0f)) {
                CHECK(false);
                break;
            }
        }
        /* The parallel path (strips) and the sequential one agree bit for bit. */
        free(a);
        a = NULL;
        CHECK(pc_vec_rasterizer_render(&r, pc_rect_make(-100, -100, 160, 160), &a, &n, NULL) == PC_OK);
        b = malloc(n * sizeof *b);
        if (a != NULL && b != NULL) {
            CHECK(pc_vec_rasterizer_render_into(&r, pc_rect_make(-100, -100, 160, 160), b, n, NULL) == PC_OK);
            CHECK(memcmp(a, b, n * sizeof *a) == 0);
        }
        free(a);
        free(b);
        free(pts);
        pc_vec_rasterizer_fini(&r);
    }
}

static void test_trace_hostile_inputs(void)
{
    float v[16];
    pc_vec_polys cs = {NULL, 0, 0};
    pc_path p;
    pc_error e;
    size_t i;
    for (i = 0; i < 16; i++) {
        v[i] = (float)(i % 2);
    }
    /* Too few values for the rect, or a rect whose area overflows. */
    CHECK(pc_vec_contours(v, 15, pc_rect_make(0, 0, 4, 4), 0.0f, &cs, &e) == PC_ERR_INVALID);
    CHECK(pc_vec_trace_mask(v, 16, pc_rect_make(INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX), 0.0f, 1.0, &p,
                            NULL) == PC_ERR_INVALID);
    CHECK(pc_vec_trace_mask(NULL, 0, pc_rect_make(0, 0, 1, 1), 0.0f, 1.0, &p, NULL) == PC_ERR_INVALID);
    /* Empty rects (even huge ones with zero height) trace nothing, instantly. */
    CHECK(pc_vec_trace_mask(NULL, 0, pc_rect_make(INT32_MIN, 0, INT32_MAX, 0), 1.0f, 1.0, &p, NULL) == PC_OK);
    CHECK(p.nsubpaths == 0);
    pc_path_fini(&p);
    /* Non-finite values, outside values and tolerances. */
    {
        const float outs[4] = {NAN, INFINITY, -INFINITY, 0.5f};
        const double tols[4] = {NAN, INFINITY, -1.0, 0.0};
        float w[64];
        size_t k;
        for (k = 0; k < 4; k++) {
            for (i = 0; i < 64; i++) {
                w[i] = i % 3 == 0 ? NAN : i % 5 == 0 ? INFINITY : (float)((i * 7 + k) % 4) / 3.0f;
            }
            CHECK(pc_vec_trace_mask(w, 64, pc_rect_make(INT32_MAX - 8, INT32_MIN, INT32_MAX, INT32_MIN + 8),
                                    outs[k], tols[k], &p, NULL) == PC_OK);
            pc_path_fini(&p);
        }
    }
    /* A large checkerboard: many tiny contours and saddles. */
    {
        size_t wdt = 256;
        float *cb = malloc(wdt * wdt * sizeof *cb);
        size_t x;
        size_t y;
        if (cb != NULL) {
            for (y = 0; y < wdt; y++) {
                for (x = 0; x < wdt; x++) {
                    cb[y * wdt + x] = (x + y) % 2 == 0 ? 1.0f : 0.0f;
                }
            }
            CHECK(pc_vec_trace_mask(cb, wdt * wdt, pc_rect_make(0, 0, 256, 256), 0.0f, 1.0, &p, NULL) ==
                  PC_OK);
            CHECK(p.nsubpaths > 0);
            pc_path_fini(&p);
            free(cb);
        }
    }
}

static void test_edit_hostile(void)
{
    pc_path p = vh_curve();
    pc_path empty;
    pc_vec_knot_ref refs[2];
    double nd[2] = {NAN, INFINITY};
    double box[4] = {NAN, NAN, INFINITY, -INFINITY};
    pc_vec_knot_ref *v = NULL;
    size_t n = 0;
    pc_vec_hit hit;
    pc_vec_handle_ref hr;
    refs[0].sub = 0;
    refs[0].knot = 0;
    refs[1].sub = 0;
    refs[1].knot = 2;
    CHECK(pc_vec_move_anchors(&p, refs, 2, nd, NULL) == PC_OK);
    CHECK(pc_vec_move_handle(&p, refs[1], PC_VEC_HANDLE_IN, pc_point_make(NAN, INFINITY), false, NULL) ==
          PC_OK);
    CHECK(pc_vec_bend_segment(&p, refs[0], NAN, nd, NULL) == PC_OK);
    CHECK(pc_vec_anchors_in(&p, box, &v, &n, NULL) == PC_OK);
    free(v);
    hr.knot = refs[1];
    hr.handle = PC_VEC_HANDLE_OUT;
    (void)pc_vec_hit_test(&p, pc_point_make(NAN, NAN), NAN, &hr, 1, &hit);
    (void)pc_vec_hit_test(&p, pc_point_make(1.0, 1.0), INFINITY, &hr, 1, &hit);
    exercise_path(&p, 0.01);
    pc_path_fini(&p);
    memset(&empty, 0, sizeof empty);
    CHECK(pc_vec_move_anchors(&empty, NULL, 0, nd, NULL) == PC_OK);
    CHECK(pc_vec_move_anchors(&empty, refs, 1, nd, NULL) == PC_ERR_INVALID);
    CHECK(!pc_vec_hit_test(&empty, pc_point_make(0.0, 0.0), 10.0, &hr, 1, &hit));
    CHECK(pc_vec_subpath_anchors(&empty, SIZE_MAX, &v, &n, NULL) == PC_OK && n == 0);
}

static void test_shapes_hostile(void)
{
    const uint32_t sides[4] = {0, 1, UINT32_MAX, 7};
    size_t i;
    size_t j;
    for (i = 0; i < NNASTY; i++) {
        double x = NASTY[i];
        double radii[4] = {x, -x, NAN, 1e300};
        double a[2] = {x, 1.0};
        double b[2] = {2.0, x};
        pc_path p;
        CHECK(pc_vec_shape_rect(x, 0.0, x, 5.0, &p, NULL) == PC_OK);
        exercise_path(&p, 0.01);
        pc_path_fini(&p);
        CHECK(pc_vec_shape_rounded_rect(0.0, x, 10.0, x, radii, &p, NULL) == PC_OK);
        exercise_path(&p, 0.01);
        pc_path_fini(&p);
        CHECK(pc_vec_shape_ellipse(x, x, x, 3.0, &p, NULL) == PC_OK);
        exercise_path(&p, 0.01);
        pc_path_fini(&p);
        for (j = 0; j < 4; j++) {
            CHECK(pc_vec_shape_polygon(0.0, 0.0, x, 10.0, sides[j], x, &p, NULL) == PC_OK);
            CHECK(p.subpaths[0].nknots >= 3 && p.subpaths[0].nknots <= 200);
            pc_path_fini(&p);
        }
        CHECK(pc_vec_shape_line(a, b, x, &p, NULL) == PC_OK);
        exercise_path(&p, 0.01);
        pc_path_fini(&p);
    }
}

static void test_gradient_hostile(void)
{
    const float fs[6] = {NAN, INFINITY, -INFINITY, 0.0f, -1e30f, 1e30f};
    size_t i;
    for (i = 0; i < 6 * 5; i++) {
        pc_fill f;
        pc_vec_paint paint;
        pc_color_stop stops[3];
        float out[4];
        double frame[4] = {0.0, 0.0, (double)fs[i % 6], 1e300};
        size_t k;
        stops[0].pos = fs[i % 6];
        stops[0].color = PC_COLOR_BLACK;
        stops[1].pos = 0.5f;
        stops[1].color = pc_color_rgba(1.0f, NAN, 0.0f, 0.5f);
        stops[2].pos = fs[(i + 1) % 6];
        stops[2].color = PC_COLOR_WHITE;
        CHECK(pc_fill_gradient(stops, i % 4, fs[i % 6], fs[(i + 2) % 6], (pc_gradient_style)(i % 5),
                               i % 2 == 0, &f, NULL) == PC_OK);
        f.u.gradient.offset[0] = fs[(i + 3) % 6];
        f.u.gradient.dither = true;
        CHECK(pc_vec_paint_from_fill(&f, frame, frame, &paint, NULL) == PC_OK);
        for (k = 0; k < NNASTY; k++) {
            pc_vec_paint_at(&paint, NASTY[k], NASTY[(k + 3) % NNASTY], out);
        }
        pc_vec_paint_fini(&paint);
        pc_fill_fini(&f);
    }
}

int main(int argc, char **argv)
{
    RUN(test_nonfinite_coordinates_everywhere);
    RUN(test_empty_and_degenerate_paths);
    RUN(test_absurd_stroke_parameters);
    RUN(test_tiny_dashes_are_bounded);
    RUN(test_flatten_point_cap);
    RUN(test_huge_render_requests_fail_cleanly);
    RUN(test_extreme_coordinates_saturate);
    RUN(test_crossing_heavy_polygons_stay_bounded);
    RUN(test_trace_hostile_inputs);
    RUN(test_edit_hostile);
    RUN(test_shapes_hostile);
    RUN(test_gradient_hostile);
    return pc_t_finish();
}
