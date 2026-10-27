/* layer.list: the layer tree alone, for agents and scripts (port of crates/engine/layer_list_cmds.rs). */
#ifndef PC_ENGINE_LAYER_LIST_CMDS_H
#define PC_ENGINE_LAYER_LIST_CMDS_H

#include <stddef.h>

#include "engine.h"

const pc_engine_command *pc_engine_layer_list_specs(size_t *n);

#endif
