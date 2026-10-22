/* The Edit > Fill modal dialog (port of crates/ui-egui/src/fill_ui.rs). A generic Command dialog
 * (edit.fill) with a bespoke body: Contents dropdown, the per-contents Color/Pattern/Color-Adaptation
 * row, and the shared Mode/Opacity/Preserve-Transparency blending row. Thin; NULL-safe; never
 * crashes (REWRITE_PLAN.md rule 9). */
#ifndef PC_UI_SHELL_FILL_UI_H
#define PC_UI_SHELL_FILL_UI_H

#include "app.h"
#include "dialogs.h"

/* Open the Fill dialog (invoked for the no-params edit.fill command). */
pc_status pc_ui_shell_fill_open(pc_ui_shell_app *app, pc_json_value *out, pc_error *err);

/* Draw the Fill dialog body. Called from the dialog render loop when the __fill marker is set. */
void pc_ui_shell_fill_body(pc_ui_shell_app *app, pc_ui_dialog *dialog);

#endif /* PC_UI_SHELL_FILL_UI_H */
