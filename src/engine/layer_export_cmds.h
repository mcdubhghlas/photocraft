/* Layer menu export + Create Layer: layer.quickExportAsPng, layer.exportAs,
 * layer.layerStyle.createLayer (layer_export_cmds.c). */
#ifndef PC_ENGINE_LAYER_EXPORT_CMDS_H
#define PC_ENGINE_LAYER_EXPORT_CMDS_H

#include "engine.h"

/* The command specs for register_modules() (command.c). */
const pc_engine_command *pc_engine_layer_export_specs(size_t *n);

#endif /* PC_ENGINE_LAYER_EXPORT_CMDS_H */
