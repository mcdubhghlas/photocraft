/* Layer > Matting, Mask All Objects, and Smart Objects > Reveal in Finder (gap wave re-port of
 * crates/engine/src/layer_menu_cmds.rs). See matting_gap_cmds.c. */
#ifndef PC_ENGINE_MATTING_GAP_CMDS_H
#define PC_ENGINE_MATTING_GAP_CMDS_H

#include <stddef.h>

#include "engine.h"

const pc_engine_command *pc_engine_matting_gap_specs(size_t *n);

#endif
