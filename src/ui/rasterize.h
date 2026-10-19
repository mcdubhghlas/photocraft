#ifndef PC_UI_RASTERIZE_H
#define PC_UI_RASTERIZE_H
#include "ui/state.h"
#include "engine/engine.h"
typedef struct pc_ui_rasterize_kind {
    const char *command, *message;
} pc_ui_rasterize_kind;
const pc_ui_rasterize_kind *pc_ui_rasterize_needed(const pc_engine_session *session, const pc_ui_state *ui);
bool pc_ui_rasterize_filter_needed(const pc_engine_session *session, const char *command, bool *smart);
#endif
