#ifndef PC_UI_PRESET_SWATCH_H
#define PC_UI_PRESET_SWATCH_H
/* Pure helpers for the Gradients/Patterns preset panels (U5): resolving gradient
 * stop colours and sampling a gradient bar. No UI or engine dependencies, so the
 * colour maths is unit-testable headlessly. */
#include <stdbool.h>
#include <stddef.h>

/* Resolve a gradient stop colour token to linear-ish [0,1] RGB in out_rgb.
 * Accepts "foreground" / "background" (copied from fg/bg, which may be NULL only
 * if the token is a literal colour), "#rrggbb" or "rrggbb" (6 hex digits).
 * Returns false (leaving out_rgb untouched) for a NULL/empty/malformed token. */
bool pc_ui_preset_stop_color(const char *token, const float fg[4], const float bg[4], float out_rgb[3]);

/* Piecewise-linear sample of a gradient at position t in [0,1]. locs[i] is the
 * stop location and rgb[i] its colour; the array need not be sorted. Ends are
 * clamped (t below the first stop -> first colour, above the last -> last). A
 * zero-width span between two equal locations takes the later colour. Does
 * nothing (leaves out_rgb untouched) when n == 0 or any pointer is NULL. */
void pc_ui_preset_gradient_sample(const float *locs, const float (*rgb)[3], size_t n, float t,
                                  float out_rgb[3]);

#endif
