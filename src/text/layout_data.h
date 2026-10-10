/* The result of laying out a text layer (the data half of crates/text/src/layout.rs): glyph
 * faces, placed glyphs, decorations, lines and clusters in text space (pixels, y down, first
 * baseline of point text at y = 0). The layouter that fills it (shaping, bidi, line breaking)
 * and the hit-testing/caret helpers are src/text/layout.c (chunk L14); rendering is render.h.
 *
 * Vertical type is laid out in *line space* and turned 90° clockwise into text space; lines,
 * clusters, carets and hit tests stay in line space (pc_text_layout_to_text / _to_line convert).
 *
 * Ownership: a pc_text_layout owns its arrays, one reference to each face's font and each
 * face's coords array; release everything with pc_text_layout_fini (accepts a zeroed or already
 * finalised layout). Plain data otherwise; not synchronised. */
#ifndef PC_TEXT_LAYOUT_DATA_H
#define PC_TEXT_LAYOUT_DATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "doc/text.h"
#include "text/font.h"

/* How a placed glyph's outline is oriented in text space. */
typedef enum pc_glyph_orient {
    PC_GLYPH_HORIZONTAL, /* horizontal type: origin on the baseline, outline upright */
    PC_GLYPH_UPRIGHT,    /* vertical type, upright; baseline shift moves it right */
    PC_GLYPH_ROTATED     /* vertical type, turned 90° clockwise about its origin */
} pc_glyph_orient;

/* A font instance used by some glyphs. */
typedef struct pc_glyph_face {
    pc_font *font;   /* one reference, owned */
    int16_t *coords; /* normalised variation coordinates (F2Dot14 bits), owned; may be NULL */
    size_t ncoords;
    float size_px;
    bool embolden;  /* synthetic bold requested by font matching */
    float skew_deg; /* synthetic oblique angle (face lacks italics) */
} pc_glyph_face;

typedef struct pc_placed_glyph {
    uint32_t face;  /* index into faces */
    uint32_t id;    /* glyph id */
    float x, y;     /* pen position: x along the baseline, y = baseline (before baseline shift) */
    uint32_t style; /* character run index (into styles) */
    pc_glyph_orient orient;
} pc_placed_glyph;

/* Underline/strikethrough rectangle (text space px). */
typedef struct pc_decoration_rect {
    float x0, y0, x1, y1;
    uint32_t style;
} pc_decoration_rect;

/* A line (a column in vertical type) in line space. */
typedef struct pc_line_info {
    size_t start, end; /* byte range in the layer text (without the paragraph break) */
    float baseline;
    float x0, x1; /* visual extent along the baseline */
    float ascent, descent;
    size_t paragraph;
} pc_line_info;

/* A grapheme cluster (caret stops, hit testing, selection), in line space. */
typedef struct pc_cluster_info {
    size_t start, end;
    float x, advance;
    size_t line;
    bool rtl;
} pc_cluster_info;

typedef struct pc_text_layout {
    pc_glyph_face *faces;
    size_t nfaces;
    pc_placed_glyph *glyphs;
    size_t nglyphs;
    pc_decoration_rect *decorations;
    size_t ndecorations;
    pc_line_info *lines;
    size_t nlines;
    pc_cluster_info *clusters;
    size_t nclusters;
    pc_char_style *styles; /* resolved character styles, one per run (owned, pc_char_style_fini) */
    size_t nstyles;
    float px_per_pt; /* dpi / 72 */
    bool vertical;
} pc_text_layout;

/* An empty layout (px_per_pt 0, horizontal). Never fails. */
void pc_text_layout_init(pc_text_layout *l);
void pc_text_layout_fini(pc_text_layout *l);

/* Line space -> text space (identity for horizontal type; vertical: (-y, x)) and back. */
void pc_text_layout_to_text(const pc_text_layout *l, float x, float y, float *ox, float *oy);
void pc_text_layout_to_line(const pc_text_layout *l, float x, float y, float *ox, float *oy);
/* Logical bounds {x0, y0, x1, y1} of all lines in line space / text space; false (out
 * untouched) when there are no lines. */
bool pc_text_layout_line_bounds(const pc_text_layout *l, float out[4]);
bool pc_text_layout_bounds(const pc_text_layout *l, float out[4]);

/* Appends a face (retaining `font`, copying `coords`) and returns its index in *index. */
pc_status pc_text_layout_add_face(pc_text_layout *l, pc_font *font, const int16_t *coords, size_t ncoords,
                                  float size_px, bool embolden, float skew_deg, uint32_t *index,
                                  pc_error *err);

#endif
