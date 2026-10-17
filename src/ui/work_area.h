/* The part of the window the user can actually see and reach (#315): a window can be larger than
 * its display and run under a taskbar/Dock. Port of crates/ui-egui/src/work_area.rs's geometry
 * (visible_part / fits); the egui ViewportInfo plumbing and fit_window live in the shell.
 *
 * Pure geometry, no SDL/GL; unit-testable headlessly. */
#ifndef PC_UI_WORK_AREA_H
#define PC_UI_WORK_AREA_H

#include <stdbool.h>

/* A rectangle as min/max corners in UI points (matching egui's Rect). */
typedef struct pc_ui_rect {
    float x0, y0, x1, y1;
} pc_ui_rect;

/* Room kept free at the monitor's bottom for a taskbar/Dock egui can't see. */
#define PC_UI_TASKBAR 48.0f

/* How the window sits on its monitor. */
typedef enum pc_ui_window_state {
    PC_UI_WIN_NORMAL = 0,
    PC_UI_WIN_MAXIMIZED,
    PC_UI_WIN_FULLSCREEN, /* full screen covers the taskbar on purpose: all of it is visible */
} pc_ui_window_state;

/* Where the window's content can be seen. `content` is the content rect. `monitor_*` is the
 * monitor's size (has_monitor false = unknown). `inner_*` is the content's rect on the desktop
 * (has_inner false = unknown, e.g. Wayland). Returns the visible rect. */
pc_ui_rect pc_ui_visible_part(pc_ui_rect content, bool has_monitor, float monitor_w, float monitor_h,
                              bool has_inner, pc_ui_rect inner, pc_ui_window_state state);

/* Does a window of `outer_w` x `outer_h` fit on a `monitor_w` x `monitor_h` monitor less the
 * taskbar? */
bool pc_ui_window_fits(float outer_w, float outer_h, float monitor_w, float monitor_h);

#endif /* PC_UI_WORK_AREA_H */
