/* Private to src/paint (and its tests): the rasteriser with Wet Edges depth output and the
 * stroke mask with Wet Edges, shared by render.c and stroke.c. */
#ifndef PC_PAINT_RENDER_PRIV_H
#define PC_PAINT_RENDER_PRIV_H

#include "paint/render.h"

/* pc_paint_context_rasterize; with depth (non-NULL), Wet Edges is left out of out and each
 * painted pixel's depth inside the dab (1 - rn) goes to depth, for the stroke to apply it.
 * fast = false skips the computed-round-tip fast path (the tests compare the two). */
pc_status pc_paint_rasterize_into(const pc_paint_context *ctx, const pc_paint_dab *d, bool dual, pc_rect rect,
                                  pc_paint_fbuf *out, pc_paint_fbuf *depth, bool fast, pc_error *err);
/* pc_paint_context_stroke_mask with Wet Edges from the stroke's depth at the pixel (NULL: none). */
float pc_paint_context_stroke_mask_wet(const pc_paint_context *ctx, float c, const float *depth,
                                       bool has_dual, float dual, int32_t x, int32_t y);
/* Wet Edges: the paint kept at a pixel the nearest dab reaches at rn (0 centre, 1 rim). */
float pc_paint_wet_factor(float rn);

#endif
