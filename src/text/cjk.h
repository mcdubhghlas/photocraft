/* CJK font fallback (port of crates/text/src/cjk.rs): which installed system fonts cover
 * Japanese, Simplified Chinese, Traditional Chinese and Korean, and in which order to try them
 * for the user's locale. Han characters are shared between the four but their preferred glyph
 * forms differ, so the locale's script goes first; kana always prefers Japanese, Hangul Korean,
 * Bopomofo Traditional Chinese.
 *
 * Ported in L13 (not L14 as planned) because the font database's fallback list needs it.
 * Thread-safe; pc_cjk_ui_locale caches its answer on first use (pthread_once). */
#ifndef PC_TEXT_CJK_H
#define PC_TEXT_CJK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"

typedef enum pc_cjk_script {
    PC_CJK_JAPANESE,
    PC_CJK_SIMPLIFIED_CHINESE,
    PC_CJK_TRADITIONAL_CHINESE,
    PC_CJK_KOREAN
} pc_cjk_script;

/* The kind of CJK character that needs a fallback font. */
typedef enum pc_cjk_char {
    PC_CJK_NONE, /* not a CJK character */
    PC_CJK_HAN,  /* ideographs, CJK punctuation, full-width forms: font follows the locale */
    PC_CJK_KANA,
    PC_CJK_HANGUL,
    PC_CJK_BOPOMOFO
} pc_cjk_char;

pc_cjk_char pc_cjk_classify(uint32_t cp);
/* The script whose fonts should be tried first for a character of kind c. */
pc_cjk_script pc_cjk_preferred(pc_cjk_char c, const pc_cjk_script order[4]);

/* Script order for a locale tag such as "ja_JP.UTF-8", "zh-Hant-TW", "zh_CN" or "ko-KR"
 * (NULL = none). Non-CJK or unknown locales get Simplified Chinese first. */
void pc_cjk_script_order(const char *locale, pc_cjk_script out[4]);
/* The user's UI locale: PHOTOCRAFT_LOCALE, then macOS's first AppleLanguages entry (macOS
 * only), then LC_ALL / LC_MESSAGES / LANG / LANGUAGE (first ':' item); values "C", "POSIX",
 * "C.*" and blanks are skipped. NULL when nothing is set. Cached for the process. */
const char *pc_cjk_ui_locale(void);
void pc_cjk_ui_script_order(pc_cjk_script out[4]);

/* First AppleLanguages entry of a binary property list (bounds-checked; false on anything
 * else). Appends the UTF-8 string to out. */
bool pc_cjk_bplist_first_language(const uint8_t *b, size_t len, pc_buf *out);

/* Family names per script, most preferred first (static strings, NULL-terminated array). */
const char *const *pc_cjk_families(pc_cjk_script s);

/* An installed font file to try: its path and the family of the wanted face in a collection
 * ("" = face 0). */
typedef struct pc_cjk_font_file {
    char *path;   /* owned */
    char *family; /* owned */
} pc_cjk_font_file;

/* System font files for a script / the broad-coverage last resorts, most preferred first, for
 * this platform (paths may not exist). Free with pc_cjk_font_files_free. */
pc_status pc_cjk_font_files(pc_cjk_script s, pc_cjk_font_file **out, size_t *n, pc_error *err);
pc_status pc_cjk_last_resort_files(pc_cjk_font_file **out, size_t *n, pc_error *err);
void pc_cjk_font_files_free(pc_cjk_font_file *f, size_t n);

/* Starts the optional Fontconfig CJK lookup (`fc-match :lang=...` per script, Linux only) on a
 * background thread, once per process; later calls do nothing. Its answers (files that really
 * cover the script) are appended to pc_cjk_font_files once it finished; missing fc-match or a
 * failed match leaves the built-in candidates unchanged. No-op elsewhere. */
void pc_cjk_start_fontconfig_scan(void);
/* Whether that lookup was started and hasn't finished. */
bool pc_cjk_fontconfig_scan_pending(void);
/* fontconfig_match: asks fc-match for the script's font now (blocking). *found false when there
 * is no usable answer; otherwise *out holds an owned path (canonical) and family. */
pc_status pc_cjk_fontconfig_match(pc_cjk_script s, pc_cjk_font_file *out, bool *found, pc_error *err);
/* Whether the face `family` of a font file maps every character of the script's sample
 * ("レイヤー図", "图层", "圖層", "카드 배경"). */
bool pc_cjk_font_covers_sample(const uint8_t *bytes, size_t len, const char *family, pc_cjk_script s);

/* macOS PingFang.ttc (pre-installed asset or the pre-Catalina location); NULL elsewhere.
 * Caller frees. */
char *pc_cjk_mac_pingfang(void);

/* Face index in a font file whose typographic or legacy family name is `family`; 0 when
 * family is empty, the file isn't a collection or no face matches. */
uint32_t pc_cjk_face_index_for_family(const uint8_t *bytes, size_t len, const char *family);

#endif
