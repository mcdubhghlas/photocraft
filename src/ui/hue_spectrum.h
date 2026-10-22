/* Hue/Saturation editor colour cores (headless; no GUI). Port of the pure helpers in
 * crates/ui-egui/src/adjust_editors.rs (hue_color and the Colorize saturation nicety). */
#ifndef PC_UI_HUE_SPECTRUM_H
#define PC_UI_HUE_SPECTRUM_H

/* Fully-saturated hue (HSV h, 1, 1) as display RGB in 0..1. `deg` wraps modulo 360. */
void pc_ui_hue_color(double deg, float rgb[3]);

/* Saturation after switching Colorize on: at least 25 (Photoshop nudges a flat image to a visible
 * tint). Returns max(current, 25). */
double pc_ui_hue_colorize_saturation(double current);

#include <stddef.h>
#include <stdint.h>

/* Fill `n` columns of a one-row straight-alpha RGBA8 strip (4 * n bytes) with the full-saturation
 * hue spectrum: column i is HSV(h = i / n * 360, 1, 1), opaque. This is the INPUT spectrum; the
 * live "adjusted" spectrum under a Hue/Saturation editor is this strip run through
 * pc_compose_adjust_apply_rgba8. NULL/0 is a no-op. Never reads out of bounds. */
void pc_ui_hue_spectrum_fill(uint8_t *rgba, size_t n);

#endif
