#include "format/zip.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/compress.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"
#include "format/format.h"

enum {
    LOCAL_SIG = 0x04034b50,
    CENTRAL_SIG = 0x02014b50,
    EOCD_SIG = 0x06054b50,
    LOCAL_LEN = 30,
    CENTRAL_LEN = 46,
    EOCD_LEN = 22
};

uint32_t pc_zip_crc32(const uint8_t *data, size_t len)
{
    return pc_crc32(0, data, len);
}

void pc_zip_writer_init(pc_zip_writer *w)
{
    pc_buf_init(&w->out);
    pc_buf_init(&w->central);
    w->count = 0;
}

void pc_zip_writer_fini(pc_zip_writer *w)
{
    if (w != NULL) {
        pc_buf_fini(&w->out);
        pc_buf_fini(&w->central);
        w->count = 0;
    }
}

pc_status pc_zip_writer_add(pc_zip_writer *w, const char *name, size_t name_len, const uint8_t *data,
                            size_t len, pc_error *err)
{
    uint8_t lh[LOCAL_LEN];
    uint8_t ch[CENTRAL_LEN];
    size_t local_extra;
    size_t central_extra;
    uint32_t crc;
    pc_status s;

    if (w->count == UINT16_MAX) {
        return pc_format_unsupported(err, "more than 65535 zip entries (ZIP64 not supported)");
    }
    if ((uint64_t)len > UINT32_MAX) {
        return pc_format_unsupported(err, "zip entry over 4 GiB");
    }
    if ((uint64_t)w->out.len > UINT32_MAX) {
        return pc_format_unsupported(err, "zip over 4 GiB");
    }
    if (name_len > UINT16_MAX) {
        return pc_format_unsupported(err, "zip entry name over 65535 bytes");
    }
    /* Reserve everything first so a failure leaves the writer unchanged. */
    if (!pc_size_add(LOCAL_LEN, name_len, &local_extra) || !pc_size_add(local_extra, len, &local_extra) ||
        !pc_size_add(CENTRAL_LEN, name_len, &central_extra)) {
        return pc_format_unsupported(err, "zip over 4 GiB");
    }
    s = pc_buf_reserve(&w->out, local_extra, err);
    if (s == PC_OK) {
        s = pc_buf_reserve(&w->central, central_extra, err);
    }
    if (s != PC_OK) {
        return s;
    }
    crc = pc_zip_crc32(data, len);

    pc_store_le32(lh, LOCAL_SIG);
    pc_store_le16(lh + 4, 20);     /* version needed */
    pc_store_le16(lh + 6, 0x0800); /* flags: UTF-8 names */
    pc_store_le16(lh + 8, 0);      /* method: store */
    pc_store_le16(lh + 10, 0);     /* time */
    pc_store_le16(lh + 12, 0x21);  /* date: 1980-01-01 */
    pc_store_le32(lh + 14, crc);
    pc_store_le32(lh + 18, (uint32_t)len);
    pc_store_le32(lh + 22, (uint32_t)len);
    pc_store_le16(lh + 26, (uint16_t)name_len);
    pc_store_le16(lh + 28, 0);

    pc_store_le32(ch, CENTRAL_SIG);
    pc_store_le16(ch + 4, 20); /* made by */
    pc_store_le16(ch + 6, 20); /* needed */
    pc_store_le16(ch + 8, 0x0800);
    pc_store_le16(ch + 10, 0);
    pc_store_le16(ch + 12, 0);
    pc_store_le16(ch + 14, 0x21);
    pc_store_le32(ch + 16, crc);
    pc_store_le32(ch + 20, (uint32_t)len);
    pc_store_le32(ch + 24, (uint32_t)len);
    pc_store_le16(ch + 28, (uint16_t)name_len);
    memset(ch + 30, 0, 12); /* extra len, comment len, disk, internal and external attrs */
    pc_store_le32(ch + 42, (uint32_t)w->out.len);

    /* Room is reserved: these appends cannot fail. */
    (void)pc_buf_append(&w->out, lh, sizeof lh, NULL);
    (void)pc_buf_append(&w->out, name, name_len, NULL);
    (void)pc_buf_append(&w->out, data, len, NULL);
    (void)pc_buf_append(&w->central, ch, sizeof ch, NULL);
    (void)pc_buf_append(&w->central, name, name_len, NULL);
    w->count++;
    return PC_OK;
}

pc_status pc_zip_writer_finish(pc_zip_writer *w, pc_buf *out, pc_error *err)
{
    uint8_t eocd[EOCD_LEN];
    size_t extra;
    pc_status s;

    if ((uint64_t)w->out.len > UINT32_MAX || (uint64_t)w->central.len > UINT32_MAX) {
        return pc_format_unsupported(err, "zip over 4 GiB");
    }
    if (!pc_size_add(w->central.len, EOCD_LEN, &extra)) {
        return pc_format_unsupported(err, "zip over 4 GiB");
    }
    s = pc_buf_reserve(&w->out, extra, err);
    if (s != PC_OK) {
        return s;
    }
    pc_store_le32(eocd, EOCD_SIG);
    memset(eocd + 4, 0, 4); /* disk numbers */
    pc_store_le16(eocd + 8, w->count);
    pc_store_le16(eocd + 10, w->count);
    pc_store_le32(eocd + 12, (uint32_t)w->central.len);
    pc_store_le32(eocd + 16, (uint32_t)w->out.len);
    pc_store_le16(eocd + 20, 0);
    (void)pc_buf_append(&w->out, w->central.data, w->central.len, NULL);
    (void)pc_buf_append(&w->out, eocd, sizeof eocd, NULL);
    pc_buf_fini(out);
    *out = w->out;
    pc_buf_init(&w->out);
    pc_buf_fini(&w->central);
    w->count = 0;
    return PC_OK;
}

/* ---- reading ---- */

static pc_status u16_at(const uint8_t *b, size_t len, size_t o, size_t *out, pc_error *err)
{
    if (o > len || len - o < 2) {
        return pc_format_corrupt(err, "truncated zip");
    }
    *out = pc_load_le16(b + o);
    return PC_OK;
}

static pc_status u32_at(const uint8_t *b, size_t len, size_t o, uint32_t *out, pc_error *err)
{
    if (o > len || len - o < 4) {
        return pc_format_corrupt(err, "truncated zip");
    }
    *out = pc_load_le32(b + o);
    return PC_OK;
}

/* end = a + b (+ c), true when it fits and end <= limit. */
static bool span_ok(size_t a, size_t b, size_t limit, size_t *end)
{
    size_t e;
    if (!pc_size_add(a, b, &e) || e > limit) {
        return false;
    }
    *end = e;
    return true;
}

static void entries_free(pc_zip_entry *entries, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        pc_str_fini(&entries[i].name);
    }
    free(entries);
}

/* One central-directory entry at *p (advanced past it on success). */
static pc_status read_entry(const uint8_t *bytes, size_t len, size_t cd_end, size_t *p, pc_zip_entry *e,
                            pc_error *err)
{
    size_t header_end;
    size_t name_end;
    size_t entry_end;
    size_t flags;
    size_t method;
    size_t nlen;
    size_t elen;
    size_t clen;
    size_t lnlen;
    size_t lelen;
    size_t data_offset;
    size_t end;
    uint32_t sig;
    uint32_t crc;
    uint32_t compressed;
    uint32_t uncompressed;
    uint32_t local;
    pc_buf name = PC_BUF_INIT;
    pc_status s;

    if (!span_ok(*p, CENTRAL_LEN, cd_end, &header_end)) {
        return pc_format_corrupt(err, "truncated zip central directory entry");
    }
    if ((s = u32_at(bytes, len, *p, &sig, err)) != PC_OK) {
        return s;
    }
    if (sig != CENTRAL_SIG) {
        return pc_format_corrupt(err, "bad zip central directory entry");
    }
    /* header_end <= cd_end <= len, so these fixed-offset reads are in range. */
    flags = pc_load_le16(bytes + *p + 8);
    method = pc_load_le16(bytes + *p + 10);
    crc = pc_load_le32(bytes + *p + 16);
    compressed = pc_load_le32(bytes + *p + 20);
    uncompressed = pc_load_le32(bytes + *p + 24);
    nlen = pc_load_le16(bytes + *p + 28);
    elen = pc_load_le16(bytes + *p + 30);
    clen = pc_load_le16(bytes + *p + 32);
    local = pc_load_le32(bytes + *p + 42);
    if (!span_ok(header_end, nlen, cd_end, &name_end)) {
        return pc_format_corrupt(err, "zip entry name out of range");
    }
    s = pc_utf8_lossy(bytes + header_end, nlen, &name, err);
    if (s != PC_OK) {
        return s;
    }
    if ((flags & 1u) != 0) {
        s = pc_format_unsupported(err, "encrypted zip entries");
        goto fail;
    }
    /* Bound each entry by the declared central-directory extent before advancing. */
    if (!span_ok(name_end, elen, cd_end, &entry_end) || !span_ok(entry_end, clen, cd_end, &entry_end)) {
        s = pc_format_corrupt(err, "zip central directory entry out of range");
        goto fail;
    }
    /* The local header gives the real data offset. */
    if ((s = u32_at(bytes, len, local, &sig, err)) != PC_OK) {
        goto fail;
    }
    if (sig != LOCAL_SIG) {
        s = pc_format_corrupt(err, "bad zip local header");
        goto fail;
    }
    if (!pc_size_add(local, 26, &end) || !pc_size_add(local, 28, &data_offset)) {
        s = pc_format_corrupt(err, "zip local header offset overflow");
        goto fail;
    }
    if ((s = u16_at(bytes, len, end, &lnlen, err)) != PC_OK ||
        (s = u16_at(bytes, len, data_offset, &lelen, err)) != PC_OK) {
        goto fail;
    }
    if (!pc_size_add(local, LOCAL_LEN, &data_offset) || !pc_size_add(data_offset, lnlen, &data_offset) ||
        !pc_size_add(data_offset, lelen, &data_offset)) {
        s = pc_format_corrupt(err, "zip data offset overflow");
        goto fail;
    }
    if (!span_ok(data_offset, compressed, len, &end)) {
        s = pc_format_corrupt(err, "zip entry `%s` out of range", pc_buf_cstr(&name));
        goto fail;
    }
    {
        size_t n = name.len;
        e->name = pc_str_adopt((char *)pc_buf_take(&name, NULL), n);
    }
    e->method = (uint16_t)method;
    e->crc = crc;
    e->compressed = compressed;
    e->uncompressed = uncompressed;
    e->data_offset = data_offset;
    *p = entry_end;
    return PC_OK;
fail:
    pc_buf_fini(&name);
    return s;
}

static uint64_t name_hash(const char *name, size_t len)
{
    uint64_t h = 0xcbf29ce484222325ull; /* FNV-1a */
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)name[i];
        h *= 0x100000001b3ull;
    }
    return h ^ (h >> 29);
}

/* The name index; best effort (out of memory leaves r->index NULL: pc_zip_find scans). The first
 * entry of a name wins, as with a front-to-back scan. */
static void build_index(pc_zip_reader *r)
{
    size_t cap = 16;
    r->index = NULL;
    r->index_cap = 0;
    if (r->count == 0) {
        return;
    }
    while (cap / 2 < r->count) {
        if (cap > SIZE_MAX / 4) {
            return;
        }
        cap *= 2;
    }
    r->index = pc_calloc_array(cap, sizeof *r->index);
    if (r->index == NULL) {
        return;
    }
    r->index_cap = cap;
    for (size_t k = 0; k < r->count; k++) {
        const pc_str *nm = &r->entries[k].name;
        for (size_t i = (size_t)name_hash(pc_str_c(nm), nm->len) & (cap - 1);; i = (i + 1) & (cap - 1)) {
            size_t j = r->index[i];
            if (j == 0) {
                r->index[i] = k + 1;
                break;
            }
            if (pc_str_eq_mem(&r->entries[j - 1].name, pc_str_c(nm), nm->len)) {
                break; /* a duplicate name: the first entry stays */
            }
        }
    }
}

pc_status pc_zip_reader_open(pc_zip_reader *r, const uint8_t *bytes, size_t len, pc_error *err)
{
    size_t min;
    size_t i;
    size_t eocd = SIZE_MAX;
    size_t count;
    size_t cd_end;
    size_t p;
    size_t cap;
    size_t n = 0;
    pc_zip_entry *entries = NULL;
    pc_status s = PC_OK;

    if (len < EOCD_LEN) {
        return pc_format_corrupt(err, "not a zip archive (too short)");
    }
    /* End of central directory: scan back over a possible comment. */
    min = len - EOCD_LEN > 65535 ? len - EOCD_LEN - 65535 : 0;
    for (i = len - EOCD_LEN + 1; i-- > min;) {
        if (pc_load_le32(bytes + i) == EOCD_SIG) {
            eocd = i;
            break;
        }
    }
    if (eocd == SIZE_MAX) {
        return pc_format_corrupt(err, "zip end-of-central-directory not found");
    }
    count = pc_load_le16(bytes + eocd + 10);
    {
        size_t cd_size = pc_load_le32(bytes + eocd + 12);
        size_t cd_off = pc_load_le32(bytes + eocd + 16);
        if (!span_ok(cd_off, cd_size, eocd, &cd_end)) {
            return pc_format_corrupt(err, "zip central directory out of range");
        }
        p = cd_off;
    }
    cap = count < len / CENTRAL_LEN ? count : len / CENTRAL_LEN;
    if (cap > 0) {
        entries = pc_calloc_array(cap, sizeof *entries);
        if (entries == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
    }
    for (i = 0; i < count; i++) {
        pc_zip_entry e;
        s = read_entry(bytes, len, cd_end, &p, &e, err);
        if (s != PC_OK) {
            goto fail;
        }
        if (n == cap) {
            pc_zip_entry *grown = pc_grow_array(entries, &cap, n + 1, sizeof *entries);
            if (grown == NULL) {
                pc_str_fini(&e.name);
                s = pc_fail(err, PC_ERR_NOMEM, "out of memory");
                goto fail;
            }
            entries = grown;
        }
        entries[n++] = e;
    }
    r->bytes = bytes;
    r->len = len;
    r->entries = entries;
    r->count = n;
    build_index(r);
    return PC_OK;
fail:
    entries_free(entries, n);
    return s;
}

void pc_zip_reader_close(pc_zip_reader *r)
{
    if (r != NULL) {
        entries_free(r->entries, r->count);
        free(r->index);
        r->entries = NULL;
        r->count = 0;
        r->index = NULL;
        r->index_cap = 0;
    }
}

const pc_zip_entry *pc_zip_find(const pc_zip_reader *r, const char *name, size_t len)
{
    if (r->index != NULL) {
        size_t mask = r->index_cap - 1;
        for (size_t i = (size_t)name_hash(name, len) & mask;; i = (i + 1) & mask) {
            size_t k = r->index[i];
            if (k == 0) {
                return NULL;
            }
            if (pc_str_eq_mem(&r->entries[k - 1].name, name, len)) {
                return &r->entries[k - 1];
            }
        }
    }
    for (size_t i = 0; i < r->count; i++) {
        if (pc_str_eq_mem(&r->entries[i].name, name, len)) {
            return &r->entries[i];
        }
    }
    return NULL;
}

pc_status pc_zip_read(const pc_zip_reader *r, const pc_zip_entry *e, size_t max, pc_buf *out, pc_error *err)
{
    const uint8_t *raw;
    const char *name = pc_str_c(&e->name);
    pc_buf data = PC_BUF_INIT;
    pc_status s;

    if (e->uncompressed > max) {
        return pc_format_limit(err, "zip entry `%s` is %zu bytes (max %zu)", name, e->uncompressed, max);
    }
    /* Entries are public: one from another archive must not read out of range. */
    if (e->data_offset > r->len || e->compressed > r->len - e->data_offset) {
        return pc_format_corrupt(err, "zip entry `%s` out of range", name);
    }
    raw = r->bytes + e->data_offset;
    if (e->method == 0) {
        s = pc_buf_append(&data, raw, e->compressed, err);
        if (s != PC_OK) {
            return s;
        }
    } else if (e->method == 8) {
        /* Read one byte past max, so an entry that inflates beyond it is caught below. */
        size_t limit = max == SIZE_MAX ? max : max + 1;
        s = pc_inflate(PC_Z_RAW, raw, e->compressed, limit, &data, NULL, err);
        if (s == PC_ERR_NOMEM || s == PC_ERR_LIMIT) {
            return s;
        }
        if (s != PC_OK) {
            char inner[PC_ERROR_MSG_MAX];
            (void)snprintf(inner, sizeof inner, "%s", err != NULL ? err->msg : "corrupt deflate stream");
            return pc_format_corrupt(err, "zip entry `%s`: %s", name, inner);
        }
    } else {
        return pc_format_unsupported(err, "zip compression method %u for `%s`", (unsigned)e->method, name);
    }
    if (data.len != e->uncompressed || data.len > max) {
        pc_buf_fini(&data);
        return pc_format_corrupt(err, "zip entry `%s` has wrong size", name);
    }
    if (pc_zip_crc32(data.data, data.len) != e->crc) {
        pc_buf_fini(&data);
        return pc_format_corrupt(err, "zip entry `%s` failed its CRC check", name);
    }
    pc_buf_fini(out);
    *out = data;
    return PC_OK;
}

pc_status pc_zip_read_by_name(const pc_zip_reader *r, const char *name, size_t max, pc_buf *out,
                              pc_error *err)
{
    const pc_zip_entry *e = pc_zip_find(r, name, strlen(name));
    if (e == NULL) {
        return pc_format_corrupt(err, "missing `%s`", name);
    }
    return pc_zip_read(r, e, max, out, err);
}
