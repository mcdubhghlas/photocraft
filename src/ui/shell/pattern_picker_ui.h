#ifndef PC_UI_SHELL_PATTERN_PICKER_UI_H
#define PC_UI_SHELL_PATTERN_PICKER_UI_H

#include "app.h"
#include "dialogs.h"

/* A pattern picker for a modal field holding a pattern id/name (Layer Style Pattern Overlay and
 * Bevel texture, Layer > New Fill Layer > Pattern). Draws the current pattern's thumbnail and a
 * dropdown of every pattern (pattern.list) with thumbnails (pc_engine_pattern_thumbnail); choosing
 * one writes its id into `params[key]` (an object owned by `dialog->fields`). `mapping_prefix`
 * namespaces the control-channel widget ids so several pickers in one dialog don't collide.
 * NULL-safe; never crashes on any session or pattern state (REWRITE_PLAN.md rule 9). */
void pc_ui_shell_pattern_picker(pc_ui_shell_app *app, pc_ui_dialog *dialog, yyjson_mut_val *params,
                                const char *key, const char *current, const char *mapping_prefix);

#endif
