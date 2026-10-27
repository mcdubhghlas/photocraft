/* Private helpers shared by the src/vector files. Not part of the public API. */
#ifndef PC_VECTOR_INTERNAL_H
#define PC_VECTOR_INTERNAL_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "vector/vector.h"

/* Rust's f64::total_cmp as a sortable key: -NaN < -inf < ... < -0 < +0 < ... < +inf < +NaN. */
static inline int64_t pc_vec_total_key(double x)
{
    uint64_t u;
    int64_t k;
    memcpy(&u, &x, sizeof u);
    /* Flip the magnitude bits of negatives so integer order matches the float order (done on
     * the unsigned bits: shifting a negative signed value is implementation-defined). */
    if (u >> 63) {
        u ^= 0x7fffffffffffffffu;
    }
    memcpy(&k, &u, sizeof k);
    return k;
}

static inline int pc_vec_total_cmp(double a, double b)
{
    int64_t ka = pc_vec_total_key(a);
    int64_t kb = pc_vec_total_key(b);
    return (ka > kb) - (ka < kb);
}

static inline int32_t pc_vec_total_key_f32(float x)
{
    uint32_t u;
    int32_t b;
    memcpy(&u, &x, sizeof u);
    if (u >> 31) {
        u ^= 0x7fffffffu;
    }
    memcpy(&b, &u, sizeof b);
    return b;
}

static inline int pc_vec_total_cmp_f32(float a, float b)
{
    int32_t ka = pc_vec_total_key_f32(a);
    int32_t kb = pc_vec_total_key_f32(b);
    return (ka > kb) - (ka < kb);
}

/* Stable merge sort of n elements of `size` bytes (Rust's slice::sort_by is stable, and ties
 * decide accumulation order, so qsort would break bit parity). scratch must hold n elements. */
void pc_vec_stable_sort(void *base, size_t n, size_t size, void *scratch,
                        int (*cmp)(const void *a, const void *b));

/* Polyline append that skips a point equal (within 1e-12) to the previous one, upgrading its
 * knot flag (flatten.rs Polyline::push). */
pc_status pc_vec_polyline_push(pc_vec_polyline *p, double x, double y, bool knot, pc_error *err);
/* Plain append (no de-duplication). */
pc_status pc_vec_polyline_append(pc_vec_polyline *p, double x, double y, bool knot, pc_error *err);
/* Appends a polygon taking ownership of pts (freed on failure too). */
pc_status pc_vec_polys_push(pc_vec_polys *l, double (*pts)[2], size_t len, pc_error *err);
/* Frees the polygons from index `from` on and truncates (rollback after a failure). */
void pc_vec_polys_truncate(pc_vec_polys *l, size_t from);

/* Builds a pc_path from n subpaths (the array is taken over, also on failure). */
void pc_vec_path_from(pc_subpath *subpaths, size_t n, pc_path *out);

/* ---- mask feathering (feather.c, port of feather.rs) ---- */
/* Gaussian sigma of a feather of `px` pixels (non-finite -> 0; capped at Photoshop's 1000 px). */
float pc_vec_feather_sigma(float px);
/* `r` grown by how far a feather of Gaussian `sigma` carries a mask's edge (empty stays empty). */
pc_rect pc_vec_feather_grow(pc_rect r, float sigma);
/* Where a feathered path mask can differ from its far-outside value: the path's control bounds
 * (plus two pixels of anti-aliasing) grown by the feather. Empty for a path without knots. */
pc_rect pc_vec_feather_path_area(const pc_path *path, float sigma);
/* In-place approximate Gaussian blur of a w x h plane (three extended-box passes per axis, exact
 * variance sigma^2, edges clamp). Bit-identical to the compositor's vector-mask feather. */
pc_status pc_vec_feather_gaussian(float *v, size_t w, size_t h, float sigma, pc_error *err);

/* Rust's f64 to_radians / to_degrees: a multiply by the folded constant. */
static inline double pc_vec_to_radians(double x)
{
    return x * (3.141592653589793 / 180.0);
}
static inline double pc_vec_to_degrees(double x)
{
    return x * (180.0 / 3.141592653589793);
}

#define PC_VEC_PI 3.141592653589793
#define PC_VEC_TAU 6.283185307179586
#define PC_VEC_FRAC_PI_2 1.5707963267948966

#endif
