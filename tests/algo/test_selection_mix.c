/* tests_selection_mix.rs: the tiled driver mixes filtered pixels back by the selection's
 * coverage, premultiplied where the filter changed alpha. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/filter.h"
#include "algo/image.h"
#include "harness.h"

static void selection(pc_surface *s, float coverage)
{
    const pc_pixel_format f = {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_F32, false};
    pc_surface_init_default(s, f, &coverage, 1);
}

static bool filter_of(const char *json, pc_algo_filter *f)
{
    return pc_algo_filter_from_json_str(json, strlen(json), f, NULL) == PC_OK;
}

static void test_feathered_color_to_alpha_preserves_appearance_over_the_key(void)
{
    static const float covs[5] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    static const pc_color_mode modes[2] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_GRAYSCALE};
    static const float alphas[2] = {0.5f, 1.0f};
    pc_rect r = pc_rect_make(0, 0, 1, 1);
    pc_algo_filter f;
    int st, m, al, k;
    CHECK(filter_of("{\"filter\":\"colorToAlpha\",\"color\":[1,1,1,1],\"transparencyThreshold\":0,"
                    "\"opacityThreshold\":1}",
                    &f));
    for (st = 0; st < PC_SAMPLE_TYPE_COUNT; st++) {
        for (m = 0; m < 2; m++) {
            for (al = 0; al < 2; al++) {
                pc_pixel_format fmt = {modes[m], (pc_sample_type)st, true};
                size_t n = pc_pixel_format_channels(fmt), c, a = n - 1;
                float p[PC_RASTER_MAX_CHANNELS], orig[PC_RASTER_MAX_CHANNELS];
                float tol = st == PC_SAMPLE_TYPE_U8 ? 2.0f / 255.0f : 1e-4f;
                pc_surface s;
                pc_surface_init(&s, fmt);
                for (c = 0; c < n; c++) {
                    p[c] = 0.5f;
                }
                p[a] = alphas[al];
                CHECK(pc_surface_write_pixel(&s, 0, 0, p, n, NULL) == PC_OK);
                pc_surface_read_pixel(&s, 0, 0, orig);
                for (k = 0; k < 5; k++) {
                    pc_surface sel, out;
                    float got[PC_RASTER_MAX_CHANNELS];
                    selection(&sel, covs[k]);
                    CHECK(pc_algo_apply(&s, &f, r, r, &sel, &out, NULL) == PC_OK);
                    pc_surface_read_pixel(&out, 0, 0, got);
                    for (c = 0; c < a; c++) {
                        float before = orig[c] * orig[a] + 1.0f - orig[a];
                        float after = got[c] * got[a] + 1.0f - got[a];
                        CHECK(fabsf(before - after) < tol);
                    }
                    pc_surface_fini(&out), pc_surface_fini(&sel);
                }
                pc_surface_fini(&s);
            }
        }
    }
    pc_algo_filter_fini(&f);
}

static void test_feathered_offset_ignores_hidden_colour_of_the_original_pixel(void)
{
    pc_rect r = pc_rect_make(0, 0, 2, 1);
    const float red0[4] = {1.0f, 0.0f, 0.0f, 0.0f}, blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    pc_algo_filter f;
    int st;
    CHECK(filter_of("{\"filter\":\"offset\",\"horizontal\":1,\"vertical\":0,\"undefined\":\"wrap\"}", &f));
    for (st = 0; st < PC_SAMPLE_TYPE_COUNT; st++) {
        pc_pixel_format fmt = {PC_COLOR_MODE_RGB, (pc_sample_type)st, true};
        pc_surface s, sel, out;
        float p[4];
        pc_surface_init(&s, fmt);
        CHECK(pc_surface_write_pixel(&s, 0, 0, red0, 4, NULL) == PC_OK);
        CHECK(pc_surface_write_pixel(&s, 1, 0, blue, 4, NULL) == PC_OK);
        selection(&sel, 0.5f);
        CHECK(pc_algo_apply(&s, &f, r, r, &sel, &out, NULL) == PC_OK);
        pc_surface_read_pixel(&out, 0, 0, p);
        CHECK(p[0] == 0.0f && p[1] == 0.0f && p[2] == 1.0f); /* hidden red must not leak */
        CHECK(fabsf(p[3] - 0.5f) < 1.0f / 255.0f);
        pc_surface_fini(&s), pc_surface_fini(&sel), pc_surface_fini(&out);
    }
    pc_algo_filter_fini(&f);
}

static void test_feathered_blurs_do_not_reintroduce_hidden_colour(void)
{
    static const char *const js[2] = {"{\"filter\":\"boxBlur\",\"radius\":1.0}",
                                      "{\"filter\":\"motionBlur\",\"angle\":0.0,\"distance\":3.0}"};
    pc_rect r = pc_rect_make(0, 0, 3, 3);
    const float red0[4] = {1.0f, 0.0f, 0.0f, 0.0f}, blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    pc_surface s;
    int k;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA32F);
    CHECK(pc_surface_fill_rect(&s, r, red0, 4, NULL) == PC_OK);
    CHECK(pc_surface_write_pixel(&s, 1, 1, blue, 4, NULL) == PC_OK);
    for (k = 0; k < 2; k++) {
        pc_algo_filter f;
        pc_surface sel, full, out;
        float p[4], q[4];
        CHECK(filter_of(js[k], &f));
        selection(&sel, 0.5f);
        CHECK(pc_algo_apply(&s, &f, r, r, NULL, &full, NULL) == PC_OK);
        CHECK(pc_algo_apply(&s, &f, r, r, &sel, &out, NULL) == PC_OK);
        pc_surface_read_pixel(&out, 0, 1, p);
        pc_surface_read_pixel(&full, 0, 1, q);
        CHECK(p[3] > 0.0f);
        CHECK(fabsf(p[3] - q[3] * 0.5f) < 1e-6f);
        CHECK(fabsf(p[0]) < 1e-6f && fabsf(p[2] - 1.0f) < 1e-6f);
        pc_surface_fini(&sel), pc_surface_fini(&full), pc_surface_fini(&out);
        pc_algo_filter_fini(&f);
    }
    pc_surface_fini(&s);
}

static void test_selection_mixing_keeps_hdr_range_and_exact_endpoints(void)
{
    pc_rect r = pc_rect_make(0, 0, 1, 1);
    float od[4] = {-2.0f, 4.0f, 0.5f, 0.25f}, filtered[4] = {8.0f, -4.0f, 0.75f, 1.0f}, data[4];
    const float want[4] = {6.0f, -2.4f, 0.7f, 0.625f};
    pc_algo_image orig;
    pc_surface sel;
    int c;
    orig.rect = r, orig.ch = 4, orig.data = od, orig.len = 4;
    memcpy(data, filtered, sizeof data);
    selection(&sel, 0.5f);
    pc_algo_mix_selection(data, r, &sel, &orig, true);
    for (c = 0; c < 4; c++) {
        CHECK(fabsf(data[c] - want[c]) < 1e-6f);
    }
    pc_surface_fini(&sel);
    selection(&sel, 0.0f);
    memcpy(data, filtered, sizeof data);
    pc_algo_mix_selection(data, r, &sel, &orig, true);
    CHECK(memcmp(data, od, sizeof data) == 0);
    pc_surface_fini(&sel);
    selection(&sel, 1.0f);
    memcpy(data, filtered, sizeof data);
    pc_algo_mix_selection(data, r, &sel, &orig, true);
    CHECK(memcmp(data, filtered, sizeof data) == 0);
    pc_surface_fini(&sel);
}

static void test_selection_mixing_without_alpha_keeps_every_channel_independent(void)
{
    static const size_t chs[3] = {1, 3, 4};
    pc_rect r = pc_rect_make(0, 0, 1, 1);
    pc_surface sel;
    int k;
    selection(&sel, 0.25f);
    for (k = 0; k < 3; k++) {
        float od[4] = {0.25f, 0.25f, 0.25f, 0.25f}, data[4] = {0.75f, 0.75f, 0.75f, 0.75f};
        pc_algo_image orig;
        size_t c;
        orig.rect = r, orig.ch = chs[k], orig.data = od, orig.len = chs[k];
        pc_algo_mix_selection(data, r, &sel, &orig, false);
        for (c = 0; c < chs[k]; c++) {
            CHECK(data[c] == 0.375f);
        }
    }
    pc_surface_fini(&sel);
}

int main(int argc, char **argv)
{
    RUN(test_feathered_color_to_alpha_preserves_appearance_over_the_key);
    RUN(test_feathered_offset_ignores_hidden_colour_of_the_original_pixel);
    RUN(test_feathered_blurs_do_not_reintroduce_hidden_colour);
    RUN(test_selection_mixing_keeps_hdr_range_and_exact_endpoints);
    RUN(test_selection_mixing_without_alpha_keeps_every_channel_independent);
    return pc_t_finish();
}
