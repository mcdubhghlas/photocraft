/* SVG text internals (chunk I5b): the port of what usvg 0.45.1 does with <text> through fontdb 0.23,
 * rustybuzz 0.20 and ttf-parser 0.25, private to src/io/svg_text*.c and svg_font*.c.
 *
 *   svg_text_tree.c  svgtree text preprocessing (parser/svgtree/text.rs): tspan/tref/textPath
 *                    children, xml:space and white-space trimming; the CSS `font` shorthand and
 *                    font-family lists (svgtypes font.rs)
 *   svg_fontcfg.c    the fontconfig files fontdb reads on Linux (fontconfig-parser 0.5: <dir>,
 *                    <include>, <reset-dirs>, <alias>) for the font directories and the generic
 *                    families
 *   svg_fontdb.c     fontdb::Database: faces found by scanning those directories (plus the bundled
 *                    fonts), CSS matching (`query`), the fallback scan, face bytes on demand
 *   svg_font.c       ttf-parser's face metrics (load_font), glyf outlines in float, cmap lookup,
 *                    and shaping (unicode-bidi runs + HarfBuzz standing in for rustybuzz)
 *   svg_text.c       parser/text.rs: chunks, spans, positions, decorations, textPath
 *   svg_text_shape.c  text/layout.rs, part 1: font selection and fallback, shaping, the cluster
 *                    merge of process_chunk, glyph clusters
 *   svg_text_layout.c text/layout.rs, part 2: spacing, anchors, baselines, text on a path,
 *                    decorations, layout_text
 *   svg_text_flat.c  text/flatten.rs and text/mod.rs: outlines as paths, bounding boxes
 *
 * Fonts are untrusted: every table read is bounds-checked. The font database is process-wide,
 * built once (pthread_once) when a drawing first needs it, read-only afterwards except for the
 * per-face byte cache, which is guarded by a mutex; everything else a conversion allocates lives in
 * its sv_arena or is freed before sv_convert_text returns. */
#ifndef PC_IO_SVG_TEXT_H
#define PC_IO_SVG_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "io/svg_internal.h"

/* ---------------------------------------------------------------- caps (never-crash budgets) */

/* Characters of one <text> element; a larger element is dropped (the reference lays it out). */
#define SVT_MAX_CHARS ((size_t)1 << 18)
/* Glyphs laid out per import; text past it is dropped. */
#define SVT_MAX_GLYPHS ((size_t)1 << 20)
/* Work units (glyph visits) the cluster merge of process_chunk may spend per import. */
#define SVT_MAX_WORK ((size_t)1 << 28)
/* Font files scanned for the database, and the largest one read. */
#define SVF_MAX_FILES 100000u
#define SVF_MAX_FILE_BYTES ((size_t)512 << 20)

/* ---------------------------------------------------------------- fonts (svgtypes / usvg Font) */

typedef enum { SVF_NAMED, SVF_SERIF, SVF_SANS, SVF_CURSIVE, SVF_FANTASY, SVF_MONO } svf_kind;
typedef enum { SVF_STYLE_NORMAL, SVF_STYLE_ITALIC, SVF_STYLE_OBLIQUE } svf_style;

typedef struct svf_family {
    svf_kind kind;
    const char *name; /* SVF_NAMED */
} svf_family;

/* usvg::Font (compared by value: the layout's font cache key). Stretch 1..9 as fontdb::Stretch. */
typedef struct svf_font {
    svf_family *fams;
    size_t nfams;
    svf_style style;
    uint8_t stretch;
    uint16_t weight;
} svf_font;

bool svf_font_eq(const svf_font *a, const svf_font *b);

/* svgtypes::parse_font_families; false on a syntax error (the caller then uses the default). */
bool svf_parse_families(sv_arena *a, const char *s, svf_family **out, size_t *n);

/* ---------------------------------------------------------------- font database (fontdb) */

typedef struct svf_face {
    size_t src;     /* source (file or bundled data) */
    uint32_t index; /* face index within it */
    char **families;
    size_t nfamilies; /* >= 1; the English (US) one first */
    svf_style style;
    uint16_t weight;
    uint8_t stretch; /* 1..9 */
    bool monospaced;
} svf_face;

typedef struct svf_db svf_db;

/* The process-wide database: system fonts (unless PHOTOCRAFT_NO_SYSTEM_FONTS is set), then the
 * bundled fonts. Built on first use; NULL only when that ran out of memory. Thread-safe. */
const svf_db *svf_db_get(void);
size_t svf_db_len(const svf_db *db);
const svf_face *svf_db_face(const svf_db *db, size_t id);
/* Database::family_name for a generic family (fontconfig aliases or fontdb's defaults). */
const char *svf_db_generic(const svf_db *db, svf_kind kind);
/* Database::query over families (fontdb::Query); SIZE_MAX when nothing matches. */
size_t svf_db_query(const svf_db *db, const svf_family *fams, size_t nfams, uint16_t weight, uint8_t stretch,
                    svf_style style);
/* The whole font file of face id (borrowed for the process lifetime) and the face index; false
 * when it can't be read. */
bool svf_db_data(const svf_db *db, size_t id, const uint8_t **data, size_t *len, uint32_t *index);
/* A HarfBuzz face for face id (hb_face_t *, owned by the database), NULL when unreadable. */
void *svf_db_hb_face(const svf_db *db, size_t id);

/* fontconfig (svg_fontcfg.c): font directories in order and the generic aliases (NULL when a
 * family has no alias). Returns false when no configuration names a directory (fontdb then
 * scans its fixed list). Strings are malloc'd; free with svf_fc_fini. */
typedef struct svf_fc {
    char **dirs;
    size_t ndirs;
    char *serif, *sans, *cursive, *fantasy, *mono;
} svf_fc;

bool svf_fc_load(svf_fc *out);
void svf_fc_fini(svf_fc *fc);

/* ---------------------------------------------------------------- resolved fonts (svg_font.c) */

/* layout.rs ResolvedFont plus what shaping and outlining need. */
typedef struct svt_rfont {
    size_t id;
    uint16_t upem;
    int16_t ascent, descent;
    uint16_t x_height;
    int16_t underline_pos;
    uint16_t underline_thick;
    int16_t line_through_pos;
    int16_t sub_off, super_off;
    const uint8_t *data; /* the file (borrowed from the database) */
    size_t len;
    uint32_t index;
    void *hb_font; /* hb_font_t *, destroyed by svt_rfont_fini */
    void *ft;      /* pc_font * for CFF outlines, opened on demand (svt_rfont_fini) */
    bool glyf;     /* TrueType outlines (else CFF through FreeType) */
} svt_rfont;

/* DatabaseExt::load_font; false when the face can't be parsed (ttf_parser::Face::parse). */
bool svt_rfont_load(const svf_db *db, size_t id, svt_rfont *out);
void svt_rfont_fini(svt_rfont *f);

static inline float svt_scale(const svt_rfont *f, float size)
{
    return size / (float)f->upem;
}

/* has_char: does the face map c (ttf-parser glyph_index over the Unicode cmap subtables)? */
bool svf_has_char(const svf_db *db, size_t id, uint32_t c);

/* Glyph outline in font units (ttf-parser outline_glyph into a tiny-skia PathBuilder; glyf in
 * float, CFF through FreeType). NULL when the glyph has none. */
const sv_path *svt_outline(sv_arena *a, const svt_rfont *f, uint32_t gid);

/* One shaped glyph (layout.rs Glyph). */
typedef struct svt_glyph {
    uint32_t id;
    size_t byte_idx;
    size_t cluster_len;
    int32_t dx, dy, width;
    const svt_rfont *font;
} svt_glyph;

/* shape_text_with_font: bidi runs of the first paragraph, each shaped by HarfBuzz. Glyphs in the
 * arena; false on out of memory or a shaping failure (*n = 0 then means "None"). */
bool svt_shape(sv_arena *a, const char *text, size_t len, const svt_rfont *font, bool small_caps,
               bool kerning, svt_glyph **out, size_t *n);

/* Unicode helpers (svg_text_flat.c). */
bool svt_is_upright(uint32_t c);                     /* unicode-vo: Vertical_Orientation U */
bool svt_script_supports_letter_spacing(uint32_t c); /* unicode-script based list */

/* ---------------------------------------------------------------- text model (usvg tree/text.rs) */

typedef enum { SVT_START, SVT_MIDDLE, SVT_END } svt_anchor;

/* AlignmentBaseline and DominantBaseline values (one enum for both). */
typedef enum {
    SVT_BL_AUTO,
    SVT_BL_BASELINE,
    SVT_BL_BEFORE_EDGE,
    SVT_BL_TEXT_BEFORE_EDGE,
    SVT_BL_MIDDLE,
    SVT_BL_CENTRAL,
    SVT_BL_AFTER_EDGE,
    SVT_BL_TEXT_AFTER_EDGE,
    SVT_BL_IDEOGRAPHIC,
    SVT_BL_ALPHABETIC,
    SVT_BL_HANGING,
    SVT_BL_MATHEMATICAL,
    SVT_BL_USE_SCRIPT,
    SVT_BL_NO_CHANGE,
    SVT_BL_RESET_SIZE
} svt_baseline;

typedef enum { SVT_SHIFT_BASELINE, SVT_SHIFT_SUB, SVT_SHIFT_SUPER, SVT_SHIFT_NUMBER } svt_shift_kind;

typedef struct svt_shift {
    svt_shift_kind kind;
    float n;
} svt_shift;

typedef struct svt_deco {
    bool on;
    sv_fill *fill;
    sv_stroke *stroke;
} svt_deco;

typedef struct svt_span {
    size_t start, end; /* bytes of the chunk text */
    sv_fill *fill;
    sv_stroke *stroke;
    bool stroke_first;
    const svf_font *font;
    float font_size;
    bool small_caps, kerning;
    svt_deco under, over, through;
    bool visible;
    svt_baseline dominant, alignment;
    const svt_shift *shift;
    size_t nshift;
    float letter_spacing, word_spacing;
    bool has_length;
    float text_length;
    bool glyphs_adjust; /* lengthAdjust="spacingAndGlyphs" */
} svt_span;

typedef struct svt_tpath {
    const char *id;
    float start_offset;
    const sv_path *path;
} svt_tpath;

typedef struct svt_chunk {
    bool has_x, has_y;
    float x, y;
    svt_anchor anchor;
    svt_span *spans;
    size_t nspans, capspans;
    const svt_tpath *path; /* NULL: linear */
    char *text;
    size_t len, cap;
} svt_chunk;

typedef struct svt_text {
    const char *id;
    float *dx, *dy, *rotate; /* per character */
    size_t nchars;
    bool vertical; /* writing-mode top-to-bottom */
    bool crisp;    /* text-rendering optimizeSpeed: glyph paths without anti-aliasing */
    svt_chunk *chunks;
    size_t nchunks;
    sv_ts abs;
} svt_text;

/* ---------------------------------------------------------------- layout (text/layout.rs) */

typedef struct svt_pglyph {
    sv_ts glyph_ts, cluster_ts, span_ts;
    uint16_t upem;
    float font_size;
    uint32_t id;
    const svt_rfont *font;
} svt_pglyph;

/* PositionedGlyph::outline_transform. */
sv_ts svt_outline_ts(const svt_pglyph *g);

typedef struct svt_lspan {
    sv_fill *fill;
    sv_stroke *stroke;
    bool stroke_first;
    bool visible;
    svt_pglyph *glyphs;
    size_t nglyphs;
    sv_unode *underline, *overline, *line_through; /* decoration paths or NULL */
} svt_lspan;

/* The fonts one text element uses (layout.rs FontsCache), resources released by svt_fonts_fini. */
typedef struct svt_fonts {
    const svf_db *db;
    const svf_font **keys;
    svt_rfont **vals; /* NULL: no font matched */
    size_t n, cap;
} svt_fonts;

void svt_fonts_fini(svt_fonts *f);
const svt_rfont *svt_fonts_get(const svt_fonts *f, const svf_font *key);

/* Per-import text state kept in the converter (budgets). */
typedef struct svt_budget {
    size_t glyphs;
    size_t work;
} svt_budget;

/* layout_text: spans in *out (arena) and the text bbox; false when the text has none. */
bool svt_layout(sv_conv *c, const svt_text *t, svt_fonts *fonts, svt_budget *bud, svt_lspan **out, size_t *n,
                sv_rect *bbox);

/* ---------------------------------------------------------------- layout internals
 * (svg_text_shape.c: fonts, shaping, clusters; svg_text_layout.c: the rest) */

typedef struct svt_lctx {
    sv_conv *c;
    const svt_text *t;
    svt_fonts *fonts;
    svt_budget *bud;
    bool fail;  /* out of memory */
    bool limit; /* budget exhausted */
} svt_lctx;

typedef struct svt_cluster {
    size_t byte_idx;
    uint32_t cp;
    float width, advance, ascent, descent;
    bool has_relative_shift;
    svt_pglyph *glyphs;
    size_t nglyphs;
    sv_ts transform, path_transform;
    bool visible;
} svt_cluster;

bool svt_charge(svt_lctx *l, size_t units);
uint32_t svt_char_at(const char *s, size_t n, size_t i);
bool svt_fonts_has(const svt_fonts *f, const svf_font *key);
bool svt_select_font(sv_arena *a, svt_fonts *f, const svf_font *font);
const svt_span *svt_span_at(const svt_chunk *ch, size_t b, size_t *idx);
bool svt_span_contains(const svt_span *s, size_t b);
sv_ts svt_cluster_ts(const svt_cluster *c);
/* process_chunk: shaping with fallback per span font, the reference's cluster merge, clusters. */
bool svt_process_chunk(svt_lctx *l, const svt_chunk *ch, svt_cluster **out, size_t *nout);

/* flatten: the flattened group and its stroke bbox; NULL when it has none. */
sv_unode *svt_flatten(sv_conv *c, const svt_text *t, const svt_lspan *spans, size_t n, sv_rect *stroke_bbox);

/* kurbo pieces used by textPath (svg_text_flat.c). */
typedef struct svt_cubic {
    double x0, y0, x1, y1, x2, y2, x3, y3;
} svt_cubic;

double svt_cubic_arclen(const svt_cubic *c, double accuracy);
double svt_cubic_inv_arclen(const svt_cubic *c, double arclen, double accuracy);
/* tiny-skia path segments as kurbo cubics (lines at 0.33/0.66, quads raised). Calls f per
 * segment; MoveTo segments are skipped. */
void svt_path_cubics(const sv_path *p, void (*f)(void *ctx, const svt_cubic *c), void *ctx);

#endif
