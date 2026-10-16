/* Keyboard shortcuts: parse/format/match plus default and effective bindings.
 * Port of the data-and-logic half of crates/ui-egui/src/shortcuts.rs. */
#include "ui/shortcuts.h"

#include <stdio.h>
#include <string.h>

#include "engine/prefs.h"
#include "ui/menu_catalog.h"
#include "ui/menus.h"

/* ---- key names (egui::Key) ---------------------------------------------------------------- */

static const struct {
    pc_ui_key key;
    const char *name;
} KEY_NAMES[] = {
    {PC_KEY_A, "A"}, {PC_KEY_B, "B"}, {PC_KEY_C, "C"}, {PC_KEY_D, "D"}, {PC_KEY_E, "E"},
    {PC_KEY_F, "F"}, {PC_KEY_G, "G"}, {PC_KEY_H, "H"}, {PC_KEY_I, "I"}, {PC_KEY_J, "J"},
    {PC_KEY_K, "K"}, {PC_KEY_L, "L"}, {PC_KEY_M, "M"}, {PC_KEY_N, "N"}, {PC_KEY_O, "O"},
    {PC_KEY_P, "P"}, {PC_KEY_Q, "Q"}, {PC_KEY_R, "R"}, {PC_KEY_S, "S"}, {PC_KEY_T, "T"},
    {PC_KEY_U, "U"}, {PC_KEY_V, "V"}, {PC_KEY_W, "W"}, {PC_KEY_X, "X"}, {PC_KEY_Y, "Y"},
    {PC_KEY_Z, "Z"},
    {PC_KEY_NUM0, "Num0"}, {PC_KEY_NUM1, "Num1"}, {PC_KEY_NUM2, "Num2"}, {PC_KEY_NUM3, "Num3"},
    {PC_KEY_NUM4, "Num4"}, {PC_KEY_NUM5, "Num5"}, {PC_KEY_NUM6, "Num6"}, {PC_KEY_NUM7, "Num7"},
    {PC_KEY_NUM8, "Num8"}, {PC_KEY_NUM9, "Num9"},
    {PC_KEY_F1, "F1"}, {PC_KEY_F2, "F2"}, {PC_KEY_F3, "F3"}, {PC_KEY_F4, "F4"}, {PC_KEY_F5, "F5"},
    {PC_KEY_F6, "F6"}, {PC_KEY_F7, "F7"}, {PC_KEY_F8, "F8"}, {PC_KEY_F9, "F9"}, {PC_KEY_F10, "F10"},
    {PC_KEY_F11, "F11"}, {PC_KEY_F12, "F12"},
    {PC_KEY_ENTER, "Enter"}, {PC_KEY_TAB, "Tab"}, {PC_KEY_SPACE, "Space"},
    {PC_KEY_BACKSPACE, "Backspace"}, {PC_KEY_DELETE, "Delete"}, {PC_KEY_ESCAPE, "Escape"},
    {PC_KEY_INSERT, "Insert"}, {PC_KEY_HOME, "Home"}, {PC_KEY_END, "End"},
    {PC_KEY_PAGEUP, "PageUp"}, {PC_KEY_PAGEDOWN, "PageDown"},
    {PC_KEY_ARROWUP, "ArrowUp"}, {PC_KEY_ARROWDOWN, "ArrowDown"},
    {PC_KEY_ARROWLEFT, "ArrowLeft"}, {PC_KEY_ARROWRIGHT, "ArrowRight"},
    {PC_KEY_MINUS, "Minus"}, {PC_KEY_PLUS, "Plus"}, {PC_KEY_EQUALS, "Equals"},
    {PC_KEY_SEMICOLON, "Semicolon"}, {PC_KEY_COLON, "Colon"}, {PC_KEY_QUOTE, "Quote"},
    {PC_KEY_BACKTICK, "Backtick"}, {PC_KEY_BACKSLASH, "Backslash"}, {PC_KEY_PIPE, "Pipe"},
    {PC_KEY_SLASH, "Slash"}, {PC_KEY_QUESTIONMARK, "Questionmark"},
    {PC_KEY_OPENBRACKET, "OpenBracket"}, {PC_KEY_CLOSEBRACKET, "CloseBracket"},
    {PC_KEY_OPENCURLYBRACKET, "OpenCurlyBracket"}, {PC_KEY_CLOSECURLYBRACKET, "CloseCurlyBracket"},
    {PC_KEY_COMMA, "Comma"}, {PC_KEY_PERIOD, "Period"}, {PC_KEY_EXCLAMATIONMARK, "Exclamationmark"},
};
enum { KEY_NAMES_N = (int)(sizeof(KEY_NAMES) / sizeof(KEY_NAMES[0])) };

pc_ui_key pc_ui_key_from_name(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return PC_KEY_NONE;
    }
    /* Single letter (case-insensitive): "a"/"A" -> PC_KEY_A. */
    if (name[1] == '\0') {
        char c = name[0];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        if (c >= 'A' && c <= 'Z') {
            return (pc_ui_key)(PC_KEY_A + (c - 'A'));
        }
        if (c >= '0' && c <= '9') {
            return (pc_ui_key)(PC_KEY_NUM0 + (c - '0'));
        }
    }
    for (int i = 0; i < KEY_NAMES_N; i++) {
        if (strcmp(KEY_NAMES[i].name, name) == 0) {
            return KEY_NAMES[i].key;
        }
    }
    return PC_KEY_NONE;
}

const char *pc_ui_key_name(pc_ui_key k)
{
    for (int i = 0; i < KEY_NAMES_N; i++) {
        if (KEY_NAMES[i].key == k) {
            return KEY_NAMES[i].name;
        }
    }
    return NULL;
}

/* ---- parse -------------------------------------------------------------------------------- */

bool pc_ui_shortcut_parse(const char *s, pc_ui_shortcut *out)
{
    if (s == NULL || out == NULL) {
        return false;
    }
    size_t len = strlen(s);
    /* Split the key off first (a trailing '+' names the Plus key; "Cmd+" and "Cmd++" both work). */
    const char *prefix;
    size_t prefix_len;
    const char *key_name;
    char kbuf[64];
    if (len > 0 && s[len - 1] == '+') {
        /* strip the final '+', then one more '+' if present as separator */
        size_t p = len - 1;
        if (p > 0 && s[p - 1] == '+') {
            p -= 1;
        }
        prefix = s;
        prefix_len = p;
        key_name = "+";
    } else {
        const char *last = NULL;
        for (const char *c = s; *c; c++) {
            if (*c == '+') {
                last = c;
            }
        }
        if (last == NULL) {
            prefix = "";
            prefix_len = 0;
            key_name = s;
        } else {
            prefix = s;
            prefix_len = (size_t)(last - s);
            key_name = last + 1;
        }
    }

    unsigned mods = PC_UI_MOD_NONE;
    if (prefix_len > 0) {
        /* Split the prefix on '+' exactly as Rust's str::split does: N separators yield N+1
         * tokens, so a trailing '+' produces a final empty token (which is rejected). */
        size_t i = 0;
        for (;;) {
            size_t j = i;
            while (j < prefix_len && prefix[j] != '+') {
                j++;
            }
            size_t plen = j - i;
            const char *part = prefix + i;
            if (plen == 3 && strncmp(part, "Cmd", 3) == 0 && !(mods & PC_UI_MOD_COMMAND)) {
                mods |= PC_UI_MOD_COMMAND;
            } else if (plen == 5 && strncmp(part, "Shift", 5) == 0 && !(mods & PC_UI_MOD_SHIFT)) {
                mods |= PC_UI_MOD_SHIFT;
            } else if (plen == 3 && strncmp(part, "Alt", 3) == 0 && !(mods & PC_UI_MOD_ALT)) {
                mods |= PC_UI_MOD_ALT;
            } else if (plen == 4 && strncmp(part, "Ctrl", 4) == 0 && !(mods & PC_UI_MOD_CTRL)) {
                mods |= PC_UI_MOD_CTRL;
            } else {
                return false; /* unknown component, duplicate modifier, or empty token */
            }
            if (j >= prefix_len) {
                break;
            }
            i = j + 1; /* skip the '+' */
        }
    }

    pc_ui_key key;
    if (strcmp(key_name, "+") == 0) {
        key = PC_KEY_PLUS;
    } else if (strcmp(key_name, "=") == 0) {
        key = PC_KEY_EQUALS;
    } else if (strcmp(key_name, "-") == 0) {
        key = PC_KEY_MINUS;
    } else if (strcmp(key_name, "[") == 0) {
        key = PC_KEY_OPENBRACKET;
    } else if (strcmp(key_name, ";") == 0) {
        key = PC_KEY_SEMICOLON;
    } else if (strcmp(key_name, "'") == 0) {
        key = PC_KEY_QUOTE;
    } else if (strcmp(key_name, "]") == 0) {
        key = PC_KEY_CLOSEBRACKET;
    } else if (strcmp(key_name, "`") == 0) {
        key = PC_KEY_BACKTICK;
    } else {
        /* copy key_name into a bounded buffer for from_name */
        size_t kl = strlen(key_name);
        if (kl >= sizeof kbuf) {
            return false;
        }
        memcpy(kbuf, key_name, kl + 1);
        key = pc_ui_key_from_name(kbuf);
    }
    if (key == PC_KEY_NONE) {
        return false;
    }
    out->mods = mods;
    out->key = key;
    return true;
}

/* ---- pretty ------------------------------------------------------------------------------- */

bool pc_ui_shortcut_platform_is_mac(void)
{
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

/* Append src to out[cap] starting at *pos (always counts the full wanted length). */
static void putstr(char *out, size_t cap, size_t *pos, const char *src)
{
    for (const char *c = src; *c; c++) {
        if (*pos + 1 < cap) {
            out[*pos] = *c;
        }
        (*pos)++;
    }
}

size_t pc_ui_shortcut_pretty_ex(const char *s, bool mac, char *out, size_t cap)
{
    size_t pos = 0;
    if (s == NULL) {
        if (cap > 0) {
            out[0] = '\0';
        }
        return 0;
    }
    const char *sep = mac ? "" : "+";
    size_t len = strlen(s);
    bool trailing_plus = len > 0 && s[len - 1] == '+';

    /* Iterate '+'-separated parts; a trailing '+' is the Plus key, not an empty component. */
    size_t i = 0;
    bool first = true;
    /* Effective length excludes the trailing '+' (and a possible empty part before it). */
    while (i <= len) {
        size_t j = i;
        while (j < len && s[j] != '+') {
            j++;
        }
        size_t plen = j - i;
        const char *part = s + i;
        bool is_last_region = (j >= len);
        if (trailing_plus && is_last_region) {
            /* The region after the last real '+' is empty; emit the Plus key once. */
            if (!first) {
                putstr(out, cap, &pos, sep);
            }
            putstr(out, cap, &pos, "+");
            break;
        }
        if (plen == 0) {
            /* Empty component (e.g. the middle of "Cmd++"): drop it, as Rust pops it. */
            if (is_last_region) {
                break;
            }
            i = j + 1;
            continue;
        }
        /* translate this part */
        char buf[32];
        const char *emit;
        if (plen == 3 && strncmp(part, "Cmd", 3) == 0) {
            emit = mac ? "\xE2\x8C\x98" : "Ctrl"; /* ⌘ */
        } else if (plen == 5 && strncmp(part, "Shift", 5) == 0 && mac) {
            emit = "\xE2\x87\xA7"; /* ⇧ */
        } else if (plen == 3 && strncmp(part, "Alt", 3) == 0 && mac) {
            emit = "\xE2\x8C\xA5"; /* ⌥ */
        } else if (plen == 5 && strncmp(part, "Enter", 5) == 0 && mac) {
            emit = "\xE2\x86\xA9"; /* ↩ */
        } else if (plen == 6 && strncmp(part, "Delete", 6) == 0 && mac) {
            emit = "\xE2\x8C\xAB"; /* ⌫ */
        } else {
            size_t n = plen < sizeof buf - 1 ? plen : sizeof buf - 1;
            memcpy(buf, part, n);
            buf[n] = '\0';
            emit = buf;
        }
        if (!first) {
            putstr(out, cap, &pos, sep);
        }
        putstr(out, cap, &pos, emit);
        first = false;
        if (is_last_region) {
            break;
        }
        i = j + 1;
    }
    if (cap > 0) {
        out[pos < cap ? pos : cap - 1] = '\0';
    }
    return pos;
}

size_t pc_ui_shortcut_pretty(const char *s, char *out, size_t cap)
{
    return pc_ui_shortcut_pretty_ex(s, pc_ui_shortcut_platform_is_mac(), out, cap);
}

/* ---- matching ----------------------------------------------------------------------------- */

/* With ⇧ held the OS reports the shifted character as the logical key (US layout). */
static pc_ui_key shifted(pc_ui_key k)
{
    switch (k) {
    case PC_KEY_EQUALS:
        return PC_KEY_PLUS;
    case PC_KEY_SEMICOLON:
        return PC_KEY_COLON;
    case PC_KEY_OPENBRACKET:
        return PC_KEY_OPENCURLYBRACKET;
    case PC_KEY_CLOSEBRACKET:
        return PC_KEY_CLOSECURLYBRACKET;
    case PC_KEY_SLASH:
        return PC_KEY_QUESTIONMARK;
    case PC_KEY_BACKSLASH:
        return PC_KEY_PIPE;
    case PC_KEY_NUM1:
        return PC_KEY_EXCLAMATIONMARK;
    default:
        return PC_KEY_NONE;
    }
}

static bool is_shifted_symbol(pc_ui_key key)
{
    switch (key) {
    case PC_KEY_PLUS:
    case PC_KEY_COLON:
    case PC_KEY_OPENCURLYBRACKET:
    case PC_KEY_CLOSECURLYBRACKET:
    case PC_KEY_QUESTIONMARK:
    case PC_KEY_PIPE:
    case PC_KEY_EXCLAMATIONMARK:
        return true;
    default:
        return false;
    }
}

bool pc_ui_shortcut_key_matches(const pc_ui_shortcut *sc, pc_ui_key key, unsigned mods)
{
    if (sc == NULL) {
        return false;
    }
    pc_ui_key k = sc->key;
    bool sc_shift = (sc->mods & PC_UI_MOD_SHIFT) != 0;
    bool direct = key == k || (k == PC_KEY_DELETE && key == PC_KEY_BACKSPACE) ||
                  (k == PC_KEY_BACKSPACE && key == PC_KEY_DELETE);
    bool plus = k == PC_KEY_EQUALS && key == PC_KEY_PLUS;
    bool via_shift = sc_shift && shifted(k) == key && key != PC_KEY_NONE;
    if (!(direct || plus || via_shift)) {
        return false;
    }
    bool want_alt = (sc->mods & PC_UI_MOD_ALT) != 0;
    bool want_cmd = (sc->mods & PC_UI_MOD_COMMAND) != 0;
    bool want_ctrl = (sc->mods & PC_UI_MOD_CTRL) != 0;
    bool have_alt = (mods & PC_UI_MOD_ALT) != 0;
    bool have_shift = (mods & PC_UI_MOD_SHIFT) != 0;
    bool have_cmd = (mods & PC_UI_MOD_COMMAND) != 0;
    bool have_ctrl = (mods & PC_UI_MOD_CTRL) != 0;
    /* Typing a shifted symbol may need ⇧ on this layout: don't require it absent. */
    bool shift_ok = have_shift == sc_shift || (is_shifted_symbol(key) && !sc_shift);
    /* Modifiers::cmd_ctrl_matches: off the Mac, Cmd and Ctrl are the same primary modifier
     * (pressing Ctrl sets both), so match on "a primary modifier is held" rather than on the raw
     * bit; on the Mac ⌘ and Control are distinct and both must match. */
#if defined(__APPLE__)
    bool cmd_ctrl_ok = have_cmd == want_cmd && have_ctrl == want_ctrl;
#else
    bool cmd_ctrl_ok = (have_cmd || have_ctrl) == (want_cmd || want_ctrl);
#endif
    return have_alt == want_alt && shift_ok && cmd_ctrl_ok;
}

/* ---- default / effective bindings --------------------------------------------------------- */

bool pc_ui_default_shortcut(const char *id, char *out, size_t cap)
{
    if (id == NULL || out == NULL || cap == 0) {
        return false;
    }
    /* 1) UI command's own shortcut. */
    const pc_ui_command *uc = pc_ui_command_find(id);
    if (uc != NULL && uc->shortcut != NULL) {
        snprintf(out, cap, "%s", uc->shortcut);
        return true;
    }
    /* 2) Engine registry shortcut. */
    const pc_engine_command *ec = pc_engine_command_find(id);
    if (ec != NULL && ec->shortcut != NULL) {
        snprintf(out, cap, "%s", ec->shortcut);
        return true;
    }
    /* 3) Catalogue shortcut (first row with this id). */
    for (size_t i = 0; i < PC_UI_CATALOG_N; i++) {
        if (strcmp(PC_UI_CATALOG[i].id, id) == 0 && PC_UI_CATALOG[i].shortcut != NULL) {
            snprintf(out, cap, "%s", PC_UI_CATALOG[i].shortcut);
            return true;
        }
    }
    /* 4) Temporary tool default. */
    size_t ntt = 0;
    const pc_engine_temporary_tool *tt = pc_engine_temporary_tools(&ntt);
    for (size_t i = 0; i < ntt; i++) {
        if (strcmp(tt[i].id, id) == 0) {
            snprintf(out, cap, "%s", tt[i].def);
            return true;
        }
    }
    /* 5) Toolbar tool single-key default. */
    pc_ui_tool tool;
    if (pc_ui_tool_from_select_id(id, &tool)) {
        char key = pc_ui_tool_key(tool);
        if (key != '\0') {
            if (cap >= 2) {
                out[0] = key;
                out[1] = '\0';
            }
            return true;
        }
    }
    return false;
}

bool pc_ui_effective_shortcut(const pc_engine_session *s, const char *id, char *out, size_t cap)
{
    if (id == NULL || out == NULL || cap == 0) {
        return false;
    }
    char def[128];
    bool has_def = pc_ui_default_shortcut(id, def, sizeof def);
    if (s != NULL) {
        return pc_engine_prefs_effective_shortcut(s, id, has_def ? def : NULL, out, cap);
    }
    if (has_def) {
        snprintf(out, cap, "%s", def);
        return true;
    }
    return false;
}

/* ---- navigation --------------------------------------------------------------------------- */

const char *const PC_UI_NAV_COMMANDS[4] = {"view.zoomIn", "view.zoomOut", "view.fitOnScreen",
                                           "view.actualPixels"};

bool pc_ui_is_nav_command(const char *id)
{
    if (id == NULL) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (strcmp(PC_UI_NAV_COMMANDS[i], id) == 0) {
            return true;
        }
    }
    return false;
}
