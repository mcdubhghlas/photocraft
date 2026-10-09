#include <float.h>
#include <math.h>

#include "base/num.h"
#include "harness.h"

static void test_size_add_mul(void)
{
    size_t r = 77;
    CHECK(pc_size_add(1, 2, &r) && r == 3);
    r = 77;
    CHECK(!pc_size_add(SIZE_MAX, 1, &r));
    CHECK_EQ_UINT(r, 77); /* untouched on overflow */
    CHECK(pc_size_add(SIZE_MAX - 1, 1, &r) && r == SIZE_MAX);
    CHECK(pc_size_mul(0, SIZE_MAX, &r) && r == 0);
    CHECK(pc_size_mul(SIZE_MAX, 1, &r) && r == SIZE_MAX);
    r = 5;
    CHECK(!pc_size_mul(SIZE_MAX / 2 + 1, 2, &r));
    CHECK_EQ_UINT(r, 5);
    CHECK(pc_size_mul3(4, 5, 6, &r) && r == 120);
    CHECK(!pc_size_mul3(SIZE_MAX, 2, 1, &r));
    CHECK(!pc_size_mul3(1, 2, SIZE_MAX, &r));
}

static void test_i64_checked(void)
{
    int64_t r = 0;
    CHECK(pc_i64_add(INT64_MAX - 1, 1, &r) && r == INT64_MAX);
    CHECK(!pc_i64_add(INT64_MAX, 1, &r));
    CHECK(!pc_i64_add(INT64_MIN, -1, &r));
    CHECK(pc_i64_add(INT64_MIN, INT64_MAX, &r) && r == -1);
    CHECK(pc_i64_mul(-3, 4, &r) && r == -12);
    CHECK(pc_i64_mul(0, INT64_MIN, &r) && r == 0);
    CHECK(!pc_i64_mul(-1, INT64_MIN, &r));
    CHECK(!pc_i64_mul(INT64_MIN, -1, &r));
    CHECK(!pc_i64_mul(INT64_MAX / 2 + 1, 2, &r));
    CHECK(!pc_i64_mul(INT64_MIN / 2 - 1, 2, &r));
    CHECK(pc_i64_mul(INT64_MIN / 2, 2, &r) && r == INT64_MIN);
    CHECK(!pc_i64_mul(-(INT64_MAX / 2) - 2, -2, &r));
}

static void test_i32_saturating(void)
{
    CHECK_EQ_INT(pc_i32_sat_add(INT32_MAX, 1), INT32_MAX);
    CHECK_EQ_INT(pc_i32_sat_add(INT32_MIN, -1), INT32_MIN);
    CHECK_EQ_INT(pc_i32_sat_add(-5, 3), -2);
    CHECK_EQ_INT(pc_i32_sat_sub(INT32_MIN, 1), INT32_MIN);
    CHECK_EQ_INT(pc_i32_sat_sub(INT32_MAX, -1), INT32_MAX);
    CHECK_EQ_INT(pc_u32_as_i32(0xFFFFFFFFu), -1);
    CHECK_EQ_INT(pc_u32_as_i32(5), 5);
}

static void test_i32_wrapping(void)
{
    CHECK_EQ_INT(pc_i32_wrapping_add(INT32_MAX, 1), INT32_MIN);
    CHECK_EQ_INT(pc_i32_wrapping_sub(INT32_MIN, 1), INT32_MAX);
    CHECK_EQ_INT(pc_i32_wrapping_mul(0x10000, 0x10000), 0);
    CHECK_EQ_INT(pc_i32_wrapping_mul(-7, 3), -21);
    CHECK_EQ_INT(pc_i32_from_bits(0xFFFFFFFFu), -1);
    CHECK_EQ_INT(pc_i32_from_bits(0x80000000u), INT32_MIN);
}

/* Expected values are what Rust's `as` produces for the same inputs. */
static void test_float_casts_match_rust(void)
{
    CHECK_EQ_UINT(pc_f32_to_u8(NAN), 0);
    CHECK_EQ_UINT(pc_f32_to_u8(-1.0f), 0);
    CHECK_EQ_UINT(pc_f32_to_u8(-0.9f), 0);
    CHECK_EQ_UINT(pc_f32_to_u8(254.99f), 254);
    CHECK_EQ_UINT(pc_f32_to_u8(255.0f), 255);
    CHECK_EQ_UINT(pc_f32_to_u8(1e30f), 255);
    CHECK_EQ_UINT(pc_f32_to_u8(INFINITY), 255);
    CHECK_EQ_UINT(pc_f32_to_u16(65535.9f), 65535);
    CHECK_EQ_UINT(pc_f32_to_u16(-INFINITY), 0);
    CHECK_EQ_UINT(pc_f32_to_u32(4294967296.0f), UINT32_MAX);
    CHECK_EQ_UINT(pc_f64_to_u32(4294967294.5), 4294967294u);
    CHECK_EQ_UINT(pc_f32_to_u64(1e30f), UINT64_MAX);
    CHECK_EQ_UINT(pc_f64_to_u64(18446744073709549568.0), 18446744073709549568u); /* 2^64 - 2048 */
    CHECK_EQ_INT(pc_f32_to_i32(NAN), 0);
    CHECK_EQ_INT(pc_f32_to_i32(-2.7f), -2);
    CHECK_EQ_INT(pc_f32_to_i32(2.7f), 2);
    CHECK_EQ_INT(pc_f32_to_i32(3e9f), INT32_MAX);
    CHECK_EQ_INT(pc_f32_to_i32(-3e9f), INT32_MIN);
    CHECK_EQ_INT(pc_f64_to_i32(2147483647.9), INT32_MAX);
    CHECK_EQ_INT(pc_f64_to_i32(-2147483648.9), INT32_MIN);
    CHECK_EQ_INT(pc_f64_to_i64(1e300), INT64_MAX);
    CHECK_EQ_INT(pc_f64_to_i64(-1e300), INT64_MIN);
    CHECK_EQ_INT(pc_f64_to_i64(-9223372036854775808.0), INT64_MIN);
    CHECK_EQ_UINT(pc_f64_to_size(-5.0), 0);
    CHECK_EQ_UINT(pc_f64_to_size(1e300), SIZE_MAX);
    CHECK_EQ_UINT(pc_f32_to_size(12.9f), 12);
}

static void test_f64_to_f32_like_rust(void)
{
    CHECK_NEAR(pc_f64_to_f32(0.1), 0.100000001490116119384765625, 0.0);
    CHECK(isinf(pc_f64_to_f32(1e300)) && pc_f64_to_f32(1e300) > 0);
    CHECK(isinf(pc_f64_to_f32(-1e300)) && pc_f64_to_f32(-1e300) < 0);
    CHECK(pc_f64_to_f32(0x1.fffffe8p127) == FLT_MAX); /* above FLT_MAX, below the midpoint: rounds down */
    CHECK(isinf(pc_f64_to_f32(0x1.ffffffp127)));            /* the midpoint rounds to even: inf */
    CHECK(isnan(pc_f64_to_f32(NAN)));
    CHECK(pc_f64_to_f32(1e-50) == 0.0f);
}

static void test_clamp(void)
{
    CHECK_NEAR(pc_clampf(2.0f, 0.0f, 1.0f), 1.0, 0.0);
    CHECK_NEAR(pc_clampf(-2.0f, 0.0f, 1.0f), 0.0, 0.0);
    CHECK_NEAR(pc_clampf(0.25f, 0.0f, 1.0f), 0.25, 0.0);
    CHECK(isnan(pc_clampf(NAN, 0.0f, 1.0f))); /* Rust: NaN.clamp(..) is NaN */
    CHECK(isnan(pc_clampd(NAN, 0.0, 1.0)));
    CHECK_NEAR(pc_clampd(-INFINITY, -1.0, 1.0), -1.0, 0.0);
    CHECK_EQ_INT(pc_clamp_i32(-5, 0, 10), 0);
    CHECK_EQ_INT(pc_clamp_i32(50, 0, 10), 10);
    CHECK_EQ_INT(pc_clamp_i64(INT64_MIN, -1, 1), -1);
}

int main(int argc, char **argv)
{
    RUN(test_size_add_mul);
    RUN(test_i64_checked);
    RUN(test_i32_saturating);
    RUN(test_i32_wrapping);
    RUN(test_float_casts_match_rust);
    RUN(test_f64_to_f32_like_rust);
    RUN(test_clamp);
    return pc_t_finish();
}
