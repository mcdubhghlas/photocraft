/* Brush-engine tests, part 3: the tests brush_tests.rs gained after efa62efa (pixel-grid Pencil,
 * Texture levels and modes as measured against Photoshop, stroke-level Wet Edges, time-based
 * smoothing, the live union of symmetry passes, the (1 - t^2)^4 tip, small-tip area coverage,
 * 8-bit dithering) and render_reference.rs (the computed-tip fast path against the general
 * rasteriser, bit for bit). */
#include <math.h>
#include <stdio.h>

#include "paint/render_priv.h"
#include "support.h"

/* ---------- Pencil ---------- */

static void test_pencil_1px_line_is_a_clean_8_connected_staircase(void)
{
    static const double cases[5][4] = {{2.5, 2.5, 22.5, 9.5},
                                       {3.5, 30.5, 10.5, 5.5},
                                       {1.2, 1.7, 25.8, 4.1},
                                       {5.5, 5.5, 17.5, 17.5},
                                       {20.3, 3.6, 4.9, 12.2}};
    pc_paint_brush b = tb_brush();
    b.aliased = true;
    b.hardness = 1.0f;
    b.size = 1.0f;
    for (int c = 0; c < 5; c++) {
        pc_paint_point pts[2] = {pc_paint_point_make(cases[c][0], cases[c][1], 1.0f),
                                 pc_paint_point_make(cases[c][2], cases[c][3], 1.0f)};
        double ax = floor(cases[c][0]), ay = floor(cases[c][1]), ex = floor(cases[c][2]),
               ey = floor(cases[c][3]);
        bool steep = fabs(ey - ay) > fabs(ex - ax);
        double m0 = steep ? ay : ax, n0 = steep ? ax : ay, m1 = steep ? ey : ex, n1 = steep ? ex : ey;
        bool column[64] = {false};
        size_t painted = 0, distinct = 0;
        pc_surface s;
        tb_paint(&s, &b, pts, 2);
        for (int32_t y = 0; y < 40; y++) {
            for (int32_t x = 0; x < 40; x++) {
                double m, n, ideal;
                if (tb_alpha(&s, x, y) <= 0.0f) {
                    continue;
                }
                painted++;
                m = steep ? y : x;
                n = steep ? x : y;
                ideal = n0 + (n1 - n0) * (m - m0) / (m1 - m0);
                CHECK(fabs(n - ideal) <= 0.5 + 1e-9);
                if (!column[(int)m]) {
                    column[(int)m] = true;
                    distinct++;
                }
            }
        }
        CHECK_EQ_UINT(painted, (uint64_t)(fabs(m1 - m0) + 1.0));
        CHECK_EQ_UINT(distinct, painted); /* no column holds two pixels */
        pc_surface_fini(&s);
    }
    pc_paint_brush_fini(&b);
}

/* ---------- Texture ---------- */

static float level_of(float brightness, float contrast, bool invert, float v)
{
    pc_paint_texture t;
    pc_paint_texture_default(&t);
    t.brightness = brightness / 150.0f;
    t.contrast = contrast < 0.0f ? contrast / 50.0f : contrast / 100.0f;
    t.invert = invert;
    return pc_paint_texture_level(&t, v / 255.0f) * 255.0f;
}

static void test_texture_brightness_and_contrast_match_photoshop(void)
{
    static const float cases[16][5] = {
        {87, 0, 0, 8, 95},      {87, 0, 0, 200, 255},    {-87, 0, 0, 104, 17},  {150, 0, 0, 8, 158},
        {0, 50, 0, 104, 81},    {0, 50, 0, 136, 145},    {0, -50, 0, 8, 67},    {0, -50, 0, 248, 188},
        {0, 100, 0, 120, 0},    {0, 100, 0, 136, 255},   {40, 50, 0, 104, 161}, {40, 50, 0, 40, 33},
        {-40, -30, 0, 120, 82}, {-40, -30, 0, 248, 172}, {40, 0, 1, 8, 207},    {0, 50, 1, 72, 238},
    };
    for (int i = 0; i < 16; i++) {
        float got = level_of(cases[i][0], cases[i][1], cases[i][2] != 0.0f, cases[i][3]);
        CHECK_NEAR(got, cases[i][4], 2.0);
    }
}

static void test_texture_modes_match_photoshop(void)
{
    static const struct {
        pc_paint_mask_mode m;
        float d, v, t, want;
    } cases[] = {
        {PC_PAINT_MASK_HEIGHT, 0.0f, 255, 8, 0},
        {PC_PAINT_MASK_HEIGHT, 0.01f, 255, 8, 28},
        {PC_PAINT_MASK_HEIGHT, 0.05f, 255, 8, 148},
        {PC_PAINT_MASK_HEIGHT, 0.05f, 255, 136, 20},
        {PC_PAINT_MASK_HEIGHT, 0.13f, 255, 8, 255},
        {PC_PAINT_MASK_HEIGHT, 0.13f, 255, 152, 244},
        {PC_PAINT_MASK_HEIGHT, 0.13f, 255, 248, 148},
        {PC_PAINT_MASK_HEIGHT, 0.13f, 186, 88, 201},
        {PC_PAINT_MASK_HEIGHT, 0.13f, 142, 104, 116},
        {PC_PAINT_MASK_HEIGHT, 0.25f, 255, 248, 255},
        {PC_PAINT_MASK_LINEAR_HEIGHT, 0.01f, 255, 152, 15},
        {PC_PAINT_MASK_LINEAR_HEIGHT, 0.05f, 255, 8, 151},
        {PC_PAINT_MASK_LINEAR_HEIGHT, 0.05f, 255, 152, 63},
        {PC_PAINT_MASK_LINEAR_HEIGHT, 0.10f, 255, 72, 240},
        {PC_PAINT_MASK_LINEAR_HEIGHT, 0.13f, 101, 120, 82},
        {PC_PAINT_MASK_LINEAR_HEIGHT, 0.13f, 142, 104, 130},
        {PC_PAINT_MASK_SUBTRACT, 1.0f, 255, 8, 247},
        {PC_PAINT_MASK_SUBTRACT, 1.0f, 255, 248, 7},
        {PC_PAINT_MASK_SUBTRACT, 0.5f, 224, 72, 188},
        {PC_PAINT_MASK_COLOR_BURN, 0.5f, 224, 72, 207},
        {PC_PAINT_MASK_COLOR_DODGE, 0.5f, 99, 120, 130},
        {PC_PAINT_MASK_MULTIPLY, 0.13f, 255, 8, 223},
        {PC_PAINT_MASK_DARKEN, 0.5f, 255, 8, 131},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        float got =
            pc_paint_texture_combine(cases[i].m, cases[i].v / 255.0f, cases[i].t / 255.0f, cases[i].d) *
            255.0f;
        CHECK_NEAR(got, cases[i].want, 3.0);
    }
}

static float checker_fn(void *ctx, uint32_t x, uint32_t y)
{
    (void)ctx;
    return (x + y) % 2 == 0 ? 0.45f : 0.6f;
}

static void test_low_depth_height_texture_paints(void)
{
    pc_paint_gray_tile tile = PC_PAINT_GRAY_TILE_INIT;
    pc_paint_point pts[2];
    CHECK(pc_paint_gray_tile_from_fn(8, 8, checker_fn, NULL, &tile, NULL) == PC_OK);
    tb_line(20.0, 80.0, 30.0, pts);
    for (int k = 0; k < 2; k++) {
        pc_paint_brush b = tb_brush();
        pc_surface s;
        b.size = 40.0f;
        b.texture.enabled = true;
        b.texture.pattern.kind = PC_PAINT_PATTERN_TILE;
        pc_paint_gray_tile_copy(&b.texture.pattern.tile, &tile);
        b.texture.mode = k == 0 ? PC_PAINT_MASK_HEIGHT : PC_PAINT_MASK_LINEAR_HEIGHT;
        b.texture.depth = 0.13f;
        b.texture.brightness = -87.0f / 150.0f;
        tb_paint(&s, &b, pts, 2);
        CHECK(tb_alpha(&s, 50, 30) > 0.95f);
        pc_surface_fini(&s);
        pc_paint_brush_fini(&b);
    }
    pc_paint_gray_tile_fini(&tile);
}

/* ---------- Wet Edges ---------- */

static void test_wet_edges_shape_the_stroke_not_each_dab(void)
{
    pc_paint_brush b = tb_brush();
    pc_paint_point pts[2];
    pc_surface s;
    float lo = 1e30f, hi = -1e30f;
    b.size = 40.0f;
    b.hardness = 0.8f;
    b.wet_edges = true;
    tb_line(20.0, 220.0, 50.0, pts);
    tb_paint(&s, &b, pts, 2);
    for (int32_t x = 60; x < 180; x++) {
        float a = tb_alpha(&s, x, 50);
        lo = fminf(lo, a);
        hi = fmaxf(hi, a);
    }
    CHECK(hi - lo < 0.02f); /* the centre line is even */
    CHECK(hi < 0.6f);       /* the wet interior is lighter */
    for (int32_t x = 80; x <= 160; x += 40) {
        CHECK(tb_alpha(&s, x, 66) > hi + 0.15f);
    }
    pc_surface_fini(&s);
    pc_paint_brush_fini(&b);
}

/* ---------- smoothing ---------- */

static void test_smoothing_string_runs_from_the_lagging_brush_to_the_pointer(void)
{
    pc_paint_smoothing cfg;
    pc_paint_smoother s, off;
    pc_paint_point_vec out = {0};
    double br[2], pt[2];
    pc_paint_smoothing_default(&cfg);
    cfg.amount = 0.8f;
    pc_paint_smoother_init(&s, &cfg, 1.0f);
    CHECK(!pc_paint_smoother_string(&s, br, pt)); /* no stroke yet */
    CHECK(pc_paint_smoother_push(&s, pc_paint_point_make(0.0, 0.0, 1.0f), &out, NULL) == PC_OK);
    CHECK(!pc_paint_smoother_string(&s, br, pt)); /* the first point is the brush */
    CHECK(pc_paint_smoother_push(&s, pc_paint_point_make(100.0, 0.0, 1.0f), &out, NULL) == PC_OK);
    CHECK(pc_paint_smoother_string(&s, br, pt));
    CHECK(br[0] > 0.0 && br[0] < 100.0);
    CHECK(pt[0] == 100.0 && pt[1] == 0.0);
    cfg.amount = 0.0f;
    pc_paint_smoother_init(&off, &cfg, 1.0f);
    CHECK(pc_paint_smoother_push(&off, pc_paint_point_make(0.0, 0.0, 1.0f), &out, NULL) == PC_OK);
    CHECK(pc_paint_smoother_push(&off, pc_paint_point_make(100.0, 0.0, 1.0f), &out, NULL) == PC_OK);
    CHECK(!pc_paint_smoother_string(&off, br, pt)); /* smoothing off: no string */
    pc_paint_point_vec_fini(&out);
}

static double held_jump(const pc_paint_smoothing *cfg, double interval)
{
    pc_paint_smoother s;
    pc_paint_point_vec out = {0};
    double t = 0.0, x;
    pc_paint_smoother_init(&s, cfg, 1.0f);
    CHECK(pc_paint_smoother_push(&s, pc_paint_point_make(0.0, 0.0, 1.0f), &out, NULL) == PC_OK);
    while (t < 32.0) {
        pc_paint_point p = pc_paint_point_make(100.0, 0.0, 1.0f);
        t += interval;
        p.time = t;
        CHECK(pc_paint_smoother_push(&s, p, &out, NULL) == PC_OK);
    }
    x = out.len > 0 ? out.v[out.len - 1].x : 0.0;
    pc_paint_point_vec_fini(&out);
    return x;
}

static void test_smoothing_pulls_by_time_not_by_how_often_the_device_reports(void)
{
    pc_paint_smoothing cfg;
    pc_paint_smoother s;
    pc_paint_point_vec out = {0};
    double fast, slow;
    pc_paint_smoothing_default(&cfg);
    cfg.amount = 0.8f;
    cfg.catch_up = true;
    cfg.catch_up_on_end = false;
    fast = held_jump(&cfg, 1.0);
    slow = held_jump(&cfg, 16.0);
    /* Two 16 ms steps of a 0.2 pull: 1 - 0.8^2 = 36 % of the way. */
    CHECK_NEAR(slow, 36.0, 1e-4);
    CHECK_NEAR(fast, slow, 1e-4);
    /* Without timestamps each point is one step. */
    pc_paint_smoother_init(&s, &cfg, 1.0f);
    CHECK(pc_paint_smoother_push(&s, pc_paint_point_make(0.0, 0.0, 1.0f), &out, NULL) == PC_OK);
    CHECK(pc_paint_smoother_push(&s, pc_paint_point_make(100.0, 0.0, 1.0f), &out, NULL) == PC_OK);
    CHECK_NEAR(out.v[out.len - 1].x, 20.0, 1e-4);
    pc_paint_point_vec_fini(&out);
}

static void test_smoothing_without_catch_up_stops_while_the_pointer_pauses(void)
{
    pc_paint_smoothing cfg;
    pc_paint_smoother s;
    pc_paint_point_vec out = {0};
    pc_paint_point p = pc_paint_point_make(100.0, 0.0, 1.0f);
    size_t n;
    pc_paint_smoothing_default(&cfg);
    cfg.amount = 0.8f;
    cfg.catch_up = false;
    cfg.catch_up_on_end = false;
    pc_paint_smoother_init(&s, &cfg, 1.0f);
    CHECK(pc_paint_smoother_push(&s, pc_paint_point_make(0.0, 0.0, 1.0f), &out, NULL) == PC_OK);
    p.time = 16.0;
    CHECK(pc_paint_smoother_push(&s, p, &out, NULL) == PC_OK);
    n = out.len;
    for (double t = 32.0; t <= 64.0; t += 16.0) {
        p.time = t;
        CHECK(pc_paint_smoother_push(&s, p, &out, NULL) == PC_OK);
    }
    CHECK_EQ_UINT(out.len, n); /* held still: the brush doesn't move */
    pc_paint_point_vec_fini(&out);
}

/* ---------- live union of symmetry passes ---------- */

#define NPTS 40

static bool same_region(const pc_surface *a, const pc_surface *b, pc_rect area)
{
    float *va = NULL, *vb = NULL;
    size_t na = 0, nb = 0;
    bool same;
    CHECK(pc_surface_read_region(a, area, &va, &na, NULL) == PC_OK);
    CHECK(pc_surface_read_region(b, area, &vb, &nb, NULL) == PC_OK);
    same = na == nb && va != NULL && vb != NULL;
    for (size_t i = 0; same && i < na; i++) {
        same = va[i] == vb[i];
    }
    free(va);
    free(vb);
    return same;
}

static void live_case(const pc_paint_brush *b, pc_paint_point paths[4][NPTS], const pc_surface *pre,
                      pc_pixel_format fmt, bool four, const pc_surface *sel)
{
    pc_paint_renderer *r[4] = {NULL, NULL, NULL, NULL};
    pc_paint_union_live live = {0};
    pc_surface target;
    pc_rect area = pc_rect_make(-20, -20, 240, 190);
    size_t at = 0, chunk = 1, merged = 0;
    CHECK(pc_surface_clone(pre, &target, NULL) == PC_OK);
    for (int i = 0; i < 4; i++) {
        CHECK(pc_paint_renderer_new(b, &fmt, 1.0f, &r[i], NULL) == PC_OK);
    }
    while (at < NPTS) {
        size_t end = at + chunk < NPTS ? at + chunk : NPTS, count = four && at >= 12 ? 4 : 2;
        bool full = merged != count;
        pc_paint_renderer *fin[4] = {NULL, NULL, NULL, NULL};
        pc_surface expected;
        pc_rect dmg, bounds = PC_RECT_EMPTY;
        for (int i = 0; i < 4; i++) {
            CHECK(pc_paint_renderer_push(r[i], &paths[i][at], end - at, NULL) == PC_OK);
        }
        merged = count;
        CHECK(pc_paint_renderer_composite_union_live(r, count, &live, full, pre, &target, sel, false, &dmg,
                                                     NULL) == PC_OK);
        for (size_t i = 0; i < count; i++) {
            CHECK(pc_paint_renderer_clone(r[i], &fin[i], NULL) == PC_OK);
            CHECK(pc_paint_renderer_finish(fin[i], NULL) == PC_OK);
            bounds = pc_rect_union(bounds, pc_paint_renderer_bounds(fin[i]));
        }
        CHECK(pc_surface_clone(pre, &expected, NULL) == PC_OK);
        CHECK(pc_paint_renderer_composite_union_many(fin[0], (const pc_paint_renderer *const *)&fin[1],
                                                     count - 1, pre, &expected, sel, false, &dmg,
                                                     NULL) == PC_OK);
        if (!same_region(&target, &expected, area)) {
            PC_T_FAIL("live union differs: four %d, selection %d, points ..%zu", four, sel != NULL, end);
        }
        CHECK(pc_rect_eq(pc_paint_union_live_bounds(&live), bounds));
        pc_surface_fini(&expected);
        for (size_t i = 0; i < count; i++) {
            pc_paint_renderer_free(fin[i]);
        }
        at = end;
        chunk = chunk % 3 + 1;
    }
    for (int i = 0; i < 4; i++) {
        pc_paint_renderer_free(r[i]);
    }
    pc_paint_union_live_fini(&live);
    pc_surface_fini(&target);
}

static void test_live_union_of_passes_matches_merging_the_finished_passes_after_every_chunk(void)
{
    static const pc_sample_type samples[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    pc_paint_brush br[8];
    pc_paint_point paths[4][NPTS];
    pc_surface sel;
    pc_rect sel_rect = pc_rect_make(0, 0, 160, 120), fill = pc_rect_make(10, 10, 190, 140);
    float *sv = malloc(160 * 120 * sizeof *sv);
    CHECK(sv != NULL);
    br[0] = tb_brush();
    br[0].size = 14.0f;
    br[0].hardness = 0.6f;
    br[0].opacity = 0.6f;
    for (int i = 1; i < 8; i++) {
        pc_paint_brush_copy(&br[i], &br[0]);
    }
    br[1].smoothing.amount = 0.7f;
    br[2].dual_brush.enabled = true;
    br[2].dual_brush.size = 6.0f;
    br[2].dual_brush.scatter = 1.5f;
    br[3].color_dynamics.enabled = true;
    br[3].color_dynamics.hue_jitter = 0.5f;
    br[4].wet_edges = true;
    br[5].mode = PC_BLEND_DISSOLVE;
    br[6].erase = true;
    br[7].texture.enabled = true;
    br[7].texture.each_tip = false;
    for (int i = 0; i < NPTS; i++) {
        double x = 30.0 + (double)i * 3.7, y = 40.0 + 25.0 * sin((double)i / 5.0);
        /* Mirrors about x = 100 and y = 75; the first point sits near the vertical axis. */
        paths[0][i] = pc_paint_point_make(x, y, 1.0f);
        paths[1][i] = pc_paint_point_make(200.0 - x, y, 1.0f);
        paths[2][i] = pc_paint_point_make(x, 150.0 - y, 1.0f);
        paths[3][i] = pc_paint_point_make(200.0 - x, 150.0 - y, 1.0f);
    }
    pc_surface_init(&sel, (pc_pixel_format){PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_F32, false});
    for (int i = 0; sv != NULL && i < 160 * 120; i++) {
        sv[i] = fminf((float)(i % 160) / 120.0f, 1.0f);
    }
    CHECK(sv != NULL && pc_surface_write_region(&sel, sel_rect, sv, 160 * 120, NULL) == PC_OK);
    for (int si = 0; si < 3; si++) {
        pc_pixel_format fmt = {PC_COLOR_MODE_RGB, samples[si], true};
        size_t npx = (size_t)pc_rect_width(fill) * pc_rect_height(fill);
        float *px = malloc(npx * 4 * sizeof *px);
        pc_surface pre;
        CHECK(px != NULL);
        for (size_t i = 0; px != NULL && i < npx; i++) {
            px[4 * i] = (float)(i % 7) / 7.0f;
            px[4 * i + 1] = 0.5f;
            px[4 * i + 2] = (float)(i % 13) / 13.0f;
            px[4 * i + 3] = 0.8f;
        }
        pc_surface_init(&pre, fmt);
        CHECK(px != NULL && pc_surface_write_region(&pre, fill, px, npx * 4, NULL) == PC_OK);
        for (int bi = 0; bi < 8; bi++) {
            live_case(&br[bi], paths, &pre, fmt, false, NULL);
            live_case(&br[bi], paths, &pre, fmt, true, NULL);
            live_case(&br[bi], paths, &pre, fmt, true, &sel);
        }
        pc_surface_fini(&pre);
        free(px);
    }
    for (int i = 0; i < 8; i++) {
        pc_paint_brush_fini(&br[i]);
    }
    pc_surface_fini(&sel);
    free(sv);
}

/* ---------- the soft tip ---------- */

static void cross_section(const pc_paint_brush *b, const pc_paint_point *pts, size_t n, int32_t x,
                          float out[70])
{
    pc_surface s;
    tb_paint(&s, b, pts, n);
    for (int32_t y = 100; y < 170; y++) {
        out[y - 100] = tb_alpha(&s, x, y);
    }
    pc_surface_fini(&s);
}

static size_t soft_edge_width(const float p[70])
{
    size_t below10 = 70, below90 = 70;
    for (size_t i = 0; i < 70; i++) {
        if (p[i] < 0.1f && below10 == 70) {
            below10 = i;
        }
        if (p[i] < 0.9f && below90 == 70) {
            below90 = i;
        }
    }
    return below10 > below90 ? below10 - below90 : 0;
}

static void test_a_soft_stroke_keeps_most_of_its_tips_softness(void)
{
    pc_paint_brush b;
    pc_paint_point pts[2], one = pc_paint_point_make(200.5, 100.5, 1.0f);
    float stroke[70], dab[70];
    size_t s, d;
    pc_paint_brush_default(&b);
    b.size = 100.0f;
    b.hardness = 0.0f;
    b.pressure_size = false;
    tb_line(0.0, 400.0, 100.5, pts);
    cross_section(&b, pts, 2, 200, stroke);
    cross_section(&b, &one, 1, 200, dab);
    s = soft_edge_width(stroke);
    d = soft_edge_width(dab);
    CHECK(d >= 20 && s * 100 >= d * 65);
}

static void test_a_tiny_dabs_coverage_does_not_depend_on_where_it_lands_in_a_pixel(void)
{
    static const double pos[4][2] = {{5.5, 5.5}, {5.75, 5.5}, {5.0, 5.0}, {5.3, 5.8}};
    pc_paint_brush b;
    float lo = 1e30f, hi = 0.0f;
    pc_paint_brush_default(&b);
    b.size = 1.0f;
    b.hardness = 1.0f;
    b.pressure_size = false;
    for (int i = 0; i < 4; i++) {
        pc_paint_point p = pc_paint_point_make(pos[i][0], pos[i][1], 1.0f);
        pc_surface s;
        float sum;
        tb_paint(&s, &b, &p, 1);
        sum = tb_alpha_sum(&s, pc_rect_make(0, 0, 10, 10));
        lo = fminf(lo, sum);
        hi = fmaxf(hi, sum);
        pc_surface_fini(&s);
    }
    CHECK(hi - lo < 0.03f * hi);
}

static void test_the_tip_falls_off_from_the_hard_core_to_zero_at_the_edge(void)
{
    float prev = 2.0f;
    CHECK(pc_paint_tip_falloff(0.0f, 10.0f, 0.0f) == 1.0f);
    CHECK(pc_paint_tip_falloff(10.0f, 10.0f, 0.0f) == 0.0f);
    CHECK(pc_paint_tip_falloff(4.0f, 10.0f, 0.5f) == 1.0f); /* inside the hard core */
    for (int i = 0; i <= 20; i++) {
        float v = pc_paint_tip_falloff((float)i * 0.5f, 10.0f, 0.0f);
        CHECK(v <= prev);
        prev = v;
    }
    CHECK_NEAR(pc_paint_tip_falloff(5.0f, 10.0f, 0.0f), pow(0.75, 4.0), 1e-6);
}

static void test_half_coverage_radius_is_the_tips_50_percent_contour(void)
{
    static const float hs[5] = {0.0f, 0.25f, 0.5f, 0.75f, 0.99f};
    for (int i = 0; i < 5; i++) {
        float d = pc_paint_half_coverage_radius(10.5f, hs[i]);
        CHECK_NEAR(pc_paint_tip_falloff(d, 10.5f, hs[i]), 0.5, 1e-3);
    }
    CHECK(pc_paint_half_coverage_radius(10.5f, 1.0f) == 10.5f);
    CHECK(isfinite(pc_paint_half_coverage_radius(10.5f, NAN)));
}

/* ---------- 8-bit dithering ---------- */

static void soft_low_flow(pc_pixel_format fmt, pc_surface *s)
{
    static const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    pc_paint_brush b;
    pc_paint_point pts[2];
    pc_rect d;
    pc_surface_init_default(s, fmt, white, 4);
    pc_paint_brush_default(&b);
    b.size = 120.0f;
    b.hardness = 0.0f;
    b.flow = 0.05f;
    b.opacity = 0.3f;
    b.color[0] = b.color[1] = b.color[2] = 0.0f;
    b.color[3] = 1.0f;
    b.pressure_size = false;
    tb_line(0.0, 300.0, 100.0, pts);
    CHECK(pc_paint_render_stroke(s, &b, pts, 2, NULL, false, 1.0f, &d, NULL) == PC_OK);
}

static void test_an_8bit_soft_stroke_is_dithered_instead_of_banded(void)
{
    pc_surface ref, eight;
    float worst = 0.0f;
    soft_low_flow(PC_PIXEL_FORMAT_RGBA32F, &ref);
    soft_low_flow(PC_PIXEL_FORMAT_RGBA8, &eight);
    for (int32_t y = 30; y < 170; y++) {
        float g = 0.0f, w = 0.0f;
        for (int32_t x = 100; x < 200; x++) {
            float a[4], b[4];
            pc_surface_rgba(&eight, x, y, a);
            pc_surface_rgba(&ref, x, y, b);
            g += a[0];
            w += b[0];
        }
        worst = fmaxf(worst, fabsf(g - w) / 100.0f * 255.0f);
    }
    CHECK(worst < 0.15f);
    pc_surface_fini(&ref);
    pc_surface_fini(&eight);
}

static int q255(float v)
{
    return (int)roundf(v * 255.0f);
}

static void test_dithering_never_moves_an_exact_colour_or_an_untouched_pixel(void)
{
    static const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    pc_surface s;
    pc_paint_brush b;
    pc_paint_point pts[2];
    pc_rect d;
    float far[4];
    pc_surface_init_default(&s, PC_PIXEL_FORMAT_RGBA8, white, 4);
    pc_paint_brush_default(&b);
    b.size = 20.0f;
    b.hardness = 1.0f;
    b.color[0] = 0.2f;
    b.color[1] = 0.4f;
    b.color[2] = 0.6f;
    b.color[3] = 1.0f;
    b.pressure_size = false;
    tb_line(10.0, 90.0, 50.0, pts);
    CHECK(pc_paint_render_stroke(&s, &b, pts, 2, NULL, false, 1.0f, &d, NULL) == PC_OK);
    for (int32_t x = 15; x < 85; x++) {
        float p[4];
        pc_surface_rgba(&s, x, 50, p);
        CHECK_EQ_INT(q255(p[0]), q255(0.2f));
        CHECK_EQ_INT(q255(p[1]), q255(0.4f));
        CHECK_EQ_INT(q255(p[2]), q255(0.6f));
    }
    pc_surface_rgba(&s, 50, 5, far);
    CHECK(far[0] == 1.0f && far[1] == 1.0f && far[2] == 1.0f && far[3] == 1.0f);
    pc_surface_fini(&s);
}

/* ---------- render_reference.rs: fast path == general rasteriser ---------- */

static uint32_t bits(float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    return u;
}

static void compare(const pc_paint_context *ctx, const pc_paint_dab *d, bool dual, pc_rect rect,
                    bool with_depth)
{
    pc_paint_fbuf a = {0}, e = {0}, ad = {0}, ed = {0};
    pc_status sa, se;
    /* Reused, dirty buffers also check clearing and resizing. */
    CHECK(pc_paint_fbuf_zeroed(&a, 19, NULL) == PC_OK && pc_paint_fbuf_zeroed(&e, 19, NULL) == PC_OK);
    CHECK(pc_paint_fbuf_zeroed(&ad, 23, NULL) == PC_OK && pc_paint_fbuf_zeroed(&ed, 23, NULL) == PC_OK);
    for (size_t i = 0; i < 19; i++) {
        a.v[i] = e.v[i] = 0.75f;
    }
    for (size_t i = 0; i < 23; i++) {
        ad.v[i] = ed.v[i] = 0.25f;
    }
    sa = pc_paint_rasterize_into(ctx, d, dual, rect, &a, with_depth ? &ad : NULL, true, NULL);
    se = pc_paint_rasterize_into(ctx, d, dual, rect, &e, with_depth ? &ed : NULL, false, NULL);
    CHECK_EQ_INT(sa, se);
    CHECK_EQ_UINT(a.len, e.len);
    for (size_t i = 0; sa == PC_OK && i < a.len && i < e.len; i++) {
        if (bits(a.v[i]) != bits(e.v[i])) {
            PC_T_FAIL("coverage mismatch at %zu: %08x vs %08x (radius %g, rect %d,%d,%d,%d)", i, bits(a.v[i]),
                      bits(e.v[i]), (double)d->radius, rect.x0, rect.y0, rect.x1, rect.y1);
            break;
        }
    }
    if (with_depth) {
        CHECK_EQ_UINT(ad.len, ed.len);
        for (size_t i = 0; i < ad.len && i < ed.len; i++) {
            if (bits(ad.v[i]) != bits(ed.v[i])) {
                PC_T_FAIL("depth mismatch at %zu", i);
                break;
            }
        }
    }
    pc_paint_fbuf_fini(&a);
    pc_paint_fbuf_fini(&e);
    pc_paint_fbuf_fini(&ad);
    pc_paint_fbuf_fini(&ed);
}

static void test_computed_tips_match_scalar_bits(void)
{
    static const float hard[7] = {-0.5f, 0.0f, 0.1f, 0.5f, 0.9999f, 1.0f, 1.5f};
    static const float radii[7] = {0.5f, 1.0f, 2.99f, 3.0f, 3.01f, 15.0f, 31.5f};
    static const float round_[4] = {0.0f, 0.1f, 0.4f, 1.0f};
    static const float alphas[5] = {-0.5f, -0.0f, 0.0f, 0.37f, 1.0f};
    for (int al = 0; al < 2; al++) {
        for (int h = 0; h < 7; h++) {
            pc_paint_brush b;
            pc_paint_context ctx;
            pc_paint_brush_default(&b);
            b.aliased = al == 1;
            b.hardness = hard[h];
            CHECK(pc_paint_context_init(&ctx, &b, NULL) == PC_OK);
            for (int r = 0; r < 7; r++) {
                for (int ro = 0; ro < 4; ro++) {
                    for (int a = 0; a < 5; a++) {
                        pc_paint_dab d = pc_paint_dab_round((pc_point){-31.25, 63.75}, radii[r], alphas[a]);
                        d.roundness = round_[ro];
                        d.flip_x = true;
                        d.flip_y = true;
                        compare(&ctx, &d, false, pc_paint_context_dab_rect(&ctx, &d, false), false);
                    }
                }
            }
            pc_paint_context_fini(&ctx);
        }
    }
}

static void test_large_corner_dabs_keep_off_canvas_coverage(void)
{
    static const struct {
        float hardness;
        bool aliased;
    } cases[3] = {{0.0f, false}, {1.0f, false}, {1.0f, true}};
    for (int c = 0; c < 3; c++) {
        pc_paint_brush b;
        pc_paint_context ctx;
        pc_paint_dab d = pc_paint_dab_round((pc_point){-1050.25, -1050.75}, 1500.0f, 0.73f);
        pc_paint_fbuf cov = {0};
        pc_rect rect;
        bool off = false;
        pc_paint_brush_default(&b);
        b.hardness = cases[c].hardness;
        b.aliased = cases[c].aliased;
        CHECK(pc_paint_context_init(&ctx, &b, NULL) == PC_OK);
        rect = pc_paint_context_dab_rect(&ctx, &d, false);
        compare(&ctx, &d, false, rect, false);
        CHECK(pc_paint_context_rasterize(&ctx, &d, false, rect, &cov, NULL) == PC_OK);
        /* The layer is an infinite plane: paint outside the visible document must survive. */
        for (size_t i = 0; i < cov.len && !off; i++) {
            off = cov.v[i] > 0.0f && rect.x0 + (int32_t)(i % pc_rect_width(rect)) < 0;
        }
        CHECK(off);
        compare(&ctx, &d, false, pc_rect_make(0, 0, 32, 32), false);
        compare(&ctx, &d, false, pc_rect_make(0, 0, 0, 0), false);
        pc_paint_fbuf_fini(&cov);
        pc_paint_context_fini(&ctx);
    }
}

static float ref_tip_fn(void *ctx, uint32_t x, uint32_t y)
{
    (void)ctx;
    return (float)((x + y) % 7) / 6.0f;
}

static void test_feature_and_exceptional_dabs_match_scalar_bits(void)
{
    static const float vals[4] = {NAN, 0.0f, INFINITY, -INFINITY};
    static const float views[3][2] = {{0.0f, 1.0f}, {0.7f, 1.0f}, {0.0f, 0.6f}};
    pc_paint_gray_tile tip = PC_PAINT_GRAY_TILE_INIT;
    pc_paint_brush b;
    pc_paint_context ctx;
    float weird;
    uint32_t wb = 0x7f800123u;
    memcpy(&weird, &wb, 4);
    CHECK(pc_paint_gray_tile_from_fn(8, 5, ref_tip_fn, NULL, &tip, NULL) == PC_OK);
    for (int feature = 0; feature < 7; feature++) {
        pc_paint_brush_default(&b);
        b.hardness = 0.25f;
        switch (feature) {
        case 0:
            b.noise = true;
            break;
        case 1:
            b.wet_edges = true;
            break;
        case 2:
            b.aliased = true;
            b.square = true;
            break;
        case 3:
            b.tip.kind = PC_PAINT_TIP_SAMPLED;
            pc_paint_gray_tile_copy(&b.tip.tile, &tip);
            break;
        case 4:
            b.texture.enabled = true;
            b.texture.each_tip = true;
            b.texture.pattern.kind = PC_PAINT_PATTERN_TILE;
            pc_paint_gray_tile_copy(&b.texture.pattern.tile, &tip);
            break;
        case 5:
            b.dual_brush.tip.kind = PC_PAINT_TIP_SAMPLED;
            pc_paint_gray_tile_copy(&b.dual_brush.tip.tile, &tip);
            break;
        default:
            break;
        }
        CHECK(pc_paint_context_init(&ctx, &b, NULL) == PC_OK);
        for (int dual = 0; dual < 2; dual++) {
            for (int wd = 0; wd < 2; wd++) {
                for (int v = 0; v < 3; v++) {
                    pc_paint_dab d = pc_paint_dab_round((pc_point){63.5, -0.25}, 15.0f, 0.47f);
                    d.angle = views[v][0];
                    d.roundness = 0.4f;
                    d.proj_scale = views[v][1];
                    compare(&ctx, &d, dual == 1, pc_paint_context_dab_rect(&ctx, &d, dual == 1), wd == 1);
                }
            }
        }
        pc_paint_context_fini(&ctx);
        pc_paint_brush_fini(&b);
    }
    for (int i = 0; i < 4; i++) {
        float value = i == 1 ? weird : vals[i];
        pc_paint_brush_default(&b);
        CHECK(pc_paint_context_init(&ctx, &b, NULL) == PC_OK);
        for (int field = 0; field < 4; field++) {
            pc_paint_dab d = pc_paint_dab_round((pc_point){0.0, 0.0}, 15.0f, 1.0f);
            if (field == 0) {
                d.alpha = value;
            } else if (field == 1) {
                d.angle = value;
            } else if (field == 2) {
                d.radius = value;
            } else {
                d.roundness = value;
            }
            compare(&ctx, &d, false, pc_rect_make(-4, -4, 4, 4), false);
        }
        pc_paint_context_fini(&ctx);
        b.hardness = value;
        CHECK(pc_paint_context_init(&ctx, &b, NULL) == PC_OK);
        {
            pc_paint_dab d = pc_paint_dab_round((pc_point){0.0, 0.0}, 15.0f, 1.0f);
            compare(&ctx, &d, false, pc_rect_make(-16, -16, 16, 16), false);
        }
        pc_paint_context_fini(&ctx);
        pc_paint_brush_fini(&b);
    }
    pc_paint_gray_tile_fini(&tip);
}

static void test_computed_tips_match_at_pixel_and_float_boundaries(void)
{
    static const double centers[4][2] = {
        {0.0, -0.0}, {0.5, 0.5}, {255.9999, -256.0001}, {16777216.25, -16777216.75}};
    static const float radii[4] = {3.0f, 3.5f, 15.25f, 113.75f};
    static const float hard[3] = {0.0f, 0.6f, 1.0f};
    for (int al = 0; al < 2; al++) {
        for (int c = 0; c < 4; c++) {
            for (int r = 0; r < 4; r++) {
                for (int h = 0; h < 3; h++) {
                    pc_paint_brush b;
                    pc_paint_context ctx;
                    pc_paint_dab d =
                        pc_paint_dab_round((pc_point){centers[c][0], centers[c][1]}, radii[r], 0.137f);
                    pc_paint_brush_default(&b);
                    b.hardness = hard[h];
                    b.aliased = al == 1;
                    CHECK(pc_paint_context_init(&ctx, &b, NULL) == PC_OK);
                    d.roundness = 0.375f;
                    compare(&ctx, &d, false, pc_paint_context_dab_rect(&ctx, &d, false), false);
                    pc_paint_context_fini(&ctx);
                }
            }
        }
    }
}

int main(int argc, char **argv)
{
    RUN(test_pencil_1px_line_is_a_clean_8_connected_staircase);
    RUN(test_texture_brightness_and_contrast_match_photoshop);
    RUN(test_texture_modes_match_photoshop);
    RUN(test_low_depth_height_texture_paints);
    RUN(test_wet_edges_shape_the_stroke_not_each_dab);
    RUN(test_smoothing_string_runs_from_the_lagging_brush_to_the_pointer);
    RUN(test_smoothing_pulls_by_time_not_by_how_often_the_device_reports);
    RUN(test_smoothing_without_catch_up_stops_while_the_pointer_pauses);
    RUN(test_live_union_of_passes_matches_merging_the_finished_passes_after_every_chunk);
    RUN(test_a_soft_stroke_keeps_most_of_its_tips_softness);
    RUN(test_a_tiny_dabs_coverage_does_not_depend_on_where_it_lands_in_a_pixel);
    RUN(test_the_tip_falls_off_from_the_hard_core_to_zero_at_the_edge);
    RUN(test_half_coverage_radius_is_the_tips_50_percent_contour);
    RUN(test_an_8bit_soft_stroke_is_dithered_instead_of_banded);
    RUN(test_dithering_never_moves_an_exact_colour_or_an_untouched_pixel);
    RUN(test_computed_tips_match_scalar_bits);
    RUN(test_large_corner_dabs_keep_off_canvas_coverage);
    RUN(test_feature_and_exceptional_dabs_match_scalar_bits);
    RUN(test_computed_tips_match_at_pixel_and_float_boundaries);
    return pc_t_finish();
}
