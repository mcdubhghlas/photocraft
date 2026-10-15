/* Everyday Edit-menu commands: the session clipboard (Cut / Copy / Copy Merged / Paste / Paste in
 * Place), Clear, Layer via Copy / Cut, Merge Visible, Toggle Last State, and New from Clipboard
 * (port of crates/engine/src/edit_cmds.rs and the `edit.clear` spec from commands.rs::build(), E4).
 *
 * Internal to the engine crate (not the public engine.h API); it exposes only the module registry.
 * The whole-layer clipboard (a copied smart / type / shape / group layer that Paste recreates
 * rather than rasterising) depends on photocraft_io::linked::select_items and the layer_copy
 * adapt/translate helpers, and is deferred (REWRITE_PLAN.md §7/§8); the Clip carries only pixels. */
#ifndef PC_ENGINE_EDIT_CMDS_H
#define PC_ENGINE_EDIT_CMDS_H

#include "engine_internal.h"

/* The Edit clipboard/clear registry (register_modules() in command.c). */
const pc_engine_command *pc_engine_edit_specs(size_t *n);

#endif /* PC_ENGINE_EDIT_CMDS_H */
