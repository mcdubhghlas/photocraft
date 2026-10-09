/* Warp geometry for Edit > Transform > Warp, smart-object warps and Warp Text
 * (port of crates/geom/src/warp.rs).
 *
 * Two kinds of warp, both forward maps from a source box to document space:
 *  - styles: Photoshop's fifteen presets plus bend and horizontal/vertical distortion (the PSD
 *    `warp` descriptor's warpStyle/warpValue/warpPerspective*). The shapes are the reference's own
 *    closed-form approximations (observed behaviour, not Adobe's formulas); every style is the
 *    identity at bend 0 and continuous in the bend.
 *  - custom meshes (pc_mesh): a grid of bicubic Bezier patches. Splits add patch rows/columns by
 *    exact de Casteljau subdivision, so splitting never changes the shape.
 * Pure geometry; resampling belongs to src/algo. Results match the reference bit for bit.
 *
 * Ownership: pc_mesh and pc_warp own their arrays; release them with pc_mesh_fini / pc_warp_fini
 * (both accept zeroed or already-finalised values). Functions that build a mesh write it to *out
 * only on success. Not synchronised; pure functions are thread-safe. */
#ifndef PC_GEOM_WARP_H
#define PC_GEOM_WARP_H

#include <stdbool.h>
#include <stddef.h>

#include "base/status.h"
#include "geom.h"

/* Declaration order matches the reference (none, custom, then the presets in menu order). */
typedef enum pc_warp_style {
    PC_WARP_NONE,
    PC_WARP_CUSTOM,
    PC_WARP_ARC,
    PC_WARP_ARC_LOWER,
    PC_WARP_ARC_UPPER,
    PC_WARP_ARCH,
    PC_WARP_BULGE,
    PC_WARP_SHELL_LOWER,
    PC_WARP_SHELL_UPPER,
    PC_WARP_FLAG,
    PC_WARP_WAVE,
    PC_WARP_FISH,
    PC_WARP_RISE,
    PC_WARP_FISHEYE,
    PC_WARP_INFLATE,
    PC_WARP_SQUEEZE,
    PC_WARP_TWIST,
    PC_WARP_STYLE_COUNT
} pc_warp_style;

/* The fifteen presets in Photoshop's menu order. */
extern const pc_warp_style PC_WARP_PRESETS[15];

const char *pc_warp_style_psd_name(pc_warp_style s); /* "warpArcLower" */
const char *pc_warp_style_id(pc_warp_style s);       /* "arcLower" */
const char *pc_warp_style_label(pc_warp_style s);    /* "Arc Lower" */
bool pc_warp_style_is_preset(pc_warp_style s);
/* Parse a short id, PSD name or label: ASCII case-insensitive, Unicode white space ignored.
 * text is UTF-8 of `len` bytes. False (out untouched) when nothing matches. */
bool pc_warp_style_parse(const char *text, size_t len, pc_warp_style *out);

/* A preset prepared for one box. Plain data. */
typedef struct pc_style_warp {
    pc_warp_style style;
    double b, hd, vd; /* bend, horizontal / vertical distortion, -1..1 */
    bool vertical;    /* warp along the vertical axis */
    double cx, cy;    /* box centre */
    double hw, hh;    /* half width / height */
} pc_style_warp;

/* Prepare style with bend/distortions in percent (-100..100, non-finite = 0) for
 * bounds = {x0, y0, x1, y1}. False (out untouched) for a non-preset, an all-zero warp or an
 * empty box. */
bool pc_style_warp_init(pc_style_warp *out, pc_warp_style style, double bend, double h_distort, double v_distort,
                        bool vertical, const double bounds[4]);
/* Longest segment (px) worth mapping unsplit. */
double pc_style_warp_max_segment(const pc_style_warp *w);
pc_point pc_style_warp_apply(const pc_style_warp *w, double x, double y);

/* A grid of bicubic Bezier patches over the unit square of a source box.
 *
 * us[0..nus) / vs[0..nvs) are patch boundaries in normalised coordinates (strictly increasing,
 * first 0, last 1). points is a (3*(nus-1)+1) x (3*(nvs-1)+1) grid, row-major, in output
 * coordinates; neighbouring patches share their edge row. Treat the fields as read-only and
 * change meshes through these functions. */
typedef struct pc_mesh {
    double *us;
    size_t nus;
    double *vs;
    size_t nvs;
    pc_point *points;
    size_t npoints;
} pc_mesh;

/* Normalised (s, t) -> output point. */
typedef pc_point (*pc_mesh_map_fn)(void *ctx, double s, double t);
typedef pc_point (*pc_point_map_fn)(void *ctx, pc_point p);

void pc_mesh_fini(pc_mesh *m);
pc_status pc_mesh_clone(const pc_mesh *m, pc_mesh *out, pc_error *err);

/* The identity mesh over bounds with cols x rows patches, each clamped to 1..64. */
pc_status pc_mesh_identity(const double bounds[4], size_t cols, size_t rows, pc_mesh *out, pc_error *err);

/* Fit a mesh with boundaries us/vs (copied; at least 2 each) to f by interpolating it at each
 * patch's thirds. Exact for bicubic (including affine) maps. */
pc_status pc_mesh_fit(pc_mesh_map_fn f, void *ctx, const double *us, size_t nus, const double *vs, size_t nvs,
                      pc_mesh *out, pc_error *err);

size_t pc_mesh_nx(const pc_mesh *m); /* control points per row (0 if nus < 1) */
size_t pc_mesh_ny(const pc_mesh *m); /* control point rows */
/* Control point (i, j); false if out of range. */
bool pc_mesh_point(const pc_mesh *m, size_t i, size_t j, pc_point *out);

/* Knots and point count consistent and every point finite. Check meshes read from files. */
bool pc_mesh_is_valid(const pc_mesh *m);

/* The surface at normalised (s, t) (clamped to 0..1). A structurally inconsistent mesh (knot
 * count < 2 or wrong point count) yields (NaN, NaN). */
pc_point pc_mesh_eval(const pc_mesh *m, double s, double t);

/* Split the patch column containing s at s (exact). *did_split is false, and the mesh
 * unchanged, when s is on or too close to an existing boundary. */
pc_status pc_mesh_split_u(pc_mesh *m, double s, bool *did_split, pc_error *err);
pc_status pc_mesh_split_v(pc_mesh *m, double t, bool *did_split, pc_error *err);
/* Remove interior boundary k (1..nus-2), merging its two patches; exact for unedited splits.
 * *removed is false (mesh unchanged) for a k out of range. */
pc_status pc_mesh_remove_split_u(pc_mesh *m, size_t k, bool *removed, pc_error *err);
pc_status pc_mesh_remove_split_v(pc_mesh *m, size_t k, bool *removed, pc_error *err);

/* A copy with f applied to every control point (exact for affine maps). */
pc_status pc_mesh_map_points(const pc_mesh *m, pc_point_map_fn f, void *ctx, pc_mesh *out, pc_error *err);

/* Normalised (s, t) whose surface point is nearest p (coarse search, then Newton steps). */
void pc_mesh_param_at(const pc_mesh *m, pc_point p, double *s, double *t);

/* Bounding box {x0, y0, x1, y1} of the control points (the surface lies inside it). */
void pc_mesh_control_bounds(const pc_mesh *m, double out[4]);

/* A complete warp: what Photoshop's `warp` descriptor stores, plus the custom mesh. */
typedef struct pc_warp {
    pc_warp_style style;
    double bend;      /* percent, -100..100, presets only */
    double h_distort; /* percent */
    double v_distort; /* percent */
    bool vertical;
    double bounds[4]; /* source box {x0, y0, x1, y1} */
    bool has_mesh;
    pc_mesh mesh; /* owned; meaningful when has_mesh (custom warps) */
} pc_warp;

/* Initialisers (no allocation). pc_warp_custom takes ownership of *mesh and empties it. */
void pc_warp_none(pc_warp *w, const double bounds[4]);
void pc_warp_preset(pc_warp *w, pc_warp_style style, double bend, const double bounds[4]);
void pc_warp_custom(pc_warp *w, pc_mesh *mesh, const double bounds[4]);
void pc_warp_fini(pc_warp *w);

/* Whether every point of the box maps onto itself (within 1e-9 px). */
bool pc_warp_is_identity(const pc_warp *w);
/* Map a source point to output space. */
pc_point pc_warp_map(const pc_warp *w, double x, double y);
/* The warp as a control mesh: presets are fitted with cols x rows patches (clamped 1..64);
 * custom warps return a copy of their mesh. */
pc_status pc_warp_to_mesh(const pc_warp *w, size_t cols, size_t rows, pc_mesh *out, pc_error *err);
/* The warp followed by the affine map m = {a, b, c, d, e, f}, as a custom warp. *out must be
 * empty (fresh or finalised) or the same object as w, which is then replaced. */
pc_status pc_warp_then_affine(const pc_warp *w, const double m[6], pc_warp *out, pc_error *err);
/* Approximate output bounds {x0, y0, x1, y1} (sampled on a 33 x 33 grid). */
void pc_warp_output_bounds(const pc_warp *w, double out[4]);

#endif
