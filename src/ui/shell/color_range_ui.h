#ifndef PC_UI_SHELL_COLOR_RANGE_UI_H
#define PC_UI_SHELL_COLOR_RANGE_UI_H
#include "dialogs.h"
#include "ui/canvas.h"
#include <SDL3/SDL.h>
pc_status pc_ui_shell_color_range_init(pc_ui_shell_app *app, yyjson_mut_doc *fields, pc_error *err);
void pc_ui_shell_color_range_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime, pc_ui_dialog *dialog);
/* Add one Color Range sample point in image space (tool: 0 sample/replace, 1 add, 2 subtract). */
void pc_ui_shell_color_range_add_point(pc_ui_dialog *dialog, float x, float y, unsigned tool);
/* Route a canvas left-click to the open Color Range dialog's on-canvas pipette. True if consumed. */
bool pc_ui_shell_color_range_canvas(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, const SDL_Event *e,
                                    float factor);
/* Draw the Color Range selection-preview matte over the canvas from the dialog preview's selection. */
void pc_ui_shell_color_range_overlay(pc_ui_shell_app *app, pc_doc_arc *preview, const pc_ui_canvas_xform *xf);
#endif
