/* Tests for src/algo/matting.c (ports of the #[test]s in crates/algo/src/matting.rs) plus
 * never-crash checks. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "base/num.h"
#include "base/rmath.h"
#include "harness.h"

static float erf_approx(float x)
{
    float t = 1.0f / (1.0f + 0.3275911f * fabsf(x));
    float y =
        1.0f - (((((1.0614054f * t - 1.4531521f) * t) + 1.4214137f) * t - 0.28449674f) * t + 0.2548296f) * t *
                   expf(-x * x);
    return x >= 0.0f ? y : -y;
}

/* Horizontal blend from a (left) to b (right) across a Gaussian-blurred edge at x = 40. */
static float *blurred_edge(size_t w, size_t h, float sigma, pc_algo_rgb_image *img)
{
    static const float a[3] = {0.9f, 0.8f, 0.2f}, b[3] = {0.1f, 0.2f, 0.6f};
    float *alpha = malloc(w * sizeof(float));
    size_t x, y;
    int c;
    for (x = 0; x < w; x++) {
        alpha[x] = 0.5f * (1.0f - erf_approx(((float)x + 0.5f - 40.0f) / (sigma * 1.41421356f)));
    }
    (void)pc_algo_rgb_image_init(img, w, h, NULL);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            for (c = 0; c < 3; c++) {
                img->px[y * w + x][c] = a[c] * alpha[x] + b[c] * (1.0f - alpha[x]);
            }
        }
    }
    return alpha;
}

static void test_max_square_matches_naive(void)
{
    size_t w = 17, h = 11, i, x, y, xx, yy, r;
    float src[17 * 11], *fast = NULL;
    int mx;
    bool ok = true;
    for (i = 0; i < w * h; i++) {
        src[i] = (float)((i * 37) % 23);
    }
    for (r = 1; r <= 5; r += (r == 2 ? 3 : 1)) {
        for (mx = 0; mx < 2; mx++) {
            (void)pc_algo_matting_max_square(src, w, h, r, mx != 0, &fast, NULL);
            for (y = 0; y < h; y++) {
                for (x = 0; x < w; x++) {
                    float v = mx ? -3.4e38f : 3.4e38f;
                    for (yy = y > r ? y - r : 0; yy < (y + r + 1 < h ? y + r + 1 : h); yy++) {
                        for (xx = x > r ? x - r : 0; xx < (x + r + 1 < w ? x + r + 1 : w); xx++) {
                            v = mx ? pc_rs_maxf(v, src[yy * w + xx]) : pc_rs_minf(v, src[yy * w + xx]);
                        }
                    }
                    ok &= fast[y * w + x] == v;
                }
            }
            free(fast);
        }
    }
    CHECK(ok);
}

static void test_box_mean_matches_naive(void)
{
    size_t w = 13, h = 9, i, x, y, xx, yy;
    float src[13 * 9], *fast = NULL;
    bool ok = true;
    for (i = 0; i < w * h; i++) {
        src[i] = (float)((i * 7) % 11);
    }
    (void)pc_algo_matting_box_mean(src, w, h, 2, &fast, NULL);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float s = 0, n = 0;
            for (yy = y > 2 ? y - 2 : 0; yy < (y + 3 < h ? y + 3 : h); yy++) {
                for (xx = x > 2 ? x - 2 : 0; xx < (x + 3 < w ? x + 3 : w); xx++) {
                    s += src[yy * w + xx];
                    n += 1;
                }
            }
            ok &= fabsf(fast[y * w + x] - s / n) < 1e-4f;
        }
    }
    CHECK(ok);
    free(fast);
}

static void test_guided_filter_matting_softens_blurred_edge(void)
{
    size_t w = 80, h = 20, i, x, soft = 0;
    pc_algo_rgb_image img, sharp;
    float *truth = blurred_edge(w, h, 3.0f, &img), *m = malloc(w * h * sizeof(float)), *a = NULL, *row;
    pc_algo_refine_params p = pc_algo_refine_params_default();
    bool close = true, mono = true;
    for (i = 0; i < w * h; i++) {
        m[i] = i % w < 40 ? 1.0f : 0.0f;
    }
    p.radius = 10.0f;
    CHECK(pc_algo_matting_refine_buffer(&img, m, w, h, &p, &a, NULL) == PC_OK);
    row = a + 10 * w;
    for (x = 0; x < w; x++) {
        soft += row[x] > 0.05f && row[x] < 0.95f;
        close &= fabsf(row[x] - truth[x]) < 0.12f;
        if (x > 0) {
            mono &= row[x] <= row[x - 1] + 0.02f;
        }
    }
    CHECK(soft >= 6);
    CHECK(close);
    CHECK(mono);
    CHECK(row[5] == 1.0f && row[75] == 0.0f);
    free(a);
    (void)pc_algo_rgb_image_init(&sharp, w, h, NULL);
    for (i = 0; i < w * h; i++) {
        bool l = i % w < 40;
        sharp.px[i][0] = l ? 0.9f : 0.1f;
        sharp.px[i][1] = l ? 0.8f : 0.2f;
        sharp.px[i][2] = l ? 0.2f : 0.6f;
    }
    p.smart_radius = true;
    (void)pc_algo_matting_refine_buffer(&sharp, m, w, h, &p, &a, NULL);
    soft = 0;
    for (x = 0; x < w; x++) {
        soft += a[10 * w + x] > 0.05f && a[10 * w + x] < 0.95f;
    }
    CHECK(soft <= 2);
    free(a);
    free(m);
    free(truth);
    pc_algo_rgb_image_fini(&img);
    pc_algo_rgb_image_fini(&sharp);
}

static void test_feather_contrast_and_shift(void)
{
    size_t w = 60, h = 10, i;
    float *m = malloc(w * h * sizeof(float)), *f = NULL, *c = NULL, *g = NULL;
    pc_algo_refine_params p = pc_algo_refine_params_default();
    for (i = 0; i < w * h; i++) {
        m[i] = i % w < 30 ? 1.0f : 0.0f;
    }
    p.feather = 6.0f;
    (void)pc_algo_matting_refine_buffer(NULL, m, w, h, &p, &f, NULL);
    CHECK(f[5 * w + 29] > 0.5f && f[5 * w + 29] < 0.8f && f[5 * w + 31] > 0.1f);
    p = pc_algo_refine_params_default();
    p.contrast = 100.0f;
    (void)pc_algo_matting_refine_buffer(NULL, f, w, h, &p, &c, NULL);
    CHECK(c[5 * w + 29] == 1.0f && c[5 * w + 31] == 0.0f);
    free(c);
    p = pc_algo_refine_params_default();
    p.shift_edge = 100.0f;
    (void)pc_algo_matting_refine_buffer(NULL, m, w, h, &p, &g, NULL);
    CHECK(g[5 * w + 33] == 1.0f && g[5 * w + 34] == 0.0f);
    free(g);
    p.shift_edge = -50.0f;
    (void)pc_algo_matting_refine_buffer(NULL, m, w, h, &p, &g, NULL);
    CHECK(g[5 * w + 27] == 1.0f && g[5 * w + 28] == 0.0f);
    free(g);
    p = pc_algo_refine_params_default();
    p.smooth = 50.0f;
    (void)pc_algo_matting_refine_buffer(NULL, m, w, h, &p, &g, NULL);
    CHECK(g[5 * w + 10] == 1.0f && g[5 * w + 50] == 0.0f);
    free(g);
    free(f);
    free(m);
}

static void test_refine_mask_tiles_match_single_buffer(void)
{
    size_t w = 600, h = 300, x, y, i;
    pc_algo_rgb_image img;
    float *alpha = blurred_edge(w, h, 2.0f, &img), *whole = malloc(w * h * sizeof(float)), *full = NULL,
          maxd = 0;
    pc_algo_image_sampler hold = {&img, 0, 0};
    pc_algo_sampler s = pc_algo_image_sampler_make(&hold);
    pc_algo_region reg, out;
    pc_algo_refine_params p = pc_algo_refine_params_default();
    reg.bbox = pc_rect_make(0, 0, (int32_t)w, (int32_t)h);
    reg.len = w * h;
    reg.mask = malloc(reg.len);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float dx = (float)x - 40.0f, dy = (float)y - 150.0f;
            reg.mask[y * w + x] = dx * dx * 0.05f + dy * dy < 120.0f * 120.0f || x < 40 ? 255 : 0;
        }
    }
    p.radius = 6.0f;
    p.feather = 2.0f;
    p.contrast = 10.0f;
    CHECK(pc_algo_matting_refine_mask(&s, pc_algo_matting_region_reader, &reg, reg.bbox, reg.bbox, &p, &out,
                                      NULL) == PC_OK);
    CHECK(!pc_algo_region_is_none(&out));
    for (i = 0; i < w * h; i++) {
        whole[i] = (float)reg.mask[i] / 255.0f;
    }
    (void)pc_algo_matting_refine_buffer(&img, whole, w, h, &p, &full, NULL);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            maxd = pc_rs_maxf(maxd, fabsf(pc_algo_region_at(&out, (int32_t)x, (int32_t)y) - full[y * w + x]));
        }
    }
    CHECK(maxd <= 1.5f / 255.0f);
    free(full);
    free(whole);
    free(alpha);
    pc_algo_region_fini(&out);
    pc_algo_region_fini(&reg);
    pc_algo_rgb_image_fini(&img);
}

static void test_decontaminate_and_masked_copy_any_depth(void)
{
    static const pc_sample_type samples[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    const float red[4] = {1, 0, 0, 1}, mix[4] = {0.5f, 0, 0.5f, 1}, blue[4] = {0, 0, 1, 1}, half = 0.5f;
    pc_algo_region a, a2;
    size_t i, k;
    pc_surface g, c;
    float p[5], q[5];
    a.bbox = pc_rect_make(0, 0, 20, 8);
    a.len = 160;
    a.mask = malloc(160);
    for (i = 0; i < 160; i++) {
        a.mask[i] = i % 20 < 10 ? 255 : (i % 20 == 10 ? 128 : 0);
    }
    for (k = 0; k < 3; k++) {
        pc_pixel_format fmt = {PC_COLOR_MODE_RGB, samples[k], true};
        pc_surface s, d;
        pc_surface_init(&s, fmt);
        (void)pc_surface_fill_rect(&s, pc_rect_make(0, 0, 10, 8), red, 4, NULL);
        (void)pc_surface_fill_rect(&s, pc_rect_make(10, 0, 20, 8), mix, 4, NULL);
        (void)pc_surface_fill_rect(&s, pc_rect_make(11, 0, 20, 8), blue, 4, NULL);
        CHECK(pc_algo_matting_decontaminate(&s, &a, 3.0f, 100.0f, &d, NULL) == PC_OK);
        pc_surface_read_pixel(&d, 10, 4, p);
        CHECK(p[0] > 0.9f && p[2] < 0.1f);
        pc_surface_read_pixel(&d, 15, 4, p);
        pc_surface_read_pixel(&s, 15, 4, q);
        CHECK(memcmp(p, q, 4 * sizeof(float)) == 0);
        CHECK(pc_algo_matting_masked_copy(&d, &a, &c, NULL) == PC_OK);
        CHECK(pc_pixel_format_eq(pc_surface_format(&c), fmt));
        pc_surface_read_pixel(&c, 10, 4, p);
        CHECK(fabsf(p[3] - 128.0f / 255.0f) < 0.01f);
        pc_surface_read_pixel(&c, 15, 4, p);
        CHECK(p[3] == 0.0f);
        pc_surface_read_pixel(&c, 3, 3, p);
        CHECK(p[0] == 1.0f);
        pc_surface_fini(&s);
        pc_surface_fini(&d);
        pc_surface_fini(&c);
    }
    pc_surface_init(&g, PC_PIXEL_FORMAT_GRAY8);
    (void)pc_surface_fill_rect(&g, pc_rect_make(0, 0, 4, 4), &half, 1, NULL);
    a2.bbox = pc_rect_make(0, 0, 2, 4);
    a2.len = 8;
    a2.mask = malloc(8);
    memset(a2.mask, 255, 8);
    (void)pc_algo_matting_masked_copy(&g, &a2, &c, NULL);
    CHECK(pc_pixel_format_eq(pc_surface_format(&c), PC_PIXEL_FORMAT_GRAYA8));
    pc_surface_read_pixel(&c, 3, 1, p);
    CHECK(p[1] == 0.0f);
    pc_surface_read_pixel(&c, 1, 1, p);
    CHECK(fabsf(p[0] - 0.5f) < 0.01f && p[1] == 1.0f);
    pc_surface_fini(&c);
    pc_surface_fini(&g);
    pc_algo_region_fini(&a);
    pc_algo_region_fini(&a2);
}

/* ---- never crash ---- */

static void test_hostile_inputs(void)
{
    float m[16], *o = NULL;
    pc_algo_refine_params p = pc_algo_refine_params_default();
    pc_algo_rgb_image img;
    pc_algo_region reg = {{0, 0, 4, 4}, NULL, 0}, out;
    pc_algo_image_sampler hold;
    pc_algo_sampler s;
    pc_surface surf, d;
    size_t i;
    for (i = 0; i < 16; i++) {
        m[i] = i % 3 == 0 ? NAN : (float)(i % 2);
    }
    /* Absurd blur radius: a limit, not gigabytes of kernel. */
    CHECK(pc_algo_matting_gaussian_blur(m, 4, 4, 1e9f, &o, NULL) == PC_ERR_LIMIT);
    CHECK(pc_algo_matting_gaussian_blur(m, 4, 4, NAN, &o, NULL) == PC_OK);
    free(o);
    /* A huge morphology radius stops once nothing can change. */
    CHECK(pc_algo_matting_morph(m, 4, 4, (size_t)1 << 40, true, &o, NULL) == PC_OK);
    free(o);
    CHECK(pc_algo_matting_box_mean(m, 4, 4, SIZE_MAX, &o, NULL) == PC_OK);
    free(o);
    CHECK(pc_algo_matting_max_square(m, 4, 4, SIZE_MAX / 4, true, &o, NULL) == PC_OK);
    free(o);
    /* Guide size mismatch is refused. */
    (void)pc_algo_rgb_image_init(&img, 3, 3, NULL);
    p.radius = 5.0f;
    CHECK(pc_algo_matting_refine_buffer(&img, m, 4, 4, &p, &o, NULL) == PC_ERR_INVALID);
    /* NaN parameters behave, absurd feather is a limit. */
    p.radius = NAN;
    p.smooth = NAN;
    p.contrast = INFINITY;
    p.shift_edge = NAN;
    CHECK(pc_algo_matting_refine_buffer(NULL, m, 4, 4, &p, &o, NULL) == PC_OK);
    free(o);
    hold.img = &img;
    hold.ox = 0;
    hold.oy = 0;
    s = pc_algo_image_sampler_make(&hold);
    reg.len = 16;
    reg.mask = malloc(16);
    memset(reg.mask, 128, 16);
    p = pc_algo_refine_params_default();
    p.feather = 1e12f;
    CHECK(pc_algo_matting_refine_mask(&s, pc_algo_matting_region_reader, &reg, reg.bbox, reg.bbox, &p, &out,
                                      NULL) == PC_ERR_LIMIT);
    p.feather = 0.0f;
    CHECK(pc_algo_matting_refine_mask(&s, pc_algo_matting_region_reader, &reg, PC_RECT_EMPTY, reg.bbox, &p,
                                      &out, NULL) == PC_OK &&
          pc_algo_region_is_none(&out));
    pc_surface_init(&surf, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_algo_matting_decontaminate(&surf, &reg, 1e30f, NAN, &d, NULL) == PC_ERR_LIMIT);
    CHECK(pc_algo_matting_decontaminate(&surf, &reg, NAN, NAN, &d, NULL) == PC_OK);
    pc_surface_fini(&d);
    pc_surface_fini(&surf);
    pc_algo_region_fini(&reg);
    pc_algo_rgb_image_fini(&img);
}

int main(int argc, char **argv)
{
    RUN(test_max_square_matches_naive);
    RUN(test_box_mean_matches_naive);
    RUN(test_guided_filter_matting_softens_blurred_edge);
    RUN(test_feather_contrast_and_shift);
    RUN(test_refine_mask_tiles_match_single_buffer);
    RUN(test_decontaminate_and_masked_copy_any_depth);
    RUN(test_hostile_inputs);
    return pc_t_finish();
}
