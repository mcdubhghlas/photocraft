#ifndef PC_UI_SHELL_LAYER_THUMBS_H
#define PC_UI_SHELL_LAYER_THUMBS_H
#include "panels.h"
#include "ui/layer_thumbnail.h"
void pc_ui_layer_thumb_widget(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime,
                              const pc_engine_doc_state *state, const pc_layer *layer,
                              pc_ui_layer_thumbnail_kind kind);
void pc_ui_layer_mask_chain(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime, const pc_layer *layer,
                            bool vector);
#endif
