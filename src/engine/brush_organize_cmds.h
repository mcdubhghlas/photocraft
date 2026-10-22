/* Organising brush presets: rename/move/moveGroup/renameGroup/deleteGroup (port of
 * crates/engine/src/brush_preset_cmds.rs). See brush_organize_cmds.c. */
#ifndef PC_ENGINE_BRUSH_ORGANIZE_CMDS_H
#define PC_ENGINE_BRUSH_ORGANIZE_CMDS_H

#include <stddef.h>

#include "engine_internal.h"

const pc_engine_command *pc_engine_brush_organize_specs(size_t *n);

#endif /* PC_ENGINE_BRUSH_ORGANIZE_CMDS_H */
