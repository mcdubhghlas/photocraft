/* Filter > Other > Color to Alpha (port of crates/algo/src/color_to_alpha.rs). */
#ifndef PC_ALGO_COLOR_TO_ALPHA_H
#define PC_ALGO_COLOR_TO_ALPHA_H

#include <stddef.h>

/* The opacity a (0..1) a pixel's n colour channels need to be a mix of some in-range colour and
 * `key`. Out-of-range samples (HDR beyond what key can reach) need full opacity; non-finite
 * per-channel results are ignored. */
float pc_algo_c2a_opacity(const float *colour, const float *key, size_t n);
/* The thresholds: a <= transparency -> 0, a >= opacity -> 1, linear in between. */
float pc_algo_c2a_remap(float a, float transparency, float opacity);
/* Unmixes key from one pixel's n colour channels in place; returns the factor its alpha is
 * multiplied by. Fully opaque results keep the colour; fully transparent ones take the key. */
float pc_algo_c2a_unmix(float *colour, const float *key, size_t n, float transparency,
                        float opacity_threshold);

#endif
