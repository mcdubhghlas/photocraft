/* Pure model for the Gradient Editor's stop strip (port of the stops model in
 * crates/ui-egui/src/gradient_ui.rs): parse the engine's gradient JSON ({stops, transparency}),
 * sample the ramp, and hit-test stop markers. No GUI dependency, so it is unit-tested headlessly;
 * the strip/field drawing lives in ui/shell/gradient_ui.c. */
#ifndef PC_UI_GRADIENT_EDIT_H
#define PC_UI_GRADIENT_EDIT_H

#include <stdbool.h>
#include <stddef.h>

#include <yyjson.h>

#define PC_UI_GRAD_MAX 256

typedef struct pc_ui_grad_cstop {
    float loc; /* 0..1 */
    float rgb[3];
} pc_ui_grad_cstop;
typedef struct pc_ui_grad_ostop {
    float loc;     /* 0..1 */
    float opacity; /* 0..1 */
} pc_ui_grad_ostop;
typedef struct pc_ui_gradient {
    pc_ui_grad_cstop colors[PC_UI_GRAD_MAX];
    size_t ncolors;
    pc_ui_grad_ostop opacity[PC_UI_GRAD_MAX];
    size_t nopacity;
} pc_ui_gradient;

/* Black -> white, fully opaque. */
void pc_ui_gradient_init(pc_ui_gradient *g);

/* Parse the engine's current-gradient object ({stops:[[t,"#rrggbb"|"foreground"|"background"]],
 * transparency:[[t,0..100]]}); "foreground"/"background" named stops resolve to fg/bg (each rgb,
 * may be NULL -> black/white). Returns false (leaves a sane default) when `current` is unusable.
 * Stops are sorted by location; an empty transparency array becomes two opaque stops. */
bool pc_ui_gradient_parse(pc_ui_gradient *g, yyjson_mut_val *current, const float fg[3], const float bg[3]);

/* Straight-alpha RGBA at location t (0..1), interpolating colour and opacity stops linearly. */
void pc_ui_gradient_sample(const pc_ui_gradient *g, float t, float out_rgba[4]);

/* Index of the colour/opacity stop whose location is nearest `loc` and within `tol`, else -1. */
int pc_ui_gradient_nearest_color(const pc_ui_gradient *g, float loc, float tol);
int pc_ui_gradient_nearest_opacity(const pc_ui_gradient *g, float loc, float tol);

#endif /* PC_UI_GRADIENT_EDIT_H */
