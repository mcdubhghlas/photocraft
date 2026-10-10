/* Gradient tool and Paint Bucket (src/algo/algo_paint.c): the crates/algo/src/paint.rs tests plus
 * never-crash cases. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "harness.h"
#include "raster/pixel.h"

static void test_gradient_t_shapes(void)
{
    const float f[2] = {0, 0}, t[2] = {10, 0};
    CHECK(pc_algo_paint_gradient_t(PC_ALGO_GRADIENT_LINEAR, f, t, 5, 3) == 0.5f);
    CHECK(pc_algo_paint_gradient_t(PC_ALGO_GRADIENT_LINEAR, f, t, -5, 0) == 0.0f);
    CHECK(pc_algo_paint_gradient_t(PC_ALGO_GRADIENT_RADIAL, f, t, 0, 5) == 0.5f);
    CHECK(pc_algo_paint_gradient_t(PC_ALGO_GRADIENT_REFLECTED, f, t, -5, 0) == 0.5f);
    CHECK(pc_algo_paint_gradient_t(PC_ALGO_GRADIENT_DIAMOND, f, t, 2.5f, 2.5f) == 0.5f);
    CHECK(fabsf(pc_algo_paint_gradient_t(PC_ALGO_GRADIENT_ANGLE, f, t, 0, 5) - 0.25f) < 1e-5f);
}

static void test_gradient_paints_ramp_within_selection(void)
{
    const pc_algo_gradient_stop stops[2] = {{0.0f, {0, 0, 0, 1}}, {1.0f, {1, 1, 1, 1}}};
    const float from[2] = {0.5f, 0.0f}, to[2] = {10.5f, 0.0f}, one = 1.0f;
    pc_surface s, sel;
    float p[4];
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    pc_surface_init(&sel, PC_PIXEL_FORMAT_GRAY8);
    CHECK(pc_surface_fill_rect(&sel, pc_rect_make(0, 0, 11, 1), &one, 1, NULL) == PC_OK);
    CHECK(pc_algo_paint_gradient(&s, pc_rect_make(0, 0, 11, 2), from, to, PC_ALGO_GRADIENT_LINEAR, stops, 2,
                                 false, 1.0f, PC_BLEND_NORMAL, false, &sel, false, NULL) == PC_OK);
    pc_surface_read_pixel(&s, 0, 0, p);
    CHECK(p[0] < 0.01f);
    pc_surface_read_pixel(&s, 10, 0, p);
    CHECK(p[0] > 0.99f);
    pc_surface_read_pixel(&s, 5, 0, p);
    CHECK(fabsf(p[0] - 0.5f) < 0.01f);
    pc_surface_read_pixel(&s, 5, 1, p);
    CHECK(p[3] == 0.0f); /* outside the selection */
    pc_surface_fini(&s), pc_surface_fini(&sel);
}

static void test_bucket_fills_contiguous_region(void)
{
    const float white[4] = {1, 1, 1, 1}, black[4] = {0, 0, 0, 1}, red[4] = {1, 0, 0, 1};
    pc_surface s;
    pc_rect a = pc_rect_make(0, 0, 10, 10);
    float p[4];
    bool filled = false;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_surface_fill_rect(&s, a, white, 4, NULL) == PC_OK);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(5, 0, 6, 10), black, 4, NULL) == PC_OK);
    CHECK(pc_algo_paint_bucket_fill(&s, a, 1, 1, 10.0f, true, false, red, 1.0f, NULL, false, &filled, NULL) ==
          PC_OK);
    CHECK(filled);
    pc_surface_read_pixel(&s, 2, 2, p);
    CHECK(p[0] == 1.0f && p[1] == 0.0f && p[2] == 0.0f && p[3] == 1.0f);
    pc_surface_read_pixel(&s, 8, 2, p);
    CHECK(p[0] == 1.0f && p[1] == 1.0f && p[2] == 1.0f && p[3] == 1.0f);
    CHECK(pc_algo_paint_bucket_fill(&s, a, 50, 1, 10.0f, true, false, red, 1.0f, NULL, false, &filled,
                                    NULL) == PC_OK);
    CHECK(!filled);
    pc_surface_fini(&s);
}

static void red_src(void *ctx, int32_t x, int32_t y, float out[4])
{
    (void)ctx, (void)x, (void)y;
    out[0] = 1, out[1] = 0, out[2] = 0, out[3] = 1;
}

static void test_bucket_region_uses_document_coordinates_and_multiplies_coverage(void)
{
    uint8_t mask[3] = {255, 128, 255};
    pc_algo_region region = {{-3, 4, 0, 5}, mask, 3};
    const float half = 0.5f;
    pc_surface s, sel;
    float rgba[4];
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    pc_surface_init(&sel, PC_PIXEL_FORMAT_GRAY8);
    CHECK(pc_surface_fill_rect(&sel, pc_rect_make(-3, 4, -1, 5), &half, 1, NULL) == PC_OK);
    CHECK(pc_algo_paint_bucket_fill_region_src(&s, &region, 0.5f, &sel, false, red_src, NULL, NULL) == PC_OK);
    pc_surface_rgba(&s, -3, 4, rgba);
    CHECK(fabsf(rgba[3] - 0.25f) < 0.005f);
    pc_surface_rgba(&s, -2, 4, rgba);
    CHECK(fabsf(rgba[3] - 0.125f) < 0.005f);
    pc_surface_rgba(&s, -1, 4, rgba);
    CHECK(rgba[0] == 0.0f && rgba[3] == 0.0f); /* outside the selection */
    pc_surface_rgba(&s, -4, 4, rgba);
    CHECK(rgba[0] == 0.0f && rgba[3] == 0.0f); /* outside the region */
    pc_surface_fini(&s), pc_surface_fini(&sel);
}

static void test_names_and_stops(void)
{
    const pc_algo_gradient_stop stops[3] = {{0.2f, {0, 0, 0, 0}}, {0.2f, {1, 1, 1, 1}}, {0.8f, {0, 1, 0, 1}}};
    pc_algo_gradient_shape s;
    float o[4];
    CHECK_EQ_STR(pc_algo_gradient_shape_name(PC_ALGO_GRADIENT_REFLECTED), "reflected");
    CHECK(pc_algo_gradient_shape_from_name("diamond", 7, &s) && s == PC_ALGO_GRADIENT_DIAMOND);
    CHECK(!pc_algo_gradient_shape_from_name("Linear", 6, &s));
    pc_algo_paint_sample_stops(NULL, 0, 0.5f, o);
    CHECK(o[0] == 0.0f && o[3] == 0.0f);
    pc_algo_paint_sample_stops(stops, 3, 0.1f, o);
    CHECK(o[3] == 0.0f);
    pc_algo_paint_sample_stops(stops, 3, 0.2f, o); /* first window wins a coincident stop */
    CHECK(o[3] == 0.0f);
    pc_algo_paint_sample_stops(stops, 3, 0.5f, o);
    CHECK(fabsf(o[0] - 0.5f) < 1e-6f && o[1] == 1.0f);
    pc_algo_paint_sample_stops(stops, 3, 2.0f, o);
    CHECK(o[0] == 0.0f && o[1] == 1.0f);
}

static void nan_src(void *ctx, int32_t x, int32_t y, float out[4])
{
    (void)ctx, (void)x, (void)y;
    out[0] = NAN, out[1] = INFINITY, out[2] = -1.0f, out[3] = NAN;
}

static void test_never_crash(void)
{
    const pc_algo_gradient_stop stops[2] = {{NAN, {NAN, 0, 0, 1}}, {INFINITY, {1, INFINITY, 1, 1}}};
    const float from[2] = {NAN, 0}, to[2] = {INFINITY, -INFINITY}, red[4] = {1, 0, 0, 1};
    uint8_t mask[4] = {255, 255, 255, 255};
    pc_algo_region rg = {{INT32_MAX - 2, INT32_MAX - 2, INT32_MAX, INT32_MAX}, mask, 4};
    pc_surface s;
    bool filled;
    pc_surface_init(&s, PC_PIXEL_FORMAT_CMYKA8);
    /* NaN geometry, stops and opacity; every shape; an empty area; areas at the coordinate edge. */
    CHECK(pc_algo_paint_gradient(&s, pc_rect_make(0, 0, 30, 20), from, to, PC_ALGO_GRADIENT_ANGLE, stops, 2,
                                 true, NAN, PC_BLEND_LUMINOSITY, true, NULL, true, NULL) == PC_OK);
    CHECK(pc_algo_paint_gradient(&s, pc_rect_make(0, 0, 30, 20), to, to, PC_ALGO_GRADIENT_DIAMOND, stops, 0,
                                 false, 2.0f, PC_BLEND_DIFFERENCE, false, NULL, false, NULL) == PC_OK);
    CHECK(pc_algo_paint_gradient(&s, PC_RECT_EMPTY, from, to, PC_ALGO_GRADIENT_RADIAL, stops, 2, false, 1.0f,
                                 PC_BLEND_NORMAL, false, NULL, false, NULL) == PC_OK);
    CHECK(pc_algo_paint_gradient(&s, pc_rect_make(INT32_MAX - 5, INT32_MIN, INT32_MAX, INT32_MIN + 3), from,
                                 to, PC_ALGO_GRADIENT_LINEAR, stops, 2, false, 1.0f, PC_BLEND_NORMAL, false,
                                 NULL, false, NULL) == PC_OK);
    CHECK(pc_algo_paint_gradient(&s, pc_rect_make(0, 0, 3, 3), NULL, to, PC_ALGO_GRADIENT_LINEAR, stops, 2,
                                 false, 1.0f, PC_BLEND_NORMAL, false, NULL, false, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_paint_bucket_fill_region_src(&s, &rg, 1.0f, NULL, false, nan_src, NULL, NULL) == PC_OK);
    CHECK(pc_algo_paint_bucket_fill_region_src(&s, &rg, 1.0f, NULL, false, NULL, NULL, NULL) ==
          PC_ERR_INVALID);
    CHECK(pc_algo_paint_bucket_fill(&s, pc_rect_make(0, 0, 30, 20), 3, 3, NAN, false, true, red, NAN, NULL,
                                    true, &filled, NULL) == PC_OK);
    CHECK(pc_algo_paint_bucket_fill(&s, pc_rect_make(0, 0, 30, 20), 3, 3, 1e30f, true, true, red, 1.0f, &s,
                                    false, &filled, NULL) == PC_OK);
    CHECK(pc_algo_paint_bucket_fill(&s, pc_rect_make(0, 0, 30, 20), 3, 3, 1.0f, true, true, NULL, 1.0f, NULL,
                                    false, &filled, NULL) == PC_ERR_INVALID);
    pc_surface_fini(&s);
}

int main(int argc, char **argv)
{
    RUN(test_gradient_t_shapes);
    RUN(test_gradient_paints_ramp_within_selection);
    RUN(test_bucket_fills_contiguous_region);
    RUN(test_bucket_region_uses_document_coordinates_and_multiplies_coverage);
    RUN(test_names_and_stops);
    RUN(test_never_crash);
    return pc_t_finish();
}
