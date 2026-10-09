/* POSIX file I/O that handles EINTR and partial transfers, bounded whole-file reads, and
 * crash-safe file replacement. Not available to code that must run without a file system
 * (none in the C tree; the web build is dropped, REWRITE_PLAN.md D2). Thread-safe. */
#ifndef PC_BASE_FILE_H
#define PC_BASE_FILE_H

#include <stddef.h>

#include "buf.h"
#include "status.h"

/* Read up to len bytes, retrying on EINTR and short reads until len bytes or end of file.
 * *got receives the number of bytes read (less than len only at EOF). */
pc_status pc_fd_read(int fd, void *buf, size_t len, size_t *got, pc_error *err);

/* Write all len bytes, retrying on EINTR and short writes. */
pc_status pc_fd_write(int fd, const void *buf, size_t len, pc_error *err);

/* Read from fd until EOF. More than max bytes -> PC_ERR_LIMIT. On success *out is replaced
 * (its old contents freed); on failure *out is unchanged. */
pc_status pc_fd_read_all(int fd, size_t max, pc_buf *out, pc_error *err);

/* Read a whole regular file (at most max bytes) into *out, with the same contract as
 * pc_fd_read_all. Directories and oversize files are rejected before reading. */
pc_status pc_file_read(const char *path, size_t max, pc_buf *out, pc_error *err);

/* Create a directory and its missing parents (mode 0777 & ~umask). Existing directories are fine. */
pc_status pc_mkdir_p(const char *path, pc_error *err);

/* Atomically replace (or create) `path` with `len` bytes of `data`.
 *
 * Writes a hidden temporary file in the same directory, fsyncs it, copies the old file's mode,
 * renames it over the destination, then fsyncs the directory (best effort). The destination is
 * never truncated: on any failure it keeps its previous contents and the temporary file is
 * removed. A symlink destination is written through (the link is kept). Missing parent
 * directories are created. Error messages end with "the original file was not changed". */
pc_status pc_file_write_atomic(const char *path, const void *data, size_t len, pc_error *err);

#endif
