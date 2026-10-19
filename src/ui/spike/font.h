/* A FreeType-rasterised glyph atlas exposed to Nuklear as an nk_user_font.
 *
 * Glyphs are rendered at size * scale pixels (scale = framebuffer pixels per UI unit, so text
 * stays sharp at HiDPI) and reported to Nuklear in UI units. The atlas is RGBA: white with
 * coverage in alpha, plus a solid white texel for untextured shapes (Nuklear's null texture),
 * so one texture serves both text and shapes.
 *
 * Glyphs are added on demand to a bounded atlas. The engine font database supplies installed
 * and optional craft-font fallbacks. Whole-string HarfBuzz/FriBidi shaping shares the atlas through a bounded
 * text cache. Not thread-safe; used from the UI thread only. */
#ifndef PC_UI_FONT_H
#define PC_UI_FONT_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"
#include "photocraft_nuklear.h"

typedef struct ui_glyph {
    uint32_t cp;
    bool missing;
    float u0, v0, u1, v1;    /* atlas texture coordinates */
    float w, h, ox, oy, adv; /* UI units */
} ui_glyph;

typedef struct ui_font {
    struct nk_user_font nk; /* hand &font->nk to Nuklear; set nk.texture after uploading */
    float size;             /* UI units */
    float scale;            /* pixels per UI unit */
    float ascent;           /* UI units */
    ui_glyph *glyphs;       /* owned, sorted by cp */
    size_t nglyphs;
    uint8_t *atlas; /* owned RGBA, atlas_w * atlas_h * 4 */
    int atlas_w, atlas_h;
    struct nk_vec2 white_uv; /* a fully white texel */
    struct ui_font_runtime *runtime;
    bool dirty;
    int dirty_top, dirty_bottom;
    pc_buf file; /* the font bytes (FreeType reads them in place) */
} ui_font;

/* Load the font file at `path` and build the atlas. On failure *f is left empty (safe to fini). */
pc_status ui_font_init(ui_font *f, const char *path, float size, float scale, pc_error *err);

/* Resolve/cache a Unicode scalar. False when only the replacement glyph is available. */
bool ui_font_has_glyph(ui_font *font, uint32_t codepoint);

typedef struct ui_font_shape_info {
    size_t glyphs, runs, missing;
    float width;
} ui_font_shape_info;
/* Query whole-string shaping; false for the simple rune path, invalid input or allocation failure. */
bool ui_font_shape_metrics(ui_font *font, const char *text, int length, ui_font_shape_info *out);

/* Free everything. f may be NULL or already finalised. */
void ui_font_fini(ui_font *f);

#endif
