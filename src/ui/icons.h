/* Tool -> icon-name mapping (port of the tool_icon table from crates/ui-egui/src/icons.rs). The
 * embedded SVG set (icon_data.rs) and the egui/Nuklear rasterising/tinting are reimplemented by the
 * chunks that draw the toolbar; U1 owns the stable icon ids every tool maps to. No GUI dependency. */
#ifndef PC_UI_ICONS_H
#define PC_UI_ICONS_H

#include "ui/state.h"
#include "compose/compose.h"

/* The icon id for a tool (e.g. "brush", "gradient"); never NULL or empty. */
const char *pc_ui_tool_icon(pc_ui_tool t);

size_t pc_ui_icon_count(void);
const char *pc_ui_icon_name(size_t index);
bool pc_ui_icon_exists(const char *name);
const uint8_t *pc_ui_icon_svg(const char *name, size_t *length); /* unknown: square fallback */
/* White straight RGBA icon at the requested UI pixel size, tinted by the drawing widget. */
pc_status pc_ui_icon_rasterize(const char *name, uint16_t side, pc_rgba8_image *out, pc_error *err);
#endif
