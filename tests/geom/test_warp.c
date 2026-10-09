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

static void test_arc_maps_known_points(void)
{
    static const double bb[4] = {0.0, -40.0, 200.0, 10.0};
    const double frac_pi_4 = 0.785398163397448309616;
    pc_warp w;
    double theta = frac_pi_4;
    double r = 4.0 / frac_pi_4;
    double ex = 100.0 - r * sin(theta) * 100.0 / 4.0;
    double ey = -15.0 + (r - r * cos(theta)) * 25.0;
    pc_warp_preset(&w, PC_WARP_ARC, 50.0, bb);
    CHECK(close2(pc_warp_map(&w, 100.0, -15.0), 100.0, -15.0, 1e-9));
    CHECK(close2(pc_warp_map(&w, 0.0, -15.0), ex, ey, 1e-6));
    pc_warp_fini(&w);
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
        CHECK(pc_style_warp_init(&sw, PC_WARP_ARC, 1e300, 0, 0, false, B) && sw.b == 1.0);
        CHECK_NEAR(pc_style_warp_max_segment(&sw), 50.0 / 2.0 / 16.0, 0.0);
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
    CHECK_EQ_UINT(hs, 0xdc0940307f83a579u);

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
    CHECK_EQ_UINT(hm, 0xaf13f292b4459dd7u);
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

int main(int argc, char **argv)
{
    RUN(test_styles_parse_and_round_trip_names);
    RUN(test_presets_at_bend_zero_are_identity);
    RUN(test_identity_mesh_is_exact_and_fit_reproduces_affine);
    RUN(test_splits_are_exact_and_removable);
    RUN(test_arc_maps_known_points);
    RUN(test_preset_fits_closely_as_mesh_and_affine_composes);
    RUN(test_malformed_meshes_never_crash);
    RUN(test_matches_reference_bit_for_bit);
    return pc_t_finish();
}
