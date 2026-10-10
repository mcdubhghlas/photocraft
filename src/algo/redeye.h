/* Click-local red-eye correction: neutralise a red pupil around a click (port of
 * crates/algo/src/redeye.rs).
 *
 * The kernel never scans a full document: callers pass the search window plus a read halo
 * (pc_algo_redeye_halo_radius) that covers a 1 px morphological shrink and a Gaussian of
 * sigma PC_ALGO_REDEYE_SIGMA. Pure; thread-safe. */
#ifndef PC_ALGO_REDEYE_H
#define PC_ALGO_REDEYE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"

/* Gaussian sigma applied to the pupil mask after a 1 px shrink. */
#define PC_ALGO_REDEYE_SIGMA 0.8f
/* Largest window (search + halo) the engine may allocate, in pixels on a side. */
#define PC_ALGO_REDEYE_MAX_WINDOW 512u
/* Clicks one command may apply in a single history step. */
#define PC_ALGO_REDEYE_MAX_POINTS 64u

/* How far outside the search window the mask filter reads: 1 (erode) + ceil(3 sigma) + 1. */
int32_t pc_algo_redeye_halo_radius(void);
/* Search radius in document pixels for a Pupil Size in 1..=100 (non-finite: 50). */
float pc_algo_redeye_search_radius(float pupil_size);
/* Side of the read rect (search + halo) before clamping Pupil Size; false when pupil_size is
 * non-finite or the side would exceed PC_ALGO_REDEYE_MAX_WINDOW. */
bool pc_algo_redeye_unclamped_window_side(float pupil_size, uint32_t *side);

/* Correct red-eye in px (row-major straight RGBA, w x h including the halo border; n pixels).
 * origin is the document coordinate of px[0]. *changed receives how many pixels changed (0 when
 * n < w * h or the window is empty). Pixels farther than the search radius from click stay. Only
 * PC_ERR_NOMEM can fail. */
pc_status pc_algo_redeye_apply(float (*px)[4], size_t n, size_t w, size_t h, int32_t click_x, int32_t click_y,
                               int32_t origin_x, int32_t origin_y, float pupil_size, float darken,
                               uint32_t *changed, pc_error *err);

#endif
