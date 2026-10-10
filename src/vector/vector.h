/* Vector rasterisation and geometry (port of crates/vector): Bezier flattening, an exact
 * area-coverage scanline rasteriser with Photoshop path operations, polygon stroking with
 * joins/caps/dashes, live-shape path generators, point-level path editing and hit testing,
 * and tracing a coverage mask back into a path ("Make Work Path").
 *
 * The data model (pc_path, pc_subpath, pc_knot, pc_shape_layer, pc_vector_mask, ...) lives in
 * doc/vector.h; this module only reads it (and builds new paths). Public names are pc_vec_*
 * so they never clash with the model's pc_path_* functions.
 *
 * Conventions (REWRITE_PLAN.md §4): fallible functions return pc_status and take an optional
 * pc_error; out-params are written only on success; every output that owns memory has a _fini
 * (NULL-safe, leaves the value zeroed so it may be re-used). Nothing ever aborts: non-finite
 * coordinates, empty or degenerate paths, absurd widths, tolerances and dash patterns all give
 * a result (usually empty) or an error, never a crash. Allocations sized by the input are capped
 * (PC_VEC_MAX_PIXELS for coverage buffers, PC_VEC_MAX_POINTS / PC_VEC_MAX_DASHES for generated
 * geometry) and fail with PC_ERR_LIMIT; the reference would abort on out-of-memory there
 * instead (§7).
 *
 * Thread safety: everything is pure or works on caller-owned values. A compiled rasteriser,
 * shape or mask may be rendered from several threads at once (render is const). Rendering of
 * large areas runs on pc_pool_global(); results do not depend on the thread count. */
#ifndef PC_VECTOR_VECTOR_H
#define PC_VECTOR_VECTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "color/color.h"
#include "doc/vector.h"
#include "geom/geom.h"
#include "raster/surface.h"

/* Default flattening tolerance in pixels (curve-to-chord distance). */
#define PC_VEC_DEFAULT_TOLERANCE 0.01

/* Handle length of a quarter-circle cubic, 4/3 * (sqrt(2) - 1). */
#define PC_VEC_KAPPA 0.5522847498307934

/* Largest coverage / RGBA buffer (pixels) one render call allocates. */
#define PC_VEC_MAX_PIXELS ((size_t)1 << 30)
/* Largest number of points one flatten / stroke / dash / trace result may hold (256 MiB of
 * coordinates). */
#define PC_VEC_MAX_POINTS ((size_t)1 << 24)
/* Largest number of dashes one polyline is split into. */
#define PC_VEC_MAX_DASHES ((size_t)1 << 20)

/* ------------------------------------------------------------------------------------------ */
/* Flattening (flatten.rs) */

/* A flattened subpath. knot[i] is true for points that are knots of the source path (joins
 * apply there) and false for points inserted while flattening a curve. */
typedef struct pc_vec_polyline {
    double (*pts)[2];
    bool *knot;
    size_t len;
    size_t cap;
    bool closed;
} pc_vec_polyline;

void pc_vec_polyline_fini(pc_vec_polyline *p);
/* Frees n polylines and the array itself (NULL-safe). */
void pc_vec_polylines_free(pc_vec_polyline *v, size_t n);
pc_status pc_vec_polyline_clone(const pc_vec_polyline *p, pc_vec_polyline *out, pc_error *err);

/* Flattens one subpath with chord tolerance tol (px). Closed subpaths do not repeat the first
 * point at the end. */
pc_status pc_vec_flatten_subpath(const pc_subpath *s, double tol, pc_vec_polyline *out, pc_error *err);
/* Flattens every subpath: *out is a malloc'd array of p->nsubpaths polylines (NULL when there
 * are none); free with pc_vec_polylines_free. */
pc_status pc_vec_flatten_path(const pc_path *p, double tol, pc_vec_polyline **out, size_t *n, pc_error *err);

/* ------------------------------------------------------------------------------------------ */
/* Polygons (rasteriser input, stroke output) */

typedef struct pc_vec_poly {
    double (*pts)[2];
    size_t len;
} pc_vec_poly;

/* A growable list of owned polygons. Zero-initialised = empty. */
typedef struct pc_vec_polys {
    pc_vec_poly *v;
    size_t len;
    size_t cap;
} pc_vec_polys;

void pc_vec_polys_fini(pc_vec_polys *p);

/* ------------------------------------------------------------------------------------------ */
/* Rasteriser (raster.rs) */

/* A compiled set of polygons, ready to render exact anti-aliased coverage for any rectangle:
 * each component is filled with its rule and folded into the result with its path operation
 * (the first one always combines), and the final result may be inverted. Fields are private;
 * a zero-initialised value is NOT valid, use pc_vec_rasterizer_init. */
typedef struct pc_vec_raster_edge {
    double x0, y0, y1, dxdy;
    int32_t dir; /* +1 when the source segment runs downwards (y increasing), -1 upwards */
    uint32_t comp;
} pc_vec_raster_edge;

typedef struct pc_vec_component {
    pc_path_op op;
    pc_fill_rule rule;
} pc_vec_component;

typedef struct pc_vec_rasterizer {
    pc_vec_raster_edge *edges; /* sorted by y0 (stable) */
    size_t nedges;
    size_t edges_cap;
    pc_vec_component *comps;
    size_t ncomps;
    size_t comps_cap;
    bool inverted;
    bool has_bounds;
    double bounds[4];
    bool union_only; /* every component combines: inside = any component inside */
} pc_vec_rasterizer;

/* Empty rasteriser: renders nothing, or everything when inverted (never fails). */
void pc_vec_rasterizer_init(pc_vec_rasterizer *r, bool inverted);
void pc_vec_rasterizer_fini(pc_vec_rasterizer *r);
pc_status pc_vec_rasterizer_clone(const pc_vec_rasterizer *r, pc_vec_rasterizer *out, pc_error *err);
/* Adds a component made of closed polygons (open input is closed implicitly; non-finite
 * edges are skipped). On failure r is unchanged. */
pc_status pc_vec_rasterizer_add_component(pc_vec_rasterizer *r, const pc_vec_poly *polys, size_t n,
                                          pc_path_op op, pc_fill_rule rule, pc_error *err);
/* Pixel bounds of everything that may be covered; false (out untouched) when nothing is, and
 * for inverted rasterisers (the whole plane: callers clip to their own area). The upper edge
 * is clipped to INT32_MAX. */
bool pc_vec_rasterizer_pixel_bounds(const pc_vec_rasterizer *r, pc_rect *out);
bool pc_vec_rasterizer_is_inverted(const pc_vec_rasterizer *r);
/* Coverage (0..=1) of every pixel of rect, row-major; *out is malloc'd (free()), *len =
 * width * height (NULL/0 for an empty rect). PC_ERR_LIMIT above PC_VEC_MAX_PIXELS. */
pc_status pc_vec_rasterizer_render(const pc_vec_rasterizer *r, pc_rect rect, float **out, size_t *len,
                                   pc_error *err);
/* As render, into a caller buffer of len >= width * height values, sequentially. */
pc_status pc_vec_rasterizer_render_into(const pc_vec_rasterizer *r, pc_rect rect, float *out, size_t len,
                                        pc_error *err);

/* ------------------------------------------------------------------------------------------ */
/* Stroking (stroke.rs) */

typedef struct pc_vec_stroke_style {
    double width;
    pc_line_cap cap;
    pc_line_join join;
    double miter_limit; /* miter length / width, like SVG */
    double *dashes;     /* on, off, on, off... in px; empty = solid (malloc'd, owned) */
    size_t ndashes;
    double dash_offset;
} pc_vec_stroke_style;

/* Width 1, butt, miter, limit 4, solid. */
void pc_vec_stroke_style_default(pc_vec_stroke_style *out);
void pc_vec_stroke_style_fini(pc_vec_stroke_style *s);
/* Stroke geometry for a model shape stroke (its dash lengths are multiples of the width). */
pc_status pc_vec_stroke_style_from(const pc_shape_stroke *s, pc_vec_stroke_style *out, pc_error *err);

/* Stroke outline pieces for the polylines, all positively oriented: fill them together with
 * the non-zero rule. Appends to *out (on failure *out is left as it was). */
pc_status pc_vec_stroke_polygons(const pc_vec_polyline *lines, size_t n, const pc_vec_stroke_style *style,
                                 double tol, pc_vec_polys *out, pc_error *err);
/* Splits a polyline into dashes (open polylines); pattern alternates on/off lengths in px.
 * *out is malloc'd; free with pc_vec_polylines_free. */
pc_status pc_vec_dash_polyline(const pc_vec_polyline *pl, const double *pattern, size_t npattern,
                               double offset, pc_vec_polyline **out, size_t *n, pc_error *err);

/* ------------------------------------------------------------------------------------------ */
/* Compiling paths (lib.rs) */

/* The fill area of path: one component per subpath group (pc_path_components), filled with the
 * path's rule and folded with the subpath operations; open subpaths close implicitly. */
pc_status pc_vec_fill_rasterizer(const pc_path *path, double tol, pc_vec_rasterizer *out, pc_error *err);
/* The stroke outline of path (all subpaths, one non-zero component). */
pc_status pc_vec_stroke_rasterizer(const pc_path *path, const pc_vec_stroke_style *style, double tol,
                                   pc_vec_rasterizer *out, pc_error *err);
/* Fill coverage of path over rect at PC_VEC_DEFAULT_TOLERANCE (see rasterizer_render). */
pc_status pc_vec_path_coverage(const pc_path *path, pc_rect rect, float **out, size_t *len, pc_error *err);

/* Renders a rasteriser's coverage into a new single-channel surface (grayscale, no alpha, the
 * sample type of format) over clip. Outside clip the surface reads 0; inverted rasterisers
 * write their coverage everywhere inside clip. Only non-empty tiles are allocated. */
pc_status pc_vec_coverage_surface(const pc_vec_rasterizer *r, pc_pixel_format format, pc_rect clip,
                                  pc_surface *out, pc_error *err);

/* A vector mask's geometry and density, compiled once and reusable across rectangles. */
typedef struct pc_vec_compiled_mask {
    bool is_path;
    float constant;           /* !is_path: the value everywhere */
    pc_vec_rasterizer raster; /* is_path */
    bool has_density;
    float density;
} pc_vec_compiled_mask;

pc_status pc_vec_compiled_mask_new(const pc_vector_mask *m, pc_vec_compiled_mask *out, pc_error *err);
void pc_vec_compiled_mask_fini(pc_vec_compiled_mask *m);
/* Effective mask values over rect (density applied); *out malloc'd as rasterizer_render. */
pc_status pc_vec_compiled_mask_render(const pc_vec_compiled_mask *m, pc_rect rect, float **out, size_t *len,
                                      pc_error *err);
/* Effective vector-mask values over rect: all ones when disabled; like Photoshop an enabled
 * mask without any knot reveals everything (hides everything when inverted). */
pc_status pc_vec_mask_values(const pc_vector_mask *m, pc_rect rect, float **out, size_t *len, pc_error *err);

/* ------------------------------------------------------------------------------------------ */
/* Paint and shape layers (lib.rs) */

typedef enum pc_vec_paint_kind {
    PC_VEC_PAINT_NONE,
    PC_VEC_PAINT_SOLID,
    PC_VEC_PAINT_GRADIENT
} pc_vec_paint_kind;

typedef struct pc_vec_rgba_stop {
    float pos;
    float rgba[4]; /* straight sRGB + alpha */
} pc_vec_rgba_stop;

/* Colour source for fills and strokes. Gradient stops are sorted by location; layout is the
 * effective (angle, scale, offset) after Photoshop's whole-pixel end-point snapping; frame is
 * the layout frame {x0, y0, x1, y1}. */
typedef struct pc_vec_paint {
    pc_vec_paint_kind kind;
    float solid[4];
    pc_vec_rgba_stop *stops;
    size_t nstops;
    float *midpoints; /* per segment between sorted stops; missing = 0.5 */
    size_t nmidpoints;
    pc_opacity_stop *opacity_stops; /* sorted; empty = each stop's alpha */
    size_t nopacity_stops;
    pc_gradient_style style;
    float layout_angle;
    float layout_scale;
    float layout_offset[2];
    bool reverse;
    bool dither;
    double frame[4];
} pc_vec_paint;

/* Paint for a model fill, with gradients laid out over bounds (the layer's frame) or over
 * canvas when the fill is not aligned with the layer. Pattern fills give PAINT_NONE
 * (transparent): the pattern table lives above this module. */
pc_status pc_vec_paint_from_fill(const pc_fill *f, const double bounds[4], const double canvas[4],
                                 pc_vec_paint *out, pc_error *err);
void pc_vec_paint_fini(pc_vec_paint *p);
pc_status pc_vec_paint_clone(const pc_vec_paint *p, pc_vec_paint *out, pc_error *err);
/* Straight RGBA at pixel centre (x, y). */
void pc_vec_paint_at(const pc_vec_paint *p, double x, double y, float out[4]);

/* Everything needed to render a shape layer, compiled once. */
typedef struct pc_vec_compiled_shape {
    bool has_fill;
    pc_vec_rasterizer fill;
    pc_vec_paint fill_paint;
    bool has_stroke;
    pc_vec_rasterizer stroke;
    pc_vec_paint stroke_paint;
    float stroke_opacity;
    bool has_align; /* inside/outside stroke: clipped by the fill area */
    pc_vec_rasterizer align;
    bool align_inside;
    bool has_bounds;
    pc_rect bounds;
} pc_vec_compiled_shape;

/* Compiles shape at flattening tolerance tol; canvas is the frame gradients without "Align with
 * layer" are laid out in. */
pc_status pc_vec_compiled_shape_new(const pc_shape_layer *shape, double tol, pc_rect canvas,
                                    pc_vec_compiled_shape *out, pc_error *err);
void pc_vec_compiled_shape_fini(pc_vec_compiled_shape *s);
/* Pixel bounds of the rendering; false = nothing visible. */
bool pc_vec_compiled_shape_bounds(const pc_vec_compiled_shape *s, pc_rect *out);
/* Straight RGBA for every pixel of rect (malloc'd, *n = pixel count). */
pc_status pc_vec_compiled_shape_render_rgba(const pc_vec_compiled_shape *s, pc_rect rect, float (**out)[4],
                                            size_t *n, pc_error *err);
/* Renders into a new surface of format over clip (typically the canvas). */
pc_status pc_vec_compiled_shape_render(const pc_vec_compiled_shape *s, pc_pixel_format format, pc_rect clip,
                                       pc_surface *out, pc_error *err);
/* A shape layer's appearance (fill, then stroke over it) in format, clipped to clip, which also
 * lays out gradients whose fill is not aligned with the layer. */
pc_status pc_vec_render_shape(const pc_shape_layer *shape, pc_pixel_format format, pc_rect clip,
                              pc_surface *out, pc_error *err);

/* ------------------------------------------------------------------------------------------ */
/* Live shapes (shapes.rs). Knots run clockwise on screen (y down), starting at the top. */

pc_status pc_vec_shape_rect(double x, double y, double w, double h, pc_path *out, pc_error *err);
/* Clamps corner radii {tl, tr, br, bl} so adjacent radii never exceed a side (scaled down
 * uniformly, like CSS border-radius). */
void pc_vec_clamp_radii(double w, double h, const double radii[4], double out[4]);
pc_status pc_vec_shape_rounded_rect(double x, double y, double w, double h, const double radii[4],
                                    pc_path *out, pc_error *err);
/* Ellipse inscribed in the rectangle. */
pc_status pc_vec_shape_ellipse(double x, double y, double w, double h, pc_path *out, pc_error *err);
/* Regular polygon (a star when 0 < star_ratio < 1) inscribed in the rectangle's ellipse, first
 * vertex at the top; sides clamp to 3..=100. */
pc_status pc_vec_shape_polygon(double x, double y, double w, double h, uint32_t sides, double star_ratio,
                               pc_path *out, pc_error *err);
/* A line drawn as a filled bar of weight px from a to b. */
pc_status pc_vec_shape_line(const double a[2], const double b[2], double weight, pc_path *out, pc_error *err);
pc_status pc_vec_live_path(const pc_live_shape *s, pc_path *out, pc_error *err);

/* ------------------------------------------------------------------------------------------ */
/* Point editing and hit testing (edit.rs). Knots are addressed as {subpath, knot}; every edit
 * validates its indices first and fails with PC_ERR_INVALID and the reference's message,
 * leaving the path unchanged. */

typedef enum pc_vec_handle { PC_VEC_HANDLE_IN, PC_VEC_HANDLE_OUT } pc_vec_handle;

/* "in" / "out" (len bytes, no NUL needed); false for anything else. */
bool pc_vec_handle_parse(const char *s, size_t len, pc_vec_handle *out);
const char *pc_vec_handle_name(pc_vec_handle h);

typedef struct pc_vec_knot_ref {
    size_t sub;
    size_t knot;
} pc_vec_knot_ref;

typedef struct pc_vec_handle_ref {
    pc_vec_knot_ref knot;
    pc_vec_handle handle;
} pc_vec_handle_ref;

typedef enum pc_vec_hit_kind { PC_VEC_HIT_ANCHOR, PC_VEC_HIT_HANDLE, PC_VEC_HIT_SEGMENT } pc_vec_hit_kind;

/* SEGMENT: the segment leaving `knot`, at curve parameter t. */
typedef struct pc_vec_hit {
    pc_vec_hit_kind kind;
    pc_vec_knot_ref knot;
    pc_vec_handle handle; /* HANDLE */
    double t;             /* SEGMENT */
} pc_vec_hit;

/* Moves anchors (with their handles) by d; each knot moves once however often it is listed. */
pc_status pc_vec_move_anchors(pc_path *path, const pc_vec_knot_ref *anchors, size_t n, const double d[2],
                              pc_error *err);
/* Puts handle h of a knot at `to`. A smooth knot turns its other handle to stay collinear,
 * unless independent (Convert Point / Alt), which makes it a corner. */
pc_status pc_vec_move_handle(pc_path *path, pc_vec_knot_ref knot, pc_vec_handle h, pc_point to,
                             bool independent, pc_error *err);
/* Convert Point: out NULL gives a corner with retracted handles; otherwise a smooth knot whose
 * handles are *out and its mirror. */
pc_status pc_vec_convert_point(pc_path *path, pc_vec_knot_ref knot, const pc_point *out, pc_error *err);
/* Drags the segment leaving knot at curve parameter t by d (see edit.c). */
pc_status pc_vec_bend_segment(pc_path *path, pc_vec_knot_ref knot, double t, const double d[2],
                              pc_error *err);
/* Every anchor of subpath s (empty when there is none); *out malloc'd (free()). */
pc_status pc_vec_subpath_anchors(const pc_path *path, size_t s, pc_vec_knot_ref **out, size_t *n,
                                 pc_error *err);
/* Anchors inside the box {x0, y0, x1, y1} (any corner order), in path order. */
pc_status pc_vec_anchors_in(const pc_path *path, const double box[4], pc_vec_knot_ref **out, size_t *n,
                            pc_error *err);
/* The part of path within tol of p: one of the shown handles, else an anchor, else a segment;
 * the nearest wins within each kind. False when nothing is hit. Never allocates. */
bool pc_vec_hit_test(const pc_path *path, pc_point p, double tol, const pc_vec_handle_ref *handles,
                     size_t nhandles, pc_vec_hit *out);

/* ------------------------------------------------------------------------------------------ */
/* Tracing (trace.rs) */

/* Closed 0.5 iso-contours of values (row-major over rect, len >= width * height; values
 * outside rect read as `outside`) through pixel centres, inside on the left (y down). */
pc_status pc_vec_contours(const float *values, size_t len, pc_rect rect, float outside, pc_vec_polys *out,
                          pc_error *err);
/* Traces the contours and fits cubic Beziers within tolerance px: the first contour combines,
 * the rest exclude (the even-odd area of nested contours). */
pc_status pc_vec_trace_mask(const float *values, size_t len, pc_rect rect, float outside, double tolerance,
                            pc_path *out, pc_error *err);

#endif
