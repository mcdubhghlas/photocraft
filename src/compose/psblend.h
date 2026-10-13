/* Photoshop-exact blend functions where they differ from the generic reference in color/blend.h,
 * plus the straight-alpha compositing formula (port of crates/compose/src/psblend.rs).
 *
 * Derived from Photoshop's rendered composites (psd-tools blend-modes files):
 *   - Vivid Light: the source extremes win (cs = 0 gives 0 even over white, cs = 1 gives 1 even
 *     over black).
 *   - Hard Mix: the thresholded *generic* vivid light (backdrop extremes win).
 *   - Color Burn / Dodge (and Hard Mix): backdrop values within PC_PSBLEND_EDGE of 0 / 1 count as
 *     exact, so a value a rounding step off doesn't flip the corner case by 255 levels.
 *
 * Per-thread document flags (the reference's thread_local LAB_MIX / HDR): while LAB_MIX is set
 * (rendering a Lab document) Normal blending mixes in CIELAB; while HDR is set (a 32-bit float
 * document) Linear Dodge (Add) and Divide don't clip at 1 and Hue / Saturation / Color /
 * Luminosity keep values above 1. The compositor sets them on every thread that renders a tile and
 * restores the values it found afterwards. The text gamma is process-wide.
 *
 * Pure functions apart from those flags; thread-safe. `out` may alias any input. */
#ifndef PC_COMPOSE_PSBLEND_H
#define PC_COMPOSE_PSBLEND_H

#include <stdbool.h>

#include "color/blend.h"

#define PC_PSBLEND_EDGE 1e-4f
/* Photoshop's default "Blend Text Colors Using Gamma". */
#define PC_PSBLEND_TEXT_GAMMA 1.45f

/* This thread's Lab-mix / HDR flags. The setters return the previous value, so a scope can
 * restore what it found (with_lab_mix / with_hdr: a nested parallel job can run another tile on
 * this thread mid-composite, and clearing the flags there broke the outer tile, #1112). */
bool pc_psblend_set_lab_mix(bool on);
bool pc_psblend_lab_mix(void);
bool pc_psblend_set_hdr(bool on);
bool pc_psblend_hdr(void);

/* B(Cb, Cs) with Photoshop's variants (and the HDR flag). */
void pc_psblend_rgb(pc_blend_mode mode, const float cb[3], const float cs[3], float out[3]);
/* Photoshop's "special eight" (Color Burn, Linear Burn, Color Dodge, Linear Dodge, Vivid Light,
 * Linear Light, Hard Mix, Difference): the modes for which a layer's Fill isn't just less
 * coverage. Below 100 % Fill the blend runs at full strength on a source pulled toward the mode's
 * neutral colour (Hard Mix has its own formula); Opacity still mixes the result in. */
bool pc_psblend_fill_is_special(pc_blend_mode mode);
/* pc_psblend_rgb for a layer at fill (0..1): the special eight change their blend below 100 %. */
void pc_psblend_rgb_fill(pc_blend_mode mode, const float cb[3], const float cs[3], float fill, float out[3]);
/* pc_psblend_composite_gamma for a layer with separate opacity and fill: Normal modes take both
 * as coverage; for the special eight below 100 % Fill, coverage over the backdrop is opacity and
 * the blend is pc_psblend_rgb_fill, while over transparency Fill stays coverage. */
void pc_psblend_composite_fill(pc_blend_mode mode, const float backdrop[4], const float source[4],
                               float opacity, float fill, float gamma, float out[4]);
/* Straight-alpha source over straight-alpha backdrop (W3C/PDF general formula) with
 * pc_psblend_rgb; returns the backdrop unchanged when the source alpha times opacity is <= 0. */
void pc_psblend_composite(pc_blend_mode mode, const float backdrop[4], const float source[4], float opacity,
                          float out[4]);
/* pc_psblend_composite with the coverage mix done in the text blending space of gamma
 * (gamma == 1 is pc_psblend_composite). */
void pc_psblend_composite_gamma(pc_blend_mode mode, const float backdrop[4], const float source[4],
                                float opacity, float gamma, float out[4]);

/* Color Settings > "Blend Text Colors Using Gamma" (1 = off), clamped to 1.00 - 2.20; non-finite
 * values restore the default. Process-wide (atomic). */
void pc_psblend_set_text_gamma(float gamma);
float pc_psblend_text_gamma(void);
/* Into / out of the text blending space: linear light raised to 1 / gamma. */
float pc_psblend_text_encode(float v, float gamma);
float pc_psblend_text_decode(float v, float gamma);

#endif
