/* TIFF Options: the prompt File › Save As shows before writing a layered TIFF, as Photoshop does
 * when "Ask Before Saving Layered TIFF Files" is on (Preferences › File Handling). Port of
 * crates/ui-egui/src/tiff_options_ui.rs (U10 B2). */
#ifndef PC_UI_SHELL_TIFF_OPTIONS_H
#define PC_UI_SHELL_TIFF_OPTIONS_H
#include "dialogs.h"
/* Whether a Save As to `path` should go through the prompt: a TIFF that would carry layer data,
 * with the preference on and a document open. */
bool pc_ui_shell_tiff_options_wants(pc_ui_shell_app *app, const char *path);
/* Parks the save behind a TIFF Options modal (a __tiffOptions command dialog). */
pc_status pc_ui_shell_tiff_options_open(pc_ui_shell_app *app, const char *path, pc_json_value *out,
                                        pc_error *err);
/* True when `fields` is a TIFF Options dialog. */
bool pc_ui_shell_is_tiff_options(yyjson_mut_val *fields);
void pc_ui_shell_tiff_options_body(pc_ui_shell_app *app, pc_ui_dialog *dialog);
/* OK: writes the save with/without layers, optionally clears the preference, closes the dialog. */
pc_status pc_ui_shell_tiff_options_confirm(pc_ui_shell_app *app, uint64_t id, pc_json_value *out,
                                           pc_error *err);
#endif
