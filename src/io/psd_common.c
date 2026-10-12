/* PSD version choice, size estimate and channel-name resources (from crates/io/src/psd_export.rs). */
#include "io/psd_common.h"

#include "base/blob.h"
#include "base/utf8.h"
#include "raster/surface.h"

#define FIXED_OVERHEAD ((uint64_t)1024 * 1024)
#define LAYER_OVERHEAD ((uint64_t)64 * 1024)

pc_psd_version pc_io_psd_version(const pc_document *doc, bool force_psb, bool size_exceeds_limit)
{
    bool exceeds_dimensions = doc->size.width > 30000 || doc->size.height > 30000;
    return force_psb || exceeds_dimensions || size_exceeds_limit ? PC_PSD_V2 : PC_PSD_V1;
}

static bool add(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a > UINT64_MAX - b) {
        return false;
    }
    *out = a + b;
    return true;
}

static bool mul(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a != 0 && b > UINT64_MAX / a) {
        return false;
    }
    *out = a * b;
    return true;
}

static bool add_plane(uint64_t *raw, uint64_t *rows, uint64_t w, uint64_t h, uint64_t channels, uint64_t bps)
{
    uint64_t plane;
    uint64_t prow;
    return mul(w, h, &plane) && mul(plane, channels, &plane) && mul(plane, bps, &plane) &&
           mul(h, channels, &prow) && add(*raw, plane, raw) && add(*rows, prow, rows);
}

static void dims(const pc_surface *s, uint64_t *w, uint64_t *h)
{
    pc_rect b = pc_surface_content_bounds(s);
    *w = pc_rect_width(b);
    *h = pc_rect_size(b).height;
}

/* The per-layer part of the estimate; false on overflow. */
static bool layer_estimate(const pc_layer *l, uint64_t width, uint64_t height, uint64_t layer_channels,
                           uint64_t bps, uint64_t *raw, uint64_t *rows, uint64_t *structural)
{
    const pc_surface *surface = NULL;
    uint64_t sw;
    uint64_t sh;
    if (!add(*structural, LAYER_OVERHEAD, structural) || !add(*structural, l->name.len, structural)) {
        return false;
    }
    for (size_t i = 0; i < l->npsd_blocks; i++) {
        if (!add(*structural, pc_blob_len(l->psd_blocks[i].data), structural)) {
            return false;
        }
    }
    if (l->content.kind == PC_LAYER_FILL) {
        uint64_t fw = width;
        uint64_t fh = height;
        if (l->has_fill_cache && pc_fill_eq(&l->fill_cache.fill, &l->content.u.fill)) {
            dims(&l->fill_cache.surface, &sw, &sh);
            fw = fw > sw ? fw : sw;
            fh = fh > sh ? fh : sh;
        }
        if (!add_plane(raw, rows, fw, fh, layer_channels, bps)) {
            return false;
        }
    } else {
        surface = pc_layer_surface(l);
    }
    if (surface != NULL) {
        dims(surface, &sw, &sh);
        if (!add_plane(raw, rows, sw, sh, layer_channels, bps)) {
            return false;
        }
    }
    if (l->has_mask) {
        dims(&l->mask.surface, &sw, &sh);
        if (!add_plane(raw, rows, sw, sh, 1, bps)) {
            return false;
        }
    }
    if (l->content.kind == PC_LAYER_SMART && !l->content.u.smart.source.linked) {
        if (!add(*structural, pc_blob_len(l->content.u.smart.source.bytes), structural)) {
            return false;
        }
    }
    return true;
}

pc_status pc_io_estimate_psd_size(const pc_document *doc, bool *ok, uint64_t *out, pc_error *err)
{
    uint64_t raw = 0;
    uint64_t rows = 0;
    uint64_t structural = FIXED_OVERHEAD;
    uint64_t width = doc->size.width;
    uint64_t height = doc->size.height;
    bool good = true;
    uint64_t total;
    if (doc->mode == PC_COLOR_MODE_MULTICHANNEL) {
        size_t n = doc->nchannels < 1 ? 1 : (doc->nchannels > 56 ? 56 : doc->nchannels);
        uint64_t bps = doc->depth == PC_SAMPLE_TYPE_F32 ? 2 : pc_sample_type_bytes(doc->depth);
        good = add_plane(&raw, &rows, width, height, n, bps);
    } else {
        pc_pixel_format f = pc_document_pixel_format(doc);
        size_t cc = pc_color_mode_channels(f.mode);
        uint64_t bps = pc_sample_type_bytes(f.sample);
        size_t max_extra = 56 > cc + 1 ? 56 - (cc + 1) : 0;
        size_t extra = doc->nchannels < max_extra ? doc->nchannels : max_extra;
        pc_walk w;
        pc_status st = pc_document_walk(doc, &w, err);
        if (st != PC_OK) {
            return st;
        }
        for (size_t i = 0; i < w.len && good; i++) {
            good = layer_estimate(w.items[i].layer, width, height, (uint64_t)cc + 1, bps, &raw, &rows,
                                  &structural);
        }
        pc_walk_fini(&w);
        if (doc->has_quick_mask && extra < max_extra) {
            extra++;
        }
        good = good && add_plane(&raw, &rows, width, height, (uint64_t)cc + 1 + extra, bps);
        for (size_t i = 0; i < extra && i < doc->nchannels && good; i++) {
            good = add(structural, doc->channels[i].name.len, &structural);
        }
    }
    good = good && add(structural, pc_blob_len(doc->icc_profile), &structural) &&
           add(structural, doc->metadata.has_xmp ? doc->metadata.xmp.len : 0, &structural) &&
           add(structural, pc_blob_len(doc->metadata.exif), &structural);
    for (size_t i = 0; i < doc->metadata.npsd_resources && good; i++) {
        const pc_doc_psd_resource *r = &doc->metadata.psd_resources[i];
        good =
            add(structural, r->name.len, &structural) && add(structural, pc_blob_len(r->data), &structural);
    }
    for (size_t i = 0; i < doc->metadata.npsd_global_blocks && good; i++) {
        good = add(structural, pc_blob_len(doc->metadata.psd_global_blocks[i].data), &structural);
    }
    /* raw + ceil-ish raw / 64 (RLE worst case) + 8 bytes per row + structure. */
    good = good && raw <= UINT64_MAX - 63 && add(raw, (raw + 63) / 64, &total) && mul(rows, 8, &rows) &&
           add(total, rows, &total) && add(total, structural, &total);
    *ok = good;
    *out = good ? total : 0;
    return PC_OK;
}

pc_status pc_io_psd_size_exceeds_limit(const pc_document *doc, bool *out, pc_error *err)
{
    bool ok = false;
    uint64_t size = 0;
    pc_status st = pc_io_estimate_psd_size(doc, &ok, &size, err);
    if (st == PC_OK) {
        *out = !ok || size > PC_IO_PSD_MAX_FILE_SIZE;
    }
    return st;
}

/* The next scalar of s (invalid bytes read as U+FFFD, one byte at a time). */
static uint32_t next_cp(const pc_str *s, size_t *i)
{
    uint32_t cp;
    if (pc_utf8_next((const uint8_t *)s->ptr, s->len, i, &cp)) {
        return cp;
    }
    (*i)++;
    return 0xFFFD;
}

static pc_status be16(pc_buf *b, uint32_t v, pc_error *err)
{
    uint8_t x[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    return pc_buf_append(b, x, 2, err);
}

pc_status pc_io_unicode_names_resource(const pc_str *names, size_t n, pc_buf *out, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    pc_status st = PC_OK;
    for (size_t k = 0; k < n && st == PC_OK; k++) {
        size_t units = 1; /* the terminator */
        size_t i = 0;
        uint8_t cnt[4];
        while (i < names[k].len) {
            units += next_cp(&names[k], &i) >= 0x10000 ? 2 : 1;
        }
        cnt[0] = (uint8_t)(units >> 24);
        cnt[1] = (uint8_t)(units >> 16);
        cnt[2] = (uint8_t)(units >> 8);
        cnt[3] = (uint8_t)units;
        st = pc_buf_append(&b, cnt, 4, err);
        i = 0;
        while (st == PC_OK && i < names[k].len) {
            uint32_t cp = next_cp(&names[k], &i);
            if (cp >= 0x10000) {
                cp -= 0x10000;
                st = be16(&b, 0xD800 + (cp >> 10), err);
                if (st == PC_OK) {
                    st = be16(&b, 0xDC00 + (cp & 0x3FF), err);
                }
            } else {
                st = be16(&b, cp, err);
            }
        }
        if (st == PC_OK) {
            st = be16(&b, 0, err);
        }
    }
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    pc_buf_fini(out);
    *out = b;
    return PC_OK;
}

pc_status pc_io_pascal_names_resource(const pc_str *names, size_t n, pc_buf *out, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    pc_status st = PC_OK;
    for (size_t k = 0; k < n && st == PC_OK; k++) {
        uint8_t legacy[256];
        size_t len = 0;
        size_t i = 0;
        while (i < names[k].len && len < 255) {
            uint32_t cp = next_cp(&names[k], &i);
            legacy[1 + len++] = cp < 0x80 && cp >= 0x20 && cp != 0x7F ? (uint8_t)cp : (uint8_t)'?';
        }
        legacy[0] = (uint8_t)len;
        st = pc_buf_append(&b, legacy, len + 1, err);
    }
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    pc_buf_fini(out);
    *out = b;
    return PC_OK;
}
