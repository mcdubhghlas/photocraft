/* Magic Eraser (src/algo/erase.c): the crates/algo/src/erase.rs tests plus never-crash cases. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "harness.h"
#include "raster/pixel.h"

static pc_algo_region region(pc_rect bbox, uint8_t v)
{
    pc_algo_region r;
    r.bbox = bbox;
    r.len = (size_t)pc_rect_width(bbox) * pc_rect_height(bbox);
    r.mask = malloc(r.len + 1);
    memset(r.mask, v, r.len);
    return r;
}

static void fill_rgba(pc_surface *s, pc_rect r, const float rgba[4])
{
    float px[PC_RASTER_MAX_CHANNELS];
    size_t n = pc_from_rgba(pc_surface_format(s), rgba, px);
    CHECK(pc_surface_fill_rect(s, r, px, n, NULL) == PC_OK);
}

static void test_erases_alpha_in_every_format(void)
{
    static const pc_color_mode MODES[4] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_CMYK, PC_COLOR_MODE_GRAYSCALE,
                                           PC_COLOR_MODE_LAB};
    static const pc_sample_type SAMPLES[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    const float col[4] = {0.2f, 0.5f, 0.7f, 1.0f};
    size_t mi, si;
    for (mi = 0; mi < 4; mi++) {
        for (si = 0; si < 3; si++) {
            pc_pixel_format fmt = {MODES[mi], SAMPLES[si], true};
            pc_surface s, h;
            float before[PC_RASTER_MAX_CHANNELS], after[PC_RASTER_MAX_CHANNELS], rgba[4],
                p[PC_RASTER_MAX_CHANNELS];
            size_t a = pc_color_mode_channels(fmt.mode), n = pc_pixel_format_channels(fmt);
            pc_algo_region rg = region(pc_rect_make(0, 0, 300, 300), 255),
                           rg2 = region(pc_rect_make(0, 0, 10, 10), 255);
            pc_rect d = PC_RECT_EMPTY;
            pc_surface_init(&s, fmt);
            fill_rgba(&s, pc_rect_make(0, 0, 600, 300), col);
            pc_surface_read_pixel(&s, 10, 10, before);
            CHECK(pc_algo_erase_magic(&s, &rg, 1.0f, NULL, NULL, 0, &d, NULL) == PC_OK);
            CHECK(pc_rect_eq(d, pc_rect_make(0, 0, 300, 300)));
            pc_surface_rgba(&s, 10, 10, rgba);
            CHECK(rgba[3] == 0.0f);
            pc_surface_read_pixel(&s, 400, 10, after);
            CHECK(memcmp(after, before, n * sizeof(float)) == 0); /* outside the region untouched */
            /* Half opacity halves alpha and keeps colour. */
            pc_surface_init(&h, fmt);
            fill_rgba(&h, pc_rect_make(0, 0, 10, 10), col);
            CHECK(pc_algo_erase_magic(&h, &rg2, 0.5f, NULL, NULL, 0, &d, NULL) == PC_OK);
            pc_surface_read_pixel(&h, 5, 5, p);
            CHECK(fabsf(p[a] - 0.5f) < 0.01f);
            CHECK(fabsf(p[0] - before[0]) < 1e-6f);
            pc_surface_fini(&s);
            pc_surface_fini(&h);
            free(rg.mask), free(rg2.mask);
        }
    }
}

static void test_respects_selection_and_lock(void)
{
    const float red[4] = {1, 0, 0, 1}, one = 1.0f, white[3] = {1, 1, 1};
    pc_surface s, sel, l;
    pc_algo_region rg = region(pc_rect_make(0, 0, 20, 20), 255);
    pc_rect d;
    float rgba[4];
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(0, 0, 20, 20), red, 4, NULL) == PC_OK);
    pc_surface_init(&sel, PC_PIXEL_FORMAT_GRAY8);
    CHECK(pc_surface_fill_rect(&sel, pc_rect_make(0, 0, 10, 20), &one, 1, NULL) == PC_OK);
    CHECK(pc_algo_erase_magic(&s, &rg, 1.0f, &sel, NULL, 0, &d, NULL) == PC_OK);
    pc_surface_rgba(&s, 5, 5, rgba);
    CHECK(rgba[3] == 0.0f);
    pc_surface_rgba(&s, 15, 5, rgba);
    CHECK(rgba[3] == 1.0f);
    /* Locked transparency paints the lock colour, alpha kept. */
    pc_surface_init(&l, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_surface_fill_rect(&l, pc_rect_make(0, 0, 20, 20), red, 4, NULL) == PC_OK);
    CHECK(pc_algo_erase_magic(&l, &rg, 1.0f, NULL, white, 3, &d, NULL) == PC_OK);
    pc_surface_rgba(&l, 5, 5, rgba);
    CHECK(rgba[0] == 1.0f && rgba[1] == 1.0f && rgba[2] == 1.0f && rgba[3] == 1.0f);
    pc_surface_fini(&s), pc_surface_fini(&sel), pc_surface_fini(&l);
    free(rg.mask);
}

static void test_degenerate_inputs_do_nothing(void)
{
    const float red[4] = {1, 0, 0, 1};
    pc_surface s, orig, g;
    pc_algo_region bad = region(pc_rect_make(0, 0, 4, 4), 255), ok = region(pc_rect_make(0, 0, 4, 4), 255);
    pc_algo_region empty = {PC_RECT_EMPTY, NULL, 0};
    pc_rect d = pc_rect_make(1, 2, 3, 4);
    bad.len = 3;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(0, 0, 4, 4), red, 4, NULL) == PC_OK);
    CHECK(pc_surface_clone(&s, &orig, NULL) == PC_OK);
    /* Mask length mismatch, zero / NaN amount, empty box. */
    CHECK(pc_algo_erase_magic(&s, &bad, 1.0f, NULL, NULL, 0, &d, NULL) == PC_OK && pc_rect_is_empty(d));
    CHECK(pc_algo_erase_magic(&s, &ok, NAN, NULL, NULL, 0, &d, NULL) == PC_OK && pc_rect_is_empty(d));
    CHECK(pc_algo_erase_magic(&s, &ok, 0.0f, NULL, NULL, 0, &d, NULL) == PC_OK && pc_rect_is_empty(d));
    CHECK(pc_algo_erase_magic(&s, &empty, 1.0f, NULL, NULL, 0, &d, NULL) == PC_OK && pc_rect_is_empty(d));
    CHECK(pc_surface_eq(&s, &orig));
    /* No alpha channel and no lock colour: nothing to erase. */
    pc_surface_init(&g, PC_PIXEL_FORMAT_GRAY8);
    CHECK(pc_algo_erase_magic(&g, &ok, 1.0f, NULL, NULL, 0, &d, NULL) == PC_OK && pc_rect_is_empty(d));
    pc_surface_fini(&s), pc_surface_fini(&orig), pc_surface_fini(&g);
    free(bad.mask), free(ok.mask);
}

static void test_never_crash(void)
{
    const float red[4] = {1, 0, 0, 1}, lock[1] = {NAN};
    pc_surface s;
    pc_algo_region far = region(pc_rect_make(INT32_MAX - 3, INT32_MIN, INT32_MAX, INT32_MIN + 2), 200);
    pc_algo_region rg = region(pc_rect_make(-300, -300, 300, 300), 128);
    pc_rect d;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA16);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(0, 0, 50, 50), red, 4, NULL) == PC_OK);
    /* Regions at the coordinate extremes, short / NaN lock colours, huge amounts, NULL arguments. */
    CHECK(pc_algo_erase_magic(&s, &far, 1.0f, NULL, NULL, 0, &d, NULL) == PC_OK);
    CHECK(pc_algo_erase_magic(&s, &rg, 1e30f, NULL, lock, 1, &d, NULL) == PC_OK);
    CHECK(pc_algo_erase_magic(&s, &rg, INFINITY, &s, NULL, 0, &d, NULL) == PC_OK);
    CHECK(pc_algo_erase_magic(&s, &rg, -1.0f, NULL, NULL, 0, &d, NULL) == PC_OK && pc_rect_is_empty(d));
    CHECK(pc_algo_erase_magic(NULL, &rg, 1.0f, NULL, NULL, 0, &d, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_erase_magic(&s, NULL, 1.0f, NULL, NULL, 0, &d, NULL) == PC_ERR_INVALID);
    pc_surface_fini(&s);
    free(far.mask), free(rg.mask);
}

int main(int argc, char **argv)
{
    RUN(test_erases_alpha_in_every_format);
    RUN(test_respects_selection_and_lock);
    RUN(test_degenerate_inputs_do_nothing);
    RUN(test_never_crash);
    return pc_t_finish();
}
