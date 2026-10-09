/* UTF-8 helpers matching the reference's Rust string behaviour. Thread-safe. */
#ifndef PC_BASE_UTF8_H
#define PC_BASE_UTF8_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "buf.h"
#include "status.h"

/* Decode one scalar value at s[*i] (strict UTF-8: no overlongs, surrogates or > U+10FFFF).
 * On success advances *i and returns true; on malformed input returns false, *i unchanged. */
bool pc_utf8_next(const uint8_t *s, size_t len, size_t *i, uint32_t *cp);

/* Append cp as UTF-8 (cp must be a scalar value; others become U+FFFD). */
pc_status pc_utf8_put(pc_buf *out, uint32_t cp, pc_error *err);

/* Unicode White_Space (Rust's char::is_whitespace). */
bool pc_unicode_is_space(uint32_t cp);

/* Append bytes as valid UTF-8 like String::from_utf8_lossy: every maximal invalid subpart
 * becomes one U+FFFD. */
pc_status pc_utf8_lossy(const uint8_t *bytes, size_t len, pc_buf *out, pc_error *err);

/* Append `n` UTF-16 code units like String::from_utf16_lossy (unpaired surrogates become
 * U+FFFD). Units are read big-endian from `be` (2 * n bytes). */
pc_status pc_utf16be_lossy(const uint8_t *be, size_t n, pc_buf *out, pc_error *err);

/* str::trim: the span of valid UTF-8 text s[0..len) without leading/trailing Unicode white
 * space. Returns the trimmed length and sets *start. */
size_t pc_utf8_trim(const char *s, size_t len, size_t *start);

/* Rust's `{:?}` of String::from_utf8_lossy(b): quoted, with \" \\ \0 \t \n \r and \u{x} escapes
 * for other ASCII controls. Written to out (NUL-terminated, truncated to cap). */
void pc_utf8_debug_quote(const uint8_t *b, size_t len, char *out, size_t cap);

#endif
