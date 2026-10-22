/* ⇧ constraints for painting and the Gradient tool (Photoshop parity):
 *
 *   - ⇧ while dragging a painting stroke locks it to a straight 0/45/90° line from where the
 *     constraint began. The direction locks once the pointer has travelled a few screen points.
 *   - ⇧-click after a stroke paints a straight line from the previous stroke's end to the click,
 *     as part of the new stroke (one command, one undo step), remembering the endpoint's pressure.
 *   - The Gradient tool's ⇧ snaps the gradient angle to 45° increments.
 *
 * These are pure helpers; the gesture layer (tool_gesture.c) carries the per-stroke Axis and the
 * per-document last-stroke endpoint. */
#ifndef PC_UI_STROKE_CONSTRAINT_H
#define PC_UI_STROKE_CONSTRAINT_H
#include "ui/state.h"

/* A ⇧ constraint in progress: its origin and, once locked, the unit direction of the line. */
typedef struct pc_ui_stroke_axis {
    bool active; /* origin captured */
    bool locked; /* dir resolved */
    double origin[2];
    double dir[2];
} pc_ui_stroke_axis;

/* Tools whose drag ⇧ constrains: every painting tool and the Gradient tool. */
bool pc_ui_stroke_constraint_applies(pc_ui_tool tool);
/* Tools that ⇧-click connects to the previous stroke with a straight line. */
bool pc_ui_stroke_constraint_connects(pc_ui_tool tool);
/* Unit direction from origin toward p, snapped to the nearest multiple of step radians.
 * Returns false (out untouched) when the points coincide or inputs are non-finite. */
bool pc_ui_stroke_snapped_dir(const double origin[2], const double p[2], double step, double out[2]);
/* p projected onto the line through origin along unit dir. */
void pc_ui_stroke_project(const double origin[2], const double dir[2], const double p[2], double out[2]);
/* p moved onto the nearest 45° ray from origin (the Gradient tool's ⇧). */
void pc_ui_stroke_snap45(const double origin[2], const double p[2], double out[2]);
/* Constrain pointer p for a drag of tool that started at start and last reached last. axis carries
 * the lock between events; zoom converts the lock distance to document pixels. Without ⇧ the point
 * passes through and the lock is dropped. */
void pc_ui_stroke_constrain(pc_ui_tool tool, pc_ui_stroke_axis *axis, const double start[2],
                            const double last[2], const double p[2], bool shift, float zoom, double out[2]);
#endif
