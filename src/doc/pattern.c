/* Port of crates/doc/src/pattern.rs: pattern tiles, deterministic ids and lookups. */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "doc/doc.h"

/* 64-bit FNV-1a, used for deterministic pattern ids. */
static uint64_t fnv(uint64_t h, const void *bytes, size_t n)
{
    const unsigned char *b = bytes;
    size_t i;
    for (i = 0; i < n; i++) {
        h = (h ^ (uint64_t)b[i]) * UINT64_C(0x00000100000001b3);
    }
    return h;
}

static uint64_t fnv_u32_le(uint64_t h, uint32_t v)
{
    unsigned char b[4];
    b[0] = (unsigned char)(v & 0xffu);
    b[1] = (unsigned char)((v >> 8) & 0xffu);
    b[2] = (unsigned char)((v >> 16) & 0xffu);
    b[3] = (unsigned char)((v >> 24) & 0xffu);
    return fnv(h, b, 4);
}

/* Variant names of the reference's derived Debug. */
static const char *mode_debug(pc_color_mode m)
{
    static const char *const names[PC_COLOR_MODE_COUNT] = {
        "Bitmap", "Grayscale", "Indexed", "Rgb", "Cmyk", "Lab", "Multichannel", "Duotone",
    };
    size_t i = (size_t)m;
    return i < PC_COLOR_MODE_COUNT ? names[i] : "Rgb";
}

static const char *sample_debug(pc_sample_type s)
{
    static const char *const names[PC_SAMPLE_TYPE_COUNT] = {"U8", "U16", "F32"};
    size_t i = (size_t)s;
    return i < PC_SAMPLE_TYPE_COUNT ? names[i] : "U8";
}

pc_status pc_pattern_new(const char *name, size_t name_len, pc_surface *surface, uint32_t width,
                         uint32_t height, pc_pattern *out, pc_error *err)
{
    pc_pattern p;
    pc_rect r = pc_rect_make(0, 0, pc_u32_as_i32(width), pc_u32_as_i32(height));
    pc_pixel_format f = pc_surface_format(surface);
    float *px = NULL;
    size_t npx = 0;
    size_t i;
    uint64_t h1;
    uint64_t h2;
    char dbg[96];
    char id[40];
    int k;
    pc_status s;
    if (name == NULL && name_len > 0) {
        return pc_fail(err, PC_ERR_INVALID, "pattern name is NULL");
    }
    s = pc_surface_read_region(surface, r, &px, &npx, err);
    if (s != PC_OK) {
        return s;
    }
    h1 = fnv(UINT64_C(0xcbf29ce484222325), name, name_len);
    h1 = fnv_u32_le(h1, width);
    h1 = fnv_u32_le(h1, height);
    for (i = 0; i < npx; i++) {
        uint32_t bits;
        memcpy(&bits, &px[i], sizeof bits);
        h1 = fnv_u32_le(h1, bits);
    }
    free(px);
    k = snprintf(dbg, sizeof dbg, "PixelFormat { mode: %s, sample: %s, alpha: %s }", mode_debug(f.mode),
                 sample_debug(f.sample), f.alpha ? "true" : "false");
    if (k < 0 || (size_t)k >= sizeof dbg) {
        return pc_fail(err, PC_ERR_INVALID, "pattern: format description too long");
    }
    h2 = fnv(h1 ^ UINT64_C(0x9e3779b97f4a7c15), dbg, (size_t)k);
    k = snprintf(id, sizeof id, "%08" PRIx32 "-%04x-%04x-%04x-%012" PRIx64, (uint32_t)(h1 >> 32),
                 (unsigned)(uint16_t)(h1 >> 16), (unsigned)(uint16_t)h1, (unsigned)(uint16_t)(h2 >> 48),
                 h2 & UINT64_C(0xffffffffffff));
    if (k < 0 || (size_t)k >= sizeof id) {
        return pc_fail(err, PC_ERR_INVALID, "pattern: id too long");
    }
    memset(&p, 0, sizeof p);
    s = pc_str_set(&p.id, id, (size_t)k, err);
    if (s == PC_OK) {
        s = pc_str_set(&p.name, name, name_len, err);
    }
    if (s != PC_OK) {
        pc_str_fini(&p.id);
        pc_str_fini(&p.name);
        return s;
    }
    p.width = width;
    p.height = height;
    p.surface = *surface;
    memset(surface, 0, sizeof *surface);
    *out = p;
    return PC_OK;
}

void pc_pattern_fini(pc_pattern *p)
{
    if (p == NULL) {
        return;
    }
    pc_str_fini(&p->id);
    pc_str_fini(&p->name);
    pc_surface_fini(&p->surface);
    memset(p, 0, sizeof *p);
}

pc_status pc_pattern_clone(const pc_pattern *p, pc_pattern *out, pc_error *err)
{
    pc_pattern c;
    pc_status s;
    memset(&c, 0, sizeof c);
    c.width = p->width;
    c.height = p->height;
    s = pc_str_clone(&p->id, &c.id, err);
    if (s == PC_OK) {
        s = pc_str_clone(&p->name, &c.name, err);
    }
    if (s != PC_OK) {
        pc_str_fini(&c.id);
        return s;
    }
    s = pc_surface_clone(&p->surface, &c.surface, err);
    if (s != PC_OK) {
        pc_str_fini(&c.id);
        pc_str_fini(&c.name);
        return s;
    }
    *out = c;
    return PC_OK;
}

bool pc_pattern_eq(const pc_pattern *a, const pc_pattern *b)
{
    return pc_str_eq(&a->id, &b->id) && pc_str_eq(&a->name, &b->name) && a->width == b->width &&
           a->height == b->height && pc_surface_eq(&a->surface, &b->surface);
}

pc_rect pc_pattern_rect(const pc_pattern *p)
{
    return pc_rect_make(0, 0, pc_u32_as_i32(p->width), pc_u32_as_i32(p->height));
}

bool pc_pattern_is_empty(const pc_pattern *p)
{
    return p->width == 0 || p->height == 0;
}

const char *pc_pattern_display_name(const char *name, size_t len, size_t *out_len)
{
    size_t i;
    if (name == NULL) {
        *out_len = 0;
        return "";
    }
    while (len > 0 && name[len - 1] == '\0') {
        len--;
    }
    if (len >= 4 && memcmp(name, "$$$/", 4) == 0) {
        for (i = 4; i < len; i++) {
            if (name[i] == '=') {
                *out_len = len - i - 1;
                return name + i + 1;
            }
        }
    }
    *out_len = len;
    return name;
}

static unsigned char ascii_lower(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + ('a' - 'A')) : c;
}

static bool eq_ignore_ascii_case(const char *a, size_t na, const char *b, size_t nb)
{
    size_t i;
    if (na != nb) {
        return false;
    }
    for (i = 0; i < na; i++) {
        if (ascii_lower((unsigned char)a[i]) != ascii_lower((unsigned char)b[i])) {
            return false;
        }
    }
    return true;
}

const pc_pattern *pc_pattern_find(const pc_pattern *patterns, size_t n, const char *id, size_t id_len,
                                  const char *name, size_t name_len)
{
    size_t i;
    const char *want;
    size_t want_len;
    if (patterns == NULL) {
        n = 0;
    }
    if (id_len > 0 && id != NULL) {
        for (i = 0; i < n; i++) {
            if (pc_str_eq_mem(&patterns[i].id, id, id_len)) {
                return &patterns[i];
            }
        }
    }
    if (name_len == 0 || name == NULL) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        if (pc_str_eq_mem(&patterns[i].name, name, name_len)) {
            return &patterns[i];
        }
    }
    want = pc_pattern_display_name(name, name_len, &want_len);
    for (i = 0; i < n; i++) {
        size_t dl;
        const char *d = pc_pattern_display_name(pc_str_c(&patterns[i].name), patterns[i].name.len, &dl);
        if (eq_ignore_ascii_case(d, dl, want, want_len)) {
            return &patterns[i];
        }
    }
    return NULL;
}

uint64_t pc_pattern_pixel_count(const pc_pattern *p)
{
    return (uint64_t)p->width * (uint64_t)p->height;
}

bool pc_pattern_is_paintable(const pc_pattern *p)
{
    return !pc_pattern_is_empty(p) && pc_pattern_pixel_count(p) <= PC_PATTERN_MAX_PAINT_PIXELS;
}
