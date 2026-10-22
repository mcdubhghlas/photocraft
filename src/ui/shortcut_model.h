/* Keyboard Shortcuts / Menus editor model (B8): the editable list of commands with their menu
 * path and default shortcut, plus key-capture to a shortcut string. Pure logic, no GUI, so the
 * list shape and capture rules are unit-tested without a window (port of prefs_ui.rs
 * shortcut_items / shortcut_text). */
#ifndef PC_UI_SHORTCUT_MODEL_H
#define PC_UI_SHORTCUT_MODEL_H

#include "engine/engine.h"
#include "ui/shortcuts.h"
#include <stdbool.h>
#include <stddef.h>

/* One editable command: owned strings; `def` is the normalised default shortcut, or NULL. The
 * menu path is up to three owned segments (depth == 0 means "no menu", listed under its group). */
typedef struct pc_ui_shortcut_item {
    char *id;
    char *label;
    char *path[3];
    size_t depth;
    char *def;
} pc_ui_shortcut_item;

/* Build the shortcut editor list for the session: every menu command (de-duplicated), the
 * shortcut-bearing commands with no menu item (Tools / Layer), the toolbar tools and the held
 * temporary tools. Writes an owned array of *n items into *out; free with
 * pc_ui_shortcut_items_free. */
pc_status pc_ui_shortcut_items(const pc_engine_session *s, pc_ui_shortcut_item **out, size_t *n,
                               pc_error *err);
void pc_ui_shortcut_items_free(pc_ui_shortcut_item *items, size_t n);

/* Turn a captured key press (logical key + modifier mask) into a normalised shortcut string, as
 * the editor records it. Returns false for a bare modifier or an unrepresentable key; out[cap] is
 * always NUL-terminated when cap > 0. Mirrors prefs_ui.rs shortcut_text. */
bool pc_ui_shortcut_capture(pc_ui_key key, unsigned mods, char *out, size_t cap);

#endif /* PC_UI_SHORTCUT_MODEL_H */
