#ifndef PC_UI_TOOLBAR_H
#define PC_UI_TOOLBAR_H
#include "ui/state.h"
/* Photoshop toolbar slots. Every registered tool occurs once, in a stable section. */
size_t pc_ui_toolbar_slots(void);
const pc_ui_tool *pc_ui_toolbar_slot(size_t slot, size_t *count, unsigned *section);
pc_ui_tool pc_ui_toolbar_current(const pc_ui_state *state, size_t slot);
#endif
