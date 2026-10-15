/* Computational-photography registration and blending primitives (port of the shared helpers in
 * crates/engine/src/photo_cmds.rs), exposed for Edit > Auto-Align / Auto-Blend Layers
 * (align_cmds.c, E9) and -- when E3 file I/O lands -- File > Automate > Photomerge / Merge to HDR.
 *
 * These are pure functions over surfaces: feature / intensity registration, placement warping and
 * seam + multi-band blending (photocraft_algo::panorama / pyramid / direct). They never read the
 * Session or files, so they are reachable now; the Photomerge / HDR / Crop-and-Straighten COMMANDS
 * that load files are deferred to E3. Never crash on hostile inputs. */
#ifndef PC_ENGINE_PHOTO_CMDS_H
#define PC_ENGINE_PHOTO_CMDS_H

#include "algo/panorama.h"
#include "algo/transform.h"
#include "base/status.h"
#include "geom/geom.h"
#include "raster/surface.h"

/* Longest side used for feature registration (REGISTER_SIDE). */
#define PC_ENGINE_PHOTO_REGISTER_SIDE ((size_t)1200)
/* The intensity registration searches planes at most this many px across (DIRECT_SIDE). */
#define PC_ENGINE_PHOTO_DIRECT_SIDE ((size_t)2048)
/* The error message when either registration finds no matching detail (photo_cmds::NO_DETAIL). */
extern const char *const PC_ENGINE_PHOTO_NO_DETAIL;

/* One input image: a surface and the content area to register / warp. */
typedef struct pc_engine_photo_image {
    const pc_surface *surf;
    pc_rect rect;
} pc_engine_photo_image;

/* Luminance and coverage of `surf` over `area`, box-downsampled by `k` (>= 1). *out_l and *out_a
 * are newly allocated (free()) arrays of (*out_sw) * (*out_sh) floats. NOMEM / LIMIT. */
pc_status pc_engine_photo_luma_alpha(const pc_surface *surf, pc_rect area, size_t k, size_t *out_sw,
                                     size_t *out_sh, float **out_l, float **out_a, pc_error *err);

/* Feature registration of `images` (Brown-Lowe). *found is false (out untouched) when fewer than
 * two images connect, like the reference's `None`. On *found, `out` is the full-resolution
 * alignment (caller: pc_algo_alignment_fini). focal35 is the 35 mm-equivalent focal length when
 * known (has_focal35). */
pc_status pc_engine_photo_register(const pc_engine_photo_image *images, size_t n, pc_algo_layout layout,
                                   bool has_ref, size_t reference, bool geometric, bool has_focal35,
                                   double focal35, bool *found, pc_algo_alignment *out, pc_error *err);

/* Intensity registration of `images` (all one size) for layers that nearly coincide (a bracket).
 * On error `err` carries the reference's message (compare to PC_ENGINE_PHOTO_NO_DETAIL to tell
 * "no detail" from a bad projection); on PC_OK `out` is the alignment (pc_algo_alignment_fini). */
pc_status pc_engine_photo_register_direct(const pc_engine_photo_image *images, size_t n,
                                          pc_algo_layout layout, size_t reference,
                                          pc_algo_alignment *out, pc_error *err);

/* The projective matrix of a planar, distortion-free placement (surface px -> panorama px), or
 * false when the placement is projected / distorted (warp through pc_engine_photo_warp_placed). */
bool pc_engine_photo_plane_matrix(const pc_algo_placement *pl, double ox, double oy,
                                  pc_algo_homography *out);

/* Warps the content of `surf` inside `rect` by `pl` shifted by (offx, offy); image coordinates are
 * surface coordinates minus (ox, oy). `out` is initialised on success (pc_surface_fini). */
pc_status pc_engine_photo_warp_placed(const pc_surface *surf, pc_rect rect, double ox, double oy,
                                      const pc_algo_placement *pl, double offx, double offy,
                                      pc_algo_interp interp, pc_surface *out, pc_error *err);

/* Seam order: the reference first, then by distance of each image's centre from it. `out` has `n`
 * entries (caller-allocated). */
void pc_engine_photo_seam_order(const pc_surface *warped, size_t n, size_t reference, size_t *out);

/* Result of pc_engine_photo_seam_blend: per input the pixels to keep (blended inside its seam
 * region) and its mask, plus the blended composite. (The reference also returns gain maps, used
 * only by Photomerge; omitted here, REWRITE_PLAN.md §7.) */
typedef struct pc_engine_photo_blended {
    pc_surface *layer_px;   /* n surfaces */
    pc_surface *layer_mask; /* n grayscale surfaces */
    size_t n;
    pc_surface composite;
    double vignette[2];
} pc_engine_photo_blended;

void pc_engine_photo_blended_fini(pc_engine_photo_blended *b); /* NULL-safe */

/* Gain compensation (+ optional vignetting with `has_geometry`), seams in `order`, multi-band
 * blending. Inputs are surfaces already in panorama coordinates with alpha. `out` is initialised
 * on success (pc_engine_photo_blended_fini). */
pc_status pc_engine_photo_seam_blend(const pc_surface *warped, size_t n, pc_rect canvas,
                                     const size_t *order, bool seamless, bool gains_on,
                                     bool has_geometry, const pc_algo_placement *geo_pl,
                                     const size_t (*geo_sizes)[2], double geo_off_x, double geo_off_y,
                                     pc_engine_photo_blended *out, pc_error *err);

#endif /* PC_ENGINE_PHOTO_CMDS_H */
