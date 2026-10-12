/* Private to the SVG rasteriser (svg_raster.c, svg_ts_*.c): a port of the parts of tiny-skia 0.11.4 /
 * tiny-skia-path 0.11.4 that decide which pixels a path covers and by how much, so resvg 0.45's
 * edge anti-aliasing is reproduced bit for bit:
 *
 *   svg_ts_geom.c    scalar / point helpers, path_geometry (both crates), conics as quads, path64's
 *                    cubic-line intersection, PathBuilder, Rect::from_points, path transforms
 *   svg_ts_edge.c    fixed_point, edge.rs (line / quad / cubic edges), edge_builder.rs,
 *                    edge_clipper.rs, line_clipper.rs
 *   svg_ts_scan.c    scan/path.rs (edge walking), scan/path_aa.rs (4x supersampling, alpha runs),
 *                    fill_rect / fill_rect_aa, and painter.rs's fill_path (DrawTiler)
 *   svg_ts_hair.c    scan/hairline.rs, scan/hairline_aa.rs
 *   svg_ts_stroke.c  stroker.rs
 *   svg_ts_dash.c    dash.rs
 *
 * Numbers follow the reference exactly: f32 geometry, 26.6 / 16.16 / 24.8 fixed point with
 * wrapping i32 arithmetic where release Rust wraps, saturating float -> int casts. Where the
 * reference would panic (unwrap, index out of bounds, division overflow) the port draws nothing
 * or skips the offending piece instead. Work and memory are bounded (TS_MAX_* below).
 *
 * Coverage leaves through sv_spans (svg_internal.h): every blitter call of the reference becomes
 * span calls with the same pixels, alpha and order, so callers that blend span by span see the
 * same compositing as tiny-skia's RasterPipelineBlitter. Not thread-safe per object; no globals. */
#ifndef PC_IO_SVG_TS_H
#define PC_IO_SVG_TS_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/num.h"
#include "base/rmath.h"
#include "io/svg_internal.h"

/* Points of one built path (stroke outlines, dashes, transformed copies): 8 Mi points, 64 MiB. */
#define TS_MAX_POINTS ((size_t)1 << 23)
/* Edges of one fill: 2 Mi (about 130 MiB). */
#define TS_MAX_EDGES ((size_t)1 << 21)

#define TS_NEARLY_ZERO (1.0f / 4096.0f)
#define TS_ROOT_2_OVER_2 0.707106781f
#define TS_SCALAR_MAX 3.402823466e+38f

/* ---------------------------------------------------------------- scalars and points */

static inline bool ts_nearly_zero(float v)
{
    return fabsf(v) <= TS_NEARLY_ZERO;
}

static inline sv_pt ts_pt(float x, float y)
{
    sv_pt p;
    p.x = x;
    p.y = y;
    return p;
}

static inline sv_pt ts_add(sv_pt a, sv_pt b)
{
    return ts_pt(a.x + b.x, a.y + b.y);
}

static inline sv_pt ts_sub(sv_pt a, sv_pt b)
{
    return ts_pt(a.x - b.x, a.y - b.y);
}

static inline sv_pt ts_neg(sv_pt a)
{
    return ts_pt(-a.x, -a.y);
}

static inline sv_pt ts_scaled(sv_pt a, float s)
{
    return ts_pt(a.x * s, a.y * s);
}

static inline float ts_dot(sv_pt a, sv_pt b)
{
    return a.x * b.x + a.y * b.y;
}

static inline float ts_cross(sv_pt a, sv_pt b)
{
    return a.x * b.y - a.y * b.x;
}

static inline bool ts_pt_eq(sv_pt a, sv_pt b)
{
    return a.x == b.x && a.y == b.y;
}

static inline bool ts_pt_is_finite(sv_pt p)
{
    return isfinite(p.x * p.y);
}

static inline bool ts_can_normalize(sv_pt p)
{
    return isfinite(p.x) && isfinite(p.y) && (p.x != 0.0f || p.y != 0.0f);
}

static inline float ts_dist_sqd(sv_pt a, sv_pt b)
{
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

float ts_length(sv_pt p);
/* Point::set_length_from: (x, y) scaled to `length`; false (and zero) when that isn't finite. */
bool ts_set_length_from(sv_pt *p, float x, float y, float length);
static inline bool ts_set_length(sv_pt *p, float length)
{
    return ts_set_length_from(p, p->x, p->y, length);
}
static inline bool ts_normalize(sv_pt *p)
{
    return ts_set_length_from(p, p->x, p->y, 1.0f);
}

/* i32 arithmetic as release Rust does it (two's complement wrapping). */
static inline int32_t ts_wadd(int32_t a, int32_t b)
{
    return pc_i32_wrapping_add(a, b);
}
static inline int32_t ts_wsub(int32_t a, int32_t b)
{
    return pc_i32_wrapping_sub(a, b);
}
static inline int32_t ts_wmul(int32_t a, int32_t b)
{
    return pc_i32_wrapping_mul(a, b);
}
static inline int32_t ts_shl(int32_t v, int32_t s) /* math::left_shift */
{
    return pc_i32_from_bits((uint32_t)v << (s & 31));
}
static inline int32_t ts_abs(int32_t v) /* i32::abs in release (MIN stays MIN) */
{
    return v < 0 ? ts_wsub(0, v) : v;
}

/* ---------------------------------------------------------------- rects */

/* Rect::from_ltrb: finite, ordered, with a representable width and height. */
bool ts_rect_ltrb(float l, float t, float r, float b, sv_rect *out);
/* Rect::from_points (the reference's partial finiteness check included). */
bool ts_rect_from_points(const sv_pt *p, size_t n, sv_rect *out);

/* IntRect / ScreenIntRect as x, y, w, h (w, h >= 1; x + w and y + h fit i32). */
typedef struct ts_irect {
    int32_t x, y;
    uint32_t w, h;
} ts_irect;

bool ts_irect_xywh(int32_t x, int32_t y, uint32_t w, uint32_t h, ts_irect *out);
bool ts_irect_ltrb(int32_t l, int32_t t, int32_t r, int32_t b, ts_irect *out);
bool ts_irect_intersect(ts_irect a, ts_irect b, ts_irect *out);
bool ts_irect_contains(ts_irect a, ts_irect b);
static inline int32_t ts_irect_r(ts_irect r)
{
    return (int32_t)((int64_t)r.x + r.w);
}
static inline int32_t ts_irect_b(ts_irect r)
{
    return (int32_t)((int64_t)r.y + r.h);
}
/* Rect::round_out */
bool ts_rect_round_out(sv_rect r, ts_irect *out);

/* ---------------------------------------------------------------- paths */

/* PathBuilder (malloc'd; `failed` after an allocation failure or TS_MAX_POINTS). */
typedef struct ts_pb {
    uint8_t *verbs;
    sv_pt *pts;
    size_t nv, capv, np, capp;
    size_t last_move_to;
    bool move_to_required;
    bool failed; /* out of memory or over TS_MAX_POINTS */
    bool limit;  /* failed because of TS_MAX_POINTS */
} ts_pb;

/* A finished path: a view of an sv_path or a builder's arrays. */
typedef struct ts_path {
    const uint8_t *verbs;
    const sv_pt *pts;
    size_t nv, np;
    sv_rect bounds;
} ts_path;

void ts_pb_init(ts_pb *b);
void ts_pb_fini(ts_pb *b);
void ts_pb_clear(ts_pb *b);
void ts_pb_move_to(ts_pb *b, float x, float y);
void ts_pb_line_to(ts_pb *b, float x, float y);
void ts_pb_quad_to(ts_pb *b, float x1, float y1, float x, float y);
void ts_pb_cubic_to(ts_pb *b, float x1, float y1, float x2, float y2, float x, float y);
void ts_pb_conic_to(ts_pb *b, float x1, float y1, float x, float y, float w);
void ts_pb_close(ts_pb *b);
bool ts_pb_last_point(const ts_pb *b, sv_pt *out);
void ts_pb_set_last_point(ts_pb *b, sv_pt p);
bool ts_pb_zero_length_since(const ts_pb *b, size_t start);
void ts_pb_push_circle(ts_pb *b, float x, float y, float r);
void ts_pb_push_builder(ts_pb *b, const ts_pb *other);
void ts_pb_reverse_path_to(ts_pb *b, const ts_pb *other);
/* PathBuilder::finish: false for an empty path, a lone move-to or non-finite bounds. The view
 * borrows b's arrays. */
bool ts_pb_finish(const ts_pb *b, ts_path *out);

static inline ts_path ts_path_of(const sv_path *p)
{
    ts_path v;
    v.verbs = p->verbs;
    v.pts = p->pts;
    v.nv = p->nverbs;
    v.np = p->npts;
    v.bounds = p->bounds;
    return v;
}

/* Transform::map_points */
void ts_map_points(sv_ts t, sv_pt *p, size_t n);
/* A copy of p in b mapped by t with recomputed bounds (Path::transform); false when the bounds
 * aren't finite (or out of memory, b->failed). */
bool ts_path_transform(const ts_path *p, sv_ts t, ts_pb *b, ts_path *out);

/* PathSegmentsIter */
typedef struct ts_seg {
    sv_verb verb;
    sv_pt p[3];
} ts_seg;

typedef struct ts_iter {
    const ts_path *path;
    size_t vi, pi;
    bool auto_close;
    sv_pt last_move_to, last_point;
} ts_iter;

void ts_iter_init(ts_iter *it, const ts_path *p);
bool ts_iter_next(ts_iter *it, ts_seg *out);
/* The verb after the current one, or -1 at the end (next_verb). */
int ts_iter_next_verb(const ts_iter *it);

/* ---------------------------------------------------------------- path_geometry */

bool ts_valid_unit_divide(float numer, float denom, float *t);
size_t ts_find_unit_quad_roots(float a, float b, float c, float roots[3]);
void ts_chop_quad_at(const sv_pt src[3], float t, sv_pt dst[5]);
void ts_chop_cubic_at2(const sv_pt src[4], float t, sv_pt dst[7]);
void ts_chop_cubic_at(const sv_pt src[4], const float *t, size_t nt, sv_pt *dst);
size_t ts_chop_quad_at_x_extrema(const sv_pt src[3], sv_pt dst[5]);
size_t ts_chop_quad_at_y_extrema(const sv_pt src[3], sv_pt dst[5]);
size_t ts_chop_cubic_at_x_extrema(const sv_pt src[4], sv_pt dst[10]);
size_t ts_chop_cubic_at_y_extrema(const sv_pt src[4], sv_pt dst[10]);
size_t ts_find_cubic_extrema(float a, float b, float c, float d, float t[3]);
size_t ts_chop_cubic_at_max_curvature(const sv_pt src[4], sv_pt dst[13]);
float ts_find_quad_max_curvature(const sv_pt src[3]);
sv_pt ts_eval_quad_at(const sv_pt src[3], float t);
sv_pt ts_eval_quad_tangent_at(const sv_pt src[3], float t);
size_t ts_find_cubic_max_curvature(const sv_pt src[4], float t[3]);
sv_pt ts_eval_cubic_pos_at(const sv_pt src[4], float t);
sv_pt ts_eval_cubic_tangent_at(const sv_pt src[4], float t);
sv_pt ts_eval_cubic_derivative(const sv_pt src[4], float t);
size_t ts_find_cubic_inflections(const sv_pt src[4], float t[3]);
bool ts_find_cubic_cusp(const sv_pt src[4], float *t);
/* path64-based chop of a monotonic cubic at x (or y) = v; false when no root is found. */
bool ts_chop_mono_cubic_at_x(const sv_pt src[4], float x, sv_pt dst[7]);
bool ts_chop_mono_cubic_at_y(const sv_pt src[4], float y, sv_pt dst[7]);

/* A conic as tiny-skia stores it. */
typedef struct ts_conic {
    sv_pt p[3];
    float w;
} ts_conic;

/* Conic::build_unit_arc: number of conics written to dst (0 = None). */
size_t ts_build_unit_arc(sv_pt start, sv_pt stop, bool ccw, sv_ts user, ts_conic dst[5]);

/* ---------------------------------------------------------------- blitting */

/* The edge walker's span sink (Blitter::blit_h). */
typedef void (*ts_blit_h_fn)(void *ctx, uint32_t x, uint32_t y, uint32_t w);

/* A pixel rectangle the scan converters clip to (ScreenIntRect). */
typedef struct ts_clip {
    uint32_t x, y, w, h;
} ts_clip;

/* scan/path.rs fill_path_impl: walks the edges of path between rows start_y..stop_y (pixel rows,
 * shifted up by `shift` for supersampling) calling h. False when out of memory or over a cap. */
pc_status ts_fill_path_impl(const ts_path *path, bool evenodd, ts_clip clip, int32_t start_y, int32_t stop_y,
                            int32_t shift, bool contained, ts_blit_h_fn h, void *ctx);

/* scan::path::fill_path and scan::path_aa::fill_path into out (offset by ox, oy). */
pc_status ts_scan_fill(const ts_path *path, bool evenodd, bool aa, ts_clip clip, const sv_spans *out,
                       uint32_t ox, uint32_t oy);
/* scan::fill_rect / scan::fill_rect_aa */
void ts_scan_fill_rect(sv_rect r, bool aa, ts_clip clip, const sv_spans *out);
/* scan::hairline(_aa)::stroke_path */
void ts_scan_hairline(const ts_path *path, sv_cap cap, bool aa, ts_clip clip, const sv_spans *out,
                      uint32_t ox, uint32_t oy);

/* Emits one span (skipping empty ones and alpha 0). */
static inline void ts_emit(const sv_spans *s, uint32_t x, uint32_t y, uint32_t w, uint8_t a)
{
    if (w > 0 && a != 0) {
        s->span(s->ctx, x, y, w, a);
    }
}

/* ---------------------------------------------------------------- stroking and dashing */

typedef struct ts_stroke {
    float width, miter_limit;
    sv_cap cap;
    sv_join join;
} ts_stroke;

/* PathStroker::stroke into out (cleared first): PC_OK and *ok when there is an outline,
 * PC_ERR_NOMEM / PC_ERR_LIMIT on failure. */
pc_status ts_stroke_path(const ts_path *path, const ts_stroke *s, float res_scale, ts_pb *out, bool *ok);
/* PathStroker::compute_resolution_scale */
float ts_res_scale(sv_ts t);

/* StrokeDash: false when the array / offset is not usable (no dashing). */
typedef struct ts_dash {
    const float *array;
    size_t n;
    float offset, interval_len, first_len;
    size_t first_index;
} ts_dash;

bool ts_dash_new(const float *array, size_t n, float offset, ts_dash *out);
/* Path::dash into out: *ok false when the reference returns None (nothing is drawn). */
pc_status ts_dash_path(const ts_path *path, const ts_dash *d, float res_scale, ts_pb *out, bool *ok);

#endif
