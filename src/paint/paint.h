/* PhotoCraft brush engine (port of crates/paint): umbrella header.
 *
 * Photoshop-grade brushes: computed and sampled tips, Shape Dynamics, Scattering, Texture, Dual
 * Brush, Color Dynamics, Transfer, Brush Pose, Noise, Wet Edges, Build-up and Smoothing, plus
 * the dab machinery shared by the retouching tools. Strokes are data (pc_paint_stroke) so they
 * can be recorded, replayed and tested deterministically: every jitter is a hash of the brush
 * seed and the dab index (rng.h). Dabs build up with flow towards each dab's opacity ceiling
 * within one stroke, which then composites once at the stroke opacity (Opacity versus Flow).
 *
 * Map: brush.h (the model, stroke data, dab coverage), tile.h (gray tiles, mips, patterns,
 * base64), rng.h, procedural.h (generated patterns and tips), dynamics.h (points -> dabs),
 * render.h (dabs -> stroke buffer -> pixels), presets.h (built-ins), json.h (serde forms).
 * retouch.h (regions, coverage compositing, sequential dab strokes, Smudge), mixer.h (Mixer
 * Brush), replace.h (Color Replacement), bg_erase.h (Background Eraser), outline.h (tip
 * outlines for the cursor), symmetry.h (painting symmetry axes). */
#ifndef PC_PAINT_PAINT_H
#define PC_PAINT_PAINT_H

#include "paint/bg_erase.h"
#include "paint/brush.h"
#include "paint/dynamics.h"
#include "paint/json.h"
#include "paint/mixer.h"
#include "paint/outline.h"
#include "paint/presets.h"
#include "paint/procedural.h"
#include "paint/render.h"
#include "paint/replace.h"
#include "paint/retouch.h"
#include "paint/rng.h"
#include "paint/symmetry.h"
#include "paint/tile.h"

#endif
