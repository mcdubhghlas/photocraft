/* Anti-aliased path rasteriser for type (port of crates/text/src/raster.rs): exact signed-area
 * accumulation (the font-rs technique) into f32 coverage, |winding| clamped to 1; curves are
 * flattened to lines by an outline pen that applies an affine map.
 *
 * Plain data and pure functions; a pc_coverage is single-owner (not synchronised). */
#ifndef PC_TEXT_RASTER_H
#define PC_TEXT_RASTER_H

#include <stdbool.h>
#include <stddef.h>

#include "base/status.h"
#include "text/font.h"

/* A 2D affine map [a c e; b d f], m = {a, b, c, d, e, f}. */
typedef struct pc_xform {
    double m[6];
} pc_xform;

#define PC_XFORM_IDENTITY ((pc_xform){{1.0, 0.0, 0.0, 1.0, 0.0, 0.0}})

void pc_xform_apply(const pc_xform *t, double x, double y, double *ox, double *oy);
/* t ∘ o (apply o first). */
pc_xform pc_xform_mul(const pc_xform *t, const pc_xform *o);
/* Largest scale factor (for the flattening tolerance). */
double pc_xform_max_scale(const pc_xform *t);

/* Receives flattened line segments (Rust's LineSink trait). */
typedef struct pc_line_sink {
    void (*line)(void *ctx, double x0, double y0, double x1, double y1);
    void *ctx;
} pc_line_sink;

/* Bounding box of everything drawn; non-finite points are ignored. */
typedef struct pc_bounds {
    bool has;
    double r[4]; /* x0, y0, x1, y1 */
} pc_bounds;

void pc_bounds_add(pc_bounds *b, double x, double y);
pc_line_sink pc_bounds_sink(pc_bounds *b);

/* Adds a transformed axis-aligned rectangle as four lines. */
void pc_rect_to(const pc_line_sink *sink, const pc_xform *t, double x0, double y0, double x1, double y1);

/* Coverage accumulation buffer of width x height pixels. */
typedef struct pc_coverage {
    size_t width, height;
    float *acc; /* width * height + 2 */
} pc_coverage;

/* PC_ERR_LIMIT when width * height overflows or exceeds the allocation cap, PC_ERR_NOMEM. */
pc_status pc_coverage_init(pc_coverage *c, size_t width, size_t height, pc_error *err);
void pc_coverage_fini(pc_coverage *c);
/* Adds a line segment (pixel coordinates, y down). Horizontal and non-finite segments are
 * ignored; everything is clipped to the buffer. */
void pc_coverage_line(pc_coverage *c, double x0, double y0, double x1, double y1);
pc_line_sink pc_coverage_sink(pc_coverage *c);
/* Resolves the accumulation into per-pixel coverage in 0..=1 (width * height values written
 * to out). */
void pc_coverage_finish(const pc_coverage *c, float *out);

/* Outline pen that maps font-space points (y up) through `xf` and flattens curves into `sink`
 * (tolerance 0.03 output px; at most 100 segments per curve). */
typedef struct pc_flatten_pen {
    const pc_line_sink *sink;
    pc_xform xf;
    double sx, sy; /* contour start */
    double lx, ly; /* current point */
    double tol;
} pc_flatten_pen;

void pc_flatten_pen_init(pc_flatten_pen *p, const pc_line_sink *sink, pc_xform xf);
/* The pc_outline_pen view of p (for pc_font_draw). */
pc_outline_pen pc_flatten_pen_outline(pc_flatten_pen *p);
void pc_flatten_pen_move_to(pc_flatten_pen *p, float x, float y);
void pc_flatten_pen_line_to(pc_flatten_pen *p, float x, float y);
void pc_flatten_pen_quad_to(pc_flatten_pen *p, float cx, float cy, float x, float y);
void pc_flatten_pen_curve_to(pc_flatten_pen *p, float c0x, float c0y, float c1x, float c1y, float x, float y);
void pc_flatten_pen_close(pc_flatten_pen *p);

#endif
