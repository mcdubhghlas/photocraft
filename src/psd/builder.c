#include "psd/builder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/mem.h"
#include "psd/internal.h"
#include "psd/misc.h"

typedef struct owned_px {
    pc_psd_px_kind kind;
    void *data;
    size_t len;
} owned_px;

typedef struct owned_layer {
    char *name;
    int32_t left;
    int32_t top;
    uint32_t width;
    uint32_t height;
    owned_px px;
    uint8_t blend[4];
    uint8_t opacity;
    bool has_fill;
    uint8_t fill;
    bool visible;
    bool clipping;
    bool has_mask;
    pc_psd_rect mrect;
    pc_psd_bytes mdata;
    uint8_t mdefault;
    bool mdisabled;
    pc_psd_block *extra;
    size_t nextra;
} owned_layer;

typedef struct owned_group {
    char *name;
    uint8_t blend[4];
    uint8_t opacity;
    bool visible;
    bool open;
} owned_group;

typedef enum entry_kind { ENTRY_LAYER, ENTRY_GROUP_START, ENTRY_GROUP_END } entry_kind;

typedef struct entry {
    entry_kind kind;
    owned_layer layer;
    owned_group group;
} entry;

struct pc_psd_builder {
    uint32_t width;
    uint32_t height;
    uint16_t color_mode;
    uint16_t depth;
    pc_psd_version version;
    uint16_t compression;
    entry *entries;
    size_t n;
    size_t cap;
    owned_group *open;
    size_t nopen;
    size_t capopen;
    bool has_composite;
    owned_px composite;
    pc_psd_resource *resources;
    size_t nres;
};

/* ---- pixel data ---- */

static bool is16(pc_psd_px_kind k)
{
    return k == PC_PSD_PX_RGBA16 || k == PC_PSD_PX_GRAYA16 || k == PC_PSD_PX_CMYKA16;
}

static uint16_t px_mode(pc_psd_px_kind k)
{
    switch (k) {
    case PC_PSD_PX_RGBA8:
    case PC_PSD_PX_RGBA16:
        return PC_PSD_MODE_RGB;
    case PC_PSD_PX_GRAYA8:
    case PC_PSD_PX_GRAYA16:
        return PC_PSD_MODE_GRAYSCALE;
    default:
        return PC_PSD_MODE_CMYK;
    }
}

static size_t spp(pc_psd_px_kind k)
{
    uint16_t m = px_mode(k);
    return m == PC_PSD_MODE_RGB ? 4 : m == PC_PSD_MODE_GRAYSCALE ? 2 : 5;
}

static uint32_t sample(const owned_px *p, size_t i)
{
    return is16(p->kind) ? ((const uint16_t *)p->data)[i] : ((const uint8_t *)p->data)[i];
}

static const char *mode_debug(uint16_t m, char buf[32])
{
    switch (m) {
    case PC_PSD_MODE_BITMAP:
        return "Bitmap";
    case PC_PSD_MODE_GRAYSCALE:
        return "Grayscale";
    case PC_PSD_MODE_INDEXED:
        return "Indexed";
    case PC_PSD_MODE_RGB:
        return "Rgb";
    case PC_PSD_MODE_CMYK:
        return "Cmyk";
    case PC_PSD_MODE_MULTICHANNEL:
        return "Multichannel";
    case PC_PSD_MODE_DUOTONE:
        return "Duotone";
    case PC_PSD_MODE_LAB:
        return "Lab";
    default:
        (void)snprintf(buf, 32, "Unknown(%u)", (unsigned)m);
        return buf;
    }
}

/* Planar big-endian plane for interleaved component c, as stored (CMYK inverted). */
static pc_status plane(const owned_px *p, size_t c, size_t n, pc_psd_bytes *out, pc_error *err)
{
    size_t s = spp(p->kind);
    bool invert = px_mode(p->kind) == PC_PSD_MODE_CMYK && c < 4;
    bool wide = is16(p->kind);
    size_t bytes = n * (wide ? 2 : 1);
    uint8_t *o = NULL;
    size_t i;
    if (n == 0) {
        out->data = NULL;
        out->len = 0;
        return PC_OK;
    }
    o = malloc(bytes);
    if (o == NULL) {
        return psd_nomem(err);
    }
    for (i = 0; i < n; i++) {
        uint32_t v = sample(p, i * s + c);
        if (wide) {
            pc_store_be16(o + 2 * i, (uint16_t)(invert ? 65535u - v : v));
        } else {
            o[i] = (uint8_t)(invert ? 255u - v : v);
        }
    }
    out->data = o;
    out->len = bytes;
    return PC_OK;
}

/* Merged composite plane: Photoshop stores transparent RGB composites against white (layer channels
 * stay straight); other planes as `plane`. */
static pc_status composite_plane(const owned_px *p, size_t c, size_t n, pc_psd_bytes *out, pc_error *err)
{
    bool wide = p->kind == PC_PSD_PX_RGBA16;
    uint8_t *o;
    size_t i;
    if ((p->kind != PC_PSD_PX_RGBA8 && !wide) || c >= 3) {
        return plane(p, c, n, out, err);
    }
    if (n == 0) {
        out->data = NULL;
        out->len = 0;
        return PC_OK;
    }
    o = malloc(n * (wide ? 2u : 1u));
    if (o == NULL) {
        return psd_nomem(err);
    }
    for (i = 0; i < n; i++) {
        uint32_t color = sample(p, i * 4 + c);
        uint32_t alpha = sample(p, i * 4 + 3);
        if (wide) {
            uint32_t v = alpha == 0 ? color : (color * alpha + 32767u) / 65535u + 65535u - alpha;
            pc_store_be16(o + 2 * i, (uint16_t)v);
        } else {
            o[i] = (uint8_t)(alpha == 0 ? color : (color * alpha + 127u) / 255u + 255u - alpha);
        }
    }
    out->data = o;
    out->len = n * (wide ? 2u : 1u);
    return PC_OK;
}

static bool alpha_is_opaque(const owned_px *p, size_t n)
{
    size_t s = spp(p->kind);
    uint32_t full = is16(p->kind) ? 65535u : 255u;
    size_t i;
    for (i = 0; i < n; i++) {
        if (sample(p, i * s + s - 1) != full) {
            return false;
        }
    }
    return true;
}

static pc_status copy_px(const pc_psd_pixels *p, owned_px *out, pc_error *err)
{
    size_t bytes = p->len * (is16(p->kind) ? 2 : 1);
    out->kind = p->kind;
    out->len = p->len;
    out->data = NULL;
    if (bytes > 0) {
        out->data = malloc(bytes);
        if (out->data == NULL) {
            return psd_nomem(err);
        }
        memcpy(out->data, p->data, bytes);
    }
    return PC_OK;
}

static char *dup_str(const char *s)
{
    size_t n = strlen(s != NULL ? s : "");
    char *d = malloc(n + 1);
    if (d != NULL) {
        memcpy(d, s != NULL ? s : "", n + 1);
    }
    return d;
}

/* ---- builder ---- */

pc_psd_layer_spec pc_psd_layer_spec_make(const char *name, int32_t left, int32_t top, uint32_t width, uint32_t height,
                                         pc_psd_pixels pixels)
{
    pc_psd_layer_spec s;
    memset(&s, 0, sizeof s);
    s.name = name;
    s.left = left;
    s.top = top;
    s.width = width;
    s.height = height;
    s.pixels = pixels;
    memcpy(s.blend, "norm", 4);
    s.opacity = 255;
    s.visible = true;
    return s;
}

pc_psd_group_spec pc_psd_group_spec_make(const char *name)
{
    pc_psd_group_spec g;
    g.name = name;
    memcpy(g.blend, "pass", 4);
    g.opacity = 255;
    g.visible = true;
    g.open = true;
    return g;
}

pc_psd_builder *pc_psd_builder_new(uint32_t width, uint32_t height)
{
    pc_psd_builder *b = calloc(1, sizeof *b);
    if (b != NULL) {
        b->width = width;
        b->height = height;
        b->color_mode = PC_PSD_MODE_RGB;
        b->depth = 8;
        b->version = PC_PSD_V1;
        b->compression = PC_PSD_RLE;
    }
    return b;
}

static void layer_fini(owned_layer *l)
{
    free(l->name);
    free(l->px.data);
    pc_psd_bytes_fini(&l->mdata);
    pc_psd_blocks_free(l->extra, l->nextra);
    memset(l, 0, sizeof *l);
}

void pc_psd_builder_free(pc_psd_builder *b)
{
    size_t i;
    if (b == NULL) {
        return;
    }
    for (i = 0; i < b->n; i++) {
        layer_fini(&b->entries[i].layer);
        free(b->entries[i].group.name);
    }
    free(b->entries);
    for (i = 0; i < b->nopen; i++) {
        free(b->open[i].name);
    }
    free(b->open);
    free(b->composite.data);
    pc_psd_resources_free(b->resources, b->nres);
    free(b);
}

void pc_psd_builder_color_mode(pc_psd_builder *b, uint16_t mode)
{
    b->color_mode = mode;
}

void pc_psd_builder_depth(pc_psd_builder *b, uint16_t depth)
{
    b->depth = depth;
}

void pc_psd_builder_version(pc_psd_builder *b, pc_psd_version v)
{
    b->version = v;
}

void pc_psd_builder_compression(pc_psd_builder *b, uint16_t c)
{
    b->compression = c;
}

static pc_status push_resource(pc_psd_builder *b, pc_psd_resource *r, pc_error *err)
{
    pc_psd_resource *g = realloc(b->resources, (b->nres + 1) * sizeof *g);
    if (g == NULL) {
        pc_psd_resource_fini(r);
        return psd_nomem(err);
    }
    b->resources = g;
    b->resources[b->nres++] = *r;
    return PC_OK;
}

pc_status pc_psd_builder_resolution(pc_psd_builder *b, double dpi, pc_error *err)
{
    pc_psd_resolution ri = pc_psd_resolution_from_dpi(dpi);
    uint8_t d[16];
    pc_psd_resource r;
    pc_status st;
    pc_psd_resolution_write(&ri, d);
    st = pc_psd_resource_init(&r, PC_PSD_RES_RESOLUTION_INFO, d, 16, err);
    return st == PC_OK ? push_resource(b, &r, err) : st;
}

pc_status pc_psd_builder_icc(pc_psd_builder *b, const uint8_t *icc, size_t len, pc_error *err)
{
    pc_psd_resource r;
    pc_status st = pc_psd_resource_init(&r, PC_PSD_RES_ICC_PROFILE, icc, len, err);
    return st == PC_OK ? push_resource(b, &r, err) : st;
}

pc_status pc_psd_builder_resource(pc_psd_builder *b, const pc_psd_resource *res, pc_error *err)
{
    pc_psd_resource r;
    pc_status st = pc_psd_resource_clone(res, &r, err);
    return st == PC_OK ? push_resource(b, &r, err) : st;
}

static pc_status push_entry(pc_psd_builder *b, entry *e, pc_error *err)
{
    if (b->n == b->cap) {
        entry *g = pc_grow_array(b->entries, &b->cap, b->n + 1, sizeof *g);
        if (g == NULL) {
            layer_fini(&e->layer);
            free(e->group.name);
            return psd_nomem(err);
        }
        b->entries = g;
    }
    b->entries[b->n++] = *e;
    return PC_OK;
}

pc_status pc_psd_builder_push_layer(pc_psd_builder *b, const pc_psd_layer_spec *spec, pc_error *err)
{
    entry e;
    owned_layer *l = &e.layer;
    pc_status st;
    memset(&e, 0, sizeof e);
    e.kind = ENTRY_LAYER;
    l->name = dup_str(spec->name);
    if (l->name == NULL) {
        return psd_nomem(err);
    }
    l->left = spec->left;
    l->top = spec->top;
    l->width = spec->width;
    l->height = spec->height;
    memcpy(l->blend, spec->blend, 4);
    l->opacity = spec->opacity;
    l->has_fill = spec->has_fill_opacity;
    l->fill = spec->fill_opacity;
    l->visible = spec->visible;
    l->clipping = spec->clipping;
    CK(copy_px(&spec->pixels, &l->px, err));
    if (spec->mask != NULL) {
        l->has_mask = true;
        l->mrect = spec->mask->rect;
        l->mdefault = spec->mask->default_color;
        l->mdisabled = spec->mask->disabled;
        CK(pc_psd_bytes_set(&l->mdata, spec->mask->data, spec->mask->len, err));
    }
    CK(pc_psd_blocks_clone(spec->extra_blocks, spec->nextra, &l->extra, err));
    l->nextra = spec->nextra;
    return push_entry(b, &e, err);
fail:
    layer_fini(l);
    return st;
}

static pc_status own_group(const pc_psd_group_spec *spec, owned_group *g, pc_error *err)
{
    g->name = dup_str(spec->name);
    if (g->name == NULL) {
        return psd_nomem(err);
    }
    memcpy(g->blend, spec->blend, 4);
    g->opacity = spec->opacity;
    g->visible = spec->visible;
    g->open = spec->open;
    return PC_OK;
}

pc_status pc_psd_builder_begin_group(pc_psd_builder *b, const pc_psd_group_spec *spec, pc_error *err)
{
    entry e;
    owned_group g;
    pc_status st;
    if (b->nopen == b->capopen) {
        owned_group *grown = pc_grow_array(b->open, &b->capopen, b->nopen + 1, sizeof *grown);
        if (grown == NULL) {
            return psd_nomem(err);
        }
        b->open = grown;
    }
    st = own_group(spec, &g, err);
    if (st != PC_OK) {
        return st;
    }
    memset(&e, 0, sizeof e);
    e.kind = ENTRY_GROUP_START;
    st = push_entry(b, &e, err);
    if (st != PC_OK) {
        free(g.name);
        return st;
    }
    b->open[b->nopen++] = g;
    return PC_OK;
}

pc_status pc_psd_builder_end_group(pc_psd_builder *b, pc_error *err)
{
    entry e;
    if (b->nopen == 0) {
        return psd_misuse(err, "end_group without begin_group");
    }
    memset(&e, 0, sizeof e);
    e.kind = ENTRY_GROUP_END;
    e.group = b->open[b->nopen - 1];
    b->nopen--;
    return push_entry(b, &e, err);
}

pc_status pc_psd_builder_composite(pc_psd_builder *b, pc_psd_pixels pixels, pc_error *err)
{
    owned_px p;
    pc_status st = copy_px(&pixels, &p, err);
    if (st != PC_OK) {
        return st;
    }
    free(b->composite.data);
    b->composite = p;
    b->has_composite = true;
    return PC_OK;
}

static pc_status check_pixels(const pc_psd_builder *b, const owned_px *p, size_t n, pc_error *err)
{
    char m1[32];
    char m2[32];
    uint16_t mode = px_mode(p->kind);
    uint16_t depth = is16(p->kind) ? 16 : 8;
    if (mode != b->color_mode || depth != b->depth) {
        return psd_misuse(err, "pixel format (%s, %u) does not match document %s/%u", mode_debug(mode, m1),
                          (unsigned)depth, mode_debug(b->color_mode, m2), (unsigned)b->depth);
    }
    if (p->len != n * spp(p->kind)) {
        return psd_misuse(err, "pixel buffer has %zu samples, expected %zu", p->len, n * spp(p->kind));
    }
    return PC_OK;
}

static pc_status color_channels(const pc_psd_builder *b, size_t *cc, pc_error *err)
{
    char buf[32];
    switch (b->color_mode) {
    case PC_PSD_MODE_RGB:
        *cc = 3;
        return PC_OK;
    case PC_PSD_MODE_GRAYSCALE:
        *cc = 1;
        return PC_OK;
    case PC_PSD_MODE_CMYK:
        *cc = 4;
        return PC_OK;
    default:
        return psd_unsupported(err, "builder color mode %s", mode_debug(b->color_mode, buf));
    }
}

static pc_status channel(const pc_psd_builder *b, int16_t id, const pc_psd_bytes *pl, size_t w, size_t h,
                         pc_psd_layer *l, pc_error *err)
{
    pc_psd_channel c;
    pc_status st = pc_psd_channel_encode(id, b->compression, pl->data, pl->len, w, h, b->depth, b->version, &c, err);
    return st == PC_OK ? pc_psd_layer_push_channel(l, &c, err) : st;
}

static pc_status empty_channels(const pc_psd_builder *b, size_t cc, pc_psd_layer *l, pc_error *err)
{
    pc_psd_bytes none = {NULL, 0};
    size_t c;
    pc_status st = channel(b, -1, &none, 0, 0, l, err);
    for (c = 0; c < cc && st == PC_OK; c++) {
        st = channel(b, (int16_t)c, &none, 0, 0, l, err);
    }
    return st;
}

static pc_status add_block(pc_psd_layer *l, pc_status st, pc_psd_block *blk, pc_error *err)
{
    return st == PC_OK ? pc_psd_layer_push_block(l, blk, err) : st;
}

static pc_status common_blocks(pc_psd_layer *l, const char *name, uint32_t id, pc_error *err)
{
    pc_psd_block blk;
    pc_status st = pc_psd_block_make_unicode_name(name, &blk, err);
    st = add_block(l, st, &blk, err);
    if (st == PC_OK) {
        st = pc_psd_block_make_u32("lyid", id, &blk, err);
        st = add_block(l, st, &blk, err);
    }
    return st;
}

static pc_status layer_record(const pc_psd_builder *b, const owned_layer *s, size_t cc, uint32_t id, pc_psd_layer *out,
                              pc_error *err)
{
    size_t w = s->width;
    size_t h = s->height;
    size_t n = w * h;
    pc_psd_layer l;
    pc_psd_bytes pl = {NULL, 0};
    pc_psd_block blk;
    size_t c;
    size_t i;
    pc_status st;
    pc_psd_layer_init(&l);
    CK(check_pixels(b, &s->px, n, err));
    CK(plane(&s->px, cc, n, &pl, err));
    CK(channel(b, -1, &pl, w, h, &l, err));
    pc_psd_bytes_fini(&pl);
    for (c = 0; c < cc; c++) {
        CK(plane(&s->px, c, n, &pl, err));
        CK(channel(b, (int16_t)c, &pl, w, h, &l, err));
        pc_psd_bytes_fini(&pl);
    }
    if (s->has_mask) {
        size_t mw;
        size_t mh;
        CK(pc_psd_rect_size(s->mrect, &mw, &mh, err));
        if (s->mdata.len != mw * mh) {
            st = psd_misuse(err, "mask buffer size mismatch");
            goto fail;
        }
        if (b->depth == 16 && s->mdata.len > 0) {
            pl.len = s->mdata.len * 2;
            pl.data = malloc(pl.len);
            if (pl.data == NULL) {
                st = psd_nomem(err);
                goto fail;
            }
            for (i = 0; i < s->mdata.len; i++) {
                pc_store_be16(pl.data + 2 * i, (uint16_t)(s->mdata.data[i] * 257u));
            }
        } else {
            CK(pc_psd_bytes_clone(&s->mdata, &pl, err));
        }
        CK(channel(b, -2, &pl, mw, mh, &l, err));
        pc_psd_bytes_fini(&pl);
        CK(pc_psd_layer_mask_make(s->mrect, s->mdefault, s->mdisabled ? PC_PSD_MF_DISABLED : 0, &l.mask.mask, err));
        l.mask.kind = PC_PSD_MASK_PARSED;
    }
    l.flags = s->visible ? 0 : PC_PSD_LF_HIDDEN;
    CK(common_blocks(&l, s->name, id, err));
    if (s->has_fill) {
        CK(pc_psd_block_make_flag("iOpa", s->fill, &blk, err));
        CK(pc_psd_layer_push_block(&l, &blk, err));
    }
    for (i = 0; i < s->nextra; i++) {
        CK(pc_psd_block_clone(&s->extra[i], &blk, err));
        CK(pc_psd_layer_push_block(&l, &blk, err));
    }
    l.rect = pc_psd_rect_from_xywh(s->left, s->top, s->width, s->height);
    memcpy(l.blend, s->blend, 4);
    l.opacity = s->opacity;
    l.clipping = s->clipping ? 1 : 0;
    CK(pc_psd_blending_ranges_full(cc, &l.blending_ranges, err));
    CK(psd_encode_legacy_name(s->name, &l.name, err));
    *out = l;
    return PC_OK;
fail:
    pc_psd_bytes_fini(&pl);
    pc_psd_layer_fini(&l);
    return st;
}

static pc_status group_record(const pc_psd_builder *b, const owned_group *g, size_t cc, uint32_t id, bool start,
                              pc_psd_layer *out, pc_error *err)
{
    pc_psd_layer l;
    pc_psd_block blk;
    pc_status st;
    pc_psd_layer_init(&l);
    CK(empty_channels(b, cc, &l, err));
    CK(pc_psd_blending_ranges_full(cc, &l.blending_ranges, err));
    if (start) {
        CK(pc_psd_bytes_set(&l.name, "</Layer group>", 14, err));
        CK(common_blocks(&l, "</Layer group>", id, err));
        CK(pc_psd_block_make_section(PC_PSD_SECTION_DIVIDER, NULL, NULL, &blk, err));
    } else {
        memcpy(l.blend, g->blend, 4);
        l.opacity = g->opacity;
        l.flags = g->visible ? 0 : PC_PSD_LF_HIDDEN;
        CK(psd_encode_legacy_name(g->name, &l.name, err));
        CK(common_blocks(&l, g->name, id, err));
        CK(pc_psd_block_make_section(g->open ? PC_PSD_SECTION_OPEN : PC_PSD_SECTION_CLOSED, g->blend, NULL, &blk, err));
    }
    CK(pc_psd_layer_push_block(&l, &blk, err));
    *out = l;
    return PC_OK;
fail:
    pc_psd_layer_fini(&l);
    return st;
}

pc_status pc_psd_builder_build(const pc_psd_builder *b, pc_psd_file *out, pc_error *err)
{
    pc_psd_file f;
    size_t cc;
    size_t i;
    size_t n;
    bool real = b->has_composite;
    bool with_alpha = false;
    pc_buf planes = PC_BUF_INIT;
    pc_psd_resource vr;
    pc_status st;
    pc_psd_file_init(&f);
    if (b->nopen > 0) {
        return psd_misuse(err, "unclosed group");
    }
    if (b->depth != 8 && b->depth != 16) {
        return psd_unsupported(err, "builder depth %u", (unsigned)b->depth);
    }
    CK(color_channels(b, &cc, err));
    for (i = 0; i < b->n; i++) {
        pc_psd_layer l;
        uint32_t id = (uint32_t)(i + 1);
        const entry *e = &b->entries[i];
        if (e->kind == ENTRY_LAYER) {
            CK(layer_record(b, &e->layer, cc, id, &l, err));
        } else {
            CK(group_record(b, &e->group, cc, id, e->kind == ENTRY_GROUP_START, &l, err));
        }
        CK(pc_psd_layer_info_push(&f.layer_info, &l, err));
    }
    n = (size_t)b->width * (size_t)b->height;
    if (b->has_composite) {
        CK(check_pixels(b, &b->composite, n, err));
        with_alpha = !alpha_is_opaque(&b->composite, n);
    }
    f.header = pc_psd_header_make(b->version, b->width, b->height, (uint16_t)(cc + (with_alpha ? 1 : 0)), b->depth,
                                  b->color_mode);
    CK(pc_psd_header_validate(&f.header, err));
    if (b->has_composite) {
        for (i = 0; i < cc + (with_alpha ? 1 : 0); i++) {
            pc_psd_bytes pl = {NULL, 0};
            CK(with_alpha ? composite_plane(&b->composite, i, n, &pl, err)
                          : plane(&b->composite, i, n, &pl, err));
            st = pc_buf_append(&planes, pl.data, pl.len, err);
            pc_psd_bytes_fini(&pl);
            if (st != PC_OK) {
                goto fail;
            }
        }
    } else {
        /* White: 0xff in RGB/gray; no ink (stored inverted as 0xff) in CMYK. */
        size_t bytes = n * (b->depth / 8u) * cc;
        CK(pc_buf_reserve(&planes, bytes, err));
        memset(planes.data, 0xff, bytes);
        planes.len = bytes;
    }
    f.image_compression = b->compression;
    CK(pc_psd_image_encode(b->compression, planes.data, planes.len, &f.header, &f.image_data, err));
    pc_buf_fini(&planes);
    for (i = 0; i < b->nres; i++) {
        pc_psd_resource r;
        pc_psd_resource *g;
        CK(pc_psd_resource_clone(&b->resources[i], &r, err));
        g = realloc(f.resources, (f.nresources + 1) * sizeof *g);
        if (g == NULL) {
            pc_psd_resource_fini(&r);
            st = psd_nomem(err);
            goto fail;
        }
        f.resources = g;
        f.resources[f.nresources++] = r;
    }
    CK(pc_psd_version_info_resource(real, &vr, err));
    {
        pc_psd_resource *g = realloc(f.resources, (f.nresources + 1) * sizeof *g);
        if (g == NULL) {
            pc_psd_resource_fini(&vr);
            st = psd_nomem(err);
            goto fail;
        }
        f.resources = g;
        f.resources[f.nresources++] = vr;
    }
    if (f.layer_info.n > 0) {
        f.has_layer_info = true;
        f.layer_info.merged_alpha = with_alpha;
        f.has_global_mask = true;
        if (b->depth == 16) {
            f.placement.where = PC_PSD_LI_GLOBAL_BLOCK;
            f.placement.index = 0;
            memcpy(f.placement.signature, "8BIM", 4);
            memcpy(f.placement.key, "Lr16", 4);
        }
    }
    *out = f;
    return PC_OK;
fail:
    pc_buf_fini(&planes);
    pc_psd_file_fini(&f);
    return st;
}

pc_status pc_psd_builder_to_bytes(const pc_psd_builder *b, pc_psd_bytes *out, pc_error *err)
{
    pc_psd_file f;
    pc_status st = pc_psd_builder_build(b, &f, err);
    if (st == PC_OK) {
        st = pc_psd_file_to_bytes(&f, out, err);
        pc_psd_file_fini(&f);
    }
    return st;
}
