/* High-quality separable resampling of surfaces: Image Size (port of crates/algo/src/resample.rs).
 *
 * Output pixel o maps to source u = (o + 0.5) / scale - 0.5 (rounded to 1/128 pixel for Bilinear
 * and the bicubics, as Photoshop does). Bicubic, Bicubic Smoother and Sharper are Photoshop's Keys
 * cubics measured on Photoshop 25.4 and run as two passes (the smaller scale first, the image
 * stored in its own format, i.e. clipped and rounded, between them); Bicubic Automatic picks
 * Smoother / Sharper per axis; Bilinear below 1/2 averages whole pixels; Lanczos-3 widens by
 * 1/scale; Preserve Details is Lanczos plus a light Unsharp Mask enlarging and Bicubic Sharper
 * reducing. Taps outside the content read the default pixel, and premultiplied samples stream
 * through in bounded reads (64 MB of floats) in bands of 64 output rows, a few bands at a time in
 * parallel, so memory stays bounded whatever the scale.
 *
 * Failure: NOMEM; PC_ERR_LIMIT when a scale is infinite or so extreme that one output pixel would
 * need more than 2^26 taps, or the output exceeds the surface limits (the reference runs out of
 * memory); PC_ERR_UNSUPPORTED for Preserve Details enlargements while the Unsharp Mask filter
 * (chunk L5) is not registered. NaN, zero and negative scales give an empty surface, as in the
 * reference. Output is independent of the thread count. */
#ifndef PC_ALGO_RESAMPLE_H
#define PC_ALGO_RESAMPLE_H

#include <stddef.h>
#include <stdint.h>

#include "algo/params.h"
#include "base/status.h"
#include "geom/geom.h"
#include "raster/surface.h"

/* Samples one source read may hold (64 MB of f32). */
#define PC_ALGO_RESAMPLE_READ_BUDGET ((size_t)1 << 24)

/* The output rect for r scaled by (sx, sy) about the origin (floor / ceil, saturating). */
pc_rect pc_algo_scaled_rect(pc_rect r, double sx, double sy);

/* Resizes a surface by (sx, sy) about the document origin, keeping its default pixel. `out` is
 * initialised on success. */
pc_status pc_algo_resize_surface(const pc_surface *s, double sx, double sy, pc_algo_resample filter,
                                 pc_surface *out, pc_error *err);
/* The same for a layer, mask or channel of a canvas: pixels repeat outwards at the canvas edges
 * (content that reaches an edge keeps it). */
pc_status pc_algo_resize_surface_in_canvas(const pc_surface *s, double sx, double sy, pc_algo_resample filter,
                                           pc_rect canvas, pc_surface *out, pc_error *err);
/* resize_with_budget: explicit read budget in samples and canvas (NULL = none, as
 * pc_algo_resize_surface); for tests. */
pc_status pc_algo_resize_with_budget(const pc_surface *s, double sx, double sy, pc_algo_resample filter,
                                     const pc_rect *canvas, size_t budget, pc_surface *out, pc_error *err);

/* Moves a surface by whole pixels (default pixel kept). */
pc_status pc_algo_translate_surface(const pc_surface *s, int32_t dx, int32_t dy, pc_surface *out,
                                    pc_error *err);
/* Keeps only the pixels inside `keep` (others become the default pixel). */
pc_status pc_algo_crop_surface(const pc_surface *s, pc_rect keep, pc_surface *out, pc_error *err);

/* One resampling tap set (exposed for tests): output sample k reads source samples start ..
 * start + len - 1 with weights w[0..len), plus `outside` x the default pixel. */
typedef struct pc_algo_taps {
    int32_t *start;
    size_t *off; /* offset of each tap set in w */
    size_t *len;
    float *outside;
    float *w;
    size_t n; /* output samples */
} pc_algo_taps;

/* The filter of one axis (exposed for tests). */
typedef enum pc_algo_rs_kernel_kind {
    PC_ALGO_KERNEL_NEAREST,
    PC_ALGO_KERNEL_BILINEAR, /* never widened; below 1/2 the plain average of the covered pixels */
    PC_ALGO_KERNEL_KEYS,     /* the Keys cubic with parameter a, stretched over `stretch` pixels */
    PC_ALGO_KERNEL_LANCZOS   /* Lanczos-3, widened by 1/scale when reducing */
} pc_algo_rs_kernel_kind;
typedef struct pc_algo_rs_kernel {
    pc_algo_rs_kernel_kind kind;
    double a, stretch;
} pc_algo_rs_kernel;

/* Photoshop's bicubic kernel for `method` (Bicubic / Smoother / Sharper; others give Bicubic)
 * designed for scale s. */
pc_algo_rs_kernel pc_algo_resample_keys(pc_algo_resample method, double s);

/* Taps for outputs [dst0, dst1) at `scale` (a scale of 1 copies); edge (or NULL) clamps tap
 * indices to [edge[0], edge[1]]; taps outside content [content[0], content[1]] fold into
 * `outside`. PC_ERR_LIMIT past 2^24 taps per sample. */
pc_status pc_algo_taps_make(int32_t dst0, int32_t dst1, double scale, pc_algo_rs_kernel kernel,
                            const int32_t *edge, const int32_t content[2], pc_algo_taps *out, pc_error *err);
void pc_algo_taps_fini(pc_algo_taps *t);
/* The horizontal pass of one premultiplied source row (row spans x_lo.. with n channels, row_len
 * floats) into out (t->n * n floats, zeroed by the caller); def holds n default samples. */
void pc_algo_resample_horizontal(const float *row, size_t row_len, float *out, const pc_algo_taps *t,
                                 int32_t x_lo, const float *def, size_t n);

#endif
