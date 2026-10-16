/* UI localisation (port of crates/ui-egui/src/i18n/). Strings in code stay English and are the
 * default lookup keys; per-language catalogs (the embedded *.tsv) map them to display text. Command
 * ids, the control channel, the CLI and MCP never see translated text, so agents are unaffected.
 *
 * Pure data + logic: no GUI dependency. A string without a translation is shown in English. */
#ifndef PC_UI_I18N_H
#define PC_UI_I18N_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"

/* A supported UI language (a handle into the static registry; compare by pointer). */
typedef struct pc_i18n_lang pc_i18n_lang;

/* A script's reading direction. */
typedef enum pc_i18n_dir {
    PC_I18N_LTR,
    PC_I18N_RTL
} pc_i18n_dir;

/* English: the fallback and source language (registry[0]). */
const pc_i18n_lang *pc_i18n_en(void);

/* The registry (English first). */
size_t pc_i18n_count(void);
const pc_i18n_lang *pc_i18n_at(size_t index);

/* Accessors. */
const char *pc_i18n_lang_code(const pc_i18n_lang *lang);
const char *pc_i18n_lang_name(const pc_i18n_lang *lang);
bool pc_i18n_lang_complete_menus(const pc_i18n_lang *lang);
pc_i18n_dir pc_i18n_lang_dir(const pc_i18n_lang *lang);
/* Plural form index for a count (English 0=one 1=other; see the catalog's @plural). */
size_t pc_i18n_lang_plural(const pc_i18n_lang *lang, uint64_t n);
/* The raw embedded catalog bytes (NULL/0 for English); *len gets the length. For tooling/tests. */
const unsigned char *pc_i18n_lang_source(const pc_i18n_lang *lang, size_t *len);

/* The reading direction of a language code (`ar`, `fa-ir`, `he`; any case). */
pc_i18n_dir pc_i18n_dir_for_code(const char *code);

/* A language by its exact registry code (case-insensitive); NULL if none. */
const pc_i18n_lang *pc_i18n_from_code(const char *code);

/* The registered language for a locale tag (`ja_JP.UTF-8`, `zh-TW`); NULL if unsupported.
 * `C`/`POSIX` mean English. */
const pc_i18n_lang *pc_i18n_from_tag(const char *tag);

/* Resolve the interface.language preference: a code, or `auto` (and anything unknown) to the
 * system locale. Never NULL. */
const pc_i18n_lang *pc_i18n_from_pref(const char *pref);

/* Candidate codes for a locale tag, most specific first (for tests / diagnostics). Writes up to
 * `cap` NUL-terminated codes into out[] (each at most 31 bytes + NUL) and returns the count. */
size_t pc_i18n_candidates(const char *tag, char out[][32], size_t cap);

/* Current drawing language (thread-local; defaults to English). */
void pc_i18n_set_current(const pc_i18n_lang *lang);
const pc_i18n_lang *pc_i18n_current(void);

/* The system UI language (PHOTOCRAFT_LOCALE, else the POSIX locale environment). English on
 * anything missing or unsupported. */
const pc_i18n_lang *pc_i18n_system_lang(void);
/* Test hook: override the system locale tags (NULL/0 clears the override and restores env use).
 * Thread-local, like the Rust reference's test harness. */
void pc_i18n_set_system_tags_for_test(const char *const *tags, size_t n);

/* Translate an English UI string into `lang`; unknown strings come back unchanged (borrowed). */
const char *pc_i18n_tr(const pc_i18n_lang *lang, const char *s);
/* tr for an English string that needs a disambiguating context. */
const char *pc_i18n_tr_ctx(const pc_i18n_lang *lang, const char *context, const char *s);
/* A string keyed by command id, falling back to the translation of the English label. */
const char *pc_i18n_tr_id(const pc_i18n_lang *lang, const char *id, const char *label);
/* tr in the current drawing language. */
const char *pc_i18n_t(const char *s);
/* Does lang have a catalog entry for this plain string? (English never does.) */
bool pc_i18n_has(const pc_i18n_lang *lang, const char *s);

/* Fill `{name}` placeholders. keys[i]/vals[i] for i<n; unknown placeholders stay. The whole result
 * is written as `out`'s contents (out is cleared first). */
pc_status pc_i18n_fmt(const char *template, const char *const *keys, const char *const *vals, size_t n,
                      pc_buf *out, pc_error *err);

/* A plural-aware message: `one`/`other` are the English forms (with `{n}`); result in `out`. */
pc_status pc_i18n_trn(const pc_i18n_lang *lang, uint64_t n, const char *one, const char *other, pc_buf *out,
                      pc_error *err);

/* Parse a catalog's text (TSV). Diagnostics only: returns the number of malformed lines; when
 * `first_error`/`first_error_cap` is given, writes the first error message there. Used by tests to
 * prove the bundled catalogs are clean. */
size_t pc_i18n_catalog_errors(const char *text, size_t len, char *first_error, size_t first_error_cap);

/* `{name}` placeholders of a template, in order of appearance (for tests). Writes up to `cap`
 * NUL-terminated names into out[] (each at most 63 bytes) and returns the count. */
size_t pc_i18n_placeholders(const char *s, char out[][64], size_t cap);

#endif
