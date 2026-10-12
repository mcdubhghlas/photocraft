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

#include "base/mem.h"
#include "base/num.h"
#include "io/pixels.h"
#include "io/psd_common.h"
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
        pc_io_apply_display_info(r->data.data, r->data.len, true, doc->channels, doc->nchannels);
    } else if ((r = pc_psd_file_resource(file, PC_IO_DISPLAY_INFO_OLD)) != NULL) {
        pc_io_apply_display_info(r->data.data, r->data.len, false, doc->channels, doc->nchannels);
    }
    return PC_OK;
}

/* Appends resource (id, data) to the array. */
static pc_status add_res(pc_psd_resource **res, size_t *n, size_t *cap, uint16_t id, const void *data,
                         size_t len, pc_error *err)
{
    pc_psd_resource *v = pc_grow_array(*res, cap, *n + 1, sizeof *v);
    pc_status st;
    if (v == NULL) {
        return pc_io_nomem(err, "adding a resource");
    }
    *res = v;
    st = pc_psd_resource_init(&v[*n], id, data, len, err);
    if (st == PC_OK) {
        (*n)++;
    }
    return st;
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
    pc_psd_resource *res = NULL;
    size_t nres = 0;
    size_t cap = 0;
    pc_buf tmp = PC_BUF_INIT;
    pc_status st = PC_OK;
    pc_psd_file_init(&f);
    /* Photoshop's Multichannel mode is 8 or 16 bits per channel. */
    if (sample == PC_SAMPLE_TYPE_F32) {
        st = pc_io_warn(warnings, err,
                        "32-bit Multichannel written as 16 bits/channel "
                        "(Photoshop has no 32-bit Multichannel)");
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
        pc_psd_resolution r = pc_psd_resolution_from_dpi((double)doc->resolution_dpi);
        pc_psd_resolution_write(&r, ri);
        st = add_res(&res, &nres, &cap, PC_PSD_RES_RESOLUTION_INFO, ri, sizeof ri, err);
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
            st = add_res(&res, &nres, &cap, 1006, tmp.data, tmp.len, err);
        }
        if (st == PC_OK) {
            st = pc_io_unicode_names_resource(names, nch, &tmp, err);
        }
        if (st == PC_OK) {
            st = add_res(&res, &nres, &cap, 1045, tmp.data, tmp.len, err);
        }
        if (st == PC_OK) {
            st = pc_io_display_info(chs, nch, &tmp, err);
        }
        if (st == PC_OK) {
            st = add_res(&res, &nres, &cap, PC_IO_DISPLAY_INFO, tmp.data, tmp.len, err);
        }
        free(chs);
        free(names);
    }
    if (st == PC_OK && doc->metadata.has_xmp) {
        st = add_res(&res, &nres, &cap, PC_PSD_RES_XMP, doc->metadata.xmp.ptr, doc->metadata.xmp.len, err);
    }
    if (st == PC_OK) {
        pc_psd_resource *v = pc_grow_array(res, &cap, nres + 1, sizeof *v);
        if (v == NULL) {
            st = pc_io_nomem(err, "adding a resource");
        } else {
            res = v;
            st = pc_psd_version_info_resource(true, &res[nres], err);
            if (st == PC_OK) {
                nres++;
            }
        }
    }
    if (st == PC_OK) {
        st = pc_psd_color_mode_data_for_depth(f.header.depth, &f.color_mode_data, err);
    }
    pc_buf_fini(&tmp);
    free(planes);
    if (st != PC_OK) {
        pc_psd_resources_free(res, nres);
        pc_psd_file_fini(&f);
        return st;
    }
    f.resources = res;
    f.nresources = nres;
    f.has_layer_info = false;
    f.placement.where = PC_PSD_LI_SECTION;
    f.has_global_mask = false;
    *out = f;
    return PC_OK;
}
