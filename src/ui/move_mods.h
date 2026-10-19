#ifndef PC_UI_MOVE_MODS_H
#define PC_UI_MOVE_MODS_H
#include "ui/shortcuts.h"
pc_status pc_ui_move_key(pc_engine_session *session, const pc_ui_state *ui, pc_ui_key key, unsigned modifiers,
                         pc_json_value *out, pc_error *err);
/* 0 draws normally, 1 moves the selection outline, 2 floats its pixels, 3 moves whole layers. */
unsigned pc_ui_selection_move_kind(const pc_engine_session *session, const pc_ui_state *ui, double x,
                                   double y, unsigned modifiers);
/* Nearest direction with eight degrees of hysteresis around the previous direction. */
void pc_ui_move_constrain(const double delta[2], const double *previous, unsigned directions, double out[2]);
#endif
