/* Tagged-block helpers: Advanced Blending, Blend If, locks and labels, descriptor accessors and
 * colours (port of the first half of crates/io/src/blocks.rs; fills, gradients, patterns, text
 * and smart-object blocks are in blocks_fill.c). Parse failures of file data are "not found"
 * (the reference's None); only out of memory is an error. */
#include "io/blocks.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "io/desc_util.h"
#include "psd/layer.h"

/* ---- brst, Advanced Blending, Blend If ---- */

uint32_t pc_io_parse_brst(const uint8_t *data, size_t len)
{
    uint32_t m = 0;
    for (size_t at = 0; data != NULL && at + 4 <= len; at += 4) {
        uint32_t i = (uint32_t)data[at] << 24 | (uint32_t)data[at + 1] << 16 | (uint32_t)data[at + 2] << 8 |
                     data[at + 3];
        if (i < 32) {
            m |= 1u << i;
        }
    }
    return m;
}

pc_status pc_io_brst_data(uint32_t mask, bool *has, pc_buf *out, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    if (mask == 0) {
        *has = false;
        return PC_OK;
    }
    for (uint32_t i = 0; i < 32; i++) {
        if (mask & (1u << i)) {
            uint8_t be[4] = {0, 0, 0, (uint8_t)i};
            pc_status st = pc_buf_append(&b, be, 4, err);
            if (st != PC_OK) {
                pc_buf_fini(&b);
                return st;
            }
        }
    }
    pc_buf_fini(out);
    *out = b;
    *has = true;
    return PC_OK;
}

typedef struct adv_flag {
    char key[5];
    uint8_t v;
    uint8_t def;
} adv_flag;

/* blocks::advanced_flags: (key, value for a, Photoshop's default). */
static void advanced_flags(const pc_advanced_blending *a, adv_flag out[6])
{
    const adv_flag f[6] = {{"knko", pc_knockout_to_psd(a->knockout), 0},
                           {"infx", a->blend_interior, 0},
                           {"clbl", a->blend_clipped, 1},
                           {"tsly", a->transparency_shapes, 1},
                           {"lmgm", a->layer_mask_hides_effects, 0},
                           {"vmgm", a->vector_mask_hides_effects, 0}};
    memcpy(out, f, sizeof f);
}

pc_advanced_blending
pc_io_advanced_from_blocks(const uint8_t *(*block)(void *ctx, const uint8_t key[4], size_t *len), void *ctx)
{
    pc_advanced_blending a = pc_advanced_blending_default();
    bool *fields[5] = {&a.blend_interior, &a.blend_clipped, &a.transparency_shapes,
                       &a.layer_mask_hides_effects, &a.vector_mask_hides_effects};
    static const char keys[6][5] = {"knko", "infx", "clbl", "tsly", "lmgm", "vmgm"};
    for (size_t i = 0; i < 6; i++) {
        size_t len = 0;
        const uint8_t *d = block(ctx, (const uint8_t *)keys[i], &len);
        if (d == NULL || len == 0) {
            continue;
        }
        if (i == 0) {
            a.knockout = pc_knockout_from_psd(d[0]);
        } else {
            *fields[i - 1] = d[0] != 0;
        }
    }
    return a;
}

pc_status pc_io_put_advanced(const pc_advanced_blending *a, pc_io_raw_blocks *raw, pc_error *err)
{
    adv_flag f[6];
    advanced_flags(a, f);
    for (size_t i = 0; i < 6; i++) {
        pc_io_raw_block *e = NULL;
        for (size_t k = 0; k < raw->n; k++) {
            if (memcmp(raw->v[k].key, f[i].key, 4) == 0) {
                e = &raw->v[k];
                break;
            }
        }
        if (e != NULL) {
            /* Keep any extra bytes a reader wrote; the flag is the first byte. */
            if (e->len < 4) {
                uint8_t *d = pc_malloc(4);
                if (d == NULL) {
                    return pc_io_nomem(err, "writing an Advanced Blending block");
                }
                memset(d, 0, 4);
                if (e->len > 0) {
                    memcpy(d, e->data, e->len);
                }
                free(e->data);
                e->data = d;
                e->len = 4;
            }
            e->data[0] = f[i].v;
        } else if (f[i].v != f[i].def) {
            uint8_t d[4] = {f[i].v, 0, 0, 0};
            pc_status st = pc_io_raw_blocks_push(raw, (const uint8_t *)f[i].key, d, 4, err);
            if (st != PC_OK) {
                return st;
            }
        }
    }
    return PC_OK;
}

pc_status pc_io_blend_if_from_ranges(const uint8_t *data, size_t len, pc_blend_if *out, pc_error *err)
{
    size_t n = data != NULL ? len / 8 : 0;
    size_t keep = 0;
    pc_blend_if b = {NULL, 0};
    for (size_t i = 0; i < n; i++) {
        if (!pc_blend_range_is_full(pc_blend_range_from_bytes(data + i * 8)) ||
            !pc_blend_range_is_full(pc_blend_range_from_bytes(data + i * 8 + 4))) {
            keep = i + 1;
        }
    }
    /* Trailing full entries carry nothing (export pads them back), as BlendIf::set drops them. */
    if (keep > 0) {
        b.ranges = pc_malloc_array(keep, sizeof *b.ranges);
        if (b.ranges == NULL) {
            return pc_io_nomem(err, "reading Blend If");
        }
        for (size_t i = 0; i < keep; i++) {
            b.ranges[i][0] = pc_blend_range_from_bytes(data + i * 8);
            b.ranges[i][1] = pc_blend_range_from_bytes(data + i * 8 + 4);
        }
        b.nranges = keep;
    }
    *out = b;
    return PC_OK;
}

pc_status pc_io_ranges_from_blend_if(const pc_blend_if *b, size_t channels, pc_psd_bytes *out, pc_error *err)
{
    size_t n;
    uint8_t *d;
    if (pc_blend_if_is_default(b)) {
        return pc_psd_blending_ranges_full(channels, out, err);
    }
    n = b->nranges > channels + 1 ? b->nranges : channels + 1;
    d = pc_malloc_array(n, 8);
    if (d == NULL) {
        return pc_io_nomem(err, "writing blending ranges");
    }
    for (size_t i = 0; i < n; i++) {
        pc_blend_range pair[2];
        pc_blend_if_get(b, i, pair);
        pc_blend_range_to_bytes(pair[0], d + i * 8);
        pc_blend_range_to_bytes(pair[1], d + i * 8 + 4);
    }
    out->data = d;
    out->len = n * 8;
    return PC_OK;
}

/* ---- locks and labels ---- */

pc_locks pc_io_locks_from_lspf(uint32_t v)
{
    pc_locks l;
    l.transparency = (v & 1) != 0;
    l.pixels = (v & 2) != 0;
    l.position = (v & 4) != 0;
    /* Older PhotoCraft builds wrote the artboard lock at 0x10; still read, rewritten at 0x08. */
    l.artboard = (v & 0x08) != 0 || (v & 0x10) != 0;
    l.all = (v & 0x80000000u) != 0;
    return l;
}

uint32_t pc_io_lspf_from_locks(const pc_locks *l)
{
    return (uint32_t)l->transparency | (uint32_t)l->pixels << 1 | (uint32_t)l->position << 2 |
           (uint32_t)l->artboard << 3 | (uint32_t)l->all << 31;
}

pc_label_color pc_io_label_from_index(uint16_t v)
{
    /* Photoshop 2024 added 8..11 without renumbering the original labels. */
    static const pc_label_color map[12] = {PC_LABEL_NONE,   PC_LABEL_RED,     PC_LABEL_ORANGE,
                                           PC_LABEL_YELLOW, PC_LABEL_GREEN,   PC_LABEL_BLUE,
                                           PC_LABEL_VIOLET, PC_LABEL_GRAY,    PC_LABEL_SEAFOAM,
                                           PC_LABEL_INDIGO, PC_LABEL_MAGENTA, PC_LABEL_FUCHSIA};
    return v < 12 ? map[v] : PC_LABEL_NONE;
}

uint16_t pc_io_label_index(pc_label_color l)
{
    switch (l) {
    case PC_LABEL_RED:
        return 1;
    case PC_LABEL_ORANGE:
        return 2;
    case PC_LABEL_YELLOW:
        return 3;
    case PC_LABEL_GREEN:
        return 4;
    case PC_LABEL_BLUE:
        return 5;
    case PC_LABEL_VIOLET:
        return 6;
    case PC_LABEL_GRAY:
        return 7;
    case PC_LABEL_SEAFOAM:
        return 8;
    case PC_LABEL_INDIGO:
        return 9;
    case PC_LABEL_MAGENTA:
        return 10;
    case PC_LABEL_FUCHSIA:
        return 11;
    case PC_LABEL_NONE:
    default:
        return 0;
    }
}

/* ---- descriptor accessors ---- */

bool pc_io_desc_num(const pc_psd_value *v, double *out)
{
    if (v == NULL) {
        return false;
    }
    switch (v->kind) {
    case PC_PSD_V_DOUBLE:
    case PC_PSD_V_UNIT_FLOAT:
        *out = v->d;
        return true;
    case PC_PSD_V_INTEGER:
        *out = (double)v->i;
        return true;
    default:
        return false;
    }
}

const pc_psd_desc *pc_io_desc_get(const pc_psd_desc *d, const char *key)
{
    const pc_psd_value *v = d != NULL ? pc_psd_desc_get(d, key) : NULL;
    if (v != NULL && (v->kind == PC_PSD_V_DESCRIPTOR || v->kind == PC_PSD_V_GLOBAL_OBJECT)) {
        return v->desc;
    }
    return NULL;
}

const uint8_t *pc_io_desc_enum(const pc_psd_desc *d, const char *key, size_t *len)
{
    const pc_psd_value *v = d != NULL ? pc_psd_desc_get(d, key) : NULL;
    if (v != NULL && v->kind == PC_PSD_V_ENUMERATED) {
        const uint8_t *b = pc_psd_id_bytes(&v->value, len);
        /* An empty id still counts as present: hand out a valid pointer. */
        return b != NULL ? b : (const uint8_t *)"";
    }
    return NULL;
}

bool pc_io_desc_bool(const pc_psd_desc *d, const char *key)
{
    const pc_psd_value *v = d != NULL ? pc_psd_desc_get(d, key) : NULL;
    return v != NULL && v->kind == PC_PSD_V_BOOLEAN && v->b;
}

/* num(d.get(k)) as f32. */
static bool gf(const pc_psd_desc *d, const char *k, float *out)
{
    double v;
    if (!pc_io_desc_num(pc_psd_desc_get(d, k), &v)) {
        return false;
    }
    *out = pc_f64_to_f32(v);
    return true;
}

static bool class_is(const pc_psd_desc *d, const char *code)
{
    size_t n = 0;
    const uint8_t *b = pc_psd_id_bytes(&d->class_id, &n);
    return b != NULL && n == 4 && memcmp(b, code, 4) == 0;
}

static bool rgb255(const pc_psd_desc *d, pc_color *out)
{
    float r;
    float g;
    float b;
    if (!gf(d, "Rd  ", &r) || !gf(d, "Grn ", &g) || !gf(d, "Bl  ", &b)) {
        return false;
    }
    *out = pc_color_rgb(r / 255.0f, g / 255.0f, b / 255.0f);
    return true;
}

static bool rgb_float(const pc_psd_desc *d, pc_color *out)
{
    float r;
    float g;
    float b;
    if (!gf(d, "redFloat", &r) || !gf(d, "greenFloat", &g) || !gf(d, "blueFloat", &b)) {
        return false;
    }
    *out = pc_color_rgb(r, g, b);
    return true;
}

bool pc_io_color_from_desc(const pc_psd_desc *d, pc_color *out)
{
    bool has_float = pc_psd_desc_get(d, "redFloat") != NULL;
    if (class_is(d, "RGBC")) {
        /* Newer Photoshop writes 0..1 floats (redFloat / greenFloat / blueFloat). */
        return has_float ? rgb_float(d, out) : rgb255(d, out);
    }
    if (class_is(d, "CMYC")) {
        float c;
        float m;
        float y;
        float k;
        if (!gf(d, "Cyn ", &c) || !gf(d, "Mgnt", &m) || !gf(d, "Ylw ", &y) || !gf(d, "Blck", &k)) {
            return false;
        }
        *out = (pc_color){PC_COLOR_MODE_CMYK, {c / 100.0f, m / 100.0f, y / 100.0f, k / 100.0f}, 1.0f};
        return true;
    }
    if (class_is(d, "Grsc")) {
        float g;
        if (!gf(d, "Gry ", &g)) {
            return false;
        }
        *out = pc_color_gray(1.0f - g / 100.0f);
        return true;
    }
    if (class_is(d, "LbCl")) {
        /* L 0..100, a / b -128..127, stored like Lab pixels (offset by 128). */
        float l;
        float a;
        float b;
        if (!gf(d, "Lmnc", &l) || !gf(d, "A   ", &a) || !gf(d, "B   ", &b)) {
            return false;
        }
        *out = (pc_color){
            PC_COLOR_MODE_LAB, {l / 100.0f, (a + 128.0f) / 255.0f, (b + 128.0f) / 255.0f, 0.0f}, 1.0f};
        return true;
    }
    /* Anything else: float RGB, or any RGB-like triple. */
    return has_float ? rgb_float(d, out) : rgb255(d, out);
}

static pc_status add_double(pc_psd_desc *d, const char *key, float v, pc_error *err)
{
    pc_psd_value x = pc_psd_value_double((double)v);
    return pc_psd_desc_add(d, key, &x, err);
}

pc_status pc_io_color_to_desc(const pc_color *c, pc_psd_desc *out, pc_error *err)
{
    pc_psd_desc d;
    pc_status st;
    switch (c->mode) {
    case PC_COLOR_MODE_CMYK:
        st = pc_psd_desc_init(&d, "CMYC", err);
        st = st != PC_OK ? st : add_double(&d, "Cyn ", c->c[0] * 100.0f, err);
        st = st != PC_OK ? st : add_double(&d, "Mgnt", c->c[1] * 100.0f, err);
        st = st != PC_OK ? st : add_double(&d, "Ylw ", c->c[2] * 100.0f, err);
        st = st != PC_OK ? st : add_double(&d, "Blck", c->c[3] * 100.0f, err);
        break;
    case PC_COLOR_MODE_GRAYSCALE:
        st = pc_psd_desc_init(&d, "Grsc", err);
        st = st != PC_OK ? st : add_double(&d, "Gry ", (1.0f - c->c[0]) * 100.0f, err);
        break;
    case PC_COLOR_MODE_LAB:
        st = pc_psd_desc_init(&d, "LbCl", err);
        st = st != PC_OK ? st : add_double(&d, "Lmnc", c->c[0] * 100.0f, err);
        st = st != PC_OK ? st : add_double(&d, "A   ", c->c[1] * 255.0f - 128.0f, err);
        st = st != PC_OK ? st : add_double(&d, "B   ", c->c[2] * 255.0f - 128.0f, err);
        break;
    default: {
        float rgb[3];
        pc_color_to_rgb(c, rgb);
        st = pc_psd_desc_init(&d, "RGBC", err);
        st = st != PC_OK ? st : add_double(&d, "Rd  ", rgb[0] * 255.0f, err);
        st = st != PC_OK ? st : add_double(&d, "Grn ", rgb[1] * 255.0f, err);
        st = st != PC_OK ? st : add_double(&d, "Bl  ", rgb[2] * 255.0f, err);
        break;
    }
    }
    if (st != PC_OK) {
        pc_psd_desc_fini(&d);
        return st;
    }
    *out = d;
    return PC_OK;
}

pc_status pc_io_parse_prefix_versioned(const uint8_t *data, size_t len, bool *found, pc_psd_desc *out,
                                       pc_error *err)
{
    size_t used = 0;
    pc_psd_desc d;
    pc_error e;
    pc_status st = pc_psd_vdesc_parse_prefix(data != NULL ? data : (const uint8_t *)"", len, &d, &used, &e);
    if (st == PC_ERR_NOMEM) {
        if (err != NULL) {
            *err = e;
        }
        return st;
    }
    if (st != PC_OK) {
        *found = false;
        return PC_OK;
    }
    *found = true;
    *out = d;
    return PC_OK;
}

pc_status pc_io_effects_enabled(const uint8_t *data, size_t len, bool *out, pc_error *err)
{
    pc_psd_desc d;
    bool found = false;
    bool on = true;
    pc_status st;
    if (data != NULL && len >= 4) {
        st = pc_io_parse_prefix_versioned(data + 4, len - 4, &found, &d, err);
        if (st != PC_OK) {
            return st;
        }
        if (found) {
            const pc_psd_value *v = pc_psd_desc_get(&d, "masterFXSwitch");
            if (v != NULL && v->kind == PC_PSD_V_BOOLEAN) {
                on = v->b;
            }
            pc_psd_desc_fini(&d);
        }
    }
    *out = on;
    return PC_OK;
}
