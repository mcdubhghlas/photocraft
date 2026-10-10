/* Math functions the Rust reference does NOT take from the C library.
 *
 * Rust's std implements some f32/f64 functions itself (compiler_builtins' libm) instead of
 * calling libm, and its compiler rewrites a few calls with constant arguments. Where the
 * results differ from glibc's, PhotoCraft uses these so output stays bit-identical to the
 * reference (REWRITE_PLAN.md §4, numeric parity). Everything else comes from <math.h>.
 *
 *   pc_cbrt   correctly rounded cube root (CORE-MATH, as in Rust's libm 0.2.16); glibc's cbrt
 *             is off by an ulp on ~20% of inputs
 *   pc_cbrtf  FreeBSD cbrtf, as in Rust's libm
 *
 * Also remember: Rust/LLVM compiles pow(x, 0.5) to fabs(sqrt(x)) and pow(x, 2.0) to x * x when
 * the exponent is a compile-time constant; write those forms explicitly when porting.
 * Pure functions; thread-safe. */
#ifndef PC_BASE_RMATH_H
#define PC_BASE_RMATH_H

#include <math.h>

double pc_cbrt(double x);
float pc_cbrtf(float x);

/* Rust's f32/f64 min and max (LLVM minnum/maxnum) exactly as rustc lowers them on x86-64:
 * a NaN operand yields the other one; when the operands compare equal (0.0 vs -0.0) the
 * FIRST is returned. C's fminf leaves that sign unspecified (glibc versions differ), and the
 * sign of zero shows up in bit-exact comparisons. Port `a.min(b)` as pc_rs_minf(a, b). */
static inline float pc_rs_minf(float a, float b)
{
    return isnan(a) ? b : (b < a ? b : a);
}
static inline float pc_rs_maxf(float a, float b)
{
    return isnan(a) ? b : (b > a ? b : a);
}
static inline double pc_rs_min(double a, double b)
{
    return isnan(a) ? b : (b < a ? b : a);
}
static inline double pc_rs_max(double a, double b)
{
    return isnan(a) ? b : (b > a ? b : a);
}

/* `a.min(K)` / `a.max(K)` where K is a literal constant (never NaN): LLVM then emits a single
 * minss/maxss computing `a < K ? a : K`, so on a tie the SECOND operand wins: (-0.0).max(0.0)
 * is +0.0 here but pc_rs_maxf(-0.0f, 0.0f) is -0.0. Use these when the Rust source has a
 * literal (or a constant that inlining makes literal) as the second argument. A NaN `a` gives K, as
 * in Rust; the explicit isnan keeps clang from swapping the minss operands (seen with clang 23 -O2
 * after an inlined clamp: it returned the NaN). */
static inline float pc_rs_minf_k(float a, float k)
{
    return isnan(a) ? k : (a < k ? a : k);
}
static inline float pc_rs_maxf_k(float a, float k)
{
    return isnan(a) ? k : (a > k ? a : k);
}
static inline double pc_rs_min_k(double a, double k)
{
    return isnan(a) ? k : (a < k ? a : k);
}
static inline double pc_rs_max_k(double a, double k)
{
    return isnan(a) ? k : (a > k ? a : k);
}

#endif
