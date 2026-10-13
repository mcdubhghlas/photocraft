/* Non-local patch-based completion, the Remove Tool's fill (src/algo/nonlocal.c): the
 * crates/algo/src/nonlocal.rs tests plus never-crash cases. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "algo/nonlocal.h"
#include "harness.h"

static float noise(size_t i)
{
    return (float)(((uint32_t)i * 2654435761u) >> 8) / (float)(1u << 24);
}

/* Mean |dx| + |dy| over the masked cells (one channel). */
static float grain(size_t w, size_t len, const float *img, const bool *m)
{
    float s = 0.0f;
    size_t i, n = 0;
    for (i = 0; i < len; i++) {
        if (m[i] && i % w + 1 < w && i + w < len) {
            s += fabsf(img[i + 1] - img[i]) + fabsf(img[i + w] - img[i]);
            n++;
        }
    }
    return s / (float)(n > 1 ? n : 1);
}

static bool *box(size_t w, size_t h, size_t x0, size_t x1, size_t y0, size_t y1)
{
    bool *m = malloc(w * h);
    size_t i;
    for (i = 0; i < w * h; i++) {
        m[i] = i % w >= x0 && i % w < x1 && i / w >= y0 && i / w < y1;
    }
    return m;
}

/* The failure that motivates the texture features: a hole in noise next to a flat area of the same
 * mean must be filled with noise, not with the flat area. */
static void test_a_hole_in_texture_next_to_a_flat_area_of_the_same_mean_gets_texture(void)
{
    size_t w = 96, h = 64, n = w * h, i;
    float *img = malloc(n * sizeof(float)), *out = NULL;
    bool *hole = box(w, h, 60, 84, 20, 44), kept = true;
    pc_algo_nonlocal_params p = pc_algo_nonlocal_params_default();
    for (i = 0; i < n; i++) {
        img[i] = i % w < 48 ? 0.5f : 0.25f + 0.5f * noise(i);
    }
    CHECK(pc_algo_nonlocal_complete(w, h, 1, img, n, hole, n, &p, NULL, &out, NULL) == PC_OK);
    CHECK(out != NULL);
    if (out != NULL) {
        float filled = grain(w, n, out, hole), truth = grain(w, n, img, hole);
        CHECK(filled > truth * 0.7f);
        for (i = 0; i < n; i++) {
            kept = kept && (hole[i] || out[i] == img[i]);
        }
        CHECK(kept);
    }
    free(img), free(out), free(hole);
}

static void test_a_hole_in_a_gradient_follows_the_gradient(void)
{
    size_t w = 80, h = 60, ch = 2, n = w * h, i;
    float *img = malloc(n * ch * sizeof(float)), *out = NULL;
    bool *hole = box(w, h, 30, 50, 20, 40), ok = true;
    pc_algo_nonlocal_params p = pc_algo_nonlocal_params_default();
    for (i = 0; i < n; i++) {
        img[i * 2] = (float)(i % w) / (float)w, img[i * 2 + 1] = 1.0f;
    }
    CHECK(pc_algo_nonlocal_complete(w, h, ch, img, n * ch, hole, n, &p, NULL, &out, NULL) == PC_OK);
    CHECK(out != NULL);
    for (i = 0; out != NULL && i < n; i++) {
        ok = ok && (!hole[i] || fabsf(out[i * ch] - img[i * ch]) < 0.1f);
    }
    CHECK(ok);
    free(img), free(out), free(hole);
}

static void test_levels_follow_the_hole_thickness(void)
{
    size_t w = 400, h = 400, n = w * h, i, levels = 0;
    bool *line = malloc(n), *disc = malloc(n);
    for (i = 0; i < n; i++) {
        line[i] = i / w == 200 && i % w >= 20 && i % w < 380;
        disc[i] = hypotf((float)(i % w) - 200.0f, (float)(i / w) - 200.0f) < 90.0f;
    }
    /* A thin line stays at full resolution. */
    CHECK(pc_algo_nonlocal_level_count(w, h, line, 3, &levels, NULL) == PC_OK);
    CHECK_EQ_UINT(levels, 1);
    /* ceil(log2(2 * 90 / 7)). */
    CHECK(pc_algo_nonlocal_level_count(w, h, disc, 3, &levels, NULL) == PC_OK);
    CHECK_EQ_UINT(levels, 5);
    free(line), free(disc);
}

static void test_nothing_to_copy_from_and_bad_sizes(void)
{
    float img[144], *out = (float *)img;
    bool all[144];
    pc_algo_nonlocal_params p = pc_algo_nonlocal_params_default();
    size_t i;
    for (i = 0; i < 144; i++) {
        img[i] = 0.5f, all[i] = true;
    }
    CHECK(pc_algo_nonlocal_complete(12, 12, 1, img, 144, all, 144, &p, NULL, &out, NULL) == PC_OK);
    CHECK(out == NULL);
    CHECK(pc_algo_nonlocal_complete(12, 12, 1, img, 144, all, 3, &p, NULL, &out, NULL) == PC_OK);
    CHECK(out != NULL && memcmp(out, img, sizeof img) == 0);
    free(out);
    out = NULL;
    CHECK(pc_algo_nonlocal_complete(12, 12, 0, img, 144, all, 144, &p, NULL, &out, NULL) == PC_OK);
    CHECK(out != NULL && memcmp(out, img, sizeof img) == 0);
    free(out);
}

static bool yes(void *ctx)
{
    (void)ctx;
    return true;
}

static void progress(void *ctx, float f)
{
    (void)ctx, (void)f;
}

static void test_cancelled_is_an_error(void)
{
    size_t w = 64, h = 64, n = w * h, i;
    float *img = malloc(n * sizeof(float)), *out = NULL;
    bool *hole = box(w, h, 20, 40, 20, 40);
    pc_algo_nonlocal_params p = pc_algo_nonlocal_params_default();
    pc_interrupt stop = {yes, progress, NULL};
    for (i = 0; i < n; i++) {
        img[i] = noise(i);
    }
    CHECK(pc_algo_nonlocal_complete(w, h, 1, img, n, hole, n, &p, &stop, &out, NULL) == PC_ERR_CANCELLED);
    CHECK(out == NULL);
    free(img), free(hole);
}

static void test_never_crash(void)
{
    size_t w = 30, h = 24, n = w * h, i, levels;
    float *img = malloc(n * 3 * sizeof(float)), *out = NULL;
    bool *hole = box(w, h, 10, 18, 8, 15), *none = calloc(n, 1);
    pc_algo_nonlocal_params p = pc_algo_nonlocal_params_default(), q;
    for (i = 0; i < n * 3; i++) {
        img[i] = i % 11 == 0 ? NAN : (i % 13 == 0 ? INFINITY : noise(i));
    }
    /* NaN / infinite pixels, odd parameters. */
    CHECK(pc_algo_nonlocal_complete(w, h, 3, img, n * 3, hole, n, &p, NULL, &out, NULL) == PC_OK);
    free(out);
    out = NULL;
    q = p, q.lambda = NAN, q.tolerance = NAN, q.patch_radius = 0, q.max_iters = 0, q.pm_passes = 0;
    CHECK(pc_algo_nonlocal_complete(w, h, 3, img, n * 3, hole, n, &q, NULL, &out, NULL) == PC_OK);
    free(out);
    out = NULL;
    q = p, q.lambda = -INFINITY, q.tolerance = INFINITY, q.seed = UINT64_MAX;
    CHECK(pc_algo_nonlocal_complete(w, h, 3, img, n * 3, hole, n, &q, NULL, &out, NULL) == PC_OK);
    free(out);
    out = NULL;
    /* A radius larger than the image: nothing to copy from. */
    q = p, q.patch_radius = 40;
    CHECK(pc_algo_nonlocal_complete(w, h, 3, img, n * 3, hole, n, &q, NULL, &out, NULL) == PC_OK);
    CHECK(out == NULL);
    /* Hostile radii and counts. */
    q = p, q.patch_radius = SIZE_MAX;
    CHECK(pc_algo_nonlocal_complete(w, h, 3, img, n * 3, hole, n, &q, NULL, &out, NULL) == PC_ERR_LIMIT);
    q = p, q.max_iters = SIZE_MAX;
    CHECK(pc_algo_nonlocal_complete(w, h, 3, img, n * 3, hole, n, &q, NULL, &out, NULL) == PC_ERR_LIMIT);
    q = p, q.pm_passes = SIZE_MAX;
    CHECK(pc_algo_nonlocal_complete(w, h, 3, img, n * 3, hole, n, &q, NULL, &out, NULL) == PC_ERR_LIMIT);
    /* No hole, NULL params, a short buffer, absurd sizes, an empty image. */
    CHECK(pc_algo_nonlocal_complete(w, h, 3, img, n * 3, none, n, &p, NULL, &out, NULL) == PC_OK);
    CHECK(out != NULL && memcmp(out, img, n * 3 * sizeof(float)) == 0);
    free(out);
    out = NULL;
    CHECK(pc_algo_nonlocal_complete(w, h, 3, img, n * 3, hole, n, NULL, NULL, &out, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_nonlocal_complete(w, h, 3, img, n * 3 - 1, hole, n, &p, NULL, &out, NULL) == PC_OK);
    free(out);
    out = NULL;
    CHECK(pc_algo_nonlocal_complete(SIZE_MAX, SIZE_MAX, 3, img, n * 3, hole, n, &p, NULL, &out, NULL) ==
          PC_OK);
    free(out);
    out = NULL;
    CHECK(pc_algo_nonlocal_complete(0, 0, 1, NULL, 0, NULL, 0, &p, NULL, &out, NULL) == PC_OK);
    free(out);
    CHECK(pc_algo_nonlocal_level_count(w, h, hole, SIZE_MAX / 4, &levels, NULL) == PC_OK);
    CHECK_EQ_UINT(levels, 1);
    free(img), free(hole), free(none);
}

int main(int argc, char **argv)
{
    RUN(test_a_hole_in_texture_next_to_a_flat_area_of_the_same_mean_gets_texture);
    RUN(test_a_hole_in_a_gradient_follows_the_gradient);
    RUN(test_levels_follow_the_hole_thickness);
    RUN(test_nothing_to_copy_from_and_bad_sizes);
    RUN(test_cancelled_is_an_error);
    RUN(test_never_crash);
    return pc_t_finish();
}
