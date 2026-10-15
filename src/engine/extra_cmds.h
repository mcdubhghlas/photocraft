/* Everyday Photoshop commands that build on the core ones (port of the self-contained parts of
 * crates/engine/src/extra_cmds.rs, E12): Edit > Stroke, Reselect, Equalize, Layer From Background,
 * Hide/Show All Effects, the layer-mask enable/link toggles, the Rasterize variants, Delete Hidden
 * / Empty Layers, Ungroup, Hide/Show Layers, Lock Transparent Pixels, Show Only This Layer, and
 * Filter > Blur > Average / Render > Clouds.
 *
 * This header exposes only the Background<->Layer helpers the reference keeps as pub(crate) fns and
 * that other chunks reuse (E3 Canvas Size, E6 smart objects, E10 erasers, and the full Free
 * Transform): turning the structural Background into a normal "Layer 0" before an edit it can't
 * take. Internal to the engine crate (not the public engine.h API). */
#ifndef PC_ENGINE_EXTRA_CMDS_H
#define PC_ENGINE_EXTRA_CMDS_H

#include "engine_internal.h"

/* Make `l` a normal layer: rename to "Layer 0" and drop the transparency and position locks the
 * Background carries (port of extra_cmds::unlock_background). Never fails. */
void pc_engine_unlock_background(pc_layer *l);

/* If `id` is the document's structural Background (pc_document_background, whatever its name), turn
 * it into a normal layer ("Layer 0"), as Photoshop does before an edit the Background can't take
 * (moving, transforming, exposing transparency, a style, a mask). Other layers are left untouched.
 * Port of extra_cmds::background_to_layer; the KEY export E3/E6/E10 and Free Transform reach for. */
void pc_engine_background_to_layer(pc_document *doc, pc_layer_id id);

/* Alias used before a layer gains a mask (extra_cmds::background_to_layer_for_mask). */
void pc_engine_background_to_layer_for_mask(pc_document *doc, pc_layer_id id);

/* The registry module (register_modules() in command.c). */
const pc_engine_command *pc_engine_extra_specs(size_t *n);

#endif /* PC_ENGINE_EXTRA_CMDS_H */
