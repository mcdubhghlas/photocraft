/* PSD / PSB -> document (port of crates/io/src/psd_import.rs, document level: resources, global
 * blocks, the merged image, flattened modes and extra channels; layer records are in
 * psd_layers.c). Never fails on file data: problems become warnings. Failures are out of memory,
 * cancellation, and functions of later io chunks that are not ported yet ("... (I3)/(I4)"). */
#include "io/psd_import.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"
#include "io/annotations_map.h"
#include "io/channel_map.h"
#include "io/comps_map.h"
#include "io/pattern_map.h"
#include "io/pixels.h"
#include "io/psd_import_internal.h"
#include "io/slices_map.h"
#include "io/text_styles_map.h"
#include "io/vector_map.h"
#include "psd/pixels.h"
#include "psd/resources.h"
#include "raster/pixel.h"
#include "text/psd.h"

#define TRY(x)                                                                                               \
    do {                                                                                                     \
        st = (x);                                                                                            \
        if (st != PC_OK) {                                                                                   \
            goto fail;                                                                                       \
        }                                                                                                    \
    } while (0)

/* Resources that are mapped to document fields or regenerated on export. */
const uint16_t PC_IO_MAPPED_RESOURCES[19] = {PC_PSD_RES_GLOBAL_ANGLE,
                                             PC_PSD_RES_GLOBAL_ALTITUDE,
                                             PC_PSD_RES_RESOLUTION_INFO,
                                             PC_PSD_RES_ICC_PROFILE,
                                             PC_PSD_RES_XMP,
                                             PC_PSD_RES_EXIF,
                                             PC_PSD_RES_VERSION_INFO,
                                             PC_PSD_RES_THUMBNAIL,
                                             PC_PSD_RES_THUMBNAIL_PS4,
                                             PC_PSD_RES_LAYER_STATE,
                                             PC_PSD_RES_LAYER_GROUP_INFO,
                                             1032, /* grid and guides */
                                             1045, /* unicode alpha names */
                                             1006, /* pascal alpha names */
                                             1069, /* layer selection ids */
                                             1072, /* layer group(s) enabled id */
                                             PC_IO_DISPLAY_INFO,
                                             PC_IO_DISPLAY_INFO_OLD,
                                             PC_IO_QUICK_MASK_INFO};

static bool mapped(uint16_t id)
{
    for (size_t i = 0; i < 19; i++) {
        if (PC_IO_MAPPED_RESOURCES[i] == id) {
            return true;
        }
    }
    return false;
}

static bool doc_mode(uint16_t m, pc_color_mode *out)
{
    switch (m) {
    case PC_PSD_MODE_BITMAP:
        *out = PC_COLOR_MODE_BITMAP;
        return true;
    case PC_PSD_MODE_GRAYSCALE:
        *out = PC_COLOR_MODE_GRAYSCALE;
        return true;
    case PC_PSD_MODE_INDEXED:
        *out = PC_COLOR_MODE_INDEXED;
        return true;
    case PC_PSD_MODE_RGB:
        *out = PC_COLOR_MODE_RGB;
        return true;
    case PC_PSD_MODE_CMYK:
        *out = PC_COLOR_MODE_CMYK;
        return true;
    case PC_PSD_MODE_MULTICHANNEL:
        *out = PC_COLOR_MODE_MULTICHANNEL;
        return true;
    case PC_PSD_MODE_DUOTONE:
        *out = PC_COLOR_MODE_DUOTONE;
        return true;
    case PC_PSD_MODE_LAB:
        *out = PC_COLOR_MODE_LAB;
        return true;
    default:
        return false;
    }
}

/* Rust's {:?} of the psd ColorMode. */
static void psd_mode_debug(uint16_t m, char *out, size_t cap)
{
    static const char *const names[10] = {"Bitmap", "Grayscale", "Indexed",      "Rgb",     "Cmyk",
                                          NULL,     NULL,        "Multichannel", "Duotone", "Lab"};
    if (m < 10 && names[m] != NULL) {
        (void)snprintf(out, cap, "%s", names[m]);
    } else {
        (void)snprintf(out, cap, "Unknown(%u)", (unsigned)m);
    }
}

static pc_status lossy_str(const uint8_t *b, size_t len, pc_str *out, pc_error *err)
{
    pc_buf t = PC_BUF_INIT;
    pc_status st = pc_utf8_lossy(b != NULL ? b : (const uint8_t *)"", len, &t, err);
    if (st == PC_OK) {
        st = pc_str_set(out, (const char *)t.data, t.len, err);
    }
    pc_buf_fini(&t);
    return st;
}

/* Resource 1045 (unicode alpha names): u32 count + UTF-16 units per name. */
static pc_status unicode_names(const uint8_t *data, size_t len, pc_str **out, size_t *n, pc_error *err)
{
    pc_str *v = NULL;
    size_t k = 0;
    size_t cap = 0;
    size_t at = 0;
    while (at + 4 <= len) {
        size_t units =
            (size_t)data[at] << 24 | (size_t)data[at + 1] << 16 | (size_t)data[at + 2] << 8 | data[at + 3];
        pc_buf s = PC_BUF_INIT;
        pc_str *nv;
        pc_status st;
        at += 4;
        if (units > (len - at) / 2) {
            break;
        }
        /* from_utf16_lossy, then trailing U+0000 trimmed. */
        st = pc_utf16be_lossy(data + at, units, &s, err);
        if (st == PC_OK) {
            size_t l = s.len;
            while (l > 0 && s.data[l - 1] == 0) {
                l--;
            }
            nv = pc_grow_array(v, &cap, k + 1, sizeof *v);
            if (nv == NULL) {
                st = pc_io_nomem(err, "reading channel names");
            } else {
                v = nv;
                v[k] = (pc_str)PC_STR_INIT;
                st = pc_str_set(&v[k], (const char *)s.data, l, err);
                if (st == PC_OK) {
                    k++;
                }
            }
        }
        pc_buf_fini(&s);
        if (st != PC_OK) {
            for (size_t i = 0; i < k; i++) {
                pc_str_fini(&v[i]);
            }
            free(v);
            return st;
        }
        at += units * 2;
    }
    *out = v;
    *n = k;
    return PC_OK;
}

static void free_names(pc_str *v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        pc_str_fini(&v[i]);
    }
    free(v);
}

static pc_status push_float(float **v, size_t *n, float x, pc_error *err)
{
    size_t cap = *n;
    float *nv = pc_grow_array(*v, &cap, *n + 1, sizeof *nv);
    if (nv == NULL) {
        return pc_io_nomem(err, "reading guides");
    }
    nv[(*n)++] = x;
    *v = nv;
    return PC_OK;
}

static uint32_t rd32(const uint8_t *d)
{
    return (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3];
}

static pc_status parse_guides(const uint8_t *data, size_t len, pc_document *doc, pc_error *err)
{
    uint32_t count;
    if (len < 16) {
        return PC_OK;
    }
    count = rd32(data + 12);
    for (size_t i = 0; i < count; i++) {
        size_t at = 16 + i * 5;
        float pos;
        pc_status st;
        if (at > len || len - at < 5) {
            break;
        }
        pos = (float)pc_u32_as_i32(rd32(data + at)) / 32.0f;
        st = data[at + 4] == 1 ? push_float(&doc->guides.horizontal, &doc->guides.nhorizontal, pos, err)
                               : push_float(&doc->guides.vertical, &doc->guides.nvertical, pos, err);
        if (st != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

static pc_status push_resource(pc_document *doc, const pc_psd_resource *r, pc_error *err)
{
    pc_metadata *m = &doc->metadata;
    size_t cap = m->npsd_resources;
    pc_doc_psd_resource e;
    pc_doc_psd_resource *nv;
    pc_status st;
    memset(&e, 0, sizeof e);
    e.id = r->id;
    st = lossy_str(r->name.data, r->name.len, &e.name, err);
    st = st != PC_OK ? st : pc_io_set_blob(&e.data, r->data.data, r->data.len, err);
    if (st != PC_OK) {
        pc_str_fini(&e.name);
        pc_blob_release(e.data);
        return st;
    }
    nv = pc_grow_array(m->psd_resources, &cap, m->npsd_resources + 1, sizeof *nv);
    if (nv == NULL) {
        pc_str_fini(&e.name);
        pc_blob_release(e.data);
        return pc_io_nomem(err, "keeping a resource");
    }
    m->psd_resources = nv;
    nv[m->npsd_resources++] = e;
    return PC_OK;
}

static pc_status saved_path(pc_document *doc, const pc_psd_resource *r, pc_error *err)
{
    {
        bool found = false;
        pc_path path;
        pc_status st = pc_io_path_from_resource(r->data.data, r->data.len, doc->size.width, doc->size.height,
                                                &found, &path, err);
        if (st != PC_OK) {
            return st;
        }
        if (!found) {
            return push_resource(doc, r, err);
        }
        {
            pc_named_path np;
            size_t cap = doc->npaths;
            pc_named_path *nv;
            memset(&np, 0, sizeof np);
            np.path = path;
            st = lossy_str(r->name.data, r->name.len, &np.name, err);
            st = st != PC_OK ? st : pc_io_set_blob(&np.psd_raw, r->data.data, r->data.len, err);
            nv = st == PC_OK ? pc_grow_array(doc->paths, &cap, doc->npaths + 1, sizeof *nv) : NULL;
            if (nv == NULL) {
                pc_named_path_fini(&np);
                return st != PC_OK ? st : pc_io_nomem(err, "adding a path");
            }
            doc->paths = nv;
            nv[doc->npaths++] = np;
        }
    }
    return PC_OK;
}

static pc_status read_resources(const pc_psd_file *file, pc_document *doc, pc_io_warnings *warnings,
                                pc_error *err)
{
    const pc_psd_header *h = &file->header;
    pc_status st = PC_OK;
    for (size_t i = 0; i < file->nresources && st == PC_OK; i++) {
        const pc_psd_resource *r = &file->resources[i];
        const uint8_t *d = r->data.data;
        size_t len = r->data.len;
        if (r->id == PC_PSD_RES_RESOLUTION_INFO) {
            pc_psd_resolution ri;
            if (pc_psd_resolution_parse(d, len, &ri, NULL) == PC_OK) {
                double x = pc_psd_resolution_h(&ri);
                double y = pc_psd_resolution_v(&ri);
                if (ri.h_res_unit == 2) {
                    x *= 2.54;
                }
                if (ri.v_res_unit == 2) {
                    y *= 2.54;
                }
                doc->resolution_dpi = pc_f64_to_f32(x);
                st = pc_io_unequal_resolution_warning(x, y, warnings, err);
            }
        } else if (r->id == PC_PSD_RES_ICC_PROFILE) {
            st = pc_io_set_blob(&doc->icc_profile, d, len, err);
        } else if (r->id == PC_PSD_RES_XMP) {
            st = lossy_str(d, len, &doc->metadata.xmp, err);
            doc->metadata.has_xmp = st == PC_OK;
        } else if (r->id == PC_PSD_RES_EXIF) {
            st = pc_io_set_blob(&doc->metadata.exif, d, len, err);
        } else if (r->id == 1032) {
            st = parse_guides(d, len, doc, err);
        } else if (r->id == PC_PSD_RES_GLOBAL_ANGLE || r->id == PC_PSD_RES_GLOBAL_ALTITUDE) {
            if (len >= 4) {
                float v = (float)pc_u32_as_i32(rd32(d));
                if (r->id == PC_PSD_RES_GLOBAL_ANGLE) {
                    doc->global_light.angle = v;
                } else {
                    doc->global_light.altitude = v;
                }
            }
        } else if (r->id >= PC_IO_SAVED_PATHS_FIRST && r->id <= PC_IO_SAVED_PATHS_LAST) {
            st = saved_path(doc, r, err);
        } else if (r->id == PC_IO_WORK_PATH) {
            bool found = false;
            pc_path path;
            st = pc_io_path_from_resource(d, len, h->width, h->height, &found, &path, err);
            if (st == PC_OK) {
                if (doc->has_work_path) {
                    pc_path_fini(&doc->work_path);
                }
                doc->has_work_path = found;
                if (found) {
                    doc->work_path = path;
                }
            }
        } else if (r->id == PC_IO_CLIPPING_PATH) {
            /* Kept raw (layout beyond the name is not modelled); the name is exposed. */
            size_t n = len > 0 ? d[0] : 0;
            pc_clipping_path cp;
            memset(&cp, 0, sizeof cp);
            if (len >= 1 + n) {
                st = lossy_str(d + 1, n, &cp.name, err);
            }
            if (st == PC_OK) {
                if (doc->has_clipping_path) {
                    pc_clipping_path_fini(&doc->clipping_path);
                }
                doc->clipping_path = cp;
                doc->has_clipping_path = true;
                st = push_resource(doc, r, err);
            }
        } else if (!mapped(r->id)) {
            st = push_resource(doc, r, err);
        }
    }
    return st;
}

static bool has_global(const pc_document *doc, const char *key)
{
    for (size_t i = 0; i < doc->metadata.npsd_global_blocks; i++) {
        if (memcmp(doc->metadata.psd_global_blocks[i].key, key, 4) == 0) {
            return true;
        }
    }
    return false;
}

static const pc_doc_psd_resource *doc_resource(const pc_document *doc, uint16_t id)
{
    for (size_t i = 0; i < doc->metadata.npsd_resources; i++) {
        if (doc->metadata.psd_resources[i].id == id) {
            return &doc->metadata.psd_resources[i];
        }
    }
    return NULL;
}

/* Whether any type layer keeps its TySh block (text_styles_map::import has nothing to read
 * otherwise). */
static bool has_text_raw(const pc_layer *v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (v[i].content.kind == PC_LAYER_TEXT && v[i].content.u.text.psd_raw != NULL) {
            return true;
        }
        if (v[i].content.kind == PC_LAYER_GROUP &&
            has_text_raw(v[i].content.u.group.children, v[i].content.u.group.nchildren)) {
            return true;
        }
    }
    return false;
}

/* Global blocks and what the later io chunks read from them (patterns, notes, the measurement
 * scale). The I4 readers are called only when their data is present: without it they return
 * nothing, so the skip changes no result. */
static pc_status read_globals(const pc_psd_file *file, pc_document *doc, pc_io_warnings *warnings,
                              pc_error *err)
{
    pc_metadata *m = &doc->metadata;
    pc_measurement_scale scale;
    pc_status st;
    if (file->nglobal > 0) {
        m->psd_global_blocks = pc_calloc_array(file->nglobal, sizeof *m->psd_global_blocks);
        if (m->psd_global_blocks == NULL) {
            return pc_io_nomem(err, "keeping global blocks");
        }
    }
    for (size_t i = 0; i < file->nglobal; i++) {
        const pc_psd_block *b = &file->global_blocks[i];
        pc_doc_psd_global_block *g = &m->psd_global_blocks[i];
        memcpy(g->signature, b->signature, 4);
        memcpy(g->key, b->key, 4);
        st = pc_io_set_blob(&g->data, b->data.data, b->data.len, err);
        if (st != PC_OK) {
            return st;
        }
        m->npsd_global_blocks++;
    }
    if (has_global(doc, "Patt") || has_global(doc, "Pat2") || has_global(doc, "Pat3")) {
        st = pc_io_patterns_from_global_blocks(doc, &doc->patterns, &doc->npatterns, err);
        if (st != PC_OK) {
            return st;
        }
    }
    /* Notes (Anno) and the measurement scale (resource 1074); raw data stays for export. */
    if (has_global(doc, "Anno")) {
        st = pc_io_notes_from_blocks(doc, &doc->notes, &doc->nnotes, err);
        if (st != PC_OK) {
            return st;
        }
    }
    if (pc_io_anno_unreadable(doc)) {
        st = pc_io_warn(
            warnings, err, "%s",
            "the notes (Anno block) could not be read; they are kept unchanged on PSD save unless a "
            "note is added");
        if (st != PC_OK) {
            return st;
        }
    }
    if (pc_io_raw_scale(doc, &scale)) {
        pc_measurement_scale_fini(&doc->measurement.scale);
        doc->measurement.scale = scale;
    }
    return PC_OK;
}

static void imp_fini(pc_io_imp *cx)
{
    if (cx->has_txt2) {
        pc_ed_fini(&cx->txt2);
    }
    for (size_t i = 0; i < cx->nfx; i++) {
        pc_psd_fx_item_fini(&cx->fx[i]);
    }
    free(cx->fx);
    cx->fx = NULL;
    cx->nfx = 0;
}

/* Smart-filter caches from the global FEid / FXid blocks. */
static pc_status read_filter_effects(pc_io_imp *cx, const pc_psd_file *file, pc_error *err)
{
    for (size_t i = 0; i < file->nglobal; i++) {
        const pc_psd_block *b = &file->global_blocks[i];
        pc_psd_filter_effects fx;
        pc_error e;
        pc_status st;
        if (memcmp(b->key, "FEid", 4) != 0 && memcmp(b->key, "FXid", 4) != 0) {
            continue;
        }
        /* Smart-filter caches can be large: a cancelled open stops between blocks. */
        if (pc_interrupt_cancelled(cx->ctl)) {
            return pc_io_cancelled(err);
        }
        st = pc_psd_filter_effects_parse(b->data.data, b->data.len, &fx, &e);
        if (st == PC_ERR_NOMEM) {
            return pc_io_nomem(err, "reading smart filter masks");
        }
        if (st != PC_OK) {
            pc_str k = PC_STR_INIT;
            st = lossy_str(b->key, 4, &k, err);
            st = st != PC_OK ? st
                             : pc_io_warn(cx->warnings, err, "smart filter masks (%s) could not be read: %s",
                                          pc_str_c(&k), e.msg);
            pc_str_fini(&k);
            if (st != PC_OK) {
                return st;
            }
            continue;
        }
        if (fx.n > 0) {
            size_t cap = cx->nfx;
            pc_psd_fx_item *nv = pc_grow_array(cx->fx, &cap, cx->nfx + fx.n, sizeof *nv);
            if (nv == NULL) {
                pc_psd_filter_effects_fini(&fx);
                return pc_io_nomem(err, "reading smart filter masks");
            }
            memcpy(nv + cx->nfx, fx.items, fx.n * sizeof *nv);
            cx->fx = nv;
            cx->nfx += fx.n;
            free(fx.items);
            fx.items = NULL;
            fx.n = 0;
        }
        pc_psd_filter_effects_fini(&fx);
    }
    return PC_OK;
}

static pc_status push_layer(pc_document *doc, pc_layer *l, pc_error *err)
{
    pc_status st = pc_io_push_layer(doc, l, err);
    if (st != PC_OK) {
        pc_layer_fini(l);
    }
    return st;
}

/* Flattened file: the merged image becomes the background layer. */
static pc_status merged_background(pc_io_imp *cx, pc_document *doc, const pc_psd_bytes *all, pc_error *err)
{
    const pc_psd_file *file = cx->file;
    const pc_psd_header *h = &file->header;
    size_t cc = cx->cc;
    size_t hh = h->height;
    size_t n = (size_t)h->width * hh;
    size_t plane = pc_psd_header_row_bytes(h) * hh;
    bool alpha = pc_psd_file_merged_has_alpha(file);
    pc_rect canvas = pc_rect_make(0, 0, pc_u32_as_i32(h->width), pc_u32_as_i32(h->height));
    pc_io_plane in[8];
    uint8_t fill[8][4];
    bool inv[8];
    uint8_t *bytes = NULL;
    size_t len = 0;
    float *vals = NULL;
    pc_surface s;
    pc_layer bg;
    pc_layer_content c;
    pc_status st;
    memset(&c, 0, sizeof c);
    if (cc + 1 > 8) {
        return PC_OK;
    }
    for (size_t k = 0; k <= cc; k++) {
        bool is_alpha = k == cc;
        size_t idx = k;
        bool ok =
            (!is_alpha || alpha) && plane > 0 && idx < SIZE_MAX / plane && (idx + 1) * plane <= all->len;
        in[k] = (pc_io_plane){ok, ok ? all->data + idx * plane : NULL, ok ? plane : 0};
        if (is_alpha) {
            (void)pc_io_max_sample(doc->depth, fill[k]);
        } else {
            (void)pc_io_zero_sample(doc->depth, fill[k]);
        }
        inv[k] = !is_alpha && cx->cmyk;
    }
    st = pc_io_interleave(in, cc + 1, (const uint8_t (*)[4])fill, n, doc->depth, inv, &bytes, &len, err);
    if (st != PC_OK) {
        return st;
    }
    if (cx->fmt.mode == PC_COLOR_MODE_LAB && doc->depth == PC_SAMPLE_TYPE_U16) {
        pc_io_lab16_chroma(bytes, len, cc + 1, true);
    }
    st = pc_surface_from_interleaved(cx->fmt, canvas, bytes, len, &s, err);
    free(bytes);
    if (st != PC_OK) {
        return st;
    }
    if (alpha && n > 0) {
        /* Undo Photoshop's white matting, a band of tile rows at a time (no full-size float copy). */
        float white[PC_RASTER_MAX_CHANNELS];
        const float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        size_t band_px;
        size_t vlen;
        (void)pc_from_rgba(cx->fmt, one, white);
        if (!pc_size_mul((size_t)h->width, PC_TILE_SIZE, &band_px) || !pc_size_mul(band_px, cc + 1, &vlen) ||
            vlen > PC_MAX_ALLOC / sizeof(float)) {
            pc_surface_fini(&s);
            return pc_fail(err, PC_ERR_LIMIT, "limit exceeded: merged image of width %u", (unsigned)h->width);
        }
        vals = pc_malloc_array(vlen, sizeof *vals);
        if (vals == NULL) {
            pc_surface_fini(&s);
            return pc_io_nomem(err, "un-matting the merged image");
        }
        for (int32_t y = canvas.y0; y < canvas.y1;) {
            int32_t y1 = y > canvas.y1 - PC_TILE_SIZE ? canvas.y1 : y + PC_TILE_SIZE;
            pc_rect band = pc_rect_make(canvas.x0, y, canvas.x1, y1);
            size_t cnt = (size_t)h->width * (size_t)(y1 - y) * (cc + 1);
            st = pc_surface_read_region_into(&s, band, vals, cnt, err);
            if (st != PC_OK) {
                break;
            }
            for (size_t p = 0; p < cnt; p += cc + 1) {
                float a = vals[p + cc];
                for (size_t k = 0; k < cc; k++) {
                    vals[p + k] = a <= 0.0f ? 0.0f : pc_io_unmatte(vals[p + k], a, white[k]);
                }
            }
            st = pc_surface_write_region(&s, band, vals, cnt, err);
            if (st != PC_OK) {
                break;
            }
            y = y1;
        }
        free(vals);
        if (st != PC_OK) {
            pc_surface_fini(&s);
            return st;
        }
    }
    pc_surface_prune(&s);
    c.kind = PC_LAYER_RASTER;
    c.u.raster = s;
    st = pc_layer_init(&bg, "Background", &c, err);
    if (st != PC_OK) {
        pc_surface_fini(&s);
        return st;
    }
    if (!alpha) {
        bg.locks.transparency = true;
        bg.locks.position = true;
    }
    return push_layer(doc, &bg, err);
}

/* Flattened modes (Bitmap, Indexed, Duotone, 1-bit, unknown): the composite as 8-bit RGBA,
 * or for Multichannel the first channels as RGB. *ok false when there is none. */
static pc_status flat_rgba(const pc_psd_file *file, const pc_psd_bytes *merged, bool merged_ok,
                           pc_psd_image8 *img, bool *ok, pc_error *err)
{
    const pc_psd_header *h = &file->header;
    pc_error e;
    pc_status st = pc_psd_composite_rgba8(file, img, &e);
    *ok = false;
    if (st == PC_OK) {
        *ok = true;
        return PC_OK;
    }
    if (st == PC_ERR_NOMEM) {
        return pc_io_nomem(err, "decoding the composite");
    }
    if (merged_ok) {
        size_t hh = h->height;
        size_t w = h->width;
        size_t plane = pc_psd_header_row_bytes(h) * hh;
        size_t n = w * hh;
        size_t nc = h->channels < 3 ? h->channels : 3;
        uint8_t *data;
        if (n > PC_MAX_ALLOC / 4) {
            return PC_OK;
        }
        data = pc_malloc(n * 4 > 0 ? n * 4 : 1);
        if (data == NULL) {
            return pc_io_nomem(err, "decoding the composite");
        }
        memset(data, 255, n * 4);
        for (size_t c = 0; c < nc; c++) {
            pc_psd_bytes p = {NULL, 0};
            /* The reference slices the merged planes unchecked; a short image is no composite here. */
            if (plane == 0 || c >= merged->len / plane || (c + 1) * plane > merged->len) {
                free(data);
                return PC_OK;
            }
            st = pc_psd_plane_to_u8(merged->data + c * plane, plane, h->depth, w, hh, &p, &e);
            if (st != PC_OK || p.len < n) {
                pc_psd_bytes_fini(&p);
                free(data);
                return st == PC_ERR_NOMEM ? pc_io_nomem(err, "decoding the composite") : PC_OK;
            }
            for (size_t i = 0; i < n; i++) {
                data[i * 4 + c] = p.data[i];
            }
            pc_psd_bytes_fini(&p);
        }
        memset(img, 0, sizeof *img);
        img->width = h->width;
        img->height = h->height;
        img->data.data = data;
        img->data.len = n * 4;
        *ok = true;
    }
    return PC_OK;
}

static pc_status flattened(pc_io_imp *cx, pc_document *doc, const pc_psd_bytes *merged, bool merged_ok,
                           pc_error *err)
{
    const pc_psd_file *file = cx->file;
    const pc_psd_header *h = &file->header;
    size_t nlayers = cx->nlayers;
    char mode[32];
    pc_psd_image8 img;
    bool ok = false;
    pc_status st;
    psd_mode_debug(h->color_mode, mode, sizeof mode);
    if (nlayers > 0) {
        st = pc_io_warn(cx->warnings, err,
                        "%s documents are imported flattened: %zu layer records were not imported", mode,
                        nlayers);
    } else {
        st = pc_io_warn(cx->warnings, err, "%s %u-bit document converted to %s 8-bit for editing", mode,
                        (unsigned)h->depth, pc_color_mode_name(cx->fmt.mode));
    }
    if (st != PC_OK) {
        return st;
    }
    if (h->color_mode == PC_PSD_MODE_INDEXED && file->color_mode_data.len >= 768) {
        /* Planar palette: 256 reds, 256 greens, 256 blues (the Color Table). */
        const uint8_t *m = file->color_mode_data.data;
        doc->color_table.colors = pc_malloc_array(256, sizeof *doc->color_table.colors);
        if (doc->color_table.colors == NULL) {
            return pc_io_nomem(err, "reading the color table");
        }
        for (size_t i = 0; i < 256; i++) {
            doc->color_table.colors[i][0] = m[i];
            doc->color_table.colors[i][1] = m[256 + i];
            doc->color_table.colors[i][2] = m[512 + i];
        }
        doc->color_table.ncolors = 256;
        doc->has_color_table = true;
    } else if (h->color_mode == PC_PSD_MODE_DUOTONE && file->color_mode_data.len > 0) {
        /* The duotone ink block is undocumented: keep it raw; the image shows as its gray plate. */
        const float black[3] = {0.0f, 0.0f, 0.0f};
        pc_duotone d;
        memset(&d, 0, sizeof d);
        d.inks = pc_malloc_array(1, sizeof *d.inks);
        if (d.inks == NULL) {
            return pc_io_nomem(err, "reading duotone inks");
        }
        st = pc_duotone_ink_new("Black", black, &d.inks[0], err);
        if (st != PC_OK) {
            free(d.inks);
            return st;
        }
        d.ninks = 1;
        st = pc_io_set_blob(&d.psd_raw, file->color_mode_data.data, file->color_mode_data.len, err);
        if (st != PC_OK) {
            pc_duotone_fini(&d);
            return st;
        }
        doc->duotone = d;
        doc->has_duotone = true;
        st = pc_io_warn(cx->warnings, err, "%s", "duotone inks are not interpreted (shown as grayscale)");
    } else if (file->color_mode_data.len > 0) {
        st = pc_io_warn(cx->warnings, err, "%s", "color mode data is not preserved");
    }
    if (st != PC_OK) {
        return st;
    }
    st = flat_rgba(file, merged, merged_ok, &img, &ok, err);
    if (st != PC_OK || !ok) {
        return st;
    }
    {
        size_t n = (size_t)img.width * img.height;
        size_t ch = pc_pixel_format_channels(cx->fmt);
        float *vals = NULL;
        pc_rect canvas = pc_rect_make(0, 0, pc_u32_as_i32(h->width), pc_u32_as_i32(h->height));
        pc_layer_content c;
        pc_layer bg;
        if (img.data.len / 4 < n || n > PC_MAX_ALLOC / sizeof(float) / (ch > 0 ? ch : 1)) {
            pc_psd_image8_fini(&img);
            return PC_OK;
        }
        vals = pc_malloc_array(n * ch > 0 ? n * ch : 1, sizeof *vals);
        if (vals == NULL) {
            pc_psd_image8_fini(&img);
            return pc_io_nomem(err, "importing the composite");
        }
        for (size_t i = 0; i < n; i++) {
            float rgba[4];
            float px[PC_RASTER_MAX_CHANNELS];
            for (size_t k = 0; k < 4; k++) {
                rgba[k] = (float)img.data.data[i * 4 + k] / 255.0f;
            }
            (void)pc_from_rgba(cx->fmt, rgba, px);
            memcpy(vals + i * ch, px, ch * sizeof *vals);
        }
        pc_psd_image8_fini(&img);
        memset(&c, 0, sizeof c);
        c.kind = PC_LAYER_RASTER;
        pc_surface_init(&c.u.raster, cx->fmt);
        st = pc_surface_write_region(&c.u.raster, canvas, vals, n * ch, err);
        free(vals);
        if (st != PC_OK) {
            pc_surface_fini(&c.u.raster);
            return st;
        }
        pc_surface_prune(&c.u.raster);
        st = pc_layer_init(&bg, "Background", &c, err);
        if (st != PC_OK) {
            pc_surface_fini(&c.u.raster);
            return st;
        }
        return push_layer(doc, &bg, err);
    }
}

/* Extra (alpha / spot) channels of the merged image, their options and the Quick Mask. */
static pc_status extra_channels(pc_io_imp *cx, pc_document *doc, const pc_psd_bytes *all, pc_error *err)
{
    const pc_psd_file *file = cx->file;
    const pc_psd_header *h = &file->header;
    size_t first = cx->cc + (pc_psd_file_merged_has_alpha(file) ? 1 : 0);
    size_t hh = h->height;
    size_t n = (size_t)h->width * hh;
    size_t plane = pc_psd_header_row_bytes(h) * hh;
    pc_rect canvas = pc_rect_make(0, 0, pc_u32_as_i32(h->width), pc_u32_as_i32(h->height));
    const pc_psd_resource *r = pc_psd_file_resource(file, 1045);
    pc_str *names = NULL;
    size_t nnames = 0;
    pc_status st = PC_OK;
    if (r != NULL) {
        st = unicode_names(r->data.data, r->data.len, &names, &nnames, err);
        if (st != PC_OK) {
            return st;
        }
    }
    for (size_t idx = first, k = 0; idx < h->channels && st == PC_OK; idx++, k++) {
        pc_io_plane in;
        uint8_t fill[1][4];
        bool inv = false;
        uint8_t *bytes = NULL;
        size_t len = 0;
        pc_surface s;
        pc_alpha_channel ch;
        char fallback[32];
        pc_alpha_channel *nv;
        size_t cap;
        if (plane == 0 || idx >= all->len / plane || (idx + 1) * plane > all->len) {
            break;
        }
        in = (pc_io_plane){true, all->data + idx * plane, plane};
        (void)pc_io_zero_sample(doc->depth, fill[0]);
        st = pc_io_interleave(&in, 1, (const uint8_t (*)[4])fill, n, doc->depth, &inv, &bytes, &len, err);
        if (st != PC_OK) {
            break;
        }
        pc_surface_init(&s, cx->mask_fmt);
        st = pc_surface_write_interleaved(&s, canvas, bytes, len, err);
        free(bytes);
        if (st != PC_OK) {
            pc_surface_fini(&s);
            break;
        }
        pc_surface_prune(&s);
        (void)snprintf(fallback, sizeof fallback, "Alpha %zu", k + 1);
        st = pc_alpha_channel_init(&ch, fallback, &s, err);
        if (st != PC_OK) {
            pc_surface_fini(&s);
            break;
        }
        if (k < nnames) {
            /* Names may hold U+0000: keep the exact bytes. */
            pc_str nm = PC_STR_INIT;
            st = pc_str_clone(&names[k], &nm, err);
            if (st == PC_OK) {
                pc_str_fini(&ch.name);
                ch.name = nm;
            }
        }
        cap = doc->nchannels;
        nv = st == PC_OK ? pc_grow_array(doc->channels, &cap, doc->nchannels + 1, sizeof *nv) : NULL;
        if (nv == NULL) {
            pc_alpha_channel_fini(&ch);
            st = st != PC_OK ? st : pc_io_nomem(err, "adding a channel");
            break;
        }
        doc->channels = nv;
        nv[doc->nchannels++] = ch;
    }
    free_names(names, nnames);
    if (st != PC_OK) {
        return st;
    }
    /* Channel options (overlay colour, opacity, colour indicates) and spot inks. */
    if ((r = pc_psd_file_resource(file, PC_IO_DISPLAY_INFO)) != NULL) {
        pc_io_apply_display_info(r->data.data, r->data.len, true, doc->channels, doc->nchannels);
    } else if ((r = pc_psd_file_resource(file, PC_IO_DISPLAY_INFO_OLD)) != NULL) {
        pc_io_apply_display_info(r->data.data, r->data.len, false, doc->channels, doc->nchannels);
    }
    /* Saved in Quick Mask mode: that channel becomes the document's Quick Mask again. */
    r = pc_psd_file_resource(file, PC_IO_QUICK_MASK_INFO);
    if (r != NULL && r->data.len >= 2) {
        size_t id = (size_t)r->data.data[0] << 8 | r->data.data[1];
        if (id >= first && id - first < doc->nchannels) {
            size_t k = id - first;
            doc->quick_mask = doc->channels[k];
            doc->has_quick_mask = true;
            memmove(doc->channels + k, doc->channels + k + 1,
                    (doc->nchannels - k - 1) * sizeof *doc->channels);
            doc->nchannels--;
        }
    }
    return PC_OK;
}

/* Layer comps (resource 1065 + per-layer cmls), slices (1050) and text styles: the I4 readers,
 * called only when their data is present (without it they leave the document unchanged). */
static pc_status later_maps(pc_document *doc, pc_io_warnings *warnings, pc_error *err)
{
    const pc_doc_psd_resource *comps = doc_resource(doc, PC_IO_LAYER_COMPS);
    pc_status st;
    if (comps != NULL) {
        pc_io_comps c;
        memset(&c, 0, sizeof c);
        st = pc_io_decode_comps(pc_blob_data(comps->data), pc_blob_len(comps->data), doc, &c, warnings, err);
        if (st != PC_OK) {
            return st;
        }
        doc->layer_comps = c.comps;
        doc->nlayer_comps = c.n;
        doc->has_last_applied_comp = c.has_last_applied;
        doc->last_applied_comp = c.last_applied;
        doc->has_last_document_state = c.has_last_state;
        doc->last_document_state = c.last_state;
    }
    if (doc_resource(doc, PC_IO_SLICES) != NULL) {
        st = pc_io_slices_import(doc, err);
        if (st != PC_OK) {
            return st;
        }
    }
    if (has_text_raw(doc->layers, doc->nlayers)) {
        st = pc_io_text_styles_import(doc, warnings, err);
        if (st != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

static pc_status layered(pc_io_imp *cx, pc_document *doc, const pc_psd_bytes *merged, bool merged_ok,
                         pc_error *err)
{
    const pc_psd_file *file = cx->file;
    pc_psd_node *tree = NULL;
    size_t ntree = 0;
    const pc_psd_resource *gi;
    pc_status st = pc_psd_layer_tree(file, &tree, &ntree, err);
    if (st != PC_OK) {
        return st == PC_ERR_NOMEM ? st : PC_OK;
    }
    st = pc_io_imp_build(cx, tree, ntree, 0, &doc->layers, &doc->nlayers, err);
    /* Layer > Link Layers: resource 1026 holds one group id per layer record (0 = unlinked). */
    gi = st == PC_OK ? pc_psd_file_resource(file, PC_PSD_RES_LAYER_GROUP_INFO) : NULL;
    if (gi != NULL) {
        uint16_t *ids = NULL;
        size_t nids = 0;
        if (pc_psd_res_group_ids(gi, &ids, &nids, NULL) == PC_OK) {
            pc_io_imp_link_groups(tree, ntree, doc->layers, doc->nlayers, ids, nids, 0);
            free(ids);
        }
    }
    pc_psd_nodes_free(tree, ntree);
    if (st != PC_OK) {
        return st;
    }
    if (doc->nlayers == 0 && merged_ok) {
        st = merged_background(cx, doc, merged, err);
    }
    return st;
}

pc_status pc_io_psd_to_document_with(const pc_psd_file *file, const pc_interrupt *ctl, pc_document *out,
                                     pc_io_warnings *warnings, pc_error *err)
{
    const pc_psd_header *h = &file->header;
    pc_color_mode mode;
    bool is_layered;
    bool multichannel;
    pc_sample_type depth;
    pc_document doc;
    pc_io_imp cx;
    pc_psd_bytes merged = {NULL, 0};
    bool merged_ok = false;
    size_t nl = 0;
    pc_status st;
    memset(&doc, 0, sizeof doc);
    memset(&cx, 0, sizeof cx);
    if (!doc_mode(h->color_mode, &mode)) {
        mode = PC_COLOR_MODE_RGB;
        st = pc_io_warn(warnings, err, "unknown color mode %u; importing as RGB", (unsigned)h->color_mode);
        if (st != PC_OK) {
            return st;
        }
    }
    is_layered = (mode == PC_COLOR_MODE_GRAYSCALE || mode == PC_COLOR_MODE_RGB ||
                  mode == PC_COLOR_MODE_CMYK || mode == PC_COLOR_MODE_LAB) &&
                 h->depth != 1;
    multichannel = mode == PC_COLOR_MODE_MULTICHANNEL && h->depth != 1;
    depth = is_layered || multichannel ? pc_io_sample_for_depth(h->depth) : PC_SAMPLE_TYPE_U8;
    st = pc_document_init(&doc, "Untitled", pc_size_make(h->width, h->height), mode, depth, err);
    if (st != PC_OK) {
        return st;
    }
    TRY(read_resources(file, &doc, warnings, err));
    TRY(read_globals(file, &doc, warnings, err));

    cx.file = file;
    cx.layers = pc_psd_file_layers(file, &nl);
    cx.nlayers = nl;
    cx.fmt = pc_document_pixel_format(&doc);
    cx.mask_fmt = (pc_pixel_format){PC_COLOR_MODE_GRAYSCALE, depth, false};
    cx.cc = pc_color_mode_channels(cx.fmt.mode);
    cx.cmyk = cx.fmt.mode == PC_COLOR_MODE_CMYK;
    cx.warnings = warnings;
    cx.dpi = doc.resolution_dpi;
    cx.ctl = ctl;
    {
        const pc_psd_block *t = pc_psd_file_global_block(file, "Txt2");
        if (t != NULL) {
            st = pc_txt2_parse(t->data.data, t->data.len, &cx.txt2, NULL);
            if (st == PC_ERR_NOMEM) {
                st = pc_io_nomem(err, "reading Txt2");
                goto fail;
            }
            cx.has_txt2 = st == PC_OK;
        }
    }
    TRY(read_filter_effects(&cx, file, err));
    if (pc_interrupt_cancelled(ctl)) {
        st = pc_io_cancelled(err);
        goto fail;
    }

    /* The merged composite is decoded only when something consumes it: a flattened or
     * multichannel file (it is the image), or extra alpha / spot channels behind the colour
     * ones. A normal layered save carries a merged composite nobody reads (about half the file),
     * which isn't decoded and thrown away on every open. */
    {
        bool extra = h->channels > cx.cc + (pc_psd_file_merged_has_alpha(file) ? 1 : 0);
        if (!is_layered || nl == 0 || extra) {
            pc_error e;
            st = pc_psd_file_decode_merged(file, &merged, &e);
            if (st == PC_ERR_NOMEM) {
                st = pc_io_nomem(err, "decoding the merged image");
                goto fail;
            }
            if (pc_interrupt_cancelled(ctl)) {
                st = pc_io_cancelled(err);
                goto fail;
            }
            pc_interrupt_progress(ctl, 0.1f);
            merged_ok = st == PC_OK;
            if (!merged_ok) {
                TRY(pc_io_warn(warnings, err, "merged image could not be decoded: %s", e.msg));
            }
        }
    }

    if (multichannel) {
        /* Ink channels only (no layers); see multichannel_map. */
        if (merged_ok) {
            const pc_psd_resource *r = pc_psd_file_resource(file, 1045);
            pc_str *names = NULL;
            size_t nnames = 0;
            if (r != NULL) {
                TRY(unicode_names(r->data.data, r->data.len, &names, &nnames, err));
            }
            st = pc_io_multichannel_import(file, merged.data, merged.len, &doc, names, nnames, warnings, err);
            free_names(names, nnames);
            if (st != PC_OK) {
                goto fail;
            }
        }
        if (nl > 0) {
            TRY(pc_io_warn(warnings, err,
                           "Multichannel documents have no layers: %zu layer records were not imported", nl));
        }
    } else if (is_layered) {
        TRY(layered(&cx, &doc, &merged, merged_ok, err));
    } else {
        TRY(flattened(&cx, &doc, &merged, merged_ok, err));
    }
    if (is_layered && merged_ok) {
        TRY(extra_channels(&cx, &doc, &merged, err));
    }
    TRY(later_maps(&doc, warnings, err));
    if (pc_interrupt_cancelled(ctl)) {
        st = pc_io_cancelled(err);
        goto fail;
    }
    pc_interrupt_progress(ctl, 1.0f);
    pc_psd_bytes_fini(&merged);
    imp_fini(&cx);
    *out = doc;
    return PC_OK;
fail:
    pc_psd_bytes_fini(&merged);
    imp_fini(&cx);
    pc_document_fini(&doc);
    return st;
}

pc_status pc_io_psd_to_document(const pc_psd_file *file, pc_document *out, pc_io_warnings *warnings,
                                pc_error *err)
{
    return pc_io_psd_to_document_with(file, NULL, out, warnings, err);
}
