/* Single-layer base commands and their shared helpers (port of the Layer-menu specs in
 * crates/engine/src/commands.rs build(), crates/engine/src/new_layer.rs and
 * crates/engine/src/clip_group.rs).
 *
 * E5 (layer_multi_cmds.c) left the single-layer base commands and the clip-group / translate_layer
 * / layer_copy / background helpers they need to this chunk (see that file's scope note). This
 * header is internal to the engine crate (not engine.h); it exposes the clip-group primitives the
 * New-Layer, Delete and Arrange commands share, plus the three module registries.
 */
#ifndef PC_ENGINE_LAYER_BASE_CMDS_H
#define PC_ENGINE_LAYER_BASE_CMDS_H

#include "layer_cmds.h"

/* ---- clip groups (port of crates/engine/src/clip_group.rs) -------------------------------- *
 *
 * A clipped layer clips to the nearest unclipped layer below it (its base); a base and the run of
 * clipped layers directly above it form a clip group. These helpers keep groups whole as layers
 * are added, moved and deleted (#2203). */

/* The clip groups of the layers about to move or be deleted, read before they leave their slots.
 * Owned; release with pc_engine_clip_before_fini. A zeroed value is the "default" (new layers). */
typedef struct pc_engine_clip_before {
    pc_layer_id *moving;
    size_t nmoving;
    pc_layer_id *clipped; /* each moving layer that was clipped */
    size_t nclipped;
    struct pc_engine_clip_group { /* each moving layer's clip group (base first), when it had one */
        pc_layer_id id;
        pc_layer_id *ids;
        size_t n;
    } *groups;
    size_t ngroups;
    pc_layer_id *left; /* layers that lose their base (clipped to a moving base, not moving) */
    size_t nleft;
} pc_engine_clip_before;

/* Read the clip groups of `moving` (moved or deleted together). On OOM fills err and leaves *out
 * a valid empty value. */
pc_status pc_engine_clip_before_read(const pc_document *doc, const pc_layer_id *moving, size_t nmoving,
                                     pc_engine_clip_before *out, pc_error *err);
void pc_engine_clip_before_fini(pc_engine_clip_before *b); /* NULL-safe */

/* After `placed` were added or moved (`before` read before they moved, or NULL for new layers):
 * each placed layer joins or leaves clip groups (rules 1-2), topmost first, or is clipped anyway
 * when `force`. Then layers that lost their base are released (rule 3), and clipped runs with
 * nothing below them (rule 4). On OOM fills err (the document is left partially settled). */
pc_status pc_engine_clip_settle(pc_document *doc, const pc_layer_id *placed, size_t nplaced,
                                const pc_engine_clip_before *before, bool force, pc_error *err);

/* ---- module registries (register_modules() in command.c) ---------------------------------- */

const pc_engine_command *pc_engine_new_layer_specs(size_t *n);   /* layer.new.layer */
const pc_engine_command *pc_engine_layer_base_specs(size_t *n);  /* new.group/duplicate/delete/… */
const pc_engine_command *pc_engine_clip_specs(size_t *n);        /* clipping mask + arrange */

#endif /* PC_ENGINE_LAYER_BASE_CMDS_H */
