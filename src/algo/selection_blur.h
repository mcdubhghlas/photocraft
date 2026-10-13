/* Fast mask blurs behind Select > Modify > Smooth and Feather (port of
 * crates/algo/src/selection_blur.rs). Both are separable and give the direct definitions' numbers
 * to within 1/255 (Smooth: exactly, except where the average is within rounding of 1/2):
 *
 *   - Smooth: a box average of radius round(r) with edge-replicated borders (running sums, O(1)
 *     per pixel), then a hard threshold at 1/2 (as Photoshop: straight edges stay where they
 *     were, corners round off, specks drop);
 *   - Feather: a Gaussian (sigma = radius, as Photoshop's, kernel truncated at ceil(3 sigma) and normalised,
 *     zero outside the canvas); wide kernels run as a sliding five-term cosine series fitted per
 *     call (error checked, falling back to the direct convolution).
 *
 * Work is confined to the mask's non-zero bounding box grown by the radius. Rows and column strips
 * run in parallel on the global pool; results don't depend on the thread count.
 *
 * Both take a w x h mask of `len` samples and return a new buffer of `len` samples (free()). A
 * mask whose len isn't w * h is returned unblurred (Smooth applies the curve), as in the
 * reference; non-finite or absurd radii are capped. PC_ERR_NOMEM on allocation failure. */
#ifndef PC_ALGO_SELECTION_BLUR_H
#define PC_ALGO_SELECTION_BLUR_H

#include <stddef.h>

#include "base/status.h"

pc_status pc_algo_selection_smooth(const float *m, size_t len, size_t w, size_t h, float r, float **out,
                                   pc_error *err);
pc_status pc_algo_selection_feather(const float *m, size_t len, size_t w, size_t h, float radius, float **out,
                                    pc_error *err);

#endif
