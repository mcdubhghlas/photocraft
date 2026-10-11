/* POSIX file I/O that handles EINTR and partial transfers, bounded whole-file reads, and
 * crash-safe file replacement. Not available to code that must run without a file system
 * (none in the C tree; the web build is dropped, REWRITE_PLAN.md D2). Thread-safe. */
#ifndef PC_BASE_FILE_H
#define PC_BASE_FILE_H

#include <stddef.h>
#include <stdint.h>

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

/* ---- crash-safe file replacement (port of crates/format/src/atomic.rs) ----
 *
 * pc_file_write_atomic atomically replaces (or creates) `path` with `len` bytes of `data`. It
 * never truncates the destination: it writes a hidden temporary file in the same directory
 * (".<at most 64 characters of the name>.<pid>-<n>.pcsave.tmp"), fsyncs it, copies the old
 * file's mode onto it, renames it over the destination, then fsyncs the directory (best effort).
 * On any failure the destination keeps its previous contents, the temporary file is removed and
 * the message ends with "the original file was not changed". A cross-device rename (EXDEV) or an
 * unwritable folder fails with a clear error; it never falls back to writing in place. A symlink
 * destination is written through (the link is kept). Missing parent directories are created.
 *
 * pc_atomic_write_with runs the same algorithm through a pc_atomic_fs so tests can inject a
 * failure at each step; pc_file_write_atomic is it over pc_atomic_real_fs with the platform
 * retry policy. pc_atomic_retry_rename / pc_atomic_temp_name serve callers that rename through
 * their own handles. The reference's Windows rename retry is kept as a policy (pc_rename_retry)
 * whose POSIX default never retries: there, EACCES is a real permission problem that waiting
 * won't fix. */

/* The file-system steps of an atomic write. Each returns 0 or an errno value. */
typedef struct pc_atomic_fs {
    void *ctx;
    /* Create `tmp` (it must not exist: EEXIST otherwise), write all of data and fsync it. */
    int (*write_new)(void *ctx, const char *tmp, const void *data, size_t len);
    /* Rename `from` over `to`, replacing it. */
    int (*rename)(void *ctx, const char *from, const char *to);
    /* Remove a (temporary) file. */
    int (*remove)(void *ctx, const char *path);
    /* Flush a directory entry change. */
    int (*sync_dir)(void *ctx, const char *dir);
    /* Copy the destination's permissions onto the temporary file. Best effort. */
    int (*copy_permissions)(void *ctx, const char *from, const char *to);
} pc_atomic_fs;

/* The real file system (ctx unused). */
extern const pc_atomic_fs pc_atomic_real_fs;

/* How hard to retry a rename that fails with EACCES/EPERM. */
typedef struct pc_rename_retry {
    uint32_t attempts;       /* retries after the first attempt (0 = none) */
    uint64_t first_delay_ms; /* delay before the first retry; doubles each time */
} pc_rename_retry;

/* The platform default: no retries on POSIX systems. */
pc_rename_retry pc_rename_retry_platform(void);

pc_status pc_file_write_atomic(const char *path, const void *data, size_t len, pc_error *err);
/* The same function under the reference's name (atomic::atomic_write). */
pc_status pc_atomic_write(const char *path, const void *data, size_t len, pc_error *err);

/* pc_file_write_atomic through an explicit file system and retry policy (the failure-injection
 * seam). */
pc_status pc_atomic_write_with(const pc_atomic_fs *fs, pc_rename_retry retry, const char *path,
                               const void *data, size_t len, pc_error *err);

/* Run rename_fn(ctx) (0 or an errno), retrying with backoff while it fails with EACCES or EPERM.
 * Returns the last result. */
int pc_atomic_retry_rename(pc_rename_retry retry, int (*rename_fn)(void *ctx), void *ctx);

/* A unique hidden temporary name to write beside `file` before renaming over it:
 * ".<at most 64 characters of file>.<pid>-<n>.pcsave.tmp". *out is replaced. */
pc_status pc_atomic_temp_name(const char *file, pc_buf *out, pc_error *err);

#endif
