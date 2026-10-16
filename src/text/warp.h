/* Warp Text (port of crates/text/src/warp.rs): bends the glyph outlines of a type layer with one
 * of Photoshop's fifteen warp styles (the PSD `warp` descriptor's `warpStyle`). The style math
 * is geom/warp.h (shared with Edit > Transform > Warp); this file maps the layer's settings onto
 * it over the laid-out text's bounds. Pure functions; thread-safe. */
#ifndef PC_TEXT_WARP_H
#define PC_TEXT_WARP_H

#include <stdbool.h>
#include <stddef.h>

#include "doc/text.h"
#include "geom/warp.h"
#include "text/layout_data.h"

/* Warp styles in Photoshop's menu order: {PSD warpStyle value, short id used by commands}. */
extern const char *const PC_TEXT_WARP_STYLES[15][2];

/* PSD style name ("warpArc") for a short id ("arc", ASCII case-insensitive) or a PSD name;
 * "none"/"warpNone" give "warpNone". NULL when unknown. Static strings. */
const char *pc_text_warp_psd_style(const char *id, size_t len);
/* Short id ("flag", "none") for a PSD style name ("warpFlag", "warpNone"; exact spelling): the
 * reverse of pc_text_warp_psd_style. NULL for other names. Static strings. */
const char *pc_text_warp_short_style(const char *psd, size_t len);

/* Prepares w for text whose bounds are {x0, y0, x1, y1} (text space). False (out untouched) for
 * warpNone, unknown styles (the style must be a preset spelled as its PSD name), an all-zero
 * warp or empty bounds. */
bool pc_text_warp_prepare(const pc_text_warp *w, const float bounds[4], pc_style_warp *out);

/* render.rs layout_warp: the layer warp over pc_text_layout_bounds; false for none. */
bool pc_text_layout_warp(const pc_text_layout *l, const pc_text_warp *w, pc_style_warp *out);

/* A flattened type path with arc lengths (warp.rs PathFrame): text-space x is arc length from
 * `start`, y the distance to the left of the direction of travel. Owns its arrays. */
typedef struct pc_path_frame {
    pc_point *pts; /* flattened path points, text space */
    double *cum;   /* arc length at each point (cum[0] = 0) */
    size_t n;
    double start;
} pc_path_frame;

/* The deformation a type layer applies to its glyph outlines (warp.rs Warp): either Photoshop's
 * style warp over the text box, or a path the type runs along. A STYLE deform owns nothing; a
 * PATH deform owns its frame (free with pc_text_deform_fini). */
typedef enum pc_deform_kind { PC_DEFORM_STYLE, PC_DEFORM_PATH } pc_deform_kind;

typedef struct pc_text_deform {
    pc_deform_kind kind;
    pc_style_warp style;  /* STYLE */
    pc_path_frame path;   /* PATH */
} pc_text_deform;

void pc_text_deform_fini(pc_text_deform *w);

/* Warp::on_path: the deformation of type on a path. False (out untouched, nothing to free) for any
 * other shape, or a path without length. On success *out is a PATH deform owning its frame. */
bool pc_text_warp_on_path(const pc_text_shape *shape, pc_text_deform *out);

/* render.rs layout_deform: the path the type runs along, else the layer's Warp Text settings.
 * False (out untouched) when neither applies. */
bool pc_text_layout_deform(const pc_text_layout *l, const pc_text_layer *layer, pc_text_deform *out);

/* Warp::max_segment / Warp::apply (warp.rs). */
double pc_text_deform_max_segment(const pc_text_deform *w);
pc_point pc_text_deform_apply(const pc_text_deform *w, double x, double y);

/* Warp::place_glyph: where a glyph whose horizontal centre is at text-space cx goes.
 *  - PC_PLACEMENT_BEND: not path text; bend the outline point by point with pc_text_deform_apply.
 *  - PC_PLACEMENT_HIDDEN: beyond either end of the path; the glyph is not drawn.
 *  - PC_PLACEMENT_FRAME: *frame (a, b, c, d, e, f) is the rigid text-space map for the glyph. */
typedef enum pc_placement_kind { PC_PLACEMENT_BEND, PC_PLACEMENT_HIDDEN, PC_PLACEMENT_FRAME } pc_placement_kind;
pc_placement_kind pc_text_deform_place_glyph(const pc_text_deform *w, double cx, double frame[6]);

#endif
