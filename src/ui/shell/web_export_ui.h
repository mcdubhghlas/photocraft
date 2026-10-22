/* Save for Web (Legacy) modal dialog (clean-room port of crates/ui-egui/src/file_ui.rs __web).
 * A Command dialog (file.export.saveForWebLegacy) with a bespoke body: Original / Optimized / 2-Up
 * / 4-Up preview panes over a transparency checker, the format + per-format optimisation settings
 * (GIF / PNG-8 / PNG-24 / JPEG / WBMP), matte, metadata, Convert to sRGB and Image Size, plus an
 * estimated file size. Confirm dispatches file.export.saveForWebLegacy with the chosen path. Thin;
 * NULL-safe; never crashes (REWRITE_PLAN.md rule 9). */
#ifndef PC_UI_SHELL_WEB_EXPORT_UI_H
#define PC_UI_SHELL_WEB_EXPORT_UI_H

#include "app.h"
#include "dialogs.h"

/* Open the Save for Web dialog (invoked for the no-params file.export.saveForWebLegacy command). */
pc_status pc_ui_shell_web_export_open(pc_ui_shell_app *app, pc_json_value *out, pc_error *err);

/* Draw the dialog body. Called from the dialog render loop when the __web marker is set. Sets the
 * root's __choose flag to true when the user asks for the Save file chooser. */
void pc_ui_shell_web_export_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog);

#endif /* PC_UI_SHELL_WEB_EXPORT_UI_H */
