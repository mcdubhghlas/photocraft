/* Font database (port of crates/text/src/fonts.rs and the fontique 0.11 collection it used):
 * bundled fonts (Inter, JetBrains Mono; always available), the optional craft-fonts Japanese
 * fonts (craft_fonts.h), system fonts found by scanning the platform font directories (no
 * fontconfig), user-registered font data, CSS font matching, and PostScript-name lookup for PSD
 * import.
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

#include "base/status.h"
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
    float weight; /* CSS weight; variable fonts report their default */
    bool italic;
    pc_font_axis *axes;
    size_t naxes;
} pc_face_info;

/* Faces of a family (empty for an unknown one). Free with pc_face_infos_free. */
pc_status pc_fontdb_faces(pc_fontdb *db, const char *family, size_t len, pc_face_info **out, size_t *n,
                          pc_error *err);
void pc_face_infos_free(pc_face_info *f, size_t n);

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
 * the UI locale (borrowed until the database changes). */
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
