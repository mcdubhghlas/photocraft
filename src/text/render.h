/* Rasterises a laid-out text (pc_text_layout) into a document-space surface of any pixel format,
 * and extracts glyph outlines (port of crates/text/src/render.rs).
 *
 * Warp Text: functions take an optional prepared style warp (geom/warp.h). The reference builds
 * it with `layout_warp(layout, layer.warp)` (warp.rs, chunk L14: the style must be a preset
 * whose PSD name matches, applied over pc_text_layout_bounds); pass NULL for none.
 *
 * Every function is pure apart from FreeType's per-font caches (font.h: safe from any thread).
 * Glyphs or decorations whose face/style index is out of range are skipped (the reference would
 * panic). */
#ifndef PC_TEXT_RENDER_H
#define PC_TEXT_RENDER_H

#include "color/color.h"
#include "doc/text.h"
#include "geom/geom.h"
#include "geom/warp.h"
#include "raster/surface.h"
#include "text/layout_data.h"
#include "text/raster.h"

/* Faux-italic slant in degrees (Photoshop-like). */
#define PC_TEXT_FAUX_ITALIC_DEG 12.0f
/* Faux-bold ring radius as a fraction of the font size (tuned so stem weight stays close to the
   old summed-coverage look). */
#define PC_TEXT_FAUX_BOLD_RADIUS 0.036f
/* Largest raster produced (pixels), a guard against absurd sizes. */
#define PC_TEXT_MAX_PIXELS ((uint64_t)256 * 1024 * 1024)

/* Rendered text: pixels in document space plus the covered rectangle. */
typedef struct pc_rendered {
    pc_surface surface;
    pc_rect rect;
} pc_rendered;

void pc_rendered_fini(pc_rendered *r); /* NULL-safe */

/* Font units (scaled to px, y up) -> text space for a placed glyph: scales, faux italic,
 * baseline shift and the vertical-type orientation. */
pc_xform pc_text_glyph_xform(const pc_text_layout *l, const pc_placed_glyph *g, float face_skew_deg);

/* Integer pixel rectangle around float bounds {x0, y0, x1, y1}: floor/ceil, one pixel of
 * margin, clamped to i32; empty for non-finite bounds or a side over PC_TEXT_MAX_PIXELS. */
pc_rect pc_text_rect_from_bounds(const double b[4]);

/* Document-space bounds of the drawn text through `transform` (text space -> document px),
 * bent by `warp` (may be NULL). Empty when nothing is drawn. Fails only when out of memory. */
pc_status pc_text_ink_rect(const pc_text_layout *l, const pc_affine *transform, const pc_style_warp *warp,
                           pc_rect *out, pc_error *err);

/* Rasterises through `transform`. The surface always has alpha; colour is written in the
 * format's model and depth. An empty or oversized result is an empty surface at rect
 * (0, 0, 0, 0). PC_ERR_NOMEM / PC_ERR_LIMIT when the accumulation buffer can't be allocated.
 * `out` is initialised on success only (release with pc_rendered_fini). */
pc_status pc_text_rasterize(const pc_text_layout *l, const pc_affine *transform, pc_pixel_format format,
                            pc_antialias aa, const pc_style_warp *warp, pc_rendered *out, pc_error *err);

/* One element of a glyph outline in document space. */
typedef enum pc_path_kind {
    PC_PATH_MOVE,
    PC_PATH_LINE,
    PC_PATH_QUAD,
    PC_PATH_CURVE,
    PC_PATH_CLOSE
} pc_path_kind;

typedef struct pc_path_el {
    pc_path_kind kind;
    double p[3][2]; /* MOVE/LINE: p[0]; QUAD: control p[0], end p[1]; CURVE: p[0], p[1], end p[2] */
} pc_path_el;

typedef struct pc_glyph_outline {
    pc_path_el *els;
    size_t n;
} pc_glyph_outline;

/* Glyph outlines in document space (through `transform`, bent by `warp`), one element list per
 * glyph with a non-empty outline, for Type > Create Work Path and Convert to Shape. Faux bold is
 * not applied (Photoshop also converts the regular outline); faux italic and scaling are.
 * Under a warp, control points are mapped directly. *out is NULL when there are none; free with
 * pc_glyph_outlines_free. */
pc_status pc_text_outlines(const pc_text_layout *l, const pc_affine *transform, const pc_style_warp *warp,
                           pc_glyph_outline **out, size_t *n, pc_error *err);
void pc_glyph_outlines_free(pc_glyph_outline *o, size_t n);

#endif
