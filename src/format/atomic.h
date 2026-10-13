/* Crash-safe file replacement for the format module (crates/format/src/atomic.rs).
 *
 * The implementation lives in base (src/base/file.h: pc_file_write_atomic / pc_atomic_write,
 * the failure-injection seam pc_atomic_write_with over a pc_atomic_fs, pc_atomic_retry_rename,
 * pc_atomic_temp_name), so every save in the tree shares one algorithm and one set of messages.
 * This header keeps the module's include path and adds the progress-reporting write (atomic.c). */
#ifndef PC_FORMAT_ATOMIC_H
#define PC_FORMAT_ATOMIC_H

#include "base/file.h"

/* Receives the fraction (0..1] of the bytes written so far. */
typedef void (*pc_atomic_progress_fn)(void *ctx, float fraction);

/* pc_atomic_write reporting progress as it writes, once per 8 MiB chunk and 1.0 after the last
 * (a save's progress bar); the flush and rename follow the last report. progress may be NULL. */
pc_status pc_atomic_write_progress(const char *path, const void *data, size_t len,
                                   pc_atomic_progress_fn progress, void *ctx, pc_error *err);

#endif
