/* Keyboard shortcut binding table (port of shortcut_dispatch::bindings, default path). */
#include "ui/shortcut_dispatch.h"

#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "ui/menu_catalog.h"
#include "ui/menus.h"

const struct pc_ui_secondary PC_UI_SECONDARY[1] = {{"edit.fill", "Shift+Backspace"}};

/* The modifier count the final ordering uses: Shift + Alt + Command (Ctrl excluded, as in Rust). */
static int mod_count(const pc_ui_shortcut *sc)
{
    int n = 0;
    if (sc->mods & PC_UI_MOD_SHIFT) {
        n++;
    }
    if (sc->mods & PC_UI_MOD_ALT) {
        n++;
    }
    if (sc->mods & PC_UI_MOD_COMMAND) {
        n++;
    }
    return n;
}

static bool chord_eq(const pc_ui_shortcut *a, const pc_ui_shortcut *b)
{
    return a->mods == b->mods && a->key == b->key;
}

/* Is id already covered by the UI/engine passes ("own" set): a UI command, or an engine command
 * that declares its own default shortcut? (Override presence is not modelled; see the header.) */
static bool is_own(const char *id)
{
    if (pc_ui_command_find(id) != NULL) {
        return true;
    }
    const pc_engine_command *c = pc_engine_command_find(id);
    return c != NULL && c->shortcut != NULL;
}

/* Append (id, chord parsed from its effective shortcut) unless the chord is already taken. */
static pc_status push_effective(const pc_engine_session *s, const char *id, pc_ui_binding **arr,
                                size_t *n, size_t *cap, pc_error *err)
{
    char buf[128];
    if (!pc_ui_effective_shortcut(s, id, buf, sizeof buf)) {
        return PC_OK;
    }
    pc_ui_shortcut sc;
    if (!pc_ui_shortcut_parse(buf, &sc)) {
        return PC_OK;
    }
    for (size_t i = 0; i < *n; i++) {
        if (chord_eq(&(*arr)[i].sc, &sc)) {
            return PC_OK;
        }
    }
    *arr = pc_grow_array(*arr, cap, *n + 1, sizeof **arr);
    if (*arr == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "pc_ui_bindings: out of memory");
    }
    (*arr)[*n].id = id;
    (*arr)[*n].sc = sc;
    (*n)++;
    return PC_OK;
}

static pc_status push_literal(const char *id, const char *scstr, pc_ui_binding **arr, size_t *n,
                              size_t *cap, pc_error *err)
{
    pc_ui_shortcut sc;
    if (!pc_ui_shortcut_parse(scstr, &sc)) {
        return PC_OK;
    }
    for (size_t i = 0; i < *n; i++) {
        if (chord_eq(&(*arr)[i].sc, &sc)) {
            return PC_OK;
        }
    }
    *arr = pc_grow_array(*arr, cap, *n + 1, sizeof **arr);
    if (*arr == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "pc_ui_bindings: out of memory");
    }
    (*arr)[*n].id = id;
    (*arr)[*n].sc = sc;
    (*n)++;
    return PC_OK;
}

pc_status pc_ui_bindings(const pc_engine_session *s, pc_ui_binding **out, size_t *n, pc_error *err)
{
    if (out == NULL || n == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "pc_ui_bindings: NULL out");
    }
    *out = NULL;
    *n = 0;
    pc_ui_binding *arr = NULL;
    size_t cnt = 0, cap = 0;
    pc_status st = PC_OK;

    /* ui: shell commands. */
    for (size_t i = 0; i < PC_UI_COMMANDS_N && st == PC_OK; i++) {
        st = push_effective(s, PC_UI_COMMANDS[i].id, &arr, &cnt, &cap, err);
    }
    /* engine: registry commands. */
    size_t neng = 0;
    const pc_engine_command *eng = pc_engine_commands(&neng);
    for (size_t i = 0; i < neng && st == PC_OK; i++) {
        st = push_effective(s, eng[i].id, &arr, &cnt, &cap, err);
    }
    /* catalogue: live items with a shortcut not already owned by a ui/engine command. */
    for (size_t i = 0; i < PC_UI_CATALOG_N && st == PC_OK; i++) {
        const pc_ui_catalog_item *c = &PC_UI_CATALOG[i];
        if (c->shortcut != NULL && !is_own(c->id) && pc_ui_is_live(c->id)) {
            st = push_effective(s, c->id, &arr, &cnt, &cap, err);
        }
    }
    /* secondary: Photoshop's second shortcuts. */
    for (size_t i = 0; i < 1 && st == PC_OK; i++) {
        st = push_literal(PC_UI_SECONDARY[i].id, PC_UI_SECONDARY[i].shortcut, &arr, &cnt, &cap, err);
    }
    if (st != PC_OK) {
        free(arr);
        return st;
    }

    /* Order most-modifiers-first (stable within a count): a 4-bucket stable pass (count 3..0). */
    pc_ui_binding *sorted = pc_calloc_array(cnt > 0 ? cnt : 1, sizeof *sorted);
    if (sorted == NULL && cnt > 0) {
        free(arr);
        return pc_fail(err, PC_ERR_NOMEM, "pc_ui_bindings: out of memory");
    }
    size_t w = 0;
    for (int bucket = 3; bucket >= 0; bucket--) {
        for (size_t i = 0; i < cnt; i++) {
            if (mod_count(&arr[i].sc) == bucket) {
                sorted[w++] = arr[i];
            }
        }
    }
    free(arr);
    *out = sorted;
    *n = cnt;
    return PC_OK;
}
