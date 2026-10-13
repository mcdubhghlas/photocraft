/* The Remove Tool: fill what a brush stroke painted over from its surroundings (port of
 * crates/algo/src/remove.rs).
 *
 *  - pc_algo_remove_close_loops: a stroke drawn around an object also removes what it encloses.
 *  - pc_algo_remove_dilate: the hole grows by a few pixels so an object's soft edge goes with it.
 *  - pc_algo_remove: the hole is completed by the non-local completion (nonlocal.h); sparse holes
 *    (long strokes, scattered marks) tile by tile.
 *  - pc_algo_remove_hole_radius: how thick a hole is (sets how many scales the completion uses).
 *
 * Buffers are interleaved w x h x ch normalised floats (any model/depth) with w x h masks;
 * deterministic; outputs newly allocated (free()). */
#ifndef PC_ALGO_REMOVE_H
#define PC_ALGO_REMOVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "raster/interrupt.h"

/* mask plus everything it encloses (cells from which no 4-connected path of unmasked cells reaches
 * the border): *out holds len bools. A mask of the wrong length (len != w * h) or an empty grid
 * comes back unchanged. */
pc_status pc_algo_remove_close_loops(size_t w, size_t h, const bool *mask, size_t len, bool **out,
                                     pc_error *err);

/* mask grown by r cells (a square), in two separable passes; unchanged for r == 0 or a mask of the
 * wrong length. *out holds len bools. */
pc_status pc_algo_remove_dilate(size_t w, size_t h, const bool *mask, size_t len, size_t r, bool **out,
                                pc_error *err);

/* The largest chessboard distance from a hole cell to the nearest cell outside the hole (beyond the
 * grid doesn't count as outside). UINT32_MAX when no cell lies outside, 0 for a mask of the wrong
 * size. PC_ERR_NOMEM only. */
pc_status pc_algo_remove_hole_radius(size_t w, size_t h, const bool *hole, size_t len, uint32_t *out,
                                     pc_error *err);

/* Fill `hole` in img from the pixels around it (remove_with): *out is the full buffer, unchanged
 * outside the hole; a buffer or mask of the wrong size, or ch 0, comes back unchanged. With nothing
 * fully known around the hole it is diffused from its edges. ctl may be NULL (PC_ERR_CANCELLED). */
pc_status pc_algo_remove(size_t w, size_t h, size_t ch, const float *img, size_t len, const bool *hole,
                         size_t hole_len, uint64_t seed, const pc_interrupt *ctl, float **out, pc_error *err);

#endif
