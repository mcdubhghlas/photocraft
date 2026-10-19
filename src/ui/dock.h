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

/* An owned list of tab names (hidden or ordered), by the English tab name. */
typedef struct pc_ui_name_list {
    char **names; /* owned */
    size_t n;
} pc_ui_name_list;

/* Order, heights, collapsed state and tab visibility/order of the dock groups. */
typedef struct pc_ui_dock_layout {
    pc_ui_group order[PC_UI_DOCK_MAX_ORDER]; /* top-to-bottom order as stored (may be partial/dup) */
    size_t n_order;
    bool has_height[PC_UI_GROUP_N];
    float heights[PC_UI_GROUP_N];
    pc_ui_group collapsed[PC_UI_GROUP_N]; /* groups collapsed to their tab strip, insertion order */
    size_t n_collapsed;
    pc_ui_name_list hidden_tabs[PC_UI_GROUP_N]; /* tabs hidden by Close, by name */
    pc_ui_name_list tab_order[PC_UI_GROUP_N];   /* left-to-right tab order, by name */
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
