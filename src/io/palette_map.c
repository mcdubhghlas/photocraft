/* Indexed Color and Bitmap documents -> PSD colour modes 2 and 0 (port of
 * crates/io/src/palette_map.rs). Neither mode has layers in Photoshop, so the file holds only the
 * merged image (the document flattened): for Indexed Color one plane of palette indices, with the
 * Color Table as Color Mode Data (768 bytes: 256 reds, 256 greens, 256 blues), its length in
 * resource 1046 and the transparent entry in resource 1047; for Bitmap one plane of 1 bit per
 * pixel, 1 = black. Conventions: io/core.h. */
#include "io/psd_export_internal.h"

#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/mem.h"
#include "base/num.h"
#include "compose/compose.h"
#include "doc/mode.h"
#include "io/channel_map.h"
#include "io/core.h"
#include "io/pixels.h"
#include "io/psd_common.h"
#include "psd/header.h"
#include "psd/misc.h"
#include "psd/resources.h"

#define COLOR_TABLE_COUNT 1046
#define TRANSPARENCY_INDEX 1047
#define GRID_AND_GUIDES 1032

/* Channel resources (names, display info, alpha identifiers): these modes have no extra channels. */
static bool palette_keep(void *ctx, uint16_t id, const uint8_t *data, size_t len)
{
    (void)ctx;
    (void)data;
    (void)len;
    return id != 1006 && id != 1045 && id != 1053 && id != PC_IO_DISPLAY_INFO &&
           id != TRANSPARENCY_INDEX && id != COLOR_TABLE_COUNT;
}

/* Whether the merged image holds everything the layers do: no layer, or a single visible pixel
 * layer at full opacity, Normal blend, unclipped, without masks or layer effects. */
static bool flattens_losslessly(const pc_document *doc)
{
    const pc_layer *l;
    if (doc->nlayers == 0) {
        return true;
    }
    if (doc->nlayers != 1) {
        return false;
    }
    l = &doc->layers[0];
    if (!l->visible || l->content.kind != PC_LAYER_RASTER || l->has_mask || l->has_vector_mask ||
        l->clipped || l->opacity < 1.0f || l->fill_opacity < 1.0f || l->blend != PC_BLEND_NORMAL) {
        return false;
    }
    if (l->effects.enabled) {
        for (size_t i = 0; i < l->effects.nitems; i++) {
            if (pc_effect_enabled(&l->effects.items[i])) {
                return false;
            }
        }
    }
    return true;
}

/* The Color Table as Color Mode Data: 256 reds, 256 greens, 256 blues (unused entries black). */
static pc_status palette_bytes(const pc_color_table *t, pc_psd_bytes *out, pc_error *err)
{
    uint8_t *m = pc_calloc_array(768, 1);
    size_t n = t->ncolors < 256 ? t->ncolors : 256;
    if (m == NULL) {
        return pc_io_nomem(err, "indexed palette");
    }
    for (size_t i = 0; i < n; i++) {
        m[i] = t->colors[i][0];
        m[256 + i] = t->colors[i][1];
        m[512 + i] = t->colors[i][2];
    }
    out->data = m;
    out->len = 768;
    return PC_OK;
}

typedef struct idx_ctx {
    const pc_color_table *full;
    const pc_color_table *opaque;
    const uint8_t *opaque_indices;
    bool has_transparent;
    uint8_t transparent;
    uint8_t *idx;
    size_t len;
    size_t at;
} idx_ctx;

static pc_status idx_sink(void *vctx, pc_compose_buffer *band, pc_error *err)
{
    idx_ctx *c = vctx;
    (void)err;
    for (size_t i = 0; i < band->len && c->at < c->len; i++) {
        const float *p = band->px[i];
        if (c->has_transparent && p[3] < 0.5f) {
            c->idx[c->at++] = c->transparent;
        } else if (c->has_transparent) {
            float rgb[3] = {p[0], p[1], p[2]};
            size_t k = pc_color_table_nearest(c->opaque, rgb);
            c->idx[c->at++] = c->opaque->ncolors > 0 ? c->opaque_indices[k] : 0;
        } else {
            float a = p[3];
            float rgb[3] = {p[0] * a + (1.0f - a), p[1] * a + (1.0f - a), p[2] * a + (1.0f - a)};
            c->idx[c->at++] = (uint8_t)pc_color_table_nearest(c->full, rgb);
        }
    }
    return PC_OK;
}

typedef struct bits_ctx {
    uint32_t w;
    size_t row;
    uint8_t *out;
    size_t len;
    size_t at;
} bits_ctx;

static pc_status bits_sink(void *vctx, pc_compose_buffer *band, pc_error *err)
{
    bits_ctx *c = vctx;
    (void)err;
    for (size_t i = 0; i < band->len; i++) {
        const float *p = band->px[i];
        float a = p[3];
        float grey = (p[0] + p[1] + p[2]) / 3.0f * a + (1.0f - a);
        size_t x = c->w > 0 ? c->at % c->w : 0;
        size_t y = c->w > 0 ? c->at / c->w : 0;
        size_t byte = y * c->row + x / 8;
        if (grey < 0.5f && byte < c->len) {
            c->out[byte] |= (uint8_t)(0x80u >> (x % 8));
        }
        c->at++;
    }
    return PC_OK;
}

/* A valid colour table for Indexed export: at most 256 entries, with at least one that is not the
 * transparent entry. */
static const pc_color_table *valid_table(const pc_document *doc)
{
    const pc_color_table *t = &doc->color_table;
    if (!doc->has_color_table || t->ncolors > 256) {
        return NULL;
    }
    for (size_t i = 0; i < t->ncolors; i++) {
        if (!t->has_transparent || i != (size_t)t->transparent) {
            return t;
        }
    }
    return NULL;
}

static pc_status encode_plane(uint32_t w, uint32_t h, uint16_t depth, uint16_t mode, pc_psd_version version,
                              const uint8_t *plane, size_t plen, pc_psd_header *header_out,
                              pc_psd_bytes *img_out, uint16_t *comp_out, pc_error *err)
{
    pc_psd_header header = pc_psd_header_make(version, w, h, 1, depth, mode);
    pc_psd_bytes enc = {NULL, 0};
    pc_status st = pc_psd_image_encode(PC_PSD_RLE, plane, plen, &header, &enc, err);
    uint16_t comp = PC_PSD_RLE;
    if (st != PC_OK && st != PC_ERR_NOMEM) {
        st = pc_psd_image_encode(PC_PSD_RAW, plane, plen, &header, &enc, err);
        comp = PC_PSD_RAW;
        if (st != PC_OK && st != PC_ERR_NOMEM) {
            /* As the reference: RLE, else raw, else the plane as it is. */
            enc.data = pc_malloc(plen > 0 ? plen : 1);
            if (enc.data == NULL) {
                return pc_io_nomem(err, "indexed image data");
            }
            memcpy(enc.data, plane, plen);
            enc.len = plen;
            st = PC_OK;
        }
    }
    if (st != PC_OK) {
        return st;
    }
    *header_out = header;
    *img_out = enc;
    *comp_out = comp;
    return PC_OK;
}

pc_status pc_io_palette_to_psd(const pc_document *doc, bool force_psb, bool *handled, pc_psd_file *out,
                               pc_io_warnings *warnings, pc_error *err)
{
    const pc_color_table *table = NULL;
    uint16_t mode;
    uint16_t depth;
    uint8_t *plane = NULL;
    size_t plen = 0;
    uint8_t (*ocolors)[3] = NULL;
    uint8_t *omap = NULL;
    pc_color_table opaque;
    bool size_exceeds = false;
    bool big = doc->size.width > 30000 || doc->size.height > 30000;
    pc_psd_version version;
    pc_psd_header header;
    pc_psd_bytes img = {NULL, 0};
    pc_psd_bytes cmd = {NULL, 0};
    uint16_t comp = PC_PSD_RAW;
    pc_io_res_list res = {NULL, 0, 0};
    pc_rect canvas = pc_document_bounds(doc);
    size_t area;
    pc_status st = PC_OK;
    *handled = false;
    /* A document with alpha channels or a Quick Mask is written like the others: the importer does
     * not read the extra channels of these modes back, so they would be lost. */
    if (doc->nchannels != 0 || doc->has_quick_mask) {
        return PC_OK;
    }
    if (doc->mode == PC_COLOR_MODE_INDEXED) {
        table = valid_table(doc);
        if (table == NULL) {
            return PC_OK;
        }
        mode = PC_PSD_MODE_INDEXED;
        depth = 8;
    } else if (doc->mode == PC_COLOR_MODE_BITMAP) {
        mode = PC_PSD_MODE_BITMAP;
        depth = 1;
    } else {
        return PC_OK;
    }
    if (!pc_size_mul(pc_rect_width(canvas), (size_t)pc_rect_size(canvas).height, &area)) {
        return pc_fail(err, PC_ERR_LIMIT, "limit exceeded: palette image too large");
    }
    if (table != NULL) {
        /* Indexed: palette indices. The transparent entry is kept out of the opaque lookup. */
        idx_ctx c;
        size_t nop = 0;
        ocolors = pc_malloc_array(table->ncolors > 0 ? table->ncolors : 1, sizeof *ocolors);
        omap = pc_malloc(table->ncolors > 0 ? table->ncolors : 1);
        plane = pc_malloc(area > 0 ? area : 1);
        if (ocolors == NULL || omap == NULL || plane == NULL) {
            free(ocolors);
            free(omap);
            free(plane);
            return pc_io_nomem(err, "indexed indices");
        }
        for (size_t i = 0; i < table->ncolors; i++) {
            if (table->has_transparent && i == (size_t)table->transparent) {
                continue;
            }
            ocolors[nop][0] = table->colors[i][0];
            ocolors[nop][1] = table->colors[i][1];
            ocolors[nop][2] = table->colors[i][2];
            omap[nop] = (uint8_t)i;
            nop++;
        }
        memset(&opaque, 0, sizeof opaque);
        opaque.colors = ocolors;
        opaque.ncolors = nop;
        memset(&c, 0, sizeof c);
        c.full = table;
        c.opaque = &opaque;
        c.opaque_indices = omap;
        c.has_transparent = table->has_transparent;
        c.transparent = table->transparent;
        c.idx = plane;
        c.len = area;
        st = pc_compose_render_bands(doc, canvas, 0, NULL, idx_sink, &c, err);
        plen = c.at;
        free(ocolors);
        free(omap);
        ocolors = NULL;
        omap = NULL;
    } else {
        /* Bitmap: one bit per pixel, 1 = black, rows padded to a byte. */
        bits_ctx c;
        size_t row = ((size_t)doc->size.width + 7) / 8;
        size_t total;
        if (!pc_size_mul(row, doc->size.height, &total)) {
            return pc_fail(err, PC_ERR_LIMIT, "limit exceeded: bitmap image too large");
        }
        plane = pc_calloc_array(total > 0 ? total : 1, 1);
        if (plane == NULL) {
            return pc_io_nomem(err, "bitmap bits");
        }
        memset(&c, 0, sizeof c);
        c.w = doc->size.width;
        c.row = row;
        c.out = plane;
        c.len = total;
        st = pc_compose_render_bands(doc, canvas, 0, NULL, bits_sink, &c, err);
        plen = total;
    }
    if (st != PC_OK) {
        free(plane);
        return st;
    }
    if (!flattens_losslessly(doc)) {
        st = pc_io_warn(warnings, err, "%s documents have no layers in Photoshop: written flattened",
                        pc_color_mode_name(doc->mode));
    }
    if (st == PC_OK && !force_psb && !big) {
        st = pc_io_psd_size_exceeds_limit(doc, &size_exceeds, err);
    }
    version = pc_io_psd_version(doc, force_psb, size_exceeds);
    if (st == PC_OK) {
        st = encode_plane(doc->size.width, doc->size.height, depth, mode, version, plane, plen, &header,
                          &img, &comp, err);
    }
    free(plane);
    plane = NULL;
    if (st != PC_OK) {
        return st;
    }
    if (table != NULL) {
        st = palette_bytes(table, &cmd, err);
    }
    /* Resolution, global light, grid and guides, then (Indexed) the table-count, transparency and
     * the RGB ICC profile. */
    if (st == PC_OK) {
        uint8_t ri[16];
        uint8_t b4[4];
        pc_psd_resolution r = pc_psd_resolution_from_dpi((double)doc->resolution_dpi);
        pc_psd_resolution_write(&r, ri);
        st = pc_io_res_add(&res, PC_PSD_RES_RESOLUTION_INFO, ri, sizeof ri, err);
        pc_store_be32(b4, (uint32_t)pc_f32_to_i32(roundf(doc->global_light.angle)));
        st = st == PC_OK ? pc_io_res_add(&res, PC_PSD_RES_GLOBAL_ANGLE, b4, 4, err) : st;
        pc_store_be32(b4, (uint32_t)pc_f32_to_i32(roundf(doc->global_light.altitude)));
        st = st == PC_OK ? pc_io_res_add(&res, PC_PSD_RES_GLOBAL_ALTITUDE, b4, 4, err) : st;
    }
    if (st == PC_OK) {
        pc_buf tmp = PC_BUF_INIT;
        st = pc_io_guides_resource(doc, &tmp, err);
        st = st == PC_OK ? pc_io_res_add(&res, GRID_AND_GUIDES, tmp.data, tmp.len, err) : st;
        pc_buf_fini(&tmp);
    }
    if (st == PC_OK && table != NULL) {
        uint8_t b2[2];
        pc_store_be16(b2, (uint16_t)table->ncolors);
        st = pc_io_res_add(&res, COLOR_TABLE_COUNT, b2, 2, err);
        if (st == PC_OK && table->has_transparent) {
            pc_store_be16(b2, table->transparent);
            st = pc_io_res_add(&res, TRANSPARENCY_INDEX, b2, 2, err);
        }
        /* Photoshop keeps the RGB profile of an Indexed document (Bitmap has none). A profile of
         * another colour space does not describe the table. */
        if (st == PC_OK && doc->icc_profile != NULL) {
            const uint8_t *icc = pc_blob_data(doc->icc_profile);
            size_t iccn = pc_blob_len(doc->icc_profile);
            if (iccn >= 20 && memcmp(icc + 16, "RGB ", 4) == 0) {
                st = pc_io_res_add(&res, PC_PSD_RES_ICC_PROFILE, icc, iccn, err);
            }
        }
    }
    if (st != PC_OK) {
        pc_psd_resources_free(res.v, res.n);
        pc_psd_bytes_fini(&img);
        pc_psd_bytes_fini(&cmd);
        return st;
    }
    /* Consumes res, img and cmd. */
    st = pc_io_layerless_file(doc, header, img, comp, &cmd, &res, palette_keep, NULL, warnings, out, err);
    if (st == PC_OK) {
        *handled = true;
    }
    return st;
}
