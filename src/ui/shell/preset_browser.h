/* U5 preset-browser rendering (port of crates/ui-egui/src/preset_panels.rs's `browser`, the floating
 * `windows`, and the Tool Presets / Clone Source bodies). The serde-visible PresetUi state model and
 * folder-reorder logic live in src/ui/preset_ui.{c,h}; this draws against it with Nuklear.
 *
 * The shared browser renders a thumbnail grid (or a list when the size slider is at its minimum),
 * folder disclosure rows, an inline rename field, a right-click context menu and the footer
 * (size slider · New Group · New · Delete). Clicking selects a preset, double-clicking or dragging
 * onto the canvas applies it, and dragging a folder header reorders the folders. Generic library
 * edits (rename, delete, move, new, and the group commands) are dispatched through the engine inside the
 * browser; the panel-specific Select/Activate/Drop come back to the caller to apply via the engine. */
#ifndef PC_UI_SHELL_PRESET_BROWSER_H
#define PC_UI_SHELL_PRESET_BROWSER_H

#include "panels.h"
#include "photocraft_nuklear.h"

/* One preset in a folder. Both strings are borrowed; they must outlive the browser call (they come
 * from the caller's list-result JSON doc, which stays alive across the draw and dispatch). */
typedef struct pc_ui_preset_item {
    const char *key;  /* command key: preset name or pattern id */
    const char *name; /* label shown */
} pc_ui_preset_item;

/* One folder: a name and its items (both borrowed). */
typedef struct pc_ui_preset_group {
    const char *name;
    const pc_ui_preset_item *items;
    size_t n;
} pc_ui_preset_group;

/* Paints item (gi,ii) into `r`. `list` is true in list mode (a small 20 px swatch). */
typedef void (*pc_ui_preset_thumb_fn)(pc_ui_shell_app *app, void *user, struct nk_rect r, size_t gi,
                                      size_t ii, bool list);

typedef enum pc_ui_preset_ev_kind {
    PC_UI_PRESET_EV_NONE = 0,
    PC_UI_PRESET_EV_SELECT,
    PC_UI_PRESET_EV_ACTIVATE,
    PC_UI_PRESET_EV_DROP,
} pc_ui_preset_ev_kind;

typedef struct pc_ui_preset_event {
    pc_ui_preset_ev_kind kind;
    char key[256];
    float drop[2]; /* document point, valid only for PC_UI_PRESET_EV_DROP */
} pc_ui_preset_event;

typedef struct pc_ui_preset_browser {
    const char *panel;   /* e.g. "gradients" (PresetUi panel key) */
    const char *prefix;  /* e.g. "gradient.presets" (command prefix for generic edits) */
    const char *new_tip; /* tooltip on the New button */
    const char *new_name; /* name for New ({"name":new_name}); NULL => {} */
    bool rename_after_new; /* open a rename on the created preset (false for patterns) */
    bool swatches_all;     /* every folder starts open (Swatches), not just the first */
    const pc_ui_preset_group *groups;
    size_t ngroups;
    pc_ui_preset_thumb_fn thumb;
    void *user;
} pc_ui_preset_browser;

/* Group/item views built from a `*.presets.list` result. All strings are borrowed from the result
 * doc `root`, which must outlive the browser call. */
typedef struct pc_ui_preset_group_data {
    pc_ui_preset_group *groups;
    size_t ngroups;
    pc_ui_preset_item *items; /* flat backing store for every group's items */
} pc_ui_preset_group_data;

/* Build group views from `root->groups`, each group's items under `items_key`, each item's command
 * key from `key_field` (and its label from "name"). Returns false (and leaves *out zeroed) on OOM or
 * a missing/!array groups field; always safe to fini. */
bool pc_ui_preset_groups_from_json(yyjson_mut_val *root, const char *items_key, const char *key_field,
                                   pc_ui_preset_group_data *out);
void pc_ui_preset_group_data_fini(pc_ui_preset_group_data *d);

/* Draw the browser body + footer. Returns true and fills *ev when the user selected/activated/
 * dropped a preset (the caller applies it through the engine). Never crashes on empty libraries,
 * huge counts or missing thumbnails. */
bool pc_ui_shell_preset_browser(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime,
                                const pc_ui_preset_browser *info, pc_ui_preset_event *ev);

/* Draw the floating preset windows (Window > Styles / Shapes / Tool Presets / Clone Source), each a
 * movable Nuklear window keyed off the PresetUi float flags. Called after the main window. */
void pc_ui_shell_preset_windows_draw(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime, float w, float h);

#endif /* PC_UI_SHELL_PRESET_BROWSER_H */
