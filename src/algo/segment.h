/* Smart selection: Quick Selection, Object Selection, Select Subject, Sky and Focus Area (port of
 * crates/algo/src/segment/: mod.rs, maxflow.rs, gmm.rs, slic.rs, grabcut.rs, quick.rs, sky.rs,
 * subject.rs, focus.rs). Everything is classical and implemented from the published papers:
 * Boykov-Kolmogorov max-flow, GMM colour models (k-means++ seeding, EM), SLIC superpixels,
 * GrabCut, geodesic stroke segmentation, saliency optimisation, Laplacian focus measures.
 *
 * Colour analysis runs on straight RGB samples in 0..=1 read through a pc_algo_sampler, so any
 * depth or colour model can be segmented; results are 8-bit coverage pc_algo_regions (None is an
 * empty region, see selection.h). Large images run at a reduced working resolution and the
 * boundary is re-cut at full resolution in a narrow band, tile by tile on the global pool.
 *
 * Randomness is the reference's xorshift (pc_algo_segment_rng), seeded deterministically, so
 * results are reproducible and identical with any thread count.
 *
 * Failure: PC_ERR_NOMEM / PC_ERR_LIMIT (sizes past PC_MAX_ALLOC), PC_ERR_INVALID for buffers
 * whose length doesn't match the image (the reference would panic), or the sampler's error.
 * Outputs are written only on success. */
#ifndef PC_ALGO_SEGMENT_H
#define PC_ALGO_SEGMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "algo/selection.h"
#include "base/status.h"
#include "geom/geom.h"
#include "raster/surface.h"

typedef struct pc_algo_gmm pc_algo_gmm;

/* ---- RGB images and samplers (mod.rs) ---- */

/* Row-major RGB, samples in 0..=1. Owned (pc_algo_rgb_image_fini); px NULL when empty. */
typedef struct pc_algo_rgb_image {
    size_t w, h;
    float (*px)[3];
} pc_algo_rgb_image;

pc_status pc_algo_rgb_image_init(pc_algo_rgb_image *img, size_t w, size_t h, pc_error *err); /* zeros */
void pc_algo_rgb_image_fini(pc_algo_rgb_image *img);                                         /* NULL-safe */
pc_status pc_algo_rgb_image_clone(const pc_algo_rgb_image *img, pc_algo_rgb_image *out, pc_error *err);
/* Straight RGBA to RGB over mid grey (n must be w * h, else PC_ERR_INVALID). */
pc_status pc_algo_rgb_image_from_rgba(const float (*px)[4], size_t n, size_t w, size_t h,
                                      pc_algo_rgb_image *out, pc_error *err);
/* Box-average downsampling by an integer step (output ceil(w/step) x ceil(h/step)). */
pc_status pc_algo_rgb_image_downsample(const pc_algo_rgb_image *img, size_t step, pc_algo_rgb_image *out,
                                       pc_error *err);

/* Where the algorithms read colours from: fills out[0..n) (n = r.w * r.h, zeroed) with straight
 * RGBA in 0..=1 over r (document pixels), row-major. Called concurrently from pool threads. */
typedef pc_status (*pc_algo_sampler_rgba_fn)(const void *ctx, pc_rect r, float (*out)[4], size_t n,
                                             pc_error *err);

typedef struct pc_algo_sampler {
    pc_algo_sampler_rgba_fn rgba;
    const void *ctx;
} pc_algo_sampler;

/* RGB over r at full resolution. */
pc_status pc_algo_sampler_rgb(const pc_algo_sampler *s, pc_rect r, pc_algo_rgb_image *out, pc_error *err);
/* RGB over r box-downsampled by step, read in strips of about 64 rows (in parallel). */
pc_status pc_algo_sampler_rgb_scaled(const pc_algo_sampler *s, pc_rect r, size_t step, pc_algo_rgb_image *out,
                                     pc_error *err);
/* Samples a raster surface (any format, converted to straight RGBA); the surface is borrowed. */
pc_algo_sampler pc_algo_surface_sampler(const pc_surface *s);
/* Samples an in-memory RGB image placed at (ox, oy) (transparent outside). `img` must outlive
 * the sampler; `holder` is caller storage the sampler points at. */
typedef struct pc_algo_image_sampler {
    const pc_algo_rgb_image *img;
    int32_t ox, oy;
} pc_algo_image_sampler;
pc_algo_sampler pc_algo_image_sampler_make(const pc_algo_image_sampler *holder);

/* Integer working-resolution step so that `area` holds at most about max_px pixels. */
size_t pc_algo_segment_scale_for(pc_rect area, size_t max_px);
/* GrabCut's beta = 1 / (2 <|z_m - z_n|^2>) over 8-neighbour pairs (0 for a flat image). */
float pc_algo_segment_contrast_beta(const pc_algo_rgb_image *img);

/* Pixel label constraints for grid_cut. */
#define PC_ALGO_SEGMENT_FREE 0
#define PC_ALGO_SEGMENT_HARD_FG 1
#define PC_ALGO_SEGMENT_HARD_BG 2

/* One min cut over the 8-connected grid (Boykov & Jolly 2001): data costs per pixel and the
 * contrast-sensitive smoothness gamma e^(-beta |dz|^2) / dist. All arrays hold w * h entries.
 * fg_out (w * h bools, caller buffer) is true for foreground. */
pc_status pc_algo_segment_grid_cut(const pc_algo_rgb_image *img, const float *cost_fg, const float *cost_bg,
                                   const uint8_t *fixed, float gamma, float beta, bool *fg_out,
                                   pc_error *err);
/* Data costs (-ln p(z|fg), -ln p(z|bg)) for every pixel (caller buffers of w * h). */
pc_status pc_algo_segment_data_costs(const pc_algo_rgb_image *img, const pc_algo_gmm *fg,
                                     const pc_algo_gmm *bg, float *cost_fg, float *cost_bg, pc_error *err);
/* 8-connected components of mask: labels (0 = background, newly allocated) and sizes indexed by
 * label (newly allocated, nsizes = components + 1). */
pc_status pc_algo_segment_components(const bool *mask, size_t w, size_t h, uint32_t **labels, size_t **sizes,
                                     size_t *nsizes, pc_error *err);
/* Keeps the components of mask that contain a seed (out: caller buffer of w * h). */
pc_status pc_algo_segment_keep_seeded(const bool *mask, const bool *seeds, size_t w, size_t h, bool *out,
                                      pc_error *err);
/* Keeps components at least frac of the largest and fills holes smaller than hole_frac of the
 * kept area (out: caller buffer of w * h). */
pc_status pc_algo_segment_clean_mask(const bool *mask, size_t w, size_t h, float frac, float hole_frac,
                                     bool *out, pc_error *err);
/* Softens a hard 0/255 mask's edges in place (the magic wand's rule). */
pc_status pc_algo_segment_antialias_u8(uint8_t *mask, size_t w, size_t h, pc_error *err);
/* Shrinks a region's box to its non-zero pixels; the input is consumed (moved into *out, or
 * freed when nothing remains: *out is then None). */
pc_status pc_algo_segment_trim_region(pc_algo_region *r, pc_algo_region *out, pc_error *err);

/* Deterministic xorshift (segment::Rng). */
typedef struct pc_algo_segment_rng {
    uint64_t s;
} pc_algo_segment_rng;
pc_algo_segment_rng pc_algo_segment_rng_new(uint64_t seed);
uint64_t pc_algo_segment_rng_next(pc_algo_segment_rng *r);
float pc_algo_segment_rng_f32(pc_algo_segment_rng *r);    /* [0, 1) */
float pc_algo_segment_rng_normal(pc_algo_segment_rng *r); /* Box-Muller */
/* Every k-th sample so that at most max remain (out newly allocated, free()). */
pc_status pc_algo_segment_subsample(const float (*v)[3], size_t n, size_t max, float (**out)[3], size_t *nout,
                                    pc_error *err);

/* ---- max-flow (maxflow.rs) ---- */

typedef struct pc_algo_maxflow pc_algo_maxflow;
/* A graph with `nodes` nodes and room reserved for `edges` edge pairs (more may be added). */
pc_status pc_algo_maxflow_new(size_t nodes, size_t edges, pc_algo_maxflow **out, pc_error *err);
void pc_algo_maxflow_free(pc_algo_maxflow *g); /* NULL-safe */
size_t pc_algo_maxflow_node_count(const pc_algo_maxflow *g);
/* Accumulates source / sink capacities of node i (ignored for i out of range). */
void pc_algo_maxflow_add_tweights(pc_algo_maxflow *g, size_t i, float cap_source, float cap_sink);
/* Edge i -> j with cap and j -> i with rev_cap (PC_ERR_INVALID for i == j or out of range). */
pc_status pc_algo_maxflow_add_edge(pc_algo_maxflow *g, size_t i, size_t j, float cap, float rev_cap,
                                   pc_error *err);
/* Computes the maximum flow (= minimum cut capacity) into *flow. */
pc_status pc_algo_maxflow_run(pc_algo_maxflow *g, double *flow, pc_error *err);
/* After run: is node i on the source side of the minimum cut? */
bool pc_algo_maxflow_in_source(const pc_algo_maxflow *g, size_t i);
double pc_algo_maxflow_flow(const pc_algo_maxflow *g);
/* Sum of residual capacities on arcs leaving the source side (0 after run). */
float pc_algo_maxflow_residual_out_of_source_side(const pc_algo_maxflow *g);

/* ---- Gaussian mixtures (gmm.rs) ---- */

#define PC_ALGO_GMM_DEFAULT_REG 2e-5f

typedef struct pc_algo_gmm_component {
    float weight;
    float mean[3];
    float cov[3][3];
    double inv[3][3];
    double log_norm; /* ln(weight) - 1/2 ln det - 3/2 ln 2pi */
} pc_algo_gmm_component;

/* A Gaussian mixture over RGB. Owned: pc_algo_gmm_fini. */
struct pc_algo_gmm {
    pc_algo_gmm_component *comps;
    size_t n;
    float reg; /* ridge added to covariance diagonals */
};

void pc_algo_gmm_fini(pc_algo_gmm *g); /* NULL-safe */
pc_status pc_algo_gmm_clone(const pc_algo_gmm *g, pc_algo_gmm *out, pc_error *err);
/* Gmm::fit / fit_em: k components (fewer if not enough distinct samples). *some false (out
 * untouched) without samples or usable components. */
pc_status pc_algo_gmm_fit(const float (*samples)[3], size_t n, size_t k, float reg, pc_algo_gmm *out,
                          bool *some, pc_error *err);
pc_status pc_algo_gmm_fit_em(const float (*samples)[3], size_t n, size_t k, float reg, size_t em_iters,
                             pc_algo_gmm *out, bool *some, pc_error *err);
/* Component statistics from hard assignments (labels < k, else PC_ERR_INVALID). */
pc_status pc_algo_gmm_from_labels(const float (*samples)[3], size_t n, const size_t *labels, size_t k,
                                  float reg, pc_algo_gmm *out, bool *some, pc_error *err);
/* GrabCut update: assign each sample to its most likely component and re-estimate. */
pc_status pc_algo_gmm_refit(const pc_algo_gmm *g, const float (*samples)[3], size_t n, pc_algo_gmm *out,
                            bool *some, pc_error *err);
size_t pc_algo_gmm_component_of(const pc_algo_gmm *g, const float z[3]);
double pc_algo_gmm_log_prob(const pc_algo_gmm *g, const float z[3]);
float pc_algo_gmm_neg_log(const pc_algo_gmm *g, const float z[3]); /* -ln p, clamped to -50..200 */
double pc_algo_gmm_component_log_weighted(const pc_algo_gmm_component *c, const float z[3]);
/* k-means with k-means++ seeding (deterministic); drops empty clusters. n must be > 0.
 * centers: newly allocated (free()), *ncenters <= k. */
pc_status pc_algo_gmm_kmeans(const float (*samples)[3], size_t n, size_t k, size_t iters,
                             float (**centers)[3], size_t *ncenters, pc_error *err);

/* ---- SLIC superpixels (slic.rs) ---- */

/* sRGB (0..=1) to CIELAB (D65), L in 0..=100. */
void pc_algo_slic_rgb_to_lab(const float c[3], float out[3]);

typedef struct pc_algo_superpixels {
    size_t w, h;
    uint32_t *labels; /* label per pixel, 0..count (free()) */
    size_t count;
    float step; /* grid interval S used */
} pc_algo_superpixels;
void pc_algo_superpixels_fini(pc_algo_superpixels *sp);

/* About `target` superpixels; compactness is SLIC's m, iters the k-means iterations. */
pc_status pc_algo_slic(const pc_algo_rgb_image *img, size_t target, float compactness, size_t iters,
                       pc_algo_superpixels *out, pc_error *err);
pc_status pc_algo_slic_lab(const float (*lab)[3], size_t w, size_t h, size_t target, float compactness,
                           size_t iters, pc_algo_superpixels *out, pc_error *err);

/* ---- GrabCut and rectangle Object Selection (grabcut.rs) ---- */

#define PC_ALGO_GRABCUT_BG 0
#define PC_ALGO_GRABCUT_FG 1
#define PC_ALGO_GRABCUT_PR_BG 2
#define PC_ALGO_GRABCUT_PR_FG 3
#define PC_ALGO_GRABCUT_GAMMA 50.0f
#define PC_ALGO_GRABCUT_K 5

/* Runs iters GrabCut iterations, updating the probable labels of trimap (w * h) in place.
 * *some false when either side has no pixels; else fg / bg receive the final models. */
pc_status pc_algo_grabcut(const pc_algo_rgb_image *img, uint8_t *trimap, size_t iters, pc_algo_gmm *fg,
                          pc_algo_gmm *bg, bool *some, pc_error *err);
/* Object Selection (rectangle mode) on a working copy of about work_px pixels, re-cut at full
 * resolution. */
pc_status pc_algo_grabcut_object_select(const pc_algo_sampler *s, pc_rect canvas, pc_rect rect,
                                        size_t work_px, pc_algo_region *out, pc_error *err);

/* Working-resolution labelling over `window` (sampled with step) to a full-resolution region,
 * the boundary re-cut with the models (fg and bg both NULL: no re-cut), anti-aliased. */
pc_status pc_algo_segment_finish_region(const pc_algo_sampler *s, pc_rect window, size_t step,
                                        const bool *low, size_t lw, size_t lh, const pc_algo_gmm *fg,
                                        const pc_algo_gmm *bg, pc_algo_region *out, pc_error *err);

/* ---- Quick Selection (quick.rs) ---- */

#define PC_ALGO_QUICK_WORK_PX 160000

/* Grows a selection from a brush stroke (points in document pixels, brush diameter size). */
pc_status pc_algo_quick_select(const pc_algo_sampler *s, pc_rect canvas, const float (*points)[2],
                               size_t npoints, float size, size_t work_px, pc_algo_region *out,
                               pc_error *err);
/* The working-resolution segmentation: models and labels (low: caller buffer of w * h). */
pc_status pc_algo_quick_segment(const pc_algo_rgb_image *img, const bool *seeds, pc_algo_gmm *fg,
                                pc_algo_gmm *bg, bool *low, pc_error *err);
/* A near-uniform density over the RGB cube. */
pc_status pc_algo_quick_broad_model(pc_algo_gmm *out, pc_error *err);
/* Pixels within radius of the polyline, plus each point's own pixel (out: caller w * h). */
void pc_algo_quick_stroke_mask(const float (*pts)[2], size_t n, float radius, size_t w, size_t h, bool *out);
/* Geodesic distance from seeds over the barrier map (out: newly allocated w * h, free()). */
pc_status pc_algo_quick_geodesic(const pc_algo_rgb_image *img, const bool *seeds, float **out, pc_error *err);

/* ---- Select > Sky (sky.rs) ---- */

#define PC_ALGO_SKY_WORK_PX 160000

typedef struct pc_algo_sky_params {
    float threshold; /* 0..100: higher is stricter */
    float softness;  /* 0..100: edge softness */
} pc_algo_sky_params;
pc_algo_sky_params pc_algo_sky_params_default(void); /* 50, 50 */

float pc_algo_sky_colour_prior(const float p[3]);
/* Per-pixel sky likelihood (out: newly allocated w * h, free()). */
pc_status pc_algo_sky_likelihood(const pc_algo_rgb_image *img, float **out, pc_error *err);
/* Binary sky mask at working resolution (*some false when no sky touches the top). */
pc_status pc_algo_sky_mask_low(const pc_algo_rgb_image *img, float threshold, bool **out, bool *some,
                               pc_error *err);
pc_status pc_algo_sky_select(const pc_algo_sampler *s, pc_rect canvas, pc_algo_sky_params p,
                             pc_algo_region *out, pc_error *err);

/* ---- Select Subject (subject.rs) ---- */

#define PC_ALGO_SUBJECT_WORK_PX 110000

pc_status pc_algo_subject_select(const pc_algo_sampler *s, pc_rect canvas, pc_algo_region *out,
                                 pc_error *err);
/* Per-pixel saliency in 0..=1 (out: newly allocated w * h, free()). */
pc_status pc_algo_subject_saliency(const pc_algo_rgb_image *img, float **out, pc_error *err);
/* Otsu's threshold of values in 0..=1 (256 bins). */
float pc_algo_subject_otsu(const float *v, size_t n);

/* ---- Focus Area (focus.rs) ---- */

#define PC_ALGO_FOCUS_WORK_PX 1000000

/* Per-pixel normalised sharpness (out: newly allocated w * h, free(); NULL for an empty image). */
pc_status pc_algo_focus_sharpness(const pc_algo_rgb_image *img, float noise, float **out, pc_error *err);
pc_status pc_algo_focus_area(const pc_algo_sampler *s, pc_rect canvas, float range, float noise,
                             pc_algo_region *out, pc_error *err);

#endif
