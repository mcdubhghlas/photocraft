/* vectorscope.rs. */
#include <math.h>
#include <stdlib.h>

#include "algo/vectorscope.h"
#include "harness.h"

static void test_rgb_primary_positions_neutrals_and_transparency(void)
{
    const float cases[4][3] = {{1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}};
    const float expect[4] = {0.0f, 1.0f / 6.0f, 1.0f / 3.0f, 2.0f / 3.0f};
    const float px[4][4] = {{0.5f, 0.5f, 0.5f, 1.0f}, {1, 0, 0, 1}, {0, 1, 0, 0}, {NAN, 0, 0, 1}};
    pc_algo_vectorscope *v = malloc(sizeof *v);
    uint64_t total = 0;
    size_t i;
    for (i = 0; i < 4; i++) {
        float h, s;
        pc_algo_vectorscope_hue_saturation(cases[i], &h, &s);
        CHECK(fabsf(h - expect[i]) < 1e-6f);
        CHECK(s == 1.0f);
    }
    pc_algo_vectorscope_from_rgba(px, 4, v);
    CHECK_EQ_UINT(v->samples, 2);
    CHECK_EQ_UINT(v->counts[0], 1);
    CHECK_EQ_UINT(v->counts[PC_ALGO_VECTORSCOPE_SATURATIONS - 1], 1);
    for (i = 0; i < PC_ALGO_VECTORSCOPE_HUES * PC_ALGO_VECTORSCOPE_SATURATIONS; i++) {
        total += v->counts[i];
    }
    CHECK_EQ_UINT(total, v->samples);
    free(v);
}

static void test_hostile_values(void)
{
    const float px[5][4] = {
        {INFINITY, 0, 0, 1}, {2, -1, 0.5f, 1}, {1, 0.999999f, 0, 1}, {0, 0, 0, 1}, {1, 1, 1, NAN}};
    pc_algo_vectorscope *v = malloc(sizeof *v);
    float h, s;
    const float black[3] = {0, 0, 0}, neg[3] = {-1, -2, -3};
    pc_algo_vectorscope_from_rgba(px, 5, v);
    CHECK_EQ_UINT(v->samples, 3);
    pc_algo_vectorscope_from_rgba(NULL, 0, v);
    CHECK_EQ_UINT(v->samples, 0);
    pc_algo_vectorscope_hue_saturation(black, &h, &s);
    CHECK(h == 0.0f && s == 0.0f);
    pc_algo_vectorscope_hue_saturation(neg, &h, &s);
    CHECK(h == 0.0f && s == 0.0f);
    free(v);
}

int main(int argc, char **argv)
{
    RUN(test_rgb_primary_positions_neutrals_and_transparency);
    RUN(test_hostile_values);
    return pc_t_finish();
}
