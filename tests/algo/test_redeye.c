/* Red-eye correction (src/algo/redeye.c): the crates/algo/src/redeye.rs tests plus never-crash
 * cases. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "harness.h"

static bool disc(size_t x, size_t y, float cx, float cy, float r)
{
    return hypotf((float)x - cx, (float)y - cy) <= r;
}

static void portrait_pixel(size_t x, size_t y, float out[4])
{
    /* Brown that fails R > G + 0.15, so the iris is never in the red-eye mask. */
    static const float iris[4] = {0.38f, 0.28f, 0.16f, 1}, red[4] = {0.95f, 0.10f, 0.10f, 1},
                       white[4] = {1, 1, 1, 1};
    memcpy(out, disc(x, y, 16, 16, 1.2f) ? white : (disc(x, y, 16, 16, 10) ? red : iris), 4 * sizeof(float));
}

static float luma(const float p[4])
{
    return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
}

static void test_halo_covers_erode_and_blur_reach(void)
{
    uint32_t s = 0;
    CHECK_EQ_INT(pc_algo_redeye_halo_radius(), 1 + (int)ceilf(PC_ALGO_REDEYE_SIGMA * 3.0f) + 1);
    CHECK_EQ_INT(pc_algo_redeye_halo_radius(), 5);
    CHECK(pc_algo_redeye_search_radius(50.0f) == 64.0f);
    CHECK(pc_algo_redeye_search_radius(0.0f) == 8.0f);
    CHECK(pc_algo_redeye_search_radius(101.0f) == pc_algo_redeye_search_radius(100.0f));
    CHECK(pc_algo_redeye_unclamped_window_side(50.0f, &s) && s < PC_ALGO_REDEYE_MAX_WINDOW);
    CHECK(!pc_algo_redeye_unclamped_window_side(10000.0f, &s));
    CHECK(!pc_algo_redeye_unclamped_window_side(NAN, &s));
}

static void test_red_disc_is_neutralized_and_iris_is_left_alone(void)
{
    size_t w = 32, x, y;
    float (*px)[4] = malloc(w * w * sizeof *px), iris_before[4], *after, *iris_after, *c;
    float neut = (0.10f + 0.10f) / 2.0f;
    const float red[4] = {0.95f, 0.10f, 0.10f, 1.0f};
    uint32_t n = 0;
    int i;
    for (y = 0; y < w; y++) {
        for (x = 0; x < w; x++) {
            portrait_pixel(x, y, px[y * w + x]);
        }
    }
    memcpy(iris_before, px[2 * w + 2], sizeof iris_before);
    CHECK(pc_algo_redeye_apply(px, w * w, w, w, 16, 16, 0, 0, 50.0f, 100.0f, &n, NULL) == PC_OK);
    CHECK(n > 0);
    /* 5 px from the centre: in the red ring, outside the catchlight, inside the eroded mask. */
    after = px[11 * w + 16];
    CHECK(fabsf(after[0] - after[1]) < 0.08f && fabsf(after[0] - after[2]) < 0.08f);
    CHECK(fabsf(after[0] - neut * 0.55f) < 0.08f);
    CHECK(luma(after) < luma(red) - 0.05f);
    iris_after = px[2 * w + 2];
    for (i = 0; i < 4; i++) {
        CHECK(fabsf(iris_after[i] - iris_before[i]) < 1e-5f);
    }
    c = px[16 * w + 16];
    CHECK(c[0] >= 0.9f && c[1] >= 0.9f && c[2] >= 0.9f);
    free(px);
}

static void test_gray_pixels_are_not_treated_as_red_eye(void)
{
    float px[16 * 16][4];
    uint32_t n = 9;
    size_t i;
    for (i = 0; i < 256; i++) {
        px[i][0] = px[i][1] = px[i][2] = 0.4f, px[i][3] = 1.0f;
    }
    CHECK(pc_algo_redeye_apply(px, 256, 16, 16, 8, 8, 0, 0, 50.0f, 50.0f, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 0);
    CHECK(px[8 * 16 + 8][0] == 0.4f && px[8 * 16 + 8][3] == 1.0f);
}

/* Applies the correction tile by tile with `halo` pixels of context, as the engine does. */
static void apply_tiled(const float (*src)[4], size_t w, size_t h, int32_t cx, int32_t cy, float pupil,
                        float darken, size_t tile, size_t halo, float (*out)[4])
{
    size_t x0, y0;
    memcpy(out, src, w * h * sizeof *out);
    for (y0 = 0; y0 < h; y0 += tile) {
        for (x0 = 0; x0 < w; x0 += tile) {
            size_t x1 = x0 + tile < w ? x0 + tile : w, y1 = y0 + tile < h ? y0 + tile : h;
            size_t rx0 = x0 > halo ? x0 - halo : 0, ry0 = y0 > halo ? y0 - halo : 0;
            size_t rx1 = x1 + halo < w ? x1 + halo : w, ry1 = y1 + halo < h ? y1 + halo : h;
            size_t bw = rx1 - rx0, bh = ry1 - ry0, xx, yy;
            float (*b)[4] = malloc(bw * bh * sizeof *b);
            uint32_t n;
            for (yy = 0; yy < bh; yy++) {
                for (xx = 0; xx < bw; xx++) {
                    memcpy(b[yy * bw + xx], src[(ry0 + yy) * w + rx0 + xx], sizeof *b);
                }
            }
            CHECK(pc_algo_redeye_apply(b, bw * bh, bw, bh, cx, cy, (int32_t)rx0, (int32_t)ry0, pupil, darken,
                                       &n, NULL) == PC_OK);
            for (yy = y0; yy < y1; yy++) {
                for (xx = x0; xx < x1; xx++) {
                    memcpy(out[yy * w + xx], b[(yy - ry0) * bw + (xx - rx0)], sizeof *b);
                }
            }
            free(b);
        }
    }
}

static float max_neighbour_jump(const float (*px)[4], size_t w, size_t h, size_t x)
{
    float m = 0.0f;
    size_t y;
    for (y = 0; y < h; y++) {
        const float *a = px[y * w + x - 1], *b = px[y * w + x];
        float d = fmaxf(fmaxf(fabsf(a[0] - b[0]), fabsf(a[1] - b[1])), fabsf(a[2] - b[2]));
        m = d > m ? d : m;
    }
    return m;
}

static float max_err(const float (*a)[4], const float (*b)[4], size_t n)
{
    float e = 0.0f;
    size_t i;
    int c;
    for (i = 0; i < n; i++) {
        for (c = 0; c < 3; c++) {
            e = fmaxf(e, fabsf(a[i][c] - b[i][c]));
        }
    }
    return e;
}

static void test_tile_seam_matches_an_untiled_reference(void)
{
    size_t w = 128, i;
    float (*src)[4] = malloc(w * w * sizeof *src), (*ref)[4] = malloc(w * w * sizeof *ref);
    float (*tiled)[4] = malloc(w * w * sizeof *tiled), (*shrt)[4] = malloc(w * w * sizeof *shrt), err, serr;
    uint32_t n;
    /* A red half-plane ending on the 64 px tile boundary: the mask edge is where a short halo fails. */
    for (i = 0; i < w * w; i++) {
        const float a[4] = {0.95f, 0.10f, 0.10f, 1}, b[4] = {0.38f, 0.28f, 0.16f, 1};
        memcpy(src[i], i % w < 64 ? a : b, sizeof a);
    }
    memcpy(ref, src, w * w * sizeof *ref);
    CHECK(pc_algo_redeye_apply(ref, w * w, w, w, 60, 64, 0, 0, 50.0f, 50.0f, &n, NULL) == PC_OK);
    apply_tiled((const float (*)[4])src, w, w, 60, 64, 50.0f, 50.0f, 64, (size_t)pc_algo_redeye_halo_radius(),
                tiled);
    err = max_err((const float (*)[4])ref, (const float (*)[4])tiled, w * w);
    CHECK(err < 1e-4f);
    CHECK(fabsf(max_neighbour_jump((const float (*)[4])ref, w, w, 64) -
                max_neighbour_jump((const float (*)[4])tiled, w, w, 64)) < 1e-4f);
    /* Reach is erode (1) + ceil(3 sigma) (3): a 1 px halo cannot cover it. */
    apply_tiled((const float (*)[4])src, w, w, 60, 64, 50.0f, 50.0f, 64, 1, shrt);
    serr = max_err((const float (*)[4])ref, (const float (*)[4])shrt, w * w);
    CHECK(serr > err + 1e-4f);
    free(src), free(ref), free(tiled), free(shrt);
}

static void test_empty_or_mismatched_buffer_changes_nothing(void)
{
    float tiny[4][4];
    uint32_t n = 7;
    size_t i;
    CHECK(pc_algo_redeye_apply(NULL, 0, 0, 0, 0, 0, 0, 0, 50, 50, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 0);
    for (i = 0; i < 4; i++) {
        tiny[i][0] = 0.95f, tiny[i][1] = 0.1f, tiny[i][2] = 0.1f, tiny[i][3] = 1.0f;
    }
    n = 7;
    CHECK(pc_algo_redeye_apply(tiny, 4, 8, 8, 1, 1, 0, 0, 50, 50, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 0);
}

static void test_never_crash(void)
{
    float px[10 * 10][4];
    uint32_t n = 0, s = 0;
    size_t i;
    for (i = 0; i < 100; i++) {
        px[i][0] = i % 7 == 0 ? NAN : 0.9f, px[i][1] = i % 11 == 0 ? INFINITY : 0.1f, px[i][2] = 0.1f;
        px[i][3] = 1.0f;
    }
    /* Extreme clicks / origins (saturating offsets), NaN and huge parameters, overflowing sizes. */
    CHECK(pc_algo_redeye_apply(px, 100, 10, 10, INT32_MAX, INT32_MIN, INT32_MIN, INT32_MAX, NAN, INFINITY, &n,
                               NULL) == PC_OK);
    CHECK(pc_algo_redeye_apply(px, 100, 10, 10, 5, 5, 0, 0, -1e30f, -1e30f, &n, NULL) == PC_OK);
    CHECK(pc_algo_redeye_apply(px, 100, 10, 10, 5, 5, 0, 0, 1e30f, 1e30f, &n, NULL) == PC_OK);
    CHECK(pc_algo_redeye_apply(px, 100, SIZE_MAX, 2, 5, 5, 0, 0, 50, 50, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 0);
    CHECK(pc_algo_redeye_apply(px, 100, 100, 1, 5, 0, 0, 0, 50, 50, &n, NULL) == PC_OK);
    CHECK(!pc_algo_redeye_unclamped_window_side(INFINITY, &s));
    CHECK(!pc_algo_redeye_unclamped_window_side(-1e30f, &s));
    CHECK(pc_algo_redeye_search_radius(-INFINITY) == 64.0f); /* non-finite: pupil 50 */
}

int main(int argc, char **argv)
{
    RUN(test_halo_covers_erode_and_blur_reach);
    RUN(test_red_disc_is_neutralized_and_iris_is_left_alone);
    RUN(test_gray_pixels_are_not_treated_as_red_eye);
    RUN(test_tile_seam_matches_an_untiled_reference);
    RUN(test_empty_or_mismatched_buffer_changes_nothing);
    RUN(test_never_crash);
    return pc_t_finish();
}
