/* Menu bar model: the Photoshop menu tree (ui/menu_catalog.h) plus the shell's own UI-level
 * commands, and the liveness predicate shared with the parity report. Port of the data-and-logic
 * half of crates/ui-egui/src/menus.rs. The interactive parts (invoke/dispatch, rendering, the
 * per-view `checked`/`is_enabled` that read PhotocraftApp view state) are reimplemented when the
 * app shell is assembled (later U-chunks); this layer provides the menu *model* every frontend and
 * the parity report build on. No GUI dependency. */
#ifndef PC_UI_MENUS_H
#define PC_UI_MENUS_H

#include <stdbool.h>
#include <stddef.h>

#include "engine/engine.h"
#include "ui/state.h"

/* Top-level menus in Photoshop order. */
extern const char *const PC_UI_TOP_MENUS[10];

/* A UI-level command handled by the shell rather than the engine: id, label, menu path (`depth`
 * components, unused slots NULL), default shortcut (NULL = none). */
typedef struct pc_ui_command {
    const char *id;
    const char *label;
    const char *path[3];
    size_t depth;
    const char *shortcut;
} pc_ui_command;

/* The UI-level command table, and its length. */
extern const pc_ui_command PC_UI_COMMANDS[];
extern const size_t PC_UI_COMMANDS_N;
/* The UI command with this id, or NULL. */
const pc_ui_command *pc_ui_command_find(const char *id);

/* Prefix of a Window › <document> menu id (the open documents are listed at the Window menu's end,
 * their index appended). */
extern const char *const PC_UI_DOCUMENT_ITEM; /* "window.document." */

/* Photoshop's Window › <panel> id (window.panel.*) → the window.toggle.* shell command it runs,
 * or NULL when the shell has no panel for it. Borrowed static string. */
const char *pc_ui_panel_alias(const char *id);
/* View › Proof Setup preset id (view.proofSetup.*) → the proof profile it simulates, or NULL. */
const char *pc_ui_proof_preset(const char *id);
/* Window › Workspace preset id (window.workspace.*) → its workspace name ("" = Essentials/Reset),
 * or NULL when the id names no workspace. Returns true and sets *out when it is a workspace id. */
bool pc_ui_workspace_name(const char *id, const char **out);

/* The toolbar-tool select command id for a tool ("tools.select.<lowerCamelName>"). Writes into
 * out[cap]; returns the length it wanted. */
size_t pc_ui_tool_select_id(pc_ui_tool tool, char *out, size_t cap);
/* The tool a "tools.select.<name>" id names, verifying the id is exactly canonical. */
bool pc_ui_tool_from_select_id(const char *id, pc_ui_tool *out);

/* Is `id` implemented by the engine or the shell (a live menu item)? Shared by the menus and the
 * parity report. Mirrors menus::is_live: engine registry, toolbar tools, UI commands, panel
 * aliases, workspace/proof presets, and the id-sets handled by the view and panel UI modules. */
bool pc_ui_is_live(const char *id);

/* One resolved menu item for a frontend to draw. All strings are owned (free with
 * pc_ui_menu_items_free). `checked` is a tri-state: -1 = not a check item / unknown here. `enabled`
 * reflects the engine's current enablement for engine commands (always-enabled for UI commands);
 * per-view enablement that needs app state is applied by the shell. */
typedef struct pc_ui_menu_item {
    char *id;
    char *label;
    char *path[3];
    size_t depth;
    char *shortcut; /* NULL = none */
    bool enabled;
    int checked;
} pc_ui_menu_item;

/* Build the menu model (Photoshop catalogue, then shell UI commands and engine commands not in the
 * catalogue, de-duplicated and placed after the item they belong next to). `s` supplies command
 * enablement and effective shortcuts (may be NULL: everything is reported disabled with default
 * shortcuts). Writes an owned array of *n items into *out; free with pc_ui_menu_items_free. */
pc_status pc_ui_menu_items(const pc_engine_session *s, pc_ui_menu_item **out, size_t *n, pc_error *err);
void pc_ui_menu_items_free(pc_ui_menu_item *items, size_t n);

#endif /* PC_UI_MENUS_H */
