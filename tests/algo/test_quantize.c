/* quantize.rs: palettes, forced colours, dithering, bitmap methods. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/quantize.h"
#include "harness.h"

static float (*gradient(size_t w, size_t h))[4]
{
    float (*px)[4] = malloc(w * h * sizeof *px);
    size_t i;
    for (i = 0; i < w * h; i++) {
        px[i][0] = (float)(i % w) / (float)(w - 1);
        px[i][1] = (float)(i / w) / (float)(h - 1);
        px[i][2] = 0.3f;
        px[i][3] = 1.0f;
    }
    return px;
}

static int cmp3(const void *a, const void *b)
{
    return memcmp(a, b, 3);
}

static void test_fixed_palettes_have_expected_sizes(void)
{
    pc_algo_palette p;
    uint8_t *u = NULL;
    size_t n = 0, i, uniq = 1;
    pc_algo_quantize_web_palette(&p);
    CHECK_EQ_UINT(p.n, 216);
    pc_algo_quantize_mac_palette(&p);
    CHECK_EQ_UINT(p.n, 256);
    qsort(&p.c[0][0], p.n, 3, cmp3);
    for (i = 1; i < p.n; i++) {
        uniq += memcmp(p.c[i - 1], p.c[i], 3) != 0 ? 1u : 0u;
    }
    CHECK_EQ_UINT(uniq, 256); /* mac palette entries are unique */
    pc_algo_quantize_windows_palette(&p);
    CHECK_EQ_UINT(p.n, 256);
    CHECK(pc_algo_quantize_uniform_palette(27, &u, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 27);
    free(u);
}

static void test_exact_palette_and_failure(void)
{
    const float px[3][4] = {{1, 0, 0, 1}, {0, 0, 1, 1}, {1, 0, 0, 1}};
    float (*g)[4] = gradient(64, 64);
    pc_algo_palette p;
    pc_error e;
    CHECK(pc_algo_quantize_build_palette(px, 3, PC_ALGO_PALETTE_EXACT, 256, PC_ALGO_FORCED_NONE, &p, NULL) ==
          PC_OK);
    CHECK_EQ_UINT(p.n, 2);
    CHECK(pc_algo_quantize_build_palette((const float (*)[4])g, 64 * 64, PC_ALGO_PALETTE_EXACT, 256,
                                         PC_ALGO_FORCED_NONE, &p, &e) == PC_ERR_INVALID);
    CHECK_EQ_STR(e.msg, "the image has 4096 colours; Exact needs 256 or fewer");
    free(g);
}

static void test_adaptive_and_perceptual_respect_count_and_forced(void)
{
    float (*px)[4] = gradient(40, 40);
    const pc_algo_palette_kind kinds[3] = {PC_ALGO_PALETTE_ADAPTIVE, PC_ALGO_PALETTE_PERCEPTUAL,
                                           PC_ALGO_PALETTE_SELECTIVE};
    int k;
    for (k = 0; k < 3; k++) {
        pc_algo_palette p;
        CHECK(pc_algo_quantize_build_palette((const float (*)[4])px, 1600, kinds[k], 16,
                                             PC_ALGO_FORCED_BLACK_WHITE, &p, NULL) == PC_OK);
        CHECK(p.n <= 16 && p.n >= 8);
        CHECK(memcmp(p.c[0], "\0\0\0\xff\xff\xff", 6) == 0);
    }
    free(px);
}

static void test_forced_colours_filling_the_count_do_not_panic(void)
{
    float px[16][4];
    float (*g)[4] = gradient(8, 8);
    const pc_algo_palette_kind kinds[3] = {PC_ALGO_PALETTE_ADAPTIVE, PC_ALGO_PALETTE_PERCEPTUAL,
                                           PC_ALGO_PALETTE_SELECTIVE};
    int k, i;
    for (i = 0; i < 16; i++) {
        px[i][0] = px[i][1] = px[i][2] = px[i][3] = 1.0f;
    }
    for (k = 0; k < 3; k++) {
        pc_algo_palette p;
        CHECK(pc_algo_quantize_build_palette((const float (*)[4])px, 16, kinds[k], 2,
                                             PC_ALGO_FORCED_BLACK_WHITE, &p, NULL) == PC_OK);
        CHECK(p.n == 2 && memcmp(p.c[0], "\0\0\0\xff\xff\xff", 6) == 0);
        CHECK(pc_algo_quantize_build_palette((const float (*)[4])g, 64, kinds[k], 8, PC_ALGO_FORCED_PRIMARIES,
                                             &p, NULL) == PC_OK);
        CHECK_EQ_UINT(p.n, 8);
    }
    free(g);
}

static void test_quantize_maps_to_palette_with_each_dither(void)
{
    const uint8_t pal[6] = {0, 0, 0, 255, 255, 255};
    const pc_algo_dither ds[4] = {PC_ALGO_DITHER_NONE, PC_ALGO_DITHER_DIFFUSION, PC_ALGO_DITHER_PATTERN,
                                  PC_ALGO_DITHER_NOISE};
    float px[64][4], one[1][4] = {{1, 1, 1, 0.2f}};
    uint8_t idx[64];
    int d, i;
    for (d = 0; d < 4; d++) {
        bool binary = true;
        for (i = 0; i < 64; i++) {
            px[i][0] = px[i][1] = px[i][2] = (float)i / 63.0f;
            px[i][3] = 1.0f;
        }
        CHECK(pc_algo_quantize(px, 64, 8, pal, 2, ds[d], 1.0f, false, 0, idx, NULL) == PC_OK);
        for (i = 0; i < 64; i++) {
            binary = binary && (px[i][0] == 0.0f || px[i][0] == 1.0f);
        }
        CHECK(binary);
        /* Mean brightness is preserved by dithering (not by plain thresholding). */
        if (ds[d] == PC_ALGO_DITHER_DIFFUSION) {
            float mean = -0.0f;
            for (i = 0; i < 64; i++) {
                mean += px[i][0];
            }
            CHECK(fabsf(mean / 64.0f - 0.5f) < 0.08f);
        }
    }
    CHECK(pc_algo_quantize(one, 1, 1, pal, 2, PC_ALGO_DITHER_NONE, 1.0f, true, 1, idx, NULL) == PC_OK);
    CHECK(idx[0] == 1 && one[0][3] == 0.0f);
}

static void test_bitmap_methods_are_binary_and_track_tone(void)
{
    pc_algo_bitmap_method ms[4] = {{PC_ALGO_BITMAP_THRESHOLD, 0, 0, PC_ALGO_HALFTONE_ROUND},
                                   {PC_ALGO_BITMAP_PATTERN, 0, 0, PC_ALGO_HALFTONE_ROUND},
                                   {PC_ALGO_BITMAP_DIFFUSION, 0, 0, PC_ALGO_HALFTONE_ROUND},
                                   {PC_ALGO_BITMAP_HALFTONE, 6.0f, 45.0f, PC_ALGO_HALFTONE_ROUND}};
    static float g[32 * 32];
    int m, i;
    for (m = 0; m < 4; m++) {
        float mean = -0.0f;
        bool binary = true;
        for (i = 0; i < 32 * 32; i++) {
            g[i] = 0.25f;
        }
        CHECK(pc_algo_quantize_to_bitmap(g, 32 * 32, 32, ms[m], NULL) == PC_OK);
        for (i = 0; i < 32 * 32; i++) {
            binary = binary && (g[i] == 0.0f || g[i] == 1.0f);
            mean += g[i];
        }
        CHECK(binary);
        mean /= 1024.0f;
        if (m != 0) {
            CHECK(fabsf(mean - 0.25f) < 0.15f);
        } else {
            CHECK(mean == 0.0f);
        }
    }
}

static void test_never_crash(void)
{
    /* All-identical colours, empty images, hostile values and ids. */
    float same[100][4], (*nanpx)[4] = malloc(50 * sizeof *nanpx);
    pc_algo_palette p;
    pc_algo_palette_kind k;
    uint8_t idx[100], *u = NULL;
    size_t n, i, kind;
    pc_algo_bitmap_method hm = {PC_ALGO_BITMAP_HALFTONE, NAN, INFINITY, PC_ALGO_HALFTONE_CROSS};
    float g[10] = {0, 0.1f, NAN, 1, 2, -1, INFINITY, 0.5f, 0.5f, 0.7f};
    for (i = 0; i < 100; i++) {
        same[i][0] = 0.25f, same[i][1] = 0.5f, same[i][2] = 0.75f, same[i][3] = 1.0f;
    }
    for (i = 0; i < 50; i++) {
        nanpx[i][0] = NAN, nanpx[i][1] = INFINITY, nanpx[i][2] = -INFINITY,
        nanpx[i][3] = (i % 2) ? 1.0f : NAN;
    }
    for (kind = 0; kind <= PC_ALGO_PALETTE_ADAPTIVE; kind++) {
        for (n = 0; n < 300; n += 37) {
            CHECK(pc_algo_quantize_build_palette((const float (*)[4])same, 100, (pc_algo_palette_kind)kind, n,
                                                 PC_ALGO_FORCED_WEB, &p, NULL) == PC_OK);
            CHECK(p.n >= 1 && p.n <= 256);
            CHECK(pc_algo_quantize_build_palette(NULL, 0, (pc_algo_palette_kind)kind, n, PC_ALGO_FORCED_NONE,
                                                 &p, NULL) == PC_OK);
            CHECK(p.n >= 1);
            CHECK(pc_algo_quantize_build_palette((const float (*)[4])nanpx, 50, (pc_algo_palette_kind)kind, n,
                                                 PC_ALGO_FORCED_NONE, &p, NULL) == PC_OK);
        }
    }
    CHECK(pc_algo_quantize_build_palette((const float (*)[4])same, 100, PC_ALGO_PALETTE_ADAPTIVE, 16,
                                         PC_ALGO_FORCED_NONE, &p, NULL) == PC_OK);
    CHECK(p.n == 1 && p.c[0][0] == 64 && p.c[0][1] == 128 && p.c[0][2] == 191);
    CHECK(pc_algo_quantize(nanpx, 50, 7, &p.c[0][0], p.n, PC_ALGO_DITHER_DIFFUSION, NAN, false, 0, idx,
                           NULL) == PC_OK);
    CHECK(pc_algo_quantize(nanpx, 50, 0, &p.c[0][0], p.n, PC_ALGO_DITHER_PATTERN, 1.0f, true, 300, idx,
                           NULL) == PC_OK);
    CHECK(pc_algo_quantize(same, 100, 3, &p.c[0][0], 0, PC_ALGO_DITHER_NONE, 1.0f, false, 0, idx, NULL) ==
          PC_ERR_INVALID);
    CHECK(pc_algo_quantize_to_bitmap(g, 10, 3, hm, NULL) == PC_OK);
    CHECK(pc_algo_quantize_to_bitmap(g, 10, 0, hm, NULL) == PC_ERR_INVALID);
    hm.kind = PC_ALGO_BITMAP_DIFFUSION;
    CHECK(pc_algo_quantize_to_bitmap(g, 10, 0, hm, NULL) == PC_OK);
    CHECK(pc_algo_quantize_to_bitmap(g, 10, 4, hm, NULL) == PC_OK);
    CHECK(pc_algo_quantize_uniform_palette(SIZE_MAX, &u, &n, NULL) == PC_ERR_LIMIT);
    CHECK(pc_algo_quantize_uniform_palette(0, &u, &n, NULL) == PC_OK && n == 8);
    free(u);
    CHECK(!pc_algo_palette_kind_from_id("bogus", &k) && !pc_algo_palette_kind_from_id(NULL, &k));
    CHECK(pc_algo_palette_kind_from_id("localSelective", &k) && k == PC_ALGO_PALETTE_SELECTIVE);
    CHECK(pc_algo_halftone_shape_from_id("diamond") == PC_ALGO_HALFTONE_DIAMOND);
    CHECK(pc_algo_halftone_shape_from_id("x") == PC_ALGO_HALFTONE_ROUND);
    free(nanpx);
}

int main(int argc, char **argv)
{
    RUN(test_fixed_palettes_have_expected_sizes);
    RUN(test_exact_palette_and_failure);
    RUN(test_adaptive_and_perceptual_respect_count_and_forced);
    RUN(test_forced_colours_filling_the_count_do_not_panic);
    RUN(test_quantize_maps_to_palette_with_each_dither);
    RUN(test_bitmap_methods_are_binary_and_track_tone);
    RUN(test_never_crash);
    return pc_t_finish();
}
