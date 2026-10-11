/* Internal helpers shared by the manifest conversion files (convert_save.c, convert_load.c). */
#ifndef PC_FORMAT_CONVERT_H
#define PC_FORMAT_CONVERT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Lower-case hex of n bytes into out (2 * n + 1 bytes, NUL-terminated). */
void pc_format_hex(const uint8_t *bytes, size_t n, char *out);

/* convert::unhex: an even number of two-character pairs, each parsed like
 * u8::from_str_radix(pair, 16) (an optional leading '+'). Writes at most cap bytes to out
 * (pass cap 0 to only validate) and the decoded length to *n. False on bad hex. */
bool pc_format_unhex(const char *s, size_t len, uint8_t *out, size_t cap, size_t *n);

#endif
