/* Filter > Liquify: a dense displacement field edited by brush strokes, then used to resample the
 * layer once (port of crates/algo/src/liquify.rs).
 *
 * The field stores, for every node of a regular grid (one node per `cell` document pixels), the
 * offset d such that output pixel p shows the source at p + d(p) (an inverse map, so applying it
 * never leaves holes). Strokes are plain data (tool, brush, points) applied in order; replaying
 * the same strokes on the same bounds gives the same field bit for bit, which is what makes the
 * filter a replayable command and a smart filter. Every tool is "where does the new output at p
 * come from in the current output": q = M(p), d'(p) = q + d(q) - p, sampled bilinearly from the
 * field as it was before the dab. Reconstruct scales d toward zero, Smooth relaxes it toward its
 * local mean, Freeze/Thaw paint a mask that scales every tool's strength down.
 *
 * pc_algo_liquify_apply upsamples the field with Catmull-Rom per pixel and samples the source
 * (premultiplied, bicubic) tile by tile in parallel; pc_algo_liquify_proxy is the cheap preview
 * (bilinear field and source on a downsampled premultiplied RGBA copy).
 *
 * Not a FilterParams filter: the engine's filter.liquify command and smart filters call it.
 *
 * Failure: NOMEM; PC_ERR_LIMIT when a field or proxy would exceed PC_MAX_ALLOC (the reference
 * aborts on allocation failure); PC_ERR_INVALID for a too-short output buffer (the reference
 * panics). Hostile numbers (NaN, inf, far-off points, huge brushes) behave as the reference does:
 * a segment walk skips the dabs that cannot reach the field, so it always returns.
 * Thread safety: fields and proxies are plain data; functions are re-entrant. Results do not
 * depend on the thread count. */
#ifndef PC_ALGO_LIQUIFY_H
#define PC_ALGO_LIQUIFY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <yyjson.h>

#include "algo/params.h"
#include "base/status.h"
#include "geom/geom.h"
#include "raster/surface.h"

/* The Liquify tools (serde camelCase names in pc_algo_liquify_tool_desc). */
typedef enum pc_algo_liquify_tool {
    PC_ALGO_LIQUIFY_FORWARD_WARP, /* default */
    PC_ALGO_LIQUIFY_RECONSTRUCT,
    PC_ALGO_LIQUIFY_SMOOTH,
    PC_ALGO_LIQUIFY_TWIRL_CW,
    PC_ALGO_LIQUIFY_TWIRL_CCW,
    PC_ALGO_LIQUIFY_PUCKER,
    PC_ALGO_LIQUIFY_BLOAT,
    PC_ALGO_LIQUIFY_PUSH_LEFT,
    PC_ALGO_LIQUIFY_FREEZE,
    PC_ALGO_LIQUIFY_THAW,
    PC_ALGO_LIQUIFY_LASSO_MASK,
    PC_ALGO_LIQUIFY_RECONSTRUCT_ALL, /* the Reconstruct... button: whole field toward zero by amount % */
    PC_ALGO_LIQUIFY_FREEZE_ALL,      /* Mask Options > Mask All / None / Invert All */
    PC_ALGO_LIQUIFY_THAW_ALL,
    PC_ALGO_LIQUIFY_INVERT_FREEZE
} pc_algo_liquify_tool;

#define PC_ALGO_LIQUIFY_TOOL_COUNT 15
extern const pc_algo_enum_desc pc_algo_liquify_tool_desc;
/* LiquifyTool::ALL: the 11 brush tools of the tool strip, in order. */
extern const pc_algo_liquify_tool pc_algo_liquify_tools_all[11];

const char *pc_algo_liquify_tool_label(pc_algo_liquify_tool t);  /* "Forward Warp", ...; "" if invalid */
bool pc_algo_liquify_tool_is_global(pc_algo_liquify_tool t);     /* whole-field operations */
bool pc_algo_liquify_tool_is_stationary(pc_algo_liquify_tool t); /* dabs while held still */

/* One brush stroke. Points are [x, y] or [x, y, pressure 0..1] in document pixels, stored flat:
 * point i is coords[off[i] .. off[i + 1]) (any length, as the JSON gave it; validate() refuses
 * lengths other than 2 and 3). off has npoints + 1 entries (NULL when npoints == 0). */
typedef struct pc_algo_liquify_stroke {
    pc_algo_liquify_tool tool;
    double size;     /* brush diameter px (default 100) */
    double density;  /* edge feathering 0..100 (50) */
    double pressure; /* strength 0..100 (100) */
    double rate;     /* speed of the stationary tools 0..100 (80) */
    double *coords;
    size_t *off;
    size_t npoints;
    /* reconstructAll: how much to restore 0..100; lassoMask: 1.0 / none freezes, 0.0 thaws. */
    bool has_amount;
    double amount;
} pc_algo_liquify_stroke;

/* LiquifyStroke::new: defaults, no points. */
pc_algo_liquify_stroke pc_algo_liquify_stroke_make(pc_algo_liquify_tool tool, double size);
void pc_algo_liquify_stroke_fini(pc_algo_liquify_stroke *s); /* NULL-safe; leaves no points */
/* Appends a point of n values (n may be anything; 2 or 3 are valid). */
pc_status pc_algo_liquify_stroke_push(pc_algo_liquify_stroke *s, const double *v, size_t n, pc_error *err);
pc_status pc_algo_liquify_stroke_copy(const pc_algo_liquify_stroke *s, pc_algo_liquify_stroke *out,
                                      pc_error *err);
bool pc_algo_liquify_stroke_eq(const pc_algo_liquify_stroke *a, const pc_algo_liquify_stroke *b);
/* Finite size 1..15000, every point 2 or 3 finite numbers; the reference's messages. */
pc_status pc_algo_liquify_stroke_validate(const pc_algo_liquify_stroke *s, pc_error *err);

/* serde_json::from_value::<Vec<LiquifyStroke>> (and of one stroke): serde's error texts. On
 * success *out / *n own the strokes (free with pc_algo_liquify_strokes_free). */
pc_status pc_algo_liquify_strokes_from_json(yyjson_val *v, pc_algo_liquify_stroke **out, size_t *n,
                                            pc_error *err);
pc_status pc_algo_liquify_stroke_from_json(yyjson_val *v, pc_algo_liquify_stroke *out, pc_error *err);
void pc_algo_liquify_strokes_free(pc_algo_liquify_stroke *s, size_t n);
/* serde_json::to_value: fields in declaration order, non-finite numbers as null. NULL on NOMEM. */
yyjson_mut_val *pc_algo_liquify_stroke_to_json(yyjson_mut_doc *d, const pc_algo_liquify_stroke *s);

/* The displacement field plus the freeze mask over `bounds`. d and freeze have w * h entries,
 * row-major; d holds the inverse displacement (x, y) per node, freeze 0 (free) .. 1 (frozen). */
typedef struct pc_algo_liquify_field {
    pc_rect bounds;
    double cell; /* document pixels per node */
    size_t w, h;
    float (*d)[2];
    float *freeze;
} pc_algo_liquify_field;

/* auto_cell: 2 px per node up to 4.2 MP, 4 px beyond. */
double pc_algo_liquify_auto_cell(pc_rect bounds);
/* LiquifyField::new (cell clamped to 1..64; NaN stays NaN as in the reference). */
pc_status pc_algo_liquify_field_init(pc_algo_liquify_field *f, pc_rect bounds, double cell, pc_error *err);
void pc_algo_liquify_field_fini(pc_algo_liquify_field *f); /* NULL-safe */
pc_status pc_algo_liquify_field_clone(const pc_algo_liquify_field *f, pc_algo_liquify_field *out,
                                      pc_error *err);
bool pc_algo_liquify_field_eq(const pc_algo_liquify_field *a, const pc_algo_liquify_field *b);

/* Document position of node (i, j) (node 0 sits on the first pixel centre). */
void pc_algo_liquify_node_pos(const pc_algo_liquify_field *f, size_t i, size_t j, double out[2]);
bool pc_algo_liquify_is_identity(const pc_algo_liquify_field *f);
float pc_algo_liquify_max_displacement(const pc_algo_liquify_field *f);
/* Bilinear displacement at a document point (zero outside the field). */
void pc_algo_liquify_sample(const pc_algo_liquify_field *f, double x, double y, double out[2]);
/* Freeze value at a document point (bilinear, clamped to the field). */
float pc_algo_liquify_freeze_at(const pc_algo_liquify_field *f, double x, double y);

/* Applies one stroke; *dirty (may be NULL) receives the document rect whose output changed. On
 * failure (NOMEM) the field may hold part of the stroke. */
pc_status pc_algo_liquify_apply_stroke(pc_algo_liquify_field *f, const pc_algo_liquify_stroke *s,
                                       pc_rect *dirty, pc_error *err);
/* Interactive use: start a stroke at p = [x, y, pressure] (stationary tools dab once), then one
 * segment per new point; apply_stroke is exactly begin + segments over the point list. */
pc_status pc_algo_liquify_stroke_begin(pc_algo_liquify_field *f, const pc_algo_liquify_stroke *s,
                                       const double p[3], pc_rect *dirty, pc_error *err);
pc_status pc_algo_liquify_stroke_segment(pc_algo_liquify_field *f, const pc_algo_liquify_stroke *s,
                                         const double a[3], const double b[3], pc_rect *dirty, pc_error *err);
/* Replays strokes on a fresh field (`out` initialised on success). */
pc_status pc_algo_liquify_field_from_strokes(pc_rect bounds, double cell,
                                             const pc_algo_liquify_stroke *strokes, size_t n,
                                             pc_algo_liquify_field *out, pc_error *err);

/* Resamples src through the field: inside field->bounds each pixel shows the source at p + d(p)
 * (bicubic field, bicubic premultiplied source, edge pixels repeat past the bounds); everything
 * outside is kept. Same format as src; `out` initialised on success. */
pc_status pc_algo_liquify_apply(const pc_surface *src, const pc_algo_liquify_field *field, pc_surface *out,
                                pc_error *err);

/* A downsampled, premultiplied RGBA copy of a layer for the interactive preview. */
typedef struct pc_algo_liquify_proxy {
    pc_rect bounds; /* document region covered (the field bounds) */
    double scale;   /* document pixels per proxy pixel */
    size_t w, h;
    float (*px)[4]; /* premultiplied RGBA, row-major, w * h */
} pc_algo_liquify_proxy;

/* Box-downsamples src over bounds so the longer side is at most max_side. */
pc_status pc_algo_liquify_proxy_init(pc_algo_liquify_proxy *p, const pc_surface *src, pc_rect bounds,
                                     size_t max_side, pc_error *err);
void pc_algo_liquify_proxy_fini(pc_algo_liquify_proxy *p); /* NULL-safe */
/* Renders proxy pixels r = {x0, y0, x1, y1} (proxy coordinates, clamped to the proxy) through the
 * field into out (premultiplied RGBA8, w x h, out_len pixels); PC_ERR_INVALID when out is too
 * short for the rows rendered. */
pc_status pc_algo_liquify_proxy_render(const pc_algo_liquify_proxy *p, const pc_algo_liquify_field *field,
                                       const size_t r[4], uint8_t (*out)[4], size_t out_len, pc_error *err);
/* The proxy pixel rectangle covering document rect r. */
void pc_algo_liquify_proxy_rect(const pc_algo_liquify_proxy *p, pc_rect r, size_t out[4]);

#endif
