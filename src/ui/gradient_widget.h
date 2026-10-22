/* Pure geometry model for the Gradient tool's on-canvas fill-layer widget (port of the Widget hit
 * test + per-style decoration maths in crates/ui-egui/src/gradient_ui.rs and
 * crates/compose/src/gradient_fill.rs). No GUI, engine or compose dependency, so it is unit-tested
 * headlessly; the drawing and pointer routing live in ui/shell/gradient_ui.c + canvas_view.c.
 *
 * All coordinates are DOCUMENT pixels; gradient locations are 0..1. The widget's endpoints (`from`,
 * `to`), reverse flag, colour-stop locations and per-segment midpoints come straight from
 * gradient.fill.get, so no angle/scale/offset layout maths is needed here. Everything is NaN- and
 * NULL-safe and never indexes out of bounds (REWRITE_PLAN.md rule 9). */
#ifndef PC_UI_GRADIENT_WIDGET_H
#define PC_UI_GRADIENT_WIDGET_H

#include <stdbool.h>
#include <stddef.h>

#define PC_UI_GRAD_WIDGET_MAX 256

typedef enum pc_ui_grad_grab_kind {
    PC_UI_GRAD_GRAB_NONE = 0,
    PC_UI_GRAD_GRAB_START, /* the `from` endpoint handle */
    PC_UI_GRAD_GRAB_END,   /* the `to` endpoint handle */
    PC_UI_GRAD_GRAB_LINE,  /* the gradient line body (grab.t = gradient location at the hit) */
    PC_UI_GRAD_GRAB_STOP,  /* a colour stop marker (grab.index = stop index) */
    PC_UI_GRAD_GRAB_MID    /* a segment midpoint marker (grab.index = segment index) */
} pc_ui_grad_grab_kind;

typedef struct pc_ui_grad_grab {
    pc_ui_grad_grab_kind kind;
    int index; /* STOP: colour-stop index; MID: segment (between stop index, index+1) */
    double t;  /* LINE: gradient location (0..1), already un-reversed to the stop coordinate */
} pc_ui_grad_grab;

/* One gradient fill layer's widget geometry. Fill the fields directly (tests) or from the
 * gradient.fill.get result in the shell. Locations must be within 0..1; counts are capped at
 * PC_UI_GRAD_WIDGET_MAX by the filler. */
typedef struct pc_ui_grad_widget {
    double from[2], to[2];
    bool reverse;
    double stops[PC_UI_GRAD_WIDGET_MAX]; /* sorted colour-stop locations 0..1 */
    size_t nstops;
    double mids[PC_UI_GRAD_WIDGET_MAX]; /* per-segment midpoint fractions 0..1 (segment i: stops i,i+1) */
    size_t nmids;
} pc_ui_grad_widget;

/* Point on the gradient line for gradient location t (0..1 clamped), honouring reverse. NULL-safe. */
void pc_ui_grad_widget_at(const pc_ui_grad_widget *w, double t, double out[2]);

/* Project the pointer onto the line: *out_u = location 0..1 along from->to (un-reversed, clamped),
 * *out_perp = perpendicular distance in document units. A degenerate (from==to) line gives u=0 and
 * the straight distance. NULL-safe (out params optional). */
void pc_ui_grad_widget_project(const pc_ui_grad_widget *w, double px, double py, double *out_u,
                               double *out_perp);

/* Hit-test the pointer (doc coords) with tolerance `tol` (doc units), in Photoshop order: end
 * handle, start handle, colour stops, segment midpoints, then the line body. Returns kind NONE on a
 * miss or on NULL/degenerate input. */
pc_ui_grad_grab pc_ui_grad_widget_hit(const pc_ui_grad_widget *w, double px, double py, double tol);

/* Snap the vector anchor->p to the nearest 45 degrees when `shift` is set (keeps the length); copies
 * p through otherwise. NaN/NULL-safe. */
void pc_ui_grad_snap45(const double anchor[2], const double p[2], bool shift, double out[2]);

/* Per-style decoration geometry in document coordinates. Each returns false for a degenerate line
 * (from==to) or NULL input; the caller chooses which to draw from the fill style:
 *   - circle:    radial and angle gradients (centre = `from`, radius = |to - from|)
 *   - diamond:   diamond gradients (four corners about `from` at +-d and +-n, d = to-from)
 *   - reflected: reflected gradients (the mirrored endpoint 2*from - to)
 *   - linear has no decoration. */
bool pc_ui_grad_decor_circle(const pc_ui_grad_widget *w, double center[2], double *radius);
bool pc_ui_grad_decor_diamond(const pc_ui_grad_widget *w, double corners[4][2]);
bool pc_ui_grad_decor_reflected(const pc_ui_grad_widget *w, double mirror[2]);

#endif /* PC_UI_GRADIENT_WIDGET_H */
