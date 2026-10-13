/* Font database (port of crates/text/src/fonts.rs and the fontique 0.11 collection it used):
 * bundled fonts (Inter, JetBrains Mono; always available), the optional craft-fonts Japanese
 * fonts (craft_fonts.h), system fonts found by scanning the platform font directories plus
 * Fontconfig's file list on Linux (`fc-list`, when installed; not linked), user-registered font
 * data, CSS font matching, exact named faces, and PostScript-name lookup for PSD import.
 *
 * Families are keyed by their Unicode-lowercased name (fontique's NameKey). A face's family is
 * its typographic family name (name id 16), else its family name (id 1), chosen by fontique's
 * english-or-first rule; faces without a usable cmap are skipped. Registering the same data
 * twice appends its faces again (as the reference does).
 *
 * Ownership and threads: a pc_fontdb is single-owner and not synchronised (the reference's
 * FontDb is used through &mut / a Mutex); use one per thread or lock around it. Fonts it hands
 * out (pc_font) are reference counted and thread-safe on their own. System font files are read
 * lazily, when a face is first loaded, and kept for the database's lifetime. Strings returned
 * through out-parameters are owned by the caller unless documented as borrowed. */
#ifndef PC_TEXT_FONTS_H
#define PC_TEXT_FONTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"
#include "doc/text.h"
#include "text/cjk.h"
#include "text/font.h"
#include "text/sfnt.h"

/* Family used when a style names no family or an unknown one. */
#define PC_TEXT_DEFAULT_FAMILY "Inter"
/* Bundled monospace family. */
#define PC_TEXT_MONO_FAMILY "JetBrains Mono"

/* Fonts shipped with PhotoCraft (OFL; licences in assets/fonts), compiled in by the Makefile. */
typedef struct pc_bundled_font {
    const char *file; /* "Inter-Regular.ttf" */
    const unsigned char *data;
    size_t len;
} pc_bundled_font;

const pc_bundled_font *pc_text_bundled_fonts(size_t *n);

/* A list of owned strings. */
typedef struct pc_strlist {
    char **v;
    size_t n;
} pc_strlist;

void pc_strlist_fini(pc_strlist *l);
/* Appends a copy of s[0..len) to l. */
pc_status pc_strlist_push(pc_strlist *l, const char *s, size_t len, pc_error *err);

typedef struct pc_fontdb pc_fontdb;

/* Bundled fonts plus the Japanese craft fonts (when configured): deterministic (tests). */
pc_status pc_fontdb_new(pc_fontdb **out, pc_error *err);
/* pc_fontdb_new plus the fonts installed on this machine. */
pc_status pc_fontdb_with_system_fonts(pc_fontdb **out, pc_error *err);
void pc_fontdb_free(pc_fontdb *db); /* NULL-safe */

/* Scans the platform font directories (once per database; later calls do nothing). Unreadable
 * directories and files are skipped. Fails only when out of memory. */
pc_status pc_fontdb_load_system_fonts(pc_fontdb *db, pc_error *err);
/* The directories pc_fontdb_load_system_fonts scans, in order (for tests and diagnostics). */
pc_status pc_text_system_font_dirs(pc_strlist *out, pc_error *err);
/* Font files (ttf/otf/ttc/otc, any case) under dir, depth-first in directory order, at most
 * 8 directory levels deep; a missing directory adds nothing. Appends to out. */
pc_status pc_text_collect_font_files(const char *dir, pc_strlist *out, pc_error *err);
/* Registers every face of one font file read from disk (lazily loaded later). */
pc_status pc_fontdb_add_font_file(pc_fontdb *db, const char *path, pc_error *err);

/* Registers font data (TTF/OTF, or every face of a TTC/OTC); the bytes are copied. `names`
 * (may be NULL) receives the families the data added to, each once, user-facing ones first
 * (hidden families last), then in Unicode-lowercase order, as the reference sorts them. */
pc_status pc_fontdb_register_font_data(pc_fontdb *db, const uint8_t *data, size_t len, pc_strlist *names,
                                       pc_error *err);

/* All family names except hidden ones, sorted by their lowercase form. */
pc_status pc_fontdb_families(pc_fontdb *db, pc_strlist *out, pc_error *err);
bool pc_fontdb_has_family(const pc_fontdb *db, const char *name, size_t len);

/* One face of a family (for font menus). */
typedef struct pc_face_info {
    char *family;
    /* OpenType typographic subfamily (name 17), or legacy subfamily (name 2), else a name from
     * the weight and slope ("SemiBold Italic"). */
    char *style;
    /* Stable face identity (name 6), also for faces that share CSS attributes; NULL when none. */
    char *postscript_name;
    float weight; /* CSS weight; variable fonts report their default */
    bool italic;
    pc_font_axis *axes;
    size_t naxes;
} pc_face_info;

/* Faces of a family (empty for an unknown one). Free with pc_face_infos_free. */
pc_status pc_fontdb_faces(pc_fontdb *db, const char *family, size_t len, pc_face_info **out, size_t *n,
                          pc_error *err);
void pc_face_infos_free(pc_face_info *f, size_t n);
/* The face of `family` whose style name is `style` (ASCII case-insensitive). *found false when
 * there is none (out untouched). Free with pc_face_infos_free(out, 1). */
pc_status pc_fontdb_named_face(pc_fontdb *db, const char *family, const char *style, pc_face_info **out,
                               bool *found, pc_error *err);
/* Selects an exact face for shaping (select_named_face): the face of style->font_family whose
 * PostScript name is the style's, else whose style name, weight and slope are the style's. Such a
 * face is registered once under a private alias family (".PhotoCraft-face-<id>-<index>", runtime
 * only, never listed), which replaces style->font_family; weight and italic become the face's.
 * Otherwise the style is unchanged. CSS matching alone can't tell apart, e.g., numeric
 * subfamilies that all declare weight 400. */
pc_status pc_fontdb_select_named_face(pc_fontdb *db, pc_char_style *style, pc_error *err);
/* A collection's face `index` as a one-face collection (only the face directory changes; table
 * offsets are absolute); a single font for index 0 as is. *ok false for unreadable data, a bad
 * index or a collection over 256 MiB. *out (when ok) holds a new reference. */
pc_status pc_text_isolated_face(pc_blob *blob, uint32_t index, pc_blob **out, bool *ok, pc_error *err);
/* A style name from a weight and slope ("Regular", "Italic", "SemiBold Italic"; static). */
const char *pc_text_fallback_style(float weight, bool italic, char buf[32]);
/* Each listed family's localized (non-English) family name, as (english, localized) pairs in
 * out (2 strings per family; families without one are absent): Show Font Names in English off. */
pc_status pc_fontdb_localized_family_names(pc_fontdb *db, pc_strlist *out, pc_error *err);
/* localized_name: the first name whose language is known, not English and not empty, that isn't
 * blank and differs from `english`; NULL when none. `langs` holds each name's language tag (NULL
 * for none). Borrowed from names. */
const char *pc_text_localized_name(const char *english, const char *const *names, const char *const *langs,
                                   size_t n);
/* Changes whenever font data is registered in any database (font menus compare it). */
uint64_t pc_text_fonts_generation(void);

/* Preferences > Type > Missing Glyph Protection (on by default): off, only the default family
 * is in the fallback stack. */
void pc_fontdb_set_missing_glyph_protection(pc_fontdb *db, bool on);
bool pc_fontdb_missing_glyph_protection(const pc_fontdb *db);

/* Southeast Asian fallback families (NULL-terminated, static), tried after FALLBACK_CANDIDATES. */
extern const char *const PC_TEXT_THAI_FAMILIES[];
extern const char *const PC_TEXT_KHMER_FAMILIES[];
extern const char *const PC_TEXT_LAO_FAMILIES[];
extern const char *const PC_TEXT_MYANMAR_FAMILIES[];

/* ---- Fontconfig helpers (Linux; fonts_scan.c) ---- */

/* Runs argv (PATH lookup, no shell) with stdin/stderr on /dev/null and appends its standard output
 * (at most `max` bytes) to out. *ok: it ran, exited 0 and its output fit. Fails only when out of
 * memory. */
pc_status pc_text_run_capture(const char *const *argv, size_t max, pc_buf *out, bool *ok, pc_error *err);
/* A path whose file name has a font extension (ttf/otf/ttc/otc, any case). */
bool pc_text_font_path_ext(const char *path, size_t len);
/* The unique absolute font paths of `fc-list --format=%{file}\n` output, canonicalised where
 * possible, in order. */
pc_status pc_text_fontconfig_paths_from_output(const uint8_t *data, size_t len, pc_strlist *out,
                                               pc_error *err);

/* Result of a PostScript-name lookup. */
typedef struct pc_resolved_font {
    char *family;
    uint16_t weight;
    bool italic;
    bool exact; /* the exact face was found; false for a heuristic family guess */
} pc_resolved_font;

void pc_resolved_font_fini(pc_resolved_font *r);

/* Heuristic PostScript-name split: "MyriadPro-BoldIt" -> ("Myriad Pro", 700, italic). */
pc_status pc_text_guess_from_postscript(const char *ps, size_t len, pc_resolved_font *out, pc_error *err);
/* Finds a face by PostScript name (as stored in PSD files): exact match by reading the name
 * table of candidate faces, else the heuristic guess (its family replaced by an installed one
 * that matches ignoring spaces and ASCII case). Results are cached per database. */
pc_status pc_fontdb_resolve_postscript(pc_fontdb *db, const char *ps, size_t len, pc_resolved_font *out,
                                       pc_error *err);

/* Number of faces in a font file (1 for TTF/OTF, n for TTC/OTC, 0 if unreadable). */
uint32_t pc_text_face_count(const uint8_t *data, size_t len);
/* A platform-private family (a leading '.'): never listed or picked by default. */
bool pc_text_is_hidden_family(const char *name, size_t len);

/* Every fallback family candidate, CJK ordered for `order` (craft-font Japanese families go in
 * the Japanese slot, ahead of the installed ones). The strings are borrowed (static or the
 * process-wide craft list); free only the array. */
pc_status pc_text_fallback_candidates(const pc_cjk_script order[4], const char ***out, size_t *n,
                                      pc_error *err);
/* Families tried after a style's own: the default family, then the installed candidates for
 * the UI locale and the served script fallbacks that arrived (borrowed until the database
 * changes); only the default family with missing glyph protection off. */
size_t pc_fontdb_fallback_count(const pc_fontdb *db);
const char *pc_fontdb_fallback(const pc_fontdb *db, size_t i);
/* Family set for a CSS generic ("sans-serif", "serif", "system-ui", "ui-sans-serif",
 * "monospace", "ui-monospace"); NULL when none (borrowed). */
const char *pc_fontdb_generic(const pc_fontdb *db, const char *generic);

/* Characters mapped by the face of `family` nearest to weight/italic, ascending, without
 * controls and white space other than U+0020 and without variation selectors FE00-FE0F. *out is
 * NULL for an unknown family (free()). (glyphs.rs) */
pc_status pc_fontdb_charmap(pc_fontdb *db, const char *family, size_t len, uint16_t weight, bool italic,
                            uint32_t **out, size_t *n, pc_error *err);
/* charmap_for_style: the character coverage of the exact named face the type tool uses
 * (pc_fontdb_select_named_face, then pc_fontdb_charmap). */
pc_status pc_fontdb_charmap_for_style(pc_fontdb *db, const pc_char_style *style, uint32_t **out, size_t *n,
                                      pc_error *err);
/* Whether the face selected for a character style has GSUB feature `tag`. Unknown or empty
 * families use the default family. */
bool pc_fontdb_selected_face_has_feature(pc_fontdb *db, const char *family, size_t len, uint16_t weight,
                                         bool italic, uint32_t tag);

/* ---- font matching (for layout) ---- */

typedef struct pc_font_query {
    float width;         /* ratio, 1.0 normal */
    pc_font_slope slope; /* normal, italic or oblique */
    bool has_angle;      /* oblique angle given (else the default 14°) */
    float angle;
    float weight; /* 1..1000 */
} pc_font_query;

/* A matched face with fontique's synthesis suggestions. */
typedef struct pc_font_match {
    pc_font *font;      /* one reference, owned by the caller (pc_font_release) */
    const char *family; /* borrowed; valid until the database changes */
    float weight;       /* the face's own attributes */
    pc_font_slope slope;
    bool embolden;        /* faux bold: requested weight above the face's and no wght axis */
    float skew_deg;       /* faux oblique angle, 0 for none */
    uint32_t var_tags[3]; /* variation settings to apply (wdth/wght/ital/slnt) */
    float var_values[3];
    size_t nvars;
} pc_font_match;

/* CSS font matching within `family` (fontique match_font + synthesis). PC_ERR_NOT_FOUND for
 * an unknown or empty family; PC_ERR_FORMAT when the matched face can't be opened. */
pc_status pc_fontdb_match(pc_fontdb *db, const char *family, size_t len, const pc_font_query *q,
                          bool synthesize_style, pc_font_match *out, pc_error *err);
/* The loaded font for face `i` of family (borrowed reference; NULL when unreadable). For
 * callers walking pc_fontdb_faces' order. */
pc_font *pc_fontdb_face_font(pc_fontdb *db, const char *family, size_t len, size_t i, pc_error *err);

#endif
