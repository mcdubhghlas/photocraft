/* Direct (intensity-based) registration for Edit > Auto-Align Layers, for layers that nearly
 * coincide: a focus bracket, an exposure bracket, a burst (port of crates/algo/src/direct.rs).
 *
 * The target is registered to the reference coarse-to-fine on a Gaussian pyramid of the
 * luminance. The cost of a pose is the RMS of the reference minus the warped target, the mean
 * removed (exposure changes don't count), over the pixels both cover. Each level runs a bounded
 * Nelder-Mead search whose simplex starts one pixel of that level wide and stops at a tenth of
 * one. The pose is resolution independent, so a fit that stops a few levels short of full
 * resolution still applies there. Deterministic and bit-identical to the reference for any thread
 * count (per-row sums folded in row order). */
#ifndef PC_ALGO_DIRECT_H
#define PC_ALGO_DIRECT_H

#include <stdbool.h>
#include <stddef.h>

#include "algo/panorama.h"
#include "algo/transform.h"
#include "base/status.h"

/* A layer's transform onto the reference, about the layer's centre: a similarity (shift as
 * fractions of the width/height, scale, rotation in radians); with aspect (vertical over
 * horizontal scale) and shear an affine transform; with px and py (the divisor
 * 1 + px*u/w + py*v/h over the centred pixel) a projective one. */
typedef struct pc_algo_pose {
    double dx, dy, scale, rot, aspect, shear, px, py;
} pc_algo_pose;

#define PC_ALGO_POSE_N 8
#define PC_ALGO_POSE_IDENTITY ((pc_algo_pose){0.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 0.0})

void pc_algo_pose_to_vec(const pc_algo_pose *p, double out[PC_ALGO_POSE_N]);
pc_algo_pose pc_algo_pose_from_vec(const double v[PC_ALGO_POSE_N]);
/* Which parameters a search over `motion` may move. */
void pc_algo_pose_free(pc_algo_motion motion, bool out[PC_ALGO_POSE_N]);
/* The homography mapping the layer's pixels onto the reference's (w x h layer). */
pc_algo_homography pc_algo_pose_matrix(const pc_algo_pose *p, size_t w, size_t h);

/* A luminance plane (w * h values) with, where the layer does not cover its whole area, its
 * coverage (NULL: covered everywhere). */
typedef struct pc_algo_direct_plane {
    size_t w, h;
    const float *luma;
    const bool *valid;
} pc_algo_direct_plane;

typedef struct pc_algo_registration {
    pc_algo_pose pose;
    pc_algo_homography h; /* the pose as a homography of the target's pixels onto the reference's */
    double rms;           /* the cost at the pose, at the finest level searched */
    size_t overlap;       /* pixels of that level both layers cover at the pose */
    double correlation;   /* Pearson correlation of reference and warped target there (-1..1) */
} pc_algo_registration;

/* Registers target to reference (planes of the same size, both at least 1 x 1) with the
 * parameters motion moves, from init, stopping `coarsen` levels short of full resolution (at
 * least one level is searched). PC_ERR_INVALID on mismatched or empty planes; NOMEM. */
pc_status pc_algo_direct_register(const pc_algo_direct_plane *reference, const pc_algo_direct_plane *target,
                                  pc_algo_motion motion, pc_algo_pose init, size_t coarsen,
                                  pc_algo_registration *out, pc_error *err);

/* src warped by pose into a plane of the same size (cubic B-spline), and the coverage of the
 * result. *out_luma (w*h floats) and *out_valid (w*h bools) are newly allocated. */
pc_status pc_algo_direct_warp(const pc_algo_direct_plane *src, const pc_algo_pose *pose, float **out_luma,
                              bool **out_valid, pc_error *err);

/* Bounded Nelder-Mead (exposed for tests): minimises f over [lo, hi] from x0 (n parameters) with
 * first steps `step` until the vertices agree to 1e-4 relative and lie within tol of the best, or
 * 200 iterations. out receives n values. False on NOMEM. */
typedef double (*pc_algo_nm_fn)(void *ctx, const double *x);
bool pc_algo_nelder_mead(pc_algo_nm_fn f, void *ctx, size_t n, const double *x0, const double *lo,
                         const double *hi, const double *step, const double *tol, double *out);

#endif
