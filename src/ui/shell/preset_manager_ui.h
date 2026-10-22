/* Edit › Presets › Preset Manager and Export/Import Presets modals (U10 B8). Two command-form
 * dialogs that drive the edit.presets.presetManager and edit.presets.exportImportPresets engine
 * commands: the manager lists a chosen library (brushes / custom shapes / patterns) with
 * select / rename / delete / reorder, and Export/Import round-trips brush + custom-shape sets through
 * a "photocraft-presets" file via the native file chooser. Port of prefs_ui.rs presets_body /
 * presets_io_body / confirm against SDL3 + Nuklear; the list/selection logic lives in
 * src/ui/preset_manager.c. */
#ifndef PC_UI_SHELL_PRESET_MANAGER_UI_H
#define PC_UI_SHELL_PRESET_MANAGER_UI_H

#include "app.h"
#include "dialogs.h"

/* ---- Preset Manager ---- */

pc_status pc_ui_shell_preset_manager_open(pc_ui_shell_app *app, pc_json_value *out, pc_error *err);
bool pc_ui_shell_preset_manager_owns(yyjson_mut_val *fields);
void pc_ui_shell_preset_manager_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime,
                                     pc_ui_dialog *dialog);

/* ---- Export/Import Presets ---- */

pc_status pc_ui_shell_presets_io_open(pc_ui_shell_app *app, pc_json_value *out, pc_error *err);
bool pc_ui_shell_presets_io_owns(yyjson_mut_val *fields);
void pc_ui_shell_presets_io_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime,
                                 pc_ui_dialog *dialog);

/* OK on the Export/Import dialog: decide the direction. For Export, run the export command now and
 * stash the serialised file text in the dialog (`*saving` = true, so the caller opens a Save
 * chooser); for Import, `*saving` = false (the caller opens an Open chooser). The chooser result is
 * then handed to pc_ui_shell_presets_io_write / _read. */
pc_status pc_ui_shell_presets_io_begin(pc_ui_shell_app *app, pc_ui_dialog *dialog, bool *saving,
                                       pc_error *err);
/* Write the stashed export text to `path` (the Save chooser result). */
pc_status pc_ui_shell_presets_io_write(pc_ui_shell_app *app, pc_ui_dialog *dialog, const char *path,
                                       pc_error *err);
/* Read `path` (the Open chooser result) and import it through the engine. */
pc_status pc_ui_shell_presets_io_read(pc_ui_shell_app *app, pc_ui_dialog *dialog, const char *path,
                                      pc_error *err);

#endif /* PC_UI_SHELL_PRESET_MANAGER_UI_H */
