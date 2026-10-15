/* ADJ-parameter machinery for the per-pixel adjustment kinds, shared by the destructive
 * `image.adjustments.*` commands, `layer.newAdjustmentLayer.*` and `layer.setAdjustment`
 * (port of crates/engine/src/adjust_params.rs + the selective/lookup readers of adjust_cmds.rs).
 *
 * from_params reads the adjustment of a kind over a base (missing keys keep the base's value, so a
 * partial update changes only what it names); to_params writes the complete set back, so
 * from_params(kind, to_params(a), NULL, mode) == a. Values out of range clamp like Photoshop's
 * fields; a key of the wrong type is PC_ERR_INVALID (BadParams). Units: Levels/Curves in 0..255
 * levels, percentages for the rest; colours "#rrggbb" or [r, g, b] in 0..1.
 *
 * E8-adjust note: the Levels/Curves eyedropper (Set Black/Neutral Gray/White Point) is NOT ported
 * here; it needs the merged-composite sampler (E10 sample_cmds) for the `at` path. The commands
 * report the `eyedropper` param as unsupported in this build. */
#ifndef PC_ENGINE_ADJUST_PARAMS_H
#define PC_ENGINE_ADJUST_PARAMS_H

#include <yyjson.h>

#include "color/color.h"
#include "doc/adjust.h"
#include "engine.h"

/* Read the adjustment of `kind` from `p` over `base` (NULL or another kind = the kind's defaults).
 * `mode` picks the Levels/Curves channel space when neither params nor base name one. *out is
 * written only on success (caller frees with pc_adjustment_fini). PC_ERR_INVALID for a bad key or
 * an unknown kind. */
pc_status pc_engine_adjustment_from_params(const char *kind, yyjson_val *p, const pc_adjustment *base,
                                           pc_color_mode mode, pc_adjustment *out, pc_error *err);

/* The neutral adjustment of `kind` (what "Reset to defaults" restores). */
pc_status pc_engine_adjustment_default_for(const char *kind, pc_color_mode mode, pc_adjustment *out,
                                           pc_error *err);

/* The complete parameter set of `adj`, built into `d` (the inverse of from_params). *out is a
 * mutable value owned by `d`. */
pc_status pc_engine_adjustment_to_params(const pc_adjustment *adj, yyjson_mut_doc *d,
                                         yyjson_mut_val **out, pc_error *err);

/* The id of an adjustment kind ("hueSaturation", "levels", ...; "invert" for Invert/Unsupported and
 * anything else). Static string (commands::adjustment_kind). */
const char *pc_engine_adjustment_kind(const pc_adjustment *a);

/* Whether `p` has no user keys (every key is a private one: layer/coalesce/channel/targetChannel or
 * starts with "__"). A no-parameter layer.setAdjustment resets to defaults (adjust_params::user_keys). */
bool pc_engine_adjustment_user_keys_empty(yyjson_val *p);

/* Selective Color / Color Lookup from params over an optional base (adjust_cmds.rs). Exposed because
 * from_params routes to them and the setAdjustment "merge" path reuses them. */
pc_status pc_engine_selective_from_params(yyjson_val *p, const pc_adjustment *base, pc_adjustment *out,
                                          pc_error *err);
pc_status pc_engine_lookup_from_params(yyjson_val *p, const pc_adjustment *base, pc_adjustment *out,
                                       pc_error *err);

#endif /* PC_ENGINE_ADJUST_PARAMS_H */
