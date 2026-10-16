/* Layer > Align (6) and Layer > Distribute (8): port of the align/distribute CommandSpecs in
 * crates/engine/src/layer_multi_cmds.rs (align, distribute, can_align, can_distribute, and the
 * content-bounds / move helpers they share with layer_multi_cmds.c). Registered in command.c. */
#ifndef PC_ENGINE_ALIGN_DISTRIBUTE_CMDS_H
#define PC_ENGINE_ALIGN_DISTRIBUTE_CMDS_H

#include <stddef.h>

#include "engine.h"

const pc_engine_command *pc_engine_align_distribute_specs(size_t *n);

#endif
