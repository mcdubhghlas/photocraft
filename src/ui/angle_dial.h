/* Angle dial core (headless; no GUI). Port of the direction maths in
 * crates/ui-egui/src/widgets/angle_dial.rs: a small wheel pointing anywhere in degrees,
 * counter-clockwise from three o'clock with y up. */
#ifndef PC_UI_ANGLE_DIAL_H
#define PC_UI_ANGLE_DIAL_H

#include <stdbool.h>

/* Direction (degrees, CCW from +x, y up, rounded) from a pointer at (px,py) relative to the dial
 * centre (cx,cy), clamped to [lo,hi]. Returns false and leaves *out untouched when the pointer is
 * within 1px of the centre (no meaningful direction). Clamps with min/max so a reversed range
 * cannot misbehave. */
bool pc_ui_angle_from_point(double cx, double cy, double px, double py, double lo, double hi,
                            double *out);

/* Needle tip for `angle_deg` at `radius` from the centre (cx,cy). */
void pc_ui_angle_tip(double angle_deg, double cx, double cy, double radius, double *tx, double *ty);

/* Step `angle_deg` by `step` degrees within [lo,hi]. A half-turn range (lo >= -180 && hi <= 180)
 * wraps round the circle; a wider range steps on and clamps. */
double pc_ui_angle_step(double angle_deg, double step, double lo, double hi);

#endif
