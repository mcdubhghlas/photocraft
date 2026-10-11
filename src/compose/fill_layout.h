/* Gradient fill layer geometry (port of crates/compose/src/fill_layout.rs).
 *
 * Photoshop lays a gradient fill layer's Linear and Reflected gradients out between whole-pixel
 * end points: the chord through the frame's centre along the angle ends at points truncated to
 * the pixel grid. On small frames that changes the effective angle (a 30 degree Reflected fill on
 * a 4 x 4 canvas renders as 45 degrees). The snapped end points are expressed as the angle /
 * scale / centre offset that pc_compose_gradient_t takes, so every gradient shares one layout.
 * Pure functions; thread-safe; non-finite input passes through unchanged. */
#ifndef PC_COMPOSE_FILL_LAYOUT_H
#define PC_COMPOSE_FILL_LAYOUT_H

#include <stdbool.h>

#include "doc/effects.h"
#include "geom/geom.h"

typedef struct pc_compose_gradient_geom {
    float angle; /* degrees */
    float scale;
    float offset[2]; /* fraction of the frame off its centre */
} pc_compose_gradient_geom;

/* Effective layout of a gradient fill with style, angle (degrees) and scale in frame. */
pc_compose_gradient_geom pc_compose_fill_gradient_layout(pc_gradient_style style, float angle, float scale,
                                                         pc_rect frame);
/* The same for a gradient centred `offset` (fraction of the frame) off the frame's centre
 * (layer-effect gradients snap the same way). */
pc_compose_gradient_geom pc_compose_gradient_layout(pc_gradient_style style, float angle, float scale,
                                                    const float offset[2], pc_rect frame);
/* Gradient parameter t of a gradient fill at pixel centre (x, y). */
float pc_compose_fill_gradient_t(pc_gradient_style style, float angle, float scale, bool reverse,
                                 pc_rect frame, float x, float y);

/* From effects.rs, implemented here because the layout needs them (C3 uses them too). */
/* The chord (Linear / Reflected) and ellipse-norm length (others) a 100 % gradient at angle
 * spans in a w x h frame. */
void pc_compose_gradient_units(float angle, float w, float h, float *chord, float *len);
/* Gradient parameter t in [0, 1] for pixel centre (x, y) inside bounds. */
float pc_compose_gradient_t(pc_gradient_style style, float angle, float scale, bool reverse,
                            const float offset[2], pc_rect bounds, float x, float y);

#endif
