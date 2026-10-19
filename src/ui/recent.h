#ifndef PC_UI_RECENT_H
#define PC_UI_RECENT_H
#include "ui/state.h"
pc_status pc_ui_recent_push(pc_ui_state *state, const char *path, size_t limit, pc_error *err);
void pc_ui_recent_clear(pc_ui_state *state);
#endif
