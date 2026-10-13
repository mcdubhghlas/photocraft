/* Edge refinement ("Select and Mask" / Refine Edge) and colour decontamination (port of
 * crates/algo/src/matting.rs).
 *
 * Soft edges come from the guided filter (He, Sun & Tang, ECCV 2010 / PAMI 2013) inside a band of
 * `radius` pixels around the selection edge ("smart radius" adapts it to the transition width in
 * the image); then, in Photoshop's order, Smooth, Feather (Gaussian, sigma = feather / 2),
 * Contrast and Shift Edge (grey dilation / erosion). Decontaminate Colors replaces fringe colours
 * by the normalised-convolution average of nearby fully selected pixels.
 *
 * Work is tiled (256 px): only tiles near the selection boundary are processed, on the global
 * pool; results don't depend on the thread count.
 *
 * Buffers are w x h row-major floats. Functions returning `float **out` allocate it (free());
 * inputs must hold w * h values. Failure: PC_ERR_NOMEM, PC_ERR_LIMIT (sizes past PC_MAX_ALLOC,
 * Gaussian kernels past 2^24 taps), or the sampler's / reader's error. */
#ifndef PC_ALGO_MATTING_H
#define PC_ALGO_MATTING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "algo/params.h"
#include "algo/segment.h"
#include "algo/selection.h"
#include "base/status.h"
#include "geom/geom.h"
#include "raster/surface.h"

/* Select and Mask parameters (Photoshop's units). */
typedef struct pc_algo_refine_params {
    float radius;      /* edge-detection band radius in px (0 = no edge detection) */
    bool smart_radius; /* adapt the band to the edge's observed softness */
    float smooth;      /* 0..100 */
    float feather;     /* px */
    float contrast;    /* 0..100 */
    float shift_edge;  /* -100..100: 100% moves the edge by max(radius, 4) px; fractions of a pixel
                        * blend the two neighbouring whole-pixel shifts */
} pc_algo_refine_params;

/* The serde field table (camelCase, every field #[serde(default)]) for pc_algo_jfields_*. */
extern const pc_algo_jfield pc_algo_refine_params_fields[];
extern const size_t pc_algo_refine_params_nfields;

pc_algo_refine_params pc_algo_refine_params_default(void); /* all zero */
/* Pixels read around an output pixel, and how far the refined mask can extend. */
int32_t pc_algo_refine_halo(const pc_algo_refine_params *p);
int32_t pc_algo_refine_spread(const pc_algo_refine_params *p);

/* Guided-filter regulariser (colour guide, samples in 0..1). */
#define PC_ALGO_MATTING_EPS 1e-4f

/* Box mean with radius r (window clipped to the image, divided by the pixel count). */
pc_status pc_algo_matting_box_mean(const float *src, size_t w, size_t h, size_t r, float **out,
                                   pc_error *err);
pc_status pc_algo_matting_guided_filter_gray(const float *guide, const float *p, size_t w, size_t h, size_t r,
                                             float eps, float **out, pc_error *err);
/* Colour guide (He et al., Eq. 19-21); p holds guide->w * guide->h values. */
pc_status pc_algo_matting_guided_filter_color(const pc_algo_rgb_image *guide, const float *p, size_t r,
                                              float eps, float **out, pc_error *err);
/* Separable Gaussian blur (edge-clamped); sigma below 0.1 copies. */
pc_status pc_algo_matting_gaussian_blur(const float *src, size_t w, size_t h, float sigma, float **out,
                                        pc_error *err);
/* Grey dilation (grow) or erosion with the octagonal element of r alternating 3x3 square / cross
 * steps (matting_morph.c): O(w h log r) for masks without NaNs or negative zeros, which keep the
 * step by step order of pc_algo_matting_morph_repeated (the same values otherwise). */
pc_status pc_algo_matting_morph(const float *src, size_t w, size_t h, size_t r, bool grow, float **out,
                                pc_error *err);
pc_status pc_algo_matting_morph_repeated(const float *src, size_t w, size_t h, size_t r, bool grow,
                                         float **out, pc_error *err);
/* Per-pixel transition width (px) of the guide's luminance over a window of radius r. */
pc_status pc_algo_matting_edge_width(const pc_algo_rgb_image *img, size_t r, float **out, pc_error *err);
/* Running max / min over a square window of radius r (edge-clipped). */
pc_status pc_algo_matting_max_square(const float *src, size_t w, size_t h, size_t r, bool max, float **out,
                                     pc_error *err);

/* Refines a soft mask (w x h) with img as guide (needed when radius > 0; NULL skips that). */
pc_status pc_algo_matting_refine_buffer(const pc_algo_rgb_image *img, const float *m, size_t w, size_t h,
                                        const pc_algo_refine_params *p, float **out, pc_error *err);

/* Fills out[0..n) (n = r.w * r.h, zeroed) with mask coverage over r, row-major. Called
 * concurrently from pool threads. */
typedef pc_status (*pc_algo_mask_fn)(const void *ctx, pc_rect r, float *out, size_t n, pc_error *err);
/* Readers: ctx is a const pc_algo_region * (zero outside its box) / a const pc_surface *. */
pc_status pc_algo_matting_region_reader(const void *ctx, pc_rect r, float *out, size_t n, pc_error *err);
pc_status pc_algo_matting_surface_reader(const void *ctx, pc_rect r, float *out, size_t n, pc_error *err);

/* Refines a selection mask: `content` bounds its non-zero pixels, the result is clipped to
 * canvas; the sampler is read only when radius > 0. None when nothing remains selected. */
pc_status pc_algo_matting_refine_mask(const pc_algo_sampler *s, pc_algo_mask_fn mask, const void *mask_ctx,
                                      pc_rect content, pc_rect canvas, const pc_algo_refine_params *p,
                                      pc_algo_region *out, pc_error *err);
/* ---- Refine Edge Brush (matting/brush.rs) ----
 *
 * Painting (add) marks an area as unknown: its coverage is estimated from the image (each colour
 * projected on the line between the mean known foreground and background colours nearby, the
 * colour-line model of Levin, Lischinski & Weiss, CVPR 2006), so soft detail beyond the
 * edge-detection band is picked up. Painting with add false (Photoshop's Alt) keeps the original
 * edge there. Strokes apply in order. */

/* One stroke in document pixels (a single point paints a dab); points are borrowed. */
typedef struct pc_algo_edge_stroke {
    const float (*points)[2];
    size_t npoints;
    float size; /* brush diameter, clamped to PC_ALGO_EDGE_BRUSH_SIZE_MIN..MAX */
    bool add;
} pc_algo_edge_stroke;

#define PC_ALGO_EDGE_BRUSH_SIZE_MIN 1.0f
#define PC_ALGO_EDGE_BRUSH_SIZE_MAX 5000.0f
/* Most pixels the strokes of one brush may sweep (sum of length x size + size^2 per stroke). */
#define PC_ALGO_EDGE_BRUSH_MAX_SWEPT_AREA 268435456.0

/* The painted area: signed coverage (+1 refined, -1 restored) x 127 over bbox. Owned:
 * pc_algo_edge_brush_fini. */
typedef struct pc_algo_edge_brush {
    pc_rect bbox;
    int8_t *v;
    size_t len;
    pc_rect added; /* bounds of the added (positive) coverage */
    size_t window; /* box radius of the colour estimate (it reads up to 4x as far) */
} pc_algo_edge_brush;

/* The area the strokes sweep; non-finite strokes count as infinite. */
double pc_algo_edge_brush_swept_area(const pc_algo_edge_stroke *strokes, size_t n);
/* Rasterises strokes clipped to canvas. *some false (out untouched) when they paint nothing or
 * sweep more than PC_ALGO_EDGE_BRUSH_MAX_SWEPT_AREA. */
pc_status pc_algo_edge_brush_new(const pc_algo_edge_stroke *strokes, size_t n, pc_rect canvas,
                                 pc_algo_edge_brush *out, bool *some, pc_error *err);
void pc_algo_edge_brush_fini(pc_algo_edge_brush *b); /* NULL-safe */
/* Signed coverage at a document pixel (0 outside the painted area). */
float pc_algo_edge_brush_at(const pc_algo_edge_brush *b, int32_t x, int32_t y);
/* Signed coverage over r, row-major, into out[0..n). */
void pc_algo_edge_brush_read(const pc_algo_edge_brush *b, pc_rect r, float *out, size_t n);
/* Coverage estimated from img where brush (signed, w * h) adds; elsewhere (and where no known
 * foreground / background is within window, or 4 x window, px) m. out: w * h (free()). */
pc_status pc_algo_matting_brush_estimate(const pc_algo_rgb_image *img, const float *m, const float *brush,
                                         size_t w, size_t h, size_t window, float **out, pc_error *err);
/* refine_buffer with a brush buffer (signed, w * h; NULL: none) and its window. */
pc_status pc_algo_matting_refine_buffer_brushed(const pc_algo_rgb_image *img, const float *m, size_t w,
                                                size_t h, const pc_algo_refine_params *p, const float *brush,
                                                size_t window, float **out, pc_error *err);
/* refine_mask with the Refine Edge Brush (NULL: none): where it adds, coverage comes from the
 * image (the area can reach beyond the band, the sampler is read whatever the radius); where it
 * restores, the original edge stays. */
pc_status pc_algo_matting_refine_mask_brushed(const pc_algo_sampler *s, pc_algo_mask_fn mask,
                                              const void *mask_ctx, pc_rect content, pc_rect canvas,
                                              const pc_algo_refine_params *p, const pc_algo_edge_brush *brush,
                                              pc_algo_region *out, pc_error *err);

/* GRAY8 surface (default 0) holding a region's coverage. `out` initialised on success. */
pc_status pc_algo_matting_region_surface(const pc_algo_region *reg, pc_surface *out, pc_error *err);
/* Decontaminates colours in the soft fringe of alpha (any model / depth). `out` initialised on
 * success. */
pc_status pc_algo_matting_decontaminate(const pc_surface *src, const pc_algo_region *alpha, float radius,
                                        float amount, pc_surface *out, pc_error *err);
/* src's pixels with alpha multiplied by the region (zero outside its box); the result always has
 * an alpha channel. */
pc_status pc_algo_matting_masked_copy(const pc_surface *src, const pc_algo_region *alpha, pc_surface *out,
                                      pc_error *err);

#endif
