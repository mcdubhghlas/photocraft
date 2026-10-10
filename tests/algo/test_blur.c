/* Unit tests of the L5 filters: the tests.rs / tests_ext.rs cases for blur, sharpen, noise and
 * denoise (the registry-wide ones iterate over a representative of every FilterParams variant of
 * tests.rs and tests_ext.rs and skip variants not registered yet), and the #[test]s of blur.rs,
 * noise.rs, shape_blur_tests.rs and selection_blur.rs. Benchmarks (surface_timing, bench_24mp,
 * the 36 MP shape test) are not ported. */
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

static const pc_rect R = {0, 0, 40, 30};

static pc_pixel_format rgba(pc_sample_type s)
{
    pc_pixel_format f = {PC_COLOR_MODE_RGB, s, true};
    return f;
}

static void pattern(pc_surface *s, pc_sample_type st, pc_rect r)
{
    size_t n = (size_t)pc_rect_width(r) * pc_rect_height(r) * 4, k = 0;
    float *v = malloc(n * sizeof(float) + 4);
    int32_t x, y;
    pc_surface_init(s, rgba(st));
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
    pc_surface_init(s, rgba(st));
    CHECK(pc_surface_fill_rect(s, r, px, 4, NULL) == PC_OK);
}

static float px_at(const pc_surface *s, int32_t x, int32_t y, size_t c)
{
    float p[PC_RASTER_MAX_CHANNELS];
    pc_surface_read_pixel(s, x, y, p);
    return p[c];
}

static float *region(const pc_surface *s, pc_rect r, size_t *len)
{
    float *v = NULL;
    CHECK(pc_surface_read_region(s, r, &v, len, NULL) == PC_OK);
    return v;
}

static bool regions_equal(const pc_surface *a, const pc_surface *b, pc_rect r)
{
    size_t la, lb;
    float *x = region(a, r, &la), *y = region(b, r, &lb);
    bool eq = la == lb && (la == 0 || memcmp(x, y, la * sizeof(float)) == 0);
    free(x), free(y);
    return eq;
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

static bool make(const char *json, pc_algo_filter *f)
{
    pc_error err;
    pc_status st = pc_algo_filter_from_json_str(json, strlen(json), f, &err);
    if (st == PC_ERR_UNSUPPORTED) {
        return false; /* a variant of a later chunk */
    }
    if (st != PC_OK) {
        PC_T_FAIL("%s: %s", json, err.msg);
        return false;
    }
    return true;
}

static pc_status run(const pc_surface *s, const pc_algo_filter *f, pc_surface *out)
{
    pc_rect area = pc_algo_output_area(f, pc_surface_content_bounds(s), R, NULL);
    return pc_algo_apply(s, f, area, R, NULL, out, NULL);
}

static void run_json(const pc_surface *s, const char *json, pc_surface *out)
{
    pc_algo_filter f;
    if (!make(json, &f)) {
        pc_surface_init(out, PC_PIXEL_FORMAT_GRAY8);
        return;
    }
    CHECK(run(s, &f, out) == PC_OK);
    pc_algo_filter_fini(&f);
}

/* tests.rs all_filters() plus the L5 cases of tests_ext.rs new_filters(), as JSON. */
static const char *const ALL[] = {
    "{\"filter\":\"gaussianBlur\",\"radius\":2.5}",
    "{\"filter\":\"gaussianBlur\",\"radius\":9}",
    "{\"filter\":\"boxBlur\",\"radius\":2}",
    "{\"filter\":\"boxBlur\",\"radius\":6}",
    "{\"filter\":\"motionBlur\",\"angle\":30,\"distance\":7}",
    "{\"filter\":\"radialBlur\",\"amount\":20,\"method\":\"spin\",\"quality\":\"good\",\"centerX\":0.5,"
    "\"centerY\":0.5}",
    "{\"filter\":\"radialBlur\",\"amount\":30,\"method\":\"zoom\",\"quality\":\"good\",\"centerX\":0.3,"
    "\"centerY\":0.6}",
    "{\"filter\":\"surfaceBlur\",\"radius\":3,\"threshold\":20}",
    "{\"filter\":\"unsharpMask\",\"amount\":150,\"radius\":1.5,\"threshold\":2}",
    "{\"filter\":\"smartSharpen\",\"amount\":100,\"radius\":1,\"reduceNoise\":20}",
    "{\"filter\":\"highPass\",\"radius\":3}",
    "{\"filter\":\"addNoise\",\"amount\":25,\"distribution\":\"gaussian\",\"monochromatic\":false,\"seed\":"
    "3}",
    "{\"filter\":\"median\",\"radius\":2}",
    "{\"filter\":\"dustAndScratches\",\"radius\":2,\"threshold\":10}",
    "{\"filter\":\"minimum\",\"radius\":1,\"preserve\":\"squareness\"}",
    "{\"filter\":\"maximum\",\"radius\":2,\"preserve\":\"squareness\"}",
    "{\"filter\":\"minimum\",\"radius\":2.5,\"preserve\":\"roundness\"}",
    "{\"filter\":\"maximum\",\"radius\":3,\"preserve\":\"roundness\"}",
    "{\"filter\":\"offset\",\"horizontal\":7,\"vertical\":-3,\"undefined\":\"wrap\"}",
    "{\"filter\":\"mosaic\",\"cellSize\":6}",
    "{\"filter\":\"emboss\",\"angle\":135,\"height\":2,\"amount\":100}",
    "{\"filter\":\"findEdges\"}",
    "{\"filter\":\"solarize\"}",
    "{\"filter\":\"invert\"}",
    "{\"filter\":\"desaturate\"}",
    "{\"filter\":\"twirl\",\"angle\":90}",
    "{\"filter\":\"pinch\",\"amount\":50}",
    "{\"filter\":\"spherize\",\"amount\":-60,\"mode\":\"horizontalOnly\"}",
    "{\"filter\":\"wave\",\"generators\":3,\"wavelengthMin\":5,\"wavelengthMax\":20,\"amplitudeMin\":1,"
    "\"amplitudeMax\":4,\"waveType\":\"triangle\",\"undefined\":\"repeat\",\"seed\":9}",
    "{\"filter\":\"ripple\",\"amount\":200,\"size\":\"small\"}",
    "{\"filter\":\"polarCoordinates\",\"mode\":\"rectangularToPolar\"}",
    "{\"filter\":\"polarCoordinates\",\"mode\":\"polarToRectangular\"}",
    "{\"filter\":\"reduceNoise\",\"strength\":6,\"preserveDetails\":60,\"reduceColorNoise\":45,"
    "\"sharpenDetails\":25,"
    "\"removeJpegArtifact\":true}",
    "{\"filter\":\"smartBlur\",\"radius\":3,\"threshold\":40,\"quality\":\"high\",\"mode\":\"normal\"}",
    "{\"filter\":\"smartBlur\",\"radius\":3,\"threshold\":25,\"quality\":\"low\",\"mode\":\"overlayEdge\"}",
    "{\"filter\":\"lensBlur\",\"radius\":4,\"blades\":6,\"curvature\":20,\"rotation\":10,\"depth\":\"none\","
    "\"focalDistance\":0,\"invertDepth\":false,\"brightness\":30,\"threshold\":200,\"noise\":5,"
    "\"distribution\":\"uniform\",\"monochromatic\":false,\"seed\":9}",
    "{\"filter\":\"shapeBlur\",\"radius\":3,\"shape\":\"star\"}",
};
#define NALL (sizeof ALL / sizeof ALL[0])
/* Index of the first tests_ext.rs case: those allow 1.5 levels between tilings. */
#define FIRST_EXT 32

/* ---- tests.rs ---- */

static void test_every_filter_is_tile_independent(void)
{
    pc_surface s;
    size_t i;
    pattern(&s, PC_SAMPLE_TYPE_U8, R);
    for (i = 0; i < NALL; i++) {
        pc_algo_filter f;
        pc_surface a, b;
        pc_rect area, big;
        if (!make(ALL[i], &f)) {
            continue;
        }
        area = pc_algo_output_area(&f, pc_surface_content_bounds(&s), R, NULL);
        big = pc_rect_union(area, R);
        CHECK(pc_algo_apply_tiled(&s, &f, area, R, NULL, 256, NULL, &a, NULL) == PC_OK);
        CHECK(pc_algo_apply_tiled(&s, &f, area, R, NULL, 7, NULL, &b, NULL) == PC_OK);
        if (i < FIRST_EXT) {
            if (!regions_equal(&a, &b, big)) {
                PC_T_FAIL("%s differs between tile sizes", pc_algo_filter_label(&f));
            }
        } else {
            size_t la, lb;
            float *x = region(&a, big, &la), *y = region(&b, big, &lb);
            CHECK(la == lb && max_diff_v(x, y, la) <= 1.5f / 255.0f);
            free(x), free(y);
        }
        pc_surface_fini(&a), pc_surface_fini(&b);
        pc_algo_filter_fini(&f);
    }
    pc_surface_fini(&s);
}

static void test_every_filter_changes_something_and_respects_zero_selection(void)
{
    pc_surface s, none;
    size_t i;
    pattern(&s, PC_SAMPLE_TYPE_U16, R);
    pc_surface_init(&none, PC_PIXEL_FORMAT_GRAY8);
    for (i = 0; i < NALL; i++) {
        pc_algo_filter f;
        pc_surface out, masked;
        pc_rect area;
        if (!make(ALL[i], &f)) {
            continue;
        }
        CHECK(run(&s, &f, &out) == PC_OK);
        if (pc_surface_eq(&out, &s)) {
            PC_T_FAIL("%s did nothing", pc_algo_filter_label(&f));
        }
        area = pc_algo_output_area(&f, pc_surface_content_bounds(&s), R, NULL);
        CHECK(pc_algo_apply(&s, &f, area, R, &none, &masked, NULL) == PC_OK);
        if (!regions_equal(&masked, &s, area)) {
            PC_T_FAIL("%s ignored an empty selection", pc_algo_filter_label(&f));
        }
        pc_surface_fini(&out), pc_surface_fini(&masked);
        pc_algo_filter_fini(&f);
    }
    pc_surface_fini(&s), pc_surface_fini(&none);
}

static void test_params_serde_roundtrip(void)
{
    size_t i;
    pc_algo_filter f, back;
    pc_algo_unsharp_params *u;
    for (i = 0; i < NALL; i++) {
        pc_buf text;
        if (!make(ALL[i], &f)) {
            continue;
        }
        pc_buf_init(&text);
        CHECK(pc_algo_filter_to_json_text(&f, &text, NULL) == PC_OK);
        CHECK(pc_algo_filter_from_json_str((const char *)text.data, text.len, &back, NULL) == PC_OK);
        CHECK(pc_algo_filter_eq(&f, &back));
        pc_algo_filter_fini(&back);
        pc_buf_fini(&text);
        pc_algo_filter_fini(&f);
    }
    CHECK(make("{\"filter\":\"unsharpMask\",\"amount\":50,\"radius\":1,\"threshold\":0}", &f));
    u = f.params;
    CHECK(f.def == &PC_ALGO_FILTER_UNSHARP_MASK && u->amount == 50.0f && u->radius == 1.0f &&
          u->threshold == 0.0f);
    pc_algo_filter_fini(&f);
}

static void test_radial_blur_quality_defaults_for_older_parameters(void)
{
    pc_algo_filter f;
    const pc_algo_radial_params *p;
    CHECK(make(
        "{\"filter\":\"radialBlur\",\"amount\":20.0,\"method\":\"spin\",\"centerX\":0.5,\"centerY\":0.5}",
        &f));
    p = f.params;
    CHECK(p->amount == 20.0f && p->method == PC_ALGO_RADIAL_SPIN && p->quality == PC_ALGO_RADIAL_GOOD &&
          p->center_x == 0.5f && p->center_y == 0.5f);
    pc_algo_filter_fini(&f);
}

static void test_gaussian_halo_and_output_area(void)
{
    pc_algo_radius_params g = {2.0f}, b = {1.0f};
    pc_algo_filter f;
    pc_rect content = pc_rect_make(10, 10, 20, 20), sb = pc_rect_make(0, 0, 12, 12);
    CHECK(pc_algo_filter_make(&PC_ALGO_FILTER_GAUSSIAN_BLUR, &g, &f, NULL) == PC_OK);
    CHECK(pc_algo_halo_eq(pc_algo_filter_halo(&f), pc_algo_halo_radius(7)));
    pc_algo_filter_fini(&f);
    CHECK(pc_algo_filter_make(&PC_ALGO_FILTER_BOX_BLUR, &b, &f, NULL) == PC_OK);
    CHECK(pc_rect_eq(pc_algo_output_area(&f, content, R, &sb), pc_rect_make(8, 8, 12, 12)));
    pc_algo_filter_fini(&f);
}

static void test_gaussian_identity_at_tiny_radius(void)
{
    pc_surface s, out;
    pattern(&s, PC_SAMPLE_TYPE_U8, R);
    run_json(&s, "{\"filter\":\"gaussianBlur\",\"radius\":0.01}", &out);
    CHECK(regions_equal(&out, &s, R));
    pc_surface_fini(&s), pc_surface_fini(&out);
}

static void test_blur_preserves_flat_colour_and_mass(void)
{
    const float c[4] = {0.3f, 0.6f, 0.9f, 1.0f}, red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    const char *const ps[2] = {"{\"filter\":\"gaussianBlur\",\"radius\":2}",
                               "{\"filter\":\"boxBlur\",\"radius\":3}"};
    pc_surface s, out, small;
    int k;
    flat(&s, PC_SAMPLE_TYPE_F32, R, c);
    run_json(&s, "{\"filter\":\"gaussianBlur\",\"radius\":3}", &out);
    CHECK(fabsf(px_at(&out, 20, 15, 0) - 0.3f) < 1e-5f && fabsf(px_at(&out, 20, 15, 3) - 1.0f) < 1e-5f);
    pc_surface_fini(&out);
    flat(&small, PC_SAMPLE_TYPE_F32, pc_rect_make(10, 10, 20, 20), red);
    for (k = 0; k < 2; k++) {
        size_t len, i;
        float *v, sum = -0.0f;
        run_json(&small, ps[k], &out);
        v = region(&out, pc_rect_make(-10, -10, 40, 40), &len);
        for (i = 3; i < len; i += 4) {
            sum += v[i];
        }
        CHECK(fabsf(sum - 100.0f) < 0.01f);
        CHECK(px_at(&out, 9, 15, 3) > 0.0f && fabsf(px_at(&out, 9, 15, 0) - 1.0f) < 1e-4f);
        free(v);
        pc_surface_fini(&out);
    }
    pc_surface_fini(&s), pc_surface_fini(&small);
}

static void test_blur_keeps_16bit_precision(void)
{
    pc_surface s, out;
    pc_rect r = pc_rect_make(0, 0, 200, 1);
    float v[800];
    int32_t x;
    pc_algo_radius_params p = {2.0f};
    pc_algo_filter f;
    for (x = 0; x < 200; x++) {
        v[x * 4] = (float)x / 65535.0f, v[x * 4 + 1] = 0.0f, v[x * 4 + 2] = 0.0f, v[x * 4 + 3] = 1.0f;
    }
    pc_surface_init(&s, rgba(PC_SAMPLE_TYPE_U16));
    CHECK(pc_surface_write_region(&s, r, v, 800, NULL) == PC_OK);
    CHECK(pc_algo_filter_make(&PC_ALGO_FILTER_GAUSSIAN_BLUR, &p, &f, NULL) == PC_OK);
    CHECK(pc_algo_apply(&s, &f, r, r, NULL, &out, NULL) == PC_OK);
    for (x = 20; x < 180; x++) {
        CHECK_EQ_INT(pc_f32_to_i32(roundf(px_at(&out, x, 0, 0) * 65535.0f)), x);
    }
    pc_algo_filter_fini(&f);
    pc_surface_fini(&s), pc_surface_fini(&out);
}

static void test_float_surfaces_keep_hdr_values(void)
{
    const float c[4] = {4.0f, 2.0f, 0.5f, 1.0f};
    pc_surface s, out;
    flat(&s, PC_SAMPLE_TYPE_F32, R, c);
    run_json(&s, "{\"filter\":\"gaussianBlur\",\"radius\":2}", &out);
    CHECK(fabsf(px_at(&out, 20, 15, 0) - 4.0f) < 1e-4f);
    pc_surface_fini(&s), pc_surface_fini(&out);
}

static void test_gaussian_sigma_matches_radius(void)
{
    const float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    pc_algo_radius_params p = {2.0f};
    pc_algo_filter f;
    pc_surface s, out;
    pc_surface_init(&s, rgba(PC_SAMPLE_TYPE_F32));
    CHECK(pc_surface_write_pixel(&s, 20, 15, one, 4, NULL) == PC_OK);
    CHECK(pc_algo_filter_make(&PC_ALGO_FILTER_GAUSSIAN_BLUR, &p, &f, NULL) == PC_OK);
    CHECK(pc_algo_apply(&s, &f, R, R, NULL, &out, NULL) == PC_OK);
    CHECK(fabsf(px_at(&out, 22, 15, 3) / px_at(&out, 20, 15, 3) - expf(-0.5f)) < 1e-3f);
    pc_algo_filter_fini(&f);
    pc_surface_fini(&s), pc_surface_fini(&out);
}

static float red_var(const pc_surface *s)
{
    size_t len, i, n;
    float *v = region(s, pc_rect_make(5, 5, 35, 25), &len), m = 0.0f, acc = 0.0f;
    n = len / 4;
    for (i = 0; i < n; i++) {
        m += v[i * 4];
    }
    m /= (float)n;
    for (i = 0; i < n; i++) {
        acc += (v[i * 4] - m) * (v[i * 4] - m);
    }
    free(v);
    return acc;
}

static void test_unsharp_threshold_and_contrast(void)
{
    pc_surface s, none, sharp;
    pattern(&s, PC_SAMPLE_TYPE_F32, R);
    run_json(&s, "{\"filter\":\"unsharpMask\",\"amount\":200,\"radius\":2,\"threshold\":255}", &none);
    CHECK(regions_equal(&none, &s, R));
    run_json(&s, "{\"filter\":\"unsharpMask\",\"amount\":200,\"radius\":2,\"threshold\":0}", &sharp);
    CHECK(red_var(&sharp) > red_var(&s));
    pc_surface_fini(&s), pc_surface_fini(&none), pc_surface_fini(&sharp);
}

static void test_high_pass_of_flat_is_mid_gray(void)
{
    const float c[4] = {0.9f, 0.1f, 0.4f, 1.0f};
    pc_surface s, out;
    flat(&s, PC_SAMPLE_TYPE_F32, R, c);
    run_json(&s, "{\"filter\":\"highPass\",\"radius\":4}", &out);
    CHECK(fabsf(px_at(&out, 20, 15, 0) - 0.5f) < 1e-5f && fabsf(px_at(&out, 20, 15, 1) - 0.5f) < 1e-5f);
    pc_surface_fini(&s), pc_surface_fini(&out);
}

static void test_noise_is_deterministic_monochrome_and_centred(void)
{
    const float c[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    const char *p = "{\"filter\":\"addNoise\",\"amount\":50,\"distribution\":\"uniform\",\"monochromatic\":"
                    "true,\"seed\":1}";
    pc_surface s, a, b, o;
    size_t len, i;
    float *v, mean = 0.0f;
    flat(&s, PC_SAMPLE_TYPE_F32, R, c);
    run_json(&s, p, &a);
    run_json(&s, p, &b);
    CHECK(pc_surface_eq(&a, &b));
    v = region(&a, R, &len);
    for (i = 0; i < len; i += 4) {
        CHECK(v[i] == v[i + 1] && v[i + 1] == v[i + 2]);
        CHECK(fabsf(v[i] - 0.5f) <= 0.25f + 1e-6f);
        mean += v[i];
    }
    mean /= (float)(len / 4);
    CHECK(fabsf(mean - 0.5f) < 0.03f);
    free(v);
    run_json(&s,
             "{\"filter\":\"addNoise\",\"amount\":50,\"distribution\":\"uniform\",\"monochromatic\":true,"
             "\"seed\":2}",
             &o);
    CHECK(!pc_surface_eq(&a, &o));
    pc_surface_fini(&s), pc_surface_fini(&a), pc_surface_fini(&b), pc_surface_fini(&o);
}

static void test_median_and_dust_remove_a_speck(void)
{
    const float c[4] = {0.2f, 0.2f, 0.2f, 1.0f}, w[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    pc_surface s, m, keep, rem;
    flat(&s, PC_SAMPLE_TYPE_F32, R, c);
    CHECK(pc_surface_write_pixel(&s, 10, 10, w, 4, NULL) == PC_OK);
    run_json(&s, "{\"filter\":\"median\",\"radius\":1}", &m);
    CHECK(fabsf(px_at(&m, 10, 10, 0) - 0.2f) < 1e-6f);
    run_json(&s, "{\"filter\":\"dustAndScratches\",\"radius\":1,\"threshold\":255}", &keep);
    CHECK(fabsf(px_at(&keep, 10, 10, 0) - 1.0f) < 1e-6f);
    run_json(&s, "{\"filter\":\"dustAndScratches\",\"radius\":1,\"threshold\":50}", &rem);
    CHECK(fabsf(px_at(&rem, 10, 10, 0) - 0.2f) < 1e-6f);
    pc_surface_fini(&s), pc_surface_fini(&m), pc_surface_fini(&keep), pc_surface_fini(&rem);
}

static void test_motion_blur_spreads_along_angle_only(void)
{
    const float k[4] = {0.0f, 0.0f, 0.0f, 1.0f}, w[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    pc_surface s, h, v;
    int32_t y;
    flat(&s, PC_SAMPLE_TYPE_F32, R, k);
    for (y = 0; y < 30; y++) {
        CHECK(pc_surface_write_pixel(&s, 20, y, w, 4, NULL) == PC_OK);
    }
    run_json(&s, "{\"filter\":\"motionBlur\",\"angle\":0,\"distance\":6}", &h);
    CHECK(px_at(&h, 22, 15, 0) > 0.05f);
    run_json(&s, "{\"filter\":\"motionBlur\",\"angle\":90,\"distance\":6}", &v);
    CHECK(px_at(&v, 22, 15, 0) < 1e-4f);
    pc_surface_fini(&s), pc_surface_fini(&h), pc_surface_fini(&v);
}

static void test_layer_filters_repeat_the_canvas_edge(void)
{
    const int32_t pts[5][2] = {{0, 0}, {39, 0}, {0, 29}, {39, 29}, {20, 0}};
    int st, k;
    for (st = 0; st < 3; st++) {
        const float c[4] = {0.2f, 0.6f, 0.4f, 1.0f};
        pc_algo_radius_params p = {6.0f};
        pc_algo_filter f;
        pc_surface s, clamped, faded;
        pc_rect area;
        flat(&s, (pc_sample_type)st, R, c);
        CHECK(pc_algo_filter_make(&PC_ALGO_FILTER_GAUSSIAN_BLUR, &p, &f, NULL) == PC_OK);
        area = pc_algo_output_area(&f, pc_surface_content_bounds(&s), R, NULL);
        CHECK(pc_algo_apply_in(&s, &f, area, R, NULL, R, &clamped, NULL) == PC_OK);
        CHECK(pc_rect_eq(pc_surface_content_bounds(&clamped), R));
        for (k = 0; k < 5; k++) {
            CHECK(fabsf(px_at(&clamped, pts[k][0], pts[k][1], 3) - 1.0f) < 1e-3f);
            CHECK(fabsf(px_at(&clamped, pts[k][0], pts[k][1], 1) - 0.6f) < 2e-3f);
        }
        CHECK(pc_algo_apply(&s, &f, area, R, NULL, &faded, NULL) == PC_OK);
        CHECK(px_at(&faded, 0, 0, 3) < 0.9f);
        pc_surface_fini(&s), pc_surface_fini(&clamped), pc_surface_fini(&faded);
        pc_algo_filter_fini(&f);
    }
}

/* ---- tests_ext.rs (L5 filters) ---- */

static void test_new_filters_agree_across_bit_depths_and_modes(void)
{
    size_t i, k, n;
    for (i = FIRST_EXT; i < NALL; i++) {
        float *outs[3];
        size_t len = 0;
        bool thresholdy = strstr(ALL[i], "smartBlur") != NULL;
        int st, m;
        for (st = 0; st < 3; st++) {
            pc_surface s, o;
            pattern(&s, (pc_sample_type)st, R);
            run_json(&s, ALL[i], &o);
            outs[st] = region(&o, R, &len);
            for (k = 0; k < len; k++) {
                outs[st][k] = pc_clampf(outs[st][k], 0.0f, 1.0f);
            }
            pc_surface_fini(&s), pc_surface_fini(&o);
        }
        for (k = 1; k < 3; k++) {
            float mx = max_diff_v(outs[0], outs[k], len), mn = 0.0f;
            for (n = 0; n < len; n++) {
                mn += fabsf(outs[0][n] - outs[k][n]);
            }
            mn /= (float)len;
            CHECK(thresholdy ? mn < 0.06f : (mx < 0.06f && mn < 0.01f));
        }
        free(outs[0]), free(outs[1]), free(outs[2]);
        /* Other colour modes keep their format and stay finite. */
        for (m = 0; m < 3; m++) {
            static const pc_color_mode modes[3] = {PC_COLOR_MODE_GRAYSCALE, PC_COLOR_MODE_CMYK,
                                                   PC_COLOR_MODE_LAB};
            pc_pixel_format f = {modes[m], PC_SAMPLE_TYPE_U8, true};
            pc_surface src, s, o;
            float *v;
            int32_t x, y;
            pattern(&src, PC_SAMPLE_TYPE_F32, R);
            pc_surface_init(&s, f);
            for (y = 0; y < 30; y++) {
                for (x = 0; x < 40; x++) {
                    float rgb[4], px[PC_RASTER_MAX_CHANNELS];
                    pc_surface_rgba(&src, x, y, rgb);
                    (void)pc_from_rgba(f, rgb, px);
                    CHECK(pc_surface_write_pixel(&s, x, y, px, pc_pixel_format_channels(f), NULL) == PC_OK);
                }
            }
            run_json(&s, ALL[i], &o);
            CHECK(pc_surface_format(&o).mode == f.mode);
            v = region(&o, R, &len);
            for (k = 0; k < len; k++) {
                CHECK(isfinite(v[k]));
            }
            free(v);
            pc_surface_fini(&src), pc_surface_fini(&s), pc_surface_fini(&o);
        }
    }
}

static void test_identities_at_zero_strength(void)
{
    const char *const cases[3] = {
        "{\"filter\":\"reduceNoise\",\"strength\":0,\"preserveDetails\":60,\"reduceColorNoise\":0,"
        "\"sharpenDetails\":0,"
        "\"removeJpegArtifact\":false}",
        "{\"filter\":\"lensBlur\",\"radius\":0,\"blades\":6,\"curvature\":0,\"rotation\":0,\"depth\":"
        "\"none\","
        "\"focalDistance\":0,\"invertDepth\":false,\"brightness\":0,\"threshold\":255,\"noise\":0,"
        "\"distribution\":\"uniform\",\"monochromatic\":false,\"seed\":0}",
        "{\"filter\":\"shapeBlur\",\"radius\":0,\"shape\":\"heart\"}",
    };
    pc_surface s;
    int k;
    pattern(&s, PC_SAMPLE_TYPE_U8, R);
    for (k = 0; k < 3; k++) {
        pc_surface o;
        size_t la, lb;
        float *a, *b;
        run_json(&s, cases[k], &o);
        a = region(&o, R, &la);
        b = region(&s, R, &lb);
        CHECK(la == lb && max_diff_v(a, b, la) <= 0.5f / 255.0f);
        free(a), free(b);
        pc_surface_fini(&o);
    }
    pc_surface_fini(&s);
}

static void test_flat_images_stay_flat(void)
{
    const float c[4] = {0.3f, 0.55f, 0.8f, 1.0f};
    const char *const cases[4] = {
        "{\"filter\":\"reduceNoise\",\"strength\":10,\"preserveDetails\":0,\"reduceColorNoise\":100,"
        "\"sharpenDetails\":0,\"removeJpegArtifact\":false}",
        "{\"filter\":\"smartBlur\",\"radius\":3,\"threshold\":25,\"quality\":\"high\",\"mode\":\"normal\"}",
        "{\"filter\":\"lensBlur\",\"radius\":5,\"blades\":5,\"curvature\":50,\"rotation\":0,\"depth\":"
        "\"none\","
        "\"focalDistance\":0,\"invertDepth\":false,\"brightness\":0,\"threshold\":255,\"noise\":0,"
        "\"distribution\":\"uniform\",\"monochromatic\":false,\"seed\":0}",
        "{\"filter\":\"shapeBlur\",\"radius\":4,\"shape\":\"ring\"}",
    };
    pc_surface s;
    int k, ch;
    flat(&s, PC_SAMPLE_TYPE_F32, R, c);
    for (k = 0; k < 4; k++) {
        pc_surface o;
        run_json(&s, cases[k], &o);
        for (ch = 0; ch < 4; ch++) {
            CHECK(fabsf(px_at(&o, 20, 15, (size_t)ch) - c[ch]) < 2e-3f);
        }
        pc_surface_fini(&o);
    }
    pc_surface_fini(&s);
}

static float variance(const pc_surface *s, pc_rect r, size_t c)
{
    size_t len, i, n;
    float *v = region(s, r, &len), m = -0.0f, acc = -0.0f;
    n = len / 4;
    for (i = 0; i < n; i++) {
        m += v[i * 4 + c];
    }
    m /= (float)n;
    for (i = 0; i < n; i++) {
        acc += (v[i * 4 + c] - m) * (v[i * 4 + c] - m);
    }
    free(v);
    return acc / (float)n;
}

static void noisy(pc_surface *out)
{
    const float c[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    pc_surface s;
    flat(&s, PC_SAMPLE_TYPE_F32, R, c);
    run_json(&s,
             "{\"filter\":\"addNoise\",\"amount\":20,\"distribution\":\"gaussian\",\"monochromatic\":false,"
             "\"seed\":4}",
             out);
    pc_surface_fini(&s);
}

static void test_reduce_noise_lowers_noise(void)
{
    pc_surface s, o;
    pc_rect inner = pc_rect_make(8, 8, 32, 22);
    size_t c;
    noisy(&s);
    run_json(&s,
             "{\"filter\":\"reduceNoise\",\"strength\":10,\"preserveDetails\":0,\"reduceColorNoise\":100,"
             "\"sharpenDetails\":0,\"removeJpegArtifact\":false}",
             &o);
    for (c = 0; c < 3; c++) {
        CHECK(variance(&o, inner, c) < variance(&s, inner, c) * 0.5f);
    }
    pc_surface_fini(&s), pc_surface_fini(&o);
}

static void test_smart_blur_keeps_edges_and_smooths_noise(void)
{
    const float w[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    pc_surface s, o, e;
    pc_rect r = pc_rect_make(4, 4, 16, 26);
    noisy(&s);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(20, 0, 40, 30), w, 4, NULL) == PC_OK);
    run_json(
        &s,
        "{\"filter\":\"smartBlur\",\"radius\":3,\"threshold\":60,\"quality\":\"high\",\"mode\":\"normal\"}",
        &o);
    CHECK(fabsf(px_at(&o, 20, 15, 0) - 1.0f) < 1e-5f && px_at(&o, 19, 15, 0) < 0.8f);
    CHECK(variance(&o, r, 0) < variance(&s, r, 0));
    run_json(
        &s,
        "{\"filter\":\"smartBlur\",\"radius\":3,\"threshold\":60,\"quality\":\"high\",\"mode\":\"edgeOnly\"}",
        &e);
    CHECK(px_at(&e, 20, 15, 0) == 1.0f && px_at(&e, 30, 15, 0) == 0.0f);
    pc_surface_fini(&s), pc_surface_fini(&o), pc_surface_fini(&e);
}

static void test_shaped_blurs_spread_a_point_into_the_shape(void)
{
    const float k[4] = {0.0f, 0.0f, 0.0f, 1.0f}, w[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    pc_surface s, ring, lb;
    size_t len, i;
    float *v, sum = -0.0f;
    flat(&s, PC_SAMPLE_TYPE_F32, R, k);
    CHECK(pc_surface_write_pixel(&s, 20, 15, w, 4, NULL) == PC_OK);
    run_json(&s, "{\"filter\":\"shapeBlur\",\"radius\":6,\"shape\":\"ring\"}", &ring);
    CHECK(px_at(&ring, 20, 15, 0) == 0.0f && px_at(&ring, 25, 15, 0) > 0.0f);
    run_json(&s,
             "{\"filter\":\"lensBlur\",\"radius\":6,\"blades\":3,\"curvature\":0,\"rotation\":0,\"depth\":"
             "\"none\","
             "\"focalDistance\":0,\"invertDepth\":false,\"brightness\":100,\"threshold\":200,\"noise\":0,"
             "\"distribution\":\"uniform\",\"monochromatic\":false,\"seed\":0}",
             &lb);
    v = region(&lb, R, &len);
    for (i = 0; i < len; i += 4) {
        sum += v[i];
    }
    CHECK(sum > 1.5f);
    CHECK(px_at(&lb, 20, 20, 0) != px_at(&lb, 20, 10, 0));
    free(v);
    pc_surface_fini(&s), pc_surface_fini(&ring), pc_surface_fini(&lb);
}

static void test_lens_blur_depth_map_keeps_focal_plane_sharp(void)
{
    pc_surface s, o;
    pc_algo_image d;
    pc_algo_lens_blur_params p;
    pc_algo_filter f;
    float a[4], b[4];
    int32_t x, y;
    pattern(&s, PC_SAMPLE_TYPE_F32, R);
    CHECK(pc_algo_image_init(&d, R, 1, NULL) == PC_OK);
    for (y = 0; y < 30; y++) {
        for (x = 20; x < 40; x++) {
            d.data[y * 40 + x] = 1.0f;
        }
    }
    memset(&p, 0, sizeof p);
    p.radius = 5.0f, p.blades = 6, p.curvature = 100.0f, p.depth = PC_ALGO_DEPTH_LAYER_MASK,
    p.threshold = 255.0f;
    p.distribution = PC_ALGO_DIST_UNIFORM, p.depth_map = &d;
    CHECK(pc_algo_filter_make(&PC_ALGO_FILTER_LENS_BLUR, &p, &f, NULL) == PC_OK);
    CHECK(run(&s, &f, &o) == PC_OK);
    pc_surface_read_pixel(&o, 5, 10, a);
    pc_surface_read_pixel(&s, 5, 10, b);
    CHECK(memcmp(a, b, sizeof a) == 0);
    pc_surface_read_pixel(&o, 30, 10, a);
    pc_surface_read_pixel(&s, 30, 10, b);
    CHECK(memcmp(a, b, sizeof a) != 0);
    pc_algo_filter_fini(&f);
    pc_algo_image_fini(&d);
    pc_surface_fini(&s), pc_surface_fini(&o);
}

static bool cancel_yes(void *ctx)
{
    (void)ctx;
    return true;
}
static bool cancel_no(void *ctx)
{
    (void)ctx;
    return false;
}
typedef struct prog {
    atomic_uint high, calls;
    atomic_bool bad;
} prog;
static void on_progress(void *ctx, float f)
{
    prog *p = ctx;
    uint32_t bits, cur;
    if (!(f >= 0.0f && f <= 1.0f)) {
        atomic_store(&p->bad, true);
        return;
    }
    memcpy(&bits, &f, 4);
    cur = atomic_load(&p->high);
    while (bits > cur && !atomic_compare_exchange_weak(&p->high, &cur, bits)) {
    }
    atomic_fetch_add(&p->calls, 1);
}

static void test_cancelled_filters_and_progress_never_regresses(void)
{
    pc_surface s, out, ref;
    pc_algo_radius_params rp = {3.0f};
    pc_algo_filter f;
    pc_interrupt yes = {cancel_yes, NULL, NULL};
    prog p;
    pc_interrupt no = {cancel_no, on_progress, &p};
    pc_rect area;
    float high;
    uint32_t hb;
    pattern(&s, PC_SAMPLE_TYPE_U8, R);
    CHECK(pc_algo_filter_make(&PC_ALGO_FILTER_GAUSSIAN_BLUR, &rp, &f, NULL) == PC_OK);
    area = pc_algo_output_area(&f, pc_surface_content_bounds(&s), R, NULL);
    CHECK(pc_algo_apply_tiled_with(&s, &f, area, R, NULL, 16, &R, &yes, &out, NULL) == PC_ERR_CANCELLED);
    atomic_init(&p.high, 0), atomic_init(&p.calls, 0), atomic_init(&p.bad, false);
    CHECK(pc_algo_apply_tiled_with(&s, &f, area, R, NULL, 16, &R, &no, &out, NULL) == PC_OK);
    hb = atomic_load(&p.high);
    memcpy(&high, &hb, 4);
    CHECK(high == 1.0f && atomic_load(&p.calls) >= 1 && !atomic_load(&p.bad));
    CHECK(pc_algo_apply_tiled(&s, &f, area, R, NULL, 16, &R, &ref, NULL) == PC_OK);
    CHECK(regions_equal(&out, &ref, R));
    pc_surface_fini(&s), pc_surface_fini(&out), pc_surface_fini(&ref);
    pc_algo_filter_fini(&f);
}

/* ---- blur.rs ---- */

static uint32_t xs(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

static void image_new(pc_algo_image *img, pc_rect r, size_t ch)
{
    CHECK(pc_algo_image_init(img, r, ch, NULL) == PC_OK);
}

static float *buf(pc_rect out, size_t ch)
{
    return calloc((size_t)pc_rect_width(out) * pc_rect_height(out) * ch + 1, sizeof(float));
}

static size_t spin_8192(const void *octx, float x, float y, float (*pts)[2], size_t cap);
static size_t spin_64(const void *octx, float x, float y, float (*pts)[2], size_t cap);

static void test_radial_blur_samples_large_arcs_beyond_64_intervals(void)
{
    pc_rect bounds = pc_rect_make(0, 0, 1000, 1000), out = pc_rect_make(999, 500, 1000, 501);
    pc_algo_ctx ctx = {bounds, PC_COLOR_MODE_GRAYSCALE, false};
    pc_algo_image img;
    pc_algo_radial_params p = {100.0f, PC_ALGO_RADIAL_SPIN, PC_ALGO_RADIAL_GOOD, 0.5f, 0.5f};
    float cx = 500.0f, cy = 500.0f, dx = 999.5f - cx, dy = 500.5f - cy, arc = pc_algo_to_radians(100.0f), sm,
          s, c;
    float actual;
    int32_t px, py, xx, yy;
    image_new(&img, bounds, 1);
    CHECK_EQ_UINT(pc_algo_radial_intervals(arc * hypotf(dx, dy), PC_ALGO_RADIAL_GOOD), 256);
    CHECK_EQ_UINT(pc_algo_radial_intervals(INFINITY, PC_ALGO_RADIAL_DRAFT), 64);
    CHECK_EQ_UINT(pc_algo_radial_intervals(INFINITY, PC_ALGO_RADIAL_GOOD), 256);
    CHECK_EQ_UINT(pc_algo_radial_intervals(INFINITY, PC_ALGO_RADIAL_BEST), 4096);
    CHECK_EQ_UINT(pc_algo_radial_intervals(NAN, PC_ALGO_RADIAL_BEST), 1);
    /* A bright patch halfway between two samples of the former 64-interval limit. */
    sm = (31.5f / 64.0f - 0.5f) * arc;
    s = sinf(sm), c = cosf(sm);
    px = pc_f32_to_i32(roundf(cx + dx * c - dy * s - 0.5f));
    py = pc_f32_to_i32(roundf(cy + dx * s + dy * c - 0.5f));
    for (yy = py - 1; yy <= py + 1; yy++) {
        for (xx = px - 1; xx <= px + 1; xx++) {
            img.data[yy * 1000 + xx] = 1.0f;
        }
    }
    {
        float d[1] = {0};
        CHECK(pc_algo_radial_k(&img, out, &ctx, &p, d, NULL) == PC_OK);
        actual = d[0];
    }
    {
        float expected = 0, coarse = 0;
        CHECK(pc_algo_average_samples(&img, out, &ctx, spin_8192, NULL, 8193, &expected, NULL) == PC_OK);
        CHECK(pc_algo_average_samples(&img, out, &ctx, spin_64, NULL, 65, &coarse, NULL) == PC_OK);
        CHECK(actual > 0.001f);
        CHECK(fabsf(actual - expected) < 0.0015f);
        CHECK(coarse < expected * 0.1f);
    }
    pc_algo_image_fini(&img);
}

static void test_convolution_matches_scalar_tap_order(void)
{
    size_t n, wi, ri, x, i, c;
    static const size_t widths[3] = {1, 7, 256};
    static const float radii[4] = {0.0f, 0.5f, 3.0f, 4.0f};
    const float one = 1.0f;
    for (n = 1; n <= 5; n++) {
        for (wi = 0; wi < 3; wi++) {
            for (ri = 0; ri < 4; ri++) {
                float *k, *want, *got;
                size_t nk, w = widths[wi];
                pc_algo_image src;
                pc_rect out = pc_rect_make(0, 0, (int32_t)w, 1);
                CHECK(pc_algo_gaussian_kernel(radii[ri], &k, &nk, NULL) == PC_OK);
                image_new(&src, pc_rect_make(-(int32_t)(nk / 2), 0, (int32_t)(w + nk / 2), 1), n);
                for (i = 0; i < src.len; i++) {
                    src.data[i] = (float)((i * 73) % 251) / 250.0f;
                }
                want = buf(out, n), got = buf(out, n);
                for (x = 0; x < w; x++) {
                    for (i = 0; i < nk; i++) {
                        for (c = 0; c < n; c++) {
                            want[x * n + c] += src.data[(x + i) * n + c] * k[i];
                        }
                    }
                }
                CHECK(pc_algo_conv_sep(&src, out, k, nk, &one, 1, false, got, NULL) == PC_OK);
                CHECK(memcmp(want, got, w * n * sizeof(float)) == 0);
                free(k), free(want), free(got);
                pc_algo_image_fini(&src);
            }
        }
    }
}

static void gauss_boxes(float sigma, size_t out[3])
{
    pc_algo_radius_params p = {sigma};
    size_t nb;
    CHECK(pc_algo_box_widths(&PC_ALGO_FILTER_GAUSSIAN_BLUR, &p, out, &nb, NULL) == PC_OK && nb == 3);
}

static void test_box_gaussian_matches_exact_kernel(void)
{
    pc_rect sr = pc_rect_make(-60, -60, 180, 68), out = pc_rect_make(0, 0, 120, 8);
    pc_algo_image img;
    size_t boxes[3], nk, i;
    float *k, *exact = buf(out, 2), *fast = buf(out, 2);
    image_new(&img, sr, 2);
    for (i = 0; i < img.len / 2; i++) {
        img.data[i * 2] = sr.x0 + (int32_t)(i % 240) < 60 ? 1.0f : 0.0f;
        img.data[i * 2 + 1] = 1.0f;
    }
    CHECK(pc_algo_gaussian_kernel(12.0f, &k, &nk, NULL) == PC_OK);
    CHECK(pc_algo_conv_sep(&img, out, k, nk, k, nk, true, exact, NULL) == PC_OK);
    gauss_boxes(12.0f, boxes);
    CHECK(pc_algo_box_passes(&img, out, true, boxes, 3, fast, NULL) == PC_OK);
    CHECK(max_diff_v(exact, fast, 120 * 8 * 2) < 0.02f);
    free(k), free(exact), free(fast);
    pc_algo_image_fini(&img);
}

static void test_box_passes_match_the_direct_box_kernel(void)
{
    static const size_t rs[3] = {5, 17, 60};
    int t;
    for (t = 0; t < 3; t++) {
        size_t r = rs[t], nk = 2 * r + 1, i;
        int32_t m = (int32_t)r + 2;
        pc_rect sr = pc_rect_make(-m, -m, 40 + m, 24 + m), out = pc_rect_make(0, 0, 40, 24);
        pc_algo_image img;
        pc_algo_ctx ctx = {sr, PC_COLOR_MODE_RGB, true};
        uint32_t s = 0x9e3779b9u;
        float *k = malloc(nk * sizeof(float)), *direct = buf(out, 4), *fast = buf(out, 4);
        image_new(&img, sr, 4);
        for (i = 0; i < img.len; i++) {
            img.data[i] = (float)(xs(&s) % 256) / 255.0f;
        }
        for (i = 0; i < nk; i++) {
            k[i] = 1.0f / (float)nk;
        }
        CHECK(pc_algo_conv_sep(&img, out, k, nk, k, nk, true, direct, NULL) == PC_OK);
        CHECK(pc_algo_boxed_k(&img, out, &ctx, (float)r, fast, NULL) == PC_OK);
        CHECK(max_diff_v(direct, fast, 40 * 24 * 4) < 1e-5f);
        free(k), free(direct), free(fast);
        pc_algo_image_fini(&img);
    }
}

/* Each box applied in turn as a direct convolution of img for out. */
static float *direct_boxes(const pc_algo_image *img, pc_rect out, const size_t *boxes, size_t nb, bool alpha)
{
    pc_algo_image cur;
    int32_t left = 0;
    size_t b, i;
    CHECK(pc_algo_image_clone(img, &cur, NULL) == PC_OK);
    for (b = 0; b < nb; b++) {
        left += (int32_t)(boxes[b] / 2);
    }
    for (b = 0; b < nb; b++) {
        size_t w = boxes[b];
        float *k = malloc(w * sizeof(float)), *d;
        pc_rect rect;
        pc_algo_image next;
        left -= (int32_t)(w / 2);
        for (i = 0; i < w; i++) {
            k[i] = 1.0f / (float)w;
        }
        rect = pc_rect_inflate(out, left);
        d = buf(rect, img->ch);
        CHECK(pc_algo_conv_sep(&cur, rect, k, w, k, w, alpha, d, NULL) == PC_OK);
        CHECK(pc_algo_image_wrap(&next, rect, img->ch, d,
                                 (size_t)pc_rect_width(rect) * pc_rect_height(rect) * img->ch,
                                 NULL) == PC_OK);
        pc_algo_image_fini(&cur);
        cur = next;
        free(k);
    }
    return cur.data;
}

static void noisy_surface(pc_surface *s, pc_pixel_format f, pc_rect r, uint32_t seed)
{
    size_t n = pc_pixel_format_channels(f), len = (size_t)pc_rect_width(r) * pc_rect_height(r) * n, i;
    float *v = malloc(len * sizeof(float));
    uint32_t st = seed;
    for (i = 0; i < len; i++) {
        float x = (float)(xs(&st) % 256) / 255.0f;
        v[i] = f.alpha && i % n == n - 1 && st % 5 == 0 ? 0.0f : x;
    }
    pc_surface_init(s, f);
    CHECK(pc_surface_write_region(s, r, v, len, NULL) == PC_OK);
    free(v);
}

static void test_layer_box_blurs_match_direct_convolution_at_any_radius(void)
{
    pc_rect canvas = pc_rect_make(0, 0, 37, 23);
    const pc_pixel_format fmts[6] = {PC_PIXEL_FORMAT_RGBA8,   PC_PIXEL_FORMAT_RGBA16,
                                     PC_PIXEL_FORMAT_RGBA32F, PC_PIXEL_FORMAT_GRAY8,
                                     PC_PIXEL_FORMAT_CMYKA8,  {PC_COLOR_MODE_LAB, PC_SAMPLE_TYPE_F32, false}};
    const char *const ps[5] = {
        "{\"filter\":\"gaussianBlur\",\"radius\":4.5}", "{\"filter\":\"gaussianBlur\",\"radius\":9.3}",
        "{\"filter\":\"gaussianBlur\",\"radius\":40}", "{\"filter\":\"boxBlur\",\"radius\":5}",
        "{\"filter\":\"boxBlur\",\"radius\":70}"};
    int fi, pi, way;
    for (fi = 0; fi < 6; fi++) {
        pc_surface s;
        float tol = (fmts[fi].sample == PC_SAMPLE_TYPE_U8    ? 1.0f / 255.0f
                     : fmts[fi].sample == PC_SAMPLE_TYPE_U16 ? 1.0f / 65535.0f
                                                             : 0.0f) +
                    2e-5f;
        noisy_surface(&s, fmts[fi], canvas, 0x9e3779b9u + (uint32_t)fi);
        for (pi = 0; pi < 5; pi++) {
            pc_algo_filter f;
            size_t boxes[3], nb, reach = 0, b;
            pc_rect area;
            if (!make(ps[pi], &f)) {
                continue;
            }
            CHECK(pc_algo_box_widths(f.def, f.params, boxes, &nb, NULL) == PC_OK && nb > 0);
            for (b = 0; b < nb; b++) {
                reach += boxes[b] / 2;
            }
            area = pc_algo_output_area(&f, pc_surface_content_bounds(&s), canvas, NULL);
            for (way = 0; way < 2; way++) {
                pc_surface got;
                pc_algo_image img;
                pc_rect out = way == 0 ? canvas : area;
                size_t len, n = pc_pixel_format_channels(fmts[fi]);
                float *g, *w;
                if (way == 0) {
                    CHECK(pc_algo_apply_in(&s, &f, area, canvas, NULL, canvas, &got, NULL) == PC_OK);
                    CHECK(pc_algo_image_read_clamped(&s, pc_rect_inflate(canvas, (int32_t)reach), canvas,
                                                     &img, NULL) == PC_OK);
                } else {
                    CHECK(pc_algo_apply(&s, &f, area, canvas, NULL, &got, NULL) == PC_OK);
                    CHECK(pc_algo_image_read(&s, pc_rect_inflate(area, (int32_t)reach), &img, NULL) == PC_OK);
                }
                g = region(&got, out, &len);
                w = direct_boxes(&img, out, boxes, nb, fmts[fi].alpha);
                pc_algo_premultiply(g, len, n, fmts[fi].alpha);
                pc_algo_premultiply(w, len, n, fmts[fi].alpha);
                if (!(max_diff_v(g, w, len) <= tol)) {
                    PC_T_FAIL("%s format %d way %d: max error %g", ps[pi], fi, way,
                              (double)max_diff_v(g, w, len));
                }
                free(g), free(w);
                pc_algo_image_fini(&img);
                pc_surface_fini(&got);
            }
            pc_algo_filter_fini(&f);
        }
        pc_surface_fini(&s);
    }
}

static void test_layer_box_blurs_mix_by_the_selection_and_stop_when_cancelled(void)
{
    pc_rect canvas = pc_rect_make(0, 0, 37, 23);
    const int32_t pts[3][2] = {{0, 0}, {18, 11}, {36, 22}};
    pc_pixel_format g = {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_F32, false};
    const float half = 0.5f;
    pc_algo_radius_params rp = {12.0f};
    pc_interrupt yes = {cancel_yes, NULL, NULL};
    pc_surface s, full, mixed, sel, o;
    pc_algo_filter f;
    int k, c;
    noisy_surface(&s, PC_PIXEL_FORMAT_RGBA32F, canvas, 7);
    CHECK(pc_algo_filter_make(&PC_ALGO_FILTER_GAUSSIAN_BLUR, &rp, &f, NULL) == PC_OK);
    pc_surface_init_default(&sel, g, &half, 1);
    CHECK(pc_algo_apply_in(&s, &f, canvas, canvas, NULL, canvas, &full, NULL) == PC_OK);
    CHECK(pc_algo_apply_in(&s, &f, canvas, canvas, &sel, canvas, &mixed, NULL) == PC_OK);
    for (k = 0; k < 3; k++) {
        for (c = 0; c < 4; c++) {
            float ov = px_at(&s, pts[k][0], pts[k][1], (size_t)c),
                  fv = px_at(&full, pts[k][0], pts[k][1], (size_t)c);
            CHECK(fabsf(px_at(&mixed, pts[k][0], pts[k][1], (size_t)c) - (ov + fv) / 2.0f) < 1e-5f);
        }
    }
    CHECK(pc_algo_apply_in_with(&s, &f, canvas, canvas, NULL, canvas, &yes, &o, NULL) == PC_ERR_CANCELLED);
    pc_algo_filter_fini(&f);
    pc_surface_fini(&s), pc_surface_fini(&full), pc_surface_fini(&mixed), pc_surface_fini(&sel);
}

static void test_surface_blur_8bit_histogram_matches_the_direct_sum(void)
{
    static const float cases[4][2] = {{1, 2}, {4, 15}, {12, 60}, {12, 255}};
    pc_rect sr = pc_rect_make(-14, -14, 40, 34), out = pc_rect_make(-2, 0, 26, 20);
    pc_algo_ctx ctx = {sr, PC_COLOR_MODE_RGB, true};
    pc_algo_image img;
    uint32_t s = 0x6c078965u;
    size_t i, c;
    int k;
    bool declined;
    image_new(&img, sr, 4);
    for (i = 0; i < img.len; i += 4) {
        for (c = 0; c < 4; c++) {
            img.data[i + c] = (float)(xs(&s) % 256) / 255.0f;
        }
        if (s % 7 == 0) {
            img.data[i + 3] = 0.0f;
        }
    }
    for (k = 0; k < 4; k++) {
        float t = (cases[k][1] / 255.0f) * 2.5f, *fast = buf(out, 4), *direct = buf(out, 4);
        size_t reach = pc_f32_to_size(ceilf(t * 255.0f));
        reach = reach < 255 ? reach : 255;
        CHECK(pc_algo_surface_8bit(&img, out, &ctx, (int32_t)cases[k][0], t, reach, fast, &declined, NULL) ==
                  PC_OK &&
              !declined);
        CHECK(pc_algo_surface_direct(&img, out, &ctx, (int32_t)cases[k][0], t, direct, NULL) == PC_OK);
        CHECK(max_diff_v(direct, fast, 28 * 20 * 4) < 1e-5f);
        free(fast), free(direct);
    }
    img.data[(14 * 54 + 14) * 4] = 0.5f / 255.0f;
    {
        float *d = buf(out, 4);
        CHECK(pc_algo_surface_8bit(&img, out, &ctx, 4, 0.1f, 26, d, &declined, NULL) == PC_OK && declined);
        free(d);
    }
    pc_algo_image_fini(&img);
}

typedef struct off_ctx {
    float (*o)[2];
    size_t n;
} off_ctx;

static size_t fixed_offsets(const void *octx, float x, float y, float (*pts)[2], size_t cap)
{
    const off_ctx *c = octx;
    size_t i;
    for (i = 0; i < c->n && i < cap; i++) {
        pts[i][0] = x + c->o[i][0];
        pts[i][1] = y + c->o[i][1];
    }
    return i;
}

static void test_motion_kernel_matches_the_per_pixel_samples(void)
{
    static const float cases[7][2] = {{0, 1}, {0, 9}, {30, 7}, {90, 12}, {-45, 20}, {137, 33.5f}, {200, 40}};
    pc_rect sr = pc_rect_make(-30, -30, 50, 40), out = pc_rect_make(-12, -6, 30, 22);
    pc_algo_image img;
    uint32_t s = 0x1b873593u;
    size_t i, c;
    int al, k;
    image_new(&img, sr, 4);
    for (i = 0; i < img.len; i += 4) {
        for (c = 0; c < 4; c++) {
            img.data[i + c] = (float)(xs(&s) % 1000) / 999.0f;
        }
        if (s % 5 == 0) {
            img.data[i + 3] = 0.0f;
        }
    }
    for (al = 0; al < 2; al++) {
        pc_algo_ctx ctx = {sr, PC_COLOR_MODE_RGB, al != 0};
        for (k = 0; k < 7; k++) {
            float *fast = buf(out, 4), *slow = buf(out, 4);
            off_ctx oc;
            CHECK(pc_algo_motion_k(&img, out, &ctx, cases[k][0], cases[k][1], fast, NULL) == PC_OK);
            CHECK(pc_algo_motion_offsets(cases[k][0], cases[k][1], &oc.o, &oc.n, NULL) == PC_OK);
            CHECK(pc_algo_average_samples(&img, out, &ctx, fixed_offsets, &oc, oc.n, slow, NULL) == PC_OK);
            CHECK(max_diff_v(slow, fast, 42 * 28 * 4) < 1e-5f);
            free(oc.o);
            free(fast), free(slow);
        }
    }
    pc_algo_image_fini(&img);
}

static void spin_points(float x, float y, float (*pts)[2], size_t intervals)
{
    float cx = 500.0f, cy = 500.0f, dx = x - cx, dy = y - cy, arc = pc_algo_to_radians(100.0f);
    size_t i;
    for (i = 0; i <= intervals; i++) {
        float t = ((float)i / (float)intervals - 0.5f) * arc, s = sinf(t), c = cosf(t);
        pts[i][0] = cx + dx * c - dy * s;
        pts[i][1] = cy + dx * s + dy * c;
    }
}
static size_t spin_8192(const void *octx, float x, float y, float (*pts)[2], size_t cap)
{
    (void)octx, (void)cap;
    spin_points(x, y, pts, 8192);
    return 8193;
}
static size_t spin_64(const void *octx, float x, float y, float (*pts)[2], size_t cap)
{
    (void)octx, (void)cap;
    spin_points(x, y, pts, 64);
    return 65;
}

int main(int argc, char **argv)
{
    RUN(test_every_filter_is_tile_independent);
    RUN(test_every_filter_changes_something_and_respects_zero_selection);
    RUN(test_params_serde_roundtrip);
    RUN(test_radial_blur_quality_defaults_for_older_parameters);
    RUN(test_gaussian_halo_and_output_area);
    RUN(test_gaussian_identity_at_tiny_radius);
    RUN(test_blur_preserves_flat_colour_and_mass);
    RUN(test_blur_keeps_16bit_precision);
    RUN(test_float_surfaces_keep_hdr_values);
    RUN(test_gaussian_sigma_matches_radius);
    RUN(test_unsharp_threshold_and_contrast);
    RUN(test_high_pass_of_flat_is_mid_gray);
    RUN(test_noise_is_deterministic_monochrome_and_centred);
    RUN(test_median_and_dust_remove_a_speck);
    RUN(test_motion_blur_spreads_along_angle_only);
    RUN(test_layer_filters_repeat_the_canvas_edge);
    RUN(test_new_filters_agree_across_bit_depths_and_modes);
    RUN(test_identities_at_zero_strength);
    RUN(test_flat_images_stay_flat);
    RUN(test_reduce_noise_lowers_noise);
    RUN(test_smart_blur_keeps_edges_and_smooths_noise);
    RUN(test_shaped_blurs_spread_a_point_into_the_shape);
    RUN(test_lens_blur_depth_map_keeps_focal_plane_sharp);
    RUN(test_cancelled_filters_and_progress_never_regresses);
    RUN(test_radial_blur_samples_large_arcs_beyond_64_intervals);
    RUN(test_convolution_matches_scalar_tap_order);
    RUN(test_box_gaussian_matches_exact_kernel);
    RUN(test_box_passes_match_the_direct_box_kernel);
    RUN(test_layer_box_blurs_match_direct_convolution_at_any_radius);
    RUN(test_layer_box_blurs_mix_by_the_selection_and_stop_when_cancelled);
    RUN(test_surface_blur_8bit_histogram_matches_the_direct_sum);
    RUN(test_motion_kernel_matches_the_per_pixel_samples);
    return pc_t_finish();
}
