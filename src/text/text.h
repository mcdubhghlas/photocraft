/* PhotoCraft's type engine (port of crates/text): umbrella header.
 *
 *  - fonts.h        font database: bundled fonts, craft fonts, system scan, matching, PostScript
 *                   names, charmaps (fonts.rs, glyphs.rs charmap)
 *  - font.h         one face with FreeType outlines drawn like skrifa (pc_font)
 *  - sfnt.h         OpenType table readers (name, OS/2, cmap, fvar, GSUB features, ...)
 *  - cjk.h          CJK fallback families, locale script order, CJK font files (cjk.rs)
 *  - craft_fonts.h  the optional craft-fonts checkout (craft_fonts.rs, build.rs)
 *  - glyphs.h       Glyphs panel categories and previews (glyphs.rs)
 *  - optical.h      optical kerning (optical.rs)
 *  - raster.h       anti-aliased coverage rasteriser (raster.rs)
 *  - layout_data.h  the laid-out result (layout.rs data types)
 *  - render.h       rasterising a layout and extracting outlines (render.rs)
 *  - unicode.h      Rust char properties used by the above
 *
 * Chunk L14 adds the layouter (layout.h: shaping with HarfBuzz, FriBidi, libunibreak, CJK and
 * vertical type, warp, spell) and the text engine (lib.rs TextEngine: layout + render of a
 * pc_text_layer); L15 adds PSD type layers and EngineData. */
#ifndef PC_TEXT_TEXT_H
#define PC_TEXT_TEXT_H

#include "text/cjk.h"
#include "text/craft_fonts.h"
#include "text/engine.h"
#include "text/font.h"
#include "text/fonts.h"
#include "text/glyphs.h"
#include "text/layout.h"
#include "text/layout_data.h"
#include "text/optical.h"
#include "text/psd.h"
#include "text/raster.h"
#include "text/render.h"
#include "text/sfnt.h"
#include "text/spell.h"
#include "text/unicode.h"
#include "text/warp.h"

#endif
