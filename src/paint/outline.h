/* Brush tip outlines for the painting cursor (port of crates/paint/src/outline.rs): closed rings
 * in units of the brush radius (1 = size / 2), screen-oriented (x right, y down), for the UI to
 * scale by the radius. A computed round tip is a 64-point ellipse; a sampled tip's contour is
 * traced with marching squares (resampled ~2x, at most 160 px a side), simplified with
 * Douglas-Peucker to at most 512 points in all (the smallest rings dropped first).
 *
 * Ownership: an outline owns its rings (pc_paint_outline_fini). Thread-safe (no shared state). */
#ifndef PC_PAINT_OUTLINE_H
#define PC_PAINT_OUTLINE_H

#include <stdbool.h>
#include <stddef.h>

#include "base/status.h"
#include "paint/brush.h"

typedef struct pc_paint_ring {
    float (*pts)[2]; /* n points, x then y */
    size_t n;
} pc_paint_ring;

typedef struct pc_paint_outline {
    pc_paint_ring *rings;
    size_t count; /* 0 = no outline (the reference's None) */
} pc_paint_outline;

/* The tip outline of b: `full` outlines everything above 1/255, otherwise the 50 % contour (the
 * normal brush tip cursor). out->count is 0 when the tip has no usable shape (non-finite or
 * non-positive size, non-finite angle or roundness, an invalid or blank sampled tip, or a
 * non-finite result). PC_ERR_NOMEM (*out is then empty). */
pc_status pc_paint_tip_outline(const pc_paint_brush *b, bool full, pc_paint_outline *out, pc_error *err);
void pc_paint_outline_fini(pc_paint_outline *o); /* NULL-safe; leaves it empty */

#endif
