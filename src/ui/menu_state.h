#ifndef PC_UI_MENU_STATE_H
#define PC_UI_MENU_STATE_H
#include "ui/menus.h"
/* Shared native/control menu state, including view checks and open-document entries. */
pc_status pc_ui_menu_state_items(const pc_engine_session *session, const pc_ui_state *state,
                                 pc_ui_menu_item **out, size_t *count, pc_error *err);
#endif
