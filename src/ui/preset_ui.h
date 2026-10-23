/* U5 preset panels state (Window > Gradients, Patterns, Styles, Shapes, Tool Presets, Clone Source).
 *
 * Port of crates/ui-egui/src/preset_panels.rs's `PresetUi` state model: the serde-visible panel
 * state (floating-window flags, per-panel thumbnail sizes, folder open state, selection, folder
 * order) plus the pure folder-reorder logic (display_order / move_group, which mirror the dock's
 * per-group tab order). The egui browser rendering, drag-to-canvas and floating-window rendering are
 * the host's job; this module is pure data + logic (no SDL/GL/Nuklear), so it links into the library
 * and is unit-testable headlessly, and the control channel can read and drive it through UiState.
 *
 * Maps (BTreeMap in the reference) are kept as arrays sorted by key, so (de)serialisation reproduces
 * the reference's sorted-key order byte for byte. */
#ifndef PC_UI_PRESET_UI_H
#define PC_UI_PRESET_UI_H

#include <stdbool.h>
#include <stddef.h>

#include "base/json.h"
#include "base/status.h"

/* Cap on folder names stored per panel (reference MAX_GROUP_ORDER). */
#define PC_UI_PRESET_MAX_GROUP_ORDER 10000

/* panel -> thumbnail size (px). */
typedef struct pc_ui_preset_size {
    char *panel; /* owned */
    float size;
} pc_ui_preset_size;

/* panel -> selected preset name / pattern id. */
typedef struct pc_ui_preset_sel {
    char *panel; /* owned */
    char *name;  /* owned */
} pc_ui_preset_sel;

/* panel -> ordered folder names (library order == no entry). */
typedef struct pc_ui_preset_group_order {
    char *panel;  /* owned */
    char **names; /* owned */
    size_t n;
} pc_ui_preset_group_order;

typedef struct pc_ui_preset_ui {
    /* Floating panels. */
    bool styles, shapes, tool_presets, clone_source;
    /* Per-panel thumbnail size (sorted by panel). */
    pc_ui_preset_size *sizes;
    size_t n_sizes;
    /* Folders whose open state differs from the default (sorted set of names). */
    char **toggled;
    size_t n_toggled;
    /* Selected preset per panel (sorted by panel). */
    pc_ui_preset_sel *selected;
    size_t n_selected;
    /* Tool Presets: Current Tool Only. */
    bool current_tool_only;
    /* Custom Shape tool: the shape it draws ("" = the default). */
    char *custom_shape; /* owned, never NULL */
    /* Folder/group order per panel (sorted by panel; empty = not serialised). */
    pc_ui_preset_group_order *group_order;
    size_t n_group_order;
    /* In-place rename: (panel, preset or "group:<name>", text). Transient, not serialised. */
    bool has_renaming;
    char *rename_panel; /* owned */
    char *rename_key;   /* owned */
    char *rename_text;  /* owned */
} pc_ui_preset_ui;

void pc_ui_preset_ui_init(pc_ui_preset_ui *p); /* PresetUi::default() */
void pc_ui_preset_ui_fini(pc_ui_preset_ui *p); /* NULL-safe */
void pc_ui_preset_ui_copy(pc_ui_preset_ui *dst, const pc_ui_preset_ui *src); /* deep copy */
bool pc_ui_preset_ui_eq(const pc_ui_preset_ui *a, const pc_ui_preset_ui *b);

/* The custom shape name, or the default ("Heart") when none is chosen. Borrowed. */
const char *pc_ui_preset_ui_shape(const pc_ui_preset_ui *p);

/* Display order of `panel`'s folders as library indices into `names` (the stored order first, bad
 * names dropped, then the rest in library order). `out` holds >= n_names entries; returns n_names. */
size_t pc_ui_preset_ui_display_order(const pc_ui_preset_ui *p, const char *panel,
                                     const char *const *names, size_t n_names, size_t *out);
/* Move folder `from` so it is drawn before `before` (NULL = last). A no-op order is not stored (the
 * library's own order is "no entry", like the dock's default tab order). */
void pc_ui_preset_ui_move_group(pc_ui_preset_ui *p, const char *panel, const char *const *names,
                                size_t n_names, const char *from, const char *before);

/* ----------------------------------------------------- browser accessors (U5 rendering)
 * Small mutators/queries the Nuklear browser uses to read and drive the serde-visible state
 * without touching the sorted arrays directly. All NULL-safe; string inputs are copied. */

/* Default thumbnail edge when a panel has no stored size (matches the reference's 40 px). */
#define PC_UI_PRESET_DEFAULT_SIZE 40.0f
/* Below this edge the browser shows a list instead of a grid (reference MIN_THUMB). */
#define PC_UI_PRESET_MIN_THUMB 20.0f

/* Thumbnail edge for `panel` (PC_UI_PRESET_DEFAULT_SIZE when unset). */
float pc_ui_preset_ui_size(const pc_ui_preset_ui *p, const char *panel);
/* Set `panel`'s thumbnail edge (clamped to [MIN_THUMB, 96]). */
void pc_ui_preset_ui_set_size(pc_ui_preset_ui *p, const char *panel, float size);

/* Selected preset name/id for `panel`, or NULL. Borrowed. */
const char *pc_ui_preset_ui_selected(const pc_ui_preset_ui *p, const char *panel);
/* Set / clear `panel`'s selection. */
void pc_ui_preset_ui_set_selected(pc_ui_preset_ui *p, const char *panel, const char *name);
void pc_ui_preset_ui_clear_selected(pc_ui_preset_ui *p, const char *panel);

/* Whether folder `gi` named `name` in `panel` is open. Groups in `swatches_all` panels (and the
 * first folder of any panel) start open; `toggled` flips that default (reference group_open). */
bool pc_ui_preset_ui_group_open(const pc_ui_preset_ui *p, const char *panel, size_t gi,
                                const char *name, bool swatches_all);
/* Flip folder `name`'s open state for `panel`. */
void pc_ui_preset_ui_toggle_group(pc_ui_preset_ui *p, const char *panel, const char *name);

/* In-place rename field. set stores (panel,key,text); the getters read the active rename, or NULL
 * when none is active or it belongs to another panel. */
void pc_ui_preset_ui_set_rename(pc_ui_preset_ui *p, const char *panel, const char *key,
                                const char *text);
void pc_ui_preset_ui_clear_rename(pc_ui_preset_ui *p);
/* When a rename for `panel` is active, returns its key and writes the current text pointer; else
 * returns NULL. `text_out` may be NULL. */
const char *pc_ui_preset_ui_rename_key(const pc_ui_preset_ui *p, const char *panel,
                                       const char **text_out);
/* Replace the active rename's live text (keeps the panel/key). */
void pc_ui_preset_ui_set_rename_text(pc_ui_preset_ui *p, const char *text);

/* The six Window-menu panel ids this state covers. */
bool pc_ui_preset_ui_handles(const char *id);
/* The Color card tab a Window > Gradients/Patterns id selects (gradients=2, patterns=3). */
bool pc_ui_preset_ui_color_tab(const char *id, size_t *tab);
/* The floating-window flag for a Window > Styles/Shapes/Tool Presets/Clone Source id, or NULL. */
bool *pc_ui_preset_ui_float_flag(pc_ui_preset_ui *p, const char *id);

/* Serde (field names byte-exact with preset_panels.rs PresetUi; maps in sorted-key order). */
pc_status pc_ui_preset_ui_to_json(const pc_ui_preset_ui *p, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                  pc_error *err);
/* Lenient: an older state without a field keeps the default; garbage in one field never fails. */
void pc_ui_preset_ui_from_json(pc_ui_preset_ui *p, yyjson_val *obj);

#endif /* PC_UI_PRESET_UI_H */
