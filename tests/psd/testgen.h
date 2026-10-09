/* Deterministic synthetic PSD/PSB generator for the psd tests (port of
 * crates/psd/src/testgen.rs). Covers every depth, colour mode and compression, layers with masks
 * (incl. real masks and parameters), nested groups, all blend modes, Unicode names, empty layers,
 * negative offsets, many tagged blocks, descriptors and resources. Test-only: allocation or
 * encoding failures abort, which is the point of a generator. */
#ifndef PC_TESTS_PSD_TESTGEN_H
#define PC_TESTS_PSD_TESTGEN_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/buf.h"
#include "psd/descriptor.h"
#include "psd/file.h"
#include "psd/internal.h"
#include "psd/resources.h"

static inline void tg_ok(pc_status st, const char *what)
{
    if (st != PC_OK) {
        (void)fprintf(stderr, "testgen: %s failed (%d)\n", what, (int)st);
        abort();
    }
}

#define TG_MODES 8
static const uint16_t TG_MODE_LIST[TG_MODES] = {PC_PSD_MODE_BITMAP, PC_PSD_MODE_GRAYSCALE, PC_PSD_MODE_INDEXED,
                                                PC_PSD_MODE_RGB,    PC_PSD_MODE_CMYK,      PC_PSD_MODE_LAB,
                                                PC_PSD_MODE_MULTICHANNEL, PC_PSD_MODE_DUOTONE};
static const char *const TG_MODE_NAMES[10] = {"Bitmap", "Grayscale", "Indexed", "Rgb", "Cmyk", "?", "?",
                                              "Multichannel", "Duotone", "Lab"};
/* BlendMode Debug names, in PC_PSD_BLEND_KEYS order. */
static const char *const TG_BLEND_NAMES[PC_PSD_BLEND_COUNT] = {
    "PassThrough", "Normal",       "Dissolve",  "Darken",      "Multiply",   "ColorBurn", "LinearBurn",
    "DarkerColor", "Lighten",      "Screen",    "ColorDodge",  "LinearDodge", "LighterColor", "Overlay",
    "SoftLight",   "HardLight",    "VividLight", "LinearLight", "PinLight",  "HardMix",   "Difference",
    "Exclusion",   "Subtract",     "Divide",    "Hue",         "Saturation", "Color",     "Luminosity"};
static const char *const TG_COMP_NAMES[4] = {"Raw", "Rle", "Zip", "ZipPrediction"};

/* Deterministic planar samples for a w × h plane. */
static inline pc_psd_bytes tg_pattern_plane(size_t w, size_t h, uint16_t depth, uint32_t seed)
{
    pc_buf o = PC_BUF_INIT;
    size_t s = seed;
    size_t x;
    size_t y;
    pc_psd_bytes out;
    for (y = 0; y < h; y++) {
        if (depth == 1) {
            size_t rb = pc_psd_row_bytes(w, 1);
            for (x = 0; x < rb; x++) {
                tg_ok(pc_buf_append_byte(&o, (uint8_t)((uint8_t)(x * 37 + y * 11 + s) ^ 0x5a), NULL), "plane");
            }
        } else if (depth == 8) {
            for (x = 0; x < w; x++) {
                uint8_t v = (x / 4 + y) % 3 == 0 ? (uint8_t)(s * 17) : (uint8_t)(x * 3 + y * 7 + s);
                tg_ok(pc_buf_append_byte(&o, v, NULL), "plane");
            }
        } else if (depth == 16) {
            for (x = 0; x < w; x++) {
                uint8_t b[2];
                pc_store_be16(b, (uint16_t)((x * 997 + y * 131 + s * 7919) % 65536));
                tg_ok(pc_buf_append(&o, b, 2, NULL), "plane");
            }
        } else {
            for (x = 0; x < w; x++) {
                uint8_t b[4];
                float v = (float)((x + y + s) % 17) / 16.0f;
                pc_store_be32(b, pc_f32_to_bits(v));
                tg_ok(pc_buf_append(&o, b, 4, NULL), "plane");
            }
        }
    }
    out.data = o.data;
    out.len = o.len;
    return out;
}

static inline uint16_t tg_mode_channels(uint16_t mode)
{
    uint16_t c;
    if (mode == PC_PSD_MODE_MULTICHANNEL) {
        return 2;
    }
    c = pc_psd_mode_color_channels(mode);
    return c != 0 ? c : 1;
}

/* Valid depths for a mode, written to d; returns the count. */
static inline size_t tg_mode_depths(uint16_t mode, uint16_t d[3])
{
    switch (mode) {
    case PC_PSD_MODE_BITMAP:
        d[0] = 1;
        return 1;
    case PC_PSD_MODE_INDEXED:
    case PC_PSD_MODE_DUOTONE:
        d[0] = 8;
        return 1;
    case PC_PSD_MODE_MULTICHANNEL:
    case PC_PSD_MODE_CMYK:
    case PC_PSD_MODE_LAB:
        d[0] = 8;
        d[1] = 16;
        return 2;
    default:
        d[0] = 8;
        d[1] = 16;
        d[2] = 32;
        return 3;
    }
}

static inline pc_psd_bytes tg_color_mode_data(uint16_t mode)
{
    pc_psd_bytes b = {NULL, 0};
    uint32_t i;
    if (mode == PC_PSD_MODE_INDEXED) {
        b.data = malloc(768);
        b.len = 768;
        for (i = 0; i < 768; i++) {
            b.data[i] = (uint8_t)(i * 7 % 256);
        }
    } else if (mode == PC_PSD_MODE_DUOTONE) {
        b.data = malloc(24);
        b.len = 24;
        for (i = 0; i < 24; i++) {
            b.data[i] = (uint8_t)i;
        }
    }
    return b;
}

static inline void tg_push_res(pc_psd_file *f, pc_psd_resource *r)
{
    f->resources = realloc(f->resources, (f->nresources + 1) * sizeof *f->resources);
    if (f->resources == NULL) {
        abort();
    }
    f->resources[f->nresources++] = *r;
}

static inline void tg_res(pc_psd_file *f, uint16_t id, const void *data, size_t len)
{
    pc_psd_resource r;
    tg_ok(pc_psd_resource_init(&r, id, data, len, NULL), "resource");
    tg_push_res(f, &r);
}

static inline void tg_resolution(pc_psd_file *f)
{
    pc_psd_resolution ri = pc_psd_resolution_from_dpi(72.0);
    uint8_t d[16];
    pc_psd_resolution_write(&ri, d);
    tg_res(f, PC_PSD_RES_RESOLUTION_INFO, d, 16);
}

static inline void tg_base_resources(pc_psd_file *f)
{
    static const uint8_t thirty[4] = {0, 0, 0, 30};
    static const char xmp[] = "<x:xmpmeta xmlns:x='adobe:ns:meta/'/>";
    static const uint8_t named_data[3] = {1, 2, 3};
    uint8_t icc[13];
    pc_psd_resource named;
    pc_psd_resource vi;
    memset(icc, 0x42, sizeof icc);
    tg_resolution(f);
    tg_res(f, PC_PSD_RES_GLOBAL_ANGLE, thirty, 4);
    tg_res(f, PC_PSD_RES_GLOBAL_ALTITUDE, thirty, 4);
    tg_res(f, PC_PSD_RES_ICC_PROFILE, icc, 13);
    tg_res(f, PC_PSD_RES_XMP, xmp, strlen(xmp));
    tg_ok(pc_psd_resource_init(&named, 2000, named_data, 3, NULL), "named");
    tg_ok(pc_psd_bytes_set(&named.name, "Path 1", 6, NULL), "name");
    tg_push_res(f, &named);
    tg_ok(pc_psd_version_info_resource(true, &vi, NULL), "version info");
    tg_push_res(f, &vi);
}

static inline void tg_encode_merged(pc_psd_file *f, uint16_t compression, uint32_t seed0, bool may_fail)
{
    pc_buf planes = PC_BUF_INIT;
    uint16_t c;
    pc_status st;
    for (c = 0; c < f->header.channels; c++) {
        pc_psd_bytes p = tg_pattern_plane(f->header.width, f->header.height, f->header.depth, seed0 + c);
        tg_ok(pc_buf_append(&planes, p.data, p.len, NULL), "planes");
        pc_psd_bytes_fini(&p);
    }
    f->image_compression = compression;
    st = pc_psd_image_encode(compression, planes.data, planes.len, &f->header, &f->image_data, NULL);
    if (!may_fail) {
        tg_ok(st, "encode merged");
    }
    pc_buf_fini(&planes);
}

/* A flattened file (no layer section) with a patterned merged image. */
static inline pc_psd_file tg_merged_only(pc_psd_version v, uint16_t mode, uint16_t depth, uint16_t compression, uint32_t w,
                                  uint32_t h)
{
    pc_psd_file f;
    pc_psd_file_init(&f);
    f.header = pc_psd_header_make(v, w, h, tg_mode_channels(mode), depth, mode);
    tg_encode_merged(&f, compression, 0, true);
    f.color_mode_data = tg_color_mode_data(mode);
    tg_base_resources(&f);
    return f;
}

static inline void tg_block(pc_psd_layer *l, pc_status st, pc_psd_block *b)
{
    tg_ok(st, "block");
    tg_ok(pc_psd_layer_push_block(l, b, NULL), "push block");
}

static inline void tg_channel(pc_psd_layer *l, int16_t id, uint16_t comp, size_t w, size_t h, uint16_t depth,
                       pc_psd_version v, uint32_t seed)
{
    pc_psd_bytes p = tg_pattern_plane(w, h, depth, seed);
    pc_psd_channel c;
    tg_ok(pc_psd_channel_encode(id, comp, p.data, p.len, w, h, depth, v, &c, NULL), "encode channel");
    tg_ok(pc_psd_layer_push_channel(l, &c, NULL), "push channel");
    pc_psd_bytes_fini(&p);
}

static inline size_t tg_w(pc_psd_rect r)
{
    size_t w = 0;
    size_t h = 0;
    if (pc_psd_rect_size(r, &w, &h, NULL) != PC_OK) {
        return 0;
    }
    return w;
}

static inline size_t tg_h(pc_psd_rect r)
{
    size_t w = 0;
    size_t h = 0;
    if (pc_psd_rect_size(r, &w, &h, NULL) != PC_OK) {
        return 0;
    }
    return h;
}

/* mask may be NULL; it is copied. */
static inline pc_psd_layer tg_raster_layer(const pc_psd_header *hd, uint16_t compression, pc_psd_rect rect, const char *name,
                                    const char blend[4], const pc_psd_layer_mask *mask, uint32_t seed)
{
    pc_psd_layer l;
    size_t w = tg_w(rect);
    size_t h = tg_h(rect);
    int16_t cc = (int16_t)tg_mode_channels(hd->color_mode);
    int16_t id;
    pc_psd_block b;
    pc_psd_layer_init(&l);
    for (id = -1; id < cc; id++) {
        uint16_t comp = id == -1 && compression == PC_PSD_RLE ? PC_PSD_ZIP : compression;
        tg_channel(&l, id, comp, w, h, hd->depth, hd->version, seed + (uint32_t)(id + 1));
    }
    if (mask != NULL) {
        tg_channel(&l, -2, compression, tg_w(mask->rect), tg_h(mask->rect), hd->depth, hd->version, seed + 99);
        if (mask->has_real) {
            tg_channel(&l, -3, compression, tg_w(mask->real.rect), tg_h(mask->real.rect), hd->depth, hd->version, seed + 7);
        }
        l.mask.kind = PC_PSD_MASK_PARSED;
        l.mask.mask = *mask;
        l.mask.mask.trailing.data = NULL;
        l.mask.mask.trailing.len = 0;
        tg_ok(pc_psd_bytes_clone(&mask->trailing, &l.mask.mask.trailing, NULL), "mask");
    }
    l.flags = seed % 5 == 4 ? PC_PSD_LF_HIDDEN : 0;
    l.rect = rect;
    memcpy(l.blend, blend, 4);
    l.opacity = (uint8_t)(255 - (seed * 13) % 128);
    l.clipping = seed % 7 == 3 ? 1 : 0;
    tg_ok(pc_psd_blending_ranges_full((size_t)cc, &l.blending_ranges, NULL), "ranges");
    tg_ok(psd_encode_legacy_name(name, &l.name, NULL), "legacy name");
    tg_block(&l, pc_psd_block_make_unicode_name(name, &b, NULL), &b);
    tg_block(&l, pc_psd_block_make_u32("lyid", seed + 1, &b, NULL), &b);
    return l;
}

static inline pc_psd_layer tg_group_record(const pc_psd_header *hd, const char *name, uint32_t kind, const char blend[4],
                                    uint32_t id)
{
    pc_psd_layer l;
    int16_t cc = (int16_t)tg_mode_channels(hd->color_mode);
    int16_t c;
    pc_psd_block b;
    uint32_t zero = 0;
    pc_psd_layer_init(&l);
    for (c = -1; c < cc; c++) {
        pc_psd_channel ch;
        memset(&ch, 0, sizeof ch);
        ch.id = c;
        ch.has_compression = true;
        ch.compression = PC_PSD_RAW;
        tg_ok(pc_psd_layer_push_channel(&l, &ch, NULL), "channel");
    }
    memcpy(l.blend, blend, 4);
    tg_ok(psd_encode_legacy_name(name, &l.name, NULL), "legacy name");
    tg_ok(pc_psd_blending_ranges_full((size_t)cc, &l.blending_ranges, NULL), "ranges");
    tg_block(&l, pc_psd_block_make_unicode_name(name, &b, NULL), &b);
    tg_block(&l, pc_psd_block_make_u32("lyid", id, &b, NULL), &b);
    if (kind == PC_PSD_SECTION_DIVIDER) {
        tg_block(&l, pc_psd_block_make_section(kind, NULL, NULL, &b, NULL), &b);
    } else {
        tg_block(&l, pc_psd_block_make_section(kind, (const uint8_t *)blend, &zero, &b, NULL), &b);
    }
    return l;
}

static inline void tg_add(pc_psd_desc *d, const char *key, pc_psd_value v)
{
    tg_ok(pc_psd_desc_add(d, key, &v, NULL), "desc add");
}

static inline pc_psd_value tg_text(const char *s, bool nul)
{
    pc_psd_value v;
    tg_ok(pc_psd_value_text(s, nul, &v, NULL), "text");
    return v;
}

static inline pc_psd_value tg_obj(pc_psd_desc *d)
{
    pc_psd_value v;
    tg_ok(pc_psd_value_object(d, &v, NULL), "object");
    return v;
}

static inline pc_psd_value tg_unit(const char unit[4], double value)
{
    pc_psd_value v;
    memset(&v, 0, sizeof v);
    v.kind = PC_PSD_V_UNIT_FLOAT;
    memcpy(v.unit, unit, 4);
    v.d = value;
    return v;
}

/* The descriptor used by lfx2-style blocks (versioned, version 16). */
static inline pc_psd_desc tg_sample_descriptor(void)
{
    pc_psd_desc root;
    pc_psd_desc drsh;
    pc_psd_desc clr;
    pc_psd_value e;
    pc_psd_value list;
    pc_psd_value big;
    tg_ok(pc_psd_desc_init(&root, "null", NULL), "desc");
    tg_add(&root, "Scl ", tg_unit("#Prc", 100.0));
    tg_add(&root, "masterFXSwitch", pc_psd_value_bool(true));
    tg_ok(pc_psd_desc_init(&drsh, "DrSh", NULL), "desc");
    tg_add(&drsh, "enab", pc_psd_value_bool(true));
    tg_ok(pc_psd_value_enum("BlnM", "Mltp", &e, NULL), "enum");
    tg_add(&drsh, "Md  ", e);
    tg_ok(pc_psd_desc_init(&clr, "RGBC", NULL), "desc");
    tg_add(&clr, "Rd  ", pc_psd_value_double(0.0));
    tg_add(&drsh, "Clr ", tg_obj(&clr));
    tg_add(&drsh, "lagl", tg_unit("#Ang", 120.0));
    tg_add(&drsh, "Nm  ", tg_text("Shadow", true));
    tg_add(&root, "DrSh", tg_obj(&drsh));
    memset(&list, 0, sizeof list);
    list.kind = PC_PSD_V_LIST;
    e = pc_psd_value_int(1);
    tg_ok(pc_psd_value_list_push(&list, &e, NULL), "list");
    memset(&big, 0, sizeof big);
    big.kind = PC_PSD_V_LARGE_INTEGER;
    big.l = -2;
    tg_ok(pc_psd_value_list_push(&list, &big, NULL), "list");
    tg_add(&root, "list", list);
    return root;
}

static inline void tg_extra_blocks(pc_psd_layer *l, uint32_t seed)
{
    pc_psd_block b;
    pc_psd_desc d = tg_sample_descriptor();
    pc_psd_bytes vd = {NULL, 0};
    pc_buf lfx2 = PC_BUF_INIT;
    static const uint8_t cont[4] = {'c', 'o', 'n', 't'};
    static const uint8_t zero4[4] = {0, 0, 0, 0};
    static const uint8_t zero16[16];
    static const uint8_t odd[3] = {1, 2, 3};
    static const uint8_t nine[1] = {9};
    tg_ok(pc_psd_vdesc_to_bytes(&d, &vd, NULL), "vdesc");
    pc_psd_desc_fini(&d);
    tg_ok(pc_buf_append(&lfx2, zero4, 4, NULL), "lfx2");
    tg_ok(pc_buf_append(&lfx2, vd.data, vd.len, NULL), "lfx2");
    pc_psd_bytes_fini(&vd);
    tg_block(l, pc_psd_block_make_name_source(cont, &b, NULL), &b);
    tg_block(l, pc_psd_block_make_flag("clbl", 1, &b, NULL), &b);
    tg_block(l, pc_psd_block_make_flag("infx", 0, &b, NULL), &b);
    tg_block(l, pc_psd_block_make_flag("knko", 0, &b, NULL), &b);
    tg_block(l, pc_psd_block_make_u32("lspf", seed % 8, &b, NULL), &b);
    tg_block(l, pc_psd_block_make_sheet_color((uint16_t)(seed % 8), &b, NULL), &b);
    tg_block(l, pc_psd_block_make_flag("iOpa", 200, &b, NULL), &b);
    tg_block(l, pc_psd_block_init(&b, "shmd", zero4, 4, NULL), &b);
    tg_block(l, pc_psd_block_init(&b, "lfx2", lfx2.data, lfx2.len, NULL), &b);
    pc_buf_fini(&lfx2);
    tg_block(l, pc_psd_block_init(&b, "fxrp", zero16, 16, NULL), &b);
    tg_ok(pc_psd_block_init(&b, "zOdd", odd, 3, NULL), "odd");
    b.padding.has = true;
    tg_ok(pc_psd_bytes_set(&b.padding.v, zero4, 3, NULL), "pad");
    tg_block(l, PC_OK, &b);
    tg_ok(pc_psd_block_init(&b, "zNop", nine, 1, NULL), "nop");
    b.padding.has = true;
    tg_block(l, PC_OK, &b);
}

typedef struct tg_state {
    const pc_psd_header *hd;
    uint16_t compression;
    uint32_t seed;
    pc_psd_layer_info *li;
} tg_state;

static inline void tg_next(tg_state *s, pc_psd_rect rect, const char *name, const char blend[4], const pc_psd_layer_mask *mask)
{
    pc_psd_layer l;
    s->seed++;
    l = tg_raster_layer(s->hd, s->compression, rect, name, blend, mask, s->seed);
    if (s->seed % 3 == 0) {
        tg_extra_blocks(&l, s->seed);
    }
    tg_ok(pc_psd_layer_info_push(s->li, &l, NULL), "push layer");
}

static inline void tg_push(pc_psd_layer_info *li, pc_psd_layer l)
{
    tg_ok(pc_psd_layer_info_push(li, &l, NULL), "push layer");
}

static inline void tg_global(pc_psd_file *f, const char key[4], const void *data, size_t len)
{
    pc_psd_block b;
    tg_ok(pc_psd_block_init(&b, key, data, len, NULL), "global");
    f->global_blocks = realloc(f->global_blocks, (f->nglobal + 1) * sizeof *f->global_blocks);
    if (f->global_blocks == NULL) {
        abort();
    }
    f->global_blocks[f->nglobal++] = b;
}

/* A layered file: masks, nested groups, every blend mode, names, an empty layer, negative
 * offsets and many tagged blocks. */
static inline pc_psd_file tg_layered(pc_psd_version v, uint16_t mode, uint16_t depth, uint16_t compression)
{
    static const uint8_t gmask[14] = {0, 0, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 50, 128, 0};
    static const uint8_t txt2[4] = {0, 1, 2, 3};
    static const uint8_t fmsk[10];
    uint32_t w = 24;
    uint32_t h = 16;
    pc_psd_file f;
    tg_state s;
    pc_psd_layer_mask m1;
    pc_psd_layer_mask real;
    uint32_t id0 = 1000;
    int i;
    pc_psd_file_init(&f);
    f.header = pc_psd_header_make(v, w, h, (uint16_t)(tg_mode_channels(mode) + 1), depth, mode);
    s.hd = &f.header;
    s.compression = compression;
    s.seed = 0;
    s.li = &f.layer_info;
    tg_next(&s, pc_psd_rect_from_xywh(0, 0, w, h), "Background", "norm", NULL);
    tg_ok(pc_psd_layer_mask_make(pc_psd_rect_from_xywh(-2, -2, 6, 5), 0, PC_PSD_MF_RELATIVE, &m1, NULL), "mask");
    tg_next(&s, pc_psd_rect_from_xywh(-5, -3, 10, 7), "Neg offset \xF0\x9F\x98\x80", "mul ", &m1);
    pc_psd_layer_mask_fini(&m1);
    tg_next(&s, (pc_psd_rect){0, 0, 0, 0}, "Empty", "scrn", NULL);
    tg_next(&s, pc_psd_rect_from_xywh(-100, 5, 400, 2), "Wide", "over", NULL);
    tg_push(&f.layer_info, tg_group_record(&f.header, "</Layer group>", PC_PSD_SECTION_DIVIDER, "norm", id0));
    tg_next(&s, pc_psd_rect_from_xywh(2, 2, 5, 5), "In outer", "dark", NULL);
    tg_push(&f.layer_info, tg_group_record(&f.header, "</Layer group>", PC_PSD_SECTION_DIVIDER, "norm", id0 + 1));
    memset(&real, 0, sizeof real);
    real.rect = pc_psd_rect_from_xywh(1, 1, 4, 3);
    real.default_color = 255;
    real.flags = PC_PSD_MF_PARAMETERS;
    real.has_params = true;
    real.params.flags = 3;
    real.params.has_user_density = true;
    real.params.user_density = 200;
    real.params.has_user_feather = true;
    real.params.user_feather = 1.5;
    real.has_real = true;
    real.real.flags = 0;
    real.real.background = 255;
    real.real.rect = pc_psd_rect_from_xywh(0, 0, 3, 3);
    real.real_first = true;
    tg_next(&s, pc_psd_rect_from_xywh(3, 3, 6, 4), "Gr\xC3\xBC\xC3\x9F" "e", "div ", &real);
    tg_push(&f.layer_info, tg_group_record(&f.header, "Inner", PC_PSD_SECTION_CLOSED, "norm", id0 + 2));
    tg_push(&f.layer_info, tg_group_record(&f.header, "Outer", PC_PSD_SECTION_OPEN, "pass", id0 + 3));
    for (i = 0; i < PC_PSD_BLEND_COUNT; i++) {
        char name[64];
        (void)snprintf(name, sizeof name, "blend %s", TG_BLEND_NAMES[i]);
        tg_next(&s, pc_psd_rect_from_xywh(i % 20, i % 12, 3, 2), name, PC_PSD_BLEND_KEYS[i], NULL);
    }
    tg_next(&s, pc_psd_rect_from_xywh(0, 0, 2, 2), "unknown blend", "zzzz", NULL);
    tg_encode_merged(&f, compression, 50, false);
    f.has_layer_info = true;
    f.layer_info.merged_alpha = true;
    if (depth == 16 || depth == 32) {
        f.placement.where = PC_PSD_LI_GLOBAL_BLOCK;
        f.placement.index = 1;
        memcpy(f.placement.signature, "8BIM", 4);
        memcpy(f.placement.key, depth == 16 ? "Lr16" : "Lr32", 4);
        tg_global(&f, "Patt", NULL, 0);
        tg_global(&f, "Txt2", txt2, 4);
    } else {
        tg_global(&f, "Patt", NULL, 0);
        tg_global(&f, "FMsk", fmsk, 10);
    }
    f.color_mode_data = tg_color_mode_data(mode);
    tg_base_resources(&f);
    f.has_global_mask = true;
    tg_ok(pc_psd_bytes_set(&f.global_mask, gmask, 14, NULL), "gmask");
    return f;
}

/* A tiny layered RGB file (truncation sweeps). */
static inline pc_psd_file tg_small(pc_psd_version v, uint16_t compression)
{
    static const uint8_t patt[2] = {1, 2};
    pc_psd_file f;
    pc_psd_layer_mask mask;
    pc_psd_file_init(&f);
    f.header = pc_psd_header_make(v, 4, 3, 4, 8, PC_PSD_MODE_RGB);
    tg_ok(pc_psd_layer_mask_make(pc_psd_rect_from_xywh(0, 0, 2, 2), 0, 0, &mask, NULL), "mask");
    tg_push(&f.layer_info, tg_raster_layer(&f.header, compression, pc_psd_rect_from_xywh(0, 0, 4, 3), "a", "norm", &mask, 1));
    pc_psd_layer_mask_fini(&mask);
    tg_push(&f.layer_info, tg_group_record(&f.header, "</Layer group>", PC_PSD_SECTION_DIVIDER, "norm", 9));
    tg_push(&f.layer_info, tg_raster_layer(&f.header, compression, pc_psd_rect_from_xywh(-1, 1, 2, 2), "b", "scrn", NULL, 2));
    tg_push(&f.layer_info, tg_group_record(&f.header, "g", PC_PSD_SECTION_OPEN, "pass", 10));
    tg_encode_merged(&f, compression, 0, false);
    tg_resolution(&f);
    f.has_layer_info = true;
    f.layer_info.merged_alpha = true;
    f.has_global_mask = true;
    tg_global(&f, "Patt", patt, 2);
    return f;
}

typedef struct tg_case {
    char name[96];
    pc_psd_file file;
} tg_case;

/* Every generated case (caller frees with tg_free_cases). */
static inline tg_case *tg_all_cases(size_t *n)
{
    tg_case *v = NULL;
    size_t k = 0;
    int vi;
    for (vi = 0; vi < 2; vi++) {
        pc_psd_version ver = vi == 0 ? PC_PSD_V1 : PC_PSD_V2;
        const char *vn = vi == 0 ? "Psd" : "Psb";
        size_t m;
        uint16_t c;
        static const uint16_t LAYERED[4] = {PC_PSD_MODE_GRAYSCALE, PC_PSD_MODE_RGB, PC_PSD_MODE_CMYK, PC_PSD_MODE_LAB};
        for (m = 0; m < TG_MODES; m++) {
            uint16_t d[3];
            size_t nd = tg_mode_depths(TG_MODE_LIST[m], d);
            size_t j;
            for (j = 0; j < nd; j++) {
                for (c = 0; c < 4; c++) {
                    v = realloc(v, (k + 1) * sizeof *v);
                    if (v == NULL) {
                        abort();
                    }
                    (void)snprintf(v[k].name, sizeof v[k].name, "merged %s %s %ubit %s", vn,
                                   TG_MODE_NAMES[TG_MODE_LIST[m]], (unsigned)d[j], TG_COMP_NAMES[c]);
                    v[k].file = tg_merged_only(ver, TG_MODE_LIST[m], d[j], c, 13, 7);
                    k++;
                }
            }
        }
        for (m = 0; m < 4; m++) {
            uint16_t d[3];
            size_t nd = tg_mode_depths(LAYERED[m], d);
            size_t j;
            for (j = 0; j < nd; j++) {
                if (d[j] < 8) {
                    continue;
                }
                for (c = 0; c < 4; c++) {
                    v = realloc(v, (k + 1) * sizeof *v);
                    if (v == NULL) {
                        abort();
                    }
                    (void)snprintf(v[k].name, sizeof v[k].name, "layered %s %s %ubit %s", vn,
                                   TG_MODE_NAMES[LAYERED[m]], (unsigned)d[j], TG_COMP_NAMES[c]);
                    v[k].file = tg_layered(ver, LAYERED[m], d[j], c);
                    k++;
                }
            }
        }
        for (c = 0; c < 4; c++) {
            v = realloc(v, (k + 1) * sizeof *v);
            if (v == NULL) {
                abort();
            }
            (void)snprintf(v[k].name, sizeof v[k].name, "small %s %s", vn, TG_COMP_NAMES[c]);
            v[k].file = tg_small(ver, c);
            k++;
        }
        v = realloc(v, (k + 1) * sizeof *v);
        if (v == NULL) {
            abort();
        }
        (void)snprintf(v[k].name, sizeof v[k].name, "1x1 canvas %s", vn);
        v[k].file = tg_merged_only(ver, PC_PSD_MODE_RGB, 8, PC_PSD_RLE, 1, 1);
        k++;
    }
    *n = k;
    return v;
}

static inline void tg_free_cases(tg_case *v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        pc_psd_file_fini(&v[i].file);
    }
    free(v);
}

#endif
