/* Clone/Healing source crosshair decision (headless; see clone_marker.c). */
#ifndef PC_UI_CLONE_MARKER_H
#define PC_UI_CLONE_MARKER_H

#include "ui/state.h"
#include "ui/tool_gesture.h"

/* True and writes the source point (document coordinates) in `out` when `tool` is Clone Stamp or
 * Healing Brush and `gesture` holds an Alt-click source for `document`; false otherwise. NULL-safe. */
bool pc_ui_clone_source_marker(const pc_ui_tool_gesture *gesture, pc_ui_tool tool, pc_doc_id document,
                               double out[2]);

#endif /* PC_UI_CLONE_MARKER_H */
