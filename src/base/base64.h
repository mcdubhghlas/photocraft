/* Standard base64 (RFC 4648 alphabet, '=' padding), behaving like the reference's
 * `base64::engine::general_purpose::STANDARD` (base64 0.22): encoding always pads; decoding
 * requires canonical padding, rejects non-zero trailing bits and any whitespace, and reports the
 * same error messages ("Invalid symbol 33, offset 4.", "Invalid padding", ...). Thread-safe. */
#ifndef PC_BASE_BASE64_H
#define PC_BASE_BASE64_H

#include <stddef.h>
#include <stdint.h>

#include "buf.h"
#include "status.h"

/* Append the encoding of data to *out. */
pc_status pc_base64_encode(const uint8_t *data, size_t len, pc_buf *out, pc_error *err);

/* Decode text into *out, replacing its contents. Malformed input -> PC_ERR_FORMAT with the
 * reference's message; *out is unchanged on failure. */
pc_status pc_base64_decode(const char *text, size_t len, pc_buf *out, pc_error *err);

#endif
