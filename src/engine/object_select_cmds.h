/* Object Selection (select.object): the GrabCut object finder, rectangle and point modes (port of
 * the object_selection half of crates/engine/src/smartselect_cmds.rs). See object_select_cmds.c. */
#ifndef PC_ENGINE_OBJECT_SELECT_CMDS_H
#define PC_ENGINE_OBJECT_SELECT_CMDS_H

#include <stddef.h>

#include "engine_internal.h"

const pc_engine_command *pc_engine_object_select_specs(size_t *n);

#endif /* PC_ENGINE_OBJECT_SELECT_CMDS_H */
