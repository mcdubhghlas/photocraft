/* Shared Properties-panel layout helpers (#155): the layer-kind icon, field-column widths and the
 * Quick Actions each layer kind offers. Port of the pure parts of crates/ui-egui/src/props_layout.rs
 * (the egui header/section/quick-actions rendering and kind_label's adjustment/artboard specifics
 * land with the Properties panel body in U5). Pure logic, unit-testable headlessly. */
#ifndef PC_UI_PROPS_LAYOUT_H
#define PC_UI_PROPS_LAYOUT_H

#include <stddef.h>

#include "doc/doc.h"

/* Height of a section header row. */
#define PC_UI_PROPS_SECTION_H 24.0f
/* Width of the short label/glyph column in front of a field. */
#define PC_UI_PROPS_LABEL_W 22.0f
/* Horizontal gap between a label and its field. */
#define PC_UI_PROPS_LABEL_GAP 4.0f
/* Horizontal gap between two label+field columns. */
#define PC_UI_PROPS_COL_GAP 8.0f

/* The icon id for a layer kind (same glyphs as the Layers panel's kind filter). */
const char *pc_ui_props_kind_icon(pc_layer_kind kind);

/* Width of each field when `cols` label+field pairs (labels `label_w` wide) share the row. */
float pc_ui_props_field_width(float avail, size_t cols, float label_w);

/* A Quick Action: a user-facing label and the command id it runs. */
typedef struct pc_ui_quick_action {
    const char *label;
    const char *id;
} pc_ui_quick_action;

/* The Quick Actions a layer kind offers, as a static table. Returns the array and its count. */
const pc_ui_quick_action *pc_ui_props_quick_actions(pc_layer_kind kind, size_t *n_out);

#endif /* PC_UI_PROPS_LAYOUT_H */
