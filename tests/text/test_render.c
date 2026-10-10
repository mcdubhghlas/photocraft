/* Rasteriser, rendering, optical kerning and glyph previews (ports of the #[test]s in
 * crates/text/src/raster.rs, render.rs, optical.rs and glyphs.rs (preview), plus never-crash
 * tests: absurd sizes, NaN transforms, bad indexes, and concurrent drawing). */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/file.h"
#include "harness.h"
#include "text/text.h"

static float sum(const float *v, size_t n)
{
    float s = -0.0f;
    size_t i;
    for (i = 0; i < n; i++) {
        s += v[i];
    }
    return s;
}

/* ---- raster.rs ---- */

static void test_square_area_exact(void)
{
    pc_coverage c;
    float v[100];
    pc_line_sink s;
    pc_xform id = PC_XFORM_IDENTITY;
    CHECK(pc_coverage_init(&c, 10, 10, NULL) == PC_OK);
    s = pc_coverage_sink(&c);
    pc_rect_to(&s, &id, 2.25, 3.5, 6.75, 8.0);
    pc_coverage_finish(&c, v);
    CHECK_NEAR(sum(v, 100), 4.5 * 4.5, 1e-3);
    CHECK(v[5 * 10 + 4] == 1.0f);
    CHECK(v[0] == 0.0f);
    pc_coverage_fini(&c);
}

static void test_triangle_area_and_clipping(void)
{
    pc_coverage c;
    float v[64];
    static const double pts[3][2] = {{-4.0, 2.0}, {6.0, 2.0}, {-4.0, 12.0}};
    int i;
    CHECK(pc_coverage_init(&c, 8, 8, NULL) == PC_OK);
    for (i = 0; i < 3; i++) {
        pc_coverage_line(&c, pts[i][0], pts[i][1], pts[(i + 1) % 3][0], pts[(i + 1) % 3][1]);
    }
    pc_coverage_finish(&c, v);
    /* Visible part: x in [0,6], y in [2,8] under x + y <= 8: area 18. */
    CHECK_NEAR(sum(v, 64), 18.0, 1e-3);
    for (i = 0; i < 64; i++) {
        CHECK(v[i] >= 0.0f && v[i] <= 1.0f);
    }
    pc_coverage_fini(&c);
}

static void test_circle_area_via_pen(void)
{
    pc_coverage c;
    float v[1600];
    pc_line_sink s;
    pc_flatten_pen p;
    float r = 15.0f, k = 0.5522848f * 15.0f;
    double exact = 3.14159265358979323846 * 225.0, a;
    CHECK(pc_coverage_init(&c, 40, 40, NULL) == PC_OK);
    s = pc_coverage_sink(&c);
    pc_flatten_pen_init(&p, &s, (pc_xform){{1.0, 0.0, 0.0, -1.0, 20.0, 20.0}});
    pc_flatten_pen_move_to(&p, r, 0.0f);
    pc_flatten_pen_curve_to(&p, r, k, k, r, 0.0f, r);
    pc_flatten_pen_curve_to(&p, -k, r, -r, k, -r, 0.0f);
    pc_flatten_pen_curve_to(&p, -r, -k, -k, -r, 0.0f, -r);
    pc_flatten_pen_curve_to(&p, k, -r, r, -k, r, 0.0f);
    pc_flatten_pen_close(&p);
    pc_coverage_finish(&c, v);
    a = (double)sum(v, 1600);
    CHECK(fabs(a - exact) / exact < 0.005);
    pc_coverage_fini(&c);
}

static void test_quad_pen_and_bounds(void)
{
    pc_bounds b = {false, {0, 0, 0, 0}};
    pc_line_sink s = pc_bounds_sink(&b);
    pc_flatten_pen p;
    pc_flatten_pen_init(&p, &s, PC_XFORM_IDENTITY);
    pc_flatten_pen_move_to(&p, 0.0f, 0.0f);
    pc_flatten_pen_quad_to(&p, 5.0f, 10.0f, 10.0f, 0.0f);
    pc_flatten_pen_close(&p);
    CHECK(b.has && b.r[0] == 0.0 && b.r[2] == 10.0 && b.r[3] > 4.9 && b.r[3] <= 5.0);
    pc_bounds_add(&b, INFINITY, 0.0);
    CHECK(b.r[2] == 10.0);
}

/* ---- render.rs ---- */

static void test_bounds_are_clipped_without_overflow_and_oversized_rectangles_rejected(void)
{
    double mn = (double)INT32_MIN, mx = (double)INT32_MAX;
    struct {
        double b[4];
        pc_rect want;
    } cases[] = {
        {{mn, 0.0, mn + 8.0, 8.0}, {INT32_MIN, -1, INT32_MIN + 9, 9}},
        {{mx - 8.0, 0.0, mx, 8.0}, {INT32_MAX - 9, -1, INT32_MAX, 9}},
        {{-1.7976931348623157e308, 0.0, 1.7976931348623157e308, 8.0}, {0, 0, 0, 0}},
        {{0.0, 0.0, (double)PC_TEXT_MAX_PIXELS, 8.0}, {0, 0, 0, 0}},
        {{0.25, -3.25, 10.1, 8.8}, {-1, -5, 12, 10}},
        {{NAN, 0.0, 1.0, 1.0}, {0, 0, 0, 0}},
    };
    size_t i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pc_rect r = pc_text_rect_from_bounds(cases[i].b);
        CHECK(pc_rect_eq(r, cases[i].want));
        CHECK(pc_rect_eq(pc_text_rect_from_bounds(cases[i].b), r));
    }
}

/* ---- helpers ---- */

static pc_font *inter(void)
{
    size_t n;
    const pc_bundled_font *b = pc_text_bundled_fonts(&n);
    pc_blob *bl = NULL;
    pc_font *f = NULL;
    CHECK(pc_blob_new(b[0].data, b[0].len, &bl, NULL) == PC_OK);
    CHECK(pc_font_new(bl, 0, &f, NULL) == PC_OK);
    pc_blob_release(bl);
    return f;
}

static uint32_t gid_of(pc_font *f, uint32_t cp)
{
    pc_sfnt s;
    pc_sfnt_cmap c;
    pc_font_sfnt(f, &s);
    pc_sfnt_cmap_skrifa(&s, &c);
    return pc_sfnt_cmap_map(&c, cp);
}

/* A one-glyph layout like the reference's preview() makes (a line from the font's extents). */
static void one_glyph(pc_text_layout *l, pc_font *f, uint32_t cp, float size_px, pc_char_style *st)
{
    static pc_placed_glyph g;
    static pc_line_info li;
    float adv = 0.0f;
    pc_text_layout_init(l);
    pc_char_style_default(st);
    pc_text_layout_add_face(l, f, NULL, 0, size_px, false, 0.0f, NULL, NULL);
    g = (pc_placed_glyph){0, gid_of(f, cp), 0.0f, 0.0f, 0, PC_GLYPH_HORIZONTAL};
    (void)pc_font_advance(f, g.id, NULL, 0, &adv);
    li = (pc_line_info){
        0, 1, 0.0f, 0.0f, adv / (float)pc_font_units_per_em(f) * size_px, size_px * 0.97f, size_px * 0.24f,
        0};
    l->glyphs = &g;
    l->nglyphs = 1;
    l->lines = &li;
    l->nlines = 1;
    l->styles = st;
    l->nstyles = 1;
    l->px_per_pt = 1.0f;
}

static void drop_borrowed(pc_text_layout *l)
{
    l->glyphs = NULL;
    l->nglyphs = 0;
    l->lines = NULL;
    l->nlines = 0;
    l->styles = NULL;
    l->nstyles = 0;
    pc_text_layout_fini(l);
}

/* glyphs.rs preview_draws_ink, over a hand-made layout (the layouter is chunk L14). */
static void test_preview_draws_ink(void)
{
    pc_font *f = inter();
    pc_text_layout l;
    pc_char_style st;
    uint32_t px = 0, ink = 0;
    uint8_t *a = NULL;
    size_t i;
    one_glyph(&l, f, 'W', 32.0f * 0.72f, &st);
    l.px_per_pt = 1.0f;
    CHECK(pc_glyph_preview_from_layout(&l, 32, &px, &a, NULL) == PC_OK);
    CHECK_EQ_UINT(px, 32);
    for (i = 0; a != NULL && i < 32 * 32; i++) {
        ink += a[i];
    }
    CHECK(ink > 20 * 255);
    free(a);
    drop_borrowed(&l);
    one_glyph(&l, f, ' ', 32.0f * 0.72f, &st);
    a = NULL;
    CHECK(pc_glyph_preview_from_layout(&l, 32, &px, &a, NULL) == PC_OK);
    for (i = 0, ink = 0; a != NULL && i < 32 * 32; i++) {
        ink += a[i];
    }
    CHECK_EQ_UINT(ink, 0);
    free(a);
    a = NULL;
    CHECK(pc_glyph_preview_from_layout(&l, 100000, &px, &a, NULL) == PC_OK);
    CHECK_EQ_UINT(px, 512);
    free(a);
    drop_borrowed(&l);
    pc_font_release(f);
}

/* ---- optical.rs: the fitted model against Photoshop's optical kerning ---- */

static const char SAMPLE[] =
    "HHOOHnnoonvAVTAToTyLTWaYoPAFAVaxyrnilHIOHnHoxoxcdbpkkwwAAVVff1471LYTeKOnHeHaHsHtHgHzHkHxHvHwrst";
static const int PS_ARIAL[] = {
    -16,  -19, -20, -17,  -20,  -27, -22, -17, -20,  -44, -52,  -84, 15,  -108, -110, -118, -122, -123, -18,
    -130, 14,  -50, -111, -117, -15, -84, -12, -86,  -84, -73,  -6,  1,   -19,  -7,   -27,  -24,  -23,  -24,
    -22,  -17, -20, -23,  -15,  -44, -42, -44, -45,  -6,  -28,  -23, -23, -17,  7,    16,   -40,  34,   -84,
    26,   -23, 15,  -34,  -76,  -74, -88, -69, -141, 15,  -120, -16, -67, -13,  -23,  -17,  -19,  -18,  -22,
    -19,  -21, -20, -20,  -16,  -21, -28, -29, -21,  -19, -22,  -22, -24, -24,  -19,  -18,  -11,  -28};
static const int PS_GEORGIA[] = {
    -28, -51, -18, -53, -24,  -12, -38,  -17, -37, -36,  -79,  -110, 33,  -44, -42, -115, -115, -113, -46,
    -61, 22,  -84, -62, -102, -50, -109, 15,  -91, -110, -95,  11,   16,  13,  2,   -12,  -6,   -17,  -23,
    -53, -53, -24, -16, -50,  -41, -40,  -41, -37, -10,  -28,  -38,  -37, 12,  7,   31,   -63,  49,   -110,
    47,  -39, 13,  -14, -6,   -58, -10,  -20, -59, 36,   -110, -40,  -51, -19, -16, -50,  -39,  -33,  -10,
    -26, -40, -58, -10, -37,  -15, -18,  -18, -18, 2,    -7,   -6,   -49, -46, -53, 13,   -11,  -19};
static const int PS_TIMES[] = {
    3,    -38, -17, -38,  4,    11,  -26,  -16, -24,  -36,  -96,  -159, -3,  -70, -74, -114, -117, -122, -44,
    -105, 0,   -93, -114, -137, -36, -120, 8,   -117, -159, -104, -5,   -14, 10,  22,  -1,   -13,  2,    -1,
    -40,  -38, 4,   10,   -38,  -47, -49,  -47, -46,  -15,  -17,  -16,  -29, 20,  8,   12,   -91,  13,   -159,
    12,   -79, -2,  14,   -98,  -65, -71,  -55, -133, -4,   -116, -29,  -67, -3,  10,  -36,  -29,  -22,  -4,
    -16,  -28, -42, -5,   -35,  -1,  0,    -3,  7,    16,   2,    6,    -46, -44, -44, 8,    -7,   -22};
static const int PS_VERDANA[] = {
    -33,  -26, -21, -27,  -35,  -35, -27, -20, -28,  -52, -66,  -82, 33,  -93, -93, -120, -120, -121, -32,
    -102, 16,  -67, -103, -104, -26, -72, -20, -70,  -82, -59,  -27, 4,   -30, -12, -37,  -37,  -35,  -47,
    -56,  -27, -35, -33,  -26,  -53, -53, -53, -57,  -16, -37,  -28, -30, -19, 0,   -10,  -49,  26,   -82,
    26,   -20, 17,  -38,  -98,  -87, -69, -72, -102, 38,  -120, -22, -78, -24, -33, -28,  -20,  -33,  -33,
    -23,  -22, -29, -27,  -28,  -35, -41, -37, -37,  -19, -32,  -32, -32, -32, -38, -37,  5,    -15};

/* The reference reads macOS's Supplemental fonts; the same families from msttcorefonts are
 * tried too (other versions, so the thresholds are a check of the model, not of the files). */
static void test_optical_matches_photoshop(void)
{
    static const struct {
        const char *name;
        const char *paths[2];
        const int *ps;
    } F[4] = {
        {"ArialMT",
         {"/System/Library/Fonts/Supplemental/Arial.ttf", "/usr/share/fonts/corefonts/arial.ttf"},
         PS_ARIAL},
        {"Georgia",
         {"/System/Library/Fonts/Supplemental/Georgia.ttf", "/usr/share/fonts/corefonts/georgia.ttf"},
         PS_GEORGIA},
        {"TimesNewRomanPSMT",
         {"/System/Library/Fonts/Supplemental/Times New Roman.ttf", "/usr/share/fonts/corefonts/times.ttf"},
         PS_TIMES},
        {"Verdana",
         {"/System/Library/Fonts/Supplemental/Verdana.ttf", "/usr/share/fonts/corefonts/verdana.ttf"},
         PS_VERDANA},
    };
    size_t npairs = strlen(SAMPLE) - 1, i, k, n = 0;
    double total = 0.0;
    for (i = 0; i < 4; i++) {
        pc_buf data = PC_BUF_INIT;
        pc_blob *b = NULL;
        pc_font *f = NULL;
        pc_optical_cache *c = NULL;
        double fs = 0.0, rms;
        int wrong = 0;
        for (k = 0; k < 2 && data.len == 0; k++) {
            (void)pc_file_read(F[i].paths[k], (size_t)1 << 26, &data, NULL);
        }
        if (data.len == 0) {
            pc_buf_fini(&data);
            continue;
        }
        CHECK(pc_blob_new(data.data, data.len, &b, NULL) == PC_OK);
        CHECK(pc_font_new(b, 0, &f, NULL) == PC_OK);
        CHECK(pc_optical_cache_new(&c, NULL) == PC_OK);
        for (k = 0; f != NULL && k < npairs; k++) {
            float ours = 0.0f, theirs = (float)F[i].ps[k];
            if (!pc_optical_cache_pair(c, f, NULL, 0, gid_of(f, (uint8_t)SAMPLE[k]),
                                       gid_of(f, (uint8_t)SAMPLE[k + 1]), &ours)) {
                ours = 0.0f;
            }
            fs += (double)((ours - theirs) * (ours - theirs));
            if ((theirs < -40.0f && ours > 0.0f) || (theirs > 15.0f && ours < -15.0f)) {
                wrong++;
            }
        }
        rms = sqrt(fs / (double)npairs);
        (void)printf("  %s: rms %.1f/1000 em over %zu pairs\n", F[i].name, rms, npairs);
        CHECK(rms < 20.0);
        CHECK(wrong <= 2);
        total += fs;
        n += npairs;
        pc_optical_cache_free(c);
        pc_font_release(f);
        pc_blob_release(b);
        pc_buf_fini(&data);
    }
    if (n > 0) {
        double rms = sqrt(total / (double)n);
        (void)printf("  all: rms %.1f/1000 em\n", rms);
        CHECK(rms < 12.5);
    } else {
        (void)printf("  skipped: none of Arial, Georgia, Times New Roman, Verdana installed\n");
    }
}

static void test_optical_basics(void)
{
    pc_font *f = inter();
    pc_optical_profile p;
    pc_optical_cache *c = NULL;
    float k, f2[2], r2[2];
    CHECK(pc_optical_profile_of(f, NULL, 0, gid_of(f, ' '), &p, NULL) == PC_OK);
    CHECK(pc_optical_profile_is_empty(&p) && p.advance > 0.0f);
    CHECK(pc_optical_profile_of(f, NULL, 0, gid_of(f, 'o'), &p, NULL) == PC_OK);
    CHECK(!pc_optical_profile_is_empty(&p));
    CHECK(pc_optical_profile_of(f, NULL, 0, 999999, &p, NULL) == PC_ERR_NOT_FOUND);
    CHECK(pc_optical_cache_new(&c, NULL) == PC_OK);
    CHECK(!pc_optical_cache_pair(c, f, NULL, 0, gid_of(f, ' '), gid_of(f, 'o'), &k)); /* spaces don't kern */
    CHECK(pc_optical_cache_pair(c, f, NULL, 0, gid_of(f, 'A'), gid_of(f, 'V'), &k) && k < -20.0f);
    CHECK(!pc_optical_cache_pair(c, f, NULL, 0, 999999, gid_of(f, 'o'), &k));
    CHECK(pc_optical_size_adjust(12.0f) > pc_optical_size_adjust(48.0f));
    CHECK(pc_optical_size_adjust(NAN) == 0.0f && pc_optical_size_adjust(-3.0f) == 0.0f);
    f2[0] = NAN;
    f2[1] = 0.0f;
    r2[0] = 0.1f;
    r2[1] = 0.1f;
    CHECK(pc_optical_kern_from_features(f2, r2) == 0.0f);
    f2[0] = -10.0f;
    CHECK(pc_optical_kern_from_features(f2, r2) == 400.0f);
    /* The cache stays bounded: many glyphs, then a clear. */
    {
        uint32_t g;
        for (g = 0; g < 600; g++) {
            (void)pc_optical_cache_pair(c, f, NULL, 0, g, g + 1, &k);
        }
    }
    pc_optical_cache_free(c);
    pc_font_release(f);
}

/* ---- never crash ---- */

static void test_absurd_sizes_and_transforms(void)
{
    pc_font *f = inter();
    pc_text_layout l;
    pc_char_style st;
    static const float SIZES[] = {0.0f, -12.0f, 1e-30f, 0.3f, 1e5f, 7e5f, 3.4e38f, INFINITY, -INFINITY, NAN};
    static const double T[][6] = {
        {NAN, 0, 0, 1, 0, 0},    {1, 0, 0, 1, INFINITY, 0},    {1e300, 0, 0, 1e300, 0, 0},
        {0, 0, 0, 0, 0, 0},      {1e-300, 0, 0, 1e-300, 5, 5}, {1e6, 0, 0, 1e6, 0, 0},
        {1, 2, 3, 4, -1e9, 1e9},
    };
    size_t i, k;
    for (i = 0; i < sizeof SIZES / sizeof SIZES[0]; i++) {
        one_glyph(&l, f, 'W', SIZES[i], &st);
        for (k = 0; k < sizeof T / sizeof T[0]; k++) {
            pc_affine t;
            pc_rendered r;
            pc_rect ink;
            pc_glyph_outline *o = NULL;
            size_t no = 0;
            pc_status s;
            memcpy(t.m, T[k], sizeof t.m);
            CHECK(pc_text_ink_rect(&l, &t, NULL, &ink, NULL) == PC_OK);
            s = pc_text_rasterize(&l, &t, (pc_pixel_format){PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true},
                                  PC_AA_SMOOTH, NULL, &r, NULL);
            CHECK(s == PC_OK || s == PC_ERR_LIMIT || s == PC_ERR_NOMEM);
            if (s == PC_OK) {
                CHECK(pc_rect_width(r.rect) == 0 ||
                      (uint64_t)pc_rect_width(r.rect) * pc_rect_height(r.rect) <= PC_TEXT_MAX_PIXELS);
                pc_rendered_fini(&r);
            }
            CHECK(pc_text_outlines(&l, &t, NULL, &o, &no, NULL) == PC_OK);
            pc_glyph_outlines_free(o, no);
        }
        drop_borrowed(&l);
    }
    pc_font_release(f);
}

static void test_bad_indexes_and_empty_layouts(void)
{
    pc_font *f = inter();
    pc_text_layout l;
    pc_char_style st;
    pc_placed_glyph g[3] = {{5, 36, 0, 0, 0, PC_GLYPH_HORIZONTAL},
                            {0, 36, 0, 0, 9, PC_GLYPH_ROTATED},
                            {0, 0xFFFFFFFFu, 0, 0, 0, 7}};
    pc_decoration_rect d = {0, 0, 10, 2, 42};
    pc_affine id = PC_AFFINE_IDENTITY;
    pc_rendered r;
    pc_rect ink;
    pc_glyph_outline *o = NULL;
    size_t no = 0;
    pc_coverage c;
    float b[4];
    /* Empty layout. */
    pc_text_layout_init(&l);
    CHECK(pc_text_rasterize(&l, &id, (pc_pixel_format){PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true},
                            PC_AA_NONE, NULL, &r, NULL) == PC_OK);
    CHECK(pc_rect_is_empty(r.rect));
    pc_rendered_fini(&r);
    CHECK(!pc_text_layout_bounds(&l, b));
    pc_text_layout_fini(&l);
    /* Out-of-range face, style and orientation values. */
    one_glyph(&l, f, 'W', 20.0f, &st);
    l.glyphs = g;
    l.nglyphs = 3;
    l.decorations = &d;
    l.ndecorations = 1;
    CHECK(pc_text_ink_rect(&l, &id, NULL, &ink, NULL) == PC_OK);
    CHECK(pc_text_outlines(&l, &id, NULL, &o, &no, NULL) == PC_OK);
    pc_glyph_outlines_free(o, no);
    l.decorations = NULL;
    l.ndecorations = 0;
    drop_borrowed(&l);
    /* Arguments. */
    CHECK(pc_text_rasterize(NULL, &id, (pc_pixel_format){PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true},
                            PC_AA_NONE, NULL, &r, NULL) == PC_ERR_INVALID);
    CHECK(pc_coverage_init(&c, SIZE_MAX / 2, 4, NULL) == PC_ERR_LIMIT);
    CHECK(pc_font_draw(f, 36, NULL, NULL, 3, NULL, NULL) == PC_ERR_INVALID);
    pc_font_release(f);
}

/* pc_font is shared between threads: every thread must draw the same outlines. */
typedef struct job {
    pc_font *f;
    uint64_t hash;
} job;

static void h_line(void *ctx, double x0, double y0, double x1, double y1)
{
    uint64_t *h = ctx;
    double v[4] = {x0, y0, x1, y1};
    const uint8_t *b = (const uint8_t *)v;
    size_t i;
    for (i = 0; i < sizeof v; i++) {
        *h = (*h ^ b[i]) * 0x100000001b3ull;
    }
}

static void *draw_many(void *arg)
{
    job *j = arg;
    uint32_t g;
    j->hash = 0xcbf29ce484222325ull;
    for (g = 0; g < 300; g++) {
        pc_line_sink s = {h_line, &j->hash};
        pc_flatten_pen p;
        pc_outline_pen op;
        float size = 10.0f + (float)(g % 7);
        pc_flatten_pen_init(&p, &s, PC_XFORM_IDENTITY);
        op = pc_flatten_pen_outline(&p);
        (void)pc_font_draw(j->f, g, &size, NULL, 0, &op, NULL);
        (void)pc_font_draw(j->f, g, NULL, NULL, 0, &op, NULL);
    }
    return NULL;
}

static void test_threads_share_a_font(void)
{
    pc_font *f = inter();
    pthread_t t[4];
    job j[4];
    int i, started = 0;
    for (i = 0; i < 4; i++) {
        j[i].f = f;
        if (pthread_create(&t[i], NULL, draw_many, &j[i]) == 0) {
            started++;
        } else {
            break;
        }
    }
    for (i = 0; i < started; i++) {
        pthread_join(t[i], NULL);
    }
    for (i = 1; i < started; i++) {
        CHECK(j[i].hash == j[0].hash);
    }
    pc_font_release(f);
}

int main(int argc, char **argv)
{
    RUN(test_square_area_exact);
    RUN(test_triangle_area_and_clipping);
    RUN(test_circle_area_via_pen);
    RUN(test_quad_pen_and_bounds);
    RUN(test_bounds_are_clipped_without_overflow_and_oversized_rectangles_rejected);
    RUN(test_preview_draws_ink);
    RUN(test_optical_matches_photoshop);
    RUN(test_optical_basics);
    RUN(test_absurd_sizes_and_transforms);
    RUN(test_bad_indexes_and_empty_layouts);
    RUN(test_threads_share_a_font);
    return pc_t_finish();
}
