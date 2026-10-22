/* Options bars for the retouch tools (Clone Stamp, Pattern Stamp, Healing Brush, Spot Healing,
 * Patch, Content-Aware Move, Red Eye, Dodge/Burn/Sponge, Blur/Sharpen/Smudge, Color Replacement),
 * a thin draw layer over the engine commands and the shared tool_options state (port of the
 * options_bar arms in crates/ui-egui/src/retouch_ui.rs). The stroke/click dispatch lives in
 * ui/tool_gesture.c; this file only edits ui->tool_options / the current brush and dispatches the
 * pattern selection. NULL-safe; never crashes (REWRITE_PLAN.md rule 9). */
#ifndef PC_UI_SHELL_RETOUCH_UI_H
#define PC_UI_SHELL_RETOUCH_UI_H

#include "app.h"
#include "ui/state.h"
#include "ui/canvas.h"
#include "ui/tool_gesture.h"

/* True for the tools whose options bar this module draws. */
bool pc_ui_retouch_tool(pc_ui_tool tool);

/* Draw the options bar for the current retouch tool. Call only when pc_ui_retouch_tool(ui->tool). */
void pc_ui_shell_retouch_options(pc_ui_shell_app *app);

/* Draw the Clone Stamp / Healing Brush source crosshair over the canvas (gap 16). Call from the
 * canvas overlay pass with the active view's gesture. NULL-safe. */
void pc_ui_shell_clone_source_draw(pc_ui_shell_app *app, const pc_ui_tool_gesture *gesture,
                                   const pc_ui_canvas_xform *xf);

#endif /* PC_UI_SHELL_RETOUCH_UI_H */
