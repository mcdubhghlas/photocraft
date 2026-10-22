/* Load and save the desktop UI state (ui/state.c's pc_ui_state) to <config dir>/ui_state.json.
 *
 * The Rust reference (apps/photocraft/src/ui_state.rs) sanitised eframe's RON persistence file to
 * drop values that would crash wgpu/egui at startup. The C stack (SDL3 + Nuklear) has no such
 * persistence layer, so this module instead serialises pc_ui_state directly, keeping the same
 * robustness guarantee: a missing, truncated or malformed file never crashes startup; it is ignored
 * (and removed, so it cannot keep breaking launches) and the UI opens at its defaults
 * (REWRITE_PLAN.md §7). */
#ifndef PC_APP_UI_STATE_H
#define PC_APP_UI_STATE_H

#include <stddef.h>

#include "base/status.h"
#include "ui/state.h"

/* Write "<config_dir>/ui_state.json" into out[cap]; false when config_dir is NULL or too long. */
bool pc_app_ui_state_path(const char *config_dir, char *out, size_t cap);

/* Serialise `s` to the UI-state file under `config_dir` (atomically). Never writes when config_dir
 * is NULL. */
pc_status pc_app_ui_state_save(const pc_ui_state *s, const char *config_dir, pc_error *err);

/* Load the saved UI state into `s`, in place. Best-effort: on any read/parse error `s` is left as it
 * was (its defaults) and a malformed file is removed. Never crashes. */
void pc_app_ui_state_load(pc_ui_state *s, const char *config_dir);

#endif /* PC_APP_UI_STATE_H */
