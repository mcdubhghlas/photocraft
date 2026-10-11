/* A layer's effective mask (pixel mask with density x vector mask, each blurred by its feather)
 * as one single-channel f32 surface (port of crates/compose/src/masks.rs), for the GPU
 * compositor and for feathered masks on the CPU. Results are cached per mask state (64 entries,
 * least recently used dropped first; the key includes the mask's tile addresses, and the entry
 * keeps those tiles alive so the addresses can't be reused); unchanged masks return the same
 * tiles. Thread-safe (the cache is mutex-protected). */
#ifndef PC_COMPOSE_MASKS_H
#define PC_COMPOSE_MASKS_H

#include <stdbool.h>

#include "doc/doc.h"

/* Gaussian sigma of a feather of px pixels (sigma = feather, capped at 1000; 0 for non-finite). */
float pc_compose_feather_sigma(float px);
/* The layer has an enabled mask with a feather. */
bool pc_compose_has_feather(const pc_layer *layer);
/* The combined mask over canvas into *out (a new surface sharing the cached tiles), with
 * *found = true; *found = false (out untouched) when the layer has no enabled vector mask and no
 * feathered pixel mask (use the pixel mask directly). */
pc_status pc_compose_combined_mask(const pc_layer *layer, pc_rect canvas, bool *found, pc_surface *out,
                                   pc_error *err);
/* Drops every cached combined mask (tests, purge). */
void pc_compose_purge_mask_cache(void);

#endif
