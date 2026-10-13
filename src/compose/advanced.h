/* Layer Style > Blending Options > Advanced Blending (port of crates/compose/src/advanced.rs):
 * knockout, Blend Clipped Layers as Group off, Transparency Shapes Layer off, Layer / Vector Mask
 * Hides Effects. Chunk C2 implements it and deletes stub_c2.c; the compositing hooks that take
 * the render context are declared in compose/internal.h. Pure predicates; thread-safe. */
#ifndef PC_COMPOSE_ADVANCED_H
#define PC_COMPOSE_ADVANCED_H

#include <stdbool.h>
#include <stddef.h>

#include "doc/doc.h"

/* Any Advanced Blending option changes how layer (with its clipping group) composites. */
bool pc_compose_advanced_active(const pc_layer *layer, const pc_layer *clipped, size_t nclipped);
/* Effects or knockout cover the whole layer (Transparency Shapes Layer off with either). */
bool pc_compose_shapeless(const pc_layer *layer);
/* Whether the bottom layer of a stack is the Background: identified by its locks and kind
 * (pc_layer_has_background_traits), never by its (localized) name. */
bool pc_compose_is_background(const pc_layer *layer);

#endif
