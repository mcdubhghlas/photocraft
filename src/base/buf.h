/* pc_buf: a growable byte buffer that doubles as a string builder.
 *
 * Invariant: when data != NULL, data[len] == '\0', so the contents can always be used as a C
 * string (binary data may contain earlier NULs; use len). The buffer owns `data`; release it with
 * pc_buf_fini or take it with pc_buf_take. A zero-initialised pc_buf (PC_BUF_INIT) is empty and
 * valid. Not thread-safe: one owner at a time. */
#ifndef PC_BASE_BUF_H
#define PC_BASE_BUF_H

#include <stddef.h>
#include <stdint.h>

#include "status.h"

typedef struct pc_buf {
    uint8_t *data; /* NULL until the first append */
    size_t len;    /* bytes of content, excluding the terminator */
    size_t cap;    /* bytes allocated, including room for the terminator */
} pc_buf;

#define PC_BUF_INIT {NULL, 0, 0}

void pc_buf_init(pc_buf *b);

/* Free the contents and reset to empty. b may be NULL. */
void pc_buf_fini(pc_buf *b);

/* Ensure room for `extra` more bytes (plus terminator) without reallocating. */
pc_status pc_buf_reserve(pc_buf *b, size_t extra, pc_error *err);

/* Append bytes. On failure the buffer is unchanged. */
pc_status pc_buf_append(pc_buf *b, const void *data, size_t len, pc_error *err);
pc_status pc_buf_append_byte(pc_buf *b, uint8_t byte, pc_error *err);
pc_status pc_buf_append_str(pc_buf *b, const char *s, pc_error *err);

/* Append printf-formatted text. On failure the buffer is unchanged. */
pc_status pc_buf_appendf(pc_buf *b, pc_error *err, const char *fmt, ...);

/* Shrink to `len` bytes (no-op if len >= b->len). */
void pc_buf_truncate(pc_buf *b, size_t len);

/* The contents as a C string; "" when empty. Borrowed: valid until the next mutation. */
const char *pc_buf_cstr(const pc_buf *b);

/* Transfer ownership of the contents to the caller (free() it); the buffer becomes empty.
 * Returns NULL for an empty buffer that never allocated. *len_out (if non-NULL) gets the length. */
uint8_t *pc_buf_take(pc_buf *b, size_t *len_out);

#endif
