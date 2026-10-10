/* Internal helpers shared by the L6 filter modules (not part of the public API): def macros, the
 * reference's halo helpers and small numeric ports. */
#ifndef PC_ALGO_FX_DEFS_H
#define PC_ALGO_FX_DEFS_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "algo/filter.h"
#include "algo/fxutil.h"
#include "algo/image.h"
#include "base/num.h"
#include "base/rmath.h"

#define PC_FX_N(a) (sizeof(a) / sizeof((a)[0]))

/* A struct variant: tag, params type, field table, defaults (or NULL), halo, kernel. */
#define PC_FX_DEF(name, tg, T, F, init, halo, kern)                                                          \
    const pc_algo_filter_def name = {(tg), sizeof(T), (F),  PC_FX_N(F), (init), NULL, (halo),                \
                                     NULL, NULL,      NULL, (kern),     NULL,   0}
/* A unit variant (no parameters). */
#define PC_FX_UNIT(name, tg, halo, kern)                                                                     \
    const pc_algo_filter_def name = {(tg), 0, NULL, 0, NULL, NULL, (halo), NULL, NULL, NULL, (kern), NULL, 0}

/* Rust's f32::to_radians (multiplies by the f32 constant PI / 180). */
#define PC_FX_RAD_PER_DEG (3.14159265358979323846f / 180.0f)
static inline float pc_fx_rad(float deg)
{
    return deg * PC_FX_RAD_PER_DEG;
}

#define PC_FX_PI 3.14159265358979323846f
#define PC_FX_TAU 6.28318530717958647692f
#define PC_FX_FRAC_PI_2 1.57079632679489661923f
#define PC_FX_FRAC_PI_4 0.785398163397448309616f
#define PC_FX_FRAC_1_SQRT_2 0.707106781186547524401f
#define PC_FX_SQRT_2 1.41421356237309504880f

/* Pixels of a rect (out rects are bounded by the driver; never negative). */
static inline size_t pc_fx_npx(pc_rect r)
{
    return (size_t)pc_rect_width(r) * (size_t)pc_rect_height(r);
}

/* other::edge_of: the sampling edge for an UndefinedAreas choice (other.c). */
pc_algo_edge pc_fx_edge_of(pc_algo_undefined_areas u);

/* Halo::Bounds. */
static inline pc_algo_halo pc_fx_halo_bounds(const void *p, const pc_rect *b)
{
    (void)p;
    (void)b;
    return pc_algo_halo_bounds();
}

/* Sample at 64-bit coordinates (0 outside the image or the i32 range). */
static inline float pc_fx_get64(const pc_algo_image *img, int64_t x, int64_t y, size_t c)
{
    if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX) {
        return 0.0f;
    }
    return pc_algo_image_get(img, (int32_t)x, (int32_t)y, c);
}

/* i32::div_euclid (b > 0; 0 otherwise). */
static inline int32_t pc_fx_div_euclid(int32_t a, int32_t b)
{
    int32_t q;
    if (b <= 0) {
        return 0; /* callers pass positive cell sizes */
    }
    q = a / b;
    if (a % b < 0) {
        q -= 1;
    }
    return q;
}

/* x.powi(2) / x.powi(3) as LLVM expands them. */
static inline float pc_fx_sq(float x)
{
    return x * x;
}

/* f32::signum (NaN stays NaN). */
static inline float pc_fx_signum(float x)
{
    if (isnan(x)) {
        return x;
    }
    return signbit(x) ? -1.0f : 1.0f;
}

/* f32::fract. */
static inline float pc_fx_fract(float x)
{
    return x - truncf(x);
}

#endif
