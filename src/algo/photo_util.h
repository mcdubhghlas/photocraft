/* Numeric helpers shared by the computational-photography modules (panorama, HDR, lens, wide
 * angle, Camera Raw, features): dense and sparse least squares, the SplitMix64 generator, a
 * position hash, the sRGB transfer pair, bilinear sampling and Catmull-Rom weights (port of
 * crates/algo/src/photo_util.rs; prefix pc_algo_photo_). photo_util's par_rows / par_rows2 /
 * par_map are pc_algo_par_for (fxutil.h).
 *
 * Arithmetic follows the reference operation by operation (f64 sums start at -0.0, pivots are
 * the LAST maximum as Iterator::max_by picks), so results are bit-identical. Pure functions
 * unless stated; thread-safe on distinct objects. */
#ifndef PC_ALGO_PHOTO_UTIL_H
#define PC_ALGO_PHOTO_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"

/* One sparse coefficient: coef * x[i]. */
typedef struct pc_algo_photo_term {
    size_t i;
    double c;
} pc_algo_photo_term;

/* solve_dense: solves the n x n system a . x = b (row-major) by Gaussian elimination with partial
 * pivoting. `a` (n * n) and `b` (n) are scratch and are overwritten. Returns false (x untouched)
 * when a pivot is below 1e-14 or the solution is not finite. */
bool pc_algo_photo_solve_dense(size_t n, double *a, double *b, double *x);

/* Normal: accumulates weighted rows into the normal equations A^T A x = A^T b. */
typedef struct pc_algo_photo_normal {
    size_t n;
    double *ata; /* n * n */
    double *atb; /* n */
} pc_algo_photo_normal;

/* Zeroed system with n unknowns (NOMEM / LIMIT). */
pc_status pc_algo_photo_normal_init(pc_algo_photo_normal *ne, size_t n, pc_error *err);
void pc_algo_photo_normal_fini(pc_algo_photo_normal *ne);
/* Adds the sparse row sum(coef * x[idx]) = rhs with weight w (applied squared). Terms whose index
 * is >= n are ignored (the reference would panic). */
void pc_algo_photo_normal_add(pc_algo_photo_normal *ne, const pc_algo_photo_term *row, size_t count,
                              double rhs, double w);
/* Solves the system (destroying ata / atb, as the reference consumes it); x has n entries and is
 * written only on success. */
bool pc_algo_photo_normal_solve(pc_algo_photo_normal *ne, double *x);

/* SparseLs: min sum(w^2 (row . x - rhs)^2) solved by CGLS. */
typedef struct pc_algo_photo_sparse_ls {
    size_t n;
    pc_algo_photo_term *terms; /* all rows' terms, already multiplied by the row weight */
    size_t nterms, terms_cap;
    size_t *row_end; /* row r covers terms [row_end[r-1], row_end[r]) */
    double *rhs;     /* rhs * w per row */
    size_t nrows, rows_cap;
} pc_algo_photo_sparse_ls;

void pc_algo_photo_sparse_ls_init(pc_algo_photo_sparse_ls *ls, size_t n);
void pc_algo_photo_sparse_ls_fini(pc_algo_photo_sparse_ls *ls);
/* Adds a row (NOMEM). Terms with an index >= n are dropped (the reference would panic). */
pc_status pc_algo_photo_sparse_ls_add(pc_algo_photo_sparse_ls *ls, const pc_algo_photo_term *row,
                                      size_t count, double rhs, double w, pc_error *err);
/* CGLS from x0 (n entries), at most `iters` iterations; x (n entries) written on success. */
pc_status pc_algo_photo_sparse_ls_solve(const pc_algo_photo_sparse_ls *ls, const double *x0, size_t iters,
                                        double *x, pc_error *err);

/* Rng: deterministic SplitMix64 (photo_util::Rng and features' private copy, which adds unit). */
typedef struct pc_algo_photo_rng {
    uint64_t s;
} pc_algo_photo_rng;

uint64_t pc_algo_photo_rng_next(pc_algo_photo_rng *r);
/* next() % max(n, 1). */
size_t pc_algo_photo_rng_below(pc_algo_photo_rng *r, size_t n);
/* (next() >> 11) / 2^53, in [0, 1). */
double pc_algo_photo_rng_unit(pc_algo_photo_rng *r);

/* photo_util::hash01: integer hash of (x, y, seed) to [0, 1). Not noise::hash01 (fxutil.h). */
float pc_algo_photo_hash01(int64_t x, int64_t y, uint64_t seed);

/* sRGB transfer functions, extended linearly below 0 and as a power above 1. */
float pc_algo_photo_srgb_to_linear(float v);
float pc_algo_photo_linear_to_srgb(float v);

/* Bilinear sample of channel c of a w x h x ch buffer (len floats), clamped to the edges.
 * 0 for an empty buffer or out-of-range c (the reference panics). */
float pc_algo_photo_bilinear(const float *buf, size_t len, size_t w, size_t h, size_t ch, size_t c, float x,
                             float y);

/* Catmull-Rom weights for a fractional offset t. */
void pc_algo_photo_catmull_rom(float t, float out[4]);

#endif
