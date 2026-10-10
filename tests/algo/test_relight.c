/* Relight (port of the tests in crates/algo/src/relight.rs and of the Relight cases of
 * tests_ext.rs: tile independence, empty selection, bit depths, colour modes, serde round trip). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "base/buf.h"
#include "base/num.h"
#include "harness.h"
#include "raster/pixel.h"

static pc_pixel_format fmt(pc_sample_type s)
{
    return (pc_pixel_format){PC_COLOR_MODE_RGB, s, true};
}

static pc_algo_filter relight(float angle, float elevation, float intensity, float ambient, float warmth,
                              float softness)
{
    pc_algo_relight p = {angle, elevation, intensity, ambient, warmth, softness};
    pc_algo_filter f;
    memset(&f, 0, sizeof f);
    CHECK(pc_algo_filter_make(&PC_ALGO_FILTER_RELIGHT, &p, &f, NULL) == PC_OK);
    return f;
}

static pc_algo_filter params(float intensity, float angle, float warmth, float softness)
{
    return relight(angle, 40.0f, intensity, 55.0f, warmth, softness);
}

static void run(const pc_surface *s, const pc_algo_filter *f, pc_rect bounds, pc_surface *out)
{
    pc_rect area = pc_algo_output_area(f, pc_surface_content_bounds(s), bounds, NULL);
    CHECK(pc_algo_apply(s, f, area, bounds, NULL, out, NULL) == PC_OK);
}

static float *region(const pc_surface *s, pc_rect r, size_t *len)
{
    float *v = NULL;
    CHECK(pc_surface_read_region(s, r, &v, len, NULL) == PC_OK);
    return v;
}

static float max_diff(const float *a, const float *b, size_t n)
{
    float m = 0.0f;
    size_t i;
    for (i = 0; i < n; i++) {
        m = fmaxf(m, fabsf(a[i] - b[i]));
    }
    return m;
}

static void pattern(pc_surface *s, pc_sample_type st, pc_rect r)
{
    size_t w = pc_rect_width(r), h = pc_rect_height(r), i = 0;
    float *v = malloc(w * h * 4 * sizeof *v);
    int32_t x, y;
    pc_surface_init(s, fmt(st));
    for (y = r.y0; y < r.y1; y++) {
        for (x = r.x0; x < r.x1; x++) {
            v[i++] = (float)((x * 7 + y * 3) % 64) / 63.0f;
            v[i++] = (float)((x * x + y) % 50) / 49.0f;
            v[i++] = (float)(y % 9) / 8.0f;
            v[i++] = 1.0f;
        }
    }
    CHECK(pc_surface_write_region(s, r, v, w * h * 4, NULL) == PC_OK);
    free(v);
}

static void flat(pc_surface *s, pc_sample_type st, pc_rect r, const float px[4])
{
    pc_surface_init(s, fmt(st));
    CHECK(pc_surface_fill_rect(s, r, px, 4, NULL) == PC_OK);
}

static void shaded_sphere(pc_surface *s, pc_sample_type st, int32_t size)
{
    float cx = (float)size / 2.0f, rad = (float)size * 0.42f,
          *v = malloc((size_t)size * (size_t)size * 4 * 4);
    size_t i = 0;
    int32_t x, y;
    pc_surface_init(s, fmt(st));
    for (y = 0; y < size; y++) {
        for (x = 0; x < size; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cx, d2 = dx * dx + dy * dy;
            if (d2 <= rad * rad) {
                float nz = sqrtf(fmaxf(rad * rad - d2, 0.0f)) / rad, sh = 0.22f + 0.78f * nz;
                v[i] = v[i + 1] = v[i + 2] = 0.55f * sh;
            } else {
                v[i] = v[i + 1] = v[i + 2] = 0.18f;
            }
            v[i + 3] = 1.0f;
            i += 4;
        }
    }
    CHECK(pc_surface_write_region(s, pc_rect_make(0, 0, size, size), v, i, NULL) == PC_OK);
    free(v);
}

static float luma(const float *px)
{
    return 0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2];
}

static float sphere_half_mean(const pc_surface *s, int32_t size, bool left)
{
    float cx = (float)size / 2.0f, rad = (float)size * 0.42f, sum = 0.0f, n = 0.0f, px[4];
    int32_t x, y;
    for (y = 0; y < size; y++) {
        for (x = 0; x < size; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cx;
            if (dx * dx + dy * dy > rad * rad || (left && (float)x >= cx - 4.0f) ||
                (!left && (float)x < cx + 4.0f)) {
                continue;
            }
            pc_surface_read_pixel(s, x, y, px);
            sum += luma(px);
            n += 1.0f;
        }
    }
    return sum / fmaxf(n, 1.0f);
}

static void test_halo_is_zero_at_identity_and_covers_sigma_otherwise(void)
{
    pc_algo_filter a = params(0.0f, 45.0f, 0.0f, 25.0f), b = params(40.0f, 45.0f, 0.0f, 25.0f);
    pc_rect wide = pc_rect_make(0, 0, 1600, 1600);
    float sig;
    int32_t reach;
    CHECK(pc_algo_halo_eq(pc_algo_filter_halo(&a), pc_algo_halo_radius(0)));
    CHECK(pc_algo_halo_eq(pc_algo_filter_halo(&b), pc_algo_halo_radius(pc_algo_relight_halo_radius_max())));
    CHECK_EQ_INT(pc_algo_relight_halo_radius_max(), pc_algo_relight_read_radius(PC_ALGO_RELIGHT_SIGMA_MAX));
    /* Short side 1600 px, default softness 25: sigma 32, read 3 sigma + 1 + step = 129. */
    sig = pc_algo_relight_sigma(25.0f, wide);
    CHECK(fabsf(sig - 32.0f) < 1e-4f);
    reach = pc_algo_relight_read_radius(sig);
    CHECK_EQ_INT(reach, 96 + 1 + 32);
    CHECK_EQ_INT(pc_algo_relight_halo_radius(25.0f, wide), reach);
    CHECK(pc_algo_halo_eq(pc_algo_filter_halo_for(&b, wide), pc_algo_halo_radius(reach)));
    CHECK(pc_algo_halo_eq(pc_algo_filter_halo_for(&a, wide), pc_algo_halo_radius(0)));
    pc_algo_filter_fini(&a), pc_algo_filter_fini(&b);
}

static void test_identity_at_zero_intensity_all_depths(void)
{
    pc_rect r = pc_rect_make(0, 0, 24, 16);
    pc_algo_filter p = params(0.0f, 120.0f, 80.0f, 90.0f);
    pc_sample_type st[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    int k;
    for (k = 0; k < 3; k++) {
        pc_surface s, out, g;
        float *a, *b, gp[2] = {0.4f, 1.0f};
        size_t la, lb;
        pattern(&s, st[k], r);
        run(&s, &p, r, &out);
        a = region(&s, r, &la);
        b = region(&out, r, &lb);
        CHECK(la == lb && memcmp(a, b, la * sizeof *a) == 0);
        free(a), free(b);
        pc_surface_fini(&s), pc_surface_fini(&out);
        pc_surface_init(&g, (pc_pixel_format){PC_COLOR_MODE_GRAYSCALE, st[k], true});
        CHECK(pc_surface_fill_rect(&g, r, gp, 2, NULL) == PC_OK);
        run(&g, &p, r, &out);
        a = region(&g, r, &la);
        b = region(&out, r, &lb);
        CHECK(la == lb && memcmp(a, b, la * sizeof *a) == 0);
        free(a), free(b);
        pc_surface_fini(&g), pc_surface_fini(&out);
    }
    pc_algo_filter_fini(&p);
}

static void assert_direction_flips(int32_t size)
{
    pc_rect bounds = pc_rect_make(0, 0, size, size);
    pc_surface s, left_lit, right_lit;
    pc_algo_filter l = relight(180.0f, 30.0f, 90.0f, 12.0f, 0.0f, 15.0f);
    pc_algo_filter r = relight(0.0f, 30.0f, 90.0f, 12.0f, 0.0f, 15.0f);
    shaded_sphere(&s, PC_SAMPLE_TYPE_F32, size);
    run(&s, &l, bounds, &left_lit);
    run(&s, &r, bounds, &right_lit);
    CHECK(sphere_half_mean(&left_lit, size, true) - sphere_half_mean(&left_lit, size, false) >= 0.05f);
    CHECK(sphere_half_mean(&right_lit, size, false) - sphere_half_mean(&right_lit, size, true) >= 0.05f);
    pc_surface_fini(&s), pc_surface_fini(&left_lit), pc_surface_fini(&right_lit);
    pc_algo_filter_fini(&l), pc_algo_filter_fini(&r);
}

static void test_direction_flips_on_a_shaded_sphere(void)
{
    assert_direction_flips(64);
}

static void test_direction_flips_on_a_large_shaded_sphere(void)
{
    /* A 1 px gradient vanishes at this size unless the kernel steps by sigma. */
    assert_direction_flips(256);
}

static void test_warmth_tints_a_neutral_field(void)
{
    pc_rect r = pc_rect_make(0, 0, 32, 32);
    const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    pc_surface s, warm, cool;
    pc_algo_filter fw = relight(45.0f, 40.0f, 50.0f, 55.0f, 80.0f, 25.0f),
                   fc = relight(45.0f, 40.0f, 50.0f, 55.0f, -80.0f, 25.0f);
    float w[4], c[4];
    flat(&s, PC_SAMPLE_TYPE_F32, r, grey);
    run(&s, &fw, r, &warm);
    run(&s, &fc, r, &cool);
    pc_surface_read_pixel(&warm, 16, 16, w);
    pc_surface_read_pixel(&cool, 16, 16, c);
    CHECK(w[0] > w[2]);
    CHECK(c[2] > c[0]);
    pc_surface_fini(&s), pc_surface_fini(&warm), pc_surface_fini(&cool);
    pc_algo_filter_fini(&fw), pc_algo_filter_fini(&fc);
}

static void test_high_ambient_keeps_white_finite(void)
{
    pc_rect r = pc_rect_make(0, 0, 16, 16);
    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    pc_surface s, out;
    pc_algo_filter f = relight(45.0f, 40.0f, 40.0f, 100.0f, 0.0f, 25.0f);
    float px[4];
    flat(&s, PC_SAMPLE_TYPE_F32, r, white);
    run(&s, &f, r, &out);
    pc_surface_read_pixel(&out, 8, 8, px);
    CHECK(isfinite(px[0]) && isfinite(px[1]) && isfinite(px[2]) && isfinite(px[3]));
    pc_surface_fini(&s), pc_surface_fini(&out);
    pc_algo_filter_fini(&f);
}

static void test_alpha_is_preserved(void)
{
    pc_rect r = pc_rect_make(0, 0, 16, 16);
    const float c0[4] = {0.4f, 0.4f, 0.5f, 1.0f}, c1[4] = {0.4f, 0.4f, 0.5f, 0.0f},
                c2[4] = {0.2f, 0.3f, 0.4f, 0.5f};
    pc_surface s, out;
    pc_algo_filter f = params(50.0f, 180.0f, 20.0f, 25.0f);
    float px[4];
    flat(&s, PC_SAMPLE_TYPE_F32, r, c0);
    CHECK(pc_surface_write_pixel(&s, 3, 3, c1, 4, NULL) == PC_OK);
    CHECK(pc_surface_write_pixel(&s, 4, 4, c2, 4, NULL) == PC_OK);
    run(&s, &f, r, &out);
    pc_surface_read_pixel(&out, 3, 3, px);
    CHECK(px[3] == 0.0f);
    pc_surface_read_pixel(&out, 5, 5, px);
    CHECK(fabsf(px[3] - 1.0f) < 1e-6f);
    pc_surface_read_pixel(&out, 4, 4, px);
    CHECK(fabsf(px[3] - 0.5f) < 1e-6f);
    pc_surface_fini(&s), pc_surface_fini(&out);
    pc_algo_filter_fini(&f);
}

static void tiles_match(int32_t size, int32_t big, int32_t small)
{
    pc_rect bounds = pc_rect_make(0, 0, size, size);
    pc_surface s, full, tiled;
    pc_algo_filter p = relight(180.0f, 40.0f, 60.0f, 40.0f, 0.0f, 25.0f);
    pc_rect area;
    float *a, *b;
    size_t la, lb;
    shaded_sphere(&s, PC_SAMPLE_TYPE_F32, size);
    area = pc_algo_output_area(&p, pc_surface_content_bounds(&s), bounds, NULL);
    CHECK(pc_algo_apply_tiled(&s, &p, area, bounds, NULL, big, NULL, &full, NULL) == PC_OK);
    CHECK(pc_algo_apply_tiled(&s, &p, area, bounds, NULL, small, NULL, &tiled, NULL) == PC_OK);
    a = region(&full, bounds, &la);
    b = region(&tiled, bounds, &lb);
    CHECK(la == lb && max_diff(a, b, la) <= 1e-4f);
    free(a), free(b);
    pc_surface_fini(&s), pc_surface_fini(&full), pc_surface_fini(&tiled);
    pc_algo_filter_fini(&p);
}

static void test_tiles_match_a_full_apply(void)
{
    tiles_match(64, 256, 32);
}

static void test_tiles_match_a_full_apply_past_the_old_halo(void)
{
    /* Default softness on a 1600 px short side: sigma 32, reach 129 px; 128 px tiles vs one tile. */
    CHECK(pc_algo_relight_read_radius(pc_algo_relight_sigma(25.0f, pc_rect_make(0, 0, 1600, 1600))) > 97);
    tiles_match(1600, 4096, 128);
}

static void test_layers_above_8mp_are_relit(void)
{
    /* 4096 x 2048 = 8.39 MP: one full-frame kernel call inflates past 8 MP of luma (bands). */
    const int32_t w = 4096, h = 2048;
    pc_rect bounds = pc_rect_make(0, 0, w, h);
    uint8_t *bytes = malloc((size_t)w * (size_t)h * 4);
    pc_surface s, out;
    pc_algo_filter p = relight(180.0f, 30.0f, 90.0f, 12.0f, 0.0f, 25.0f);
    pc_rect area;
    float before[4], after[4], b8[4], a8[4];
    size_t i = 0;
    int32_t x, y;
    for (y = 0; y < h; y++) {
        uint8_t v = (uint8_t)((y * 200) / h);
        for (x = 0; x < w; x++) {
            uint8_t u = (uint8_t)((x * 200) / w);
            bytes[i++] = (uint8_t)(80 + u / 4);
            bytes[i++] = (uint8_t)(40 + v / 3);
            bytes[i++] = 90;
            bytes[i++] = 255;
        }
    }
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_surface_write_interleaved(&s, bounds, bytes, i, NULL) == PC_OK);
    free(bytes);
    area = pc_algo_output_area(&p, pc_surface_content_bounds(&s), bounds, NULL);
    pc_surface_read_pixel(&s, w / 2, h / 2, before);
    CHECK(pc_algo_apply_tiled(&s, &p, area, bounds, NULL, 8192, NULL, &out, NULL) == PC_OK);
    pc_surface_read_pixel(&out, w / 2, h / 2, after);
    CHECK(isfinite(after[0]) && isfinite(after[1]) && isfinite(after[2]) && isfinite(after[3]));
    CHECK(max_diff(before, after, 4) > 1e-4f);
    pc_surface_read_pixel(&s, 8, 8, b8);
    pc_surface_read_pixel(&out, 8, 8, a8);
    CHECK(max_diff(b8, a8, 4) > 1e-4f);
    pc_surface_fini(&s), pc_surface_fini(&out);
    pc_algo_filter_fini(&p);
}

static void test_hostile_sizes_do_not_panic(void)
{
    pc_algo_filter p = params(40.0f, 45.0f, 0.0f, 25.0f);
    const int32_t sizes[4][2] = {{1, 1}, {2, 2}, {1, 2}, {0, 0}};
    const float c[4] = {0.4f, 0.5f, 0.6f, 1.0f};
    int k;
    for (k = 0; k < 4; k++) {
        pc_rect r = pc_rect_make(0, 0, sizes[k][0], sizes[k][1]);
        pc_surface s, out;
        pc_surface_init(&s, fmt(PC_SAMPLE_TYPE_F32));
        if (!pc_rect_is_empty(r)) {
            CHECK(pc_surface_fill_rect(&s, r, c, 4, NULL) == PC_OK);
        }
        run(&s, &p, pc_rect_is_empty(r) ? pc_rect_make(0, 0, 1, 1) : r, &out);
        pc_surface_fini(&out);
        CHECK(pc_algo_apply(&s, &p, PC_RECT_EMPTY, pc_rect_make(0, 0, 4, 4), NULL, &out, NULL) == PC_OK);
        pc_surface_fini(&out), pc_surface_fini(&s);
    }
    pc_algo_filter_fini(&p);
}

static void test_nan_and_inf_pixels_pass_through(void)
{
    pc_rect r = pc_rect_make(0, 0, 24, 24);
    pc_surface s, out;
    const float a[4] = {NAN, 0.4f, 0.4f, 1.0f}, b[4] = {INFINITY, 0.2f, 0.2f, 1.0f};
    pc_algo_filter p = params(50.0f, 180.0f, 0.0f, 20.0f);
    float before[4], px[4];
    pattern(&s, PC_SAMPLE_TYPE_F32, r);
    CHECK(pc_surface_write_pixel(&s, 5, 5, a, 4, NULL) == PC_OK);
    CHECK(pc_surface_write_pixel(&s, 6, 8, b, 4, NULL) == PC_OK);
    pc_surface_read_pixel(&s, 7, 5, before);
    run(&s, &p, r, &out);
    pc_surface_read_pixel(&out, 5, 5, px);
    CHECK(isnan(px[0]));
    pc_surface_read_pixel(&out, 6, 8, px);
    CHECK(isinf(px[0]));
    pc_surface_read_pixel(&out, 7, 5, px);
    CHECK(isfinite(px[0]) && isfinite(px[1]) && isfinite(px[2]) && isfinite(px[3]));
    CHECK(memcmp(px, before, sizeof px) != 0);
    pc_surface_fini(&s), pc_surface_fini(&out);
    pc_algo_filter_fini(&p);
}

/* ---- tests_ext.rs cases for Relight (R = 40 x 30) ---- */

static const pc_rect R = {0, 0, 40, 30};

static void test_ext_tile_independent_selection_and_depths(void)
{
    pc_algo_filter p = params(40.0f, 45.0f, 0.0f, 25.0f);
    pc_surface s8, s16, none, a, b, out, masked;
    pc_rect area, big;
    pc_sample_type st[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    float *ra, *rb, *outs[3];
    size_t la, lb, lens[3], i;
    int k;
    pattern(&s8, PC_SAMPLE_TYPE_U8, R);
    area = pc_algo_output_area(&p, pc_surface_content_bounds(&s8), R, NULL);
    CHECK(pc_algo_apply_tiled(&s8, &p, area, R, NULL, 256, NULL, &a, NULL) == PC_OK);
    CHECK(pc_algo_apply_tiled(&s8, &p, area, R, NULL, 7, NULL, &b, NULL) == PC_OK);
    big = pc_rect_union(area, R);
    ra = region(&a, big, &la);
    rb = region(&b, big, &lb);
    CHECK(la == lb && max_diff(ra, rb, la) <= 1.5f / 255.0f);
    free(ra), free(rb);
    pc_surface_fini(&a), pc_surface_fini(&b);
    /* changes pixels and respects an empty selection */
    pattern(&s16, PC_SAMPLE_TYPE_U16, R);
    run(&s16, &p, R, &out);
    CHECK(!pc_surface_eq(&out, &s16));
    pc_surface_init(&none, PC_PIXEL_FORMAT_GRAY8);
    area = pc_algo_output_area(&p, pc_surface_content_bounds(&s16), R, NULL);
    CHECK(pc_algo_apply(&s16, &p, area, R, &none, &masked, NULL) == PC_OK);
    ra = region(&masked, area, &la);
    rb = region(&s16, area, &lb);
    CHECK(la == lb && memcmp(ra, rb, la * sizeof *ra) == 0);
    free(ra), free(rb);
    pc_surface_fini(&out), pc_surface_fini(&masked), pc_surface_fini(&none), pc_surface_fini(&s16);
    pc_surface_fini(&s8);
    /* agree across bit depths (float results clamped like integer storage) */
    for (k = 0; k < 3; k++) {
        pc_surface s;
        pattern(&s, st[k], R);
        run(&s, &p, R, &out);
        outs[k] = region(&out, R, &lens[k]);
        for (i = 0; i < lens[k]; i++) {
            outs[k][i] = pc_clampf(outs[k][i], 0.0f, 1.0f);
        }
        pc_surface_fini(&s), pc_surface_fini(&out);
    }
    for (k = 1; k < 3; k++) {
        float mx = max_diff(outs[0], outs[k], lens[0]), mn = -0.0f;
        for (i = 0; i < lens[0]; i++) {
            mn += fabsf(outs[0][i] - outs[k][i]);
        }
        mn /= (float)lens[0];
        CHECK(mx < 0.06f && mn < 0.01f);
    }
    for (k = 0; k < 3; k++) {
        free(outs[k]);
    }
    pc_algo_filter_fini(&p);
}

static void test_ext_other_colour_modes(void)
{
    pc_color_mode modes[3] = {PC_COLOR_MODE_GRAYSCALE, PC_COLOR_MODE_CMYK, PC_COLOR_MODE_LAB};
    pc_algo_filter p = params(40.0f, 45.0f, 0.0f, 25.0f);
    pc_surface src;
    int k;
    pattern(&src, PC_SAMPLE_TYPE_F32, R);
    for (k = 0; k < 3; k++) {
        pc_pixel_format f = {modes[k], PC_SAMPLE_TYPE_U8, true};
        pc_surface s, out;
        float *v;
        size_t len, i;
        int32_t x, y;
        pc_surface_init(&s, f);
        for (y = R.y0; y < R.y1; y++) {
            for (x = R.x0; x < R.x1; x++) {
                float px[4], rgba[4], o[PC_RASTER_MAX_CHANNELS];
                size_t n;
                pc_surface_read_pixel(&src, x, y, px);
                pc_to_rgba(pc_surface_format(&src), px, 4, rgba);
                n = pc_from_rgba(f, rgba, o);
                CHECK(pc_surface_write_pixel(&s, x, y, o, n, NULL) == PC_OK);
            }
        }
        run(&s, &p, R, &out);
        CHECK(pc_pixel_format_eq(pc_surface_format(&out), f));
        v = region(&out, R, &len);
        for (i = 0; i < len; i++) {
            CHECK(isfinite(v[i]));
        }
        free(v);
        pc_surface_fini(&s), pc_surface_fini(&out);
    }
    pc_surface_fini(&src);
    pc_algo_filter_fini(&p);
}

static void test_ext_params_serde_roundtrip(void)
{
    pc_algo_filter p = params(40.0f, 45.0f, 0.0f, 25.0f), back;
    pc_buf text;
    const char *in = "{\"filter\":\"relight\",\"angle\":45,\"elevation\":40,\"intensity\":40,\"ambient\":55,"
                     "\"warmth\":0,\"softness\":25}";
    pc_buf_init(&text);
    CHECK(pc_algo_filter_to_json_text(&p, &text, NULL) == PC_OK);
    CHECK_EQ_STR((const char *)text.data,
                 "{\"filter\":\"relight\",\"angle\":45.0,\"elevation\":40.0,\"intensity\":40.0,"
                 "\"ambient\":55.0,\"warmth\":0.0,\"softness\":25.0}");
    CHECK(pc_algo_filter_from_json_str((const char *)text.data, text.len, &back, NULL) == PC_OK);
    CHECK(pc_algo_filter_eq(&p, &back));
    pc_algo_filter_fini(&back);
    CHECK(pc_algo_filter_from_json_str(in, strlen(in), &back, NULL) == PC_OK);
    CHECK(pc_algo_filter_eq(&p, &back));
    CHECK_EQ_STR(pc_algo_filter_label(&back), "Relight");
    pc_algo_filter_fini(&back);
    pc_buf_fini(&text);
    pc_algo_filter_fini(&p);
}

static void test_hostile_params_never_crash(void)
{
    float vals[4] = {NAN, INFINITY, -INFINITY, 1e30f};
    pc_surface s, out;
    int k, f;
    pattern(&s, PC_SAMPLE_TYPE_F32, R);
    for (k = 0; k < 4; k++) {
        for (f = 0; f < 6; f++) {
            float v[6] = {45.0f, 40.0f, 40.0f, 55.0f, 0.0f, 25.0f};
            pc_algo_filter p;
            v[f] = vals[k];
            p = relight(v[0], v[1], v[2], v[3], v[4], v[5]);
            run(&s, &p, R, &out);
            pc_surface_fini(&out);
            run(&s, &p, PC_RECT_EMPTY, &out);
            pc_surface_fini(&out);
            pc_algo_filter_fini(&p);
        }
    }
    pc_surface_fini(&s);
}

int main(int argc, char **argv)
{
    RUN(test_halo_is_zero_at_identity_and_covers_sigma_otherwise);
    RUN(test_identity_at_zero_intensity_all_depths);
    RUN(test_direction_flips_on_a_shaded_sphere);
    RUN(test_direction_flips_on_a_large_shaded_sphere);
    RUN(test_warmth_tints_a_neutral_field);
    RUN(test_high_ambient_keeps_white_finite);
    RUN(test_alpha_is_preserved);
    RUN(test_tiles_match_a_full_apply);
    RUN(test_tiles_match_a_full_apply_past_the_old_halo);
    RUN(test_layers_above_8mp_are_relit);
    RUN(test_hostile_sizes_do_not_panic);
    RUN(test_nan_and_inf_pixels_pass_through);
    RUN(test_ext_tile_independent_selection_and_depths);
    RUN(test_ext_other_colour_modes);
    RUN(test_ext_params_serde_roundtrip);
    RUN(test_hostile_params_never_crash);
    return pc_t_finish();
}
