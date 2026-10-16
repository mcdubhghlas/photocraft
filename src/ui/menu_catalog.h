/* Photoshop menu structure (standard public command names), used to present the full, familiar
 * menu tree, in order, with separators ("---"). Port of crates/ui-egui/src/menu_catalog.rs.
 *
 * This is the AUTHORITATIVE list of every Photoshop menu item and the engine/UI command id each
 * maps to. The engine command registry uses these exact ids/labels/menu paths; command_lookup and
 * the menu catalogue depend on them, so the strings here are byte-for-byte the reference's. Items
 * whose id matches a live engine or UI command render enabled; the rest render disabled until
 * implemented. The parity report (ui/parity.h) measures coverage against this table. No GUI
 * dependency: pure data at layer L6. */
#ifndef PC_UI_MENU_CATALOG_H
#define PC_UI_MENU_CATALOG_H

#include <stddef.h>

/* One catalogue row: (menu path, label or "---", default shortcut, command id).
 * `path` holds `depth` components (top-level menu first); unused slots are NULL. A row with
 * id == "---" (and label "---") is a separator. `shortcut` is the egui-style default
 * ("Cmd+Shift+N"), NULL when the item has none; the UI maps Cmd per platform. All strings are
 * static-lifetime. The maximum menu depth in Photoshop's tree is three (e.g. Layer › Smart
 * Objects › Stack Mode). */
typedef struct pc_ui_catalog_item {
    const char *path[3];
    size_t depth;
    const char *label;
    const char *shortcut;
    const char *id;
} pc_ui_catalog_item;

/* The full catalogue in Photoshop order, and its length. */
extern const pc_ui_catalog_item PC_UI_CATALOG[];
extern const size_t PC_UI_CATALOG_N;

/* True when `id` names a separator row ("---"). */
static inline int pc_ui_catalog_is_separator(const char *id)
{
    return id != NULL && id[0] == '-' && id[1] == '-' && id[2] == '-' && id[3] == '\0';
}

#endif /* PC_UI_MENU_CATALOG_H */
