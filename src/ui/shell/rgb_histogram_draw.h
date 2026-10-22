/* Nuklear drawing for the Camera-Raw-style RGB histogram ribbon (src/ui/rgb_histogram.c model).
 * Shell layer: draws the filled crossing-split mesh plus per-channel outline polylines into the
 * current window canvas. Coordinates are clamped to Nuklear's 16-bit draw range. */
#ifndef PC_UI_SHELL_RGB_HISTOGRAM_DRAW_H
#define PC_UI_SHELL_RGB_HISTOGRAM_DRAW_H

#include "shell_internal.h"

#include "algo/histogram.h"
#include "ui/theme.h"

/* Paint histogram `h` filling `rect` using theme `t`. A NULL arg or bad rect draws nothing. */
void pc_ui_shell_rgb_histogram_paint(struct nk_context *ctx, struct nk_rect rect,
                                     const pc_algo_rgb_histogram *h, const pc_ui_tokens *t);

#endif
