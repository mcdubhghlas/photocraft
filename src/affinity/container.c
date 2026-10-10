/* The Affinity archive (port of crates/affinity/src/container.rs): header, the chain of file
 * allocation tables (one per save) and bounded, CRC-checked extraction of named entries such as
 * doc.dat and raster tiles. */
#include <stdlib.h>
#include <string.h>

#include "affinity/internal.h"
#include "base/buf.h"
#include "base/compress.h"
#include "base/mem.h"
#include "base/utf8.h"

const uint8_t PC_AF_MAGIC[4] = {0x00, 0xFF, 'K', 'A'};

/* A save appends one table; real files have one to a few hundred. */
#define MAX_TABLES 4096u
/* zstd window ceiling: 64 MiB. */
#define MAX_ZSTD_WINDOW_LOG 26

pc_af_limits pc_af_limits_default(void)
{
    pc_af_limits l;
    l.max_entry = (size_t)256 << 20;
    l.max_total = (size_t)1 << 30;
    return l;
}

bool pc_af_is_affinity(const uint8_t *b, size_t n) { return n >= 4 && memcmp(b, PC_AF_MAGIC, 4) == 0; }

/* ---- cursor ---- */

size_t ax_remaining(const ax_cursor *c) { return c->n > c->pos ? c->n - c->pos : 0; }

pc_status ax_take(ax_cursor *c, size_t n, const uint8_t **out, pc_error *err)
{
    if (c->pos > SIZE_MAX - n) {
        return ax_malformed(err, "length overflow");
    }
    if (c->pos > c->n || c->pos + n > c->n) {
        return ax_malformed(err, "truncated data");
    }
    if (out != NULL) {
        *out = c->b + c->pos;
    }
    c->pos += n;
    return PC_OK;
}

pc_status ax_u8(ax_cursor *c, uint8_t *v, pc_error *err)
{
    const uint8_t *p;
    pc_status st = ax_take(c, 1, &p, err);
    if (st == PC_OK) {
        *v = p[0];
    }
    return st;
}

pc_status ax_u16(ax_cursor *c, uint16_t *v, pc_error *err)
{
    const uint8_t *p;
    pc_status st = ax_take(c, 2, &p, err);
    if (st == PC_OK) {
        *v = (uint16_t)(p[0] | p[1] << 8);
    }
    return st;
}

pc_status ax_u32(ax_cursor *c, uint32_t *v, pc_error *err)
{
    const uint8_t *p;
    pc_status st = ax_take(c, 4, &p, err);
    if (st == PC_OK) {
        *v = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    }
    return st;
}

pc_status ax_u64(ax_cursor *c, uint64_t *v, pc_error *err)
{
    uint32_t lo;
    uint32_t hi;
    pc_status st;
    if ((st = ax_u32(c, &lo, err)) != PC_OK || (st = ax_u32(c, &hi, err)) != PC_OK) {
        return st;
    }
    *v = (uint64_t)hi << 32 | lo;
    return PC_OK;
}

char *ax_lossy(const uint8_t *b, size_t n, size_t *len)
{
    pc_buf s = PC_BUF_INIT;
    char *out;
    if (pc_utf8_lossy(b, n, &s, NULL) != PC_OK || pc_buf_append_byte(&s, 0, NULL) != PC_OK) {
        pc_buf_fini(&s);
        return NULL;
    }
    if (len != NULL) {
        *len = s.len - 1;
    }
    out = (char *)pc_buf_take(&s, NULL);
    return out;
}

/* ---- header ---- */

pc_status pc_af_header_read(const uint8_t *b, size_t n, pc_af_header *out, pc_error *err)
{
    ax_cursor c = {b, n, 4};
    pc_af_header h;
    const uint8_t *p;
    uint64_t saved;
    pc_status st;
    if (!pc_af_is_affinity(b, n)) {
        return ax_malformed(err, "missing container signature");
    }
    if ((st = ax_u16(&c, &h.version, err)) != PC_OK || (st = ax_u16(&c, &h.flags, err)) != PC_OK) {
        return st;
    }
    if (h.version < 8 || h.version > 12) {
        return ax_unsupported(err, "unknown container version");
    }
    if ((h.flags & 3) != 0) {
        return ax_unsupported(err, "container variant");
    }
    if ((st = ax_take(&c, 4, &p, err)) != PC_OK) {
        return st;
    }
    if (memcmp(p, "nsrP", 4) != 0) {
        return ax_unsupported(err, "this Affinity file is a library (assets, brushes or styles), not a document");
    }
    if ((st = ax_take(&c, 4, &p, err)) != PC_OK) {
        return st;
    }
    if (memcmp(p, "#Inf", 4) != 0) {
        return ax_malformed(err, "missing information header");
    }
    if ((st = ax_u64(&c, &h.fat_offset, err)) != PC_OK || (st = ax_u64(&c, &h.thumbnail_offset, err)) != PC_OK ||
        (st = ax_take(&c, 16, NULL, err)) != PC_OK || (st = ax_u64(&c, &saved, err)) != PC_OK ||
        (st = ax_take(&c, 8, NULL, err)) != PC_OK || (st = ax_take(&c, 4, &p, err)) != PC_OK) {
        return st;
    }
    h.saved = saved <= UINT32_MAX ? (uint32_t)saved : 0;
    if (memcmp(p, "Prot", 4) != 0) {
        return ax_malformed(err, "missing protocol header");
    }
    *out = h;
    return PC_OK;
}

/* ---- id -> record map (open addressing) ---- */

typedef struct rec {
    uint32_t id;
    bool used;
    uint64_t date;
    bool has_entry;
    pc_af_entry e;
    char *name; /* first (newest) table's name for the id */
} rec;

typedef struct idmap {
    rec *v;
    size_t cap;
    size_t n;
} idmap;

static void idmap_fini(idmap *m)
{
    size_t i;
    for (i = 0; i < m->cap; i++) {
        free(m->v[i].name);
    }
    free(m->v);
    memset(m, 0, sizeof *m);
}

static size_t slot_of(const idmap *m, uint32_t id)
{
    size_t i = (size_t)(id * 2654435761u) & (m->cap - 1);
    while (m->v[i].used && m->v[i].id != id) {
        i = (i + 1) & (m->cap - 1);
    }
    return i;
}

static rec *idmap_get(idmap *m, uint32_t id, bool *fresh)
{
    size_t i;
    if (m->cap == 0 || (m->n + 1) * 2 > m->cap) {
        size_t cap = m->cap > 0 ? m->cap * 2 : 64;
        rec *old = m->v;
        size_t oc = m->cap;
        size_t k;
        rec *nv = calloc(cap, sizeof *nv);
        if (nv == NULL) {
            return NULL;
        }
        m->v = nv;
        m->cap = cap;
        for (k = 0; k < oc; k++) {
            if (old[k].used) {
                m->v[slot_of(m, old[k].id)] = old[k];
            }
        }
        free(old);
    }
    i = slot_of(m, id);
    *fresh = !m->v[i].used;
    if (*fresh) {
        memset(&m->v[i], 0, sizeof m->v[i]);
        m->v[i].used = true;
        m->v[i].id = id;
        m->n++;
    }
    return &m->v[i];
}

static int cmp_entry(const void *a, const void *b)
{
    const pc_af_entry *x = a;
    const pc_af_entry *y = b;
    int c = strcmp(x->name, y->name);
    return c != 0 ? c : (x->id < y->id ? -1 : x->id > y->id);
}

void pc_af_archive_fini(pc_af_archive *a)
{
    size_t i;
    if (a == NULL) {
        return;
    }
    for (i = 0; i < a->nentries; i++) {
        free(a->entries[i].name);
    }
    free(a->entries);
    memset(a, 0, sizeof *a);
}

pc_status pc_af_archive_open(const uint8_t *b, size_t n, pc_af_limits limits, pc_af_archive *out, pc_error *err)
{
    pc_af_archive a;
    idmap m = {NULL, 0, 0};
    uint64_t seen[MAX_TABLES];
    size_t nseen = 0;
    uint64_t next;
    size_t tables = 0;
    size_t i;
    size_t k;
    pc_status st;
    memset(&a, 0, sizeof a);
    if ((st = pc_af_header_read(b, n, &a.header, err)) != PC_OK) {
        return st;
    }
    next = a.header.fat_offset;
    while (next != 0) {
        ax_cursor c;
        const uint8_t *tag;
        int level;
        uint64_t date;
        uint32_t files;
        uint16_t dirs;
        uint32_t f;
        for (i = 0; i < nseen; i++) {
            if (seen[i] == next) {
                idmap_fini(&m);
                return ax_malformed(err, "allocation tables form a cycle");
            }
        }
        tables++;
        if (tables > MAX_TABLES) {
            idmap_fini(&m);
            return ax_limit(err, "too many saved revisions");
        }
        seen[nseen++] = next;
        c.b = b;
        c.n = n;
        c.pos = next > SIZE_MAX ? SIZE_MAX : (size_t)next;
        if ((st = ax_take(&c, 4, &tag, err)) != PC_OK) {
            goto fail;
        }
        if (memcmp(tag, "#FAT", 4) == 0) {
            level = 1;
        } else if (memcmp(tag, "#FT2", 4) == 0) {
            level = 2;
        } else if (memcmp(tag, "#FT3", 4) == 0) {
            level = 3;
        } else if (memcmp(tag, "#FT4", 4) == 0) {
            level = 4;
        } else {
            st = ax_malformed(err, "missing allocation table");
            goto fail;
        }
        if ((st = ax_u64(&c, &next, err)) != PC_OK || (st = ax_u64(&c, &date, err)) != PC_OK ||
            (st = ax_take(&c, 24, NULL, err)) != PC_OK || (st = ax_u32(&c, &files, err)) != PC_OK ||
            (st = ax_take(&c, 8, NULL, err)) != PC_OK || (st = ax_u16(&c, &dirs, err)) != PC_OK ||
            (st = ax_take(&c, 1, NULL, err)) != PC_OK) {
            goto fail;
        }
        for (f = 0; f < files; f++) {
            uint32_t id;
            uint8_t flag;
            bool has = false;
            pc_af_entry e;
            rec *r;
            bool fresh;
            memset(&e, 0, sizeof e);
            if ((st = ax_u32(&c, &id, err)) != PC_OK || (st = ax_u8(&c, &flag, err)) != PC_OK) {
                goto fail;
            }
            if (flag == 0 || flag == 1) {
                uint8_t stored;
                if ((st = ax_u64(&c, &e.offset, err)) != PC_OK || (st = ax_u64(&c, &e.size, err)) != PC_OK ||
                    (st = ax_u64(&c, &e.compressed, err)) != PC_OK || (st = ax_u32(&c, &e.crc, err)) != PC_OK ||
                    (st = ax_u8(&c, &stored, err)) != PC_OK) {
                    goto fail;
                }
                if (level >= 2 && (st = ax_take(&c, 4, NULL, err)) != PC_OK) {
                    goto fail;
                }
                if (level >= 4 && (st = ax_take(&c, 4, NULL, err)) != PC_OK) {
                    goto fail;
                }
                /* Old tables number the methods; newer ones store the method byte itself. */
                if (level <= 2) {
                    e.compression = stored == 1 ? 0x01 : stored == 2 ? 0x41 : stored == 3 ? 0x81 : stored == 4 ? 0xC1 : 0;
                } else {
                    e.compression = stored;
                }
                e.id = id;
                has = true;
            } else if (flag != 2) {
                st = ax_malformed(err, "unknown allocation record");
                goto fail;
            }
            r = idmap_get(&m, id, &fresh);
            if (r == NULL) {
                st = ax_nomem(err);
                goto fail;
            }
            if (flag == 0) {
                uint16_t len;
                const uint8_t *nm;
                if ((st = ax_u16(&c, &len, err)) != PC_OK || (st = ax_take(&c, len, &nm, err)) != PC_OK) {
                    goto fail;
                }
                if (r->name == NULL && (r->name = ax_lossy(nm, len, NULL)) == NULL) {
                    st = ax_nomem(err);
                    goto fail;
                }
            }
            /* Equal dates: the first table in the chain (the newest) wins. */
            if (fresh || date > r->date) {
                r->date = date;
                r->has_entry = has;
                r->e = e;
            }
        }
        for (k = 0; k < dirs; k++) {
            uint16_t len;
            if ((st = ax_u16(&c, &len, err)) != PC_OK || (st = ax_take(&c, 10, NULL, err)) != PC_OK ||
                (st = ax_take(&c, len, NULL, err)) != PC_OK) {
                goto fail;
            }
        }
    }
    a.entries = pc_malloc_array(m.n > 0 ? m.n : 1, sizeof *a.entries);
    if (a.entries == NULL) {
        st = ax_nomem(err);
        goto fail;
    }
    for (i = 0; i < m.cap; i++) {
        rec *r = &m.v[i];
        if (r->used && r->has_entry && r->name != NULL) {
            pc_af_entry e = r->e;
            e.name = r->name;
            r->name = NULL;
            a.entries[a.nentries++] = e;
        }
    }
    idmap_fini(&m);
    /* Sorted by name; of two ids with one name the larger id wins (the reference's HashMap
     * makes that choice at random). */
    qsort(a.entries, a.nentries, sizeof *a.entries, cmp_entry);
    k = 0;
    for (i = 0; i < a.nentries; i++) {
        if (k > 0 && strcmp(a.entries[k - 1].name, a.entries[i].name) == 0) {
            free(a.entries[k - 1].name);
            a.entries[k - 1] = a.entries[i];
        } else {
            a.entries[k++] = a.entries[i];
        }
    }
    a.nentries = k;
    a.bytes = b;
    a.len = n;
    a.revisions = tables;
    a.limits = limits;
    *out = a;
    return PC_OK;
fail:
    idmap_fini(&m);
    pc_af_archive_fini(&a);
    return st;
}

const pc_af_entry *pc_af_archive_entry(const pc_af_archive *a, const char *name)
{
    size_t lo = 0;
    size_t hi = a->nentries;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(a->entries[mid].name, name);
        if (c == 0) {
            return &a->entries[mid];
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

static pc_status unzstd(const uint8_t *data, size_t n, size_t size, pc_buf *out, pc_error *err)
{
    size_t pos = 0;
    /* Entries may hold several concatenated frames. */
    while (pos < n) {
        pc_buf frame = PC_BUF_INIT;
        pc_decomp_info info;
        size_t left = size - out->len;
        if (pc_zstd_decompress(data + pos, n - pos, left + 1, MAX_ZSTD_WINDOW_LOG, &frame, &info, NULL) != PC_OK ||
            frame.len == 0) {
            pc_buf_fini(&frame);
            return ax_malformed(err, "corrupt compressed entry");
        }
        if (pc_buf_append(out, frame.data, frame.len, NULL) != PC_OK) {
            pc_buf_fini(&frame);
            return ax_nomem(err);
        }
        pc_buf_fini(&frame);
        if (out->len > size) {
            return ax_malformed(err, "archive entry size mismatch");
        }
        if (info.consumed == 0) {
            return ax_malformed(err, "corrupt compressed entry");
        }
        pos += info.consumed;
    }
    return PC_OK;
}

static void byte_delta(uint8_t *b, size_t n)
{
    uint8_t prev = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        b[i] = (uint8_t)(b[i] + prev);
        prev = b[i];
    }
}

/* Undoes the byte predictor selected by the high bits of the compression byte. */
static pc_status unpredict(uint8_t *b, size_t n, uint8_t compression, pc_error *err)
{
    unsigned mode = compression & 0xC0;
    bool split = (compression & 0x20) != 0;
    if (mode == 0x00) {
        return PC_OK;
    }
    if (mode == 0x40) {
        byte_delta(b, n);
        return PC_OK;
    }
    if (mode == 0x80 && !split) {
        uint16_t prev = 0;
        size_t i;
        for (i = 0; i + 2 <= n; i += 2) {
            uint16_t v = (uint16_t)((uint16_t)(b[i] | b[i + 1] << 8) + prev);
            b[i] = (uint8_t)v;
            b[i + 1] = (uint8_t)(v >> 8);
            prev = v;
        }
        return PC_OK;
    }
    if (mode == 0x80) {
        byte_delta(b, n);
        /* A 64 KiB tile of 16-bit samples is stored as all high bytes, then all low bytes. */
        if (n == 0x10000) {
            uint8_t *planes = malloc(n);
            size_t i;
            if (planes == NULL) {
                return ax_nomem(err);
            }
            memcpy(planes, b, n);
            for (i = 0; i < n / 4; i++) {
                const uint8_t *h = planes + 2 * i;
                const uint8_t *l = planes + 0x8000 + 2 * i;
                b[4 * i] = l[0];
                b[4 * i + 1] = h[0];
                b[4 * i + 2] = l[1];
                b[4 * i + 3] = h[1];
            }
            free(planes);
        }
        return PC_OK;
    }
    return ax_unsupported(err, "archive entry predictor");
}

pc_status pc_af_archive_read(pc_af_archive *a, const char *name, uint8_t **out, size_t *nout, pc_error *err)
{
    const pc_af_entry *e = pc_af_archive_entry(a, name);
    size_t size;
    size_t total;
    ax_cursor c;
    const uint8_t *tag;
    const uint8_t *data;
    unsigned method;
    uint64_t stored;
    pc_buf o = PC_BUF_INIT;
    pc_status st;
    if (e == NULL) {
        return ax_malformed(err, "missing archive entry");
    }
    if (e->size > SIZE_MAX) {
        return ax_limit(err, "archive entry too large");
    }
    size = (size_t)e->size;
    if (size > a->limits.max_entry) {
        return ax_limit(err, "archive entry too large");
    }
    if (a->extracted > SIZE_MAX - size) {
        return ax_limit(err, "document too large");
    }
    total = a->extracted + size;
    if (total > a->limits.max_total) {
        return ax_limit(err, "document too large");
    }
    c.b = a->bytes;
    c.n = a->len;
    c.pos = e->offset > SIZE_MAX ? SIZE_MAX : (size_t)e->offset;
    if ((st = ax_take(&c, 4, &tag, err)) != PC_OK) {
        return st;
    }
    if (memcmp(tag, "#Fil", 4) != 0) {
        return ax_malformed(err, "archive entry has no data block");
    }
    method = e->compression & 3;
    stored = method == 1 || method == 2 ? e->compressed : e->size;
    if (stored > SIZE_MAX) {
        return ax_malformed(err, "offset beyond addressable memory");
    }
    if ((st = ax_take(&c, (size_t)stored, &data, err)) != PC_OK) {
        return st;
    }
    if (method == 1) {
        if (pc_inflate(PC_Z_ZLIB, data, (size_t)stored, size + 1, &o, NULL, NULL) != PC_OK) {
            return ax_malformed(err, "corrupt compressed entry");
        }
    } else if (method == 2) {
        if ((st = unzstd(data, (size_t)stored, size, &o, err)) != PC_OK) {
            pc_buf_fini(&o);
            return st;
        }
    } else if (pc_buf_append(&o, data, (size_t)stored, NULL) != PC_OK) {
        return ax_nomem(err);
    }
    if (o.len != size) {
        pc_buf_fini(&o);
        return ax_malformed(err, "archive entry size mismatch");
    }
    if ((st = unpredict(o.data, o.len, e->compression, err)) != PC_OK) {
        pc_buf_fini(&o);
        return st;
    }
    if (pc_crc32(0, o.data, o.len) != e->crc) {
        pc_buf_fini(&o);
        return ax_malformed(err, "archive entry checksum mismatch");
    }
    a->extracted = total;
    if (o.data == NULL) {
        *out = malloc(1);
        if (*out == NULL) {
            return ax_nomem(err);
        }
        *nout = 0;
        return PC_OK;
    }
    *nout = o.len;
    *out = pc_buf_take(&o, NULL);
    return PC_OK;
}
