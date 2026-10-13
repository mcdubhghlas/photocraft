/* Patch-based hole filling (src/algo/inpaint.c): the crates/algo/src/inpaint.rs tests plus
 * cancellation, progress and never-crash cases. */
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "algo/inpaint_int.h"
#include "harness.h"

static const char *ARGV0;

#define TAU_F 6.28318530717958647692f

/* Smooth gradient plus deterministic fine noise (SplitMix64 seeded 7, as the reference test). */
static float *texture(size_t w, size_t h)
{
    float *v = malloc(w * h * 3 * sizeof(float));
    uint64_t s = 7ull ^ 0x9E3779B97F4A7C15ull;
    size_t i;
    for (i = 0; i < w * h; i++) {
        uint64_t z;
        float x = (float)(i % w), y = (float)(i / w), n;
        s += 0x9E3779B97F4A7C15ull;
        z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        n = (float)(z % 1000) / 1000.0f * 0.04f - 0.02f;
        v[i * 3] = 0.3f + 0.004f * x + n;
        v[i * 3 + 1] = 0.5f + 0.002f * y + n;
        v[i * 3 + 2] = 0.4f + n;
    }
    return v;
}

static bool *disc(size_t w, size_t h, float cx, float cy, float r)
{
    bool *m = malloc(w * h);
    size_t i;
    for (i = 0; i < w * h; i++) {
        m[i] = hypotf((float)(i % w) - cx, (float)(i / w) - cy) < r;
    }
    return m;
}

static float hole_rmse(const float *a, const float *b, const bool *hole, size_t n, size_t ch)
{
    float e = 0.0f;
    size_t i, c, cnt = 0;
    for (i = 0; i < n; i++) {
        if (hole[i]) {
            for (c = 0; c < ch; c++) {
                float d = a[i * ch + c] - b[i * ch + c];
                e += d * d;
                cnt++;
            }
        }
    }
    return sqrtf(e / (float)cnt);
}

static void test_completion_fills_blemish_from_texture(void)
{
    size_t w = 96, h = 96, ch = 3, i, n = w * h;
    float *clean = texture(w, h), *dirty = malloc(n * ch * sizeof(float)), *out = NULL, *again = NULL;
    bool *hole = disc(w, h, 48, 48, 9), outside_same = true;
    pc_algo_complete_params p = pc_algo_complete_params_default();
    float before, after;
    memcpy(dirty, clean, n * ch * sizeof(float));
    for (i = 0; i < n; i++) {
        if (hole[i]) {
            dirty[i * 3] = dirty[i * 3 + 1] = dirty[i * 3 + 2] = 0.02f;
        }
    }
    CHECK(pc_algo_inpaint_complete(w, h, ch, dirty, n * ch, hole, n, &p, NULL, &out, NULL) == PC_OK);
    CHECK(out != NULL);
    if (out == NULL) {
        free(clean), free(dirty), free(hole);
        return;
    }
    before = hole_rmse(dirty, clean, hole, n, ch);
    after = hole_rmse(out, clean, hole, n, ch);
    CHECK(after < 0.06f && after < before * 0.15f);
    /* Outside the hole untouched. */
    for (i = 0; i < n * ch; i++) {
        outside_same = outside_same && (hole[i / ch] || out[i] == dirty[i]);
    }
    CHECK(outside_same);
    /* Deterministic. */
    CHECK(pc_algo_inpaint_complete(w, h, ch, dirty, n * ch, hole, n, &p, NULL, &again, NULL) == PC_OK);
    CHECK(again != NULL && memcmp(out, again, n * ch * sizeof(float)) == 0);
    free(clean), free(dirty), free(hole), free(out), free(again);
}

static void test_completion_continues_structure(void)
{
    /* Vertical stripes: the filled hole should keep the stripes (not a blur). */
    size_t w = 80, h = 80, i, n = w * h;
    float *clean = malloc(n * sizeof(float)), *dirty = malloc(n * sizeof(float)), *out = NULL;
    bool *hole = disc(w, h, 40, 40, 7);
    pc_algo_complete_params p = pc_algo_complete_params_default();
    for (i = 0; i < n; i++) {
        clean[i] = ((i % w / 4) % 2 == 0) ? 0.2f : 0.8f;
        dirty[i] = hole[i] ? 0.5f : clean[i];
    }
    CHECK(pc_algo_inpaint_complete(w, h, 1, dirty, n, hole, n, &p, NULL, &out, NULL) == PC_OK);
    CHECK(out != NULL && hole_rmse(out, clean, hole, n, 1) < 0.15f);
    free(clean), free(dirty), free(hole), free(out);
}

static void test_proximity_finds_matching_offset(void)
{
    size_t w = 80, h = 60, i;
    float *im = malloc(w * h * sizeof(float));
    bool *hole = disc(w, h, 40, 30, 5), found = false;
    int32_t dx = 0, dy = 0;
    /* Periodic pattern with period 10 in x: a match is any multiple-of-10 displacement. */
    for (i = 0; i < w * h; i++) {
        im[i] = sinf((float)(i % w) * TAU_F / 10.0f) * 0.4f + 0.5f;
    }
    CHECK(pc_algo_inpaint_best_offset(w, h, 1, im, w * h, hole, w * h, 3, 20, &found, &dx, &dy, NULL) ==
          PC_OK);
    CHECK(found);
    CHECK_EQ_INT(((dx % 10) + 10) % 10, 0);
    CHECK(dx != 0 || dy != 0);
    free(im), free(hole);
}

static void test_synthesis_fills_hole_with_plausible_values(void)
{
    size_t w = 64, h = 64, i;
    float *clean = texture(w, h), *out = NULL;
    bool *hole = disc(w, h, 32, 32, 6), finite = true;
    CHECK(pc_algo_inpaint_synthesize(w, h, 3, clean, w * h * 3, hole, w * h, 8, 3, &out, NULL) == PC_OK);
    CHECK(hole_rmse(out, clean, hole, w * h, 3) < 0.15f);
    for (i = 0; i < w * h * 3; i++) {
        finite = finite && isfinite(out[i]);
    }
    CHECK(finite);
    free(clean), free(hole), free(out);
}

static void test_proximity_search_clamps_hostile_radii_without_overflow_or_hanging(void)
{
    size_t w = 24, h = 20, i;
    float *im = malloc(w * h * sizeof(float));
    bool *hole = disc(w, h, 12, 10, 2), found = true;
    int32_t dx = 0, dy = 0;
    for (i = 0; i < w * h; i++) {
        im[i] = (float)(i % w) / (float)w;
    }
    /* User-supplied values used to overflow i32 loops, cast a huge ring into a negative number, or
     * take billions of search iterations. */
    CHECK(pc_algo_inpaint_best_offset(w, h, 1, im, w * h, hole, w * h, SIZE_MAX, INT32_MAX, &found, &dx, &dy,
                                      NULL) == PC_OK);
    if (found) {
        CHECK(abs(dx) <= (int)w && abs(dy) <= (int)h);
    }
    CHECK(pc_algo_inpaint_best_offset(w, h, 1, im, w * h, hole, w * h, 2, -1, &found, &dx, &dy, NULL) ==
          PC_OK);
    CHECK(!found);
    CHECK(pc_algo_inpaint_best_offset(w, h, 1, im, w * h, hole, w * h, 2, 0, &found, &dx, &dy, NULL) ==
          PC_OK);
    CHECK(!found);
    free(im), free(hole);
}

/* Structure, fine noise and flat patches (so that some matches tie exactly), ch channels. */
static float *busy(size_t w, size_t h, size_t ch, uint64_t seed)
{
    float *v = malloc(w * h * ch * sizeof(float));
    pc_ip_rng rng = pc_ip_rng_new(seed);
    size_t i, c;
    for (i = 0; i < w * h; i++) {
        size_t x = i % w, y = i / w;
        bool flat = (x / 9 + y / 7) % 5 == 0;
        float n = (float)(pc_ip_rng_next(&rng) % 1000) / 1000.0f;
        for (c = 0; c < ch; c++) {
            v[i * ch + c] =
                flat
                    ? 0.25f + 0.1f * (float)c
                    : fminf(0.2f + 0.004f * (float)x + 0.1f * (float)((x + 2 * y + c) % 6) + 0.05f * n, 1.0f);
        }
    }
    return v;
}

/* Roles: a hole, a band that is context only, a corner that is no image. */
static uint8_t *roles(size_t w, size_t h, const bool *hole)
{
    uint8_t *px = malloc(w * h);
    size_t i;
    for (i = 0; i < w * h; i++) {
        size_t x = i % w, y = i / w;
        px[i] = hole[i]                                 ? PC_ALGO_PX_HOLE
                : (x < 4 && y < 6)                      ? PC_ALGO_PX_VOID
                : (x >= w * 3 / 4 && x < w * 3 / 4 + 3) ? PC_ALGO_PX_KNOWN
                                                        : PC_ALGO_PX_SOURCE;
    }
    return px;
}

static uint64_t fnv_floats(const float *v, size_t n)
{
    uint64_t hsh = 0xcbf29ce484222325ull;
    size_t i, b;
    for (i = 0; i < n; i++) {
        uint32_t u;
        memcpy(&u, &v[i], 4);
        for (b = 0; b < 4; b++) {
            hsh ^= (u >> (8 * b)) & 0xffu;
            hsh *= 0x100000001b3ull;
        }
    }
    return hsh;
}

/* The busy 300 x 260 completion of results_do_not_depend_on_the_thread_count: case 0 is 3 channels
 * without adaptation, case 1 four channels with it. Returns the hash of the result (0 on failure). */
static uint64_t busy_case(int which)
{
    size_t w = 300, h = 260, ch = which == 0 ? 3 : 4, n = w * h;
    float *im = busy(w, h, ch, 3), *out = NULL;
    bool *hole = disc(w, h, 140, 130, 40);
    uint8_t *px = roles(w, h, hole);
    pc_algo_complete_params p = pc_algo_complete_params_default();
    uint64_t hsh = 0;
    if (which == 1) {
        p.adapt = (pc_algo_adapt){0.9f, 1.1f, 0.04f};
    }
    if (pc_algo_inpaint_complete_masked(w, h, ch, im, n * ch, px, n, &p, NULL, &out, NULL) == PC_OK && out) {
        hsh = fnv_floats(out, n * ch);
    }
    free(im), free(hole), free(px), free(out);
    return hsh;
}

/* Run busy_case in a fresh process with PHOTOCRAFT_THREADS=threads (the pool's size is fixed per
 * process). */
static uint64_t busy_case_in_child(int which, int threads)
{
    char cmd[4096];
    unsigned long long v = 0;
    FILE *f;
    if (ARGV0 == NULL || strchr(ARGV0, '\'') != NULL ||
        snprintf(cmd, sizeof cmd, "PHOTOCRAFT_THREADS=%d '%s' --busy-child %d", threads, ARGV0, which) >=
            (int)sizeof cmd) {
        return 0;
    }
    f = popen(cmd, "r");
    if (f == NULL) {
        return 0;
    }
    if (fscanf(f, "%llx", &v) != 1) {
        v = 0;
    }
    if (pclose(f) != 0) {
        v = 0;
    }
    return (uint64_t)v;
}

static void test_results_do_not_depend_on_the_thread_count(void)
{
    /* Big enough for the parallel passes. Each thread count runs in its own process. */
    int which;
    for (which = 0; which < 2; which++) {
        uint64_t one = busy_case_in_child(which, 1);
        CHECK(one != 0);
        CHECK(one == busy_case_in_child(which, 3));
        CHECK(one == busy_case_in_child(which, 16));
    }
}

static bool yes2(void *ctx)
{
    (void)ctx;
    return true;
}

static void test_owned_completion_fills_in_place_or_gives_the_pixels_back(void)
{
    size_t w = 64, h = 52, ch = 3, n = w * h, i;
    float *im = busy(w, h, ch, 6), *borrowed = NULL, *owned = malloc(n * ch * sizeof(float)),
          *none_out = NULL;
    bool *hole = disc(w, h, 30, 26, 8), filled = false;
    uint8_t *px = roles(w, h, hole), *known = malloc(n);
    pc_algo_complete_params p = pc_algo_complete_params_default();
    pc_interrupt stop = {yes2, NULL, NULL};
    CHECK(pc_algo_inpaint_complete_masked(w, h, ch, im, n * ch, px, n, &p, NULL, &borrowed, NULL) == PC_OK);
    memcpy(owned, im, n * ch * sizeof(float));
    CHECK(pc_algo_inpaint_complete_masked_inplace(w, h, ch, owned, n * ch, px, n, &p, NULL, &filled, NULL) ==
          PC_OK);
    CHECK(filled);
    CHECK(borrowed != NULL && memcmp(owned, borrowed, n * ch * sizeof(float)) == 0);
    /* Nothing may be copied: no completion, and the pixels come back as they were. */
    for (i = 0; i < n; i++) {
        known[i] = px[i] == PC_ALGO_PX_SOURCE ? PC_ALGO_PX_KNOWN : px[i];
    }
    none_out = (float *)&w;
    CHECK(pc_algo_inpaint_complete_masked(w, h, ch, im, n * ch, known, n, &p, NULL, &none_out, NULL) ==
          PC_OK);
    CHECK(none_out == NULL);
    memcpy(owned, im, n * ch * sizeof(float));
    filled = true;
    CHECK(pc_algo_inpaint_complete_masked_inplace(w, h, ch, owned, n * ch, known, n, &p, NULL, &filled,
                                                  NULL) == PC_OK);
    CHECK(!filled);
    CHECK(memcmp(owned, im, n * ch * sizeof(float)) == 0);
    /* Cancelled, it stops. */
    memcpy(owned, im, n * ch * sizeof(float));
    CHECK(pc_algo_inpaint_complete_masked_inplace(w, h, ch, owned, n * ch, px, n, &p, &stop, &filled, NULL) ==
          PC_ERR_CANCELLED);
    free(im), free(borrowed), free(owned), free(hole), free(px), free(known);
}

static void test_masked_never_crash(void)
{
    size_t w = 20, h = 16, n = w * h, i;
    float *im = busy(w, h, 2, 1), *out = NULL;
    bool *hole = disc(w, h, 10, 8, 3), filled;
    uint8_t *px = roles(w, h, hole);
    pc_algo_complete_params p = pc_algo_complete_params_default();
    /* Unknown roles, bad lengths, unusable adaptation limits. */
    px[5] = 4;
    CHECK(pc_algo_inpaint_complete_masked(w, h, 2, im, n * 2, px, n, &p, NULL, &out, NULL) == PC_ERR_INVALID);
    px[5] = 255;
    CHECK(pc_algo_inpaint_complete_masked_inplace(w, h, 2, im, n * 2, px, n, &p, NULL, &filled, NULL) ==
          PC_ERR_INVALID);
    px[5] = PC_ALGO_PX_SOURCE;
    CHECK(pc_algo_inpaint_complete_masked(w, h, 2, im, n * 2, px, n - 1, &p, NULL, &out, NULL) ==
          PC_ERR_INVALID);
    CHECK(pc_algo_inpaint_complete_masked(w, h, 2, im, n * 2, NULL, n, &p, NULL, &out, NULL) ==
          PC_ERR_INVALID);
    p.adapt = (pc_algo_adapt){1.1f, 0.9f, 0.0f};
    CHECK(pc_algo_inpaint_complete_masked(w, h, 2, im, n * 2, px, n, &p, NULL, &out, NULL) == PC_ERR_INVALID);
    p.adapt = (pc_algo_adapt){0.9f, 1.1f, NAN};
    CHECK(pc_algo_inpaint_complete_masked(w, h, 2, im, n * 2, px, n, &p, NULL, &out, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_adapt_check((pc_algo_adapt){0.9f, 1.1f, -1.0f}, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_adapt_check(PC_ALGO_ADAPT_NONE, NULL) == PC_OK);
    /* Extreme but valid limits, NaN pixels, every role void. */
    p.adapt = (pc_algo_adapt){0.0f, INFINITY, INFINITY};
    for (i = 0; i < n * 2; i += 7) {
        im[i] = NAN;
    }
    CHECK(pc_algo_inpaint_complete_masked(w, h, 2, im, n * 2, px, n, &p, NULL, &out, NULL) == PC_OK);
    free(out);
    out = NULL;
    memset(px, PC_ALGO_PX_VOID, n);
    CHECK(pc_algo_inpaint_complete_masked(w, h, 2, im, n * 2, px, n, &p, NULL, &out, NULL) == PC_OK);
    free(out);
    free(im), free(hole), free(px);
}

static bool yes(void *ctx)
{
    (void)ctx;
    return true;
}

static bool after_n(void *ctx)
{
    atomic_int *n = ctx;
    return atomic_fetch_add(n, 1) >= 40;
}

static atomic_uint LAST;
static atomic_bool BACKWARDS;

static void track(void *ctx, float f)
{
    uint32_t u;
    float last;
    unsigned int prev = atomic_load(&LAST);
    (void)ctx;
    memcpy(&last, &prev, 4);
    if (f < last) {
        atomic_store(&BACKWARDS, true);
    }
    memcpy(&u, &f, 4);
    atomic_store(&LAST, u);
}

static void test_cancel_and_progress(void)
{
    size_t w = 96, h = 80, n = w * h;
    float *tex = texture(w, h), *out = (float *)&w, *plain = NULL, last;
    bool *hole = disc(w, h, 48, 40, 14);
    pc_algo_complete_params p = pc_algo_complete_params_default();
    pc_interrupt stop = {yes, NULL, NULL}, prog = {NULL, track, NULL};
    atomic_int count = 0;
    pc_interrupt later = {after_n, NULL, &count};
    unsigned int u;
    CHECK(pc_algo_inpaint_complete(w, h, 3, tex, n * 3, hole, n, &p, &stop, &out, NULL) == PC_ERR_CANCELLED);
    CHECK(out == (float *)&w);
    CHECK(pc_algo_inpaint_complete(w, h, 3, tex, n * 3, hole, n, &p, &later, &out, NULL) == PC_ERR_CANCELLED);
    atomic_store(&LAST, 0u);
    atomic_store(&BACKWARDS, false);
    CHECK(pc_algo_inpaint_complete(w, h, 3, tex, n * 3, hole, n, &p, &prog, &out, NULL) == PC_OK);
    u = atomic_load(&LAST);
    memcpy(&last, &u, 4);
    CHECK(last == 1.0f);
    CHECK(!atomic_load(&BACKWARDS));
    CHECK(pc_algo_inpaint_complete(w, h, 3, tex, n * 3, hole, n, &p, NULL, &plain, NULL) == PC_OK);
    CHECK(out != (float *)&w && plain != NULL && memcmp(out, plain, n * 3 * sizeof(float)) == 0);
    free(tex), free(hole), free(out), free(plain);
}

static void test_none_and_trivial(void)
{
    float im[64], *out = (float *)im;
    bool all[64], none[64];
    pc_algo_complete_params p = pc_algo_complete_params_default();
    size_t i;
    for (i = 0; i < 64; i++) {
        im[i] = (float)i / 64.0f, all[i] = true, none[i] = false;
    }
    /* No fully-known patch: None (NULL with PC_OK). */
    CHECK(pc_algo_inpaint_complete(8, 8, 1, im, 64, all, 64, &p, NULL, &out, NULL) == PC_OK);
    CHECK(out == NULL);
    /* No hole: a copy. */
    CHECK(pc_algo_inpaint_complete(8, 8, 1, im, 64, none, 64, &p, NULL, &out, NULL) == PC_OK);
    CHECK(out != NULL && memcmp(out, im, sizeof im) == 0);
    free(out);
    CHECK(pc_algo_inpaint_synthesize(8, 8, 1, im, 64, none, 64, 8, 1, &out, NULL) == PC_OK);
    CHECK(memcmp(out, im, sizeof im) == 0);
    free(out);
}

static void test_never_crash(void)
{
    float im[100], *out = NULL;
    bool hole[100], found = true;
    int32_t dx, dy;
    pc_algo_complete_params p = pc_algo_complete_params_default(), q;
    pc_error err;
    size_t i;
    for (i = 0; i < 100; i++) {
        im[i] = i % 3 == 0 ? NAN : (i % 7 == 0 ? INFINITY : (float)i / 100.0f);
        hole[i] = (i % 10 > 3 && i % 10 < 7 && i / 10 > 3 && i / 10 < 7);
    }
    /* NaN / infinite pixels. */
    CHECK(pc_algo_inpaint_complete(10, 10, 1, im, 100, hole, 100, &p, NULL, &out, NULL) == PC_OK);
    free(out);
    out = NULL;
    CHECK(pc_algo_inpaint_synthesize(10, 10, 1, im, 100, hole, 100, 4, 1, &out, NULL) == PC_OK);
    free(out);
    CHECK(pc_algo_inpaint_best_offset(10, 10, 1, im, 100, hole, 100, 2, 5, &found, &dx, &dy, NULL) == PC_OK);
    /* Hostile parameters: radii, iteration counts, rings and blocks. */
    q = p, q.patch_radius = SIZE_MAX;
    CHECK(pc_algo_inpaint_complete(10, 10, 1, im, 100, hole, 100, &q, NULL, &out, &err) == PC_ERR_LIMIT);
    q = p, q.em_iters = SIZE_MAX;
    CHECK(pc_algo_inpaint_complete(10, 10, 1, im, 100, hole, 100, &q, NULL, &out, NULL) == PC_ERR_LIMIT);
    q = p, q.pm_iters = (size_t)1 << 40;
    CHECK(pc_algo_inpaint_complete(10, 10, 1, im, 100, hole, 100, &q, NULL, &out, NULL) == PC_ERR_LIMIT);
    q = p, q.patch_radius = 0, q.em_iters = 0, q.min_em_iters = 0, q.pm_iters = 0, q.seed = UINT64_MAX;
    CHECK(pc_algo_inpaint_complete(10, 10, 1, im, 100, hole, 100, &q, NULL, &out, NULL) == PC_OK);
    free(out);
    q = p, q.patch_radius = PC_ALGO_INPAINT_MAX_RADIUS; /* larger than the image: None */
    CHECK(pc_algo_inpaint_complete(10, 10, 1, im, 100, hole, 100, &q, NULL, &out, NULL) == PC_OK);
    CHECK(out == NULL);
    /* A huge ring is clamped to the image's extent (as the reference). */
    CHECK(pc_algo_inpaint_best_offset(10, 10, 1, im, 100, hole, 100, SIZE_MAX, 5, &found, &dx, &dy, NULL) ==
          PC_OK);
    /* The reference never finishes for radii near i32::MAX (its counters overflow). */
    CHECK(pc_algo_inpaint_best_offset(10, 10, 1, im, 100, hole, 100, 1, INT32_MAX, &found, &dx, &dy, NULL) ==
          PC_OK);
    CHECK(pc_algo_inpaint_best_offset(10, 10, 1, im, 100, hole, 100, 1, INT32_MIN, &found, &dx, &dy, NULL) ==
          PC_OK);
    CHECK(!found);
    CHECK(pc_algo_inpaint_best_offset(10, 10, 1, im, 100, hole, 100, PC_ALGO_HEAL_MAX_SIDE, 3, &found, &dx,
                                      &dy, NULL) == PC_OK);
    CHECK(pc_algo_inpaint_synthesize(10, 10, 1, im, 100, hole, 100, SIZE_MAX, 1, &out, NULL) == PC_ERR_LIMIT);
    CHECK(pc_algo_inpaint_synthesize(10, 10, 1, im, 100, hole, 100, PC_ALGO_HEAL_MAX_SIDE, 1, &out, NULL) ==
          PC_OK);
    free(out);
    /* Bad lengths, channel counts and sizes; NULL params. */
    CHECK(pc_algo_inpaint_complete(10, 10, 1, im, 99, hole, 100, &p, NULL, &out, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_inpaint_complete(10, 10, 1, im, 100, hole, 9, &p, NULL, &out, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_inpaint_complete(10, 10, 1, im, 100, hole, 100, NULL, NULL, &out, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_inpaint_complete(10, 10, 0, im, 0, hole, 100, &p, NULL, &out, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_inpaint_complete(SIZE_MAX, SIZE_MAX, 1, im, 100, hole, 100, &p, NULL, &out, NULL) ==
          PC_ERR_LIMIT);
    CHECK(pc_algo_inpaint_best_offset(10, 10, 1, im, 100, hole, 1, 2, 5, &found, &dx, &dy, NULL) ==
          PC_ERR_INVALID);
    CHECK(pc_algo_inpaint_synthesize(10, 10, 2, im, 100, hole, 100, 4, 1, &out, NULL) == PC_ERR_INVALID);
    /* Empty images. */
    CHECK(pc_algo_inpaint_complete(0, 0, 3, NULL, 0, NULL, 0, &p, NULL, &out, NULL) == PC_OK);
    free(out);
    CHECK(pc_algo_inpaint_synthesize(0, 5, 3, NULL, 0, NULL, 0, 4, 1, &out, NULL) == PC_OK);
    free(out);
    CHECK(pc_algo_inpaint_best_offset(0, 0, 1, NULL, 0, NULL, 0, 2, 5, &found, &dx, &dy, NULL) == PC_OK);
    CHECK(!found);
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--busy-child") == 0) {
        printf("%016llx\n", (unsigned long long)busy_case(atoi(argv[2])));
        return 0;
    }
    ARGV0 = argv[0];
    RUN(test_completion_fills_blemish_from_texture);
    RUN(test_completion_continues_structure);
    RUN(test_proximity_finds_matching_offset);
    RUN(test_proximity_search_clamps_hostile_radii_without_overflow_or_hanging);
    RUN(test_results_do_not_depend_on_the_thread_count);
    RUN(test_owned_completion_fills_in_place_or_gives_the_pixels_back);
    RUN(test_masked_never_crash);
    RUN(test_synthesis_fills_hole_with_plausible_values);
    RUN(test_cancel_and_progress);
    RUN(test_none_and_trivial);
    RUN(test_never_crash);
    return pc_t_finish();
}
