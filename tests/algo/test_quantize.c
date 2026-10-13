/* quantize.rs: palettes, forced colours, dithering, bitmap methods. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/quantize.h"
#include "algo/quantize_nearest.h"
#include "base/num.h"
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
    {
        pc_algo_halftone_shape hs = PC_ALGO_HALFTONE_LINE;
        CHECK(pc_algo_halftone_shape_from_id("diamond", &hs) && hs == PC_ALGO_HALFTONE_DIAMOND);
        CHECK(pc_algo_halftone_shape_from_id("round", &hs) && hs == PC_ALGO_HALFTONE_ROUND);
        CHECK(!pc_algo_halftone_shape_from_id("x", &hs) && !pc_algo_halftone_shape_from_id(NULL, &hs));
    }
    free(nanpx);
}

static size_t linear_nearest(const float (*pts)[3], size_t n, const float q[3])
{
    float best = 3.40282347e+38f;
    size_t bi = 0, i;
    for (i = 0; i < n; i++) {
        float d0 = pts[i][0] - q[0], d1 = pts[i][1] - q[1], d2 = pts[i][2] - q[2];
        float d = d0 * d0 + d1 * d1 + d2 * d2;
        if (d < best) {
            best = d, bi = i;
        }
    }
    return bi;
}

static uint32_t lcg_seed;

static float lcg01(void)
{
    lcg_seed = lcg_seed * 1664525u + 1013904223u;
    return (float)(lcg_seed >> 8) / 16777215.0f;
}

/* nearest.rs: search_matches_linear_oracle_in_rgb_and_lab_with_ties_and_nonfinite_input. */
static void test_nearest_matches_linear_oracle(void)
{
    static const size_t counts[10] = {0, 2, 8, 16, 17, 32, 64, 65, 128, 256};
    static const float offs[2][2] = {{0.0f, 255.0f}, {-128.0f, 256.0f}};
    static float pts[256][3];
    pc_algo_nearest lk;
    size_t c, o, k, mism = 0;
    int a;
    lcg_seed = 123456789u;
    for (c = 0; c < 10; c++) {
        for (o = 0; o < 2; o++) {
            const float sp[4][3] = {{NAN, NAN, NAN},
                                    {INFINITY, INFINITY, INFINITY},
                                    {3.40282347e+38f, 3.40282347e+38f, 3.40282347e+38f},
                                    {0, 0, 0}};
            for (k = 0; k < counts[c]; k++) {
                for (a = 0; a < 3; a++) {
                    pts[k][a] = lcg01() * offs[o][1] + offs[o][0];
                }
            }
            CHECK(pc_algo_nearest_init(&lk, (const float (*)[3])pts, counts[c]) == PC_OK);
            for (k = 0; k < 3000; k++) {
                float q[3];
                for (a = 0; a < 3; a++) {
                    q[a] = lcg01() * offs[o][1] + offs[o][0];
                }
                mism += pc_algo_nearest_find(&lk, q) != linear_nearest((const float (*)[3])pts, counts[c], q);
            }
            for (k = 0; k < 4; k++) {
                CHECK_EQ_UINT(pc_algo_nearest_find(&lk, sp[k]),
                              linear_nearest((const float (*)[3])pts, counts[c], sp[k]));
            }
            pc_algo_nearest_fini(&lk);
        }
    }
    CHECK_EQ_UINT(mism, 0);
    for (k = 0; k < 256; k++) {
        pts[k][0] = pts[k][1] = pts[k][2] = 1.0f;
    }
    pts[250][0] = pts[250][1] = pts[250][2] = -1.0f;
    {
        const float zero[3] = {0, 0, 0};
        CHECK(pc_algo_nearest_init(&lk, (const float (*)[3])pts, 256) == PC_OK);
        CHECK_EQ_UINT(pc_algo_nearest_find(&lk, zero), 0); /* equidistant: original order */
        pc_algo_nearest_fini(&lk);
        pts[0][0] = pts[0][1] = pts[0][2] = NAN;
        CHECK(pc_algo_nearest_init(&lk, (const float (*)[3])pts, 256) == PC_OK);
        CHECK_EQ_UINT(pc_algo_nearest_find(&lk, zero), 1);
        pc_algo_nearest_fini(&lk);
    }
}

/* quantize.rs: all_dithers_match_the_original_linear_palette_search_exactly. The C API has no
 * quantize_with hook, so the Dither::None indices are checked against the linear scan directly
 * and every dither must map pixels onto the entries it reports. */
static void test_all_dithers_match_the_linear_palette_search(void)
{
    static const size_t counts[6] = {8, 16, 32, 64, 128, 256};
    static const pc_algo_dither dithers[4] = {PC_ALGO_DITHER_NONE, PC_ALGO_DITHER_DIFFUSION,
                                              PC_ALGO_DITHER_PATTERN, PC_ALGO_DITHER_NOISE};
    static const float alphas[4] = {0.0f, 0.2f, 0.7f, 1.0f};
    const size_t n = 93 * 61;
    float (*src)[4] = gradient(93, 61), (*px)[4] = malloc(n * sizeof *px), (*pf)[3] = NULL;
    uint8_t *idx = malloc(n);
    size_t i, c, d, t, bad = 0;
    for (i = 0; i < n; i++) {
        src[i][2] = (float)(i % 71) / 70.0f;
        src[i][3] = alphas[i % 4];
    }
    pf = malloc(256 * sizeof *pf);
    for (c = 0; c < 6; c++) {
        pc_algo_palette pal;
        CHECK(pc_algo_quantize_build_palette((const float (*)[4])src, n, PC_ALGO_PALETTE_SELECTIVE, counts[c],
                                             PC_ALGO_FORCED_BLACK_WHITE, &pal, NULL) == PC_OK);
        for (i = 0; i < pal.n; i++) {
            pf[i][0] = (float)pal.c[i][0], pf[i][1] = (float)pal.c[i][1], pf[i][2] = (float)pal.c[i][2];
        }
        for (d = 0; d < 4; d++) {
            for (t = 0; t < 2; t++) {
                memcpy(px, src, n * sizeof *px);
                CHECK(pc_algo_quantize(px, n, 93, &pal.c[0][0], pal.n, dithers[d], 0.75f, t == 1, 1, idx,
                                       NULL) == PC_OK);
                for (i = 0; i < n; i++) {
                    float q[3];
                    int a;
                    if (src[i][3] < 0.5f && (t == 1 || src[i][3] <= 0.0f)) {
                        continue;
                    }
                    for (a = 0; a < 3; a++) {
                        bad += px[i][a] != (float)pal.c[idx[i]][a] / 255.0f;
                        q[a] = pc_clampf(src[i][a], 0.0f, 1.0f) * 255.0f;
                    }
                    if (dithers[d] == PC_ALGO_DITHER_NONE) {
                        bad += idx[i] != linear_nearest((const float (*)[3])pf, pal.n, q);
                    }
                }
            }
        }
    }
    CHECK_EQ_UINT(bad, 0);
    free(src), free(px), free(pf), free(idx);
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
    RUN(test_nearest_matches_linear_oracle);
    RUN(test_all_dithers_match_the_linear_palette_search);
    return pc_t_finish();
}
