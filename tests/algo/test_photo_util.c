/* photo_util.rs: dense / sparse least squares, sRGB, sampling, hash, RNG. */
#include <math.h>
#include <stdlib.h>

#include "algo/photo_util.h"
#include "harness.h"

static void test_dense_and_sparse_solvers_agree(void)
{
    /* Overdetermined line fit y = 2x + 1. */
    pc_algo_photo_normal ne;
    pc_algo_photo_sparse_ls sp;
    double a[2], b[2], x0[2] = {0.0, 0.0};
    double m[4] = {1.0, 2.0, 2.0, 4.0}, r[2] = {1.0, 2.0}, sol[2] = {7.0, 7.0};
    int x;
    CHECK(pc_algo_photo_normal_init(&ne, 2, NULL) == PC_OK);
    pc_algo_photo_sparse_ls_init(&sp, 2);
    for (x = 0; x < 10; x++) {
        double y = 2.0 * x + 1.0;
        pc_algo_photo_term row[2] = {{0, (double)x}, {1, 1.0}};
        pc_algo_photo_normal_add(&ne, row, 2, y, 1.0);
        CHECK(pc_algo_photo_sparse_ls_add(&sp, row, 2, y, 1.0, NULL) == PC_OK);
    }
    CHECK(pc_algo_photo_normal_solve(&ne, a));
    CHECK(pc_algo_photo_sparse_ls_solve(&sp, x0, 50, b, NULL) == PC_OK);
    CHECK(fabs(a[0] - 2.0) < 1e-9 && fabs(a[1] - 1.0) < 1e-9);
    CHECK(fabs(b[0] - 2.0) < 1e-6 && fabs(b[1] - 1.0) < 1e-6);
    CHECK(!pc_algo_photo_solve_dense(2, m, r, sol));
    CHECK(sol[0] == 7.0 && sol[1] == 7.0); /* out untouched on failure */
    pc_algo_photo_normal_fini(&ne);
    pc_algo_photo_sparse_ls_fini(&sp);
}

static void test_srgb_round_trip_and_sampling(void)
{
    const float vs[5] = {0.0f, 0.01f, 0.2f, 0.5f, 1.0f};
    const float buf[4] = {0.0f, 1.0f, 2.0f, 3.0f};
    float w[4], a;
    int i;
    for (i = 0; i < 5; i++) {
        CHECK(fabsf(pc_algo_photo_linear_to_srgb(pc_algo_photo_srgb_to_linear(vs[i])) - vs[i]) < 1e-5f);
    }
    CHECK(fabsf(pc_algo_photo_bilinear(buf, 4, 2, 2, 1, 0, 0.5f, 0.5f) - 1.5f) < 1e-6f);
    pc_algo_photo_catmull_rom(0.3f, w);
    CHECK(fabsf(w[0] + w[1] + w[2] + w[3] - 1.0f) < 1e-6f);
    a = pc_algo_photo_hash01(3, 4, 1);
    CHECK(a >= 0.0f && a < 1.0f && a == pc_algo_photo_hash01(3, 4, 1) && a != pc_algo_photo_hash01(4, 3, 1));
}

static void test_hostile_inputs(void)
{
    const float buf[4] = {0.0f, 1.0f, 2.0f, 3.0f};
    pc_algo_photo_rng r = {0};
    pc_algo_photo_normal ne;
    pc_algo_photo_term t[2] = {{5, 1.0}, {0, 1.0}};
    double x[1];
    /* Empty, short or out-of-range buffers sample as 0 instead of reading out of bounds. */
    CHECK(pc_algo_photo_bilinear(buf, 4, 0, 0, 1, 0, 0.0f, 0.0f) == 0.0f);
    CHECK(pc_algo_photo_bilinear(buf, 3, 2, 2, 1, 0, 0.0f, 0.0f) == 0.0f);
    CHECK(pc_algo_photo_bilinear(buf, 4, 2, 2, 1, 1, 0.0f, 0.0f) == 0.0f);
    CHECK(isnan(pc_algo_photo_bilinear(buf, 4, 2, 2, 1, 0, NAN, 0.0f)));
    CHECK(pc_algo_photo_bilinear(buf, 4, 2, 2, 1, 0, INFINITY, -INFINITY) == 1.0f);
    CHECK(pc_algo_photo_rng_below(&r, 0) == 0);
    CHECK(pc_algo_photo_rng_unit(&r) < 1.0);
    /* Terms past n are ignored. */
    CHECK(pc_algo_photo_normal_init(&ne, 1, NULL) == PC_OK);
    pc_algo_photo_normal_add(&ne, t, 2, 3.0, 1.0);
    CHECK(pc_algo_photo_normal_solve(&ne, x) && x[0] == 3.0);
    pc_algo_photo_normal_fini(&ne);
    CHECK(pc_algo_photo_solve_dense(0, NULL, NULL, NULL));
}

int main(int argc, char **argv)
{
    (void)argc, (void)argv;
    RUN(test_dense_and_sparse_solvers_agree);
    RUN(test_srgb_round_trip_and_sampling);
    RUN(test_hostile_inputs);
    return pc_t_finish();
}
