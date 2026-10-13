/* Bounds-checked readers for the OpenType tables the font database needs: the table directory
 * and collection header, `name`, `OS/2`, `head`, `post`, `maxp`, `fvar`, `hhea`/`hmtx`, `cmap`
 * and the GSUB feature list. They reproduce the reference's parsers (read-fonts 0.41 through
 * skrifa 0.44 and fontique 0.11), including how they treat damaged data: tables are read lazily,
 * an array that runs past its table reads as empty, and a table whose fixed header is cut off is
 * missing. Glyph outlines come from FreeType (font.h), not from here.
 *
 * Fonts are untrusted input: every read is bounds-checked against the bytes given, no function
 * allocates more than its output, and none recurses. All functions are pure and thread-safe; a
 * pc_sfnt borrows the bytes, which must outlive it. */
#ifndef PC_TEXT_SFNT_H
#define PC_TEXT_SFNT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"

#define PC_SFNT_TAG(a, b, c, d)                                                                              \
    (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) | ((uint32_t)(uint8_t)(c) << 8) |       \
     (uint32_t)(uint8_t)(d))

/* One face of a font file (a table directory). */
typedef struct pc_sfnt {
    const uint8_t *data; /* the whole file */
    size_t len;
    size_t dir;       /* offset of the table directory */
    uint16_t ntables; /* records readable within the file (0 when the array is cut off) */
    bool sorted;      /* tags strictly ascending: binary search, else first match */
} pc_sfnt;

/* Faces in a font file: 1 for a TTF/OTF, the header's count for a TTC/OTC (0 when its offset
 * array is cut off), 0 for anything unreadable (read-fonts FileRef semantics). */
uint32_t pc_sfnt_face_count(const uint8_t *data, size_t len);
/* Opens face `index` (FontRef::from_index). False for unreadable data or a bad index. */
bool pc_sfnt_open(const uint8_t *data, size_t len, uint32_t index, pc_sfnt *out);
/* Table bytes (borrowed) by tag; false when absent, at offset 0 or past the end of the file. */
bool pc_sfnt_table(const pc_sfnt *f, uint32_t tag, const uint8_t **p, size_t *n);
bool pc_sfnt_has_table(const pc_sfnt *f, uint32_t tag);
/* Offset (in the file) of the `head` table's flags field, for the FreeType size patch in
 * font.c; false when there is no readable `head`. */
bool pc_sfnt_head_flags_offset(const pc_sfnt *f, size_t *off);

/* ---- name ---- */

typedef enum pc_sfnt_name_rule {
    /* fontique's english_or_first: language id 0x0409 wins at once, then id 0 (rank 2), then the
     * table's very first record (rank 1). */
    PC_SFNT_NAME_FONTIQUE,
    /* skrifa's LocalizedStrings::english_or_first: language "en-US" wins at once, then "en",
     * then an unknown language, then the first matching record. */
    PC_SFNT_NAME_SKRIFA
} pc_sfnt_name_rule;

/* Appends the chosen string for name id `id` (decoded: UTF-16BE or Mac Roman, unknown encodings
 * read as empty, bad surrogates as U+FFFD) to `out` as UTF-8. Returns false (out unchanged) when
 * there is no name table, no record with that id, or (fontique rule) the chosen record's string
 * lies outside the table. Under the skrifa rule an unreadable chosen string is empty (true). */
bool pc_sfnt_name(const pc_sfnt *f, uint16_t id, pc_sfnt_name_rule rule, pc_buf *out, pc_error *err);
/* Whether the face has a readable `name` table (fontique skips faces without one). */
bool pc_sfnt_has_name(const pc_sfnt *f);
/* Calls fn(ctx, utf8, len) for every record with name id `id`, in table order (skrifa's
 * localized_strings iteration). Stops early when fn returns true; returns that. */
bool pc_sfnt_each_name(const pc_sfnt *f, uint16_t id, bool (*fn)(void *ctx, const char *s, size_t len),
                       void *ctx);

/* The class of a name record's language as skrifa reads it (LocalizedString::language). */
typedef enum pc_sfnt_lang {
    PC_SFNT_LANG_NONE,    /* None: an unknown language id or an unreadable tag */
    PC_SFNT_LANG_EMPTY,   /* an empty language tag (name table version 1) */
    PC_SFNT_LANG_ENGLISH, /* primary subtag "en" */
    PC_SFNT_LANG_OTHER
} pc_sfnt_lang;

/* pc_sfnt_each_name with each record's language class. */
bool pc_sfnt_each_name_lang(const pc_sfnt *f, uint16_t id,
                            bool (*fn)(void *ctx, const char *s, size_t len, pc_sfnt_lang lang), void *ctx);

enum {
    PC_SFNT_NAME_FAMILY = 1,
    PC_SFNT_NAME_SUBFAMILY = 2,
    PC_SFNT_NAME_POSTSCRIPT = 6,
    PC_SFNT_NAME_TYPOGRAPHIC_FAMILY = 16,
    PC_SFNT_NAME_TYPOGRAPHIC_SUBFAMILY = 17
};

/* ---- font attributes (fontique read_attributes) ---- */

typedef enum pc_font_slope { PC_SLOPE_NORMAL, PC_SLOPE_ITALIC, PC_SLOPE_OBLIQUE } pc_font_slope;

typedef struct pc_font_attrs {
    float width;         /* ratio, 1.0 = normal (from usWidthClass) */
    pc_font_slope slope; /* fsSelection italic/oblique, else head.macStyle italic */
    bool has_angle;      /* oblique with a `post` italic angle */
    float angle;         /* post.italicAngle (degrees) when has_angle */
    float weight;        /* usWeightClass, else 700/400 from head.macStyle */
} pc_font_attrs;

void pc_sfnt_attrs(const pc_sfnt *f, pc_font_attrs *out);

typedef struct pc_font_axis {
    char tag[5]; /* NUL-terminated 4-byte tag */
    float min, def, max;
} pc_font_axis;

/* Variation axes from `fvar` (at most `cap` written); returns the total count, 0 without fvar. */
size_t pc_sfnt_axes(const pc_sfnt *f, pc_font_axis *out, size_t cap);

uint16_t pc_sfnt_units_per_em(const pc_sfnt *f); /* 0 when `head` is unreadable */
uint32_t pc_sfnt_num_glyphs(const pc_sfnt *f);   /* maxp.numGlyphs; 0 without maxp */
/* Unscaled advance width of glyph gid from hmtx (the last long metric past numberOfHMetrics);
 * false when gid >= numGlyphs or the tables are missing. */
bool pc_sfnt_advance(const pc_sfnt *f, uint32_t gid, int32_t *out);
/* Does GSUB's feature list contain `tag`? */
bool pc_sfnt_gsub_has_feature(const pc_sfnt *f, uint32_t tag);

/* ---- cmap ---- */

/* skrifa's Charmap: the subtable it selects (formats 4 and 12 only; symbol preferred, then
 * UCS-4, then BMP; records searched in reverse), plus the iteration limits. */
typedef struct pc_sfnt_cmap {
    const uint8_t *sub; /* subtable start; NULL when the font has no usable mapping */
    size_t len;         /* bytes from sub to the end of the cmap table */
    uint16_t format;
    bool symbol;
    uint32_t glyph_count; /* maxp.numGlyphs, or 65535 without maxp (format 12 iteration limit) */
} pc_sfnt_cmap;

void pc_sfnt_cmap_skrifa(const pc_sfnt *f, pc_sfnt_cmap *out);
/* Charmap::map: the nominal glyph, 0 for none (symbol fonts also try U+F000 + cp below 0x100). */
uint32_t pc_sfnt_cmap_map(const pc_sfnt_cmap *c, uint32_t cp);
/* Charmap::mappings: calls fn(ctx, cp, gid) for every pair with gid != 0, in the reference's
 * iteration order. Stops early when fn returns false. */
void pc_sfnt_cmap_each(const pc_sfnt_cmap *c, bool (*fn)(void *ctx, uint32_t cp, uint32_t gid), void *ctx);

/* fontique's CharmapIndex: the best subtable by HarfBuzz's preference order (formats 0, 4, 6,
 * 10, 12, 13). False when there is none, in which case fontique doesn't register the face. */
bool pc_sfnt_cmap_fontique(const pc_sfnt *f, pc_sfnt_cmap *out);
/* Lookup through a fontique subtable (any of those formats); 0 for none. */
uint32_t pc_sfnt_cmap_map_any(const pc_sfnt_cmap *c, uint32_t cp);

#endif
