/* Small helpers of chunk L9 (Camera Raw, HDR, lens, wide angle): Rust's total_cmp orderings and
 * checked float-buffer allocation. photo_util.h has the rest of photo_util.rs. Pure functions;
 * thread-safe. */
#ifndef PC_ALGO_L9UTIL_H
#define PC_ALGO_L9UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"

/* Rust's f32/f64 total_cmp: -1, 0 or 1. */
int pc_algo_l9u_total_cmpf(float a, float b);
int pc_algo_l9u_total_cmp(double a, double b);
/* Sorts in total_cmp order (ascending). */
void pc_algo_l9u_sortf(float *v, size_t n);
void pc_algo_l9u_sort(double *v, size_t n);

/* malloc of n floats / doubles, PC_ERR_LIMIT past PC_MAX_ALLOC, PC_ERR_NOMEM on failure.
 * `zero` clears the buffer. *out is written on success only. */
pc_status pc_algo_l9u_alloc_f(size_t n, bool zero, float **out, pc_error *err);
pc_status pc_algo_l9u_alloc_d(size_t n, bool zero, double **out, pc_error *err);

#endif
