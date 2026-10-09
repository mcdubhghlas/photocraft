/* Checked size arithmetic and Rust-compatible numeric conversions.
 *
 * Why this exists: in C, an out-of-range float->int cast and signed overflow are undefined
 * behaviour, while the Rust reference saturates (`as`) or wraps (`wrapping_*`). Port every such
 * expression through these helpers so results match the reference bit for bit (REWRITE_PLAN.md §4).
 * All functions are pure and thread-safe. */
#ifndef PC_BASE_NUM_H
#define PC_BASE_NUM_H

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- checked size_t arithmetic: return false on overflow and leave *out untouched ---- */

static inline bool pc_size_add(size_t a, size_t b, size_t *out)
{
    if (a > SIZE_MAX - b) {
        return false;
    }
    *out = a + b;
    return true;
}

static inline bool pc_size_mul(size_t a, size_t b, size_t *out)
{
    if (a != 0 && b > SIZE_MAX / a) {
        return false;
    }
    *out = a * b;
    return true;
}

static inline bool pc_size_mul3(size_t a, size_t b, size_t c, size_t *out)
{
    size_t ab;
    return pc_size_mul(a, b, &ab) && pc_size_mul(ab, c, out);
}

/* ---- checked signed 64-bit arithmetic ---- */

static inline bool pc_i64_add(int64_t a, int64_t b, int64_t *out)
{
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
        return false;
    }
    *out = a + b;
    return true;
}

static inline bool pc_i64_mul(int64_t a, int64_t b, int64_t *out)
{
    if (a == 0 || b == 0) {
        *out = 0;
        return true;
    }
    if ((a == -1 && b == INT64_MIN) || (b == -1 && a == INT64_MIN)) {
        return false;
    }
    if (a > 0 ? (b > 0 ? a > INT64_MAX / b : b < INT64_MIN / a)
              : (b > 0 ? a < INT64_MIN / b : a < INT64_MAX / b)) {
        return false;
    }
    *out = a * b;
    return true;
}

/* ---- Rust `wrapping_*` on i32 (two's-complement results without signed overflow) ---- */

static inline int32_t pc_i32_from_bits(uint32_t u)
{
    /* Avoids the implementation-defined unsigned->signed conversion for values > INT32_MAX. */
    if (u <= (uint32_t)INT32_MAX) {
        return (int32_t)u;
    }
    return (int32_t)(u - 0x80000000u) + INT32_MIN;
}

static inline int32_t pc_i32_wrapping_add(int32_t a, int32_t b)
{
    return pc_i32_from_bits((uint32_t)a + (uint32_t)b);
}

static inline int32_t pc_i32_wrapping_sub(int32_t a, int32_t b)
{
    return pc_i32_from_bits((uint32_t)a - (uint32_t)b);
}

static inline int32_t pc_i32_wrapping_mul(int32_t a, int32_t b)
{
    return pc_i32_from_bits((uint32_t)a * (uint32_t)b);
}

/* ---- Rust `saturating_*` on i32 ---- */

static inline int32_t pc_i32_sat_add(int32_t a, int32_t b)
{
    int64_t r = (int64_t)a + b;
    return r > INT32_MAX ? INT32_MAX : (r < INT32_MIN ? INT32_MIN : (int32_t)r);
}

static inline int32_t pc_i32_sat_sub(int32_t a, int32_t b)
{
    int64_t r = (int64_t)a - b;
    return r > INT32_MAX ? INT32_MAX : (r < INT32_MIN ? INT32_MIN : (int32_t)r);
}

/* Rust `u32 as i32` (two's-complement reinterpretation). */
static inline int32_t pc_u32_as_i32(uint32_t u)
{
    return pc_i32_from_bits(u);
}

/* ---- Rust `as` from floats: truncate toward zero, saturate at the bounds, NaN -> 0 ---- */
/* The f32 variants widen to double exactly, so one implementation serves both. */

static inline uint8_t pc_f64_to_u8(double x)
{
    if (!(x > 0.0)) { return 0; } /* also NaN */
    if (x >= 255.0) { return UINT8_MAX; }
    return (uint8_t)x;
}

static inline uint16_t pc_f64_to_u16(double x)
{
    if (!(x > 0.0)) { return 0; }
    if (x >= 65535.0) { return UINT16_MAX; }
    return (uint16_t)x;
}

static inline uint32_t pc_f64_to_u32(double x)
{
    if (!(x > 0.0)) { return 0; }
    if (x >= 4294967295.0) { return UINT32_MAX; }
    return (uint32_t)x;
}

static inline uint64_t pc_f64_to_u64(double x)
{
    if (!(x > 0.0)) { return 0; }
    if (x >= 18446744073709551616.0) { return UINT64_MAX; } /* 2^64 */
    return (uint64_t)x;
}

static inline int32_t pc_f64_to_i32(double x)
{
    if (isnan(x)) { return 0; }
    if (x >= 2147483647.0) { return INT32_MAX; }
    if (x <= -2147483648.0) { return INT32_MIN; }
    return (int32_t)x;
}

static inline int64_t pc_f64_to_i64(double x)
{
    if (isnan(x)) { return 0; }
    if (x >= 9223372036854775808.0) { return INT64_MAX; }  /* 2^63 */
    if (x <= -9223372036854775808.0) { return INT64_MIN; }
    return (int64_t)x;
}

/* usize on the reference's 64-bit targets; saturates at SIZE_MAX on narrower size_t. */
static inline size_t pc_f64_to_size(double x)
{
    uint64_t v = pc_f64_to_u64(x);
    return v > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)v;
}

static inline uint8_t pc_f32_to_u8(float x) { return pc_f64_to_u8((double)x); }
static inline uint16_t pc_f32_to_u16(float x) { return pc_f64_to_u16((double)x); }
static inline uint32_t pc_f32_to_u32(float x) { return pc_f64_to_u32((double)x); }
static inline uint64_t pc_f32_to_u64(float x) { return pc_f64_to_u64((double)x); }
static inline int32_t pc_f32_to_i32(float x) { return pc_f64_to_i32((double)x); }
static inline int64_t pc_f32_to_i64(float x) { return pc_f64_to_i64((double)x); }
static inline size_t pc_f32_to_size(float x) { return pc_f64_to_size((double)x); }

/* ---- Rust `f64 as f32`: round to nearest even; beyond float range -> +-inf ----
 * (C leaves out-of-range double -> float conversion undefined unless Annex F applies.) */
static inline float pc_f64_to_f32(double x)
{
    /* 0x1.ffffffp127 is halfway between FLT_MAX and 2^128: from there on, nearest-even
     * rounding gives infinity. */
    if (x >= 0x1.ffffffp127) { return (float)INFINITY; }
    if (x <= -0x1.ffffffp127) { return -(float)INFINITY; }
    return (float)x; /* in range (or NaN): exact IEEE rounding */
}

/* ---- Rust f32/f64::clamp: NaN stays NaN (unlike fminf(fmaxf())) ---- */

static inline float pc_clampf(float x, float lo, float hi)
{
    if (x < lo) { x = lo; }
    if (x > hi) { x = hi; }
    return x;
}

static inline double pc_clampd(double x, double lo, double hi)
{
    if (x < lo) { x = lo; }
    if (x > hi) { x = hi; }
    return x;
}

/* Integer clamps (Ord::clamp). */
static inline int32_t pc_clamp_i32(int32_t x, int32_t lo, int32_t hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline int64_t pc_clamp_i64(int64_t x, int64_t lo, int64_t hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

#endif
