/* Held keys: Photoshop's temporary tools (Space = Hand, ⌘Space = Zoom In, ⌥Space = Zoom Out,
 * ⌘ = Move with non-selection tools, ` = erase with Brush/Pencil). Port of the pure, input-free
 * half of crates/ui-egui/src/hold_keys.rs: the tool classification and the Temporary enum. The
 * input-driven resolution (held_tool/for_frame/binding/cmd_nudges/erase_held/reposition_held) reads
 * live key state and PhotocraftApp view state and is reimplemented against SDL input when the app
 * shell is assembled (later U-chunks). No GUI dependency. */
#ifndef PC_UI_HOLD_KEYS_H
#define PC_UI_HOLD_KEYS_H

#include <stdbool.h>
#include <stddef.h>

#include "ui/state.h"

/* A tool in effect only while its key is held. */
typedef enum pc_ui_temporary {
    PC_UI_TEMP_HAND,
    PC_UI_TEMP_ZOOM_IN,
    PC_UI_TEMP_ZOOM_OUT,
    PC_UI_TEMP_MOVE /* ⌘ held with a cmd_moves tool: a modifier, not a rebindable key */
} pc_ui_temporary;

/* The Edit › Keyboard Shortcuts binding id of a temporary tool. */
const char *pc_ui_temporary_id(pc_ui_temporary t);
/* The tool a temporary stands in for. */
pc_ui_tool pc_ui_temporary_tool(pc_ui_temporary t);
/* The three key-bound temporary tools (Hand, Zoom In, Zoom Out), in that order. */
extern const pc_ui_temporary PC_UI_TEMPORARY_BOUND[3];

/* Is holding ⌘ (Ctrl off the Mac) the Move tool for this tool? (All but the Hand/Rotate View/Zoom/
 * Crop/Slice/Path/Direct Selection/Pen/shape/Type and marquee/lasso/Magic Wand tools.) */
bool pc_ui_cmd_moves(pc_ui_tool t);
/* Tools whose strokes the erase key turns into erasing strokes (Brush, Pencil). */
bool pc_ui_erases_while_held(pc_ui_tool t);
/* Tools whose in-progress drag the reposition (Hand) key moves. */
bool pc_ui_repositions(pc_ui_tool t);
/* Is `id` a held temporary tool (prefs::TEMPORARY_TOOLS) rather than a command? */
bool pc_ui_is_temporary(const char *id);
/* Is `t` a vector shape tool (Rectangle/Ellipse/Triangle/Polygon/Line/Custom Shape)? Port of
 * vector_ui::is_shape_tool, needed by cmd_moves/repositions. */
bool pc_ui_is_shape_tool(pc_ui_tool t);

#endif /* PC_UI_HOLD_KEYS_H */
