/* Clone/Healing source crosshair: the testable half of the on-canvas source marker. The shell
 * draws it from pc_ui_shell_clone_source_draw() (src/ui/shell/retouch_ui.c); this decides whether
 * and where. Port of source_marker_point in crates/ui-egui/src/retouch_ui.rs: the marker shows the
 * Alt-click source while a Clone Stamp or Healing Brush tool is active and a source has been set on
 * the current document. The engine owns the authoritative source + aligned pairing (cloneSource.set);
 * the gesture keeps the Alt-click point for this feedback only. NULL-safe; never crashes (rule 9). */
#include "ui/clone_marker.h"

#include <math.h>

bool pc_ui_clone_source_marker(const pc_ui_tool_gesture *gesture, pc_ui_tool tool, pc_doc_id document,
                               double out[2])
{
    if (gesture == NULL || out == NULL) {
        return false;
    }
    if (tool != PC_TOOL_CLONE_STAMP && tool != PC_TOOL_HEALING) {
        return false;
    }
    if (!gesture->has_clone_source || gesture->clone_source_doc != document) {
        return false;
    }
    double x = gesture->clone_source[0], y = gesture->clone_source[1];
    if (!isfinite(x) || !isfinite(y)) {
        return false;
    }
    out[0] = x;
    out[1] = y;
    return true;
}
