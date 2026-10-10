/* Content-Aware Fill (src/algo/content_aware.c): the crates/algo/src/content_aware.rs tests plus
 * never-crash cases. */
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "harness.h"

/* Vertical stripes (period 6) in grey, with an 7x7 hole in the middle. */
static void stripes(size_t w, size_t h, float **im, bool **hole)
{
    size_t i;
    *im = malloc(w * h * sizeof(float));
    *hole = malloc(w * h);
    for (i = 0; i < w * h; i++) {
        size_t x = i % w, y = i / w;
        (*im)[i] = x % 6 < 3 ? 0.2f : 0.8f;
        (*hole)[i] = (x > w / 2 ? x - w / 2 : w / 2 - x) < 4 && (y > h / 2 ? y - h / 2 : h / 2 - y) < 4;
    }
}

static bool *all_true(size_t n)
{
    bool *m = malloc(n);
    memset(m, 1, n);
    return m;
}

static bool yes(void *ctx)
{
    (void)ctx;
    return true;
}

static atomic_uint LAST;
static atomic_bool BACKWARDS;

static void track(void *ctx, float f)
{
    unsigned int prev = atomic_load(&LAST), u;
    float last;
    (void)ctx;
    memcpy(&last, &prev, 4);
    if (f < last) {
        atomic_store(&BACKWARDS, true);
    }
    memcpy(&u, &f, 4);
    atomic_store(&LAST, u);
}

static void test_cancelled_fill_stops_and_progress_reaches_one(void)
{
    size_t w = 60, h = 40;
    float *im, *out = NULL, *plain = NULL, last;
    bool *hole, *src = all_true(w * h);
    pc_algo_ca_fill_options o = pc_algo_ca_fill_options_default();
    pc_interrupt stop = {yes, NULL, NULL}, prog = {NULL, track, NULL};
    unsigned int u;
    stripes(w, h, &im, &hole);
    CHECK(pc_algo_ca_fill(w, h, 1, im, w * h, hole, src, w * h, &o, &stop, &out, NULL) == PC_ERR_CANCELLED);
    atomic_store(&LAST, 0u);
    atomic_store(&BACKWARDS, false);
    CHECK(pc_algo_ca_fill(w, h, 1, im, w * h, hole, src, w * h, &o, &prog, &out, NULL) == PC_OK);
    u = atomic_load(&LAST);
    memcpy(&last, &u, 4);
    CHECK(last == 1.0f);
    CHECK(!atomic_load(&BACKWARDS));
    CHECK(pc_algo_ca_fill(w, h, 1, im, w * h, hole, src, w * h, &o, NULL, &plain, NULL) == PC_OK);
    CHECK(out != NULL && plain != NULL && memcmp(out, plain, w * h * sizeof(float)) == 0);
    free(im), free(hole), free(src), free(out), free(plain);
}

static void test_fills_only_the_hole_with_known_texture(void)
{
    size_t w = 48, h = 32, i;
    float *im, *out = NULL;
    bool *hole, *src = all_true(w * h), lo = false, hi = false, same = true;
    pc_algo_ca_fill_options o = pc_algo_ca_fill_options_default();
    stripes(w, h, &im, &hole);
    o.color_adaptation = 0.0f;
    CHECK(pc_algo_ca_fill(w, h, 1, im, w * h, hole, src, w * h, &o, NULL, &out, NULL) == PC_OK);
    for (i = 0; i < w * h; i++) {
        if (!hole[i]) {
            same = same && out[i] == im[i];
        } else {
            lo = lo || out[i] < 0.35f;
            hi = hi || out[i] > 0.65f;
        }
    }
    CHECK(same);
    /* The hole is filled with stripe values, not a flat average. */
    CHECK(lo && hi);
    free(im), free(hole), free(src), free(out);
}

static void test_sampling_area_restricts_sources(void)
{
    /* Left half dark, right half bright; sampling only the right half fills bright. */
    size_t w = 40, h = 20, i;
    float *im = malloc(w * h * sizeof(float)), *out = NULL, mean = -0.0f;
    bool *hole = malloc(w * h), *right = malloc(w * h);
    pc_algo_ca_fill_options o = pc_algo_ca_fill_options_default();
    for (i = 0; i < w * h; i++) {
        im[i] = i % w < 20 ? 0.1f : 0.9f;
        hole[i] = i % w >= 16 && i % w < 24 && i / w >= 6 && i / w < 14;
        right[i] = i % w >= 24;
    }
    o.color_adaptation = 0.0f;
    CHECK(pc_algo_ca_fill(w, h, 1, im, w * h, hole, right, w * h, &o, NULL, &out, NULL) == PC_OK);
    for (i = 0; i < w * h; i++) {
        if (hole[i]) {
            mean += out[i];
        }
    }
    CHECK(mean / 64.0f > 0.8f);
    free(im), free(hole), free(right), free(out);
}

static void test_adaptations_run_and_stay_deterministic(void)
{
    size_t w = 32, h = 24, i;
    float *im, *rgb = malloc(w * h * 3 * sizeof(float)), *a = NULL, *b = NULL, low[3];
    bool *hole, *src = all_true(w * h), ok = true;
    pc_algo_ca_fill_options o;
    size_t nl = pc_algo_ca_rotation_level("low", low);
    stripes(w, h, &im, &hole);
    for (i = 0; i < w * h; i++) {
        rgb[i * 3] = im[i], rgb[i * 3 + 1] = im[i] * 0.5f, rgb[i * 3 + 2] = 1.0f - im[i];
    }
    o = (pc_algo_ca_fill_options){0.65f, low, nl, true, true, 7};
    CHECK(pc_algo_ca_fill(w, h, 3, rgb, w * h * 3, hole, src, w * h, &o, NULL, &a, NULL) == PC_OK);
    CHECK(pc_algo_ca_fill(w, h, 3, rgb, w * h * 3, hole, src, w * h, &o, NULL, &b, NULL) == PC_OK);
    CHECK(memcmp(a, b, w * h * 3 * sizeof(float)) == 0);
    for (i = 0; i < w * h * 3; i++) {
        ok = ok && isfinite(a[i]) && a[i] >= 0.0f && a[i] <= 1.0f;
    }
    CHECK(ok);
    free(im), free(rgb), free(hole), free(src), free(a), free(b);
}

static void test_color_adaptation_follows_a_gradient(void)
{
    /* A horizontal brightness ramp with a hole: adaptation keeps the fill near the ramp. */
    size_t w = 40, h = 20, i;
    float *im = malloc(w * h * sizeof(float)), *out = NULL, err = -0.0f;
    bool *hole = malloc(w * h), *src = all_true(w * h);
    pc_algo_ca_fill_options o = pc_algo_ca_fill_options_default();
    for (i = 0; i < w * h; i++) {
        im[i] = (float)(i % w) / (float)w;
        hole[i] = i % w >= 14 && i % w < 26 && i / w >= 5 && i / w < 15;
    }
    o.color_adaptation = 0.9f;
    CHECK(pc_algo_ca_fill(w, h, 1, im, w * h, hole, src, w * h, &o, NULL, &out, NULL) == PC_OK);
    for (i = 0; i < w * h; i++) {
        if (hole[i]) {
            err += fabsf(out[i] - im[i]);
        }
    }
    CHECK(err / 120.0f < 0.12f);
    free(im), free(hole), free(src), free(out);
}

static void test_variant_geometry(void)
{
    float im[60], *vi = NULL;
    bool ok[60], *vo = NULL;
    size_t ow = 0, oh = 0, i, cnt = 0;
    for (i = 0; i < 60; i++) {
        im[i] = 0.5f, ok[i] = true;
    }
    CHECK(pc_algo_ca_variant(10, 6, 1, im, ok, 90.0f, 1.0f, false, &ow, &oh, &vi, &vo, NULL) == PC_OK);
    CHECK_EQ_UINT(ow, 6);
    CHECK_EQ_UINT(oh, 10);
    free(vi), free(vo);
    CHECK(pc_algo_ca_variant(10, 6, 1, im, ok, 0.0f, 0.8f, true, &ow, &oh, &vi, &vo, NULL) == PC_OK);
    CHECK_EQ_UINT(ow, 8);
    CHECK_EQ_UINT(oh, 5);
    for (i = 0; i < ow * oh; i++) {
        cnt += vo[i] ? 1u : 0u;
    }
    CHECK(cnt > 20);
    free(vi), free(vo);
}

static void test_levels(void)
{
    float r[3];
    CHECK_EQ_UINT(pc_algo_ca_rotation_level("full", r), 3);
    CHECK(r[0] == 90.0f && r[2] == 270.0f);
    CHECK_EQ_UINT(pc_algo_ca_rotation_level("medium", r), 2);
    CHECK(r[0] == -20.0f);
    CHECK_EQ_UINT(pc_algo_ca_rotation_level("bogus", r), 0);
    CHECK_EQ_UINT(pc_algo_ca_rotation_level(NULL, r), 0);
    CHECK(pc_algo_ca_color_level("none") == 0.0f);
    CHECK(pc_algo_ca_color_level("veryHigh") == 0.9f);
    CHECK(pc_algo_ca_color_level(NULL) == 0.35f);
}

static void test_never_crash(void)
{
    float im[100], *out = NULL, rots[80], *vi;
    bool hole[100], src[100], none[100], *vo;
    size_t i, ow, oh;
    pc_algo_ca_fill_options o = pc_algo_ca_fill_options_default();
    for (i = 0; i < 100; i++) {
        im[i] = i % 3 == 0 ? NAN : (float)i / 100.0f;
        hole[i] = i % 10 > 3 && i % 10 < 7 && i / 10 > 3 && i / 10 < 7;
        src[i] = i % 2 == 0, none[i] = false;
    }
    for (i = 0; i < 80; i++) {
        rots[i] = i % 2 ? NAN : (float)i * 45.0f;
    }
    /* NaN pixels / rotations / adaptation, all sources excluded, too many rotations. */
    o.rotations = rots, o.rotation_count = 8, o.scale = true, o.mirror = true, o.color_adaptation = NAN;
    CHECK(pc_algo_ca_fill(10, 10, 1, im, 100, hole, src, 100, &o, NULL, &out, NULL) == PC_OK);
    free(out);
    o.color_adaptation = 1e30f;
    CHECK(pc_algo_ca_fill(10, 10, 1, im, 100, hole, none, 100, &o, NULL, &out, NULL) == PC_OK);
    free(out);
    o.rotation_count = 80;
    CHECK(pc_algo_ca_fill(10, 10, 1, im, 100, hole, src, 100, &o, NULL, &out, NULL) == PC_ERR_LIMIT);
    o.rotation_count = 2, o.rotations = NULL;
    CHECK(pc_algo_ca_fill(10, 10, 1, im, 100, hole, src, 100, &o, NULL, &out, NULL) == PC_ERR_LIMIT);
    /* Bad lengths, NULL options, empty and absurd sizes. */
    o = pc_algo_ca_fill_options_default();
    CHECK(pc_algo_ca_fill(10, 10, 1, im, 100, hole, src, 99, &o, NULL, &out, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_ca_fill(10, 10, 1, im, 100, hole, src, 100, NULL, NULL, &out, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_ca_fill(SIZE_MAX, 10, 1, im, 100, hole, src, 100, &o, NULL, &out, NULL) == PC_ERR_LIMIT);
    CHECK(pc_algo_ca_fill(0, 0, 1, NULL, 0, NULL, NULL, 0, &o, NULL, &out, NULL) == PC_OK);
    free(out);
    /* Variants with NaN / infinite angles and scales, and an empty source. */
    CHECK(pc_algo_ca_variant(10, 10, 1, im, src, NAN, 1.0f, false, &ow, &oh, &vi, &vo, NULL) == PC_OK);
    free(vi), free(vo);
    CHECK(pc_algo_ca_variant(10, 10, 1, im, src, 30.0f, INFINITY, false, &ow, &oh, &vi, &vo, NULL) ==
          PC_ERR_LIMIT);
    CHECK(pc_algo_ca_variant(10, 10, 1, im, src, 30.0f, 0.0f, true, &ow, &oh, &vi, &vo, NULL) == PC_OK);
    CHECK(ow == 1 && oh == 1);
    free(vi), free(vo);
    CHECK(pc_algo_ca_variant(0, 10, 1, im, src, 0.0f, 1.0f, false, &ow, &oh, &vi, &vo, NULL) ==
          PC_ERR_INVALID);
}

int main(int argc, char **argv)
{
    RUN(test_cancelled_fill_stops_and_progress_reaches_one);
    RUN(test_fills_only_the_hole_with_known_texture);
    RUN(test_sampling_area_restricts_sources);
    RUN(test_adaptations_run_and_stay_deterministic);
    RUN(test_color_adaptation_follows_a_gradient);
    RUN(test_variant_geometry);
    RUN(test_levels);
    RUN(test_never_crash);
    return pc_t_finish();
}
