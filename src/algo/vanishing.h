/* Filter > Vanishing Point (port of crates/algo/src/vanishing.rs): perspective planes, pasting
 * onto a plane and cloning in perspective.
 *
 * A plane's two vanishing points give the focal length for a centred principal point
 * (f^2 = -(v1 - c).(v2 - c), Hartley & Zisserman 8.8), which lifts the quad to 3-D: that gives
 * the plane's metric aspect (pasted images keep their proportions) and lets perpendicular planes
 * be torn off an edge. Pasting warps into the plane's metric frame; clone stamping copies, for
 * each destination dab, the pixels at the same metric offset from the source point.
 *
 * Plain data; a scene owns its plane array. Reentrant. */
#ifndef PC_ALGO_VANISHING_H
#define PC_ALGO_VANISHING_H

#include <stdbool.h>
#include <stddef.h>

#include "algo/transform.h"
#include "base/status.h"
#include "geom/geom.h"
#include "raster/surface.h"

/* A perspective plane: corners in image px, clockwise from the plane's top-left. */
typedef struct pc_algo_vp_plane {
    double corners[4][2];
} pc_algo_vp_plane;

/* Plane edges for tearing off a perpendicular plane. */
typedef enum pc_algo_vp_edge {
    PC_ALGO_VP_TOP,
    PC_ALGO_VP_RIGHT,
    PC_ALGO_VP_BOTTOM,
    PC_ALGO_VP_LEFT
} pc_algo_vp_edge;

/* The camera and a plane lifted to 3-D (camera coordinates, plane at unit distance along its
 * normal). */
typedef struct pc_algo_vp_lifted {
    double focal;
    double center[2];
    double pts[4][3];
} pc_algo_vp_lifted;

/* Unit square (u, v) -> image; false when the quad is degenerate. */
bool pc_algo_vp_plane_homography(const pc_algo_vp_plane *p, pc_algo_homography *out);
/* Focal length (px) implied by the plane's vanishing points for a camera centred at c; false
 * when a vanishing point is at infinity or the geometry is inconsistent. */
bool pc_algo_vp_plane_focal(const pc_algo_vp_plane *p, const double c[2], double *out);
/* Plane validity (VpPlane::check): Photoshop draws a valid plane's grid blue, a doubtful one
 * yellow and an invalid one red. */
typedef enum pc_algo_vp_plane_check {
    PC_ALGO_VP_VALID,
    PC_ALGO_VP_QUESTIONABLE,
    PC_ALGO_VP_INVALID
} pc_algo_vp_plane_check;
/* Invalid for a quad that isn't convex (self-intersecting, a corner pushed inside, a near-straight
 * or near-zero corner), Questionable for a convex quad whose perspective can't come from a camera
 * centred at `center` (or with a corner sharper than 10 degrees), else Valid. */
pc_algo_vp_plane_check pc_algo_vp_plane_check_at(const pc_algo_vp_plane *p, const double center[2]);
/* "valid" / "questionable" / "invalid" (serde camelCase). */
const char *pc_algo_vp_plane_check_name(pc_algo_vp_plane_check c);
/* The same quad with its corners clockwise on screen, starting at the top-left one (the order
 * paste and tear_off expect), however they were clicked. */
pc_algo_vp_plane pc_algo_vp_plane_normalized(const pc_algo_vp_plane *p);
/* Lifts the plane to 3-D with focal f (camera at the origin looking along +z). */
void pc_algo_vp_plane_lift(const pc_algo_vp_plane *p, double f, const double c[2], pc_algo_vp_lifted *out);
/* 3-D point -> image; false behind the camera. */
bool pc_algo_vp_lifted_project(const pc_algo_vp_lifted *l, const double x[3], double out[2]);
/* Metric side lengths (u along the top edge, v along the left edge). */
void pc_algo_vp_lifted_lengths(const pc_algo_vp_lifted *l, double *lu, double *lv);

/* Planes plus the shared focal length. */
typedef struct pc_algo_vp_scene {
    pc_algo_vp_plane *planes; /* owned */
    size_t n, cap;
    double focal;
    double center[2];
} pc_algo_vp_scene;

/* Focal length from `focal` when > 1 (has_focal), else from the first plane, else fallback. */
pc_status pc_algo_vp_scene_init(pc_algo_vp_scene *s, const pc_algo_vp_plane *first, const double center[2],
                                bool has_focal, double focal, double fallback, pc_error *err);
void pc_algo_vp_scene_fini(pc_algo_vp_scene *s); /* NULL-safe */
/* Plane i lifted with the scene's focal length (i < n). */
void pc_algo_vp_scene_lifted(const pc_algo_vp_scene *s, size_t i, pc_algo_vp_lifted *out);
/* Tears a plane off edge `edge` of plane `from`, turned by `angle` degrees about that edge
 * (90 = perpendicular), extending `depth` times the parent's adjacent side. *index receives the
 * new plane's index; PC_ERR_INVALID when no orientation projects (the reference's None) or
 * `from` is out of range. */
pc_status pc_algo_vp_scene_tear_off(pc_algo_vp_scene *s, size_t from, pc_algo_vp_edge edge, double angle,
                                    double depth, size_t *index, pc_error *err);
/* Plane-metric frame of plane i: homography unit square -> image, metric width and height. */
bool pc_algo_vp_scene_frame(const pc_algo_vp_scene *s, size_t i, pc_algo_homography *h, double *lu,
                            double *lv);
/* The last plane whose quad contains image point p; false when none. */
bool pc_algo_vp_scene_plane_at(const pc_algo_vp_scene *s, const double p[2], size_t *out);

/* Pastes src (content in src_rect) onto plane i with its top-left at plane coordinates `at` and
 * its width `width` (fraction of the plane's width), aspect kept in the plane's metric. `out`
 * receives the warped image clipped to the plane. PC_ERR_INVALID for a degenerate plane or an
 * empty src_rect (the reference's None). */
pc_status pc_algo_vp_paste(const pc_algo_vp_scene *s, size_t i, const pc_surface *src, pc_rect src_rect,
                           const double at[2], double width, pc_surface *out, pc_error *err);
/* Perspective clone stamp: one stroke of dabs at image points on the destination plane, copying
 * from `source` with the offset fixed in plane-metric units at the first dab. `size` is the brush
 * diameter in px at the first dab; hardness 0..=1. *dabs receives the number of dabs painted. */
pc_status pc_algo_vp_clone_stroke(const pc_algo_vp_scene *s, pc_surface *surf, const double source[2],
                                  const double (*points)[2], size_t n_points, double size, double hardness,
                                  float opacity, size_t *dabs, pc_error *err);

#endif
