/* Hardware cursor bitmaps: straight RGBA, centred hotspot, independent of canvas repaint.
 * Geometry is in logical points. Input and allocations are bounded; failed builds preserve out. */
#ifndef PC_UI_TOOL_CURSOR_H
#define PC_UI_TOOL_CURSOR_H
#include "base/status.h"
#include "paint/outline.h"
#include "ui/shortcuts.h"

typedef enum pc_ui_cursor_kind { PC_UI_CURSOR_CIRCLE, PC_UI_CURSOR_CROSS, PC_UI_CURSOR_OUTLINE, PC_UI_CURSOR_HAND } pc_ui_cursor_kind;
typedef enum pc_ui_badge { PC_UI_BADGE_NONE, PC_UI_BADGE_ADD, PC_UI_BADGE_SUBTRACT, PC_UI_BADGE_INTERSECT } pc_ui_badge;
typedef struct pc_ui_cursor_shape {
    pc_ui_cursor_kind kind;
    float radius, length, gap;
    bool center, closed;
    const pc_paint_ring *rings;
    size_t count;
    pc_ui_badge badge;
} pc_ui_cursor_shape;
typedef struct pc_ui_cursor_bitmap {
    uint8_t *rgba;
    uint16_t side, hotspot;
} pc_ui_cursor_bitmap;
void pc_ui_cursor_bitmap_fini(pc_ui_cursor_bitmap *bitmap);
pc_status pc_ui_cursor_rasterize(const pc_ui_cursor_shape *shape, float scale, uint16_t max_side,
                                  pc_ui_cursor_bitmap *out, pc_error *err);
/* Same tool/modifier semantics for the cursor badge and the subsequent selection gesture. */
bool pc_ui_is_selection_tool(pc_ui_tool tool);
const char *pc_ui_selection_mode(pc_ui_tool tool, uint8_t bar, unsigned mods, bool has_selection);
pc_ui_badge pc_ui_badge_from_mode(const char *mode);
#endif
