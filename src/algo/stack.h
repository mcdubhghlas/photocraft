/* Image stack statistics, Layer > Smart Objects > Stack Mode (port of crates/algo/src/stack.rs):
 * a per-pixel, per-channel statistic across N aligned images, e.g. median to remove passers-by or
 * mean to reduce noise.
 *
 * Inputs are straight RGBA buffers. Only samples with alpha > 0 contribute; the output alpha is
 * the largest input alpha. Moments use the population definitions; skewness and kurtosis are
 * standardised and scaled into 0..=1 for display; entropy is the Shannon entropy (bits) of the
 * 8-bit values over log2(N). Pure; thread-safe. */
#ifndef PC_ALGO_STACK_H
#define PC_ALGO_STACK_H

#include <stdbool.h>
#include <stddef.h>

#include "base/status.h"

/* Stat (serde names "Entropy", "Kurtosis", ...; declaration order). */
typedef enum pc_algo_stack_stat {
    PC_ALGO_STACK_ENTROPY,
    PC_ALGO_STACK_KURTOSIS,
    PC_ALGO_STACK_MAXIMUM,
    PC_ALGO_STACK_MEAN,
    PC_ALGO_STACK_MEDIAN,
    PC_ALGO_STACK_MINIMUM,
    PC_ALGO_STACK_RANGE,
    PC_ALGO_STACK_SKEWNESS,
    PC_ALGO_STACK_STANDARD_DEVIATION,
    PC_ALGO_STACK_SUMMATION,
    PC_ALGO_STACK_VARIANCE
} pc_algo_stack_stat;
#define PC_ALGO_STACK_STAT_COUNT 11

const char *pc_algo_stack_stat_name(pc_algo_stack_stat s); /* NULL for an unknown value */
bool pc_algo_stack_stat_from_name(const char *s, size_t len, pc_algo_stack_stat *out);

/* The statistic of the samples v[0..n) (v may be reordered: the median sorts it). */
float pc_algo_stack_stat_of(float *v, size_t n, pc_algo_stack_stat s);

/* Combines `count` layers (layer k holds lens[k] RGBA pixels) with statistic s: *out holds
 * max(lens) pixels (free()), *out_len that count. Colour results keep their float range (no clamp);
 * Entropy, Skewness and Kurtosis apply their own 0..1 normalisation. */
pc_status pc_algo_stack_combine(const float (*const *layers)[4], const size_t *lens, size_t count,
                                pc_algo_stack_stat s, float (**out)[4], size_t *out_len, pc_error *err);

#endif
