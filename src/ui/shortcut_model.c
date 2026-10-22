#include "shortcut_model.h"
#include "ui/menus.h"
#include "ui/state.h"
#include "engine/prefs.h"
#include "engine/fill_cmds.h"
#include "base/mem.h"
#include <stdlib.h>
#include <string.h>

static char *dup(const char *s)
{
    return s ? pc_strndup(s, strlen(s)) : NULL;
}

/* Growable item list. On any allocation failure the builder stops and reports PC_ERR_NOMEM; the
 * partial list is freed by the caller's error path. */
typedef struct builder {
    pc_ui_shortcut_item *items;
    size_t n, cap;
    bool ok;
} builder;

static bool contains(const builder *b, const char *id)
{
    for (size_t i = 0; i < b->n; i++) {
        if (b->items[i].id && id && !strcmp(b->items[i].id, id)) {
            return true;
        }
    }
    return false;
}

/* The default shortcut for an id as an owned normalised string, or NULL. */
static char *default_of(const char *id)
{
    char buf[128];
    if (pc_ui_default_shortcut(id, buf, sizeof buf) && buf[0]) {
        return dup(buf);
    }
    return NULL;
}

static void push(builder *b, const char *id, const char *label, const char *const path[3], size_t depth)
{
    if (!b->ok || !id || contains(b, id)) {
        return;
    }
    if (b->n == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 64;
        pc_ui_shortcut_item *grown = realloc(b->items, cap * sizeof *grown);
        if (!grown) {
            b->ok = false;
            return;
        }
        b->items = grown;
        b->cap = cap;
    }
    pc_ui_shortcut_item *it = &b->items[b->n];
    *it = (pc_ui_shortcut_item){0};
    it->id = dup(id);
    it->label = dup(label ? label : id);
    it->depth = depth > 3 ? 3 : depth;
    bool ok = it->id && it->label;
    for (size_t i = 0; i < it->depth; i++) {
        it->path[i] = dup(path && path[i] ? path[i] : "");
        ok = ok && it->path[i];
    }
    it->def = default_of(id);
    if (!ok) {
        /* Clean up this half-built row; the id/label were the only required strings. */
        free(it->id);
        free(it->label);
        for (size_t i = 0; i < 3; i++) {
            free(it->path[i]);
        }
        free(it->def);
        *it = (pc_ui_shortcut_item){0};
        b->ok = false;
        return;
    }
    b->n++;
}

static bool is_tools_key(const char *id)
{
    if (!strncmp(id, "tools.", 6)) {
        return true;
    }
    size_t n = 0;
    const pc_engine_command *fill = pc_engine_fill_key_specs(&n);
    for (size_t i = 0; i < n; i++) {
        if (fill[i].id && !strcmp(fill[i].id, id)) {
            return true;
        }
    }
    return false;
}

pc_status pc_ui_shortcut_items(const pc_engine_session *s, pc_ui_shortcut_item **out, size_t *n,
                               pc_error *err)
{
    if (!out || !n) {
        return pc_fail(err, PC_ERR_INVALID, "missing shortcut item output");
    }
    *out = NULL;
    *n = 0;
    builder b = {.ok = true};

    /* 1. Every menu command, de-duplicated, in menu order. */
    pc_ui_menu_item *menu = NULL;
    size_t menu_n = 0;
    pc_status rc = pc_ui_menu_items(s, &menu, &menu_n, err);
    if (rc != PC_OK) {
        return rc;
    }
    for (size_t i = 0; i < menu_n && b.ok; i++) {
        if (menu[i].label && !strcmp(menu[i].label, "---")) {
            continue;
        }
        const char *path[3] = {menu[i].path[0], menu[i].path[1], menu[i].path[2]};
        push(&b, menu[i].id, menu[i].label, path, menu[i].depth);
    }
    pc_ui_menu_items_free(menu, menu_n);

    /* 2. Shortcut-bearing commands with no menu item: Tools (tools.* and the colour/fill keys) and
     * the Layer stamp commands sit under their groups; the rest keep their registry menu path. */
    size_t cmd_n = 0;
    const pc_engine_command *cmds = pc_engine_commands(&cmd_n);
    size_t layer_mark = b.n; /* append Layer-group extras, then Tools extras, after the main list */
    for (size_t i = 0; i < cmd_n && b.ok; i++) {
        const pc_engine_command *c = &cmds[i];
        if (!c->id || !c->shortcut || contains(&b, c->id)) {
            continue;
        }
        if (c->menu_len == 0 && is_tools_key(c->id)) {
            continue; /* handled in the Tools pass below, after Layer */
        }
        if (c->menu_len == 0 && !strncmp(c->id, "layer.", 6)) {
            continue; /* handled in the Layer pass below */
        }
        const char *path[3] = {NULL, NULL, NULL};
        size_t depth = c->menu_len > 3 ? 3 : c->menu_len;
        for (size_t j = 0; j < depth; j++) {
            path[j] = c->menu[j];
        }
        push(&b, c->id, c->label, path, depth);
    }
    (void)layer_mark;
    /* Layer stamp commands (no menu item in Photoshop), then the colour/fill keys under Tools. */
    for (size_t i = 0; i < cmd_n && b.ok; i++) {
        const pc_engine_command *c = &cmds[i];
        if (c->id && c->shortcut && c->menu_len == 0 && !strncmp(c->id, "layer.", 6) && !contains(&b, c->id)) {
            const char *path[3] = {"Layer", NULL, NULL};
            push(&b, c->id, c->label, path, 1);
        }
    }
    for (size_t i = 0; i < cmd_n && b.ok; i++) {
        const pc_engine_command *c = &cmds[i];
        if (c->id && c->shortcut && c->menu_len == 0 && is_tools_key(c->id) && !contains(&b, c->id)) {
            const char *path[3] = {"Tools", NULL, NULL};
            push(&b, c->id, c->label, path, 1);
        }
    }

    /* 3. The toolbar tools (their single-key selects). */
    for (size_t i = 0; i < PC_TOOL_N && b.ok; i++) {
        pc_ui_tool tool = PC_UI_TOOL_ALL[i];
        char id[96];
        if (pc_ui_tool_select_id(tool, id, sizeof id) >= sizeof id) {
            continue;
        }
        const char *path[3] = {"Tools", NULL, NULL};
        push(&b, id, pc_ui_tool_label(tool), path, 1);
    }

    /* 4. The held temporary tools (Hand / Zoom / Erase). */
    size_t temp_n = 0;
    const pc_engine_temporary_tool *temp = pc_engine_temporary_tools(&temp_n);
    for (size_t i = 0; i < temp_n && b.ok; i++) {
        const char *path[3] = {"Tools", "Temporary", NULL};
        push(&b, temp[i].id, temp[i].label, path, 2);
    }

    if (!b.ok) {
        pc_ui_shortcut_items_free(b.items, b.n);
        return pc_fail(err, PC_ERR_NOMEM, "shortcut items");
    }
    *out = b.items;
    *n = b.n;
    return PC_OK;
}

void pc_ui_shortcut_items_free(pc_ui_shortcut_item *items, size_t n)
{
    if (!items) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        free(items[i].id);
        free(items[i].label);
        for (size_t j = 0; j < 3; j++) {
            free(items[i].path[j]);
        }
        free(items[i].def);
    }
    free(items);
}

/* The punctuation key tokens the editor records, so "Cmd+=" round-trips (prefs_ui.rs shortcut_text). */
static const char *key_token(pc_ui_key key)
{
    switch (key) {
    case PC_KEY_EQUALS:
        return "=";
    case PC_KEY_MINUS:
        return "-";
    case PC_KEY_OPENBRACKET:
        return "[";
    case PC_KEY_CLOSEBRACKET:
        return "]";
    case PC_KEY_SEMICOLON:
        return ";";
    case PC_KEY_QUOTE:
        return "'";
    case PC_KEY_PLUS:
        return ""; /* a trailing '+' names the Plus key */
    default:
        return pc_ui_key_name(key);
    }
}

bool pc_ui_shortcut_capture(pc_ui_key key, unsigned mods, char *out, size_t cap)
{
    if (cap) {
        out[0] = '\0';
    }
    if (key == PC_KEY_NONE) {
        return false;
    }
    const char *name = key_token(key);
    if (!name) {
        return false;
    }
    char raw[160];
    size_t at = 0;
    const char *parts[4];
    size_t np = 0;
    if (mods & PC_UI_MOD_COMMAND) {
        parts[np++] = "Cmd";
    }
    if ((mods & PC_UI_MOD_CTRL) && !(mods & PC_UI_MOD_COMMAND)) {
        parts[np++] = "Ctrl";
    }
    if (mods & PC_UI_MOD_ALT) {
        parts[np++] = "Alt";
    }
    if (mods & PC_UI_MOD_SHIFT) {
        parts[np++] = "Shift";
    }
    for (size_t i = 0; i < np && at < sizeof raw; i++) {
        int w = snprintf(raw + at, sizeof raw - at, "%s+", parts[i]);
        if (w < 0 || (size_t)w >= sizeof raw - at) {
            return false;
        }
        at += (size_t)w;
    }
    int w = snprintf(raw + at, sizeof raw - at, "%s", name);
    if (w < 0 || (size_t)w >= sizeof raw - at) {
        return false;
    }
    return pc_pref_normalize_shortcut(raw, strlen(raw), out, cap);
}
