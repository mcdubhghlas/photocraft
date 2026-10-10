/* Background Eraser (port of crates/paint/src/bg_erase.rs): along a stroke, sample the colour
 * under the brush hotspot and erase pixels of similar colour inside the brush to transparency.
 *
 * Colours are compared in the surface's own colour channels (RGB, CMYK, Gray, Lab... at any
 * depth), so nothing is converted through RGB; only alpha changes.
 *
 * Thread safety: the target must not be in use elsewhere. */
#ifndef PC_PAINT_BG_ERASE_H
#define PC_PAINT_BG_ERASE_H

#include <stdbool.h>

#include "base/status.h"
#include "geom/geom.h"
#include "paint/brush.h"
#include "paint/replace.h"
#include "raster/surface.h"

typedef struct pc_paint_bg_erase_settings {
    pc_paint_sampling sampling;
    pc_paint_limits limits;
    float tolerance;         /* 0..1: largest per-channel difference from the sampled colour that is erased */
    bool protect_foreground; /* keep pixels that match the foreground colour */
    float foreground[4];     /* straight RGBA (Protect Foreground Color) */
    float background[4];     /* straight RGBA swatch (PC_PAINT_SAMPLING_BACKGROUND_SWATCH) */
} pc_paint_bg_erase_settings;

void pc_paint_bg_erase_settings_default(pc_paint_bg_erase_settings *s);
bool pc_paint_bg_erase_settings_eq(const pc_paint_bg_erase_settings *a, const pc_paint_bg_erase_settings *b);

/* Erase along a stroke. The brush opacity is ignored (Photoshop's Background Eraser has none);
 * the selection limits the effect. *damage = the stroke bounds; empty when the surface has no
 * alpha channel (nothing changes). A non-finite tolerance acts as 0. On failure target is as
 * for pc_paint_apply_dab_stroke. */
pc_status pc_paint_apply_background_eraser(pc_surface *target, const pc_paint_stroke *stroke,
                                           const pc_paint_bg_erase_settings *bs, const pc_surface *selection,
                                           pc_rect *damage, pc_error *err);

#endif
