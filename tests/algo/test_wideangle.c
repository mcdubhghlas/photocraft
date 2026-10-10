/* Adaptive Wide Angle (port of the tests in crates/algo/src/wideangle.rs) plus never-crash and
 * JSON checks. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "harness.h"

static pc_algo_wide_angle wa(pc_algo_wide_model model, double focal)
{
    pc_algo_wide_angle p;
    pc_algo_wide_angle_default(&p);
    p.model = model;
    p.focal_length = focal;
    return p;
}

static void test_camera_models_round_trip(void)
{
    pc_rect frame = pc_rect_make(0, 0, 600, 400);
    const double pts[3][2] = {{10, 20}, {300, 200}, {590, 350}};
    pc_algo_wide_model models[2] = {PC_ALGO_WIDE_FISHEYE, PC_ALGO_WIDE_PERSPECTIVE};
    pc_algo_wide_camera cam;
    pc_algo_wide_angle p;
    double d[3], q[2], a[2] = {50, 50}, b[2] = {550, 80}, (*arc)[2] = NULL;
    size_t n = 0;
    int m, k;
    for (m = 0; m < 2; m++) {
        p = wa(models[m], 15.0);
        pc_algo_wide_camera_init(&cam, &p, frame);
        for (k = 0; k < 3; k++) {
            pc_algo_wide_camera_ray(&cam, pts[k][0], pts[k][1], d);
            CHECK(pc_algo_wide_camera_pixel(&cam, d, q));
            CHECK(fabs(q[0] - pts[k][0]) < 1e-6 && fabs(q[1] - pts[k][1]) < 1e-6);
        }
    }
    p = wa(PC_ALGO_WIDE_AUTO, 0.0);
    pc_algo_wide_camera_init(&cam, &p, pc_rect_make(0, 0, 800, 400));
    CHECK(cam.model == PC_ALGO_WIDE_FULL_SPHERICAL);
    pc_algo_wide_camera_ray(&cam, 123.0, 77.0, d);
    CHECK(pc_algo_wide_camera_pixel(&cam, d, q));
    CHECK(fabs(q[0] - 123.0) < 1e-6 && fabs(q[1] - 77.0) < 1e-6);
    /* Perspective arcs are straight; fisheye arcs bend. */
    p = wa(PC_ALGO_WIDE_PERSPECTIVE, 0.0);
    pc_algo_wide_camera_init(&cam, &p, frame);
    CHECK(pc_algo_wide_camera_arc(&cam, a, b, 10, &arc, &n, NULL) == PC_OK);
    CHECK(n == 11 && fabs(arc[5][1] - 65.0) < 2.0);
    free(arc);
    p = wa(PC_ALGO_WIDE_FISHEYE, 8.0);
    pc_algo_wide_camera_init(&cam, &p, frame);
    a[0] = 50, a[1] = 60, b[0] = 550, b[1] = 60;
    CHECK(pc_algo_wide_camera_arc(&cam, a, b, 10, &arc, &n, NULL) == PC_OK);
    CHECK(n == 11 && arc[5][1] < 50.0);
    free(arc);
}

static void test_constraints_become_straight_and_level(void)
{
    pc_rect frame = pc_rect_make(0, 0, 480, 320);
    pc_algo_wide_constraint cs[2] = {{{60, 70}, {420, 70}, PC_ALGO_WIDE_HORIZONTAL},
                                     {{80, 60}, {80, 260}, PC_ALGO_WIDE_FREE}};
    pc_algo_wide_angle p = wa(PC_ALGO_WIDE_FISHEYE, 8.0);
    pc_algo_wide_camera cam;
    pc_algo_wide_mesh m;
    double (*arc)[2] = NULL, before = 0.0, lo = 1e300, hi = -1e300, c[2], q[2] = {300, 250};
    size_t n = 0, i;
    p.constraints = cs;
    p.n_constraints = 2;
    pc_algo_wide_camera_init(&cam, &p, frame);
    CHECK(pc_algo_wide_camera_arc(&cam, cs[0].a, cs[0].b, 24, &arc, &n, NULL) == PC_OK);
    for (i = 0; i < n; i++) {
        before = fmax(before, fabs(arc[i][1] - 70.0));
    }
    free(arc);
    CHECK(before > 10.0);
    CHECK(pc_algo_wide_solve(&p, frame, &m, NULL) == PC_OK);
    CHECK(m.residual < 2.0);
    /* The horizontal constraint is level. */
    for (i = 0; i < m.curve_len[0]; i++) {
        double o[2];
        pc_algo_wide_mesh_map(&m, frame, m.curves[0][i], o);
        lo = fmin(lo, o[1]);
        hi = fmax(hi, o[1]);
    }
    CHECK(hi - lo < 2.0);
    /* Far from the constraints the image barely moves. */
    pc_algo_wide_mesh_map(&m, frame, q, c);
    CHECK(fabs(c[0] - 300.0) < 80.0 && fabs(c[1] - 250.0) < 40.0);
    pc_algo_wide_mesh_fini(&m);
}

static void test_no_constraints_is_identity(void)
{
    pc_rect frame = pc_rect_make(0, 0, 64, 48);
    pc_algo_wide_angle p = wa(PC_ALGO_WIDE_AUTO, 0.0);
    pc_algo_wide_mesh m;
    size_t i, nv, nt = 0;
    size_t (*tris)[3] = NULL;
    CHECK(pc_algo_wide_solve(&p, frame, &m, NULL) == PC_OK);
    nv = (m.nx + 1) * (m.ny + 1);
    for (i = 0; i < nv; i++) {
        CHECK(fabs(m.out[i][0] - m.src[i][0]) < 1e-9 && fabs(m.out[i][1] - m.src[i][1]) < 1e-9);
    }
    CHECK(pc_algo_wide_mesh_triangles(&m, &tris, &nt, NULL) == PC_OK);
    CHECK(nt == m.nx * m.ny * 2);
    free(tris);
    pc_algo_wide_mesh_fini(&m);
}

static void test_json(void)
{
    const char *text = "{\"model\":\"fisheye\",\"focalLength\":8,\"constraints\":[{\"a\":[1,2],\"b\":[3.5,4],"
                       "\"orientation\":\"vertical\"},{\"b\":[0,0],\"a\":[1,1]}]}";
    yyjson_doc *d = yyjson_read(text, strlen(text), 0);
    pc_algo_wide_angle p;
    pc_error e;
    yyjson_mut_doc *md;
    char *out;
    CHECK(pc_algo_wide_angle_from_json(yyjson_doc_get_root(d), &p, NULL) == PC_OK);
    CHECK(p.model == PC_ALGO_WIDE_FISHEYE && p.focal_length == 8.0 && p.crop_factor == 1.0 &&
          p.scale == 100.0);
    CHECK(p.n_constraints == 2 && p.constraints[0].b[0] == 3.5 &&
          p.constraints[0].orientation == PC_ALGO_WIDE_VERTICAL);
    CHECK(p.constraints[1].orientation == PC_ALGO_WIDE_FREE);
    md = yyjson_mut_doc_new(NULL);
    yyjson_mut_doc_set_root(md, pc_algo_wide_angle_to_json(md, &p));
    out = yyjson_mut_write(md, 0, NULL);
    CHECK_EQ_STR(
        out, "{\"model\":\"fisheye\",\"focalLength\":8.0,\"cropFactor\":1.0,\"scale\":100.0,\"constraints\":"
             "[{\"a\":[1.0,2.0],\"b\":[3.5,4.0],\"orientation\":\"vertical\"},{\"a\":[1.0,1.0],\"b\":[0.0,"
             "0.0],\"orientation\":\"free\"}]}");
    free(out);
    yyjson_mut_doc_free(md);
    yyjson_doc_free(d);
    pc_algo_wide_angle_fini(&p);
    d = yyjson_read("{\"constraints\":[{\"a\":[1,2]}]}", 29, 0);
    CHECK(pc_algo_wide_angle_from_json(yyjson_doc_get_root(d), &p, &e) == PC_ERR_INVALID);
    CHECK_EQ_STR(e.msg, "missing field `b`");
    yyjson_doc_free(d);
    d = yyjson_read("{\"model\":\"wide\"}", 16, 0);
    CHECK(pc_algo_wide_angle_from_json(yyjson_doc_get_root(d), &p, &e) == PC_ERR_INVALID);
    CHECK_EQ_STR(e.msg,
                 "unknown variant `wide`, expected one of `auto`, `fisheye`, `perspective`, `fullSpherical`");
    yyjson_doc_free(d);
}

static void test_hostile_settings_never_crash(void)
{
    /* Thin frames keep the mesh small (48 cells on the long side); NaN rows run every CGLS
     * iteration. */
    pc_rect frames[3] = {pc_rect_make(0, 0, 4, 300), pc_rect_make(0, 0, 300, 3), pc_rect_make(5, 5, 6, 400)};
    double vals[4] = {NAN, INFINITY, -1e300, 0.0};
    int f, k, q;
    for (f = 0; f < 3; f++) {
        for (k = 0; k < 4; k++) {
            for (q = 0; q < 4; q++) {
                pc_algo_wide_constraint cs[2] = {{{vals[k], 1}, {30, vals[k]}, PC_ALGO_WIDE_FREE},
                                                 {{1, 1}, {1, 1}, PC_ALGO_WIDE_HORIZONTAL}};
                pc_algo_wide_angle p = wa((pc_algo_wide_model)q, vals[k]);
                pc_algo_wide_mesh m;
                p.crop_factor = vals[(k + 1) % 4];
                p.scale = vals[(k + 2) % 4];
                p.constraints = cs;
                p.n_constraints = 2;
                CHECK(pc_algo_wide_solve(&p, frames[f], &m, NULL) == PC_OK);
                pc_algo_wide_mesh_fini(&m);
            }
        }
    }
    {
        pc_algo_wide_constraint c = {{NAN, 1}, {30, 2}, PC_ALGO_WIDE_FREE};
        pc_algo_wide_angle p = wa(PC_ALGO_WIDE_PERSPECTIVE, 10.0);
        pc_algo_wide_mesh m;
        p.constraints = &c;
        p.n_constraints = 1;
        CHECK(pc_algo_wide_solve(&p, PC_RECT_EMPTY, &m, NULL) == PC_OK);
        pc_algo_wide_mesh_fini(&m);
    }
}

static void test_apply_renders_and_has_no_gaps_across_tiles(void)
{
    const float fillc[4] = {0.2f, 0.4f, 0.6f, 1.0f};
    pc_rect frame = pc_rect_make(0, 0, 64, 48), big = pc_rect_make(0, 0, 772, 517);
    pc_algo_wide_angle p = wa(PC_ALGO_WIDE_AUTO, 0.0);
    pc_algo_wide_constraint c = {{20, 30}, {700, 35}, PC_ALGO_WIDE_HORIZONTAL};
    pc_surface s, out;
    float px[4];
    int k, x, y, holes;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_surface_fill_rect(&s, frame, fillc, 4, NULL) == PC_OK);
    p.scale = 80.0;
    CHECK(pc_algo_wide_apply(&s, frame, &p, PC_ALGO_INTERP_BILINEAR, &out, NULL) == PC_OK);
    pc_surface_rgba(&out, 32, 24, px);
    CHECK(px[3] > 0.99f);
    pc_surface_rgba(&out, 1, 1, px);
    CHECK(px[3] < 0.01f);
    pc_surface_fini(&out), pc_surface_fini(&s);
    /* tile_tests::no_gaps_across_tiles */
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_surface_fill_rect(&s, big, fillc, 4, NULL) == PC_OK);
    for (k = 0; k < 2; k++) {
        p = k == 0 ? wa(PC_ALGO_WIDE_AUTO, 0.0) : wa(PC_ALGO_WIDE_FISHEYE, 12.0);
        if (k == 1) {
            p.constraints = &c;
            p.n_constraints = 1;
        }
        CHECK(pc_algo_wide_apply(&s, big, &p, PC_ALGO_INTERP_BILINEAR, &out, NULL) == PC_OK);
        holes = 0;
        for (y = 80; y < 440; y++) {
            for (x = 80; x < 690; x++) {
                pc_surface_rgba(&out, x, y, px);
                holes += px[3] < 0.5f;
            }
        }
        CHECK_EQ_INT(holes, 0);
        pc_surface_fini(&out);
    }
    pc_surface_fini(&s);
}

int main(int argc, char **argv)
{
    RUN(test_apply_renders_and_has_no_gaps_across_tiles);
    RUN(test_camera_models_round_trip);
    RUN(test_constraints_become_straight_and_level);
    RUN(test_no_constraints_is_identity);
    RUN(test_json);
    RUN(test_hostile_settings_never_crash);
    return pc_t_finish();
}
