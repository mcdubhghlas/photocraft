#include "psd/filter_effects.h"

#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/mem.h"
#include "base/utf8.h"
#include "psd/compression.h"
#include "psd/internal.h"

static pc_psd_layout plane_layout(size_t w, size_t h, uint16_t depth)
{
    pc_psd_layout l;
    l.planes = 1;
    l.width = w;
    l.height = h;
    l.depth = depth;
    l.version = PC_PSD_V2;
    return l;
}

pc_status pc_psd_fx_plane_encode(uint16_t compression, const uint8_t *decoded, size_t len, size_t w, size_t h,
                                 uint16_t depth, pc_psd_fx_plane *out, pc_error *err)
{
    pc_psd_layout l = plane_layout(w, h, depth);
    pc_psd_bytes d = {NULL, 0};
    pc_status st = pc_psd_encode_planes(compression, decoded, len, &l, &d, err);
    if (st == PC_OK) {
        out->compression = compression;
        out->data = d;
    }
    return st;
}

pc_status pc_psd_fx_plane_decode(const pc_psd_fx_plane *p, size_t w, size_t h, uint16_t depth, pc_psd_bytes *out,
                                 pc_error *err)
{
    pc_psd_layout l = plane_layout(w, h, depth);
    uint64_t n = p->data.len;
    uint64_t most;
    uint64_t total;
    pc_status st;
    if (p->compression == PC_PSD_RAW) {
        most = n;
    } else if (p->compression == PC_PSD_RLE) {
        most = n > UINT64_MAX / 64 ? UINT64_MAX : n * 64;
    } else {
        most = n > (UINT64_MAX - 64) / 1032 ? UINT64_MAX : n * 1032 + 64;
    }
    st = pc_psd_layout_total_bytes(&l, &total, err);
    if (st != PC_OK) {
        return st;
    }
    if (total > most) {
        return psd_invalid(err, "filter effects: plane larger than its data can hold");
    }
    return pc_psd_decode_planes(p->compression, p->data.data, p->data.len, &l, out, err);
}

void pc_psd_fx_item_fini(pc_psd_fx_item *it)
{
    size_t i;
    if (it == NULL) {
        return;
    }
    free(it->id);
    for (i = 0; i < it->nslots; i++) {
        pc_psd_bytes_fini(&it->slots[i].data);
    }
    free(it->slots);
    free(it->slot_written);
    pc_psd_bytes_fini(&it->mask.data);
    memset(it, 0, sizeof *it);
}

void pc_psd_filter_effects_fini(pc_psd_filter_effects *fx)
{
    size_t i;
    if (fx == NULL) {
        return;
    }
    for (i = 0; i < fx->n; i++) {
        pc_psd_fx_item_fini(&fx->items[i]);
    }
    free(fx->items);
    memset(fx, 0, sizeof *fx);
}

static bool plane_eq(const pc_psd_fx_plane *a, const pc_psd_fx_plane *b)
{
    return a->compression == b->compression && pc_psd_bytes_eq(&a->data, &b->data);
}

bool pc_psd_fx_item_eq(const pc_psd_fx_item *a, const pc_psd_fx_item *b)
{
    size_t i;
    if (strcmp(a->id, b->id) != 0 || a->version != b->version || !pc_psd_rect_eq(a->rect, b->rect) ||
        a->depth != b->depth || a->max_channels != b->max_channels || a->nslots != b->nslots ||
        a->has_mask != b->has_mask) {
        return false;
    }
    for (i = 0; i < a->nslots; i++) {
        if (a->slot_written[i] != b->slot_written[i] ||
            (a->slot_written[i] && !plane_eq(&a->slots[i], &b->slots[i]))) {
            return false;
        }
    }
    return !a->has_mask || (pc_psd_rect_eq(a->mask_rect, b->mask_rect) && plane_eq(&a->mask, &b->mask));
}

static pc_status read_rect(psd_rd *r, pc_psd_rect *out, pc_error *err)
{
    pc_status st;
    CK(psd_i32(r, &out->top, err));
    CK(psd_i32(r, &out->left, err));
    CK(psd_i32(r, &out->bottom, err));
    CK(psd_i32(r, &out->right, err));
    return PC_OK;
fail:
    return st;
}

static void write_rect(psd_w *w, pc_psd_rect r)
{
    psd_put_i32(w, r.top);
    psd_put_i32(w, r.left);
    psd_put_i32(w, r.bottom);
    psd_put_i32(w, r.right);
}

static pc_status read_plane(psd_rd *r, pc_psd_fx_plane *out, pc_error *err)
{
    uint64_t len;
    psd_rd sub;
    pc_status st = psd_u64(r, &len, err);
    if (st != PC_OK) {
        return st;
    }
    if (len < 2) {
        return psd_invalid(err, "filter effects: plane shorter than its compression field");
    }
    st = psd_sub(r, len, &sub, err);
    if (st == PC_OK) {
        st = psd_u16(&sub, &out->compression, err);
    }
    if (st == PC_OK) {
        st = pc_psd_bytes_set(&out->data, psd_rest(&sub), psd_remaining(&sub), err);
    }
    return st;
}

static void write_plane(psd_w *w, const pc_psd_fx_plane *p)
{
    psd_put_u64(w, (uint64_t)p->data.len + 2);
    psd_put_u16(w, p->compression);
    psd_put(w, p->data.data, p->data.len);
}

static pc_status read_item(psd_rd *body, pc_psd_fx_item *it, pc_error *err)
{
    pc_psd_bytes idb = {NULL, 0};
    pc_buf idu = PC_BUF_INIT;
    uint64_t item_len;
    psd_rd sub;
    uint32_t k;
    pc_status st;
    memset(it, 0, sizeof *it);
    CK(psd_read_pascal(body, 1, &idb, err));
    CK(pc_utf8_lossy(idb.data, idb.len, &idu, err));
    CK(pc_buf_append_byte(&idu, 0, err));
    it->id = (char *)idu.data;
    pc_buf_init(&idu);
    CK(psd_u32(body, &it->version, err));
    CK(psd_u64(body, &item_len, err));
    CK(psd_sub(body, item_len, &sub, err));
    CK(read_rect(&sub, &it->rect, err));
    CK(psd_u32(&sub, &it->depth, err));
    CK(psd_u32(&sub, &it->max_channels, err));
    if (it->max_channels > PC_PSD_FX_MAX_SLOTS) {
        st = psd_limit(err, "filter effects: too many channels");
        goto fail;
    }
    it->slots = pc_calloc_array(it->max_channels + 2u, sizeof *it->slots);
    it->slot_written = pc_calloc_array(it->max_channels + 2u, sizeof *it->slot_written);
    if (it->slots == NULL || it->slot_written == NULL) {
        st = psd_nomem(err);
        goto fail;
    }
    it->nslots = it->max_channels + 2u;
    for (k = 0; k < it->max_channels + 2u; k++) {
        uint32_t flag;
        CK(psd_u32(&sub, &flag, err));
        if (flag != 0) {
            CK(read_plane(&sub, &it->slots[k], err));
            it->slot_written[k] = true;
        }
    }
    if (!psd_empty(&sub)) {
        st = psd_invalid(err, "filter effects item %s: %zu unexpected bytes", it->id, psd_remaining(&sub));
        goto fail;
    }
    if (!psd_empty(body)) {
        uint8_t flag;
        CK(psd_u8(body, &flag, err));
        if (flag != 0) {
            CK(read_rect(body, &it->mask_rect, err));
            CK(read_plane(body, &it->mask, err));
            it->has_mask = true;
        }
    }
    pc_psd_bytes_fini(&idb);
    return PC_OK;
fail:
    pc_psd_bytes_fini(&idb);
    pc_buf_fini(&idu);
    pc_psd_fx_item_fini(it);
    return st;
}

pc_status pc_psd_filter_effects_parse(const uint8_t *data, size_t len, pc_psd_filter_effects *out, pc_error *err)
{
    psd_rd r = psd_rd_make(data, len);
    pc_psd_filter_effects fx;
    size_t cap = 0;
    pc_status st;
    memset(&fx, 0, sizeof fx);
    CK(psd_u32(&r, &fx.version, err));
    if (fx.version < 1 || fx.version > 3) {
        st = psd_invalid(err, "filter effects version %u", (unsigned)fx.version);
        goto fail;
    }
    for (;;) {
        uint64_t slen;
        psd_rd body;
        uint64_t pad;
        const uint8_t *rest;
        size_t nrest;
        size_t i;
        bool zeros = true;
        CK(psd_u64(&r, &slen, err));
        CK(psd_sub(&r, slen, &body, err));
        while (!psd_empty(&body)) {
            pc_psd_fx_item it;
            if (fx.n == cap) {
                pc_psd_fx_item *g = pc_grow_array(fx.items, &cap, fx.n + 1, sizeof *g);
                if (g == NULL) {
                    st = psd_nomem(err);
                    goto fail;
                }
                fx.items = g;
            }
            CK(read_item(&body, &it, err));
            fx.items[fx.n++] = it;
        }
        /* Sections are padded to 4 bytes; the last one's padding may be cut short. */
        pad = (4 - slen % 4) % 4;
        if (pad > psd_remaining(&r)) {
            pad = psd_remaining(&r);
        }
        CK(psd_skip(&r, (size_t)pad, err));
        rest = psd_rest(&r);
        nrest = psd_remaining(&r);
        for (i = 0; i < nrest; i++) {
            zeros = zeros && rest[i] == 0;
        }
        if (zeros && nrest <= 3) {
            break;
        }
        if (nrest < 8) {
            st = psd_invalid(err, "filter effects: %zu unexpected bytes after the data", nrest);
            goto fail;
        }
    }
    *out = fx;
    return PC_OK;
fail:
    pc_psd_filter_effects_fini(&fx);
    return st;
}

pc_status pc_psd_filter_effects_to_bytes(const pc_psd_filter_effects *fx, pc_psd_bytes *out, pc_error *err)
{
    psd_w w;
    psd_w body;
    size_t i;
    psd_w_init(&w, err);
    psd_put_u32(&w, fx->version);
    for (i = 0; i < fx->n; i++) {
        const pc_psd_fx_item *it = &fx->items[i];
        size_t at;
        size_t k;
        psd_w_init(&body, err);
        psd_write_pascal(&body, (const uint8_t *)it->id, strlen(it->id), 1);
        psd_put_u32(&body, it->version);
        at = psd_begin_len(&body, true);
        write_rect(&body, it->rect);
        psd_put_u32(&body, it->depth);
        psd_put_u32(&body, it->max_channels);
        for (k = 0; k < (size_t)it->max_channels + 2; k++) {
            if (k < it->nslots && it->slot_written[k]) {
                psd_put_u32(&body, 1);
                write_plane(&body, &it->slots[k]);
            } else {
                psd_put_u32(&body, 0);
            }
        }
        psd_end_len(&body, at, true);
        if (it->has_mask) {
            psd_put_u8(&body, 1);
            write_rect(&body, it->mask_rect);
            write_plane(&body, &it->mask);
        } else {
            psd_put_u8(&body, 0);
        }
        if (body.st != PC_OK) {
            psd_w_fail(&w, body.st);
        }
        psd_put_u64(&w, body.b.len);
        psd_put(&w, body.b.data, body.b.len);
        psd_put_zeros(&w, (4 - w.b.len % 4) % 4);
        psd_w_fini(&body);
    }
    if (fx->n == 0) {
        psd_put_u64(&w, 0); /* one empty section, so the block still parses */
    }
    return psd_w_finish(&w, out);
}
