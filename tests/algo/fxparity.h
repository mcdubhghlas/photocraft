/* Shared helpers of tests/algo/test_fx_parity.c (the rustref algo-fx-vectors replay): FNV-1a
 * hashing of floats, rects and surfaces, the deterministic test pattern, and the format table.
 * Must mirror tools/rustref/src/algo_fx_vectors.rs. Header-only (one test executable uses it). */
#ifndef PC_TESTS_ALGO_FXPARITY_H
#define PC_TESTS_ALGO_FXPARITY_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "harness.h"
#include "raster/surface.h"

#define H0 0xcbf29ce484222325ull

static void fnv(uint64_t *h, const void *p, size_t n)
{
    const uint8_t *b = p;
    size_t i;
    for (i = 0; i < n; i++) {
        *h ^= b[i];
        *h *= 0x100000001b3ull;
    }
}
static void hf(uint64_t *h, const float *v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint32_t u;
        uint8_t b[4];
        memcpy(&u, &v[i], 4);
        b[0] = (uint8_t)u, b[1] = (uint8_t)(u >> 8), b[2] = (uint8_t)(u >> 16), b[3] = (uint8_t)(u >> 24);
        fnv(h, b, 4);
    }
}
static void h64(uint64_t *h, uint64_t u)
{
    uint8_t b[8];
    int i;
    for (i = 0; i < 8; i++) {
        b[i] = (uint8_t)(u >> (8 * i));
    }
    fnv(h, b, 8);
}
static void hi32(uint64_t *h, int32_t v)
{
    uint32_t u = (uint32_t)v;
    uint8_t b[4] = {(uint8_t)u, (uint8_t)(u >> 8), (uint8_t)(u >> 16), (uint8_t)(u >> 24)};
    fnv(h, b, 4);
}
static void hrect(uint64_t *h, pc_rect r)
{
    hi32(h, r.x0), hi32(h, r.y0), hi32(h, r.x1), hi32(h, r.y1);
}
/* fnv of a sub-hash's little-endian bytes. */
static void hh(uint64_t *h, uint64_t sub)
{
    h64(h, sub);
}

/* Content bounds, the samples inside them and the default pixel. */
static uint64_t hs(const pc_surface *s)
{
    uint64_t h = H0;
    pc_rect cb = pc_surface_content_bounds(s);
    float *v = NULL, def[PC_RASTER_MAX_CHANNELS];
    size_t len = 0;
    uint8_t tail[2];
    hrect(&h, cb);
    CHECK(pc_surface_read_region(s, cb, &v, &len, NULL) == PC_OK);
    hf(&h, v, len);
    free(v);
    pc_surface_default_pixel(s, def);
    hf(&h, def, pc_surface_channels(s));
    tail[0] = (uint8_t)pc_surface_format(s).alpha;
    tail[1] = (uint8_t)pc_surface_channels(s);
    fnv(&h, tail, 2);
    return h;
}

/* A failed operation hashes as a marker that never matches a reference value. */
static uint64_t hs_st(pc_status st, pc_surface *s)
{
    uint64_t h;
    if (st != PC_OK) {
        return 0xbad;
    }
    h = hs(s);
    pc_surface_fini(s);
    return h;
}

static float pat(int32_t x, int32_t y, size_t c, int32_t seed)
{
    int64_t v = (int64_t)x * 7 + (int64_t)y * 13 + (int64_t)c * 29 + (int64_t)seed * 31;
    v %= 97;
    if (v < 0) {
        v += 97;
    }
    return (float)v / 96.0f;
}
static float alpha_at(int32_t x, int32_t y, int32_t seed)
{
    int64_t v = ((int64_t)x * 3 + (int64_t)y * 5 + seed) % 7;
    if (v < 0) {
        v += 7;
    }
    return (float)v / 6.0f;
}
/* A surface of fmt with pattern pixels over r (alpha varies unless opaque). */
static void surf(pc_surface *s, pc_pixel_format fmt, pc_rect r, int32_t seed, bool opaque)
{
    size_t n = pc_pixel_format_channels(fmt), k = 0, c;
    size_t len = (size_t)pc_rect_width(r) * pc_rect_height(r) * n;
    float *v = malloc(len * sizeof(float) + 4);
    int32_t x, y;
    pc_surface_init(s, fmt);
    if (v == NULL) {
        CHECK(v != NULL);
        return;
    }
    for (y = r.y0; y < r.y1; y++) {
        for (x = r.x0; x < r.x1; x++) {
            for (c = 0; c < n; c++) {
                v[k++] =
                    fmt.alpha && c == n - 1 ? (opaque ? 1.0f : alpha_at(x, y, seed)) : pat(x, y, c, seed);
            }
        }
    }
    CHECK(pc_surface_write_region(s, r, v, len, NULL) == PC_OK);
    free(v);
}

typedef struct fmt_entry {
    const char *name;
    pc_pixel_format f;
} fmt_entry;

static const fmt_entry FORMATS[] = {
    {"rgba8", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true}},
    {"rgba16", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, true}},
    {"rgba32", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, true}},
    {"rgb8", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false}},
    {"gray8", {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, false}},
    {"graya16", {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U16, true}},
    {"cmyka8", {PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, true}},
    {"laba16", {PC_COLOR_MODE_LAB, PC_SAMPLE_TYPE_U16, true}},
    {"laba32", {PC_COLOR_MODE_LAB, PC_SAMPLE_TYPE_F32, true}},
};

static pc_pixel_format fmt_named(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof FORMATS / sizeof FORMATS[0]; i++) {
        if (strcmp(FORMATS[i].name, name) == 0) {
            return FORMATS[i].f;
        }
    }
    return PC_PIXEL_FORMAT_RGBA8;
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *data = NULL;
    long n;
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0) {
        data = malloc((size_t)n + 2);
        if (data != NULL) {
            *len = fread(data, 1, (size_t)n, f);
            data[*len] = '\0';
        }
    }
    (void)fclose(f);
    return data;
}

/* Splits `line` at spaces into at most `max` tokens (the last one takes the rest). */
static size_t split(char *line, char **tok, size_t max)
{
    size_t n = 0;
    char *p = line;
    while (n < max && *p != '\0') {
        tok[n++] = p;
        if (n == max) {
            break;
        }
        p = strchr(p, ' ');
        if (p == NULL) {
            break;
        }
        *p++ = '\0';
    }
    return n;
}

/* The sections beyond the filter cases (fxparity2.h). */
static bool fx_replay_more(const char *want, char *got, size_t cap);

#endif
