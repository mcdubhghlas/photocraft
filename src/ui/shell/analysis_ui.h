#ifndef PC_UI_SHELL_ANALYSIS_UI_H
#define PC_UI_SHELL_ANALYSIS_UI_H
#include "shell_internal.h"
#include "ui/analysis.h"

/* The analysis runtime lives in the shell app (defined in app.c). */
pc_ui_analysis *pc_ui_shell_analysis_runtime(pc_ui_shell_app *app);

/* Menu/command front end for the Ruler/Count tool selection, the Measurement Log / Notes panel
 * toggles, the Set Measurement Scale / Select Data Points / Place Scale Marker dialogs and Record
 * Measurements. Returns PC_ERR_NOT_FOUND when `id` is not handled here (so the caller tries the
 * engine command directly). */
pc_status pc_ui_shell_analysis_command(pc_ui_shell_app *app, const char *id, yyjson_val *params,
                                       pc_json_value *out, pc_error *err);

/* Routes a canvas mouse event to the Ruler/Count/Note tools (these tools are not gesture-supported,
 * so the generic canvas path skips them). Returns true when the event is consumed. */
bool pc_ui_shell_analysis_canvas(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, const SDL_Event *event,
                                 float factor);

/* Draws the Ruler/Count/Note options bar; returns true when `tool` is one of those (so the generic
 * options bar is skipped). */
bool pc_ui_shell_analysis_options(pc_ui_shell_app *app);

/* The canvas overlay (ruler line, count markers, note icons). */
void pc_ui_shell_analysis_draw(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf);

/* The Measurement Log and Notes floating panels and the analysis dialogs. */
void pc_ui_shell_analysis_windows(pc_ui_shell_app *app);

#endif
