/* Right-dock layout (#88): panel groups with fixed heights, like Photoshop's dock columns
 * (reimplementation of crates/ui-egui/src/dock.rs's data model against the C UI stack; the
 * egui-specific drag/drop rendering is rebuilt in the shell's frame loop).
 *
 * A group's height never follows its content: content taller than the group scrolls inside it.
 * The last expanded group (Layers by default) fills what the others leave. The layout is carried
 * in the U1 UiState (`dock`), serialisable and drivable with `ui.set`, saved by Window > Workspace
 * > New Workspace, reset by Reset Workspace, and remembered across launches in the preferences.
 *
 * This module is pure data + layout maths (no SDL/GL/Nuklear), so it links into the library and is
 * unit-testable headlessly. */
#ifndef PC_UI_DOCK_H
#define PC_UI_DOCK_H

#include <stdbool.h>
#include <stddef.h>

#include "base/json.h"
#include "base/status.h"

/* A dock panel group (one tab strip). Declaration order is the Photoshop Essentials order, top to
 * bottom, and also the serde / BTreeMap ordering used when (de)serialising. */
typedef enum pc_ui_group {
    PC_UI_GROUP_COLOR = 0,  /* Color | Swatches | Gradients | Patterns */
    PC_UI_GROUP_PROPERTIES, /* Properties | Adjustments */
    PC_UI_GROUP_CHARACTER,  /* Character | Paragraph (Window > Character opens it) */
    PC_UI_GROUP_NAVIGATOR,  /* Navigator | Histogram | Info */
    PC_UI_GROUP_HISTORY,    /* History | Actions | Layer Comps */
    PC_UI_GROUP_LAYERS,     /* Layers | Channels | Paths */
    PC_UI_GROUP_N
} pc_ui_group;

/* Essentials order, top to bottom (== enum order). */
extern const pc_ui_group PC_UI_GROUP_ALL[PC_UI_GROUP_N];

/* The `panels` / `dockTabs` key ("color", "properties", ...). */
const char *pc_ui_group_key(pc_ui_group g);
/* The group for a key, or false if unknown. */
bool pc_ui_group_from_key(const char *key, pc_ui_group *out);

float pc_ui_group_default_height(pc_ui_group g);
float pc_ui_group_compact_height(pc_ui_group g);
float pc_ui_group_preferred_fill(pc_ui_group g);
float pc_ui_group_min_height(pc_ui_group g);

/* The tab names for a group (English keys), 2..4 per group. `*n_out` receives the count. The
 * Color group's first two tabs swap with the theme (Pro: Color first; Studio: Swatches first). */
const char *const *pc_ui_group_tabs(pc_ui_group g, bool pro, size_t *n_out);
/* Tabs that lay out their own scrolling body (Layers, History, Layer Comps, Navigator preview). */
bool pc_ui_group_scrolls_itself(pc_ui_group g, size_t tab);

/* Gap between groups; also the splitter grab area. */
#define PC_UI_DOCK_GAP 6.0f
/* Upper bound on a stored height (guards absurd values from ui.set). */
#define PC_UI_DOCK_MAX_HEIGHT 4000.0f
/* Most tabs any group has. */
#define PC_UI_DOCK_MAX_TABS 4
/* order[] may hold a few duplicates from a bad saved layout; order() dedups. */
#define PC_UI_DOCK_MAX_ORDER 16
/* Tab names kept per group in tab_order (take 16 in the reference; groups have <= 4 tabs). */
#define PC_UI_DOCK_MAX_ORDER_NAMES 16
/* Every panel could end up re-parented into a single group (cross-group moves): the whole panel
 * universe is 17 (see dock.c). Member lists and label output arrays are sized to hold them all. */
#define PC_UI_DOCK_PANEL_COUNT 17
#define PC_UI_DOCK_MAX_MEMBERS 17

/* An owned list of tab names (hidden or ordered), by the English tab name. */
typedef struct pc_ui_name_list {
    char **names; /* owned */
    size_t n;
} pc_ui_name_list;

/* Smallest a floating dock window may be (logical points). */
#define PC_UI_DOCK_FLOAT_MIN_W 180.0f
#define PC_UI_DOCK_FLOAT_MIN_H 70.0f
/* Default floating window size. */
#define PC_UI_DOCK_FLOAT_DEFAULT_W 300.0f

/* Order, heights, collapsed state, float state and tab visibility/order of the dock groups. */
typedef struct pc_ui_dock_layout {
    pc_ui_group order[PC_UI_DOCK_MAX_ORDER]; /* top-to-bottom order as stored (may be partial/dup) */
    size_t n_order;
    bool has_height[PC_UI_GROUP_N];
    float heights[PC_UI_GROUP_N];
    pc_ui_group collapsed[PC_UI_GROUP_N]; /* groups collapsed to their tab strip, insertion order */
    size_t n_collapsed;
    /* Groups detached from the right column into their own floating window, insertion order. The
     * reference floats individual panels through the external craft_ui tree (not vendored, §7); with
     * fixed group membership a group floats as a unit. Orthogonal to order/collapse/heights. */
    pc_ui_group floating[PC_UI_GROUP_N];
    size_t n_floating;
    bool has_float_rect[PC_UI_GROUP_N]; /* a user-placed window rect (else a cascade default) */
    float float_rects[PC_UI_GROUP_N][4]; /* [x, y, w, h] in logical points */
    pc_ui_name_list hidden_tabs[PC_UI_GROUP_N]; /* tabs hidden by Close, by name */
    pc_ui_name_list tab_order[PC_UI_GROUP_N];   /* left-to-right tab order, by name */
    /* Mutable panel -> group assignment (cross-group re-parent). While `customized` is false the
     * groups use the fixed native tab arrays (byte-exact legacy behaviour); the first cross-group
     * move materialises explicit per-group label lists in `members` and sets `customized`. A panel
     * is identified by its English label (same key space as tab_order / hidden_tabs). This mirrors
     * the reference's flexible craft_ui docking tree, mapped onto the six fixed C group slots. */
    pc_ui_name_list members[PC_UI_GROUP_N];
    bool customized;
} pc_ui_dock_layout;

void pc_ui_dock_layout_init(pc_ui_dock_layout *l); /* default (empty) */
void pc_ui_dock_layout_fini(pc_ui_dock_layout *l); /* free owned names; NULL-safe */
void pc_ui_dock_layout_copy(pc_ui_dock_layout *dst, const pc_ui_dock_layout *src); /* deep copy */
bool pc_ui_dock_layout_eq(const pc_ui_dock_layout *a, const pc_ui_dock_layout *b);

/* Every group once, in display order (always PC_UI_GROUP_N entries): stored order first (deduped),
 * then any missing group slotted in just below its default predecessor. */
void pc_ui_dock_order(const pc_ui_dock_layout *l, pc_ui_group out[PC_UI_GROUP_N]);

bool pc_ui_dock_is_collapsed(const pc_ui_dock_layout *l, pc_ui_group g);
void pc_ui_dock_set_collapsed(pc_ui_dock_layout *l, pc_ui_group g, bool on);

/* Floating dock windows. `float` detaches `g` into a window (`rect` NULL keeps/defaults its place),
 * `unfloat` re-docks it, `is_floating` queries. `float_rect` writes the sanitised [x,y,w,h] into
 * `out` and returns true when `g` floats with a user-placed rect (false → the renderer cascades a
 * default). `set_float_rect` records a (sanitised) window placement. All NULL/odd-value safe. */
bool pc_ui_dock_is_floating(const pc_ui_dock_layout *l, pc_ui_group g);
void pc_ui_dock_float(pc_ui_dock_layout *l, pc_ui_group g, const float rect[4]);
void pc_ui_dock_unfloat(pc_ui_dock_layout *l, pc_ui_group g);
bool pc_ui_dock_float_rect(const pc_ui_dock_layout *l, pc_ui_group g, float out[4]);
void pc_ui_dock_set_float_rect(pc_ui_dock_layout *l, pc_ui_group g, const float rect[4]);

/* The stored (or default) height, sanitised to [min_height, MAX_HEIGHT]. */
float pc_ui_dock_height(const pc_ui_dock_layout *l, pc_ui_group g);

/* Move `g` so it is drawn just before `before` (NULL = last). */
void pc_ui_dock_move_group(pc_ui_dock_layout *l, pc_ui_group g, const pc_ui_group *before);

/* Every tab of `group` in the user's order, by original index. Fills `out`, returns the count. */
size_t pc_ui_dock_ordered_tabs(const pc_ui_dock_layout *l, pc_ui_group g, bool pro,
                               size_t out[PC_UI_DOCK_MAX_TABS]);
/* Visible tabs (hidden ones removed) in display order, by original index. */
size_t pc_ui_dock_visible_tabs(const pc_ui_dock_layout *l, pc_ui_group g, bool pro,
                               size_t out[PC_UI_DOCK_MAX_TABS]);
/* Move tab `tab` so it is drawn just before tab `*before` (NULL = last). */
void pc_ui_dock_move_tab(pc_ui_dock_layout *l, pc_ui_group g, size_t tab, const size_t *before, bool pro);
void pc_ui_dock_hide_tab(pc_ui_dock_layout *l, pc_ui_group g, size_t tab, bool pro);
void pc_ui_dock_show_tab(pc_ui_dock_layout *l, pc_ui_group g, size_t tab, bool pro);

/* ---- mutable panel -> group membership (cross-group re-parent) ---- */

/* The native (default) group a panel label belongs to. False for an unknown label. */
bool pc_ui_dock_label_native_group(const char *label, pc_ui_group *out);
/* Panels that lay out their own scrolling body (Layers, History, Actions, Navigator), by label. */
bool pc_ui_dock_label_scrolls_itself(const char *label);
/* Effective ordered labels of group `g`: `members[g]` when customized, else the native theme order.
 * Writes label pointers (static or owned-by-the-layout strings, valid until the layout changes) into
 * `out` and returns the count (<= PC_UI_DOCK_MAX_MEMBERS). */
size_t pc_ui_dock_group_labels(const pc_ui_dock_layout *l, pc_ui_group g, bool pro,
                               const char *out[PC_UI_DOCK_MAX_MEMBERS]);
/* As above, with hidden tabs removed. */
size_t pc_ui_dock_visible_labels(const pc_ui_dock_layout *l, pc_ui_group g, bool pro,
                                 const char *out[PC_UI_DOCK_MAX_MEMBERS]);
/* The group currently holding `label`, or false if it is in no group. */
bool pc_ui_dock_label_group(const pc_ui_dock_layout *l, const char *label, bool pro, pc_ui_group *out);
/* Re-parent panel `panel` (a label) into group `target`, drawn just before `before` (NULL = end;
 * a label not in `target` also means end). Materialises the membership on first use and sets
 * `customized`. No-op for an unknown panel/target. NULL/odd-value safe, never crashes. */
void pc_ui_dock_move_panel(pc_ui_dock_layout *l, const char *panel, pc_ui_group target,
                           const char *before, bool pro);

/* Lay out `shown` (display order, `n` groups) in a column `avail` tall with `strip`-high tab
 * strips. Fills `out[i]` with each group's height; the last expanded group fills the rest. */
void pc_ui_dock_heights_for(const pc_ui_dock_layout *l, const pc_ui_group *shown, size_t n, float avail,
                            float strip, float *out);

/* Splitter `i` (below group `i` in `shown`/`heights`) dragged by `dy`. `heights` are the laid-out
 * heights for the same `shown`/`avail` (from pc_ui_dock_heights_for). */
void pc_ui_dock_resize(pc_ui_dock_layout *l, const pc_ui_group *shown, const float *heights, size_t n,
                       size_t i, float dy);

/* The group a dragged group lands before when released at `y`; returns true and writes *out, or
 * false for "last". `order`/`rect_top`/`rect_bottom` describe the laid-out groups. */
bool pc_ui_dock_drop_before(const pc_ui_group *order, size_t n_order, const pc_ui_group *rect_group,
                            const float *rect_top, const float *rect_bottom, size_t n_rects,
                            pc_ui_group dragged, float y, pc_ui_group *out);

/* Serde (field names byte-exact with crates/ui-egui/src/dock.rs). */
pc_status pc_ui_dock_layout_to_json(const pc_ui_dock_layout *l, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                    pc_error *err);
/* Never fails: unreadable parts keep their defaults (the reference is lenient on tabOrder and the
 * whole field falls back to default when any part is wrong). */
/* Transactional equivalent; reports allocation/size failure without losing the old layout. */
pc_status pc_ui_dock_layout_parse(pc_ui_dock_layout *out, yyjson_val *object, pc_error *err);
void pc_ui_dock_layout_from_json(pc_ui_dock_layout *l, yyjson_val *obj);

#endif /* PC_UI_DOCK_H */
