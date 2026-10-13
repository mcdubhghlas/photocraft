#include "psd/tiff.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/buf.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"
#include "psd/descriptor.h"
#include "psd/filter_effects.h"
#include "psd/internal.h"

const uint8_t PC_PSD_ISD_SIGNATURE[36] = "Adobe Photoshop Document Data Block";
const uint8_t PC_PSD_ISD_SIGNATURE_PSB[36] = "Adobe Photoshop Document Data V0002";

/* ---- warnings ---- */

/* Moves every warning of src to the end of dst. */
static pc_status warn_append(pc_psd_warnings *dst, pc_psd_warnings *src, pc_error *err)
{
    char **g;
    if (src->n == 0) {
        return PC_OK;
    }
    g = realloc(dst->v, (dst->n + src->n) * sizeof *g);
    if (g == NULL) {
        return psd_nomem(err);
    }
    dst->v = g;
    memcpy(dst->v + dst->n, src->v, src->n * sizeof *g);
    dst->n += src->n;
    free(src->v);
    src->v = NULL;
    src->n = 0;
    return PC_OK;
}

void pc_psd_layer_info_key(uint16_t depth, uint8_t out[4])
{
    memcpy(out, depth == 16 ? "Lr16" : depth == 32 ? "Lr32" : "Layr", 4);
}

/* Lossy key text for messages. */
static void key_text(const uint8_t key[4], char out[32])
{
    pc_buf s = PC_BUF_INIT;
    out[0] = '\0';
    if (pc_utf8_lossy(key, 4, &s, NULL) == PC_OK && s.len < 32) {
        memcpy(out, s.data, s.len);
        out[s.len] = '\0';
    }
    pc_buf_fini(&s);
}

/* ---- the transcoder ---- */

typedef struct tx {
    psd_rd r;
    pc_buf out;
    bool from_le;
    bool to_le;
    pc_psd_version version;
    pc_psd_warnings warn;
    size_t depth;
} tx;

typedef pc_status (*tx_fn)(tx *t, void *ctx, pc_error *err);

static void tx_init(tx *t, const uint8_t *data, size_t len, pc_psd_version v, bool from_le, bool to_le)
{
    memset(t, 0, sizeof *t);
    t->r = psd_rd_make(data, len);
    t->r.little = from_le;
    t->from_le = from_le;
    t->to_le = to_le;
    t->version = v;
}

static void tx_fini(tx *t)
{
    pc_buf_fini(&t->out);
    pc_psd_warnings_fini(&t->warn);
}

static bool same_order(const tx *t)
{
    return t->from_le == t->to_le;
}

static pc_status put(tx *t, const void *d, size_t n, pc_error *err)
{
    return n == 0 ? PC_OK : pc_buf_append(&t->out, d, n, err);
}

/* An n-byte number: read in the source order, written in the target order. */
static pc_status num(tx *t, size_t n, uint64_t *v, pc_error *err)
{
    const uint8_t *p;
    uint8_t b[8];
    uint64_t x = 0;
    size_t i;
    pc_status st = psd_bytes(&t->r, n, &p, err);
    if (st != PC_OK) {
        return st;
    }
    for (i = 0; i < n; i++) {
        x = x << 8 | p[t->from_le ? n - 1 - i : i];
    }
    for (i = 0; i < n; i++) {
        b[i] = t->from_le == t->to_le ? p[i] : p[n - 1 - i];
    }
    if (v != NULL) {
        *v = x;
    }
    return put(t, b, n, err);
}

static pc_status u8_(tx *t, uint8_t *v, pc_error *err)
{
    uint64_t x;
    pc_status st = num(t, 1, &x, err);
    if (st == PC_OK && v != NULL) {
        *v = (uint8_t)x;
    }
    return st;
}

static pc_status u16_(tx *t, uint16_t *v, pc_error *err)
{
    uint64_t x;
    pc_status st = num(t, 2, &x, err);
    if (st == PC_OK && v != NULL) {
        *v = (uint16_t)x;
    }
    return st;
}

static pc_status i16_(tx *t, int16_t *v, pc_error *err)
{
    uint16_t u;
    pc_status st = u16_(t, &u, err);
    if (st == PC_OK && v != NULL) {
        *v = (int16_t)(u > 0x7FFF ? (int32_t)u - 0x10000 : (int32_t)u);
    }
    return st;
}

static pc_status u32_(tx *t, uint32_t *v, pc_error *err)
{
    uint64_t x;
    pc_status st = num(t, 4, &x, err);
    if (st == PC_OK && v != NULL) {
        *v = (uint32_t)x;
    }
    return st;
}

static pc_status i32_(tx *t, int32_t *v, pc_error *err)
{
    uint32_t u;
    pc_status st = u32_(t, &u, err);
    if (st == PC_OK && v != NULL) {
        *v = pc_u32_as_i32(u);
    }
    return st;
}

static pc_status u64_(tx *t, uint64_t *v, pc_error *err)
{
    return num(t, 8, v, err);
}

#define F64(t, err) num((t), 8, NULL, (err))
#define F32(t, err) num((t), 4, NULL, (err))

static pc_status raw(tx *t, size_t n, pc_error *err)
{
    const uint8_t *p;
    pc_status st = psd_bytes(&t->r, n, &p, err);
    return st == PC_OK ? put(t, p, n, err) : st;
}

static pc_status rest(tx *t, pc_error *err)
{
    return raw(t, psd_remaining(&t->r), err);
}

/* A four-character code: stored reversed in little-endian data. Returns the canonical spelling. */
static pc_status key(tx *t, uint8_t out[4], pc_error *err)
{
    uint8_t k[4];
    uint8_t w[4];
    int i;
    pc_status st = psd_arr4(&t->r, k, err);
    if (st != PC_OK) {
        return st;
    }
    if (t->from_le) {
        uint8_t c[4];
        for (i = 0; i < 4; i++) {
            c[i] = k[3 - i];
        }
        memcpy(k, c, 4);
    }
    for (i = 0; i < 4; i++) {
        w[i] = t->to_le ? k[3 - i] : k[i];
    }
    if (out != NULL) {
        memcpy(out, k, 4);
    }
    return put(t, w, 4, err);
}

static pc_status len_(tx *t, bool wide, uint64_t *v, pc_error *err)
{
    uint32_t u;
    pc_status st;
    if (wide) {
        return u64_(t, v, err);
    }
    st = u32_(t, &u, err);
    if (st == PC_OK) {
        *v = u;
    }
    return st;
}

static pc_status u16s(tx *t, size_t n, pc_error *err)
{
    size_t i;
    pc_status st = PC_OK;
    for (i = 0; i < n && st == PC_OK; i++) {
        st = u16_(t, NULL, err);
    }
    return st;
}

static pc_status u16s_upto(tx *t, size_t n, pc_error *err)
{
    size_t i;
    pc_status st = PC_OK;
    for (i = 0; i < n && st == PC_OK && psd_remaining(&t->r) >= 2; i++) {
        st = u16_(t, NULL, err);
    }
    return st;
}

static pc_status i32s(tx *t, size_t n, pc_error *err)
{
    size_t i;
    pc_status st = PC_OK;
    for (i = 0; i < n && st == PC_OK; i++) {
        st = u32_(t, NULL, err);
    }
    return st;
}

static pc_status f64s(tx *t, size_t n, pc_error *err)
{
    size_t i;
    pc_status st = PC_OK;
    for (i = 0; i < n && st == PC_OK; i++) {
        st = F64(t, err);
    }
    return st;
}

static pc_status u16s_to_end(tx *t, pc_error *err)
{
    pc_status st = PC_OK;
    while (st == PC_OK && psd_remaining(&t->r) >= 2) {
        st = u16_(t, NULL, err);
    }
    return st == PC_OK ? rest(t, err) : st;
}

static pc_status u32s_to_end(tx *t, pc_error *err)
{
    pc_status st = PC_OK;
    while (st == PC_OK && psd_remaining(&t->r) >= 4) {
        st = u32_(t, NULL, err);
    }
    return st == PC_OK ? rest(t, err) : st;
}

static pc_status unicode(tx *t, pc_error *err)
{
    uint32_t n;
    pc_status st = u32_(t, &n, err);
    if (st == PC_OK) {
        st = psd_check_count(&t->r, n, 2, err);
    }
    return st == PC_OK ? u16s(t, n, err) : st;
}

static pc_status pascal(tx *t, size_t align, pc_error *err)
{
    uint8_t n;
    pc_status st = u8_(t, &n, err);
    if (st == PC_OK) {
        st = raw(t, n, err);
    }
    return st == PC_OK ? raw(t, (align - (1u + n) % align) % align, err) : st;
}

static pc_status patch_len(tx *t, size_t at, bool wide, uint64_t n, pc_error *err)
{
    uint8_t b[8];
    size_t w = wide ? 8 : 4;
    size_t i;
    if (!wide && n > 0xFFFFFFFFu) {
        return psd_limit(err, "structure too large for a 32-bit length");
    }
    for (i = 0; i < w; i++) {
        size_t shift = 8 * (t->to_le ? i : w - 1 - i);
        b[i] = (uint8_t)(n >> shift);
    }
    if (at + w <= t->out.len) {
        memcpy(t->out.data + at, b, w);
    }
    return PC_OK;
}

static void child_of(const tx *t, tx *c, const uint8_t *data, size_t len)
{
    tx_init(c, data, len, t->version, t->from_le, t->to_le);
    c->depth = t->depth;
}

/* Transcodes the next len bytes with f; what f leaves is copied verbatim. */
static pc_status region(tx *t, uint64_t len, tx_fn f, void *ctx, pc_error *err)
{
    const uint8_t *data;
    size_t n;
    tx c;
    pc_status st = psd_bytes_u64(&t->r, len, &data, &n, err);
    if (st != PC_OK) {
        return st;
    }
    child_of(t, &c, data, n);
    st = f(&c, ctx, err);
    if (st == PC_OK) {
        st = rest(&c, err);
    }
    if (st == PC_OK && c.out.len != n) {
        st = psd_invalid(err, "byte-order conversion changed a structure's length");
    }
    if (st == PC_OK) {
        st = warn_append(&t->warn, &c.warn, err);
    }
    if (st == PC_OK) {
        st = put(t, c.out.data, c.out.len, err);
    }
    tx_fini(&c);
    return st;
}

/* A length field (4 bytes, 8 when wide; counting its own 4 bytes when inclusive), then that many
 * bytes transcoded with f; the field is rewritten with the result's length. */
static pc_status sized_counting(tx *t, bool wide, bool inclusive, tx_fn f, void *ctx, pc_error *err)
{
    uint64_t len;
    size_t at;
    const uint8_t *data;
    size_t n;
    tx c;
    static const uint8_t zeros[8];
    pc_status st = psd_len_field(&t->r, wide, &len, err);
    if (st != PC_OK) {
        return st;
    }
    at = t->out.len;
    st = put(t, zeros, wide ? 8 : 4, err);
    if (st != PC_OK) {
        return st;
    }
    if (inclusive) {
        if (len < 4) {
            return psd_invalid(err, "length shorter than its own field");
        }
        len -= 4;
    }
    st = psd_bytes_u64(&t->r, len, &data, &n, err);
    if (st != PC_OK) {
        return st;
    }
    child_of(t, &c, data, n);
    st = f(&c, ctx, err);
    if (st == PC_OK) {
        st = rest(&c, err);
    }
    if (st == PC_OK) {
        st = warn_append(&t->warn, &c.warn, err);
    }
    if (st == PC_OK) {
        st = patch_len(t, at, wide, (uint64_t)c.out.len + (inclusive ? 4 : 0), err);
    }
    if (st == PC_OK) {
        st = put(t, c.out.data, c.out.len, err);
    }
    tx_fini(&c);
    return st;
}

static pc_status sized(tx *t, bool wide, tx_fn f, void *ctx, pc_error *err)
{
    return sized_counting(t, wide, false, f, ctx, err);
}

static pc_status sized_inclusive(tx *t, tx_fn f, void *ctx, pc_error *err)
{
    return sized_counting(t, false, true, f, ctx, err);
}

/* Tries f on the rest of the data: a match only when at most three trailing zero bytes are
 * left. On a miss nothing is consumed or written. */
static bool attempt(tx *t, tx_fn f, void *ctx)
{
    psd_rd saved = t->r;
    size_t written = t->out.len;
    pc_error e;
    bool ok = f(t, ctx, &e) == PC_OK && psd_remaining(&t->r) <= 3;
    size_t i;
    for (i = 0; ok && i < psd_remaining(&t->r); i++) {
        ok = psd_rest(&t->r)[i] == 0;
    }
    if (!ok) {
        t->r = saved;
        pc_buf_truncate(&t->out, written);
    }
    return ok;
}

static pc_status fn_rest(tx *t, void *ctx, pc_error *err)
{
    (void)ctx;
    return rest(t, err);
}

/* ---- descriptors ---- */

static pc_status descriptor(tx *t, pc_error *err);
static pc_status value(tx *t, pc_error *err);

static pc_status id_(tx *t, pc_error *err)
{
    uint32_t n;
    pc_status st = u32_(t, &n, err);
    if (st != PC_OK) {
        return st;
    }
    return n == 0 ? key(t, NULL, err) : raw(t, n, err);
}

static pc_status class_(tx *t, pc_error *err)
{
    pc_status st = unicode(t, err);
    return st == PC_OK ? id_(t, err) : st;
}

static pc_status descriptor(tx *t, pc_error *err)
{
    uint32_t n;
    uint32_t i;
    pc_status st;
    if (t->depth > PC_PSD_DESC_MAX_DEPTH) {
        return psd_limit(err, "descriptor nesting too deep");
    }
    t->depth++;
    st = unicode(t, err);
    if (st == PC_OK) {
        st = id_(t, err);
    }
    if (st == PC_OK) {
        st = u32_(t, &n, err);
    }
    if (st == PC_OK) {
        st = psd_check_count(&t->r, n, 10, err);
    }
    for (i = 0; st == PC_OK && i < n; i++) {
        st = id_(t, err);
        if (st == PC_OK) {
            st = value(t, err);
        }
    }
    t->depth--;
    return st;
}

static pc_status versioned_descriptor(tx *t, pc_error *err)
{
    pc_status st = u32_(t, NULL, err);
    return st == PC_OK ? descriptor(t, err) : st;
}

static pc_status fn_vdesc(tx *t, void *ctx, pc_error *err)
{
    (void)ctx;
    return versioned_descriptor(t, err);
}

static pc_status fn_u32_vdesc(tx *t, void *ctx, pc_error *err)
{
    pc_status st = u32_(t, NULL, err);
    (void)ctx;
    return st == PC_OK ? versioned_descriptor(t, err) : st;
}

static pc_status unsupported_code(pc_error *err, const char *what, const uint8_t k[4])
{
    char q[32];
    pc_utf8_debug_quote(k, 4, q, sizeof q);
    return psd_unsupported(err, "%s %s", what, q);
}

static pc_status reference_item(tx *t, pc_error *err)
{
    uint8_t ty[4];
    pc_status st = key(t, ty, err);
    if (st != PC_OK) {
        return st;
    }
    if (memcmp(ty, "prop", 4) == 0) {
        st = class_(t, err);
        return st == PC_OK ? id_(t, err) : st;
    }
    if (memcmp(ty, "Clss", 4) == 0) {
        return class_(t, err);
    }
    if (memcmp(ty, "Enmr", 4) == 0) {
        st = class_(t, err);
        if (st == PC_OK) {
            st = id_(t, err);
        }
        return st == PC_OK ? id_(t, err) : st;
    }
    if (memcmp(ty, "rele", 4) == 0) {
        st = class_(t, err);
        return st == PC_OK ? u32_(t, NULL, err) : st;
    }
    if (memcmp(ty, "Idnt", 4) == 0 || memcmp(ty, "indx", 4) == 0) {
        return u32_(t, NULL, err);
    }
    if (memcmp(ty, "name", 4) == 0) {
        st = class_(t, err);
        return st == PC_OK ? unicode(t, err) : st;
    }
    return unsupported_code(err, "reference item type", ty);
}

static pc_status value(tx *t, pc_error *err)
{
    uint8_t ty[4];
    uint32_t n;
    uint32_t i;
    pc_status st;
    if (t->depth > PC_PSD_DESC_MAX_DEPTH) {
        return psd_limit(err, "descriptor nesting too deep");
    }
    st = key(t, ty, err);
    if (st != PC_OK) {
        return st;
    }
    if (memcmp(ty, "obj ", 4) == 0) {
        CK(u32_(t, &n, err));
        CK(psd_check_count(&t->r, n, 8, err));
        for (i = 0; i < n; i++) {
            CK(reference_item(t, err));
        }
    } else if (memcmp(ty, "Objc", 4) == 0 || memcmp(ty, "GlbO", 4) == 0) {
        CK(descriptor(t, err));
    } else if (memcmp(ty, "VlLs", 4) == 0) {
        CK(u32_(t, &n, err));
        CK(psd_check_count(&t->r, n, 4, err));
        t->depth++;
        for (i = 0; i < n; i++) {
            st = value(t, err);
            if (st != PC_OK) {
                t->depth--;
                return st;
            }
        }
        t->depth--;
    } else if (memcmp(ty, "doub", 4) == 0) {
        CK(F64(t, err));
    } else if (memcmp(ty, "UntF", 4) == 0) {
        CK(key(t, NULL, err));
        CK(F64(t, err));
    } else if (memcmp(ty, "UnFl", 4) == 0) {
        CK(key(t, NULL, err));
        CK(u32_(t, &n, err));
        CK(psd_check_count(&t->r, n, 8, err));
        CK(f64s(t, n, err));
    } else if (memcmp(ty, "TEXT", 4) == 0) {
        CK(unicode(t, err));
    } else if (memcmp(ty, "enum", 4) == 0) {
        CK(id_(t, err));
        CK(id_(t, err));
    } else if (memcmp(ty, "long", 4) == 0) {
        CK(u32_(t, NULL, err));
    } else if (memcmp(ty, "comp", 4) == 0) {
        CK(u64_(t, NULL, err));
    } else if (memcmp(ty, "bool", 4) == 0) {
        CK(u8_(t, NULL, err));
    } else if (memcmp(ty, "type", 4) == 0 || memcmp(ty, "GlbC", 4) == 0) {
        CK(class_(t, err));
    } else if (memcmp(ty, "alis", 4) == 0 || memcmp(ty, "Pth ", 4) == 0 || memcmp(ty, "tdta", 4) == 0) {
        CK(u32_(t, &n, err));
        CK(raw(t, n, err));
    } else if (memcmp(ty, "ObAr", 4) == 0) {
        CK(u32_(t, NULL, err));
        CK(descriptor(t, err));
    } else {
        return unsupported_code(err, "descriptor OSType", ty);
    }
    return PC_OK;
fail:
    return st;
}

/* Unknown keys very often hold a versioned descriptor, optionally behind a u32 version. */
static bool descriptor_shaped(tx *t)
{
    return attempt(t, fn_vdesc, NULL) || attempt(t, fn_u32_vdesc, NULL);
}

/* ---- channel data ---- */

static size_t height(int32_t top, int32_t bottom)
{
    int32_t d = pc_i32_sat_sub(bottom, top);
    return d > 0 ? (size_t)d : 0;
}

static pc_status rle_counts(tx *t, size_t rows, pc_error *err)
{
    size_t i;
    pc_status st = PC_OK;
    for (i = 0; i < rows && st == PC_OK; i++) {
        if (t->version == PC_PSD_V2) {
            if (psd_remaining(&t->r) < 4) {
                break;
            }
            st = u32_(t, NULL, err);
        } else {
            if (psd_remaining(&t->r) < 2) {
                break;
            }
            st = u16_(t, NULL, err);
        }
    }
    return st;
}

typedef struct chan_ctx {
    size_t rows;
    bool long_counts;
} chan_ctx;

/* The compression field, the RLE row counts (container order) and the samples (copied). */
static pc_status channel_data(tx *t, void *ctx, pc_error *err)
{
    chan_ctx *c = ctx;
    pc_psd_version saved = t->version;
    uint16_t comp;
    pc_status st;
    if (c->long_counts) {
        t->version = PC_PSD_V2;
    }
    st = u16_(t, &comp, err);
    if (st == PC_OK && comp == 1) {
        st = rle_counts(t, c->rows, err);
    }
    t->version = saved;
    return st == PC_OK ? rest(t, err) : st;
}

/* ---- path records ---- */

static pc_status path_records(tx *t, pc_error *err)
{
    pc_status st = PC_OK;
    while (st == PC_OK && psd_remaining(&t->r) >= 26) {
        uint16_t sel;
        CK(u16_(t, &sel, err));
        switch (sel) {
        case 1:
        case 2:
        case 4:
        case 5:
            st = i32s(t, 6, err);
            break;
        case 0:
        case 3:
            st = u16s(t, 12, err);
            break;
        case 7:
            st = i32s(t, 5, err);
            if (st == PC_OK) {
                st = raw(t, 4, err);
            }
            break;
        case 8:
            st = u16_(t, NULL, err);
            if (st == PC_OK) {
                st = raw(t, 22, err);
            }
            break;
        default:
            st = raw(t, 24, err);
            break;
        }
    }
    return st;
fail:
    return st;
}

/* ---- image resources ---- */

static bool is_sig(const uint8_t *b, size_t len, bool little)
{
    if (len < 4) {
        return false;
    }
    if (little) {
        return memcmp(b, "MIB8", 4) == 0 || memcmp(b, "46B8", 4) == 0;
    }
    return memcmp(b, "8BIM", 4) == 0 || memcmp(b, "8B64", 4) == 0 || pc_psd_is_foreign_order_signature(b, len);
}

static pc_status resource_data(tx *t, void *ctx, pc_error *err)
{
    uint16_t id = *(const uint16_t *)ctx;
    uint32_t n;
    uint32_t i;
    pc_status st = PC_OK;
    switch (id) {
    case 1005:
        CK(u32_(t, NULL, err));
        CK(u16_(t, NULL, err));
        CK(u16_(t, NULL, err));
        CK(u32_(t, NULL, err));
        CK(u16_(t, NULL, err));
        CK(u16_(t, NULL, err));
        break;
    case 1024:
    case 1026:
        CK(u16s_to_end(t, err));
        break;
    case 1037:
    case 1049:
        CK(u32_(t, NULL, err));
        break;
    case 1069:
        CK(u16_(t, NULL, err));
        CK(u32s_to_end(t, err));
        break;
    case 1057:
        CK(u32_(t, NULL, err));
        CK(u8_(t, NULL, err));
        CK(unicode(t, err));
        CK(unicode(t, err));
        CK(u32_(t, NULL, err));
        break;
    case 1032:
        CK(u32_(t, NULL, err));
        CK(u32_(t, NULL, err));
        CK(u32_(t, NULL, err));
        CK(u32_(t, &n, err));
        for (i = 0; i < n && psd_remaining(&t->r) >= 5; i++) {
            CK(u32_(t, NULL, err));
            CK(u8_(t, NULL, err));
        }
        break;
    default:
        if ((id >= 2000 && id <= 2997) || id == 2999) {
            CK(path_records(t, err));
        }
        break;
    }
    return PC_OK;
fail:
    return st;
}

static pc_status resources(tx *t, pc_error *err)
{
    pc_status st = PC_OK;
    while (psd_remaining(&t->r) >= 12 && is_sig(psd_rest(&t->r), psd_remaining(&t->r), t->from_le)) {
        uint16_t id;
        psd_rd peek;
        uint32_t len;
        bool odd;
        CK(key(t, NULL, err));
        CK(u16_(t, &id, err));
        CK(pascal(t, 2, err));
        peek = t->r;
        CK(psd_u32(&peek, &len, err));
        odd = len % 2 == 1;
        CK(sized(t, false, resource_data, &id, err));
        if (odd && psd_remaining(&t->r) > 0) {
            CK(raw(t, 1, err));
        }
    }
    return rest(t, err);
fail:
    return st;
}

/* ---- tagged blocks ---- */

static pc_status blocks(tx *t, pc_error *err);
static pc_status block_body(tx *t, const uint8_t key4[4], bool *handled, pc_error *err);

static size_t detect_pad(const tx *t)
{
    const uint8_t *r = psd_rest(&t->r);
    size_t n = psd_remaining(&t->r);
    size_t k;
    for (k = 0; k <= 3; k++) {
        if (k == n || (k < n && is_sig(r + k, n - k, t->from_le))) {
            return k;
        }
    }
    return 0;
}

static pc_status block(tx *t, pc_error *err)
{
    size_t before = t->out.len;
    static const uint8_t zeros[8];
    uint8_t sig[4];
    uint8_t k4[4];
    bool wide;
    uint64_t len;
    size_t len_at;
    size_t sig_at;
    const uint8_t *data;
    size_t n;
    const uint8_t *pad_bytes;
    size_t pad;
    tx c;
    pc_error e;
    pc_status hs;
    bool handled = false;
    char ktext[32];
    pc_status st;
    /* A MIB8 / 46B8 marker in big-endian data: a block kept verbatim from a little-endian file. */
    if (!t->from_le && pc_psd_is_foreign_order_signature(psd_rest(&t->r), psd_remaining(&t->r))) {
        uint8_t kk[4];
        int i;
        CK(psd_arr4(&t->r, sig, err));
        CK(psd_arr4(&t->r, k4, err));
        for (i = 0; i < 4; i++) {
            kk[i] = t->to_le ? k4[3 - i] : k4[i];
        }
        wide = pc_psd_uses_long_length(t->version, kk);
        CK(psd_len_field(&t->r, wide, &len, err));
        CK(psd_bytes_u64(&t->r, len, &data, &n, err));
        CK(put(t, sig, 4, err));
        CK(put(t, kk, 4, err)); /* the key reversed into little-endian output, as is */
        len_at = t->out.len;
        CK(put(t, zeros, wide ? 8 : 4, err));
        CK(patch_len(t, len_at, wide, len, err));
        CK(put(t, data, n, err));
        pad = detect_pad(t);
        CK(psd_bytes(&t->r, pad, &pad_bytes, err));
        return put(t, pad_bytes, pad, err);
    }
    sig_at = t->out.len;
    CK(key(t, sig, err));
    CK(key(t, k4, err));
    wide = pc_psd_uses_long_length(t->version, k4);
    CK(psd_len_field(&t->r, wide, &len, err));
    len_at = t->out.len;
    CK(put(t, zeros, wide ? 8 : 4, err));
    CK(psd_bytes_u64(&t->r, len, &data, &n, err));
    child_of(t, &c, data, n);
    c.depth = t->depth + 1;
    if (c.depth > PC_PSD_DESC_MAX_DEPTH) {
        hs = psd_limit(&e, "tagged block nesting too deep");
    } else if (n == 0) {
        hs = PC_OK;
        handled = true;
    } else {
        hs = block_body(&c, k4, &handled, &e);
    }
    pad = detect_pad(t);
    st = psd_bytes(&t->r, pad, &pad_bytes, err);
    if (st != PC_OK) {
        tx_fini(&c);
        return st;
    }
    key_text(k4, ktext);
    if (hs == PC_ERR_NOMEM) {
        tx_fini(&c);
        return psd_nomem(err);
    }
    if (hs == PC_OK && handled) {
        st = rest(&c, err);
        if (st == PC_OK) {
            st = warn_append(&t->warn, &c.warn, err);
        }
        if (st == PC_OK) {
            st = patch_len(t, len_at, wide, c.out.len, err);
        }
        if (st == PC_OK) {
            st = put(t, c.out.data, c.out.len, err);
        }
        if (st == PC_OK) {
            st = put(t, pad_bytes, pad, err);
        }
    } else if (hs != PC_OK && same_order(t)) {
        char prefix[48];
        tx_fini(&c);
        (void)snprintf(prefix, sizeof prefix, "%s block", ktext);
        if (err != NULL) {
            *err = e;
        }
        return psd_wrap_invalid(err, hs, prefix);
    } else if (same_order(t) || t->from_le) {
        /* Unknown layout: copied as is (same order), or kept marked as little-endian data
         * (little-endian in, big-endian model). */
        if (!same_order(t)) {
            memcpy(t->out.data + sig_at, memcmp(sig, "8B64", 4) == 0 ? "46B8" : "MIB8", 4);
        }
        st = patch_len(t, len_at, wide, n, err);
        if (st == PC_OK) {
            st = put(t, data, n, err);
        }
        if (st == PC_OK) {
            st = put(t, pad_bytes, pad, err);
        }
        if (st == PC_OK && hs != PC_OK) {
            st = psd_warn(&t->warn, err, "%s block (%zu bytes) kept as stored, in little-endian layout: %s", ktext,
                           n, e.msg);
        }
    } else {
        pc_buf_truncate(&t->out, before);
        if (hs == PC_OK) {
            st = psd_warn(&t->warn, err,
                           "%s block (%zu bytes) dropped: its layout is unknown and cannot be converted to the "
                           "file's byte order",
                           ktext, n);
        } else {
            st = psd_warn(&t->warn, err, "%s block (%zu bytes) dropped: %s", ktext, n, e.msg);
        }
    }
    tx_fini(&c);
    return st;
fail:
    return st;
}

static pc_status blocks(tx *t, pc_error *err)
{
    for (;;) {
        pc_status st;
        if (psd_remaining(&t->r) < 12 || !is_sig(psd_rest(&t->r), psd_remaining(&t->r), t->from_le)) {
            return rest(t, err);
        }
        st = block(t, err);
        if (st != PC_OK) {
            return st;
        }
    }
}

/* ---- layer info ---- */

static pc_status mask(tx *t, size_t *h_mask, size_t *h_real, pc_error *err)
{
    const uint8_t *whole = psd_rest(&t->r);
    size_t wlen = psd_remaining(&t->r);
    int32_t top;
    int32_t bottom;
    uint8_t flags;
    bool has_params;
    bool real_first;
    int pass;
    pc_status st;
    *h_mask = 0;
    *h_real = 0;
    if (wlen == 0) {
        return PC_OK;
    }
    CK(i32_(t, &top, err));
    CK(i32_(t, NULL, err));
    CK(i32_(t, &bottom, err));
    CK(i32_(t, NULL, err));
    CK(u8_(t, NULL, err));
    CK(u8_(t, &flags, err));
    *h_mask = height(top, bottom);
    if (psd_remaining(&t->r) <= 2) {
        return PC_OK;
    }
    has_params = (flags & 16) != 0;
    real_first = has_params && pc_psd_mask_real_before_parameters(whole, wlen);
    for (pass = 0; pass < 3; pass++) {
        bool do_real = (pass == 0 && real_first) || (pass == 2 && !real_first);
        if (do_real && psd_remaining(&t->r) >= 18) {
            int32_t rt;
            int32_t rb;
            CK(u8_(t, NULL, err));
            CK(u8_(t, NULL, err));
            CK(i32_(t, &rt, err));
            CK(i32_(t, NULL, err));
            CK(i32_(t, &rb, err));
            CK(i32_(t, NULL, err));
            *h_real = height(rt, rb);
        }
        if (pass == 1 && has_params) {
            uint8_t pf;
            size_t need;
            CK(u8_(t, &pf, err));
            need = pc_psd_mask_parameter_bytes(pf);
            if (need > psd_remaining(&t->r)) {
                return psd_eof(err, t->r.pos, need - psd_remaining(&t->r));
            }
            if (pf & 1) {
                CK(u8_(t, NULL, err));
            }
            if (pf & 2) {
                CK(F64(t, err));
            }
            if (pf & 4) {
                CK(u8_(t, NULL, err));
            }
            if (pf & 8) {
                CK(F64(t, err));
            }
        }
    }
    return PC_OK;
fail:
    return st;
}

typedef struct mask_ctx {
    size_t h_mask;
    size_t h_real;
} mask_ctx;

static pc_status fn_mask(tx *t, void *ctx, pc_error *err)
{
    mask_ctx *m = ctx;
    return mask(t, &m->h_mask, &m->h_real, err);
}

static pc_status fn_extra(tx *t, void *ctx, pc_error *err)
{
    pc_status st = sized(t, false, fn_mask, ctx, err);
    /* Blending ranges are bytes: the same in either order (swapping them as u32 hides every
     * pixel of the layer). */
    if (st == PC_OK) {
        st = sized(t, false, fn_rest, NULL, err);
    }
    if (st == PC_OK) {
        st = pascal(t, 4, err);
    }
    return st == PC_OK ? blocks(t, err) : st;
}

typedef struct rec_shape {
    int16_t *ids;
    uint64_t *lens;
    size_t n;
    size_t heights[3];
} rec_shape;

static pc_status layer_info(tx *t, pc_error *err)
{
    int16_t count;
    size_t n;
    size_t i;
    size_t k;
    rec_shape *recs = NULL;
    pc_status st;
    CK(i16_(t, &count, err));
    n = (size_t)(count < 0 ? -(int32_t)count : count);
    CK(psd_check_count(&t->r, n, 34, err));
    if (n > 0) {
        recs = pc_calloc_array(n, sizeof *recs);
        if (recs == NULL) {
            return psd_nomem(err);
        }
    }
    for (i = 0; i < n; i++) {
        int32_t top;
        int32_t bottom;
        uint16_t nch;
        uint8_t sig[4];
        mask_ctx mc = {0, 0};
        CK(i32_(t, &top, err));
        CK(i32_(t, NULL, err));
        CK(i32_(t, &bottom, err));
        CK(i32_(t, NULL, err));
        CK(u16_(t, &nch, err));
        if (nch > PC_PSD_MAX_LAYER_CHANNELS) {
            st = psd_limit(err, "too many channels in layer");
            goto fail;
        }
        if (nch > 0) {
            recs[i].ids = pc_calloc_array(nch, sizeof *recs[i].ids);
            recs[i].lens = pc_calloc_array(nch, sizeof *recs[i].lens);
            if (recs[i].ids == NULL || recs[i].lens == NULL) {
                st = psd_nomem(err);
                goto fail;
            }
        }
        for (k = 0; k < nch; k++) {
            CK(i16_(t, &recs[i].ids[k], err));
            CK(len_(t, t->version == PC_PSD_V2, &recs[i].lens[k], err));
            recs[i].n++;
        }
        CK(key(t, sig, err));
        if (memcmp(sig, "8BIM", 4) != 0) {
            st = psd_bad_sig(err, "8BIM (layer blend mode)", sig);
            goto fail;
        }
        CK(key(t, NULL, err));
        CK(raw(t, 4, err));
        CK(sized(t, false, fn_extra, &mc, err));
        recs[i].heights[0] = height(top, bottom);
        recs[i].heights[1] = mc.h_mask;
        recs[i].heights[2] = mc.h_real;
    }
    for (i = 0; i < n; i++) {
        for (k = 0; k < recs[i].n; k++) {
            chan_ctx cc;
            int16_t id = recs[i].ids[k];
            uint64_t len = recs[i].lens[k];
            if (len == 0) {
                continue;
            }
            if (len == 1) {
                st = psd_invalid(err, "channel data length 1");
                goto fail;
            }
            cc.rows = id == -2 ? recs[i].heights[1] : id == -3 ? recs[i].heights[2] : recs[i].heights[0];
            cc.long_counts = false;
            CK(region(t, len, channel_data, &cc, err));
        }
    }
    st = rest(t, err);
fail:
    for (i = 0; recs != NULL && i < n; i++) {
        free(recs[i].ids);
        free(recs[i].lens);
    }
    free(recs);
    return st;
}

/* ---- other block layouts ---- */

typedef struct meta_ctx {
    uint8_t key[4];
    bool same;
} meta_ctx;

static pc_status fn_meta_item(tx *t, void *ctx, pc_error *err)
{
    meta_ctx *m = ctx;
    static const char *const DESC[6] = {"cmls", "mlst", "cust", "tmln", "sgrp", "cinf"};
    int i;
    for (i = 0; i < 6; i++) {
        if (memcmp(m->key, DESC[i], 4) == 0) {
            return versioned_descriptor(t, err);
        }
    }
    if (memcmp(m->key, "mdyn", 4) == 0) {
        return u32s_to_end(t, err);
    }
    if (m->same || descriptor_shaped(t)) {
        return PC_OK;
    }
    return unsupported_code(err, "metadata item", m->key);
}

/* shmd: an item whose layout is unknown is dropped on its own (the count patched). */
static pc_status metadata_setting(tx *t, pc_error *err)
{
    uint32_t n;
    uint32_t i;
    uint32_t kept = 0;
    size_t count_at;
    static const uint8_t zero4[4];
    pc_status st;
    CK(psd_u32(&t->r, &n, err));
    CK(psd_check_count(&t->r, n, 16, err));
    count_at = t->out.len;
    CK(put(t, zero4, 4, err));
    for (i = 0; i < n; i++) {
        size_t before = t->out.len;
        meta_ctx m;
        pc_error e;
        pc_status rs;
        CK(key(t, NULL, err));
        CK(key(t, m.key, err));
        CK(u8_(t, NULL, err));
        CK(raw(t, 3, err));
        m.same = same_order(t);
        rs = sized(t, false, fn_meta_item, &m, &e);
        if (rs == PC_OK) {
            kept++;
        } else if (m.same || rs == PC_ERR_NOMEM) {
            if (err != NULL) {
                *err = e;
            }
            return rs;
        } else {
            char kt[32];
            pc_buf_truncate(&t->out, before);
            key_text(m.key, kt);
            CK(psd_warn(&t->warn, err, "shmd item %s dropped: %s", kt, e.msg));
        }
    }
    return patch_len(t, count_at, false, kept, err);
fail:
    return st;
}

static pc_status fn_pattern_channel(tx *t, void *ctx, pc_error *err)
{
    int32_t ct;
    int32_t cb;
    uint8_t comp;
    pc_status st;
    (void)ctx;
    CK(u32_(t, NULL, err));
    CK(i32_(t, &ct, err));
    CK(i32_(t, NULL, err));
    CK(i32_(t, &cb, err));
    CK(i32_(t, NULL, err));
    CK(u16_(t, NULL, err));
    CK(u8_(t, &comp, err));
    if (comp == 1) {
        pc_psd_version saved = t->version;
        t->version = PC_PSD_V1;
        st = rle_counts(t, height(ct, cb), err);
        t->version = saved;
        return st;
    }
    return PC_OK;
fail:
    return st;
}

static pc_status fn_pattern_array(tx *t, void *ctx, pc_error *err)
{
    uint32_t count;
    uint64_t i;
    pc_status st;
    (void)ctx;
    CK(i32s(t, 4, err));
    CK(u32_(t, &count, err));
    /* count.saturating_add(2) in the reference */
    for (i = 0; i < (count > 0xFFFFFFFDu ? 0xFFFFFFFFu : (uint64_t)count + 2); i++) {
        uint32_t written;
        psd_rd peek;
        uint32_t next;
        if (psd_remaining(&t->r) < 4) {
            break;
        }
        CK(u32_(t, &written, err));
        if (written == 0) {
            continue;
        }
        peek = t->r;
        CK(psd_u32(&peek, &next, err));
        if (next == 0) {
            CK(u32_(t, NULL, err));
            continue;
        }
        CK(sized(t, false, fn_pattern_channel, NULL, err));
    }
    return PC_OK;
fail:
    return st;
}

/* One pattern of a Patt block (the data after its u32 length). */
static pc_status fn_pattern(tx *t, void *ctx, pc_error *err)
{
    uint32_t mode;
    pc_status st;
    (void)ctx;
    CK(u32_(t, NULL, err));
    CK(u32_(t, &mode, err));
    CK(u16_(t, NULL, err));
    CK(u16_(t, NULL, err));
    CK(unicode(t, err));
    CK(pascal(t, 1, err));
    if (mode == 2) {
        psd_rd peek;
        uint32_t v;
        bool next_is_version;
        CK(raw(t, 768, err));
        /* Some writers follow the palette with 4 extra bytes before the array list version. */
        peek = t->r;
        next_is_version = psd_u32(&peek, &v, NULL) == PC_OK && v == 3;
        if (!next_is_version && psd_remaining(&t->r) >= 4) {
            CK(raw(t, 4, err));
        }
    }
    CK(u32_(t, NULL, err));
    return sized(t, false, fn_pattern_array, NULL, err);
fail:
    return st;
}

static pc_status effects_plane(tx *t, size_t rows, pc_error *err)
{
    chan_ctx c;
    c.rows = rows;
    c.long_counts = true;
    return sized(t, true, channel_data, &c, err);
}

static pc_status fn_fx_item(tx *t, void *ctx, pc_error *err)
{
    int32_t top;
    int32_t bottom;
    uint32_t max;
    uint32_t k;
    size_t rect_h;
    pc_status st;
    (void)ctx;
    CK(i32_(t, &top, err));
    CK(i32_(t, NULL, err));
    CK(i32_(t, &bottom, err));
    CK(i32_(t, NULL, err));
    rect_h = height(top, bottom);
    CK(u32_(t, NULL, err));
    CK(u32_(t, &max, err));
    if (max > PC_PSD_FX_MAX_SLOTS) {
        return psd_limit(err, "filter effects: too many channels");
    }
    for (k = 0; k < max + 2; k++) {
        uint32_t flag;
        CK(u32_(t, &flag, err));
        if (flag != 0) {
            CK(effects_plane(t, rect_h, err));
        }
    }
    return PC_OK;
fail:
    return st;
}

static pc_status fn_fx_section(tx *t, void *ctx, pc_error *err)
{
    pc_status st;
    (void)ctx;
    while (!psd_empty(&t->r)) {
        uint8_t flag;
        CK(pascal(t, 1, err));
        CK(u32_(t, NULL, err));
        CK(sized(t, true, fn_fx_item, NULL, err));
        if (!psd_empty(&t->r)) {
            CK(u8_(t, &flag, err));
            if (flag != 0) {
                int32_t top;
                int32_t bottom;
                CK(i32_(t, &top, err));
                CK(i32_(t, NULL, err));
                CK(i32_(t, &bottom, err));
                CK(i32_(t, NULL, err));
                CK(effects_plane(t, height(top, bottom), err));
            }
        }
    }
    return PC_OK;
fail:
    return st;
}

static pc_status filter_effects(tx *t, pc_error *err)
{
    pc_status st = u32_(t, NULL, err);
    return st == PC_OK ? sized(t, true, fn_fx_section, NULL, err) : st;
}

static pc_status colour(tx *t, pc_error *err)
{
    return u16s(t, 5, err);
}

static pc_status blend2(tx *t, pc_error *err)
{
    pc_status st = key(t, NULL, err);
    return st == PC_OK ? key(t, NULL, err) : st;
}

static pc_status u8s(tx *t, size_t n, pc_error *err)
{
    size_t i;
    pc_status st = PC_OK;
    for (i = 0; i < n && st == PC_OK; i++) {
        st = u8_(t, NULL, err);
    }
    return st;
}

static pc_status fn_legacy_effect(tx *t, void *ctx, pc_error *err)
{
    const uint8_t *k = ctx;
    pc_status st;
    if (memcmp(k, "cmnS", 4) == 0) {
        CK(u32_(t, NULL, err));
        CK(u8_(t, NULL, err));
        CK(u16_(t, NULL, err));
    } else if (memcmp(k, "dsdw", 4) == 0 || memcmp(k, "isdw", 4) == 0) {
        CK(u32_(t, NULL, err));
        CK(i32s(t, 4, err));
        CK(colour(t, err));
        CK(blend2(t, err));
        CK(u8s(t, 3, err));
        if (psd_remaining(&t->r) >= 10) {
            CK(colour(t, err));
        }
    } else if (memcmp(k, "oglw", 4) == 0) {
        CK(u32_(t, NULL, err));
        CK(i32s(t, 2, err));
        CK(colour(t, err));
        CK(blend2(t, err));
        CK(u8s(t, 2, err));
        if (psd_remaining(&t->r) >= 10) {
            CK(colour(t, err));
        }
    } else if (memcmp(k, "iglw", 4) == 0) {
        CK(u32_(t, NULL, err));
        CK(i32s(t, 2, err));
        CK(colour(t, err));
        CK(blend2(t, err));
        CK(u8s(t, 2, err));
        if (psd_remaining(&t->r) >= 11) {
            CK(u8_(t, NULL, err));
            CK(colour(t, err));
        }
    } else if (memcmp(k, "bevl", 4) == 0) {
        CK(u32_(t, NULL, err));
        CK(i32s(t, 3, err));
        CK(blend2(t, err));
        CK(blend2(t, err));
        CK(colour(t, err));
        CK(colour(t, err));
        CK(u8s(t, 6, err));
        if (psd_remaining(&t->r) >= 20) {
            CK(colour(t, err));
            CK(colour(t, err));
        }
    } else if (memcmp(k, "sofi", 4) == 0) {
        CK(u32_(t, NULL, err));
        CK(blend2(t, err));
        CK(colour(t, err));
        CK(u8s(t, 2, err));
        if (psd_remaining(&t->r) >= 10) {
            CK(colour(t, err));
        }
    }
    return PC_OK;
fail:
    return st;
}

static pc_status legacy_effects(tx *t, pc_error *err)
{
    uint16_t n;
    uint16_t i;
    pc_status st;
    CK(u16_(t, NULL, err));
    CK(u16_(t, &n, err));
    for (i = 0; i < n && psd_remaining(&t->r) >= 12; i++) {
        uint8_t k[4];
        CK(key(t, NULL, err));
        CK(key(t, k, err));
        CK(sized(t, false, fn_legacy_effect, k, err));
    }
    return PC_OK;
fail:
    return st;
}

static pc_status curves(tx *t, pc_error *err)
{
    uint32_t bits;
    int bit;
    pc_status st;
    CK(u8_(t, NULL, err));
    CK(u16_(t, NULL, err));
    CK(u32_(t, &bits, err));
    for (bit = 0; bit < 32; bit++) {
        uint16_t n;
        if ((bits & ((uint32_t)1 << bit)) == 0) {
            continue;
        }
        if (psd_remaining(&t->r) < 2) {
            break;
        }
        CK(u16_(t, &n, err));
        CK(u16s(t, (size_t)n * 2, err));
    }
    if (psd_remaining(&t->r) >= 10) {
        uint32_t n;
        uint32_t i;
        CK(key(t, NULL, err));
        CK(u16_(t, NULL, err));
        CK(u32_(t, &n, err));
        for (i = 0; i < n && psd_remaining(&t->r) >= 4; i++) {
            uint16_t pts;
            CK(u16_(t, NULL, err));
            CK(u16_(t, &pts, err));
            CK(u16s(t, (size_t)pts * 2, err));
        }
    }
    return PC_OK;
fail:
    return st;
}

static pc_status gradient_map(tx *t, pc_error *err)
{
    uint16_t version;
    uint16_t n;
    uint16_t i;
    int k;
    pc_status st;
    CK(u16_(t, &version, err));
    CK(u8_(t, NULL, err));
    CK(u8_(t, NULL, err));
    if (version == 3) {
        CK(key(t, NULL, err));
    }
    CK(unicode(t, err));
    CK(u16_(t, &n, err));
    for (i = 0; i < n && psd_remaining(&t->r) >= 20; i++) {
        CK(u32_(t, NULL, err));
        CK(u32_(t, NULL, err));
        CK(u16s(t, 6, err));
    }
    CK(u16_(t, &n, err));
    for (i = 0; i < n && psd_remaining(&t->r) >= 10; i++) {
        CK(u32_(t, NULL, err));
        CK(u32_(t, NULL, err));
        CK(u16_(t, NULL, err));
    }
    for (k = 0; k < 4; k++) {
        if (psd_remaining(&t->r) < 2) {
            return PC_OK;
        }
        CK(u16_(t, NULL, err));
    }
    if (psd_remaining(&t->r) >= 4) {
        CK(u32_(t, NULL, err));
    }
    return u16s_to_end(t, err);
fail:
    return st;
}

static pc_status fn_linked_item(tx *t, void *ctx, pc_error *err)
{
    uint8_t kind[4];
    uint32_t version;
    uint64_t data_len;
    uint8_t has_desc;
    bool fe;
    pc_status st;
    (void)ctx;
    CK(key(t, kind, err));
    CK(u32_(t, &version, err));
    CK(pascal(t, 1, err));
    CK(unicode(t, err));
    CK(key(t, NULL, err));
    CK(key(t, NULL, err));
    CK(u64_(t, &data_len, err));
    CK(u8_(t, &has_desc, err));
    if (has_desc != 0) {
        CK(versioned_descriptor(t, err));
    }
    fe = memcmp(kind, "liFE", 4) == 0;
    if (fe && version > 3) {
        CK(u32_(t, NULL, err));
        CK(u8s(t, 4, err));
        CK(F64(t, err));
    }
    if (fe) {
        CK(u64_(t, NULL, err));
    }
    if (memcmp(kind, "liFA", 4) == 0) {
        CK(raw(t, 8, err));
    }
    if (data_len > SIZE_MAX) {
        return psd_limit(err, "linked file length");
    }
    if (memcmp(kind, "liFD", 4) == 0) {
        CK(raw(t, (size_t)data_len, err));
    }
    if (version >= 5) {
        CK(unicode(t, err));
    }
    if (version >= 6) {
        CK(F64(t, err));
    }
    if (version >= 7) {
        CK(u8_(t, NULL, err));
    }
    if (fe && version == 2) {
        CK(raw(t, (size_t)data_len, err));
    }
    return PC_OK;
fail:
    return st;
}

static pc_status linked_layers(tx *t, pc_error *err)
{
    pc_status st;
    while (psd_remaining(&t->r) >= 8) {
        psd_rd peek = t->r;
        uint64_t len;
        size_t pad;
        CK(psd_u64(&peek, &len, err));
        CK(sized(t, true, fn_linked_item, NULL, err));
        pad = (size_t)((4 - len % 4) % 4);
        CK(raw(t, pad < psd_remaining(&t->r) ? pad : psd_remaining(&t->r), err));
    }
    return PC_OK;
fail:
    return st;
}

static pc_status fn_anno_data(tx *t, void *ctx, pc_error *err)
{
    pc_status st = key(t, NULL, err);
    (void)ctx;
    return st == PC_OK ? u32_(t, NULL, err) : st;
}

static pc_status fn_anno(tx *t, void *ctx, pc_error *err)
{
    pc_status st;
    (void)ctx;
    CK(key(t, NULL, err));
    CK(u8_(t, NULL, err));
    CK(u8_(t, NULL, err));
    CK(u16_(t, NULL, err));
    CK(i32s(t, 8, err));
    CK(u16s(t, 5, err));
    CK(pascal(t, 2, err));
    CK(pascal(t, 2, err));
    CK(pascal(t, 2, err));
    return sized_inclusive(t, fn_anno_data, NULL, err);
fail:
    return st;
}

static pc_status annotations(tx *t, pc_error *err)
{
    uint32_t n;
    uint32_t i;
    pc_status st;
    CK(u16_(t, NULL, err));
    CK(u16_(t, NULL, err));
    CK(u32_(t, &n, err));
    for (i = 0; i < n && psd_remaining(&t->r) >= 4; i++) {
        CK(sized_inclusive(t, fn_anno, NULL, err));
    }
    return PC_OK;
fail:
    return st;
}

static bool is(const uint8_t k[4], const char *s)
{
    return memcmp(k, s, 4) == 0;
}

static bool is_any(const uint8_t k[4], const char *const *list, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (is(k, list[i])) {
            return true;
        }
    }
    return false;
}

/* Transcodes a block's data by key; *handled false for keys without a known layout. */
static pc_status block_body(tx *t, const uint8_t k[4], bool *handled, pc_error *err)
{
    static const char *const U32[5] = {"lyid", "lspf", "lyvr", "sn2P", "vowv"};
    static const char *const FLAG[7] = {"clbl", "infx", "knko", "tsly", "lmgm", "vmgm", "iOpa"};
    static const char *const VFX[4] = {"lfx2", "lmfx", "lfxs", "CAI "};
    static const char *const VD[16] = {"SoCo", "GdFl", "PtFl", "vstk", "blwh", "vibA", "CgEd", "artb",
                                       "artd", "cinf", "cust", "PxSD", "GenI", "OCIO", "extn", "pths"};
    static const char *const LNK[4] = {"lnk2", "lnk3", "lnkD", "lnkE"};
    pc_status st = PC_OK;
    *handled = true;
    if (is(k, "Layr") || is(k, "Lr16") || is(k, "Lr32")) {
        CK(layer_info(t, err));
    } else if (is(k, "luni")) {
        CK(unicode(t, err));
    } else if (is(k, "lsct") || is(k, "lsdk")) {
        CK(u32_(t, NULL, err));
        if (psd_remaining(&t->r) >= 8) {
            CK(key(t, NULL, err));
            CK(key(t, NULL, err));
        }
        if (psd_remaining(&t->r) >= 4) {
            CK(u32_(t, NULL, err));
        }
    } else if (is_any(k, U32, 5)) {
        CK(u32_(t, NULL, err));
    } else if (is(k, "Txt2")) {
        CK(rest(t, err));
    } else if (is(k, "lnsr")) {
        CK(key(t, NULL, err));
    } else if (is_any(k, FLAG, 7)) {
        CK(u8_(t, NULL, err));
    } else if (is(k, "lclr")) {
        CK(u16s_upto(t, 4, err));
    } else if (is(k, "fxrp")) {
        while (psd_remaining(&t->r) >= 8) {
            CK(F64(t, err));
        }
    } else if (is(k, "brst")) {
        CK(u32s_to_end(t, err));
    } else if (is(k, "LMsk")) {
        CK(u16s_upto(t, 6, err));
    } else if (is(k, "FMsk")) {
        CK(u16s_to_end(t, err));
    } else if (is(k, "shmd")) {
        CK(metadata_setting(t, err));
    } else if (is_any(k, VFX, 4)) {
        CK(u32_(t, NULL, err));
        CK(versioned_descriptor(t, err));
    } else if (is_any(k, VD, 16)) {
        CK(versioned_descriptor(t, err));
    } else if (is(k, "clrL")) {
        CK(u16_(t, NULL, err));
        CK(versioned_descriptor(t, err));
    } else if (is_any(k, LNK, 4)) {
        CK(linked_layers(t, err));
    } else if (is(k, "Anno")) {
        CK(annotations(t, err));
    } else if (is(k, "vogk")) {
        CK(u32_(t, NULL, err));
        CK(versioned_descriptor(t, err));
    } else if (is(k, "vscg")) {
        CK(key(t, NULL, err));
        CK(versioned_descriptor(t, err));
    } else if (is(k, "SoLd") || is(k, "SoLE")) {
        CK(key(t, NULL, err));
        CK(u32_(t, NULL, err));
        CK(versioned_descriptor(t, err));
    } else if (is(k, "PlLd")) {
        CK(key(t, NULL, err));
        CK(u32_(t, NULL, err));
        CK(pascal(t, 1, err));
        CK(i32s(t, 4, err));
        CK(f64s(t, 8, err));
        CK(u32_(t, NULL, err));
        CK(versioned_descriptor(t, err));
    } else if (is(k, "TySh")) {
        CK(u16_(t, NULL, err));
        CK(f64s(t, 6, err));
        CK(u16_(t, NULL, err));
        CK(versioned_descriptor(t, err));
        CK(u16_(t, NULL, err));
        CK(versioned_descriptor(t, err));
        CK(i32s(t, 4, err));
    } else if (is(k, "vmsk") || is(k, "vsms")) {
        CK(u32_(t, NULL, err));
        CK(u32_(t, NULL, err));
        CK(path_records(t, err));
    } else if (is(k, "Patt") || is(k, "Pat2") || is(k, "Pat3")) {
        while (psd_remaining(&t->r) >= 4) {
            psd_rd peek = t->r;
            uint32_t len;
            size_t pad;
            CK(psd_u32(&peek, &len, err));
            if (len == 0) {
                CK(u32_(t, NULL, err));
                continue;
            }
            CK(sized(t, false, fn_pattern, NULL, err));
            pad = (size_t)((4 - len % 4) % 4);
            CK(raw(t, pad < psd_remaining(&t->r) ? pad : psd_remaining(&t->r), err));
        }
    } else if (is(k, "FEid") || is(k, "FXid")) {
        CK(filter_effects(t, err));
    } else if (is(k, "lrFX")) {
        CK(legacy_effects(t, err));
    } else if (is(k, "levl")) {
        int i;
        CK(u16_(t, NULL, err));
        for (i = 0; i < 29 * 5 && psd_remaining(&t->r) >= 2; i++) {
            CK(u16_(t, NULL, err));
        }
        if (psd_remaining(&t->r) >= 4) {
            CK(key(t, NULL, err));
        }
        CK(u16s_to_end(t, err));
    } else if (is(k, "curv")) {
        CK(curves(t, err));
    } else if (is(k, "hue2")) {
        CK(u16_(t, NULL, err));
        CK(u8_(t, NULL, err));
        CK(u8_(t, NULL, err));
        CK(u16s_to_end(t, err));
    } else if (is(k, "brit")) {
        CK(u16s_upto(t, 3, err));
    } else if (is(k, "thrs") || is(k, "post")) {
        CK(u16_(t, NULL, err));
    } else if (is(k, "nvrt")) {
        /* no fields */
    } else if (is(k, "expA")) {
        CK(u16_(t, NULL, err));
        CK(F32(t, err));
        CK(F32(t, err));
        CK(F32(t, err));
    } else if (is(k, "blnc")) {
        CK(u16s_upto(t, 9, err));
    } else if (is(k, "mixr") || is(k, "selc")) {
        CK(u16s_to_end(t, err));
    } else if (is(k, "phfl")) {
        uint16_t v;
        CK(u16_(t, &v, err));
        if (v == 2) {
            CK(u16s(t, 5, err));
            CK(u32_(t, NULL, err));
            CK(u8_(t, NULL, err));
        } else if (v == 3) {
            CK(i32s(t, 4, err));
            CK(u8_(t, NULL, err));
        } else {
            return psd_unsupported(err, "photo filter version %u", (unsigned)v);
        }
    } else if (is(k, "grdm")) {
        CK(gradient_map(t, err));
    } else if (!same_order(t)) {
        *handled = descriptor_shaped(t);
    } else {
        *handled = false;
    }
    return PC_OK;
fail:
    return st;
}

/* ---- public transcoding ---- */

typedef pc_status (*tx_root)(tx *t, pc_error *err);

static pc_status run(const uint8_t *data, size_t len, pc_psd_version v, bool from_le, bool to_le, tx_root f,
                     pc_psd_bytes *out, pc_psd_warnings *warnings, pc_error *err)
{
    tx t;
    pc_status st;
    tx_init(&t, data, len, v, from_le, to_le);
    st = f(&t, err);
    if (st != PC_OK) {
        tx_fini(&t);
        return st;
    }
    pc_psd_bytes_fini(out);
    out->data = t.out.data;
    out->len = t.out.len;
    pc_buf_init(&t.out);
    if (warnings != NULL) {
        *warnings = t.warn;
        t.warn.v = NULL;
        t.warn.n = 0;
    }
    tx_fini(&t);
    return PC_OK;
}

pc_status pc_psd_transcode_blocks(const uint8_t *data, size_t len, pc_psd_version v, pc_psd_byte_order from,
                                  pc_psd_byte_order to, pc_psd_bytes *out, pc_psd_warnings *warnings, pc_error *err)
{
    return run(data, len, v, from == PC_PSD_LITTLE, to == PC_PSD_LITTLE, blocks, out, warnings, err);
}

pc_status pc_psd_transcode_layer_info(const uint8_t *data, size_t len, pc_psd_version v, pc_psd_byte_order from,
                                      pc_psd_byte_order to, pc_psd_bytes *out, pc_psd_warnings *warnings,
                                      pc_error *err)
{
    return run(data, len, v, from == PC_PSD_LITTLE, to == PC_PSD_LITTLE, layer_info, out, warnings, err);
}

pc_status pc_psd_transcode_resources(const uint8_t *data, size_t len, pc_psd_byte_order from, pc_psd_byte_order to,
                                     pc_psd_bytes *out, pc_psd_warnings *warnings, pc_error *err)
{
    return run(data, len, PC_PSD_V1, from == PC_PSD_LITTLE, to == PC_PSD_LITTLE, resources, out, warnings, err);
}

/* ---- ImageSourceData ---- */

void pc_psd_isd_init(pc_psd_isd *d)
{
    memset(d, 0, sizeof *d);
    d->version = PC_PSD_V1;
    d->byte_order = PC_PSD_BIG;
    memcpy(d->layer_info_key, "Layr", 4);
}

void pc_psd_isd_fini(pc_psd_isd *d)
{
    if (d == NULL) {
        return;
    }
    pc_psd_layer_info_fini(&d->layer_info);
    pc_psd_bytes_fini(&d->global_mask);
    pc_psd_blocks_free(d->global_blocks, d->nglobal);
    pc_psd_bytes_fini(&d->trailing);
    pc_psd_isd_init(d);
}

bool pc_psd_isd_eq(const pc_psd_isd *a, const pc_psd_isd *b)
{
    return a->version == b->version && a->byte_order == b->byte_order && a->has_layer_info == b->has_layer_info &&
           (!a->has_layer_info || pc_psd_layer_info_eq(&a->layer_info, &b->layer_info)) &&
           memcmp(a->layer_info_key, b->layer_info_key, 4) == 0 && a->has_global_mask == b->has_global_mask &&
           (!a->has_global_mask || pc_psd_bytes_eq(&a->global_mask, &b->global_mask)) &&
           pc_psd_blocks_eq(a->global_blocks, a->nglobal, b->global_blocks, b->nglobal) &&
           pc_psd_bytes_eq(&a->trailing, &b->trailing);
}

pc_status pc_psd_isd_clone(const pc_psd_isd *d, pc_psd_isd *out, pc_error *err)
{
    pc_psd_isd c;
    pc_status st;
    pc_psd_isd_init(&c);
    c.version = d->version;
    c.byte_order = d->byte_order;
    c.has_layer_info = d->has_layer_info;
    memcpy(c.layer_info_key, d->layer_info_key, 4);
    c.has_global_mask = d->has_global_mask;
    if (d->has_layer_info) {
        CK(pc_psd_layer_info_clone(&d->layer_info, &c.layer_info, err));
    }
    CK(pc_psd_bytes_clone(&d->global_mask, &c.global_mask, err));
    CK(pc_psd_blocks_clone(d->global_blocks, d->nglobal, &c.global_blocks, err));
    c.nglobal = d->nglobal;
    CK(pc_psd_bytes_clone(&d->trailing, &c.trailing, err));
    *out = c;
    return PC_OK;
fail:
    pc_psd_isd_fini(&c);
    return st;
}

static pc_status block_order(const uint8_t *rest_, size_t n, pc_psd_byte_order *out, pc_error *err)
{
    if (n < 4 || memcmp(rest_, "8BIM", 4) == 0 || memcmp(rest_, "8B64", 4) == 0) {
        *out = PC_PSD_BIG;
        return PC_OK;
    }
    if (memcmp(rest_, "MIB8", 4) == 0 || memcmp(rest_, "46B8", 4) == 0) {
        *out = PC_PSD_LITTLE;
        return PC_OK;
    }
    return psd_bad_sig(err, "8BIM or MIB8 (Photoshop image source data block)", rest_);
}

/* Removes blocks[i] from the array, returning it. */
static pc_psd_block take_block(pc_psd_block *b, size_t *n, size_t i)
{
    pc_psd_block x = b[i];
    memmove(&b[i], &b[i + 1], (*n - i - 1) * sizeof *b);
    (*n)--;
    return x;
}

pc_status pc_psd_isd_parse(const uint8_t *data, size_t len, pc_psd_isd *out, pc_psd_warnings *warnings,
                           pc_error *err)
{
    pc_psd_isd d;
    pc_psd_bytes big = {NULL, 0};
    pc_psd_warnings w = {NULL, 0};
    const uint8_t *body;
    size_t blen;
    psd_rd r;
    size_t i;
    pc_status st;
    pc_psd_isd_init(&d);
    if (warnings != NULL) {
        warnings->v = NULL;
        warnings->n = 0;
    }
    if (len >= 36 && memcmp(data, PC_PSD_ISD_SIGNATURE, 36) == 0) {
        d.version = PC_PSD_V1;
    } else if (len >= 36 && memcmp(data, PC_PSD_ISD_SIGNATURE_PSB, 36) == 0) {
        d.version = PC_PSD_V2;
    } else {
        return psd_invalid(err, "not Photoshop image source data (missing \"Adobe Photoshop Document Data Block\")");
    }
    body = data + 36;
    blen = len - 36;
    CK(block_order(body, blen, &d.byte_order, err));
    if (d.byte_order == PC_PSD_LITTLE) {
        CK(pc_psd_transcode_blocks(body, blen, d.version, PC_PSD_LITTLE, PC_PSD_BIG, &big, &w, err));
        body = big.data;
        blen = big.len;
    }
    r = psd_rd_make(body, blen);
    CK(psd_read_blocks(&r, d.version, 2, &d.global_blocks, &d.nglobal, &d.trailing, err));
    /* Blocks in a TIFF are padded to 4 (to_bytes re-pads them): zero padding is the default. */
    for (i = 0; i < d.nglobal; i++) {
        pc_psd_opt_bytes *p = &d.global_blocks[i].padding;
        if (p->has && p->v.len <= 3) {
            size_t k;
            bool zero = true;
            for (k = 0; k < p->v.len; k++) {
                zero = zero && p->v.data[k] == 0;
            }
            if (zero) {
                pc_psd_opt_bytes_fini(p);
            }
        }
    }
    for (i = 0; i < d.nglobal; i++) {
        const pc_psd_block *b = &d.global_blocks[i];
        if (pc_psd_block_is(b, "Layr") || pc_psd_block_is(b, "Lr16") || pc_psd_block_is(b, "Lr32")) {
            pc_psd_block x = take_block(d.global_blocks, &d.nglobal, i);
            st = pc_psd_layer_info_parse(x.data.data, x.data.len, d.version, &d.layer_info, err);
            memcpy(d.layer_info_key, x.key, 4);
            pc_psd_block_fini(&x);
            if (st != PC_OK) {
                goto fail;
            }
            d.has_layer_info = true;
            break;
        }
    }
    for (i = 0; i < d.nglobal; i++) {
        if (pc_psd_block_is(&d.global_blocks[i], "LMsk")) {
            pc_psd_block x = take_block(d.global_blocks, &d.nglobal, i);
            d.global_mask = x.data;
            x.data.data = NULL;
            x.data.len = 0;
            pc_psd_block_fini(&x);
            d.has_global_mask = true;
            break;
        }
    }
    pc_psd_bytes_fini(&big);
    if (warnings != NULL) {
        *warnings = w;
    } else {
        pc_psd_warnings_fini(&w);
    }
    *out = d;
    return PC_OK;
fail:
    pc_psd_bytes_fini(&big);
    pc_psd_warnings_fini(&w);
    pc_psd_isd_fini(&d);
    return st;
}

pc_status pc_psd_isd_to_bytes(const pc_psd_isd *d, pc_psd_byte_order order, pc_psd_bytes *out,
                              pc_psd_warnings *warnings, pc_error *err)
{
    psd_w w;
    pc_psd_bytes body = {NULL, 0};
    pc_psd_bytes conv = {NULL, 0};
    pc_psd_warnings wn = {NULL, 0};
    static const uint8_t zeros[4];
    size_t i;
    pc_status st;
    if (warnings != NULL) {
        warnings->v = NULL;
        warnings->n = 0;
    }
    psd_w_init(&w, err);
    if (d->has_layer_info) {
        pc_psd_layer_info info = d->layer_info; /* shallow: only the padding differs */
        uint64_t ulen;
        size_t pad;
        pc_psd_bytes li = {NULL, 0};
        st = pc_psd_layer_info_unpadded_len(&d->layer_info, d->version, &ulen, err);
        if (st != PC_OK) {
            psd_w_fini(&w);
            return st;
        }
        pad = (size_t)((4 - ulen % 4) % 4);
        info.padding.has = true;
        info.padding.v.data = (uint8_t *)(uintptr_t)zeros;
        info.padding.v.len = pad;
        st = pc_psd_layer_info_to_bytes(&info, d->version, &li, err);
        if (st != PC_OK) {
            psd_w_fini(&w);
            return st;
        }
        {
            pc_psd_block b;
            memset(&b, 0, sizeof b);
            memcpy(b.signature, "8BIM", 4);
            memcpy(b.key, d->layer_info_key, 4);
            b.data = li;
            b.padding.has = true;
            b.padding.v.data = (uint8_t *)(uintptr_t)zeros;
            b.padding.v.len = (4 - li.len % 4) % 4;
            psd_write_block(&w, &b, d->version, 2);
        }
        pc_psd_bytes_fini(&li);
    }
    if (d->has_global_mask) {
        pc_psd_block b;
        memset(&b, 0, sizeof b);
        memcpy(b.signature, "8BIM", 4);
        memcpy(b.key, "LMsk", 4);
        b.data = d->global_mask;
        b.padding.has = true;
        b.padding.v.data = (uint8_t *)(uintptr_t)zeros;
        b.padding.v.len = (4 - d->global_mask.len % 4) % 4;
        psd_write_block(&w, &b, d->version, 2);
    }
    for (i = 0; i < d->nglobal; i++) {
        pc_psd_block b = d->global_blocks[i];
        if (!b.padding.has) {
            b.padding.has = true;
            b.padding.v.data = (uint8_t *)(uintptr_t)zeros;
            b.padding.v.len = (4 - b.data.len % 4) % 4;
        }
        psd_write_block(&w, &b, d->version, 2);
    }
    psd_put(&w, d->trailing.data, d->trailing.len);
    st = psd_w_finish(&w, &body);
    if (st != PC_OK) {
        return st;
    }
    if (order == PC_PSD_LITTLE) {
        st = pc_psd_transcode_blocks(body.data, body.len, d->version, PC_PSD_BIG, PC_PSD_LITTLE, &conv, &wn, err);
        pc_psd_bytes_fini(&body);
        if (st != PC_OK) {
            return st;
        }
        body = conv;
    }
    psd_w_init(&w, err);
    psd_put(&w, d->version == PC_PSD_V2 ? PC_PSD_ISD_SIGNATURE_PSB : PC_PSD_ISD_SIGNATURE, 36);
    psd_put(&w, body.data, body.len);
    pc_psd_bytes_fini(&body);
    st = psd_w_finish(&w, out);
    if (st != PC_OK) {
        pc_psd_warnings_fini(&wn);
        return st;
    }
    if (warnings != NULL) {
        *warnings = wn;
    } else {
        pc_psd_warnings_fini(&wn);
    }
    return PC_OK;
}

pc_status pc_psd_isd_from_psd(const pc_psd_file *f, pc_psd_isd *out, pc_error *err)
{
    pc_psd_isd d;
    pc_status st;
    pc_psd_isd_init(&d);
    d.version = f->header.version;
    if (f->placement.where == PC_PSD_LI_GLOBAL_BLOCK) {
        memcpy(d.layer_info_key, f->placement.key, 4);
    } else {
        pc_psd_layer_info_key(f->header.depth, d.layer_info_key);
    }
    d.has_layer_info = f->has_layer_info;
    if (f->has_layer_info) {
        CK(pc_psd_layer_info_clone(&f->layer_info, &d.layer_info, err));
    }
    d.has_global_mask = f->has_global_mask;
    CK(pc_psd_bytes_clone(&f->global_mask, &d.global_mask, err));
    CK(pc_psd_blocks_clone(f->global_blocks, f->nglobal, &d.global_blocks, err));
    d.nglobal = f->nglobal;
    CK(pc_psd_bytes_clone(&f->layer_mask_trailing, &d.trailing, err));
    *out = d;
    return PC_OK;
fail:
    pc_psd_isd_fini(&d);
    return st;
}

pc_status pc_psd_isd_into_psd(pc_psd_isd *d, pc_psd_header header, pc_psd_resource **resources, size_t nres,
                              uint16_t image_compression, pc_psd_bytes *image_data, pc_psd_file *out, pc_error *err)
{
    pc_psd_file f;
    (void)err;
    pc_psd_file_init(&f);
    f.header = header;
    f.resources = *resources;
    f.nresources = nres;
    *resources = NULL;
    f.has_layer_info = d->has_layer_info;
    f.layer_info = d->layer_info;
    if ((memcmp(d->layer_info_key, "Lr16", 4) == 0 || memcmp(d->layer_info_key, "Lr32", 4) == 0) &&
        d->has_layer_info) {
        f.placement.where = PC_PSD_LI_GLOBAL_BLOCK;
        f.placement.index = 0;
        memcpy(f.placement.signature, "8BIM", 4);
        memcpy(f.placement.key, d->layer_info_key, 4);
    }
    f.has_global_mask = d->has_global_mask;
    f.global_mask = d->global_mask;
    f.global_blocks = d->global_blocks;
    f.nglobal = d->nglobal;
    f.layer_mask_trailing = d->trailing;
    f.image_compression = image_compression;
    f.image_data = *image_data;
    image_data->data = NULL;
    image_data->len = 0;
    memset(d, 0, sizeof *d);
    pc_psd_isd_init(d);
    *out = f;
    return PC_OK;
}

pc_status pc_psd_tiff_resources_parse(const uint8_t *data, size_t len, pc_psd_resource **out, size_t *n,
                                      pc_psd_warnings *warnings, pc_error *err)
{
    pc_psd_byte_order order = PC_PSD_BIG;
    pc_psd_bytes big = {NULL, 0};
    pc_psd_warnings w = {NULL, 0};
    pc_psd_resource *v = NULL;
    size_t count = 0;
    size_t cap = 0;
    psd_rd r;
    pc_status st;
    static const char *const SIGS[5] = {"8BIM", "MeSa", "AgHg", "PHUT", "DCSR"};
    if (warnings != NULL) {
        warnings->v = NULL;
        warnings->n = 0;
    }
    CK(block_order(data, len, &order, err));
    if (order == PC_PSD_LITTLE) {
        CK(pc_psd_transcode_resources(data, len, PC_PSD_LITTLE, PC_PSD_BIG, &big, &w, err));
        data = big.data;
        len = big.len;
    }
    r = psd_rd_make(data, len);
    while (!psd_empty(&r)) {
        pc_psd_resource res;
        uint32_t dlen;
        int s;
        size_t k;
        bool zeros = true;
        /* Photoshop pads the tag to an even length; a lone trailing byte is not a resource. */
        if (psd_remaining(&r) < 12) {
            for (k = 0; k < psd_remaining(&r); k++) {
                zeros = zeros && psd_rest(&r)[k] == 0;
            }
            if (zeros) {
                break;
            }
        }
        memset(&res, 0, sizeof res);
        CK(psd_arr4(&r, res.signature, err));
        for (s = 0; s < 5 && memcmp(res.signature, SIGS[s], 4) != 0; s++) {
        }
        if (s == 5) {
            st = psd_bad_sig(err, "8BIM resource", res.signature);
            goto fail;
        }
        st = psd_u16(&r, &res.id, err);
        if (st == PC_OK) {
            st = psd_read_pascal(&r, 2, &res.name, err);
        }
        if (st == PC_OK) {
            st = psd_u32(&r, &dlen, err);
        }
        if (st == PC_OK) {
            st = psd_take_bytes(&r, dlen, &res.data, err);
        }
        if (st == PC_OK && dlen % 2 == 1) {
            st = psd_skip(&r, 1, err);
        }
        if (st == PC_OK && count == cap) {
            pc_psd_resource *g = pc_grow_array(v, &cap, count + 1, sizeof *g);
            if (g == NULL) {
                st = psd_nomem(err);
            } else {
                v = g;
            }
        }
        if (st != PC_OK) {
            pc_psd_resource_fini(&res);
            goto fail;
        }
        v[count++] = res;
    }
    pc_psd_bytes_fini(&big);
    if (warnings != NULL) {
        *warnings = w;
    } else {
        pc_psd_warnings_fini(&w);
    }
    *out = v;
    *n = count;
    return PC_OK;
fail:
    pc_psd_bytes_fini(&big);
    pc_psd_warnings_fini(&w);
    pc_psd_resources_free(v, count);
    return st;
}

pc_status pc_psd_tiff_resources_to_bytes(const pc_psd_resource *res, size_t n, pc_psd_bytes *out, pc_error *err)
{
    psd_w w;
    pc_psd_bytes sec = {NULL, 0};
    pc_status st;
    psd_w_init(&w, err);
    psd_write_resources(&w, res, n);
    st = psd_w_finish(&w, &sec);
    if (st != PC_OK) {
        return st;
    }
    /* Without the section's 4-byte length. */
    st = pc_psd_bytes_set(out, sec.data + 4, sec.len - 4, err);
    pc_psd_bytes_fini(&sec);
    return st;
}
