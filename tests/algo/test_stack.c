/* Image stack statistics (src/algo/stack.c): the crates/algo/src/stack.rs tests plus never-crash
 * cases. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "harness.h"

/* One-pixel layers of grey v (opaque). */
static float LAYER_PX[8][1][4];
static const float (*LAYERS[8])[4];
static size_t LENS[8];

static void set_layers(const float *v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        LAYER_PX[i][0][0] = LAYER_PX[i][0][1] = LAYER_PX[i][0][2] = v[i];
        LAYER_PX[i][0][3] = 1.0f;
        LAYERS[i] = (const float (*)[4])LAYER_PX[i];
        LENS[i] = 1;
    }
}

static float at(size_t n, pc_algo_stack_stat s)
{
    float (*out)[4] = NULL, v;
    size_t len = 0;
    CHECK(pc_algo_stack_combine(LAYERS, LENS, n, s, &out, &len, NULL) == PC_OK);
    CHECK_EQ_UINT(len, 1);
    v = out != NULL ? out[0][0] : NAN;
    free(out);
    return v;
}

static void test_basic_statistics(void)
{
    const float v[5] = {0.1f, 0.9f, 0.2f, 0.3f, 0.1f};
    float var, e;
    set_layers(v, 5);
    CHECK(fabsf(at(5, PC_ALGO_STACK_MEAN) - 0.32f) < 1e-6f);
    CHECK(fabsf(at(5, PC_ALGO_STACK_MEDIAN) - 0.2f) < 1e-6f);
    CHECK(at(5, PC_ALGO_STACK_MINIMUM) == 0.1f);
    CHECK(at(5, PC_ALGO_STACK_MAXIMUM) == 0.9f);
    CHECK(fabsf(at(5, PC_ALGO_STACK_RANGE) - 0.8f) < 1e-6f);
    CHECK(at(5, PC_ALGO_STACK_SUMMATION) == 1.0f); /* clamped */
    var = at(5, PC_ALGO_STACK_VARIANCE);
    CHECK(fabsf(at(5, PC_ALGO_STACK_STANDARD_DEVIATION) - sqrtf(var)) < 1e-6f);
    CHECK(at(5, PC_ALGO_STACK_SKEWNESS) > 0.5f); /* right-skewed */
    e = at(5, PC_ALGO_STACK_ENTROPY);
    CHECK(e > 0.0f && e <= 1.0f);
    CHECK(at(5, PC_ALGO_STACK_KURTOSIS) > 0.0f);
}

static void test_transparent_samples_are_ignored(void)
{
    const float a[1][4] = {{0.5f, 0.5f, 0.5f, 1.0f}}, b[1][4] = {{1.0f, 1.0f, 1.0f, 0.0f}};
    const float (*layers[2])[4] = {a, b};
    size_t lens[2] = {1, 1}, len = 0;
    float (*out)[4] = NULL;
    CHECK(pc_algo_stack_combine(layers, lens, 2, PC_ALGO_STACK_MEAN, &out, &len, NULL) == PC_OK);
    CHECK(out[0][0] == 0.5f && out[0][1] == 0.5f && out[0][2] == 0.5f && out[0][3] == 1.0f);
    free(out);
    CHECK(pc_algo_stack_combine(layers + 1, lens, 1, PC_ALGO_STACK_MEAN, &out, &len, NULL) == PC_OK);
    CHECK(out[0][3] == 0.0f);
    free(out);
}

static void test_median_removes_outlier(void)
{
    const float v[3] = {0.5f, 0.5f, 1.0f};
    set_layers(v, 3);
    CHECK(at(3, PC_ALGO_STACK_MEDIAN) == 0.5f);
}

static void test_names(void)
{
    int s;
    for (s = 0; s < PC_ALGO_STACK_STAT_COUNT; s++) {
        pc_algo_stack_stat back;
        const char *n = pc_algo_stack_stat_name((pc_algo_stack_stat)s);
        CHECK(n != NULL && pc_algo_stack_stat_from_name(n, strlen(n), &back) && (int)back == s);
    }
    CHECK_EQ_STR(pc_algo_stack_stat_name(PC_ALGO_STACK_STANDARD_DEVIATION), "StandardDeviation");
    CHECK(pc_algo_stack_stat_name((pc_algo_stack_stat)99) == NULL);
}

static void test_never_crash(void)
{
    const float nan_px[2][4] = {{NAN, INFINITY, -INFINITY, 1.0f}, {0.2f, NAN, 0.3f, NAN}};
    const float (*layers[3])[4] = {nan_px, nan_px, NULL};
    size_t lens[3] = {2, 1, 0}, len = 9;
    float (*out)[4] = NULL;
    int s;
    /* No layers: an empty result. */
    CHECK(pc_algo_stack_combine(NULL, NULL, 0, PC_ALGO_STACK_MEAN, &out, &len, NULL) == PC_OK);
    CHECK_EQ_UINT(len, 0);
    free(out);
    /* NaN / infinite samples, a NULL empty layer and every statistic. */
    for (s = 0; s < PC_ALGO_STACK_STAT_COUNT; s++) {
        CHECK(pc_algo_stack_combine(layers, lens, 3, (pc_algo_stack_stat)s, &out, &len, NULL) == PC_OK);
        CHECK_EQ_UINT(len, 2);
        free(out);
    }
    /* A NULL non-empty layer and an unknown statistic are refused. */
    lens[2] = 4;
    CHECK(pc_algo_stack_combine(layers, lens, 3, PC_ALGO_STACK_MEAN, &out, &len, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_stack_combine(layers, lens, 2, (pc_algo_stack_stat)42, &out, &len, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_stack_stat_of(NULL, 0, PC_ALGO_STACK_MEDIAN) == 0.0f);
}

int main(int argc, char **argv)
{
    RUN(test_basic_statistics);
    RUN(test_transparent_samples_are_ignored);
    RUN(test_median_removes_outlier);
    RUN(test_names);
    RUN(test_never_crash);
    return pc_t_finish();
}
