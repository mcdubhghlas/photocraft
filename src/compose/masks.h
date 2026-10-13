/* A layer's effective mask (pixel mask with density x vector mask, each blurred by its feather)
 * as one single-channel U16 surface (port of crates/compose/src/masks.rs), for the GPU
 * compositor and for feathered masks on the CPU. Results are cached per mask state (64 entries
 * and PC_COMPOSE_MASK_CACHE_BUDGET accounted bytes, least recently used dropped first; the key
 * includes the mask's tile addresses, and the entry keeps those tiles alive so the addresses
 * can't be reused); unchanged masks return the same tiles. Thread-safe (the cache is
 * mutex-protected). */
#ifndef PC_COMPOSE_MASKS_H
#define PC_COMPOSE_MASKS_H

#include <stdbool.h>
#include <stddef.h>

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
/* Accounted pixel-byte limit of the combined-mask cache: five 50 MP U8-input / U16-result pairs. */
#define PC_COMPOSE_MASK_CACHE_BUDGET ((size_t)768 << 20)
/* Drops every cached combined mask (Edit > Purge > All), returning its accounted pixel bytes;
 * builds that started before the purge can't refill the cache. */
size_t pc_compose_purge_mask_cache(void);
/* Pixel bytes accounted for by the cache (inputs and results; shared tiles counted per entry). */
size_t pc_compose_mask_cache_bytes(void);
/* The bytes the cache charges for a surface: its tiles plus its default pixel. */
size_t pc_compose_mask_surface_bytes(const pc_surface *s);

#endif
