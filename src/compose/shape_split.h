/* Stroked shape layers split into fill and vector stroke (port of
 * crates/compose/src/shape_split.rs). Chunk C2 implements it and deletes stub_c2.c. */
#ifndef PC_COMPOSE_SHAPE_SPLIT_H
#define PC_COMPOSE_SHAPE_SPLIT_H

#include <stdbool.h>

#include "doc/doc.h"

/* shape_split::split: *found = false is the reference's None; else *fill and *stroke are new
 * surfaces (written only on success with *found). */
pc_status pc_compose_shape_split(const pc_shape_layer *sh, pc_rect canvas, pc_sample_type depth, bool *found,
                                 pc_surface *fill, pc_surface *stroke, pc_error *err);

#endif
