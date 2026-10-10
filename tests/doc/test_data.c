/* Tests for the doc data types: adjust, effects, fill, mode, blend_if/advanced, vector, analysis,
 * slices (without a document's bounds) and variables. Ports the reference's #[test]s (except
 * serde-only assertions, in test_json*.c) plus never-crash and exact-value checks; exact values
 * were printed by rustc 1.95 from the reference code. */
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "doc/doc.h"
#include "harness.h"

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static pc_str S(const char *s)
{
    pc_str t = PC_STR_INIT;
    if (pc_str_set_c(&t, s, NULL) != PC_OK) {
        abort(); /* test-only */
    }
    return t;
}

/* ---- adjust.rs ---- */

static void test_hue_range_weights(void)
{
    pc_hue_range reds = pc_hue_range_neutral(0);
    pc_hue_range blues = pc_hue_range_neutral(4);
    pc_hue_range odd = pc_hue_range_neutral(0);
    pc_hue_range nan = pc_hue_range_neutral(0);
    size_t k;
    CHECK(pc_hue_range_weight(&reds, 0.0f) == 1.0f);
    CHECK(pc_hue_range_weight(&reds, 350.0f) == 1.0f);
    CHECK_NEAR(pc_hue_range_weight(&reds, 30.0f), 0.5, 1e-5);
    CHECK_NEAR(pc_hue_range_weight(&reds, 330.0f), 0.5, 1e-5);
    CHECK(pc_hue_range_weight(&reds, 60.0f) == 0.0f);
    CHECK(pc_hue_range_weight(&reds, 180.0f) == 0.0f);
    CHECK(pc_hue_range_weight(&blues, 240.0f) == 1.0f);
    CHECK(pc_hue_range_weight(&blues, 120.0f) == 0.0f);
    for (k = 0; k < 4; k++) {
        odd.bounds[k] = 10.0f;
        nan.bounds[k] = NAN;
    }
    CHECK(isfinite(pc_hue_range_weight(&odd, 10.0f)) && isfinite(pc_hue_range_weight(&odd, 200.0f)));
    CHECK(pc_hue_range_weight(&odd, 10.0f) == 1.0f);
    CHECK(pc_hue_range_weight(&odd, 200.0f) == 0.0f);
    CHECK(isfinite(pc_hue_range_weight(&nan, 10.0f)));
    CHECK(pc_hue_range_weight(&nan, 10.0f) == 0.0f);
    CHECK(pc_hue_range_weight(&nan, NAN) == 1.0f);
}

static void test_hue_range_exact(void)
{
    pc_hue_range reds = pc_hue_range_neutral(0);
    pc_hue_range d[6];
    CHECK_EQ_UINT(fbits(pc_hue_range_weight(&reds, 20.0f)), 0x3f555555u);
    CHECK_EQ_UINT(fbits(pc_hue_range_weight(&reds, 40.0f)), 0x3e2aaaabu);
    CHECK_EQ_UINT(fbits(pc_hue_range_weight(&reds, 337.3f)), 0x3f3e4b11u);
    CHECK(pc_hue_range_weight(&reds, -10.0f) == 1.0f);
    CHECK(pc_hue_range_weight(&reds, 725.0f) == 1.0f);
    CHECK(pc_hue_range_weight(&reds, NAN) == 0.0f);
    CHECK(pc_hue_range_weight(&reds, INFINITY) == 0.0f);
    pc_hue_range_defaults(d);
    CHECK(d[5].bounds[0] == 255.0f && d[5].bounds[3] == 345.0f);
    CHECK(pc_hue_range_is_neutral(&d[2]));
    d[2].lightness = -0.5f;
    CHECK(!pc_hue_range_is_neutral(&d[2]));
}

static void check_bounds(const float in[4], uint32_t b0, uint32_t b1, uint32_t b2, uint32_t b3)
{
    float out[4];
    pc_hue_range_canonical_bounds(in, out);
    CHECK_EQ_UINT(fbits(out[0]), b0);
    CHECK_EQ_UINT(fbits(out[1]), b1);
    CHECK_EQ_UINT(fbits(out[2]), b2);
    CHECK_EQ_UINT(fbits(out[3]), b3);
}

static void test_canonical_bounds(void)
{
    static const float a[4] = {315.0f, 345.0f, 15.0f, 45.0f};
    static const float b[4] = {10.0f, 5.0f, 0.0f, -5.0f};
    static const float c[4] = {100.3f, 1000.7f, -3.1f, 77.7f};
    static const float z[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    static const float big[4] = {1e30f, 2.0f, 3.0f, 4.0f};
    float n[4] = {NAN, 1.0f, 2.0f, 3.0f};
    float inf[4] = {0.0f, 1.0f, INFINITY, 3.0f};
    check_bounds(a, 0xc2340000u, 0xc1700000u, 0x41700000u, 0x42340000u);
    check_bounds(b, 0xc3af0000u, 0x40a00000u, 0x41100000u, 0x41100000u);
    check_bounds(c, 0x42c8999au, 0x438c599au, 0x43b27333u, 0x43dad999u);
    check_bounds(z, 0, 0, 0, 0);
    check_bounds(big, 0, 0, 0, 0);
    check_bounds(n, 0xc2340000u, 0xc1700000u, 0x41700000u, 0x42340000u);
    check_bounds(inf, 0xc2340000u, 0xc1700000u, 0x41700000u, 0x42340000u);
}

static void test_lut3d_len_rejects_tiny_and_overflowing_sizes(void)
{
    size_t n = 7;
    CHECK(!pc_lut3d_len(0, &n));
    CHECK(!pc_lut3d_len(1, &n));
    CHECK_EQ_UINT(n, 7); /* untouched on failure */
    CHECK(pc_lut3d_len(2, &n));
    CHECK_EQ_UINT(n, 24);
    CHECK(pc_lut3d_len(33, &n));
    CHECK_EQ_UINT(n, 33u * 33u * 33u * 3u);
    if (sizeof(size_t) >= 8) {
        CHECK(!pc_lut3d_len(1u << 22, &n)); /* (2^22)^3 overflows a 64-bit length */
    }
    CHECK(!pc_lut3d_len(UINT32_MAX, &n));
}

static void test_curves(void)
{
    pc_curve c = {NULL, 0};
    pc_curve d = {NULL, 0};
    pc_curve_point three[3] = {{0, 0}, {0.5f, 0.5f}, {1, 1}};
    pc_curve_point neg[2] = {{-0.0f, 0.0f}, {1, 1}};
    CHECK(pc_curve_is_identity(NULL, 0));
    CHECK(pc_curve_is_identity(three, 1));
    CHECK(!pc_curve_is_identity(three, 3));
    CHECK(pc_curve_is_identity(neg, 2)); /* -0.0 == 0.0 */
    CHECK(pc_curve_identity(&c, NULL) == PC_OK);
    CHECK(pc_curve_is_identity(c.pts, c.len));
    CHECK(pc_curve_clone(&c, &d, NULL) == PC_OK);
    CHECK(d.pts != c.pts && pc_curve_eq(&c, &d));
    d.pts[1].output = NAN;
    CHECK(!pc_curve_eq(&d, &d)); /* NaN != NaN, like the reference's PartialEq */
    pc_curve_fini(&c);
    pc_curve_fini(&d);
    pc_curve_fini(&d);
    pc_curve_fini(NULL);
    CHECK(d.pts == NULL && d.len == 0);
}

static void test_adjustment_basics(void)
{
    pc_adjustment a;
    pc_adjustment b;
    pc_adjustment z;
    size_t k;
    pc_levels_channel l = pc_levels_channel_default();
    CHECK(l.in_black == 0.0f && l.in_white == 1.0f && l.gamma == 1.0f && l.out_black == 0.0f && l.out_white == 1.0f);

    CHECK(pc_adjustment_identity_curves(&a, NULL) == PC_OK);
    CHECK_EQ_STR(pc_adjustment_label(&a), "Curves");
    CHECK(pc_curve_is_identity(a.u.curves.master.pts, a.u.curves.master.len) && a.u.curves.master.len == 2);
    for (k = 0; k < 3; k++) {
        CHECK_EQ_UINT(a.u.curves.per_channel[k].len, 2);
    }
    CHECK(a.u.curves.black.len == 0 && a.u.curves.space == PC_TONE_RGB);
    CHECK(pc_adjustment_clone(&a, &b, NULL) == PC_OK);
    CHECK(pc_adjustment_eq(&a, &b));
    CHECK(b.u.curves.per_channel[2].pts != a.u.curves.per_channel[2].pts);
    b.u.curves.per_channel[2].pts[1].output = 0.5f;
    CHECK(!pc_adjustment_eq(&a, &b));
    pc_adjustment_fini(&b);
    pc_adjustment_fini(&a);

    pc_adjustment_identity_levels(&a);
    CHECK_EQ_STR(pc_adjustment_label(&a), "Levels");
    pc_adjustment_default_hue_saturation(&b);
    CHECK_EQ_STR(pc_adjustment_label(&b), "Hue/Saturation");
    CHECK(!pc_adjustment_eq(&a, &b));
    CHECK(b.u.hue_saturation.ranges[1].bounds[0] == 15.0f);
    pc_adjustment_fini(&a);
    pc_adjustment_fini(&b);

    memset(&z, 0, sizeof z); /* zeroed = Brightness/Contrast 0, 0 */
    CHECK_EQ_STR(pc_adjustment_label(&z), "Brightness/Contrast");
    pc_adjustment_fini(&z);
    z.kind = PC_ADJ_UNSUPPORTED;
    CHECK_EQ_STR(pc_adjustment_label(&z), "Adjustment");
    z.kind = PC_ADJ_BLACK_WHITE;
    CHECK_EQ_STR(pc_adjustment_label(&z), "Black & White");
    pc_adjustment_fini(&z);
}

static void test_adjustment_lut_and_raw(void)
{
    float table[24];
    pc_adjustment a;
    pc_adjustment b;
    pc_adjustment u;
    pc_adjustment v;
    size_t n = 0;
    size_t i;
    const float *t;
    for (i = 0; i < 24; i++) {
        table[i] = (float)i / 23.0f;
    }
    memset(&a, 0, sizeof a);
    a.kind = PC_ADJ_COLOR_LOOKUP;
    a.u.color_lookup.name = S("Film");
    CHECK(pc_blob_new(table, sizeof table, &a.u.color_lookup.lut, NULL) == PC_OK);
    a.u.color_lookup.size = 2;
    t = pc_adjustment_lut(&a, &n);
    CHECK(t != NULL && n == 24 && t[23] == 1.0f);
    CHECK(pc_adjustment_clone(&a, &b, NULL) == PC_OK);
    CHECK(b.u.color_lookup.lut == a.u.color_lookup.lut); /* shared like an Arc */
    CHECK(b.u.color_lookup.name.ptr != a.u.color_lookup.name.ptr);
    CHECK(pc_adjustment_eq(&a, &b));
    /* A different blob with equal floats (-0.0 vs 0.0) still compares equal. */
    table[0] = -0.0f;
    pc_blob_release(b.u.color_lookup.lut);
    CHECK(pc_blob_new(table, sizeof table, &b.u.color_lookup.lut, NULL) == PC_OK);
    CHECK(pc_adjustment_eq(&a, &b));
    table[0] = NAN;
    pc_blob_release(b.u.color_lookup.lut);
    CHECK(pc_blob_new(table, sizeof table, &b.u.color_lookup.lut, NULL) == PC_OK);
    CHECK(!pc_adjustment_eq(&b, &b)); /* NaN in the table */
    pc_blob_release(b.u.color_lookup.lut);
    b.u.color_lookup.lut = NULL;
    CHECK(!pc_adjustment_eq(&a, &b));
    CHECK(pc_adjustment_lut(&b, &n) == NULL && n == 0);
    pc_adjustment_fini(&a);
    pc_adjustment_fini(&b);

    memset(&u, 0, sizeof u);
    u.kind = PC_ADJ_UNSUPPORTED;
    u.u.unsupported.psd_key = S("blwh");
    CHECK(pc_adjustment_clone(&u, &v, NULL) == PC_OK);
    CHECK(pc_adjustment_eq(&u, &v));
    CHECK(pc_blob_new(NULL, 0, &v.u.unsupported.raw, NULL) == PC_OK);
    CHECK(pc_adjustment_eq(&u, &v)); /* NULL raw == empty raw (both Vec::new()) */
    pc_adjustment_fini(&u);
    pc_adjustment_fini(&v);

    memset(&u, 0, sizeof u);
    u.kind = PC_ADJ_GRADIENT_MAP;
    u.u.gradient_map.stops = calloc(2, sizeof *u.u.gradient_map.stops);
    u.u.gradient_map.nstops = 2;
    u.u.gradient_map.stops[1].pos = 1.0f;
    CHECK(pc_adjustment_clone(&u, &v, NULL) == PC_OK);
    CHECK(pc_adjustment_eq(&u, &v) && v.u.gradient_map.stops != u.u.gradient_map.stops);
    v.u.gradient_map.stops[0].rgb[2] = 0.1f;
    CHECK(!pc_adjustment_eq(&u, &v));
    pc_adjustment_fini(&u);
    pc_adjustment_fini(&v);
}

static void test_adjustment_eq_by_kind(void)
{
    pc_adjustment a;
    pc_adjustment b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.kind = b.kind = PC_ADJ_BLACK_WHITE;
    a.u.black_white.tint[0] = 0.3f; /* ignored while has_tint is false (None) */
    CHECK(pc_adjustment_eq(&a, &b));
    a.u.black_white.has_tint = b.u.black_white.has_tint = true;
    CHECK(!pc_adjustment_eq(&a, &b));
    a.kind = b.kind = PC_ADJ_INVERT;
    CHECK(pc_adjustment_eq(&a, &b));
    b.kind = PC_ADJ_POSTERIZE;
    CHECK(!pc_adjustment_eq(&a, &b));
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.kind = b.kind = PC_ADJ_THRESHOLD;
    a.u.threshold.level = -0.0f;
    CHECK(pc_adjustment_eq(&a, &b));
    a.u.threshold.level = NAN;
    CHECK(!pc_adjustment_eq(&a, &a));
}

/* ---- effects.rs / Fill ---- */

static void test_effects_defaults_and_labels(void)
{
    pc_effect e;
    pc_effect c;
    pc_gradient g;
    pc_global_light gl = pc_global_light_default();
    pc_bevel_contour bc = pc_bevel_contour_default();
    pc_fx_common fc = pc_fx_common_new(PC_BLEND_SCREEN, 0.5f);
    CHECK(gl.angle == 120.0f && gl.altitude == 30.0f);
    CHECK(!bc.contour.custom && bc.range == 0.5f && !bc.anti_alias);
    CHECK(fc.enabled && fc.blend == PC_BLEND_SCREEN && fc.opacity == 0.5f);

    pc_effect_default_drop_shadow(&e);
    CHECK_EQ_STR(pc_effect_label(&e), "Drop Shadow");
    CHECK(pc_effect_enabled(&e));
    CHECK(e.u.shadow.common.blend == PC_BLEND_MULTIPLY && e.u.shadow.common.opacity == 0.75f);
    CHECK(e.u.shadow.angle == 120.0f && e.u.shadow.distance == 5.0f && e.u.shadow.size == 5.0f);
    CHECK(e.u.shadow.use_global_light && e.u.shadow.knocks_out && !e.u.shadow.contour.custom);
    CHECK(pc_effect_clone(&e, &c, NULL) == PC_OK && pc_effect_eq(&e, &c));
    c.u.shadow.common.enabled = false;
    CHECK(!pc_effect_enabled(&c) && !pc_effect_eq(&e, &c));
    c.kind = PC_FX_INNER_SHADOW;
    CHECK_EQ_STR(pc_effect_label(&c), "Inner Shadow");
    pc_effect_fini(&c);
    pc_effect_fini(&e);

    CHECK(pc_gradient_default(&g, NULL) == PC_OK);
    CHECK(g.nstops == 2 && g.stops[0].color.c[0] == 0.0f && g.stops[1].color.c[0] == 1.0f);
    CHECK(g.angle == 90.0f && g.scale == 1.0f && g.align && !g.reverse && g.nopacity_stops == 0);
    memset(&e, 0, sizeof e);
    e.kind = PC_FX_GRADIENT_OVERLAY;
    e.u.gradient_overlay.gradient = g; /* moved */
    CHECK_EQ_STR(pc_effect_label(&e), "Gradient Overlay");
    CHECK(pc_effect_clone(&e, &c, NULL) == PC_OK && pc_effect_eq(&e, &c));
    CHECK(c.u.gradient_overlay.gradient.stops != e.u.gradient_overlay.gradient.stops);
    c.u.gradient_overlay.gradient.stops[1].color.alpha = 0.5f;
    CHECK(!pc_effect_eq(&e, &c));
    pc_effect_fini(&c);
    pc_effect_fini(&e);

    memset(&e, 0, sizeof e);
    e.kind = PC_FX_BEVEL_EMBOSS;
    CHECK_EQ_STR(pc_effect_label(&e), "Bevel & Emboss");
    CHECK(!pc_effect_enabled(&e));
    e.u.bevel.enabled = true;
    e.u.bevel.has_texture = true;
    e.u.bevel.texture.name = S("Clouds");
    e.u.bevel.has_contour = true;
    e.u.bevel.contour = pc_bevel_contour_default();
    e.u.bevel.contour.contour.custom = true;
    e.u.bevel.contour.contour.name = S("Ring");
    CHECK(pc_curve_identity(&e.u.bevel.contour.contour.points, NULL) == PC_OK);
    CHECK(pc_effect_clone(&e, &c, NULL) == PC_OK && pc_effect_eq(&e, &c));
    CHECK(pc_effect_enabled(&c));
    c.u.bevel.contour.contour.points.pts[0].output = 0.25f;
    CHECK(!pc_effect_eq(&e, &c));
    pc_effect_fini(&c);
    pc_effect_fini(&e);
    CHECK(e.kind == PC_FX_DROP_SHADOW && e.u.bevel.texture.name.ptr == NULL);
}

static void test_contour_and_paint(void)
{
    pc_contour a;
    pc_contour b;
    pc_fx_paint p;
    pc_fx_paint q;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    b.name = S("ignored"); /* Linear has no fields */
    CHECK(pc_contour_eq(&a, &b));
    a.custom = b.custom = true;
    CHECK(!pc_contour_eq(&a, &b));
    pc_contour_fini(&a);
    pc_contour_fini(&b);

    memset(&p, 0, sizeof p);
    p.kind = PC_FX_PAINT_PATTERN;
    p.u.pattern.name = S("Bubbles");
    p.u.pattern.id = S("abc-123");
    p.u.pattern.scale = 1.0f;
    CHECK(pc_fx_paint_clone(&p, &q, NULL) == PC_OK && pc_fx_paint_eq(&p, &q));
    CHECK(q.u.pattern.id.ptr != p.u.pattern.id.ptr);
    q.u.pattern.scale = 2.0f;
    CHECK(!pc_fx_paint_eq(&p, &q));
    pc_fx_paint_fini(&p);
    pc_fx_paint_fini(&q);
    p.kind = PC_FX_PAINT_COLOR;
    p.u.color = PC_COLOR_WHITE;
    q.kind = PC_FX_PAINT_COLOR;
    q.u.color = PC_COLOR_WHITE;
    CHECK(pc_fx_paint_eq(&p, &q));
}

static void test_fill(void)
{
    pc_color_stop stops[2] = {{0.0f, PC_COLOR_BLACK}, {1.0f, PC_COLOR_WHITE}};
    pc_fill f;
    pc_fill g;
    pc_fill s = pc_fill_solid(PC_COLOR_BLACK);
    pc_fill t = pc_fill_solid(PC_COLOR_WHITE);
    CHECK(!pc_fill_eq(&s, &t));
    CHECK(pc_fill_gradient(stops, 2, 45.0f, 1.0f, PC_GRADIENT_RADIAL, true, &f, NULL) == PC_OK);
    CHECK(f.kind == PC_FILL_GRADIENT && f.u.gradient.align && !f.u.gradient.dither);
    CHECK(f.u.gradient.nopacity_stops == 0 && f.u.gradient.nmidpoints == 0 && f.u.gradient.reverse);
    CHECK(f.u.gradient.stops != stops && f.u.gradient.stops[1].pos == 1.0f);
    CHECK(pc_fill_clone(&f, &g, NULL) == PC_OK && pc_fill_eq(&f, &g));
    g.u.gradient.midpoints = malloc(sizeof(float));
    g.u.gradient.midpoints[0] = 0.5f;
    g.u.gradient.nmidpoints = 1;
    CHECK(!pc_fill_eq(&f, &g));
    pc_fill_fini(&g);
    CHECK(pc_fill_gradient(NULL, 0, 0.0f, 1.0f, PC_GRADIENT_LINEAR, false, &g, NULL) == PC_OK);
    CHECK(g.u.gradient.stops == NULL && !pc_fill_eq(&f, &g));
    pc_fill_fini(&g);
    pc_fill_fini(&f);

    memset(&f, 0, sizeof f);
    f.kind = PC_FILL_PATTERN;
    f.u.pattern.name = S("P");
    f.u.pattern.link = true;
    CHECK(pc_fill_clone(&f, &g, NULL) == PC_OK && pc_fill_eq(&f, &g));
    g.u.pattern.link = false;
    CHECK(!pc_fill_eq(&f, &g));
    pc_fill_fini(&f);
    pc_fill_fini(&g);
    pc_fill_fini(NULL);
}

/* ---- mode.rs ---- */

static void test_color_table_nearest(void)
{
    uint8_t cols[3][3] = {{0, 0, 0}, {255, 255, 255}, {255, 0, 0}};
    uint8_t ties[4][3] = {{10, 10, 10}, {10, 10, 10}, {0, 0, 0}, {255, 255, 255}};
    pc_color_table t = {cols, 3, false, 0};
    pc_color_table u = {ties, 4, false, 0};
    pc_color_table empty = {NULL, 0, false, 0};
    pc_color_table c;
    float a[3] = {0.9f, 0.1f, 0.1f};
    float b[3] = {0.6f, 0.6f, 0.6f};
    float g10[3] = {10.0f / 255.0f, 10.0f / 255.0f, 10.0f / 255.0f};
    float one[3] = {1.0f, 1.0f, 1.0f};
    float nan[3] = {NAN, NAN, NAN};
    float inf[3] = {INFINITY, 0.0f, 0.0f};
    float rgb[3];
    CHECK_EQ_UINT(pc_color_table_nearest(&t, a), 2);
    CHECK_EQ_UINT(pc_color_table_nearest(&t, b), 1);
    pc_color_table_rgb(&t, 2, rgb);
    CHECK(rgb[0] == 1.0f && rgb[1] == 0.0f && rgb[2] == 0.0f);
    pc_color_table_rgb(&t, 99, rgb);
    CHECK(rgb[0] == 0.0f && rgb[1] == 0.0f && rgb[2] == 0.0f);
    CHECK_EQ_UINT(pc_color_table_nearest(&u, g10), 0); /* tie keeps the lower index */
    CHECK_EQ_UINT(pc_color_table_nearest(&u, one), 3);
    CHECK_EQ_UINT(pc_color_table_nearest(&u, nan), 0);
    CHECK_EQ_UINT(pc_color_table_nearest(&u, inf), 0);
    CHECK_EQ_UINT(pc_color_table_nearest(&empty, a), 0);

    CHECK(pc_color_table_clone(&t, &c, NULL) == PC_OK && pc_color_table_eq(&t, &c));
    c.has_transparent = true;
    CHECK(!pc_color_table_eq(&t, &c));
    t.transparent = 5; /* ignored while None */
    c.has_transparent = false;
    CHECK(pc_color_table_eq(&t, &c));
    c.colors[0][1] = 1;
    CHECK(!pc_color_table_eq(&t, &c));
    pc_color_table_fini(&c);
    pc_color_table_fini(&c);
}

static pc_duotone_ink ink(const char *name, float r, float g, float b)
{
    pc_duotone_ink i;
    float c[3];
    c[0] = r;
    c[1] = g;
    c[2] = b;
    if (pc_duotone_ink_new(name, c, &i, NULL) != PC_OK) {
        abort();
    }
    return i;
}

static void test_duotone_render_black_ink_is_gray(void)
{
    pc_duotone d;
    float gs[3] = {0.0f, 0.25f, 1.0f};
    float c[3];
    size_t i;
    memset(&d, 0, sizeof d);
    d.inks = calloc(2, sizeof *d.inks);
    d.inks[0] = ink("Black", 0, 0, 0);
    d.ninks = 1;
    for (i = 0; i < 3; i++) {
        pc_duotone_render(&d, gs[i], c);
        CHECK(fabsf(c[0] - gs[i]) < 1e-6f && c[0] == c[2]);
    }
    d.inks[1] = ink("Orange", 1.0f, 0.5f, 0.0f);
    d.ninks = 2;
    pc_duotone_render(&d, 0.5f, c);
    CHECK(c[0] > c[2]);
    pc_duotone_fini(&d);
}

static void test_duotone_exact(void)
{
    pc_duotone d;
    pc_duotone e;
    pc_adjustment adj;
    pc_curve_point pts[4] = {{0, 0}, {0.5f, 0.5f}, {0.5f, 1.0f}, {1.0f, 0.25f}};
    pc_curve_point ip[3] = {{0.0f, 0.1f}, {0.3f, 0.2f}, {1.0f, 0.9f}};
    float c[3];
    memset(&d, 0, sizeof d);
    d.inks = calloc(2, sizeof *d.inks);
    d.inks[0] = ink("Black", 0, 0, 0);
    d.inks[1] = ink("Orange", 1.0f, 0.5f, 0.0f);
    d.ninks = 2;
    free(d.inks[1].curve.pts);
    d.inks[1].curve.pts = malloc(sizeof ip);
    memcpy(d.inks[1].curve.pts, ip, sizeof ip);
    d.inks[1].curve.len = 3;
    pc_duotone_render(&d, 7.0f / 32.0f, c);
    CHECK_EQ_UINT(fbits(c[0]), 0x3e600000u);
    CHECK_EQ_UINT(fbits(c[1]), 0x3e13b333u);
    CHECK_EQ_UINT(fbits(c[2]), 0x3d8eccceu);
    pc_duotone_render(&d, 13.0f / 32.0f, c);
    CHECK_EQ_UINT(fbits(c[0]), 0x3ed00000u);
    CHECK_EQ_UINT(fbits(c[1]), 0x3e9ca667u);
    CHECK_EQ_UINT(fbits(c[2]), 0x3e52999au);
    pc_duotone_render(&d, 1.0f, c);
    CHECK_EQ_UINT(fbits(c[1]), 0x3f733333u);
    CHECK_EQ_UINT(fbits(c[2]), 0x3f666666u);

    CHECK(pc_duotone_display_adjustment(&d, &adj, NULL) == PC_OK);
    CHECK(adj.kind == PC_ADJ_GRADIENT_MAP && adj.u.gradient_map.nstops == 33);
    CHECK(adj.u.gradient_map.stops[7].pos == 0.21875f);
    CHECK_EQ_UINT(fbits(adj.u.gradient_map.stops[7].rgb[1]), 0x3e13b333u);
    CHECK(!adj.u.gradient_map.reverse && !adj.u.gradient_map.dither);
    pc_adjustment_fini(&adj);

    CHECK(pc_duotone_clone(&d, &e, NULL) == PC_OK && pc_duotone_eq(&d, &e));
    CHECK(pc_blob_new("x", 1, &e.psd_raw, NULL) == PC_OK);
    CHECK(!pc_duotone_eq(&d, &e));
    pc_duotone_fini(&e);
    CHECK(pc_blob_new(NULL, 0, &d.psd_raw, NULL) == PC_OK);
    CHECK(pc_duotone_clone(&d, &e, NULL) == PC_OK && pc_duotone_eq(&d, &e));
    pc_blob_release(e.psd_raw);
    e.psd_raw = NULL;
    CHECK(!pc_duotone_eq(&d, &e)); /* Some(empty) != None */
    pc_duotone_fini(&e);
    CHECK(pc_duotone_ink_density(&d.inks[1], -5.0f) == 0.1f);
    CHECK(pc_duotone_ink_density(&d.inks[1], 9.0f) == 0.9f);
    pc_duotone_fini(&d);

    CHECK_EQ_UINT(fbits(pc_duotone_curve_eval(pts, 4, -1.0f)), 0);
    CHECK_EQ_UINT(fbits(pc_duotone_curve_eval(pts, 4, 0.25f)), 0x3e800000u);
    CHECK_EQ_UINT(fbits(pc_duotone_curve_eval(pts, 4, 0.5f)), 0x3f000000u);
    CHECK_EQ_UINT(fbits(pc_duotone_curve_eval(pts, 4, 0.75f)), 0x3f200000u);
    CHECK_EQ_UINT(fbits(pc_duotone_curve_eval(pts, 4, 2.0f)), 0x3e800000u);
    CHECK_EQ_UINT(fbits(pc_duotone_curve_eval(pts, 4, NAN)), 0x3e800000u);
    CHECK(pc_duotone_curve_eval(NULL, 0, 0.3f) == 0.3f);
    CHECK(pc_duotone_curve_eval(pts + 3, 1, 0.3f) == 0.25f);
}

static void test_stack_mode_ids_roundtrip(void)
{
    size_t i;
    pc_stack_mode m;
    for (i = 0; i < PC_STACK_MODE_COUNT; i++) {
        const char *id = pc_stack_mode_id((pc_stack_mode)i);
        CHECK(pc_stack_mode_from_id(id, strlen(id), &m) && m == (pc_stack_mode)i);
    }
    CHECK_EQ_STR(pc_stack_mode_id(PC_STACK_STANDARD_DEVIATION), "standardDeviation");
    CHECK_EQ_STR(pc_stack_mode_label(PC_STACK_STANDARD_DEVIATION), "Standard Deviation");
    CHECK(!pc_stack_mode_from_id("Mean", 4, &m));
    CHECK(!pc_stack_mode_from_id("mean", 3, &m));
    CHECK(!pc_stack_mode_from_id("", 0, &m));
    CHECK(!pc_stack_mode_from_id(NULL, 5, &m));
}

/* ---- blend_if.rs / advanced.rs ---- */

static pc_blend_range br(uint8_t bl, uint8_t bh, uint8_t wl, uint8_t wh)
{
    pc_blend_range r;
    r.black[0] = bl;
    r.black[1] = bh;
    r.white[0] = wl;
    r.white[1] = wh;
    return r;
}

static void test_full_range_shows_everything(void)
{
    pc_blend_range r = PC_BLEND_RANGE_FULL;
    float vs[5] = {0.0f, 0.5f, 127.0f, 254.9f, 255.0f};
    size_t i;
    CHECK(pc_blend_range_is_full(r));
    for (i = 0; i < 5; i++) {
        CHECK(pc_blend_range_weight(r, vs[i]) == 1.0f);
    }
}

static void test_unsplit_points_are_hard_thresholds(void)
{
    pc_blend_range r = br(50, 50, 200, 200);
    CHECK(pc_blend_range_weight(r, 49.0f) == 0.0f);
    CHECK(pc_blend_range_weight(r, 49.9f) == 0.0f);
    CHECK(pc_blend_range_weight(r, 50.0f) == 1.0f);
    CHECK(pc_blend_range_weight(r, 200.0f) == 1.0f);
    CHECK(pc_blend_range_weight(r, 200.1f) == 0.0f);
    CHECK(!pc_blend_range_is_full(r));
}

static void test_split_points_ramp_linearly(void)
{
    pc_blend_range r = br(0, 100, 155, 255);
    CHECK(pc_blend_range_weight(r, 0.0f) == 0.0f);
    CHECK_NEAR(pc_blend_range_weight(r, 50.0f), 0.5, 1e-6);
    CHECK(pc_blend_range_weight(r, 100.0f) == 1.0f);
    CHECK(pc_blend_range_weight(r, 155.0f) == 1.0f);
    CHECK_NEAR(pc_blend_range_weight(r, 205.0f), 0.5, 1e-6);
    CHECK(pc_blend_range_weight(r, 255.0f) == 0.0f);
    CHECK(!pc_blend_range_is_full(r));
    CHECK(isnan(pc_blend_range_weight(r, NAN))); /* clamp keeps NaN, as in Rust */
    CHECK(pc_blend_range_weight(r, -INFINITY) == 0.0f);
}

static void test_bytes_round_trip_in_psd_order(void)
{
    pc_blend_range r = br(10, 20, 230, 240);
    uint8_t b[4];
    pc_blend_range_to_bytes(r, b);
    CHECK(b[0] == 10 && b[1] == 20 && b[2] == 230 && b[3] == 240);
    CHECK(pc_blend_range_eq(pc_blend_range_from_bytes(b), r));
}

static void test_set_trims_trailing_full_entries(void)
{
    pc_blend_if b = {NULL, 0};
    pc_blend_if empty = {NULL, 0};
    pc_blend_if c;
    pc_blend_range r = br(30, 30, 255, 255);
    pc_blend_range pair[2];
    pc_blend_range got[2];
    CHECK(pc_blend_if_is_default(&b));
    pair[0] = PC_BLEND_RANGE_FULL;
    pair[1] = r;
    CHECK(pc_blend_if_set(&b, 2, pair, NULL) == PC_OK);
    CHECK_EQ_UINT(b.nranges, 3);
    pc_blend_if_get(&b, 2, got);
    CHECK(pc_blend_range_eq(got[0], PC_BLEND_RANGE_FULL) && pc_blend_range_eq(got[1], r));
    pc_blend_if_get(&b, 7, got);
    CHECK(pc_blend_range_is_full(got[0]) && pc_blend_range_is_full(got[1]));
    CHECK(!pc_blend_if_is_default(&b));
    CHECK(pc_blend_if_clone(&b, &c, NULL) == PC_OK && pc_blend_if_eq(&b, &c) && c.ranges != b.ranges);
    pair[1] = PC_BLEND_RANGE_FULL;
    CHECK(pc_blend_if_set(&b, 2, pair, NULL) == PC_OK);
    CHECK(pc_blend_if_eq(&b, &empty));
    CHECK(b.ranges == NULL);
    pc_blend_if_fini(&b);
    pc_blend_if_fini(&c);
}

static void test_blend_if_hostile_indices(void)
{
    pc_blend_if b = {NULL, 0};
    pc_blend_range pair[2] = {PC_BLEND_RANGE_FULL, PC_BLEND_RANGE_FULL};
    pc_blend_range r = br(1, 2, 3, 4);
    pc_error err;
    /* A full pair at any index is a no-op (the trim removes the growth again). */
    CHECK(pc_blend_if_set(&b, SIZE_MAX, pair, NULL) == PC_OK && b.nranges == 0);
    pair[0] = r;
    CHECK(pc_blend_if_set(&b, 1, pair, NULL) == PC_OK && b.nranges == 2);
    CHECK(pc_blend_if_set(&b, SIZE_MAX, pair, &err) == PC_ERR_LIMIT);
    CHECK(err.code == PC_ERR_LIMIT);
    CHECK(pc_blend_if_set(&b, (size_t)1 << 40 & SIZE_MAX, pair, NULL) != PC_OK || sizeof(size_t) < 8);
    CHECK_EQ_UINT(b.nranges, 2); /* unchanged on failure */
    /* Existing trailing full entries (e.g. from a file) go when a full pair is set past them. */
    b.ranges[1][0] = PC_BLEND_RANGE_FULL;
    b.ranges[1][1] = PC_BLEND_RANGE_FULL;
    pair[0] = PC_BLEND_RANGE_FULL;
    CHECK(pc_blend_if_set(&b, 5, pair, NULL) == PC_OK && b.nranges == 0 && b.ranges == NULL);
    pc_blend_if_fini(&b);
    pc_blend_if_fini(NULL);
}

static void test_advanced_defaults_match_photoshop(void)
{
    pc_advanced_blending a = pc_advanced_blending_default();
    pc_advanced_blending c;
    CHECK(a.knockout == PC_KNOCKOUT_NONE);
    CHECK(!a.blend_interior);
    CHECK(a.blend_clipped);
    CHECK(a.transparency_shapes);
    CHECK(!a.layer_mask_hides_effects);
    CHECK(!a.vector_mask_hides_effects);
    CHECK(pc_advanced_blending_is_default(&a));
    c = a;
    c.blend_clipped = false;
    CHECK(!pc_advanced_blending_is_default(&c));
    CHECK(!pc_advanced_blending_eq(&a, &c));
}

static void test_knockout_psd_values_and_names(void)
{
    pc_knockout all[3] = {PC_KNOCKOUT_NONE, PC_KNOCKOUT_SHALLOW, PC_KNOCKOUT_DEEP};
    pc_knockout k;
    size_t i;
    for (i = 0; i < 3; i++) {
        const char *n = pc_knockout_name(all[i]);
        CHECK(pc_knockout_from_psd(pc_knockout_to_psd(all[i])) == all[i]);
        CHECK(pc_knockout_from_name(n, strlen(n), &k) && k == all[i]);
    }
    CHECK(pc_knockout_from_psd(7) == PC_KNOCKOUT_DEEP);
    CHECK(pc_knockout_from_name("DEEP", 4, &k) && k == PC_KNOCKOUT_DEEP);
    CHECK(pc_knockout_from_name("ShAlLoW", 7, &k) && k == PC_KNOCKOUT_SHALLOW);
    CHECK(!pc_knockout_from_name("medium", 6, &k));
    CHECK(!pc_knockout_from_name("deep ", 5, &k));
    CHECK(!pc_knockout_from_name("dee", 3, &k));
    CHECK(!pc_knockout_from_name(NULL, 0, &k));
}

/* ---- vector.rs ---- */

static void test_segments_open_and_closed(void)
{
    const double pts[3][2] = {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}};
    pc_subpath s;
    pc_subpath o;
    pc_point seg[4];
    CHECK(pc_subpath_polygon(pts, 3, &s, NULL) == PC_OK);
    CHECK_EQ_UINT(pc_subpath_segment_count(&s), 3);
    CHECK(pc_subpath_polyline(pts, 3, &o, NULL) == PC_OK);
    CHECK_EQ_UINT(pc_subpath_segment_count(&o), 2);
    pc_subpath_segment(&o, 1, seg);
    CHECK(seg[3].x == 10.0 && seg[3].y == 10.0);
    pc_subpath_segment(&s, 2, seg); /* the closing segment back to the first knot */
    CHECK(seg[0].x == 10.0 && seg[0].y == 10.0 && seg[3].x == 0.0 && seg[3].y == 0.0);
    pc_subpath_segment(&o, 2, seg); /* out of range: zeros, no read past the knots */
    CHECK(seg[0].x == 0.0 && seg[3].y == 0.0);
    CHECK(!s.knots[0].smooth && s.closed && !o.closed && o.op == PC_PATH_COMBINE);
    pc_subpath_fini(&s);
    pc_subpath_fini(&o);
    CHECK(pc_subpath_polygon(NULL, 0, &s, NULL) == PC_OK && pc_subpath_segment_count(&s) == 0);
    CHECK(pc_subpath_polygon(pts, 1, &o, NULL) == PC_OK && pc_subpath_segment_count(&o) == 0);
    pc_subpath_fini(&s);
    pc_subpath_fini(&o);
}

static void test_first_op_is_combine_and_bounds(void)
{
    const double pts[3][2] = {{1.0, 2.0}, {5.0, 2.0}, {5.0, 9.0}};
    pc_path p;
    pc_path t;
    double b[4];
    memset(&p, 0, sizeof p);
    p.subpaths = calloc(1, sizeof *p.subpaths);
    CHECK(pc_subpath_polygon(pts, 3, &p.subpaths[0], NULL) == PC_OK);
    p.subpaths[0].op = PC_PATH_INTERSECT;
    p.nsubpaths = 1;
    CHECK(pc_path_effective_op(&p, 0) == PC_PATH_COMBINE);
    CHECK(pc_path_effective_op(&p, 5) == PC_PATH_COMBINE); /* out of range: no crash */
    CHECK(pc_path_control_bounds(&p, b));
    CHECK(b[0] == 1.0 && b[1] == 2.0 && b[2] == 5.0 && b[3] == 9.0);
    CHECK(pc_path_transform(&p, pc_affine_translate(1.0, 1.0), &t, NULL) == PC_OK);
    CHECK(pc_path_control_bounds(&t, b));
    CHECK(b[0] == 2.0 && b[1] == 3.0 && b[2] == 6.0 && b[3] == 10.0);
    CHECK(!pc_path_eq(&p, &t));
    pc_path_fini(&t);
    CHECK(pc_path_clone(&p, &t, NULL) == PC_OK && pc_path_eq(&p, &t));
    t.subpaths[0].knots[2].smooth = true;
    CHECK(!pc_path_eq(&p, &t));
    pc_path_fini(&t);
    pc_path_fini(&p);
    CHECK(!pc_path_control_bounds(&p, b));
    CHECK(pc_path_is_empty(&p));
}

static void test_path_components_and_bounds_nan(void)
{
    const double pts[2][2] = {{0.0, 0.0}, {NAN, 4.0}};
    pc_path p;
    pc_index_range *r = NULL;
    size_t n = 99;
    double b[4];
    size_t i;
    pc_path_op ops[5] = {PC_PATH_JOIN, PC_PATH_JOIN, PC_PATH_SUBTRACT, PC_PATH_JOIN, PC_PATH_COMBINE};
    memset(&p, 0, sizeof p);
    CHECK(pc_path_components(&p, &r, &n, NULL) == PC_OK && r == NULL && n == 0);
    p.subpaths = calloc(5, sizeof *p.subpaths);
    p.nsubpaths = 5;
    for (i = 0; i < 5; i++) {
        p.subpaths[i].op = ops[i];
    }
    CHECK(pc_path_is_empty(&p));
    CHECK(!pc_path_control_bounds(&p, b));
    CHECK(pc_path_components(&p, &r, &n, NULL) == PC_OK);
    /* A leading Join still starts the first component. */
    CHECK_EQ_UINT(n, 3);
    CHECK(r[0].start == 0 && r[0].end == 2 && r[1].start == 2 && r[1].end == 4 && r[2].start == 4 &&
          r[2].end == 5);
    free(r);
    CHECK(pc_path_effective_op(&p, 2) == PC_PATH_SUBTRACT);
    pc_subpath_fini(&p.subpaths[1]);
    CHECK(pc_subpath_polyline(pts, 2, &p.subpaths[1], NULL) == PC_OK);
    CHECK(!pc_path_is_empty(&p));
    CHECK(pc_path_control_bounds(&p, b)); /* f64::min ignores the NaN */
    CHECK(b[0] == 0.0 && b[1] == 0.0 && b[2] == 0.0 && b[3] == 4.0);
    pc_path_fini(&p);
}

static void test_vector_types(void)
{
    pc_path empty;
    pc_vector_mask m;
    pc_vector_mask m2;
    pc_shape_stroke s;
    pc_shape_stroke s2;
    pc_named_path np;
    pc_named_path np2;
    pc_clipping_path cp;
    pc_clipping_path cp2;
    pc_knot k = pc_knot_smooth(pc_point_make(1, 2), pc_point_make(0, 2), pc_point_make(2, 2));
    pc_knot k2 = pc_knot_corner(1, 2);
    CHECK(k.smooth && !k2.smooth && !pc_knot_eq(&k, &k2));
    k2 = pc_knot_transform(k, PC_AFFINE_IDENTITY);
    CHECK(pc_knot_eq(&k, &k2));

    memset(&empty, 0, sizeof empty);
    pc_vector_mask_init(&m, &empty);
    CHECK(m.enabled && m.linked && m.density == 1.0f && m.feather == 0.0f);
    CHECK(pc_vector_mask_clone(&m, &m2, NULL) == PC_OK && pc_vector_mask_eq(&m, &m2));
    m2.feather = 2.0f;
    CHECK(!pc_vector_mask_eq(&m, &m2));
    pc_vector_mask_fini(&m);
    pc_vector_mask_fini(&m2);

    pc_shape_stroke_default(&s);
    CHECK(s.width == 3.0f && s.opacity == 1.0f && s.align == PC_STROKE_ALIGN_CENTER && s.cap == PC_LINE_CAP_BUTT);
    CHECK(s.join == PC_LINE_JOIN_MITER && s.miter_limit == 100.0f && s.ndashes == 0 && s.paint.kind == PC_FILL_SOLID);
    s.dashes = malloc(2 * sizeof(float));
    s.dashes[0] = 2.0f;
    s.dashes[1] = 1.0f;
    s.ndashes = 2;
    CHECK(pc_shape_stroke_clone(&s, &s2, NULL) == PC_OK && pc_shape_stroke_eq(&s, &s2) && s.dashes != s2.dashes);
    s2.dashes[1] = 1.5f;
    CHECK(!pc_shape_stroke_eq(&s, &s2));
    pc_shape_stroke_fini(&s);
    pc_shape_stroke_fini(&s2);

    memset(&np, 0, sizeof np);
    np.name = S("Path 1");
    CHECK(pc_blob_new("raw", 3, &np.psd_raw, NULL) == PC_OK);
    CHECK(pc_named_path_clone(&np, &np2, NULL) == PC_OK && pc_named_path_eq(&np, &np2));
    CHECK(np2.psd_raw == np.psd_raw);
    pc_str_fini(&np2.name);
    CHECK(!pc_named_path_eq(&np, &np2));
    pc_named_path_fini(&np);
    pc_named_path_fini(&np2);

    cp.name = S("Path 1");
    cp.flatness = 0.0f;
    CHECK(pc_clipping_path_clone(&cp, &cp2, NULL) == PC_OK && pc_clipping_path_eq(&cp, &cp2));
    cp2.flatness = -0.0f;
    CHECK(pc_clipping_path_eq(&cp, &cp2));
    pc_clipping_path_fini(&cp);
    pc_clipping_path_fini(&cp2);
}

static void test_live_shape_and_shape_layer(void)
{
    pc_live_shape a;
    pc_live_shape b;
    pc_shape_layer l;
    pc_shape_layer l2;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.kind = b.kind = PC_LIVE_ELLIPSE;
    a.radii[0] = 5.0; /* not a field of Ellipse */
    a.sides = 9;
    CHECK(pc_live_shape_eq(&a, &b));
    a.kind = b.kind = PC_LIVE_RECT;
    CHECK(!pc_live_shape_eq(&a, &b));
    a.kind = b.kind = PC_LIVE_POLYGON;
    CHECK(!pc_live_shape_eq(&a, &b));
    a.kind = b.kind = PC_LIVE_LINE;
    a.weight = NAN;
    CHECK(!pc_live_shape_eq(&a, &a));
    b.kind = PC_LIVE_RECT;
    CHECK(!pc_live_shape_eq(&a, &b));

    memset(&l, 0, sizeof l);
    l.has_fill = true;
    l.fill = pc_fill_solid(PC_COLOR_WHITE);
    l.has_stroke = true;
    pc_shape_stroke_default(&l.stroke);
    l.has_live = true;
    l.live.kind = PC_LIVE_RECT;
    l.live.rect[2] = 10.0;
    l.has_cache = true;
    pc_surface_init(&l.cache, (pc_pixel_format){PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true});
    CHECK(pc_shape_layer_clone(&l, &l2, NULL) == PC_OK && pc_shape_layer_eq(&l, &l2));
    l2.has_fill = false;
    CHECK(!pc_shape_layer_eq(&l, &l2));
    l2.has_fill = true;
    l2.live.rect[2] = 11.0;
    CHECK(!pc_shape_layer_eq(&l, &l2));
    l2.live.rect[2] = 10.0;
    CHECK(pc_blob_new(NULL, 0, &l2.psd_raw, NULL) == PC_OK);
    CHECK(!pc_shape_layer_eq(&l, &l2));
    pc_shape_layer_fini(&l2);
    pc_shape_layer_fini(&l);
    pc_shape_layer_fini(&l);
}

/* ---- analysis.rs ---- */

static void check_describe(double p, double l, const char *units, const char *want)
{
    pc_measurement_scale s;
    pc_str t = PC_STR_INIT;
    memset(&s, 0, sizeof s);
    s.pixel_length = p;
    s.logical_length = l;
    s.units = S(units);
    CHECK(pc_measurement_scale_describe(&s, &t, NULL) == PC_OK);
    CHECK_EQ_STR(pc_str_c(&t), want);
    pc_str_fini(&t);
    pc_measurement_scale_fini(&s);
}

static void test_scale_factor_and_text(void)
{
    pc_measurement_scale s;
    pc_measurement_scale d;
    memset(&s, 0, sizeof s);
    s.pixel_length = 100.0;
    s.logical_length = 2.5;
    s.units = S("cm");
    CHECK_NEAR(pc_measurement_scale_factor(&s), 0.025, 1e-12);
    check_describe(100.0, 2.5, "cm", "100 pixels = 2.5000 cm");
    CHECK(pc_measurement_scale_default(&d, NULL) == PC_OK);
    CHECK(pc_measurement_scale_is_default(&d));
    CHECK(!pc_measurement_scale_is_default(&s));
    d.pixel_length = 0.0;
    CHECK(pc_measurement_scale_factor(&d) == 1.0);
    d.pixel_length = NAN;
    CHECK(pc_measurement_scale_factor(&d) == 1.0);
    pc_measurement_scale_fini(&s);
    pc_measurement_scale_fini(&d);
}

static void test_describe_formatting(void)
{
    /* Expected strings printed by the reference's describe() under rustc 1.95. */
    check_describe(1.0, 1.0, "pixels", "1 pixels = 1.0000 pixels");
    check_describe(2.5, 0.125, "in", "2.5 pixels = 0.1250 in");
    check_describe(0.1, 1.00005, "mm", "0.1 pixels = 1.0001 mm");
    check_describe(-0.0, -0.0, "x", "0 pixels = -0.0000 x");
    check_describe(NAN, NAN, "n", "NaN pixels = NaN n");
    check_describe(INFINITY, -INFINITY, "i", "inf pixels = -inf i");
    check_describe(1e300, 1e-7, "u", "9223372036854775807 pixels = 0.0000 u");
    check_describe(-1e300, 0.00025, "u", "-9223372036854775808 pixels = 0.0003 u");
    check_describe(1e-7, 0.00025, "u", "0.0000001 pixels = 0.0003 u");
    check_describe(3.0e-5, 12345.67895, "km", "0.00003 pixels = 12345.6789 km");
    check_describe(-7.0, 9.99995, "", "-7 pixels = 10.0000 ");
    check_describe(-2.75, 1e21, "m", "-2.75 pixels = 1000000000000000000000.0000 m");
    check_describe(123456789.123456789, 0.1, "m", "123456789.12345679 pixels = 0.1000 m");
    check_describe(1.0 / 3.0, 0.0, "m", "0.3333333333333333 pixels = 0.0000 m");
    /* An exact tie between two 17-digit candidates: Rust's Display rounds it up (ryu would print
     * ...6562). */
    check_describe(3578819939694.65625, 0.5, "m", "3578819939694.6563 pixels = 0.5000 m");
    check_describe(0.3, 2.00005, "m", "0.3 pixels = 2.0000 m");
    {
        char want[400] = "0.";
        size_t i;
        for (i = 0; i < 323; i++) {
            strcat(want, "0");
        }
        strcat(want, "5 pixels = 0.0000 m");
        check_describe(5e-324, 0.0, "m", want);
    }
}

static void test_measurement(void)
{
    pc_measurement m;
    pc_measurement c;
    pc_count_group g;
    pc_note n;
    pc_note n2;
    CHECK(pc_measurement_default(&m, NULL) == PC_OK);
    CHECK(pc_measurement_is_empty(&m));
    CHECK(pc_count_group_default(&g, NULL) == PC_OK);
    CHECK(pc_str_eq_c(&g.name, "Count Group 1") && g.color.c[0] == 1.0f && g.color.c[1] == 0.0f);
    CHECK(g.marker_size == 1 && g.label_size == 8 && g.visible && g.npoints == 0);
    g.points = calloc(2, sizeof *g.points);
    g.points[0][0] = 1.0;
    g.points[0][1] = 2.0;
    g.points[1][0] = 3.0;
    g.points[1][1] = 4.0;
    g.npoints = 2;
    m.count_groups = calloc(2, sizeof *m.count_groups);
    CHECK(pc_count_group_clone(&g, &m.count_groups[0], NULL) == PC_OK);
    m.count_groups[1] = g; /* moved */
    m.ncount_groups = 2;
    CHECK_EQ_UINT(pc_measurement_count_total(&m), 4);
    CHECK(!pc_measurement_is_empty(&m));
    CHECK(pc_measurement_clone(&m, &c, NULL) == PC_OK && pc_measurement_eq(&m, &c));
    c.count_groups[1].points[1][1] = 4.5;
    CHECK(!pc_measurement_eq(&m, &c));
    c.count_groups[1].points[1][1] = 4.0;
    c.has_ruler = true;
    CHECK(!pc_measurement_eq(&m, &c));
    m.has_ruler = true;
    CHECK(pc_measurement_eq(&m, &c));
    c.ruler.protractor[0] = 3.0; /* ignored while None */
    CHECK(pc_measurement_eq(&m, &c));
    c.ruler.has_protractor = true;
    CHECK(!pc_measurement_eq(&m, &c));
    pc_measurement_fini(&c);
    pc_measurement_fini(&m);
    pc_measurement_fini(&m);

    pc_note_default(&n);
    CHECK(n.color.c[0] == 1.0f && n.color.c[1] == 1.0f && n.color.c[2] == 0.51f && n.color.alpha == 1.0f);
    CHECK(!n.open && n.text.len == 0);
    n.text = S("hi");
    n.author = S("me");
    CHECK(pc_note_clone(&n, &n2, NULL) == PC_OK && pc_note_eq(&n, &n2));
    n2.popup[3] = 1.0;
    CHECK(!pc_note_eq(&n, &n2));
    pc_note_fini(&n);
    pc_note_fini(&n2);
}

/* ---- slices.rs (document-free parts) ---- */

static int64_t area(const pc_rect *rs, size_t n)
{
    int64_t a = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        a += (int64_t)pc_rect_width(rs[i]) * (int64_t)pc_rect_height(rs[i]);
    }
    return a;
}

static void test_auto_slices_tile_the_rest_of_the_canvas(void)
{
    pc_rect canvas = pc_rect_make(0, 0, 100, 80);
    pc_rect user[1] = {pc_rect_make(20, 10, 60, 30)};
    pc_rect *a = NULL;
    size_t n = 0;
    size_t i;
    size_t j;
    CHECK(pc_auto_slices(canvas, user, 1, &a, &n, NULL) == PC_OK);
    CHECK(area(a, n) + area(user, 1) == 100 * 80);
    for (i = 0; i < n; i++) {
        CHECK(pc_rect_is_empty(pc_rect_intersect(a[i], user[0])));
        for (j = i + 1; j < n; j++) {
            CHECK(pc_rect_is_empty(pc_rect_intersect(a[i], a[j])));
        }
    }
    CHECK_EQ_UINT(n, 4);
    CHECK(pc_rect_eq(a[0], pc_rect_make(0, 0, 100, 10)));
    CHECK(pc_rect_eq(a[1], pc_rect_make(0, 10, 20, 30)));
    CHECK(pc_rect_eq(a[2], pc_rect_make(60, 10, 100, 30)));
    CHECK(pc_rect_eq(a[3], pc_rect_make(0, 30, 100, 80)));
    free(a);
    CHECK(pc_auto_slices(canvas, NULL, 0, &a, &n, NULL) == PC_OK);
    CHECK(n == 1 && pc_rect_eq(a[0], canvas));
    free(a);
    CHECK(pc_auto_slices(PC_RECT_EMPTY, user, 1, &a, &n, NULL) == PC_OK && a == NULL && n == 0);
}

static void test_auto_slices_merge_and_edges(void)
{
    /* A full-height middle slice and a small one on the right: the left column's runs merge down
     * across the bands the right slice cuts. Slices outside the canvas, or empty, are ignored. */
    pc_rect canvas = pc_rect_make(0, 0, 30, 20);
    pc_rect taken[4] = {pc_rect_make(10, 0, 20, 20), pc_rect_make(25, 5, 30, 10), pc_rect_make(100, 100, 200, 200),
                        pc_rect_make(5, 5, 5, 9)};
    pc_rect *a = NULL;
    size_t n = 0;
    CHECK(pc_auto_slices(canvas, taken, 4, &a, &n, NULL) == PC_OK);
    CHECK(area(a, n) + 200 + 25 == 30 * 20);
    CHECK_EQ_UINT(n, 4);
    CHECK(pc_rect_eq(a[0], pc_rect_make(0, 0, 10, 20)));
    CHECK(pc_rect_eq(a[1], pc_rect_make(20, 0, 30, 5)));
    CHECK(pc_rect_eq(a[2], pc_rect_make(20, 5, 25, 10)));
    CHECK(pc_rect_eq(a[3], pc_rect_make(20, 10, 30, 20)));
    free(a);
}

static void test_slice_codes_and_ids(void)
{
    pc_slice_kind k;
    CHECK(pc_slice_origin_code(PC_SLICE_AUTO) == 0 && pc_slice_origin_code(PC_SLICE_USER) == 2);
    CHECK(pc_slice_origin_from_code(1) == PC_SLICE_LAYER && pc_slice_origin_from_code(77) == PC_SLICE_USER);
    CHECK_EQ_STR(pc_slice_origin_id(PC_SLICE_LAYER), "layer");
    CHECK(pc_slice_kind_code(PC_SLICE_NO_IMAGE) == 0 && pc_slice_kind_code(PC_SLICE_IMAGE) == 1);
    CHECK(pc_slice_kind_from_code(2) == PC_SLICE_TABLE && pc_slice_kind_from_code(9) == PC_SLICE_IMAGE);
    CHECK_EQ_STR(pc_slice_kind_id(PC_SLICE_NO_IMAGE), "noImage");
    CHECK(pc_slice_kind_from_id("img", 3, &k) && k == PC_SLICE_IMAGE);
    CHECK(pc_slice_kind_from_id("no-image", 8, &k) && k == PC_SLICE_NO_IMAGE);
    CHECK(pc_slice_kind_from_id("none", 4, &k) && k == PC_SLICE_NO_IMAGE);
    CHECK(pc_slice_kind_from_id("table", 5, &k) && k == PC_SLICE_TABLE);
    CHECK(!pc_slice_kind_from_id("Table", 5, &k));
    CHECK(!pc_slice_kind_from_id(NULL, 0, &k));
}

static void test_slices_store(void)
{
    pc_slices s;
    pc_slices c;
    uint32_t id = 0;
    memset(&s, 0, sizeof s);
    CHECK(pc_slices_is_empty(&s));
    CHECK(pc_slices_next_id(&s, &id) && id == 1);
    s.list = calloc(2, sizeof *s.list);
    pc_slice_default(&s.list[0]);
    pc_slice_default(&s.list[1]);
    CHECK(s.list[0].origin == PC_SLICE_USER && s.list[0].kind == PC_SLICE_IMAGE);
    s.list[0].id = 4;
    s.list[1].id = 2;
    s.list[1].alt = S("alt text");
    s.list[1].has_background = true;
    s.len = 2;
    s.group_name = S("site");
    CHECK(pc_slices_next_id(&s, &id) && id == 5);
    CHECK(pc_slices_get(&s, 2) == &s.list[1] && pc_slices_get(&s, 3) == NULL);
    CHECK(pc_slices_get_mut(&s, 4) == &s.list[0]);
    CHECK(pc_slices_clone(&s, &c, NULL) == PC_OK && pc_slices_eq(&s, &c));
    CHECK(c.list[1].alt.ptr != s.list[1].alt.ptr);
    c.list[1].background[2] = 9;
    CHECK(!pc_slices_eq(&s, &c));
    c.list[1].background[2] = 0;
    c.list[0].layer = 12; /* ignored while None */
    CHECK(pc_slices_eq(&s, &c));
    c.list[0].has_layer = true;
    CHECK(!pc_slices_eq(&s, &c));
    pc_slices_fini(&c);
    /* exhausted_slice_id: the u32 id space is used up. */
    s.list[0].id = UINT32_MAX;
    id = 7;
    CHECK(!pc_slices_next_id(&s, &id) && id == 7);
    pc_slices_fini(&s);
    pc_slices_fini(&s);
}

static void check_base(const char *name, const char *want)
{
    pc_document d;
    pc_str out = PC_STR_INIT;
    memset(&d, 0, sizeof d);
    d.name = S(name);
    CHECK(pc_slices_base_name(&d, &out, NULL) == PC_OK);
    CHECK_EQ_STR(pc_str_c(&out), want);
    pc_str_fini(&out);
    pc_str_fini(&d.name);
}

static void test_slice_base_name(void)
{
    /* Expected values printed by the reference's base_name(). */
    check_base("site.psd", "site");
    check_base("my file.v2.psd", "my_file_v2");
    check_base(".psd", "Untitled");
    check_base("", "Untitled");
    check_base("noext", "noext");
    check_base("\xc3\x9cn\xc3\xaf" "c\xc3\xb6" "d\xc3\xa9 \xe5\x90\x8d\xe5\x89\x8d.png",
               "\xc3\x9cn\xc3\xaf" "c\xc3\xb6" "d\xc3\xa9_\xe5\x90\x8d\xe5\x89\x8d");
    check_base("a-b_c!d", "a-b_c_d");
    check_base("\xe6\x97\xa5\xe6\x9c\xac.", "\xe6\x97\xa5\xe6\x9c\xac");
    check_base("x.y.z", "x_y");
    check_base("\xd9\xa1\xd9\xa2\xd9\xa3.psd", "\xd9\xa1\xd9\xa2\xd9\xa3"); /* Arabic-Indic digits */
    check_base("e\xcc\x81x", "e_x");                                       /* combining mark is not alnum */
    check_base("bad\xff.psd", "bad_");                                      /* invalid UTF-8: no crash */
}

/* ---- variables.rs ---- */

static void test_variables_lookups_and_clone(void)
{
    pc_variables v;
    pc_variables c;
    const pc_data_value *dv;
    size_t idx = 9;
    memset(&v, 0, sizeof v);
    v.defs = calloc(3, sizeof *v.defs);
    v.defs[0].name = S("show");
    v.defs[0].layer = 1;
    v.defs[0].kind.type = PC_VAR_VISIBILITY;
    v.defs[1].name = S("title");
    v.defs[1].layer = 2;
    v.defs[1].kind.type = PC_VAR_TEXT_REPLACEMENT;
    v.defs[2].name = S("photo");
    v.defs[2].layer = 3;
    v.defs[2].kind = (pc_var_kind){PC_VAR_PIXEL_REPLACEMENT, PC_PIXEL_FILL, PC_ALIGN_TOP_LEFT, true};
    v.ndefs = 3;
    v.data_sets = calloc(1, sizeof *v.data_sets);
    v.data_sets[0].name = S("row1");
    v.data_sets[0].values = calloc(3, sizeof *v.data_sets[0].values);
    v.data_sets[0].values[0].variable = S("show");
    v.data_sets[0].values[0].kind = PC_DATA_VISIBILITY;
    v.data_sets[0].values[1].variable = S("title");
    v.data_sets[0].values[1].kind = PC_DATA_TEXT;
    v.data_sets[0].values[1].text = S("Hello");
    v.data_sets[0].values[2].variable = S("photo");
    v.data_sets[0].values[2].kind = PC_DATA_PIXELS;
    v.data_sets[0].values[2].text = S("/tmp/a.png");
    v.data_sets[0].nvalues = 3;
    v.ndata_sets = 1;
    v.has_active = true;
    v.active = 0;

    CHECK(!pc_variables_is_empty(&v));
    CHECK(pc_variables_def(&v, "title", 5)->layer == 2);
    CHECK(pc_variables_def(&v, "missing", 7) == NULL);
    CHECK(pc_variables_def(&v, "titl", 4) == NULL);
    CHECK(pc_variables_data_set_index(&v, "row1", 4, &idx) && idx == 0);
    CHECK(!pc_variables_data_set_index(&v, "row2", 4, &idx) && idx == 0);
    CHECK(pc_variables_data_set(&v, "row1", 4) == &v.data_sets[0]);
    dv = pc_data_set_get(pc_variables_data_set(&v, "row1", 4), "title", 5);
    CHECK(dv != NULL && dv->kind == PC_DATA_TEXT && pc_str_eq_c(&dv->text, "Hello"));
    CHECK(pc_data_set_get(&v.data_sets[0], "nope", 4) == NULL);

    CHECK(pc_variables_clone(&v, &c, NULL) == PC_OK && pc_variables_eq(&v, &c));
    c.defs[1].kind.clip = true; /* not a field of TextReplacement */
    CHECK(pc_variables_eq(&v, &c));
    c.defs[2].kind.clip = false;
    CHECK(!pc_variables_eq(&v, &c));
    c.defs[2].kind.clip = true;
    c.data_sets[0].values[0].text = S("ignored for Visibility");
    CHECK(pc_variables_eq(&v, &c));
    c.data_sets[0].values[0].visibility = true;
    CHECK(!pc_variables_eq(&v, &c));
    c.data_sets[0].values[0].visibility = false;
    c.active = 3;
    CHECK(!pc_variables_eq(&v, &c));
    c.has_active = v.has_active = false;
    CHECK(pc_variables_eq(&v, &c));
    pc_variables_fini(&c);
    pc_variables_fini(&v);
    pc_variables_fini(&v);
    CHECK(pc_variables_is_empty(&v));
}

/* ---- never crash: zeroed values ---- */

static void test_zeroed_values_are_safe(void)
{
    pc_adjustment a;
    pc_effect e;
    pc_fill f;
    pc_shape_layer s;
    pc_named_path np;
    pc_duotone d;
    pc_measurement m;
    pc_variables v;
    pc_slices sl;
    pc_slice one;
    pc_note n;
    pc_blend_if b;
    pc_path p;
    size_t k;
    memset(&a, 0, sizeof a);
    for (k = 0; k < PC_ADJ_KIND_COUNT; k++) {
        pc_adjustment c;
        a.kind = (pc_adjustment_kind)k;
        CHECK(pc_adjustment_clone(&a, &c, NULL) == PC_OK);
        CHECK(pc_adjustment_eq(&a, &c));
        CHECK(pc_adjustment_label(&c)[0] != '\0');
        pc_adjustment_fini(&c);
        pc_adjustment_fini(&a);
    }
    memset(&e, 0, sizeof e);
    for (k = 0; k < PC_FX_KIND_COUNT; k++) {
        pc_effect c;
        e.kind = (pc_effect_kind)k;
        CHECK(pc_effect_clone(&e, &c, NULL) == PC_OK);
        CHECK(pc_effect_eq(&e, &c));
        (void)pc_effect_enabled(&c);
        pc_effect_fini(&c);
        pc_effect_fini(&e);
    }
    memset(&f, 0, sizeof f);
    for (k = 0; k < 3; k++) {
        pc_fill c;
        f.kind = (pc_fill_kind)k;
        CHECK(pc_fill_clone(&f, &c, NULL) == PC_OK && pc_fill_eq(&f, &c));
        pc_fill_fini(&c);
        pc_fill_fini(&f);
    }
    memset(&s, 0, sizeof s);
    pc_shape_layer_fini(&s);
    memset(&np, 0, sizeof np);
    pc_named_path_fini(&np);
    memset(&d, 0, sizeof d);
    {
        float rgb[3];
        pc_duotone_render(&d, NAN, rgb); /* no inks: paper white */
        CHECK(rgb[0] == 1.0f && rgb[2] == 1.0f);
    }
    pc_duotone_fini(&d);
    memset(&m, 0, sizeof m);
    CHECK(!pc_measurement_is_empty(&m)); /* zero scale is not the default 1 = 1 pixels */
    pc_measurement_fini(&m);
    memset(&v, 0, sizeof v);
    pc_variables_fini(&v);
    memset(&sl, 0, sizeof sl);
    pc_slices_fini(&sl);
    memset(&one, 0, sizeof one);
    pc_slice_fini(&one);
    memset(&n, 0, sizeof n);
    pc_note_fini(&n);
    memset(&b, 0, sizeof b);
    pc_blend_if_fini(&b);
    memset(&p, 0, sizeof p);
    pc_path_fini(&p);
    pc_resolved_slices_free(NULL, 3);
    pc_adjustment_fini(NULL);
    pc_effect_fini(NULL);
    pc_shape_layer_fini(NULL);
    pc_variables_fini(NULL);
    pc_slices_fini(NULL);
    pc_measurement_fini(NULL);
}

int main(int argc, char **argv)
{
    RUN(test_hue_range_weights);
    RUN(test_hue_range_exact);
    RUN(test_canonical_bounds);
    RUN(test_lut3d_len_rejects_tiny_and_overflowing_sizes);
    RUN(test_curves);
    RUN(test_adjustment_basics);
    RUN(test_adjustment_lut_and_raw);
    RUN(test_adjustment_eq_by_kind);
    RUN(test_effects_defaults_and_labels);
    RUN(test_contour_and_paint);
    RUN(test_fill);
    RUN(test_color_table_nearest);
    RUN(test_duotone_render_black_ink_is_gray);
    RUN(test_duotone_exact);
    RUN(test_stack_mode_ids_roundtrip);
    RUN(test_full_range_shows_everything);
    RUN(test_unsplit_points_are_hard_thresholds);
    RUN(test_split_points_ramp_linearly);
    RUN(test_bytes_round_trip_in_psd_order);
    RUN(test_set_trims_trailing_full_entries);
    RUN(test_blend_if_hostile_indices);
    RUN(test_advanced_defaults_match_photoshop);
    RUN(test_knockout_psd_values_and_names);
    RUN(test_segments_open_and_closed);
    RUN(test_first_op_is_combine_and_bounds);
    RUN(test_path_components_and_bounds_nan);
    RUN(test_vector_types);
    RUN(test_live_shape_and_shape_layer);
    RUN(test_scale_factor_and_text);
    RUN(test_describe_formatting);
    RUN(test_measurement);
    RUN(test_auto_slices_tile_the_rest_of_the_canvas);
    RUN(test_auto_slices_merge_and_edges);
    RUN(test_slice_codes_and_ids);
    RUN(test_slices_store);
    RUN(test_slice_base_name);
    RUN(test_variables_lookups_and_clone);
    RUN(test_zeroed_values_are_safe);
    return pc_t_finish();
}
