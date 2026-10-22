/* Edit › Keyboard Shortcuts / Menus / Toolbar editor modal (U10 B8). A command-form dialog marked
 * "__prefsui":"shortcuts": the body lists every command with its binding (tab 0) or menu visibility
 * and colour (tab 1), or the toolbar tool set (tab 2); OK writes edit.keyboardShortcuts + prefs.set.
 * Port of prefs_ui.rs open_shortcuts / shortcuts_body / confirm against SDL3 + Nuklear. */
#ifndef PC_UI_SHELL_PREFS_MODALS_H
#define PC_UI_SHELL_PREFS_MODALS_H
#include "app.h"
#include "dialogs.h"
#include <SDL3/SDL.h>

/* Open the editor on a tab (0 Keyboard Shortcuts, 1 Menus, 2 Toolbar). */
pc_status pc_ui_shell_shortcuts_open(pc_ui_shell_app *app, uint64_t tab, pc_json_value *out, pc_error *err);

/* Does this dialog's fields belong to the shortcuts editor? */
bool pc_ui_shell_shortcuts_owns(yyjson_mut_val *fields);

/* Render the editor body. */
void pc_ui_shell_shortcuts_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime, pc_ui_dialog *dialog);

/* OK: commit the overrides and menu/toolbar visibility. */
pc_status pc_ui_shell_shortcuts_confirm(pc_ui_shell_app *app, uint64_t id, pc_json_value *out, pc_error *err);

/* Consume a key press for the open editor's capture mode (returns true when it handled the event,
 * so the normal dialog Enter/Escape handling is skipped). No-op when no editor is capturing. */
bool pc_ui_shell_shortcuts_key(pc_ui_shell_app *app, const SDL_Event *event);

#endif /* PC_UI_SHELL_PREFS_MODALS_H */
