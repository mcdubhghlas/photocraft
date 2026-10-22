/* Edit > Presets > Preset Manager and Export/Import Presets (port of the preset half of
 * crates/engine/src/edit_menu_cmds.rs). Two engine commands that list/rename/delete/reorder the
 * preset libraries (brushes, custom shapes, patterns) and move brush + custom-shape sets in and out
 * through a portable JSON preset file. Internal to the engine (registered from command.c).
 *
 * Never crashes on any params or session state (REWRITE_PLAN.md rule 9). */
#ifndef PC_ENGINE_PRESET_MANAGER_CMDS_H
#define PC_ENGINE_PRESET_MANAGER_CMDS_H

#include <stddef.h>

#include "engine_internal.h"

/* edit.presets.presetManager + edit.presets.exportImportPresets. */
const pc_engine_command *pc_engine_preset_manager_specs(size_t *n);

/* The exported-preset file format tag (its "format" field). */
#define PC_PRESET_FILE_FORMAT "photocraft-presets"

#endif /* PC_ENGINE_PRESET_MANAGER_CMDS_H */
