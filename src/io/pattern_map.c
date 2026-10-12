/* Patterns <-> PSD (port of crates/io/src/pattern_map.rs): the global Patt / Pat2 / Pat3 blocks
 * and .pat files (src/psd/patterns.h reads and writes them).
 *
 * Import parses the blocks into the document's patterns and keeps the raw blocks. Export reuses
 * the raw blocks verbatim while they still decode to the document's patterns (byte-exact round
 * trips); otherwise they are replaced by one block written from the document. */
#include "io/pattern_map.h"

#include <stdlib.h>
#include <string.h>

#include "base/blob.h"
#include "base/mem.h"
#include "io/core.h"
#include "io/pixels.h"
#include "psd/patterns.h"

static bool is_pattern_key(const uint8_t k[4])
{
    return memcmp(k, "Patt", 4) == 0 || memcmp(k, "Pat2", 4) == 0 || memcmp(k, "Pat3", 4) == 0;
}

static uint32_t psd_mode(pc_color_mode m)
{
    switch (m) {
    case PC_COLOR_MODE_GRAYSCALE:
        return 1;
    case PC_COLOR_MODE_CMYK:
        return 4;
    case PC_COLOR_MODE_LAB:
        return 9;
    default:
        return 3;
    }
}

/* 1-bit planes expanded to 8-bit grey (set bits are black). */
static pc_status expand_bits(const pc_psd_pattern *p, size_t w, size_t h, pc_psd_bytes *planes, pc_error *err)
{
    size_t row = (w + 7) / 8;
    size_t n = w * h;
    for (size_t c = 0; c < p->nchannels; c++) {
        const pc_psd_bytes *src = &p->channels[c];
        uint8_t *dst = pc_malloc(n);
        if (dst == NULL) {
            return pc_io_nomem(err, "reading a pattern");
        }
        for (size_t i = 0; i < n; i++) {
            size_t at = (i / w) * row + (i % w) / 8;
            bool set = at < src->len && (src->data[at] & (0x80u >> (i % w % 8))) != 0;
            dst[i] = set ? 0 : 255;
        }
        planes[c].data = dst;
        planes[c].len = n;
    }
    return PC_OK;
}

static void planes_free(pc_psd_bytes *p, size_t n, bool owned)
{
    for (size_t i = 0; owned && p != NULL && i < n; i++) {
        pc_psd_bytes_fini(&p[i]);
    }
    free(p);
}

pc_status pc_io_pattern_from_psd(const pc_psd_pattern *p, bool *found, pc_pattern *out, pc_error *err)
{
    size_t w = p->width;
    size_t h = p->height;
    pc_psd_bytes *chans = NULL; /* the colour planes used (borrowed or owned) */
    size_t nchans = p->nchannels;
    bool owned = false;
    uint16_t depth = p->depth;
    pc_color_mode mode;
    pc_sample_type s;
    pc_pixel_format fmt;
    size_t cc;
    pc_io_plane refs[5];
    uint8_t fill[5][4];
    bool invert[5];
    size_t nref = 0;
    uint8_t *bytes = NULL;
    size_t blen = 0;
    pc_pattern r;
    pc_status st = PC_OK;
    *found = false;
    if (w == 0 || h == 0) {
        return PC_OK;
    }
    if (w > INT32_MAX || h > INT32_MAX || h > SIZE_MAX / w) {
        return pc_fail(err, PC_ERR_LIMIT, "pattern of %zu x %zu pixels is too large", w, h);
    }
    if (p->depth == 1) {
        chans = pc_calloc_array(nchans > 0 ? nchans : 1, sizeof *chans);
        if (chans == NULL) {
            return pc_io_nomem(err, "reading a pattern");
        }
        owned = true;
        st = expand_bits(p, w, h, chans, err);
        depth = 8;
    } else {
        chans = pc_malloc_array(nchans > 0 ? nchans : 1, sizeof *chans);
        if (chans == NULL) {
            return pc_io_nomem(err, "reading a pattern");
        }
        if (nchans > 0) {
            memcpy(chans, p->channels, nchans * sizeof *chans);
        }
    }
    if (st != PC_OK) {
        planes_free(chans, nchans, owned);
        return st;
    }
    switch (p->mode) {
    case 1:
    case 8:
        mode = PC_COLOR_MODE_GRAYSCALE;
        break;
    case 2: {
        /* Indexed: through the palette into RGB. */
        pc_psd_bytes *rgb;
        const pc_psd_bytes *idx = nchans > 0 ? &chans[0] : NULL;
        if (!p->palette.has || idx == NULL) {
            planes_free(chans, nchans, owned);
            return PC_OK;
        }
        rgb = pc_calloc_array(3, sizeof *rgb);
        if (rgb == NULL) {
            planes_free(chans, nchans, owned);
            return pc_io_nomem(err, "reading a pattern");
        }
        for (size_t c = 0; c < 3; c++) {
            rgb[c].data = pc_malloc(idx->len);
            if (rgb[c].data == NULL) {
                planes_free(rgb, 3, true);
                planes_free(chans, nchans, owned);
                return pc_io_nomem(err, "reading a pattern");
            }
            rgb[c].len = idx->len;
            for (size_t i = 0; i < idx->len; i++) {
                size_t at = (size_t)idx->data[i] * 3 + c;
                rgb[c].data[i] = at < p->palette.v.len ? p->palette.v.data[at] : 0;
            }
        }
        planes_free(chans, nchans, owned);
        chans = rgb;
        nchans = 3;
        owned = true;
        mode = PC_COLOR_MODE_RGB;
        break;
    }
    case 4:
        mode = PC_COLOR_MODE_CMYK;
        break;
    case 9:
        mode = PC_COLOR_MODE_LAB;
        break;
    case 3:
        mode = PC_COLOR_MODE_RGB;
        break;
    default:
        mode = PC_COLOR_MODE_GRAYSCALE;
        if (nchans > 1) {
            for (size_t i = 1; owned && i < nchans; i++) {
                pc_psd_bytes_fini(&chans[i]);
            }
            nchans = 1;
        }
        break;
    }
    s = pc_io_sample_for_depth(depth);
    fmt.mode = mode;
    fmt.sample = s;
    fmt.alpha = p->alpha.has;
    cc = pc_psd_pattern_mode_channels(psd_mode(mode));
    if (nchans < cc || cc > 4) {
        planes_free(chans, nchans, owned);
        return PC_OK;
    }
    for (size_t c = 0; c < cc; c++) {
        refs[nref].present = true;
        refs[nref].data = chans[c].data;
        refs[nref].len = chans[c].len;
        (void)pc_io_zero_sample(s, fill[nref]);
        invert[nref++] = mode == PC_COLOR_MODE_CMYK;
    }
    if (p->alpha.has) {
        refs[nref].present = true;
        refs[nref].data = p->alpha.v.data;
        refs[nref].len = p->alpha.v.len;
        (void)pc_io_max_sample(s, fill[nref]);
        invert[nref++] = false;
    }
    st = pc_io_interleave(refs, nref, (const uint8_t (*)[4])fill, w * h, s, invert, &bytes, &blen, err);
    planes_free(chans, nchans, owned);
    if (st != PC_OK) {
        return st;
    }
    if (mode == PC_COLOR_MODE_LAB && s == PC_SAMPLE_TYPE_U16) {
        pc_io_lab16_chroma(bytes, blen, nref, true);
    }
    memset(&r, 0, sizeof r);
    st = pc_surface_from_interleaved(fmt, pc_rect_make(0, 0, (int32_t)w, (int32_t)h), bytes, blen, &r.surface,
                                     err);
    free(bytes);
    if (st != PC_OK) {
        return st;
    }
    r.width = p->width;
    r.height = p->height;
    st = pc_str_set(&r.id, pc_psd_str_c(&p->id), p->id.len, err);
    if (st == PC_OK) {
        st = pc_str_set(&r.name, pc_psd_str_c(&p->name), p->name.len, err);
    }
    if (st != PC_OK) {
        pc_pattern_fini(&r);
        return st;
    }
    *out = r;
    *found = true;
    return PC_OK;
}

pc_status pc_io_pattern_to_psd(const pc_pattern *p, pc_psd_pattern *out, pc_error *err)
{
    pc_pixel_format f = pc_surface_format(&p->surface);
    pc_color_mode mode = f.mode;
    pc_pixel_format fmt;
    pc_surface conv;
    const pc_surface *surf = &p->surface;
    bool converted = false;
    uint8_t *bytes = NULL;
    size_t blen = 0;
    size_t ch;
    bool invert[5];
    uint8_t *planes = NULL;
    size_t plane_len = 0;
    pc_psd_pattern r;
    pc_status st = PC_OK;
    if (mode != PC_COLOR_MODE_GRAYSCALE && mode != PC_COLOR_MODE_RGB && mode != PC_COLOR_MODE_CMYK &&
        mode != PC_COLOR_MODE_LAB) {
        mode = PC_COLOR_MODE_RGB;
    }
    fmt.mode = mode;
    fmt.sample = f.sample;
    fmt.alpha = f.alpha;
    if (!pc_pixel_format_eq(fmt, f)) {
        st = pc_surface_convert(&p->surface, fmt, &conv, err);
        if (st != PC_OK) {
            return st;
        }
        surf = &conv;
        converted = true;
    }
    st = pc_surface_to_interleaved(surf, pc_pattern_rect(p), &bytes, &blen, err);
    if (converted) {
        pc_surface_fini(&conv);
    }
    if (st != PC_OK) {
        return st;
    }
    ch = pc_pixel_format_channels(fmt);
    if (mode == PC_COLOR_MODE_LAB && fmt.sample == PC_SAMPLE_TYPE_U16) {
        pc_io_lab16_chroma(bytes, blen, ch, false);
    }
    for (size_t c = 0; c < ch && c < 5; c++) {
        invert[c] = mode == PC_COLOR_MODE_CMYK;
    }
    if (fmt.alpha) {
        invert[ch - 1] = false;
    }
    st = pc_io_deinterleave(bytes, blen, ch, fmt.sample, invert, &planes, &plane_len, err);
    free(bytes);
    if (st != PC_OK) {
        return st;
    }
    memset(&r, 0, sizeof r);
    r.mode = psd_mode(mode);
    r.width = p->width;
    r.height = p->height;
    r.depth = pc_io_psd_depth(fmt.sample);
    r.nchannels = fmt.alpha ? ch - 1 : ch;
    r.channels = pc_calloc_array(r.nchannels > 0 ? r.nchannels : 1, sizeof *r.channels);
    if (r.channels == NULL) {
        st = pc_io_nomem(err, "writing a pattern");
    }
    for (size_t c = 0; c < ch && st == PC_OK; c++) {
        pc_psd_bytes *dst = c < r.nchannels ? &r.channels[c] : &r.alpha.v;
        st = pc_psd_bytes_set(dst, planes != NULL ? planes + c * plane_len : NULL, plane_len, err);
        if (c >= r.nchannels && st == PC_OK) {
            r.alpha.has = true;
        }
    }
    free(planes);
    if (st == PC_OK) {
        st = pc_psd_str_set(&r.name, pc_str_c(&p->name), p->name.len, err);
    }
    if (st == PC_OK) {
        st = pc_psd_str_set(&r.id, pc_str_c(&p->id), p->id.len, err);
    }
    if (st != PC_OK) {
        pc_psd_pattern_fini(&r);
        return st;
    }
    *out = r;
    return PC_OK;
}

void pc_io_patterns_free(pc_pattern *v, size_t n)
{
    for (size_t i = 0; v != NULL && i < n; i++) {
        pc_pattern_fini(&v[i]);
    }
    free(v);
}

/* Appends the patterns decodable from one block (malformed blocks contribute nothing). */
static pc_status from_block(const uint8_t *data, size_t len, pc_pattern **v, size_t *n, size_t *cap,
                            pc_error *err)
{
    pc_psd_pattern *ps = NULL;
    size_t np = 0;
    pc_status st = pc_psd_parse_pattern_block(data != NULL ? data : (const uint8_t *)"", len, &ps, &np, err);
    if (st == PC_ERR_NOMEM) {
        return st;
    }
    if (st != PC_OK) {
        return PC_OK;
    }
    for (size_t i = 0; i < np && st == PC_OK; i++) {
        bool ok = false;
        pc_pattern p;
        st = pc_io_pattern_from_psd(&ps[i], &ok, &p, err);
        if (st == PC_OK && ok) {
            pc_pattern *g = pc_grow_array(*v, cap, *n + 1, sizeof **v);
            if (g == NULL) {
                pc_pattern_fini(&p);
                st = pc_io_nomem(err, "reading patterns");
            } else {
                *v = g;
                (*v)[(*n)++] = p;
            }
        }
    }
    pc_psd_patterns_free(ps, np);
    return st;
}

pc_status pc_io_patterns_from_global_blocks(const pc_document *doc, pc_pattern **out, size_t *n,
                                            pc_error *err)
{
    pc_pattern *v = NULL;
    size_t k = 0;
    size_t cap = 0;
    pc_status st = PC_OK;
    for (size_t i = 0; i < doc->metadata.npsd_global_blocks && st == PC_OK; i++) {
        const pc_doc_psd_global_block *b = &doc->metadata.psd_global_blocks[i];
        if (is_pattern_key(b->key)) {
            st = from_block(pc_blob_data(b->data), pc_blob_len(b->data), &v, &k, &cap, err);
        }
    }
    if (st != PC_OK) {
        pc_io_patterns_free(v, k);
        return st;
    }
    *out = v;
    *n = k;
    return PC_OK;
}

/* The document's patterns as PSD patterns (*out: n entries, pc_psd_patterns_free). */
static pc_status to_psd_all(const pc_pattern *p, size_t n, pc_psd_pattern **out, pc_error *err)
{
    pc_psd_pattern *v = pc_calloc_array(n > 0 ? n : 1, sizeof *v);
    if (v == NULL) {
        return pc_io_nomem(err, "writing patterns");
    }
    for (size_t i = 0; i < n; i++) {
        pc_status st = pc_io_pattern_to_psd(&p[i], &v[i], err);
        if (st != PC_OK) {
            pc_psd_patterns_free(v, i);
            return st;
        }
    }
    *out = v;
    return PC_OK;
}

pc_status pc_io_export_pattern_blocks(const pc_document *doc, pc_doc_psd_global_block **out, size_t *n,
                                      pc_error *err)
{
    const pc_metadata *m = &doc->metadata;
    pc_pattern *cur = NULL;
    size_t ncur = 0;
    bool same;
    pc_doc_psd_global_block *v;
    size_t k = 0;
    bool placed = false;
    uint8_t key[4];
    pc_blob *fresh = NULL;
    pc_status st = pc_io_patterns_from_global_blocks(doc, &cur, &ncur, err);
    if (st != PC_OK) {
        return st;
    }
    same = ncur == doc->npatterns;
    for (size_t i = 0; i < ncur && same; i++) {
        same = pc_pattern_eq(&cur[i], &doc->patterns[i]);
    }
    pc_io_patterns_free(cur, ncur);
    v = pc_malloc_array(m->npsd_global_blocks + 1, sizeof *v);
    if (v == NULL) {
        return pc_io_nomem(err, "writing patterns");
    }
    if (!same) {
        pc_psd_pattern *ps = NULL;
        pc_psd_bytes bytes = {NULL, 0};
        st = to_psd_all(doc->patterns, doc->npatterns, &ps, err);
        if (st != PC_OK) {
            free(v);
            return st;
        }
        st = pc_psd_write_pattern_block(ps, doc->npatterns, &bytes, err);
        pc_psd_patterns_free(ps, doc->npatterns);
        if (st == PC_ERR_NOMEM) {
            free(v);
            return st;
        }
        /* The reference writes an empty block when the patterns can't be encoded. */
        st = pc_blob_new(st == PC_OK ? bytes.data : NULL, st == PC_OK ? bytes.len : 0, &fresh, err);
        pc_psd_bytes_fini(&bytes);
        if (st != PC_OK) {
            free(v);
            return st;
        }
        pc_psd_pattern_block_key(pc_io_psd_depth(doc->depth), key);
    }
    for (size_t i = 0; i < m->npsd_global_blocks; i++) {
        const pc_doc_psd_global_block *b = &m->psd_global_blocks[i];
        if (!same && is_pattern_key(b->key)) {
            if (!placed) {
                memcpy(v[k].signature, "8BIM", 4);
                memcpy(v[k].key, key, 4);
                v[k++].data = pc_blob_retain(fresh);
                placed = true;
            }
            continue;
        }
        v[k] = *b;
        v[k++].data = pc_blob_retain(b->data);
    }
    if (!same && !placed && doc->npatterns > 0) {
        memcpy(v[k].signature, "8BIM", 4);
        memcpy(v[k].key, key, 4);
        v[k++].data = pc_blob_retain(fresh);
    }
    pc_blob_release(fresh);
    *out = v;
    *n = k;
    return PC_OK;
}

pc_status pc_io_read_pat(const uint8_t *bytes, size_t len, pc_pattern **out, size_t *n, pc_error *err)
{
    pc_psd_pattern *ps = NULL;
    size_t np = 0;
    pc_pattern *v = NULL;
    size_t k = 0;
    size_t cap = 0;
    pc_status st = pc_psd_parse_pat_file(bytes, len, &ps, &np, err);
    if (st != PC_OK) {
        if (st != PC_ERR_NOMEM && err != NULL) {
            char msg[sizeof err->msg];
            memcpy(msg, err->msg, sizeof msg);
            msg[sizeof msg - 1] = '\0';
            (void)pc_fail(err, st, "not a readable .pat file: %s", msg);
        }
        return st;
    }
    for (size_t i = 0; i < np && st == PC_OK; i++) {
        bool ok = false;
        pc_pattern p;
        st = pc_io_pattern_from_psd(&ps[i], &ok, &p, err);
        if (st == PC_OK && ok) {
            pc_pattern *g = pc_grow_array(v, &cap, k + 1, sizeof *v);
            if (g == NULL) {
                pc_pattern_fini(&p);
                st = pc_io_nomem(err, "reading patterns");
            } else {
                v = g;
                v[k++] = p;
            }
        }
    }
    pc_psd_patterns_free(ps, np);
    if (st != PC_OK) {
        pc_io_patterns_free(v, k);
        return st;
    }
    *out = v;
    *n = k;
    return PC_OK;
}

pc_status pc_io_write_pat(const pc_pattern *patterns, size_t n, pc_buf *out, pc_error *err)
{
    pc_psd_pattern *ps = NULL;
    pc_psd_bytes bytes;
    pc_buf b = PC_BUF_INIT;
    pc_status st = to_psd_all(patterns, n, &ps, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_psd_write_pat_file(ps, n, &bytes, err);
    pc_psd_patterns_free(ps, n);
    if (st != PC_OK) {
        return st;
    }
    st = pc_buf_append(&b, bytes.data, bytes.len, err);
    pc_psd_bytes_fini(&bytes);
    if (st != PC_OK) {
        return st;
    }
    pc_buf_fini(out);
    *out = b;
    return PC_OK;
}
