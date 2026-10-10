/* Image registration for Edit > Auto-Align Layers: corners, binary descriptors, matching and
 * robust model fitting (port of crates/algo/src/features.rs).
 *
 *  - Corners: Harris & Stephens (1988) structure tensor det - 0.04 trace^2 over a two-pass box
 *    window, 3x3 non-maximum suppression and a minimum spacing.
 *  - Descriptors: steered BRIEF (Rublee et al., ORB, ICCV 2011): 256 comparisons on a smoothed
 *    31x31 patch, rotated by the intensity-centroid orientation.
 *  - Matching: brute-force Hamming with Lowe's ratio test and a mutual-nearest check.
 *  - Fitting: RANSAC over translation / similarity / homography (normalised DLT), refit to all
 *    inliers.
 *
 * Images are w x h luminance (row-major floats, normally 0..1). Everything is deterministic: the
 * BRIEF pairs and RANSAC samples come from fixed-seed SplitMix64 streams reproduced bit for bit.
 * Lists returned through `**out` are malloc'd (caller frees with free(); NULL when empty).
 * Errors: PC_ERR_INVALID for buffers shorter than w * h, NOMEM / LIMIT. Thread-safe. */
#ifndef PC_ALGO_FEATURES_H
#define PC_ALGO_FEATURES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "algo/transform.h"
#include "base/status.h"

/* Patch radius used by descriptors (corners closer than this to the border are dropped). */
#define PC_ALGO_FEATURES_PATCH_RADIUS 15

typedef struct pc_algo_corner {
    float x, y, score;
} pc_algo_corner;

/* A 256-bit descriptor. */
typedef struct pc_algo_descriptor {
    uint64_t bits[4];
} pc_algo_descriptor;

typedef enum pc_algo_model {
    PC_ALGO_MODEL_TRANSLATION,
    PC_ALGO_MODEL_SIMILARITY, /* rotation, uniform scale and translation */
    PC_ALGO_MODEL_HOMOGRAPHY
} pc_algo_model;

/* (index in a, index in b). */
typedef struct pc_algo_match {
    size_t a, b;
} pc_algo_match;

/* Harris corners: at most `max` (at least one when any qualify, as in the reference) corners at
 * least min_dist px apart, strongest first, only where `valid` (w * h flags, or NULL) holds over
 * the descriptor patch. Images smaller than 33 x 33 have none. */
pc_status pc_algo_features_harris(size_t w, size_t h, const float *img, size_t max, float min_dist,
                                  const bool *valid, pc_algo_corner **out, size_t *count, pc_error *err);

/* Steered-BRIEF descriptors, one per corner, in order (orientation from the intensity centroid
 * unless `upright`). */
pc_status pc_algo_features_describe(size_t w, size_t h, const float *img, const pc_algo_corner *corners,
                                    size_t n, bool upright, pc_algo_descriptor **out, pc_error *err);

uint32_t pc_algo_features_hamming(const pc_algo_descriptor *a, const pc_algo_descriptor *b);

/* Matches passing the ratio test (best distance <= 80 and <= ratio x second best) and the
 * mutual-nearest check, in order of a. */
pc_status pc_algo_features_match(const pc_algo_descriptor *a, size_t na, const pc_algo_descriptor *b,
                                 size_t nb, float ratio, pc_algo_match **out, size_t *count, pc_error *err);

/* Least-squares fit of a model mapping src[i] -> dst[i] (exact for minimal sets). False when
 * there are too few points or the system is degenerate (out untouched). */
bool pc_algo_features_fit(pc_algo_model model, const double (*src)[2], const double (*dst)[2], size_t n,
                          pc_algo_homography *out);

/* RANSAC: the model with the most correspondences within `threshold` px over max(iters, 1)
 * random minimal samples (stream seeded with seed ^ 0xA11C0DE5), refit to its inliers. *found is
 * false when no model was found; otherwise *out and the inlier indices (*inliers, malloc'd,
 * ascending) are set. */
pc_status pc_algo_features_ransac(pc_algo_model model, const double (*src)[2], const double (*dst)[2],
                                  size_t n, size_t iters, double threshold, uint64_t seed, bool *found,
                                  pc_algo_homography *out, size_t **inliers, size_t *ninliers, pc_error *err);

/* Registers `moving` onto `reference` (both w x h; optional validity masks for transparent
 * areas): the model mapping moving coordinates to reference coordinates and its inlier count.
 * *found is false when registration fails (too few corners, matches or inliers). */
pc_status pc_algo_features_register(size_t w, size_t h, const float *reference, const float *moving,
                                    const bool *valid_ref, const bool *valid_mov, pc_algo_model model,
                                    bool *found, pc_algo_homography *out, size_t *ninliers, pc_error *err);

#endif
