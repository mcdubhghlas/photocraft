/* File > Automate > Merge to HDR Pro (port of crates/algo/src/hdr.rs): exposure estimation,
 * Ward's median-threshold-bitmap alignment, Debevec-Malik response recovery, the hat-weighted
 * radiance merge with ghost removal, and the four 16/8-bit tone mapping methods (Local
 * Adaptation reuses tone.h's pc_algo_hdr_toning).
 *
 * Pixels are straight RGBA in 0..=1 (display-encoded inputs); radiance is linear. Functions are
 * reentrant; merge's per-row work runs on the global pool and does not depend on the thread
 * count. */
#ifndef PC_ALGO_HDR_H
#define PC_ALGO_HDR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "algo/tone.h"
#include "base/status.h"

/* One exposure: n straight RGBA pixels (borrowed). */
typedef struct pc_algo_hdr_img {
    const float (*px)[4];
    size_t n;
} pc_algo_hdr_img;

/* Relative exposures (linear factors, the darkest = 1) estimated from the images; out has
 * n_imgs slots. */
pc_status pc_algo_hdr_estimate_exposures(const pc_algo_hdr_img *imgs, size_t n_imgs, double *out,
                                         pc_error *err);

/* Translation (dx, dy) aligning grey image img to reference (both w * h), searching up to
 * 2^levels px. */
pc_status pc_algo_hdr_mtb_offset(size_t w, size_t h, const float *reference, const float *img, size_t levels,
                                 int32_t *dx, int32_t *dy, pc_error *err);

/* Sample pixel indices for response recovery (sorted, unique), newly allocated (free()). middle
 * holds w * h pixels. */
pc_status pc_algo_hdr_sample_pixels(size_t w, size_t h, const float (*middle)[4], size_t count, size_t **out,
                                    size_t *n_out, pc_error *err);

/* Debevec-Malik response curve: z[j * n + i] is the code value of sample i in exposure j (p
 * exposures); g receives 256 values (log exposure per code value, g(128) = 0). */
pc_status pc_algo_hdr_response_curve(const uint8_t *z, size_t p, size_t n, const double *log_dt,
                                     double lambda, double g[256], pc_error *err);

typedef struct pc_algo_hdr_merge_opts {
    const double *exposures; /* linear exposure factors, one per image (at least n_imgs) */
    size_t n_exposures;
    bool remove_ghosts;
    bool has_ghost_base; /* false: the image with most well-exposed pixels */
    size_t ghost_base;
    const double (*response)[256]; /* 3 curves, or NULL: recovered from the images */
} pc_algo_hdr_merge_opts;

typedef struct pc_algo_hdr_merged {
    float (*px)[4]; /* w * h linear radiance, straight RGBA (owned) */
    size_t n;
    double response[3][256];
    size_t ghost_base;
    double ghost_fraction; /* fraction of pixels handled as ghosts */
    double stops;          /* dynamic range (1st..99.9th percentile of luminance) */
} pc_algo_hdr_merged;

void pc_algo_hdr_merged_fini(pc_algo_hdr_merged *m); /* NULL-safe */

/* Merges aligned exposures (each w * h pixels) into a radiance map. PC_ERR_INVALID for no
 * images, mismatched sizes or too few exposures. `out` written on success. */
pc_status pc_algo_hdr_merge(size_t w, size_t h, const pc_algo_hdr_img *imgs, size_t n_imgs,
                            const pc_algo_hdr_merge_opts *opts, pc_algo_hdr_merged *out, pc_error *err);

typedef enum pc_algo_hdr_tone_kind {
    PC_ALGO_HDR_LOCAL_ADAPTATION,
    PC_ALGO_HDR_EXPOSURE_GAMMA,
    PC_ALGO_HDR_HIGHLIGHT_COMPRESSION,
    PC_ALGO_HDR_EQUALIZE_HISTOGRAM
} pc_algo_hdr_tone_kind;

typedef struct pc_algo_hdr_tone_method {
    pc_algo_hdr_tone_kind kind;
    pc_algo_hdr_toning_params local; /* LOCAL_ADAPTATION */
    float exposure, gamma;           /* EXPOSURE_GAMMA */
} pc_algo_hdr_tone_method;

/* Tone maps linear radiance (n == w * h pixels) to display-encoded values in 0..=1, in place. */
pc_status pc_algo_hdr_tone_map(float (*px)[4], size_t n, size_t w, size_t h, const pc_algo_hdr_tone_method *m,
                               pc_error *err);

#endif
