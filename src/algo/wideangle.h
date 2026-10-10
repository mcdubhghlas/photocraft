/* Filter > Adaptive Wide Angle (port of crates/algo/src/wideangle.rs): straighten lines that a
 * wide-angle or fisheye lens bends.
 *
 * Constraints are marked by their two end points; the lens model (equidistant fisheye, rectilinear
 * perspective, or a full 2:1 equirectangular sphere) turns them into rays, and the scene line
 * between them is the great circle through both, sampled back into the image. A content-
 * preserving mesh warp (as-similar-as-possible shape term, Igarashi et al. 2005; line term,
 * Carroll et al. 2009) then makes every constrained curve straight, solved by CGLS.
 *
 * Plain data; a settings struct owns its constraint array and a mesh its buffers. Reentrant. */
#ifndef PC_ALGO_WIDEANGLE_H
#define PC_ALGO_WIDEANGLE_H

#include <stdbool.h>
#include <stddef.h>

#include <yyjson.h>

#include "algo/params.h"
#include "base/status.h"
#include "geom/geom.h"
#include "raster/surface.h"

/* Lens projection of the source image. */
typedef enum pc_algo_wide_model {
    PC_ALGO_WIDE_AUTO,
    PC_ALGO_WIDE_FISHEYE,
    PC_ALGO_WIDE_PERSPECTIVE,
    PC_ALGO_WIDE_FULL_SPHERICAL
} pc_algo_wide_model;

/* Constraint orientation. */
typedef enum pc_algo_wide_orientation {
    PC_ALGO_WIDE_FREE,
    PC_ALGO_WIDE_HORIZONTAL,
    PC_ALGO_WIDE_VERTICAL
} pc_algo_wide_orientation;

/* WideModel::parse: "auto", "fisheye", "perspective", "fullSpherical" / "spherical". */
bool pc_algo_wide_model_parse(const char *s, size_t len, pc_algo_wide_model *out);

/* A constraint: the end points of a scene line (image px). */
typedef struct pc_algo_wide_constraint {
    double a[2], b[2];
    pc_algo_wide_orientation orientation;
} pc_algo_wide_constraint;

typedef struct pc_algo_wide_angle {
    pc_algo_wide_model model;
    double focal_length; /* mm (35 mm frame after the crop factor); 0 picks a default */
    double crop_factor;
    double scale;                         /* output scale, percent */
    pc_algo_wide_constraint *constraints; /* owned */
    size_t n_constraints;
} pc_algo_wide_angle;

/* WideAngle::default() (allocates nothing). */
void pc_algo_wide_angle_default(pc_algo_wide_angle *p);
void pc_algo_wide_angle_fini(pc_algo_wide_angle *p); /* NULL-safe; leaves the defaults */
/* serde from a JSON object (missing fields keep their defaults); `out` written on success. */
pc_status pc_algo_wide_angle_from_json(yyjson_val *v, pc_algo_wide_angle *out, pc_error *err);
/* serde: fields in declaration order; NULL on out-of-memory. */
yyjson_mut_val *pc_algo_wide_angle_to_json(yyjson_mut_doc *d, const pc_algo_wide_angle *p);

/* Camera model over an image frame. */
typedef struct pc_algo_wide_camera {
    pc_algo_wide_model model; /* resolved, never AUTO */
    double f;                 /* focal length, px */
    double c[2];
    double size[2];
} pc_algo_wide_camera;

void pc_algo_wide_camera_init(pc_algo_wide_camera *cam, const pc_algo_wide_angle *p, pc_rect frame);
/* Image px -> unit ray (x right, y down, z forward). */
void pc_algo_wide_camera_ray(const pc_algo_wide_camera *cam, double x, double y, double out[3]);
/* Unit ray -> image px; false when it does not project. */
bool pc_algo_wide_camera_pixel(const pc_algo_wide_camera *cam, const double d[3], double out[2]);
/* Up to n + 1 image points along the scene line from a to b, newly allocated (free()); *count
 * receives how many projected. */
pc_status pc_algo_wide_camera_arc(const pc_algo_wide_camera *cam, const double a[2], const double b[2],
                                  size_t n, double (**out)[2], size_t *count, pc_error *err);

/* The solved warp: a grid over the frame with each vertex's output position. */
typedef struct pc_algo_wide_mesh {
    size_t nx, ny;
    /* (nx + 1) * (ny + 1) vertices, row-major: output position and source position */
    double (*out)[2];
    double (*src)[2];
    /* each constraint's curve in the source (for overlays) */
    double (**curves)[2];
    size_t *curve_len;
    size_t n_curves;
    double residual; /* max deviation from straight of the constrained curves (px) */
} pc_algo_wide_mesh;

void pc_algo_wide_mesh_fini(pc_algo_wide_mesh *m); /* NULL-safe */
/* Triangle vertex indices (2 per cell), newly allocated (free()); *count triangles. */
pc_status pc_algo_wide_mesh_triangles(const pc_algo_wide_mesh *m, size_t (**out)[3], size_t *count,
                                      pc_error *err);
/* Output position of source point p (bilinear in its grid cell). */
void pc_algo_wide_mesh_map(const pc_algo_wide_mesh *m, pc_rect frame, const double p[2], double out[2]);
/* Solves the content-preserving warp for p over frame; `out` written on success. */
pc_status pc_algo_wide_solve(const pc_algo_wide_angle *p, pc_rect frame, pc_algo_wide_mesh *out,
                             pc_error *err);

/* Applies Adaptive Wide Angle to src over frame (textured triangles of the solved mesh; pixels
 * outside the warped image become transparent). `out` is initialised on success. */
pc_status pc_algo_wide_apply(const pc_surface *src, pc_rect frame, const pc_algo_wide_angle *p,
                             pc_algo_interp interp, pc_surface *out, pc_error *err);

#endif
