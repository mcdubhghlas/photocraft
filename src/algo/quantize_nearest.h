/* Exact nearest-colour search for quantize (port of crates/algo/src/quantize/nearest.rs).
 *
 * Balanced median splits (a k-d tree kept in one array) bound the recursion to O(log n); palettes
 * of 64 entries or fewer, or with a non-finite entry, use a linear scan. Results equal a linear
 * scan with strict `<` (lowest index wins a tie; non-finite queries find index 0). Internal to
 * src/algo; a built lookup is read-only and safe to share between threads. */
#ifndef PC_ALGO_QUANTIZE_NEAREST_H
#define PC_ALGO_QUANTIZE_NEAREST_H

#include <stdbool.h>
#include <stddef.h>

#include "base/status.h"

typedef struct pc_algo_nearest_entry {
    float point[3];
    size_t index;
    int axis;
} pc_algo_nearest_entry;

typedef struct pc_algo_nearest {
    pc_algo_nearest_entry *entries;
    size_t n;
    bool tree;
} pc_algo_nearest;

/* Builds the lookup over `n` points (copied). PC_ERR_NOMEM only. */
pc_status pc_algo_nearest_init(pc_algo_nearest *lk, const float (*points)[3], size_t n);
void pc_algo_nearest_fini(pc_algo_nearest *lk);
/* Index of the nearest point (squared Euclidean in f32); 0 when there is none. */
size_t pc_algo_nearest_find(const pc_algo_nearest *lk, const float point[3]);

#endif
