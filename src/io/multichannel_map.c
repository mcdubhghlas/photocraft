/* Multichannel documents <-> PSD colour mode 7 (port of crates/io/src/multichannel_map.rs).
 *
 * A Multichannel document has no layers: its image is a set of ink (spot) channels with ink
 * density as the value (1 = solid ink). A PSD stores the planes as Photoshop shows them in the
 * Channels panel: 0 (black) = solid ink, max = paper. Names come from resources 1006 / 1045 and
 * ink colours from DisplayInfo (1077). */
#include "io/channel_map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/mem.h"
#include "base/num.h"
#include "codecs/resolution.h"
#include "io/annotations_map.h"
#include "io/pattern_map.h"
#include "io/pixels.h"
#include "io/psd_common.h"
#include "io/psd_export_internal.h"
#include "psd/misc.h"
#include "psd/resources.h"

pc_status pc_io_multichannel_import(const pc_psd_file *file, const uint8_t *all, size_t all_len,
                                    pc_document *doc, const pc_str *names, size_t nnames,
                                    pc_io_warnings *warnings, pc_error *err)
{
    const pc_psd_header *h = &file->header;
    pc_sample_type depth = doc->depth;
    size_t w = h->width;
    size_t hh = h->height;
    size_t n;
    size_t plane;
    pc_rect canvas = pc_rect_make(0, 0, pc_u32_as_i32(h->width), pc_u32_as_i32(h->height));
    pc_pixel_format fmt = {PC_COLOR_MODE_GRAYSCALE, depth, false};
    float *vals;
    const pc_psd_resource *r;
    pc_status st = PC_OK;
    if (!pc_size_mul(w, hh, &n) || !pc_size_mul(pc_psd_header_row_bytes(h), hh, &plane) ||
        n > PC_MAX_ALLOC / sizeof(float)) {
        return pc_fail(err, PC_ERR_LIMIT, "limit exceeded: multichannel image of %zux%zu", w, hh);
    }
    vals = pc_malloc_array(n, sizeof *vals);
    if (vals == NULL) {
        return pc_io_nomem(err, "reading multichannel planes");
    }
    for (size_t k = 0; k < h->channels && st == PC_OK; k++) {
        const uint8_t *p;
        pc_surface s;
        pc_alpha_channel ch;
        char fallback[32];
        const char *name = fallback;
        pc_str nm = PC_STR_INIT;
        if (plane > 0 && (k + 1 > all_len / plane)) {
            st = pc_io_warn(warnings, err, "multichannel: channel %zu is missing", k + 1);
            break;
        }
        p = all + k * plane;
        /* pc_io_decode_be reads 0 past the plane: a hostile 1-bit header can't overrun it. */
        for (size_t i = 0; i < n; i++) {
            vals[i] = 1.0f - pc_io_decode_be(p, plane, i, depth);
        }
        pc_surface_init(&s, fmt);
        st = pc_surface_write_region(&s, canvas, vals, n, err);
        if (st != PC_OK) {
            pc_surface_fini(&s);
            break;
        }
        pc_surface_prune(&s);
        if (k < nnames && names[k].len > 0) {
            st = pc_str_clone(&names[k], &nm, err);
            name = NULL;
        } else {
            (void)snprintf(fallback, sizeof fallback, "Channel %zu", k + 1);
        }
        if (st == PC_OK) {
            st = pc_alpha_channel_init(&ch, name != NULL ? name : "", &s, err);
        }
        if (st != PC_OK) {
            pc_str_fini(&nm);
            pc_surface_fini(&s);
            break;
        }
        if (name == NULL) {
            /* Names may hold U+0000: keep the exact bytes. */
            pc_str_fini(&ch.name);
            ch.name = nm;
        }
        /* Without DisplayInfo the channels print in black, as Photoshop shows them. */
        ch.has_spot = true;
        ch.spot_color = PC_COLOR_BLACK;
        ch.spot_solidity = 0.0f;
        {
            pc_alpha_channel *v =
                pc_grow_array(doc->channels, &(size_t){doc->nchannels}, doc->nchannels + 1, sizeof *v);
            if (v == NULL) {
                pc_alpha_channel_fini(&ch);
                st = pc_io_nomem(err, "adding a channel");
                break;
            }
            doc->channels = v;
            v[doc->nchannels++] = ch;
        }
    }
    free(vals);
    if (st != PC_OK) {
        return st;
    }
    r = pc_psd_file_resource(file, PC_IO_DISPLAY_INFO);
    if (r != NULL) {
        pc_io_apply_display_info(r->data.data, r->data.len, true, 0, doc->channels, doc->nchannels);
    } else if ((r = pc_psd_file_resource(file, PC_IO_DISPLAY_INFO_OLD)) != NULL) {
        pc_io_apply_display_info(r->data.data, r->data.len, false, 0, doc->channels, doc->nchannels);
    }
    return PC_OK;
}

/* Grid and guides. */
#define PC_IO_MC_GRID_AND_GUIDES 1032
/* Alpha identifiers: one u32 per extra channel. */
#define PC_IO_MC_ALPHA_IDENTIFIERS 1053

/* Resources that only describe layers (a Multichannel document has no layers). */
static bool layer_only_resource(uint16_t id)
{
    return id == PC_PSD_RES_LAYER_STATE || id == PC_PSD_RES_LAYER_GROUP_INFO || id == 1069 || id == 1072;
}

pc_status pc_io_multichannel_to_psd(const pc_document *doc, bool force_psb, pc_psd_file *out,
                                    pc_io_warnings *warnings, pc_error *err)
{
    pc_sample_type sample = doc->depth;
    pc_rect canvas = pc_document_bounds(doc);
    size_t nch = doc->nchannels < 56 ? doc->nchannels : 56;
    size_t npx = 0;
    size_t bps;
    size_t total = 0;
    uint8_t *planes = NULL;
    size_t at = 0;
    bool big = doc->size.width > 30000 || doc->size.height > 30000;
    bool size_exceeds = false;
    pc_psd_file f;
    pc_io_res_list res = {NULL, 0, 0};
    pc_doc_psd_global_block *pat = NULL;
    size_t npat = 0;
    pc_doc_psd_global_block *ann = NULL;
    size_t nann = 0;
    pc_psd_block *gblocks = NULL;
    size_t ngblocks = 0;
    pc_buf tmp = PC_BUF_INIT;
    pc_status st = PC_OK;
    pc_psd_file_init(&f);
    /* Photoshop's Multichannel mode is 8 or 16 bits per channel. */
    if (sample == PC_SAMPLE_TYPE_F32) {
        st = pc_io_warn(warnings, err,
                        "32-bit Multichannel written as 16 bits/channel "
                        "(32-bit Multichannel isn't supported)");
        sample = PC_SAMPLE_TYPE_U16;
    }
    if (st == PC_OK && doc->nlayers > 0) {
        st = pc_io_warn(warnings, err, "Multichannel documents have no layers: the layers were not written");
    }
    if (st == PC_OK && doc->nchannels > 56) {
        st = pc_io_warn(warnings, err, "only 56 channels fit in PSD; the rest were dropped");
    }
    bps = pc_sample_type_bytes(sample);
    if (st == PC_OK && (!pc_size_mul(pc_rect_width(canvas), pc_rect_size(canvas).height, &npx) ||
                        !pc_size_mul3(npx, nch > 0 ? nch : 1, bps, &total) || total > PC_MAX_ALLOC)) {
        st = pc_fail(err, PC_ERR_LIMIT, "limit exceeded: multichannel image of %zu pixels", npx);
    }
    if (st == PC_OK && (planes = pc_malloc(total)) == NULL) {
        st = pc_io_nomem(err, "writing multichannel planes");
    }
    if (st == PC_OK && nch == 0) {
        /* A PSD needs at least one channel: an empty (paper) plane. */
        for (size_t i = 0; i < npx; i++) {
            at += pc_io_encode_be(1.0f, sample, planes + at);
        }
    }
    for (size_t k = 0; k < nch && st == PC_OK; k++) {
        const pc_surface *s = &doc->channels[k].surface;
        size_t sch = pc_surface_channels(s);
        float *v = NULL;
        size_t len = 0;
        st = pc_surface_read_region(s, canvas, &v, &len, err);
        for (size_t i = 0; st == PC_OK && sch > 0 && i + sch <= len && at + bps <= total; i += sch) {
            at += pc_io_encode_be(1.0f - v[i], sample, planes + at);
        }
        free(v);
    }
    if (st == PC_OK) {
        st = pc_io_psd_size_exceeds_limit(doc, &size_exceeds, err);
        size_exceeds = size_exceeds && !force_psb && !big;
    }
    if (st == PC_OK && big && !force_psb) {
        st = pc_io_warn(warnings, err, "document exceeds 30000 px; written as PSB");
    } else if (st == PC_OK && size_exceeds && !force_psb) {
        st = pc_io_warn(warnings, err, "estimated encoded size exceeds 2 GB; written as PSB");
    }
    if (st == PC_OK) {
        pc_psd_version version = pc_io_psd_version(doc, force_psb, size_exceeds);
        pc_psd_bytes enc = {NULL, 0};
        f.header =
            pc_psd_header_make(version, doc->size.width, doc->size.height, (uint16_t)(nch > 0 ? nch : 1),
                               pc_io_psd_depth(sample), PC_PSD_MODE_MULTICHANNEL);
        st = pc_psd_image_encode(PC_PSD_RLE, planes, at, &f.header, &enc, err);
        if (st == PC_OK) {
            f.image_compression = PC_PSD_RLE;
            f.image_data = enc;
        } else if (st != PC_ERR_NOMEM) {
            /* As the reference: RLE, else raw, else the planes as they are. */
            st = pc_psd_image_encode(PC_PSD_RAW, planes, at, &f.header, &enc, err);
            if (st == PC_OK) {
                f.image_data = enc;
            } else if (st != PC_ERR_NOMEM) {
                f.image_data.data = planes;
                f.image_data.len = at;
                planes = NULL;
                st = PC_OK;
            }
            f.image_compression = PC_PSD_RAW;
        }
    }
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
    /* Grid and guides: Photoshop writes it in every file, guides or not. */
    if (st == PC_OK) {
        pc_buf_truncate(&tmp, 0);
        st = pc_io_guides_resource(doc, &tmp, err);
        st = st == PC_OK ? pc_io_res_add(&res, PC_IO_MC_GRID_AND_GUIDES, tmp.data, tmp.len, err) : st;
    }
    if (st == PC_OK && nch > 0) {
        const pc_alpha_channel **chs = pc_malloc_array(nch, sizeof *chs);
        pc_str *names = pc_malloc_array(nch, sizeof *names);
        if (chs == NULL || names == NULL) {
            st = pc_io_nomem(err, "writing channel names");
        }
        for (size_t k = 0; k < nch && st == PC_OK; k++) {
            chs[k] = &doc->channels[k];
            names[k] = doc->channels[k].name; /* borrowed */
        }
        if (st == PC_OK) {
            st = pc_io_pascal_names_resource(names, nch, &tmp, err);
        }
        if (st == PC_OK) {
            st = pc_io_res_add(&res, 1006, tmp.data, tmp.len, err);
        }
        if (st == PC_OK) {
            st = pc_io_unicode_names_resource(names, nch, &tmp, err);
        }
        if (st == PC_OK) {
            st = pc_io_res_add(&res, 1045, tmp.data, tmp.len, err);
        }
        if (st == PC_OK) {
            st = pc_io_display_info(chs, nch, false, &tmp, err);
        }
        if (st == PC_OK) {
            st = pc_io_res_add(&res, PC_IO_DISPLAY_INFO, tmp.data, tmp.len, err);
        }
        free(chs);
        free(names);
    }
    if (st == PC_OK) {
        st = pc_io_path_resources(&res, doc, warnings, err);
    }
    /* The XMP and EXIF resolution follow the ResolutionInfo resource, so no copy contradicts it. */
    {
        float ppi[2] = {doc->resolution_dpi, doc->resolution_dpi};
        const float *ppi_ptr = doc->resolution_dpi > 0.0f ? ppi : NULL;
        if (st == PC_OK && doc->metadata.has_xmp) {
            char *x = NULL;
            size_t xl = 0;
            st = pc_export_xmp(pc_str_c(&doc->metadata.xmp), doc->metadata.xmp.len, ppi_ptr, &x, &xl, err);
            if (st == PC_OK) {
                st = x != NULL ? pc_io_res_add(&res, PC_PSD_RES_XMP, x, xl, err)
                               : pc_io_res_add(&res, PC_PSD_RES_XMP, pc_str_c(&doc->metadata.xmp),
                                               doc->metadata.xmp.len, err);
            }
            free(x);
        }
        if (st == PC_OK && doc->metadata.exif != NULL) {
            uint8_t *x = NULL;
            size_t xl = 0;
            st = pc_export_exif(pc_blob_data(doc->metadata.exif), pc_blob_len(doc->metadata.exif), ppi_ptr,
                                &x, &xl, err);
            if (st == PC_OK) {
                st = x != NULL ? pc_io_res_add(&res, PC_PSD_RES_EXIF, x, xl, err)
                               : pc_io_res_add(&res, PC_PSD_RES_EXIF, pc_blob_data(doc->metadata.exif),
                                               pc_blob_len(doc->metadata.exif), err);
            }
            free(x);
        }
    }
    /* Every other imported resource (print settings, IPTC, the layer-id seed, ...), except the
     * ones that only describe layers and any already written above. */
    for (size_t i = 0; i < doc->metadata.npsd_resources && st == PC_OK; i++) {
        const pc_doc_psd_resource *r = &doc->metadata.psd_resources[i];
        bool written = false;
        for (size_t j = 0; j < res.n && !written; j++) {
            written = res.v[j].id == r->id;
        }
        if (layer_only_resource(r->id) || written ||
            !pc_io_keep_imported_resource(doc, r->id, pc_blob_data(r->data), pc_blob_len(r->data))) {
            continue;
        }
        /* Alpha identifiers: one u32 per channel, only while they still match the channels. */
        if (r->id == PC_IO_MC_ALPHA_IDENTIFIERS && pc_blob_len(r->data) != nch * 4) {
            continue;
        }
        st = pc_io_res_add_named(&res, r->id, &r->name, pc_blob_data(r->data), pc_blob_len(r->data), err);
    }
    if (st == PC_OK) {
        bool has = false;
        pc_buf_truncate(&tmp, 0);
        st = pc_io_fresh_scale_resource(doc, &has, &tmp, err);
        if (st == PC_OK && has) {
            st = pc_io_res_add(&res, PC_IO_MEASUREMENT_SCALE, tmp.data, tmp.len, err);
        }
    }
    if (st == PC_OK) {
        pc_psd_resource *v = pc_grow_array(res.v, &res.cap, res.n + 1, sizeof *v);
        if (v == NULL) {
            st = pc_io_nomem(err, "adding a resource");
        } else {
            res.v = v;
            st = pc_psd_version_info_resource(true, &res.v[res.n], err);
            if (st == PC_OK) {
                res.n++;
            }
        }
    }
    /* Document-level tagged blocks (patterns, notes, ...) are valid without a layer section; a
     * kept layer block would describe layers the document doesn't have. */
    if (st == PC_OK) {
        st = pc_io_export_pattern_blocks(doc, &pat, &npat, err);
    }
    if (st == PC_OK) {
        st = pc_io_annotation_export_blocks(doc, pat, npat, &ann, &nann, err);
    }
    if (st == PC_OK && nann > 0) {
        gblocks = pc_calloc_array(nann, sizeof *gblocks);
        if (gblocks == NULL) {
            st = pc_io_nomem(err, "writing global blocks");
        }
    }
    for (size_t i = 0; i < nann && st == PC_OK; i++) {
        if (memcmp(ann[i].key, "Layr", 4) == 0 || memcmp(ann[i].key, "Lr16", 4) == 0 ||
            memcmp(ann[i].key, "Lr32", 4) == 0) {
            continue;
        }
        st = pc_io_global_block(ann[i].signature, ann[i].key, pc_blob_data(ann[i].data),
                                pc_blob_len(ann[i].data), &gblocks[ngblocks], err);
        if (st == PC_OK) {
            ngblocks++;
        }
    }
    for (size_t i = 0; i < nann; i++) {
        pc_blob_release(ann[i].data);
    }
    free(ann);
    if (st == PC_OK) {
        st = pc_psd_color_mode_data_for_depth(f.header.depth, &f.color_mode_data, err);
    }
    pc_buf_fini(&tmp);
    free(planes);
    if (st != PC_OK) {
        pc_psd_resources_free(res.v, res.n);
        pc_psd_blocks_free(gblocks, ngblocks);
        pc_psd_file_fini(&f);
        return st;
    }
    f.resources = res.v;
    f.nresources = res.n;
    f.has_layer_info = false;
    f.placement.where = PC_PSD_LI_SECTION;
    f.has_global_mask = ngblocks > 0;
    f.global_blocks = gblocks;
    f.nglobal = ngblocks;
    *out = f;
    return PC_OK;
}
