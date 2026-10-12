/* Layered TIFF import and export (port of crates/io/src/tiff_layers.rs). */
#include "io/tiff_layers.h"

#include <stdlib.h>
#include <string.h>

#include "base/blob.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"
#include "io/flat.h"
#include "io/psd_export.h"
#include "io/psd_import.h"
#include "psd/resources.h"
#include "psd/tiff.h"

bool pc_io_would_write_layers(const pc_document *doc)
{
    return !(pc_io_single_layer(doc) != NULL && doc->nlayers > 0 && doc->layers[0].locks.transparency);
}

/* Documents a layered TIFF can hold: the composite in the document's model (Gray, RGB or CMYK;
 * the codecs have no Lab TIFF) and layers to keep (Multichannel has none). */
static bool layered_mode(const pc_document *doc)
{
    pc_color_mode m = pc_document_pixel_format(doc).mode;
    return (m == PC_COLOR_MODE_GRAYSCALE || m == PC_COLOR_MODE_RGB || m == PC_COLOR_MODE_CMYK) &&
           doc->mode != PC_COLOR_MODE_MULTICHANNEL && doc->nlayers > 0;
}

typedef struct res_list {
    pc_psd_resource *v;
    size_t n;
    size_t cap;
} res_list;

/* Replaces (or appends) resource id; removes it when data is NULL. */
static pc_status set_resource(res_list *r, uint16_t id, const void *data, size_t len, pc_error *err)
{
    size_t kept = 0;
    pc_psd_resource *v;
    pc_status st;
    for (size_t i = 0; i < r->n; i++) {
        if (r->v[i].id == id) {
            pc_psd_resource_fini(&r->v[i]);
        } else {
            r->v[kept++] = r->v[i];
        }
    }
    r->n = kept;
    if (data == NULL) {
        return PC_OK;
    }
    v = pc_grow_array(r->v, &r->cap, r->n + 1, sizeof *v);
    if (v == NULL) {
        return pc_io_nomem(err, "adding a resource");
    }
    r->v = v;
    st = pc_psd_resource_init(&v[r->n], id, data, len, err);
    if (st == PC_OK) {
        r->n++;
    }
    return st;
}

/* Interleaved native-endian samples -> planar big-endian planes (PSD merged-image order),
 * inverting the colour channels of CMYK (PSD stores ink as max - value). */
static pc_status planar_big_endian(const pc_cimage *img, size_t color_channels, bool invert_color,
                                   uint8_t **out, size_t *out_len, pc_error *err)
{
    size_t n = (size_t)img->width * img->height;
    size_t ch = pc_clayout_channels(img->layout);
    size_t bps = pc_csample_bytes(img->sample);
    uint8_t *buf = NULL;
    size_t len = 0;
    pc_status st;
    if (bps != 1 && bps != 2 && bps != 4) {
        return pc_io_unsupported(err, "%zu-byte samples", bps);
    }
    st = pc_io_try_buffer(n * ch, bps, &buf, &len, err);
    if (st != PC_OK) {
        return st;
    }
    if (img->len / (ch * bps) < n) {
        free(buf);
        return pc_fail(err, PC_ERR_INVALID, "invalid image: buffer is shorter than %zu pixels", n);
    }
    for (size_t c = 0; c < ch; c++) {
        bool invert = invert_color && c < color_channels;
        uint8_t *plane = buf + c * n * bps;
        for (size_t i = 0; i < n; i++) {
            const uint8_t *src = img->data + (i * ch + c) * bps;
            uint8_t *dst = plane + i * bps;
            if (bps == 1) {
                dst[0] = invert ? (uint8_t)(255 - src[0]) : src[0];
            } else if (bps == 2) {
                uint16_t v;
                memcpy(&v, src, 2);
                v = invert ? (uint16_t)(65535 - v) : v;
                dst[0] = (uint8_t)(v >> 8);
                dst[1] = (uint8_t)v;
            } else {
                float f;
                uint32_t b;
                memcpy(&f, src, 4);
                f = invert ? 1.0f - f : f;
                memcpy(&b, &f, 4);
                dst[0] = (uint8_t)(b >> 24);
                dst[1] = (uint8_t)(b >> 16);
                dst[2] = (uint8_t)(b >> 8);
                dst[3] = (uint8_t)b;
            }
        }
    }
    *out = buf;
    *out_len = len;
    return PC_OK;
}

/* Planar big-endian planes (the PSD merged image) -> interleaved native-endian samples of
 * `channels` channels, un-inverting CMYK colour channels. */
static pc_status interleave_planes(const uint8_t *planes, size_t planes_len, uint32_t w, uint32_t h,
                                   size_t channels, size_t color_channels, pc_csample sample,
                                   bool invert_color, uint8_t **out, size_t *out_len, pc_error *err)
{
    size_t n = (size_t)w * h;
    size_t bps = pc_csample_bytes(sample);
    size_t plane = n * bps;
    size_t need;
    uint8_t *buf = NULL;
    size_t len = 0;
    size_t at = 0;
    pc_status st;
    if (!pc_size_mul(plane, channels, &need) || planes_len < need) {
        return pc_io_unsupported(err, "the merged image is shorter than its header says");
    }
    if (bps != 1 && bps != 2 && bps != 4) {
        return pc_io_unsupported(err, "%zu-byte samples", bps);
    }
    st = pc_io_try_buffer(n * channels, bps, &buf, &len, err);
    if (st != PC_OK) {
        return st;
    }
    for (size_t i = 0; i < n; i++) {
        for (size_t c = 0; c < channels; c++) {
            const uint8_t *s = planes + c * plane + i * bps;
            bool invert = invert_color && c < color_channels;
            if (bps == 1) {
                buf[at] = invert ? (uint8_t)(255 - s[0]) : s[0];
            } else if (bps == 2) {
                uint16_t v = (uint16_t)((unsigned)s[0] << 8 | s[1]);
                v = invert ? (uint16_t)(65535 - v) : v;
                memcpy(buf + at, &v, 2);
            } else {
                uint32_t b = (uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 | s[3];
                float f;
                memcpy(&f, &b, 4);
                f = invert ? 1.0f - f : f;
                memcpy(buf + at, &f, 4);
            }
            at += bps;
        }
    }
    *out = buf;
    *out_len = len;
    return PC_OK;
}

/* The resources from tag 34377 plus the TIFF's own tags, with warnings. */
static pc_status gather_resources(const pc_cimage *img, res_list *res, pc_io_warnings *warnings,
                                  pc_error *err)
{
    pc_status st = PC_OK;
    if (img->meta.photoshop_resources.has && img->meta.photoshop_resources.len > 0) {
        pc_psd_warnings w = {NULL, 0};
        pc_error e;
        st = pc_psd_tiff_resources_parse(img->meta.photoshop_resources.data,
                                         img->meta.photoshop_resources.len, &res->v, &res->n, &w, &e);
        if (st == PC_OK) {
            res->cap = res->n;
            st = pc_io_warnings_extend(warnings, "image resources: ", w.v, w.n, err);
        } else if (st == PC_ERR_NOMEM) {
            if (err != NULL) {
                *err = e;
            }
        } else {
            res->v = NULL;
            res->n = 0;
            st = pc_io_warn(warnings, err, "the Photoshop image resources could not be read: %s", e.msg);
        }
        pc_psd_warnings_fini(&w);
        if (st != PC_OK) {
            return st;
        }
    }
    /* The profile, resolution, XMP and EXIF are the TIFF's own tags; they win over any copy in the
     * resources. */
    st = set_resource(res, PC_PSD_RES_ICC_PROFILE, img->icc.has ? (const void *)img->icc.data : NULL,
                      img->icc.len, err);
    if (st == PC_OK && img->meta.has_dpi && img->meta.dpi_x > 0.0f) {
        /* Both axes, so the PSD import notes a vertical resolution the document can't keep. */
        pc_psd_resolution ri = pc_psd_resolution_from_dpi((double)img->meta.dpi_x);
        uint8_t b[16];
        ri.v_res_fixed = pc_psd_resolution_from_dpi((double)img->meta.dpi_y).v_res_fixed;
        pc_psd_resolution_write(&ri, b);
        st = set_resource(res, PC_PSD_RES_RESOLUTION_INFO, b, sizeof b, err);
    }
    if (st == PC_OK && img->meta.xmp.has) {
        st = set_resource(res, PC_PSD_RES_XMP, img->meta.xmp.data, img->meta.xmp.len, err);
    }
    if (st == PC_OK && img->meta.exif.has) {
        st = set_resource(res, PC_PSD_RES_EXIF, img->meta.exif.data, img->meta.exif.len, err);
    }
    return st;
}

pc_status pc_io_import_layered(const char *name, const pc_cimage *img0, const uint8_t *layers,
                               size_t layers_len, pc_io_import_result *out, pc_error *err)
{
    pc_io_import_result r;
    pc_psd_isd isd;
    pc_psd_warnings w = {NULL, 0};
    pc_error e;
    res_list res = {NULL, 0, 0};
    pc_cimage conv;
    const pc_cimage *img = img0;
    bool converted = false;
    uint16_t mode;
    size_t color_channels;
    uint16_t depth;
    pc_psd_header header;
    pc_psd_bytes data = {NULL, 0};
    pc_psd_file file;
    pc_status st = pc_psd_isd_parse(layers, layers_len, &isd, &w, &e);
    if (st != PC_OK) {
        pc_psd_warnings_fini(&w);
        if (st == PC_ERR_NOMEM) {
            if (err != NULL) {
                *err = e;
            }
            return st;
        }
        st = pc_io_image_to_document(name, img0, &r, err);
        if (st == PC_OK) {
            st = pc_io_warn(&r.warnings, err,
                            "the Photoshop layer data could not be read (%s); opened flattened", e.msg);
            if (st != PC_OK) {
                pc_io_import_result_fini(&r);
            }
        }
        if (st == PC_OK) {
            *out = r;
        }
        return st;
    }
    memset(&r, 0, sizeof r);
    pc_io_warnings_init(&r.warnings);
    st = pc_io_warnings_extend(&r.warnings, "layer data: ", w.v, w.n, err);
    pc_psd_warnings_fini(&w);
    if (st == PC_OK) {
        st = gather_resources(img0, &res, &r.warnings, err);
    }
    /* The TIFF's pixels become the PSD's merged image: planar, big-endian, CMYK inverted. */
    if (st == PC_OK && img0->sample == PC_CS_F16) {
        st = pc_io_warn(&r.warnings, err, "16-bit float samples are stored as 32-bit float");
        if (st == PC_OK) {
            st = pc_cimage_convert(img0, img0->layout, PC_CS_F32, &conv, err);
            converted = st == PC_OK;
            img = converted ? &conv : img0;
        }
    }
    if (pc_clayout_is_gray(img->layout)) {
        mode = PC_PSD_MODE_GRAYSCALE;
        color_channels = 1;
    } else if (pc_clayout_is_rgb(img->layout)) {
        mode = PC_PSD_MODE_RGB;
        color_channels = 3;
    } else {
        mode = PC_PSD_MODE_CMYK;
        color_channels = 4;
    }
    depth = img->sample == PC_CS_U8 ? 8 : img->sample == PC_CS_U16 ? 16 : 32;
    header = pc_psd_header_make(isd.version, img->width, img->height,
                                (uint16_t)pc_clayout_channels(img->layout), depth, mode);
    if (st == PC_OK) {
        st = planar_big_endian(img, color_channels, mode == PC_PSD_MODE_CMYK, &data.data, &data.len, err);
    }
    if (st != PC_OK) {
        pc_psd_isd_fini(&isd);
        pc_psd_resources_free(res.v, res.n);
        if (converted) {
            pc_cimage_fini(&conv);
        }
        pc_io_warnings_fini(&r.warnings);
        return st;
    }
    /* into_psd takes over isd, the resources and the planes, also on failure. */
    st = pc_psd_isd_into_psd(&isd, header, &res.v, res.n, PC_PSD_RAW, &data, &file, err);
    if (st == PC_OK) {
        st = pc_io_psd_to_document(&file, &r.document, &r.warnings, err);
        pc_psd_file_fini(&file);
    }
    if (st == PC_OK) {
        st = pc_str_set_c(&r.document.name, name, err);
        if (st != PC_OK) {
            pc_document_fini(&r.document);
        }
    }
    if (st != PC_OK) {
        if (converted) {
            pc_cimage_fini(&conv);
        }
        pc_io_warnings_fini(&r.warnings);
        return st;
    }
    if (r.document.icc_profile == NULL && img->icc.has) {
        st = pc_io_set_blob(&r.document.icc_profile, img->icc.data, img->icc.len, err);
    }
    if (st == PC_OK && img->meta.ntext > 0) {
        st = pc_io_warn(&r.warnings, err, "%zu text metadata entries are not kept in the document",
                        img->meta.ntext);
    }
    if (converted) {
        pc_cimage_fini(&conv);
    }
    if (st != PC_OK) {
        pc_io_import_result_fini(&r);
        return st;
    }
    *out = r;
    return PC_OK;
}

pc_status pc_io_export_layered(const pc_document *doc, const pc_io_export_opts *opts,
                               pc_io_export_result *out, pc_error *err)
{
    pc_io_warnings warnings = PC_IO_WARNINGS_INIT;
    pc_io_psd_export_opts po = pc_io_psd_export_opts_default();
    bool big = doc->size.width > 30000 || doc->size.height > 30000;
    pc_psd_file file;
    pc_psd_isd isd;
    pc_psd_bytes layers = {NULL, 0};
    pc_psd_bytes resources = {NULL, 0};
    pc_psd_warnings w = {NULL, 0};
    pc_psd_resource *kept = NULL;
    size_t nkept = 0;
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    size_t cc = pc_color_mode_channels(fmt.mode);
    bool has_alpha;
    size_t channels;
    pc_psd_bytes merged = {NULL, 0};
    const pc_psd_bytes *planes;
    uint8_t *data = NULL;
    size_t len = 0;
    pc_cimage img;
    pc_status st;
    if (!layered_mode(doc)) {
        pc_io_export_result r;
        st = pc_io_export_flat(doc, PC_CF_TIFF, opts, &r, err);
        if (st == PC_OK && doc->nlayers > 0) {
            st = pc_io_warn(&r.warnings, err,
                            "%s documents are saved as a flat TIFF: their layers are not kept",
                            pc_color_mode_name(fmt.mode));
            if (st != PC_OK) {
                pc_io_export_result_fini(&r);
            }
        }
        if (st == PC_OK) {
            *out = r;
        }
        return st;
    }
    /* The PSD exporter renders the composite the TIFF needs: unmatted (the TIFF keeps straight
     * alpha) and uncompressed (its planes are interleaved below). */
    po.force_psb = opts->force_psb || big;
    po.merged_matte = false;
    po.merged_compression = PC_PSD_RAW;
    st = pc_io_document_to_psd_with(doc, &po, &file, &warnings, err);
    if (st != PC_OK) {
        pc_io_warnings_fini(&warnings);
        return st;
    }
    st = pc_psd_isd_from_psd(&file, &isd, err);
    if (st == PC_OK) {
        st = pc_psd_isd_to_bytes(&isd, pc_ctiff_writes_little_endian() ? PC_PSD_LITTLE : PC_PSD_BIG, &layers,
                                 &w, err);
        pc_psd_isd_fini(&isd);
        st = pc_io_err_prefix(st, err, "PSD: ");
    } else {
        st = pc_io_err_prefix(st, err, "PSD: ");
    }
    if (st == PC_OK) {
        st = pc_io_warnings_extend(&warnings, "layer data: ", w.v, w.n, err);
    }
    pc_psd_warnings_fini(&w);
    /* The profile and XMP are TIFF tags, not resources. EXIF stays a resource (1058): the TIFF
     * codec has no EXIF directory, and the PSD importer reads it back from there. */
    if (st == PC_OK) {
        kept = pc_malloc_array(file.nresources > 0 ? file.nresources : 1, sizeof *kept);
        if (kept == NULL) {
            st = pc_io_nomem(err, "filtering resources");
        }
    }
    for (size_t i = 0; st == PC_OK && i < file.nresources; i++) {
        if (file.resources[i].id != PC_PSD_RES_ICC_PROFILE && file.resources[i].id != PC_PSD_RES_XMP) {
            kept[nkept++] = file.resources[i]; /* shallow, borrowed */
        }
    }
    if (st == PC_OK) {
        st = pc_io_err_prefix(pc_psd_tiff_resources_to_bytes(kept, nkept, &resources, err), err, "PSD: ");
    }
    free(kept);
    has_alpha = file.has_layer_info && file.layer_info.merged_alpha;
    channels = cc + (has_alpha ? 1 : 0);
    if (st == PC_OK && file.header.channels > channels) {
        st = pc_io_warn(&warnings, err,
                        "%zu alpha channel(s) are not written to TIFF (one transparency channel only)",
                        (size_t)file.header.channels - channels);
    }
    planes = &file.image_data;
    if (st == PC_OK && file.image_compression != PC_PSD_RAW) {
        /* The exporter writes raw planes when asked; decode anything else rather than misread it. */
        st = pc_io_err_prefix(pc_psd_file_decode_merged(&file, &merged, err), err, "PSD: ");
        planes = &merged;
    }
    if (st == PC_OK) {
        st = interleave_planes(planes->data, planes->len, doc->size.width, doc->size.height, channels, cc,
                               pc_io_csample(fmt.sample), fmt.mode == PC_COLOR_MODE_CMYK, &data, &len, err);
    }
    pc_psd_bytes_fini(&merged);
    pc_psd_file_fini(&file);
    if (st == PC_OK) {
        st = pc_cimage_from_raw(doc->size.width, doc->size.height, pc_io_layout_for(fmt.mode, has_alpha),
                                pc_io_csample(fmt.sample), data, len, &img, err);
        st = pc_io_err_prefix(st, err, "codec: ");
    }
    if (st != PC_OK) {
        pc_psd_bytes_fini(&layers);
        pc_psd_bytes_fini(&resources);
        pc_io_warnings_fini(&warnings);
        return st;
    }
    if (doc->icc_profile != NULL) {
        st = pc_meta_blob_set(&img.icc, pc_blob_data(doc->icc_profile), pc_blob_len(doc->icc_profile), err);
    }
    /* EXIF is deliberately not set: the TIFF codec has no EXIF directory, so resource 1058 carries
     * it and the importer reads it back from there (a second copy made the export claim it would
     * drop metadata it kept, #1545). */
    if (st == PC_OK && doc->metadata.has_xmp && opts->xmp == PC_IO_XMP_ALL) {
        st = pc_meta_blob_set(&img.meta.xmp, doc->metadata.xmp.ptr, doc->metadata.xmp.len, err);
    }
    img.meta.has_dpi = true;
    img.meta.dpi_x = doc->resolution_dpi;
    img.meta.dpi_y = doc->resolution_dpi;
    if (st == PC_OK) {
        st = pc_meta_blob_set(&img.meta.photoshop_resources, resources.data, resources.len, err);
    }
    if (st == PC_OK) {
        st = pc_meta_blob_set(&img.meta.photoshop_layers, layers.data, layers.len, err);
    }
    pc_psd_bytes_fini(&layers);
    pc_psd_bytes_fini(&resources);
    if (st == PC_OK) {
        st = pc_io_encode_image(&img, PC_CF_TIFF, opts, &warnings, out, err);
    }
    pc_cimage_fini(&img);
    pc_io_warnings_fini(&warnings);
    return st;
}

/* Keeps the blocks that are not in foreign byte order; a warning per dropped block. */
static pc_status strip(pc_psd_block *blocks, size_t *n, const char *owner, pc_io_warnings *warnings,
                       pc_error *err)
{
    size_t kept = 0;
    pc_status st = PC_OK;
    for (size_t i = 0; i < *n; i++) {
        pc_psd_block *b = &blocks[i];
        if (!pc_psd_is_foreign_order_signature(b->signature, 4)) {
            blocks[kept++] = *b;
            continue;
        }
        if (st == PC_OK) {
            pc_buf key = PC_BUF_INIT;
            st = pc_utf8_lossy(b->key, 4, &key, err);
            if (st == PC_OK) {
                st = pc_io_warn(
                    warnings, err,
                    "%s: %s block (%zu bytes) kept from a little-endian TIFF cannot be written to PSD "
                    "and was dropped",
                    owner, pc_buf_cstr(&key), b->data.len);
            }
            pc_buf_fini(&key);
        }
        pc_psd_block_fini(b);
    }
    *n = kept;
    return st;
}

pc_status pc_io_strip_foreign_order_blocks(pc_psd_file *file, pc_io_warnings *warnings, pc_error *err)
{
    pc_status st = PC_OK;
    if (file->has_layer_info) {
        for (size_t i = 0; i < file->layer_info.n && st == PC_OK; i++) {
            pc_psd_layer *l = &file->layer_info.layers[i];
            char *name = NULL;
            pc_buf owner = PC_BUF_INIT;
            st = pc_psd_layer_name(l, &name, err);
            if (st == PC_OK) {
                st = pc_buf_appendf(&owner, err, "layer \"%s\"", name);
            }
            if (st == PC_OK) {
                st = strip(l->blocks, &l->nblocks, pc_buf_cstr(&owner), warnings, err);
            }
            free(name);
            pc_buf_fini(&owner);
        }
    }
    if (st == PC_OK) {
        st = strip(file->global_blocks, &file->nglobal, "document", warnings, err);
    }
    return st;
}
