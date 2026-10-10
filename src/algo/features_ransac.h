/* Internal: the RANSAC loop shared by features::ransac and panorama::ransac_motion (the two
 * reference copies differ only in the fit function, the stream seed and whether a minimal set
 * (n == k) still draws one random index per iteration). Not part of the public algo API. */
#ifndef PC_ALGO_FEATURES_RANSAC_H
#define PC_ALGO_FEATURES_RANSAC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "algo/transform.h"
#include "base/status.h"

typedef bool (*pc_algo_fit_fn)(void *ctx, const double (*src)[2], const double (*dst)[2], size_t n,
                               pc_algo_homography *out);

/* k points per sample; `stream` is the SplitMix64 state (seed already mixed). Outputs as
 * pc_algo_features_ransac. */
pc_status pc_algo_ransac_generic(pc_algo_fit_fn fit, void *fit_ctx, size_t k, bool draw_when_minimal,
                                 const double (*src)[2], const double (*dst)[2], size_t n, size_t iters,
                                 double threshold, uint64_t stream, bool *found, pc_algo_homography *out,
                                 size_t **inliers, size_t *ninliers, pc_error *err);

#endif
