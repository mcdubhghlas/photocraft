/* Keyboard shortcut binding table. Port of the pure table-building half of
 * crates/ui-egui/src/shortcut_dispatch.rs (`bindings`): the menu catalogue's defaults, commands'
 * own shortcuts and the shell's, merged with Edit › Keyboard Shortcuts overrides on top, each key
 * owned by exactly one command, ordered most-modifiers-first. The event dispatch itself (reading
 * egui/SDL key events, focus handling, the dry-run log) is reimplemented against SDL input when the
 * app shell is assembled (later U-chunks). No GUI dependency.
 *
 * Not yet modelled here (needs APIs or state that land with later chunks; the default table is
 * unaffected): the Actions-panel function keys, the window.panel.* ↔ window.toggle.* alias rule,
 * and overrides assigned to otherwise-unbound menu items (all three require enumerating the prefs
 * override map / the actions list). */
#ifndef PC_UI_SHORTCUT_DISPATCH_H
#define PC_UI_SHORTCUT_DISPATCH_H

#include <stddef.h>

#include "engine/engine.h"
#include "ui/shortcuts.h"

/* One resolved binding: the command id (borrowed static string) and the chord that fires it. */
typedef struct pc_ui_binding {
    const char *id;
    pc_ui_shortcut sc;
} pc_ui_binding;

/* Photoshop's second shortcuts, kept while the command's main one is the default:
 * Edit › Fill… is Shift+F5 (its menu default) and also Shift+Backspace. */
extern const struct pc_ui_secondary {
    const char *id;
    const char *shortcut;
} PC_UI_SECONDARY[1];

/* Build the binding table for the current session (overrides applied; `s` may be NULL for the
 * defaults). Writes an owned array of *n bindings into *out (free with free()); ids are borrowed
 * static strings, so the array itself is the only allocation. Ordered most-modifiers-first, one
 * command per chord. */
pc_status pc_ui_bindings(const pc_engine_session *s, pc_ui_binding **out, size_t *n, pc_error *err);

#endif /* PC_UI_SHORTCUT_DISPATCH_H */
