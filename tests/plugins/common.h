/* Shared helpers for tests/plugins: the abi.rs test surfaces, golden-file reading, scratch
 * directories. */
#ifndef PC_TESTS_PLUGINS_COMMON_H
#define PC_TESTS_PLUGINS_COMMON_H

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/buf.h"
#include "base/file.h"
#include "plugins/plugins.h"
#include "raster/surface.h"

/* abi.rs `surface`: a deterministic pattern with some transparent pixels. */
static inline void t_surface(pc_surface *s, pc_pixel_format fmt, int32_t w, int32_t h)
{
    pc_surface_init(s, fmt);
    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            float v = (float)((x * 7 + y * 13) % 31) / 30.0f;
            float a = (x + y) % 9 == 0 ? 0.0f : 0.25f + 0.75f * ((float)(x % 4) / 3.0f);
            float px[4] = {v, 1.0f - v, (float)(x % 5) / 4.0f, a};
            (void)pc_surface_write_pixel(s, x, y, px, 4, NULL);
        }
    }
}

/* Coverage 1 over the left half, 0.5 over its top third. */
static inline void t_selection(pc_surface *sel)
{
    float one = 1.0f, half = 0.5f;
    pc_surface_init(sel, PC_PIXEL_FORMAT_GRAY8);
    (void)pc_surface_fill_rect(sel, pc_rect_make(0, 0, 20, 30), &one, 1, NULL);
    (void)pc_surface_fill_rect(sel, pc_rect_make(0, 0, 20, 10), &half, 1, NULL);
}

static inline int t_read_file(const char *path, pc_buf *out)
{
    return pc_file_read(path, (size_t)64 << 20, out, NULL) == PC_OK ? 0 : -1;
}

static inline int t_mkdtemp(const char *tag, char *out, size_t cap)
{
    const char *base = getenv("TMPDIR");
    if (base == NULL || base[0] == '\0') {
        base = "/tmp";
    }
    if (snprintf(out, cap, "%s/pc-plugins-%s-XXXXXX", base, tag) >= (int)cap) {
        return -1;
    }
    return mkdtemp(out) != NULL ? 0 : -1;
}

static inline void t_rm_rf(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
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
    FILE *f = fopen(path, "wb");
    int ok;
    if (f == NULL) {
        return -1;
    }
    ok = len == 0 || fwrite(data, 1, len, f) == len;
    return fclose(f) == 0 && ok ? 0 : -1;
}

/* The invert example plug-in (tests/fixtures/plugins/invert.wasm). */
static inline int t_invert(pc_buf *out)
{
    return t_read_file("tests/fixtures/plugins/invert.wasm", out);
}

#endif
