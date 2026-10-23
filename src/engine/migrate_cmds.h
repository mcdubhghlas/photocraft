/* Edit > Presets > Migrate Presets: edit.presets.migratePresets (migrate_cmds.c). */
#ifndef PC_ENGINE_MIGRATE_CMDS_H
#define PC_ENGINE_MIGRATE_CMDS_H

#include "engine.h"

/* The command specs for register_modules() (command.c). */
const pc_engine_command *pc_engine_migrate_specs(size_t *n);

#endif /* PC_ENGINE_MIGRATE_CMDS_H */
