/* Gradient tool options bar and the Gradient Editor window (port of the Tool::Gradient options-bar
 * arm in crates/ui-egui/src/panels.rs and the no-layer "edit the tool's gradient" mode of
 * crates/ui-egui/src/gradient_ui.rs). The options bar edits ui->tool_options (style/mode/opacity/
 * reverse/dither/classic); the editor edits the current gradient preset through gradient.presets.*.
 * Thin draw layer; NULL-safe; never crashes (REWRITE_PLAN.md rule 9). */
#ifndef PC_UI_SHELL_GRADIENT_UI_H
#define PC_UI_SHELL_GRADIENT_UI_H

#include "shell_internal.h"

struct pc_ui_canvas_view;
struct pc_layer;

/* Draw the Gradient tool's options bar. Call only when ui->tool == PC_TOOL_GRADIENT. */
void pc_ui_shell_gradient_options(pc_ui_shell_app *app);

/* Draw the floating Gradient Editor window when ui->panels.gradient_editor is set. Called from the
 * frame loop after the other floating panels; a no-op otherwise. */
void pc_ui_shell_gradient_editor_draw(pc_ui_shell_app *app, float width, float height);

/* Live Gradient Fill tool: handle a canvas pointer event (create / edit a Gradient Fill layer with
 * the on-canvas widget). Returns true when it consumed the event. A no-op (returns false) unless the
 * Gradient tool is active in its non-classic fill-layer mode. Updates cv->grad_drag and the live
 * cv->grad_preview. */
bool pc_ui_shell_gradient_canvas(pc_ui_shell_app *app, struct pc_ui_canvas_view *cv,
                                 const pc_ui_canvas_xform *xf, const SDL_Event *event, float factor);

/* Draw the live Gradient Fill widget overlay (the line, per-style decoration and handles) for the
 * active gradient fill layer or the in-progress drag. Call from the canvas overlay pass. */
void pc_ui_shell_gradient_widget_draw(pc_ui_shell_app *app, struct pc_ui_canvas_view *cv,
                                      const pc_ui_canvas_xform *xf);

/* Draw the Properties-panel section for a Gradient Fill layer (style/angle/scale/reverse/dither/
 * align + the stop editor). Call from the Properties panel when the layer is a gradient fill. */
void pc_ui_shell_gradient_properties(pc_ui_shell_app *app, const struct pc_layer *layer);

#endif /* PC_UI_SHELL_GRADIENT_UI_H */
