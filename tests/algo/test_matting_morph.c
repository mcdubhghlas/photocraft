/* Octagonal morphology behind Shift Edge (port of crates/algo/src/matting/morphology/tests.rs).
 *
 * The C port has no `tiled` path (the reference tiles large masks only to bound scratch memory;
 * the values are those of the dense transform) and no separate `parallel_square` (the square pass
 * is pc_algo_matting_max_square), so the tests comparing those against `dense` / `max_square`
 * compare pc_algo_matting_morph and pc_algo_matting_max_square against the step-by-step and
 * naive definitions instead. The C pool size is fixed per process: the worker-count tests run
 * at PHOTOCRAFT_THREADS (the gates run 1 and 8) against single-threaded oracles.
 *
 * Not ported: octagonal_morphology_release_comparison, tiled_refinement_release_comparison and
 * dense_matte_release_comparison (#[ignore]d release timings on 24-36 MP masks). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "base/num.h"
#include "base/rmath.h"
#include "harness.h"

static float *morph(const float *src, size_t w, size_t h, size_t r, bool grow)
{
    float *o = NULL;
    CHECK(pc_algo_matting_morph(src, w, h, r, grow, &o, NULL) == PC_OK);
    return o;
}

static float *repeated(const float *src, size_t w, size_t h, size_t r, bool grow)
{
    float *o = NULL;
    CHECK(pc_algo_matting_morph_repeated(src, w, h, r, grow, &o, NULL) == PC_OK);
    return o;
}

/* Bit-identical (NaN payloads and zero signs included). */
static bool same_bits(const float *a, const float *b, size_t n)
{
    return a != NULL && b != NULL && (n == 0 || memcmp(a, b, n * sizeof(float)) == 0);
}

static bool morph_is_repeated(const float *src, size_t w, size_t h, size_t r, bool grow)
{
    float *a = morph(src, w, h, r, grow), *b = repeated(src, w, h, r, grow);
    bool ok = same_bits(a, b, w * h);
    free(a);
    free(b);
    return ok;
}

static void test_every_small_binary_mask_matches_repeated_octagons(void)
{
    static const size_t dims[6][2] = {{1, 1}, {1, 7}, {7, 1}, {2, 3}, {3, 3}, {4, 3}};
    static const size_t radii[9] = {0, 1, 2, 3, 4, 5, 7, 10, 13};
    float src[12];
    size_t k, bits, i, ri;
    int grow, bad = 0;
    for (k = 0; k < 6; k++) {
        size_t w = dims[k][0], h = dims[k][1];
        for (bits = 0; bits < (size_t)1 << (w * h); bits++) {
            for (i = 0; i < w * h; i++) {
                src[i] = (float)((bits >> i) & 1u);
            }
            for (ri = 0; ri < 9; ri++) {
                for (grow = 0; grow < 2; grow++) {
                    if (!morph_is_repeated(src, w, h, radii[ri], grow)) {
                        bad++;
                        PC_T_FAIL("%zux%zu bits=%zu r=%zu grow=%d", w, h, bits, radii[ri], grow);
                    }
                }
            }
        }
    }
    CHECK_EQ_INT(bad, 0);
}

static void test_gray_and_hdr_values_match_at_edges_and_every_radius(void)
{
    static const size_t dims[5][2] = {{2, 29}, {29, 2}, {37, 29}, {1, 51}, {51, 1}};
    size_t k, i, r;
    int depth, grow, bad = 0;
    for (k = 0; k < 5; k++) {
        size_t w = dims[k][0], h = dims[k][1];
        float *src = malloc(w * h * sizeof(float));
        for (depth = 0; depth < 3; depth++) {
            for (i = 0; i < w * h; i++) {
                float v = (float)((i * 101 + i / w * 37) % 251) / 250.0f;
                src[i] = depth == 0   ? roundf(v * 255.0f) / 255.0f
                         : depth == 1 ? roundf(v * 65535.0f) / 65535.0f
                                      : v * 4.0f - 1.0f;
            }
            for (r = 0; r <= 65; r++) {
                for (grow = 0; grow < 2; grow++) {
                    if (!morph_is_repeated(src, w, h, r, grow)) {
                        bad++;
                        PC_T_FAIL("%zux%zu depth=%d r=%zu grow=%d", w, h, depth, r, grow);
                    }
                }
            }
        }
        free(src);
    }
    CHECK_EQ_INT(bad, 0);
}

static size_t sat_sub(size_t a, size_t b)
{
    return a > b ? a - b : 0;
}

static size_t abs_diff(size_t a, size_t b)
{
    return a > b ? a - b : b - a;
}

static void test_impulses_match_the_closed_octagonal_footprint(void)
{
    static const size_t seeds[5][2] = {{0, 0}, {0, 41}, {48, 41}, {96, 82}, {5, 79}};
    const size_t w = 97, h = 83;
    float *src = malloc(w * h * sizeof(float));
    size_t k, r, x, y, i;
    int grow, bad = 0;
    for (k = 0; k < 5; k++) {
        size_t sx = seeds[k][0], sy = seeds[k][1];
        for (r = 1; r <= 65; r++) {
            for (grow = 1; grow >= 0; grow--) {
                size_t a = r / 2 + r % 2, b = r / 2;
                float *out;
                for (i = 0; i < w * h; i++) {
                    src[i] = grow ? 0.0f : 1.0f;
                }
                src[sy * w + sx] = grow ? 1.0f : 0.0f;
                out = morph(src, w, h, r, grow);
                for (y = 0; out != NULL && y < h; y++) {
                    for (x = 0; x < w; x++) {
                        bool affected = sat_sub(abs_diff(x, sx), a) + sat_sub(abs_diff(y, sy), a) <= b;
                        float want = affected == (grow != 0) ? 1.0f : 0.0f;
                        if (out[y * w + x] != want) {
                            bad++;
                        }
                    }
                }
                free(out);
            }
        }
    }
    CHECK_EQ_INT(bad, 0);
    free(src);
}

static float from_bits(uint32_t b)
{
    float f;
    memcpy(&f, &b, sizeof f);
    return f;
}

static void test_exceptional_floats_keep_the_old_visitation_order(void)
{
    static const size_t radii[5] = {1, 3, 4, 17, 64};
    const size_t w = 13, h = 9, n = w * h;
    float specials[4], src[13 * 9];
    size_t k, i, ri;
    int grow;
    float *o;
    specials[0] = from_bits(0x7fc00001u);
    specials[1] = -INFINITY;
    specials[2] = INFINITY;
    specials[3] = -0.0f;
    for (k = 0; k < 4; k++) {
        for (i = 0; i < n; i++) {
            src[i] = i % 7 == 0 ? specials[k] : (float)(i % 11) / 10.0f;
        }
        for (ri = 0; ri < 5; ri++) {
            for (grow = 0; grow < 2; grow++) {
                if (!morph_is_repeated(src, w, h, radii[ri], grow)) {
                    PC_T_FAIL("special %zu r=%zu grow=%d", k, radii[ri], grow);
                }
            }
        }
    }
    for (i = 0; i < n; i++) {
        src[i] = from_bits(0x7fc00002u);
    }
    o = morph(src, w, h, 17, true);
    CHECK(same_bits(o, src, n));
    free(o);
}

/* The reference returns an empty vector for impossible sizes and short buffers and keeps values
 * past w * h; in C buffers have no length (only w * h values are read), and impossible sizes are
 * PC_ERR_LIMIT. */
static void test_invalid_sizes_and_large_radii_are_bounded(void)
{
    const float four[4] = {0.0f, 0.5f, 1.0f, 0.1f};
    float *o = NULL;
    size_t i;
    bool ok = true;
    CHECK(pc_algo_matting_morph(four, SIZE_MAX, 2, 3, true, &o, NULL) == PC_ERR_LIMIT && o == NULL);
    CHECK(pc_algo_matting_morph(NULL, 0, 5, SIZE_MAX, true, &o, NULL) == PC_OK);
    free(o);
    o = morph(four, 2, 2, SIZE_MAX, true);
    for (i = 0; o != NULL && i < 4; i++) {
        ok &= o[i] == 1.0f;
    }
    CHECK(o != NULL && ok);
    free(o);
    o = morph(four, 2, 2, 5, true);
    ok = true;
    for (i = 0; o != NULL && i < 4; i++) {
        ok &= o[i] == 1.0f;
    }
    CHECK(o != NULL && ok);
    free(o);
}

static void test_saturated_radius_does_not_pad_a_thin_axis_to_the_long_axis(void)
{
    const size_t w = 2, h = 30001, n = w * h;
    float *src = malloc(n * sizeof(float)), *g, *s;
    size_t i, bad = 0;
    for (i = 0; i < n; i++) {
        src[i] = (float)(i % 251);
    }
    g = morph(src, w, h, SIZE_MAX, true);
    s = morph(src, w, h, SIZE_MAX, false);
    for (i = 0; g != NULL && s != NULL && i < n; i++) {
        bad += g[i] != 250.0f || s[i] != 0.0f;
    }
    CHECK(g != NULL && s != NULL);
    CHECK_EQ_UINT(bad, 0);
    free(g);
    free(s);
    free(src);
}

static float *ramp(size_t w, size_t h)
{
    float *src = malloc(w * h * sizeof(float));
    size_t i;
    for (i = 0; i < w * h; i++) {
        src[i] = (float)((i * 101 + i / w * 37) % 251) / 250.0f;
    }
    return src;
}

static void test_worker_counts_are_bit_identical(void)
{
    const size_t w = 769, h = 351;
    float *src = ramp(w, h);
    CHECK(morph_is_repeated(src, w, h, 33, false));
    free(src);
}

/* Square extremum, one axis then the other, by direct windows. */
static float *naive_square(const float *src, size_t w, size_t h, size_t r, bool grow)
{
    float *tmp = malloc(w * h * sizeof(float)), *out = malloc(w * h * sizeof(float));
    size_t x, y, k;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float v = src[y * w + x];
            for (k = sat_sub(x, r); k <= x + r && k < w; k++) {
                v = grow ? pc_rs_maxf(v, src[y * w + k]) : pc_rs_minf(v, src[y * w + k]);
            }
            tmp[y * w + x] = v;
        }
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float v = tmp[y * w + x];
            for (k = sat_sub(y, r); k <= y + r && k < h; k++) {
                v = grow ? pc_rs_maxf(v, tmp[k * w + x]) : pc_rs_minf(v, tmp[k * w + x]);
            }
            out[y * w + x] = v;
        }
    }
    free(tmp);
    return out;
}

static void test_parallel_square_matches_serial_for_partial_blocks_and_thin_images(void)
{
    static const size_t dims[4][2] = {{769, 351}, {513, 512}, {3, 90001}, {90001, 3}};
    static const size_t radii[5] = {0, 1, 3, 16, 32};
    size_t k, ri;
    int grow;
    for (k = 0; k < 4; k++) {
        size_t w = dims[k][0], h = dims[k][1];
        float *src = ramp(w, h);
        for (ri = 0; ri < 5; ri++) {
            for (grow = 1; grow >= 0; grow--) {
                float *a = NULL, *b = naive_square(src, w, h, radii[ri], grow);
                CHECK(pc_algo_matting_max_square(src, w, h, radii[ri], grow, &a, NULL) == PC_OK);
                if (!same_bits(a, b, w * h)) {
                    PC_T_FAIL("%zux%zu r=%zu grow=%d", w, h, radii[ri], grow);
                }
                free(a);
                free(b);
            }
        }
        free(src);
    }
}

static void test_shifted_tiles_match_a_whole_mask_at_canvas_edges(void)
{
    static const float shifts[4] = {-100.0f, -51.0f, 51.0f, 100.0f};
    const size_t w = 611, h = 317;
    pc_algo_rgb_image img;
    pc_algo_image_sampler hold = {&img, 0, 0};
    pc_algo_sampler s = pc_algo_image_sampler_make(&hold);
    pc_algo_region region;
    float *input = malloc(w * h * sizeof(float));
    size_t i, x, y;
    int k;
    (void)pc_algo_rgb_image_init(&img, w, h, NULL);
    for (i = 0; i < w * h; i++) {
        img.px[i][0] = img.px[i][1] = img.px[i][2] = 0.5f;
    }
    region.bbox = pc_rect_make(0, 0, (int32_t)w, (int32_t)h);
    region.len = w * h;
    region.mask = malloc(region.len);
    for (i = 0; i < w * h; i++) {
        region.mask[i] = (i % w < 63 && i / w < 191) || (i % w > 280 && i / w > 171) ? 255 : 0;
        input[i] = (float)region.mask[i] / 255.0f;
    }
    for (k = 0; k < 4; k++) {
        /* A constant guide keeps this check independent of image detail. */
        pc_algo_refine_params p = pc_algo_refine_params_default();
        pc_algo_region tiled;
        float *whole = NULL;
        size_t bad = 0;
        p.radius = 32.0f;
        p.shift_edge = shifts[k];
        CHECK(pc_algo_matting_refine_buffer(&img, input, w, h, &p, &whole, NULL) == PC_OK);
        CHECK(pc_algo_matting_refine_mask(&s, pc_algo_matting_region_reader, &region, region.bbox,
                                          region.bbox, &p, &tiled, NULL) == PC_OK);
        for (y = 0; whole != NULL && y < h; y++) {
            for (x = 0; x < w; x++) {
                bad += !(fabsf(pc_algo_region_at(&tiled, (int32_t)x, (int32_t)y) - whole[y * w + x]) <=
                         1.5f / 255.0f);
            }
        }
        if (bad) {
            PC_T_FAIL("shift=%g: %zu pixels differ", (double)shifts[k], bad);
        }
        free(whole);
        pc_algo_region_fini(&tiled);
    }
    free(input);
    pc_algo_region_fini(&region);
    pc_algo_rgb_image_fini(&img);
}

static void test_bounded_tiles_match_dense_windows_on_partial_cores_and_fractional_values(void)
{
    static const size_t radii[4] = {3, 16, 33, 64};
    static const float values[5] = {0.0f, 0.5f, 1.0f, INFINITY, -INFINITY};
    const size_t w = 1031, h = 1021, n = w * h;
    float *src = malloc(n * sizeof(float));
    size_t i, ri;
    int kind, grow, k;
    for (kind = 0; kind < 2; kind++) {
        for (i = 0; i < n; i++) {
            size_t x = i % w, y = i / w;
            src[i] = kind == 0 ? (float)x / 1000.0f + (float)y / 3000.0f - 0.3f
                     : (abs_diff(x, 255) + abs_diff(y, 127)) < 35 || (x == 768 && y == 896) ? 0.75f
                                                                                            : 0.0f;
        }
        for (ri = 0; ri < 4; ri++) {
            for (grow = 0; grow < 2; grow++) {
                if (!morph_is_repeated(src, w, h, radii[ri], grow)) {
                    PC_T_FAIL("kind=%d r=%zu grow=%d", kind, radii[ri], grow);
                }
            }
        }
    }
    for (k = 0; k < 5; k++) {
        for (i = 0; i < n; i++) {
            src[i] = values[k];
        }
        for (grow = 1; grow >= 0; grow--) {
            float *o = morph(src, w, h, 64, grow);
            CHECK(same_bits(o, src, n));
            free(o);
        }
    }
    free(src);
}

int main(int argc, char **argv)
{
    RUN(test_every_small_binary_mask_matches_repeated_octagons);
    RUN(test_gray_and_hdr_values_match_at_edges_and_every_radius);
    RUN(test_impulses_match_the_closed_octagonal_footprint);
    RUN(test_exceptional_floats_keep_the_old_visitation_order);
    RUN(test_invalid_sizes_and_large_radii_are_bounded);
    RUN(test_saturated_radius_does_not_pad_a_thin_axis_to_the_long_axis);
    RUN(test_worker_counts_are_bit_identical);
    RUN(test_parallel_square_matches_serial_for_partial_blocks_and_thin_images);
    RUN(test_shifted_tiles_match_a_whole_mask_at_canvas_edges);
    RUN(test_bounded_tiles_match_dense_windows_on_partial_cores_and_fractional_values);
    return pc_t_finish();
}
