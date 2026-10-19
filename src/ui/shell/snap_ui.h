#ifndef PC_UI_SHELL_SNAP_UI_H
#define PC_UI_SHELL_SNAP_UI_H
#include "shell_internal.h"
#include "ui/snap.h"
#include "ui/crop.h"
pc_ui_snap *pc_ui_shell_snap_runtime(pc_ui_shell_app *app);
void pc_ui_shell_snap_filter(pc_ui_shell_app *app, pc_ui_pointer_kind kind, pc_ui_tool_point *point,
                             unsigned mods);
void pc_ui_shell_snap_retarget_move(pc_ui_shell_app *app, pc_ui_tool_point point);
void pc_ui_shell_snap_draw(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf);
double pc_ui_shell_snap_guide(pc_ui_shell_app *app, bool vertical, double position);
void pc_ui_shell_snap_crop(pc_ui_shell_app *app, const pc_ui_crop_frame *frame, const pc_ui_crop_drag *drag,
                           double point[2], double ratio, bool shift, bool alt, bool space,
                           unsigned modifiers, double zoom);
#endif
