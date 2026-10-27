/* History panel menu: history.stepForward / stepBackward / clear, and history.setSource
 * (port of crates/engine/history_menu_cmds.rs + history_source_cmds.rs). */
#ifndef PC_ENGINE_HISTORY_MENU_CMDS_H
#define PC_ENGINE_HISTORY_MENU_CMDS_H

#include <stddef.h>

#include "engine.h"

const pc_engine_command *pc_engine_history_menu_specs(size_t *n);
const pc_engine_command *pc_engine_history_source_specs(size_t *n);

#endif
