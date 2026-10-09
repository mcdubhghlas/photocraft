#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"
#include "psd/internal.h"
#include "psd/types.h"

/* ---- shared types ---- */

void pc_psd_bytes_fini(pc_psd_bytes *b)
{
    if (b != NULL) {
        free(b->data);
        b->data = NULL;
        b->len = 0;
    }
}

pc_status pc_psd_bytes_set(pc_psd_bytes *b, const void *data, size_t len, pc_error *err)
{
    uint8_t *p = NULL;
    if (len > 0) {
        p = malloc(len);
        if (p == NULL) {
            return psd_nomem(err);
        }
        memcpy(p, data, len);
    }
    free(b->data);
    b->data = p;
    b->len = len;
    return PC_OK;
}

pc_status pc_psd_bytes_clone(const pc_psd_bytes *b, pc_psd_bytes *out, pc_error *err)
{
    pc_psd_bytes c = {NULL, 0};
    pc_status st = pc_psd_bytes_set(&c, b->data, b->len, err);
    if (st == PC_OK) {
        *out = c;
    }
    return st;
}

bool pc_psd_bytes_eq(const pc_psd_bytes *a, const pc_psd_bytes *b)
{
    return a->len == b->len && (a->len == 0 || memcmp(a->data, b->data, a->len) == 0);
}

bool pc_psd_opt_bytes_eq(const pc_psd_opt_bytes *a, const pc_psd_opt_bytes *b)
{
    return a->has == b->has && (!a->has || pc_psd_bytes_eq(&a->v, &b->v));
}

pc_status pc_psd_opt_bytes_clone(const pc_psd_opt_bytes *b, pc_psd_opt_bytes *out, pc_error *err)
{
    pc_psd_opt_bytes c = {b->has, {NULL, 0}};
    pc_status st = b->has ? pc_psd_bytes_clone(&b->v, &c.v, err) : PC_OK;
    if (st == PC_OK) {
        *out = c;
    }
    return st;
}

void pc_psd_opt_bytes_fini(pc_psd_opt_bytes *b)
{
    if (b != NULL) {
        pc_psd_bytes_fini(&b->v);
        b->has = false;
    }
}

uint16_t pc_psd_mode_color_channels(uint16_t mode)
{
    switch (mode) {
    case PC_PSD_MODE_BITMAP:
    case PC_PSD_MODE_GRAYSCALE:
    case PC_PSD_MODE_INDEXED:
    case PC_PSD_MODE_DUOTONE:
        return 1;
    case PC_PSD_MODE_RGB:
    case PC_PSD_MODE_LAB:
        return 3;
    case PC_PSD_MODE_CMYK:
        return 4;
    default:
        return 0;
    }
}

const char PC_PSD_BLEND_KEYS[PC_PSD_BLEND_COUNT][5] = {
    "pass", "norm", "diss", "dark", "mul ", "idiv", "lbrn", "dkCl", "lite", "scrn", "div ", "lddg", "lgCl", "over",
    "sLit", "hLit", "vLit", "lLit", "pLit", "hMix", "diff", "smud", "fsub", "fdiv", "hue ", "sat ", "colr", "lum "};

int pc_psd_blend_index(const uint8_t key[4])
{
    int i;
    for (i = 0; i < PC_PSD_BLEND_COUNT; i++) {
        if (memcmp(PC_PSD_BLEND_KEYS[i], key, 4) == 0) {
            return i;
        }
    }
    return -1;
}

pc_psd_rect pc_psd_rect_from_xywh(int32_t left, int32_t top, uint32_t width, uint32_t height)
{
    pc_psd_rect r;
    int32_t w = width > (uint32_t)INT32_MAX ? INT32_MAX : (int32_t)width;
    int32_t h = height > (uint32_t)INT32_MAX ? INT32_MAX : (int32_t)height;
    r.top = top;
    r.left = left;
    r.bottom = pc_i32_sat_add(top, h);
    r.right = pc_i32_sat_add(left, w);
    return r;
}

pc_status pc_psd_rect_size(pc_psd_rect r, size_t *w, size_t *h, pc_error *err)
{
    int64_t ww = pc_psd_rect_width(r);
    int64_t hh = pc_psd_rect_height(r);
    if (ww < 0 || hh < 0) {
        return psd_invalid(err, "rectangle has negative size");
    }
    if (ww > (int64_t)PC_PSD_MAX_DIMENSION || hh > (int64_t)PC_PSD_MAX_DIMENSION) {
        return psd_limit(err, "layer dimensions exceed 300000");
    }
    *w = (size_t)ww;
    *h = (size_t)hh;
    return PC_OK;
}

/* ---- errors ---- */

pc_status psd_eof(pc_error *err, size_t offset, size_t needed)
{
    return pc_fail(err, PC_ERR_FORMAT, "unexpected end of data at offset %zu (needed %zu more bytes)", offset, needed);
}

pc_status psd_bad_sig(pc_error *err, const char *expected, const uint8_t found[4])
{
    return pc_fail(err, PC_ERR_FORMAT, "invalid signature [%u, %u, %u, %u], expected %s", (unsigned)found[0],
                   (unsigned)found[1], (unsigned)found[2], (unsigned)found[3], expected);
}

pc_status psd_wrap_invalid(pc_error *err, pc_status st, const char *prefix)
{
    char old[PC_ERROR_MSG_MAX];
    if (err == NULL) {
        return PC_ERR_FORMAT;
    }
    (void)st;
    memcpy(old, err->msg, sizeof old);
    old[sizeof old - 1] = '\0';
    return psd_invalid(err, "%s: %s", prefix, old);
}

/* ---- reader ---- */

pc_status psd_bytes(psd_rd *r, size_t n, const uint8_t **out, pc_error *err)
{
    size_t rem = psd_remaining(r);
    if (n > rem) {
        return psd_eof(err, r->pos, n - rem);
    }
    *out = r->data + r->pos;
    r->pos += n;
    return PC_OK;
}

pc_status psd_bytes_u64(psd_rd *r, uint64_t n, const uint8_t **out, size_t *len, pc_error *err)
{
    pc_status st;
    if (n > SIZE_MAX) {
        return psd_limit(err, "length exceeds address space");
    }
    st = psd_bytes(r, (size_t)n, out, err);
    if (st == PC_OK && len != NULL) {
        *len = (size_t)n;
    }
    return st;
}

pc_status psd_sub(psd_rd *r, uint64_t n, psd_rd *out, pc_error *err)
{
    const uint8_t *p;
    size_t len;
    pc_status st = psd_bytes_u64(r, n, &p, &len, err);
    if (st == PC_OK) {
        out->data = p;
        out->len = len;
        out->pos = 0;
        out->little = r->little;
    }
    return st;
}

pc_status psd_skip(psd_rd *r, size_t n, pc_error *err)
{
    const uint8_t *p;
    return psd_bytes(r, n, &p, err);
}

pc_status psd_arr4(psd_rd *r, uint8_t out[4], pc_error *err)
{
    const uint8_t *p;
    pc_status st = psd_bytes(r, 4, &p, err);
    if (st == PC_OK) {
        memcpy(out, p, 4);
    }
    return st;
}

pc_status psd_u8(psd_rd *r, uint8_t *v, pc_error *err)
{
    const uint8_t *p;
    pc_status st = psd_bytes(r, 1, &p, err);
    if (st == PC_OK) {
        *v = p[0];
    }
    return st;
}

pc_status psd_u16(psd_rd *r, uint16_t *v, pc_error *err)
{
    const uint8_t *p;
    pc_status st = psd_bytes(r, 2, &p, err);
    if (st == PC_OK) {
        *v = r->little ? pc_load_le16(p) : pc_load_be16(p);
    }
    return st;
}

pc_status psd_i16(psd_rd *r, int16_t *v, pc_error *err)
{
    uint16_t u;
    pc_status st = psd_u16(r, &u, err);
    if (st == PC_OK) {
        *v = (int16_t)(u > 0x7FFFu ? (int32_t)u - 0x10000 : (int32_t)u);
    }
    return st;
}

pc_status psd_u32(psd_rd *r, uint32_t *v, pc_error *err)
{
    const uint8_t *p;
    pc_status st = psd_bytes(r, 4, &p, err);
    if (st == PC_OK) {
        *v = r->little ? pc_load_le32(p) : pc_load_be32(p);
    }
    return st;
}

pc_status psd_i32(psd_rd *r, int32_t *v, pc_error *err)
{
    uint32_t u;
    pc_status st = psd_u32(r, &u, err);
    if (st == PC_OK) {
        *v = pc_u32_as_i32(u);
    }
    return st;
}

pc_status psd_u64(psd_rd *r, uint64_t *v, pc_error *err)
{
    const uint8_t *p;
    pc_status st = psd_bytes(r, 8, &p, err);
    if (st == PC_OK) {
        *v = r->little ? pc_load_le64(p) : pc_load_be64(p);
    }
    return st;
}

pc_status psd_i64(psd_rd *r, int64_t *v, pc_error *err)
{
    uint64_t u;
    pc_status st = psd_u64(r, &u, err);
    if (st == PC_OK) {
        *v = u > (uint64_t)INT64_MAX ? -(int64_t)(~u) - 1 : (int64_t)u;
    }
    return st;
}

pc_status psd_f64(psd_rd *r, double *v, pc_error *err)
{
    uint64_t u;
    pc_status st = psd_u64(r, &u, err);
    if (st == PC_OK) {
        *v = pc_f64_from_bits(u);
    }
    return st;
}

pc_status psd_len_field(psd_rd *r, bool wide, uint64_t *v, pc_error *err)
{
    uint32_t u;
    pc_status st;
    if (wide) {
        return psd_u64(r, v, err);
    }
    st = psd_u32(r, &u, err);
    if (st == PC_OK) {
        *v = u;
    }
    return st;
}

pc_status psd_check_count(const psd_rd *r, uint64_t count, uint64_t min_item, pc_error *err)
{
    uint64_t need = min_item != 0 && count > UINT64_MAX / min_item ? UINT64_MAX : count * min_item;
    uint64_t rem = psd_remaining(r);
    if (need > rem) {
        uint64_t miss = need - rem;
        return psd_eof(err, r->pos, miss > SIZE_MAX ? SIZE_MAX : (size_t)miss);
    }
    return PC_OK;
}

pc_status psd_take_bytes(psd_rd *r, uint64_t n, pc_psd_bytes *out, pc_error *err)
{
    const uint8_t *p;
    size_t len;
    pc_psd_bytes b = {NULL, 0};
    pc_status st = psd_bytes_u64(r, n, &p, &len, err);
    if (st == PC_OK) {
        st = pc_psd_bytes_set(&b, p, len, err);
    }
    if (st == PC_OK) {
        *out = b;
    }
    return st;
}

pc_status psd_read_pascal(psd_rd *r, size_t align, pc_psd_bytes *out, pc_error *err)
{
    uint8_t n;
    size_t total;
    size_t pad;
    pc_psd_bytes name = {NULL, 0};
    pc_status st = psd_u8(r, &n, err);
    if (st != PC_OK) {
        return st;
    }
    st = psd_take_bytes(r, n, &name, err);
    if (st != PC_OK) {
        return st;
    }
    total = 1u + n;
    pad = (align - total % align) % align;
    st = psd_skip(r, pad, err);
    if (st != PC_OK) {
        pc_psd_bytes_fini(&name);
        return st;
    }
    *out = name;
    return PC_OK;
}

pc_status psd_read_units(psd_rd *r, uint16_t **units, size_t *n, pc_error *err)
{
    uint32_t count;
    uint16_t *u = NULL;
    size_t i;
    pc_status st = psd_u32(r, &count, err);
    if (st != PC_OK) {
        return st;
    }
    st = psd_check_count(r, count, 2, err);
    if (st != PC_OK) {
        return st;
    }
    if (count > 0) {
        u = pc_malloc_array(count, sizeof *u);
        if (u == NULL) {
            return psd_nomem(err);
        }
        for (i = 0; i < count; i++) {
            (void)psd_u16(r, &u[i], NULL); /* checked above */
        }
    }
    *units = u;
    *n = count;
    return PC_OK;
}

/* ---- writer ---- */

void psd_w_init(psd_w *w, pc_error *err)
{
    pc_buf_init(&w->b);
    w->st = PC_OK;
    w->err = err;
}

void psd_w_fini(psd_w *w)
{
    pc_buf_fini(&w->b);
}

pc_status psd_w_finish(psd_w *w, pc_psd_bytes *out)
{
    if (w->st != PC_OK) {
        pc_buf_fini(&w->b);
        return w->st;
    }
    out->data = w->b.data;
    out->len = w->b.len;
    pc_buf_init(&w->b);
    return PC_OK;
}

void psd_w_fail(psd_w *w, pc_status st)
{
    if (w->st == PC_OK) {
        w->st = st;
    }
}

void psd_put(psd_w *w, const void *data, size_t n)
{
    if (w->st == PC_OK && n > 0) {
        w->st = pc_buf_append(&w->b, data, n, w->err);
    }
}

void psd_put_zeros(psd_w *w, size_t n)
{
    static const uint8_t z[64];
    while (w->st == PC_OK && n > 0) {
        size_t k = n < sizeof z ? n : sizeof z;
        psd_put(w, z, k);
        n -= k;
    }
}

void psd_put_u8(psd_w *w, uint8_t v)
{
    psd_put(w, &v, 1);
}

void psd_put_u16(psd_w *w, uint16_t v)
{
    uint8_t b[2];
    pc_store_be16(b, v);
    psd_put(w, b, 2);
}

void psd_put_i16(psd_w *w, int16_t v)
{
    psd_put_u16(w, (uint16_t)v);
}

void psd_put_u32(psd_w *w, uint32_t v)
{
    uint8_t b[4];
    pc_store_be32(b, v);
    psd_put(w, b, 4);
}

void psd_put_i32(psd_w *w, int32_t v)
{
    psd_put_u32(w, (uint32_t)v);
}

void psd_put_u64(psd_w *w, uint64_t v)
{
    uint8_t b[8];
    pc_store_be64(b, v);
    psd_put(w, b, 8);
}

void psd_put_i64(psd_w *w, int64_t v)
{
    psd_put_u64(w, (uint64_t)v);
}

void psd_put_f64(psd_w *w, double v)
{
    psd_put_u64(w, pc_f64_to_bits(v));
}

static void too_large(psd_w *w)
{
    if (w->st == PC_OK) {
        w->st = psd_limit(w->err, "section too large for 32-bit length (use PSB)");
    }
}

void psd_put_len(psd_w *w, uint64_t v, bool wide)
{
    if (wide) {
        psd_put_u64(w, v);
    } else if (v > 0xFFFFFFFFu) {
        too_large(w);
    } else {
        psd_put_u32(w, (uint32_t)v);
    }
}

size_t psd_begin_len(psd_w *w, bool wide)
{
    size_t at = w->b.len;
    psd_put_zeros(w, wide ? 8 : 4);
    return at;
}

void psd_end_len(psd_w *w, size_t at, bool wide)
{
    size_t hdr = wide ? 8 : 4;
    uint64_t n;
    if (w->st != PC_OK || at + hdr > w->b.len) {
        return;
    }
    n = (uint64_t)(w->b.len - at - hdr);
    if (wide) {
        pc_store_be64(w->b.data + at, n);
    } else if (n > 0xFFFFFFFFu) {
        too_large(w);
    } else {
        pc_store_be32(w->b.data + at, (uint32_t)n);
    }
}

void psd_write_pascal(psd_w *w, const uint8_t *name, size_t len, size_t align)
{
    size_t n = len < 255 ? len : 255;
    size_t total = 1 + n;
    psd_put_u8(w, (uint8_t)n);
    psd_put(w, name, n);
    psd_put_zeros(w, (align - total % align) % align);
}

void psd_write_units(psd_w *w, const uint16_t *units, size_t n)
{
    size_t i;
    psd_put_u32(w, (uint32_t)n);
    for (i = 0; i < n; i++) {
        psd_put_u16(w, units[i]);
    }
}

/* ---- names ---- */

static pc_status buf_to_cstr(pc_buf *b, char **out, pc_error *err)
{
    pc_status st = pc_buf_append_byte(b, 0, err);
    if (st != PC_OK) {
        pc_buf_fini(b);
        return st;
    }
    *out = (char *)b->data;
    return PC_OK;
}

pc_status psd_decode_legacy_name(const uint8_t *b, size_t len, char **out, pc_error *err)
{
    pc_buf o = PC_BUF_INIT;
    size_t i = 0;
    uint32_t cp;
    bool valid = true;
    pc_status st = PC_OK;
    while (i < len) {
        if (!pc_utf8_next(b, len, &i, &cp)) {
            valid = false;
            break;
        }
    }
    if (valid) {
        st = pc_buf_append(&o, b, len, err);
    } else {
        for (i = 0; i < len && st == PC_OK; i++) {
            st = pc_utf8_put(&o, b[i], err); /* Latin-1 */
        }
    }
    if (st != PC_OK) {
        pc_buf_fini(&o);
        return st;
    }
    return buf_to_cstr(&o, out, err);
}

pc_status psd_encode_legacy_name(const char *s, pc_psd_bytes *out, pc_error *err)
{
    const uint8_t *p = (const uint8_t *)s;
    size_t len = strlen(s);
    size_t i = 0;
    uint8_t tmp[255];
    size_t n = 0;
    while (i < len && n < 255) {
        uint32_t cp;
        if (!pc_utf8_next(p, len, &i, &cp)) { /* not reachable for valid UTF-8 input */
            i++;
            cp = 0xFFFD;
        }
        tmp[n++] = cp < 0x80 && cp >= 0x20 && cp != 0x7F ? (uint8_t)cp : (uint8_t)'?';
    }
    return pc_psd_bytes_set(out, tmp, n, err);
}

pc_status psd_units_to_utf8(const uint16_t *u, size_t n, char **out, pc_error *err)
{
    pc_buf o = PC_BUF_INIT;
    size_t i = 0;
    pc_status st = PC_OK;
    while (i < n && st == PC_OK) {
        uint32_t c = u[i++];
        if (c >= 0xD800 && c <= 0xDBFF && i < n && u[i] >= 0xDC00 && u[i] <= 0xDFFF) {
            c = 0x10000u + ((c - 0xD800u) << 10) + (u[i++] - 0xDC00u);
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        st = pc_utf8_put(&o, c, err);
    }
    if (st != PC_OK) {
        pc_buf_fini(&o);
        return st;
    }
    return buf_to_cstr(&o, out, err);
}

pc_status psd_utf8_to_units(const char *s, uint16_t **out, size_t *n, pc_error *err)
{
    const uint8_t *p = (const uint8_t *)s;
    size_t len = strlen(s);
    size_t i = 0;
    size_t k = 0;
    uint16_t *u = NULL;
    if (len > 0) {
        u = pc_malloc_array(len, 2 * sizeof *u); /* at most 2 units per byte */
        if (u == NULL) {
            return psd_nomem(err);
        }
    }
    while (i < len) {
        uint32_t cp;
        if (!pc_utf8_next(p, len, &i, &cp)) {
            i++;
            cp = 0xFFFD;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            u[k++] = (uint16_t)(0xD800 + (cp >> 10));
            u[k++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            u[k++] = (uint16_t)cp;
        }
    }
    *out = u;
    *n = k;
    return PC_OK;
}
