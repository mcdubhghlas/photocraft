/* craft-fonts at run time (see craft_fonts.h). */
#define _XOPEN_SOURCE 700 /* realpath (XSI), as build.rs canonicalises font paths */

#include "text/craft_fonts.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/file.h"
#include "base/log.h"
#include "base/mem.h"
#include "base/utf8.h"
#include "text/unicode.h"

/* Bounds for a hostile or broken checkout. */
#define MAX_MANIFEST ((size_t)1 << 20)
#define MAX_FONT_FILE ((size_t)256 << 20)
#define MAX_FONTS 256

void pc_craft_fonts_free(pc_craft_font *f, size_t n)
{
    size_t i, k;
    if (f == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        free(f[i].family);
        free(f[i].style);
        for (k = 0; k < f[i].nscripts; k++) {
            free(f[i].scripts[k]);
        }
        free(f[i].scripts);
        pc_blob_release(f[i].bytes);
    }
    free(f);
}

/* The trimmed field [s, e) as a new string. */
static char *dup_trim(const char *s, size_t len)
{
    size_t start, n = pc_utf8_trim(s, len, &start);
    return pc_strndup(s + start, n);
}

static pc_status parse_scripts(const char *s, size_t len, pc_craft_font *f, pc_error *err)
{
    size_t i = 0;
    for (;;) {
        size_t e = i;
        char **g, *one;
        size_t cap = f->nscripts;
        while (e < len && s[e] != ',') {
            e++;
        }
        one = dup_trim(s + i, e - i);
        g = one != NULL ? pc_grow_array(f->scripts, &cap, f->nscripts + 1, sizeof *g) : NULL;
        if (g == NULL) {
            free(one);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        f->scripts = g;
        f->scripts[f->nscripts++] = one;
        if (e >= len) {
            return PC_OK;
        }
        i = e + 1;
    }
}

/* One manifest line (already trimmed, non-empty, not a comment). */
static pc_status parse_line(const char *dir, const char *line, size_t len, pc_craft_font *f, pc_error *err)
{
    const char *fields[4];
    size_t flen[4], nf = 0, i = 0, start = 0;
    char path_in[PATH_MAX], *real;
    pc_buf bytes = PC_BUF_INIT;
    pc_status st;
    char *file;
    memset(f, 0, sizeof *f);
    /* split(" | "): fields separated by exactly space, bar, space. */
    while (nf < 4) {
        bool found = false;
        for (i = start; i + 3 <= len; i++) {
            if (line[i] == ' ' && line[i + 1] == '|' && line[i + 2] == ' ') {
                found = true;
                break;
            }
        }
        fields[nf] = line + start;
        flen[nf] = found ? i - start : len - start;
        nf++;
        if (!found) {
            break;
        }
        start = i + 3;
    }
    if (nf < 4) {
        return pc_fail(err, PC_ERR_FORMAT, "malformed manifest line: %.*s", (int)(len > 200 ? 200 : len),
                       line);
    }
    f->family = dup_trim(fields[0], flen[0]);
    f->style = dup_trim(fields[1], flen[1]);
    file = dup_trim(fields[2], flen[2]);
    if (f->family == NULL || f->style == NULL || file == NULL) {
        free(file);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (snprintf(path_in, sizeof path_in, "%s/%s", dir, file) >= (int)sizeof path_in) {
        st = pc_fail(err, PC_ERR_LIMIT, "%s: path too long", file);
        free(file);
        return st;
    }
    real = realpath(path_in, NULL);
    if (real == NULL) {
        st = pc_fail_errno(err, PC_ERR_IO, errno, "%s", file);
        free(file);
        return st;
    }
    st = pc_file_read(real, MAX_FONT_FILE, &bytes, err);
    free(real);
    if (st == PC_OK) {
        st = pc_blob_new(bytes.data, bytes.len, &f->bytes, err);
    }
    pc_buf_fini(&bytes);
    free(file);
    if (st == PC_OK) {
        /* The scripts field runs to the next separator (or the end). */
        st = parse_scripts(fields[3], flen[3], f, err);
    }
    return st;
}

pc_status pc_craft_fonts_load_dir(const char *dir, pc_craft_font **out, size_t *n, pc_error *err)
{
    pc_buf manifest = PC_BUF_INIT, path = PC_BUF_INIT;
    pc_craft_font *v = NULL;
    size_t count = 0, cap = 0, i = 0;
    pc_status st;
    if (dir == NULL || out == NULL || n == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "craft-fonts: no directory");
    }
    if (pc_buf_appendf(&path, err, "%s/fonts/manifest.txt", dir) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    st = pc_file_read(pc_buf_cstr(&path), MAX_MANIFEST, &manifest, err);
    if (st != PC_OK) {
        pc_error e2;
        (void)pc_fail(&e2, st, "%s: %s", pc_buf_cstr(&path), err != NULL ? err->msg : "unreadable");
        if (err != NULL) {
            *err = e2;
        }
        pc_buf_fini(&path);
        return st;
    }
    pc_buf_fini(&path);
    while (i < manifest.len) {
        const char *line = (const char *)manifest.data + i;
        size_t e = i, start, len;
        while (e < manifest.len && manifest.data[e] != '\n') {
            e++;
        }
        len = e - i;
        if (len > 0 && line[len - 1] == '\r') {
            len--; /* str::lines strips "\r\n" */
        }
        i = e + 1;
        len = pc_utf8_trim(line, len, &start);
        line += start;
        if (len == 0 || line[0] == '#') {
            continue;
        }
        if (count >= MAX_FONTS) {
            st = pc_fail(err, PC_ERR_LIMIT, "too many fonts in the manifest");
            break;
        }
        {
            pc_craft_font *g = pc_grow_array(v, &cap, count + 1, sizeof *g);
            if (g == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
                break;
            }
            v = g;
        }
        st = parse_line(dir, line, len, &v[count], err);
        count++; /* parse_line leaves a releasable entry even on failure */
        if (st != PC_OK) {
            break;
        }
    }
    pc_buf_fini(&manifest);
    if (st != PC_OK) {
        pc_craft_fonts_free(v, count);
        return st;
    }
    *out = v;
    *n = count;
    return PC_OK;
}

static pthread_once_t once = PTHREAD_ONCE_INIT;
static pc_craft_font *fonts;
static size_t nfonts;
static char load_error[PC_ERROR_MSG_MAX];

static void load(void)
{
    const char *dir = getenv("CRAFT_FONTS_DIR");
    pc_error e;
#ifdef PC_CRAFT_FONTS_DIR
    if (dir == NULL || dir[0] == '\0') {
        dir = PC_CRAFT_FONTS_DIR;
    }
#endif
    if (dir == NULL || dir[0] == '\0') {
        return;
    }
    if (pc_craft_fonts_load_dir(dir, &fonts, &nfonts, &e) != PC_OK) {
        (void)snprintf(load_error, sizeof load_error, "%s", e.msg);
        pc_log(getenv("CRAFT_FONTS_REQUIRED") != NULL ? PC_LOG_ERROR : PC_LOG_WARN,
               "running without craft-fonts: CRAFT_FONTS_DIR=%s: %s", dir, e.msg);
        fonts = NULL;
        nfonts = 0;
    }
}

const pc_craft_font *pc_craft_fonts(size_t *n)
{
    (void)pthread_once(&once, load);
    if (n != NULL) {
        *n = nfonts;
    }
    return fonts;
}

const char *pc_craft_fonts_error(void)
{
    (void)pthread_once(&once, load);
    return load_error;
}

bool pc_craft_font_is_japanese(const pc_craft_font *f)
{
    size_t i;
    for (i = 0; i < f->nscripts; i++) {
        if (strcmp(f->scripts[i], "Jpan") == 0) {
            return true;
        }
    }
    return false;
}

bool pc_craft_font_is_mincho(const pc_craft_font *f)
{
    return strstr(f->family, "Mincho") != NULL;
}

/* Sort key of japanese_for_ui: (family != UI family, style != "Regular"), stable. */
static int ui_key(const pc_craft_font *f)
{
    return (strcmp(f->family, PC_CRAFT_UI_JAPANESE_FAMILY) != 0 ? 2 : 0) +
           (strcmp(f->style, "Regular") != 0 ? 1 : 0);
}

pc_status pc_craft_japanese_for_ui(const pc_craft_font *list, size_t n, const pc_craft_font ***out,
                                   size_t *count, pc_error *err)
{
    const pc_craft_font **v = pc_malloc_array(n > 0 ? n : 1, sizeof *v);
    size_t m = 0, i;
    int key;
    if (v == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    /* Stable: bucket by key in order. */
    for (key = 0; key < 4; key++) {
        for (i = 0; i < n; i++) {
            if (pc_craft_font_is_japanese(&list[i]) && ui_key(&list[i]) == key) {
                v[m++] = &list[i];
            }
        }
    }
    *out = v;
    *count = m;
    return PC_OK;
}

pc_status pc_craft_japanese_families(const pc_craft_font *list, size_t n, const char ***out, size_t *count,
                                     pc_error *err)
{
    const pc_craft_font **ui;
    const char **v;
    size_t nui, m = 0, i, k, w = 0;
    pc_status st = pc_craft_japanese_for_ui(list, n, &ui, &nui, err);
    if (st != PC_OK) {
        return st;
    }
    v = pc_malloc_array(nui > 0 ? nui : 1, sizeof *v);
    if (v == NULL) {
        free(ui);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < nui; i++) {
        bool seen = false;
        for (k = 0; k < m && !seen; k++) {
            seen = strcmp(v[k], ui[i]->family) == 0;
        }
        if (!seen) {
            v[m++] = ui[i]->family;
        }
    }
    free(ui);
    /* Stable sort by contains("Mincho"): non-Mincho first. */
    {
        const char **tmp = pc_malloc_array(m > 0 ? m : 1, sizeof *tmp);
        if (tmp == NULL) {
            free(v);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        for (i = 0; i < m; i++) {
            if (strstr(v[i], "Mincho") == NULL) {
                tmp[w++] = v[i];
            }
        }
        for (i = 0; i < m; i++) {
            if (strstr(v[i], "Mincho") != NULL) {
                tmp[w++] = v[i];
            }
        }
        memcpy(v, tmp, m * sizeof *v);
        free(tmp);
    }
    *out = v;
    *count = m;
    return PC_OK;
}

static bool in_list(const char *s, const char *const *craft, size_t ncraft)
{
    size_t i;
    for (i = 0; i < ncraft; i++) {
        if (strcmp(s, craft[i]) == 0) {
            return true;
        }
    }
    return false;
}

void pc_craft_mincho_first(const char **fallback, size_t n, const char *const *craft, size_t ncraft)
{
    size_t i, k = 0;
    const char *group[64] = {NULL};
    size_t slots[64] = {0}, ns = 0;
    /* Slots holding craft families, and those families with Mincho first (stable). Fallback
     * lists are short; only the first 64 craft slots take part. */
    for (i = 0; i < n && ns < 64; i++) {
        if (in_list(fallback[i], craft, ncraft)) {
            slots[ns++] = i;
        }
    }
    for (i = 0; i < ns; i++) {
        if (strstr(fallback[slots[i]], "Mincho") != NULL) {
            group[k++] = fallback[slots[i]];
        }
    }
    for (i = 0; i < ns; i++) {
        if (strstr(fallback[slots[i]], "Mincho") == NULL) {
            group[k++] = fallback[slots[i]];
        }
    }
    for (i = 0; i < ns && i < k; i++) {
        fallback[slots[i]] = group[i];
    }
}

bool pc_craft_is_serif_family(const char *name, size_t len)
{
    static const char *const SERIF[] = {
        "serif",  "mincho", "minchō",   "song",     "ming",         "myeongjo",    "myungjo",
        "batang", "times",  "georgia",  "garamond", "minion",       "baskerville", "caslon",
        "bodoni", "didot",  "palatino", "cambria",  "book antiqua", "century",     "hoefler"};
    pc_buf b = PC_BUF_INIT;
    size_t i;
    bool res = false;
    /* to_ascii_lowercase: non-ASCII letters (Ō) keep their case. */
    if (pc_text_ascii_lower(name, len, &b, NULL) != PC_OK) {
        pc_buf_fini(&b);
        return false;
    }
    if (pc_text_contains(pc_buf_cstr(&b), b.len, "sans") ||
        pc_text_contains(pc_buf_cstr(&b), b.len, "gothic")) {
        pc_buf_fini(&b);
        return false;
    }
    for (i = 0; i < sizeof SERIF / sizeof SERIF[0] && !res; i++) {
        res = pc_text_contains(pc_buf_cstr(&b), b.len, SERIF[i]);
    }
    pc_buf_fini(&b);
    return res;
}
