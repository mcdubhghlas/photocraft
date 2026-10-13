/* Ports crates/geom/src/warp.rs tests, adds robustness cases, and checks bit-exact parity with
 * the reference: test_matches_reference_bit_for_bit replays `rustref geom-vectors`. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "geom/warp.h"
#include "harness.h"

static const double B[4] = {10.0, 20.0, 110.0, 70.0};

static int close2(pc_point a, double x, double y, double eps)
{
    return fabs(a.x - x) < eps && fabs(a.y - y) < eps;
}

static void test_styles_parse_and_round_trip_names(void)
{
    int s;
    pc_warp_style out = PC_WARP_NONE;
    for (s = 0; s < PC_WARP_STYLE_COUNT; s++) {
        const char *names[3];
        int k;
        names[0] = pc_warp_style_id((pc_warp_style)s);
        names[1] = pc_warp_style_psd_name((pc_warp_style)s);
        names[2] = pc_warp_style_label((pc_warp_style)s);
        for (k = 0; k < 3; k++) {
            CHECK(pc_warp_style_parse(names[k], strlen(names[k]), &out) && out == (pc_warp_style)s);
        }
    }
    CHECK(!pc_warp_style_parse("spiral", 6, &out));
    CHECK(pc_warp_style_parse("  ARC\tlower ", 12, &out) && out == PC_WARP_ARC_LOWER);
    CHECK(pc_warp_style_parse("Shell\xc2\xa0Upper", 12, &out) && out == PC_WARP_SHELL_UPPER); /* NBSP */
    CHECK(pc_warp_style_parse("fish\xe3\x80\x80", 7, &out) && out == PC_WARP_FISH);           /* ideographic */
    CHECK(!pc_warp_style_parse("\xc3\xa1rc", 4, &out));   /* non-ASCII letter */
    CHECK(!pc_warp_style_parse("ar\xff" "c", 4, &out));   /* malformed UTF-8 */
    CHECK(!pc_warp_style_parse("ar\xe2\x80", 4, &out));   /* truncated UTF-8 */
    CHECK(!pc_warp_style_parse("", 0, &out));
    CHECK(!pc_warp_style_parse(NULL, 3, &out));
    CHECK(!pc_warp_style_parse("warpArcLowerXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX", 46, &out));
    CHECK_EQ_INT(PC_WARP_PRESETS[0], PC_WARP_ARC);
    CHECK_EQ_INT(PC_WARP_PRESETS[14], PC_WARP_TWIST);
    CHECK(!pc_warp_style_is_preset(PC_WARP_CUSTOM) && pc_warp_style_is_preset(PC_WARP_FLAG));
    CHECK_EQ_STR(pc_warp_style_label((pc_warp_style)999), "None");
}

static void test_presets_at_bend_zero_are_identity(void)
{
    int k;
    for (k = 0; k < 15; k++) {
        pc_warp w;
        pc_point p;
        pc_warp_preset(&w, PC_WARP_PRESETS[k], 0.0, B);
        CHECK(pc_warp_is_identity(&w));
        p = pc_warp_map(&w, 33.0, 44.0);
        CHECK(p.x == 33.0 && p.y == 44.0);
        pc_warp_fini(&w);
    }
}

static pc_point affine_f(void *ctx, double s, double t)
{
    (void)ctx;
    return pc_point_make(3.0 + 2.0 * s - t, 1.0 + 0.5 * s + 4.0 * t);
}

static void test_identity_mesh_is_exact_and_fit_reproduces_affine(void)
{
    static const double us[] = {0.0, 1.0};
    static const double vs[] = {0.0, 0.4, 1.0};
    static const double st[][2] = {{0.1, 0.9}, {0.5, 0.2}, {0.77, 0.41}};
    pc_mesh m;
    pc_warp w;
    size_t k;
    CHECK_EQ_INT(pc_mesh_identity(B, 1, 1, &m, NULL), PC_OK);
    CHECK(pc_mesh_nx(&m) == 4 && pc_mesh_ny(&m) == 4);
    pc_warp_custom(&w, &m, B);
    CHECK(m.points == NULL); /* ownership moved */
    CHECK(pc_warp_is_identity(&w));
    CHECK(close2(pc_warp_map(&w, 37.5, 61.25), 37.5, 61.25, 1e-9));
    pc_warp_fini(&w);
    CHECK_EQ_INT(pc_mesh_fit(affine_f, NULL, us, 2, vs, 3, &m, NULL), PC_OK);
    for (k = 0; k < 3; k++) {
        pc_point p = pc_mesh_eval(&m, st[k][0], st[k][1]);
        pc_point e = affine_f(NULL, st[k][0], st[k][1]);
        CHECK(close2(p, e.x, e.y, 1e-9));
    }
    pc_mesh_fini(&m);
}

static void sample(const pc_mesh *m, pc_point out[121])
{
    int i;
    int j;
    for (j = 0; j <= 10; j++) {
        for (i = 0; i <= 10; i++) {
            out[j * 11 + i] = pc_mesh_eval(m, i / 10.0, j / 10.0);
        }
    }
}

static void test_splits_are_exact_and_removable(void)
{
    pc_mesh m;
    pc_mesh orig;
    pc_point before[121];
    pc_point after[121];
    bool ok = false;
    double ls;
    double lt;
    size_t k;
    int bad = 0;
    CHECK_EQ_INT(pc_mesh_identity(B, 1, 1, &m, NULL), PC_OK);
    m.points[5] = pc_point_make(50.0, 10.0); /* bend it: move an inner control point */
    sample(&m, before);
    CHECK_EQ_INT(pc_mesh_clone(&m, &orig, NULL), PC_OK);
    CHECK(pc_mesh_split_u(&m, 0.3, &ok, NULL) == PC_OK && ok);
    CHECK(pc_mesh_split_v(&m, 0.6, &ok, NULL) == PC_OK && ok);
    CHECK(pc_mesh_split_u(&m, 0.3, &ok, NULL) == PC_OK && !ok); /* existing boundary */
    CHECK(pc_mesh_nx(&m) == 7 && pc_mesh_ny(&m) == 7);
    CHECK(pc_mesh_is_valid(&m));
    sample(&m, after);
    for (k = 0; k < 121; k++) {
        bad += !close2(after[k], before[k].x, before[k].y, 1e-9);
    }
    CHECK_EQ_INT(bad, 0);
    pc_mesh_param_at(&m, pc_mesh_eval(&m, 0.42, 0.77), &ls, &lt);
    CHECK_NEAR(ls, 0.42, 1e-6);
    CHECK_NEAR(lt, 0.77, 1e-6);
    CHECK(pc_mesh_remove_split_u(&m, 1, &ok, NULL) == PC_OK && ok);
    CHECK(pc_mesh_remove_split_v(&m, 1, &ok, NULL) == PC_OK && ok);
    CHECK_EQ_UINT(m.nus, orig.nus);
    CHECK(m.nus == orig.nus && memcmp(m.us, orig.us, m.nus * sizeof *m.us) == 0);
    bad = 0;
    for (k = 0; k < m.npoints && k < orig.npoints; k++) {
        bad += !close2(m.points[k], orig.points[k].x, orig.points[k].y, 1e-9);
    }
    CHECK_EQ_INT(bad, 0);
    CHECK(pc_mesh_remove_split_u(&m, 0, &ok, NULL) == PC_OK && !ok);
    CHECK(pc_mesh_remove_split_v(&m, 7, &ok, NULL) == PC_OK && !ok);
    pc_mesh_fini(&m);
    pc_mesh_fini(&orig);
}

/* Photoshop 27.11, measured: a 256 px grid image whose colours encode each pixel's own position,
 * warped in a 512 px document. Each pair is a source point and where Photoshop put it. */
typedef struct ps_case {
    pc_warp_style style;
    double bend, hd, vd;
    bool vertical;
    double box[4];
    int npairs;
    double pairs[3][4];
} ps_case;

#define SQUARE {128.0, 128.0, 384.0, 384.0}
#define WIDE {128.0, 208.0, 384.0, 304.0}
#define TALL {208.0, 128.0, 304.0, 384.0}

static void test_presets_match_photoshop(void)
{
    static const ps_case cases[] = {
        {PC_WARP_ARC,
         50.0,
         0.0,
         0.0,
         false,
         SQUARE,
         3,
         {{272.5, 155.5, 296.5, 104.5}, {331.5, 182.5, 424.5, 168.5}, {259.5, 157.5, 264.5, 104.5}}},
        {PC_WARP_ARC,
         -100.0,
         0.0,
         0.0,
         false,
         SQUARE,
         3,
         {{277.5, 354.5, 344.5, 472.5}, {140.5, 218.5, 40.5, 168.5}, {172.5, 237.5, 56.5, 264.5}}},
        {PC_WARP_FLAG,
         50.0,
         0.0,
         0.0,
         false,
         SQUARE,
         3,
         {{136.5, 143.5, 136.5, 120.5}, {136.5, 159.5, 136.5, 136.5}, {136.5, 175.5, 136.5, 152.5}}},
        {PC_WARP_TWIST,
         -50.0,
         0.0,
         0.0,
         false,
         SQUARE,
         3,
         {{148.5, 205.5, 152.5, 216.5}, {197.5, 212.5, 200.5, 232.5}, {134.5, 212.5, 136.5, 216.5}}},
        {PC_WARP_SHELL_LOWER,
         50.0,
         0.0,
         0.0,
         false,
         SQUARE,
         3,
         {{248.5, 136.5, 248.5, 136.5}, {264.5, 136.5, 264.5, 136.5}, {232.5, 136.5, 232.5, 136.5}}},
        {PC_WARP_FISH,
         -100.0,
         0.0,
         0.0,
         false,
         SQUARE,
         3,
         {{296.5, 355.5, 296.5, 440.5}, {344.5, 298.5, 344.5, 344.5}, {328.5, 356.5, 328.5, 472.5}}},
        {PC_WARP_BULGE,
         50.0,
         50.0,
         0.0,
         false,
         SQUARE,
         3,
         {{293.5, 147.5, 296.5, 88.5}, {293.5, 375.5, 296.5, 440.5}, {340.5, 281.5, 344.5, 296.5}}},
        {PC_WARP_ARC,
         50.0,
         0.0,
         -50.0,
         false,
         SQUARE,
         3,
         {{181.5, 350.5, 184.5, 328.5}, {291.5, 372.5, 280.5, 344.5}, {232.5, 349.5, 232.5, 312.5}}},
        {PC_WARP_ARC,
         50.0,
         0.0,
         0.0,
         true,
         SQUARE,
         3,
         {{155.5, 272.5, 104.5, 296.5}, {182.5, 331.5, 168.5, 424.5}, {157.5, 259.5, 104.5, 264.5}}},
        /* Wide (256 x 96) and tall (96 x 256) boxes, and a vertical warp with distortion. */
        {PC_WARP_ARC,
         50.0,
         0.0,
         0.0,
         false,
         WIDE,
         2,
         {{144.5, 269.6, 120.5, 264.5}, {271.5, 220.42, 280.5, 168.5}}},
        {PC_WARP_BULGE,
         50.0,
         0.0,
         0.0,
         false,
         TALL,
         2,
         {{249.11, 374.5, 248.5, 392.5}, {249.11, 360.5, 248.5, 376.5}}},
        {PC_WARP_FLAG,
         50.0,
         0.0,
         0.0,
         true,
         WIDE,
         2,
         {{137.5, 296.42, 184.5, 296.5}, {153.5, 296.42, 200.5, 296.5}}},
        {PC_WARP_RISE,
         50.0,
         0.0,
         0.0,
         true,
         TALL,
         2,
         {{250.97, 152.5, 344.5, 152.5}, {301.26, 264.5, 344.5, 264.5}}},
        {PC_WARP_BULGE,
         60.0,
         20.0,
         -10.0,
         false,
         WIDE,
         2,
         {{208.5, 224.15, 200.5, 184.5}, {219.5, 277.42, 216.5, 296.5}}},
        {PC_WARP_ARC,
         50.0,
         30.0,
         0.0,
         true,
         SQUARE,
         2,
         {{331.5, 327.5, 296.5, 360.5}, {318.5, 240.5, 264.5, 232.5}}},
    };
    size_t c;
    for (c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        const ps_case *k = &cases[c];
        pc_warp w;
        int i;
        pc_warp_preset(&w, k->style, k->bend, k->box);
        w.h_distort = k->hd;
        w.v_distort = k->vd;
        w.vertical = k->vertical;
        for (i = 0; i < k->npairs; i++) {
            pc_point got = pc_warp_map(&w, k->pairs[i][0], k->pairs[i][1]);
            double d = hypot(got.x - k->pairs[i][2], got.y - k->pairs[i][3]);
            if (!(d < 1.5)) {
                PC_T_FAIL("case %zu pair %d: (%g, %g) vs Photoshop (%g, %g)", c, i, got.x, got.y,
                          k->pairs[i][2], k->pairs[i][3]);
            }
        }
        pc_warp_fini(&w);
    }
}

#undef SQUARE
#undef WIDE
#undef TALL

/* The largest mismatch between the tangents on the two sides of any interior grid line. */
static double tangent_defect(const pc_mesh *m)
{
    size_t nx = pc_mesh_nx(m);
    size_t ny = pc_mesh_ny(m);
    double worst = 0.0;
    size_t a;
    size_t i;
    size_t j;
    for (a = 1; a + 1 < m->nus; a++) {
        double hl = m->us[a] - m->us[a - 1];
        double hr = m->us[a + 1] - m->us[a];
        for (j = 0; j < ny; j++) {
            pc_point l = m->points[j * nx + 3 * a - 1];
            pc_point c = m->points[j * nx + 3 * a];
            pc_point r = m->points[j * nx + 3 * a + 1];
            worst = fmax(worst, fabs((c.x - l.x) / hl - (r.x - c.x) / hr));
            worst = fmax(worst, fabs((c.y - l.y) / hl - (r.y - c.y) / hr));
        }
    }
    for (a = 1; a + 1 < m->nvs; a++) {
        double hl = m->vs[a] - m->vs[a - 1];
        double hr = m->vs[a + 1] - m->vs[a];
        for (i = 0; i < nx; i++) {
            pc_point l = m->points[(3 * a - 1) * nx + i];
            pc_point c = m->points[3 * a * nx + i];
            pc_point r = m->points[(3 * a + 1) * nx + i];
            worst = fmax(worst, fabs((c.x - l.x) / hl - (r.x - c.x) / hr));
            worst = fmax(worst, fabs((c.y - l.y) / hl - (r.y - c.y) / hr));
        }
    }
    return worst;
}

/* Fraction of the surface where the map folds over itself (its Jacobian is not positive). */
static double fold_fraction(const pc_mesh *m)
{
    const int n = 80;
    const double h = 1e-4;
    int bad = 0;
    int i;
    int j;
    for (j = 0; j < n; j++) {
        for (i = 0; i < n; i++) {
            double s = ((double)i + 0.5) / n;
            double t = ((double)j + 0.5) / n;
            pc_point q = pc_mesh_eval(m, s, t);
            pc_point qs = pc_mesh_eval(m, s + h, t);
            pc_point qt = pc_mesh_eval(m, s, t + h);
            if ((qs.x - q.x) * (qt.y - q.y) - (qt.x - q.x) * (qs.y - q.y) <= 0.0) {
                bad++;
            }
        }
    }
    return (double)bad / (double)(n * n);
}

static void split_both(pc_mesh *m, const double *at, size_t n)
{
    size_t i;
    bool ok;
    for (i = 0; i < n; i++) {
        CHECK(pc_mesh_split_u(m, at[i], &ok, NULL) == PC_OK && ok);
        CHECK(pc_mesh_split_v(m, at[i], &ok, NULL) == PC_OK && ok);
    }
}

static pc_mesh clone_mesh(const pc_mesh *m)
{
    pc_mesh c;
    memset(&c, 0, sizeof c);
    CHECK_EQ_INT(pc_mesh_clone(m, &c, NULL), PC_OK);
    return c;
}

static void test_dragging_an_anchor_or_handle_keeps_the_surface_smooth_across_the_lines(void)
{
    static const double box[4] = {0.0, 0.0, 300.0, 300.0};
    static const double cuts[3] = {0.3, 0.55, 0.9};
    static const size_t ij[12][2] = {{6, 6}, {7, 6}, {5, 6}, {6, 7}, {6, 5}, {3, 9},
                                     {9, 3}, {0, 6}, {6, 0}, {4, 3}, {3, 4}, {12, 12}};
    pc_mesh base;
    size_t nx;
    size_t k;
    CHECK_EQ_INT(pc_mesh_identity(box, 1, 1, &base, NULL), PC_OK);
    split_both(&base, cuts, 3);
    nx = pc_mesh_nx(&base);
    /* Every kind of point on a grid line, interior and border: anchors, handles each way. */
    for (k = 0; k < 12; k++) {
        size_t i = ij[k][0];
        size_t j = ij[k][1];
        pc_mesh m = clone_mesh(&base);
        pc_point b = base.points[j * nx + i];
        pc_mesh_drag_point(&m, base.points, base.npoints, j * nx + i, pc_point_make(23.0, -31.0));
        CHECK(m.points[j * nx + i].x == b.x + 23.0 && m.points[j * nx + i].y == b.y - 31.0);
        CHECK(tangent_defect(&m) < 1e-6);
        pc_mesh_fini(&m);
    }
    /* Ignored: wrong base size, index out of range, non-finite d. */
    {
        pc_mesh m = clone_mesh(&base);
        pc_mesh_drag_point(&m, base.points, base.npoints - 1, 0, pc_point_make(1.0, 1.0));
        pc_mesh_drag_point(&m, base.points, base.npoints, base.npoints, pc_point_make(1.0, 1.0));
        pc_mesh_drag_point(&m, base.points, base.npoints, 0, pc_point_make(NAN, 1.0));
        CHECK(memcmp(m.points, base.points, base.npoints * sizeof *m.points) == 0);
        pc_mesh_fini(&m);
    }
    pc_mesh_fini(&base);
}

static void test_a_pull_within_a_quarter_cell_never_folds_the_surface(void)
{
    /* 75 px cells: dragging a quarter of a cell, any direction, anywhere, must not tear the picture. */
    static const double box[4] = {0.0, 0.0, 300.0, 300.0};
    static const double st[6][2] = {{0.42, 0.42}, {0.75, 0.2}, {0.2, 0.2},
                                    {0.5, 0.5},   {0.85, 0.7}, {0.3, 0.7}};
    static const double dir[6][2] = {{-1.0, 0.0}, {1.0, 0.0}, {0.0, -1.0},
                                     {0.0, 1.0},  {0.7, 0.7}, {-0.7, 0.7}};
    pc_mesh base;
    size_t a;
    size_t b;
    CHECK_EQ_INT(pc_mesh_identity(box, 4, 4, &base, NULL), PC_OK);
    for (a = 0; a < 6; a++) {
        for (b = 0; b < 6; b++) {
            pc_mesh m = clone_mesh(&base);
            CHECK_EQ_INT(pc_mesh_pull(&m, base.points, base.npoints, st[a][0], st[a][1],
                                      pc_point_make(dir[b][0] * 19.0, dir[b][1] * 19.0), NULL),
                         PC_OK);
            if (fold_fraction(&m) != 0.0) {
                PC_T_FAIL("(%g, %g) by (%g, %g) quarter cells folds", st[a][0], st[a][1], dir[b][0],
                          dir[b][1]);
            }
            pc_mesh_fini(&m);
        }
    }
    pc_mesh_fini(&base);
}

static bool border_unmoved(const pc_mesh *m, const pc_mesh *base)
{
    size_t nx = pc_mesh_nx(base);
    size_t ny = pc_mesh_ny(base);
    size_t i;
    size_t j;
    for (j = 0; j < ny; j++) {
        for (i = 0; i < nx; i++) {
            if (i == 0 || j == 0 || i == nx - 1 || j == ny - 1) {
                pc_point a = m->points[j * nx + i];
                pc_point b = base->points[j * nx + i];
                if (!(fabs(a.x - b.x) < 1e-6 && fabs(a.y - b.y) < 1e-6)) {
                    return false;
                }
            }
        }
    }
    return true;
}

static void test_a_pull_in_the_middle_leaves_the_border_where_it_is(void)
{
    static const double box[4] = {0.0, 0.0, 600.0, 800.0};
    pc_mesh base;
    pc_mesh m;
    pc_point before;
    pc_point after;
    bool ok;
    CHECK_EQ_INT(pc_mesh_identity(box, 1, 1, &base, NULL), PC_OK);
    CHECK(pc_mesh_split_u(&base, 0.33, &ok, NULL) == PC_OK && ok);
    CHECK(pc_mesh_split_u(&base, 0.66, &ok, NULL) == PC_OK && ok);
    CHECK(pc_mesh_split_v(&base, 0.5, &ok, NULL) == PC_OK && ok);
    m = clone_mesh(&base);
    CHECK_EQ_INT(pc_mesh_pull(&m, base.points, base.npoints, 0.5, 0.5, pc_point_make(-180.0, 40.0), NULL),
                 PC_OK);
    CHECK(border_unmoved(&m, &base));
    /* The pull itself still lands exactly. */
    before = pc_mesh_eval(&base, 0.5, 0.5);
    after = pc_mesh_eval(&m, 0.5, 0.5);
    CHECK(fabs(after.x - before.x + 180.0) < 1e-4 && fabs(after.y - before.y - 40.0) < 1e-4);
    pc_mesh_fini(&m);
    pc_mesh_fini(&base);
}

static void test_a_pull_anywhere_away_from_the_border_never_moves_the_border(void)
{
    /* The default one-patch grid and a cut one; grabs all over, none within 12% of an edge. */
    static const double box[4] = {0.0, 0.0, 600.0, 800.0};
    static const double st[6][2] = {{0.5, 0.5},  {0.2, 0.5},   {0.8, 0.3},
                                    {0.3, 0.85}, {0.14, 0.14}, {0.86, 0.5}};
    static const double ds[3][2] = {{-400.0, 0.0}, {250.0, -90.0}, {0.0, 300.0}};
    pc_mesh bases[2];
    size_t q;
    bool ok;
    CHECK_EQ_INT(pc_mesh_identity(box, 1, 1, &bases[0], NULL), PC_OK);
    bases[1] = clone_mesh(&bases[0]);
    CHECK(pc_mesh_split_u(&bases[1], 0.4, &ok, NULL) == PC_OK && ok);
    CHECK(pc_mesh_split_u(&bases[1], 0.7, &ok, NULL) == PC_OK && ok);
    CHECK(pc_mesh_split_v(&bases[1], 0.35, &ok, NULL) == PC_OK && ok);
    CHECK(pc_mesh_split_v(&bases[1], 0.8, &ok, NULL) == PC_OK && ok);
    for (q = 0; q < 2; q++) {
        const pc_mesh *base = &bases[q];
        size_t a;
        size_t b;
        for (a = 0; a < 6; a++) {
            for (b = 0; b < 3; b++) {
                pc_mesh m = clone_mesh(base);
                pc_point before;
                pc_point after;
                CHECK_EQ_INT(pc_mesh_pull(&m, base->points, base->npoints, st[a][0], st[a][1],
                                          pc_point_make(ds[b][0], ds[b][1]), NULL),
                             PC_OK);
                if (!border_unmoved(&m, base)) {
                    PC_T_FAIL("mesh %zu (%g, %g) by d%zu moved the border", q, st[a][0], st[a][1], b);
                }
                before = pc_mesh_eval(base, st[a][0], st[a][1]);
                after = pc_mesh_eval(&m, st[a][0], st[a][1]);
                CHECK(fabs(after.x - before.x - ds[b][0]) < 1e-3 &&
                      fabs(after.y - before.y - ds[b][1]) < 1e-3);
                pc_mesh_fini(&m);
            }
        }
    }
    pc_mesh_fini(&bases[0]);
    pc_mesh_fini(&bases[1]);
}

static void test_pulling_on_the_border_bows_it_and_leaves_the_corners_put(void)
{
    /* A one-patch grid, grabbed on the middle of its right edge and pulled 170 px out. */
    static const double box[4] = {0.0, 0.0, 800.0, 1200.0};
    static const double corners[4][2] = {{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}, {1.0, 1.0}};
    pc_mesh base;
    pc_mesh m;
    pc_point at;
    size_t nx;
    size_t k;
    size_t nmoved = 0;
    size_t moved[16];
    CHECK_EQ_INT(pc_mesh_identity(box, 1, 1, &base, NULL), PC_OK);
    m = clone_mesh(&base);
    CHECK_EQ_INT(pc_mesh_pull(&m, base.points, base.npoints, 1.0, 0.5, pc_point_make(170.0, 0.0), NULL),
                 PC_OK);
    at = pc_mesh_eval(&m, 1.0, 0.5);
    CHECK(fabs(at.x - 970.0) < 1e-3 && fabs(at.y - 600.0) < 1e-3);
    for (k = 0; k < 4; k++) {
        pc_point a = pc_mesh_eval(&m, corners[k][0], corners[k][1]);
        pc_point b = pc_mesh_eval(&base, corners[k][0], corners[k][1]);
        CHECK(fabs(a.x - b.x) < 1e-6 && fabs(a.y - b.y) < 1e-6);
    }
    /* The other three edges did not move. */
    for (k = 0; k <= 10; k++) {
        double u = (double)k / 10.0;
        double e[3][2] = {{0.0, u}, {u, 0.0}, {u, 1.0}};
        size_t q;
        for (q = 0; q < 3; q++) {
            pc_point a = pc_mesh_eval(&m, e[q][0], e[q][1]);
            pc_point b = pc_mesh_eval(&base, e[q][0], e[q][1]);
            CHECK(fabs(a.x - b.x) < 1e-6 && fabs(a.y - b.y) < 1e-6);
        }
    }
    /* Only the right edge's two handles carry the pull. */
    nx = pc_mesh_nx(&base);
    for (k = 0; k < m.npoints; k++) {
        if (m.points[k].x != base.points[k].x || m.points[k].y != base.points[k].y) {
            moved[nmoved++] = k;
        }
    }
    CHECK_EQ_UINT(nmoved, 2);
    CHECK(nmoved == 2 && moved[0] == nx + 3 && moved[1] == 2 * nx + 3);
    CHECK(fold_fraction(&m) == 0.0);
    pc_mesh_fini(&m);
    pc_mesh_fini(&base);
}

static void test_pull_keeps_the_surface_smooth_across_the_grid_lines(void)
{
    /* A grid cut unevenly both ways, then pulled hard inside one cell and then another. */
    static const double box[4] = {0.0, 0.0, 300.0, 300.0};
    static const double cuts[3] = {0.3, 0.55, 0.9};
    static const double cases[3][4] = {
        {0.42, 0.42, 60.0, -35.0}, {0.75, 0.2, -40.0, 50.0}, {0.1, 0.95, 30.0, 30.0}};
    pc_mesh base;
    size_t k;
    CHECK_EQ_INT(pc_mesh_identity(box, 1, 1, &base, NULL), PC_OK);
    split_both(&base, cuts, 3);
    CHECK(tangent_defect(&base) < 1e-9); /* cuts are exact */
    for (k = 0; k < 3; k++) {
        pc_mesh m = clone_mesh(&base);
        pc_point before;
        pc_point after;
        CHECK_EQ_INT(pc_mesh_pull(&m, base.points, base.npoints, cases[k][0], cases[k][1],
                                  pc_point_make(cases[k][2], cases[k][3]), NULL),
                     PC_OK);
        CHECK(tangent_defect(&m) < 1e-6);
        before = pc_mesh_eval(&base, cases[k][0], cases[k][1]);
        after = pc_mesh_eval(&m, cases[k][0], cases[k][1]);
        CHECK(fabs(after.x - before.x - cases[k][2]) < 1e-4 && fabs(after.y - before.y - cases[k][3]) < 1e-4);
        pc_mesh_fini(&m);
    }
    pc_mesh_fini(&base);
}

static void test_pull_moves_the_grabbed_surface_point_exactly_and_stays_local(void)
{
    static const double st[5][2] = {{0.5, 0.5}, {0.1, 0.9}, {0.4, 0.25}, {0.0, 0.5}, {1.0, 0.3}};
    pc_mesh base;
    pc_mesh m;
    size_t nx;
    size_t ny;
    size_t k;
    bool last_moved = false;
    bool any_moved = false;
    CHECK_EQ_INT(pc_mesh_identity(B, 3, 2, &base, NULL), PC_OK);
    for (k = 0; k < 5; k++) {
        pc_point before = pc_mesh_eval(&base, st[k][0], st[k][1]);
        pc_point after;
        m = clone_mesh(&base);
        CHECK_EQ_INT(
            pc_mesh_pull(&m, base.points, base.npoints, st[k][0], st[k][1], pc_point_make(7.0, -4.5), NULL),
            PC_OK);
        after = pc_mesh_eval(&m, st[k][0], st[k][1]);
        CHECK(fabs(after.x - before.x - 7.0) < 1e-4 && fabs(after.y - before.y + 4.5) < 1e-4);
        pc_mesh_fini(&m);
    }
    /* The pull stays near the grabbed patch: the far end of the grid does not move. */
    m = clone_mesh(&base);
    CHECK_EQ_INT(pc_mesh_pull(&m, base.points, base.npoints, 0.1, 0.25, pc_point_make(5.0, 5.0), NULL),
                 PC_OK);
    nx = pc_mesh_nx(&base);
    ny = pc_mesh_ny(&base);
#define MOVED(q)                                                                                             \
    (fabs(m.points[q].x - base.points[q].x) > 1e-6 || fabs(m.points[q].y - base.points[q].y) > 1e-6)
    for (k = 0; k < ny; k++) {
        last_moved = last_moved || MOVED(k * nx + nx - 1);
    }
    for (k = 0; k < nx; k++) {
        any_moved = any_moved || MOVED(k);
    }
#undef MOVED
    CHECK(!last_moved);
    CHECK(any_moved);
    /* Pulling again from the same base replaces the pull (a drag is not cumulative). */
    CHECK_EQ_INT(pc_mesh_pull(&m, base.points, base.npoints, 0.5, 0.25, pc_point_make(0.0, 0.0), NULL),
                 PC_OK);
    for (k = 0; k < m.npoints; k++) {
        CHECK(fabs(m.points[k].x - base.points[k].x) < 1e-9 && fabs(m.points[k].y - base.points[k].y) < 1e-9);
    }
    /* Ignored: a base of the wrong size or a non-finite pull; a malformed mesh is an error. */
    CHECK_EQ_INT(pc_mesh_pull(&m, base.points, base.npoints - 1, 0.5, 0.5, pc_point_make(9.0, 9.0), NULL),
                 PC_OK);
    CHECK_EQ_INT(pc_mesh_pull(&m, base.points, base.npoints, 0.5, 0.5, pc_point_make(INFINITY, 0.0), NULL),
                 PC_OK);
    CHECK(memcmp(m.points, base.points, base.npoints * sizeof *m.points) == 0);
    CHECK_EQ_INT(pc_mesh_pull(&m, base.points, base.npoints, NAN, 0.5, pc_point_make(1.0, 0.0), NULL), PC_OK);
    pc_mesh_fini(&m);
    {
        pc_mesh bad;
        memset(&bad, 0, sizeof bad);
        CHECK_EQ_INT(pc_mesh_pull(&bad, base.points, base.npoints, 0.5, 0.5, pc_point_make(1.0, 0.0), NULL),
                     PC_ERR_INVALID);
    }
    pc_mesh_fini(&base);
}

static void test_preset_fits_closely_as_mesh_and_affine_composes(void)
{
    static const double xy[][2] = {{20.0, 30.0}, {60.0, 45.0}, {100.0, 65.0}};
    static const double shift[6] = {1.0, 0.0, 0.0, 1.0, 5.0, -2.0};
    pc_warp w;
    pc_warp mw;
    pc_warp id;
    pc_warp t;
    pc_mesh m;
    double ob[4];
    size_t k;
    pc_warp_preset(&w, PC_WARP_BULGE, 40.0, B);
    CHECK_EQ_INT(pc_warp_to_mesh(&w, 4, 4, &m, NULL), PC_OK);
    pc_warp_custom(&mw, &m, B);
    for (k = 0; k < 3; k++) {
        pc_point a = pc_warp_map(&w, xy[k][0], xy[k][1]);
        CHECK(close2(pc_warp_map(&mw, xy[k][0], xy[k][1]), a.x, a.y, 0.5));
    }
    CHECK_EQ_INT(pc_mesh_identity(B, 1, 1, &m, NULL), PC_OK);
    pc_warp_custom(&id, &m, B);
    CHECK_EQ_INT(pc_warp_then_affine(&id, shift, &t, NULL), PC_OK);
    CHECK(close2(pc_warp_map(&t, 50.0, 50.0), 55.0, 48.0, 1e-9));
    pc_warp_none(&w, B);
    pc_warp_output_bounds(&w, ob);
    CHECK_NEAR(ob[0], 10.0, 1e-9);
    CHECK_NEAR(ob[3], 70.0, 1e-9);
    /* then_affine may write over its own input (its old mesh is released, not leaked). */
    CHECK_EQ_INT(pc_warp_then_affine(&t, shift, &t, NULL), PC_OK);
    CHECK(close2(pc_warp_map(&t, 50.0, 50.0), 60.0, 46.0, 1e-9));
    pc_warp_fini(&mw);
    pc_warp_fini(&id);
    pc_warp_fini(&t);
}

static void test_malformed_meshes_never_crash(void)
{
    pc_mesh m;
    pc_mesh bad;
    pc_warp w;
    pc_point p;
    pc_error e;
    bool ok = true;
    double knots[2] = {0.0, 1.0};
    double s;
    double t;
    double cb[4];
    memset(&bad, 0, sizeof bad);
    CHECK(!pc_mesh_is_valid(&bad));
    p = pc_mesh_eval(&bad, 0.5, 0.5);
    CHECK(isnan(p.x) && isnan(p.y));
    CHECK_EQ_INT(pc_mesh_split_u(&bad, 0.5, &ok, &e), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_mesh_split_v(&bad, 0.5, &ok, &e), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_mesh_remove_split_u(&bad, 1, &ok, &e), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_mesh_fit(affine_f, NULL, knots, 1, knots, 2, &m, &e), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_mesh_fit(affine_f, NULL, knots, 2, knots, (size_t)-1 / 2, &m, &e), PC_ERR_LIMIT);
    CHECK(!pc_mesh_point(&bad, 0, 0, &p));
    pc_mesh_param_at(&bad, pc_point_make(1, 1), &s, &t);
    pc_mesh_control_bounds(&bad, cb);
    /* A custom warp holding a broken mesh maps to NaN instead of crashing, and is not identity. */
    pc_warp_custom(&w, &bad, B);
    CHECK(!pc_warp_is_identity(&w));
    p = pc_warp_map(&w, 50.0, 50.0);
    CHECK(isnan(p.x));
    pc_warp_fini(&w);
    /* Valid structure but non-finite values: is_valid says no, evaluation still safe. */
    CHECK_EQ_INT(pc_mesh_identity(B, 2, 2, &m, NULL), PC_OK);
    m.points[3].x = INFINITY;
    CHECK(!pc_mesh_is_valid(&m));
    m.us[1] = 0.0; /* knots no longer increasing */
    m.points[3].x = 1.0;
    CHECK(!pc_mesh_is_valid(&m));
    (void)pc_mesh_eval(&m, 0.25, 0.75);
    CHECK(pc_mesh_point(&m, 6, 6, &p) && !pc_mesh_point(&m, 7, 0, &p));
    pc_mesh_fini(&m);
    /* Patch counts are clamped to 1..64. */
    CHECK_EQ_INT(pc_mesh_identity(B, 0, 1000, &m, NULL), PC_OK);
    CHECK(pc_mesh_nx(&m) == 4 && pc_mesh_ny(&m) == 193);
    pc_mesh_fini(&m);
    /* Style warps reject empty boxes and non-finite strengths. */
    {
        pc_style_warp sw;
        static const double empty[4] = {5.0, 5.0, 5.0, 9.0};
        CHECK(!pc_style_warp_init(&sw, PC_WARP_ARC, 50.0, 0, 0, false, empty));
        CHECK(!pc_style_warp_init(&sw, PC_WARP_ARC, NAN, INFINITY, 0, false, B));
        CHECK(!pc_style_warp_init(&sw, PC_WARP_CUSTOM, 50.0, 0, 0, false, B));
        pc_style_warp full;
        CHECK(pc_style_warp_init(&sw, PC_WARP_ARC, 1e300, 0, 0, false, B));
        CHECK(pc_style_warp_init(&full, PC_WARP_ARC, 100.0, 0, 0, false, B));
        CHECK(memcmp(sw.grid, full.grid, sizeof sw.grid) == 0); /* the bend clamps to 100% */
        CHECK_NEAR(pc_style_warp_max_segment(&sw), 50.0 / 32.0, 0.0);
    }
}

/* ---- bit-exact parity with `rustref geom-vectors` ---- */

static void fnv(uint64_t *h, const uint8_t *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        *h ^= b[i];
        *h *= 0x100000001b3u;
    }
}

static void put(uint64_t *h, double v)
{
    uint8_t t[8];
    pc_store_le64(t, pc_f64_to_bits(v));
    fnv(h, t, 8);
}

static void put_byte(uint64_t *h, int v)
{
    uint8_t b = (uint8_t)(v != 0);
    fnv(h, &b, 1);
}

static void grid(const pc_mesh *m, uint64_t *h)
{
    int i;
    int j;
    for (j = 0; j <= 20; j++) {
        for (i = 0; i <= 20; i++) {
            pc_point p = pc_mesh_eval(m, (double)i / 20.0, (double)j / 20.0);
            put(h, p.x);
            put(h, p.y);
        }
    }
}

static void pts(const pc_mesh *m, uint64_t *h)
{
    size_t k;
    for (k = 0; k < m->npoints; k++) {
        put(h, m->points[k].x);
        put(h, m->points[k].y);
    }
}

static void test_matches_reference_bit_for_bit(void)
{
    static const double bends[] = {-100.0, -37.5, 0.0, 25.0, 100.0};
    static const double dist[2][2] = {{0.0, 0.0}, {30.0, -20.0}};
    static const double st[5][2] = {{0.42, 0.77}, {0.05, 0.95}, {0.5, 0.5}, {0.9, 0.1}, {0.3, 0.6}};
    static const double aff[6] = {0.9, 0.1, -0.2, 1.1, 5.0, -3.0};
    uint64_t hs = 0xcbf29ce484222325u;
    uint64_t hm = 0xcbf29ce484222325u;
    uint64_t ha = 0xcbf29ce484222325u;
    int style;
    pc_mesh m;
    pc_mesh fit;
    pc_warp bulge;
    pc_warp t;
    pc_warp idw;
    bool ok = false;
    size_t k;
    int i;
    int j;

    for (style = 0; style < PC_WARP_STYLE_COUNT; style++) {
        size_t bi;
        size_t di;
        int vertical;
        for (bi = 0; bi < 5; bi++) {
            for (di = 0; di < 2; di++) {
                for (vertical = 0; vertical <= 1; vertical++) {
                    pc_warp w;
                    double ob[4];
                    pc_warp_none(&w, B);
                    w.style = (pc_warp_style)style;
                    w.bend = bends[bi];
                    w.h_distort = dist[di][0];
                    w.v_distort = dist[di][1];
                    w.vertical = vertical != 0;
                    for (j = -2; j <= 12; j++) {
                        for (i = -2; i <= 12; i++) {
                            pc_point p = pc_warp_map(&w, 10.0 + 100.0 * (double)i / 10.0, 20.0 + 50.0 * (double)j / 10.0);
                            put(&hs, p.x);
                            put(&hs, p.y);
                        }
                    }
                    put_byte(&hs, pc_warp_is_identity(&w));
                    pc_warp_output_bounds(&w, ob);
                    for (k = 0; k < 4; k++) {
                        put(&hs, ob[k]);
                    }
                }
            }
        }
    }
    CHECK_EQ_UINT(hs, 0xdd39a8a9c293e4d0u);

    CHECK_EQ_INT(pc_mesh_identity(B, 1, 1, &m, NULL), PC_OK);
    m.points[5] = pc_point_make(50.0, 10.0);
    m.points[10] = pc_point_make(70.0, 65.0);
    grid(&m, &hm);
    CHECK_EQ_INT(pc_mesh_split_u(&m, 0.3, &ok, NULL), PC_OK);
    put_byte(&hm, ok);
    CHECK_EQ_INT(pc_mesh_split_v(&m, 0.6, &ok, NULL), PC_OK);
    put_byte(&hm, ok);
    CHECK_EQ_INT(pc_mesh_split_u(&m, 0.75, &ok, NULL), PC_OK);
    put_byte(&hm, ok);
    CHECK_EQ_INT(pc_mesh_split_u(&m, 0.3, &ok, NULL), PC_OK);
    put_byte(&hm, ok);
    pts(&m, &hm);
    grid(&m, &hm);
    for (k = 0; k < 5; k++) {
        double ps;
        double pt;
        pc_mesh_param_at(&m, pc_mesh_eval(&m, st[k][0], st[k][1]), &ps, &pt);
        put(&hm, ps);
        put(&hm, pt);
    }
    CHECK_EQ_INT(pc_mesh_remove_split_u(&m, 1, &ok, NULL), PC_OK);
    put_byte(&hm, ok);
    CHECK_EQ_INT(pc_mesh_remove_split_v(&m, 1, &ok, NULL), PC_OK);
    put_byte(&hm, ok);
    CHECK_EQ_INT(pc_mesh_remove_split_u(&m, 9, &ok, NULL), PC_OK);
    put_byte(&hm, ok);
    pts(&m, &hm);
    {
        double cb[4];
        pc_mesh_control_bounds(&m, cb);
        for (k = 0; k < 4; k++) {
            put(&hm, cb[k]);
        }
    }
    pc_warp_preset(&bulge, PC_WARP_BULGE, 40.0, B);
    CHECK_EQ_INT(pc_warp_to_mesh(&bulge, 4, 4, &fit, NULL), PC_OK);
    pts(&fit, &hm);
    pc_mesh_fini(&fit);
    CHECK_EQ_INT(pc_warp_then_affine(&bulge, aff, &t, NULL), PC_OK);
    for (j = 0; j <= 10; j++) {
        for (i = 0; i <= 10; i++) {
            pc_point p = pc_warp_map(&t, 10.0 + 10.0 * (double)i, 20.0 + 5.0 * (double)j);
            put(&hm, p.x);
            put(&hm, p.y);
        }
    }
    put_byte(&hm, pc_warp_is_identity(&t));
    CHECK_EQ_INT(pc_mesh_identity(B, 3, 2, &fit, NULL), PC_OK);
    pc_warp_custom(&idw, &fit, B);
    put_byte(&hm, pc_warp_is_identity(&idw));
    CHECK_EQ_UINT(hm, 0x526fd75c6c1475d2u);
    pc_mesh_fini(&m);
    pc_warp_fini(&t);
    pc_warp_fini(&idw);
    pc_warp_fini(&bulge);

    {
        pc_affine a = pc_affine_then(pc_affine_then(pc_affine_translate(5.0, -3.0), pc_affine_rotate(0.7)),
                                     pc_affine_scale(2.5));
        pc_affine inv = PC_AFFINE_IDENTITY;
        pc_point q;
        CHECK(pc_affine_inverse(a, &inv));
        for (k = 0; k < 6; k++) {
            put(&ha, a.m[k]);
        }
        for (k = 0; k < 6; k++) {
            put(&ha, inv.m[k]);
        }
        q = pc_affine_apply(inv, pc_affine_apply(a, pc_point_make(12.0, -7.5)));
        put(&ha, q.x);
        put(&ha, q.y);
        put(&ha, pc_affine_determinant(a));
    }
    CHECK_EQ_UINT(ha, 0x0f18ca65fb1edf0au);
}

/* rustref geom-vectors `pull`: pc_mesh_pull and pc_mesh_drag_point bit for bit. */
static void test_pull_and_drag_match_reference(void)
{
    static const double one_box[4] = {0.0, 0.0, 600.0, 800.0};
    static const double cut_box[4] = {0.0, 0.0, 300.0, 300.0};
    static const double cuts[3] = {0.3, 0.55, 0.9};
    static const double st[8][2] = {{0.5, 0.5}, {0.1, 0.9},   {0.42, 0.42}, {0.0, 0.5},
                                    {1.0, 0.3}, {0.05, 0.02}, {0.86, 0.5},  {0.97, 0.99}};
    static const double ds[4][2] = {{7.0, -4.5}, {-180.0, 40.0}, {170.0, 0.0}, {0.0, 0.0}};
    uint64_t h = 0xcbf29ce484222325u;
    pc_mesh bases[3];
    size_t q;
    bool ok;
    size_t c;
    CHECK_EQ_INT(pc_mesh_identity(one_box, 1, 1, &bases[0], NULL), PC_OK);
    CHECK_EQ_INT(pc_mesh_identity(cut_box, 1, 1, &bases[1], NULL), PC_OK);
    for (c = 0; c < 3; c++) {
        CHECK_EQ_INT(pc_mesh_split_u(&bases[1], cuts[c], &ok, NULL), PC_OK);
        CHECK_EQ_INT(pc_mesh_split_v(&bases[1], cuts[c], &ok, NULL), PC_OK);
    }
    CHECK_EQ_INT(pc_mesh_identity(B, 3, 2, &bases[2], NULL), PC_OK);
    for (q = 0; q < 3; q++) {
        const pc_mesh *base = &bases[q];
        size_t nx = pc_mesh_nx(base);
        size_t len = base->npoints;
        size_t idx[13];
        size_t a;
        size_t b;
        idx[0] = 0;
        idx[1] = 1;
        idx[2] = 2;
        idx[3] = 3;
        idx[4] = 4;
        idx[5] = nx;
        idx[6] = nx + 1;
        idx[7] = 2 * nx + 3;
        idx[8] = 3 * nx + 3;
        idx[9] = 3 * nx + 4;
        idx[10] = 4 * nx + 3;
        idx[11] = len - 1;
        idx[12] = len / 2;
        for (a = 0; a < 8; a++) {
            for (b = 0; b < 4; b++) {
                pc_mesh m = clone_mesh(base);
                CHECK_EQ_INT(pc_mesh_pull(&m, base->points, len, st[a][0], st[a][1],
                                          pc_point_make(ds[b][0], ds[b][1]), NULL),
                             PC_OK);
                pts(&m, &h);
                pc_mesh_fini(&m);
            }
        }
        for (a = 0; a < 13; a++) {
            pc_mesh m = clone_mesh(base);
            pc_mesh_drag_point(&m, base->points, len, idx[a] < len - 1 ? idx[a] : len - 1,
                               pc_point_make(23.0, -31.0));
            pts(&m, &h);
            pc_mesh_fini(&m);
        }
    }
    CHECK_EQ_UINT(h, 0x0fa88a4538d36d24u);
    for (q = 0; q < 3; q++) {
        pc_mesh_fini(&bases[q]);
    }
}

int main(int argc, char **argv)
{
    RUN(test_styles_parse_and_round_trip_names);
    RUN(test_presets_at_bend_zero_are_identity);
    RUN(test_identity_mesh_is_exact_and_fit_reproduces_affine);
    RUN(test_splits_are_exact_and_removable);
    RUN(test_presets_match_photoshop);
    RUN(test_dragging_an_anchor_or_handle_keeps_the_surface_smooth_across_the_lines);
    RUN(test_a_pull_within_a_quarter_cell_never_folds_the_surface);
    RUN(test_a_pull_in_the_middle_leaves_the_border_where_it_is);
    RUN(test_a_pull_anywhere_away_from_the_border_never_moves_the_border);
    RUN(test_pulling_on_the_border_bows_it_and_leaves_the_corners_put);
    RUN(test_pull_keeps_the_surface_smooth_across_the_grid_lines);
    RUN(test_pull_moves_the_grabbed_surface_point_exactly_and_stays_local);
    RUN(test_preset_fits_closely_as_mesh_and_affine_composes);
    RUN(test_malformed_meshes_never_crash);
    RUN(test_matches_reference_bit_for_bit);
    RUN(test_pull_and_drag_match_reference);
    return pc_t_finish();
}
