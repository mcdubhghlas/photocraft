/* Private to src/paint: Rust's `a.min(K)` / `a.max(K)` with a literal K, NaN-guarded.
 *
 * Same results as pc_rs_minf_k / pc_rs_maxf_k in base/rmath.h (NaN -> K, a tie -> K), but with an
 * explicit isnan test: clang 23 at -O2 folds `pc_rs_minf_k(pc_clampf(x, 0, 1), 1)` to the clamp
 * result, so a NaN survived where the reference (and gcc) give K. */
#ifndef PC_PAINT_PMATH_H
#define PC_PAINT_PMATH_H

#include <math.h>

static inline float pc_paint_minf_k(float a, float k)
{
    return isnan(a) ? k : (a < k ? a : k);
}
static inline float pc_paint_maxf_k(float a, float k)
{
    return isnan(a) ? k : (a > k ? a : k);
}
static inline double pc_paint_min_k(double a, double k)
{
    return isnan(a) ? k : (a < k ? a : k);
}
static inline double pc_paint_max_k(double a, double k)
{
    return isnan(a) ? k : (a > k ? a : k);
}

#endif
