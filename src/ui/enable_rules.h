#ifndef PC_UI_ENABLE_RULES_H
#define PC_UI_ENABLE_RULES_H
#include "engine/engine.h"
#include "ui/state.h"
bool pc_ui_disabled_for_layer(const pc_document *doc, const pc_layer *layer, const char *command);
bool pc_ui_disabled_for_selection(const pc_document *doc, const pc_layer_id *selected, size_t count,
                                  const pc_layer *active, const char *command);
bool pc_ui_command_disabled(const pc_engine_session *session, const pc_ui_state *state, const char *command);
#endif
