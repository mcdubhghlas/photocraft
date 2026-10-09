/* pc_cbrt / pc_cbrtf must equal Rust's f64::cbrt / f32::cbrt bit for bit (hashes from rustc 1.95:
 * the same LCG over raw bit patterns, FNV-1a over little-endian results). */
#include <math.h>

#include "base/bytes.h"
#include "base/rmath.h"
#include "harness.h"

static void fnv(uint64_t *h, uint64_t v, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        *h ^= (v >> (8 * i)) & 0xffu;
        *h *= 0x100000001b3u;
    }
}

static void test_matches_rust_over_random_bits(void)
{
    static const double edge[10] = {0.0, -0.0, 1.0, -8.0, 1e-310, -1e-320, INFINITY, 2.2250738585072014e-308, 27.0,
                                    0x1.9b78223aa307cp+1};
    uint64_t h = 0xcbf29ce484222325u;
    uint64_t g = 0xcbf29ce484222325u;
    uint64_t s = 1;
    int i;
    for (i = 0; i < 1000000; i++) {
        s = s * 6364136223846793005u + 1442695040888963407u;
        fnv(&h, pc_f64_to_bits(pc_cbrt(pc_f64_from_bits(s))), 8);
        fnv(&g, pc_f32_to_bits(pc_cbrtf(pc_f32_from_bits((uint32_t)(s >> 32)))), 4);
    }
    for (i = 0; i < 10; i++) {
        fnv(&h, pc_f64_to_bits(pc_cbrt(edge[i])), 8);
    }
    CHECK_EQ_UINT(h, 0x5991973dea5bbe3bu);
    CHECK_EQ_UINT(g, 0x5f917fb61b85afb3u);
}

static void test_values(void)
{
    CHECK_NEAR(pc_cbrt(27.0), 3.0, 0.0);
    CHECK_NEAR(pc_cbrt(-8.0), -2.0, 0.0);
    CHECK_NEAR(pc_cbrtf(64.0f), 4.0, 0.0);
    CHECK(isnan(pc_cbrt(NAN)) && isnan(pc_cbrtf(NAN)));
    CHECK(pc_cbrt(-INFINITY) == -INFINITY);
    CHECK(pc_f64_to_bits(pc_cbrt(-0.0)) == 0x8000000000000000u);
}

int main(int argc, char **argv)
{
    RUN(test_matches_rust_over_random_bits);
    RUN(test_values);
    return pc_t_finish();
}
