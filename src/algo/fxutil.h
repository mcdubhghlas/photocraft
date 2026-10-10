/* Shared filter helpers (port of crates/algo/src/fxutil.rs, plus small helpers other algo modules
 * share: noise::hash01, other2's HSL/HSB conversions, selection::color_range, and the parallel
 * loops of photo_util.rs).
 *
 * All functions are pure or work on caller buffers; thread-safe. Functions that allocate return
 * pc_status (PC_ERR_NOMEM); box blurs refuse radii above PC_ALGO_MAX_BOX_RADIUS (PC_ERR_LIMIT)
 * where the reference would loop for hours. */
#ifndef PC_ALGO_FXUTIL_H
#define PC_ALGO_FXUTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "algo/filter.h"
#include "algo/image.h"
#include "base/pool.h"
#include "base/status.h"
#include "color/color.h"
#include "geom/geom.h"

/* Max channels per pixel kept on the stack (CMYK + alpha + spare). */
#define PC_ALGO_MAXC 8
/* Largest running-sum box radius (a window of 2r+1 samples); beyond it the reference's start-up
 * sum alone would take minutes. */
#define PC_ALGO_MAX_BOX_RADIUS ((size_t)1 << 24)

/* The context's model as an f32 format (for colour conversions). */
pc_pixel_format pc_algo_ctx_format(const pc_algo_ctx *ctx);
/* Colour channels (excluding alpha) of an n-channel pixel. */
static inline size_t pc_algo_ncol(const pc_algo_ctx *ctx, size_t n)
{
    return ctx->alpha && n > 0 ? n - 1 : n;
}
/* Channel values are ink amounts (1 = dark): CMYK. */
static inline bool pc_algo_subtractive(const pc_algo_ctx *ctx)
{
    return ctx->mode == PC_COLOR_MODE_CMYK;
}
/* Per-channel "brightness" operations run on an RGB conversion (Lab's a/b are not intensities). */
static inline bool pc_algo_via_rgb(const pc_algo_ctx *ctx)
{
    return ctx->mode == PC_COLOR_MODE_LAB;
}
/* Document coordinates of the i-th pixel of `out`. */
void pc_algo_xy(pc_rect out, size_t i, int32_t *x, int32_t *y);
/* Straight sRGB RGBA of a native pixel of n channels. */
void pc_algo_rgba(const pc_algo_ctx *ctx, const float *px, size_t n, float out[4]);
/* Writes straight sRGB RGBA into a native pixel of n channels (as many as both hold). */
void pc_algo_set_rgba(const pc_algo_ctx *ctx, float *px, size_t n, const float c[4]);
/* A straight sRGB RGBA colour in the native model (unused slots 0). */
void pc_algo_native(const pc_algo_ctx *ctx, const float c[4], float out[PC_ALGO_MAXC]);
/* Rec. 601 luma of a native pixel. */
float pc_algo_luma(const pc_algo_ctx *ctx, const float *px, size_t n);

/* Interleaved premultiplied copy of src over win (zero outside), newly allocated (free()). */
pc_status pc_algo_premul_window(const pc_algo_image *src, pc_rect win, bool alpha, float **out, size_t *len,
                                pc_error *err);
/* Unpremultiplies one interleaved pixel of n samples in place. */
void pc_algo_unpremul_px(float *px, size_t n, bool alpha);

/* Box blur (radius r both ways, edges clamped, f64 running sums) of a w x h x n buffer. */
pc_status pc_algo_box_blur_n(float *buf, size_t w, size_t h, size_t n, size_t r, pc_error *err);
/* Kovesi's three box radii approximating a Gaussian of sigma (never all zero). */
void pc_algo_gauss_box_radii(float sigma, size_t out[3]);
/* Approximate Gaussian blur (three box passes per direction); no-op below sigma 0.3. */
pc_status pc_algo_gauss_blur_n(float *buf, size_t w, size_t h, size_t n, float sigma, pc_error *err);

/* noise::hash01: a deterministic value in [0, 1) for (x, y, channel, seed). */
float pc_algo_hash01(int32_t x, int32_t y, uint32_t c, uint32_t seed);
/* Jittered feature point of grid cell (cx, cy) (cells of `cell` px anchored at origin). */
void pc_algo_cell_point(int32_t cx, int32_t cy, float cell, uint32_t seed, float ox, float oy, float *px,
                        float *py);
/* smoothstep with a guard for e1 <= e0 (a step at e1). */
float pc_algo_smoothstep(float e0, float e1, float x);
/* Adds the premultiplied bilinear sample of src at (x, y) (pixel centres at .5, edges clamped to
 * clip, which must lie inside src->rect) into acc[0..src->ch). */
void pc_algo_add_sample_premul(const pc_algo_image *src, float x, float y, pc_rect clip, bool alpha,
                               float *acc);

/* other2.rs colour encodings (all components 0..1). */
void pc_algo_rgb_to_hsl(const float c[3], float out[3]);
void pc_algo_hsl_to_rgb(const float c[3], float out[3]);
void pc_algo_hsb_to_rgb(const float c[3], float out[3]);
/* f32::rem_euclid. */
float pc_algo_rem_euclidf(float a, float b);

/* selection::color_range over straight RGBA pixels: coverage 1 at `color`, fading to 0 at
 * fuzziness + 1 levels (max channel difference); transparent pixels 0. mask holds n values. */
void pc_algo_color_range(const float (*px)[4], size_t n, const float color[3], float fuzziness, float *mask);

/* Parallel loop over [0, n) on the global pool in chunks of `grain` (photo_util's par_rows /
 * par_map). Chunks depend only on n and grain, so per-index work is deterministic; the status of
 * the lowest failing chunk is returned. */
pc_status pc_algo_par_for(size_t n, size_t grain, pc_pool_fn fn, void *ctx);

#endif
