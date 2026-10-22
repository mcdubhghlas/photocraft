/* Levels editor numeric cores (headless; no GUI). Port of the pure helpers in
 * crates/ui-egui/src/adjust_editors.rs (auto_levels, gamma_pos, gamma_from_pos, nearest). */
#ifndef PC_UI_LEVEL_AUTO_H
#define PC_UI_LEVEL_AUTO_H

#include <stddef.h>
#include <stdint.h>

/* Photoshop's Auto for Levels: input black/white that clip 0.1% of the pixels at each end of a
 * 256-bin histogram. Writes *lo (0..253) and *hi (lo+2..255). NULL-safe; a flat/empty histogram
 * yields (253, 255), matching the reference's edge behaviour. */
void pc_ui_auto_levels(const uint32_t hist[256], double *lo, double *hi);

/* Input level (0..255) where the gamma handle sits: the input that maps to 50% grey. */
double pc_ui_levels_gamma_pos(double black, double white, double gamma);

/* Gamma implied by a handle dropped at input `x` between `black` and `white` (clamped 0.01..9.99). */
double pc_ui_levels_gamma_from_pos(double black, double white, double x);

/* Index of the nearest of the `n` positions in `xs` to `x`, or SIZE_MAX when n == 0 or xs is NULL. */
size_t pc_ui_slider_nearest(const double *xs, size_t n, double x);

#endif
