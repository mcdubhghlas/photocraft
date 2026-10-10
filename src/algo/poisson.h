/* Gradient-domain ("Poisson") blending, the core of the Healing Brush and Spot Healing (port of
 * crates/algo/src/poisson.rs).
 *
 * Method: P. Perez, M. Gangnet, A. Blake, "Poisson Image Editing", SIGGRAPH 2003. Seamless cloning
 * with the guidance field v = grad(src) splits as f = src + h, where h is the membrane (harmonic)
 * interpolant of the boundary mismatch dst - src. The Laplace equation is solved on the
 * 4-connected grid by successive over-relaxation, accelerated cascadically (solve on a
 * half-resolution grid, prolong bilinearly as the initial guess, a few SOR sweeps per level).
 *
 * Buffers: interleaved w x h x ch normalised floats (any colour model; channels are solved
 * independently, in parallel on the global pool; results do not depend on the thread count).
 * Masks are w x h bools. Lengths are checked (PC_ERR_INVALID where the reference asserts); sides
 * above PC_ALGO_HEAL_MAX_SIDE, more than 2^32 - 1 pixels or ch outside 1..=PC_ALGO_HEAL_MAX_CH
 * give PC_ERR_LIMIT / PC_ERR_INVALID. Output buffers are newly allocated (free()). */
#ifndef PC_ALGO_POISSON_H
#define PC_ALGO_POISSON_H

#include <stdbool.h>
#include <stddef.h>

#include "base/status.h"
#include "raster/interrupt.h"

/* Largest image side the healing / inpainting / seam functions accept (coordinates stay in i32). */
#define PC_ALGO_HEAL_MAX_SIDE ((size_t)1 << 24)
/* Most channels per pixel they accept. */
#define PC_ALGO_HEAL_MAX_CH ((size_t)16)

/* Checks w x h x ch against the limits above and `len` (the buffer's float count; pass
 * SIZE_MAX to skip the length check). Writes w * h to *npx when non-NULL. Shared by the L8
 * modules. */
pc_status pc_algo_heal_dims(size_t w, size_t h, size_t ch, size_t len, size_t *npx, pc_error *err);

/* Solve Laplace(v) = 0 on `unknown` pixels with Dirichlet values from the known pixels of v
 * (out-of-grid neighbours are ignored, a Neumann boundary). v (w * h) holds the known values on
 * entry and the solution on exit. Stops between sweeps when ctl is cancelled (v then holds a
 * partial solution; PC_ERR_CANCELLED is NOT returned here, callers check ctl). A coarsest-level
 * solve over more than 2^24 unknown pixels (a degenerate strip) is PC_ERR_LIMIT. */
pc_status pc_algo_poisson_solve_membrane(size_t w, size_t h, const bool *unknown, float *v,
                                         const pc_interrupt *ctl, pc_error *err);

/* Reference solver for tests: plain Gauss-Seidel from a zero start until the update is below
 * tol (at most max_iter sweeps). */
pc_status pc_algo_poisson_solve_membrane_reference(size_t w, size_t h, const bool *unknown, float *v,
                                                   float tol, size_t max_iter, pc_error *err);

/* Seamless cloning: *out (w * h * ch) is dst outside `mask` and, inside, src plus the membrane
 * that removes the boundary mismatch. src and dst hold `len` floats (must be w * h * ch). */
pc_status pc_algo_poisson_seamless_clone(size_t w, size_t h, size_t ch, const float *src, const float *dst,
                                         size_t len, const bool *mask, size_t mask_len, float **out,
                                         pc_error *err);

/* Membrane fill (no source texture): harmonic interpolation of the known pixels into `hole`.
 * ctl may be NULL; once cancelled the solve stops at the next sweep and PC_ERR_CANCELLED is
 * returned (membrane_fill_with). */
pc_status pc_algo_poisson_membrane_fill(size_t w, size_t h, size_t ch, const float *img, size_t len,
                                        const bool *hole, size_t hole_len, const pc_interrupt *ctl,
                                        float **out, pc_error *err);

#endif
