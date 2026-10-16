/* The PhotoCraft brand mark (the kitsune app icon) shown at the left of the title bar, port of the
 * decode half of crates/ui-egui/src/brand.rs. The texture upload + title-bar painting are
 * reimplemented by the window-chrome chunk (U3); U1 owns decoding the embedded PNG to RGBA8 so the
 * mark can be uploaded once. No GUI dependency (decoding is pure). */
#ifndef PC_UI_BRAND_H
#define PC_UI_BRAND_H

#include <stddef.h>
#include <stdint.h>

#include "base/status.h"

/* The embedded 128x128 app-icon PNG (assets/app-icon/.../ai.storyteller.photocraft.png). */
extern const unsigned char pc_ui_app_icon_png[];
extern const size_t pc_ui_app_icon_png_len;

/* Decode the app icon to unmultiplied RGBA8. On success *out is a malloc'd w*h*4 buffer (caller
 * frees) and the width and height are written to *w and *h. On failure *out is untouched. */
pc_status pc_ui_brand_icon_rgba(uint8_t **out, uint32_t *w, uint32_t *h, pc_error *err);

#endif
