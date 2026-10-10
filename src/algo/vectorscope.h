/* Bounded hue/saturation distribution for the Vectorscope panel (port of
 * crates/algo/src/vectorscope.rs). Input colours must already be in the chosen RGB space;
 * display orientation and colour management belong to callers. Pure; thread-safe. */
#ifndef PC_ALGO_VECTORSCOPE_H
#define PC_ALGO_VECTORSCOPE_H

#include <stddef.h>
#include <stdint.h>

#define PC_ALGO_VECTORSCOPE_HUES 128
#define PC_ALGO_VECTORSCOPE_SATURATIONS 64

typedef struct pc_algo_vectorscope {
    /* counts[hue * SATURATIONS + saturation], saturating at UINT32_MAX */
    uint32_t counts[PC_ALGO_VECTORSCOPE_HUES * PC_ALGO_VECTORSCOPE_SATURATIONS];
    uint64_t samples;
} pc_algo_vectorscope;

/* Fills *out from n straight RGBA pixels; transparent (alpha <= 0) and non-finite pixels are
 * skipped, colour channels are clamped to 0..1. */
void pc_algo_vectorscope_from_rgba(const float (*px)[4], size_t n, pc_algo_vectorscope *out);

/* Hue in turns [0, 1) and saturation in 0..1; achromatic or black pixels give (0, 0). */
void pc_algo_vectorscope_hue_saturation(const float rgb[3], float *hue, float *saturation);

#endif
