/* Internal interfaces of the L5 filters (blur*.c, motion_*.c, shape_blur.c, sharpen.c, noise.c,
 * denoise.c) shared with their registry (filters_l5.c) and their tests. Not part of the public
 * API: names and signatures may change.
 *
 * Every kernel writes out.w * out.h * src->ch floats to dst (zeroed by the caller) and returns a
 * status instead of panicking. */
#ifndef PC_ALGO_BLUR_INT_H
#define PC_ALGO_BLUR_INT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "algo/blur.h"
#include "algo/filter.h"
#include "algo/image.h"
#include "base/status.h"
#include "raster/interrupt.h"
#include "raster/surface.h"

/* Width in pixels of the column strips the box passes work on (a divisor of the tile size). */
#define PC_ALGO_BOX_STRIP 64
/* Longest motion streak (steps); beyond it the reference would allocate gigabytes of taps. */
#define PC_ALGO_MAX_MOTION_STEPS ((int32_t)1 << 22)
/* Largest radius of the median / surface blur windows (the reference's i32 arithmetic wraps
 * beyond ~46,000, and a window that size never fits in memory). */
#define PC_ALGO_MAX_WINDOW_RADIUS 32768

/* f32::to_radians (x * (PI / 180) in f32, as std does). */
static inline float pc_algo_to_radians(float deg)
{
    return deg * (3.14159265358979323846f / 180.0f);
}

/* slice::partition_point over n elements with Rust's exact binary search (so the index matches
 * even for a predicate that isn't perfectly partitioned). */
size_t pc_algo_partition_point(size_t n, bool (*pred)(const void *ctx, size_t i), const void *ctx);

/* ---- blur.c: Gaussian and Box Blur ---- */

/* blur::box_widths: the odd box widths a Gaussian (radius > 4) or Box Blur (radius > 4) runs as,
 * or 0 boxes when the direct kernel is used. PC_ERR_LIMIT when a box radius exceeds
 * PC_ALGO_MAX_BOX_RADIUS. */
pc_status pc_algo_box_widths(const pc_algo_filter_def *def, const void *params, size_t out[3], size_t *count,
                             pc_error *err);
/* Box passes of an image (edges repeated) for `out` (blur::box_passes). */
pc_status pc_algo_box_passes(const pc_algo_image *src, pc_rect out, bool alpha, const size_t *boxes,
                             size_t nboxes, float *dst, pc_error *err);
pc_status pc_algo_gaussian_k(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx, float radius,
                             float *dst, pc_error *err);
pc_status pc_algo_boxed_k(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx, float radius,
                          float *dst, pc_error *err);
/* The whole-area box-pass path of apply (lib.rs apply_box_blur) for Gaussian / Box Blur. */
pc_status pc_algo_box_blur_apply(const pc_algo_apply_args *a, pc_surface *out, bool *handled, pc_error *err);

/* ---- blur_motion.c: Motion and Radial Blur ---- */

/* The mean of `count` sample points per pixel (blur::average_samples); offsets(ctx, cx, cy, pts,
 * cap) writes up to cap points and returns how many. */
typedef size_t (*pc_algo_offsets_fn)(const void *octx, float cx, float cy, float (*pts)[2], size_t cap);
pc_status pc_algo_average_samples(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx,
                                  pc_algo_offsets_fn offsets, const void *octx, size_t cap, float *dst,
                                  pc_error *err);
/* One merged bilinear tap of the motion kernel (offset and normalised f32 weight). */
typedef struct pc_algo_motion_tap {
    int32_t x, y;
    float w;
} pc_algo_motion_tap;
/* The merged taps of a streak of `distance` at `angle`, in first-seen order (blur::motion and
 * motion_fft::taps), newly allocated (free()). PC_ERR_LIMIT past PC_ALGO_MAX_MOTION_STEPS. */
pc_status pc_algo_motion_taps(float angle, float distance, pc_algo_motion_tap **out, size_t *count,
                              pc_error *err);
/* blur::motion_offsets: (steps + 1) points, newly allocated (free()). */
pc_status pc_algo_motion_offsets(float angle, float distance, float (**pts)[2], size_t *n, pc_error *err);
/* The merged-tap row kernel (blur::motion). */
pc_status pc_algo_motion_k(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx, float angle,
                           float distance, float *dst, pc_error *err);
size_t pc_algo_radial_intervals(float path_length, pc_algo_radial_quality quality);
pc_status pc_algo_radial_k(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx,
                           const pc_algo_radial_params *p, float *dst, pc_error *err);

/* ---- blur_surface.c ---- */

pc_status pc_algo_surface_blur_k(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx, float radius,
                                 float threshold, float *dst, pc_error *err);
/* The three strategies, for the oracle tests. *declined is set (and dst untouched) when the
 * strategy doesn't apply (8-bit: a sample isn't k/255; ranked: a sample isn't finite). */
pc_status pc_algo_surface_8bit(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx, int32_t r,
                               float t, size_t reach, float *dst, bool *declined, pc_error *err);
pc_status pc_algo_surface_ranked(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx, int32_t r,
                                 float t, float *dst, bool *declined, pc_error *err);
pc_status pc_algo_surface_direct(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx, int32_t r,
                                 float t, float *dst, pc_error *err);

/* ---- motion_fft.c / motion_apply.c: the FFT Motion Blur ---- */

/* Shared FFT plan and kernel spectrum of one apply (opaque). */
typedef struct pc_algo_motion_plan pc_algo_motion_plan;

#define PC_ALGO_MOTION_COORDINATE_LIMIT 16384

bool pc_algo_motion_ordinary_coordinates(pc_rect r);
/* motion_fft::worthwhile; false with *ok false when the estimate overflows (Rust's None). */
bool pc_algo_motion_worthwhile(float angle, float distance, size_t tile, pc_rect area, size_t row_tile,
                               size_t channels, size_t workers, bool *ok);
/* Plan::build: NULL when declined (unsupported geometry, cancelled, or out of memory). `clip`
 * (source, out) drops taps that only read transparency (the small-window oracle), or NULL. */
pc_algo_motion_plan *pc_algo_motion_plan_new(float angle, float distance, size_t tile_w, size_t tile_h,
                                             const pc_rect clip[2], const pc_interrupt *ctl);
void pc_algo_motion_plan_free(pc_algo_motion_plan *p);
bool pc_algo_motion_plan_read_rect(const pc_algo_motion_plan *p, pc_rect out, pc_rect *read);
bool pc_algo_motion_plan_resident_bytes(const pc_algo_motion_plan *p, size_t *out);
bool pc_algo_motion_plan_worker_bytes(const pc_algo_motion_plan *p, size_t channels, size_t tile,
                                      size_t *out);
/* Plan::filter: the convolution of src for `out` (out.w * out.h * src->ch floats into a new buffer,
 * free()); false when declined (unsupported samples, cancelled, out of memory). */
bool pc_algo_motion_plan_filter(const pc_algo_motion_plan *p, const pc_algo_image *src, pc_rect out,
                                bool alpha, const pc_interrupt *ctl, float **data, size_t *len);
/* motion_fft::filter (tests): a plan clipped to src/out, then its filter. */
bool pc_algo_motion_fft_filter(const pc_algo_image *src, pc_rect out, bool alpha, float angle, float distance,
                               float **data, size_t *len);
/* motion_fft::row_point_for_test: one pixel of the row kernel (src->ch values). */
bool pc_algo_motion_row_point(const pc_algo_image *src, int32_t x, int32_t y, bool alpha, float angle,
                              float distance, float out[5]);
/* motion_apply::group_size. */
bool pc_algo_motion_group_size(size_t resident, size_t metadata, size_t worker, size_t output, size_t tiles,
                               size_t workers, size_t *out);
/* motion_apply::apply_inner as a def apply override (force: skip the crossover, for tests). */
pc_status pc_algo_motion_apply_ex(const pc_algo_apply_args *a, bool force, pc_surface *out, bool *handled,
                                  pc_error *err);
pc_status pc_algo_motion_apply(const pc_algo_apply_args *a, pc_surface *out, bool *handled, pc_error *err);

/* ---- blur2.c: span kernels, Smart Blur, Lens Blur ---- */

/* A binary kernel as horizontal runs (dy, dx0, dx1) (inclusive). */
typedef struct pc_algo_spans {
    int32_t (*rows)[3];
    size_t n;
    double count;
} pc_algo_spans;

typedef bool (*pc_algo_inside_fn)(const void *ictx, float u, float v);
/* Spans::new: rasterises inside(u, v) (u, v in -1..1) over a disc of radius r. */
pc_status pc_algo_spans_new(float r, pc_algo_inside_fn inside, const void *ictx, pc_algo_spans *out,
                            pc_error *err);
void pc_algo_spans_fini(pc_algo_spans *s);
/* blur2::shape_inside as a callback: ictx points at a pc_algo_shape_ctx (the shape and the Star's
 * polygon, computed once by _init). */
typedef struct pc_algo_shape_ctx {
    pc_algo_blur_shape shape;
    float star[10][2];
} pc_algo_shape_ctx;
void pc_algo_shape_ctx_init(pc_algo_shape_ctx *k, pc_algo_blur_shape shape);
bool pc_algo_shape_inside(const void *ictx, float u, float v);
pc_status pc_algo_smart_blur_k(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx,
                               const pc_algo_smart_blur_params *p, float *dst, pc_error *err);
pc_status pc_algo_lens_blur_k(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx,
                              const pc_algo_lens_blur_params *p, float *dst, pc_error *err);
/* blur2's prefix-sum convolution over the whole of src (the scalar oracle of Shape Blur). */
pc_status pc_algo_conv_spans(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx,
                             const pc_algo_spans *spans, float *dst, pc_error *err);

/* ---- selection_blur.c ---- */

/* Whether Feather's kernel for (sigma, r) along lines of n samples is the sliding cosine series. */
bool pc_algo_feather_kernel_is_cos(double sigma, size_t r, size_t n);

/* ---- shape_blur.c ---- */

/* shape_blur::radius: the radius clamped to 0..=1000 (non-finite: 0). */
float pc_algo_shape_blur_radius(float r);
/* Prepared::new / checked_run. A NULL spans rasterises them for this call. PC_ERR_INVALID /
 * PC_ERR_LIMIT where the reference returns None (malformed or oversized input). */
pc_status pc_algo_shape_blur_prepare(const void *params, void **prep, pc_error *err);
void pc_algo_shape_blur_prep_free(void *prep);
pc_status pc_algo_shape_blur_run(const pc_algo_spans *spans, const pc_algo_image *src, pc_rect out,
                                 const pc_algo_ctx *ctx, float *dst, pc_error *err);

/* ---- sharpen.c, noise.c, denoise.c ---- */

pc_status pc_algo_unsharp_k(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx,
                            const pc_algo_unsharp_params *p, float *dst, pc_error *err);
pc_status pc_algo_smart_sharpen_k(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx,
                                  const pc_algo_smart_sharpen_params *p, float *dst, pc_error *err);
pc_status pc_algo_high_pass_k(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx, float radius,
                              float *dst, pc_error *err);
pc_status pc_algo_add_noise_k(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx,
                              const pc_algo_add_noise_params *p, float *dst, pc_error *err);
/* noise::median; has_threshold: Dust & Scratches. */
pc_status pc_algo_median_k(const pc_algo_image *src, pc_rect out, float radius, bool has_threshold,
                           float threshold, float *dst, pc_error *err);
/* The two median strategies (oracle tests); t < 0 means no threshold. */
pc_status pc_algo_median_ranked(const pc_algo_image *src, pc_rect out, int32_t r, bool has_t, float t,
                                float *dst, pc_error *err);
pc_status pc_algo_median_direct(const pc_algo_image *src, pc_rect out, int32_t r, bool has_t, float t,
                                float *dst, pc_error *err);
/* denoise::reach. */
float pc_algo_denoise_reach(void);
pc_status pc_algo_reduce_noise_k(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx,
                                 const pc_algo_reduce_noise_params *p, float *dst, pc_error *err);

#endif
