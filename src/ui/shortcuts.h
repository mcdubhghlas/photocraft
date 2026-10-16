/* Keyboard shortcuts: Photoshop defaults from the command registry plus single-key tools, and the
 * parsing/formatting/matching helpers. Port of the data-and-logic half of
 * crates/ui-egui/src/shortcuts.rs (the egui input-event handling — handle(), consume(),
 * clipboard_keys() — is reimplemented against SDL when the app shell is assembled, U2+). No GUI
 * dependency at this layer: a pc_ui_shortcut is a (modifiers, key) pair the shell maps to events. */
#ifndef PC_UI_SHORTCUTS_H
#define PC_UI_SHORTCUTS_H

#include <stdbool.h>
#include <stddef.h>

#include "engine/engine.h"
#include "ui/state.h"

/* Modifier bitmask (matches egui::Modifiers semantics; `command` is ⌘ on macOS, Ctrl elsewhere). */
enum {
    PC_UI_MOD_NONE = 0,
    PC_UI_MOD_ALT = 1u << 0,
    PC_UI_MOD_SHIFT = 1u << 1,
    PC_UI_MOD_COMMAND = 1u << 2, /* Cmd (⌘ / Ctrl) */
    PC_UI_MOD_CTRL = 1u << 3     /* an explicit Ctrl distinct from Cmd (used on-Mac only) */
};

/* A logical key (egui::Key). Only the keys reachable through shortcut strings and Photoshop's
 * matching rules are modelled. Values are arbitrary; compare with ==. */
typedef enum pc_ui_key {
    PC_KEY_NONE = 0,
    /* Letters A..Z */
    PC_KEY_A, PC_KEY_B, PC_KEY_C, PC_KEY_D, PC_KEY_E, PC_KEY_F, PC_KEY_G, PC_KEY_H, PC_KEY_I,
    PC_KEY_J, PC_KEY_K, PC_KEY_L, PC_KEY_M, PC_KEY_N, PC_KEY_O, PC_KEY_P, PC_KEY_Q, PC_KEY_R,
    PC_KEY_S, PC_KEY_T, PC_KEY_U, PC_KEY_V, PC_KEY_W, PC_KEY_X, PC_KEY_Y, PC_KEY_Z,
    /* Digits 0..9 (egui Num0..Num9) */
    PC_KEY_NUM0, PC_KEY_NUM1, PC_KEY_NUM2, PC_KEY_NUM3, PC_KEY_NUM4,
    PC_KEY_NUM5, PC_KEY_NUM6, PC_KEY_NUM7, PC_KEY_NUM8, PC_KEY_NUM9,
    /* Function keys */
    PC_KEY_F1, PC_KEY_F2, PC_KEY_F3, PC_KEY_F4, PC_KEY_F5, PC_KEY_F6,
    PC_KEY_F7, PC_KEY_F8, PC_KEY_F9, PC_KEY_F10, PC_KEY_F11, PC_KEY_F12,
    /* Named keys */
    PC_KEY_ENTER, PC_KEY_TAB, PC_KEY_SPACE, PC_KEY_BACKSPACE, PC_KEY_DELETE, PC_KEY_ESCAPE,
    PC_KEY_INSERT, PC_KEY_HOME, PC_KEY_END, PC_KEY_PAGEUP, PC_KEY_PAGEDOWN,
    PC_KEY_ARROWUP, PC_KEY_ARROWDOWN, PC_KEY_ARROWLEFT, PC_KEY_ARROWRIGHT,
    /* Punctuation / symbols */
    PC_KEY_MINUS, PC_KEY_PLUS, PC_KEY_EQUALS, PC_KEY_SEMICOLON, PC_KEY_COLON, PC_KEY_QUOTE,
    PC_KEY_BACKTICK, PC_KEY_BACKSLASH, PC_KEY_PIPE, PC_KEY_SLASH, PC_KEY_QUESTIONMARK,
    PC_KEY_OPENBRACKET, PC_KEY_CLOSEBRACKET, PC_KEY_OPENCURLYBRACKET, PC_KEY_CLOSECURLYBRACKET,
    PC_KEY_COMMA, PC_KEY_PERIOD, PC_KEY_EXCLAMATIONMARK
} pc_ui_key;

/* A parsed keyboard shortcut: a modifier mask plus one logical key. */
typedef struct pc_ui_shortcut {
    unsigned mods;
    pc_ui_key key;
} pc_ui_shortcut;

/* egui::Key::from_name: the canonical name for a key, or PC_KEY_NONE. Accepts single letters
 * (case-insensitive), single digits, "F1".."F12" and the named keys above. */
pc_ui_key pc_ui_key_from_name(const char *name);
/* The canonical egui name of a key (e.g. "A", "Num0", "Enter", "Plus"), or NULL for PC_KEY_NONE. */
const char *pc_ui_key_name(pc_ui_key k);

/* Parse a "Cmd+Shift+N" string. Returns true and fills *out, or false on a malformed binding
 * (unknown component, duplicate modifier, unknown key). A trailing '+' names the Plus key. */
bool pc_ui_shortcut_parse(const char *s, pc_ui_shortcut *out);

/* Human-readable form for menus/tooltips. `mac` chooses ⌘⇧N (true) or Ctrl+Shift+N (false)
 * notation. Writes into out[cap] (always NUL-terminated when cap>0); returns the length it wanted. */
size_t pc_ui_shortcut_pretty_ex(const char *s, bool mac, char *out, size_t cap);
/* As above, in the current platform's notation. */
size_t pc_ui_shortcut_pretty(const char *s, char *out, size_t cap);
/* Whether this build formats shortcuts in macOS notation. */
bool pc_ui_shortcut_platform_is_mac(void);

/* Does a key press (key + modifier mask) match a shortcut, the way Photoshop matches them?
 * (Delete<->Backspace, = matches +, ⇧-punctuation shifted characters, exact modifiers otherwise.) */
bool pc_ui_shortcut_key_matches(const pc_ui_shortcut *sc, pc_ui_key key, unsigned mods);

/* The default (unmodified) shortcut of a command id: UI command, else engine registry, else the
 * catalogue, else a temporary tool, else a toolbar tool's single-key default. Writes into out[cap]
 * and returns true, or false when the id has no default binding. */
bool pc_ui_default_shortcut(const char *id, char *out, size_t cap);

/* The shortcut in effect for a command id: the session's Edit › Keyboard Shortcuts override, else
 * its default ([`pc_ui_default_shortcut`]). Returns false when the command has no binding. */
bool pc_ui_effective_shortcut(const pc_engine_session *s, const char *id, char *out, size_t cap);

/* View navigation Photoshop keeps live while a dialog is open. */
extern const char *const PC_UI_NAV_COMMANDS[4];
bool pc_ui_is_nav_command(const char *id);

#endif /* PC_UI_SHORTCUTS_H */
