/* The desktop app's log file: pc_log records go to standard error and, once the settings directory
 * is known, to <settings dir>/logs/photocraft.log. Each start rotates the previous log to
 * photocraft.1.log (and that to .2), so a run that crashed leaves its log behind.
 *
 * Behavioural port of apps/photocraft/src/logging.rs on top of base/log.c. Differences from the
 * reference are recorded in REWRITE_PLAN.md §7: the C logger has no per-target RUST_LOG filtering
 * (the single PHOTOCRAFT_LOG level governs, as base/log.c already does) and the formatted line
 * carries no thread/module target (pc_log does not pass one). Writing never crashes: a file that
 * cannot be created or written leaves standard error as the only sink. */
#ifndef PC_APP_LOGGING_H
#define PC_APP_LOGGING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "base/log.h"
#include "base/status.h"

/* The current log file's name inside the log directory. */
#define PC_APP_LOG_FILE "photocraft.log"
/* How many previous logs are kept (photocraft.1.log .. photocraft.<KEEP>.log). */
#define PC_APP_LOG_KEEP 2
/* The log file stops growing past this size (a runaway warning cannot fill the disk). */
#define PC_APP_LOG_MAX_FILE_BYTES ((uint64_t)(16u << 20))
/* Records kept in memory until the log file is attached. */
#define PC_APP_LOG_MAX_PENDING 512

/* ---- testable pieces ---- */

/* "2026-10-08T07:59:17.728Z" (UTC, milliseconds). A clock before 1970 reads as the epoch. Writes
 * at most 25 bytes including the NUL; out must be >= 25 bytes. */
void pc_app_log_timestamp(uint64_t millis_since_epoch, char out[25]);

/* One formatted log line: "<ts> <LEVEL> <message>\n" into out[cap] (truncated to fit). */
void pc_app_log_format_line(char *out, size_t cap, const char *ts, pc_log_level level, const char *message);

/* Shift previous logs up one and open a fresh, empty log file in `dir` (created if missing). On
 * success writes the new file's path into out_path[cap] and returns the open file in *out_file. */
pc_status pc_app_log_rotate(const char *dir, char *out_path, size_t cap, FILE **out_file, pc_error *err);

/* A sink: formatted lines are kept in memory until a file is attached, then appended to it until it
 * reaches its size cap. Thread-safe. */
typedef struct pc_app_log_sink {
    FILE *file;
    char *pending[PC_APP_LOG_MAX_PENDING];
    size_t n_pending;
    size_t dropped;
    uint64_t written;
    uint64_t max_bytes;
    bool capped;
    void *lock; /* opaque pthread_mutex_t* */
} pc_app_log_sink;

void pc_app_log_sink_init(pc_app_log_sink *s, uint64_t max_bytes);
void pc_app_log_sink_fini(pc_app_log_sink *s);
void pc_app_log_sink_write(pc_app_log_sink *s, const char *line);
void pc_app_log_sink_attach(pc_app_log_sink *s, FILE *file); /* takes ownership of `file` */

/* ---- installation ---- */

/* Install the pc_log sink (standard error plus the in-memory buffer). Idempotent; safe to call once
 * at startup before the settings directory is known. */
void pc_app_logging_install(void);

/* Rotate the logs in `dir` and send records to the fresh file. On success writes the log file's
 * path into out_path[cap]. Call once the settings directory is known. */
pc_status pc_app_logging_attach_dir(const char *dir, char *out_path, size_t cap, pc_error *err);

#endif /* PC_APP_LOGGING_H */
