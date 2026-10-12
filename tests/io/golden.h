/* Reading tests/golden/io-core-vectors.txt (rustref io-core-vectors): tab-separated lines, the
 * first field naming the kind. Results that a later io chunk still stubs ("... is not ported
 * yet (I2)") count as pending instead of failing; the summary prints how many. */
#ifndef PC_TESTS_IO_GOLDEN_H
#define PC_TESTS_IO_GOLDEN_H

#include "common.h"

#ifndef GOLDEN /* a test may name another vectors file before including this header */
#define GOLDEN "tests/golden/io-core-vectors.txt"
#endif
#define MAXF 8

typedef struct gline {
    char *f[MAXF];
    size_t n;
} gline;

static gline *g_lines;
static size_t g_nlines;
static size_t g_checked;
static size_t g_pending;
static size_t g_skipped;

static inline int load_golden(void)
{
    pc_buf b = PC_BUF_INIT;
    size_t cap = 0;
    char *text;
    char *p;
    if (pc_file_read(GOLDEN, (size_t)1 << 30, &b, NULL) != PC_OK) {
        return -1;
    }
    text = (char *)pc_buf_take(&b, NULL);
    p = text;
    while (p != NULL && *p != '\0') {
        char *end = strchr(p, '\n');
        gline l;
        if (end != NULL) {
            *end = '\0';
        }
        memset(&l, 0, sizeof l);
        while (l.n < MAXF) {
            char *tab = strchr(p, '\t');
            l.f[l.n++] = p;
            if (tab == NULL) {
                break;
            }
            *tab = '\0';
            p = tab + 1;
        }
        if (g_nlines == cap) {
            gline *g;
            cap = cap ? cap * 2 : 1024;
            g = realloc(g_lines, cap * sizeof *g_lines);
            if (g == NULL) {
                return -1;
            }
            g_lines = g;
        }
        g_lines[g_nlines++] = l;
        p = end != NULL ? end + 1 : NULL;
    }
    return 0;
}

static inline bool is_kind(const gline *l, const char *kind)
{
    return strcmp(l->f[0], kind) == 0;
}

static inline const gline *find_line(const char *kind, const char *name)
{
    for (size_t i = 0; i < g_nlines; i++) {
        if (g_lines[i].n >= 2 && is_kind(&g_lines[i], kind) && strcmp(g_lines[i].f[1], name) == 0) {
            return &g_lines[i];
        }
    }
    return NULL;
}

static inline bool is_pending(pc_status st, const pc_error *e)
{
    return st == PC_ERR_UNSUPPORTED && strstr(e->msg, "is not ported yet (I") != NULL;
}

static inline pc_buf b64d(const char *s)
{
    pc_buf b = PC_BUF_INIT;
    must(pc_base64_decode(s, strlen(s), &b, NULL));
    return b;
}

static inline bool starts(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);
    return strlen(s) >= n && memcmp(s, prefix, n) == 0;
}

/* Results match: exactly; or both codec errors (library wording differs); or the C message was
 * cut at the 255-byte error buffer and the reference's starts with it. */
static inline bool same_result(const char *want, const char *got)
{
    size_t gl = strlen(got);
    if (strcmp(want, got) == 0) {
        return true;
    }
    if (starts(want, "err codec ") && starts(got, "err codec ")) {
        return true;
    }
    if (strncmp(got, "err ", 4) == 0 && gl > 200 && got[gl - 1] == '"' && strncmp(want, got, gl - 1) == 0) {
        return true;
    }
    return false;
}

/* One comparison: a mismatch fails the check with both texts (cut). */
#define CHECK_RESULT(what, want, got)                                                                        \
    do {                                                                                                     \
        g_checked++;                                                                                         \
        pc_t_checks++;                                                                                       \
        if (!same_result((want), (got))) {                                                                   \
            PC_T_FAIL("%s:\n  want %.900s\n  got  %.900s", (what), (want), (got));                           \
        }                                                                                                    \
    } while (0)

static inline void golden_summary(const char *what)
{
    (void)printf("%s: %zu golden lines, %zu compared, %zu pending a later io chunk, %zu skipped\n", what,
                 g_nlines, g_checked, g_pending, g_skipped);
}

#endif
