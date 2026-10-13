#define _XOPEN_SOURCE 700 /* realpath (D21) */
/* System font discovery (port of the platform half of crates/text/src/fonts.rs): the platform
 * font directories, Fontconfig's file list on Linux, and loading what they name. See fonts.h. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "base/file.h"
#include "base/mem.h"
#include "text/fonts_internal.h"
#include "text/unicode.h"

extern char **environ;

/* Largest font file read from disk; collections of CJK fonts are tens of MB. */
#define MAX_FONT_FILE ((size_t)1 << 30)

static pc_status push_dir(pc_strlist *l, const char *a, const char *b, pc_error *err)
{
    pc_buf p = PC_BUF_INIT;
    pc_status st = pc_buf_appendf(&p, err, "%s%s", a, b);
    if (st == PC_OK) {
        st = pc_strlist_push(l, pc_buf_cstr(&p), p.len, err);
    }
    pc_buf_fini(&p);
    return st;
}

pc_status pc_text_system_font_dirs(pc_strlist *out, pc_error *err)
{
    const char *home = getenv("HOME");
    pc_strlist l = {NULL, 0};
    pc_status st;
#ifdef __APPLE__
    st = push_dir(&l, "/System/Library/Fonts", "", err);
    if (st == PC_OK) {
        st = push_dir(&l, "/Library/Fonts", "", err);
    }
    if (st == PC_OK && home != NULL) {
        st = push_dir(&l, home, "/Library/Fonts", err);
    }
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    st = push_dir(&l, "/usr/share/fonts", "", err);
    if (st == PC_OK) {
        st = push_dir(&l, "/usr/local/share/fonts", "", err);
    }
    /* Inside a Flatpak sandbox the host's system and per-user fonts are mounted here. */
    if (st == PC_OK) {
        st = push_dir(&l, "/run/host/fonts", "", err);
    }
    if (st == PC_OK) {
        st = push_dir(&l, "/run/host/user-fonts", "", err);
    }
    if (st == PC_OK && xdg != NULL) {
        st = push_dir(&l, xdg, "/fonts", err);
    }
    if (st == PC_OK && home != NULL) {
        st = push_dir(&l, home, "/.local/share/fonts", err);
    }
    if (st == PC_OK && home != NULL) {
        st = push_dir(&l, home, "/.fonts", err);
    }
#endif
    if (st != PC_OK) {
        pc_strlist_fini(&l);
        return st;
    }
    *out = l;
    return PC_OK;
}

/* Path::extension of the last component, compared case-insensitively with font extensions. */
static bool font_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot == name) {
        return false;
    }
    return pc_text_eq_ignore_ascii_case(dot + 1, strlen(dot + 1), "ttf", 3) ||
           pc_text_eq_ignore_ascii_case(dot + 1, strlen(dot + 1), "otf", 3) ||
           pc_text_eq_ignore_ascii_case(dot + 1, strlen(dot + 1), "ttc", 3) ||
           pc_text_eq_ignore_ascii_case(dot + 1, strlen(dot + 1), "otc", 3);
}

static pc_status collect(const char *dir, unsigned depth, pc_strlist *out, pc_error *err)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    pc_status st = PC_OK;
    if (d == NULL) {
        return PC_OK;
    }
    while (st == PC_OK && (e = readdir(d)) != NULL) {
        pc_buf p = PC_BUF_INIT;
        struct stat sb;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        st = pc_buf_appendf(&p, err, "%s/%s", dir, e->d_name);
        if (st != PC_OK) {
            pc_buf_fini(&p);
            break;
        }
        if (stat(pc_buf_cstr(&p), &sb) == 0 && S_ISDIR(sb.st_mode)) {
            /* Depth-limited, so symlink loops end. */
            if (depth < 8) {
                st = collect(pc_buf_cstr(&p), depth + 1, out, err);
            }
        } else if (font_ext(e->d_name)) {
            st = pc_strlist_push(out, pc_buf_cstr(&p), p.len, err);
        }
        pc_buf_fini(&p);
    }
    closedir(d);
    return st;
}

pc_status pc_text_collect_font_files(const char *dir, pc_strlist *out, pc_error *err)
{
    if (dir == NULL || out == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "fonts: invalid arguments");
    }
    return collect(dir, 0, out, err);
}

pc_status pc_fontdb_add_font_file(pc_fontdb *db, const char *path, pc_error *err)
{
    pc_buf bytes = PC_BUF_INIT;
    size_t src;
    pc_status st;
    pc_error e;
    if (db == NULL || path == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "fonts: invalid arguments");
    }
    st = pc_file_read(path, MAX_FONT_FILE, &bytes, &e);
    if (st != PC_OK) {
        pc_buf_fini(&bytes);
        if (st == PC_ERR_NOMEM) {
            return pc_fail(err, PC_ERR_NOMEM, "%s", e.msg);
        }
        return PC_OK; /* unreadable files are skipped, as fontique's scan does */
    }
    st = pc_fontdb_add_source(db, path, NULL, &src, err);
    if (st == PC_OK) {
        st = pc_fontdb_scan_source(db, src, bytes.data, bytes.len, NULL, NULL, NULL, err);
    }
    pc_buf_fini(&bytes); /* reloaded when a face is used */
    return st;
}

/* ---- Fontconfig ---- */

pc_status pc_text_run_capture(const char *const *argv, size_t max, pc_buf *out, bool *ok, pc_error *err)
{
    int fds[2];
    pid_t pid;
    posix_spawn_file_actions_t fa;
    int status = 0;
    pc_status st = PC_OK;
    bool truncated = false;
    *ok = false;
    if (pipe(fds) != 0) {
        return PC_OK;
    }
    if (posix_spawn_file_actions_init(&fa) != 0) {
        close(fds[0]);
        close(fds[1]);
        return PC_OK;
    }
    if (posix_spawn_file_actions_adddup2(&fa, fds[1], 1) != 0 ||
        posix_spawn_file_actions_addclose(&fa, fds[0]) != 0 ||
        posix_spawn_file_actions_addclose(&fa, fds[1]) != 0 ||
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0) != 0 ||
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0) != 0 ||
        posix_spawnp(&pid, argv[0], &fa, NULL, (char *const *)(uintptr_t)argv, environ) != 0) {
        posix_spawn_file_actions_destroy(&fa);
        close(fds[0]);
        close(fds[1]);
        return PC_OK; /* not installed: no list */
    }
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    for (;;) {
        uint8_t chunk[4096];
        ssize_t got = read(fds[0], chunk, sizeof chunk);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            break;
        }
        if (out->len + (size_t)got > max) {
            truncated = true;
            break;
        }
        if (pc_buf_append(out, chunk, (size_t)got, err) != PC_OK) {
            st = PC_ERR_NOMEM;
            break;
        }
    }
    close(fds[0]);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    *ok = st == PC_OK && !truncated && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return st;
}

bool pc_text_font_path_ext(const char *path, size_t len)
{
    size_t start = len, dot = len, i;
    for (i = len; i > 0; i--) {
        if (path[i - 1] == '/') {
            break;
        }
        start = i - 1;
    }
    for (i = len; i > start; i--) {
        if (path[i - 1] == '.') {
            dot = i - 1;
            break;
        }
    }
    if (dot == len || dot == start) {
        return false; /* no extension (a leading dot names a hidden file) */
    }
    return pc_text_eq_ignore_ascii_case(path + dot + 1, len - dot - 1, "ttf", 3) ||
           pc_text_eq_ignore_ascii_case(path + dot + 1, len - dot - 1, "otf", 3) ||
           pc_text_eq_ignore_ascii_case(path + dot + 1, len - dot - 1, "ttc", 3) ||
           pc_text_eq_ignore_ascii_case(path + dot + 1, len - dot - 1, "otc", 3);
}

/* path.canonicalize().unwrap_or(path), as a malloc'd string. */
static char *canonical(const char *path)
{
    char *r = realpath(path, NULL);
    return r != NULL ? r : pc_strndup(path, strlen(path));
}

pc_status pc_text_fontconfig_paths_from_output(const uint8_t *data, size_t len, pc_strlist *out,
                                               pc_error *err)
{
    size_t at = 0;
    pc_strlist l = {NULL, 0};
    pc_status st = PC_OK;
    while (at < len && st == PC_OK) {
        size_t end = at, k;
        char *line, *c;
        bool seen = false;
        while (end < len && data[end] != '\n') {
            end++;
        }
        if (end > at && data[at] == '/' && memchr(data + at, 0, end - at) == NULL &&
            pc_text_font_path_ext((const char *)data + at, end - at)) {
            line = pc_strndup((const char *)data + at, end - at);
            c = line != NULL ? canonical(line) : NULL;
            free(line);
            if (c == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
                break;
            }
            for (k = 0; k < l.n && !seen; k++) {
                seen = strcmp(l.v[k], c) == 0;
            }
            if (!seen) {
                st = pc_strlist_push(&l, c, strlen(c), err);
            }
            free(c);
        }
        at = end + 1;
    }
    if (st != PC_OK) {
        pc_strlist_fini(&l);
        return st;
    }
    *out = l;
    return PC_OK;
}

/* Fontconfig's file list (`fc-list`), for configured font directories outside the portable
 * list: existing regular files only. Empty when fc-list is missing or fails. */
static pc_status fontconfig_font_paths(pc_strlist *out, pc_error *err)
{
#if defined(__linux__)
    static const char *const argv[] = {"fc-list", "--format=%{file}\\n", NULL};
    pc_buf b = PC_BUF_INIT;
    pc_strlist all = {NULL, 0};
    bool ok = false;
    size_t i;
    pc_status st = pc_text_run_capture(argv, (size_t)64 << 20, &b, &ok, err);
    out->v = NULL;
    out->n = 0;
    if (st == PC_OK && ok) {
        st = pc_text_fontconfig_paths_from_output(b.data, b.len, &all, err);
    }
    pc_buf_fini(&b);
    for (i = 0; st == PC_OK && i < all.n; i++) {
        struct stat sb;
        if (stat(all.v[i], &sb) == 0 && S_ISREG(sb.st_mode)) {
            st = pc_strlist_push(out, all.v[i], strlen(all.v[i]), err);
        }
    }
    pc_strlist_fini(&all);
    if (st != PC_OK) {
        pc_strlist_fini(out);
    }
    return st;
#else
    (void)err;
    out->v = NULL;
    out->n = 0;
    return PC_OK;
#endif
}

pc_status pc_fontdb_load_system_fonts(pc_fontdb *db, pc_error *err)
{
    pc_strlist dirs = {NULL, 0}, files = {NULL, 0}, fc = {NULL, 0}, seen = {NULL, 0};
    size_t i, k;
    pc_status st;
    if (db == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "fonts: invalid arguments");
    }
    if (db->system_loaded) {
        return PC_OK;
    }
    db->system_loaded = true;
    st = pc_text_system_font_dirs(&dirs, err);
    for (i = 0; st == PC_OK && i < dirs.n; i++) {
        st = collect(dirs.v[i], 0, &files, err);
    }
    if (st == PC_OK) {
        st = fontconfig_font_paths(&fc, err);
    }
    for (i = 0; st == PC_OK && i < fc.n; i++) {
        st = pc_strlist_push(&files, fc.v[i], strlen(fc.v[i]), err);
    }
#ifdef __APPLE__
    if (st == PC_OK) {
        /* PingFang (the macOS Chinese UI font) lives outside the font folders. */
        char *pf = pc_cjk_mac_pingfang();
        if (pf != NULL) {
            st = pc_strlist_push(&files, pf, strlen(pf), err);
            free(pf);
        }
    }
#endif
    /* Each file once, however it was found (its canonical path). */
    for (i = 0; st == PC_OK && i < files.n; i++) {
        char *c = canonical(files.v[i]);
        bool dup = false;
        if (c == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
            break;
        }
        for (k = 0; k < seen.n && !dup; k++) {
            dup = strcmp(seen.v[k], c) == 0;
        }
        if (!dup) {
            st = pc_strlist_push(&seen, c, strlen(c), err);
            if (st == PC_OK) {
                st = pc_fontdb_add_font_file(db, files.v[i], err);
            }
        }
        free(c);
    }
    pc_strlist_fini(&dirs);
    pc_strlist_fini(&files);
    pc_strlist_fini(&fc);
    pc_strlist_fini(&seen);
    if (st == PC_OK) {
        pc_fontdb_clear_ps_cache(db);
        pc_fontdb_refresh_generics(db);
    }
    return st;
}
