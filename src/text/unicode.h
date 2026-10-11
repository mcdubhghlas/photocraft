/* Unicode character properties and small UTF-8 string helpers for src/text, matching Rust 1.95's
 * `char` methods (tables in unicode_tables.c, generated from the reference toolchain).
 *
 * All functions are pure and thread-safe. Strings are UTF-8 with an explicit length; invalid
 * bytes are treated as U+FFFD by the iterating helpers (they never read past `len`). */
#ifndef PC_TEXT_UNICODE_H
#define PC_TEXT_UNICODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"

/* char::is_numeric (general categories Nd, Nl, No). */
bool pc_text_is_numeric(uint32_t cp);
/* char::is_lowercase / is_uppercase (derived Lowercase / Uppercase properties). */
bool pc_text_is_lowercase(uint32_t cp);
bool pc_text_is_uppercase(uint32_t cp);
/* char::is_control (general category Cc). */
bool pc_text_is_control(uint32_t cp);
/* The one-to-one part of char::to_lowercase (U+0130 maps to two chars; see pc_text_lower). */
uint32_t pc_text_to_lower1(uint32_t cp);
/* char::is_alphabetic (derived Alphabetic property; unicode_upper.c, chunk L14). */
bool pc_text_is_alphabetic(uint32_t cp);
/* char::to_uppercase: writes the 1 to 3 chars of the full mapping to out, returns the count. */
size_t pc_text_to_upper(uint32_t cp, uint32_t out[3]);

/* str::to_lowercase (full mapping; U+0130 becomes "i̇"; final sigma handled as Rust does)
 * appended to `out`. */
pc_status pc_text_lower(const char *s, size_t len, pc_buf *out, pc_error *err);
/* str::to_ascii_lowercase appended to `out`. */
pc_status pc_text_ascii_lower(const char *s, size_t len, pc_buf *out, pc_error *err);
/* ASCII case-insensitive equality of two byte strings (str::eq_ignore_ascii_case). */
bool pc_text_eq_ignore_ascii_case(const char *a, size_t alen, const char *b, size_t blen);
/* Does `hay` contain `needle` (byte substring; an empty needle is always contained)? */
bool pc_text_contains(const char *hay, size_t hlen, const char *needle);
bool pc_text_starts_with(const char *s, size_t len, const char *prefix);
bool pc_text_ends_with(const char *s, size_t len, const char *suffix);
/* Unicode-lowercase comparison key order (Rust `sort_by_key(|s| s.to_lowercase())`): compares
 * the full lowercase forms byte-wise. Falls back to plain byte order if memory runs out. */
int pc_text_cmp_lower(const char *a, size_t alen, const char *b, size_t blen);

#endif
