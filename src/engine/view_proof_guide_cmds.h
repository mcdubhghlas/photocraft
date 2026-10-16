/* View > Proof Setup simulations, 32-bit Preview Options and the View > guide commands
 * (port of crates/engine/src/proof_sim.rs plus the guide specs from edit_cmds.rs and
 * guide_layout.rs / file_cmds.rs, layer L5). See view_proof_guide_cmds.c for the full note. */
#ifndef PC_ENGINE_VIEW_PROOF_GUIDE_CMDS_H
#define PC_ENGINE_VIEW_PROOF_GUIDE_CMDS_H

#include <stddef.h>

#include "engine.h"

/* The command specs for this module: the eight View > Proof Setup simulation variants,
 * view.thirtyTwoBitPreviewOptions, and view.clearGuides / view.newGuide / view.newGuideLayout.
 * Register in command.c::register_modules(). */
const pc_engine_command *pc_engine_view_proof_guide_specs(size_t *n);

#endif /* PC_ENGINE_VIEW_PROOF_GUIDE_CMDS_H */
