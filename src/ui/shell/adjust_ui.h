#ifndef PC_UI_SHELL_ADJUST_UI_H
#define PC_UI_SHELL_ADJUST_UI_H
#include "dialogs.h"
#include "ui/canvas.h"
#include <SDL3/SDL.h>
void pc_ui_shell_adjust_graph(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime, pc_ui_dialog *dialog);
/* Route a canvas left-click to an open Levels/Curves dialog whose Set Black/Gray/White Point
 * eyedropper is armed. Returns true if the event was consumed. */
bool pc_ui_shell_adjust_eyedropper_canvas(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf,
                                          const SDL_Event *e, float factor);
#endif
