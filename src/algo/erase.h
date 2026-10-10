/* Magic Eraser: erase a wand region to transparency (or, on a layer with locked transparency, to
 * a colour), tile by tile (port of crates/algo/src/erase.rs).
 *
 * Works in the surface's own channels at any depth and in any colour model: only the alpha
 * channel changes (or, under a transparency lock, the colour channels move toward lock_color).
 * Tiles are edited in parallel on the global pool and written back serially. */
#ifndef PC_ALGO_ERASE_H
#define PC_ALGO_ERASE_H

#include <stddef.h>

#include "algo/selection.h"
#include "base/status.h"
#include "geom/geom.h"
#include "raster/surface.h"

/* Erases `region` from target at `amount` (0..1, the tool's opacity; non-finite is 0), weighted by
 * the selection's coverage (NULL: everything selected). With lock_color (native colour values,
 * lock_len of them, missing ones 0; NULL for none) the pixels are painted toward that colour and
 * alpha kept. *damaged receives the changed rectangle (PC_RECT_EMPTY for nothing: an empty box,
 * zero amount, a mask whose length doesn't match the box, or no alpha and no lock colour). */
pc_status pc_algo_erase_magic(pc_surface *target, const pc_algo_region *region, float amount,
                              const pc_surface *selection, const float *lock_color, size_t lock_len,
                              pc_rect *damaged, pc_error *err);

#endif
