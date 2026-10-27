/* A smart object's filter mask (#215): how the filtered pixels and the unfiltered (placed) ones
 * combine. Port of crates/compose/src/filter_mask.rs.
 *
 * Photoshop stores a smart object's layer pixels with its filters applied but not its filter
 * mask, and mixes the unfiltered pixels back in through the mask when it composites (photoshop
 * corpus sf-filter-mask-hard.psd / sf-filter-mask-gradient.psd: the stored pixels are filtered
 * everywhere, the merged image only where the mask shows the filter). Our smart objects keep the
 * mixed result as their pixels, so PSD import mixes (pc_compose_filter_mask_mix) and export
 * unmixes (pc_compose_filter_mask_unmix).
 *
 * Conventions (REWRITE_PLAN.md §4): fallible functions return pc_status and take an optional
 * pc_error; *out is initialised on success only, and the caller frees it with pc_surface_fini.
 * Nothing aborts. */
#ifndef PC_COMPOSE_FILTER_MASK_H
#define PC_COMPOSE_FILTER_MASK_H

#include "base/status.h"
#include "doc/doc.h"
#include "raster/surface.h"

/* `filtered` mixed over `base` (the unfiltered pixels) through `mask`: premultiplied, any channel
 * layout. Pixels the mask hides entirely are `base`'s. On a format mismatch or an empty union the
 * result is a clone of `filtered`. */
pc_status pc_compose_filter_mask_mix(const pc_surface *base, const pc_surface *filtered,
                                     const pc_layer_mask *mask, pc_surface *out, pc_error *err);

/* The inverse of pc_compose_filter_mask_mix: the filtered pixels that mix with `base` through
 * `mask` to `mixed` (clamped to 0..1). Where the mask hides the filter entirely they are `base`'s,
 * which Photoshop doesn't show there. */
pc_status pc_compose_filter_mask_unmix(const pc_surface *base, const pc_surface *mixed,
                                       const pc_layer_mask *mask, pc_surface *out, pc_error *err);

#endif
