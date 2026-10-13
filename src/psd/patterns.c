#include "psd/patterns.h"

#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"
#include "psd/internal.h"

/* A PackBits run packet emits at most 128 bytes from a 2-byte packet. */
/* Budget for zero-padded channel planes summed over one pattern: Photoshop writes every channel at
 * the pattern rectangle, so a smaller channel rectangle is only padding (abr's MAX_TOTAL_BYTES). */
#define MAX_PADDED_PATTERN_BYTES ((uint64_t)512 << 20)
#define MAX_RLE_EXPANSION 64u
#define SLOTS 24u

size_t pc_psd_pattern_mode_channels(uint32_t mode)
{
    return mode == 3 || mode == 9 ? 3 : mode == 4 ? 4 : 1;
}

void pc_psd_pattern_fini(pc_psd_pattern *p)
{
    size_t i;
    if (p == NULL) {
        return;
    }
    pc_psd_str_fini(&p->name);
    pc_psd_str_fini(&p->id);
    pc_psd_opt_bytes_fini(&p->palette);
    for (i = 0; i < p->nchannels; i++) {
        pc_psd_bytes_fini(&p->channels[i]);
    }
    free(p->channels);
    pc_psd_opt_bytes_fini(&p->alpha);
    memset(p, 0, sizeof *p);
}

void pc_psd_patterns_free(pc_psd_pattern *p, size_t n)
{
    size_t i;
    for (i = 0; p != NULL && i < n; i++) {
        pc_psd_pattern_fini(&p[i]);
    }
    free(p);
}

bool pc_psd_pattern_eq(const pc_psd_pattern *a, const pc_psd_pattern *b)
{
    size_t i;
    if (a->mode != b->mode || a->width != b->width || a->height != b->height || a->depth != b->depth ||
        !pc_psd_str_eq(&a->name, &b->name) || !pc_psd_str_eq(&a->id, &b->id) ||
        !pc_psd_opt_bytes_eq(&a->palette, &b->palette) || !pc_psd_opt_bytes_eq(&a->alpha, &b->alpha) ||
        a->nchannels != b->nchannels) {
        return false;
    }
    for (i = 0; i < a->nchannels; i++) {
        if (!pc_psd_bytes_eq(&a->channels[i], &b->channels[i])) {
            return false;
        }
    }
    return true;
}

pc_status pc_psd_pattern_clone(const pc_psd_pattern *p, pc_psd_pattern *out, pc_error *err)
{
    pc_psd_pattern c;
    size_t i;
    pc_status st;
    memset(&c, 0, sizeof c);
    c.mode = p->mode;
    c.width = p->width;
    c.height = p->height;
    c.depth = p->depth;
    CK(pc_psd_str_clone(&p->name, &c.name, err));
    CK(pc_psd_str_clone(&p->id, &c.id, err));
    CK(pc_psd_opt_bytes_clone(&p->palette, &c.palette, err));
    CK(pc_psd_opt_bytes_clone(&p->alpha, &c.alpha, err));
    if (p->nchannels > 0) {
        c.channels = pc_calloc_array(p->nchannels, sizeof *c.channels);
        if (c.channels == NULL) {
            st = psd_nomem(err);
            goto fail;
        }
    }
    for (i = 0; i < p->nchannels; i++) {
        CK(pc_psd_bytes_clone(&p->channels[i], &c.channels[i], err));
        c.nchannels++;
    }
    *out = c;
    return PC_OK;
fail:
    pc_psd_pattern_fini(&c);
    return st;
}

/* ---- placing a channel ---- */

static pc_status outside(pc_error *err)
{
    return psd_invalid(err, "pattern channel rectangle falls outside pattern rectangle");
}

pc_status pc_psd_pattern_place_plane(pc_psd_bytes *plane, const pc_psd_layout *src, size_t x, size_t y,
                                     const pc_psd_layout *dst, pc_error *err)
{
    size_t x_end;
    size_t y_end;
    size_t src_len;
    size_t dst_len;
    size_t srb;
    size_t drb;
    size_t row;
    size_t col;
    uint8_t *out;
    pc_status st;
    if (src->planes != 1 || dst->planes != 1 || src->depth != dst->depth) {
        return psd_invalid(err, "incompatible pattern channel layouts");
    }
    if (!pc_size_add(x, src->width, &x_end)) {
        return psd_limit(err, "pattern channel x offset overflow");
    }
    if (!pc_size_add(y, src->height, &y_end)) {
        return psd_limit(err, "pattern channel y offset overflow");
    }
    if (x_end > dst->width || y_end > dst->height) {
        return outside(err);
    }
    if ((st = pc_psd_layout_decoded_len(src, &src_len, err)) != PC_OK) {
        return st;
    }
    if (plane->len != src_len) {
        return psd_invalid(err, "pattern channel payload does not match its rectangle");
    }
    if ((st = pc_psd_layout_decoded_len(dst, &dst_len, err)) != PC_OK) {
        return st;
    }
    srb = pc_psd_layout_row_bytes(src);
    drb = pc_psd_layout_row_bytes(dst);
    if (x == 0 && y == 0 && src->width == dst->width && src->height == dst->height) {
        /* Keep the plane; only the last byte of each non-byte-aligned 1-bit row changes. */
        if (src->depth == 1 && src->width % 8 != 0 && srb > 0) {
            uint8_t mask = (uint8_t)(0xFFu << (8 - src->width % 8));
            for (row = 0; row + srb <= plane->len; row += srb) {
                plane->data[row + srb - 1] &= mask;
            }
        }
        return PC_OK;
    }
    if (dst_len == 0) {
        /* An empty destination: the source fits inside it, so it is empty too. */
        pc_psd_bytes_fini(plane);
        return PC_OK;
    }
    out = calloc(dst_len, 1);
    if (out == NULL) {
        return psd_limit(err, "not enough memory for the padded pattern channel");
    }
    if (src->depth == 1) {
        for (row = 0; row < src->height; row++) {
            for (col = 0; col < src->width; col++) {
                size_t sp = row * srb + col / 8; /* < src_len */
                size_t dc = x + col;
                size_t dp = (y + row) * drb + dc / 8; /* < dst_len: x_end, y_end checked */
                uint8_t v = (uint8_t)(((unsigned)plane->data[sp] >> (7 - col % 8)) & 1u);
                out[dp] = (uint8_t)(out[dp] | (v << (7 - dc % 8)));
            }
        }
    } else {
        size_t bps = (size_t)(src->depth / 8);
        size_t rowb = src->width * bps; /* <= srb */
        for (row = 0; row < src->height; row++) {
            if (rowb > 0) {
                memcpy(out + (y + row) * drb + x * bps, plane->data + row * srb, rowb);
            }
        }
    }
    pc_psd_bytes_fini(plane);
    plane->data = out;
    plane->len = dst_len;
    return PC_OK;
}

/* ---- reading ---- */

typedef struct chan {
    const uint8_t *data;
    size_t len;
    uint16_t compression;
    pc_psd_layout source;
    pc_psd_layout dest;
    size_t x;
    size_t y;
} chan;

static pc_status read_rect(psd_rd *r, int32_t v[4], pc_error *err)
{
    int i;
    pc_status st = PC_OK;
    for (i = 0; i < 4 && st == PC_OK; i++) {
        st = psd_i32(r, &v[i], err);
    }
    return st;
}

static pc_status read_pattern(psd_rd *r, pc_psd_pattern *out, pc_error *err)
{
    pc_psd_pattern p;
    uint32_t version;
    uint16_t h16;
    uint16_t w16;
    uint32_t n;
    const uint8_t *raw;
    uint8_t idl;
    uint32_t vver;
    uint32_t len;
    psd_rd v;
    int32_t rect[4];
    int64_t w;
    int64_t h;
    uint32_t count;
    uint32_t k;
    chan chans[66];
    size_t nch = 0;
    uint64_t decoded = 0;
    uint64_t padded = 0;
    size_t nc;
    size_t i;
    pc_psd_bytes *planes = NULL;
    size_t nplanes = 0;
    pc_status st;
    memset(&p, 0, sizeof p);
    CK(psd_u32(r, &version, err));
    if (version != 1) {
        return psd_invalid(err, "pattern version %u", (unsigned)version);
    }
    CK(psd_u32(r, &p.mode, err));
    CK(psd_u16(r, &h16, err));
    CK(psd_u16(r, &w16, err));
    CK(psd_u32(r, &n, err));
    if (n > 65536) {
        return psd_limit(err, "pattern name length");
    }
    CK(psd_bytes(r, (size_t)n * 2, &raw, err));
    {
        uint16_t *units = n > 0 ? pc_malloc_array(n, sizeof *units) : NULL;
        size_t end = n;
        if (n > 0 && units == NULL) {
            st = psd_nomem(err);
            goto fail;
        }
        for (i = 0; i < n; i++) {
            units[i] = (uint16_t)((raw[2 * i] << 8) | raw[2 * i + 1]);
        }
        while (end > 0 && units[end - 1] == 0) {
            end--;
        }
        st = psd_str_from_units(units, end, &p.name, err);
        free(units);
        if (st != PC_OK) {
            goto fail;
        }
    }
    CK(psd_u8(r, &idl, err));
    CK(psd_bytes(r, idl, &raw, err));
    {
        pc_buf b = PC_BUF_INIT;
        st = pc_utf8_lossy(raw, idl, &b, err);
        if (st == PC_OK) {
            st = pc_psd_str_set(&p.id, (const char *)b.data, b.len, err);
        }
        pc_buf_fini(&b);
        if (st != PC_OK) {
            goto fail;
        }
    }
    if (p.mode == 2) {
        static const uint8_t three[4] = {0, 0, 0, 3};
        CK(psd_take_bytes(r, 768, &p.palette.v, err));
        p.palette.has = true;
        /* Some writers follow the palette with 4 extra bytes (ag-psd skips them). */
        if (psd_remaining(r) >= 4 && memcmp(psd_rest(r), three, 4) != 0) {
            CK(psd_skip(r, 4, err));
        }
    }
    CK(psd_u32(r, &vver, err));
    if (vver != 3) {
        st = psd_invalid(err, "virtual memory array list version %u", (unsigned)vver);
        goto fail;
    }
    CK(psd_u32(r, &len, err));
    CK(psd_sub(r, len, &v, err));
    CK(read_rect(&v, rect, err));
    w = (int64_t)rect[3] - rect[1];
    if (w > INT32_MAX || w < INT32_MIN) {
        st = psd_limit(err, "pattern width overflow");
        goto fail;
    }
    h = (int64_t)rect[2] - rect[0];
    if (h > INT32_MAX || h < INT32_MIN) {
        st = psd_limit(err, "pattern height overflow");
        goto fail;
    }
    if (w <= 0 || h <= 0 || w > PC_PSD_PATTERN_MAX_EDGE || h > PC_PSD_PATTERN_MAX_EDGE) {
        st = psd_limit(err, "pattern size");
        goto fail;
    }
    p.width = (uint32_t)w;
    p.height = (uint32_t)h;
    CK(psd_u32(&v, &count, err));
    if (count > 64) {
        st = psd_limit(err, "pattern channel count");
        goto fail;
    }
    /* Validate every channel and the total padded size before decoding any plane. */
    p.depth = 8;
    for (k = 0; k < count + 2; k++) {
        uint32_t written;
        uint32_t alen;
        uint32_t d32;
        int32_t cr[4];
        uint16_t d16;
        uint8_t comp;
        uint16_t d;
        uint64_t channel_bytes;
        uint64_t plane_bytes;
        size_t ignored;
        chan c;
        if (psd_empty(&v)) {
            break;
        }
        CK(psd_u32(&v, &written, err));
        if (written == 0) {
            continue;
        }
        CK(psd_u32(&v, &alen, err));
        if (alen == 0) {
            continue;
        }
        if (alen < 23) {
            st = psd_invalid(err, "short virtual memory array");
            goto fail;
        }
        CK(psd_u32(&v, &d32, err));
        CK(read_rect(&v, cr, err));
        CK(psd_u16(&v, &d16, err));
        CK(psd_u8(&v, &comp, err));
        CK(psd_bytes(&v, alen - 23, &c.data, err));
        c.len = alen - 23;
        d = d16 == 1 || d16 == 8 || d16 == 16 || d16 == 32 ? d16 : (uint16_t)d32;
        if (!(d == 1 || d == 8 || d == 16 || d == 32)) {
            st = psd_invalid(err, "pattern depth %u", (unsigned)d);
            goto fail;
        }
        p.depth = d;
        /* cr: top, left, bottom, right */
        if (cr[0] < rect[0] || cr[1] < rect[1] || cr[2] > rect[2] || cr[3] > rect[3] || cr[2] < cr[0] ||
            cr[3] < cr[1]) {
            st = outside(err);
            goto fail;
        }
        /* Inside a rectangle whose size fits in i32, so these neither overflow nor go negative. */
        c.source.planes = 1;
        c.source.width = (size_t)((int64_t)cr[3] - cr[1]);
        c.source.height = (size_t)((int64_t)cr[2] - cr[0]);
        c.source.depth = d;
        c.source.version = PC_PSD_V1;
        CK(pc_psd_layout_total_bytes(&c.source, &channel_bytes, err));
        c.dest = c.source;
        c.dest.width = p.width;
        c.dest.height = p.height;
        CK(pc_psd_layout_total_bytes(&c.dest, &plane_bytes, err));
        if (decoded > UINT64_MAX - plane_bytes) {
            st = psd_limit(err, "pattern decoded size overflow");
            goto fail;
        }
        decoded += plane_bytes;
        if (decoded > PC_PSD_MAX_DECODED_BYTES) {
            st = psd_limit(err, "pattern data exceeds MAX_DECODED_BYTES");
            goto fail;
        }
        if (c.source.width != p.width || c.source.height != p.height) {
            /* Padded channels (smaller than the pattern rectangle) have their own budget. */
            if (padded > UINT64_MAX - plane_bytes) {
                st = psd_limit(err, "pattern padded size overflow");
                goto fail;
            }
            padded += plane_bytes;
            if (padded > MAX_PADDED_PATTERN_BYTES) {
                st = psd_limit(err, "padded pattern channels exceed MAX_PADDED_PATTERN_BYTES");
                goto fail;
            }
        }
        CK(pc_psd_layout_decoded_len(&c.dest, &ignored, err));
        c.compression = comp;
        if (comp == PC_PSD_RAW || comp == PC_PSD_RLE) {
            uint64_t limit = (uint64_t)c.len * (comp == PC_PSD_RAW ? 1u : MAX_RLE_EXPANSION);
            if (channel_bytes > limit) {
                st = psd_limit(err, "pattern channel exceeds its input-derived size limit");
                goto fail;
            }
        }
        c.x = (size_t)((int64_t)cr[1] - rect[1]);
        c.y = (size_t)((int64_t)cr[0] - rect[0]);
        chans[nch++] = c;
    }
    nc = pc_psd_pattern_mode_channels(p.mode);
    if (nch < nc) {
        st = psd_invalid(err, "pattern has fewer channels than its colour mode");
        goto fail;
    }
    planes = pc_calloc_array(nch, sizeof *planes);
    if (planes == NULL) {
        st = psd_nomem(err);
        goto fail;
    }
    for (i = 0; i < nch; i++) {
        CK(pc_psd_decode_planes(chans[i].compression, chans[i].data, chans[i].len, &chans[i].source, &planes[i],
                                err));
        nplanes++;
        CK(pc_psd_pattern_place_plane(&planes[i], &chans[i].source, chans[i].x, chans[i].y, &chans[i].dest, err));
    }
    if (nplanes > nc) {
        p.alpha.has = true;
        p.alpha.v = planes[nc];
        planes[nc].data = NULL;
        planes[nc].len = 0;
    }
    for (i = nc; i < nplanes; i++) {
        pc_psd_bytes_fini(&planes[i]);
    }
    p.channels = planes;
    p.nchannels = nc;
    *out = p;
    return PC_OK;
fail:
    for (i = 0; i < nplanes; i++) {
        pc_psd_bytes_fini(&planes[i]);
    }
    free(planes);
    pc_psd_pattern_fini(&p);
    return st;
}

typedef struct pat_list {
    pc_psd_pattern *v;
    size_t n;
    size_t cap;
} pat_list;

static pc_status push(pat_list *l, pc_psd_pattern *p, pc_error *err)
{
    pc_psd_pattern *g = pc_grow_array(l->v, &l->cap, l->n + 1, sizeof *g);
    if (g == NULL) {
        pc_psd_pattern_fini(p);
        return psd_nomem(err);
    }
    l->v = g;
    l->v[l->n++] = *p;
    return PC_OK;
}

pc_status pc_psd_parse_pattern_block(const uint8_t *data, size_t len, pc_psd_pattern **out, size_t *n,
                                     pc_error *err)
{
    psd_rd r = psd_rd_make(data, len);
    pat_list l = {NULL, 0, 0};
    pc_status st = PC_OK;
    while (r.pos + 4 <= len) {
        uint32_t plen;
        psd_rd body;
        pc_psd_pattern p;
        size_t pad;
        CK(psd_u32(&r, &plen, err));
        if (plen == 0) {
            break;
        }
        CK(psd_sub(&r, plen, &body, err));
        CK(read_pattern(&body, &p, err));
        CK(push(&l, &p, err));
        pad = (4 - plen % 4) % 4;
        r.pos = r.pos + pad < len ? r.pos + pad : len;
    }
    *out = l.v;
    *n = l.n;
    return PC_OK;
fail:
    pc_psd_patterns_free(l.v, l.n);
    return st;
}

pc_status pc_psd_parse_pat_file(const uint8_t *data, size_t len, pc_psd_pattern **out, size_t *n, pc_error *err)
{
    psd_rd r = psd_rd_make(data, len);
    pat_list l = {NULL, 0, 0};
    uint8_t sig[4];
    uint16_t version;
    uint32_t count;
    uint32_t i;
    pc_status st;
    CK(psd_arr4(&r, sig, err));
    if (memcmp(sig, "8BPT", 4) != 0) {
        return psd_bad_sig(err, "8BPT", sig);
    }
    CK(psd_u16(&r, &version, err));
    CK(psd_u32(&r, &count, err));
    for (i = 0; i < count && i < 100000; i++) {
        pc_psd_pattern p;
        CK(read_pattern(&r, &p, err));
        CK(push(&l, &p, err));
    }
    *out = l.v;
    *n = l.n;
    return PC_OK;
fail:
    pc_psd_patterns_free(l.v, l.n);
    return st;
}

/* ---- writing ---- */

static void put_rect(psd_w *w, const int32_t rect[4])
{
    int i;
    for (i = 0; i < 4; i++) {
        psd_put_i32(w, rect[i]);
    }
}

static void put_array(psd_w *w, const pc_psd_pattern *p, const pc_psd_bytes *plane, const int32_t rect[4],
                      const pc_psd_layout *l)
{
    pc_psd_bytes enc = {NULL, 0};
    uint16_t comp = p->depth == 8 ? PC_PSD_RLE : PC_PSD_RAW;
    pc_status st;
    if (!psd_w_ok(w)) {
        return;
    }
    st = pc_psd_encode_planes(comp, plane->data, plane->len, l, &enc, w->err);
    if (st != PC_OK) {
        psd_w_fail(w, st);
        return;
    }
    psd_put_u32(w, 1);
    psd_put_u32(w, (uint32_t)(23 + enc.len));
    psd_put_u32(w, p->depth);
    put_rect(w, rect);
    psd_put_u16(w, p->depth);
    psd_put_u8(w, (uint8_t)comp);
    psd_put(w, enc.data, enc.len);
    pc_psd_bytes_fini(&enc);
}

static void write_pattern(const pc_psd_pattern *p, psd_w *out)
{
    psd_w body;
    pc_psd_bytes bb = {NULL, 0};
    uint16_t *u = NULL;
    size_t nu = 0;
    size_t i;
    size_t nc;
    int32_t rect[4];
    pc_psd_layout layout;
    pc_status st;
    if (!psd_w_ok(out)) {
        return;
    }
    if (p->width > PC_PSD_PATTERN_MAX_EDGE || p->height > PC_PSD_PATTERN_MAX_EDGE || p->width > INT16_MAX ||
        p->height > INT16_MAX) {
        psd_w_fail(out, psd_limit(out->err, "pattern size"));
        return;
    }
    psd_put_u32(out, 1);
    psd_put_u32(out, p->mode);
    psd_put_u16(out, (uint16_t)p->height);
    psd_put_u16(out, (uint16_t)p->width);
    st = psd_utf8n_to_units(pc_psd_str_c(&p->name), p->name.len, &u, &nu, out->err);
    if (st != PC_OK) {
        psd_w_fail(out, st);
        return;
    }
    psd_put_u32(out, (uint32_t)(nu + 1));
    for (i = 0; i < nu; i++) {
        psd_put_u16(out, u[i]);
    }
    psd_put_u16(out, 0);
    free(u);
    {
        size_t idl = p->id.len < 255 ? p->id.len : 255;
        psd_put_u8(out, (uint8_t)idl);
        psd_put(out, p->id.s, idl);
    }
    if (p->mode == 2) {
        size_t have = p->palette.has ? p->palette.v.len : 0;
        if (have > 768) {
            have = 768;
        }
        psd_put(out, p->palette.v.data, have);
        psd_put_zeros(out, 768 - have);
    }
    rect[0] = 0;
    rect[1] = 0;
    rect[2] = (int32_t)p->height;
    rect[3] = (int32_t)p->width;
    layout.planes = 1;
    layout.width = p->width;
    layout.height = p->height;
    layout.depth = p->depth;
    layout.version = PC_PSD_V1;
    psd_w_init(&body, out->err);
    put_rect(&body, rect);
    psd_put_u32(&body, SLOTS);
    nc = p->nchannels < SLOTS ? p->nchannels : SLOTS;
    for (i = 0; i < nc; i++) {
        put_array(&body, p, &p->channels[i], rect, &layout);
    }
    for (i = nc; i < SLOTS; i++) {
        psd_put_u32(&body, 0);
    }
    /* User mask slot: transparency; sheet mask slot: unused. */
    if (p->alpha.has) {
        put_array(&body, p, &p->alpha.v, rect, &layout);
    } else {
        psd_put_u32(&body, 0);
    }
    psd_put_u32(&body, 0);
    st = psd_w_finish(&body, &bb);
    if (st != PC_OK) {
        psd_w_fail(out, st);
        return;
    }
    psd_put_u32(out, 3);
    psd_put_u32(out, (uint32_t)bb.len);
    psd_put(out, bb.data, bb.len);
    pc_psd_bytes_fini(&bb);
}

pc_status pc_psd_write_pattern_block(const pc_psd_pattern *p, size_t n, pc_psd_bytes *out, pc_error *err)
{
    psd_w w;
    size_t i;
    psd_w_init(&w, err);
    for (i = 0; i < n && psd_w_ok(&w); i++) {
        size_t at = psd_begin_len(&w, false);
        size_t start = w.b.len;
        size_t one;
        write_pattern(&p[i], &w);
        if (!psd_w_ok(&w)) {
            break;
        }
        one = w.b.len - start;
        psd_end_len(&w, at, false);
        psd_put_zeros(&w, (4 - one % 4) % 4);
    }
    return psd_w_finish(&w, out);
}

void pc_psd_pattern_block_key(uint16_t depth, uint8_t out[4])
{
    memcpy(out, depth == 16 ? "Pat2" : depth == 32 ? "Pat3" : "Patt", 4);
}

pc_status pc_psd_write_pat_file(const pc_psd_pattern *p, size_t n, pc_psd_bytes *out, pc_error *err)
{
    psd_w w;
    size_t i;
    psd_w_init(&w, err);
    psd_put(&w, "8BPT", 4);
    psd_put_u16(&w, 1);
    psd_put_u32(&w, (uint32_t)n);
    for (i = 0; i < n; i++) {
        write_pattern(&p[i], &w);
    }
    return psd_w_finish(&w, out);
}
