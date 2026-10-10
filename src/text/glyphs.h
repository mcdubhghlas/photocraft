/* Glyphs panel support (port of crates/text/src/glyphs.rs): Photoshop's "Show" categories and
 * small glyph previews. The characters a face maps are pc_fontdb_charmap (fonts.h).
 *
 * preview() in the reference lays out one character first; that half needs the layouter
 * (chunk L14). Here is the half after layout: pc_glyph_preview_from_layout takes the layout of
 * the one-character layer that the reference builds (family/style from the caller, size
 * px * 0.72 pt at 72 dpi, no underline/strikethrough/tracking/baseline shift) and draws it.
 * Pure functions; thread-safe. */
#ifndef PC_TEXT_GLYPHS_H
#define PC_TEXT_GLYPHS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "text/layout_data.h"

/* The Glyphs panel's "Show" categories, in menu order. */
extern const char *const PC_GLYPH_CATEGORIES[14];

/* Whether character c belongs to a category (unknown names match everything). */
bool pc_glyph_in_category(uint32_t c, const char *category);

/* The preview box size for a requested px (clamped to 4..512). */
uint32_t pc_glyph_preview_size(uint32_t px);

/* An 8-bit coverage preview of the laid-out character in a px-pixel em box (px clamped to
 * 4..512), centred horizontally on the layout's bounds, baseline at 78 % of the box:
 * *out_px = px and *alpha (free()) holds px * px coverage bytes. */
pc_status pc_glyph_preview_from_layout(const pc_text_layout *layout, uint32_t px, uint32_t *out_px,
                                       uint8_t **alpha, pc_error *err);

#endif
