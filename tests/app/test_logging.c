/* logging: timestamp formatting, line formatting, rotation and the pending/cap sink. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/logging.h"
#include "harness.h"

static char *make_tmp(void)
{
    char *base = getenv("TMPDIR");
    static char path[256];
    (void)snprintf(path, sizeof path, "%s/pc-logging-XXXXXX", (base && base[0]) ? base : "/tmp");
    return mkdtemp(path);
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { return NULL; }
    (void)fseek(f, 0, SEEK_END);
    long n = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (buf) {
        size_t got = fread(buf, 1, (size_t)n, f);
        buf[got] = '\0';
        if (len) { *len = got; }
    }
    (void)fclose(f);
    return buf;
}

static void test_timestamp(void)
{
    char ts[25];
    pc_app_log_timestamp(0, ts);
    CHECK_EQ_STR(ts, "1970-01-01T00:00:00.000Z");
    pc_app_log_timestamp(1791446357728ull, ts);
    CHECK_EQ_STR(ts, "2026-10-08T07:59:17.728Z");
    pc_app_log_timestamp(1709164800000ull, ts); /* leap day */
    CHECK_EQ_STR(ts, "2024-02-29T00:00:00.000Z");
}

static void test_format_line(void)
{
    char line[128];
    pc_app_log_format_line(line, sizeof line, "2026-10-08T07:59:17.728Z", PC_LOG_WARN, "a warning");
    CHECK_EQ_STR(line, "2026-10-08T07:59:17.728Z WARN  a warning\n");
    /* One trailing newline is stripped, then re-added. */
    pc_app_log_format_line(line, sizeof line, "t", PC_LOG_ERROR, "boom\n");
    CHECK_EQ_STR(line, "t ERROR boom\n");
}

static void test_rotation(void)
{
    char *dir = make_tmp();
    CHECK(dir != NULL);
    for (int run = 1; run <= 4; run++) {
        char path[300];
        FILE *f = NULL;
        pc_error err;
        CHECK_EQ_INT(pc_app_log_rotate(dir, path, sizeof path, &f, &err), PC_OK);
        if (f) {
            (void)fprintf(f, "run %d", run);
            (void)fclose(f);
        }
    }
    char cur[300], one[300], two[300], three[300];
    (void)snprintf(cur, sizeof cur, "%s/photocraft.log", dir);
    (void)snprintf(one, sizeof one, "%s/photocraft.1.log", dir);
    (void)snprintf(two, sizeof two, "%s/photocraft.2.log", dir);
    (void)snprintf(three, sizeof three, "%s/photocraft.3.log", dir);
    char *c = slurp(cur, NULL), *o = slurp(one, NULL), *t = slurp(two, NULL);
    CHECK_EQ_STR(c, "run 4");
    CHECK_EQ_STR(o, "run 3");
    CHECK_EQ_STR(t, "run 2");
    CHECK(access(three, F_OK) != 0); /* only KEEP old logs survive */
    free(c); free(o); free(t);
}

static void test_sink_pending_then_attach(void)
{
    char *dir = make_tmp();
    CHECK(dir != NULL);
    char path[300];
    FILE *f = NULL;
    pc_error err;
    CHECK_EQ_INT(pc_app_log_rotate(dir, path, sizeof path, &f, &err), PC_OK);
    pc_app_log_sink s;
    pc_app_log_sink_init(&s, PC_APP_LOG_MAX_FILE_BYTES);
    pc_app_log_sink_write(&s, "early 1\n");
    pc_app_log_sink_write(&s, "early 2\n");
    pc_app_log_sink_attach(&s, f);
    pc_app_log_sink_write(&s, "late\n");
    pc_app_log_sink_fini(&s);
    char *text = slurp(path, NULL);
    CHECK_EQ_STR(text, "early 1\nearly 2\nlate\n");
    free(text);
}

static void test_sink_size_cap(void)
{
    char *dir = make_tmp();
    CHECK(dir != NULL);
    char path[300];
    FILE *f = NULL;
    pc_error err;
    CHECK_EQ_INT(pc_app_log_rotate(dir, path, sizeof path, &f, &err), PC_OK);
    pc_app_log_sink s;
    pc_app_log_sink_init(&s, 100);
    pc_app_log_sink_attach(&s, f);
    for (int i = 0; i < 50; i++) {
        pc_app_log_sink_write(&s, "line with some padding\n");
    }
    pc_app_log_sink_fini(&s);
    char *text = slurp(path, NULL);
    CHECK(text != NULL);
    CHECK(strstr(text, "log file reached") != NULL);
    free(text);
}

int main(int argc, char **argv)
{
    RUN(test_timestamp);
    RUN(test_format_line);
    RUN(test_rotation);
    RUN(test_sink_pending_then_attach);
    RUN(test_sink_size_cap);
    return pc_t_finish();
}
