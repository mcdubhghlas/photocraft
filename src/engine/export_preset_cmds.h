/* file.export.presets.* — named Export As configurations (port of
 * crates/engine/export_preset_cmds.rs). */
#ifndef PC_ENGINE_EXPORT_PRESET_CMDS_H
#define PC_ENGINE_EXPORT_PRESET_CMDS_H

#include <stddef.h>

#include "engine.h"

const pc_engine_command *pc_engine_export_preset_specs(size_t *n);

#endif
