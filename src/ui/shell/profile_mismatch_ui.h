/* Embedded Profile Mismatch prompt (Edit › Color Settings open policy). When file.openAs returns a
 * `color` report whose policy asked to prompt, the shell auto-opens this modal (Use embedded /
 * Convert to working / Discard, or Assign for a missing profile) and resolves it through the
 * color.profileMismatch command. Port of prefs_ui.rs open_mismatch / mismatch_body / confirm against
 * SDL3 + Nuklear; the parsing/gate live in src/ui/profile_mismatch.c. */
#ifndef PC_UI_SHELL_PROFILE_MISMATCH_UI_H
#define PC_UI_SHELL_PROFILE_MISMATCH_UI_H

#include "app.h"
#include "dialogs.h"

/* Open the prompt for a file.openAs `color` report, but only when the report asks for it
 * (pc_ui_profile_mismatch_should_open). A no-op otherwise. Best-effort: a failure to open is
 * returned but never aborts the open it followed. `report` may be NULL. */
pc_status pc_ui_shell_profile_mismatch_maybe_open(pc_ui_shell_app *app, yyjson_mut_val *report,
                                                  pc_error *err);

/* Does this dialog's fields belong to the profile-mismatch prompt? */
bool pc_ui_shell_profile_mismatch_owns(yyjson_mut_val *fields);

/* Render the prompt body (message + radio choices). */
void pc_ui_shell_profile_mismatch_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime,
                                       pc_ui_dialog *dialog);

/* OK: dispatch color.profileMismatch for the chosen action (a no-op when it matches what the open
 * policy already applied) and close the prompt. */
pc_status pc_ui_shell_profile_mismatch_confirm(pc_ui_shell_app *app, uint64_t id, pc_json_value *out,
                                               pc_error *err);

#endif /* PC_UI_SHELL_PROFILE_MISMATCH_UI_H */
