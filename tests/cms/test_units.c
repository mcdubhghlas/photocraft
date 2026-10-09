/* Ports the unit tests of crates/cms/src/{math,curve,clut,lut3d,lutfile,synth}.rs plus the
 * regen staleness check (tests/regen.rs), and adds edge cases. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "cms/builtin.h"
#include "cms/clut.h"
#include "cms/cms.h"
#include "cms/curve.h"
#include "cms/lut3d.h"
#include "cms/lutfile.h"
#include "cms/math.h"
#include "cms/synth.h"
#include "cms/transform.h"
#include "harness.h"

/* ---- math.rs ---- */

static void test_lab_roundtrip(void)
{
    static const double xyzs[3][3] = {{0.2, 0.3, 0.1}, {0.9642, 1.0, 0.8249}, {0.001, 0.002, 0.0005}};
    double lab[3];
    double back[3];
    int i;
    int k;
    for (i = 0; i < 3; i++) {
        pc_xyz_to_lab(xyzs[i], PC_D50, lab);
        pc_lab_to_xyz(lab, PC_D50, back);
        for (k = 0; k < 3; k++) {
            CHECK_NEAR(back[k], xyzs[i][k], 1e-12);
        }
    }
    pc_xyz_to_lab(PC_D50, PC_D50, lab);
    CHECK_NEAR(lab[0], 100.0, 1e-12);
    CHECK_NEAR(lab[1], 0.0, 1e-12);
    CHECK_NEAR(lab[2], 0.0, 1e-12);
}

static void test_bradford_maps_white(void)
{
    double d65[3];
    double w[3];
    pc_mat3 m;
    int i;
    pc_xy_to_xyz(PC_D65_XY, d65);
    m = pc_bradford(d65, PC_D50);
    pc_mat3_apply(&m, d65, w);
    for (i = 0; i < 3; i++) {
        CHECK_NEAR(w[i], PC_D50[i], 1e-12);
    }
}

static void test_invert_roundtrip(void)
{
    pc_mat3 m = {{{0.4, 0.3, 0.2}, {0.2, 0.7, 0.1}, {0.0, 0.1, 0.8}}};
    pc_mat3 inv;
    pc_mat3 p;
    pc_mat3 sing = {{{1, 2, 3}, {2, 4, 6}, {0, 0, 1}}};
    int i;
    int j;
    CHECK(pc_mat3_invert(&m, &inv));
    p = pc_mat3_mul(&m, &inv);
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            CHECK_NEAR(p.m[i][j], i == j ? 1.0 : 0.0, 1e-12);
        }
    }
    CHECK(!pc_mat3_invert(&sing, &inv));
    sing.m[0][0] = NAN;
    CHECK(!pc_mat3_invert(&sing, &inv));
}

/* ---- curve.rs ---- */

static void test_srgb_matches_formula(void)
{
    pc_cms_curve c = pc_cms_srgb_trc();
    int i;
    for (i = 0; i <= 255; i++) {
        double v = (double)i / 255.0;
        double want = v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
        CHECK_NEAR(pc_cms_curve_eval64(&c, v), want, 1e-12);
        CHECK_NEAR(pc_cms_curve_eval_inverse64(&c, want), v, 1e-9);
    }
}

static void test_parametric_inverses(void)
{
    double p0[7] = {2.2, 0, 0, 0, 0, 0, 0};
    double p1[7] = {2.0, 0.9, 0.1, 0, 0, 0, 0};
    double p2[7] = {2.0, 0.9, 0.1, 0.05, 0, 0, 0};
    double p4[7] = {2.4, 0.9, 0.05, 0.08, 0.04, 0.01, 0.002};
    pc_cms_curve curves[6];
    int k;
    int i;
    curves[0] = pc_cms_curve_parametric(0, p0);
    curves[1] = pc_cms_curve_parametric(1, p1);
    curves[2] = pc_cms_curve_parametric(2, p2);
    curves[3] = pc_cms_srgb_trc();
    curves[4] = pc_cms_curve_parametric(4, p4);
    curves[5] = pc_cms_curve_gamma(1.8);
    for (k = 0; k < 6; k++) {
        for (i = 1; i < 100; i++) {
            double x = (double)i / 100.0;
            double y = pc_cms_curve_eval64(&curves[k], x);
            CHECK_NEAR(pc_cms_curve_eval_inverse64(&curves[k], y), x, 1e-6);
        }
    }
}

static void test_table_inverse_exact_on_knots_and_between(void)
{
    pc_cms_curve g = pc_cms_curve_gamma(2.2);
    float *t = pc_malloc_array(1024, sizeof *t);
    float *d = pc_malloc_array(3, sizeof *d);
    pc_cms_curve c;
    pc_cms_curve dec;
    int i;
    if (t == NULL || d == NULL) {
        PC_T_FAIL("%s", "out of memory");
        free(t);
        free(d);
        return;
    }
    pc_cms_curve_sample(&g, 1024, t);
    c = pc_cms_curve_table(t, 1024);
    for (i = 0; i <= 100; i++) {
        float x = (float)i / 100.0f;
        float back = pc_cms_curve_eval_inverse(&c, pc_cms_curve_eval(&c, x));
        CHECK_NEAR(back, x, 1e-4);
    }
    d[0] = 1.0f;
    d[1] = 0.5f;
    d[2] = 0.0f;
    dec = pc_cms_curve_table(d, 3);
    CHECK_NEAR(pc_cms_curve_eval_inverse(&dec, 0.25f), 0.75, 1e-6);
    pc_cms_curve_fini(&c);
    pc_cms_curve_fini(&dec);
}

static void test_hdr_and_negative_extension(void)
{
    pc_cms_curve c = pc_cms_srgb_trc();
    pc_cms_curve g = pc_cms_curve_gamma(2.2);
    CHECK(pc_cms_curve_eval64(&c, 2.0) > 1.0);
    CHECK_NEAR(pc_cms_curve_eval_inverse64(&c, pc_cms_curve_eval64(&c, 2.0)), 2.0, 1e-9);
    CHECK_NEAR(pc_cms_curve_eval64(&g, -0.5) + pow(0.5, 2.2), 0.0, 1e-12);
}

static void test_curve_clone_eq_and_identity(void)
{
    float *t = pc_malloc_array(3, sizeof *t);
    pc_cms_curve a;
    pc_cms_curve b;
    pc_cms_curve id = pc_cms_curve_identity();
    if (t == NULL) {
        return;
    }
    t[0] = 0.0f;
    t[1] = 0.5f;
    t[2] = 1.0f;
    a = pc_cms_curve_table(t, 3);
    CHECK_EQ_INT(pc_cms_curve_clone(&a, &b, NULL), PC_OK);
    CHECK(pc_cms_curve_eq(&a, &b) && b.table != a.table);
    CHECK(pc_cms_curve_is_identity(&a) && pc_cms_curve_is_identity(&id));
    b.table[1] = 0.6f;
    CHECK(!pc_cms_curve_eq(&a, &b) && !pc_cms_curve_is_identity(&b));
    CHECK(pc_cms_curve_is_monotonic(&b));
    CHECK_EQ_UINT(pc_cms_curve_param_count(4), 7);
    pc_cms_curve_fini(&a);
    pc_cms_curve_fini(&b);
    pc_cms_curve_fini(&b); /* twice is safe */
}

/* ---- clut.rs ---- */

static void f3(void *ctx, const float *p, float *o)
{
    (void)ctx;
    o[0] = p[0] * 0.5f + p[1] * 0.3f + p[2] * 0.2f;
    o[1] = p[0] * p[1];
    o[2] = sinf(p[2] * 3.0f) * 0.5f + 0.5f;
}

static void test_exact_on_nodes_and_linear_functions(void)
{
    static const size_t grid[3] = {5, 5, 5};
    static const float node[3] = {0.25f, 0.5f, 0.75f};
    static const float mid[3] = {0.13f, 0.77f, 0.41f};
    pc_cms_clut c;
    float o[3];
    float w[3];
    int i;
    CHECK_EQ_INT(pc_cms_clut_sample(grid, 3, 3, f3, NULL, &c, NULL), PC_OK);
    pc_cms_clut_eval(&c, node, o);
    f3(NULL, node, w);
    for (i = 0; i < 3; i++) {
        CHECK_NEAR(o[i], w[i], 1e-6);
    }
    pc_cms_clut_eval(&c, mid, o);
    CHECK_NEAR(o[0], 0.13 * 0.5 + 0.77 * 0.3 + 0.41 * 0.2, 1e-6);
    CHECK_EQ_UINT(pc_cms_clut_nodes(&c), 125);
    pc_cms_clut_fini(&c);
}

static void lin4(void *ctx, const float *p, float *o)
{
    (void)ctx;
    o[0] = 0.1f * p[0] + 0.2f * p[1] + 0.3f * p[2] + 0.4f * p[3];
}

static void test_four_d_and_multilinear_agree_on_linear(void)
{
    static const size_t grid[4] = {3, 4, 5, 6};
    static const float pts[3][4] = {{0.1f, 0.9f, 0.3f, 0.7f}, {1.0f, 0.0f, 0.5f, 0.5f}, {0.33f, 0.66f, 0.99f, 0.01f}};
    pc_cms_clut c;
    int i;
    CHECK_EQ_INT(pc_cms_clut_sample(grid, 4, 1, lin4, NULL, &c, NULL), PC_OK);
    for (i = 0; i < 3; i++) {
        float a[1];
        float b[1];
        float want = 0.1f * pts[i][0] + 0.2f * pts[i][1] + 0.3f * pts[i][2] + 0.4f * pts[i][3];
        pc_cms_clut_eval(&c, pts[i], a);
        pc_cms_clut_eval_multilinear(&c, pts[i], b);
        CHECK_NEAR(a[0], want, 1e-5);
        CHECK_NEAR(b[0], want, 1e-5);
    }
    pc_cms_clut_fini(&c);
}

static void ramp1(void *ctx, const float *p, float *o)
{
    (void)ctx;
    o[0] = p[0];
    o[1] = 1.0f - p[0];
}

static void sum2(void *ctx, const float *p, float *o)
{
    (void)ctx;
    o[0] = p[0] + p[1];
}

static void test_one_and_two_d(void)
{
    static const size_t g1[1] = {3};
    static const size_t g2[2] = {2, 2};
    static const float q[1] = {0.25f};
    static const float q2[2] = {0.5f, 0.25f};
    pc_cms_clut c;
    float o[2];
    CHECK_EQ_INT(pc_cms_clut_sample(g1, 1, 2, ramp1, NULL, &c, NULL), PC_OK);
    pc_cms_clut_eval(&c, q, o);
    CHECK_NEAR(o[0], 0.25, 1e-6);
    CHECK_NEAR(o[1], 0.75, 1e-6);
    pc_cms_clut_fini(&c);
    CHECK_EQ_INT(pc_cms_clut_sample(g2, 2, 1, sum2, NULL, &c, NULL), PC_OK);
    pc_cms_clut_eval(&c, q2, o);
    CHECK_NEAR(o[0], 0.75, 1e-6);
    pc_cms_clut_fini(&c);
}

static void test_clut_rejects_bad_dimensions(void)
{
    static const size_t grid[2] = {2, 2};
    static const size_t zero[2] = {0, 2};
    pc_cms_clut c;
    pc_error e;
    float *data = pc_malloc_array(3, sizeof(float));
    /* Wrong data length: rejected, and the data is freed (ASan would report a leak). */
    CHECK_EQ_INT(pc_cms_clut_init(&c, grid, 2, 1, data, 3, &e), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_cms_clut_init(&c, grid, 0, 1, NULL, 0, &e), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_cms_clut_init(&c, grid, 2, 16, NULL, 0, &e), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_cms_clut_sample(zero, 2, 1, sum2, NULL, &c, &e), PC_ERR_INVALID);
}

/* ---- lut3d.rs ---- */

static void test_half_float_roundtrip(void)
{
    static const float vs[8] = {0.0f, 1.0f, 0.5f, 0.333f, 1e-5f, 65504.0f, -2.25f, 0.1f};
    int i;
    for (i = 0; i < 8; i++) {
        float back = pc_lut3d_f16_to_f32(pc_lut3d_f32_to_f16(vs[i]));
        CHECK(fabsf(back - vs[i]) <= fabsf(vs[i]) * 1e-3f + 1e-7f);
    }
    CHECK_EQ_UINT(pc_lut3d_f32_to_f16(1.0f), 0x3C00);
    CHECK_EQ_UINT(pc_lut3d_f32_to_f16(1e6f), 0x7C00);
    CHECK(isnan(pc_lut3d_f16_to_f32(0x7E00)));
}

static void test_lut3d_rejects_bad_transforms(void)
{
    pc_transform *t = NULL;
    pc_lut3d l;
    pc_error e;
    CHECK_EQ_INT(pc_transform_new(pc_builtin_profile(PC_BUILTIN_COATED_CMYK), pc_builtin_profile(PC_BUILTIN_SRGB),
                                  PC_INTENT_RELATIVE, false, &t, NULL),
                 PC_OK);
    CHECK_EQ_INT(pc_lut3d_from_transform(t, 17, &l, &e), PC_ERR_INVALID); /* 4 inputs */
    pc_transform_release(t);
    CHECK_EQ_INT(pc_transform_new(pc_builtin_profile(PC_BUILTIN_SRGB), pc_builtin_profile(PC_BUILTIN_SRGB),
                                  PC_INTENT_RELATIVE, false, &t, NULL),
                 PC_OK);
    CHECK_EQ_INT(pc_lut3d_from_transform(t, 1, &l, &e), PC_ERR_INVALID);
    pc_transform_release(t);
}

/* ---- lutfile.rs ---- */

static const char ROWS[] = "0 0 0\n0 0 1\n0 1 0\n0 1 1\n1 0 0\n1 0 1\n1 1 0\n1 1 1\n";

static pc_status cube(const char *text, pc_lutfile *l, pc_error *e)
{
    return pc_lutfile_parse_cube(text, strlen(text), l, e);
}

static void test_cube_domain_is_kept_and_validated(void)
{
    char buf[512];
    pc_lutfile l;
    pc_error e;
    (void)snprintf(buf, sizeof buf, "TITLE \"t\"\nLUT_3D_SIZE 2\n%s", ROWS);
    CHECK_EQ_INT(cube(buf, &l, &e), PC_OK);
    CHECK(pc_lutfile_domain_is_default(&l));
    pc_lutfile_fini(&l);
    (void)snprintf(buf, sizeof buf, "DOMAIN_MIN 0.1 0.2 0.3\nDOMAIN_MAX 0.9 0.8 0.7\nTITLE \"t\"\nLUT_3D_SIZE 2\n%s", ROWS);
    CHECK_EQ_INT(cube(buf, &l, &e), PC_OK);
    CHECK(l.domain_min[0] == 0.1f && l.domain_min[1] == 0.2f && l.domain_min[2] == 0.3f);
    CHECK(l.domain_max[0] == 0.9f && l.domain_max[1] == 0.8f && l.domain_max[2] == 0.7f);
    CHECK(!pc_lutfile_domain_is_default(&l));
    pc_lutfile_fini(&l);
    (void)snprintf(buf, sizeof buf, "DOMAIN_MIN 0.5 0.5 0.5\nDOMAIN_MAX 0.5 1 1\nTITLE \"t\"\nLUT_3D_SIZE 2\n%s", ROWS);
    CHECK_EQ_INT(cube(buf, &l, &e), PC_ERR_FORMAT);
    CHECK_CONTAINS(e.msg, "below DOMAIN_MAX");
}

static void test_cube_1d_domain_is_baked_into_the_resample(void)
{
    pc_lutfile l;
    pc_error e;
    CHECK_EQ_INT(cube("LUT_1D_SIZE 2\nLUT_1D_INPUT_RANGE 0.25 0.75\n0 0 0\n1 1 1\n", &l, &e), PC_OK);
    CHECK(pc_lutfile_domain_is_default(&l));
    CHECK_NEAR(l.data[0], 0.0, 1e-6);
    CHECK_NEAR(l.data[16 * 3], 0.5, 1e-5);
    CHECK_NEAR(l.data[8 * 3], 0.0, 1e-5);
    CHECK_NEAR(l.data[24 * 3], 1.0, 1e-5);
    CHECK_NEAR(l.data[32 * 3], 1.0, 1e-6);
    pc_lutfile_fini(&l);
    CHECK_EQ_INT(cube("LUT_1D_SIZE 2\nDOMAIN_MIN 0 0 0.5\nDOMAIN_MAX 1 1 1\n0 0 0\n1 1 1\n", &l, &e), PC_OK);
    CHECK_NEAR(l.data[(16 * 33 * 33) * 3 + 2], 0.0, 1e-5);
    pc_lutfile_fini(&l);
    CHECK_EQ_INT(cube("LUT_1D_SIZE 2\nLUT_1D_INPUT_RANGE 1 1\n0 0 0\n1 1 1\n", &l, &e), PC_ERR_FORMAT);
    CHECK_CONTAINS(e.msg, "below DOMAIN_MAX");
}

static void test_cube_roundtrip_and_identity(void)
{
    pc_lutfile id;
    pc_lutfile back;
    pc_buf text = PC_BUF_INIT;
    size_t i;
    int bad = 0;
    CHECK_EQ_INT(pc_lutfile_identity(5, &id, NULL), PC_OK);
    free(id.title);
    id.title = pc_strndup("Id", 2);
    CHECK_EQ_INT(pc_lutfile_write_cube(&id, &text, NULL), PC_OK);
    CHECK_EQ_INT(cube(pc_buf_cstr(&text), &back, NULL), PC_OK);
    CHECK_EQ_UINT(back.size, 5);
    CHECK_EQ_STR(back.title, "Id");
    for (i = 0; i < back.ndata && i < id.ndata; i++) {
        bad += fabsf(back.data[i] - id.data[i]) >= 1e-5f;
    }
    CHECK_EQ_INT(bad, 0);
    CHECK(back.data[3] == 0.25f && back.data[4] == 0.0f && back.data[5] == 0.0f); /* red fastest */
    pc_lutfile_fini(&id);
    pc_lutfile_fini(&back);
    pc_buf_fini(&text);
}

static void test_cube_1d_expands(void)
{
    pc_lutfile l;
    pc_buf big = PC_BUF_INIT;
    int i;
    CHECK_EQ_INT(cube("LUT_1D_SIZE 2\n0 0 0\n0.5 1 1\n", &l, NULL), PC_OK);
    CHECK_EQ_UINT(l.size, 33);
    CHECK_NEAR(l.data[l.ndata - 3], 0.5, 1e-6);
    CHECK_NEAR(l.data[l.ndata - 2], 1.0, 1e-6);
    pc_lutfile_fini(&l);
    CHECK_EQ_INT(pc_buf_append_str(&big, "LUT_1D_SIZE 256\n", NULL), PC_OK);
    for (i = 0; i < 256; i++) {
        float v = (float)i / 255.0f;
        CHECK_EQ_INT(pc_buf_appendf(&big, NULL, "%.9g %.9g %.9g\n", (double)v, (double)v, (double)v), PC_OK);
    }
    CHECK_EQ_INT(cube(pc_buf_cstr(&big), &l, NULL), PC_OK);
    CHECK_EQ_UINT(l.size, 33);
    pc_lutfile_fini(&l);
    pc_buf_fini(&big);
}

static void test_cube_size_overflows_are_rejected(void)
{
    pc_lutfile l;
    char text[128];
    (void)snprintf(text, sizeof text, "LUT_1D_SIZE %zu\n", (size_t)SIZE_MAX);
    CHECK(cube(text, &l, NULL) != PC_OK);
    (void)snprintf(text, sizeof text, "<look><size>%zu</size><data>00</data></look>", (size_t)SIZE_MAX);
    CHECK(pc_lutfile_parse_look(text, strlen(text), &l, NULL) != PC_OK);
    CHECK(cube("LUT_3D_SIZE 99999999999999999999999\n", &l, NULL) != PC_OK); /* usize overflow */
}

static void test_three_dl_blue_fastest_and_bit_depth(void)
{
    pc_buf text = PC_BUF_INIT;
    pc_lutfile l;
    pc_lutfile id;
    int r;
    int g;
    int b;
    CHECK_EQ_INT(pc_buf_append_str(&text, "0 1023\n", NULL), PC_OK);
    for (r = 0; r < 2; r++) {
        for (g = 0; g < 2; g++) {
            for (b = 0; b < 2; b++) {
                CHECK_EQ_INT(pc_buf_appendf(&text, NULL, "%d %d %d\n", r * 4095, g * 4095, b * 4095), PC_OK);
            }
        }
    }
    CHECK_EQ_INT(pc_lutfile_parse_3dl(pc_buf_cstr(&text), text.len, &l, NULL), PC_OK);
    CHECK_EQ_INT(pc_lutfile_identity(2, &id, NULL), PC_OK);
    CHECK_EQ_UINT(l.size, 2);
    CHECK(l.ndata == id.ndata && memcmp(l.data, id.data, l.ndata * sizeof *l.data) == 0);
    pc_lutfile_fini(&l);
    pc_lutfile_fini(&id);
    pc_buf_fini(&text);
}

static void test_look_hex_floats(void)
{
    pc_lutfile id;
    pc_lutfile l;
    pc_buf xml = PC_BUF_INIT;
    size_t i;
    CHECK_EQ_INT(pc_lutfile_identity(2, &id, NULL), PC_OK);
    CHECK_EQ_INT(pc_buf_append_str(&xml, "<?xml version=\"1.0\"?><look><LUT><size>\"2\"</size><data>\"", NULL), PC_OK);
    for (i = 0; i < id.ndata; i++) {
        uint32_t u;
        int k;
        memcpy(&u, &id.data[i], 4);
        for (k = 0; k < 4; k++) {
            CHECK_EQ_INT(pc_buf_appendf(&xml, NULL, "%02X", (unsigned)(u >> (8 * k)) & 0xFFu), PC_OK);
        }
    }
    CHECK_EQ_INT(pc_buf_append_str(&xml, "\"</data></LUT></look>", NULL), PC_OK);
    CHECK_EQ_INT(pc_lutfile_parse("a.look", xml.data, xml.len, &l, NULL), PC_OK);
    CHECK(l.ndata == id.ndata && memcmp(l.data, id.data, l.ndata * sizeof *l.data) == 0);
    pc_lutfile_fini(&l);
    pc_lutfile_fini(&id);
    pc_buf_fini(&xml);
}

static void test_malformed_luts_rejected(void)
{
    pc_lutfile l;
    pc_error e;
    CHECK(cube("LUT_3D_SIZE 2\n0 0 0\n", &l, &e) != PC_OK);
    CHECK_CONTAINS(e.msg, "expected 8 entries, found 1");
    CHECK(cube("garbage", &l, &e) != PC_OK);
    CHECK_EQ_STR(e.msg, "no LUT_3D_SIZE in .cube file");
    CHECK(cube("LUT_3D_SIZE 99999\n", &l, &e) != PC_OK);
    CHECK_EQ_STR(e.msg, "unsupported LUT size 99999");
    CHECK(pc_lutfile_parse_3dl("1 2 3\n4 5 6\n", 12, &l, &e) != PC_OK);
    CHECK_EQ_STR(e.msg, "2 rows is not a cube");
    CHECK(pc_lutfile_parse_look("<look></look>", 13, &l, &e) != PC_OK);
    CHECK_EQ_STR(e.msg, "no <size> in .look");
    CHECK(cube("LUT_3D_SIZE 2\n0x1p0 0 0\n", &l, &e) != PC_OK); /* no hex floats, as in Rust */
    CHECK(cube("LUT_3D_SIZE 2\n1 2\n", &l, &e) != PC_OK);
    CHECK_EQ_STR(e.msg, "bad data line `1 2`");
    CHECK(cube("LUT_3D_SIZE -2\n", &l, &e) != PC_OK);
    CHECK_EQ_STR(e.msg, "bad LUT_3D_SIZE");
    CHECK(cube("DOMAIN_MIN 0 0\nLUT_3D_SIZE 2\n", &l, &e) != PC_OK);
    CHECK_EQ_STR(e.msg, "bad line `DOMAIN_MIN 0 0`");
    CHECK(cube("LUT_3D_SIZE 2\n0 nan 0\n0 0 1\n0 1 0\n0 1 1\n1 0 0\n1 0 1\n1 1 0\n1 1 1\n", &l, &e) != PC_OK);
    CHECK_EQ_STR(e.msg, "non-finite LUT value");
    CHECK(pc_lutfile_parse("x.cube", NULL, 0, &l, &e) != PC_OK);
}

static void test_builtins_exist_and_are_in_range(void)
{
    size_t i;
    pc_lutfile l;
    pc_error e;
    for (i = 0; i < PC_LUTFILE_BUILTIN_COUNT; i++) {
        size_t k;
        int out = 0;
        CHECK_EQ_INT(pc_lutfile_builtin(pc_lutfile_builtin_id(i), &l, NULL), PC_OK);
        CHECK_EQ_UINT(l.ndata, 33 * 33 * 33 * 3);
        for (k = 0; k < l.ndata; k++) {
            out += !(l.data[k] >= 0.0f && l.data[k] <= 1.0f);
        }
        CHECK_EQ_INT(out, 0);
        CHECK_EQ_STR(l.title, pc_lutfile_builtin_label(i));
        pc_lutfile_fini(&l);
    }
    CHECK_EQ_INT(pc_lutfile_builtin("nope", &l, &e), PC_ERR_NOT_FOUND);
    CHECK(pc_lutfile_builtin_id(99) == NULL);
}

/* ---- synth.rs ---- */

static void test_model_anchors(void)
{
    pc_cmyk_params p = pc_cmyk_params_default();
    pc_cmyk_model m;
    double w[3];
    double k[3];
    double rich[3];
    static const double zero[4] = {0, 0, 0, 0};
    static const double kk[4] = {0, 0, 0, 1};
    static const double rc[4] = {0.75, 0.65, 0.6, 0.9};
    int ink;
    pc_cmyk_model_coated(p.tvi, &m);
    pc_cmyk_model_lab(&m, zero, w);
    CHECK_NEAR(w[0], 100.0, 1e-9);
    CHECK_NEAR(w[1], 0.0, 1e-9);
    CHECK_NEAR(w[2], 0.0, 1e-9);
    pc_cmyk_model_lab(&m, kk, k);
    CHECK(k[0] > 12.0 && k[0] < 22.0);
    pc_cmyk_model_lab(&m, rc, rich);
    CHECK(rich[0] < k[0]);
    for (ink = 0; ink < 4; ink++) {
        double prev = 101.0;
        int i;
        for (i = 0; i <= 20; i++) {
            double v[4] = {0.2, 0.2, 0.2, 0.2};
            double l[3];
            v[ink] = (double)i / 20.0;
            pc_cmyk_model_lab(&m, v, l);
            CHECK(l[0] < prev + 1e-9);
            prev = l[0];
        }
    }
}

static void test_separation_hits_in_gamut_targets(void)
{
    static const double reach[4][4] = {{0.3, 0.4, 0.1, 0.1}, {0.6, 0.1, 0.5, 0.0}, {0.05, 0.1, 0.7, 0.0},
                                       {0.5, 0.45, 0.45, 0.4}};
    pc_cmyk_params p = pc_cmyk_params_default();
    pc_cmyk_model m;
    double targets[5][3] = {{50.0, 0.0, 0.0}};
    int i;
    pc_cmyk_model_coated(p.tvi, &m);
    for (i = 0; i < 4; i++) {
        pc_cmyk_model_lab(&m, reach[i], targets[i + 1]);
    }
    for (i = 0; i < 5; i++) {
        double cmyk[4];
        double de = pc_cmyk_separate(&m, &p, targets[i], NULL, cmyk);
        CHECK(de < 1.5);
        CHECK(cmyk[0] + cmyk[1] + cmyk[2] + cmyk[3] <= p.tac + 1e-6);
    }
}

/* tests/regen.rs: the shipped AToB reproduces the model. */
static void test_shipped_profile_is_not_stale(void)
{
    const pc_profile *shipped = pc_builtin_profile(PC_BUILTIN_COATED_CMYK);
    const pc_profile *lab = pc_builtin_profile(PC_BUILTIN_LAB_D50);
    pc_transform *t = NULL;
    pc_cmyk_params p = pc_cmyk_params_default();
    pc_cmyk_model m;
    static const double cs[4] = {0.0, 0.3, 0.7, 1.0};
    static const double ms[3] = {0.0, 0.45, 1.0};
    static const double ys[3] = {0.0, 0.55, 1.0};
    static const double ks[3] = {0.0, 0.5, 1.0};
    double worst = 0.0;
    int a;
    int b;
    int c;
    int d;
    CHECK_EQ_INT(pc_transform_new(shipped, lab, PC_INTENT_RELATIVE, false, &t, NULL), PC_OK);
    pc_cmyk_model_coated(p.tvi, &m);
    for (a = 0; t != NULL && a < 4; a++) {
        for (b = 0; b < 3; b++) {
            for (c = 0; c < 3; c++) {
                for (d = 0; d < 3; d++) {
                    float in[4];
                    float o[3];
                    double got[3];
                    double want[3];
                    double v[4];
                    v[0] = cs[a];
                    v[1] = ms[b];
                    v[2] = ys[c];
                    v[3] = ks[d];
                    in[0] = (float)v[0];
                    in[1] = (float)v[1];
                    in[2] = (float)v[2];
                    in[3] = (float)v[3];
                    pc_transform_eval(t, in, o);
                    got[0] = (double)o[0] * 100.0;
                    got[1] = (double)o[1] * 255.0 - 128.0;
                    got[2] = (double)o[2] * 255.0 - 128.0;
                    pc_cmyk_model_lab(&m, v, want);
                    worst = fmax(worst, pc_delta_e76(got, want));
                }
            }
        }
    }
    CHECK(worst < 1.0);
    pc_transform_release(t);
}

/* ---- lib.rs / builtin.rs vocabulary ---- */

static void test_intents_and_ids(void)
{
    pc_intent i = PC_INTENT_PERCEPTUAL;
    pc_builtin b = PC_BUILTIN_SRGB;
    char name[32];
    CHECK(pc_intent_parse("Relative Colorimetric", 21, &i) && i == PC_INTENT_RELATIVE);
    CHECK(pc_intent_parse("ABSOLUTE", 8, &i) && i == PC_INTENT_ABSOLUTE);
    CHECK(pc_intent_parse("2", 1, &i) && i == PC_INTENT_SATURATION);
    CHECK(!pc_intent_parse("vivid", 5, &i));
    CHECK(!pc_intent_from_u32(4, &i));
    CHECK_EQ_STR(pc_intent_id(PC_INTENT_PERCEPTUAL), "perceptual");
    CHECK(pc_builtin_from_id("Adobe RGB (1998)", 16, &b) && b == PC_BUILTIN_ADOBE_RGB_COMPAT);
    CHECK(pc_builtin_from_id("sRGB IEC61966-2.1", 17, &b) && b == PC_BUILTIN_SRGB);
    CHECK(pc_builtin_from_id("gray-gamma-2.2", 14, &b) && b == PC_BUILTIN_GRAY_GAMMA22);
    CHECK(pc_builtin_from_id(PC_SYNTH_DESCRIPTION, strlen(PC_SYNTH_DESCRIPTION), &b) && b == PC_BUILTIN_COATED_CMYK);
    CHECK(!pc_builtin_from_id("nope", 4, &b));
    for (b = PC_BUILTIN_SRGB; b < PC_BUILTIN_COUNT; b = (pc_builtin)(b + 1)) {
        pc_builtin back;
        CHECK(pc_builtin_from_id(pc_builtin_id(b), strlen(pc_builtin_id(b)), &back) && back == b);
    }
    CHECK_EQ_UINT(pc_cs_channels(PC_SIG('5', 'C', 'L', 'R')), 5);
    CHECK_EQ_UINT(pc_cs_channels(PC_SIG('F', 'C', 'L', 'R')), 15);
    CHECK_EQ_UINT(pc_cs_channels(PC_SIG('1', 'C', 'L', 'R')), 0);
    pc_cs_debug_name(PC_SIG('5', 'C', 'L', 'R'), name);
    CHECK_EQ_STR(name, "Color(5)");
    pc_cs_debug_name(PC_CS_CMYK, name);
    CHECK_EQ_STR(name, "Cmyk");
    CHECK(pc_builtin_default_for(PC_CS_GRAY) == pc_builtin_profile(PC_BUILTIN_SGRAY));
    CHECK(pc_builtin_default_for(PC_CS_HSV) == NULL);
}

int main(int argc, char **argv)
{
    RUN(test_lab_roundtrip);
    RUN(test_bradford_maps_white);
    RUN(test_invert_roundtrip);
    RUN(test_srgb_matches_formula);
    RUN(test_parametric_inverses);
    RUN(test_table_inverse_exact_on_knots_and_between);
    RUN(test_hdr_and_negative_extension);
    RUN(test_curve_clone_eq_and_identity);
    RUN(test_exact_on_nodes_and_linear_functions);
    RUN(test_four_d_and_multilinear_agree_on_linear);
    RUN(test_one_and_two_d);
    RUN(test_clut_rejects_bad_dimensions);
    RUN(test_half_float_roundtrip);
    RUN(test_lut3d_rejects_bad_transforms);
    RUN(test_cube_domain_is_kept_and_validated);
    RUN(test_cube_1d_domain_is_baked_into_the_resample);
    RUN(test_cube_roundtrip_and_identity);
    RUN(test_cube_1d_expands);
    RUN(test_cube_size_overflows_are_rejected);
    RUN(test_three_dl_blue_fastest_and_bit_depth);
    RUN(test_look_hex_floats);
    RUN(test_malformed_luts_rejected);
    RUN(test_builtins_exist_and_are_in_range);
    RUN(test_model_anchors);
    RUN(test_separation_hits_in_gamut_targets);
    RUN(test_shipped_profile_is_not_stale);
    RUN(test_intents_and_ids);
    return pc_t_finish();
}
