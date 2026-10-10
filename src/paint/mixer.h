/* Mixer Brush (port of crates/paint/src/mixer.rs): a reservoir of loaded paint plus a pickup
 * well that mixes in canvas colour.
 *
 * Model (behavioural, the reference's own): each dab samples the canvas colour under its
 * footprint. The pickup colour P moves towards it by `wet`. The deposited colour mixes the
 * reservoir R (weight amount x (1 - mix x wet)) with the pickup (weight mix x wet); it lands at
 * flow times the total weight. The reservoir amount drains per dab unless load is 100 %, so low
 * Load strokes dry out. A dry brush (wet = 0) paints only reservoir colour; an empty, wet brush
 * smears canvas colour. The settings type, pc_paint_mixer_settings, is in brush.h (a brush
 * carries one).
 *
 * Thread safety: single-owner state; the target must not be in use elsewhere. */
#ifndef PC_PAINT_MIXER_H
#define PC_PAINT_MIXER_H

#include <stdbool.h>

#include "base/status.h"
#include "geom/geom.h"
#include "paint/brush.h"
#include "raster/surface.h"

/* Fraction of the way the pickup colour moves towards the canvas colour per dab at 100 % Wet. */
#define PC_PAINT_PICKUP_RATE 0.25f

/* Paint carried between strokes (session tool state). Zero-initialised = the default (clean,
 * unloaded, amount 0). */
typedef struct pc_paint_mixer_state {
    bool has_reservoir;
    float reservoir[4]; /* loaded paint, straight RGBA */
    float amount;       /* remaining reservoir amount 0..1 */
    bool has_pickup;
    float pickup[4]; /* colour picked up from the canvas, straight RGBA */
} pc_paint_mixer_state;

void pc_paint_mixer_state_default(pc_paint_mixer_state *s);
bool pc_paint_mixer_state_eq(const pc_paint_mixer_state *a, const pc_paint_mixer_state *b);
/* Load the reservoir with a colour (full). */
void pc_paint_mixer_state_load(pc_paint_mixer_state *s, const float color[4]);
/* Clean the brush (drop the picked-up colour). */
void pc_paint_mixer_state_clean(pc_paint_mixer_state *s);

/* Paint a Mixer Brush stroke. `sample` is the surface colour is picked up from (NULL = the target
 * itself; a flattened composite for Sample All Layers). The brush opacity is ignored (flow comes
 * from m). *damage = the stroke bounds. On failure state is unchanged and target is as for
 * pc_paint_apply_dab_stroke. */
pc_status pc_paint_apply_mixer_stroke(pc_surface *target, const pc_surface *sample,
                                      const pc_paint_stroke *stroke, const pc_paint_mixer_settings *m,
                                      pc_paint_mixer_state *state, const pc_surface *selection,
                                      bool lock_transparency, pc_rect *damage, pc_error *err);

#endif
