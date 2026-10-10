/* Patch-based hole filling (src/algo/inpaint.c): the crates/algo/src/inpaint.rs tests plus
 * cancellation, progress and never-crash cases. */
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "harness.h"

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
    CHECK(pc_algo_inpaint_best_offset(10, 10, 1, im, 100, hole, 100, SIZE_MAX, 5, &found, &dx, &dy, NULL) ==
          PC_ERR_LIMIT);
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
    RUN(test_completion_fills_blemish_from_texture);
    RUN(test_completion_continues_structure);
    RUN(test_proximity_finds_matching_offset);
    RUN(test_synthesis_fills_hole_with_plausible_values);
    RUN(test_cancel_and_progress);
    RUN(test_none_and_trivial);
    RUN(test_never_crash);
    return pc_t_finish();
}
