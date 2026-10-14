/* Embedded smart-object files in lnk2 / lnk3 / lnkD blocks (port of crates/io/src/linked.rs). */
#include "io/linked.h"

#include <stdlib.h>
#include <string.h>

#include "base/blob.h"
#include "base/bytes.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/str.h"
#include "base/utf8.h"
#include "codecs/codecs.h"
#include "io/io.h"
#include "psd/descriptor.h"
#include "psd/tagged.h"

void pc_io_linked_file_fini(pc_io_linked_file *f)
{
    if (f == NULL) {
        return;
    }
    pc_str_fini(&f->uuid);
    pc_str_fini(&f->file_name);
    free(f->bytes);
    f->bytes = NULL;
    f->len = 0;
}

void pc_io_linked_files_fini(pc_io_linked_files *l)
{
    if (l == NULL) {
        return;
    }
    for (size_t i = 0; i < l->n; i++) {
        pc_io_linked_file_fini(&l->v[i]);
    }
    free(l->v);
    l->v = NULL;
    l->n = 0;
    l->cap = 0;
}

/* A bounds-checked big-endian cursor (the reference's Rd). */
typedef struct rd {
    const uint8_t *d;
    size_t len;
    size_t p;
} rd;

static const uint8_t *take(rd *r, size_t n)
{
    const uint8_t *s;
    if (r->p > r->len || n > r->len - r->p) {
        return NULL;
    }
    s = r->d + r->p;
    r->p += n;
    return s;
}

static bool rd_u8(rd *r, uint8_t *v)
{
    const uint8_t *b = take(r, 1);
    if (b != NULL) {
        *v = b[0];
    }
    return b != NULL;
}

static bool rd_u32(rd *r, uint32_t *v)
{
    const uint8_t *b = take(r, 4);
    if (b != NULL) {
        *v = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    }
    return b != NULL;
}

static uint64_t be64(const uint8_t *b)
{
    uint64_t v = 0;
    for (size_t i = 0; i < 8; i++) {
        v = v << 8 | b[i];
    }
    return v;
}

static bool rd_u64(rd *r, uint64_t *v)
{
    const uint8_t *b = take(r, 8);
    if (b != NULL) {
        *v = be64(b);
    }
    return b != NULL;
}

/* Lossy UTF-8 of bytes into a pc_str. */
static pc_status lossy_str(const uint8_t *b, size_t n, pc_str *out, pc_error *err)
{
    pc_buf buf = PC_BUF_INIT;
    size_t len;
    char *p;
    pc_status st = pc_utf8_lossy(b, n, &buf, err);
    if (st != PC_OK) {
        pc_buf_fini(&buf);
        return st;
    }
    p = (char *)pc_buf_take(&buf, &len);
    if (p == NULL) {
        return pc_str_set(out, "", 0, err);
    }
    *out = pc_str_adopt(p, len);
    return PC_OK;
}

/* usize::try_from(u64). */
static bool to_size(uint64_t v, size_t *out)
{
#if SIZE_MAX < UINT64_MAX
    if (v > SIZE_MAX) {
        return false;
    }
#endif
    *out = (size_t)v;
    return true;
}

static size_t round4(size_t v)
{
    return v > SIZE_MAX - 3 ? SIZE_MAX : (v + 3) & ~(size_t)3;
}

/* Parses one item. *ok false for malformed data and non-liFD items. */
static pc_status parse_item(const uint8_t *item, size_t len, bool *ok, pc_io_linked_file *out, pc_error *err)
{
    rd r = {item, len, 0};
    const uint8_t *kind;
    const uint8_t *idb;
    const uint8_t *raw;
    const uint8_t *bytes;
    uint32_t version;
    uint32_t units;
    uint64_t dlen64;
    size_t dlen;
    size_t ulen;
    uint8_t n;
    uint8_t has_desc;
    pc_io_linked_file f;
    pc_buf name = PC_BUF_INIT;
    pc_status st;
    size_t name_len;
    *ok = false;
    if ((kind = take(&r, 4)) == NULL || !rd_u32(&r, &version) || !rd_u8(&r, &n) ||
        (idb = take(&r, n)) == NULL || !rd_u32(&r, &units) || !pc_size_mul(units, 2, &ulen) ||
        (raw = take(&r, ulen)) == NULL || take(&r, 8) == NULL || !rd_u64(&r, &dlen64) ||
        !to_size(dlen64, &dlen) || !rd_u8(&r, &has_desc)) {
        return PC_OK;
    }
    if (has_desc != 0) {
        pc_psd_desc d;
        size_t used = 0;
        st = pc_psd_vdesc_parse_prefix(item + r.p, len - r.p, &d, &used, err);
        if (st == PC_ERR_NOMEM) {
            return st;
        }
        if (st != PC_OK) {
            return PC_OK;
        }
        pc_psd_desc_fini(&d);
        r.p += used;
    }
    if (memcmp(kind, "liFD", 4) != 0 || (bytes = take(&r, dlen)) == NULL) {
        return PC_OK;
    }
    memset(&f, 0, sizeof f);
    st = lossy_str(idb, n, &f.uuid, err);
    if (st == PC_OK) {
        st = pc_utf16be_lossy(raw, units, &name, err);
    }
    if (st == PC_OK) {
        name_len = name.len;
        while (name_len > 0 && name.data[name_len - 1] == 0) {
            name_len--;
        }
        st = pc_str_set(&f.file_name, name_len > 0 ? (const char *)name.data : "", name_len, err);
    }
    pc_buf_fini(&name);
    if (st == PC_OK) {
        f.bytes = pc_malloc(dlen);
        if (f.bytes == NULL) {
            st = pc_io_nomem(err, "copying an embedded file");
        } else {
            if (dlen > 0) {
                memcpy(f.bytes, bytes, dlen);
            }
            f.len = dlen;
        }
    }
    if (st != PC_OK) {
        pc_io_linked_file_fini(&f);
        return st;
    }
    *out = f;
    *ok = true;
    return PC_OK;
}

pc_status pc_io_parse_linked_files(const uint8_t *data, size_t len, pc_io_linked_files *out, pc_error *err)
{
    pc_io_linked_files l = {NULL, 0, 0};
    rd r = {data, len, 0};
    uint64_t ilen;
    while (rd_u64(&r, &ilen)) {
        const uint8_t *item;
        pc_io_linked_file f;
        bool ok;
        pc_status st;
        size_t il;
        if (!to_size(ilen, &il) || (item = take(&r, il)) == NULL) {
            break;
        }
        st = parse_item(item, il, &ok, &f, err);
        if (st == PC_OK && ok) {
            pc_io_linked_file *v = pc_grow_array(l.v, &l.cap, l.n + 1, sizeof *v);
            if (v == NULL) {
                pc_io_linked_file_fini(&f);
                st = pc_io_nomem(err, "listing embedded files");
            } else {
                l.v = v;
                v[l.n++] = f;
            }
        }
        if (st != PC_OK) {
            pc_io_linked_files_fini(&l);
            return st;
        }
        r.p = round4(r.p);
    }
    *out = l;
    return PC_OK;
}

static bool is_link_key(const uint8_t k[4])
{
    return memcmp(k, "lnk2", 4) == 0 || memcmp(k, "lnk3", 4) == 0 || memcmp(k, "lnkD", 4) == 0;
}

pc_status pc_io_find_linked_file(const pc_metadata *meta, const char *uuid, size_t uuid_len, bool *found,
                                 pc_io_linked_file *out, pc_error *err)
{
    *found = false;
    if (uuid_len == 0) {
        return PC_OK;
    }
    for (size_t b = 0; b < meta->npsd_global_blocks; b++) {
        const pc_doc_psd_global_block *g = &meta->psd_global_blocks[b];
        pc_io_linked_files l;
        pc_status st;
        if (!is_link_key(g->key)) {
            continue;
        }
        st = pc_io_parse_linked_files(pc_blob_data(g->data), pc_blob_len(g->data), &l, err);
        if (st != PC_OK) {
            return st;
        }
        for (size_t i = 0; i < l.n; i++) {
            if (pc_str_eq_mem(&l.v[i].uuid, uuid, uuid_len)) {
                *out = l.v[i];
                memset(&l.v[i], 0, sizeof l.v[i]);
                pc_io_linked_files_fini(&l);
                *found = true;
                return PC_OK;
            }
        }
        pc_io_linked_files_fini(&l);
    }
    return PC_OK;
}

/* A JPEG's size from its first start-of-frame marker (ITU-T T.81 B.2.2), walking the marker
 * segments without decoding. Each step moves forward, so a malformed file ends the walk. */
static bool jpeg_size(const uint8_t *b, size_t n, double size[2])
{
    size_t at = 2;
    for (;;) {
        uint8_t code;
        while (at + 1 < n && b[at] == 0xFF && b[at + 1] == 0xFF) {
            at++;
        }
        if (at >= n || b[at] != 0xFF || at + 1 >= n) {
            return false;
        }
        code = b[at + 1];
        if (code >= 0xC0 && code <= 0xCF && code != 0xC4 && code != 0xC8 && code != 0xCC) {
            if (at + 9 > n) {
                return false;
            }
            size[1] = (double)((uint16_t)((b[at + 5] << 8) | b[at + 6]));
            size[0] = (double)((uint16_t)((b[at + 7] << 8) | b[at + 8]));
            return true;
        }
        if (code == 0x01 || (code >= 0xD0 && code <= 0xD7)) {
            if (at > SIZE_MAX - 2) {
                return false;
            }
            at += 2;
        } else if (code == 0xDA || code == 0xD9) {
            return false;
        } else {
            size_t seg;
            if (at + 4 > n) {
                return false;
            }
            seg = (size_t)((b[at + 2] << 8) | b[at + 3]);
            if (seg < 2 || at > SIZE_MAX - 2 - seg) {
                return false;
            }
            at += 2 + seg;
        }
    }
}

/* Pixel size of an embedded source file from its header: PSD/PSB, TIFF (default page), PNG and
 * JPEG; any other image is decoded flat. It never imports a document (a layered TIFF could nest
 * without bound, #2465). *found set with a non-empty size. */
static pc_status source_size_probe(const uint8_t *b, size_t n, bool *found, double size[2], pc_error *err)
{
    *found = false;
    if (pc_io_is_psd(b, n)) {
        if (n >= 22) {
            size[0] = (double)pc_load_be32(b + 18);
            size[1] = (double)pc_load_be32(b + 14);
            *found = true;
        }
        return PC_OK;
    }
    if (n >= 4 && (memcmp(b, "II\x2a\x00", 4) == 0 || memcmp(b, "MM\x00\x2a", 4) == 0 ||
                   memcmp(b, "II\x2b\x00", 4) == 0 || memcmp(b, "MM\x00\x2b", 4) == 0)) {
        pc_tiff_info info;
        size_t page;
        pc_status st = pc_ctiff_info(b, n, &info, err);
        if (st != PC_OK) {
            return st == PC_ERR_NOMEM ? st : PC_OK;
        }
        if (pc_tiff_info_default_page(&info, &page) && page < info.npages) {
            size[0] = (double)info.pages[page].width;
            size[1] = (double)info.pages[page].height;
            *found = true;
        }
        pc_tiff_info_fini(&info);
        return PC_OK;
    }
    if (n >= 24 && b[0] == 0x89 && memcmp(b + 1, "PNG", 3) == 0 && b[4] == 0x0D && b[5] == 0x0A &&
        b[6] == 0x1A && b[7] == 0x0A) {
        size[0] = (double)pc_load_be32(b + 16);
        size[1] = (double)pc_load_be32(b + 20);
        *found = true;
        return PC_OK;
    }
    if (n >= 2 && b[0] == 0xFF && b[1] == 0xD8) {
        *found = jpeg_size(b, n, size);
        return PC_OK;
    }
    {
        pc_cimage img;
        pc_status st = pc_cdecode(b, n, &img, err);
        if (st != PC_OK) {
            return st == PC_ERR_NOMEM ? st : PC_OK;
        }
        size[0] = (double)img.width;
        size[1] = (double)img.height;
        *found = true;
        pc_cimage_fini(&img);
        return PC_OK;
    }
}

pc_status pc_io_source_size_in_blocks(const pc_psd_block *blocks, size_t n, const char *uuid, size_t uuid_len,
                                      bool *found, double size[2], pc_error *err)
{
    *found = false;
    if (uuid_len == 0) {
        return PC_OK;
    }
    for (size_t bi = 0; bi < n; bi++) {
        const pc_psd_block *g = &blocks[bi];
        pc_io_linked_files l;
        pc_status st;
        if (!is_link_key(g->key)) {
            continue;
        }
        st = pc_io_parse_linked_files(g->data.data, g->data.len, &l, err);
        if (st != PC_OK) {
            return st;
        }
        for (size_t i = 0; i < l.n; i++) {
            if (pc_str_eq_mem(&l.v[i].uuid, uuid, uuid_len)) {
                bool ok = false;
                st = source_size_probe(l.v[i].bytes, l.v[i].len, &ok, size, err);
                pc_io_linked_files_fini(&l);
                /* A zero or empty size is treated as unavailable. */
                *found = st == PC_OK && ok && size[0] > 0.0 && size[1] > 0.0;
                return st;
            }
        }
        pc_io_linked_files_fini(&l);
    }
    return PC_OK;
}

void pc_io_linked_file_type(const uint8_t *b, size_t n, uint8_t out[4])
{
    const char *t = "    ";
    if (n >= 6 && memcmp(b, "8BPS", 4) == 0 && b[4] == 0 && b[5] == 2) {
        t = "8BPB";
    } else if (n >= 4 && memcmp(b, "8BPS", 4) == 0) {
        t = "8BPS";
    } else if (n >= 4 && b[0] == 0x89 && memcmp(b + 1, "PNG", 3) == 0) {
        t = "PNGf";
    } else if (n >= 2 && b[0] == 0xFF && b[1] == 0xD8) {
        t = "JPEG";
    } else if (n >= 4 && (memcmp(b, "II\x2a\x00", 4) == 0 || memcmp(b, "MM\x00\x2a", 4) == 0)) {
        t = "TIFF";
    } else if (n >= 3 && memcmp(b, "GIF", 3) == 0) {
        t = "GIFf";
    }
    memcpy(out, t, 4);
}

static void put32(uint8_t *o, uint32_t v)
{
    o[0] = (uint8_t)(v >> 24);
    o[1] = (uint8_t)(v >> 16);
    o[2] = (uint8_t)(v >> 8);
    o[3] = (uint8_t)v;
}

static void put64(uint8_t *o, uint64_t v)
{
    for (size_t i = 0; i < 8; i++) {
        o[i] = (uint8_t)(v >> (56 - 8 * i));
    }
}

pc_status pc_io_encode_linked_file(const pc_io_linked_file *f, pc_buf *out, pc_error *err)
{
    pc_buf item = PC_BUF_INIT;
    size_t idn = f->uuid.len < 255 ? f->uuid.len : 255;
    size_t units = 1;
    size_t i = 0;
    uint8_t tmp[8];
    uint8_t ftype[4];
    pc_status st;
    const uint8_t *name = (const uint8_t *)pc_str_c(&f->file_name);
    while (i < f->file_name.len) {
        uint32_t cp;
        if (!pc_utf8_next(name, f->file_name.len, &i, &cp)) {
            cp = 0xFFFD;
            i++;
        }
        units += cp >= 0x10000 ? 2 : 1;
    }
    st = pc_buf_append(&item, "liFD", 4, err);
    put32(tmp, 7);
    if (st == PC_OK) {
        st = pc_buf_append(&item, tmp, 4, err);
    }
    if (st == PC_OK) {
        st = pc_buf_append_byte(&item, (uint8_t)idn, err);
    }
    if (st == PC_OK && idn > 0) {
        st = pc_buf_append(&item, f->uuid.ptr, idn, err);
    }
    /* NUL-terminated, the terminator counted (as Photoshop writes and reads it). */
    put32(tmp, (uint32_t)units);
    if (st == PC_OK) {
        st = pc_buf_append(&item, tmp, 4, err);
    }
    i = 0;
    while (st == PC_OK && i <= f->file_name.len) {
        uint32_t cp = 0;
        uint8_t u[4];
        size_t nu = 2;
        if (i == f->file_name.len) {
            i++; /* the terminator */
        } else if (!pc_utf8_next(name, f->file_name.len, &i, &cp)) {
            cp = 0xFFFD;
            i++;
        }
        if (cp >= 0x10000) {
            uint32_t v = cp - 0x10000;
            uint32_t hi = 0xD800 + (v >> 10);
            uint32_t lo = 0xDC00 + (v & 0x3FF);
            u[0] = (uint8_t)(hi >> 8);
            u[1] = (uint8_t)hi;
            u[2] = (uint8_t)(lo >> 8);
            u[3] = (uint8_t)lo;
            nu = 4;
        } else {
            u[0] = (uint8_t)(cp >> 8);
            u[1] = (uint8_t)cp;
        }
        st = pc_buf_append(&item, u, nu, err);
    }
    pc_io_linked_file_type(f->bytes, f->len, ftype);
    if (st == PC_OK) {
        st = pc_buf_append(&item, ftype, 4, err);
    }
    if (st == PC_OK) {
        st = pc_buf_append(&item, "8BIM", 4, err);
    }
    put64(tmp, (uint64_t)f->len);
    if (st == PC_OK) {
        st = pc_buf_append(&item, tmp, 8, err);
    }
    if (st == PC_OK) {
        st = pc_buf_append_byte(&item, 0, err);
    }
    if (st == PC_OK && f->len > 0) {
        st = pc_buf_append(&item, f->bytes, f->len, err);
    }
    /* child document id (v5), asset modification time 0.0 (v6), asset locked (v7) */
    memset(tmp, 0, sizeof tmp);
    if (st == PC_OK) {
        st = pc_buf_append(&item, tmp, 4, err);
    }
    if (st == PC_OK) {
        st = pc_buf_append(&item, tmp, 8, err);
    }
    if (st == PC_OK) {
        st = pc_buf_append_byte(&item, 0, err);
    }
    put64(tmp, (uint64_t)item.len);
    if (st == PC_OK) {
        st = pc_buf_append(out, tmp, 8, err);
    }
    if (st == PC_OK) {
        st = pc_buf_append(out, item.data, item.len, err);
    }
    for (size_t pad = (item.len + 8) % 4; st == PC_OK && pad != 0 && pad < 4; pad++) {
        st = pc_buf_append_byte(out, 0, err);
    }
    pc_buf_fini(&item);
    return st;
}

/* One piece of a linked-layer block (borrowed bytes incl. length prefix and padding). */
typedef struct piece {
    bool has_uuid;
    pc_str uuid;
    const uint8_t *bytes;
    size_t len;
} piece;

static void pieces_free(piece *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        pc_str_fini(&p[i].uuid);
    }
    free(p);
}

/* The reference's split_items: each item with its uuid when readable; a malformed tail is one
 * unnamed piece. */
static pc_status split_items(const uint8_t *data, size_t len, piece **out, size_t *nout, pc_error *err)
{
    piece *v = NULL;
    size_t n = 0;
    size_t cap = 0;
    size_t at = 0;
    pc_status st = PC_OK;
    while (at < len && st == PC_OK) {
        const uint8_t *rest = data + at;
        size_t rlen = len - at;
        piece p;
        piece *g;
        bool valid = false;
        memset(&p, 0, sizeof p);
        p.bytes = rest;
        p.len = rlen;
        if (rlen >= 8) {
            size_t l;
            if (to_size(be64(rest), &l) && l <= SIZE_MAX - 8 && l + 8 <= rlen) {
                size_t end = l + 8;
                size_t padded = round4(end) < rlen ? round4(end) : rlen;
                const uint8_t *item = rest + 8;
                size_t ilen = end - 8;
                valid = true;
                if (ilen > 8) {
                    size_t idn = item[8];
                    if (9 + idn <= ilen) {
                        st = lossy_str(item + 9, idn, &p.uuid, err);
                        p.has_uuid = st == PC_OK;
                    }
                }
                p.len = padded;
            }
        }
        if (st != PC_OK) {
            break;
        }
        g = pc_grow_array(v, &cap, n + 1, sizeof *g);
        if (g == NULL) {
            pc_str_fini(&p.uuid);
            st = pc_io_nomem(err, "splitting a linked-layer block");
            break;
        }
        v = g;
        v[n++] = p;
        if (!valid) {
            break; /* the malformed tail */
        }
        at += p.len;
    }
    if (st != PC_OK) {
        pieces_free(v, n);
        return st;
    }
    *out = v;
    *nout = n;
    return PC_OK;
}

pc_status pc_io_rebuild_linked_block(const uint8_t *data, size_t len, pc_io_keep_uuid keep, void *ctx,
                                     const pc_io_linked_file *add, size_t nadd, bool *changed, pc_buf *out,
                                     pc_error *err)
{
    piece *items = NULL;
    size_t n = 0;
    bool dropped = false;
    pc_buf b = PC_BUF_INIT;
    pc_status st = split_items(data, len, &items, &n, err);
    if (st != PC_OK) {
        return st;
    }
    for (size_t i = 0; i < n; i++) {
        if (items[i].has_uuid && !keep(ctx, pc_str_c(&items[i].uuid), items[i].uuid.len)) {
            dropped = true;
        }
    }
    if (!dropped && nadd == 0) {
        pieces_free(items, n);
        *changed = false;
        return PC_OK;
    }
    st = pc_buf_reserve(&b, len, err);
    for (size_t i = 0; i < n && st == PC_OK; i++) {
        if (!items[i].has_uuid || keep(ctx, pc_str_c(&items[i].uuid), items[i].uuid.len)) {
            st = pc_buf_append(&b, items[i].bytes, items[i].len, err);
        }
    }
    for (size_t i = 0; i < nadd && st == PC_OK; i++) {
        st = pc_io_encode_linked_file(&add[i], &b, err);
    }
    pieces_free(items, n);
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    pc_buf_fini(out);
    *out = b;
    *changed = true;
    return PC_OK;
}

pc_status pc_io_copy_linked_item(const uint8_t *data, size_t len, const char *uuid, size_t uuid_len,
                                 const char *new_uuid, size_t new_uuid_len, bool *found, pc_buf *out,
                                 pc_error *err)
{
    piece *items = NULL;
    size_t n = 0;
    pc_status st = split_items(data, len, &items, &n, err);
    *found = false;
    if (st != PC_OK) {
        return st;
    }
    for (size_t i = 0; i < n && st == PC_OK && !*found; i++) {
        const uint8_t *raw;
        const uint8_t *item;
        size_t l;
        size_t idn;
        size_t old_end;
        uint8_t hdr[8];
        pc_buf ni = PC_BUF_INIT;
        if (!items[i].has_uuid || !pc_str_eq_mem(&items[i].uuid, uuid, uuid_len)) {
            continue;
        }
        raw = items[i].bytes;
        if (!to_size(be64(raw), &l) || l < 9 || new_uuid_len > 255) {
            break; /* None */
        }
        item = raw + 8;
        idn = item[8];
        old_end = 9 + idn;
        if (old_end > l) {
            break;
        }
        st = pc_buf_append(&ni, item, 8, err);
        if (st == PC_OK) {
            st = pc_buf_append_byte(&ni, (uint8_t)new_uuid_len, err);
        }
        if (st == PC_OK) {
            st = pc_buf_append(&ni, new_uuid, new_uuid_len, err);
        }
        if (st == PC_OK) {
            st = pc_buf_append(&ni, item + old_end, l - old_end, err);
        }
        if (st == PC_OK) {
            pc_store_be64(hdr, ni.len);
            st = pc_buf_append(out, hdr, 8, err);
        }
        if (st == PC_OK) {
            st = pc_buf_append(out, ni.data, ni.len, err);
        }
        while (st == PC_OK && out->len % 4 != 0) {
            st = pc_buf_append_byte(out, 0, err);
        }
        *found = st == PC_OK;
        pc_buf_fini(&ni);
    }
    pieces_free(items, n);
    return st;
}

pc_status pc_io_linked_block_uuids(const uint8_t *data, size_t len, pc_io_strs *out, pc_error *err)
{
    piece *items = NULL;
    size_t n = 0;
    pc_io_strs s = {NULL, 0, 0};
    pc_status st = split_items(data, len, &items, &n, err);
    for (size_t i = 0; i < n && st == PC_OK; i++) {
        if (items[i].has_uuid) {
            st = pc_io_strs_push(&s, pc_str_c(&items[i].uuid), items[i].uuid.len, err);
        }
    }
    pieces_free(items, n);
    if (st != PC_OK) {
        pc_io_strs_fini(&s);
        return st;
    }
    *out = s;
    return PC_OK;
}
