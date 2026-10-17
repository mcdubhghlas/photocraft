/* Dock tab strips that fit any width (#151): tabs shrink (widest first), elide, then overflow into
 * a chevron menu; the selected tab always stays on the strip. Reimplementation of the shared
 * craft_ui::tabs::fit policy (external craft-libs, not vendored) from its documented behaviour and
 * the crates/ui-egui/src/tab_strip.rs tests; see §7. Pure logic, no SDL/GL. */
#ifndef PC_UI_TAB_STRIP_H
#define PC_UI_TAB_STRIP_H

#include <stdbool.h>
#include <stddef.h>

/* Width of the >> overflow button. */
#define PC_UI_TAB_CHEVRON_W 18.0f
/* Most tabs a strip lays out at once. */
#define PC_UI_TAB_MAX 16

/* The result of fitting tabs to a strip. */
typedef struct pc_ui_tab_fit {
    struct {
        size_t index;
        float width;
    } shown[PC_UI_TAB_MAX];
    size_t n_shown;
    size_t overflow[PC_UI_TAB_MAX]; /* indices moved into the chevron menu, in order */
    size_t n_overflow;
    float overflow_width; /* chevron button width when n_overflow > 0, else 0 */
} pc_ui_tab_fit;

/* Fit `n` tabs of natural pixel widths `natural` into a strip `avail` wide, keeping tab `selected`
 * on the strip, each shown tab at least `min_w` wide, reserving `chevron_w` for the overflow menu
 * when some tabs don't fit. Never produces a negative width and always accounts for every tab
 * (shown + overflow == n). Hostile inputs (NaN/inf/negative) are sanitised. */
pc_ui_tab_fit pc_ui_tab_fit_compute(const float *natural, size_t n, size_t selected, float avail,
                                    float min_w, float chevron_w);

/* Where a tab dragged to (px,py) lands: before the slot-th tab on the strip (n_tabs == after the
 * last). `tab_cx` are the shown tabs' centre x; the strip area is [x0,y0]-[x1,y1]. Returns true and
 * writes *slot while the pointer is over the strip (its area expanded by 24 px on each side), false
 * once it has left. */
bool pc_ui_tab_drop_slot(const float *tab_cx, size_t n_tabs, float x0, float y0, float x1, float y1,
                         float px, float py, size_t *slot);

#endif /* PC_UI_TAB_STRIP_H */
