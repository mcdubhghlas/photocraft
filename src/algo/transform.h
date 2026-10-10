/* Free Transform: projective warps of surfaces (port of crates/algo/src/transform.rs).
 *
 * A transform is where the four corners of a source rectangle land (a quad); scale, rotate, skew
 * and flips are affine special cases, Distort and Perspective use the full homography. Warping is
 * an inverse mapping with premultiplied-alpha sampling (nearest, bilinear or Catmull-Rom bicubic),
 * per destination tile in parallel; large reductions are pre-filtered with a bicubic resize first.
 * Pure functions; thread-safe; output independent of the thread count. */
#ifndef PC_ALGO_TRANSFORM_H
#define PC_ALGO_TRANSFORM_H

#include <stdbool.h>

#include "algo/params.h"
#include "base/status.h"
#include "geom/geom.h"
#include "raster/surface.h"

/* 3x3 projective matrix, row-major: [x', y', w'] = H . [x, y, 1]. */
typedef struct pc_algo_homography {
    double m[9];
} pc_algo_homography;

#define PC_ALGO_HOMOGRAPHY_IDENTITY ((pc_algo_homography){{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}})

void pc_algo_homography_apply(const pc_algo_homography *h, double x, double y, double *ox, double *oy);
/* self . o (apply o first). */
pc_algo_homography pc_algo_homography_mul(const pc_algo_homography *a, const pc_algo_homography *b);
/* False (out untouched) when |det| < 1e-12. */
bool pc_algo_homography_inverse(const pc_algo_homography *h, pc_algo_homography *out);
/* The transform taking rect r = {x0, y0, x1, y1} onto quad (corners clockwise from top-left);
 * false for degenerate input. */
bool pc_algo_homography_rect_to_quad(const double r[4], const double quad[4][2], pc_algo_homography *out);
/* Approximate linear scale near (x, y): sqrt of the Jacobian determinant. */
double pc_algo_homography_local_scale(const pc_algo_homography *h, double x, double y);

/* Interp::parse: "nearest"/"nearestNeighbor", "bilinear", anything else bicubic. */
pc_algo_interp pc_algo_interp_parse(const char *s);

/* Warps src (its content inside src_rect) by h (source -> document space). The result has src's
 * format with alpha added if it had none; pixels outside the warped quad are transparent. `out`
 * is initialised on success (NOMEM / LIMIT for absurd footprints). */
pc_status pc_algo_warp_surface(const pc_surface *src, pc_rect src_rect, const pc_algo_homography *h,
                               pc_algo_interp interp, pc_surface *out, pc_error *err);

#endif
