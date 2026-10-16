/* Tool -> icon-name mapping (port of the tool_icon table from crates/ui-egui/src/icons.rs). The
 * embedded SVG set (icon_data.rs) and the egui/Nuklear rasterising/tinting are reimplemented by the
 * chunks that draw the toolbar; U1 owns the stable icon ids every tool maps to. No GUI dependency. */
#ifndef PC_UI_ICONS_H
#define PC_UI_ICONS_H

#include "ui/state.h"

/* The icon id for a tool (e.g. "brush", "gradient"); never NULL or empty. */
const char *pc_ui_tool_icon(pc_ui_tool t);

#endif
