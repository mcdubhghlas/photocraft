/* Edit > Puppet Warp (src/algo/puppet*.c): the reference's tests from crates/algo/src/puppet.rs
 * plus never-crash cases (coincident / colinear / NaN pins, empty meshes, huge bounds and
 * iteration counts, hostile JSON). Bit-exact parity with the reference is in test_warp_parity.c. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "base/json.h"
#include "harness.h"

static void blob(pc_sample_type st, pc_surface *s)
{
    pc_pixel_format f = {PC_COLOR_MODE_RGB, st, true};
    int32_t x, y;
    pc_surface_init(s, f);
    /* An L shape: a vertical bar and a foot. */
    for (y = 10; y < 70; y++) {
        for (x = 20; x < 36; x++) {
            float px[4] = {(float)((x + y) % 7) / 6.0f, 0.5f, (float)y / 80.0f, 1.0f};
            CHECK(pc_surface_write_pixel(s, x, y, px, 4, NULL) == PC_OK);
        }
    }
    for (y = 56; y < 70; y++) {
        for (x = 36; x < 70; x++) {
            float px[4] = {0.2f, (float)((x * 3) % 5) / 4.0f, 0.8f, 1.0f};
            CHECK(pc_surface_write_pixel(s, x, y, px, 4, NULL) == PC_OK);
        }
    }
}

static pc_algo_puppet_pin pin(double sx, double sy, double dx, double dy)
{
    pc_algo_puppet_pin p;
    memset(&p, 0, sizeof p);
    p.src[0] = sx, p.src[1] = sy, p.dst[0] = dx, p.dst[1] = dy;
    return p;
}

static pc_algo_puppet_warp warp_of(const pc_algo_puppet_pin *pins, size_t n, pc_algo_puppet_mode mode,
                                   double expansion)
{
    pc_algo_puppet_warp w = pc_algo_puppet_warp_make();
    w.pins = n > 0 ? malloc(n * sizeof *pins) : NULL;
    if (n > 0) {
        memcpy(w.pins, pins, n * sizeof *pins);
    }
    w.npins = n;
    w.mode = mode;
    w.expansion = expansion;
    return w;
}

static bool mesh_inside(const pc_algo_puppet_mesh *m, double px, double py)
{
    size_t t;
    for (t = 0; t < m->ntris; t++) {
        double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
        int k;
        for (k = 0; k < 3; k++) {
            const double *v = m->verts[m->tris[t][k]];
            x0 = fmin(x0, v[0]), x1 = fmax(x1, v[0]), y0 = fmin(y0, v[1]), y1 = fmax(y1, v[1]);
        }
        if (px >= x0 && px <= x1 && py >= y0 && py <= y1) {
            return true;
        }
    }
    return false;
}

static void test_mesh_covers_the_shape_only(void)
{
    pc_surface s;
    pc_algo_puppet_mesh m, more, fewer;
    pc_rect b;
    blob(PC_SAMPLE_TYPE_U8, &s);
    b = pc_surface_content_bounds(&s);
    CHECK(pc_algo_puppet_build_mesh(&s, b, PC_ALGO_PUPPET_DENSITY_NORMAL, 2.0, &m, NULL) == PC_OK);
    CHECK(m.ntris > 20);
    CHECK(mesh_inside(&m, 28, 30) && mesh_inside(&m, 60, 62));
    CHECK(!mesh_inside(&m, 60, 20)); /* the empty corner of the L has no mesh */
    CHECK(pc_algo_puppet_build_mesh(&s, b, PC_ALGO_PUPPET_MORE, 2.0, &more, NULL) == PC_OK);
    CHECK(pc_algo_puppet_build_mesh(&s, b, PC_ALGO_PUPPET_FEWER, 2.0, &fewer, NULL) == PC_OK);
    CHECK(more.ntris > m.ntris && m.ntris > fewer.ntris);
    pc_algo_puppet_mesh_fini(&m);
    pc_algo_puppet_mesh_fini(&more);
    pc_algo_puppet_mesh_fini(&fewer);
    pc_surface_fini(&s);
}

static double orient2(const double *a, const double *b, const double *c)
{
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}

/* Whether p lies in a triangle of the mesh (either orientation). */
static bool in_mesh(const pc_algo_puppet_mesh *m, double px, double py)
{
    double p[2] = {px, py};
    size_t t;
    for (t = 0; t < m->ntris; t++) {
        const double *a = m->verts[m->tris[t][0]], *b = m->verts[m->tris[t][1]], *c = m->verts[m->tris[t][2]];
        double d0 = orient2(a, b, p), d1 = orient2(b, c, p), d2 = orient2(c, a, p);
        if ((d0 >= 0.0 && d1 >= 0.0 && d2 >= 0.0) || (d0 <= 0.0 && d1 <= 0.0 && d2 <= 0.0)) {
            return true;
        }
    }
    return false;
}

/* #3105: the mesh was every grid cell touching the shape, a staircase up to a cell outside it. It
 * now follows the outline grown by the expansion. */
static void test_mesh_follows_the_outline(void)
{
    pc_pixel_format f = {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true};
    const float c[4] = {0.3f, 0.6f, 0.9f, 1.0f};
    pc_surface s, l;
    pc_algo_puppet_mesh m;
    double area = 0.0, disc = 3.14159265358979323846 * 62.0 * 62.0;
    size_t i, far = 0;
    int32_t x, y;
    pc_surface_init(&s, f);
    for (y = 40; y < 160; y++) {
        for (x = 40; x < 160; x++) {
            if (hypot((double)x + 0.5 - 100.0, (double)y + 0.5 - 100.0) <= 60.0) {
                CHECK(pc_surface_write_pixel(&s, x, y, c, 4, NULL) == PC_OK);
            }
        }
    }
    CHECK(pc_algo_puppet_build_mesh(&s, pc_surface_content_bounds(&s), PC_ALGO_PUPPET_DENSITY_NORMAL, 2.0, &m,
                                    NULL) == PC_OK);
    for (i = 0; i < m.nverts; i++) {
        double r = hypot(m.verts[i][0] - 100.0, m.verts[i][1] - 100.0);
        CHECK(r <= 63.0);
        far += r > 61.0;
    }
    CHECK(far >= 40);
    for (i = 0; i < m.ntris; i++) {
        area += fabs(orient2(m.verts[m.tris[i][0]], m.verts[m.tris[i][1]], m.verts[m.tris[i][2]])) / 2.0;
    }
    CHECK(fabs(area / disc - 1.0) < 0.03);
    pc_algo_puppet_mesh_fini(&m);
    /* Sharp corners and a concave corner: every opaque pixel is inside the mesh even without an
     * expansion, and the empty corner of the L is not. */
    blob(PC_SAMPLE_TYPE_U8, &l);
    CHECK(pc_algo_puppet_build_mesh(&l, pc_surface_content_bounds(&l), PC_ALGO_PUPPET_FEWER, 0.0, &m, NULL) ==
          PC_OK);
    for (x = 20; x < 36; x++) {
        for (y = 10; y < 70; y++) {
            CHECK(in_mesh(&m, x + 0.5, y + 0.5));
        }
    }
    for (x = 36; x < 70; x++) {
        for (y = 56; y < 70; y++) {
            CHECK(in_mesh(&m, x + 0.5, y + 0.5));
        }
    }
    CHECK(!in_mesh(&m, 40.0, 50.0) && !in_mesh(&m, 60.0, 20.0));
    pc_algo_puppet_mesh_fini(&m);
    pc_surface_fini(&s);
    pc_surface_fini(&l);
}

static bool render_ok(pc_status st)
{
    CHECK(st == PC_OK);
    return st == PC_OK;
}

static void test_no_pins_or_unmoved_pins_are_identity(void)
{
    static const pc_sample_type sts[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    int k, c;
    for (k = 0; k < 3; k++) {
        pc_surface s;
        pc_rect b;
        blob(sts[k], &s);
        b = pc_surface_content_bounds(&s);
        for (c = 0; c < 2; c++) {
            pc_algo_puppet_pin pins[2] = {pin(28, 20, 28, 20), pin(60, 62, 60, 62)};
            pc_algo_puppet_warp w = warp_of(pins, c == 0 ? 0 : 2, PC_ALGO_PUPPET_NORMAL, 2.0);
            pc_surface out;
            CHECK(pc_algo_puppet_warp_is_identity(&w));
            if (render_ok(pc_algo_puppet_apply(&s, b, &w, PC_ALGO_INTERP_BICUBIC, &out, NULL))) {
                float *va = NULL, *vb = NULL, worst = 0.0f;
                size_t la = 0, lb = 0, i;
                CHECK(pc_surface_read_region(&s, b, &va, &la, NULL) == PC_OK);
                CHECK(pc_surface_read_region(&out, b, &vb, &lb, NULL) == PC_OK);
                for (i = 0; i < la && i < lb; i++) {
                    worst = fmaxf(worst, fabsf(va[i] - vb[i]));
                }
                CHECK(worst <= 1.0f / 255.0f + 1e-6f);
                free(va);
                free(vb);
                pc_surface_fini(&out);
            }
            pc_algo_puppet_warp_fini(&w);
        }
        pc_surface_fini(&s);
    }
}

static void test_an_expansion_that_empties_the_mesh_warps_nothing(void)
{
    /* #951: with pins and no vertices, locate fell back to vertex 0 and the solver indexed its
     * empty union-find. */
    pc_algo_puppet_mesh empty;
    pc_algo_puppet_solver *sv = NULL;
    double src[1][2] = {{10, 10}};
    bool fixed[1] = {true};
    pc_surface s, out;
    pc_rect b;
    pc_algo_puppet_pin p = pin(28, 30, 38, 25);
    pc_algo_puppet_warp w = warp_of(&p, 1, PC_ALGO_PUPPET_NORMAL, -200.0);
    pc_algo_puppet_mesh m;
    memset(&empty, 0, sizeof empty);
    CHECK(pc_algo_puppet_solver_new(&empty, (const double (*)[2])src, 1, fixed, 1, &sv, NULL) == PC_OK);
    CHECK(pc_algo_puppet_solver_mesh(sv)->nverts == 0);
    CHECK(pc_algo_puppet_solver_bound_count(sv) == 0);
    pc_algo_puppet_solver_free(sv);
    blob(PC_SAMPLE_TYPE_U8, &s);
    b = pc_surface_content_bounds(&s);
    CHECK(pc_algo_puppet_build_mesh(&s, b, w.density, w.expansion, &m, NULL) == PC_OK);
    CHECK(m.ntris == 0);
    pc_algo_puppet_mesh_fini(&m);
    /* Nothing to warp: the pixels stay, rather than being dropped outside an empty mesh. */
    CHECK(pc_algo_puppet_apply(&s, b, &w, PC_ALGO_INTERP_BILINEAR, &out, NULL) == PC_OK);
    CHECK(pc_surface_eq(&out, &s));
    pc_surface_fini(&out);
    pc_surface_fini(&s);
    pc_algo_puppet_warp_fini(&w);
}

static void test_a_pin_dragged_absurdly_far_is_refused_before_rendering(void)
{
    /* #1019: the renderer sized its tile bookkeeping by the deformed mesh. */
    pc_surface s, out;
    pc_rect b;
    double far[3][2] = {{1048576.0, 1048576.0}, {-1e7, 62.0}, {70.0, 66.0}};
    int k;
    blob(PC_SAMPLE_TYPE_U8, &s);
    b = pc_surface_content_bounds(&s);
    for (k = 0; k < 3; k++) {
        pc_algo_puppet_pin pins[2] = {pin(28, 20, 28, 20), pin(60, 62, far[k][0], far[k][1])};
        pc_algo_puppet_warp w = warp_of(pins, 2, PC_ALGO_PUPPET_NORMAL, 2.0);
        pc_error e;
        pc_status st = pc_algo_puppet_apply(&s, b, &w, PC_ALGO_INTERP_BILINEAR, &out, &e);
        if (k < 2) {
            CHECK(st == PC_ERR_LIMIT);
        } else if (render_ok(st)) {
            CHECK(!pc_rect_is_empty(pc_surface_content_bounds(&out)));
            pc_surface_fini(&out);
        }
        pc_algo_puppet_warp_fini(&w);
    }
    pc_surface_fini(&s);
}

static void test_a_single_pin_translates_rigidly(void)
{
    pc_surface s, out;
    pc_rect b;
    pc_algo_puppet_pin p = pin(28, 30, 38, 25);
    pc_algo_puppet_warp w = warp_of(&p, 1, PC_ALGO_PUPPET_RIGID, 2.0);
    pc_algo_puppet_solver *sv = NULL;
    double (*v)[2] = NULL;
    size_t *order = NULL, i, best = 0;
    const pc_algo_puppet_mesh *m;
    bool ok = true;
    blob(PC_SAMPLE_TYPE_U8, &s);
    b = pc_surface_content_bounds(&s);
    CHECK(pc_algo_puppet_deform(&s, b, &w, PC_ALGO_PUPPET_ITERATIONS, &sv, &v, &order, NULL) == PC_OK);
    m = pc_algo_puppet_solver_mesh(sv);
    for (i = 0; i < m->nverts; i++) {
        ok =
            ok && fabs(v[i][0] - m->verts[i][0] - 10.0) < 1e-3 && fabs(v[i][1] - m->verts[i][1] + 5.0) < 1e-3;
    }
    CHECK(ok);
    pc_algo_puppet_solver_free(sv);
    free(v);
    free(order);
    if (render_ok(pc_algo_puppet_apply(&s, b, &w, PC_ALGO_INTERP_BILINEAR, &out, NULL))) {
        float a[4], c[4];
        CHECK(pc_rect_eq(pc_surface_content_bounds(&out), pc_rect_translate(b, 10, -5)));
        pc_surface_read_pixel(&out, 30, 20, a);
        pc_surface_read_pixel(&s, 20, 25, c);
        CHECK(memcmp(a, c, sizeof a) == 0);
        pc_surface_fini(&out);
    }
    /* A fixed rotation turns the whole mesh about the pin. */
    w.pins[0] = pin(28, 30, 28, 30);
    w.pins[0].rotate.set = true;
    w.pins[0].rotate.deg = 90.0;
    CHECK(pc_algo_puppet_deform(&s, b, &w, PC_ALGO_PUPPET_ITERATIONS, &sv, &v, &order, NULL) == PC_OK);
    m = pc_algo_puppet_solver_mesh(sv);
    for (i = 1; i < m->nverts; i++) {
        if (m->verts[i][0] >= m->verts[best][0]) { /* max_by keeps the last maximum */
            best = i;
        }
    }
    {
        double dx = m->verts[best][0] - 28.0, dy = m->verts[best][1] - 30.0;
        /* (dx, dy) -> (-dy, dx) for +90 degrees with y down. */
        CHECK(fabs(v[best][0] - (28.0 - dy)) < 1.0 && fabs(v[best][1] - (30.0 + dx)) < 1.0);
    }
    pc_algo_puppet_solver_free(sv);
    free(v);
    free(order);
    pc_algo_puppet_warp_fini(&w);
    pc_surface_fini(&s);
}

static void test_arap_energy_decreases_and_modes_differ(void)
{
    static const double src[3][2] = {{28, 14}, {64, 62}, {28, 62}};
    static const double dst[3][2] = {{10, 30}, {70, 50}, {28, 62}};
    pc_algo_puppet_rot rot[3] = {{false, 0}, {false, 0}, {false, 0}};
    bool fixed[3] = {false, false, false};
    pc_surface s;
    pc_algo_puppet_mesh m;
    pc_algo_puppet_solver *sv = NULL;
    double (*v)[2] = NULL, *e = NULL, (*v0)[2] = NULL, *e0 = NULL, (*vd)[2] = NULL, *ed = NULL, start = 0.0;
    size_t k, nv;
    blob(PC_SAMPLE_TYPE_U8, &s);
    CHECK(pc_algo_puppet_build_mesh(&s, pc_surface_content_bounds(&s), PC_ALGO_PUPPET_DENSITY_NORMAL, 2.0, &m,
                                    NULL) == PC_OK);
    CHECK(pc_algo_puppet_solver_new(&m, src, 3, fixed, 3, &sv, NULL) == PC_OK);
    CHECK(pc_algo_puppet_solver_factored(sv));
    nv = pc_algo_puppet_solver_mesh(sv)->nverts;
    CHECK(pc_algo_puppet_solve(sv, dst, 3, rot, 3, PC_ALGO_PUPPET_RIGID, 30, NULL, 0, &v, &e, NULL) == PC_OK);
    CHECK(pc_algo_puppet_solve(sv, dst, 3, rot, 3, PC_ALGO_PUPPET_RIGID, 0, NULL, 0, &v0, &e0, NULL) ==
          PC_OK);
    CHECK(e0 == NULL);
    CHECK(pc_algo_puppet_energy(sv, (const double (*)[2])v0, dst, 3, rot, 3, &start, NULL) == PC_OK);
    CHECK(e[0] <= start * (1.0 + 1e-9));
    for (k = 1; k < 30; k++) {
        CHECK(e[k] <= e[k - 1] * (1.0 + 1e-6) + 1e-9);
    }
    CHECK(e[29] < start);
    /* Pins are honoured closely. */
    for (k = 0; k < 3; k++) {
        double p[2];
        CHECK(pc_algo_puppet_solver_pin_point(sv, k, (const double (*)[2])v, p));
        CHECK((p[0] - dst[k][0]) * (p[0] - dst[k][0]) + (p[1] - dst[k][1]) * (p[1] - dst[k][1]) < 0.25);
    }
    CHECK(!pc_algo_puppet_solver_pin_point(sv, 3, (const double (*)[2])v, (double[2]){0, 0}));
    CHECK(pc_algo_puppet_solve(sv, dst, 3, rot, 3, PC_ALGO_PUPPET_DISTORT, 30, NULL, 0, &vd, &ed, NULL) ==
          PC_OK);
    CHECK(memcmp(v, vd, nv * sizeof *v) != 0);
    free(v), free(e), free(v0), free(vd), free(ed);
    pc_algo_puppet_solver_free(sv);
    pc_surface_fini(&s);
}

static void test_depth_orders_triangles(void)
{
    static const double pins[2][2] = {{28, 14}, {64, 62}};
    static const int32_t depths[2] = {1, 0};
    bool fixed[2] = {false, false};
    pc_surface s;
    pc_algo_puppet_mesh m;
    pc_algo_puppet_solver *sv = NULL;
    const pc_algo_puppet_mesh *mm;
    size_t *order = NULL, i, k = SIZE_MAX;
    bool ok = true;
    blob(PC_SAMPLE_TYPE_U8, &s);
    CHECK(pc_algo_puppet_build_mesh(&s, pc_surface_content_bounds(&s), PC_ALGO_PUPPET_FEWER, 2.0, &m, NULL) ==
          PC_OK);
    CHECK(pc_algo_puppet_solver_new(&m, pins, 2, fixed, 2, &sv, NULL) == PC_OK);
    CHECK(pc_algo_puppet_draw_order(sv, depths, 2, pins, 2, &order, NULL) == PC_OK);
    mm = pc_algo_puppet_solver_mesh(sv);
    /* Triangles nearest the raised pin come last, all others first. */
    for (i = 0; i < mm->ntris; i++) {
        const size_t *t = mm->tris[order[i]];
        double cx = 0, cy = 0;
        bool near0;
        int q;
        for (q = 0; q < 3; q++) {
            cx = cx + mm->verts[t[q]][0] / 3.0;
            cy = cy + mm->verts[t[q]][1] / 3.0;
        }
        near0 = (cx - 28) * (cx - 28) + (cy - 14) * (cy - 14) < (cx - 64) * (cx - 64) + (cy - 62) * (cy - 62);
        if (near0 && k == SIZE_MAX) {
            k = i;
        }
        ok = ok && (k == SIZE_MAX ? !near0 : near0);
    }
    CHECK(ok && k != SIZE_MAX);
    free(order);
    pc_algo_puppet_solver_free(sv);
    pc_surface_fini(&s);
}

/* ---- never crash ---- */

static void test_degenerate_pins_never_crash(void)
{
    double nan = (double)NAN, inf = (double)INFINITY;
    double sets[6][3][2][2] = {
        {{{30, 30}, {30, 30}}, {{30, 30}, {50, 40}}, {{30, 30}, {20, 10}}},    /* coincident sources */
        {{{25, 20}, {25, 20}}, {{25, 40}, {35, 40}}, {{25, 60}, {45, 60}}},    /* colinear */
        {{{nan, 30}, {30, 30}}, {{28, 60}, {nan, nan}}, {{30, 30}, {30, 30}}}, /* NaN */
        {{{inf, 30}, {30, 30}}, {{28, 60}, {-inf, 1}}, {{30, 30}, {30, 30}}},  /* infinite */
        {{{-500, -500}, {0, 0}}, {{900, 900}, {1, 1}}, {{28, 60}, {28, 60}}},  /* outside the mesh */
        {{{28, 60}, {1e300, -1e300}}, {{30, 20}, {30, 20}}, {{30, 21}, {31, 22}}}};
    pc_algo_puppet_rot rots[3] = {{true, nan}, {true, 1e300}, {false, 0}};
    pc_surface s;
    int k, mode;
    blob(PC_SAMPLE_TYPE_F32, &s);
    for (k = 0; k < 6; k++) {
        double src[3][2], dst[3][2];
        bool fixed[3];
        int q;
        pc_algo_puppet_warp w = pc_algo_puppet_warp_make();
        for (q = 0; q < 3; q++) {
            memcpy(src[q], sets[k][q][0], 16);
            memcpy(dst[q], sets[k][q][1], 16);
            fixed[q] = rots[q].set && k >= 2;
        }
        for (mode = 0; mode < 3; mode++) {
            pc_algo_puppet_mesh m;
            pc_algo_puppet_solver *sv = NULL;
            double (*v)[2] = NULL, *e = NULL;
            size_t *order = NULL;
            int32_t depths[3] = {3, -1, 0};
            CHECK(pc_algo_puppet_build_mesh(&s, pc_surface_content_bounds(&s), PC_ALGO_PUPPET_FEWER, 2.0, &m,
                                            NULL) == PC_OK);
            CHECK(pc_algo_puppet_solver_new(&m, (const double (*)[2])src, 3, fixed, 3, &sv, NULL) == PC_OK);
            CHECK(pc_algo_puppet_solve(sv, (const double (*)[2])dst, 3, rots, k >= 2 ? 3 : 0,
                                       (pc_algo_puppet_mode)mode, 4, NULL, 0, &v, &e, NULL) == PC_OK);
            CHECK(pc_algo_puppet_draw_order(sv, depths, 3, (const double (*)[2])src, 3, &order, NULL) ==
                  PC_OK);
            free(v), free(e), free(order);
            pc_algo_puppet_solver_free(sv);
        }
        /* The whole warp: refused (too far / NaN extent) or rendered; never a crash. */
        w.pins = calloc(3, sizeof *w.pins);
        w.npins = 3;
        for (q = 0; q < 3; q++) {
            w.pins[q] = pin(src[q][0], src[q][1], dst[q][0], dst[q][1]);
        }
        {
            pc_surface out;
            pc_status st = pc_algo_puppet_apply(&s, pc_surface_content_bounds(&s), &w, PC_ALGO_INTERP_BICUBIC,
                                                &out, NULL);
            CHECK(st == PC_OK || st == PC_ERR_LIMIT);
            if (st == PC_OK) {
                pc_surface_fini(&out);
            }
        }
        pc_algo_puppet_warp_fini(&w);
    }
    pc_surface_fini(&s);
}

static void test_hostile_meshes_and_limits(void)
{
    pc_surface s, empty, opaque;
    pc_algo_puppet_mesh m;
    pc_algo_puppet_solver *sv = NULL;
    double (*v)[2] = NULL, *e = NULL;
    pc_error err;
    blob(PC_SAMPLE_TYPE_U8, &s);
    /* Huge bounds: the outline field's blocks grow with the layer, so its grid stays bounded. */
    {
        pc_status st = pc_algo_puppet_build_mesh(&s, pc_rect_make(INT32_MIN, 0, INT32_MAX, 10),
                                                 PC_ALGO_PUPPET_MORE, 2.0, &m, &err);
        CHECK(st == PC_OK || st == PC_ERR_LIMIT);
        if (st == PC_OK) {
            pc_algo_puppet_mesh_fini(&m);
        }
    }
    /* Empty bounds: the empty mesh; an empty surface: no coverage. */
    CHECK(pc_algo_puppet_build_mesh(&s, PC_RECT_EMPTY, PC_ALGO_PUPPET_MORE, 2.0, &m, NULL) == PC_OK);
    CHECK(m.nverts == 0 && m.ntris == 0 && m.spacing == 0.0);
    pc_surface_init(&empty, (pc_pixel_format){PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true});
    CHECK(pc_algo_puppet_build_mesh(&empty, pc_rect_make(0, 0, 50, 50), PC_ALGO_PUPPET_MORE, (double)NAN, &m,
                                    NULL) == PC_OK);
    CHECK(m.ntris == 0);
    /* Without alpha everything inside the bounds is covered. */
    pc_surface_init(&opaque, (pc_pixel_format){PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, false});
    CHECK(pc_algo_puppet_build_mesh(&opaque, pc_rect_make(0, 0, 50, 50), PC_ALGO_PUPPET_FEWER, 1e9, &m,
                                    NULL) == PC_OK);
    CHECK(m.ntris > 0);
    /* Iteration counts the reference would grind on for hours. */
    CHECK(pc_algo_puppet_solver_new(&m, NULL, 0, NULL, 0, &sv, NULL) == PC_OK);
    CHECK(pc_algo_puppet_solve(sv, NULL, 0, NULL, 0, PC_ALGO_PUPPET_NORMAL, (size_t)-1, NULL, 0, &v, &e,
                               &err) == PC_ERR_LIMIT);
    /* A warm start of the wrong length is ignored, like the reference. */
    CHECK(pc_algo_puppet_solve(sv, NULL, 0, NULL, 0, PC_ALGO_PUPPET_NORMAL, 1, (const double (*)[2])m.verts,
                               1, &v, &e, NULL) == PC_OK);
    free(v), free(e);
    pc_algo_puppet_solver_free(sv);
    pc_surface_fini(&s);
    pc_surface_fini(&empty);
    pc_surface_fini(&opaque);
}

static void test_json_and_parse(void)
{
    static const char *bad[] = {"[{\"src\":[1,2]}]",
                                "[{\"src\":[1],\"dst\":[1,2]}]",
                                "[{\"src\":[1,2,3],\"dst\":[1,2]}]",
                                "[{\"src\":[1,2],\"dst\":[1,2],\"depth\":1.5}]",
                                "[7]",
                                "{}"};
    const char *good =
        "{\"pins\":[{\"src\":[1,2],\"dst\":[3,4],\"rotate\":90,\"depth\":-3}],\"mode\":\"rigid\","
        "\"density\":\"more\",\"expansion\":-5}";
    yyjson_doc *d = NULL;
    pc_algo_puppet_pin *p = NULL;
    pc_algo_puppet_warp w;
    size_t n = 0, i;
    pc_error e;
    pc_algo_puppet_mode m;
    pc_algo_puppet_density dn;
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(pc_json_parse(bad[i], strlen(bad[i]), &d, NULL) == PC_OK);
        CHECK(pc_algo_puppet_pins_from_json(yyjson_doc_get_root(d), &p, &n, &e) == PC_ERR_INVALID);
        yyjson_doc_free(d);
    }
    CHECK(pc_json_parse(good, strlen(good), &d, NULL) == PC_OK);
    CHECK(pc_algo_puppet_warp_from_json(yyjson_doc_get_root(d), &w, NULL) == PC_OK);
    yyjson_doc_free(d);
    CHECK(w.npins == 1 && w.pins[0].rotate.set && w.pins[0].rotate.deg == 90.0 && w.pins[0].depth == -3);
    CHECK(w.mode == PC_ALGO_PUPPET_RIGID && w.density == PC_ALGO_PUPPET_MORE && w.expansion == -5.0);
    CHECK(!pc_algo_puppet_warp_is_identity(&w));
    w.pins[0].dst[0] = 1, w.pins[0].dst[1] = 2, w.pins[0].rotate.deg = -720.0;
    CHECK(pc_algo_puppet_warp_is_identity(&w));
    pc_algo_puppet_warp_fini(&w);
    CHECK(pc_algo_puppet_mode_parse("DISTORT", 7, &m) && m == PC_ALGO_PUPPET_DISTORT);
    CHECK(!pc_algo_puppet_mode_parse("normall", 7, &m));
    CHECK(pc_algo_puppet_density_parse("morePoints", 10, &dn) && dn == PC_ALGO_PUPPET_MORE);
    CHECK(!pc_algo_puppet_density_parse("", 0, &dn));
}

int main(int argc, char **argv)
{
    RUN(test_mesh_covers_the_shape_only);
    RUN(test_mesh_follows_the_outline);
    RUN(test_no_pins_or_unmoved_pins_are_identity);
    RUN(test_an_expansion_that_empties_the_mesh_warps_nothing);
    RUN(test_a_pin_dragged_absurdly_far_is_refused_before_rendering);
    RUN(test_a_single_pin_translates_rigidly);
    RUN(test_arap_energy_decreases_and_modes_differ);
    RUN(test_depth_orders_triangles);
    RUN(test_degenerate_pins_never_crash);
    RUN(test_hostile_meshes_and_limits);
    RUN(test_json_and_parse);
    return pc_t_finish();
}
