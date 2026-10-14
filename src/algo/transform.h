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

/* All entries (divided by m[8]) within 1e-9 of the identity; false when |m[8]| < 1e-12. */
bool pc_algo_homography_is_identity(const pc_algo_homography *h);
/* Is q (TL, TR, BR, BL) a usable Perspective Crop quad: finite, strictly convex with every corner
 * angle above ~0.06 degrees, wound clockwise on screen (y down) and at least 1 px^2 in area? */
bool pc_algo_convex_quad(const double q[4][2]);
/* The output size a Perspective Crop of q takes when none is given: the mean length of the top
 * and bottom edges by the mean length of the left and right edges. */
void pc_algo_quad_size(const double q[4][2], double *w, double *h);
/* The homography taking q (TL, TR, BR, BL) onto the rectangle (0, 0)-(w, h), corner to corner;
 * false when q isn't a convex quad or the size isn't positive and finite. */
bool pc_algo_quad_to_rect(const double q[4][2], double w, double h, pc_algo_homography *out);
/* Interp::parse: "nearest"/"nearestNeighbor", "bilinear", anything else bicubic. */
pc_algo_interp pc_algo_interp_parse(const char *s);

/* Photoshop's Bicubic weights (Keys cubic, a = -0.75) of the four source pixels around a sample
 * `t` (0..1) past the second. */
void pc_algo_transform_bicubic(double t, double out[4]);

/* Warps src (its content inside src_rect) by h (source -> document space). The result has src's
 * format with alpha added if it had none; pixels outside the warped quad are transparent; float
 * colours keep their signed / HDR range (integer formats clip on write). `out` is initialised on
 * success (NOMEM / LIMIT for absurd footprints). */
pc_status pc_algo_warp_surface(const pc_surface *src, pc_rect src_rect, const pc_algo_homography *h,
                               pc_algo_interp interp, pc_surface *out, pc_error *err);
/* pc_algo_warp_surface keeping only the destination pixels inside clip: the work is bounded by
 * clip even when part of the source lies at or beyond the map's horizon (a Perspective Crop of a
 * layer reaching far past the crop quad). */
pc_status pc_algo_warp_surface_clipped(const pc_surface *src, pc_rect src_rect, const pc_algo_homography *h,
                                       pc_algo_interp interp, pc_rect clip, pc_surface *out, pc_error *err);

#endif
