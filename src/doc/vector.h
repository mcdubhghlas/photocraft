/* Vector data: paths, shape layers, saved/work paths and vector masks (port of
 * crates/doc/src/vector.rs). Pure data; rasterisation is src/vector, PSD mapping src/io.
 *
 * Coordinates are document pixels (f64, y down), cubic Bezier. Segment i of a subpath runs from
 * knots[i].anchor via knots[i].out_ctrl and knots[i+1].in_ctrl to knots[i+1].anchor; closed
 * subpaths add the segment back to the first knot. Path operations follow Photoshop: the first
 * subpath always combines (pc_path_effective_op); JOIN subpaths belong to the component before
 * them. Conventions: doc/adjust.h. */
#ifndef PC_DOC_VECTOR_H
#define PC_DOC_VECTOR_H

#include "doc/fill.h"
#include "geom/geom.h"
#include "raster/surface.h"

typedef struct pc_knot {
    pc_point anchor;
    pc_point in_ctrl;  /* control point of the segment arriving here */
    pc_point out_ctrl; /* control point of the segment leaving here */
    bool smooth;
} pc_knot;

pc_knot pc_knot_corner(double x, double y);
pc_knot pc_knot_smooth(pc_point anchor, pc_point in_ctrl, pc_point out_ctrl);
pc_knot pc_knot_transform(pc_knot k, pc_affine a);
bool pc_knot_eq(const pc_knot *a, const pc_knot *b);

typedef enum pc_path_op {
    PC_PATH_COMBINE,
    PC_PATH_SUBTRACT,
    PC_PATH_INTERSECT,
    PC_PATH_EXCLUDE,
    PC_PATH_JOIN
} pc_path_op;

typedef enum pc_fill_rule { PC_FILL_RULE_NONZERO, PC_FILL_RULE_EVENODD } pc_fill_rule;

typedef struct pc_subpath {
    bool closed;
    pc_knot *knots;
    size_t nknots;
    pc_path_op op;
} pc_subpath;

void pc_subpath_fini(pc_subpath *s);
pc_status pc_subpath_clone(const pc_subpath *s, pc_subpath *out, pc_error *err);
bool pc_subpath_eq(const pc_subpath *a, const pc_subpath *b);
/* Closed polygon / open polyline through n points (corner knots, op COMBINE). */
pc_status pc_subpath_polygon(const double (*pts)[2], size_t n, pc_subpath *out, pc_error *err);
pc_status pc_subpath_polyline(const double (*pts)[2], size_t n, pc_subpath *out, pc_error *err);
/* Number of cubic segments (0 below two knots), and segment i as [p0, c1, c2, p1]. */
size_t pc_subpath_segment_count(const pc_subpath *s);
void pc_subpath_segment(const pc_subpath *s, size_t i, pc_point out[4]);

typedef struct pc_path {
    pc_subpath *subpaths;
    size_t nsubpaths;
    pc_fill_rule fill_rule;
    bool inverted; /* the final area is inverted */
} pc_path;

void pc_path_fini(pc_path *p);
pc_status pc_path_clone(const pc_path *p, pc_path *out, pc_error *err);
bool pc_path_eq(const pc_path *a, const pc_path *b);
bool pc_path_is_empty(const pc_path *p); /* every subpath without knots */
pc_path_op pc_path_effective_op(const pc_path *p, size_t i);

typedef struct pc_index_range {
    size_t start;
    size_t end; /* exclusive */
} pc_index_range;

/* Shape components: ranges of subpaths filled together (malloc'd; free()). *out is NULL when
 * there are no subpaths. */
pc_status pc_path_components(const pc_path *p, pc_index_range **out, size_t *n, pc_error *err);
pc_status pc_path_transform(const pc_path *p, pc_affine a, pc_path *out, pc_error *err);
/* Bounds of every anchor and control point {x0, y0, x1, y1}; false when there are no knots. */
bool pc_path_control_bounds(const pc_path *p, double out[4]);

/* A Paths panel entry. */
typedef struct pc_named_path {
    pc_str name;
    pc_path path;
    pc_blob *psd_raw; /* original PSD path resource; NULL = None */
} pc_named_path;

void pc_named_path_fini(pc_named_path *p);
pc_status pc_named_path_clone(const pc_named_path *p, pc_named_path *out, pc_error *err);
bool pc_named_path_eq(const pc_named_path *a, const pc_named_path *b);

/* The document's clipping path (PSD resource 2999). */
typedef struct pc_clipping_path {
    pc_str name;
    float flatness;
} pc_clipping_path;

void pc_clipping_path_fini(pc_clipping_path *c);
pc_status pc_clipping_path_clone(const pc_clipping_path *c, pc_clipping_path *out, pc_error *err);
bool pc_clipping_path_eq(const pc_clipping_path *a, const pc_clipping_path *b);

/* A layer's vector mask. */
typedef struct pc_vector_mask {
    pc_path path;
    bool enabled;
    bool linked;
    float density;
    float feather;
} pc_vector_mask;

/* enabled, linked, density 1, feather 0; takes over *path (moved; never fails). */
void pc_vector_mask_init(pc_vector_mask *out, pc_path *path);
void pc_vector_mask_fini(pc_vector_mask *m);
pc_status pc_vector_mask_clone(const pc_vector_mask *m, pc_vector_mask *out, pc_error *err);
bool pc_vector_mask_eq(const pc_vector_mask *a, const pc_vector_mask *b);

typedef enum pc_stroke_align { PC_STROKE_ALIGN_INSIDE, PC_STROKE_ALIGN_CENTER, PC_STROKE_ALIGN_OUTSIDE } pc_stroke_align;
typedef enum pc_line_cap { PC_LINE_CAP_BUTT, PC_LINE_CAP_ROUND, PC_LINE_CAP_SQUARE } pc_line_cap;
typedef enum pc_line_join { PC_LINE_JOIN_MITER, PC_LINE_JOIN_ROUND, PC_LINE_JOIN_BEVEL } pc_line_join;

/* Shape stroke options (not the layer-style stroke). */
typedef struct pc_shape_stroke {
    float width;
    pc_fill paint;
    float opacity;
    pc_stroke_align align;
    pc_line_cap cap;
    pc_line_join join;
    float miter_limit;
    float *dashes; /* multiples of the width; empty = solid */
    size_t ndashes;
    float dash_offset;
} pc_shape_stroke;

/* Width 3, solid black, opacity 1, centre, butt, miter, limit 100, solid. */
void pc_shape_stroke_default(pc_shape_stroke *out);
void pc_shape_stroke_fini(pc_shape_stroke *s);
pc_status pc_shape_stroke_clone(const pc_shape_stroke *s, pc_shape_stroke *out, pc_error *err);
bool pc_shape_stroke_eq(const pc_shape_stroke *a, const pc_shape_stroke *b);

typedef enum pc_live_shape_kind {
    PC_LIVE_RECT,
    PC_LIVE_ELLIPSE,
    PC_LIVE_POLYGON,
    PC_LIVE_LINE
} pc_live_shape_kind;

/* The parametric shape a path came from. Fields not used by `kind` are ignored. */
typedef struct pc_live_shape {
    pc_live_shape_kind kind;
    double rect[4];  /* RECT, ELLIPSE, POLYGON: x, y, w, h */
    double radii[4]; /* RECT: tl, tr, br, bl */
    uint32_t sides;  /* POLYGON */
    double star_ratio;
    double from[2]; /* LINE */
    double to[2];
    double weight;
} pc_live_shape;

bool pc_live_shape_eq(const pc_live_shape *a, const pc_live_shape *b);

/* A shape layer: a filled and/or stroked path. */
typedef struct pc_shape_layer {
    pc_path path;
    bool has_fill; /* None = fill "none" */
    pc_fill fill;
    bool has_stroke;
    pc_shape_stroke stroke;
    bool has_live;
    pc_live_shape live;
    bool has_cache;
    pc_surface cache;
    pc_blob *psd_raw; /* vsms/vmsk block data; NULL = None */
} pc_shape_layer;

void pc_shape_layer_fini(pc_shape_layer *s);
pc_status pc_shape_layer_clone(const pc_shape_layer *s, pc_shape_layer *out, pc_error *err);
bool pc_shape_layer_eq(const pc_shape_layer *a, const pc_shape_layer *b);

#endif
