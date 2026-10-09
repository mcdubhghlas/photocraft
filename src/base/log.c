#include "log.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -1 = not yet read from the environment. */
static atomic_int log_level = -1;
static _Atomic(pc_log_sink) log_sink;

static int env_level(void)
{
    const char *v = getenv("PHOTOCRAFT_LOG");
    if (v == NULL) {
        return PC_LOG_WARN;
    }
    if (strcmp(v, "error") == 0) { return PC_LOG_ERROR; }
    if (strcmp(v, "info") == 0) { return PC_LOG_INFO; }
    if (strcmp(v, "debug") == 0) { return PC_LOG_DEBUG; }
    return PC_LOG_WARN;
}

static const char *level_name(pc_log_level level)
{
    switch (level) {
    case PC_LOG_ERROR: return "error";
    case PC_LOG_WARN: return "warn";
    case PC_LOG_INFO: return "info";
    case PC_LOG_DEBUG: return "debug";
    }
    return "log";
}

void pc_log_set_level(pc_log_level level)
{
    atomic_store(&log_level, (int)level);
}

void pc_log_set_sink(pc_log_sink sink)
{
    atomic_store(&log_sink, sink);
}

void pc_log(pc_log_level level, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    int cur = atomic_load(&log_level);
    pc_log_sink sink;

    if (cur < 0) {
        int expected = -1;
        cur = env_level();
        /* First caller wins; a concurrent pc_log_set_level also wins over the environment. */
        if (!atomic_compare_exchange_strong(&log_level, &expected, cur)) {
            cur = expected;
        }
    }
    if ((int)level > cur) {
        return;
    }
    va_start(ap, fmt);
    if (vsnprintf(msg, sizeof msg, fmt, ap) < 0) {
        msg[0] = '\0';
    }
    va_end(ap);
    sink = atomic_load(&log_sink);
    if (sink != NULL) {
        sink(level, msg);
    } else {
        (void)fprintf(stderr, "photocraft %s: %s\n", level_name(level), msg);
    }
}
