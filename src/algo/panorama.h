/* Panorama stitching for File > Automate > Photomerge and Edit > Auto-Align / Auto-Blend Layers
 * (port of crates/algo/src/panorama.rs), after Brown & Lowe (IJCV 2007):
 *
 *  1. features: Harris + steered BRIEF (features.h);
 *  2. pair matching: ratio-test matches, RANSAC homography, verification inliers > 8 + 0.3 matches;
 *  3. focal length from pairwise homographies of a rotating camera (Shum & Szeliski) or EXIF;
 *  4. projection + motion per layout: Perspective (plane, homography), Cylindrical / Spherical
 *     (rigid motion on the surface), Collage (similarity), Reposition (translation); Auto picks
 *     perspective unless too stretched, then cylindrical or spherical;
 *  5. maximum spanning tree from the reference + Levenberg-Marquardt bundle adjustment (optional
 *     radial distortion k1, focal refinement on cylinder / sphere);
 *  6. photometric: gains jointly with a shared radial vignetting model, linear least squares in
 *     log space (Goldman 2010);
 *  7. seams: per new image a minimum-cost DP path through its overlap with the composite;
 *  8. multi-band blending (Burt & Adelson 1983) with push-pull hole filling.
 *
 * Deterministic, bit-identical to the reference (fixed RANSAC seeds, sums in reference order,
 * parallel work combined in index order). Registration runs on luminance at a reduced scale;
 * pc_algo_placement_scaled maps the result back. Lists returned through pointers are malloc'd and
 * released with the matching _free / _fini. Errors: PC_ERR_INVALID for inconsistent inputs (where
 * the reference would panic), NOMEM, LIMIT. Thread-safe on distinct objects. */
#ifndef PC_ALGO_PANORAMA_H
#define PC_ALGO_PANORAMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "algo/features.h"
#include "algo/transform.h"
#include "base/status.h"

typedef enum pc_algo_layout {
    PC_ALGO_LAYOUT_AUTO,
    PC_ALGO_LAYOUT_PERSPECTIVE,
    PC_ALGO_LAYOUT_CYLINDRICAL,
    PC_ALGO_LAYOUT_SPHERICAL,
    PC_ALGO_LAYOUT_COLLAGE,
    PC_ALGO_LAYOUT_REPOSITION
} pc_algo_layout;

/* "auto", "perspective", "cylindrical", "spherical", "collage", "reposition". */
bool pc_algo_layout_parse(const char *s, pc_algo_layout *out);
const char *pc_algo_layout_name(pc_algo_layout l);

typedef enum pc_algo_projection {
    PC_ALGO_PROJECTION_PLANE,
    PC_ALGO_PROJECTION_CYLINDER,
    PC_ALGO_PROJECTION_SPHERE
} pc_algo_projection;

typedef enum pc_algo_motion {
    PC_ALGO_MOTION_TRANSLATION,
    PC_ALGO_MOTION_EUCLIDEAN, /* rotation + translation */
    PC_ALGO_MOTION_SIMILARITY,
    PC_ALGO_MOTION_HOMOGRAPHY
} pc_algo_motion;

/* Least-squares fit of `motion` mapping src[i] -> dst[i]; false when under-determined. */
bool pc_algo_panorama_fit_motion(pc_algo_motion motion, const double (*src)[2], const double (*dst)[2],
                                 size_t n, pc_algo_homography *out);

/* RANSAC over motion models (stream seed ^ 0x5EED0FA1); outputs as pc_algo_features_ransac. */
pc_status pc_algo_panorama_ransac_motion(pc_algo_motion motion, const double (*src)[2],
                                         const double (*dst)[2], size_t n, size_t iters, double threshold,
                                         uint64_t seed, bool *found, pc_algo_homography *out,
                                         size_t **inliers, size_t *ninliers, pc_error *err);

/* Where one image lands: image px -> (undistort) -> projection -> 2-D motion h. */
typedef struct pc_algo_placement {
    pc_algo_projection projection;
    double focal;         /* px: the projection radius */
    double center[2];     /* projection centre in image px */
    double k1;            /* radial distortion on radii normalised by the focal length (0 = none) */
    pc_algo_homography h; /* projected -> panorama */
} pc_algo_placement;

/* Image px -> projected coordinates. */
void pc_algo_placement_project(const pc_algo_placement *p, double x, double y, double out[2]);
/* Projected -> image px; false outside the valid hemisphere. */
bool pc_algo_placement_unproject(const pc_algo_placement *p, double a, double b, double *x, double *y);
/* Image px -> panorama px. */
void pc_algo_placement_forward(const pc_algo_placement *p, double x, double y, double *u, double *v);
/* Panorama px -> image px; false when not invertible or outside. */
bool pc_algo_placement_inverse(const pc_algo_placement *p, double u, double v, double *x, double *y);
/* The same placement for images k times larger. */
pc_algo_placement pc_algo_placement_scaled(const pc_algo_placement *p, double k);
pc_algo_placement pc_algo_placement_translated(const pc_algo_placement *p, double dx, double dy);
/* Bounds {x0, y0, x1, y1} of a w x h image in the panorama (sampled along its edges). */
void pc_algo_placement_bounds(const pc_algo_placement *p, double w, double h, double out[4]);

/* Keypoints and descriptors of one image (registration scale). */
typedef struct pc_algo_image_features {
    size_t w, h;
    double (*points)[2];
    pc_algo_descriptor *desc;
    size_t n;
} pc_algo_image_features;

void pc_algo_image_features_fini(pc_algo_image_features *f);
/* Deep copy (NOMEM). */
pc_status pc_algo_image_features_copy(const pc_algo_image_features *src, pc_algo_image_features *out,
                                      pc_error *err);

/* Detects up to `max` corners of a w x h luminance image (valid: w * h flags or NULL) and
 * describes them. */
pc_status pc_algo_panorama_detect(size_t w, size_t h, const float *luma, const bool *valid, size_t max,
                                  pc_algo_image_features *out, pc_error *err);

/* One image for detect_many (borrowed buffers). */
typedef struct pc_algo_prepared_image {
    size_t w, h;
    const float *luma;
    const bool *valid; /* NULL: all valid */
} pc_algo_prepared_image;

/* detect for n images in parallel; out has n entries. */
pc_status pc_algo_panorama_detect_many(const pc_algo_prepared_image *imgs, size_t n, size_t max,
                                       pc_algo_image_features *out, pc_error *err);

/* A verified pair: correspondences (image a px, image b px) and the homography b -> a. */
typedef struct pc_algo_pair_match {
    size_t a, b;
    double (*pts_a)[2];
    double (*pts_b)[2];
    size_t n;
    pc_algo_homography homography;
} pc_algo_pair_match;

void pc_algo_pair_matches_free(pc_algo_pair_match *m, size_t count);

/* Matches every pair and keeps those passing RANSAC + the Brown-Lowe verification, ordered by
 * (a, b). *out is NULL when none. */
pc_status pc_algo_panorama_match_pairs(const pc_algo_image_features *feats, size_t n,
                                       pc_algo_pair_match **out, size_t *count, pc_error *err);

/* Focal length (px) of a rotating camera from a homography between two of its images (centres
 * ca, cb); false when the homography doesn't constrain it. */
bool pc_algo_panorama_focal_from_homography(const pc_algo_homography *h, const double ca[2],
                                            const double cb[2], double *out);

typedef struct pc_algo_align_options {
    pc_algo_layout layout;
    bool has_focal;
    double focal; /* registration px (EXIF); estimated when !has_focal or <= 1 */
    bool has_reference;
    size_t reference; /* image that stays put; otherwise the best-connected one */
    bool geometric;   /* refine a radial distortion coefficient */
} pc_algo_align_options;

typedef struct pc_algo_pair_inliers {
    size_t a, b, inliers;
} pc_algo_pair_inliers;

typedef struct pc_algo_alignment {
    pc_algo_layout layout; /* Auto resolved */
    size_t reference;
    size_t n;                      /* images */
    pc_algo_placement *placements; /* n entries; meaningful where placed[i] */
    bool *placed;                  /* n entries; false for images that couldn't be connected */
    double focal, k1;
    pc_algo_pair_inliers *pairs;
    size_t npairs;
    double rms; /* RMS reprojection error after bundle adjustment (px, registration scale) */
} pc_algo_alignment;

void pc_algo_alignment_fini(pc_algo_alignment *a);

/* Aligns images from their features; *found is false when fewer than two images connect. */
pc_status pc_algo_panorama_align(const pc_algo_image_features *feats, size_t n,
                                 const pc_algo_pair_match *matches, size_t nmatches,
                                 const pc_algo_align_options *opts, bool *found, pc_algo_alignment *out,
                                 pc_error *err);

/* One image warped into the panorama: an axis-aligned region with colour channels (no alpha)
 * and coverage 0..1. Buffers are borrowed: px has w * h * ch floats, alpha w * h. */
typedef struct pc_algo_roi_image {
    int32_t x0, y0;
    size_t w, h, ch;
    const float *px;
    const float *alpha;
} pc_algo_roi_image;

/* Gains per image and channel (gains[i * ch + c]) and the vignetting model
 * log V(r) = a r^2 + b r^4 (r: distance from the image centre over the half diagonal). */
typedef struct pc_algo_photometric {
    float *gains;
    size_t n, ch;
    double vignette[2];
} pc_algo_photometric;

void pc_algo_photometric_fini(pc_algo_photometric *p);
/* Multiplier correcting a pixel of image i, channel c at normalised squared radius r2. */
float pc_algo_photometric_factor(const pc_algo_photometric *p, size_t i, size_t c, double r2);

/* Estimates exposure gains and a shared vignetting falloff from the overlaps of n images.
 * placements / sizes (one per image, {w, h}) give each pixel's source radius; without them
 * (NULL) the vignetting model is not estimated. All images must have the first one's ch. */
pc_status pc_algo_panorama_photometric(const pc_algo_roi_image *imgs, size_t n,
                                       const pc_algo_placement *placements, const size_t (*sizes)[2],
                                       bool gain, bool vignette, pc_algo_photometric *out, pc_error *err);

/* Seam labels on a w x h grid (-1 = no image): images are added in `order`; each takes the part
 * of its overlap with the composite on its side of the cheapest path through the overlap (cost:
 * luminance difference). luma / cover have `count` planes of w * h. *out has w * h entries. */
pc_status pc_algo_panorama_seam_labels(size_t w, size_t h, const float *const *luma, const bool *const *cover,
                                       size_t count, const size_t *order, size_t norder, int32_t **out,
                                       pc_error *err);

/* Pyramid levels multiband uses for a seam mask of this size. */
size_t pc_algo_panorama_blend_levels(size_t w, size_t h);

/* Multi-band blend of n images into a w x h panorama (origin 0, 0) with one weight map per image
 * over its ROI grid (weights[i] has w_i * h_i entries). *out_px gets w * h * ch colours, *out_cov
 * w * h coverage (both malloc'd). levels above 30 are PC_ERR_LIMIT. */
pc_status pc_algo_panorama_multiband(size_t w, size_t h, const pc_algo_roi_image *imgs, size_t n,
                                     const float *const *weights, size_t levels, float **out_px,
                                     float **out_cov, pc_error *err);

#endif
