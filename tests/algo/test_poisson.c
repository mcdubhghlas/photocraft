/* Poisson / membrane solver (src/algo/poisson.c): the crates/algo/src/poisson.rs tests plus
 * cancellation and never-crash cases. */
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "harness.h"

static bool *disc(size_t w, size_t h, float cx, float cy, float r)
{
    bool *m = malloc(w * h);
    size_t i;
    for (i = 0; i < w * h; i++) {
        m[i] = hypotf((float)(i % w) - cx, (float)(i / w) - cy) < r;
    }
    return m;
}

static void test_membrane_reproduces_linear_boundary(void)
{
    /* A linear function is harmonic: the solve must reproduce it inside the hole. */
    size_t w = 80, h = 60, i;
    bool *unknown = disc(w, h, 40, 30, 20);
    float *truth = malloc(w * h * sizeof(float)), *v = malloc(w * h * sizeof(float)), err = 0.0f;
    for (i = 0; i < w * h; i++) {
        truth[i] = 0.2f + 0.005f * (float)(i % w) + 0.003f * (float)(i / w);
        v[i] = unknown[i] ? 0.0f : truth[i];
    }
    CHECK(pc_algo_poisson_solve_membrane(w, h, unknown, v, NULL, NULL) == PC_OK);
    for (i = 0; i < w * h; i++) {
        err = fmaxf(err, fabsf(v[i] - truth[i]));
    }
    CHECK(err < 2e-3f);
    free(unknown), free(truth), free(v);
}

static void test_multigrid_matches_reference_solve(void)
{
    size_t w = 96, h = 72, i;
    bool *unknown = disc(w, h, 45, 36, 28), known_same = true;
    float *init = malloc(w * h * sizeof(float)), *fast = malloc(w * h * sizeof(float));
    float *ref = malloc(w * h * sizeof(float)), err = 0.0f;
    for (i = 0; i < w * h; i++) {
        init[i] = sinf((float)(i % w) * 0.21f) * 0.3f + cosf((float)(i / w) * 0.13f) * 0.2f + 0.5f;
    }
    memcpy(fast, init, w * h * sizeof(float));
    memcpy(ref, init, w * h * sizeof(float));
    CHECK(pc_algo_poisson_solve_membrane(w, h, unknown, fast, NULL, NULL) == PC_OK);
    CHECK(pc_algo_poisson_solve_membrane_reference(w, h, unknown, ref, 1e-7f, 100000, NULL) == PC_OK);
    for (i = 0; i < w * h; i++) {
        err = fmaxf(err, fabsf(fast[i] - ref[i]));
        known_same = known_same && (unknown[i] || fast[i] == init[i]);
    }
    CHECK(err < 2e-3f);
    CHECK(known_same);
    free(unknown), free(init), free(fast), free(ref);
}

static void test_seamless_clone_takes_destination_colour_and_source_texture(void)
{
    size_t w = 64, h = 64, ch = 3, i, c, n = 0, cnt = 0;
    bool *mask = disc(w, h, 32, 32, 20);
    float *dst = malloc(w * h * ch * sizeof(float)), *src = malloc(w * h * ch * sizeof(float)), *out = NULL;
    float e = 0.0f;
    for (i = 0; i < w * h; i++) {
        float t = ((i % w) / 4) % 2 == 0 ? 0.05f : -0.05f;
        dst[i * 3] = 0.7f, dst[i * 3 + 1] = 0.3f, dst[i * 3 + 2] = 0.2f;
        src[i * 3] = 0.1f + t, src[i * 3 + 1] = 0.2f + t, src[i * 3 + 2] = 0.8f + t;
    }
    CHECK(pc_algo_poisson_seamless_clone(w, h, ch, src, dst, w * h * ch, mask, w * h, &out, NULL) == PC_OK);
    /* Mean colour inside the mask matches the destination. */
    for (c = 0; c < ch; c++) {
        float m = -0.0f;
        n = 0;
        for (i = 0; i < w * h; i++) {
            if (mask[i]) {
                m += out[i * ch + c];
                n++;
            }
        }
        m /= (float)n;
        CHECK(fabsf(m - dst[c]) < 0.02f);
    }
    /* Source gradients carried: horizontal differences inside match the source's. */
    for (i = 0; i + 1 < w * h; i++) {
        if (mask[i] && mask[i + 1]) {
            e += fabsf((out[(i + 1) * ch] - out[i * ch]) - (src[(i + 1) * ch] - src[i * ch]));
            cnt++;
        }
    }
    CHECK(e / (float)cnt < 0.01f);
    /* Outside untouched. */
    CHECK(memcmp(out, dst, 3 * sizeof(float)) == 0);
    free(mask), free(dst), free(src), free(out);
}

static bool yes(void *ctx)
{
    (void)ctx;
    return true;
}

static bool after_n(void *ctx)
{
    atomic_int *n = ctx;
    return atomic_fetch_add(n, 1) >= 3;
}

static void test_cancel(void)
{
    size_t w = 64, h = 48;
    bool *hole = disc(w, h, 32, 24, 15);
    float *im = calloc(w * h * 2, sizeof(float)), *out = (float *)&w;
    pc_interrupt stop = {yes, NULL, NULL};
    atomic_int count = 0;
    pc_interrupt later = {after_n, NULL, &count};
    pc_error err;
    CHECK(pc_algo_poisson_membrane_fill(w, h, 2, im, w * h * 2, hole, w * h, &stop, &out, &err) ==
          PC_ERR_CANCELLED);
    CHECK(out == (float *)&w); /* untouched on failure */
    CHECK_EQ_STR(err.msg, "cancelled");
    CHECK(pc_algo_poisson_membrane_fill(w, h, 2, im, w * h * 2, hole, w * h, &later, &out, NULL) ==
          PC_ERR_CANCELLED);
    free(hole), free(im);
}

static void test_never_crash(void)
{
    float v[16] = {0}, *out = NULL, nanv[25];
    bool m[25], none[1] = {false};
    size_t i;
    pc_error err;
    for (i = 0; i < 25; i++) {
        m[i] = i % 3 == 0;
        nanv[i] = i % 2 ? NAN : INFINITY;
    }
    /* Empty grids, mismatched lengths, bad channel counts, absurd sizes. */
    CHECK(pc_algo_poisson_solve_membrane(0, 0, NULL, NULL, NULL, NULL) == PC_OK);
    CHECK(pc_algo_poisson_membrane_fill(0, 7, 3, NULL, 0, NULL, 0, NULL, &out, NULL) == PC_OK);
    free(out);
    CHECK(pc_algo_poisson_membrane_fill(4, 4, 1, v, 15, m, 16, NULL, &out, &err) == PC_ERR_INVALID);
    CHECK(pc_algo_poisson_membrane_fill(4, 4, 1, v, 16, m, 15, NULL, &out, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_poisson_membrane_fill(4, 4, 0, v, 0, m, 16, NULL, &out, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_poisson_membrane_fill(4, 4, 99, v, 16 * 99, m, 16, NULL, &out, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_poisson_membrane_fill((size_t)1 << 30, (size_t)1 << 30, 1, v, 16, m, 16, NULL, &out,
                                        NULL) == PC_ERR_LIMIT);
    CHECK(pc_algo_poisson_seamless_clone(SIZE_MAX, 2, 1, v, v, 16, m, 16, &out, NULL) == PC_ERR_LIMIT);
    CHECK(pc_algo_poisson_seamless_clone(4, 4, 1, v, v, 16, m, 3, &out, NULL) == PC_ERR_INVALID);
    /* NaN / infinite known values, all-unknown and no-unknown masks. */
    CHECK(pc_algo_poisson_membrane_fill(5, 5, 1, nanv, 25, m, 25, NULL, &out, NULL) == PC_OK);
    free(out);
    memset(m, 1, sizeof m);
    CHECK(pc_algo_poisson_membrane_fill(5, 5, 1, nanv, 25, m, 25, NULL, &out, NULL) == PC_OK);
    free(out);
    CHECK(pc_algo_poisson_seamless_clone(1, 1, 1, v, v, 1, none, 1, &out, NULL) == PC_OK);
    CHECK(out != NULL && out[0] == 0.0f);
    free(out);
    CHECK(pc_algo_poisson_solve_membrane_reference(5, 5, m, nanv, 1e-5f, 10, NULL) == PC_OK);
    /* A degenerate strip too long for the direct solve is refused, not run for minutes. */
    {
        size_t len = ((size_t)1 << 23) + 8; /* 2 x len > 2^24 unknowns */
        bool *strip = malloc(len * 2);
        float *sv = calloc(len * 2, sizeof(float));
        if (strip != NULL && sv != NULL) {
            memset(strip, 1, len * 2);
            strip[0] = false;
            CHECK(pc_algo_poisson_solve_membrane(2, len, strip, sv, NULL, &err) == PC_ERR_LIMIT);
        }
        free(strip), free(sv);
    }
}

int main(int argc, char **argv)
{
    RUN(test_membrane_reproduces_linear_boundary);
    RUN(test_multigrid_matches_reference_solve);
    RUN(test_seamless_clone_takes_destination_colour_and_source_texture);
    RUN(test_cancel);
    RUN(test_never_crash);
    return pc_t_finish();
}
