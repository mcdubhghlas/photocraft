/* Photoshop menu parity report (port of crates/ui-egui/src/parity.rs). The live/total counts are
 * computed from the menu catalog (U2) and the live command registry; U1 owns the FLOOR (which must
 * never be lowered), the report data structures and the Markdown rendering that
 * tools/parity.sh (Z1) writes to docs/parity-checklist.md. No GUI dependency. */
#ifndef PC_UI_PARITY_H
#define PC_UI_PARITY_H

#include <stddef.h>

#include "base/buf.h"
#include "base/status.h"

/* Minimum number of live catalog items. Raise it when parity grows; never lower it. */
#define PC_UI_PARITY_FLOOR 631

/* One top-level menu's coverage. `missing` holds live-count..total labels (with submenu path). */
typedef struct pc_ui_menu_parity {
    char *menu;
    size_t live;
    size_t total;
    char **missing;
    size_t n_missing;
} pc_ui_menu_parity;

typedef struct pc_ui_parity {
    size_t live;
    size_t total;
    pc_ui_menu_parity *menus;
    size_t n_menus;
} pc_ui_parity;

/* 100 * live / total, or 0 when total is 0. */
double pc_ui_parity_percent(const pc_ui_parity *p);

/* Compute coverage of the Photoshop menu catalog (ui/menu_catalog.h) against the live command
 * registry (ui/menus.h is_live). Separators are excluded and a duplicate id is counted once (its
 * first occurrence). Builds an owned report into *out (cleared first); free with pc_ui_parity_free.
 * Mirrors parity::compute. */
pc_status pc_ui_parity_compute(pc_ui_parity *out, pc_error *err);
/* Release a report built by pc_ui_parity_compute. NULL-safe; leaves *p zeroed. */
void pc_ui_parity_free(pc_ui_parity *p);

/* Render the report as Markdown (the body of docs/parity-checklist.md) into `out` (cleared first). */
pc_status pc_ui_parity_to_markdown(const pc_ui_parity *p, pc_buf *out, pc_error *err);

#endif
