/* Bounded RGB histograms of straight, normalised float RGBA samples (port of
 * crates/algo/src/histogram.rs).
 *
 * No colour conversion is performed: bins describe the supplied sample domain. 256 bins are a
 * display resolution, not an input bit depth. Pure functions; thread-safe. */
#ifndef PC_ALGO_HISTOGRAM_H
#define PC_ALGO_HISTOGRAM_H

#include <stddef.h>
#include <stdint.h>

#define PC_ALGO_HIST_BINS 256

typedef struct pc_algo_rgb_histogram {
    uint64_t channels[3][PC_ALGO_HIST_BINS];
    uint64_t samples;
    uint64_t transparent;
    uint64_t invalid;
    /* Finite samples outside 0..=1 are included in the edge bins and counted separately. */
    uint64_t underflow[3];
    uint64_t overflow[3];
    /* Exact endpoints (<= 0 / >= 1) before display-bin rounding. */
    uint64_t shadows[3];
    uint64_t highlights[3];
} pc_algo_rgb_histogram;

/* Each visible pixel counts once, including partially transparent ones; non-finite pixels are
 * rejected as a whole and transparent ones counted apart. `out` is fully overwritten. */
void pc_algo_rgb_histogram_from_rgba(const float (*pixels)[4], size_t n, pc_algo_rgb_histogram *out);

/* Clipped channels of one visible straight RGBA pixel as bit masks (bit 0 = R, 1 = G, 2 = B) at the
 * exact endpoints; transparent and non-finite pixels clip nothing. */
void pc_algo_clipping(const float p[4], uint8_t *shadows, uint8_t *highlights);

#endif
