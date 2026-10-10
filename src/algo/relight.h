/* Filter > Render > Relight (port of crates/algo/src/relight.rs): recover shading with a blur
 * (Retinex) and replace it with a Lambertian lamp whose direction comes from angle / elevation.
 *
 * The shading blur's sigma depends on the document size, so Relight is the one filter whose halo
 * depends on the reference bounds (halo_for). Large output areas are processed in overlapping
 * bands so the luminance field stays under PC_ALGO_RELIGHT_MAX_LUMA_PIXELS.
 *
 * The def is registered in filters_l9.c; callers use pc_algo_filter_make(&PC_ALGO_FILTER_RELIGHT,
 * &params, ...) or the "relight" JSON tag. Pure functions; thread-safe. */
#ifndef PC_ALGO_RELIGHT_H
#define PC_ALGO_RELIGHT_H

#include <stdint.h>

#include "algo/filter.h"
#include "geom/geom.h"

/* Softness maps onto this sigma range; the halo is derived from the sigma actually used. */
#define PC_ALGO_RELIGHT_SIGMA_MIN 2.0f
#define PC_ALGO_RELIGHT_SIGMA_MAX 96.0f
/* Luma field allocated for one kernel call; larger areas are split into overlapping bands. */
#define PC_ALGO_RELIGHT_MAX_LUMA_PIXELS ((size_t)8000000)

/* FilterParams::Relight, dialog units: angle -180..180 degrees, elevation 0..90, intensity and
 * ambient 0..100, warmth -100..100, softness 1..100. Intensity 0 is an identity copy. */
typedef struct pc_algo_relight {
    float angle, elevation, intensity, ambient, warmth, softness;
} pc_algo_relight;

extern const pc_algo_filter_def PC_ALGO_FILTER_RELIGHT;

/* Blur sigma from softness and the reference bounds, clamped to the sigma range. */
float pc_algo_relight_sigma(float softness, pc_rect bounds);
/* Finite-difference step (pixels) at this sigma. */
int32_t pc_algo_relight_step(float sigma);
/* Pixels read outside an output tile: 3 sigma + 1 of the blur plus the step. */
int32_t pc_algo_relight_read_radius(float sigma);
/* Halo for a concrete document size (the maximum when bounds is empty). */
int32_t pc_algo_relight_halo_radius(float softness, pc_rect bounds);
/* Conservative halo when the document size is unknown (covers the largest sigma). */
int32_t pc_algo_relight_halo_radius_max(void);

#endif
