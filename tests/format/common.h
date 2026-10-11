/* Shared helpers for tests/format: per-test temporary directories (mkdtemp under $TMPDIR, removed
 * recursively afterwards) and small file utilities. */
#ifndef PC_TESTS_FORMAT_COMMON_H
#define PC_TESTS_FORMAT_COMMON_H

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/buf.h"
#include "base/file.h"

#if defined(__APPLE__)
#define T_ATIM(st) ((st).st_atimespec)
#define T_MTIM(st) ((st).st_mtimespec)
#else
#define T_ATIM(st) ((st).st_atim)
#define T_MTIM(st) ((st).st_mtim)
#endif

/* A fresh directory "<TMPDIR>/pc-format-<tag>-XXXXXX" into out (PATH_MAX-ish buffer); 0 on success. */
static inline int t_mkdtemp(const char *tag, char *out, size_t cap)
{
    const char *base = getenv("TMPDIR");
    if (base == NULL || base[0] == '\0') {
        base = "/tmp";
    }
    if (snprintf(out, cap, "%s/pc-format-%s-XXXXXX", base, tag) >= (int)cap) {
        return -1;
    }
    return mkdtemp(out) != NULL ? 0 : -1;
}

/* rm -rf (never follows symlinks); restores write permission on directories first. */
static inline void t_rm_rf(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d;
        (void)chmod(path, 0755);
        d = opendir(path);
        if (d != NULL) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                char child[4096];
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                    continue;
                }
                if (snprintf(child, sizeof child, "%s/%s", path, e->d_name) < (int)sizeof child) {
                    t_rm_rf(child);
                }
            }
            (void)closedir(d);
        }
        (void)rmdir(path);
    } else {
        (void)unlink(path);
    }
}

static inline int t_write_file(const char *path, const void *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    pc_status s;
    if (fd < 0) {
        return -1;
    }
    s = pc_fd_write(fd, data, len, NULL);
    (void)close(fd);
    return s == PC_OK ? 0 : -1;
}

/* Whole file into *out (replaced); 0 on success. */
static inline int t_read_file(const char *path, pc_buf *out)
{
    return pc_file_read(path, (size_t)1 << 30, out, NULL) == PC_OK ? 0 : -1;
}

/* Sorted, comma-separated listing of every file and directory under root (directories end
 * with '/'), as the rustref vectors print it. */
static inline int t_names_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static inline void t_listing_rec(const char *root, const char *rel, char ***v, size_t *n, size_t *cap)
{
    char dir[4096];
    DIR *d;
    struct dirent *e;
    if (snprintf(dir, sizeof dir, "%s%s%s", root, rel[0] ? "/" : "", rel) >= (int)sizeof dir) {
        return;
    }
    d = opendir(dir);
    if (d == NULL) {
        return;
    }
    while ((e = readdir(d)) != NULL) {
        char r[4096];
        char full[4096];
        struct stat st;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        if (snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name) >= (int)sizeof r ||
            snprintf(full, sizeof full, "%s/%s", root, r) >= (int)sizeof full || lstat(full, &st) != 0) {
            continue;
        }
        if (*n == *cap) {
            size_t nc = *cap ? *cap * 2 : 64;
            char **g = realloc(*v, nc * sizeof *g);
            if (g == NULL) {
                break;
            }
            *v = g;
            *cap = nc;
        }
        {
            size_t l = strlen(r);
            char *s = malloc(l + 2);
            if (s == NULL) {
                break;
            }
            memcpy(s, r, l);
            s[l] = S_ISDIR(st.st_mode) ? '/' : '\0';
            s[l + 1] = '\0';
            (*v)[(*n)++] = s;
        }
        if (S_ISDIR(st.st_mode)) {
            t_listing_rec(root, r, v, n, cap);
        }
    }
    (void)closedir(d);
}

static inline void t_listing(const char *root, pc_buf *out)
{
    char **v = NULL;
    size_t n = 0;
    size_t cap = 0;
    pc_buf_truncate(out, 0);
    (void)pc_buf_append(out, "", 0, NULL);
    t_listing_rec(root, "", &v, &n, &cap);
    if (n > 1) {
        qsort(v, n, sizeof *v, t_names_cmp);
    }
    for (size_t i = 0; i < n; i++) {
        (void)pc_buf_appendf(out, NULL, "%s%s", i ? "," : "", v[i]);
        free(v[i]);
    }
    free(v);
}

#endif
