#include "psd/tagged.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/mem.h"
#include "base/utf8.h"
#include "psd/descriptor.h"
#include "psd/filter_effects.h"
#include "psd/internal.h"

const char PC_PSD_LONG_KEYS[PC_PSD_LONG_KEY_COUNT][5] = {
    "LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn", "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD", /* spec */
    "lnk3", "lnkE", "pths", "extd", "extn", "FELS", "cinf", "artd"};                                     /* observed */

bool pc_psd_uses_long_length(pc_psd_version v, const uint8_t key[4])
{
    int i;
    if (v != PC_PSD_V2) {
        return false;
    }
    for (i = 0; i < PC_PSD_LONG_KEY_COUNT; i++) {
        if (memcmp(PC_PSD_LONG_KEYS[i], key, 4) == 0) {
            return true;
        }
    }
    return false;
}

bool pc_psd_is_foreign_order_signature(const uint8_t *b, size_t len)
{
    return len >= 4 && (memcmp(b, "MIB8", 4) == 0 || memcmp(b, "46B8", 4) == 0);
}

static bool is_sig(const uint8_t *b, size_t len)
{
    return len >= 4 && (memcmp(b, "8BIM", 4) == 0 || memcmp(b, "8B64", 4) == 0 || pc_psd_is_foreign_order_signature(b, len));
}

pc_status pc_psd_block_init(pc_psd_block *b, const char key[4], const void *data, size_t len, pc_error *err)
{
    memset(b, 0, sizeof *b);
    memcpy(b->signature, "8BIM", 4);
    memcpy(b->key, key, 4);
    return pc_psd_bytes_set(&b->data, data, len, err);
}

void pc_psd_block_fini(pc_psd_block *b)
{
    if (b != NULL) {
        pc_psd_bytes_fini(&b->data);
        pc_psd_opt_bytes_fini(&b->padding);
    }
}

pc_status pc_psd_block_clone(const pc_psd_block *b, pc_psd_block *out, pc_error *err)
{
    pc_psd_block c;
    pc_status st;
    memset(&c, 0, sizeof c);
    memcpy(c.signature, b->signature, 4);
    memcpy(c.key, b->key, 4);
    st = pc_psd_bytes_clone(&b->data, &c.data, err);
    if (st == PC_OK) {
        st = pc_psd_opt_bytes_clone(&b->padding, &c.padding, err);
    }
    if (st != PC_OK) {
        pc_psd_block_fini(&c);
        return st;
    }
    *out = c;
    return PC_OK;
}

bool pc_psd_block_eq(const pc_psd_block *a, const pc_psd_block *b)
{
    return memcmp(a->signature, b->signature, 4) == 0 && memcmp(a->key, b->key, 4) == 0 &&
           pc_psd_bytes_eq(&a->data, &b->data) && pc_psd_opt_bytes_eq(&a->padding, &b->padding);
}

bool pc_psd_block_is(const pc_psd_block *b, const char key[4])
{
    return memcmp(b->key, key, 4) == 0;
}

static size_t default_padding(size_t len)
{
    return len % 2;
}

size_t pc_psd_block_encoded_len(const pc_psd_block *b, pc_psd_version v)
{
    size_t l = pc_psd_uses_long_length(v, b->key) ? 8 : 4;
    size_t p = b->padding.has ? b->padding.v.len : default_padding(b->data.len);
    return 8 + l + b->data.len + p;
}

/* ---- typed views ---- */

static const char *const TYPED[] = {"luni", "lsct", "lsdk", "lyid", "lnsr", "clbl", "infx", "knko",
                                    "tsly", "lmgm", "vmgm", "lspf", "lclr", "iOpa", "shmd"};

bool pc_psd_block_has_typed(const pc_psd_block *b)
{
    size_t i;
    for (i = 0; i < sizeof TYPED / sizeof TYPED[0]; i++) {
        if (memcmp(b->key, TYPED[i], 4) == 0) {
            return true;
        }
    }
    return false;
}

static psd_rd block_rd(const pc_psd_block *b)
{
    return psd_rd_make(b->data.data, b->data.len);
}

pc_status pc_psd_block_unicode_name(const pc_psd_block *b, char **out, pc_error *err)
{
    psd_rd r = block_rd(b);
    pc_psd_ustr s = {NULL, 0};
    pc_status st = psd_read_ustr(&r, &s, err);
    if (st == PC_OK) {
        st = pc_psd_ustr_to_utf8(&s, out, err);
    }
    pc_psd_ustr_fini(&s);
    return st;
}

pc_status pc_psd_block_section(const pc_psd_block *b, pc_psd_section_divider *out, pc_error *err)
{
    psd_rd r = block_rd(b);
    pc_psd_section_divider s;
    uint8_t sig[4];
    pc_status st;
    memset(&s, 0, sizeof s);
    CK(psd_u32(&r, &s.kind, err));
    if (psd_remaining(&r) >= 8) {
        CK(psd_arr4(&r, sig, err));
        if (memcmp(sig, "8BIM", 4) != 0) {
            return psd_bad_sig(err, "8BIM", sig);
        }
        CK(psd_arr4(&r, s.blend, err));
        s.has_blend = true;
        if (psd_remaining(&r) >= 4) {
            CK(psd_u32(&r, &s.sub_type, err));
            s.has_sub_type = true;
        }
    }
    *out = s;
    return PC_OK;
fail:
    return st;
}

pc_status pc_psd_block_u32(const pc_psd_block *b, uint32_t *out, pc_error *err)
{
    psd_rd r = block_rd(b);
    return psd_u32(&r, out, err);
}

pc_status pc_psd_block_u16(const pc_psd_block *b, uint16_t *out, pc_error *err)
{
    psd_rd r = block_rd(b);
    return psd_u16(&r, out, err);
}

pc_status pc_psd_block_u8(const pc_psd_block *b, uint8_t *out, pc_error *err)
{
    psd_rd r = block_rd(b);
    return psd_u8(&r, out, err);
}

pc_status pc_psd_block_name_source(const pc_psd_block *b, uint8_t out[4], pc_error *err)
{
    psd_rd r = block_rd(b);
    return psd_arr4(&r, out, err);
}

pc_status pc_psd_block_check_typed(const pc_psd_block *b, pc_error *err)
{
    union {
        uint32_t u32;
        uint16_t u16;
        uint8_t u8;
        uint8_t a4[4];
        pc_psd_section_divider sd;
    } v;
    char *name = NULL;
    pc_status st;
    if (pc_psd_block_is(b, "luni")) {
        st = pc_psd_block_unicode_name(b, &name, err);
        free(name);
        return st;
    }
    if (pc_psd_block_is(b, "lsct") || pc_psd_block_is(b, "lsdk")) {
        return pc_psd_block_section(b, &v.sd, err);
    }
    if (pc_psd_block_is(b, "lyid") || pc_psd_block_is(b, "lspf")) {
        return pc_psd_block_u32(b, &v.u32, err);
    }
    if (pc_psd_block_is(b, "lnsr")) {
        return pc_psd_block_name_source(b, v.a4, err);
    }
    if (pc_psd_block_is(b, "lclr")) {
        return pc_psd_block_u16(b, &v.u16, err);
    }
    if (pc_psd_block_has_typed(b) && !pc_psd_block_is(b, "shmd")) {
        return pc_psd_block_u8(b, &v.u8, err);
    }
    return PC_OK;
}

/* ---- constructors ---- */

pc_status pc_psd_block_make_unicode_name(const char *utf8, pc_psd_block *out, pc_error *err)
{
    pc_psd_ustr s = {NULL, 0};
    psd_w w;
    pc_psd_bytes d = {NULL, 0};
    pc_status st = pc_psd_ustr_from_utf8(utf8, false, &s, err);
    if (st != PC_OK) {
        return st;
    }
    psd_w_init(&w, err);
    psd_write_ustr(&w, &s);
    psd_put_zeros(&w, (4 - w.b.len % 4) % 4);
    pc_psd_ustr_fini(&s);
    st = psd_w_finish(&w, &d);
    if (st != PC_OK) {
        return st;
    }
    memset(out, 0, sizeof *out);
    memcpy(out->signature, "8BIM", 4);
    memcpy(out->key, "luni", 4);
    out->data = d;
    return PC_OK;
}

pc_status pc_psd_block_make_section(uint32_t kind, const uint8_t *blend, const uint32_t *sub_type, pc_psd_block *out,
                                    pc_error *err)
{
    uint8_t d[16];
    size_t n = 4;
    pc_store_be32(d, kind);
    if (blend != NULL) {
        memcpy(d + 4, "8BIM", 4);
        memcpy(d + 8, blend, 4);
        n = 12;
        if (sub_type != NULL) {
            pc_store_be32(d + 12, *sub_type);
            n = 16;
        }
    }
    return pc_psd_block_init(out, "lsct", d, n, err);
}

pc_status pc_psd_block_make_u32(const char key[4], uint32_t v, pc_psd_block *out, pc_error *err)
{
    uint8_t d[4];
    pc_store_be32(d, v);
    return pc_psd_block_init(out, key, d, 4, err);
}

pc_status pc_psd_block_make_flag(const char key[4], uint8_t v, pc_psd_block *out, pc_error *err)
{
    uint8_t d[4] = {0, 0, 0, 0};
    d[0] = v;
    return pc_psd_block_init(out, key, d, 4, err);
}

pc_status pc_psd_block_make_name_source(const uint8_t src[4], pc_psd_block *out, pc_error *err)
{
    return pc_psd_block_init(out, "lnsr", src, 4, err);
}

pc_status pc_psd_block_make_sheet_color(uint16_t color, pc_psd_block *out, pc_error *err)
{
    uint8_t d[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    pc_store_be16(d, color);
    return pc_psd_block_init(out, "lclr", d, 8, err);
}

/* ---- strict structure checks ---- */

static void key_str(const pc_psd_block *b, char out[32])
{
    pc_buf s = PC_BUF_INIT;
    out[0] = '\0';
    if (pc_utf8_lossy(b->key, 4, &s, NULL) == PC_OK && s.len < 32) {
        memcpy(out, s.data, s.len);
        out[s.len] = '\0';
    }
    pc_buf_fini(&s);
}

static pc_status expect_kind(psd_rd *r, const char kind[4], pc_error *err)
{
    uint8_t found[4];
    char q[32];
    pc_status st = psd_arr4(r, found, err);
    if (st != PC_OK) {
        return st;
    }
    if (memcmp(found, kind, 4) != 0) {
        pc_utf8_debug_quote(found, 4, q, sizeof q);
        return psd_invalid(err, "expected %.4s structure, found %s", kind, q);
    }
    return PC_OK;
}

static bool is_kind(const pc_psd_value *v, pc_psd_vkind k)
{
    return v != NULL && v->kind == k;
}

/* soLD descriptor: the unique id and transform Photoshop needs, and a well-formed smart filter
 * stack (filterFX → filterFXList of filterFX objects with name, blend options, enab, filterID). */
static pc_status check_placed_descriptor(const pc_psd_desc *d, pc_error *err)
{
    const pc_psd_value *trnf;
    const pc_psd_value *fx;
    const pc_psd_value *list;
    size_t i;
    if (!is_kind(pc_psd_desc_get(d, "Idnt"), PC_PSD_V_TEXT)) {
        return psd_invalid(err, "no Idnt (unique id)");
    }
    trnf = pc_psd_desc_get(d, "Trnf");
    if (!is_kind(trnf, PC_PSD_V_LIST) || trnf->nlist != 8) {
        return psd_invalid(err, "Trnf is not eight doubles");
    }
    for (i = 0; i < 8; i++) {
        if (trnf->list[i].kind != PC_PSD_V_DOUBLE) {
            return psd_invalid(err, "Trnf is not eight doubles");
        }
    }
    fx = pc_psd_desc_get(d, "filterFX");
    if (fx == NULL) {
        return PC_OK;
    }
    if (fx->kind != PC_PSD_V_DESCRIPTOR) {
        return psd_invalid(err, "filterFX is not an object");
    }
    list = pc_psd_desc_get(fx->desc, "filterFXList");
    if (!is_kind(list, PC_PSD_V_LIST)) {
        return psd_invalid(err, "filterFX has no filterFXList");
    }
    for (i = 0; i < list->nlist; i++) {
        const pc_psd_desc *f;
        const pc_psd_value *fltr;
        bool ok;
        if (list->list[i].kind != PC_PSD_V_DESCRIPTOR) {
            return psd_invalid(err, "filter %zu is not an object", i);
        }
        f = list->list[i].desc;
        fltr = pc_psd_desc_get(f, "Fltr");
        ok = is_kind(pc_psd_desc_get(f, "Nm  "), PC_PSD_V_TEXT) &&
             is_kind(pc_psd_desc_get(f, "blendOptions"), PC_PSD_V_DESCRIPTOR) &&
             is_kind(pc_psd_desc_get(f, "enab"), PC_PSD_V_BOOLEAN) &&
             is_kind(pc_psd_desc_get(f, "filterID"), PC_PSD_V_INTEGER) &&
             (fltr == NULL || fltr->kind == PC_PSD_V_DESCRIPTOR);
        if (!ok) {
            return psd_invalid(err, "filter %zu lacks Nm/blendOptions/enab/filterID", i);
        }
    }
    return PC_OK;
}

/* One embedded (liFD) linked-layer item. */
static pc_status check_embedded_item(const uint8_t *item, size_t len, pc_error *err)
{
    psd_rd r = psd_rd_make(item, len);
    uint32_t version;
    pc_psd_bytes name = {NULL, 0};
    uint16_t *units = NULL;
    size_t nunits;
    uint64_t dlen;
    uint8_t has_desc;
    pc_psd_desc d;
    const uint8_t *p;
    pc_status st;
    CK(psd_skip(&r, 4, err));
    CK(psd_u32(&r, &version, err));
    if (version < 1 || version > 16) {
        st = psd_invalid(err, "version %u", (unsigned)version);
        goto fail;
    }
    CK(psd_read_pascal(&r, 1, &name, err));
    CK(psd_read_units(&r, &units, &nunits, err));
    CK(psd_skip(&r, 8, err)); /* file type, creator */
    CK(psd_u64(&r, &dlen, err));
    CK(psd_u8(&r, &has_desc, err));
    if (has_desc != 0) {
        CK(psd_read_vdesc(&r, &d, err));
        pc_psd_desc_fini(&d);
    }
    CK(psd_bytes_u64(&r, dlen, &p, NULL, err));
    st = PC_OK;
fail:
    pc_psd_bytes_fini(&name);
    free(units);
    return st;
}

pc_status pc_psd_block_check_structure(const pc_psd_block *b, pc_error *err)
{
    psd_rd r = block_rd(b);
    char key[32];
    pc_psd_bytes tmp = {NULL, 0};
    pc_psd_desc d;
    uint32_t u;
    const uint8_t *rest;
    size_t nrest;
    size_t i;
    pc_status st;
    key_str(b, key);
    if (pc_psd_block_is(b, "PlLd")) {
        CK(expect_kind(&r, "plcL", err));
        CK(psd_u32(&r, &u, err));
        CK(psd_read_pascal(&r, 1, &tmp, err));
        pc_psd_bytes_fini(&tmp);
        /* Page, total pages, anti-alias policy, layer type; transform (8 doubles). */
        CK(psd_skip(&r, 4 * 4 + 8 * 8, err));
        CK(psd_u32(&r, &u, err));
        CK(psd_read_vdesc(&r, &d, err));
        pc_psd_desc_fini(&d);
    } else if (pc_psd_block_is(b, "SoLd") || pc_psd_block_is(b, "SoLE")) {
        CK(expect_kind(&r, "soLD", err));
        CK(psd_u32(&r, &u, err));
        CK(psd_read_vdesc(&r, &d, err));
        st = check_placed_descriptor(&d, err);
        pc_psd_desc_fini(&d);
        if (st != PC_OK) {
            return psd_wrap_invalid(err, st, key);
        }
    } else if (pc_psd_block_is(b, "lfx2")) {
        CK(psd_u32(&r, &u, err));
        CK(psd_read_vdesc(&r, &d, err));
        pc_psd_desc_fini(&d);
    } else if (pc_psd_block_is(b, "lnk2") || pc_psd_block_is(b, "lnk3") || pc_psd_block_is(b, "lnkD")) {
        while (psd_remaining(&r) >= 8) {
            uint64_t len;
            const uint8_t *item;
            size_t ilen;
            uint64_t pad;
            CK(psd_u64(&r, &len, err));
            CK(psd_bytes_u64(&r, len, &item, &ilen, err));
            if (!(ilen >= 4 && (memcmp(item, "liFD", 4) == 0 || memcmp(item, "liFE", 4) == 0 ||
                                memcmp(item, "liFA", 4) == 0))) {
                return psd_invalid(err, "%s: linked item of unknown type", key);
            }
            if (memcmp(item, "liFD", 4) == 0) {
                st = check_embedded_item(item, ilen, err);
                if (st != PC_OK) {
                    char prefix[64];
                    (void)snprintf(prefix, sizeof prefix, "%s: embedded file", key);
                    return psd_wrap_invalid(err, st, prefix);
                }
            }
            /* Items are padded to a multiple of 4. */
            pad = (4 - len % 4) % 4;
            if (pad > psd_remaining(&r)) {
                pad = psd_remaining(&r);
            }
            CK(psd_skip(&r, (size_t)pad, err));
        }
    } else if (pc_psd_block_is(b, "FEid") || pc_psd_block_is(b, "FXid")) {
        pc_psd_filter_effects fx;
        st = pc_psd_filter_effects_parse(b->data.data, b->data.len, &fx, err);
        if (st != PC_OK) {
            return psd_wrap_invalid(err, st, key);
        }
        pc_psd_filter_effects_fini(&fx);
        return PC_OK;
    } else {
        return PC_OK;
    }
    rest = psd_rest(&r);
    nrest = psd_remaining(&r);
    for (i = 0; i < nrest && nrest <= 3; i++) {
        if (rest[i] != 0) {
            break;
        }
    }
    if (nrest > 3 || i < nrest) {
        return psd_invalid(err, "%s: %zu unexpected bytes after the block structure", key, nrest);
    }
    return PC_OK;
fail:
    pc_psd_bytes_fini(&tmp);
    return st;
}

/* ---- lists of blocks ---- */

void pc_psd_blocks_free(pc_psd_block *blocks, size_t n)
{
    size_t i;
    for (i = 0; blocks != NULL && i < n; i++) {
        pc_psd_block_fini(&blocks[i]);
    }
    free(blocks);
}

pc_status pc_psd_blocks_clone(const pc_psd_block *blocks, size_t n, pc_psd_block **out, pc_error *err)
{
    pc_psd_block *v = NULL;
    size_t i;
    if (n > 0) {
        v = pc_calloc_array(n, sizeof *v);
        if (v == NULL) {
            return psd_nomem(err);
        }
        for (i = 0; i < n; i++) {
            pc_status st = pc_psd_block_clone(&blocks[i], &v[i], err);
            if (st != PC_OK) {
                pc_psd_blocks_free(v, i);
                return st;
            }
        }
    }
    *out = v;
    return PC_OK;
}

bool pc_psd_blocks_eq(const pc_psd_block *a, size_t na, const pc_psd_block *b, size_t nb)
{
    size_t i;
    if (na != nb) {
        return false;
    }
    for (i = 0; i < na; i++) {
        if (!pc_psd_block_eq(&a[i], &b[i])) {
            return false;
        }
    }
    return true;
}

pc_status psd_read_blocks(psd_rd *r, pc_psd_version v, pc_psd_block **blocks, size_t *n, pc_psd_bytes *trailing,
                          pc_error *err)
{
    pc_psd_block *list = NULL;
    size_t count = 0;
    size_t cap = 0;
    pc_psd_block b;
    pc_status st;
    memset(&b, 0, sizeof b);
    for (;;) {
        const uint8_t *rest = psd_rest(r);
        size_t nrest = psd_remaining(r);
        uint64_t len;
        size_t pad = 0;
        size_t k;
        bool found = false;
        const uint8_t *pb;
        bool zeros = true;
        if (nrest < 12 || !is_sig(rest, nrest)) {
            pc_psd_bytes t = {NULL, 0};
            CK(pc_psd_bytes_set(&t, rest, nrest, err));
            r->pos = r->len;
            *blocks = list;
            *n = count;
            *trailing = t;
            return PC_OK;
        }
        memset(&b, 0, sizeof b);
        CK(psd_arr4(r, b.signature, err));
        CK(psd_arr4(r, b.key, err));
        CK(psd_len_field(r, pc_psd_uses_long_length(v, b.key), &len, err));
        CK(psd_take_bytes(r, len, &b.data, err));
        /* Padding: the smallest k in 0..=3 after which the next signature or the end follows. */
        rest = psd_rest(r);
        nrest = psd_remaining(r);
        for (k = 0; k <= 3 && !found; k++) {
            if (k == nrest || (k < nrest && is_sig(rest + k, nrest - k))) {
                pad = k;
                found = true;
            }
        }
        CK(psd_bytes(r, pad, &pb, err));
        for (k = 0; k < pad; k++) {
            zeros = zeros && pb[k] == 0;
        }
        if (!(pad == default_padding(b.data.len) && zeros)) {
            b.padding.has = true;
            CK(pc_psd_bytes_set(&b.padding.v, pb, pad, err));
        }
        if (count == cap) {
            pc_psd_block *g = pc_grow_array(list, &cap, count + 1, sizeof *g);
            if (g == NULL) {
                st = psd_nomem(err);
                goto fail;
            }
            list = g;
        }
        list[count++] = b;
        memset(&b, 0, sizeof b);
    }
fail:
    pc_psd_block_fini(&b);
    pc_psd_blocks_free(list, count);
    return st;
}

void psd_write_block(psd_w *w, const pc_psd_block *b, pc_psd_version v)
{
    psd_put(w, b->signature, 4);
    psd_put(w, b->key, 4);
    psd_put_len(w, b->data.len, pc_psd_uses_long_length(v, b->key));
    psd_put(w, b->data.data, b->data.len);
    if (b->padding.has) {
        psd_put(w, b->padding.v.data, b->padding.v.len);
    } else {
        psd_put_zeros(w, default_padding(b->data.len));
    }
}

void psd_write_blocks(psd_w *w, const pc_psd_block *blocks, size_t n, pc_psd_version v)
{
    size_t i;
    for (i = 0; i < n; i++) {
        psd_write_block(w, &blocks[i], v);
    }
}
