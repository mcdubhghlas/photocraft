/* Filter > Liquify (src/algo/liquify*.c): the reference's tests from crates/algo/src/liquify.rs
 * plus never-crash cases (NaN / infinite brush positions, huge brushes and fields, short buffers,
 * hostile JSON). Bit-exact parity with the reference is in test_warp_parity.c. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "base/json.h"
#include "harness.h"

static pc_rect bounds(void)
{
    return pc_rect_make(0, 0, 96, 64);
}

static pc_algo_liquify_stroke stroke(pc_algo_liquify_tool tool, double size, const double (*pts)[3], size_t n,
                                     size_t dims)
{
    pc_algo_liquify_stroke s = pc_algo_liquify_stroke_make(tool, size);
    size_t i;
    for (i = 0; i < n; i++) {
        CHECK(pc_algo_liquify_stroke_push(&s, pts[i], dims, NULL) == PC_OK);
    }
    return s;
}

static void sample_surface(pc_sample_type st, pc_surface *s)
{
    pc_pixel_format f = {PC_COLOR_MODE_RGB, st, true};
    int32_t x, y;
    pc_surface_init(s, f);
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 96; x++) {
            float px[4] = {(float)((x / 4 + y / 4) % 2), (float)x / 96.0f, (float)y / 64.0f, 1.0f};
            CHECK(pc_surface_write_pixel(s, x, y, px, 4, NULL) == PC_OK);
        }
    }
}

static float worst(const pc_surface *a, const pc_surface *b, pc_rect r)
{
    float *va = NULL, *vb = NULL, m = 0.0f;
    size_t la = 0, lb = 0, i;
    CHECK(pc_surface_read_region(a, r, &va, &la, NULL) == PC_OK);
    CHECK(pc_surface_read_region(b, r, &vb, &lb, NULL) == PC_OK);
    for (i = 0; i < la && i < lb; i++) {
        m = fmaxf(m, fabsf(va[i] - vb[i]));
    }
    free(va);
    free(vb);
    return m;
}

static pc_algo_liquify_stroke lasso(const double (*poly)[2], size_t n, bool has_amount, double amount)
{
    pc_algo_liquify_stroke s = pc_algo_liquify_stroke_make(PC_ALGO_LIQUIFY_LASSO_MASK, 1.0);
    size_t i;
    for (i = 0; i < n; i++) {
        CHECK(pc_algo_liquify_stroke_push(&s, poly[i], 2, NULL) == PC_OK);
    }
    s.has_amount = has_amount;
    s.amount = amount;
    return s;
}

/* Reference even-odd test (ray casting), to check the scanline fill against. */
static bool inside(const double p[2], const double (*poly)[2], size_t n)
{
    bool in = false;
    size_t i, j = n - 1;
    for (i = 0; i < n; i++) {
        const double *a = poly[i], *b = poly[j];
        if ((a[1] > p[1]) != (b[1] > p[1]) && p[0] < (b[0] - a[0]) * (p[1] - a[1]) / (b[1] - a[1]) + a[0]) {
            in = !in;
        }
        j = i;
    }
    return in;
}

static void test_a_segment_to_a_far_off_point_returns(void)
{
    /* #937: about 3.3e14 steps before the fix; also a segment whose length overflows f64. */
    pc_algo_liquify_field f;
    pc_algo_liquify_stroke s = pc_algo_liquify_stroke_make(PC_ALGO_LIQUIFY_FORWARD_WARP, 30.0);
    pc_algo_liquify_stroke b = pc_algo_liquify_stroke_make(PC_ALGO_LIQUIFY_BLOAT, 30.0);
    double a0[3] = {0, 0, 1}, a1[3] = {1e15, 0, 1}, c0[3] = {-1e308, 5, 1}, c1[3] = {1e308, 5, 1};
    double d0[3] = {5, -1e15, 1}, d1[3] = {5, 1e15, 1};
    pc_rect dirty;
    CHECK(pc_algo_liquify_field_init(&f, pc_rect_make(0, 0, 24, 16), 2.0, NULL) == PC_OK);
    CHECK(pc_algo_liquify_stroke_segment(&f, &s, a0, a1, &dirty, NULL) == PC_OK);
    CHECK(!pc_rect_is_empty(dirty) && dirty.x1 <= 64);
    CHECK(pc_algo_liquify_stroke_segment(&f, &s, c0, c1, NULL, NULL) == PC_OK);
    CHECK(pc_algo_liquify_stroke_segment(&f, &b, d0, d1, NULL, NULL) == PC_OK);
    pc_algo_liquify_field_fini(&f);
}

static void test_lasso_freezes_exactly_the_nodes_inside_a_concave_polygon(void)
{
    static const double poly[8][2] = {{10, 8},  {70, 8},  {70, 20}, {30, 22.5},
                                      {33, 40}, {80, 41}, {60, 58}, {9, 55}};
    static const double cells[3] = {1.0, 2.0, 3.5};
    int c;
    for (c = 0; c < 3; c++) {
        pc_algo_liquify_field f, g;
        pc_algo_liquify_stroke l = lasso(poly, 8, false, 0.0);
        pc_rect dirty;
        size_t i, j, frozen = 0;
        CHECK(pc_algo_liquify_field_init(&f, bounds(), cells[c], NULL) == PC_OK);
        CHECK(pc_algo_liquify_apply_stroke(&f, &l, &dirty, NULL) == PC_OK);
        CHECK(pc_rect_eq(dirty, pc_rect_make(9, 8, 80, 58)));
        for (j = 0; j < f.h; j++) {
            for (i = 0; i < f.w; i++) {
                double p[2];
                pc_algo_liquify_node_pos(&f, i, j, p);
                CHECK(f.freeze[j * f.w + i] == (inside(p, poly, 8) ? 1.0f : 0.0f));
                frozen += f.freeze[j * f.w + i] == 1.0f;
            }
        }
        CHECK(frozen > 0);
        /* Strokes replay to the same field (undo rebuilds from the list). */
        CHECK(pc_algo_liquify_field_from_strokes(bounds(), cells[c], &l, 1, &g, NULL) == PC_OK);
        CHECK(pc_algo_liquify_field_eq(&f, &g));
        pc_algo_liquify_field_fini(&g);
        pc_algo_liquify_field_fini(&f);
        pc_algo_liquify_stroke_fini(&l);
    }
}

static void test_lasso_thaws_with_amount_zero(void)
{
    static const double square[4][2] = {{20, 20}, {40, 20}, {40, 40}, {20, 40}};
    pc_algo_liquify_field f;
    pc_algo_liquify_stroke all = pc_algo_liquify_stroke_make(PC_ALGO_LIQUIFY_FREEZE_ALL, 1.0);
    pc_algo_liquify_stroke l = lasso(square, 4, true, 0.0);
    size_t i;
    bool all_frozen = true;
    CHECK(pc_algo_liquify_field_init(&f, bounds(), 2.0, NULL) == PC_OK);
    CHECK(pc_algo_liquify_apply_stroke(&f, &all, NULL, NULL) == PC_OK);
    for (i = 0; i < f.w * f.h; i++) {
        all_frozen = all_frozen && f.freeze[i] == 1.0f;
    }
    CHECK(all_frozen);
    CHECK(pc_algo_liquify_apply_stroke(&f, &l, NULL, NULL) == PC_OK);
    CHECK(pc_algo_liquify_freeze_at(&f, 30.0, 30.0) == 0.0f);
    CHECK(pc_algo_liquify_freeze_at(&f, 5.0, 5.0) == 1.0f);
    pc_algo_liquify_stroke_fini(&l);
    pc_algo_liquify_field_fini(&f);
}

static void test_lasso_ignores_degenerate_and_hostile_polygons(void)
{
    static const double c1[2][2] = {{10, 10}, {50, 50}};
    static const double c3[3][2] = {{-50, -50}, {-10, -50}, {-10, -10}};
    static const double c4[3][2] = {{200, 10}, {300, 10}, {250, 50}};
    static const double c5[3][2] = {{10, 1e12}, {50, 1e12}, {30, 2e12}};
    static const double huge[4][2] = {{-1e15, -1e15}, {1e15, -1e15}, {1e15, 1e15}, {-1e15, 1e15}};
    double c2[3][2] = {{(double)NAN, 10}, {50, (double)INFINITY}, {20, 30}};
    double withnan[5][2] = {{10, 10}, {(double)NAN, 0}, {50, 10}, {50, 50}, {10, 50}};
    const double (*cases[6])[2] = {NULL, c1, (const double (*)[2])c2, c3, c4, c5};
    const size_t ns[6] = {0, 2, 3, 3, 3, 3};
    pc_algo_liquify_field empty, f;
    pc_algo_liquify_stroke l;
    size_t k, i;
    bool all = true;
    CHECK(pc_algo_liquify_field_init(&empty, bounds(), 2.0, NULL) == PC_OK);
    for (k = 0; k < 6; k++) {
        pc_rect d = pc_rect_make(1, 1, 2, 2);
        l = lasso(cases[k], ns[k], false, 0.0);
        CHECK(pc_algo_liquify_field_clone(&empty, &f, NULL) == PC_OK);
        CHECK(pc_algo_liquify_apply_stroke(&f, &l, &d, NULL) == PC_OK);
        CHECK(pc_rect_is_empty(d));
        CHECK(pc_algo_liquify_field_eq(&f, &empty));
        pc_algo_liquify_field_fini(&f);
        pc_algo_liquify_stroke_fini(&l);
    }
    /* Huge coordinates around the canvas clamp to it instead of overflowing. */
    l = lasso(huge, 4, false, 0.0);
    CHECK(pc_algo_liquify_field_clone(&empty, &f, NULL) == PC_OK);
    CHECK(pc_algo_liquify_apply_stroke(&f, &l, NULL, NULL) == PC_OK);
    for (i = 0; i < f.w * f.h; i++) {
        all = all && f.freeze[i] == 1.0f;
    }
    CHECK(all);
    pc_algo_liquify_field_fini(&f);
    pc_algo_liquify_stroke_fini(&l);
    /* A NaN point among good ones is skipped. */
    l = lasso((const double (*)[2])withnan, 5, false, 0.0);
    CHECK(pc_algo_liquify_field_clone(&empty, &f, NULL) == PC_OK);
    CHECK(pc_algo_liquify_apply_stroke(&f, &l, NULL, NULL) == PC_OK);
    CHECK(pc_algo_liquify_freeze_at(&f, 30.0, 30.0) == 1.0f);
    pc_algo_liquify_field_fini(&f);
    pc_algo_liquify_stroke_fini(&l);
    pc_algo_liquify_field_fini(&empty);
}

static void test_no_strokes_is_identity_at_every_depth(void)
{
    static const pc_sample_type sts[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    static const double pts[2][3] = {{40, 30, 1}, {60, 30, 1}};
    int k;
    for (k = 0; k < 3; k++) {
        pc_surface s, out;
        pc_algo_liquify_field f;
        pc_algo_liquify_stroke st0 = stroke(PC_ALGO_LIQUIFY_FORWARD_WARP, 30.0, pts, 2, 2);
        sample_surface(sts[k], &s);
        CHECK(pc_algo_liquify_field_init(&f, bounds(), 2.0, NULL) == PC_OK);
        CHECK(pc_algo_liquify_apply(&s, &f, &out, NULL) == PC_OK);
        CHECK(pc_surface_eq(&out, &s));
        pc_surface_fini(&out);
        pc_algo_liquify_field_fini(&f);
        /* A field with exactly-zero strokes (zero pressure) is still identity. */
        st0.pressure = 0.0;
        CHECK(pc_algo_liquify_field_from_strokes(bounds(), 2.0, &st0, 1, &f, NULL) == PC_OK);
        CHECK(pc_algo_liquify_is_identity(&f));
        pc_algo_liquify_field_fini(&f);
        pc_algo_liquify_stroke_fini(&st0);
        pc_surface_fini(&s);
    }
}

static void test_forward_warp_displaces_along_the_stroke_and_reconstruct_restores(void)
{
    static const double pts[2][3] = {{40, 32, 1}, {52, 32, 1}};
    pc_algo_liquify_stroke s = stroke(PC_ALGO_LIQUIFY_FORWARD_WARP, 40.0, pts, 2, 2);
    pc_algo_liquify_stroke rec = pc_algo_liquify_stroke_make(PC_ALGO_LIQUIFY_RECONSTRUCT_ALL, 10.0);
    pc_algo_liquify_field f;
    pc_pixel_format gf = {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_F32, true};
    pc_surface img, out;
    float bg[2] = {0.0f, 1.0f}, fg[2] = {1.0f, 1.0f}, best = -1.0f;
    double d[2];
    int32_t x, brightest = 0;
    CHECK(pc_algo_liquify_field_from_strokes(bounds(), 2.0, &s, 1, &f, NULL) == PC_OK);
    /* Content moved right: the output at the end point samples from the left. */
    pc_algo_liquify_sample(&f, 52.0, 32.0, d);
    CHECK(d[0] < -4.0 && fabs(d[1]) < 0.5);
    /* A marker column follows the stroke. */
    pc_surface_init(&img, gf);
    CHECK(pc_surface_fill_rect(&img, bounds(), bg, 2, NULL) == PC_OK);
    CHECK(pc_surface_fill_rect(&img, pc_rect_make(40, 28, 42, 36), fg, 2, NULL) == PC_OK);
    CHECK(pc_algo_liquify_apply(&img, &f, &out, NULL) == PC_OK);
    for (x = 30; x < 70; x++) {
        float px[2];
        pc_surface_read_pixel(&out, x, 32, px);
        if (px[0] >= best) { /* max_by keeps the last maximum */
            best = px[0];
            brightest = x;
        }
    }
    CHECK(brightest > 44);
    pc_surface_fini(&out);
    /* Reconstruct All restores the original exactly. */
    rec.has_amount = true;
    rec.amount = 100.0;
    CHECK(pc_algo_liquify_apply_stroke(&f, &rec, NULL, NULL) == PC_OK);
    CHECK(pc_algo_liquify_is_identity(&f));
    CHECK(pc_algo_liquify_apply(&img, &f, &out, NULL) == PC_OK);
    CHECK(pc_surface_eq(&out, &img));
    pc_surface_fini(&out);
    pc_surface_fini(&img);
    pc_algo_liquify_field_fini(&f);
    pc_algo_liquify_stroke_fini(&s);
}

static bool all_freeze(const pc_algo_liquify_field *f, float v)
{
    size_t i;
    for (i = 0; i < f->w * f->h; i++) {
        if (f->freeze[i] != v) {
            return false;
        }
    }
    return true;
}

static void test_reconstruct_brush_reduces_the_field_and_freeze_protects(void)
{
    static const double pts[2][3] = {{30, 32, 1}, {45, 32, 1}};
    static const double c[1][3] = {{40, 32, 1}};
    double rp[30][3], d[2];
    pc_algo_liquify_stroke s = stroke(PC_ALGO_LIQUIFY_FORWARD_WARP, 40.0, pts, 2, 2), r, fr, th;
    pc_algo_liquify_stroke fa = pc_algo_liquify_stroke_make(PC_ALGO_LIQUIFY_FREEZE_ALL, 1.0);
    pc_algo_liquify_stroke inv = pc_algo_liquify_stroke_make(PC_ALGO_LIQUIFY_INVERT_FREEZE, 1.0);
    pc_algo_liquify_stroke ta = pc_algo_liquify_stroke_make(PC_ALGO_LIQUIFY_THAW_ALL, 1.0);
    pc_algo_liquify_field f, g;
    float before;
    int i;
    for (i = 0; i < 30; i++) {
        rp[i][0] = 40, rp[i][1] = 32, rp[i][2] = 1;
    }
    r = stroke(PC_ALGO_LIQUIFY_RECONSTRUCT, 80.0, (const double (*)[3])rp, 30, 2);
    CHECK(pc_algo_liquify_field_from_strokes(bounds(), 2.0, &s, 1, &f, NULL) == PC_OK);
    before = pc_algo_liquify_max_displacement(&f);
    CHECK(pc_algo_liquify_apply_stroke(&f, &r, NULL, NULL) == PC_OK);
    CHECK(pc_algo_liquify_max_displacement(&f) < before * 0.2f);
    /* Frozen area doesn't move. */
    fr = stroke(PC_ALGO_LIQUIFY_FREEZE, 60.0, c, 1, 2);
    fr.density = 0.0;
    CHECK(pc_algo_liquify_field_init(&g, bounds(), 2.0, NULL) == PC_OK);
    CHECK(pc_algo_liquify_apply_stroke(&g, &fr, NULL, NULL) == PC_OK);
    CHECK(pc_algo_liquify_freeze_at(&g, 40, 32) > 0.99f);
    CHECK(pc_algo_liquify_apply_stroke(&g, &s, NULL, NULL) == PC_OK);
    pc_algo_liquify_sample(&g, 40, 32, d);
    CHECK(fabs(d[0]) < 1e-3);
    /* Thaw unfreezes. */
    CHECK(pc_algo_liquify_stroke_copy(&fr, &th, NULL) == PC_OK);
    th.tool = PC_ALGO_LIQUIFY_THAW;
    CHECK(pc_algo_liquify_apply_stroke(&g, &th, NULL, NULL) == PC_OK);
    CHECK(pc_algo_liquify_freeze_at(&g, 40, 32) < 0.01f);
    /* Mask All / Invert / None. */
    CHECK(pc_algo_liquify_apply_stroke(&g, &fa, NULL, NULL) == PC_OK);
    CHECK(all_freeze(&g, 1.0f));
    CHECK(pc_algo_liquify_apply_stroke(&g, &inv, NULL, NULL) == PC_OK);
    CHECK(all_freeze(&g, 0.0f));
    CHECK(pc_algo_liquify_apply_stroke(&g, &fr, NULL, NULL) == PC_OK);
    CHECK(pc_algo_liquify_apply_stroke(&g, &inv, NULL, NULL) == PC_OK);
    CHECK(pc_algo_liquify_freeze_at(&g, 40, 32) < 0.01f && pc_algo_liquify_freeze_at(&g, 5, 5) > 0.99f);
    CHECK(pc_algo_liquify_apply_stroke(&g, &ta, NULL, NULL) == PC_OK);
    CHECK(all_freeze(&g, 0.0f));
    pc_algo_liquify_field_fini(&f);
    pc_algo_liquify_field_fini(&g);
    pc_algo_liquify_stroke_fini(&s);
    pc_algo_liquify_stroke_fini(&r);
    pc_algo_liquify_stroke_fini(&fr);
    pc_algo_liquify_stroke_fini(&th);
}

static void sense(pc_algo_liquify_tool tool, const double (*pts)[3], size_t n, double x, double y,
                  double out[2])
{
    pc_algo_liquify_stroke s = stroke(tool, 40.0, pts, n, 2);
    pc_algo_liquify_field f;
    CHECK(pc_algo_liquify_field_from_strokes(bounds(), 2.0, &s, 1, &f, NULL) == PC_OK);
    pc_algo_liquify_sample(&f, x, y, out);
    pc_algo_liquify_field_fini(&f);
    pc_algo_liquify_stroke_fini(&s);
}

static void test_twirl_pucker_bloat_and_push_left_have_the_right_sense(void)
{
    double c[20][3], d[2];
    static const double up[2][3] = {{48, 44, 1}, {48, 20, 1}};
    static const double sm[2][3] = {{40, 32, 1}, {50, 32, 1}};
    pc_algo_liquify_stroke s, smooth;
    pc_algo_liquify_field g;
    float m0;
    int i;
    for (i = 0; i < 20; i++) {
        c[i][0] = 48, c[i][1] = 32, c[i][2] = 1;
    }
    /* Pucker pulls content in: the output samples outward. */
    sense(PC_ALGO_LIQUIFY_PUCKER, (const double (*)[3])c, 10, 58, 32, d);
    CHECK(d[0] > 0.1);
    sense(PC_ALGO_LIQUIFY_BLOAT, (const double (*)[3])c, 10, 58, 32, d);
    CHECK(d[0] < -0.1);
    /* Twirl clockwise (y down): content right of centre moves down, so it samples from above. */
    sense(PC_ALGO_LIQUIFY_TWIRL_CW, (const double (*)[3])c, 10, 58, 32, d);
    CHECK(d[1] < -0.1);
    sense(PC_ALGO_LIQUIFY_TWIRL_CCW, (const double (*)[3])c, 10, 58, 32, d);
    CHECK(d[1] > 0.1);
    /* Push Left while dragging up moves content left (samples from the right). */
    sense(PC_ALGO_LIQUIFY_PUSH_LEFT, up, 2, 48, 32, d);
    CHECK(d[0] > 0.5);
    /* Smooth reduces sharp field differences. */
    s = stroke(PC_ALGO_LIQUIFY_FORWARD_WARP, 12.0, sm, 2, 2);
    CHECK(pc_algo_liquify_field_from_strokes(bounds(), 2.0, &s, 1, &g, NULL) == PC_OK);
    m0 = pc_algo_liquify_max_displacement(&g);
    for (i = 0; i < 20; i++) {
        c[i][0] = 45;
    }
    smooth = stroke(PC_ALGO_LIQUIFY_SMOOTH, 60.0, (const double (*)[3])c, 20, 2);
    CHECK(pc_algo_liquify_apply_stroke(&g, &smooth, NULL, NULL) == PC_OK);
    CHECK(pc_algo_liquify_max_displacement(&g) < m0);
    pc_algo_liquify_field_fini(&g);
    pc_algo_liquify_stroke_fini(&s);
    pc_algo_liquify_stroke_fini(&smooth);
}

static void test_replay_is_deterministic_and_depths_agree(void)
{
    static const double p0[3] = {48, 32, 0.5}, p1[3] = {50, 33, 1.0}, p2[2] = {52, 34};
    pc_algo_liquify_stroke s = pc_algo_liquify_stroke_make(PC_ALGO_LIQUIFY_TWIRL_CW, 50.0);
    pc_algo_liquify_field a, b;
    pc_surface s8, s32, r8, r32;
    CHECK(pc_algo_liquify_stroke_push(&s, p0, 3, NULL) == PC_OK);
    CHECK(pc_algo_liquify_stroke_push(&s, p1, 3, NULL) == PC_OK);
    CHECK(pc_algo_liquify_stroke_push(&s, p2, 2, NULL) == PC_OK);
    CHECK(pc_algo_liquify_field_from_strokes(bounds(), 2.0, &s, 1, &a, NULL) == PC_OK);
    CHECK(pc_algo_liquify_field_from_strokes(bounds(), 2.0, &s, 1, &b, NULL) == PC_OK);
    CHECK(pc_algo_liquify_field_eq(&a, &b));
    sample_surface(PC_SAMPLE_TYPE_U8, &s8);
    sample_surface(PC_SAMPLE_TYPE_F32, &s32);
    CHECK(pc_algo_liquify_apply(&s8, &a, &r8, NULL) == PC_OK);
    CHECK(pc_algo_liquify_apply(&s32, &a, &r32, NULL) == PC_OK);
    CHECK(worst(&r8, &r32, bounds()) < 2.0f / 255.0f + 1e-4f);
    pc_surface_fini(&s8);
    pc_surface_fini(&s32);
    pc_surface_fini(&r8);
    pc_surface_fini(&r32);
    pc_algo_liquify_field_fini(&a);
    pc_algo_liquify_field_fini(&b);
    pc_algo_liquify_stroke_fini(&s);
}

static void test_proxy_matches_the_full_render_roughly(void)
{
    double c[8][3];
    pc_surface src, full;
    pc_algo_liquify_stroke s;
    pc_algo_liquify_field f;
    pc_algo_liquify_proxy p;
    uint8_t (*out)[4];
    size_t r[4];
    int i, bad = 0;
    int32_t x, y;
    for (i = 0; i < 8; i++) {
        c[i][0] = 48, c[i][1] = 32, c[i][2] = 1;
    }
    sample_surface(PC_SAMPLE_TYPE_U8, &src);
    s = stroke(PC_ALGO_LIQUIFY_BLOAT, 40.0, (const double (*)[3])c, 8, 2);
    CHECK(pc_algo_liquify_field_from_strokes(bounds(), 2.0, &s, 1, &f, NULL) == PC_OK);
    CHECK(pc_algo_liquify_proxy_init(&p, &src, bounds(), 96, NULL) == PC_OK);
    CHECK(p.w == 96 && p.h == 64 && p.scale == 1.0);
    out = calloc(p.w * p.h, sizeof *out);
    r[0] = 0, r[1] = 0, r[2] = p.w, r[3] = p.h;
    CHECK(pc_algo_liquify_proxy_render(&p, &f, r, out, p.w * p.h, NULL) == PC_OK);
    /* A buffer too short for the rows is refused (the reference panics). */
    CHECK(pc_algo_liquify_proxy_render(&p, &f, r, out, p.w * p.h - 1, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_liquify_apply(&src, &f, &full, NULL) == PC_OK);
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 96; x++) {
            float q[4];
            pc_surface_read_pixel(&full, x, y, q);
            if (fabsf((float)out[y * 96 + x][1] / 255.0f - q[1]) > 0.1f) {
                bad++;
            }
        }
    }
    CHECK(bad < 96 * 64 / 50);
    free(out);
    pc_surface_fini(&full);
    pc_surface_fini(&src);
    pc_algo_liquify_proxy_fini(&p);
    pc_algo_liquify_field_fini(&f);
    pc_algo_liquify_stroke_fini(&s);
}

/* ---- never crash ---- */

static void test_hostile_brushes_never_crash(void)
{
    static const pc_algo_liquify_tool tools[6] = {
        PC_ALGO_LIQUIFY_FORWARD_WARP, PC_ALGO_LIQUIFY_SMOOTH, PC_ALGO_LIQUIFY_TWIRL_CW,
        PC_ALGO_LIQUIFY_RECONSTRUCT,  PC_ALGO_LIQUIFY_FREEZE, PC_ALGO_LIQUIFY_INVERT_FREEZE};
    double nan = (double)NAN, inf = (double)INFINITY;
    double sizes[6] = {nan, inf, -5.0, 0.0, 1e300, 15000.0};
    double pts[8][3] = {{nan, 10, 1},  {10, nan, 1},      {inf, 30, 1},     {-inf, -inf, 1},
                        {30, 30, nan}, {1e300, 1e300, 1}, {-1e19, 3e19, 1}, {40, 30, inf}};
    int t, s, a, b;
    pc_surface src, out;
    sample_surface(PC_SAMPLE_TYPE_U8, &src);
    for (t = 0; t < 6; t++) {
        for (s = 0; s < 6; s++) {
            pc_algo_liquify_field f;
            pc_algo_liquify_stroke st = pc_algo_liquify_stroke_make(tools[t], sizes[s]);
            st.pressure = s == 2 ? nan : 100.0;
            st.rate = s == 3 ? inf : 80.0;
            CHECK(pc_algo_liquify_field_init(&f, bounds(), 2.0, NULL) == PC_OK);
            for (a = 0; a < 8; a++) {
                pc_rect d;
                CHECK(pc_algo_liquify_stroke_begin(&f, &st, pts[a], &d, NULL) == PC_OK);
                for (b = 0; b < 8; b++) {
                    CHECK(pc_algo_liquify_stroke_segment(&f, &st, pts[a], pts[b], &d, NULL) == PC_OK);
                }
            }
            /* Whatever the field holds now (NaN included), applying it is safe. */
            CHECK(pc_algo_liquify_apply(&src, &f, &out, NULL) == PC_OK);
            pc_surface_fini(&out);
            pc_algo_liquify_field_fini(&f);
        }
    }
    pc_surface_fini(&src);
}

static void test_hostile_fields_never_crash(void)
{
    pc_algo_liquify_field f;
    pc_algo_liquify_proxy p;
    pc_surface src, out, empty;
    pc_error e;
    static const double pts[2][3] = {{1, 1, 1}, {3, 2, 1}};
    pc_algo_liquify_stroke st = stroke(PC_ALGO_LIQUIFY_FORWARD_WARP, 20.0, pts, 2, 3);
    size_t r[4] = {0, 0, 100, 100};
    uint8_t px[4][4];
    /* A field the reference could not allocate. */
    CHECK(pc_algo_liquify_field_init(&f, pc_rect_make(INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX), 1.0, &e) ==
          PC_ERR_LIMIT);
    /* Empty bounds and a NaN cell (the reference keeps NaN, giving a 2 x 2 field). */
    CHECK(pc_algo_liquify_field_init(&f, PC_RECT_EMPTY, (double)NAN, NULL) == PC_OK);
    CHECK(f.w == 2 && f.h == 2 && isnan(f.cell));
    CHECK(pc_algo_liquify_apply_stroke(&f, &st, NULL, NULL) == PC_OK);
    sample_surface(PC_SAMPLE_TYPE_F32, &src);
    CHECK(pc_algo_liquify_apply(&src, &f, &out, NULL) == PC_OK);
    CHECK(pc_surface_eq(&out, &src));
    pc_surface_fini(&out);
    /* An empty source surface and an empty-bounds proxy. */
    pc_surface_init(&empty, (pc_pixel_format){PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true});
    CHECK(pc_algo_liquify_proxy_init(&p, &empty, PC_RECT_EMPTY, 0, NULL) == PC_OK);
    CHECK(p.w == 1 && p.h == 1);
    CHECK(pc_algo_liquify_proxy_render(&p, &f, r, px, 4, NULL) == PC_OK);
    pc_algo_liquify_proxy_fini(&p);
    pc_algo_liquify_field_fini(&f);
    CHECK(pc_algo_liquify_field_init(&f, bounds(), 2.0, NULL) == PC_OK);
    CHECK(pc_algo_liquify_apply_stroke(&f, &st, NULL, NULL) == PC_OK);
    CHECK(pc_algo_liquify_apply(&empty, &f, &out, NULL) == PC_OK);
    pc_surface_fini(&out);
    pc_algo_liquify_field_fini(&f);
    pc_surface_fini(&empty);
    pc_surface_fini(&src);
    pc_algo_liquify_stroke_fini(&st);
}

static void test_json_and_validation(void)
{
    static const char *bad[] = {"{\"tool\":\"bloat\"}",
                                "[{\"tool\":\"warp\"}]",
                                "[{\"points\":[[1,\"x\"]]}]",
                                "[[\"pucker\",1,2,3,4,[],null,7]]",
                                "[7]",
                                "[{\"size\":null}]"};
    static const char *good = "[{\"tool\":\"bloat\",\"size\":12.5,\"points\":[[1,2],[3,4,0.5]],\"amount\":3},"
                              "[\"thaw\"],{}]";
    yyjson_doc *d = NULL;
    pc_algo_liquify_stroke *s = NULL;
    size_t n = 0, i;
    pc_error e;
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(pc_json_parse(bad[i], strlen(bad[i]), &d, NULL) == PC_OK);
        CHECK(pc_algo_liquify_strokes_from_json(yyjson_doc_get_root(d), &s, &n, &e) == PC_ERR_INVALID);
        yyjson_doc_free(d);
    }
    CHECK(pc_json_parse(good, strlen(good), &d, NULL) == PC_OK);
    CHECK(pc_algo_liquify_strokes_from_json(yyjson_doc_get_root(d), &s, &n, NULL) == PC_OK);
    yyjson_doc_free(d);
    CHECK(n == 3 && s[0].tool == PC_ALGO_LIQUIFY_BLOAT && s[0].npoints == 2 && s[0].has_amount);
    CHECK(s[1].tool == PC_ALGO_LIQUIFY_THAW && s[1].size == 100.0 && s[2].rate == 80.0);
    CHECK(pc_algo_liquify_stroke_validate(&s[0], NULL) == PC_OK);
    s[0].size = 0.5;
    CHECK(pc_algo_liquify_stroke_validate(&s[0], &e) == PC_ERR_INVALID);
    CHECK_EQ_STR(e.msg, "brush size 0.5 out of range 1..15000");
    {
        pc_algo_liquify_stroke c;
        CHECK(pc_algo_liquify_stroke_copy(&s[0], &c, NULL) == PC_OK);
        CHECK(pc_algo_liquify_stroke_eq(&c, &s[0]));
        pc_algo_liquify_stroke_fini(&c);
    }
    pc_algo_liquify_strokes_free(s, n);
    CHECK_EQ_STR(pc_algo_liquify_tool_label(PC_ALGO_LIQUIFY_TWIRL_CCW), "Twirl Counterclockwise");
    CHECK_EQ_STR(pc_algo_liquify_tool_label((pc_algo_liquify_tool)99), "");
}

int main(int argc, char **argv)
{
    RUN(test_a_segment_to_a_far_off_point_returns);
    RUN(test_lasso_freezes_exactly_the_nodes_inside_a_concave_polygon);
    RUN(test_lasso_thaws_with_amount_zero);
    RUN(test_lasso_ignores_degenerate_and_hostile_polygons);
    RUN(test_no_strokes_is_identity_at_every_depth);
    RUN(test_forward_warp_displaces_along_the_stroke_and_reconstruct_restores);
    RUN(test_reconstruct_brush_reduces_the_field_and_freeze_protects);
    RUN(test_twirl_pucker_bloat_and_push_left_have_the_right_sense);
    RUN(test_replay_is_deterministic_and_depths_agree);
    RUN(test_proxy_matches_the_full_render_roughly);
    RUN(test_hostile_brushes_never_crash);
    RUN(test_hostile_fields_never_crash);
    RUN(test_json_and_validation);
    return pc_t_finish();
}
