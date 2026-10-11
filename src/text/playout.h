/* Private to src/text: a port of the parts of parley 0.11 the reference layouter drives (style
 * runs, text analysis, font selection through fontique's query, shaping, cluster data, greedy
 * line breaking, alignment and glyph-run iteration). Shaping uses HarfBuzz (harfrust in the
 * reference), bidi levels FriBidi (parley's own resolver), break opportunities libunibreak
 * (ICU4X); the data structures and the code around them follow parley so that the layouter
 * (layout.c) sees the same runs, clusters, glyphs and line metrics.
 *
 * Everything here is single-threaded and owned by one pl_layout; nothing is shared. */
#ifndef PC_TEXT_PLAYOUT_H
#define PC_TEXT_PLAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "text/fonts.h"
#include "text/fonts_internal.h"

/* parley's Boundary. */
enum { PL_BOUNDARY_NONE = 0, PL_BOUNDARY_WORD = 1, PL_BOUNDARY_LINE = 2, PL_BOUNDARY_MANDATORY = 3 };

/* A BCP 47 language as parley/parlance canonicalises it (`language[-Script][-REGION]`). */
typedef struct pl_lang {
    char bytes[12];
    uint8_t len, language_len;
} pl_lang;

/* parlance Language::parse: false for an invalid tag. */
bool pl_lang_parse(const char *s, size_t len, pl_lang *out);

/* One OpenType feature / variation setting after CSS parsing (sorted by tag). */
typedef struct pl_feature {
    uint32_t tag;
    uint16_t value;
} pl_feature;

typedef struct pl_variation {
    uint32_t tag;
    float value;
} pl_variation;

/* The style properties the layouter sets (parley's ResolvedStyle, the rest at defaults). Lists
 * are interned in the pl_builder, so equal ids mean equal content (parley's Resolved<T>). */
typedef struct pl_style {
    size_t families;   /* index into the builder's family lists */
    size_t features;   /* SIZE_MAX = none (Resolved::default) */
    size_t variations; /* SIZE_MAX = none */
    float size;
    float weight;
    bool italic;
    bool has_locale;
    pl_lang locale;
    uint32_t brush_run; /* RunBrush(run, class) */
    uint8_t brush_class;
    bool underline, strikethrough;
    float letter_spacing;
} pl_style;

bool pl_style_eq(const pl_style *a, const pl_style *b);

/* A font chosen for some clusters (fontique QueryFont + attributes). */
typedef struct pl_font_choice {
    const pc_fontdb_family *family;
    size_t face;         /* index in the family */
    pc_font *font;       /* borrowed from the database */
    pc_font_match synth; /* embolden / skew / vars only */
    float attr_weight;   /* the query's attributes (SelectedFont::attrs) */
    bool attr_italic;
} pl_font_choice;

/* parley RunMetrics (px). */
typedef struct pl_run_metrics {
    float ascent, descent, leading;
    float underline_offset, underline_size;
    float strikethrough_offset, strikethrough_size;
    float line_height;
} pl_run_metrics;

typedef struct pl_run {
    size_t font_index;
    float font_size;
    float attr_weight; /* query attributes (fontique::Attributes) */
    bool attr_italic;
    pc_font_match synth;
    size_t coords_start, coords_end;
    size_t text_start, text_end;
    uint8_t bidi_level;
    size_t cluster_start, cluster_end;
    size_t glyph_start;
    pl_run_metrics metrics;
    float letter_spacing;
    float advance;
} pl_run;

enum { PL_CLUSTER_LIGATURE_START = 1, PL_CLUSTER_LIGATURE_COMPONENT = 2 };

typedef struct pl_cluster {
    uint8_t boundary;
    uint32_t source_char;
    uint16_t flags;
    uint16_t style_index;
    uint8_t glyph_len; /* 0xFF: one inline glyph, id in glyph_offset */
    uint8_t text_len;
    uint32_t glyph_offset;
    uint16_t text_offset;
    float advance;
} pl_cluster;

typedef struct pl_glyph {
    uint32_t id;
    uint16_t style_index;
    float x, y, advance;
} pl_glyph;

/* parley's Whitespace of a cluster's source char. */
enum { PL_WS_NONE, PL_WS_SPACE, PL_WS_NBSP, PL_WS_TAB, PL_WS_NEWLINE };
int pl_whitespace(uint32_t c);

typedef enum pl_break_reason {
    PL_BREAK_NONE,
    PL_BREAK_REGULAR,
    PL_BREAK_EXPLICIT,
    PL_BREAK_EMERGENCY
} pl_break_reason;

typedef struct pl_line_metrics {
    float ascent, descent, leading, line_height, baseline, offset, advance, trailing_whitespace;
    float inline_min_coord, inline_max_coord, block_min_coord, block_max_coord;
} pl_line_metrics;

typedef struct pl_line_item {
    size_t run; /* index into runs */
    uint8_t bidi_level;
    float advance;
    bool is_whitespace, has_trailing_whitespace;
    size_t text_start, text_end;
    size_t cluster_start, cluster_end;
} pl_line_item;

typedef struct pl_line {
    size_t text_start, text_end;
    size_t item_start, item_end;
    pl_line_metrics metrics;
    pl_break_reason break_reason;
    float max_advance;
    size_t num_spaces;
    float indent;
} pl_line;

/* The shaped and broken text of one paragraph (parley's Layout). */
typedef struct pl_layout {
    uint8_t base_level;
    size_t text_len;
    pl_style *styles;
    size_t nstyles;
    pc_font **fonts; /* borrowed */
    size_t nfonts, cap_fonts;
    int16_t *coords;
    size_t ncoords, cap_coords;
    pl_run *runs;
    size_t nruns, cap_runs;
    size_t *items; /* run index per layout item (text runs only; no inline boxes) */
    size_t nitems, cap_items;
    pl_cluster *clusters;
    size_t nclusters, cap_clusters;
    pl_glyph *glyphs;
    size_t nglyphs, cap_glyphs;
    pl_line *lines;
    size_t nlines, cap_lines;
    pl_line_item *line_items;
    size_t nline_items, cap_line_items;
    float indent_amount;
} pl_layout;

void pl_layout_fini(pl_layout *l);

/* ---- building (shape.c) ---- */

/* One ranged style property push (RangedBuilder::push). */
typedef enum pl_prop_kind {
    PL_PROP_FAMILIES,
    PL_PROP_SIZE,
    PL_PROP_WEIGHT,
    PL_PROP_ITALIC,
    PL_PROP_FEATURES,
    PL_PROP_VARIATIONS,
    PL_PROP_LETTER_SPACING,
    PL_PROP_UNDERLINE,
    PL_PROP_STRIKETHROUGH,
    PL_PROP_BRUSH,
    PL_PROP_LOCALE
} pl_prop_kind;

typedef struct pl_prop {
    pl_prop_kind kind;
    size_t start, end; /* byte range; ignored for defaults */
    size_t id;         /* FAMILIES / FEATURES / VARIATIONS (interned list or SIZE_MAX) */
    float f;           /* SIZE / WEIGHT / LETTER_SPACING */
    bool b;            /* ITALIC / UNDERLINE / STRIKETHROUGH / has locale */
    pl_lang lang;
    uint32_t brush_run;
    uint8_t brush_class;
} pl_prop;

/* Shaping context: interned lists, the font database, HarfBuzz face cache. */
typedef struct pl_builder pl_builder;

pc_status pl_builder_new(pl_builder **out, pc_error *err);
void pl_builder_free(pl_builder *b);
/* Forgets the per-layout interned lists and pushed properties (keeps the face cache). */
void pl_builder_begin(pl_builder *b, pc_fontdb *db);
/* Interns a resolved family list (each entry a family of db; duplicates allowed). */
pc_status pl_builder_families(pl_builder *b, const pc_fontdb_family *const *fams, size_t n, size_t *id,
                              pc_error *err);
/* Interns features / variations from parsed CSS lists (sorted stably by tag here); n == 0
 * gives SIZE_MAX. */
pc_status pl_builder_features(pl_builder *b, pl_feature *f, size_t n, size_t *id, pc_error *err);
pc_status pl_builder_variations(pl_builder *b, pl_variation *v, size_t n, size_t *id, pc_error *err);
pc_status pl_builder_push_default(pl_builder *b, const pl_prop *p, pc_error *err);
pc_status pl_builder_push(pl_builder *b, const pl_prop *p, pc_error *err);
/* Analyses, itemises, selects fonts and shapes `text` (valid UTF-8) into out (initialised by
 * this call; release with pl_layout_fini even on failure). */
pc_status pl_builder_build(pl_builder *b, const char *text, size_t len, pl_layout *out, pc_error *err);

/* ---- line breaking and alignment (linebreak.c) ---- */

/* Layout::break_all_lines(max_advance) (FLT_MAX for none) with the text indent set before. */
pc_status pl_break_all_lines(pl_layout *l, float max_advance, pc_error *err);

typedef enum pl_align { PL_ALIGN_LEFT, PL_ALIGN_CENTER, PL_ALIGN_RIGHT, PL_ALIGN_JUSTIFY } pl_align;
void pl_align_lines(pl_layout *l, pl_align a, bool align_when_overflowing);

/* A glyph run of a line (parley's GlyphRun: consecutive glyphs of one line item that share a
 * style index), in visual order. */
typedef struct pl_glyph_run {
    size_t item;                     /* index into line_items */
    size_t index;                    /* the run's position on the line (Run::index) */
    size_t glyph_start, glyph_count; /* within the item's visual glyph sequence */
    uint16_t style_index;
    float offset, baseline, advance;
} pl_glyph_run;

/* The visual glyph sequence of a line item: glyphs of its clusters in visual order (inline
 * glyphs expanded), with the visual cluster each came from. */
typedef struct pl_item_glyphs {
    pl_glyph *glyphs;
    size_t *cluster; /* index into layout clusters */
    size_t n, cap;
} pl_item_glyphs;

pc_status pl_item_glyphs_get(const pl_layout *l, size_t item, pl_item_glyphs *out, pc_error *err);
void pl_item_glyphs_fini(pl_item_glyphs *g);
/* The glyph runs of line li (Line::items()); *out is freed with free(). */
pc_status pl_line_glyph_runs(const pl_layout *l, size_t li, pl_glyph_run **out, size_t *n, pc_error *err);
/* A cluster's glyphs (Cluster::glyphs): writes up to cap glyphs, returns the count. */
size_t pl_cluster_glyphs(const pl_layout *l, const pl_run *run, const pl_cluster *c, pl_glyph *out,
                         size_t cap);
/* Cluster text range (absolute byte offsets). */
static inline size_t pl_cluster_start(const pl_run *r, const pl_cluster *c)
{
    return r->text_start + c->text_offset;
}

/* ---- style resolution (pstyle.c) and text analysis (panalysis.c) ---- */

bool pl_lang_eq(bool has_a, const pl_lang *a, bool has_b, const pl_lang *b);
void pl_apply_prop(pl_style *s, const pl_prop *p);

/* A run of bytes with one style (StyleRun). */
typedef struct pl_style_run {
    size_t start, end;
    uint16_t index; /* `as u16`, as the reference (wraps past 65535 styles) */
} pl_style_run;

/* RangedStyleBuilder::finish: the style table and runs for text of len bytes from the root
 * style and the pushed properties (their ranges are clamped to len in place). */
pc_status pl_resolve_styles(const pl_style *root, pl_prop *props, size_t nprops, size_t len, pl_style **table,
                            size_t *ntable, pl_style_run **runs, size_t *nruns, pc_error *err);

/* Per-character analysis (CharInfo plus the bidi level and style index). */
typedef struct pl_info {
    uint32_t cp;
    uint32_t props; /* uprops.h */
    uint8_t boundary;
    uint8_t level;
    uint16_t style;
} pl_info;

/* libunibreak's one-time initialisation (thread-safe). */
void pl_unibreak_init(void);
/* analyze_text: infos for text (one space for empty text), bidi levels and the base level. */
pc_status pl_analyze_text(const char *text, size_t len, pl_info **out, size_t *nout, uint8_t *base,
                          pc_error *err);
/* A grapheme char that NFC/NFD forms are tried first for (Extend/SpacingMark). */
bool pl_force_normalize(uint32_t cp, uint32_t props);

#endif
