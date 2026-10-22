/* Editing an existing fill layer and querying a layer's content editor (port of solid_fill_cmds.rs,
 * pattern_fill_cmds.rs and layer.layerContentOptions). See fill_layer_set_cmds.c. */
#ifndef PC_ENGINE_FILL_LAYER_SET_CMDS_H
#define PC_ENGINE_FILL_LAYER_SET_CMDS_H

#include <stddef.h>

#include "engine_internal.h"

const pc_engine_command *pc_engine_fill_layer_set_specs(size_t *n);

#endif /* PC_ENGINE_FILL_LAYER_SET_CMDS_H */
