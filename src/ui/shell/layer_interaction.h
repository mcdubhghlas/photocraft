#ifndef PC_UI_SHELL_LAYER_INTERACTION_H
#define PC_UI_SHELL_LAYER_INTERACTION_H
#include "panels.h"
#include <SDL3/SDL.h>
void pc_ui_layer_interaction_fini(pc_ui_panel_runtime *runtime);
pc_status pc_ui_layer_rename_start(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime, pc_layer_id layer,
                                   pc_error *err);
bool pc_ui_layer_interaction_event(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime,
                                   const SDL_Event *event);
bool pc_ui_layer_rename_row(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime,
                            const pc_engine_doc_state *state, const pc_layer *layer);
void pc_ui_layer_drag_feedback(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime);
#endif
