/* Preset Manager model (Edit › Presets › Preset Manager): the three libraries the engine command
 * edit.presets.presetManager manages (brushes, custom shapes, patterns), parsing its `list` reply
 * into owned names, and the selection/reorder rules. Pure logic, no GUI, so the list shape and the
 * move/selection rules are unit-tested without a window (companion to the shell modal in
 * src/ui/shell/preset_manager_ui.c; mirrors prefs_ui.rs presets_body). */
#ifndef PC_UI_PRESET_MANAGER_H
#define PC_UI_PRESET_MANAGER_H

#include "base/json.h"
#include "base/status.h"
#include <stdbool.h>
#include <stddef.h>

/* One preset library the manager offers: its command id ("brushes") and display label. */
typedef struct pc_ui_preset_kind {
    const char *id;
    const char *label;
} pc_ui_preset_kind;

/* The libraries in menu order (brushes, customShapes, patterns). Writes the borrowed, static table
 * into *out and returns its length (always 3). */
size_t pc_ui_preset_kinds(const pc_ui_preset_kind **out);

/* Is `id` one of the manager's preset kinds? NULL-safe. */
bool pc_ui_preset_kind_valid(const char *id);

/* Parse the `list` reply of edit.presets.presetManager for one kind: reply[kind] is an array of
 * names. Writes an owned array of *n owned C strings into *out (empty array -> *out NULL, *n 0);
 * free with pc_ui_preset_names_free. A missing or malformed field is treated as an empty list (not
 * an error). `reply` may be NULL. */
pc_status pc_ui_preset_list_parse(yyjson_mut_val *reply, const char *kind, char ***out, size_t *n,
                                  pc_error *err);
void pc_ui_preset_names_free(char **names, size_t n);

/* Clamp a stored selection to a list of `count` items: a selection past the end snaps to the last
 * item, and 0 for an empty list. */
size_t pc_ui_preset_clamp(size_t selected, size_t count);

/* Whether the selected item (already clamped) can move toward the start / end of a `count` list. */
bool pc_ui_preset_can_move_up(size_t selected, size_t count);
bool pc_ui_preset_can_move_down(size_t selected, size_t count);

#endif /* PC_UI_PRESET_MANAGER_H */
