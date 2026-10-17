/* Shared declarations between the shell's app.c (owns the PhotocraftApp struct and the frame loop)
 * and main.c (SDL entry point + input translation). GUI types (SDL/Nuklear) cross this boundary, so
 * it is only used by the shell's own translation units, never by the control test or app.h clients. */
#ifndef PC_UI_SHELL_INTERNAL_H
#define PC_UI_SHELL_INTERNAL_H

#include <SDL3/SDL.h>

#include "app.h"
#include "photocraft_nuklear.h"

/* Translate one SDL event into Nuklear input and shell shortcuts (pointer, keys, text). Called by
 * the frame loop between nk_input_begin/end. Never crashes on any event. */
void pc_ui_shell_input_event(pc_ui_shell_app *a, const SDL_Event *e);

bool pc_ui_shell_canvas_event(pc_ui_shell_app *a, const SDL_Event *event);

/* Release the lazily-built shortcut binding table (the interactive loop builds it on first key). */
void pc_ui_shell_input_cleanup(void);

/* Internals the input translation reaches (defined in app.c). */
struct nk_context *pc_ui_shell_app_nk(pc_ui_shell_app *a);
SDL_Window *pc_ui_shell_app_sdl_window(pc_ui_shell_app *a);
float pc_ui_shell_app_ui_scale(const pc_ui_shell_app *a);

#endif /* PC_UI_SHELL_INTERNAL_H */
