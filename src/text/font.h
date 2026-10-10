/* pc_font: one face of a font file, with unhinted glyph outlines from FreeType.
 *
 * Outlines match skrifa 0.44's unhinted drawing (the reference's DrawSettings::unhinted with its
 * default FreeType path style): TrueType points scaled to 26.6 fixed point with a 16.16 scale of
 * ppem * 64 / unitsPerEm, implied on-curve points at the integer midpoint, contours started like
 * FT_Outline_Decompose. Unscaled outlines are in font units.
 *
 * Ownership and threads: a pc_font is reference counted (atomic) and immutable to callers. It
 * owns a private FT_Library and FT_Face, guarded by a per-font mutex, so any number of threads
 * may draw glyphs of the same or different fonts concurrently (FreeType objects are never shared
 * between fonts); pens are called without the lock held. The font retains its bytes (a
 * pc_blob) for its lifetime.
 *
 * Fonts are untrusted: FreeType parses them; every failure is a status, never a crash. */
#ifndef PC_TEXT_FONT_H
#define PC_TEXT_FONT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/blob.h"
#include "base/status.h"
#include "text/sfnt.h"

typedef struct pc_font pc_font;

/* Receives outline commands (skrifa's OutlinePen). y points up (font convention). */
typedef struct pc_outline_pen {
    void (*move_to)(void *ctx, float x, float y);
    void (*line_to)(void *ctx, float x, float y);
    void (*quad_to)(void *ctx, float cx, float cy, float x, float y);
    void (*curve_to)(void *ctx, float c0x, float c0y, float c1x, float c1y, float x, float y);
    void (*close)(void *ctx);
    void *ctx;
} pc_outline_pen;

/* Opens face `index` of `bytes` (retained). PC_ERR_FORMAT when FreeType or the table directory
 * reader can't open it. */
pc_status pc_font_new(pc_blob *bytes, uint32_t index, pc_font **out, pc_error *err);
pc_font *pc_font_retain(pc_font *f); /* NULL-safe; returns f */
void pc_font_release(pc_font *f);    /* NULL-safe */

/* The whole file's bytes (borrowed, valid while the font lives) and the face index. */
const uint8_t *pc_font_data(const pc_font *f, size_t *len);
pc_blob *pc_font_blob(const pc_font *f); /* borrowed */
uint32_t pc_font_index(const pc_font *f);
/* A process-unique id (cache keys; never reused while the process runs). */
uint64_t pc_font_id(const pc_font *f);
/* The face's table directory (always readable for an open font). */
void pc_font_sfnt(const pc_font *f, pc_sfnt *out);
uint16_t pc_font_units_per_em(const pc_font *f);
uint32_t pc_font_num_glyphs(const pc_font *f);

/* Draws glyph `gid` unhinted. `size_px` > 0 scales to that many pixels per em; NULL draws in font
 * units. `coords` are normalised variation coordinates (F2Dot14 bits, ncoords may be 0).
 * The size goes through skrifa's arithmetic, (size * 64) saturated to 32 bits, so absurd or
 * non-finite sizes give degenerate (but bounded) coordinates rather than an error.
 * PC_ERR_NOT_FOUND when the face has no outline for gid (skrifa's outline_glyphs().get is None),
 * PC_ERR_FORMAT for a glyph
 * FreeType can't load (nothing has been drawn then), PC_ERR_INVALID for bad arguments. An
 * empty glyph (a space) draws nothing and succeeds. */
pc_status pc_font_draw(pc_font *f, uint32_t gid, const float *size_px, const int16_t *coords, size_t ncoords,
                       const pc_outline_pen *pen, pc_error *err);

/* Unscaled advance width (font units) at the default location, or with `coords` through
 * FreeType's variation tables. False when gid is out of range. */
bool pc_font_advance(pc_font *f, uint32_t gid, const int16_t *coords, size_t ncoords, float *out);

#endif
