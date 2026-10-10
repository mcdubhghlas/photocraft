/* Fonts from the optional craft-fonts checkout (https://github.com/storytold/craft-fonts; port of
 * crates/text/src/craft_fonts.rs and build.rs).
 *
 * The reference embeds every font of `<CRAFT_FONTS_DIR>/fonts/manifest.txt` at build time. The C
 * build never embeds them: the list is read once at run time from the directory named by the
 * CRAFT_FONTS_DIR environment variable, else the build-time default PC_CRAFT_FONTS_DIR (a string
 * macro, e.g. `make EXTRA_CPPFLAGS='-DPC_CRAFT_FONTS_DIR="/path/to/craft-fonts"'`). Unset or
 * empty means no craft fonts, and everything must work with none. A bad checkout loads nothing
 * and logs a warning (an error when CRAFT_FONTS_REQUIRED is set), like build.rs.
 *
 * Manifest lines: `family | style | file | scripts[, ...] [| ...]`, blank lines and `#` comments
 * skipped; `file` is relative to the checkout.
 *
 * Thread-safe: the list is loaded under pthread_once and immutable afterwards. */
#ifndef PC_TEXT_CRAFT_FONTS_H
#define PC_TEXT_CRAFT_FONTS_H

#include <stdbool.h>
#include <stddef.h>

#include "base/blob.h"
#include "base/status.h"

/* The preferred UI family for Japanese. */
#define PC_CRAFT_UI_JAPANESE_FAMILY "BIZ UDPGothic"

typedef struct pc_craft_font {
    char *family;
    char *style;
    char **scripts; /* ISO 15924 codes, e.g. "Jpan" */
    size_t nscripts;
    pc_blob *bytes;
} pc_craft_font;

/* The loaded craft fonts in manifest order (borrowed, process lifetime). Empty when none. */
const pc_craft_font *pc_craft_fonts(size_t *n);
/* Why loading failed ("" when it didn't, or when no directory was configured). */
const char *pc_craft_fonts_error(void);

/* Parses a checkout without touching the process-wide list (exposed for tests and tools).
 * Fonts are appended to *out / *n (free with pc_craft_fonts_free). Errors use build.rs's text:
 * "<manifest path>: <reason>", "malformed manifest line: <line>", "<file>: <reason>". */
pc_status pc_craft_fonts_load_dir(const char *dir, pc_craft_font **out, size_t *n, pc_error *err);
void pc_craft_fonts_free(pc_craft_font *f, size_t n);

bool pc_craft_font_is_japanese(const pc_craft_font *f); /* scripts contain "Jpan" */
bool pc_craft_font_is_mincho(const pc_craft_font *f);   /* family contains "Mincho" */

/* Japanese craft fonts in UI preference order: BIZ UDPGothic Regular first, then its other
 * styles, then the rest in manifest order. *out (free()) holds borrowed pointers into `fonts`. */
pc_status pc_craft_japanese_for_ui(const pc_craft_font *fonts, size_t n, const pc_craft_font ***out,
                                   size_t *count, pc_error *err);
/* Japanese craft families (each once) for document fallback: Gothic first, then Mincho.
 * *out (free()) holds borrowed strings. */
pc_status pc_craft_japanese_families(const pc_craft_font *fonts, size_t n, const char ***out, size_t *count,
                                     pc_error *err);
/* `fallback` with the craft Mincho families moved ahead of the craft Gothic ones, in place (the
 * other entries keep their slots). Reorders the pointer array; `craft` is the list from
 * pc_craft_japanese_families. */
void pc_craft_mincho_first(const char **fallback, size_t n, const char *const *craft, size_t ncraft);

/* A family name that reads as serif (Mincho, Song/Ming, Myeongjo, Times...): its Japanese
 * fallback should be a Mincho face. */
bool pc_craft_is_serif_family(const char *name, size_t len);

#endif
