/* Color Range selection-preview overlay core (headless; no GUI). Port of overlay_pixel in
 * crates/ui-egui/src/color_range_ui.rs. Modes: none, grayscale, blackMatte, whiteMatte, quickMask.
 * White = selected, black = unselected, grey = partial; mattes and Quick Mask cover unselected
 * pixels. The on-canvas rendering that consumes this lives in the canvas layer. */
#ifndef PC_UI_COLOR_RANGE_PREVIEW_H
#define PC_UI_COLOR_RANGE_PREVIEW_H

#include <stddef.h>
#include <stdint.h>

/* The five selection-preview modes, in menu order (serde ids and labels). */
enum { PC_UI_COLOR_RANGE_PREVIEW_MODES = 5 };
extern const char *const PC_UI_COLOR_RANGE_PREVIEW_IDS[PC_UI_COLOR_RANGE_PREVIEW_MODES];
extern const char *const PC_UI_COLOR_RANGE_PREVIEW_LABELS[PC_UI_COLOR_RANGE_PREVIEW_MODES];

/* Index of `id` among the modes, or 0 ("none") for NULL/unknown. */
size_t pc_ui_color_range_preview_index(const char *id);

/* Unmultiplied RGBA for `mode` at `coverage` (0..1, selected = 1). NULL/"none"/unknown → fully
 * transparent (the canvas shows the image unchanged). NULL rgba is a no-op. */
void pc_ui_color_range_overlay_pixel(const char *mode, double coverage, uint8_t rgba[4]);

#endif
