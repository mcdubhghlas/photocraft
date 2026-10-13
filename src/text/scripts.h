/* UI fallback fonts for the non-CJK scripts the bundled Inter / JetBrains Mono don't draw (port of
 * crates/text/src/scripts.rs): which characters belong to which script, and which installed font
 * files to try for it. Candidates come from file names in the platform font directories, listed
 * once per process on first use; a file name is only a hint, callers check pc_text_covering_face
 * before using a file. CJK has its own lists (cjk.h). Thread-safe. */
#ifndef PC_TEXT_SCRIPTS_H
#define PC_TEXT_SCRIPTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "text/fonts.h"

typedef enum pc_text_script {
    PC_SCRIPT_THAI,
    PC_SCRIPT_LAO,
    PC_SCRIPT_KHMER,
    PC_SCRIPT_MYANMAR,
    PC_SCRIPT_TIBETAN,
    PC_SCRIPT_ARABIC,
    PC_SCRIPT_HEBREW,
    PC_SCRIPT_ARMENIAN,
    PC_SCRIPT_GEORGIAN,
    PC_SCRIPT_ETHIOPIC,
    PC_SCRIPT_DEVANAGARI,
    PC_SCRIPT_BENGALI,
    PC_SCRIPT_GURMUKHI,
    PC_SCRIPT_GUJARATI,
    PC_SCRIPT_ORIYA,
    PC_SCRIPT_TAMIL,
    PC_SCRIPT_TELUGU,
    PC_SCRIPT_KANNADA,
    PC_SCRIPT_MALAYALAM,
    PC_SCRIPT_SINHALA,
    PC_SCRIPT_COUNT
} pc_text_script;

/* The script of a character that needs a non-CJK fallback font; false for others. */
bool pc_text_script_classify(uint32_t cp, pc_text_script *out);
/* The script's font file names, most preferred first (lowercase, no extension, spaces,
 * underscores or style suffix; NULL-terminated, static). */
const char *const *pc_text_script_file_names(pc_text_script s);
/* Font files among `files` that may cover the script, most preferred first (at most 16): by name
 * rank, then regular before variable before other weights before bold/italic/condensed, then
 * path (component order). Copies in *out. */
pc_status pc_text_script_candidates(pc_text_script s, const char *const *files, size_t n, pc_strlist *out,
                                    pc_error *err);
/* Every font file in the platform font directories, listed once per process (sorted, unique;
 * borrowed, valid for the process). NULL on out of memory. */
const pc_strlist *pc_text_system_font_files(void);
/* pc_text_script_candidates over pc_text_system_font_files. */
pc_status pc_text_script_system_candidates(pc_text_script s, pc_strlist *out, pc_error *err);
/* The first face of a font or collection whose character map has cp; false for fonts without
 * it and data that doesn't parse. */
bool pc_text_covering_face(const uint8_t *bytes, size_t len, uint32_t cp, uint32_t *face);

#endif
