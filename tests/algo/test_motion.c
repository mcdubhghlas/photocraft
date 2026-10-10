/* Motion Blur: the Rust #[test]s of blur/motion_tests.rs, blur/motion_fft_tests.rs and
 * blur/motion_apply.rs (the row kernel, the FFT convolution against an independent f64 oracle,
 * and the bounded FFT scheduler). motion_bench.rs (release timings) is not ported. */
#include <float.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "algo/blur_int.h"
#include "base/num.h"
#include "base/rmath.h"
#include "harness.h"
#include "raster/pixel.h"

/* ---- helpers ---- */

static void image_new(pc_algo_image *img, pc_rect r, size_t ch)
{
    CHECK(pc_algo_image_init(img, r, ch, NULL) == PC_OK);
}

/* motion_fft_tests::pattern. */
static void fft_pattern(pc_algo_image *img, pc_rect r, size_t ch, bool alpha)
{
    static const float a4[4] = {0.0f, 0.25f, 0.5f, 1.0f};
    size_t w = pc_rect_width(r), h = pc_rect_height(r), x, y, c, k = 0;
    image_new(img, r, ch);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            for (c = 0; c < ch; c++) {
                img->data[k++] = alpha && c + 1 == ch ? a4[(x + y * 3) % 4]
                                                      : (float)((x * 31 + y * 17 + c * 43) % 251) / 250.0f;
            }
        }
    }
}

/* motion_tests::patterned_image. */
static void row_pattern(pc_algo_image *img, pc_rect r, size_t ch, bool alpha)
{
    static const float a4[4] = {0.0f, 1.0f, 0.125f, 0.875f};
    size_t w = pc_rect_width(r), h = pc_rect_height(r), x, y, c, k = 0;
    image_new(img, r, ch);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            for (c = 0; c < ch; c++) {
                size_t q = x * 37 + y * 13 + c * 29;
                img->data[k++] = alpha && c + 1 == ch ? a4[q % 4] : (float)(q % 256) / 255.0f;
            }
        }
    }
}

static float *rows_k(const pc_algo_image *src, pc_rect out, bool alpha, pc_color_mode mode, float angle,
                     float distance)
{
    pc_algo_ctx ctx = {src->rect, mode, alpha};
    size_t n = (size_t)pc_rect_width(out) * pc_rect_height(out) * src->ch;
    float *d = calloc(n + 1, sizeof(float));
    CHECK(pc_algo_motion_k(src, out, &ctx, angle, distance, d, NULL) == PC_OK);
    return d;
}

static float *fft(const pc_algo_image *src, pc_rect out, bool alpha, float angle, float distance, size_t *len)
{
    float *d = NULL;
    bool ok = pc_algo_motion_fft_filter(src, out, alpha, angle, distance, &d, len);
    CHECK(ok);
    if (!ok) {
        *len = (size_t)pc_rect_width(out) * pc_rect_height(out) * src->ch;
        d = calloc(*len + 1, sizeof(float));
    }
    return d;
}

/* The independent oracle: merged f32 taps, f64 accumulation. */
static double *ideal(const pc_algo_image *src, pc_rect out, bool alpha, float angle, float distance)
{
    pc_algo_motion_tap *t;
    size_t nt, i, c, k = 0, n = src->ch;
    double *res = calloc((size_t)pc_rect_width(out) * pc_rect_height(out) * n + 1, sizeof(double));
    int32_t x, y;
    CHECK(pc_algo_motion_taps(angle, distance, &t, &nt, NULL) == PC_OK);
    for (y = out.y0; y < out.y1; y++) {
        for (x = out.x0; x < out.x1; x++) {
            double *acc = res + k;
            for (i = 0; i < nt; i++) {
                int32_t sx = x + t[i].x, sy = y + t[i].y;
                float a = alpha ? pc_algo_image_get(src, sx, sy, n - 1) : 1.0f;
                for (c = 0; c < n; c++) {
                    float s = pc_algo_image_get(src, sx, sy, c);
                    float pm = alpha && c + 1 < n ? s * a : s;
                    acc[c] += (double)pm * (double)t[i].w;
                }
            }
            if (alpha) {
                double a = acc[n - 1];
                for (c = 0; c + 1 < n; c++) {
                    acc[c] = (float)a > 1e-7f ? acc[c] / a : 0.0;
                }
            }
            k += n;
        }
    }
    free(t);
    return res;
}

static bool assert_ideal(const float *actual, const double *expected, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        float rounded = (float)expected[i];
        double tol = 1e-9 + 2.0 * (double)FLT_EPSILON * fabs((double)rounded);
        if (!(fabs((double)actual[i] - (double)rounded) <= tol)) {
            PC_T_FAIL("sample %zu: actual %.9g ideal %.9g tolerance %g", i, (double)actual[i], expected[i],
                      tol);
            return false;
        }
    }
    return true;
}

static bool bits_equal(const float *a, const float *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (memcmp(&a[i], &b[i], 4) != 0 && !(isnan(a[i]) && isnan(b[i]))) {
            PC_T_FAIL("sample %zu: %.9g != %.9g", i, (double)a[i], (double)b[i]);
            return false;
        }
    }
    return true;
}

/* ---- motion_tests.rs ---- */

static void test_motion_identity_preserves_signed_zero_and_transparent_colours(void)
{
    pc_rect rect = pc_rect_make(-1, -1, 2, 2);
    pc_algo_image src;
    const float px[4] = {-0.0f, 8.0f, -2.0f, 0.0f};
    const float dists[4] = {-0.49f, -0.0f, 0.0f, 0.49f};
    float *out;
    size_t i;
    image_new(&src, rect, 4);
    for (i = 0; i < 36; i++) {
        src.data[i] = px[i % 4];
    }
    for (i = 0; i < 4; i++) {
        out = rows_k(&src, rect, true, PC_COLOR_MODE_RGB, 17.0f, dists[i]);
        CHECK(bits_equal(out, src.data, 36));
        free(out);
    }
    out = rows_k(&src, rect, true, PC_COLOR_MODE_RGB, 31.0f, 2.0f);
    for (i = 0; i < 36; i++) {
        CHECK(out[i] == 0.0f);
    }
    free(out);
    pc_algo_image_fini(&src);
}

static void test_motion_default_kernel_retains_merged_taps(void)
{
    /* The reference compares the default kernel with itself under another name; here it is held
     * to an independent tap-by-tap f32 accumulation in the same order (bit for bit). */
    static const int32_t origins[4][2] = {{0, 0}, {-17, 31}, {16000, -16000}, {1 << 20, -(1 << 20)}};
    static const float cases[6][2] = {{0, 1}, {90, 2.5f}, {30, 10}, {-45, -63.5f}, {89.75f, 64}, {37, 127}};
    int o, k;
    size_t ch;
    for (o = 0; o < 4; o++) {
        int32_t x0 = origins[o][0], y0 = origins[o][1];
        pc_rect rect = pc_rect_make(x0, y0, x0 + 11, y0 + 9),
                out = pc_rect_make(x0 + 2, y0 + 1, x0 + 7, y0 + 6);
        for (ch = 1; ch <= 6; ch++) {
            int al;
            for (al = 0; al < 2; al++) {
                pc_algo_image src;
                row_pattern(&src, rect, ch, al != 0);
                for (k = 0; k < 6; k++) {
                    float *got =
                        rows_k(&src, out, al != 0, PC_COLOR_MODE_MULTICHANNEL, cases[k][0], cases[k][1]);
                    pc_algo_motion_tap *t;
                    size_t nt, n = 25 * ch, i, c, q = 0;
                    float *want = calloc(n, sizeof(float));
                    int32_t x, y;
                    CHECK(pc_algo_motion_taps(cases[k][0], cases[k][1], &t, &nt, NULL) == PC_OK);
                    for (y = out.y0; y < out.y1; y++) {
                        for (x = out.x0; x < out.x1; x++, q += ch) {
                            for (i = 0; i < nt; i++) {
                                float a = al ? pc_algo_image_get(&src, x + t[i].x, y + t[i].y, ch - 1) : 1.0f;
                                for (c = 0; c < ch; c++) {
                                    float v = pc_algo_image_get(&src, x + t[i].x, y + t[i].y, c);
                                    want[q + c] += t[i].w * (al && c + 1 < ch ? v * a : v);
                                }
                            }
                            if (al) {
                                float a = want[q + ch - 1];
                                for (c = 0; c + 1 < ch; c++) {
                                    want[q + c] = a > 1e-7f ? want[q + c] / a : 0.0f;
                                }
                            }
                        }
                    }
                    CHECK(bits_equal(got, want, n));
                    free(t), free(got), free(want);
                }
                pc_algo_image_fini(&src);
            }
        }
    }
}

/* row_surface: the row kernel over the whole area, mixed and written into a clone. */
static void row_surface(const pc_surface *s, const pc_algo_motion_params *p, pc_rect area,
                        const pc_surface *sel, const pc_rect *extent, pc_surface *out)
{
    pc_algo_halo h =
        pc_algo_halo_radius(pc_i32_wrapping_add(pc_f32_to_i32(ceilf(fabsf(p->distance) / 2.0f)), 2));
    pc_rect grown = pc_rect_inflate(area, h.radius);
    pc_algo_image src;
    pc_pixel_format f = pc_surface_format(s);
    size_t len;
    float *d;
    CHECK((extent != NULL ? pc_algo_image_read_clamped(s, grown, *extent, &src, NULL)
                          : pc_algo_image_read(s, grown, &src, NULL)) == PC_OK);
    d = rows_k(&src, area, f.alpha, f.mode, p->angle, p->distance);
    if (sel != NULL) {
        pc_algo_mix_selection(d, area, sel, &src);
    }
    CHECK(pc_surface_clone(s, out, NULL) == PC_OK);
    (void)pc_algo_float_count(area, src.ch, &len);
    CHECK(pc_surface_write_region(out, area, d, len, NULL) == PC_OK);
    pc_surface_prune(out);
    free(d);
    pc_algo_image_fini(&src);
}

static bool surfaces_equal_in(const pc_surface *a, const pc_surface *b, pc_rect r)
{
    float *x = NULL, *y = NULL;
    size_t lx = 0, ly = 0;
    bool eq;
    CHECK(pc_surface_read_region(a, r, &x, &lx, NULL) == PC_OK);
    CHECK(pc_surface_read_region(b, r, &y, &ly, NULL) == PC_OK);
    eq = lx == ly && bits_equal(x, y, lx);
    free(x), free(y);
    return eq;
}

static const pc_color_mode MODES[4] = {PC_COLOR_MODE_GRAYSCALE, PC_COLOR_MODE_RGB, PC_COLOR_MODE_CMYK,
                                       PC_COLOR_MODE_LAB};

static void test_motion_pipeline_matches_rows_across_formats_edges_selections_and_tiles(void)
{
    static const float covs[5] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    static const pc_algo_motion_params params[3] = {{0.0f, 2.5f}, {32.0f, 11.0f}, {-90.0f, -7.25f}};
    static const int32_t tiles[3] = {5, 17, 256};
    pc_rect rect = pc_rect_make(-13, 9, 16, 30);
    pc_surface sel;
    size_t w = pc_rect_width(rect), h = pc_rect_height(rect), i;
    float *cov = malloc(w * h * sizeof(float));
    int m, st, al, pi, e, s, t;
    for (i = 0; i < w * h; i++) {
        cov[i] = covs[(i % w + (i / w) * 3) % 5];
    }
    pc_surface_init(&sel, PC_PIXEL_FORMAT_GRAY8);
    CHECK(pc_surface_write_region(&sel, rect, cov, w * h, NULL) == PC_OK);
    free(cov);
    for (m = 0; m < 4; m++) {
        for (st = 0; st < 3; st++) {
            for (al = 0; al < 2; al++) {
                pc_pixel_format f = {MODES[m], (pc_sample_type)st, al != 0};
                pc_surface src;
                pc_algo_image img;
                size_t ch = pc_pixel_format_channels(f), k;
                row_pattern(&img, rect, ch, al != 0);
                if (st == PC_SAMPLE_TYPE_F32) {
                    for (k = 0; k < img.len; k++) {
                        if (!(al && k % ch == ch - 1)) {
                            img.data[k] = img.data[k] * 4.0f - 1.0f;
                        }
                    }
                }
                pc_surface_init(&src, f);
                CHECK(pc_surface_write_region(&src, rect, img.data, img.len, NULL) == PC_OK);
                pc_algo_image_fini(&img);
                for (pi = 0; pi < 3; pi++) {
                    pc_algo_filter flt;
                    CHECK(pc_algo_filter_make(&PC_ALGO_FILTER_MOTION_BLUR, &params[pi], &flt, NULL) == PC_OK);
                    for (e = 0; e < 2; e++) {
                        for (s = 0; s < 2; s++) {
                            pc_surface want;
                            row_surface(&src, &params[pi], rect, s ? &sel : NULL, e ? &rect : NULL, &want);
                            for (t = 0; t < 3; t++) {
                                pc_surface got;
                                CHECK(pc_algo_apply_tiled(&src, &flt, rect, rect, s ? &sel : NULL, tiles[t],
                                                          e ? &rect : NULL, &got, NULL) == PC_OK);
                                CHECK(surfaces_equal_in(&got, &want, pc_rect_inflate(rect, 2)));
                                pc_surface_fini(&got);
                            }
                            pc_surface_fini(&want);
                        }
                    }
                    pc_algo_filter_fini(&flt);
                }
                pc_surface_fini(&src);
            }
        }
    }
    pc_surface_fini(&sel);
}

/* ---- motion_fft_tests.rs ---- */

static void test_motion_fft_matches_independent_ideal_for_channels_angles_and_distances(void)
{
    static const float cases[6][2] = {{0, 64},     {90, 65.5f},    {30, 127},
                                      {-45, -256}, {89.75f, 2000}, {37, 2000}};
    pc_rect rect = pc_rect_make(-4, -4, 5, 5), out = pc_rect_make(-1, -1, 2, 2);
    size_t ch, len;
    int al, k;
    for (ch = 1; ch <= 5; ch++) {
        for (al = 0; al < 2; al++) {
            pc_algo_image src;
            fft_pattern(&src, rect, ch, al != 0);
            for (k = 0; k < 6; k++) {
                float *a = fft(&src, out, al != 0, cases[k][0], cases[k][1], &len);
                double *e = ideal(&src, out, al != 0, cases[k][0], cases[k][1]);
                CHECK(len == 9 * ch && assert_ideal(a, e, len));
                free(a), free(e);
            }
            pc_algo_image_fini(&src);
        }
    }
}

/* Mass, centroid and directional widths of a set of weighted points. */
static void moments(const double *w, const int32_t (*xy)[2], size_t n, float angle, double out[5])
{
    double a = (double)angle * 3.14159265358979323846 / 180.0, s = sin(a), c = cos(a);
    size_t i;
    memset(out, 0, 5 * sizeof(double));
    for (i = 0; i < n; i++) {
        double x = xy[i][0], y = xy[i][1], u = x * s + y * c, v = x * c - y * s;
        out[0] += w[i];
        out[1] += x * w[i];
        out[2] += y * w[i];
        out[3] += u * u * w[i];
        out[4] += v * v * w[i];
    }
}

static void test_motion_fft_impulse_preserves_mass_centroid_and_directional_width(void)
{
    static const float dists[2] = {64.0f, 127.0f}, angles[4] = {0.0f, 30.0f, 45.0f, 89.75f};
    pc_algo_image src;
    int d, a;
    image_new(&src, pc_rect_make(0, 0, 1, 1), 1);
    src.data[0] = 1.0f;
    for (d = 0; d < 2; d++) {
        int32_t reach = (int32_t)ceilf(dists[d] / 2.0f) + 2;
        pc_rect out = pc_rect_make(-reach, -reach, reach + 1, reach + 1);
        size_t w = pc_rect_width(out), n = w * pc_rect_height(out), i, len, nt;
        for (a = 0; a < 4; a++) {
            float *act = fft(&src, out, false, angles[a], dists[d], &len);
            double *aw = malloc(n * sizeof(double)), *ew = calloc(n, sizeof(double)), am[5], em[5];
            int32_t (*xy)[2] = malloc(n * sizeof *xy);
            pc_algo_motion_tap *t;
            CHECK(pc_algo_motion_taps(angles[a], dists[d], &t, &nt, NULL) == PC_OK);
            for (i = 0; i < n; i++) {
                xy[i][0] = out.x0 + (int32_t)(i % w);
                xy[i][1] = out.y0 + (int32_t)(i / w);
                aw[i] = (double)act[i];
            }
            for (i = 0; i < nt; i++) {
                /* The impulse response is the kernel reflected. */
                int32_t x = -t[i].x, y = -t[i].y;
                if (pc_rect_contains(out, x, y)) {
                    ew[(size_t)(y - out.y0) * w + (size_t)(x - out.x0)] += (double)t[i].w;
                }
            }
            moments(aw, (const int32_t (*)[2])xy, n, angles[a], am);
            moments(ew, (const int32_t (*)[2])xy, n, angles[a], em);
            CHECK(fabs(am[0] - 1.0) <= 3e-6);
            for (i = 1; i <= 3; i++) {
                CHECK(fabs(am[i] - em[i]) <= 2e-5);
            }
            CHECK(fabs(am[4] - em[4]) <= 5e-4);
            CHECK(assert_ideal(act, ew, n));
            free(act), free(aw), free(ew), free(xy), free(t);
        }
    }
    pc_algo_image_fini(&src);
}

static void test_motion_fft_sharp_stripes_match_uniform_streak(void)
{
    static const float angles[3] = {0.0f, 30.0f, 45.0f};
    static const double freqs[2] = {0.125, 0.375};
    pc_rect rect = pc_rect_make(-80, -80, 81, 81), out = pc_rect_make(-2, -2, 3, 3);
    int a, p, f;
    for (a = 0; a < 3; a++) {
        double rad = (double)angles[a] * 3.14159265358979323846 / 180.0, s = sin(rad), c = cos(rad);
        for (p = 0; p < 2; p++) {
            for (f = 0; f < 2; f++) {
                pc_algo_image src;
                size_t i, len, w = pc_rect_width(rect);
                float dist = p ? 127.0f : 64.0f, *act;
                double *e;
                image_new(&src, rect, 1);
                for (i = 0; i < src.len; i++) {
                    double x = rect.x0 + (int32_t)(i % w), y = rect.y0 + (int32_t)(i / w);
                    double coord = p ? x * s + y * c : x * c - y * s;
                    src.data[i] = (float)(0.5 + 0.5 * cos(6.283185307179586 * freqs[f] * coord));
                }
                act = fft(&src, out, false, angles[a], dist, &len);
                e = ideal(&src, out, false, angles[a], dist);
                CHECK(assert_ideal(act, e, len));
                free(act), free(e);
                pc_algo_image_fini(&src);
            }
        }
    }
}

static void test_motion_fft_premultiplied_alpha_hidden_colour_hdr_and_cutoff(void)
{
    pc_rect rect = pc_rect_make(-5, -5, 6, 6), out = pc_rect_make(-1, -1, 2, 2),
            big = pc_rect_make(-40, -40, 41, 41);
    pc_algo_image src, hidden;
    float *a, *b, alphas[6];
    double *e, *ad;
    size_t i, len, k;
    uint32_t bits;
    fft_pattern(&src, rect, 4, true);
    fft_pattern(&hidden, rect, 4, true);
    for (i = 0; i < src.len; i += 4) {
        for (k = 0; k < 3; k++) {
            src.data[i + k] = src.data[i + k] * 32.0f - 8.0f;
            hidden.data[i + k] = src.data[i + k];
        }
        if (src.data[i + 3] == 0.0f) {
            hidden.data[i] = 1e6f, hidden.data[i + 1] = 7e5f, hidden.data[i + 2] = 9e5f;
        }
    }
    a = fft(&src, out, true, 38.0f, 67.75f, &len);
    e = ideal(&src, out, true, 38.0f, 67.75f);
    CHECK(assert_ideal(a, e, len));
    b = fft(&hidden, out, true, 38.0f, 67.75f, &len);
    ad = malloc(len * sizeof(double));
    for (i = 0; i < len; i++) {
        ad[i] = (double)a[i];
    }
    CHECK(assert_ideal(b, ad, len));
    free(a), free(b), free(e), free(ad);
    pc_algo_image_fini(&src), pc_algo_image_fini(&hidden);

    alphas[0] = 0.0f, alphas[1] = 1e-8f, alphas[2] = 1e-7f;
    memcpy(&bits, &alphas[2], 4);
    bits++;
    memcpy(&alphas[3], &bits, 4);
    alphas[4] = 0.25f, alphas[5] = 1.0f;
    for (k = 0; k < 6; k++) {
        image_new(&src, big, 4);
        for (i = 0; i < src.len; i += 4) {
            src.data[i] = 0.25f, src.data[i + 1] = 0.5f, src.data[i + 2] = 0.75f, src.data[i + 3] = alphas[k];
        }
        a = fft(&src, out, true, 30.0f, 64.0f, &len);
        if (alphas[k] >= 0.99e-7f && alphas[k] <= 1.01e-7f) {
            /* Near the colour cutoff the FFT path keeps the row kernel's own sums. */
            b = rows_k(&src, out, true, PC_COLOR_MODE_RGB, 30.0f, 64.0f);
            CHECK(bits_equal(a, b, len));
            free(b);
        } else {
            e = ideal(&src, out, true, 30.0f, 64.0f);
            CHECK(assert_ideal(a, e, len));
            free(e);
            if (alphas[k] <= 1e-7f) {
                for (i = 0; i < len; i += 4) {
                    CHECK(a[i] == 0.0f && a[i + 1] == 0.0f && a[i + 2] == 0.0f);
                }
            }
        }
        free(a);
        pc_algo_image_fini(&src);
    }
}

static void test_motion_fft_summation_error_is_bounded_in_supported_coordinates(void)
{
    static const int32_t origins[3] = {-16384, 0, 16375};
    static const float cases[2][2] = {{30.0f, 64.0f}, {89.75f, 127.0f}};
    int o, al, k;
    for (o = 0; o < 3; o++) {
        int32_t g = origins[o];
        pc_rect rect = pc_rect_make(g, g, g + 9, g + 9), out = pc_rect_make(g + 2, g + 2, g + 5, g + 5);
        for (al = 0; al < 2; al++) {
            pc_algo_image src;
            fft_pattern(&src, rect, 4, al != 0);
            for (k = 0; k < 2; k++) {
                size_t len, i;
                float *a = fft(&src, out, al != 0, cases[k][0], cases[k][1], &len);
                float *b = rows_k(&src, out, al != 0, PC_COLOR_MODE_RGB, cases[k][0], cases[k][1]);
                for (i = 0; i < len; i++) {
                    CHECK(fabsf(a[i] - b[i]) <= 1e-3f);
                }
                free(a), free(b);
            }
            pc_algo_image_fini(&src);
        }
    }
}

static void test_motion_fft_large_hdr_colours_do_not_change_small_alpha_or_cutoff(void)
{
    pc_rect rect = pc_rect_make(-40, -40, 41, 41), out = pc_rect_make(-1, -1, 2, 2);
    float alphas[3];
    uint32_t bits;
    int k;
    alphas[0] = 1e-7f;
    memcpy(&bits, &alphas[0], 4);
    bits++;
    memcpy(&alphas[1], &bits, 4);
    alphas[2] = 0.25f;
    for (k = 0; k < 3; k++) {
        pc_algo_image src;
        size_t i, len;
        float *a;
        image_new(&src, rect, 4);
        for (i = 0; i < src.len; i += 4) {
            src.data[i] = 1e10f, src.data[i + 1] = -5e9f, src.data[i + 2] = 9e9f, src.data[i + 3] = alphas[k];
        }
        a = fft(&src, out, true, 33.0f, 64.0f, &len);
        if (alphas[k] >= 0.99e-7f && alphas[k] <= 1.01e-7f) {
            float *b = rows_k(&src, out, true, PC_COLOR_MODE_RGB, 33.0f, 64.0f);
            CHECK(bits_equal(a, b, len));
            free(b);
        } else {
            double *e = ideal(&src, out, true, 33.0f, 64.0f);
            CHECK(assert_ideal(a, e, len));
            free(e);
        }
        free(a);
        pc_algo_image_fini(&src);
    }
}

static void test_motion_fft_pipeline_supports_depths_models_edges_selections_and_seams(void)
{
    static const float covs[4] = {0.0f, 0.25f, 0.5f, 1.0f};
    static const int32_t tiles[3] = {5, 17, 256};
    pc_rect rect = pc_rect_make(-6, 3, 7, 16);
    pc_algo_motion_params mp = {31.0f, 73.25f};
    int32_t reach = pc_f32_to_i32(ceilf(73.25f / 2.0f)) + 2;
    size_t w = pc_rect_width(rect), h = pc_rect_height(rect), i;
    float *cov = malloc(w * h * sizeof(float));
    pc_surface sel;
    pc_algo_filter flt;
    int m, st, al, e, s, t;
    for (i = 0; i < w * h; i++) {
        cov[i] = covs[i % 4];
    }
    pc_surface_init(&sel, PC_PIXEL_FORMAT_GRAY8);
    CHECK(pc_surface_write_region(&sel, rect, cov, w * h, NULL) == PC_OK);
    free(cov);
    CHECK(pc_algo_filter_make(&PC_ALGO_FILTER_MOTION_BLUR, &mp, &flt, NULL) == PC_OK);
    for (m = 0; m < 4; m++) {
        for (st = 0; st < 3; st++) {
            for (al = 0; al < 2; al++) {
                pc_pixel_format f = {MODES[m], (pc_sample_type)st, al != 0};
                size_t ch = pc_pixel_format_channels(f);
                float tol = st == PC_SAMPLE_TYPE_U8    ? 1.0f / 255.0f + 1e-7f
                            : st == PC_SAMPLE_TYPE_U16 ? 1.0f / 65535.0f + 1e-7f
                                                       : 1e-6f;
                pc_surface source;
                pc_algo_image img;
                fft_pattern(&img, rect, ch, al != 0);
                pc_surface_init(&source, f);
                CHECK(pc_surface_write_region(&source, rect, img.data, img.len, NULL) == PC_OK);
                pc_algo_image_fini(&img);
                for (e = 0; e < 2; e++) {
                    pc_algo_image src;
                    size_t len;
                    float *filtered;
                    CHECK((e ? pc_algo_image_read_clamped(&source, pc_rect_inflate(rect, reach), rect, &src,
                                                          NULL)
                             : pc_algo_image_read(&source, pc_rect_inflate(rect, reach), &src, NULL)) ==
                          PC_OK);
                    filtered = fft(&src, rect, al != 0, 31.0f, 73.25f, &len);
                    for (s = 0; s < 2; s++) {
                        pc_surface expected;
                        float *data = malloc(len * sizeof(float)), *ev = NULL;
                        size_t elen;
                        memcpy(data, filtered, len * sizeof(float));
                        if (s) {
                            pc_algo_mix_selection(data, rect, &sel, &src);
                        }
                        CHECK(pc_surface_clone(&source, &expected, NULL) == PC_OK);
                        CHECK(pc_surface_write_region(&expected, rect, data, len, NULL) == PC_OK);
                        CHECK(pc_surface_read_region(&expected, pc_rect_inflate(rect, 2), &ev, &elen, NULL) ==
                              PC_OK);
                        for (t = 0; t < 3; t++) {
                            pc_surface got;
                            float *gv = NULL;
                            size_t glen, k;
                            CHECK(pc_algo_apply_tiled(&source, &flt, rect, rect, s ? &sel : NULL, tiles[t],
                                                      e ? &rect : NULL, &got, NULL) == PC_OK);
                            CHECK(pc_surface_read_region(&got, pc_rect_inflate(rect, 2), &gv, &glen, NULL) ==
                                  PC_OK);
                            CHECK(glen == elen);
                            for (k = 0; k < glen && k < elen; k++) {
                                if (!(fabsf(gv[k] - ev[k]) <= tol)) {
                                    PC_T_FAIL("mode %d depth %d alpha %d extent %d sel %d tile %d sample "
                                              "%zu: %g vs %g",
                                              m, st, al, e, s, (int)tiles[t], k, (double)gv[k],
                                              (double)ev[k]);
                                    break;
                                }
                            }
                            free(gv);
                            pc_surface_fini(&got);
                        }
                        free(ev);
                        free(data);
                        pc_surface_fini(&expected);
                    }
                    free(filtered);
                    pc_algo_image_fini(&src);
                }
                pc_surface_fini(&source);
            }
        }
    }
    pc_algo_filter_fini(&flt);
    pc_surface_fini(&sel);
}

static void test_motion_fft_rejects_unsupported_values_and_extreme_geometry(void)
{
    pc_rect rect = pc_rect_make(-2, -2, 3, 3);
    pc_algo_image src, bad;
    float *d = NULL;
    size_t len, k;
    const float cases[5][2] = {
        {NAN, 64.0f}, {INFINITY, 64.0f}, {30.0f, NAN}, {30.0f, INFINITY}, {30.0f, 2001.0f}};
    const float alphas[4] = {-0.25f, 1.25f, NAN, INFINITY}, samples[4] = {NAN, INFINITY, -INFINITY, 2e10f};
    const int32_t origins[2] = {16385, INT32_MIN};
    fft_pattern(&src, rect, 4, true);
    for (k = 0; k < 5; k++) {
        CHECK(!pc_algo_motion_fft_filter(&src, rect, true, cases[k][0], cases[k][1], &d, &len));
    }
    for (k = 0; k < 4; k++) {
        CHECK(pc_algo_image_clone(&src, &bad, NULL) == PC_OK);
        bad.data[3] = alphas[k];
        CHECK(!pc_algo_motion_fft_filter(&bad, rect, true, 30.0f, 64.0f, &d, &len));
        bad.data[3] = 1.0f;
        bad.data[0] = samples[k];
        CHECK(!pc_algo_motion_fft_filter(&bad, rect, true, 30.0f, 64.0f, &d, &len));
        pc_algo_image_fini(&bad);
    }
    /* A malformed image (data shorter than its rect). */
    bad = src;
    bad.len = 1;
    CHECK(!pc_algo_motion_fft_filter(&bad, rect, true, 30.0f, 64.0f, &d, &len));
    for (k = 0; k < 2; k++) {
        pc_rect r = pc_rect_make(origins[k], 0, origins[k] + 5, 5);
        pc_algo_image z;
        image_new(&z, r, 1);
        CHECK(!pc_algo_motion_fft_filter(&z, r, false, 30.0f, 64.0f, &d, &len));
        pc_algo_image_fini(&z);
    }
    pc_algo_image_fini(&src);
}

/* apply_for_bench: the FFT scheduler with the crossover skipped. */
static pc_status bench(const pc_surface *s, const pc_algo_filter *f, pc_rect area, pc_rect bounds,
                       const pc_surface *sel, const pc_rect *extent, int32_t tile, const pc_interrupt *ctl,
                       pc_surface *out, bool *handled)
{
    pc_algo_apply_args a = {s, f, area, bounds, sel, tile, extent, ctl};
    return pc_algo_motion_apply_ex(&a, true, out, handled, NULL);
}

static void test_motion_fft_production_crosses_real_tile_seams(void)
{
    static const float covs[5] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    static const int32_t tiles[3] = {64, 128, 256};
    pc_rect extent = pc_rect_make(-180, -145, 173, 144), area = pc_rect_make(-170, -135, 163, 134);
    pc_rect check = pc_rect_inflate(extent, 8), inner = pc_rect_make(-51, -31, 53, 34);
    pc_algo_motion_params mp = {31.0f, 73.25f};
    int32_t reach = pc_f32_to_i32(ceilf(73.25f / 2.0f)) + 2, x, y;
    size_t ew = pc_rect_width(extent), eh = pc_rect_height(extent), i;
    float *cov = malloc(ew * eh * sizeof(float));
    const float fillpx[4] = {0.125f, 0.25f, 0.5f, 1.0f};
    pc_surface sel;
    pc_algo_filter flt;
    int d, cl, s, t;
    for (i = 0; i < ew * eh; i++) {
        x = extent.x0 + (int32_t)(i % ew);
        y = extent.y0 + (int32_t)(i / ew);
        cov[i] = pc_rect_contains(inner, x, y) ? covs[(((x + y) % 5) + 5) % 5] : 0.0f;
    }
    pc_surface_init(&sel, PC_PIXEL_FORMAT_GRAY8);
    CHECK(pc_surface_write_region(&sel, extent, cov, ew * eh, NULL) == PC_OK);
    free(cov);
    CHECK(pc_algo_filter_make(&PC_ALGO_FILTER_MOTION_BLUR, &mp, &flt, NULL) == PC_OK);
    for (d = 0; d < 2; d++) {
        pc_pixel_format f = {PC_COLOR_MODE_RGB, d == 0 ? PC_SAMPLE_TYPE_U16 : PC_SAMPLE_TYPE_F32, true};
        float tol = d == 0 ? 1.0f / 65535.0f + 1e-7f : 1e-6f;
        pc_surface source;
        pc_algo_image img, original;
        fft_pattern(&img, extent, 4, true);
        pc_surface_init(&source, f);
        CHECK(pc_surface_write_region(&source, extent, img.data, img.len, NULL) == PC_OK);
        pc_algo_image_fini(&img);
        CHECK(pc_surface_fill_rect(&source, pc_rect_make(-86, -61, -82, -57), fillpx, 4, NULL) == PC_OK);
        CHECK(pc_algo_image_read(&source, check, &original, NULL) == PC_OK);
        for (cl = 0; cl < 2; cl++) {
            pc_algo_image src;
            size_t len;
            float *filtered;
            CHECK((cl ? pc_algo_image_read_clamped(&source, pc_rect_inflate(area, reach), extent, &src, NULL)
                      : pc_algo_image_read(&source, pc_rect_inflate(area, reach), &src, NULL)) == PC_OK);
            filtered = fft(&src, area, true, 31.0f, 73.25f, &len);
            for (s = 0; s < 2; s++) {
                pc_surface exp_s;
                pc_algo_image expected;
                float *data = malloc(len * sizeof(float));
                memcpy(data, filtered, len * sizeof(float));
                if (s) {
                    pc_algo_mix_selection(data, area, &sel, &src);
                }
                CHECK(pc_surface_clone(&source, &exp_s, NULL) == PC_OK);
                CHECK(pc_surface_write_region(&exp_s, area, data, len, NULL) == PC_OK);
                CHECK(pc_algo_image_read(&exp_s, check, &expected, NULL) == PC_OK);
                for (t = 0; t < 3; t++) {
                    pc_surface res;
                    pc_algo_image got;
                    bool handled = false;
                    int bad = 0;
                    CHECK(bench(&source, &flt, area, extent, s ? &sel : NULL, cl ? &extent : NULL, tiles[t],
                                NULL, &res, &handled) == PC_OK);
                    CHECK(handled);
                    if (!handled) {
                        continue;
                    }
                    CHECK(pc_algo_image_read(&res, check, &got, NULL) == PC_OK);
                    for (y = check.y0; y < check.y1 && bad == 0; y++) {
                        for (x = check.x0; x < check.x1 && bad == 0; x++) {
                            size_t c;
                            for (c = 0; c < 4; c++) {
                                float a = pc_algo_image_get(&got, x, y, c);
                                if (!pc_rect_contains(area, x, y)) {
                                    float o = pc_algo_image_get(&original, x, y, c);
                                    if (memcmp(&a, &o, 4) != 0) {
                                        PC_T_FAIL("write outside area at (%d,%d)", (int)x, (int)y);
                                        bad = 1;
                                    }
                                } else if (!(fabsf(a - pc_algo_image_get(&expected, x, y, c)) <= tol)) {
                                    PC_T_FAIL("seam: depth %d clamp %d sel %d tile %d (%d,%d) c%zu: %g vs %g",
                                              d, cl, s, (int)tiles[t], (int)x, (int)y, c, (double)a,
                                              (double)pc_algo_image_get(&expected, x, y, c));
                                    bad = 1;
                                }
                            }
                        }
                    }
                    pc_algo_image_fini(&got);
                    pc_surface_fini(&res);
                }
                pc_algo_image_fini(&expected);
                pc_surface_fini(&exp_s);
                free(data);
            }
            free(filtered);
            pc_algo_image_fini(&src);
        }
        pc_algo_image_fini(&original);
        pc_surface_fini(&source);
    }
    pc_algo_filter_fini(&flt);
    pc_surface_fini(&sel);
}

static void test_motion_fft_production_unsupported_inputs_keep_row_fallback(void)
{
    pc_algo_motion_params mp = {31.0f, 64.0f};
    int32_t reach = pc_f32_to_i32(ceilf(64.0f / 2.0f)) + 2;
    pc_algo_filter flt;
    int cs;
    CHECK(pc_algo_filter_make(&PC_ALGO_FILTER_MOTION_BLUR, &mp, &flt, NULL) == PC_OK);
    for (cs = 0; cs < 5; cs++) {
        int32_t origin = cs == 4 ? 20000 : -64;
        pc_rect rect = pc_rect_make(origin, -32, origin + 128, 32);
        pc_algo_image img, src;
        pc_surface source, want, got, unused;
        size_t middle = (32 * (size_t)pc_rect_width(rect) + 64) * 4;
        bool handled = true;
        fft_pattern(&img, rect, 4, true);
        if (cs == 0) {
            img.data[middle + 3] = -0.25f;
        } else if (cs == 1) {
            img.data[middle + 3] = 1.25f;
        } else if (cs == 2) {
            img.data[middle] = NAN;
        } else if (cs == 3) {
            img.data[middle] = INFINITY;
        }
        pc_surface_init(&source, PC_PIXEL_FORMAT_RGBA32F);
        CHECK(pc_surface_write_region(&source, rect, img.data, img.len, NULL) == PC_OK);
        pc_algo_image_fini(&img);
        CHECK(pc_algo_image_read(&source, pc_rect_inflate(rect, reach), &src, NULL) == PC_OK);
        {
            float *d = rows_k(&src, rect, true, PC_COLOR_MODE_RGB, 31.0f, 64.0f);
            size_t len = (size_t)pc_rect_width(rect) * pc_rect_height(rect) * 4;
            CHECK(pc_surface_clone(&source, &want, NULL) == PC_OK);
            CHECK(pc_surface_write_region(&want, rect, d, len, NULL) == PC_OK);
            free(d);
        }
        CHECK(bench(&source, &flt, rect, rect, NULL, NULL, 64, NULL, &unused, &handled) == PC_OK);
        CHECK(!handled);
        if (handled) {
            pc_surface_fini(&unused);
        }
        CHECK(pc_algo_apply_tiled(&source, &flt, rect, rect, NULL, 64, NULL, &got, NULL) == PC_OK);
        CHECK(surfaces_equal_in(&got, &want, rect));
        pc_surface_fini(&got), pc_surface_fini(&want), pc_surface_fini(&source);
        pc_algo_image_fini(&src);
    }
    pc_algo_filter_fini(&flt);
}

static void test_motion_fft_production_near_alpha_cutoff_retains_row_summation(void)
{
    pc_rect extent = pc_rect_make(-64, -64, 65, 65), area = pc_rect_make(-32, -32, 32, 32);
    const float alphas[3] = {1e-7f, 0.999e-7f, 1.001e-7f}, dists[2] = {200.0f, 500.0f};
    int a, d;
    for (a = 0; a < 3; a++) {
        pc_surface source;
        const float px[4] = {0.25f, 0.5f, 0.75f, alphas[a]};
        pc_surface_init(&source, PC_PIXEL_FORMAT_RGBA32F);
        CHECK(pc_surface_fill_rect(&source, extent, px, 4, NULL) == PC_OK);
        for (d = 0; d < 2; d++) {
            pc_algo_motion_params mp = {0.0f, dists[d]};
            int32_t reach = pc_f32_to_i32(ceilf(dists[d] / 2.0f)) + 2;
            pc_algo_filter flt;
            pc_algo_image src;
            pc_surface want, got;
            float *rk;
            size_t len = 64 * 64 * 4;
            bool handled = false;
            CHECK(pc_algo_filter_make(&PC_ALGO_FILTER_MOTION_BLUR, &mp, &flt, NULL) == PC_OK);
            CHECK(pc_algo_image_read_clamped(&source, pc_rect_inflate(area, reach), extent, &src, NULL) ==
                  PC_OK);
            rk = rows_k(&src, area, true, PC_COLOR_MODE_RGB, 0.0f, dists[d]);
            CHECK(pc_surface_clone(&source, &want, NULL) == PC_OK);
            CHECK(pc_surface_write_region(&want, area, rk, len, NULL) == PC_OK);
            CHECK(bench(&source, &flt, area, extent, NULL, &extent, 64, NULL, &got, &handled) == PC_OK);
            CHECK(handled);
            if (handled) {
                CHECK(surfaces_equal_in(&got, &want, area));
                pc_surface_fini(&got);
            }
            free(rk);
            pc_surface_fini(&want);
            pc_algo_image_fini(&src);
            pc_algo_filter_fini(&flt);
        }
        pc_surface_fini(&source);
    }
}

static void test_motion_fft_low_alpha_at_large_coordinates(void)
{
    pc_rect rect = pc_rect_make(15950, 15998, 16080, 16002), out = pc_rect_make(16000, 16000, 16003, 16001);
    pc_algo_image src;
    size_t i, len, w = pc_rect_width(rect);
    float *e, *a;
    double *ed;
    image_new(&src, rect, 4);
    for (i = 0; i < src.len / 4; i++) {
        if (rect.y0 + (int32_t)(i / w) == 15999) {
            src.data[i * 4] = 0.25f, src.data[i * 4 + 1] = 0.5f, src.data[i * 4 + 2] = 0.75f,
                         src.data[i * 4 + 3] = 1.0f;
        }
    }
    e = rows_k(&src, out, true, PC_COLOR_MODE_RGB, 0.0005f, 64.0f);
    a = fft(&src, out, true, 0.0005f, 64.0f, &len);
    ed = malloc(len * sizeof(double));
    for (i = 0; i < len; i++) {
        ed[i] = (double)e[i];
    }
    CHECK(assert_ideal(a, ed, len));
    free(e), free(a), free(ed);
    pc_algo_image_fini(&src);
}

static void test_motion_fft_scalar_row_point_matches_rows(void)
{
    static const float cases[3][2] = {{30.0f, 64.0f}, {0.0f, 256.0f}, {43.0f, 1000.0f}};
    pc_rect rect = pc_rect_make(-5, -4, 6, 5), out = pc_rect_make(-1, 0, 2, 1),
            big = pc_rect_make(-34, -34, 35, 35);
    size_t ch, i;
    int al, k;
    float alphas[4];
    uint32_t bits;
    for (ch = 1; ch <= 5; ch++) {
        for (al = 0; al < 2; al++) {
            pc_algo_image src;
            fft_pattern(&src, rect, ch, al != 0);
            for (i = 0; i < src.len; i++) {
                if (!(al && i % ch == ch - 1)) {
                    src.data[i] = src.data[i] * 32.0f - 8.0f;
                }
            }
            for (k = 0; k < 3; k++) {
                float *want =
                    rows_k(&src, out, al != 0, PC_COLOR_MODE_MULTICHANNEL, cases[k][0], cases[k][1]);
                int32_t x;
                for (x = out.x0; x < out.x1; x++) {
                    float got[5];
                    CHECK(pc_algo_motion_row_point(&src, x, out.y0, al != 0, cases[k][0], cases[k][1], got));
                    CHECK(bits_equal(got, want + (size_t)(x - out.x0) * ch, ch));
                }
                free(want);
            }
            pc_algo_image_fini(&src);
        }
    }
    alphas[0] = 1e-7f;
    memcpy(&bits, &alphas[0], 4);
    bits++;
    memcpy(&alphas[1], &bits, 4);
    alphas[2] = 0.999e-7f, alphas[3] = 1.001e-7f;
    for (ch = 1; ch <= 5; ch++) {
        for (k = 0; k < 4; k++) {
            pc_algo_image src;
            float got[5], *want;
            image_new(&src, big, ch);
            for (i = 0; i < src.len; i++) {
                src.data[i] = i % ch == ch - 1 ? alphas[k] : 1e6f;
            }
            want = rows_k(&src, pc_rect_make(0, 0, 1, 1), true, PC_COLOR_MODE_MULTICHANNEL, 31.0f, 64.0f);
            CHECK(pc_algo_motion_row_point(&src, 0, 0, true, 31.0f, 64.0f, got));
            CHECK(bits_equal(got, want, ch));
            free(want);
            pc_algo_image_fini(&src);
        }
    }
}

static void test_motion_fft_output_outside_extent_retains_boundary_reads(void)
{
    pc_rect extent = pc_rect_make(0, 0, 128, 128), area = pc_rect_make(0, 200, 64, 264);
    pc_algo_motion_params mp = {0.0f, 256.0f};
    int32_t reach = pc_f32_to_i32(ceilf(256.0f / 2.0f)) + 2;
    const float px[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    pc_surface source, got, unused;
    pc_algo_filter flt;
    pc_algo_image src;
    pc_algo_apply_args a;
    bool handled = true;
    float *want, *gv = NULL;
    size_t glen;
    pc_surface_init(&source, PC_PIXEL_FORMAT_RGBA32F);
    CHECK(pc_surface_fill_rect(&source, extent, px, 4, NULL) == PC_OK);
    CHECK(pc_algo_filter_make(&PC_ALGO_FILTER_MOTION_BLUR, &mp, &flt, NULL) == PC_OK);
    a.surface = &source, a.filter = &flt, a.area = area, a.bounds = extent, a.selection = NULL, a.tile = 64;
    a.extent = &extent, a.ctl = NULL;
    CHECK(pc_algo_motion_apply(&a, &unused, &handled, NULL) == PC_OK);
    CHECK(!handled);
    if (handled) {
        pc_surface_fini(&unused);
    }
    CHECK(pc_algo_image_read_clamped(&source, pc_rect_inflate(area, reach), extent, &src, NULL) == PC_OK);
    want = rows_k(&src, area, true, PC_COLOR_MODE_RGB, 0.0f, 256.0f);
    CHECK(pc_algo_apply_tiled(&source, &flt, area, extent, NULL, 64, &extent, &got, NULL) == PC_OK);
    CHECK(pc_surface_read_region(&got, area, &gv, &glen, NULL) == PC_OK);
    CHECK(glen == 64 * 64 * 4 && bits_equal(gv, want, glen));
    free(gv), free(want);
    pc_surface_fini(&got), pc_surface_fini(&source);
    pc_algo_image_fini(&src);
    pc_algo_filter_fini(&flt);
}

/* ---- motion_apply.rs ---- */

static void test_scheduler_bounds_complete_groups(void)
{
    size_t mib = (size_t)1 << 20, g = 0;
    CHECK(pc_algo_motion_group_size(100 * mib, mib, 150 * mib, 5 * mib, 100, 12, &g));
    CHECK_EQ_UINT(g, 2);
    CHECK(100 * mib + mib + g * (150 * mib + 5 * mib) <= ((size_t)512 << 20));
    CHECK(!pc_algo_motion_group_size(100 * mib, 0, 450 * mib, 5 * mib, 100, 12, &g));
    CHECK(!pc_algo_motion_group_size((size_t)512 << 20, 1, 1, 1, 1, 1, &g));
    CHECK(!pc_algo_motion_group_size(0, 0, SIZE_MAX, 1, 1, 1, &g));
    CHECK(!pc_algo_motion_group_size(0, 0, 1, 1, 0, 1, &g));
}

static bool declined(const pc_surface *s, float angle, float distance, pc_rect area, pc_rect bounds,
                     const pc_rect *extent, int32_t tile)
{
    pc_algo_motion_params mp = {angle, distance};
    pc_algo_filter f;
    pc_algo_apply_args a;
    pc_surface out;
    bool handled = true;
    CHECK(pc_algo_filter_make(&PC_ALGO_FILTER_MOTION_BLUR, &mp, &f, NULL) == PC_OK);
    a.surface = s, a.filter = &f, a.area = area, a.bounds = bounds, a.selection = NULL, a.tile = tile;
    a.extent = extent, a.ctl = NULL;
    CHECK(pc_algo_motion_apply(&a, &out, &handled, NULL) == PC_OK);
    if (handled) {
        pc_surface_fini(&out);
    }
    pc_algo_filter_fini(&f);
    return !handled;
}

static void test_scheduler_preflight_declines_unsupported_geometry_and_small_distances(void)
{
    pc_surface s, out;
    pc_rect bounds = pc_rect_make(0, 0, 128, 128), empty = PC_RECT_EMPTY;
    pc_algo_motion_params mp = {30.0f, 64.0f};
    pc_algo_filter f;
    pc_algo_apply_args a;
    bool handled = false;
    pc_surface_init(&s, PC_PIXEL_FORMAT_GRAY8);
    CHECK(declined(&s, 30.0f, 10.0f, bounds, bounds, &bounds, 64));
    CHECK(declined(&s, 30.0f, 64.0f, pc_rect_make(0, 0, INT32_MAX, 128), bounds, &bounds, 64));
    CHECK(declined(&s, 30.0f, 64.0f, bounds, bounds, &empty, 64));
    CHECK(declined(&s, 30.0f, 64.0f, pc_rect_make(0, 0, 1, 1), bounds, &bounds, 512));
    CHECK(declined(&s, 30.0f, 2000.0f, bounds, bounds, &bounds, 512));
    /* valid_rect: wider than the coordinate limit. */
    CHECK(declined(&s, 30.0f, 64.0f,
                   pc_rect_make(-PC_ALGO_MOTION_COORDINATE_LIMIT, 0, PC_ALGO_MOTION_COORDINATE_LIMIT, 1),
                   bounds, NULL, 64));
    CHECK(pc_algo_filter_make(&PC_ALGO_FILTER_MOTION_BLUR, &mp, &f, NULL) == PC_OK);
    a.surface = &s, a.filter = &f, a.area = empty, a.bounds = bounds, a.selection = NULL, a.tile = 64;
    a.extent = &bounds, a.ctl = NULL;
    CHECK(pc_algo_motion_apply(&a, &out, &handled, NULL) == PC_OK && handled);
    if (handled) {
        CHECK(pc_surface_tile_count(&out) == 0);
        pc_surface_fini(&out);
    }
    pc_algo_filter_fini(&f);
    pc_surface_fini(&s);
}

static void test_crossover_keeps_rows_for_losses_and_memory_limited_streaks(void)
{
    static const size_t workers[4] = {1, 2, 4, 12};
    static const float angles[4] = {0.0f, 30.0f, 45.0f, 90.0f}, dists[4] = {10.0f, 64.0f, 128.0f, 256.0f};
    pc_rect proxy = pc_rect_make(0, 0, 1500, 1000), full = pc_rect_make(0, 0, 6000, 4000);
    size_t w, ch;
    int a, d;
    bool ok;
    for (w = 0; w < 4; w++) {
        for (ch = 1; ch <= 5; ch++) {
            for (a = 0; a < 4; a++) {
                for (d = 0; d < 4; d++) {
                    CHECK(!pc_algo_motion_worthwhile(angles[a], dists[d], 512, proxy, 576, ch, workers[w],
                                                     &ok) &&
                          ok);
                }
            }
        }
    }
    for (w = 2; w < 4; w++) {
        CHECK(pc_algo_motion_worthwhile(30.0f, 1000.0f, 512, proxy, 1024, 4, workers[w], &ok) && ok);
        CHECK(pc_algo_motion_worthwhile(45.0f, 2000.0f, 512, proxy, 2048, 4, workers[w], &ok) && ok);
        CHECK(!pc_algo_motion_worthwhile(30.0f, 2000.0f, 512, full, 2048, 4, workers[w], &ok) && ok);
    }
    CHECK(pc_algo_motion_worthwhile(30.0f, 1000.0f, 512, full, 1024, 4, 12, &ok) && ok);
    CHECK(!pc_algo_motion_worthwhile(30.0f, 1000.0f, 512, proxy, 1024, 4, 1, &ok) && ok);
    CHECK(!pc_algo_motion_worthwhile(30.0f, 1000.0f, 64, pc_rect_make(0, 0, 64, 64), 1024, 4, 12, &ok) && ok);
}

typedef struct cancel_state {
    atomic_bool cancelled;
    bool on_progress;
} cancel_state;

static bool is_cancelled(void *ctx)
{
    return atomic_load(&((cancel_state *)ctx)->cancelled);
}

static void on_progress(void *ctx, float f)
{
    cancel_state *c = ctx;
    if (c->on_progress && f > 0.0f) {
        atomic_store(&c->cancelled, true);
    }
}

static void test_scheduler_cancellation_discards_all_partial_output(void)
{
    pc_rect bounds = pc_rect_make(0, 0, 128, 128);
    pc_surface s, out;
    const float v = 0.4f;
    pc_algo_motion_params mp = {0.0f, 64.0f};
    pc_algo_filter f;
    cancel_state cs;
    pc_interrupt ctl = {is_cancelled, on_progress, &cs};
    pc_algo_apply_args a;
    bool handled = false;
    uint8_t *before = NULL, *after = NULL;
    size_t lb, la;
    pc_surface_init(&s, PC_PIXEL_FORMAT_GRAY8);
    CHECK(pc_surface_fill_rect(&s, bounds, &v, 1, NULL) == PC_OK);
    CHECK(pc_surface_to_interleaved(&s, bounds, &before, &lb, NULL) == PC_OK);
    CHECK(pc_algo_filter_make(&PC_ALGO_FILTER_MOTION_BLUR, &mp, &f, NULL) == PC_OK);
    atomic_init(&cs.cancelled, true);
    cs.on_progress = false;
    a.surface = &s, a.filter = &f, a.area = bounds, a.bounds = bounds, a.selection = NULL, a.tile = 64;
    a.extent = &bounds, a.ctl = &ctl;
    CHECK(pc_algo_motion_apply(&a, &out, &handled, NULL) == PC_ERR_CANCELLED && handled);
    atomic_store(&cs.cancelled, false);
    cs.on_progress = true;
    handled = false;
    CHECK(pc_algo_motion_apply_ex(&a, true, &out, &handled, NULL) == PC_ERR_CANCELLED && handled);
    CHECK(pc_surface_to_interleaved(&s, bounds, &after, &la, NULL) == PC_OK);
    CHECK(la == lb && memcmp(before, after, la) == 0);
    free(before), free(after);
    pc_algo_filter_fini(&f);
    pc_surface_fini(&s);
}

int main(int argc, char **argv)
{
    RUN(test_motion_identity_preserves_signed_zero_and_transparent_colours);
    RUN(test_motion_default_kernel_retains_merged_taps);
    RUN(test_motion_pipeline_matches_rows_across_formats_edges_selections_and_tiles);
    RUN(test_motion_fft_matches_independent_ideal_for_channels_angles_and_distances);
    RUN(test_motion_fft_impulse_preserves_mass_centroid_and_directional_width);
    RUN(test_motion_fft_sharp_stripes_match_uniform_streak);
    RUN(test_motion_fft_premultiplied_alpha_hidden_colour_hdr_and_cutoff);
    RUN(test_motion_fft_summation_error_is_bounded_in_supported_coordinates);
    RUN(test_motion_fft_large_hdr_colours_do_not_change_small_alpha_or_cutoff);
    RUN(test_motion_fft_pipeline_supports_depths_models_edges_selections_and_seams);
    RUN(test_motion_fft_rejects_unsupported_values_and_extreme_geometry);
    RUN(test_motion_fft_production_crosses_real_tile_seams);
    RUN(test_motion_fft_production_unsupported_inputs_keep_row_fallback);
    RUN(test_motion_fft_production_near_alpha_cutoff_retains_row_summation);
    RUN(test_motion_fft_low_alpha_at_large_coordinates);
    RUN(test_motion_fft_scalar_row_point_matches_rows);
    RUN(test_motion_fft_output_outside_extent_retains_boundary_reads);
    RUN(test_scheduler_bounds_complete_groups);
    RUN(test_scheduler_preflight_declines_unsupported_geometry_and_small_distances);
    RUN(test_crossover_keeps_rows_for_losses_and_memory_limited_streaks);
    RUN(test_scheduler_cancellation_discards_all_partial_output);
    return pc_t_finish();
}
