/* Minimal leveled logging to stderr (or a caller-provided sink).
 *
 * The default level is WARN, or PHOTOCRAFT_LOG=error|warn|info|debug when set at the first log
 * call. Messages longer than 1023 bytes are truncated. Thread-safe: each message is formatted
 * into a local buffer and emitted with a single call. */
#ifndef PC_BASE_LOG_H
#define PC_BASE_LOG_H

typedef enum pc_log_level {
    PC_LOG_ERROR = 0,
    PC_LOG_WARN = 1,
    PC_LOG_INFO = 2,
    PC_LOG_DEBUG = 3
} pc_log_level;

/* Receives one formatted message (no trailing newline). Must be thread-safe. */
typedef void (*pc_log_sink)(pc_log_level level, const char *msg);

void pc_log(pc_log_level level, const char *fmt, ...);

/* Messages above `level` are dropped. Overrides PHOTOCRAFT_LOG. */
void pc_log_set_level(pc_log_level level);

/* Replace the sink; NULL restores stderr. Set it before starting threads that log. */
void pc_log_set_sink(pc_log_sink sink);

#endif
