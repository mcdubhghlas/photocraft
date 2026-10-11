/* Reading a whole file to open it: the read side of atomic.h (port of crates/format/src/read.rs).
 *
 * What it adds over a plain read loop, for multi-gigabyte PSB files:
 *   - bounded reads: each read(2) asks for at most PC_READ_CHUNK bytes (some file systems and
 *     network redirectors reject huge single reads);
 *   - a clear out-of-memory error: the buffer is reserved up front from the file's length, and a
 *     file larger than the memory available says how much it needed
 *     ("not enough memory to read the whole file (4.75 GB)", PC_ERR_NOMEM).
 * The size hint only sizes the buffer: a file that grows or shrinks while being read is still
 * read to its end. Errors: PC_ERR_NOT_FOUND / PC_ERR_IO with errno and the system's text (the
 * reference's io::Error; callers prefix the path). Thread-safe. */
#ifndef PC_FORMAT_READ_H
#define PC_FORMAT_READ_H

#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"

/* Largest single read: big enough that a 5 GB file takes under a hundred reads. */
#define PC_READ_CHUNK ((size_t)64 << 20)

/* Largest buffer reserved up front (1 TiB; more than any machine this runs on can hold, and the
 * sanitizers' allocation limit): a larger length is the out-of-memory error. */
#define PC_READ_MAX_RESERVE ((uint64_t)1 << 40)

/* Read all of `path`, in reads of at most PC_READ_CHUNK bytes. *out is replaced on success. */
pc_status pc_read_file(const char *path, pc_buf *out, pc_error *err);
/* pc_read_file with a chosen read size (0 counts as 1). */
pc_status pc_read_file_with(const char *path, size_t chunk, pc_buf *out, pc_error *err);

/* Read an open descriptor to its end (read::read_all); size_hint (the file's length) sizes the
 * buffer up front. */
pc_status pc_read_all(int fd, uint64_t size_hint, pc_buf *out, pc_error *err);
pc_status pc_read_all_with(int fd, uint64_t size_hint, size_t chunk, pc_buf *out, pc_error *err);

/* An empty buffer with room for len bytes (*out replaced), or PC_ERR_NOMEM saying how much was
 * needed. */
pc_status pc_read_buffer_for(uint64_t len, pc_buf *out, pc_error *err);

/* len bytes for people, as the OS shows them: "512 bytes", "4.75 GB", "759.00 MB". */
void pc_read_size_text(uint64_t len, char out[48]);

#endif
