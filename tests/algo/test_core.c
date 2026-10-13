/* Unit tests of the L4 algo core: the Rust #[test]s of histogram.rs, tone.rs, resample.rs,
 * transform.rs and pyramid.rs, the lib.rs driver tests that need only the test filters of
 * testdefs.h, and never-crash checks for hostile parameters. */
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "base/json.h"
#include "harness.h"
#include "raster/pixel.h"
#include "testdefs.h"

static const pc_rect R = {0, 0, 40, 30};

static pc_pixel_format rgba_fmt(pc_sample_type s)
{
    pc_pixel_format f = {PC_COLOR_MODE_RGB, s, true};
    return f;
}

/* tests.rs pattern(): a deterministic image over r. */
static void pattern(pc_surface *s, pc_sample_type st, pc_rect r)
{
    size_t n = (size_t)pc_rect_width(r) * pc_rect_height(r) * 4, k = 0;
    float *v = malloc(n * sizeof(float) + 4);
    int32_t x, y;
    pc_surface_init(s, rgba_fmt(st));
    for (y = r.y0; y < r.y1; y++) {
        for (x = r.x0; x < r.x1; x++) {
            v[k++] = (float)((x * 7 + y * 3) % 64) / 63.0f;
            v[k++] = (float)((x * x + y) % 50) / 49.0f;
            v[k++] = (float)(y % 9) / 8.0f;
            v[k++] = 1.0f;
        }
    }
    CHECK(pc_surface_write_region(s, r, v, n, NULL) == PC_OK);
    free(v);
}

static void flat(pc_surface *s, pc_sample_type st, pc_rect r, const float px[4])
{
    pc_surface_init(s, rgba_fmt(st));
    CHECK(pc_surface_fill_rect(s, r, px, 4, NULL) == PC_OK);
}

static float px_at(const pc_surface *s, int32_t x, int32_t y, size_t c)
{
    float p[PC_RASTER_MAX_CHANNELS];
    pc_surface_read_pixel(s, x, y, p);
    return p[c];
}

static bool regions_equal(const pc_surface *a, const pc_surface *b, pc_rect r)
{
    float *x = NULL, *y = NULL;
    size_t lx = 0, ly = 0;
    bool eq;
    if (pc_surface_read_region(a, r, &x, &lx, NULL) != PC_OK ||
        pc_surface_read_region(b, r, &y, &ly, NULL) != PC_OK) {
        free(x);
        free(y);
        return false;
    }
    eq = lx == ly && (lx == 0 || memcmp(x, y, lx * sizeof(float)) == 0);
    free(x);
    free(y);
    return eq;
}

static float max_diff(const pc_surface *a, const pc_surface *b, pc_rect r)
{
    float *x = NULL, *y = NULL, d = 0.0f;
    size_t lx = 0, ly = 0, i;
    CHECK(pc_surface_read_region(a, r, &x, &lx, NULL) == PC_OK);
    CHECK(pc_surface_read_region(b, r, &y, &ly, NULL) == PC_OK);
    for (i = 0; i < lx && i < ly; i++) {
        d = pc_rs_maxf(d, fabsf(x[i] - y[i]));
    }
    free(x);
    free(y);
    return d;
}

static bool have_usm(void)
{
    return pc_algo_filter_def_find("unsharpMask") != NULL;
}

/* ---- histogram.rs ---- */

static void test_exact_bins_and_channel_totals(void)
{
    float (*px)[4] = malloc(256 * sizeof *px);
    pc_algo_rgb_histogram *h = malloc(sizeof *h);
    size_t i, c, b;
    for (i = 0; i < 256; i++) {
        px[i][0] = (float)i / 255.0f, px[i][1] = (float)(255 - i) / 255.0f, px[i][2] = 0.5f, px[i][3] = 0.25f;
    }
    pc_algo_rgb_histogram_from_rgba((const float (*)[4])px, 256, h);
    CHECK_EQ_UINT(h->samples, 256);
    for (i = 0; i < 256; i++) {
        CHECK_EQ_UINT(h->channels[0][i], 1);
        CHECK_EQ_UINT(h->channels[1][i], 1);
    }
    CHECK_EQ_UINT(h->channels[2][128], 256);
    for (c = 0; c < 3; c++) {
        uint64_t sum = 0;
        for (b = 0; b < 256; b++) {
            sum += h->channels[c][b];
        }
        CHECK_EQ_UINT(sum, h->samples);
    }
    free(h);
    free(px);
}

static void test_empty_transparent_invalid_and_hdr_are_explicit(void)
{
    pc_algo_rgb_histogram *h = malloc(sizeof *h), *z = malloc(sizeof *z);
    float px[5][4] = {{0.7f, 0.9f, 0.4f, 0.0f},
                      {NAN, 0, 0, 1},
                      {0, INFINITY, 0, 1},
                      {0, 0, 0, NAN},
                      {-2.0f, 4.0f, 0.5f, 1.0f}};
    pc_algo_rgb_histogram_from_rgba(NULL, 0, h);
    memset(z, 0, sizeof *z);
    CHECK(memcmp(h, z, sizeof *h) == 0);
    pc_algo_rgb_histogram_from_rgba((const float (*)[4])px, 5, h);
    CHECK_EQ_UINT(h->samples, 1);
    CHECK_EQ_UINT(h->transparent, 1);
    CHECK_EQ_UINT(h->invalid, 3);
    CHECK_EQ_UINT(h->underflow[0], 1);
    CHECK_EQ_UINT(h->underflow[1], 0);
    CHECK_EQ_UINT(h->underflow[2], 0);
    CHECK_EQ_UINT(h->overflow[0], 0);
    CHECK_EQ_UINT(h->overflow[1], 1);
    CHECK_EQ_UINT(h->overflow[2], 0);
    CHECK_EQ_UINT(h->channels[0][0], 1);
    CHECK_EQ_UINT(h->channels[1][255], 1);
    CHECK_EQ_UINT(h->channels[2][128], 1);
    CHECK(px[4][0] == -2.0f && isnan(px[1][0]));
    free(h);
    free(z);
}

static void test_clipping_indicators_use_exact_samples(void)
{
    pc_algo_rgb_histogram *h = malloc(sizeof *h);
    const float px[3][4] = {
        {0.0001f, 0.9999f, 0.5f, 1.0f}, {-0.2f, 1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f, 0.0f}};
    pc_algo_rgb_histogram_from_rgba(px, 3, h);
    CHECK_EQ_UINT(h->shadows[0], 1);
    CHECK_EQ_UINT(h->shadows[1], 0);
    CHECK_EQ_UINT(h->shadows[2], 1);
    CHECK_EQ_UINT(h->highlights[0], 0);
    CHECK_EQ_UINT(h->highlights[1], 1);
    CHECK_EQ_UINT(h->highlights[2], 0);
    CHECK_EQ_UINT(h->channels[0][0], 2);
    CHECK_EQ_UINT(h->channels[1][255], 2);
    free(h);
}

static void test_rgb_and_gray_at_all_depths(void)
{
    static const pc_color_mode modes[2] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_GRAYSCALE};
    static const float vals[4] = {0.0f, 0.25f, 0.75f, 1.0f};
    pc_algo_rgb_histogram *h = malloc(sizeof *h);
    int m, st;
    for (m = 0; m < 2; m++) {
        for (st = 0; st < PC_SAMPLE_TYPE_COUNT; st++) {
            pc_pixel_format f = {modes[m], (pc_sample_type)st, true};
            pc_surface s;
            float px[4][4];
            int32_t x;
            int c, b;
            static const int bins[4] = {0, 64, 191, 255};
            pc_surface_init(&s, f);
            for (x = 0; x < 4; x++) {
                float rgb[4] = {vals[x], vals[x], vals[x], 1.0f}, g[2] = {vals[x], 1.0f};
                CHECK(pc_surface_write_pixel(&s, x, 0, m == 0 ? rgb : g, m == 0 ? 4 : 2, NULL) == PC_OK);
            }
            CHECK(pc_surface_read_rgba_into(&s, pc_rect_make(0, 0, 4, 1), px, 4, NULL) == PC_OK);
            pc_algo_rgb_histogram_from_rgba((const float (*)[4])px, 4, h);
            CHECK_EQ_UINT(h->samples, 4);
            for (c = 0; c < 3; c++) {
                for (b = 0; b < 4; b++) {
                    CHECK_EQ_UINT(h->channels[c][bins[b]], 1);
                }
            }
            pc_surface_fini(&s);
        }
    }
    free(h);
}

static void test_exact_clipping_endpoints(void)
{
    const float a[4] = {0.0001f, 0.9999f, 0.5f, 1.0f}, b[4] = {-0.1f, 1.0f, 0.0f, 1.0f};
    const float c[4] = {0.0f, 1.0f, 0.0f, 0.0f}, d[4] = {0.0f, INFINITY, 0.0f, 1.0f};
    uint8_t s, h;
    pc_algo_clipping(a, &s, &h);
    CHECK(s == 0 && h == 0);
    pc_algo_clipping(b, &s, &h);
    CHECK(s == 5 && h == 2);
    pc_algo_clipping(c, &s, &h);
    CHECK(s == 0 && h == 0);
    pc_algo_clipping(d, &s, &h);
    CHECK(s == 0 && h == 0);
}

/* ---- tone.rs ---- */

static float (*ramp(size_t w, size_t h))[4]
{
    float (*v)[4] = malloc(w * h * sizeof *v);
    size_t i;
    for (i = 0; i < w * h; i++) {
        float x = (float)(i % w) / (float)(w - 1);
        v[i][0] = x, v[i][1] = x * 0.8f, v[i][2] = x * 0.6f + 0.1f, v[i][3] = 1.0f;
    }
    return v;
}

static float luma4(const float *p)
{
    return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
}

/* tone/shadows_highlights.rs: a flat 121 x 121 image with one different pixel in the middle;
 * returns that pixel after Shadows/Highlights. */
static void sh_probe(const float bg[3], const float v[3], const pc_algo_shadows_highlights_params *p,
                     float out[3])
{
    const size_t w = 121, h = 121;
    float (*img)[4] = malloc(w * h * sizeof *img);
    size_t i;
    for (i = 0; i < w * h; i++) {
        img[i][0] = bg[0], img[i][1] = bg[1], img[i][2] = bg[2], img[i][3] = 1.0f;
    }
    img[60 * w + 60][0] = v[0], img[60 * w + 60][1] = v[1], img[60 * w + 60][2] = v[2];
    CHECK(pc_algo_shadows_highlights(img, w * h, w, h, p, NULL) == PC_OK);
    memcpy(out, img[60 * w + 60], 3 * sizeof *out);
    free(img);
}

static float sh_off(const float a[3], const int b[3])
{
    float m = 0.0f;
    int c;
    for (c = 0; c < 3; c++) {
        float d = fabsf(a[c] - (float)b[c] / 32768.0f) * 255.0f;
        m = d > m ? d : m;
    }
    return m;
}

/* grey_matches_photoshop: Photoshop 25.4's results (16-bit levels of 32768) for grey pixels
 * lifted by the default Shadows (35 %, Tone 50 %, Radius 30 px; Color 0). */
static void test_sh_grey_matches_photoshop(void)
{
    static const float vs[4] = {0.125f, 0.25f, 0.5f, 0.749969f};
    static const int want[2][4] = {{11389, 19422, 28451, 31931}, {5211, 9909, 18229, 25700}};
    pc_algo_shadows_highlights_params p = pc_algo_shadows_highlights_default();
    int b, k;
    p.color = 0.0f, p.black_clip = 0.0f, p.white_clip = 0.0f;
    for (b = 0; b < 2; b++) {
        float bg[3] = {(float)b * 0.5f, (float)b * 0.5f, (float)b * 0.5f};
        for (k = 0; k < 4; k++) {
            float v[3] = {vs[k], vs[k], vs[k]}, got[3];
            int wl[3] = {want[b][k], want[b][k], want[b][k]};
            sh_probe(bg, v, &p, got);
            CHECK(sh_off(got, wl) < 2.0f);
        }
    }
}

/* colour_matches_photoshop: colour differences scaled by Color (+20; Amount 35 %, Radius 10). */
static void test_sh_colour_matches_photoshop(void)
{
    static const float bgs[4] = {0.0f, 0.0f, 4915.0f / 32768.0f, 9830.0f / 32768.0f};
    static const int pxs[4][3] = {
        {3277, 1638, 0}, {8192, 6554, 4915}, {4915, 3277, 1638}, {11469, 9830, 8192}};
    static const int want[4][3] = {
        {8235, 5202, 2170}, {19350, 16514, 13676}, {10124, 7424, 4723}, {16540, 14376, 12214}};
    pc_algo_shadows_highlights_params p = pc_algo_shadows_highlights_default();
    int k, c;
    p.shadow_radius = 10.0f, p.black_clip = 0.0f, p.white_clip = 0.0f;
    for (k = 0; k < 4; k++) {
        float bg[3] = {bgs[k], bgs[k], bgs[k]}, v[3], got[3];
        for (c = 0; c < 3; c++) {
            v[c] = (float)pxs[k][c] / 32768.0f;
        }
        sh_probe(bg, v, &p, got);
        CHECK(sh_off(got, want[k]) < 2.0f);
    }
}

/* edge_matches_photoshop: across a strong edge the neighbourhood barely leaks. */
static void test_sh_edge_matches_photoshop(void)
{
    static const int xs[7] = {92, 98, 99, 100, 101, 102, 108};
    static const int want[7] = {16370, 14439, 12146, 30891, 30403, 30146, 29979};
    const size_t w = 200, h = 24;
    const float a = 3277.0f / 32768.0f, b = 29491.0f / 32768.0f;
    float (*img)[4] = malloc(w * h * sizeof *img);
    pc_algo_shadows_highlights_params p = pc_algo_shadows_highlights_default();
    size_t i;
    int k;
    for (i = 0; i < w * h; i++) {
        float v = i % w < 100 ? a : b;
        img[i][0] = img[i][1] = img[i][2] = v, img[i][3] = 1.0f;
    }
    p.shadow_amount = 100.0f, p.shadow_tone = 100.0f, p.shadow_radius = 10.0f, p.color = 0.0f;
    p.black_clip = 0.0f, p.white_clip = 0.0f;
    CHECK(pc_algo_shadows_highlights(img, w * h, w, h, &p, NULL) == PC_OK);
    for (k = 0; k < 7; k++) {
        float got = img[12 * w + (size_t)xs[k]][0] * 32768.0f;
        CHECK(fabsf(got - (float)want[k]) / 32768.0f * 255.0f < 6.0f);
    }
    free(img);
}

static void test_sh_strength_and_curve_follow_photoshop(void)
{
    static const float vs[3] = {0.1f, 0.3f, 0.6f};
    int k;
    CHECK(fabsf(pc_algo_sh_strength(50.0f) - 2.0f) < 2e-3f &&
          fabsf(pc_algo_sh_strength(100.0f) - 3.0f) < 1e-4f);
    CHECK(fabsf(pc_algo_sh_strength(15.0f) - 1.02f) < 0.01f &&
          fabsf(pc_algo_sh_strength(1.0f) - 0.19f) < 0.01f);
    /* Amount 50 is the image screened with itself twice: 1 - (1 - v)^4. */
    for (k = 0; k < 3; k++) {
        float c = 1.0f - vs[k];
        CHECK(fabsf(pc_algo_sh_lift(2.0f, vs[k]) - (1.0f - (c * c) * (c * c))) < 1e-6f);
    }
}

static void test_sh_tonal_mask_spans_photoshop_tones(void)
{
    float m95[256], m100[256], m0[256];
    pc_algo_sh_tonal_mask(95.0f, m95);
    pc_algo_sh_tonal_mask(100.0f, m100);
    pc_algo_sh_tonal_mask(0.0f, m0);
    CHECK(fabsf(m95[128] - (1.0f - 128.0f / 255.0f)) < 1e-3f);
    CHECK(memcmp(m95, m100, sizeof m95) == 0);
    /* Tone 0 still reaches about 7 % into the levels (Photoshop: half at level 18). */
    CHECK(m0[0] > 0.999f && fabsf(m0[18] - 0.5f) < 0.03f && m0[60] < 0.02f);
}

static void test_replace_color_only_touches_matching_pixels(void)
{
    float px[2][4] = {{1, 0, 0, 1}, {0, 0, 1, 1}}, m[2];
    const float red[3] = {1, 0, 0};
    pc_algo_replace_color(px, 2, red, 40.0f, 120.0f, 0.0f, 0.0f, m);
    CHECK(m[1] == 0.0f);
    CHECK(px[0][1] > 0.9f && px[0][0] < 0.1f);
    CHECK(px[1][0] == 0.0f && px[1][1] == 0.0f && px[1][2] == 1.0f && px[1][3] == 1.0f);
}

static void test_match_color_moves_statistics_toward_source(void)
{
    float src[64][4], tgt[64][4], tint[16][4];
    pc_algo_lab_stats s, t, after, n;
    pc_algo_match_color o = pc_algo_match_color_default();
    int i, k;
    for (i = 0; i < 64; i++) {
        src[i][0] = 0.8f, src[i][1] = 0.4f + (float)(i % 8) * 0.02f, src[i][2] = 0.2f, src[i][3] = 1.0f;
        tgt[i][0] = 0.2f, tgt[i][1] = 0.3f, tgt[i][2] = 0.6f + (float)(i % 8) * 0.03f, tgt[i][3] = 1.0f;
    }
    CHECK(pc_algo_lab_stats_compute((const float (*)[4])src, 64, NULL, 0, &s));
    CHECK(pc_algo_lab_stats_compute((const float (*)[4])tgt, 64, NULL, 0, &t));
    pc_algo_match_color_apply(tgt, 64, &t, &s, &o);
    CHECK(pc_algo_lab_stats_compute((const float (*)[4])tgt, 64, NULL, 0, &after));
    for (k = 0; k < 3; k++) {
        CHECK(fabsf(after.mean[k] - s.mean[k]) < fabsf(t.mean[k] - s.mean[k]) * 0.2f + 1.0f);
    }
    for (i = 0; i < 16; i++) {
        tint[i][0] = 0.7f, tint[i][1] = 0.5f, tint[i][2] = 0.3f + (float)i * 0.01f, tint[i][3] = 1.0f;
    }
    CHECK(pc_algo_lab_stats_compute((const float (*)[4])tint, 16, NULL, 0, &t));
    o.neutralize = true;
    pc_algo_match_color_apply(tint, 16, &t, NULL, &o);
    CHECK(pc_algo_lab_stats_compute((const float (*)[4])tint, 16, NULL, 0, &n));
    CHECK(fabsf(n.mean[1]) < 2.0f && fabsf(n.mean[2]) < 2.0f);
}

static void test_hdr_toning_compresses_range_and_keeps_bounds(void)
{
    float (*px)[4] = ramp(40, 10);
    pc_algo_hdr_toning_params p = pc_algo_hdr_toning_default();
    static const float inv[2][2] = {{0.0f, 1.0f}, {1.0f, 0.0f}};
    size_t i;
    int c;
    p.strength = 2.0f, p.detail = 0.0f, p.saturation = 0.0f;
    CHECK(pc_algo_hdr_toning(px, 400, 40, 10, &p, NULL) == PC_OK);
    CHECK(luma4(px[38]) > luma4(px[2]));
    for (i = 0; i < 400; i++) {
        for (c = 0; c < 3; c++) {
            CHECK(px[i][c] >= 0.0f && px[i][c] <= 1.0f);
        }
    }
    free(px);
    px = ramp(40, 10);
    p = pc_algo_hdr_toning_default();
    p.strength = 0.0f, p.detail = 0.0f, p.saturation = 0.0f, p.curve = inv, p.curve_len = 2;
    CHECK(pc_algo_hdr_toning(px, 400, 40, 10, &p, NULL) == PC_OK);
    CHECK(luma4(px[2]) > luma4(px[38]));
    free(px);
}

/* ---- resample.rs ---- */

static void ramp_surface(pc_surface *s, pc_sample_type st)
{
    float *v = malloc(64 * 32 * 4 * sizeof(float));
    size_t k = 0;
    int x, y;
    pc_surface_init(s, rgba_fmt(st));
    for (y = 0; y < 32; y++) {
        for (x = 0; x < 64; x++) {
            v[k++] = (float)x / 63.0f, v[k++] = 0.5f, v[k++] = 1.0f - (float)x / 63.0f, v[k++] = 1.0f;
        }
    }
    CHECK(pc_surface_write_region(s, pc_rect_make(0, 0, 64, 32), v, k, NULL) == PC_OK);
    free(v);
}

/* The reference's filter lists: Nearest, Bilinear, Bicubic, Lanczos, Preserve Details. */
static const pc_algo_resample RS5[5] = {PC_ALGO_RESAMPLE_NEAREST, PC_ALGO_RESAMPLE_BILINEAR,
                                        PC_ALGO_RESAMPLE_BICUBIC, PC_ALGO_RESAMPLE_LANCZOS,
                                        PC_ALGO_RESAMPLE_PRESERVE_DETAILS};

/* fixed_channel_rows_match_dynamic_resampling's kernels: Nearest, Bilinear, Bicubic and Sharper
 * designed for the scale, Lanczos. */
static pc_algo_rs_kernel rs_kernel(size_t i, double scale)
{
    pc_algo_rs_kernel k = {PC_ALGO_KERNEL_NEAREST, 0.0, 1.0};
    switch (i) {
    case 1:
        k.kind = PC_ALGO_KERNEL_BILINEAR;
        return k;
    case 2:
        return pc_algo_resample_keys(PC_ALGO_RESAMPLE_BICUBIC, scale);
    case 3:
        return pc_algo_resample_keys(PC_ALGO_RESAMPLE_BICUBIC_SHARPER, scale);
    case 4:
        k.kind = PC_ALGO_KERNEL_LANCZOS;
        return k;
    default:
        return k;
    }
}

/* The C port has one horizontal pass (the reference keeps fixed-width and dynamic copies); it must
 * equal the direct per-tap sum the dynamic version computes. */
static void test_horizontal_matches_direct_sum(void)
{
    static const double scales[4] = {0.12, 0.5, 1.0, 2.3};
    const int32_t content[2] = {-3, 31}, edge[2] = {-3, 31};
    size_t n, si, f, e;
    for (n = 1; n <= 5; n++) {
        for (si = 0; si < 4; si++) {
            for (f = 0; f < 5; f++) {
                for (e = 0; e < 2; e++) {
                    pc_algo_rs_kernel kern = rs_kernel(f, scales[si]);
                    pc_algo_taps t;
                    float row[35 * 5], def[5], *got, *want;
                    size_t i, k, j, c;
                    for (i = 0; i < 35 * n; i++) {
                        row[i] = (float)((i * 73) % 251) / 250.0f;
                    }
                    for (i = 0; i < n; i++) {
                        def[i] = (float)i / 7.0f;
                    }
                    CHECK(pc_algo_taps_make(-5, 41, scales[si], kern, e ? edge : NULL, content, &t, NULL) ==
                          PC_OK);
                    got = calloc(t.n * n, sizeof(float));
                    want = calloc(t.n * n, sizeof(float));
                    pc_algo_resample_horizontal(row, 35 * n, got, &t, -3, def, n);
                    for (k = 0; k < t.n; k++) {
                        for (j = 0; j < t.len[k]; j++) {
                            int64_t p = (int64_t)t.start[k] + (int64_t)j + 3;
                            if (p < 0 || (size_t)(p + 1) * n > 35 * n) {
                                continue;
                            }
                            for (c = 0; c < n; c++) {
                                want[k * n + c] += row[(size_t)p * n + c] * t.w[t.off[k] + j];
                            }
                        }
                        for (c = 0; c < n; c++) {
                            want[k * n + c] += def[c] * t.outside[k];
                        }
                    }
                    CHECK(memcmp(got, want, t.n * n * sizeof(float)) == 0);
                    free(got);
                    free(want);
                    pc_algo_taps_fini(&t);
                }
            }
        }
    }
}

static const uint16_t PS_0[112] = {
    3777,  13165, 22219, 16176, 10460, 19488, 29643, 11249, 12634, 21696, 13330, 10319, 18965, 29120,
    21280, 11204, 20279, 16184, 8200,  17548, 27703, 21824, 10182, 19259, 26967, 5238,  16528, 26683,
    17366, 8895,  17922, 32768, 2402,  15190, 25345, 17493, 7834,  16903, 31316, 1748,  14172, 24326,
    24718, 6368,  15746, 30390, 0,     13107, 23169, 31215, 4790,  14456, 27639, 5080,  11597, 21880,
    29649, 3864,  13507, 24136, 20600, 10264, 20931, 28009, 0,     12446, 21385, 28735, 8754,  19642,
    26154, 4049,  10895, 20832, 23523, 7620,  18485, 23204, 20126, 8787,  19818, 21447, 6173,  17467,
    20390, 31862, 6616,  18480, 24094, 6500,  16129, 19749, 27472, 5810,  17460, 28281, 7655,  15108,
    18394, 25515, 4427,  16043, 28749, 7026,  13691, 17871, 24992, 3904,  15520, 27200, 6075,  13168,
};
static const uint16_t PS_1[112] = {
    4201,  12337, 27677, 15894, 4816,  20021, 29631, 10348, 9882,  27304, 14492, 3766,  19574, 29046,
    18743, 5944,  25299, 16116, 4147,  17829, 27787, 19399, 4206,  21790, 23748, 7966,  15117, 26569,
    15941, 2695,  19445, 28341, 10104, 13050, 25465, 16251, 2613,  18023, 26770, 8432,  11951, 24281,
    22800, 7141,  15351, 24790, 5953,  11004, 23130, 28322, 10921, 12440, 24940, 9659,  9368,  22012,
    27611, 10006, 10488, 26419, 18657, 7071,  20807, 25492, 6937,  9239,  26574, 22363, 5435,  19688,
    24808, 9136,  8184,  25295, 19708, 3939,  18677, 25053, 18234, 7120,  24061, 17941, 2545,  17568,
    24869, 23748, 6081,  21401, 21862, 6018,  15056, 23602, 22177, 4859,  17816, 28450, 11911, 11653,
    22253, 20263, 3595,  15790, 29734, 12969, 9683,  21668, 19678, 3010,  15424, 28429, 11728, 9300,
};
static const uint16_t PS_2[120] = {
    11517, 10000, 8617,  9364,  12272, 16362, 21396, 25508, 25506, 19474, 10887, 5289,  5842,  10286, 14986,
    18566, 21910, 25511, 28611, 29922, 24788, 17949, 8497,  4593,  7309,  12594, 17150, 20945, 24892, 26196,
    22979, 16123, 9691,  7483,  10061, 14653, 18841, 22420, 25514, 26824, 25702, 23396, 17586, 11378, 8127,
    8882,  12310, 17039, 22345, 25871, 24700, 18125, 9951,  5493,  6802,  11215, 15576, 19259, 22384, 23694,
    26692, 26985, 22732, 14270, 6889,  4950,  8348,  13583, 17529, 21570, 25966, 27275, 22817, 14643, 8068,
    6911,  10694, 15930, 19443, 20705, 21499, 23413, 25601, 24336, 18665, 11686, 7800,  8947,  12974, 18067,
    22707, 25285, 23077, 16645, 9789,  6586,  8147,  12329, 15935, 17465, 17962, 20462, 24615, 25377, 20036,
    11458, 5421,  5415,  9528,  14134, 17782, 22482, 26926, 27479, 21881, 13308, 7533,  7734,  11549, 13992,
};
static const uint16_t PS_3[27] = {
    11315, 18740, 17475, 15974, 16503, 16042, 18215, 14712, 16238, 18482, 14351, 14664, 18012, 20260,
    13338, 15511, 18468, 18275, 16790, 19168, 14745, 14998, 20341, 18150, 13819, 16493, 16432,
};
static const uint16_t PS_4[20] = {
    11316, 14541, 17920, 10855, 24679, 19763, 9933,  23757, 6247,  20070,
    25601, 5325,  19149, 17306, 15463, 20992, 16384, 14541, 23143, 10854,
};
static const uint16_t PS_5[112] = {
    1638,  12698, 23757, 3482,  9011,  20070, 31130, 1638,  12698, 23757, 3482,  9011,  20070, 31130,
    31130, 10854, 21914, 1638,  7168,  18227, 29286, 29286, 9011,  20070, 31130, 5325,  16384, 27443,
    29286, 9011,  20070, 31130, 5325,  16384, 27443, 27443, 7168,  18227, 29286, 3482,  14541, 25600,
    25600, 5325,  16384, 27443, 1638,  12698, 23757, 25600, 5325,  16384, 27443, 1638,  12698, 23757,
    23757, 3482,  14541, 25600, 31130, 10854, 21914, 23757, 3482,  14541, 25600, 31130, 10854, 21914,
    21914, 1638,  12698, 23757, 29286, 9011,  20070, 20070, 31130, 10854, 21914, 27443, 7168,  18227,
    20070, 31130, 10854, 21914, 27443, 7168,  18227, 18227, 29286, 9011,  20070, 25600, 5325,  16384,
    16384, 27443, 7168,  18227, 23757, 3482,  14541, 16384, 27443, 7168,  18227, 23757, 3482,  14541,
};
static const uint16_t PS_6[120] = {
    9960,  6313,  993,   0,     4525,  14807, 26472, 32768, 32768, 23510, 10514, 0,     0,     0,     10110,
    22576, 31764, 32768, 32768, 32768, 26136, 18594, 6164,  0,     0,     2565,  14499, 26983, 32768, 32768,
    26358, 14709, 3969,  0,     0,     6602,  17734, 27952, 32768, 32768, 31590, 27808, 18307, 7429,  0,
    0,     3712,  14435, 26473, 32768, 31131, 20665, 7535,  0,     0,     2177,  13238, 25049, 32768, 32768,
    32768, 32768, 26136, 14174, 2548,  0,     0,     6190,  18705, 30273, 32768, 32768, 25517, 13238, 2177,
    0,     0,     7250,  19418, 26624, 32768, 32768, 32768, 28203, 18522, 7200,  0,     0,     3285,  14794,
    26862, 32768, 29913, 19013, 6667,  0,     0,     4443,  14506, 20751, 17344, 23066, 31075, 32361, 23949,
    10738, 0,     0,     0,     10110, 22657, 32768, 32768, 32768, 22658, 10192, 1004,  0,     0,     0,
};
static const uint16_t PS_7[112] = {
    801,   3050,  32489, 16260, 0,     29819, 32767, 9161,  1781,  29578, 14350, 0,     32768, 32767,
    21132, 0,     25409, 17862, 0,     26925, 32767, 22769, 0,     24839, 27566, 0,     9397,  32768,
    19813, 0,     27795, 32768, 0,     0,     32768, 20744, 0,     25797, 32768, 0,     0,     32768,
    27555, 0,     11184, 32768, 0,     0,     32768, 32768, 0,     0,     32768, 5110,  0,     30386,
    32768, 0,     0,     32768, 20385, 0,     23267, 32768, 0,     0,     32768, 29130, 0,     19191,
    32768, 2670,  0,     31290, 25387, 0,     21263, 32767, 20198, 0,     21586, 24510, 0,     23714,
    32768, 32768, 0,     14165, 27804, 1007,  14254, 32768, 30149, 0,     16077, 32112, 2886,  1884,
    21043, 29607, 0,     16384, 32768, 3566,  0,     12683, 29616, 0,     16384, 32768, 3196,  0,
};
static const uint16_t PS_8[120] = {
    9273,  5043,  0,     0,     2698,  14484, 28464, 32768, 32768, 24841, 10877, 0,     0,     0,     9684,
    22346, 31492, 32768, 32768, 32768, 25960, 18430, 4978,  0,     0,     977,   15047, 28475, 32768, 32768,
    27939, 14157, 1813,  0,     0,     5296,  17828, 28439, 32768, 32768, 32768, 27849, 17576, 6209,  0,
    0,     2169,  15076, 28935, 32768, 32768, 22167, 7399,  0,     0,     211,   12977, 24638, 32768, 32768,
    32768, 32768, 25960, 14244, 824,   0,     0,     5093,  18802, 31993, 32768, 32768, 26467, 12977, 211,
    0,     0,     6300,  19418, 26712, 32768, 32768, 32768, 27907, 17874, 5820,  0,     0,     1853,  16055,
    29518, 32768, 32768, 20889, 5880,  0,     0,     2879,  13798, 20185, 19010, 25565, 32768, 32302, 25431,
    11478, 0,     0,     0,     9684,  23083, 32768, 32768, 32768, 23084, 10422, 1276,  0,     0,     0,
};

/* matches_photoshop_image_size: Photoshop 25.4's Image Size of a 16-bit greyscale pattern, in
 * levels of 32768 (filters and their 2-D rules; on black and white the overshoot is clipped between
 * the passes, so their order shows). */
static void test_resample_matches_photoshop_image_size(void)
{
    static const struct {
        bool binary;
        pc_algo_resample filter;
        int32_t w, h, wo, ho;
        const uint16_t *want;
    } cases[9] = {
        {false, PC_ALGO_RESAMPLE_BICUBIC_SHARPER, 12, 10, 7, 16, PS_0},
        {false, PC_ALGO_RESAMPLE_BICUBIC_AUTOMATIC, 12, 10, 7, 16, PS_1},
        {false, PC_ALGO_RESAMPLE_BICUBIC, 12, 10, 20, 6, PS_2},
        {false, PC_ALGO_RESAMPLE_BICUBIC_SMOOTHER, 40, 3, 9, 3, PS_3},
        {false, PC_ALGO_RESAMPLE_BILINEAR, 12, 10, 5, 4, PS_4},
        {false, PC_ALGO_RESAMPLE_NEAREST, 12, 10, 7, 16, PS_5},
        {true, PC_ALGO_RESAMPLE_BICUBIC, 12, 10, 20, 6, PS_6},
        {true, PC_ALGO_RESAMPLE_BICUBIC_SHARPER, 12, 10, 7, 16, PS_7},
        {true, PC_ALGO_RESAMPLE_BICUBIC_SHARPER, 12, 10, 20, 6, PS_8},
    };
    const pc_pixel_format gray16 = {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U16, false};
    size_t c;
    for (c = 0; c < 9; c++) {
        int32_t w = cases[c].w, h = cases[c].h, wo = cases[c].wo, ho = cases[c].ho, x, y;
        pc_rect canvas = pc_rect_make(0, 0, w, h);
        pc_surface s, o;
        float *px = malloc((size_t)(w * h) * sizeof *px), *got = NULL, worst = 0.0f;
        size_t glen = 0, i;
        pc_surface_init(&s, gray16);
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                float k = (float)((x * 37 + y * 101) % 17);
                px[y * w + x] = cases[c].binary ? (k > 8.0f ? 32767.0f / 32768.0f : 0.0f)
                                                : roundf((k / 16.0f * 0.9f + 0.05f) * 32768.0f) / 32768.0f;
            }
        }
        CHECK(pc_surface_write_region(&s, canvas, px, (size_t)(w * h), NULL) == PC_OK);
        CHECK(pc_algo_resize_surface_in_canvas(&s, (double)wo / (double)w, (double)ho / (double)h,
                                               cases[c].filter, canvas, &o, NULL) == PC_OK);
        CHECK(pc_surface_read_region(&o, pc_rect_make(0, 0, wo, ho), &got, &glen, NULL) == PC_OK);
        CHECK_EQ_UINT(glen, (size_t)(wo * ho));
        for (i = 0; i < glen && i < (size_t)(wo * ho); i++) {
            float d = fabsf(got[i] * 32768.0f - (float)cases[c].want[i]);
            worst = d > worst ? d : worst;
        }
        CHECK(worst <= 8.0f);
        free(px), free(got);
        pc_surface_fini(&s), pc_surface_fini(&o);
    }
}

/* bicubic_is_photoshops_keys_cubic: an impulse doubled spreads as -9, -27, 67, 225 /256 and back. */
static void test_bicubic_is_photoshops_keys_cubic(void)
{
    static const float want[8] = {-9.0f, -27.0f, 67.0f, 225.0f, 225.0f, 67.0f, -27.0f, -9.0f};
    const pc_pixel_format grayf = {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_F32, false};
    float row[30], *got = NULL;
    size_t glen = 0, i;
    pc_surface s, o;
    for (i = 0; i < 30; i++) {
        row[i] = 0.25f;
    }
    row[10] = 0.75f;
    pc_surface_init(&s, grayf);
    CHECK(pc_surface_write_region(&s, pc_rect_make(0, 0, 30, 1), row, 30, NULL) == PC_OK);
    CHECK(pc_algo_resize_surface(&s, 2.0, 1.0, PC_ALGO_RESAMPLE_BICUBIC, &o, NULL) == PC_OK);
    CHECK(pc_surface_read_region(&o, pc_rect_make(17, 0, 25, 1), &got, &glen, NULL) == PC_OK);
    for (i = 0; i < 8 && i < glen; i++) {
        CHECK(fabsf((got[i] - 0.25f) * 512.0f - want[i]) < 1e-3f);
    }
    free(got);
    pc_surface_fini(&s), pc_surface_fini(&o);
}

static void test_identity_scale_is_lossless(void)
{
    int f;
    for (f = 0; f < 4; f++) {
        pc_surface s, o;
        ramp_surface(&s, PC_SAMPLE_TYPE_U16);
        CHECK(pc_algo_resize_surface(&s, 1.0, 1.0, RS5[f], &o, NULL) == PC_OK);
        CHECK(max_diff(&s, &o, pc_rect_make(0, 0, 64, 32)) < 1e-4f);
        pc_surface_fini(&s);
        pc_surface_fini(&o);
    }
}

static void test_half_and_double_sizes(void)
{
    int f;
    for (f = 0; f < 5; f++) {
        pc_surface s, half, dbl;
        float p[4];
        int32_t x;
        ramp_surface(&s, PC_SAMPLE_TYPE_F32);
        CHECK(pc_algo_resize_surface(&s, 0.5, 0.5, RS5[f], &half, NULL) == PC_OK);
        CHECK(pc_rect_eq(pc_surface_content_bounds(&half), pc_rect_make(0, 0, 32, 16)));
        pc_surface_read_pixel(&half, 16, 8, p);
        CHECK(fabsf(p[1] - 0.5f) < 0.02f && fabsf(p[3] - 1.0f) < 1e-3f);
        pc_surface_fini(&half);
        if (RS5[f] == PC_ALGO_RESAMPLE_PRESERVE_DETAILS && !have_usm()) {
            CHECK(pc_algo_resize_surface(&s, 2.0, 2.0, RS5[f], &dbl, NULL) == PC_ERR_UNSUPPORTED);
            pc_surface_fini(&s);
            continue;
        }
        CHECK(pc_algo_resize_surface(&s, 2.0, 2.0, RS5[f], &dbl, NULL) == PC_OK);
        CHECK(pc_rect_eq(pc_surface_content_bounds(&dbl), pc_rect_make(0, 0, 128, 64)));
        for (x = 8; x + 1 < 120; x++) {
            CHECK(px_at(&dbl, x + 1, 30, 0) >= px_at(&dbl, x, 30, 0) - 0.02f);
        }
        pc_surface_fini(&dbl);
        pc_surface_fini(&s);
    }
}

static void test_downscale_averages_checkerboard(void)
{
    pc_surface s, o;
    int x, y;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 64; x++) {
            float v = (float)((x + y) % 2), p[4] = {v, v, v, 1.0f};
            CHECK(pc_surface_write_pixel(&s, x, y, p, 4, NULL) == PC_OK);
        }
    }
    CHECK(pc_algo_resize_surface(&s, 0.25, 0.25, PC_ALGO_RESAMPLE_BICUBIC, &o, NULL) == PC_OK);
    CHECK(fabsf(px_at(&o, 8, 8, 0) - 0.5f) < 0.05f);
    pc_surface_fini(&s);
    pc_surface_fini(&o);
}

static void test_transparent_edges_do_not_darken(void)
{
    pc_surface s, o;
    const float red[4] = {1, 0, 0, 1};
    float e[4];
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA32F);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(10, 10, 20, 20), red, 4, NULL) == PC_OK);
    CHECK(pc_algo_resize_surface(&s, 1.7, 1.7, PC_ALGO_RESAMPLE_LANCZOS, &o, NULL) == PC_OK);
    pc_surface_read_pixel(&o, 17, 25, e);
    CHECK(e[3] > 0.0f && fabsf(e[0] - 1.0f) < 0.05f && fabsf(e[1]) < 0.05f);
    pc_surface_fini(&s);
    pc_surface_fini(&o);
}

static void test_canvas_edges_repeat_instead_of_fading(void)
{
    pc_rect canvas = pc_rect_make(0, 0, 30, 20);
    const float colour[4] = {0.2f, 0.6f, 1.0f, 1.0f}, red[4] = {1, 0, 0, 1};
    static const double ks[3] = {2.0, 1.37, 0.5};
    int st, f, k, i;
    for (st = 0; st < 3; st++) {
        pc_pixel_format fmt = rgba_fmt((pc_sample_type)st);
        pc_surface s, inner, a, b;
        pc_rect r = pc_rect_make(0, 0, 60, 40);
        pc_surface_init(&s, fmt);
        CHECK(pc_surface_fill_rect(&s, canvas, colour, 4, NULL) == PC_OK);
        for (f = 0; f < 5; f++) {
            for (k = 0; k < 3; k++) {
                pc_surface o;
                pc_rect dst = pc_algo_scaled_rect(canvas, ks[k], ks[k]);
                int32_t pts[6][2];
                if (RS5[f] == PC_ALGO_RESAMPLE_PRESERVE_DETAILS && ks[k] > 1.0 && !have_usm()) {
                    continue;
                }
                CHECK(pc_algo_resize_surface_in_canvas(&s, ks[k], ks[k], RS5[f], canvas, &o, NULL) == PC_OK);
                CHECK(pc_rect_eq(pc_surface_content_bounds(&o), dst));
                pts[0][0] = 0, pts[0][1] = 0;
                pts[1][0] = dst.x1 - 1, pts[1][1] = 0;
                pts[2][0] = 0, pts[2][1] = dst.y1 - 1;
                pts[3][0] = dst.x1 - 1, pts[3][1] = dst.y1 - 1;
                pts[4][0] = dst.x1 / 2, pts[4][1] = 0;
                pts[5][0] = 0, pts[5][1] = dst.y1 / 2;
                for (i = 0; i < 6; i++) {
                    float p[4], off = 0.0f;
                    int c;
                    pc_surface_read_pixel(&o, pts[i][0], pts[i][1], p);
                    for (c = 0; c < 4; c++) {
                        off = pc_rs_maxf(off, fabsf(p[c] - colour[c]));
                    }
                    CHECK(off < 0.01f);
                }
                pc_surface_fini(&o);
            }
        }
        pc_surface_init(&inner, fmt);
        CHECK(pc_surface_fill_rect(&inner, pc_rect_make(10, 5, 20, 15), red, 4, NULL) == PC_OK);
        CHECK(pc_algo_resize_surface_in_canvas(&inner, 1.7, 1.7, PC_ALGO_RESAMPLE_LANCZOS, canvas, &a,
                                               NULL) == PC_OK);
        CHECK(pc_algo_resize_surface(&inner, 1.7, 1.7, PC_ALGO_RESAMPLE_LANCZOS, &b, NULL) == PC_OK);
        CHECK(regions_equal(&a, &b, r));
        pc_surface_fini(&a), pc_surface_fini(&b), pc_surface_fini(&inner), pc_surface_fini(&s);
    }
}

static void test_tiny_target_of_a_huge_sparse_source(void)
{
    pc_surface s, o;
    const float white[4] = {1, 1, 1, 1};
    double k = 1.0 / 100096.0;
    float p[4];
    pc_rect b;
    int c;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA32F);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(0, 0, 256, 256), white, 4, NULL) == PC_OK);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(99840, 99840, 100096, 100096), white, 4, NULL) == PC_OK);
    CHECK(pc_algo_resize_surface(&s, k, k, PC_ALGO_RESAMPLE_BICUBIC, &o, NULL) == PC_OK);
    CHECK(pc_rect_eq(pc_surface_content_bounds(&o), pc_rect_make(0, 0, 1, 1)));
    pc_surface_read_pixel(&o, 0, 0, p);
    CHECK(p[3] >= 0.0f && p[3] < 1e-3f);
    for (c = 0; c < 4; c++) {
        CHECK(isfinite(p[c]));
    }
    pc_surface_fini(&o);
    CHECK(pc_algo_resize_surface(&s, 1000.0 / 100096.0, k, PC_ALGO_RESAMPLE_BICUBIC, &o, NULL) == PC_OK);
    b = pc_surface_content_bounds(&o);
    CHECK(pc_rect_height(b) == 1 && pc_rect_width(b) >= 1000 && pc_rect_width(b) <= 1001);
    CHECK(px_at(&o, 0, 0, 3) > 0.0f && px_at(&o, 500, 0, 3) == 0.0f);
    pc_surface_fini(&o);
    pc_surface_fini(&s);
}

static void test_skipped_empty_rows_match_reading_them(void)
{
    static const double ks[3][2] = {{0.013, 0.013}, {0.37, 0.37}, {1.0, 0.01}};
    int which, f, k;
    for (which = 0; which < 2; which++) {
        pc_pixel_format fmt = which == 0 ? PC_PIXEL_FORMAT_RGBA32F : PC_PIXEL_FORMAT_GRAY8;
        const float def0[4] = {0, 0, 0, 0}, def1[1] = {1.0f};
        const float *def = which == 0 ? def0 : def1;
        size_t n = which == 0 ? 4 : 1;
        const float half[4] = {0.5f, 0.5f, 0.5f, 0.5f}, quarter[4] = {0.25f, 0.25f, 0.25f, 0.25f};
        float tol = which == 0 ? 0.0f : 1e-6f;
        pc_surface sparse, dense;
        pc_surface_init_default(&sparse, fmt, def, n);
        CHECK(pc_surface_fill_rect(&sparse, pc_rect_make(0, 0, 300, 40), half, n, NULL) == PC_OK);
        CHECK(pc_surface_fill_rect(&sparse, pc_rect_make(100, 1500, 400, 1540), quarter, n, NULL) == PC_OK);
        CHECK(pc_surface_clone(&sparse, &dense, NULL) == PC_OK);
        CHECK(pc_surface_fill_rect(&dense, pc_rect_make(0, 40, 400, 1500), def, n, NULL) == PC_OK);
        CHECK(pc_surface_tile_count(&dense) > pc_surface_tile_count(&sparse));
        for (f = 1; f < 4; f++) {
            for (k = 0; k < 3; k++) {
                pc_surface a, b;
                pc_rect r;
                CHECK(pc_algo_resize_surface(&sparse, ks[k][0], ks[k][1], RS5[f], &a, NULL) == PC_OK);
                CHECK(pc_algo_resize_surface(&dense, ks[k][0], ks[k][1], RS5[f], &b, NULL) == PC_OK);
                r = pc_rect_union(pc_surface_content_bounds(&a), pc_surface_content_bounds(&b));
                CHECK(max_diff(&a, &b, r) <= tol);
                pc_surface_fini(&a), pc_surface_fini(&b);
            }
        }
        pc_surface_fini(&sparse), pc_surface_fini(&dense);
    }
}

static void test_chunked_reads_match_a_single_read(void)
{
    pc_surface s, mask;
    const float c1[4] = {0.3f, 0.9f, 0.1f, 0.5f}, one = 1.0f, zero = 0.0f, q = 0.25f;
    static const double ks[3] = {0.013, 0.37, 1.6};
    const pc_surface *srcs[2];
    int i, f, k;
    ramp_surface(&s, PC_SAMPLE_TYPE_F32);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(600, 700, 640, 720), c1, 4, NULL) == PC_OK);
    pc_surface_init_default(&mask, PC_PIXEL_FORMAT_GRAY8, &one, 1);
    CHECK(pc_surface_fill_rect(&mask, pc_rect_make(0, 0, 40, 30), &zero, 1, NULL) == PC_OK);
    CHECK(pc_surface_fill_rect(&mask, pc_rect_make(500, 900, 530, 940), &q, 1, NULL) == PC_OK);
    srcs[0] = &s, srcs[1] = &mask;
    for (i = 0; i < 2; i++) {
        for (f = 0; f < 4; f++) {
            for (k = 0; k < 3; k++) {
                pc_surface a, b;
                pc_rect r;
                CHECK(pc_algo_resize_with_budget(srcs[i], ks[k], ks[k], RS5[f], NULL, SIZE_MAX, &a, NULL) ==
                      PC_OK);
                CHECK(pc_algo_resize_with_budget(srcs[i], ks[k], ks[k], RS5[f], NULL, 1, &b, NULL) == PC_OK);
                r = pc_rect_union(pc_surface_content_bounds(&a), pc_surface_content_bounds(&b));
                CHECK(regions_equal(&a, &b, r));
                pc_surface_fini(&a), pc_surface_fini(&b);
            }
        }
    }
    pc_surface_fini(&s), pc_surface_fini(&mask);
}

static void test_masks_keep_default_pixel(void)
{
    pc_surface m, o;
    const float one = 1.0f, zero = 0.0f;
    float d[PC_RASTER_MAX_CHANNELS];
    pc_surface_init_default(&m, PC_PIXEL_FORMAT_GRAY8, &one, 1);
    CHECK(pc_surface_fill_rect(&m, pc_rect_make(0, 0, 10, 10), &zero, 1, NULL) == PC_OK);
    CHECK(pc_algo_resize_surface(&m, 2.0, 2.0, PC_ALGO_RESAMPLE_BILINEAR, &o, NULL) == PC_OK);
    pc_surface_default_pixel(&o, d);
    CHECK(d[0] == 1.0f);
    CHECK(px_at(&o, 5, 5, 0) < 0.01f);
    pc_surface_fini(&m), pc_surface_fini(&o);
}

static void test_translate_and_crop(void)
{
    pc_surface s, t, c;
    float a[4], b[4];
    ramp_surface(&s, PC_SAMPLE_TYPE_U8);
    CHECK(pc_algo_translate_surface(&s, 5, -3, &t, NULL) == PC_OK);
    pc_surface_read_pixel(&t, 15, 7, a);
    pc_surface_read_pixel(&s, 10, 10, b);
    CHECK(memcmp(a, b, sizeof a) == 0);
    CHECK(pc_algo_crop_surface(&s, pc_rect_make(0, 0, 10, 10), &c, NULL) == PC_OK);
    CHECK(pc_rect_eq(pc_surface_content_bounds(&c), pc_rect_make(0, 0, 10, 10)));
    pc_surface_fini(&s), pc_surface_fini(&t), pc_surface_fini(&c);
}

static void test_nearest_neighbor_reduction_does_not_average(void)
{
    pc_surface s, o;
    const float one = 1.0f;
    int x, y;
    pc_surface_init(&s, PC_PIXEL_FORMAT_GRAY8);
    for (y = 0; y < 8; y++) {
        for (x = 0; x < 8; x++) {
            if ((x + y) % 2 == 0) {
                CHECK(pc_surface_fill_rect(&s, pc_rect_make(x, y, x + 1, y + 1), &one, 1, NULL) == PC_OK);
            }
        }
    }
    CHECK(pc_algo_resize_surface(&s, 0.5, 0.5, PC_ALGO_RESAMPLE_NEAREST, &o, NULL) == PC_OK);
    for (y = 0; y < 4; y++) {
        for (x = 0; x < 4; x++) {
            float p = px_at(&o, x, y, 0);
            CHECK(p == 0.0f || p == 1.0f);
        }
    }
    pc_surface_fini(&s), pc_surface_fini(&o);
}

/* ---- transform.rs ---- */

static void test_homography_maps_rect_corners_and_inverts(void)
{
    const double r[4] = {10.0, 20.0, 110.0, 70.0};
    const double q[4][2] = {{0.0, 0.0}, {200.0, 10.0}, {180.0, 150.0}, {-20.0, 120.0}};
    const double p[4][2] = {{10.0, 20.0}, {110.0, 20.0}, {110.0, 70.0}, {10.0, 70.0}};
    const double z[4][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
    pc_algo_homography h, inv, zz, zi;
    double x, y, bx, by;
    int i;
    CHECK(pc_algo_homography_rect_to_quad(r, q, &h));
    for (i = 0; i < 4; i++) {
        pc_algo_homography_apply(&h, p[i][0], p[i][1], &x, &y);
        CHECK(fabs(x - q[i][0]) < 1e-9 && fabs(y - q[i][1]) < 1e-9);
    }
    CHECK(pc_algo_homography_inverse(&h, &inv));
    pc_algo_homography_apply(&inv, 90.0, 60.0, &x, &y);
    pc_algo_homography_apply(&h, x, y, &bx, &by);
    CHECK(fabs(bx - 90.0) < 1e-9 && fabs(by - 60.0) < 1e-9);
    CHECK(!pc_algo_homography_rect_to_quad(r, z, &zz) || !pc_algo_homography_inverse(&zz, &zi));
}

static void test_identity_and_translation_are_exact(void)
{
    pc_surface s;
    const float c[4] = {1.0f, 0.5f, 0.0f, 1.0f};
    pc_algo_homography h = {{1.0, 0.0, 7.0, 0.0, 1.0, -3.0, 0.0, 0.0, 1.0}};
    pc_rect r;
    int it;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(4, 4, 20, 12), c, 4, NULL) == PC_OK);
    r = pc_surface_content_bounds(&s);
    for (it = 0; it < 3; it++) {
        pc_surface o;
        float p[4];
        CHECK(pc_algo_warp_surface(&s, r, &h, (pc_algo_interp)it, &o, NULL) == PC_OK);
        CHECK(pc_rect_eq(pc_surface_content_bounds(&o), pc_rect_make(11, 1, 27, 9)));
        pc_surface_read_pixel(&o, 15, 5, p);
        CHECK(p[0] == 1.0f && p[1] == 128.0f / 255.0f && p[2] == 0.0f && p[3] == 1.0f);
        pc_surface_fini(&o);
    }
    pc_surface_fini(&s);
}

static void test_scale_doubles_size_and_rotation_keeps_area(void)
{
    pc_surface s, up, rot;
    const float c[4] = {0.2f, 0.4f, 0.6f, 1.0f};
    const double rr[4] = {0.0, 0.0, 40.0, 20.0};
    const double q2[4][2] = {{0.0, 0.0}, {80.0, 0.0}, {80.0, 40.0}, {0.0, 40.0}};
    const double qr[4][2] = {{30.0, -10.0}, {30.0, 30.0}, {10.0, 30.0}, {10.0, -10.0}};
    pc_algo_homography h;
    pc_rect r, b;
    float p[4];
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(0, 0, 40, 20), c, 4, NULL) == PC_OK);
    r = pc_surface_content_bounds(&s);
    CHECK(pc_algo_homography_rect_to_quad(rr, q2, &h));
    CHECK(pc_algo_warp_surface(&s, r, &h, PC_ALGO_INTERP_BICUBIC, &up, NULL) == PC_OK);
    b = pc_surface_content_bounds(&up);
    CHECK(pc_rect_width(b) >= 80 && pc_rect_width(b) <= 82 && pc_rect_height(b) >= 40 &&
          pc_rect_height(b) <= 42);
    CHECK(pc_algo_homography_rect_to_quad(rr, qr, &h));
    CHECK(pc_algo_warp_surface(&s, r, &h, PC_ALGO_INTERP_BILINEAR, &rot, NULL) == PC_OK);
    b = pc_surface_content_bounds(&rot);
    CHECK(abs((int)pc_rect_width(b) - 20) <= 2 && abs((int)pc_rect_height(b) - 40) <= 2);
    pc_surface_read_pixel(&rot, 20, 10, p);
    CHECK(fabsf(p[0] - 0.2f) < 0.01f && p[3] > 0.99f);
    pc_surface_fini(&s), pc_surface_fini(&up), pc_surface_fini(&rot);
}

static void test_large_downscale_is_prefiltered(void)
{
    pc_surface s, o;
    const double rr[4] = {0.0, 0.0, 64.0, 64.0};
    const double q[4][2] = {{0.0, 0.0}, {8.0, 0.0}, {8.0, 8.0}, {0.0, 8.0}};
    pc_algo_homography h;
    int x, y;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 64; x++) {
            float v = (float)((x + y) % 2), p[4] = {v, v, v, 1.0f};
            CHECK(pc_surface_fill_rect(&s, pc_rect_make(x, y, x + 1, y + 1), p, 4, NULL) == PC_OK);
        }
    }
    CHECK(pc_algo_homography_rect_to_quad(rr, q, &h));
    CHECK(pc_algo_warp_surface(&s, pc_surface_content_bounds(&s), &h, PC_ALGO_INTERP_BICUBIC, &o, NULL) ==
          PC_OK);
    CHECK(fabsf(px_at(&o, 4, 4, 0) - 0.5f) < 0.1f);
    pc_surface_fini(&s), pc_surface_fini(&o);
}

static void test_surfaces_without_alpha_gain_it(void)
{
    pc_pixel_format f = {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false};
    pc_surface s, o;
    const float red[3] = {1, 0, 0};
    pc_algo_homography h = {{1.0, 0.0, 5.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}};
    float p[4];
    pc_surface_init(&s, f);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(0, 0, 10, 10), red, 3, NULL) == PC_OK);
    CHECK(pc_algo_warp_surface(&s, pc_rect_make(0, 0, 10, 10), &h, PC_ALGO_INTERP_NEAREST, &o, NULL) ==
          PC_OK);
    CHECK(pc_surface_format(&o).alpha);
    CHECK(px_at(&o, 2, 2, 3) == 0.0f);
    pc_surface_read_pixel(&o, 7, 2, p);
    CHECK(p[0] == 1.0f && p[1] == 0.0f && p[2] == 0.0f && p[3] == 1.0f);
    pc_surface_fini(&s), pc_surface_fini(&o);
}

/* ---- pyramid.rs ---- */

static void test_laplacian_round_trip_is_exact(void)
{
    const size_t w = 37, h = 23;
    float *img = malloc(w * h * 2 * sizeof(float)), *back = NULL;
    size_t i, len = 0;
    pc_algo_pyramid p;
    for (i = 0; i < w * h * 2; i++) {
        img[i] = (float)(i * 37 % 101) / 101.0f;
    }
    CHECK(pc_algo_pyramid_laplacian(w, h, 2, img, 5, &p, NULL) == PC_OK);
    CHECK_EQ_UINT(p.n, 5);
    CHECK(pc_algo_pyramid_collapse(&p, 2, &back, &len, NULL) == PC_OK);
    CHECK_EQ_UINT(len, w * h * 2);
    for (i = 0; i < len; i++) {
        CHECK(fabsf(img[i] - back[i]) < 1e-4f);
    }
    free(back), free(img);
    pc_algo_pyramid_fini(&p);
}

static void test_blend_with_one_hot_weights_reproduces_each_side(void)
{
    const size_t w = 64, h = 32;
    float *a = malloc(w * h * sizeof(float)), *b = malloc(w * h * sizeof(float));
    float *wa = malloc(w * h * sizeof(float)), *wb = malloc(w * h * sizeof(float)), *out = NULL;
    size_t i;
    for (i = 0; i < w * h; i++) {
        a[i] = 0.2f, b[i] = 0.8f;
        wa[i] = i % w < 32 ? 1.0f : 0.0f;
        wb[i] = 1.0f - wa[i];
    }
    {
        const float *im[2] = {a, b}, *wt[2] = {wa, wb};
        CHECK(pc_algo_pyramid_blend(w, h, 1, im, wt, 2, pc_algo_pyramid_auto_levels(w, h), &out, NULL) ==
              PC_OK);
    }
    CHECK(fabsf(out[5 * w + 2] - 0.2f) < 1e-3f);
    CHECK(fabsf(out[5 * w + 61] - 0.8f) < 1e-3f);
    CHECK(out[5 * w + 31] > 0.3f && out[5 * w + 31] < 0.7f);
    free(a), free(b), free(wa), free(wb), free(out);
}

static void test_stack_picks_the_sharp_image(void)
{
    const size_t w = 48, h = 48;
    float *i0 = malloc(w * h * sizeof(float)), *i1 = malloc(w * h * sizeof(float)),
          *ones = malloc(w * h * sizeof(float));
    float *wts[2];
    size_t i;
    for (i = 0; i < w * h; i++) {
        size_t x = i % w, y = i / w;
        float chk = ((x / 2 + y / 2) % 2 == 0) ? 0.1f : 0.9f;
        i0[i] = x < 24 ? chk : 0.5f;
        i1[i] = x >= 24 ? chk : 0.5f;
        ones[i] = 1.0f;
    }
    {
        const float *l[2] = {i0, i1}, *a[2] = {ones, ones};
        CHECK(pc_algo_stack_weights(w, h, l, a, 2, wts, NULL) == PC_OK);
    }
    CHECK(wts[0][20 * w + 4] == 1.0f);
    CHECK(wts[1][20 * w + 44] == 1.0f);
    free(wts[0]), free(wts[1]), free(i0), free(i1), free(ones);
}

static void test_panorama_splits_the_overlap(void)
{
    const size_t w = 60, h = 20;
    float *a = malloc(w * h * sizeof(float)), *b = malloc(w * h * sizeof(float)), *wts[2];
    size_t i, seam = w;
    for (i = 0; i < w * h; i++) {
        a[i] = i % w < 40 ? 1.0f : 0.0f;
        b[i] = i % w >= 20 ? 1.0f : 0.0f;
    }
    {
        const float *al[2] = {a, b};
        CHECK(pc_algo_panorama_weights(w, h, al, 2, wts, NULL) == PC_OK);
    }
    CHECK(wts[0][10 * w + 5] == 1.0f);
    CHECK(wts[1][10 * w + 55] == 1.0f);
    for (i = 0; i < w; i++) {
        if (wts[1][10 * w + i] == 1.0f) {
            seam = i;
            break;
        }
    }
    CHECK(seam >= 25 && seam <= 35);
    free(wts[0]), free(wts[1]), free(a), free(b);
}

/* ---- lib.rs driver (tests.rs / tests_ext.rs, with the testdefs filters) ---- */

static pc_status run_filter(const pc_surface *s, const pc_algo_filter *f, pc_surface *out)
{
    pc_rect area = pc_algo_output_area(f, pc_surface_content_bounds(s), R, NULL);
    return pc_algo_apply(s, f, area, R, NULL, out, NULL);
}

static void test_half_selection_mixes_halfway(void)
{
    pc_surface s, sel, out;
    const float c[4] = {0.2f, 0.4f, 0.6f, 1.0f}, half = 0.5f;
    pc_pixel_format g = {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_F32, false};
    pc_algo_filter f = td_make(&TD_INVERT, NULL);
    float p[4];
    flat(&s, PC_SAMPLE_TYPE_F32, R, c);
    pc_surface_init_default(&sel, g, &half, 1);
    CHECK(pc_algo_apply(&s, &f, R, R, &sel, &out, NULL) == PC_OK);
    pc_surface_read_pixel(&out, 5, 5, p);
    CHECK(fabsf(p[0] - 0.5f) < 1e-5f && fabsf(p[2] - 0.5f) < 1e-5f);
    pc_surface_fini(&s), pc_surface_fini(&sel), pc_surface_fini(&out);
}

static void test_filters_are_tile_independent_and_respect_selection(void)
{
    td_offset o1 = {7, -3, PC_ALGO_UNDEF_WRAP};
    const pc_algo_filter_def *defs[4] = {&TD_INVERT, &TD_SOLARIZE, &TD_FIND_EDGES, &TD_OFFSET};
    const void *params[4] = {NULL, NULL, NULL, &o1};
    pc_surface s8, s16, none;
    int i;
    pattern(&s8, PC_SAMPLE_TYPE_U8, R);
    pattern(&s16, PC_SAMPLE_TYPE_U16, R);
    pc_surface_init(&none, PC_PIXEL_FORMAT_GRAY8);
    for (i = 0; i < 4; i++) {
        pc_algo_filter f = td_make(defs[i], params[i]);
        pc_rect area = pc_algo_output_area(&f, pc_surface_content_bounds(&s8), R, NULL);
        pc_surface a, b, out, masked;
        CHECK(pc_algo_apply_tiled(&s8, &f, area, R, NULL, 256, NULL, &a, NULL) == PC_OK);
        CHECK(pc_algo_apply_tiled(&s8, &f, area, R, NULL, 7, NULL, &b, NULL) == PC_OK);
        CHECK(regions_equal(&a, &b, pc_rect_union(area, R)));
        CHECK(run_filter(&s16, &f, &out) == PC_OK);
        CHECK(!pc_surface_eq(&out, &s16));
        area = pc_algo_output_area(&f, pc_surface_content_bounds(&s16), R, NULL);
        CHECK(pc_algo_apply(&s16, &f, area, R, &none, &masked, NULL) == PC_OK);
        CHECK(regions_equal(&masked, &s16, area));
        pc_surface_fini(&a), pc_surface_fini(&b), pc_surface_fini(&out), pc_surface_fini(&masked);
        pc_algo_filter_fini(&f);
    }
    pc_surface_fini(&s8), pc_surface_fini(&s16), pc_surface_fini(&none);
}

static void test_offset_wrap_repeat_transparent(void)
{
    pc_surface s, full, t, rp;
    td_offset a = {40, 30, PC_ALGO_UNDEF_WRAP}, b = {5, 0, PC_ALGO_UNDEF_TRANSPARENT},
              c = {5, 0, PC_ALGO_UNDEF_REPEAT};
    pc_algo_filter fa = td_make(&TD_OFFSET, &a), fb = td_make(&TD_OFFSET, &b), fc = td_make(&TD_OFFSET, &c);
    float p[4], q[4];
    pattern(&s, PC_SAMPLE_TYPE_U8, R);
    CHECK(run_filter(&s, &fa, &full) == PC_OK);
    CHECK(regions_equal(&full, &s, R));
    CHECK(run_filter(&s, &fb, &t) == PC_OK);
    CHECK(px_at(&t, 2, 3, 3) == 0.0f);
    pc_surface_read_pixel(&t, 15, 3, p);
    pc_surface_read_pixel(&s, 10, 3, q);
    CHECK(memcmp(p, q, sizeof p) == 0);
    CHECK(run_filter(&s, &fc, &rp) == PC_OK);
    pc_surface_read_pixel(&rp, 2, 3, p);
    pc_surface_read_pixel(&s, 0, 3, q);
    CHECK(memcmp(p, q, sizeof p) == 0);
    pc_surface_fini(&s), pc_surface_fini(&full), pc_surface_fini(&t), pc_surface_fini(&rp);
    pc_algo_filter_fini(&fa), pc_algo_filter_fini(&fb), pc_algo_filter_fini(&fc);
}

static void test_stylize_basics_find_edges_and_solarize(void)
{
    pc_surface s, fe, so;
    const float c[4] = {0.8f, 0.3f, 0.1f, 1.0f};
    pc_algo_filter f1 = td_make(&TD_FIND_EDGES, NULL), f2 = td_make(&TD_SOLARIZE, NULL);
    flat(&s, PC_SAMPLE_TYPE_F32, R, c);
    CHECK(run_filter(&s, &f1, &fe) == PC_OK);
    CHECK(fabsf(px_at(&fe, 20, 15, 0) - 1.0f) < 1e-6f);
    CHECK(run_filter(&s, &f2, &so) == PC_OK);
    CHECK(fabsf(px_at(&so, 3, 3, 0) - 0.2f) < 1e-6f && fabsf(px_at(&so, 3, 3, 1) - 0.3f) < 1e-6f);
    pc_surface_fini(&s), pc_surface_fini(&fe), pc_surface_fini(&so);
    pc_algo_filter_fini(&f1), pc_algo_filter_fini(&f2);
}

static void test_halos_and_output_area(void)
{
    td_radius g = {2.0f}, bx = {1.0f};
    td_twirl tw = {1.0f};
    pc_algo_filter fg = td_make(&TD_GAUSS, &g), ft = td_make(&TD_TWIRL, &tw), fi = td_make(&TD_INVERT, NULL);
    pc_algo_filter fb = td_make(&TD_BOX, &bx);
    pc_rect content = pc_rect_make(10, 10, 20, 20), sb = pc_rect_make(0, 0, 12, 12);
    CHECK(pc_algo_halo_eq(pc_algo_filter_halo(&fg), pc_algo_halo_radius(7)));
    CHECK(pc_algo_halo_eq(pc_algo_filter_halo(&ft), pc_algo_halo_bounds()));
    CHECK(pc_algo_filter_is_global(&ft) && !pc_algo_filter_is_global(&fg));
    CHECK(pc_rect_eq(pc_algo_output_area(&fi, content, R, NULL), content));
    CHECK(pc_rect_eq(pc_algo_output_area(&ft, content, R, NULL), R));
    CHECK(pc_rect_eq(pc_algo_output_area(&fb, content, R, &sb), pc_rect_make(8, 8, 12, 12)));
    CHECK(pc_rect_is_empty(pc_algo_output_area(&fi, PC_RECT_EMPTY, R, NULL)));
    CHECK_EQ_INT(pc_algo_auto_tile(&fg, R), 256);
    CHECK_EQ_STR(pc_algo_filter_label(&fg), "Gaussian Blur");
    pc_algo_filter_fini(&fg), pc_algo_filter_fini(&ft), pc_algo_filter_fini(&fi), pc_algo_filter_fini(&fb);
}

static pc_status parse(const char *text, pc_algo_filter *f, pc_error *err)
{
    yyjson_doc *d = NULL;
    pc_status st = pc_json_parse(text, strlen(text), &d, err);
    if (st == PC_OK) {
        st = pc_algo_filter_from_json_in(yyjson_doc_get_root(d), TD_ALL, f, err);
        yyjson_doc_free(d);
    }
    return st;
}

static void test_params_serde_defaults_and_roundtrip(void)
{
    pc_algo_filter f, back;
    pc_buf b;
    pc_error err;
    const td_radial *rp;
    const td_minmax *mp;
    CHECK(
        parse(
            "{\"filter\":\"radialBlur\",\"amount\":20.0,\"method\":\"spin\",\"centerX\":0.5,\"centerY\":0.5}",
            &f, &err) == PC_OK);
    rp = f.params;
    CHECK(rp->amount == 20.0f && rp->method == PC_ALGO_RADIAL_SPIN && rp->quality == PC_ALGO_RADIAL_GOOD);
    pc_algo_filter_fini(&f);
    CHECK(parse("{\"filter\":\"minimum\",\"radius\":3}", &f, &err) == PC_OK);
    mp = f.params;
    CHECK(mp->radius == 3.0f && mp->preserve == PC_ALGO_PRESERVE_SQUARENESS);
    pc_algo_filter_fini(&f);
    CHECK(parse("{\"filter\":\"maximum\",\"radius\":2,\"preserve\":\"roundness\"}", &f, &err) == PC_OK);
    mp = f.params;
    CHECK(mp->radius == 2.0f && mp->preserve == PC_ALGO_PRESERVE_ROUNDNESS);
    /* Round trip through text, and deep copies compare equal. */
    pc_buf_init(&b);
    CHECK(pc_algo_filter_to_json_text(&f, &b, NULL) == PC_OK);
    CHECK(parse(pc_buf_cstr(&b), &back, &err) == PC_OK);
    CHECK(pc_algo_filter_eq(&f, &back));
    pc_buf_fini(&b);
    pc_algo_filter_fini(&back);
    pc_algo_filter_fini(&f);
    CHECK(parse("{\"filter\":\"pathBlur\",\"paths\":[{},{\"points\":[[0,0],[1,1]],\"speed\":10}]}", &f,
                &err) == PC_OK);
    CHECK(pc_algo_filter_clone(&f, &back, NULL) == PC_OK);
    CHECK(pc_algo_filter_eq(&f, &back));
    CHECK(((const td_path *)back.params)->paths.v != ((const td_path *)f.params)->paths.v);
    pc_algo_filter_fini(&back);
    pc_algo_filter_fini(&f);
    /* A variant without a registered def is unsupported, not a crash; once registered, its missing
     * fields are reported. Either way nothing is returned. */
    CHECK(pc_algo_filter_from_json_str("{\"filter\":\"wave\"}", 17, &f, &err) ==
          (pc_algo_filter_def_find("wave") == NULL ? PC_ERR_UNSUPPORTED : PC_ERR_INVALID));
}

typedef struct prog {
    atomic_uint high;
    atomic_uint calls;
    bool cancel;
    bool bad;
} prog;

static bool cancel_cb(void *ctx)
{
    return ((prog *)ctx)->cancel;
}
static void progress_cb(void *ctx, float f)
{
    prog *p = ctx;
    uint32_t bits, cur;
    if (!(f >= 0.0f && f <= 1.0f)) {
        p->bad = true;
    }
    memcpy(&bits, &f, 4);
    cur = atomic_load(&p->high);
    while (bits > cur && !atomic_compare_exchange_weak(&p->high, &cur, bits)) {
    }
    atomic_fetch_add(&p->calls, 1);
}

static void test_cancelled_filters_and_progress(void)
{
    pc_surface s, out, ref;
    pc_algo_filter f = td_make(&TD_FIND_EDGES, NULL);
    pc_rect area;
    prog p;
    pc_interrupt yes = {cancel_cb, progress_cb, &p}, no = {cancel_cb, progress_cb, &p};
    uint32_t hb;
    float high;
    pattern(&s, PC_SAMPLE_TYPE_U8, R);
    area = pc_algo_output_area(&f, pc_surface_content_bounds(&s), R, NULL);
    atomic_init(&p.high, 0);
    atomic_init(&p.calls, 0);
    p.bad = false;
    p.cancel = true;
    CHECK(pc_algo_apply_tiled_with(&s, &f, area, R, NULL, 16, &R, &yes, &out, NULL) == PC_ERR_CANCELLED);
    p.cancel = false;
    CHECK(pc_algo_apply_tiled_with(&s, &f, area, R, NULL, 16, &R, &no, &out, NULL) == PC_OK);
    hb = atomic_load(&p.high);
    memcpy(&high, &hb, 4);
    CHECK(high == 1.0f);
    CHECK(atomic_load(&p.calls) >= 1);
    CHECK(!p.bad);
    CHECK(pc_algo_apply_tiled(&s, &f, area, R, NULL, 16, &R, &ref, NULL) == PC_OK);
    CHECK(regions_equal(&out, &ref, R));
    pc_surface_fini(&s), pc_surface_fini(&out), pc_surface_fini(&ref);
    pc_algo_filter_fini(&f);
}

static void test_layer_filters_repeat_the_canvas_edge(void)
{
    /* tests.rs layer_filters_repeat_the_canvas_edge with Find Edges: a flat opaque layer covering
     * the canvas has no edge at the canvas border when the edge repeats, and does without. */
    int st;
    for (st = 0; st < 3; st++) {
        pc_surface s, clamped, faded;
        const float c[4] = {0.2f, 0.6f, 0.4f, 1.0f};
        pc_algo_filter f = td_make(&TD_FIND_EDGES, NULL);
        pc_rect area;
        flat(&s, (pc_sample_type)st, R, c);
        area = pc_algo_output_area(&f, pc_surface_content_bounds(&s), R, NULL);
        CHECK(pc_algo_apply_in(&s, &f, area, R, NULL, R, &clamped, NULL) == PC_OK);
        CHECK(pc_rect_eq(pc_surface_content_bounds(&clamped), R));
        CHECK(fabsf(px_at(&clamped, 0, 0, 1) - 1.0f) < 1e-6f && px_at(&clamped, 39, 29, 3) == 1.0f);
        CHECK(pc_algo_apply(&s, &f, area, R, NULL, &faded, NULL) == PC_OK);
        CHECK(px_at(&faded, 0, 0, 1) < 0.9f);
        pc_surface_fini(&s), pc_surface_fini(&clamped), pc_surface_fini(&faded);
        pc_algo_filter_fini(&f);
    }
}

/* ---- never crash ---- */

static void test_hostile_parameters_never_crash(void)
{
    pc_surface s, o;
    pc_error err;
    float b[16] = {0};
    size_t r3[3];
    pc_algo_level empty = {0, 0, NULL};
    float *x = NULL;
    pattern(&s, PC_SAMPLE_TYPE_U8, R);
    /* Absurd resample scales. */
    CHECK(pc_algo_resize_surface(&s, INFINITY, 1.0, PC_ALGO_RESAMPLE_BICUBIC, &o, &err) == PC_ERR_LIMIT);
    CHECK(pc_algo_resize_surface(&s, 1e12, 1e12, PC_ALGO_RESAMPLE_LANCZOS, &o, &err) == PC_ERR_LIMIT);
    /* One output pixel would need 2^32 taps (the reference tries to allocate them). */
    CHECK(pc_algo_resize_surface(&s, 1e-300, 1e-300, PC_ALGO_RESAMPLE_LANCZOS, &o, &err) == PC_ERR_LIMIT);
    CHECK(pc_algo_resize_surface(&s, NAN, NAN, PC_ALGO_RESAMPLE_BILINEAR, &o, &err) == PC_OK);
    CHECK(pc_rect_is_empty(pc_surface_content_bounds(&o)));
    pc_surface_fini(&o);
    CHECK(pc_algo_resize_surface(&s, 1e-9, 1e-9, (pc_algo_resample)99, &o, &err) == PC_ERR_LIMIT);
    CHECK(pc_algo_resize_surface(&s, 1e-6, 1e-6, PC_ALGO_RESAMPLE_BILINEAR, &o, &err) == PC_OK);
    pc_surface_fini(&o);
    /* Box blur radii beyond any sensible size, NaN sigma, empty buffers. */
    CHECK(pc_algo_box_blur_n(b, 4, 4, 1, (size_t)1 << 40, &err) == PC_ERR_LIMIT);
    CHECK(pc_algo_gauss_blur_n(b, 4, 4, 1, 1e30f, &err) == PC_ERR_LIMIT);
    CHECK(pc_algo_gauss_blur_n(b, 4, 4, 1, NAN, &err) == PC_OK);
    CHECK(pc_algo_gauss_blur_n(NULL, 0, 0, 1, 3.0f, &err) == PC_OK);
    pc_algo_gauss_box_radii(INFINITY, r3);
    pc_algo_gauss_box_radii(-INFINITY, r3);
    /* Warps through degenerate and enormous homographies. */
    {
        pc_algo_homography hs[4] = {{{NAN, 0, 0, 0, 1, 0, 0, 0, 1}},
                                    {{1e300, 0, 0, 0, 1e300, 0, 0, 0, 1}},
                                    {{1e-300, 0, 0, 0, 1e-300, 0, 0, 0, 1}},
                                    {{1, 0, 0, 0, 1, 0, 1e-3, 1e-3, 1}}};
        int i, it;
        for (i = 0; i < 4; i++) {
            for (it = 0; it < 3; it++) {
                pc_status st = pc_algo_warp_surface(&s, R, &hs[i], (pc_algo_interp)it, &o, &err);
                CHECK(st == PC_OK || st == PC_ERR_LIMIT || st == PC_ERR_NOMEM);
                if (st == PC_OK) {
                    pc_surface_fini(&o);
                }
            }
        }
    }
    /* Pyramids of empty levels, tone with mismatched sizes. */
    CHECK(pc_algo_pyramid_expand(&empty, 1, 4, 4, &x, &err) == PC_ERR_INVALID);
    CHECK(pc_algo_pyramid_expand(&empty, 1, 0, 0, &x, &err) == PC_OK);
    free(x);
    {
        float px[4][4] = {{0}};
        pc_algo_shadows_highlights_params p = pc_algo_shadows_highlights_default();
        pc_algo_hdr_toning_params h = pc_algo_hdr_toning_default();
        float *wts[2];
        const float *none[1] = {NULL};
        CHECK(pc_algo_shadows_highlights(px, 4, 3, 3, &p, &err) == PC_ERR_INVALID);
        CHECK(pc_algo_hdr_toning(px, 4, 5, 1, &h, &err) == PC_ERR_INVALID);
        CHECK(pc_algo_shadows_highlights(px, 0, 0, 0, &p, &err) == PC_OK);
        p.shadow_radius = NAN, p.black_clip = NAN, p.shadow_tone = NAN;
        CHECK(pc_algo_shadows_highlights(px, 4, 2, 2, &p, &err) == PC_OK);
        /* An infinite blur radius would take the reference forever. */
        p.highlight_radius = INFINITY, p.highlight_amount = 50.0f;
        CHECK(pc_algo_shadows_highlights(px, 4, 2, 2, &p, &err) == PC_ERR_LIMIT);
        h.radius = NAN, h.gamma = NAN, h.exposure = INFINITY;
        CHECK(pc_algo_hdr_toning(px, 4, 2, 2, &h, &err) == PC_OK);
        CHECK(pc_algo_stack_weights(0, 5, none, none, 0, wts, &err) == PC_OK);
        CHECK(pc_algo_panorama_weights(0, 0, none, 0, wts, &err) == PC_OK);
    }
    /* The driver: no kernel, zero and negative tiles, empty areas. */
    {
        td_radius g = {2.0f};
        pc_algo_filter nk = td_make(&TD_GAUSS, &g), inv = td_make(&TD_INVERT, NULL);
        CHECK(pc_algo_apply(&s, &nk, R, R, NULL, &o, &err) == PC_ERR_UNSUPPORTED);
        CHECK(pc_algo_apply_tiled(&s, &inv, R, R, NULL, 0, NULL, &o, &err) == PC_OK);
        pc_surface_fini(&o);
        CHECK(pc_algo_apply_tiled(&s, &inv, R, R, NULL, -5, NULL, &o, &err) == PC_OK);
        pc_surface_fini(&o);
        CHECK(pc_algo_apply(&s, &inv, PC_RECT_EMPTY, R, NULL, &o, &err) == PC_OK);
        CHECK(pc_surface_eq(&o, &s));
        pc_surface_fini(&o);
        CHECK(pc_algo_apply(&s, NULL, R, R, NULL, &o, &err) == PC_ERR_INVALID);
        pc_algo_filter_fini(&nk), pc_algo_filter_fini(&inv);
    }
    /* Hostile JSON. */
    {
        pc_algo_filter f;
        static const char *const bad[] = {
            "{\"filter\":\"gaussianBlur\",\"radius\":1e999}",
            "{\"filter\":\"lightingEffects\",\"lights\":[[1,2]]}",
            "[]",
            "[\"invert\"]",
            "{\"filter\":\"shear\",\"points\":[[1,2,3]],\"undefined\":\"wrap\"}",
            "{\"filter\":\"custom\",\"kernel\":{},\"scale\":1,\"offset\":0}"};
        size_t i;
        for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            if (parse(bad[i], &f, &err) == PC_OK) {
                pc_algo_filter_fini(&f);
            }
        }
        CHECK(parse("[\"invert\"]", &f, &err) == PC_ERR_UNSUPPORTED);
        CHECK(parse("[]", &f, &err) == PC_ERR_INVALID);
        CHECK_EQ_STR(err.msg, "missing field `filter`");
    }
    /* Image accessors outside the data. */
    {
        pc_algo_image img;
        float out[2];
        CHECK(pc_algo_image_read(&s, pc_rect_make(0, 0, 2, 2), &img, NULL) == PC_OK);
        CHECK(pc_algo_image_get(&img, 0, 0, 99) == 0.0f);
        CHECK(pc_algo_image_px(&img, 5, 5) == NULL);
        CHECK(pc_algo_image_row(&img, pc_rect_make(0, 0, 9, 1)) == NULL);
        pc_algo_image_sample(&img, NAN, INFINITY, PC_ALGO_EDGE_WRAP,
                             pc_rect_make(INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX), true, out, 2);
        CHECK(pc_algo_image_get_edge(&img, INT32_MIN, INT32_MAX, 0, PC_ALGO_EDGE_WRAP,
                                     pc_rect_make(INT32_MIN, 0, INT32_MAX, 5)) >= 0.0f);
        pc_algo_image_fini(&img);
    }
    pc_surface_fini(&s);
}

int main(int argc, char **argv)
{
    RUN(test_exact_bins_and_channel_totals);
    RUN(test_empty_transparent_invalid_and_hdr_are_explicit);
    RUN(test_clipping_indicators_use_exact_samples);
    RUN(test_rgb_and_gray_at_all_depths);
    RUN(test_exact_clipping_endpoints);
    RUN(test_sh_grey_matches_photoshop);
    RUN(test_sh_colour_matches_photoshop);
    RUN(test_sh_edge_matches_photoshop);
    RUN(test_sh_strength_and_curve_follow_photoshop);
    RUN(test_sh_tonal_mask_spans_photoshop_tones);
    RUN(test_replace_color_only_touches_matching_pixels);
    RUN(test_match_color_moves_statistics_toward_source);
    RUN(test_hdr_toning_compresses_range_and_keeps_bounds);
    RUN(test_horizontal_matches_direct_sum);
    RUN(test_identity_scale_is_lossless);
    RUN(test_resample_matches_photoshop_image_size);
    RUN(test_bicubic_is_photoshops_keys_cubic);
    RUN(test_half_and_double_sizes);
    RUN(test_downscale_averages_checkerboard);
    RUN(test_transparent_edges_do_not_darken);
    RUN(test_canvas_edges_repeat_instead_of_fading);
    RUN(test_tiny_target_of_a_huge_sparse_source);
    RUN(test_skipped_empty_rows_match_reading_them);
    RUN(test_chunked_reads_match_a_single_read);
    RUN(test_masks_keep_default_pixel);
    RUN(test_translate_and_crop);
    RUN(test_nearest_neighbor_reduction_does_not_average);
    RUN(test_homography_maps_rect_corners_and_inverts);
    RUN(test_identity_and_translation_are_exact);
    RUN(test_scale_doubles_size_and_rotation_keeps_area);
    RUN(test_large_downscale_is_prefiltered);
    RUN(test_surfaces_without_alpha_gain_it);
    RUN(test_laplacian_round_trip_is_exact);
    RUN(test_blend_with_one_hot_weights_reproduces_each_side);
    RUN(test_stack_picks_the_sharp_image);
    RUN(test_panorama_splits_the_overlap);
    RUN(test_half_selection_mixes_halfway);
    RUN(test_filters_are_tile_independent_and_respect_selection);
    RUN(test_offset_wrap_repeat_transparent);
    RUN(test_stylize_basics_find_edges_and_solarize);
    RUN(test_halos_and_output_area);
    RUN(test_params_serde_defaults_and_roundtrip);
    RUN(test_cancelled_filters_and_progress);
    RUN(test_layer_filters_repeat_the_canvas_edge);
    RUN(test_hostile_parameters_never_crash);
    return pc_t_finish();
}
