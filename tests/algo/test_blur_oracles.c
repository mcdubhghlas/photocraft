/* Oracle tests of the L5 fast paths: blur.rs's ranked Surface Blur, noise.rs's sliding median,
 * shape_blur_tests.rs, selection_blur.rs (Smooth / Feather against their direct definitions),
 * and never-crash checks of every L5 filter with hostile parameters. */
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "algo/blur_int.h"
#include "base/num.h"
#include "base/rmath.h"
#include "harness.h"
#include "raster/pixel.h"

static uint32_t xs(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

static float *buf(pc_rect out, size_t ch)
{
    return calloc((size_t)pc_rect_width(out) * pc_rect_height(out) * ch + 1, sizeof(float));
}

static float max_diff_v(const float *a, const float *b, size_t n)
{
    float d = 0.0f;
    size_t i;
    for (i = 0; i < n; i++) {
        d = pc_rs_maxf(d, fabsf(a[i] - b[i]));
    }
    return d;
}

static bool exact(const float *a, const float *b, size_t n)
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

/* ---- Surface Blur (blur.rs) ---- */

/* The direct sum with f64 accumulators, the reference both fast paths are held to. */
static float *surface_reference(const pc_algo_image *src, pc_rect out, bool alpha, int32_t r, float t)
{
    size_t n = src->ch, cc = alpha ? n - 1 : n, k = 0, c;
    float *res = buf(out, n);
    int32_t x, y, xx, yy;
    for (y = out.y0; y < out.y1; y++) {
        for (x = out.x0; x < out.x1; x++) {
            for (c = 0; c < cc; c++) {
                float v0 = pc_algo_image_get(src, x, y, c);
                double acc = 0.0, wsum = 0.0;
                for (yy = y - r; yy <= y + r; yy++) {
                    for (xx = x - r; xx <= x + r; xx++) {
                        float v, w;
                        if (alpha && pc_algo_image_get(src, xx, yy, n - 1) <= 0.0f) {
                            continue;
                        }
                        v = pc_algo_image_get(src, xx, yy, c);
                        w = pc_rs_maxf_k(1.0f - fabsf(v - v0) / t, 0.0f);
                        acc += (double)v * (double)w;
                        wsum += (double)w;
                    }
                }
                res[k++] = wsum > 0.0 ? (float)(acc / wsum) : v0;
            }
            if (alpha) {
                res[k++] = pc_algo_image_get(src, x, y, n - 1);
            }
        }
    }
    return res;
}

static void test_surface_blur_ranked_sums_match_the_weighted_window_at_any_depth(void)
{
    static const float cases[4][2] = {{1, 2}, {4, 15}, {12, 60}, {16, 255}};
    static const struct {
        float quant;
        size_t ch;
        bool alpha;
    } kinds[3] = {{65535.0f, 4, true}, {0.0f, 4, true}, {0.0f, 1, false}};
    pc_rect sr = pc_rect_make(-20, -20, 48, 40), out = pc_rect_make(-6, 0, 30, 22);
    int ki, k;
    for (ki = 0; ki < 3; ki++) {
        pc_algo_image img;
        pc_algo_ctx ctx = {sr, PC_COLOR_MODE_RGB, kinds[ki].alpha};
        uint32_t s = 0x1b873593u;
        size_t i, c, ch = kinds[ki].ch;
        CHECK(pc_algo_image_init(&img, sr, ch, NULL) == PC_OK);
        for (i = 0; i < img.len; i += ch) {
            for (c = 0; c < ch; c++) {
                float raw = (float)(xs(&s) % 10000) / 10000.0f;
                img.data[i + c] = kinds[ki].quant > 0.0f ? roundf(raw * kinds[ki].quant) / kinds[ki].quant
                                                         : raw * 1.5f - 0.25f;
            }
            if (kinds[ki].alpha && s % 7 == 0) {
                img.data[i + ch - 1] = 0.0f;
            }
        }
        for (k = 0; k < 4; k++) {
            float t = (cases[k][1] / 255.0f) * 2.5f, *ranked = buf(out, ch), *ref;
            bool declined = true;
            CHECK(pc_algo_surface_ranked(&img, out, &ctx, (int32_t)cases[k][0], t, ranked, &declined, NULL) ==
                      PC_OK &&
                  !declined);
            ref = surface_reference(&img, out, kinds[ki].alpha, (int32_t)cases[k][0], t);
            CHECK(max_diff_v(ref, ranked, 36 * 22 * ch) < 1e-6f);
            free(ranked), free(ref);
        }
        pc_algo_image_fini(&img);
    }
}

static void test_surface_blur_ranked_declines_non_finite_samples_and_empty_output(void)
{
    pc_rect sr = pc_rect_make(0, 0, 8, 8);
    pc_algo_ctx ctx = {sr, PC_COLOR_MODE_GRAYSCALE, false};
    pc_algo_image img;
    float d[64];
    bool declined = true;
    size_t i;
    CHECK(pc_algo_image_init(&img, sr, 1, NULL) == PC_OK);
    for (i = 0; i < 64; i++) {
        img.data[i] = 0.5f;
    }
    CHECK(pc_algo_surface_ranked(&img, pc_rect_make(2, 2, 2, 6), &ctx, 2, 0.1f, d, &declined, NULL) ==
              PC_OK &&
          !declined);
    img.data[9] = NAN;
    CHECK(pc_algo_surface_ranked(&img, sr, &ctx, 2, 0.1f, d, &declined, NULL) == PC_OK && declined);
    /* The public entry still answers, through the direct sum. */
    CHECK(pc_algo_surface_blur_k(&img, sr, &ctx, 12.0f, 15.0f, d, NULL) == PC_OK);
    pc_algo_image_fini(&img);
}

/* ---- Median (noise.rs) ---- */

static void test_median_histogram_matches_the_direct_selection(void)
{
    static const int32_t radii[4] = {2, 3, 7, 16};
    pc_rect sr = pc_rect_make(-10, -8, 36, 30), out = pc_rect_make(-14, -3, 30, 34);
    const float ts[4] = {-1.0f, 0.0f, 10.0f / 255.0f, 1.0f};
    uint32_t kind;
    int ri, ti;
    for (kind = 0; kind < 3; kind++) {
        pc_algo_image img;
        uint32_t s = 0x2545f491u ^ kind;
        size_t i;
        CHECK(pc_algo_image_init(&img, sr, 4, NULL) == PC_OK);
        for (i = 0; i < img.len; i++) {
            const float pick[6] = {0.0f, -0.0f, 0.25f, 1.5f, -2.0f, 0.0f};
            uint32_t v = xs(&s);
            if (kind == 0) {
                img.data[i] = i % 2 == 0 ? (float)(v % 256) / 255.0f : (float)(v % 4 * 85) / 255.0f;
            } else if (kind == 1) {
                img.data[i] = (float)(v % 65536) / 65535.0f;
            } else {
                img.data[i] = v % 6 == 5 ? (float)(v % 1000) / 997.0f : pick[v % 6];
            }
        }
        if (kind == 2) {
            img.data[100] = NAN;
        }
        for (ri = 0; ri < 4; ri++) {
            for (ti = 0; ti < 4; ti++) {
                float *a = buf(out, 4), *b = buf(out, 4);
                bool has = ts[ti] >= 0.0f;
                CHECK(pc_algo_median_ranked(&img, out, radii[ri], has, ts[ti], a, NULL) == PC_OK);
                CHECK(pc_algo_median_direct(&img, out, radii[ri], has, ts[ti], b, NULL) == PC_OK);
                CHECK(memcmp(a, b, (size_t)44 * 37 * 4 * sizeof(float)) == 0);
                free(a), free(b);
            }
        }
        pc_algo_image_fini(&img);
    }
}

/* ---- Shape Blur (shape_blur_tests.rs) ---- */

static void shape_image(pc_algo_image *img, pc_rect r, size_t n, bool alpha)
{
    size_t i, c, npx = (size_t)pc_rect_width(r) * pc_rect_height(r);
    CHECK(pc_algo_image_init(img, r, n, NULL) == PC_OK);
    for (i = 0; i < npx; i++) {
        for (c = 0; c < n; c++) {
            img->data[i * n + c] =
                alpha && c + 1 == n ? (float)(i % 7) / 6.0f : (float)((i * 31 + c * 13) % 97) / 96.0f;
        }
    }
}

static void spans_of(float radius, pc_algo_blur_shape shape, pc_algo_spans *s)
{
    pc_algo_shape_ctx k;
    pc_algo_shape_ctx_init(&k, shape);
    CHECK(pc_algo_spans_new(radius, pc_algo_shape_inside, &k, s, NULL) == PC_OK);
}

/* shape_blur(): Prepared::new(radius, shape).run (prep built per call). */
static float *shape_blur(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx, float radius,
                         pc_algo_blur_shape shape)
{
    pc_algo_shape_blur_params p = {radius, shape};
    void *prep = NULL;
    float *d = buf(out, src->ch);
    CHECK(pc_algo_shape_blur_prepare(&p, &prep, NULL) == PC_OK);
    CHECK(pc_algo_shape_blur_run(prep, src, out, ctx, d, NULL) == PC_OK);
    pc_algo_shape_blur_prep_free(prep);
    return d;
}

static void test_binary_kernel_sampling_including_fractional_square(void)
{
    static const struct {
        pc_algo_blur_shape s;
        float r;
        double count;
    } cases[7] = {{PC_ALGO_SHAPE_CIRCLE, 3, 29},  {PC_ALGO_SHAPE_RING, 3, 20},
                  {PC_ALGO_SHAPE_SQUARE, 3, 49},  {PC_ALGO_SHAPE_SQUARE, 2.5f, 49},
                  {PC_ALGO_SHAPE_DIAMOND, 3, 25}, {PC_ALGO_SHAPE_TRIANGLE, 3, 18},
                  {PC_ALGO_SHAPE_CROSS, 3, 13}};
    int k;
    for (k = 0; k < 7; k++) {
        pc_algo_spans s;
        spans_of(cases[k].r, cases[k].s, &s);
        CHECK(s.count == cases[k].count);
        pc_algo_spans_fini(&s);
    }
}

static void test_scalar_prefix_exact_all_shapes_radii_channels_subrects(void)
{
    static const float radii[10] = {0.0f, 0.49f, 0.5f, 0.75f, 1.0f, 1.1f, 2.5f, 5.0f, 25.0f, 100.0f};
    pc_rect sr = pc_rect_make(-5, -3, 9, 7);
    pc_rect outs[4] = {sr, pc_rect_make(-2, 1, 3, 2), pc_rect_make(-8, -7, 12, 10), PC_RECT_EMPTY};
    int sh, ri, al, o;
    size_t n;
    for (sh = 0; sh < 9; sh++) {
        for (ri = 0; ri < 10; ri++) {
            pc_algo_spans spans;
            spans_of(radii[ri], (pc_algo_blur_shape)sh, &spans);
            for (n = 1; n <= 8; n++) {
                for (al = 0; al < 2; al++) {
                    pc_algo_image src;
                    pc_algo_ctx ctx = {sr, PC_COLOR_MODE_MULTICHANNEL, al != 0};
                    shape_image(&src, sr, n, al != 0);
                    for (o = 0; o < 4; o++) {
                        float *a = shape_blur(&src, outs[o], &ctx, radii[ri], (pc_algo_blur_shape)sh);
                        float *b = buf(outs[o], n);
                        CHECK(pc_algo_conv_spans(&src, outs[o], &ctx, &spans, b, NULL) == PC_OK);
                        CHECK(exact(a, b, (size_t)pc_rect_width(outs[o]) * pc_rect_height(outs[o]) * n));
                        free(a), free(b);
                    }
                    pc_algo_image_fini(&src);
                }
            }
            pc_algo_spans_fini(&spans);
        }
    }
}

static void test_point_convolution_independent_of_prefix(void)
{
    static const float radii[7] = {0.0f, 0.5f, 1.0f, 1.25f, 2.5f, 5.0f, 7.75f};
    pc_rect sr = pc_rect_make(-3, -2, 4, 3), out = pc_rect_make(-5, -4, 6, 5);
    pc_algo_ctx ctx = {sr, PC_COLOR_MODE_RGB, true};
    pc_algo_image src;
    int sh, ri;
    shape_image(&src, sr, 4, true);
    for (sh = 0; sh < 9; sh++) {
        pc_algo_shape_ctx k;
        pc_algo_shape_ctx_init(&k, (pc_algo_blur_shape)sh);
        for (ri = 0; ri < 7; ri++) {
            float radius = radii[ri], *actual = shape_blur(&src, out, &ctx, radius, (pc_algo_blur_shape)sh);
            int32_t rr = (int32_t)ceilf(radius), x, y, dx, dy;
            int32_t (*pts)[2] = malloc((size_t)(2 * rr + 1) * (size_t)(2 * rr + 1) * sizeof *pts + 8);
            size_t np = 0, kk = 0, p, c;
            float *ref = buf(out, 4);
            for (dy = -rr; dy <= rr; dy++) {
                for (dx = -rr; dx <= rr; dx++) {
                    if (radius >= 0.5f && pc_algo_shape_inside(&k, (float)dx / radius, (float)dy / radius)) {
                        pts[np][0] = dx, pts[np][1] = dy, np++;
                    }
                }
            }
            if (np == 0) {
                pts[0][0] = 0, pts[0][1] = 0, np = 1;
            }
            for (y = out.y0; y < out.y1; y++) {
                for (x = out.x0; x < out.x1; x++) {
                    double sums[4] = {0, 0, 0, 0};
                    float px[4];
                    for (p = 0; p < np; p++) {
                        float a = pc_algo_image_get(&src, x + pts[p][0], y + pts[p][1], 3);
                        for (c = 0; c < 4; c++) {
                            float v = pc_algo_image_get(&src, x + pts[p][0], y + pts[p][1], c);
                            sums[c] += (double)(c < 3 ? v * a : v);
                        }
                    }
                    for (c = 0; c < 4; c++) {
                        px[c] = (float)(sums[c] / (double)np);
                    }
                    pc_algo_unpremul_px(px, 4, true);
                    memcpy(ref + kk, px, sizeof px);
                    kk += 4;
                }
            }
            CHECK(exact(actual, ref, kk));
            free(actual), free(ref), free(pts);
        }
    }
    pc_algo_image_fini(&src);
}

static void test_surfaces_exact_depth_models_alpha_selection_and_tile_boundaries(void)
{
    static const pc_color_mode modes[5] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_GRAYSCALE, PC_COLOR_MODE_CMYK,
                                           PC_COLOR_MODE_LAB, PC_COLOR_MODE_MULTICHANNEL};
    static const float radii[3] = {2.5f, 5.0f, 25.0f};
    static const int32_t tiles[2] = {17, 256};
    pc_rect bounds = pc_rect_make(-2, -1, 263, 5), area = pc_rect_make(0, 0, 260, 4);
    pc_pixel_format sf = {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_F32, false};
    pc_surface sel;
    size_t i, na = (size_t)pc_rect_width(area) * pc_rect_height(area);
    float *cov = malloc(na * sizeof(float));
    int sh, st, m, al, ri, e, t;
    for (i = 0; i < na; i++) {
        cov[i] = (float)(i % 5) / 4.0f;
    }
    pc_surface_init(&sel, sf);
    CHECK(pc_surface_write_region(&sel, area, cov, na, NULL) == PC_OK);
    free(cov);
    for (sh = 0; sh < 9; sh += 2) { /* every other shape keeps the run short; all are covered above */
        for (st = 0; st < 3; st++) {
            for (m = 0; m < 5; m++) {
                for (al = 0; al < 2; al++) {
                    pc_pixel_format f = {modes[m], (pc_sample_type)st, al != 0};
                    size_t n = pc_pixel_format_channels(f);
                    pc_surface surface;
                    pc_algo_image img;
                    pc_algo_ctx ctx = {bounds, modes[m], al != 0};
                    shape_image(&img, bounds, n, al != 0);
                    pc_surface_init(&surface, f);
                    CHECK(pc_surface_write_region(&surface, bounds, img.data, img.len, NULL) == PC_OK);
                    pc_algo_image_fini(&img);
                    for (ri = 0; ri < 3; ri++) {
                        pc_algo_shape_blur_params p = {radii[ri], (pc_algo_blur_shape)sh};
                        pc_algo_filter flt;
                        int32_t reach = (int32_t)ceilf(radii[ri]) + 1;
                        pc_algo_spans spans;
                        CHECK(pc_algo_filter_make(&PC_ALGO_FILTER_SHAPE_BLUR, &p, &flt, NULL) == PC_OK);
                        spans_of(radii[ri], (pc_algo_blur_shape)sh, &spans);
                        for (e = 0; e < 2; e++) {
                            pc_algo_image src;
                            pc_surface ref;
                            float *expected = buf(area, n), *rv = NULL;
                            size_t rl;
                            CHECK((e ? pc_algo_image_read_clamped(&surface, pc_rect_inflate(area, reach),
                                                                  bounds, &src, NULL)
                                     : pc_algo_image_read(&surface, pc_rect_inflate(area, reach), &src,
                                                          NULL)) == PC_OK);
                            CHECK(pc_algo_conv_spans(&src, area, &ctx, &spans, expected, NULL) == PC_OK);
                            pc_algo_mix_selection(expected, area, &sel, &src);
                            CHECK(pc_surface_clone(&surface, &ref, NULL) == PC_OK);
                            CHECK(pc_surface_write_region(&ref, area, expected, na * n, NULL) == PC_OK);
                            CHECK(pc_surface_read_region(&ref, bounds, &rv, &rl, NULL) == PC_OK);
                            for (t = 0; t < 2; t++) {
                                pc_surface got;
                                float *gv = NULL;
                                size_t gl;
                                CHECK(pc_algo_apply_tiled(&surface, &flt, area, bounds, &sel, tiles[t],
                                                          e ? &bounds : NULL, &got, NULL) == PC_OK);
                                CHECK(pc_surface_read_region(&got, bounds, &gv, &gl, NULL) == PC_OK);
                                CHECK(gl == rl && exact(gv, rv, gl));
                                free(gv);
                                pc_surface_fini(&got);
                            }
                            free(rv), free(expected);
                            pc_surface_fini(&ref);
                            pc_algo_image_fini(&src);
                        }
                        pc_algo_spans_fini(&spans);
                        pc_algo_filter_fini(&flt);
                    }
                    pc_surface_fini(&surface);
                }
            }
        }
    }
    pc_surface_fini(&sel);
}

static void test_largest_supported_radius_and_fraction_keep_scalar_kernel(void)
{
    pc_rect sr = pc_rect_make(-1, -1, 2, 2);
    pc_algo_ctx ctx = {sr, PC_COLOR_MODE_RGB, true};
    pc_algo_image src;
    const float radii[2] = {999.25f, 1000.0f};
    int sh, ri;
    shape_image(&src, sr, 4, true);
    for (sh = 0; sh < 9; sh++) {
        for (ri = 0; ri < 2; ri++) {
            pc_algo_spans spans;
            float *a = shape_blur(&src, sr, &ctx, radii[ri], (pc_algo_blur_shape)sh), *b = buf(sr, 4);
            spans_of(radii[ri], (pc_algo_blur_shape)sh, &spans);
            CHECK(pc_algo_conv_spans(&src, sr, &ctx, &spans, b, NULL) == PC_OK);
            CHECK(exact(a, b, 36));
            free(a), free(b);
            pc_algo_spans_fini(&spans);
        }
    }
    pc_algo_image_fini(&src);
}

static void test_degenerate_hdr_and_nonfinite_samples_keep_scalar_behaviour(void)
{
    const float vals[7] = {FLT_MAX, -FLT_MAX, 1e-30f, -17.0f, INFINITY, -INFINITY, NAN};
    const float avals[5] = {0.0f, 1e-8f, 1e-7f, 1.1e-7f, 1.0f};
    const float hostile[6] = {-INFINITY, -1.0f, NAN, 1e-30f, INFINITY, FLT_MAX};
    pc_rect rects[4] = {PC_RECT_EMPTY, pc_rect_make(0, 0, 1, 1), pc_rect_make(0, 0, 1, 7),
                        pc_rect_make(0, 0, 9, 1)};
    int ri, al, sh, k;
    for (ri = 0; ri < 4; ri++) {
        for (al = 0; al < 2; al++) {
            pc_algo_image src;
            pc_algo_ctx ctx = {rects[ri], PC_COLOR_MODE_LAB, al != 0};
            size_t i;
            shape_image(&src, rects[ri], 4, al != 0);
            for (i = 0; i < src.len; i++) {
                src.data[i] = !al || i % 4 != 3 ? vals[i % 7] : avals[i % 5];
            }
            for (sh = 0; sh < 9; sh++) {
                pc_algo_spans spans;
                float *a = shape_blur(&src, rects[ri], &ctx, 2.5f, (pc_algo_blur_shape)sh),
                      *b = buf(rects[ri], 4);
                spans_of(2.5f, (pc_algo_blur_shape)sh, &spans);
                if (src.len > 0) {
                    CHECK(pc_algo_conv_spans(&src, rects[ri], &ctx, &spans, b, NULL) == PC_OK);
                    CHECK(exact(a, b, src.len));
                }
                free(a), free(b);
                pc_algo_spans_fini(&spans);
            }
            pc_algo_image_fini(&src);
        }
    }
    for (k = 0; k < 6; k++) {
        pc_rect r = pc_rect_make(0, 0, 3, 2);
        pc_algo_ctx ctx = {r, PC_COLOR_MODE_RGB, true};
        pc_algo_image src;
        size_t i;
        CHECK(pc_algo_image_init(&src, r, 4, NULL) == PC_OK);
        for (i = 0; i < 24; i += 4) {
            src.data[i] = 1e20f, src.data[i + 1] = -17.0f, src.data[i + 2] = 0.0f,
            src.data[i + 3] = hostile[k];
        }
        for (sh = 0; sh < 9; sh++) {
            pc_algo_spans spans;
            float *a = shape_blur(&src, r, &ctx, 2.5f, (pc_algo_blur_shape)sh), *b = buf(r, 4);
            spans_of(2.5f, (pc_algo_blur_shape)sh, &spans);
            CHECK(pc_algo_conv_spans(&src, r, &ctx, &spans, b, NULL) == PC_OK);
            CHECK(exact(a, b, 24));
            free(a), free(b);
            pc_algo_spans_fini(&spans);
        }
        pc_algo_image_fini(&src);
    }
}

static void test_hostile_shape_inputs_are_bounded(void)
{
    pc_rect r = pc_rect_make(0, 0, 1, 1), huge = pc_rect_make(INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX);
    pc_algo_ctx ctx = {r, PC_COLOR_MODE_RGB, true};
    pc_algo_shape_blur_params p = {5.0f, PC_ALGO_SHAPE_CIRCLE};
    const float radii[5] = {NAN, INFINITY, -INFINITY, -1.0f, FLT_MAX};
    pc_algo_filter f;
    pc_algo_image src, bad;
    float nine[9] = {0}, *d = NULL;
    size_t len;
    int k;
    CHECK(pc_algo_filter_make(&PC_ALGO_FILTER_SHAPE_BLUR, &p, &f, NULL) == PC_OK);
    /* The reference returns an empty kernel output for these; C reports an error instead. */
    bad.rect = r, bad.ch = 0, bad.data = NULL, bad.len = 0;
    CHECK(pc_algo_kernel(&f, &bad, r, &ctx, &d, &len, NULL) == PC_OK && len == 0);
    bad.ch = 9, bad.data = nine, bad.len = 9;
    CHECK(pc_algo_kernel(&f, &bad, r, &ctx, &d, &len, NULL) == PC_ERR_INVALID);
    bad.ch = 4, bad.data = NULL, bad.len = 0;
    CHECK(pc_algo_kernel(&f, &bad, r, &ctx, &d, &len, NULL) == PC_ERR_INVALID);
    bad.rect = huge;
    CHECK(pc_algo_kernel(&f, &bad, r, &ctx, &d, &len, NULL) != PC_OK);
    pc_algo_filter_fini(&f);
    shape_image(&src, r, 4, true);
    for (k = 0; k < 5; k++) {
        pc_algo_halo h;
        p.radius = radii[k];
        CHECK(pc_algo_filter_make(&PC_ALGO_FILTER_SHAPE_BLUR, &p, &f, NULL) == PC_OK);
        h = pc_algo_filter_halo(&f);
        CHECK(!h.bounds && h.radius >= 1 && h.radius <= 1001);
        CHECK(pc_algo_kernel(&f, &src, r, &ctx, &d, &len, NULL) == PC_OK && len == 4);
        free(d);
        d = NULL;
        pc_algo_filter_fini(&f);
    }
    p.radius = 5.0f;
    CHECK(pc_algo_filter_make(&PC_ALGO_FILTER_SHAPE_BLUR, &p, &f, NULL) == PC_OK);
    CHECK(pc_algo_kernel(&f, &src, huge, &ctx, &d, &len, NULL) != PC_OK);
    pc_algo_filter_fini(&f);
    pc_algo_image_fini(&src);
}

static bool yes(void *c)
{
    (void)c;
    return true;
}

static void test_shape_tile_overrides_and_cancel_preserve_input(void)
{
    pc_rect rs[2] = {pc_rect_make(0, 0, 4, 4),
                     pc_rect_make(INT32_MAX - 8, INT32_MAX - 8, INT32_MAX - 4, INT32_MAX - 4)};
    const int32_t tiles[5] = {INT32_MIN, -1, 0, 256, INT32_MAX};
    const float px[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    pc_algo_shape_blur_params p = {0.0f, PC_ALGO_SHAPE_CIRCLE};
    pc_interrupt cancel = {yes, NULL, NULL};
    pc_algo_filter f;
    int k, t;
    CHECK(pc_algo_filter_make(&PC_ALGO_FILTER_SHAPE_BLUR, &p, &f, NULL) == PC_OK);
    for (k = 0; k < 2; k++) {
        pc_surface s, o;
        float *before = NULL, *after = NULL;
        size_t lb, la;
        pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
        CHECK(pc_surface_fill_rect(&s, rs[k], px, 4, NULL) == PC_OK);
        CHECK(pc_surface_read_region(&s, rs[k], &before, &lb, NULL) == PC_OK);
        for (t = 0; t < 5; t++) {
            CHECK(pc_algo_apply_tiled(&s, &f, rs[k], rs[k], NULL, tiles[t], &rs[k], &o, NULL) == PC_OK);
            CHECK(pc_surface_read_region(&o, rs[k], &after, &la, NULL) == PC_OK);
            CHECK(la == lb && exact(after, before, la));
            free(after);
            after = NULL;
            pc_surface_fini(&o);
        }
        CHECK(pc_algo_apply_tiled_with(&s, &f, rs[k], rs[k], NULL, 256, &rs[k], &cancel, &o, NULL) ==
              PC_ERR_CANCELLED);
        CHECK(pc_surface_read_region(&s, rs[k], &after, &la, NULL) == PC_OK);
        CHECK(exact(after, before, la));
        free(after), free(before);
        pc_surface_fini(&s);
    }
    pc_algo_filter_fini(&f);
}

/* ---- Smooth and Feather (selection_blur.rs) ---- */

static float *oracle_box(const float *m, size_t w, size_t h, size_t r)
{
    float *tmp = calloc(w * h, sizeof(float)), *out = calloc(w * h, sizeof(float)), win = (float)(2 * r + 1);
    size_t x, y;
    int64_t k;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float s = 0.0f;
            for (k = (int64_t)x - (int64_t)r; k <= (int64_t)x + (int64_t)r; k++) {
                s += m[y * w + (size_t)(k<0 ? 0 : k>(int64_t) w - 1 ? (int64_t)w - 1 : k)];
            }
            tmp[y * w + x] = s / win;
        }
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float s = 0.0f;
            for (k = (int64_t)y - (int64_t)r; k <= (int64_t)y + (int64_t)r; k++) {
                s += tmp[(size_t)(k<0 ? 0 : k>(int64_t) h - 1 ? (int64_t)h - 1 : k) * w + x];
            }
            out[y * w + x] = s / win;
        }
    }
    free(tmp);
    return out;
}

static float *oracle_smooth(const float *m, size_t w, size_t h, float r)
{
    size_t rr = pc_f32_to_size(roundf(pc_rs_maxf_k(r, 0.0f))), i;
    float *o;
    if (rr == 0) {
        o = malloc(w * h * sizeof(float));
        memcpy(o, m, w * h * sizeof(float));
    } else {
        o = oracle_box(m, w, h, rr);
    }
    for (i = 0; i < w * h; i++) {
        o[i] = pc_clampf((o[i] - 0.5f) * 4.0f + 0.5f, 0.0f, 1.0f);
    }
    return o;
}

static float *oracle_feather(const float *m, size_t w, size_t h, float radius)
{
    float sigma = pc_rs_maxf_k(radius, 0.0f) / 2.0f, s = 0.0f, *k, *tmp, *out;
    int64_t r, i, x, y;
    out = calloc(w * h + 1, sizeof(float));
    if (sigma < 0.1f) {
        memcpy(out, m, w * h * sizeof(float));
        return out;
    }
    r = (int64_t)ceilf(sigma * 3.0f);
    k = malloc((size_t)(2 * r + 1) * sizeof(float));
    for (i = -r; i <= r; i++) {
        k[i + r] = expf((float)(-(i * i)) / (2.0f * sigma * sigma));
        s += k[i + r];
    }
    for (i = 0; i <= 2 * r; i++) {
        k[i] /= s;
    }
    tmp = calloc(w * h + 1, sizeof(float));
    for (y = 0; y < (int64_t)h; y++) {
        for (x = 0; x < (int64_t)w; x++) {
            float a = 0.0f;
            for (i = 0; i <= 2 * r; i++) {
                int64_t xx = x + i - r;
                if (xx >= 0 && xx < (int64_t)w) {
                    a += m[(size_t)y * w + (size_t)xx] * k[i];
                }
            }
            tmp[(size_t)y * w + (size_t)x] = a;
        }
    }
    for (y = 0; y < (int64_t)h; y++) {
        for (x = 0; x < (int64_t)w; x++) {
            float a = 0.0f;
            for (i = 0; i <= 2 * r; i++) {
                int64_t yy = y + i - r;
                if (yy >= 0 && yy < (int64_t)h) {
                    a += tmp[(size_t)yy * w + (size_t)x] * k[i];
                }
            }
            out[(size_t)y * w + (size_t)x] = a;
        }
    }
    free(k), free(tmp);
    return out;
}

/* Masks of several kinds: binary noise, soft noise, a soft-edged blob, sparse dots, a rectangle. */
static float *test_mask(int kind, size_t w, size_t h, uint32_t seed)
{
    float *m = malloc(w * h * sizeof(float) + 4);
    uint32_t s = seed * 2654435761u + 1u;
    float cx = (float)w * (float)(xs(&s) % 1000) / 1000.0f, cy = (float)h * (float)(xs(&s) % 1000) / 1000.0f;
    float rad = pc_rs_maxf_k((float)(w < h ? w : h) * 0.4f, 1.0f);
    size_t i;
    for (i = 0; i < w * h; i++) {
        float u = (float)(xs(&s) % 100000) / 100000.0f;
        float x = (float)(i % w) + 0.5f, y = (float)(i / w) + 0.5f;
        switch (kind) {
        case 0:
            m[i] = u < 0.5f ? 1.0f : 0.0f;
            break;
        case 1:
            m[i] = u;
            break;
        case 2:
            m[i] = pc_clampf(rad - sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)), 0.0f, 1.0f);
            break;
        case 3:
            m[i] = u < 0.02f ? 1.0f : 0.0f;
            break;
        default:
            m[i] = (i % w) * 3 >= w && (i / w) * 4 < h * 3 ? 1.0f : 0.0f;
        }
    }
    return m;
}

static float sel_diff(bool feather, const float *m, size_t w, size_t h, float r)
{
    float *a = NULL, *b = feather ? oracle_feather(m, w, h, r) : oracle_smooth(m, w, h, r), d;
    CHECK((feather ? pc_algo_selection_feather(m, w * h, w, h, r, &a, NULL)
                   : pc_algo_selection_smooth(m, w * h, w, h, r, &a, NULL)) == PC_OK);
    d = a != NULL ? max_diff_v(a, b, w * h) : 1.0f;
    free(a), free(b);
    return d;
}

static void test_feather_and_smooth_match_direct_definitions(void)
{
    static const size_t dims[3][2] = {{97, 61}, {40, 150}, {13, 7}};
    static const float fr[11] = {0.1f, 0.3f, 1.0f, 2.5f, 8.0f, 9.0f, 17.0f, 25.0f, 50.0f, 120.0f, 1000.0f};
    static const float sr[8] = {0.0f, 0.4f, 1.0f, 2.0f, 5.0f, 16.0f, 50.0f, 500.0f};
    int d, kind, k;
    for (d = 0; d < 3; d++) {
        for (kind = 0; kind < 5; kind++) {
            float *m = test_mask(kind, dims[d][0], dims[d][1], (uint32_t)d + 1);
            for (k = 0; k < 11; k++) {
                float e = sel_diff(true, m, dims[d][0], dims[d][1], fr[k]);
                if (!(e <= 1.0f / 255.0f)) {
                    PC_T_FAIL("feather kind %d %zux%zu r %g: %g", kind, dims[d][0], dims[d][1], (double)fr[k],
                              (double)e);
                }
            }
            for (k = 0; k < 8; k++) {
                float e = sel_diff(false, m, dims[d][0], dims[d][1], sr[k]);
                if (!(e <= 1.0f / 255.0f)) {
                    PC_T_FAIL("smooth kind %d %zux%zu r %g: %g", kind, dims[d][0], dims[d][1], (double)sr[k],
                              (double)e);
                }
            }
            free(m);
        }
    }
}

static void test_selection_edge_cases(void)
{
    size_t n = 50 * 40, i;
    float *z = calloc(n, sizeof(float)), *dot = calloc(n, sizeof(float)), *one = malloc(n * sizeof(float));
    float *m = malloc(n * sizeof(float)), *o = NULL;
    const float hostile[5] = {NAN, INFINITY, -INFINITY, -5.0f, 1e30f}, rs[4] = {0.0f, 1.0f, 50.0f, 1000.0f};
    float single = 1.0f;
    int k;
    for (i = 0; i < n; i++) {
        one[i] = 1.0f;
        m[i] = (float)(i % 7) / 6.0f;
    }
    dot[20 * 50 + 25] = 1.0f;
    CHECK(pc_algo_selection_feather(z, n, 50, 40, 30.0f, &o, NULL) == PC_OK && memcmp(o, z, n * 4) == 0);
    free(o);
    CHECK(pc_algo_selection_smooth(z, n, 50, 40, 10.0f, &o, NULL) == PC_OK && memcmp(o, z, n * 4) == 0);
    free(o);
    CHECK(pc_algo_selection_feather(dot, n, 50, 40, 4.0f, &o, NULL) == PC_OK && o[0] == 0.0f &&
          o[20 * 50 + 25] > 0.0f);
    free(o);
    CHECK(pc_algo_selection_smooth(one, n, 50, 40, 25.0f, &o, NULL) == PC_OK && memcmp(o, one, n * 4) == 0);
    free(o);
    CHECK(pc_algo_selection_feather(one, n, 50, 40, 6.0f, &o, NULL) == PC_OK);
    CHECK(fabsf(o[20 * 50 + 25] - 1.0f) < 1e-5f && o[0] < 0.5f);
    free(o);
    CHECK(sel_diff(true, one, 50, 40, 6.0f) <= 1.0f / 255.0f);
    CHECK(sel_diff(true, one, 50, 40, 30.0f) <= 1.0f / 255.0f);
    CHECK(pc_algo_selection_feather(m, n, 50, 40, 0.0f, &o, NULL) == PC_OK && memcmp(o, m, n * 4) == 0);
    free(o);
    CHECK(sel_diff(false, m, 50, 40, 0.0f) == 0.0f);
    for (k = 0; k < 4; k++) {
        CHECK(sel_diff(true, &single, 1, 1, rs[k]) <= 1.0f / 255.0f);
        CHECK(pc_algo_selection_smooth(&single, 1, 1, 1, rs[k], &o, NULL) == PC_OK && o[0] == 1.0f);
        free(o);
    }
    CHECK(pc_algo_selection_feather(NULL, 0, 0, 0, 5.0f, &o, NULL) == PC_OK);
    free(o);
    CHECK(pc_algo_selection_smooth(NULL, 0, 0, 5, 5.0f, &o, NULL) == PC_OK);
    free(o);
    for (k = 0; k < 5; k++) {
        CHECK(pc_algo_selection_feather(dot, n, 50, 40, hostile[k], &o, NULL) == PC_OK);
        for (i = 0; i < n; i++) {
            CHECK(isfinite(o[i]));
        }
        free(o);
        CHECK(pc_algo_selection_smooth(dot, n, 50, 40, hostile[k], &o, NULL) == PC_OK);
        free(o);
    }
    CHECK(pc_algo_selection_feather(dot, n, 50, 41, 5.0f, &o, NULL) == PC_OK);
    free(o);
    CHECK(pc_algo_selection_smooth(dot, n, 49, 40, 5.0f, &o, NULL) == PC_OK);
    free(o);
    free(z), free(dot), free(one), free(m);
}

static void test_cosine_fit_is_used_and_accurate(void)
{
    const double sigmas[3] = {7.0, 25.0, 125.0};
    int k;
    for (k = 0; k < 3; k++) {
        CHECK(pc_algo_feather_kernel_is_cos(sigmas[k], (size_t)ceil(sigmas[k] * 3.0), 4000));
    }
}

/* ---- never crash ---- */

static void test_hostile_parameters_never_crash(void)
{
    static const char *const tags[15] = {"gaussianBlur", "boxBlur",     "motionBlur",       "radialBlur",
                                         "surfaceBlur",  "unsharpMask", "smartSharpen",     "highPass",
                                         "addNoise",     "median",      "dustAndScratches", "reduceNoise",
                                         "smartBlur",    "lensBlur",    "shapeBlur"};
    const float vals[8] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f, 1e9f, 3e5f, -0.0f};
    pc_rect r = pc_rect_make(0, 0, 24, 18);
    pc_surface s;
    size_t t, fi;
    int v;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    {
        const float px[4] = {0.3f, 0.6f, 0.2f, 1.0f};
        CHECK(pc_surface_fill_rect(&s, pc_rect_make(2, 2, 20, 15), px, 4, NULL) == PC_OK);
    }
    for (t = 0; t < 15; t++) {
        const pc_algo_filter_def *def = pc_algo_filter_def_find(tags[t]);
        CHECK(def != NULL);
        if (def == NULL) {
            continue;
        }
        for (fi = 0; fi < def->nfields; fi++) {
            if (def->fields[fi].kind != PC_ALGO_JF_F32) {
                continue;
            }
            for (v = 0; v < 8; v++) {
                pc_algo_filter f;
                pc_surface o;
                pc_status st;
                pc_rect area;
                CHECK(pc_algo_filter_make(def, NULL, &f, NULL) == PC_OK);
                memcpy((char *)f.params + def->fields[fi].off, &vals[v], sizeof(float));
                area = pc_algo_output_area(&f, pc_rect_make(2, 2, 20, 15), r, NULL);
                st = pc_algo_apply_in(&s, &f, area, r, NULL, r, &o, NULL);
                CHECK(st == PC_OK || st == PC_ERR_LIMIT || st == PC_ERR_NOMEM);
                if (st == PC_OK) {
                    pc_surface_fini(&o);
                }
                pc_algo_filter_fini(&f);
            }
        }
    }
    pc_surface_fini(&s);
}

int main(int argc, char **argv)
{
    RUN(test_surface_blur_ranked_sums_match_the_weighted_window_at_any_depth);
    RUN(test_surface_blur_ranked_declines_non_finite_samples_and_empty_output);
    RUN(test_median_histogram_matches_the_direct_selection);
    RUN(test_binary_kernel_sampling_including_fractional_square);
    RUN(test_scalar_prefix_exact_all_shapes_radii_channels_subrects);
    RUN(test_point_convolution_independent_of_prefix);
    RUN(test_surfaces_exact_depth_models_alpha_selection_and_tile_boundaries);
    RUN(test_largest_supported_radius_and_fraction_keep_scalar_kernel);
    RUN(test_degenerate_hdr_and_nonfinite_samples_keep_scalar_behaviour);
    RUN(test_hostile_shape_inputs_are_bounded);
    RUN(test_shape_tile_overrides_and_cancel_preserve_input);
    RUN(test_feather_and_smooth_match_direct_definitions);
    RUN(test_selection_edge_cases);
    RUN(test_cosine_fit_is_used_and_accurate);
    RUN(test_hostile_parameters_never_crash);
    return pc_t_finish();
}
