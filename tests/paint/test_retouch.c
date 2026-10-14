/* Chunk L3 unit tests: the #[cfg(test)] modules of crates/paint/src/{retouch,mixer,replace,
 * bg_erase}.rs, plus never-crash checks for the C entry points (bad lengths, hostile sizes). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "color/blend.h"
#include "harness.h"
#include "paint/paint.h"
#include "raster/pixel.h"
#include "raster/surface.h"

/* ---- fixtures ---- */

/* A stroke through the given points with a round brush (pressure size off, 25 % spacing). */
typedef struct tstroke {
    pc_paint_stroke s;
    pc_paint_point pts[8];
} tstroke;

static void mk_stroke(tstroke *t, const double (*xy)[2], size_t n, float size, float hardness)
{
    memset(t, 0, sizeof *t);
    pc_paint_brush_default(&t->s.brush);
    t->s.brush.size = size;
    t->s.brush.hardness = hardness;
    t->s.brush.spacing = 0.25f;
    t->s.brush.pressure_size = false;
    for (size_t i = 0; i < n && i < 8; i++) {
        t->pts[i] = pc_paint_point_make(xy[i][0], xy[i][1], 1.0f);
    }
    t->s.points = t->pts;
    t->s.count = n < 8 ? n : 8;
}

static void fill(pc_surface *s, pc_rect r, const float *px, size_t n)
{
    CHECK(pc_surface_fill_rect(s, r, px, n, NULL) == PC_OK);
}

/* Fill with a straight RGBA colour converted to the surface's format. */
static void fill_rgba(pc_surface *s, pc_rect r, float c0, float c1, float c2, float c3)
{
    float rgba[4] = {c0, c1, c2, c3}, px[PC_RASTER_MAX_CHANNELS];
    size_t n = pc_from_rgba(pc_surface_format(s), rgba, px);
    fill(s, r, px, n);
}

static float px_at(const pc_surface *s, int32_t x, int32_t y, size_t c)
{
    float v[PC_RASTER_MAX_CHANNELS];
    pc_surface_read_pixel(s, x, y, v);
    return v[c];
}

static void rgba_at(const pc_surface *s, int32_t x, int32_t y, float out[4])
{
    pc_surface_rgba(s, x, y, out);
}

static float alpha_at(const pc_surface *s, int32_t x, int32_t y)
{
    float c[4];
    pc_surface_rgba(s, x, y, c);
    return c[3];
}

static const pc_pixel_format RGB_DEPTHS[3] = {{PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true},
                                              {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, true},
                                              {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, true}};

/* ---- retouch.rs ---- */

static void test_coverage_matches_apply_stroke_bounds(void)
{
    static const double xy[2][2] = {{10, 10}, {40, 12}};
    tstroke t;
    pc_rect r;
    float *cov = NULL;
    size_t n = 0, w;
    bool in01 = true;
    mk_stroke(&t, xy, 2, 8.0f, 1.0f);
    CHECK(pc_paint_stroke_coverage(&t.s, &r, &cov, &n, NULL) == PC_OK);
    CHECK(pc_rect_contains(r, 10, 10) && pc_rect_contains(r, 40, 12));
    w = pc_rect_width(r);
    CHECK(cov != NULL && fabsf(cov[(size_t)(10 - r.y0) * w + (size_t)(25 - r.x0)] - 1.0f) < 1e-6f);
    for (size_t i = 0; i < n; i++) {
        in01 = in01 && cov[i] >= 0.0f && cov[i] <= 1.0f;
    }
    CHECK(in01);
    free(cov);
    pc_paint_brush_fini(&t.s.brush);
}

static void test_coverage_composite_copies_exactly_at_full_hardness(void)
{
    static const pc_pixel_format fmts[5] = {{PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true},
                                            {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, true},
                                            {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, true},
                                            {PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, true},
                                            {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, true}};
    static const double xy[1][2] = {{20, 20}};
    for (int fi = 0; fi < 5; fi++) {
        pc_pixel_format fmt = fmts[fi];
        size_t n = pc_pixel_format_channels(fmt);
        float base[PC_RASTER_MAX_CHANNELS], p[PC_RASTER_MAX_CHANNELS];
        pc_surface s;
        tstroke t;
        pc_rect r, d;
        float *cov = NULL;
        size_t ncov = 0;
        pc_paint_region paint;
        bool ok = true;
        for (size_t i = 0; i < n; i++) {
            base[i] = i + 1 == n ? 1.0f : 0.2f;
        }
        pc_surface_init(&s, fmt);
        fill(&s, pc_rect_make(0, 0, 64, 64), base, n);
        mk_stroke(&t, xy, 1, 10.0f, 1.0f);
        CHECK(pc_paint_stroke_coverage(&t.s, &r, &cov, &ncov, NULL) == PC_OK);
        CHECK(pc_paint_region_new(r, n, &paint, NULL) == PC_OK);
        for (size_t i = 0; i < paint.len; i++) {
            paint.data[i] = fmt.alpha && i % n == n - 1 ? 1.0f : 0.8f;
        }
        CHECK(pc_paint_apply_coverage(&s, r, cov, ncov, 1.0f, NULL, false, &paint, PC_BLEND_NORMAL, &d,
                                      NULL) == PC_OK);
        CHECK(pc_rect_eq(d, r));
        pc_surface_read_pixel(&s, 20, 20, p);
        for (size_t i = 0; i + 1 < n; i++) {
            ok = ok && fabsf(p[i] - 0.8f) < 1e-2f;
        }
        CHECK(ok && p[n - 1] == 1.0f);
        CHECK(fabsf(px_at(&s, 40, 40, 0) - 0.2f) < 1e-2f);
        pc_paint_region_fini(&paint);
        free(cov);
        pc_paint_brush_fini(&t.s.brush);
        pc_surface_fini(&s);
    }
}

typedef struct seen_ctx {
    size_t idx[256];
    size_t n;
} seen_ctx;

static pc_status seen_effect(void *vctx, pc_paint_region *work, const pc_paint_footprint *fp, pc_error *err)
{
    seen_ctx *c = vctx;
    (void)err;
    if (c->n < 256) {
        c->idx[c->n++] = fp->index;
    }
    for (int32_t y = fp->rect.y0; y < fp->rect.y1; y++) {
        for (int32_t x = fp->rect.x0; x < fp->rect.x1; x++) {
            float *px = pc_paint_region_px(work, x, y);
            if (px != NULL && pc_paint_footprint_at(fp, x, y) > 0.5f) {
                px[0] += 0.1f;
            }
        }
    }
    return PC_OK;
}

static void test_dab_stroke_runs_sequentially_and_respects_selection(void)
{
    static const double xy[2][2] = {{10, 10}, {50, 10}};
    float one = 1.0f;
    pc_surface s, sel;
    tstroke t;
    seen_ctx seen = {{0}, 0};
    pc_rect d;
    bool seq = true;
    pc_surface_init(&s, PC_PIXEL_FORMAT_GRAY8);
    pc_surface_init(&sel, PC_PIXEL_FORMAT_GRAY8);
    fill(&sel, pc_rect_make(0, 0, 30, 100), &one, 1);
    mk_stroke(&t, xy, 2, 6.0f, 1.0f);
    CHECK(pc_paint_apply_dab_stroke(&s, &t.s, &sel, false, 2, seen_effect, &seen, &d, NULL) == PC_OK);
    CHECK(seen.n > 1);
    for (size_t i = 1; i < seen.n; i++) {
        seq = seq && seen.idx[i] == seen.idx[i - 1] + 1;
    }
    CHECK(seq);
    CHECK(px_at(&s, 20, 10, 0) > 0.1f);  /* dabs accumulate */
    CHECK(px_at(&s, 40, 10, 0) == 0.0f); /* outside the selection untouched */
    pc_paint_brush_fini(&t.s.brush);
    pc_surface_fini(&s);
    pc_surface_fini(&sel);
}

/* Smudge one stroke with the given settings; returns the stroke status. */
static pc_status smudge(pc_surface *s, const pc_paint_stroke *st, float strength, const float *finger,
                        size_t nfinger, float max, bool alpha)
{
    pc_paint_smudge sm;
    pc_rect d;
    pc_status r = pc_paint_smudge_init(&sm, strength, finger, nfinger, max, NULL);
    if (r != PC_OK) {
        return r;
    }
    if (alpha) {
        pc_paint_smudge_with_alpha(&sm, true, 3);
    }
    r = pc_paint_apply_dab_stroke(s, st, NULL, false, 0, pc_paint_smudge_effect, &sm, &d, NULL);
    pc_paint_smudge_fini(&sm);
    return r;
}

static void test_smudge_drags_colour_along_the_stroke(void)
{
    static const double xy[2][2] = {{20, 20}, {70, 20}};
    static const float white[4] = {1, 1, 1, 1}, red[4] = {1, 0, 0, 1}, blue[4] = {0, 0, 1, 1};
    for (int fi = 0; fi < 3; fi++) {
        pc_surface s, s2;
        tstroke t;
        mk_stroke(&t, xy, 2, 12.0f, 1.0f);
        pc_surface_init(&s, RGB_DEPTHS[fi]);
        fill(&s, pc_rect_make(0, 0, 100, 40), white, 4);
        fill(&s, pc_rect_make(0, 0, 30, 40), red, 4);
        CHECK(smudge(&s, &t.s, 0.8f, NULL, 0, 12.0f, false) == PC_OK);
        /* Red dragged to the right of the edge, fading with distance. */
        CHECK(px_at(&s, 35, 20, 1) < 0.5f);
        CHECK(px_at(&s, 35, 20, 1) < px_at(&s, 60, 20, 1));
        CHECK(px_at(&s, 35, 35, 1) == 1.0f); /* off-stroke untouched */
        /* Finger painting starts with the given colour. */
        pc_surface_init(&s2, RGB_DEPTHS[fi]);
        fill(&s2, pc_rect_make(0, 0, 100, 40), white, 4);
        CHECK(smudge(&s2, &t.s, 0.8f, blue, 4, 12.0f, false) == PC_OK);
        CHECK(px_at(&s2, 22, 20, 0) < 0.5f);
        pc_paint_brush_fini(&t.s.brush);
        pc_surface_fini(&s);
        pc_surface_fini(&s2);
    }
}

static void test_smudge_into_transparency_leaves_no_dark_fringe(void)
{
    /* #207: straight mixing dragged the black of transparent pixels into the colour. */
    static const double xy[2][2] = {{20, 20}, {70, 20}};
    static const float yellow[4] = {1.0f, 0.8f, 0.0f, 1.0f};
    for (int fi = 0; fi < 3; fi++) {
        pc_surface s;
        tstroke t;
        int smudged = 0, darkened = 0;
        float p[4];
        mk_stroke(&t, xy, 2, 12.0f, 0.5f);
        pc_surface_init(&s, RGB_DEPTHS[fi]);
        fill(&s, pc_rect_make(0, 0, 30, 40), yellow, 4);
        CHECK(smudge(&s, &t.s, 0.8f, NULL, 0, 12.0f, true) == PC_OK);
        for (int32_t x = 30; x < 80; x++) {
            for (int32_t y = 14; y < 27; y++) {
                rgba_at(&s, x, y, p);
                if (p[3] > 0.02f) {
                    smudged++;
                    darkened += !(p[0] > 0.97f && fabsf(p[1] - 0.8f) < 0.03f);
                }
            }
        }
        CHECK_EQ_INT(darkened, 0);
        CHECK(smudged > 50); /* colour was dragged into the transparent area */
        pc_surface_fini(&s);
        /* And at partial opacity the mix back is premultiplied too. */
        pc_surface_init(&s, RGB_DEPTHS[fi]);
        fill(&s, pc_rect_make(0, 0, 30, 40), yellow, 4);
        t.s.brush.opacity = 0.5f;
        CHECK(smudge(&s, &t.s, 0.8f, NULL, 0, 12.0f, true) == PC_OK);
        rgba_at(&s, 34, 20, p);
        CHECK(p[3] > 0.02f && p[0] > 0.97f);
        pc_paint_brush_fini(&t.s.brush);
        pc_surface_fini(&s);
    }
}

static void test_premultiplied_mix_ignores_colour_of_transparent_pixels(void)
{
    float d[4] = {0, 0, 0, 0}, s1[4] = {1.0f, 0.5f, 0.0f, 1.0f}, d2[4] = {0.2f, 0.2f, 0.2f, 1.0f},
          s2[4] = {0};
    float d3[2] = {0.5f, 0.5f}, s3[2] = {1.0f, 1.0f};
    pc_paint_mix_premultiplied(d, 4, s1, 4, 3, 0.25f);
    CHECK(d[0] == 1.0f && d[1] == 0.5f && d[2] == 0.0f && d[3] == 0.25f);
    pc_paint_mix_premultiplied(d2, 4, s2, 4, 3, 1.0f);
    CHECK(d2[3] == 0.0f);
    /* Out-of-range alpha index: untouched, no crash. */
    pc_paint_mix_premultiplied(d3, 2, s3, 2, 7, 0.5f);
    CHECK(d3[0] == 0.5f && d3[1] == 0.5f);
}

/* ---- mixer.rs ---- */

static void mixer_stroke(tstroke *t, double y)
{
    const double xy[2][2] = {{4, y}, {196, y}};
    mk_stroke(t, xy, 2, 8.0f, 1.0f);
}

static pc_paint_mixer_settings mset(float wet, float load, float mix, float flow)
{
    pc_paint_mixer_settings m;
    pc_paint_mixer_default(&m);
    m.wet = wet;
    m.load = load;
    m.mix = mix;
    m.flow = flow;
    return m;
}

static void test_low_load_runs_dry_and_full_load_does_not(void)
{
    static const float red[4] = {1, 0, 0, 1};
    for (int i = 0; i < 2; i++) {
        float load = i == 0 ? 0.0f : 1.0f, start, end;
        bool expect_dry = i == 0;
        pc_surface s;
        pc_paint_mixer_state st;
        pc_paint_mixer_settings m = mset(0.0f, load, 0.0f, 1.0f);
        tstroke t;
        pc_rect d;
        pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
        pc_paint_mixer_state_default(&st);
        pc_paint_mixer_state_load(&st, red);
        mixer_stroke(&t, 10.0);
        CHECK(pc_paint_apply_mixer_stroke(&s, NULL, &t.s, &m, &st, NULL, false, &d, NULL) == PC_OK);
        start = alpha_at(&s, 10, 10);
        end = alpha_at(&s, 190, 10);
        CHECK(start > 0.95f);
        if (expect_dry) {
            CHECK(end < start * 0.6f);
            CHECK(st.amount < 0.2f);
        } else {
            CHECK(end > 0.95f);
            CHECK(fabsf(st.amount - 1.0f) < 1e-6f);
        }
        pc_paint_brush_fini(&t.s.brush);
        pc_surface_fini(&s);
    }
}

static void test_wet_brush_picks_up_and_smears_canvas_colour(void)
{
    static const float red[4] = {1, 0, 0, 1}, blue[4] = {0, 0, 1, 1}, white[4] = {1, 1, 1, 1};
    pc_surface s;
    pc_paint_mixer_state st;
    pc_paint_mixer_settings m = mset(1.0f, 0.3f, 0.9f, 1.0f);
    tstroke t;
    pc_rect d;
    float c[4];
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    /* Blue canvas on the left half, white on the right. */
    fill(&s, pc_rect_make(0, 0, 100, 20), blue, 4);
    fill(&s, pc_rect_make(100, 0, 200, 20), white, 4);
    pc_paint_mixer_state_default(&st);
    pc_paint_mixer_state_load(&st, red);
    mixer_stroke(&t, 10.0);
    CHECK(pc_paint_apply_mixer_stroke(&s, NULL, &t.s, &m, &st, NULL, false, &d, NULL) == PC_OK);
    /* Blue is dragged into the white half. */
    rgba_at(&s, 115, 10, c);
    CHECK(c[2] > c[1] + 0.05f && c[1] < 0.95f);
    /* The pickup well now holds a canvas-contaminated colour. */
    CHECK(st.has_pickup && st.pickup[2] > 0.2f);
    pc_paint_mixer_state_clean(&st);
    CHECK(!st.has_pickup);
    pc_paint_brush_fini(&t.s.brush);
    pc_surface_fini(&s);
}

static void test_dry_empty_brush_paints_nothing(void)
{
    pc_surface s;
    pc_paint_mixer_state st;
    pc_paint_mixer_settings m;
    tstroke t;
    pc_rect d;
    pc_paint_mixer_default(&m);
    m.wet = 0.0f;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    pc_paint_mixer_state_default(&st);
    mixer_stroke(&t, 10.0);
    CHECK(pc_paint_apply_mixer_stroke(&s, NULL, &t.s, &m, &st, NULL, false, &d, NULL) == PC_OK);
    CHECK(!pc_rect_is_empty(d));
    CHECK(alpha_at(&s, 50, 10) == 0.0f);
    pc_paint_brush_fini(&t.s.brush);
    pc_surface_fini(&s);
}

/* Wetness on Pen Pressure: no pressure means a dry brush (no pickup), full pressure picks up. */
static bool wetness_run(float pressure, pc_paint_mixer_state *st)
{
    static const float red[4] = {1, 0, 0, 1}, blue[4] = {0, 0, 1, 1};
    pc_surface s;
    pc_paint_mixer_settings m = mset(1.0f, 1.0f, 1.0f, 1.0f);
    tstroke t;
    pc_rect d;
    bool ok;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    fill(&s, pc_rect_make(0, 0, 200, 20), blue, 4);
    pc_paint_mixer_state_default(st);
    pc_paint_mixer_state_load(st, red);
    mixer_stroke(&t, 10.0);
    t.s.brush.transfer.enabled = true;
    t.s.brush.transfer.wetness.control = PC_PAINT_CONTROL_PEN_PRESSURE;
    for (size_t i = 0; i < t.s.count; i++) {
        t.pts[i].pressure = pressure;
    }
    ok = pc_paint_apply_mixer_stroke(&s, NULL, &t.s, &m, st, NULL, false, &d, NULL) == PC_OK;
    pc_paint_brush_fini(&t.s.brush);
    pc_surface_fini(&s);
    return ok;
}

static void test_transfer_wetness_control_scales_pickup_per_dab(void)
{
    pc_paint_mixer_state st;
    CHECK(wetness_run(0.0f, &st));
    CHECK(!st.has_pickup);
    CHECK(wetness_run(1.0f, &st));
    CHECK(st.has_pickup && st.pickup[2] > 0.2f);
}

/* ---- replace.rs ---- */

static void replace_setup(pc_pixel_format f, pc_surface *s)
{
    pc_surface_init(s, f);
    /* Left: mid green; right: dark blue. */
    fill_rgba(s, pc_rect_make(0, 0, 40, 40), 0.2f, 0.6f, 0.2f, 1.0f);
    fill_rgba(s, pc_rect_make(40, 0, 80, 40), 0.1f, 0.1f, 0.5f, 1.0f);
}

static void replace_stroke(tstroke *t)
{
    static const double xy[2][2] = {{20, 20}, {60, 20}};
    mk_stroke(t, xy, 2, 30.0f, 1.0f);
}

static void test_modes_follow_the_nonseparable_definitions(void)
{
    static const float green[3] = {0.2f, 0.6f, 0.2f}, redc[3] = {1, 0, 0};
    for (int mode = 0; mode < 4; mode++) {
        pc_surface s;
        pc_paint_replace_settings rs;
        tstroke t;
        pc_rect d;
        float c[4], b[4];
        replace_setup(PC_PIXEL_FORMAT_RGBA32F, &s);
        replace_stroke(&t);
        pc_paint_replace_settings_default(&rs);
        rs.mode = (pc_paint_replace_mode)mode;
        rs.sampling = PC_PAINT_SAMPLING_ONCE;
        rs.limits = PC_PAINT_LIMITS_DISCONTIGUOUS;
        rs.tolerance = 0.1f;
        CHECK(pc_paint_apply_color_replacement(&s, &t.s, &rs, NULL, false, &d, NULL) == PC_OK);
        rgba_at(&s, 20, 20, c);
        switch (rs.mode) {
        case PC_PAINT_REPLACE_HUE:
        case PC_PAINT_REPLACE_COLOR: /* keep the luminosity of the original */
            CHECK(fabsf(pc_blend_lum(c) - pc_blend_lum(green)) < 1e-3f);
            CHECK(c[0] > c[1]); /* turned red-ish */
            break;
        case PC_PAINT_REPLACE_SATURATION:
            CHECK(fabsf(pc_blend_lum(c) - pc_blend_lum(green)) < 1e-3f && c[1] > c[0]);
            break;
        default:
            CHECK(fabsf(pc_blend_lum(c) - pc_blend_lum(redc)) < 1e-3f);
            break;
        }
        /* Sampling once on green: the blue half is outside the tolerance and untouched. */
        rgba_at(&s, 60, 20, b);
        CHECK(fabsf(b[2] - 0.5f) < 1e-3f && fabsf(b[0] - 0.1f) < 1e-3f);
        pc_paint_brush_fini(&t.s.brush);
        pc_surface_fini(&s);
    }
}

static void test_continuous_sampling_follows_the_cursor_and_swatch_limits(void)
{
    pc_surface s, s2, ref;
    pc_paint_replace_settings rs;
    tstroke t;
    pc_rect d;
    float a[4], b[4], c[4];
    replace_setup(PC_PIXEL_FORMAT_RGBA8, &s);
    replace_stroke(&t);
    pc_paint_replace_settings_default(&rs);
    rs.tolerance = 0.1f;
    CHECK(pc_paint_apply_color_replacement(&s, &t.s, &rs, NULL, false, &d, NULL) == PC_OK);
    rgba_at(&s, 20, 20, a);
    rgba_at(&s, 70, 20, b);
    CHECK(a[0] > 0.3f && b[0] > b[1]); /* both halves recoloured */
    /* Background swatch = white: nothing matches. */
    replace_setup(PC_PIXEL_FORMAT_RGBA8, &s2);
    replace_setup(PC_PIXEL_FORMAT_RGBA8, &ref);
    rs.sampling = PC_PAINT_SAMPLING_BACKGROUND_SWATCH;
    CHECK(pc_paint_apply_color_replacement(&s2, &t.s, &rs, NULL, false, &d, NULL) == PC_OK);
    rgba_at(&s2, 20, 20, a);
    rgba_at(&ref, 20, 20, c);
    CHECK(memcmp(a, c, sizeof a) == 0);
    pc_paint_brush_fini(&t.s.brush);
    pc_surface_fini(&s);
    pc_surface_fini(&s2);
    pc_surface_fini(&ref);
}

static void test_contiguous_stops_at_disconnected_matches(void)
{
    static const float green[4] = {0.2f, 0.6f, 0.2f, 1.0f}, blue[4] = {0, 0, 1, 1};
    static const double xy[1][2] = {{20, 20}};
    pc_surface s;
    tstroke t;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA32F);
    fill(&s, pc_rect_make(0, 0, 60, 40), green, 4);
    /* A blue wall column separates a green island at x 34..40. */
    fill(&s, pc_rect_make(30, 0, 34, 40), blue, 4);
    mk_stroke(&t, xy, 1, 30.0f, 1.0f);
    for (int i = 0; i < 2; i++) {
        pc_paint_limits lim = i == 0 ? PC_PAINT_LIMITS_CONTIGUOUS : PC_PAINT_LIMITS_DISCONTIGUOUS;
        bool island_changed = i == 1;
        pc_surface u;
        pc_paint_replace_settings rs;
        pc_rect d;
        float c[4];
        CHECK(pc_surface_clone(&s, &u, NULL) == PC_OK);
        pc_paint_replace_settings_default(&rs);
        rs.sampling = PC_PAINT_SAMPLING_ONCE;
        rs.limits = lim;
        rs.tolerance = 0.05f;
        CHECK(pc_paint_apply_color_replacement(&u, &t.s, &rs, NULL, false, &d, NULL) == PC_OK);
        rgba_at(&u, 34, 20, c);
        CHECK((fabsf(c[0] - 0.2f) > 0.05f) == island_changed);
        rgba_at(&u, 20, 20, c);
        CHECK(c[0] > 0.3f);
        pc_surface_fini(&u);
    }
    pc_paint_brush_fini(&t.s.brush);
    pc_surface_fini(&s);
}

/* ---- bg_erase.rs ---- */

static void bg_setup(pc_pixel_format f, pc_surface *s)
{
    pc_surface_init(s, f);
    fill_rgba(s, pc_rect_make(0, 0, 40, 40), 0.2f, 0.6f, 0.2f, 1.0f);
    fill_rgba(s, pc_rect_make(40, 0, 80, 40), 0.1f, 0.1f, 0.5f, 1.0f);
}

static void bg_stroke(tstroke *t, const double (*xy)[2], size_t n)
{
    mk_stroke(t, xy, n, 30.0f, 1.0f);
}

static pc_paint_bg_erase_settings bgs(pc_paint_sampling sampling, pc_paint_limits limits, float tol)
{
    pc_paint_bg_erase_settings b;
    pc_paint_bg_erase_settings_default(&b);
    b.sampling = sampling;
    b.limits = limits;
    b.tolerance = tol;
    return b;
}

static void test_once_erases_only_the_sampled_colour_in_every_format(void)
{
    static const pc_color_mode modes[3] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_CMYK, PC_COLOR_MODE_GRAYSCALE};
    static const pc_sample_type samples[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    static const double xy[2][2] = {{20, 20}, {60, 20}};
    for (int mi = 0; mi < 3; mi++) {
        for (int si = 0; si < 3; si++) {
            pc_pixel_format f = {modes[mi], samples[si], true};
            pc_surface s;
            tstroke t;
            pc_rect d;
            pc_paint_bg_erase_settings b = bgs(PC_PAINT_SAMPLING_ONCE, PC_PAINT_LIMITS_DISCONTIGUOUS, 0.05f);
            bg_setup(f, &s);
            bg_stroke(&t, xy, 2);
            CHECK(pc_paint_apply_background_eraser(&s, &t.s, &b, NULL, &d, NULL) == PC_OK);
            CHECK(!pc_rect_is_empty(d));
            CHECK(alpha_at(&s, 20, 20) == 0.0f); /* green under the hotspot erased */
            CHECK(alpha_at(&s, 45, 20) == 1.0f); /* blue kept */
            CHECK(alpha_at(&s, 20, 39) == 1.0f); /* outside the brush kept */
            pc_paint_brush_fini(&t.s.brush);
            pc_surface_fini(&s);
        }
    }
}

static void test_continuous_follows_the_hotspot_and_swatch_matches_background(void)
{
    static const double xy[2][2] = {{20, 20}, {60, 20}}, xy2[2][2] = {{30, 20}, {50, 20}};
    static const float blue[4] = {0.1f, 0.1f, 0.5f, 1.0f};
    pc_surface s, t2;
    tstroke t;
    pc_rect d;
    pc_paint_bg_erase_settings b = bgs(PC_PAINT_SAMPLING_CONTINUOUS, PC_PAINT_LIMITS_DISCONTIGUOUS, 0.05f);
    bg_setup(PC_PIXEL_FORMAT_RGBA8, &s);
    bg_stroke(&t, xy, 2);
    CHECK(pc_paint_apply_background_eraser(&s, &t.s, &b, NULL, &d, NULL) == PC_OK);
    CHECK(alpha_at(&s, 20, 20) == 0.0f);
    CHECK(alpha_at(&s, 60, 20) == 0.0f); /* blue erased once the hotspot reached it */
    pc_paint_brush_fini(&t.s.brush);
    /* Background swatch = blue: only blue goes, wherever the hotspot is. */
    bg_setup(PC_PIXEL_FORMAT_RGBA8, &t2);
    b.sampling = PC_PAINT_SAMPLING_BACKGROUND_SWATCH;
    memcpy(b.background, blue, sizeof blue);
    bg_stroke(&t, xy2, 2);
    CHECK(pc_paint_apply_background_eraser(&t2, &t.s, &b, NULL, &d, NULL) == PC_OK);
    CHECK(alpha_at(&t2, 30, 20) == 1.0f);
    CHECK(alpha_at(&t2, 50, 20) == 0.0f);
    pc_paint_brush_fini(&t.s.brush);
    pc_surface_fini(&s);
    pc_surface_fini(&t2);
}

static void test_contiguous_and_protect_foreground(void)
{
    static const float green[4] = {0.2f, 0.6f, 0.2f, 1.0f}, blue[4] = {0.1f, 0.1f, 0.5f, 1.0f};
    static const double xy[1][2] = {{20, 20}};
    static const pc_paint_limits lims[3] = {PC_PAINT_LIMITS_CONTIGUOUS, PC_PAINT_LIMITS_FIND_EDGES,
                                            PC_PAINT_LIMITS_DISCONTIGUOUS};
    pc_surface s, u;
    tstroke t;
    pc_rect d;
    pc_paint_bg_erase_settings b;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA32F);
    fill(&s, pc_rect_make(0, 0, 60, 40), green, 4);
    /* A blue wall separates a green island at x 30..40. */
    fill(&s, pc_rect_make(26, 0, 30, 40), blue, 4);
    bg_stroke(&t, xy, 1);
    for (int i = 0; i < 3; i++) {
        bool island_erased = i == 2;
        b = bgs(PC_PAINT_SAMPLING_ONCE, lims[i], 0.05f);
        CHECK(pc_surface_clone(&s, &u, NULL) == PC_OK);
        CHECK(pc_paint_apply_background_eraser(&u, &t.s, &b, NULL, &d, NULL) == PC_OK);
        CHECK((alpha_at(&u, 31, 20) == 0.0f) == island_erased);
        CHECK(alpha_at(&u, 20, 20) == 0.0f);
        pc_surface_fini(&u);
    }
    /* Protecting a foreground equal to the sampled colour erases nothing. */
    b = bgs(PC_PAINT_SAMPLING_ONCE, PC_PAINT_LIMITS_CONTIGUOUS, 0.05f);
    b.protect_foreground = true;
    memcpy(b.foreground, green, sizeof green);
    CHECK(pc_surface_clone(&s, &u, NULL) == PC_OK);
    CHECK(pc_paint_apply_background_eraser(&u, &t.s, &b, NULL, &d, NULL) == PC_OK);
    CHECK(pc_surface_eq(&u, &s));
    pc_surface_fini(&u);
    pc_paint_brush_fini(&t.s.brush);
    pc_surface_fini(&s);
}

static void test_selection_limits_and_degenerate_inputs(void)
{
    static const double xy[1][2] = {{20, 20}}, xy5[1][2] = {{5, 5}};
    float one = 1.0f;
    pc_surface s, sel, t2, u, g;
    tstroke t, t5;
    pc_rect d;
    pc_paint_bg_erase_settings b = bgs(PC_PAINT_SAMPLING_ONCE, PC_PAINT_LIMITS_DISCONTIGUOUS, 0.05f);
    bg_setup(PC_PIXEL_FORMAT_RGBA8, &s);
    pc_surface_init(&sel, PC_PIXEL_FORMAT_GRAY8);
    fill(&sel, pc_rect_make(0, 0, 20, 40), &one, 1);
    bg_stroke(&t, xy, 1);
    bg_stroke(&t5, xy5, 1);
    CHECK(pc_paint_apply_background_eraser(&s, &t.s, &b, &sel, &d, NULL) == PC_OK);
    CHECK(alpha_at(&s, 15, 20) == 0.0f);
    CHECK(alpha_at(&s, 25, 20) == 1.0f); /* outside the selection kept */
    /* Sampling a transparent hotspot erases nothing; NaN tolerance acts as 0; no alpha: no-op. */
    pc_surface_init(&t2, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_paint_apply_background_eraser(&t2, &t5.s, &b, NULL, &d, NULL) == PC_OK);
    CHECK(pc_rect_width(d) > 0);
    CHECK(pc_rect_eq(pc_surface_content_bounds(&t2), PC_RECT_EMPTY));
    bg_setup(PC_PIXEL_FORMAT_RGBA8, &u);
    b.tolerance = NAN;
    CHECK(pc_paint_apply_background_eraser(&u, &t.s, &b, NULL, &d, NULL) == PC_OK);
    CHECK(alpha_at(&u, 20, 20) == 0.0f); /* exact match still erased at tolerance 0 */
    pc_surface_init(&g, PC_PIXEL_FORMAT_GRAY8);
    CHECK(pc_paint_apply_background_eraser(&g, &t5.s, &b, NULL, &d, NULL) == PC_OK);
    CHECK(pc_rect_is_empty(d));
    pc_paint_brush_fini(&t.s.brush);
    pc_paint_brush_fini(&t5.s.brush);
    pc_surface_fini(&s);
    pc_surface_fini(&sel);
    pc_surface_fini(&t2);
    pc_surface_fini(&u);
    pc_surface_fini(&g);
}

/* ---- C-only: enums, JSON, never crash ---- */

static void test_enum_names_and_json(void)
{
    pc_paint_replace_mode m;
    pc_paint_sampling sm;
    pc_paint_limits l;
    pc_paint_replace_settings rs, rs2;
    pc_paint_bg_erase_settings bs, bs2;
    pc_buf js = PC_BUF_INIT;
    pc_error err;
    CHECK(pc_paint_replace_mode_parse("luminosity", 10, &m) && m == PC_PAINT_REPLACE_LUMINOSITY);
    CHECK(!pc_paint_replace_mode_parse("Color", 5, &m));
    CHECK(!pc_paint_replace_mode_parse(NULL, 0, &m));
    CHECK(pc_paint_sampling_parse("backgroundSwatch", 16, &sm) && sm == PC_PAINT_SAMPLING_BACKGROUND_SWATCH);
    CHECK(pc_paint_limits_parse("findEdges", 9, &l) && l == PC_PAINT_LIMITS_FIND_EDGES);
    CHECK(pc_paint_replace_mode_name((pc_paint_replace_mode)9) == NULL);
    CHECK(pc_paint_sampling_name((pc_paint_sampling)-1) == NULL);
    CHECK_EQ_STR(pc_paint_limits_name(PC_PAINT_LIMITS_CONTIGUOUS), "contiguous");
    /* Round trips. */
    pc_paint_replace_settings_default(&rs);
    rs.mode = PC_PAINT_REPLACE_HUE;
    rs.tolerance = 0.75f;
    CHECK(pc_paint_json_to_string(PC_PAINT_JSON_REPLACE_SETTINGS, &rs, 0, &js, NULL) == PC_OK);
    CHECK(pc_paint_json_from_string(PC_PAINT_JSON_REPLACE_SETTINGS, pc_buf_cstr(&js), js.len, &rs2, NULL) ==
          PC_OK);
    CHECK(pc_paint_replace_settings_eq(&rs, &rs2));
    pc_buf_fini(&js);
    pc_paint_bg_erase_settings_default(&bs);
    bs.protect_foreground = true;
    CHECK(pc_paint_json_to_string(PC_PAINT_JSON_BG_ERASE_SETTINGS, &bs, 0, &js, NULL) == PC_OK);
    CHECK(pc_paint_json_from_string(PC_PAINT_JSON_BG_ERASE_SETTINGS, pc_buf_cstr(&js), js.len, &bs2, NULL) ==
          PC_OK);
    CHECK(pc_paint_bg_erase_settings_eq(&bs, &bs2));
    bs2.tolerance = 0.1f;
    CHECK(!pc_paint_bg_erase_settings_eq(&bs, &bs2));
    pc_buf_fini(&js);
    CHECK(pc_paint_json_from_string(PC_PAINT_JSON_SAMPLING, "\"twice\"", 7, &sm, &err) == PC_ERR_INVALID);
    CHECK_CONTAINS(err.msg, "unknown variant `twice`");
}

static void test_mixer_state_helpers(void)
{
    static const float c[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    pc_paint_mixer_state a, b;
    pc_paint_mixer_state_default(&a);
    pc_paint_mixer_state_default(&b);
    CHECK(pc_paint_mixer_state_eq(&a, &b));
    pc_paint_mixer_state_load(&a, c);
    CHECK(a.has_reservoir && a.amount == 1.0f && !pc_paint_mixer_state_eq(&a, &b));
    b = a;
    b.pickup[0] = 9.0f; /* ignored while there is no pickup */
    CHECK(pc_paint_mixer_state_eq(&a, &b));
}

static void test_regions_and_never_crash(void)
{
    pc_paint_region r = {0}, c = {0}, empty = {0};
    pc_surface s, s3;
    pc_rect d;
    float cov[4] = {1, 1, 1, 1}, v[3] = {0.5f, 0.5f, 0.5f};
    pc_error err;
    pc_paint_dab dab;
    pc_paint_footprint fp;
    pc_paint_smudge sm;
    /* Region basics. */
    CHECK(pc_paint_region_new(pc_rect_make(2, 3, 5, 7), 2, &r, NULL) == PC_OK);
    CHECK(r.len == 24 && pc_paint_region_width(&r) == 3 && pc_paint_region_height(&r) == 4);
    CHECK(pc_paint_region_px(&r, 4, 6) == r.data + 22);
    CHECK(pc_paint_region_px(&r, 5, 6) == NULL && pc_paint_region_px(&r, 1, 3) == NULL);
    r.data[22] = 7.0f;
    CHECK(pc_paint_region_crop(&r, pc_rect_make(4, 6, 99, 99), &c, NULL) == PC_OK);
    CHECK(c.len == 2 && c.data[0] == 7.0f && pc_rect_eq(c.rect, pc_rect_make(4, 6, 5, 7)));
    pc_paint_region_fini(&c);
    CHECK(pc_paint_region_crop(&r, pc_rect_make(40, 40, 50, 50), &c, NULL) == PC_OK && c.len == 0);
    pc_paint_region_fini(&c);
    CHECK(pc_paint_region_new(pc_rect_make(INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX), 4, &c, &err) ==
          PC_ERR_LIMIT);
    /* Writing a region with the wrong channel count is an error, not a crash. */
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_paint_region_write(&r, &s, NULL) != PC_OK);
    CHECK(pc_paint_region_write(&empty, &s, NULL) == PC_OK);
    /* apply_coverage: short paint channels, a short coverage map, paint not covering the rect. */
    CHECK(pc_paint_apply_coverage(&s, pc_rect_make(0, 0, 2, 2), cov, 4, 1.0f, NULL, false, &r,
                                  PC_BLEND_NORMAL, &d, &err) == PC_ERR_INVALID);
    pc_paint_region_fini(&r);
    CHECK(pc_paint_region_new(pc_rect_make(0, 0, 1, 1), 4, &r, NULL) == PC_OK);
    r.data[0] = 1.0f;
    r.data[3] = 1.0f;
    CHECK(pc_paint_apply_coverage(&s, pc_rect_make(0, 0, 4, 4), cov, 4, 1.0f, NULL, false, &r,
                                  PC_BLEND_MULTIPLY, &d, NULL) == PC_OK);
    CHECK(alpha_at(&s, 0, 0) == 1.0f && alpha_at(&s, 1, 0) == 0.0f);
    CHECK(pc_paint_apply_coverage(&s, PC_RECT_EMPTY, NULL, 0, 1.0f, NULL, false, &r, PC_BLEND_NORMAL, &d,
                                  NULL) == PC_OK);
    CHECK(pc_rect_is_empty(d));
    /* mix_back: regions that don't cover the rect. */
    CHECK(pc_paint_mix_back(&s, &r, &r, pc_rect_make(0, 0, 3, 3), 1.0f, NULL, false, &err) == PC_ERR_INVALID);
    pc_paint_region_fini(&r);
    /* Footprints: a hostile radius is a limit, not an abort. */
    dab = pc_paint_dab_round((pc_point){0, 0}, 1e9f, 1.0f);
    CHECK(pc_paint_footprint_round(&dab, 0, 1.0f, &fp, &err) == PC_ERR_LIMIT);
    dab = pc_paint_dab_round((pc_point){NAN, 1e300}, NAN, 1.0f);
    CHECK(pc_paint_footprint_round(&dab, 0, 1.0f, &fp, NULL) == PC_OK);
    pc_paint_footprint_fini(&fp);
    pc_paint_footprint_fini(NULL);
    /* Smudge with a hostile brush size reports a limit through the stroke. */
    {
        static const double xy[1][2] = {{10, 10}};
        tstroke t;
        mk_stroke(&t, xy, 1, 10.0f, 1.0f);
        CHECK(smudge(&s, &t.s, 0.5f, NULL, 0, 1e30f, false) == PC_ERR_LIMIT);
        CHECK(smudge(&s, &t.s, 0.5f, NULL, 0, -40.0f, false) == PC_ERR_LIMIT);
        CHECK(smudge(&s, &t.s, 0.5f, v, 0, NAN, true) == PC_OK); /* empty finger, NaN size: radius 2 */
        /* No effect at all, and an effect that fails: the target is untouched. */
        CHECK(pc_surface_clone(&s, &s3, NULL) == PC_OK);
        CHECK(pc_paint_apply_dab_stroke(&s, &t.s, NULL, false, 0, NULL, NULL, &d, NULL) == PC_OK);
        CHECK(pc_surface_eq(&s, &s3));
        pc_surface_fini(&s3);
        pc_paint_brush_fini(&t.s.brush);
    }
    CHECK(pc_paint_smudge_init(&sm, 0.5f, NULL, 0, 10.0f, NULL) == PC_OK);
    pc_paint_smudge_fini(&sm);
    pc_paint_smudge_fini(&sm);
    pc_paint_smudge_fini(NULL);
    /* keep_connected on an empty or out-of-range start. */
    CHECK(pc_paint_keep_connected(cov, 2, 2, 5, 5, NULL) == PC_OK);
    CHECK(cov[0] == 0.0f && cov[3] == 0.0f);
    CHECK(pc_paint_keep_connected(NULL, 0, 0, 0, 0, NULL) == PC_OK);
    pc_surface_fini(&s);
}

static pc_status failing_effect(void *ctx, pc_paint_region *work, const pc_paint_footprint *fp, pc_error *err)
{
    (void)ctx;
    for (size_t i = 0; i < work->len; i++) {
        work->data[i] = 0.5f; /* edits that must not reach the target */
    }
    return fp->index == 2 ? pc_fail(err, PC_ERR_CANCELLED, "stop") : PC_OK;
}

static void test_failed_effect_leaves_target_untouched(void)
{
    static const double xy[2][2] = {{10, 10}, {60, 10}};
    static const float red[4] = {1, 0, 0, 1};
    pc_surface s, ref;
    tstroke t;
    pc_rect d = pc_rect_make(1, 2, 3, 4);
    pc_error err;
    pc_paint_mixer_state st, st0;
    pc_paint_mixer_settings m = mset(1.0f, 0.5f, 0.5f, 1.0f);
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA16);
    fill(&s, pc_rect_make(0, 0, 80, 30), red, 4);
    CHECK(pc_surface_clone(&s, &ref, NULL) == PC_OK);
    mk_stroke(&t, xy, 2, 10.0f, 1.0f);
    CHECK(pc_paint_apply_dab_stroke(&s, &t.s, NULL, false, 0, failing_effect, NULL, &d, &err) ==
          PC_ERR_CANCELLED);
    CHECK(pc_surface_eq(&s, &ref));
    CHECK(pc_rect_eq(d, pc_rect_make(1, 2, 3, 4))); /* out-params only on success */
    /* A mixer stroke that fails (hostile brush through a smudge-free path cannot fail, so use an
     * absurd stroke bounds: the region read refuses) keeps its state. */
    pc_paint_mixer_state_default(&st);
    pc_paint_mixer_state_load(&st, red);
    st0 = st;
    t.pts[1].x = 3e9; /* walker steps are capped: PC_ERR_LIMIT or a huge region */
    if (pc_paint_apply_mixer_stroke(&s, NULL, &t.s, &m, &st, NULL, false, &d, &err) != PC_OK) {
        CHECK(pc_paint_mixer_state_eq(&st, &st0));
        CHECK(pc_surface_eq(&s, &ref));
    }
    pc_paint_brush_fini(&t.s.brush);
    pc_surface_fini(&s);
    pc_surface_fini(&ref);
}

/* The footprint never paints past the tip's reach; a hard tip reaches past the Full Size ring. */
static void test_footprint_stays_inside_the_tips_reach(void)
{
    static const float hs[3] = {1.0f, 0.5f, 0.0f};
    const float radius = 10.5f;
    for (int k = 0; k < 3; k++) {
        pc_paint_dab dab = pc_paint_dab_round((pc_point){32.0, 32.0}, radius, 1.0f);
        pc_paint_footprint fp;
        size_t painted = 0;
        double farthest = 0.0;
        double reach = (double)pc_paint_tip_reach(radius, hs[k]);
        double limit = (reach > (double)radius ? reach : (double)radius) + 0.5;
        CHECK(pc_paint_footprint_round(&dab, 0, hs[k], &fp, NULL) == PC_OK);
        for (int32_t y = fp.rect.y0; y < fp.rect.y1; y++) {
            for (int32_t x = fp.rect.x0; x < fp.rect.x1; x++) {
                double dx = (double)x + 0.5 - dab.center.x, dy = (double)y + 0.5 - dab.center.y;
                double dist = sqrt(dx * dx + dy * dy);
                if (pc_paint_footprint_at(&fp, x, y) <= 0.0f) {
                    continue;
                }
                painted++;
                farthest = dist > farthest ? dist : farthest;
                CHECK(dist < limit);
            }
        }
        CHECK(painted > 0);
        if (hs[k] == 1.0f) {
            CHECK(farthest > (double)radius);
        }
        pc_paint_footprint_fini(&fp);
    }
}

int main(int argc, char **argv)
{
    RUN(test_footprint_stays_inside_the_tips_reach);
    RUN(test_coverage_matches_apply_stroke_bounds);
    RUN(test_coverage_composite_copies_exactly_at_full_hardness);
    RUN(test_dab_stroke_runs_sequentially_and_respects_selection);
    RUN(test_smudge_drags_colour_along_the_stroke);
    RUN(test_smudge_into_transparency_leaves_no_dark_fringe);
    RUN(test_premultiplied_mix_ignores_colour_of_transparent_pixels);
    RUN(test_low_load_runs_dry_and_full_load_does_not);
    RUN(test_wet_brush_picks_up_and_smears_canvas_colour);
    RUN(test_dry_empty_brush_paints_nothing);
    RUN(test_transfer_wetness_control_scales_pickup_per_dab);
    RUN(test_modes_follow_the_nonseparable_definitions);
    RUN(test_continuous_sampling_follows_the_cursor_and_swatch_limits);
    RUN(test_contiguous_stops_at_disconnected_matches);
    RUN(test_once_erases_only_the_sampled_colour_in_every_format);
    RUN(test_continuous_follows_the_hotspot_and_swatch_matches_background);
    RUN(test_contiguous_and_protect_foreground);
    RUN(test_selection_limits_and_degenerate_inputs);
    RUN(test_enum_names_and_json);
    RUN(test_mixer_state_helpers);
    RUN(test_regions_and_never_crash);
    RUN(test_failed_effect_leaves_target_untouched);
    return pc_t_finish();
}
