/* See logging.h. Behavioural port of apps/photocraft/src/logging.rs on top of base/log.c. */
#include "logging.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_dirs.h"
#include "base/file.h"

/* Days since 1970-01-01 to a proleptic Gregorian (year, month, day) (Howard Hinnant's
 * civil_from_days, unsigned because the caller clamps at the epoch). */
static void civil_from_days(uint64_t days, uint64_t *y, uint64_t *m, uint64_t *d)
{
    uint64_t z = days + 719468;
    uint64_t era = z / 146097;
    uint64_t doe = z % 146097;
    uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint64_t mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yoe + era * 400 + (*m <= 2 ? 1 : 0);
}

void pc_app_log_timestamp(uint64_t millis_since_epoch, char out[25])
{
    uint64_t secs = millis_since_epoch / 1000;
    uint64_t millis = millis_since_epoch % 1000;
    uint64_t days = secs / 86400;
    uint64_t rem = secs % 86400;
    uint64_t y, m, d;
    civil_from_days(days, &y, &m, &d);
    /* Clamp to their real ranges so the fixed-width field cannot overflow out[25]. */
    if (y > 9999) { y = 9999; }
    if (m > 12) { m = 12; }
    if (d > 31) { d = 31; }
    (void)snprintf(out, 25, "%04llu-%02llu-%02lluT%02llu:%02llu:%02llu.%03lluZ",
                   (unsigned long long)y, (unsigned long long)m, (unsigned long long)d,
                   (unsigned long long)(rem / 3600), (unsigned long long)(rem % 3600 / 60),
                   (unsigned long long)(rem % 60), (unsigned long long)millis);
}

static const char *level_name(pc_log_level level)
{
    switch (level) {
    case PC_LOG_ERROR: return "ERROR";
    case PC_LOG_WARN: return "WARN ";
    case PC_LOG_INFO: return "INFO ";
    case PC_LOG_DEBUG: return "DEBUG";
    }
    return "LOG  ";
}

void pc_app_log_format_line(char *out, size_t cap, const char *ts, pc_log_level level, const char *message)
{
    if (cap == 0) {
        return;
    }
    /* Strip one trailing newline from the message; the line ends in exactly one. */
    size_t len = message ? strlen(message) : 0;
    if (len > 0 && message[len - 1] == '\n') {
        len--;
    }
    int n = snprintf(out, cap, "%s %s %.*s\n", ts ? ts : "", level_name(level), (int)len,
                     message ? message : "");
    if (n < 0) {
        out[0] = '\0';
    }
}

static void numbered(char *out, size_t cap, const char *dir, size_t n)
{
    if (n == 0) {
        (void)snprintf(out, cap, "%s/%s", dir, PC_APP_LOG_FILE);
    } else {
        (void)snprintf(out, cap, "%s/photocraft.%zu.log", dir, n);
    }
}

pc_status pc_app_log_rotate(const char *dir, char *out_path, size_t cap, FILE **out_file, pc_error *err)
{
    if (!dir || !out_path || cap == 0 || !out_file) {
        return pc_fail(err, PC_ERR_INVALID, "log rotate: bad arguments");
    }
    if (pc_mkdir_p(dir, err) != PC_OK) {
        return err ? err->code : PC_ERR_IO;
    }
    for (size_t n = PC_APP_LOG_KEEP; n >= 1; n--) {
        char from[PC_APP_PATH_MAX], to[PC_APP_PATH_MAX];
        numbered(from, sizeof from, dir, n - 1);
        numbered(to, sizeof to, dir, n);
        (void)rename(from, to); /* NotFound is fine; a real failure surfaces at create below */
    }
    char path[PC_APP_PATH_MAX];
    numbered(path, sizeof path, dir, 0);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return pc_fail_errno(err, PC_ERR_IO, errno, "%s", path);
    }
    int m = snprintf(out_path, cap, "%s", path);
    if (m < 0 || (size_t)m >= cap) {
        (void)fclose(f);
        return pc_fail(err, PC_ERR_LIMIT, "log path too long");
    }
    *out_file = f;
    return PC_OK;
}

static pthread_mutex_t *sink_lock(pc_app_log_sink *s)
{
    return (pthread_mutex_t *)s->lock;
}

void pc_app_log_sink_init(pc_app_log_sink *s, uint64_t max_bytes)
{
    memset(s, 0, sizeof *s);
    s->max_bytes = max_bytes;
    pthread_mutex_t *lk = calloc(1, sizeof *lk);
    if (lk) {
        (void)pthread_mutex_init(lk, NULL);
    }
    s->lock = lk;
}

void pc_app_log_sink_fini(pc_app_log_sink *s)
{
    if (!s) {
        return;
    }
    for (size_t i = 0; i < s->n_pending; i++) {
        free(s->pending[i]);
    }
    s->n_pending = 0;
    if (s->file) {
        (void)fclose(s->file);
        s->file = NULL;
    }
    if (s->lock) {
        (void)pthread_mutex_destroy(sink_lock(s));
        free(s->lock);
        s->lock = NULL;
    }
}

/* Append `line` to the open file, honouring the size cap. Caller holds the lock. */
static void sink_append(pc_app_log_sink *s, const char *line)
{
    if (s->capped || !s->file) {
        return;
    }
    size_t len = strlen(line);
    const char *text = line;
    char note[128];
    if (s->written + len > s->max_bytes) {
        s->capped = true;
        (void)snprintf(note, sizeof note,
                       "log file reached %llu bytes; later records go to standard error only\n",
                       (unsigned long long)s->max_bytes);
        text = note;
        len = strlen(note);
    } else {
        s->written += len;
    }
    if (fwrite(text, 1, len, s->file) != len || fflush(s->file) != 0) {
        /* A full disk or a vanished file must not take the app down: drop the file sink. */
        (void)fclose(s->file);
        s->file = NULL;
        s->capped = true;
    }
}

void pc_app_log_sink_write(pc_app_log_sink *s, const char *line)
{
    if (!s || !line) {
        return;
    }
    if (s->lock) {
        (void)pthread_mutex_lock(sink_lock(s));
    }
    if (!s->file) {
        if (s->n_pending < PC_APP_LOG_MAX_PENDING) {
            char *copy = strdup(line);
            if (copy) {
                s->pending[s->n_pending++] = copy;
            } else {
                s->dropped++;
            }
        } else {
            s->dropped++;
        }
    } else {
        sink_append(s, line);
    }
    if (s->lock) {
        (void)pthread_mutex_unlock(sink_lock(s));
    }
}

void pc_app_log_sink_attach(pc_app_log_sink *s, FILE *file)
{
    if (!s) {
        if (file) {
            (void)fclose(file);
        }
        return;
    }
    if (s->lock) {
        (void)pthread_mutex_lock(sink_lock(s));
    }
    if (s->file) {
        (void)fclose(s->file);
    }
    s->file = file;
    for (size_t i = 0; i < s->n_pending; i++) {
        sink_append(s, s->pending[i]);
        free(s->pending[i]);
        s->pending[i] = NULL;
    }
    s->n_pending = 0;
    if (s->dropped > 0) {
        char note[128];
        (void)snprintf(note, sizeof note,
                       "%zu earlier log lines were dropped before the log file was opened\n",
                       s->dropped);
        s->dropped = 0;
        sink_append(s, note);
    }
    if (s->lock) {
        (void)pthread_mutex_unlock(sink_lock(s));
    }
}

/* ---- the installed global logger ---- */

static pc_app_log_sink g_sink;
static bool g_installed = false;

static uint64_t now_millis(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || ts.tv_sec < 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static void log_cb(pc_log_level level, const char *msg)
{
    char ts[25];
    char line[1200];
    pc_app_log_timestamp(now_millis(), ts);
    pc_app_log_format_line(line, sizeof line, ts, level, msg);
    (void)fputs(line, stderr);
    pc_app_log_sink_write(&g_sink, line);
}

void pc_app_logging_install(void)
{
    if (g_installed) {
        return;
    }
    pc_app_log_sink_init(&g_sink, PC_APP_LOG_MAX_FILE_BYTES);
    pc_log_set_sink(log_cb);
    g_installed = true;
}

pc_status pc_app_logging_attach_dir(const char *dir, char *out_path, size_t cap, pc_error *err)
{
    if (!g_installed) {
        pc_app_logging_install();
    }
    FILE *f = NULL;
    pc_status rc = pc_app_log_rotate(dir, out_path, cap, &f, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_app_log_sink_attach(&g_sink, f);
    return PC_OK;
}
