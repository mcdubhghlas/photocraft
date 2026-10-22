/* Importing Photoshop brush files: brush.presets.importAbr (port of the importAbr half of
 * crates/engine/src/preset_import_cmds.rs). See preset_import_cmds.c. */
#ifndef PC_ENGINE_PRESET_IMPORT_CMDS_H
#define PC_ENGINE_PRESET_IMPORT_CMDS_H

#include <stddef.h>

#include "engine_internal.h"

const pc_engine_command *pc_engine_preset_import_specs(size_t *n);

#endif /* PC_ENGINE_PRESET_IMPORT_CMDS_H */
