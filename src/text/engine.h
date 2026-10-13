/* The text engine (port of crates/text/src/lib.rs TextEngine and glyphs::preview): a font
 * database plus a layouter, created once and reused (font loading and shaping caches). The
 * usual entry point is pc_text_engine_render_layer, which refreshes a pc_text_layer's cache from
 * its style model.
 *
 * Threads: an engine is single-owner and not synchronised; pc_text_engine_shared_lock hands out
 * the process-wide engine under a mutex. */
#ifndef PC_TEXT_ENGINE_H
#define PC_TEXT_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "color/color.h"
#include "doc/text.h"
#include "text/fonts.h"
#include "text/layout.h"
#include "text/render.h"

typedef struct pc_text_engine pc_text_engine;

/* Bundled (and craft) fonts only: deterministic output (tests). */
pc_status pc_text_engine_new(pc_text_engine **out, pc_error *err);
/* Bundled plus installed system fonts (desktop). */
pc_status pc_text_engine_with_system_fonts(pc_text_engine **out, pc_error *err);
void pc_text_engine_free(pc_text_engine *e); /* NULL-safe */

/* The engine's font database (borrowed; register fonts through it). */
pc_fontdb *pc_text_engine_fonts(pc_text_engine *e);

/* Lays out a text layer (text space: pixels, before the layer transform). See
 * pc_text_layouter_layout for the failure modes. */
pc_status pc_text_engine_layout(pc_text_engine *e, const pc_text_layer *layer, float dpi, pc_text_layout *out,
                                pc_error *err);

/* Kerning (1/1000 em) between the character starting at byte `at` and the next one, as laid
 * out: the manual kern, or the automatic (metrics / optical) kerning of the pair. *has is false
 * when there is no such pair on one line (out untouched then). */
pc_status pc_text_engine_pair_kerning(pc_text_engine *e, const pc_text_layer *layer, float dpi, size_t at,
                                      bool *has, float *kerning, pc_error *err);

/* Lays out and rasterises a text layer into document space (through layer->transform, bent by
 * layer->warp). `layout` may be NULL; otherwise it receives the layout (pc_text_layout_fini).
 * `out` is released with pc_rendered_fini. */
pc_status pc_text_engine_render(pc_text_engine *e, const pc_text_layer *layer, float dpi,
                                pc_pixel_format format, pc_text_layout *layout, pc_rendered *out,
                                pc_error *err);

/* Re-renders layer->cache from its model; *rect receives the document-space rectangle drawn. On
 * failure the layer is unchanged. */
pc_status pc_text_engine_render_layer(pc_text_engine *e, pc_text_layer *layer, float dpi,
                                      pc_pixel_format format, pc_rect *rect, pc_error *err);

/* An 8-bit coverage preview of character c in a px-pixel em box (glyphs.rs preview): the style's
 * family and weights at px * 0.72 pt, without decorations, tracking or baseline shift. *alpha
 * (free()) holds *out_px squared bytes. */
pc_status pc_text_glyph_preview(pc_text_engine *e, const pc_char_style *style, uint32_t c, uint32_t px,
                                uint32_t *out_px, uint8_t **alpha, pc_error *err);

/* The process-wide engine (bundled fonts plus system fonts unless PHOTOCRAFT_NO_SYSTEM_FONTS is
 * set), created on first use and locked until pc_text_engine_shared_unlock. Fails (nothing
 * locked) when the engine can't be created. */
pc_status pc_text_engine_shared_lock(pc_text_engine **out, pc_error *err);
void pc_text_engine_shared_unlock(void);
/* The shared engine without blocking on the system font scan: the first call starts building it
 * on a background thread (unless PHOTOCRAFT_NO_SYSTEM_FONTS is set, when it is cheap and built
 * here) and returns with *ready false until that finished; once ready it behaves like
 * pc_text_engine_shared_lock (unlock afterwards). Code that runs every UI frame uses this. */
pc_status pc_text_engine_shared_try_lock(pc_text_engine **out, bool *ready, pc_error *err);
/* Starts building the shared engine in the background (call once at startup; later calls do
 * nothing). */
void pc_text_engine_warm_shared(void);

#endif
