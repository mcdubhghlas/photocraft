/* Checksums come from the `half` 2.7.1 crate via tools/rustref (same FNV-1a and LCG). */
#include <math.h>

#include "base/bytes.h"
#include "base/half.h"
#include "harness.h"

static void fnv(uint64_t *h, const uint8_t *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        *h ^= b[i];
        *h *= 0x100000001b3u;
    }
}

static uint64_t lcg(uint64_t *s)
{
    *s = *s * 6364136223846793005u + 1442695040888963407u;
    return *s;
}

static void test_exhaustive_f16_to_float(void)
{
    uint64_t a = 0xcbf29ce484222325u;
    uint64_t b = 0xcbf29ce484222325u;
    uint32_t i;
    for (i = 0; i <= 0xFFFFu; i++) {
        uint8_t t4[4];
        uint8_t t8[8];
        pc_store_le32(t4, pc_f32_to_bits(pc_f16_to_f32((uint16_t)i)));
        pc_store_le64(t8, pc_f64_to_bits(pc_f16_to_f64((uint16_t)i)));
        fnv(&a, t4, 4);
        fnv(&b, t8, 8);
    }
    CHECK_EQ_UINT(a, 0x5d79f1b086f30345u);
    CHECK_EQ_UINT(b, 0x848769a3ea63c745u);
}

static void test_float_to_f16_matches_crate(void)
{
    uint64_t c = 0xcbf29ce484222325u;
    uint64_t d = 0xcbf29ce484222325u;
    uint64_t s = 1;
    int i;
    for (i = 0; i < 2000000; i++) {
        uint64_t r = lcg(&s);
        uint8_t t[2];
        pc_store_le16(t, pc_f16_from_f32(pc_f32_from_bits((uint32_t)(r >> 32))));
        fnv(&c, t, 2);
        pc_store_le16(t, pc_f16_from_f64(pc_f64_from_bits(r)));
        fnv(&d, t, 2);
    }
    CHECK_EQ_UINT(c, 0x781d51e5d5b48c8eu);
    CHECK_EQ_UINT(d, 0x19ffb1e80a13a1beu);
}

static void test_known_values(void)
{
    CHECK_EQ_UINT(pc_f16_from_f32(1.0f), 0x3C00);
    CHECK_EQ_UINT(pc_f16_from_f32(-2.0f), 0xC000);
    CHECK_EQ_UINT(pc_f16_from_f32(65504.0f), 0x7BFF);
    CHECK_EQ_UINT(pc_f16_from_f32(65520.0f), 0x7C00); /* rounds up to infinity */
    CHECK_EQ_UINT(pc_f16_from_f32(1e-8f), 0x0000);
    CHECK_EQ_UINT(pc_f16_from_f32(INFINITY), 0x7C00);
    CHECK((pc_f16_from_f32(NAN) & 0x7E00) == 0x7E00);
    CHECK_NEAR(pc_f16_to_f32(0x3555), 0.333251953125, 0.0);
    CHECK_NEAR(pc_f16_to_f64(0x0001), 5.960464477539063e-8, 0.0); /* smallest subnormal */
    CHECK(isnan(pc_f16_to_f32(0x7E00)));
    CHECK_EQ_UINT(pc_f32_to_bits(pc_f16_to_f32(0x8000)), 0x80000000u);
}

int main(int argc, char **argv)
{
    RUN(test_exhaustive_f16_to_float);
    RUN(test_float_to_f16_matches_crate);
    RUN(test_known_values);
    return pc_t_finish();
}
