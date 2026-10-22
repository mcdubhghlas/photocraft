/* Print modal dialog (clean-room port of crates/ui-egui/src/file_ui.rs __print). A Command dialog
 * (file.print) with a bespoke body: a live page preview, printer selection (from file.printers),
 * copies, paper + orientation, colour handling (profile / intent / black-point compensation),
 * position and size (center or top/left, scale, scale-to-fit), printing marks, emulsion down and a
 * Save-as-PDF path. Confirm dispatches file.print; a Print One Copy button dispatches
 * file.printOneCopy. Thin; NULL-safe; never crashes (REWRITE_PLAN.md rule 9). */
#ifndef PC_UI_SHELL_PRINT_UI_H
#define PC_UI_SHELL_PRINT_UI_H

#include "app.h"
#include "dialogs.h"

/* Open the Print dialog (invoked for the no-params file.print command). Queries file.printers. */
pc_status pc_ui_shell_print_open(pc_ui_shell_app *app, pc_json_value *out, pc_error *err);

/* Draw the dialog body. Called from the dialog render loop when the __print marker is set. */
void pc_ui_shell_print_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog);

#endif /* PC_UI_SHELL_PRINT_UI_H */
