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

#endif
