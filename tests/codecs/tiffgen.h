/* A tiny hand-rolled TIFF / BigTIFF writer for tests (port of crates/codecs/tests/common/
 * tiffgen.rs): any byte order, strips or tiles, any planar configuration, several IFDs in a
 * chain plus free-standing ones (SubIFDs), and access to every offset so tests can corrupt the
 * structure precisely. */
#ifndef PC_TESTS_CODECS_TIFFGEN_H
#define PC_TESTS_CODECS_TIFFGEN_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { TG_BYTE = 1, TG_ASCII = 2, TG_SHORT = 3, TG_LONG = 4, TG_RATIONAL = 5, TG_UNDEFINED = 7, TG_IFD = 13,
       TG_LONG8 = 16, TG_IFD8 = 18 };

static inline size_t tg_type_size(uint16_t ty)
{
    switch (ty) {
    case 1:
    case 2:
    case 7:
    case 6:
        return 1;
    case 3:
    case 8:
        return 2;
    case 4:
    case 13:
    case 9:
    case 11:
        return 4;
    case 5:
    case 10:
    case 12:
    case 16:
    case 17:
    case 18:
        return 8;
    default:
        return 1;
    }
}

/* One value: a number, or (is_ifd) a reference to directory index `v`. */
typedef struct tg_val {
    uint64_t v;
    bool is_ifd;
} tg_val;

typedef struct tg_entry {
    uint16_t tag;
    uint16_t ty;
    tg_val *vals;
    size_t n;
} tg_entry;

typedef struct tg_dir {
    tg_entry *entries;
    size_t nentries;
    uint8_t **chunks;
    size_t *chunk_len;
    size_t nchunks;
    bool tiled;
} tg_dir;

static inline void tg_dir_free(tg_dir *d)
{
    size_t i;
    for (i = 0; i < d->nentries; i++) {
        free(d->entries[i].vals);
    }
    free(d->entries);
    for (i = 0; i < d->nchunks; i++) {
        free(d->chunks[i]);
    }
    free(d->chunks);
    free(d->chunk_len);
    memset(d, 0, sizeof *d);
}

/* Replaces an existing entry with the same tag, else appends. */
static inline tg_dir *tg_tag_vals(tg_dir *d, uint16_t tag, uint16_t ty, const tg_val *vals, size_t n)
{
    size_t i;
    tg_entry *e = NULL;
    for (i = 0; i < d->nentries; i++) {
        if (d->entries[i].tag == tag) {
            e = &d->entries[i];
            free(e->vals);
            break;
        }
    }
    if (e == NULL) {
        d->entries = realloc(d->entries, (d->nentries + 1) * sizeof *d->entries);
        e = &d->entries[d->nentries++];
    }
    e->tag = tag;
    e->ty = ty;
    e->n = n;
    e->vals = malloc((n > 0 ? n : 1) * sizeof *e->vals);
    if (n > 0) {
        memcpy(e->vals, vals, n * sizeof *vals);
    }
    return d;
}

static inline tg_dir *tg_tag(tg_dir *d, uint16_t tag, uint16_t ty, const uint64_t *v, size_t n)
{
    tg_val *vals = malloc((n > 0 ? n : 1) * sizeof *vals);
    size_t i;
    for (i = 0; i < n; i++) {
        vals[i].v = v[i];
        vals[i].is_ifd = false;
    }
    tg_tag_vals(d, tag, ty, vals, n);
    free(vals);
    return d;
}

static inline tg_dir *tg_tag1(tg_dir *d, uint16_t tag, uint16_t ty, uint64_t v) { return tg_tag(d, tag, ty, &v, 1); }

static inline tg_dir *tg_bytes(tg_dir *d, uint16_t tag, uint16_t ty, const uint8_t *data, size_t n)
{
    uint64_t *v = malloc((n > 0 ? n : 1) * sizeof *v);
    size_t i;
    for (i = 0; i < n; i++) {
        v[i] = data[i];
    }
    tg_tag(d, tag, ty, v, n);
    free(v);
    return d;
}

static inline tg_dir *tg_sub_ifds(tg_dir *d, uint16_t ty, const size_t *dirs, size_t n)
{
    tg_val vals[16];
    size_t i;
    for (i = 0; i < n && i < 16; i++) {
        vals[i].v = dirs[i];
        vals[i].is_ifd = true;
    }
    return tg_tag_vals(d, 330, ty, vals, n);
}

static inline void tg_add_chunk(tg_dir *d, const uint8_t *data, size_t n)
{
    d->chunks = realloc(d->chunks, (d->nchunks + 1) * sizeof *d->chunks);
    d->chunk_len = realloc(d->chunk_len, (d->nchunks + 1) * sizeof *d->chunk_len);
    d->chunks[d->nchunks] = malloc(n > 0 ? n : 1);
    if (n > 0) {
        memcpy(d->chunks[d->nchunks], data, n);
    }
    d->chunk_len[d->nchunks] = n;
    d->nchunks++;
}

static inline tg_dir tg_base(uint32_t w, uint32_t h, uint16_t bits, uint16_t spp, uint16_t photometric)
{
    tg_dir d;
    uint64_t b[64];
    size_t i;
    memset(&d, 0, sizeof d);
    tg_tag1(&d, 256, TG_LONG, w);
    tg_tag1(&d, 257, TG_LONG, h);
    for (i = 0; i < spp && i < 64; i++) {
        b[i] = bits;
    }
    tg_tag(&d, 258, TG_SHORT, b, spp);
    tg_tag1(&d, 259, TG_SHORT, 1);
    tg_tag1(&d, 262, TG_SHORT, photometric);
    tg_tag1(&d, 277, TG_SHORT, spp);
    return d;
}

/* w x h, spp samples of bits, the given photometric, chunky, uncompressed, strips of `rows`. */
static inline tg_dir tg_strips(uint32_t w, uint32_t h, uint16_t bits, uint16_t spp, uint16_t photometric, uint32_t rows,
                               const uint8_t *data, size_t n)
{
    tg_dir d = tg_base(w, h, bits, spp, photometric);
    size_t row = ((size_t)w * bits * spp + 7) / 8;
    size_t per = row * rows;
    size_t i;
    if (per == 0) {
        per = 1;
    }
    for (i = 0; i < n; i += per) {
        tg_add_chunk(&d, data + i, n - i < per ? n - i : per);
    }
    tg_tag1(&d, 278, TG_LONG, rows);
    return d;
}

typedef struct tg_built {
    uint8_t *bytes;
    size_t len;
    uint64_t *ifd_at;
    uint64_t *next_at;
    uint64_t **chunk_at;
    size_t ndirs;
} tg_built;

static inline void tg_built_free(tg_built *b)
{
    size_t i;
    free(b->bytes);
    free(b->ifd_at);
    free(b->next_at);
    for (i = 0; i < b->ndirs; i++) {
        free(b->chunk_at[i]);
    }
    free(b->chunk_at);
    memset(b, 0, sizeof *b);
}

typedef struct tg_out {
    uint8_t *b;
    size_t n;
    size_t cap;
} tg_out;

static inline void tg_put(tg_out *o, const void *d, size_t n)
{
    if (o->n + n > o->cap) {
        o->cap = (o->n + n) * 2 + 64;
        o->b = realloc(o->b, o->cap);
    }
    memcpy(o->b + o->n, d, n);
    o->n += n;
}

static inline void tg_enc(tg_out *o, bool little, uint64_t v, size_t k)
{
    uint8_t b[8];
    size_t i;
    for (i = 0; i < k; i++) {
        b[i] = (uint8_t)(v >> (8 * (little ? i : (k - 1 - i))));
    }
    tg_put(o, b, k);
}

static inline int tg_entry_cmp(const void *a, const void *b)
{
    return (int)((const tg_entry *)a)->tag - (int)((const tg_entry *)b)->tag;
}

/* Builds a file. chain lists the directories of the main IFD chain in order; the others are
 * reachable only through SubIFD references. The dirs are modified (tables added, sorted). */
static inline tg_built tg_build(bool little, bool big, tg_dir *dirs, size_t ndirs, const size_t *chain, size_t nchain)
{
    size_t header = big ? 16 : 8;
    size_t entry_len = big ? 20 : 12;
    size_t inl = big ? 8 : 4;
    uint16_t off_ty = big ? TG_LONG8 : TG_LONG;
    tg_built r;
    tg_out f = {NULL, 0, 0};
    uint64_t pos;
    size_t k;
    size_t i;
    memset(&r, 0, sizeof r);
    r.ndirs = ndirs;
    r.ifd_at = calloc(ndirs, sizeof *r.ifd_at);
    r.next_at = calloc(ndirs, sizeof *r.next_at);
    r.chunk_at = calloc(ndirs, sizeof *r.chunk_at);
    /* complete entry lists with chunk offsets / byte counts, sorted */
    for (k = 0; k < ndirs; k++) {
        tg_dir *d = &dirs[k];
        uint16_t ot = d->tiled ? 324 : 273;
        uint16_t ct = d->tiled ? 325 : 279;
        bool has_ot = false;
        for (i = 0; i < d->nentries; i++) {
            has_ot = has_ot || d->entries[i].tag == ot;
        }
        if (d->nchunks > 0 || !has_ot) {
            uint64_t *zeros = calloc(d->nchunks > 0 ? d->nchunks : 1, sizeof *zeros);
            uint64_t *lens = calloc(d->nchunks > 0 ? d->nchunks : 1, sizeof *lens);
            for (i = 0; i < d->nchunks; i++) {
                lens[i] = d->chunk_len[i];
            }
            tg_tag(d, ot, off_ty, zeros, d->nchunks);
            tg_tag(d, ct, off_ty, lens, d->nchunks);
            free(zeros);
            free(lens);
        }
        qsort(d->entries, d->nentries, sizeof *d->entries, tg_entry_cmp);
    }
    /* pass 1: positions (header, then per directory its chunks, out-of-line values, table) */
    pos = header;
    for (k = 0; k < ndirs; k++) {
        tg_dir *d = &dirs[k];
        r.chunk_at[k] = calloc(d->nchunks > 0 ? d->nchunks : 1, sizeof **r.chunk_at);
        for (i = 0; i < d->nchunks; i++) {
            r.chunk_at[k][i] = pos;
            pos += d->chunk_len[i];
        }
        for (i = 0; i < d->nentries; i++) {
            size_t n = tg_type_size(d->entries[i].ty) * d->entries[i].n;
            if (n > inl) {
                pos += n;
            }
        }
        pos += pos % 2;
        r.ifd_at[k] = pos;
        pos += (big ? 8 : 2) + d->nentries * entry_len + (big ? 8 : 4);
    }
    /* pass 2: bytes */
    tg_put(&f, little ? "II" : "MM", 2);
    {
        uint64_t first = nchain > 0 ? r.ifd_at[chain[0]] : 0;
        if (big) {
            tg_enc(&f, little, 43, 2);
            tg_enc(&f, little, 8, 2);
            tg_enc(&f, little, 0, 2);
            tg_enc(&f, little, first, 8);
        } else {
            tg_enc(&f, little, 42, 2);
            tg_enc(&f, little, first, 4);
        }
    }
    for (k = 0; k < ndirs; k++) {
        tg_dir *d = &dirs[k];
        uint16_t ot = d->tiled ? 324 : 273;
        tg_out table = {NULL, 0, 0};
        uint64_t next = 0;
        size_t p;
        for (i = 0; i < d->nchunks; i++) {
            tg_put(&f, d->chunks[i], d->chunk_len[i]);
        }
        tg_enc(&table, little, d->nentries, big ? 8 : 2);
        for (i = 0; i < d->nentries; i++) {
            const tg_entry *e = &d->entries[i];
            size_t sz = tg_type_size(e->ty);
            tg_out data = {NULL, 0, 0};
            size_t j;
            for (j = 0; j < e->n; j++) {
                uint64_t v = e->vals[j].v;
                if (e->tag == ot && d->nchunks > 0) {
                    v = r.chunk_at[k][j];
                } else if (e->vals[j].is_ifd) {
                    v = r.ifd_at[v];
                }
                tg_enc(&data, little, v, sz);
            }
            tg_enc(&table, little, e->tag, 2);
            tg_enc(&table, little, e->ty, 2);
            tg_enc(&table, little, e->n, big ? 8 : 4);
            if (data.n <= inl) {
                uint8_t pad[8] = {0};
                if (data.n > 0) {
                    tg_put(&table, data.b, data.n);
                }
                tg_put(&table, pad, inl - data.n);
            } else {
                uint64_t at = f.n;
                tg_put(&f, data.b, data.n);
                tg_enc(&table, little, at, inl);
            }
            free(data.b);
        }
        if (f.n % 2 == 1) {
            uint8_t z = 0;
            tg_put(&f, &z, 1);
        }
        for (p = 0; p < nchain; p++) {
            if (chain[p] == k && p + 1 < nchain) {
                next = r.ifd_at[chain[p + 1]];
            }
        }
        tg_put(&f, table.b, table.n);
        free(table.b);
        r.next_at[k] = f.n;
        tg_enc(&f, little, next, big ? 8 : 4);
    }
    r.bytes = f.b;
    r.len = f.n;
    return r;
}

static inline void tg_set_next(tg_built *b, bool little, bool big, size_t k, uint64_t to)
{
    size_t n = big ? 8 : 4;
    size_t i;
    uint64_t v = big ? to : (uint32_t)to;
    for (i = 0; i < n; i++) {
        b->bytes[b->next_at[k] + i] = (uint8_t)(v >> (8 * (little ? i : (n - 1 - i))));
    }
}

static inline uint64_t tg_rd(const tg_built *b, bool little, size_t at, size_t n)
{
    uint64_t v = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        uint8_t byte = little ? b->bytes[at + n - 1 - i] : b->bytes[at + i];
        v = (v << 8) | byte;
    }
    return v;
}

/* Entry tag of directory k: entry position, type, count, value position. */
static inline bool tg_find_entry(const tg_built *b, bool little, bool big, size_t k, uint16_t tag, size_t *e_at,
                                 uint16_t *ty, uint64_t *count, size_t *value_at)
{
    size_t at = (size_t)b->ifd_at[k];
    size_t n = (size_t)(big ? tg_rd(b, little, at, 8) : tg_rd(b, little, at, 2));
    size_t first = at + (big ? 8 : 2);
    size_t entry = big ? 20 : 12;
    size_t inl = big ? 8 : 4;
    size_t i;
    for (i = 0; i < n; i++) {
        size_t e = first + i * entry;
        size_t field = e + (big ? 12 : 8);
        if ((uint16_t)tg_rd(b, little, e, 2) != tag) {
            continue;
        }
        *e_at = e;
        *ty = (uint16_t)tg_rd(b, little, e + 2, 2);
        *count = big ? tg_rd(b, little, e + 4, 8) : tg_rd(b, little, e + 4, 4);
        *value_at = *count * tg_type_size(*ty) <= inl ? field : (size_t)tg_rd(b, little, field, inl);
        return true;
    }
    return false;
}

static inline void tg_patch_value(tg_built *b, bool little, bool big, size_t k, uint16_t tag, size_t i, uint64_t value)
{
    size_t e;
    uint16_t ty;
    uint64_t count;
    size_t at;
    size_t n;
    size_t j;
    if (!tg_find_entry(b, little, big, k, tag, &e, &ty, &count, &at)) {
        return;
    }
    n = tg_type_size(ty);
    at += i * n;
    for (j = 0; j < n; j++) {
        b->bytes[at + j] = (uint8_t)(value >> (8 * (little ? j : (n - 1 - j))));
    }
}

static inline void tg_patch_count(tg_built *b, bool little, bool big, size_t k, uint16_t tag, uint64_t count)
{
    size_t e;
    uint16_t ty;
    uint64_t c;
    size_t at;
    size_t n = big ? 8 : 4;
    size_t j;
    uint64_t v = big ? count : (uint32_t)count;
    if (!tg_find_entry(b, little, big, k, tag, &e, &ty, &c, &at)) {
        return;
    }
    for (j = 0; j < n; j++) {
        b->bytes[e + 4 + j] = (uint8_t)(v >> (8 * (little ? j : (n - 1 - j))));
    }
}

/* Samples to file bytes in the given order (new arrays). */
static inline uint8_t *tg_u16s(const uint16_t *v, size_t n, bool little)
{
    uint8_t *o = malloc(2 * n + 1);
    size_t i;
    for (i = 0; i < n; i++) {
        o[2 * i] = little ? (uint8_t)v[i] : (uint8_t)(v[i] >> 8);
        o[2 * i + 1] = little ? (uint8_t)(v[i] >> 8) : (uint8_t)v[i];
    }
    return o;
}

static inline uint8_t *tg_f32s(const float *v, size_t n, bool little)
{
    uint8_t *o = malloc(4 * n + 1);
    size_t i;
    for (i = 0; i < n; i++) {
        uint32_t u;
        size_t k;
        memcpy(&u, &v[i], 4);
        for (k = 0; k < 4; k++) {
            o[4 * i + k] = (uint8_t)(u >> (8 * (little ? k : (3 - k))));
        }
    }
    return o;
}

/* Splits an interleaved image into tw x th tiles padded with zeros; chunky (one blob per tile)
 * or planar (all tiles of plane 0, then plane 1, ...), added to d. */
static inline void tg_tile_split(tg_dir *d, const uint8_t *px, size_t w, size_t h, size_t spp, size_t bps, size_t tw,
                                 size_t th, bool planar)
{
    size_t across = (w + tw - 1) / tw;
    size_t down = (h + th - 1) / th;
    size_t planes = planar ? spp : 1;
    size_t per = planar ? 1 : spp;
    size_t p;
    size_t ty;
    size_t tx;
    for (p = 0; p < planes; p++) {
        for (ty = 0; ty < down; ty++) {
            for (tx = 0; tx < across; tx++) {
                size_t len = tw * th * per * bps;
                uint8_t *t = calloc(len > 0 ? len : 1, 1);
                size_t x;
                size_t y;
                for (y = 0; y < th; y++) {
                    for (x = 0; x < tw; x++) {
                        size_t ix = tx * tw + x;
                        size_t iy = ty * th + y;
                        size_t s;
                        if (ix >= w || iy >= h) {
                            continue;
                        }
                        for (s = 0; s < per; s++) {
                            size_t src = ((iy * w + ix) * spp + (planar ? p : s)) * bps;
                            size_t dst = ((y * tw + x) * per + s) * bps;
                            memcpy(t + dst, px + src, bps);
                        }
                    }
                }
                tg_add_chunk(d, t, len);
                free(t);
            }
        }
    }
}

/* Planar strips of `rows` rows: plane 0's strips, then plane 1's, ... added to d. */
static inline void tg_planar_strips(tg_dir *d, const uint8_t *px, size_t w, size_t h, size_t spp, size_t bps,
                                    size_t rows)
{
    size_t p;
    size_t y0;
    for (p = 0; p < spp; p++) {
        for (y0 = 0; y0 < h; y0 += rows) {
            size_t y1 = y0 + rows < h ? y0 + rows : h;
            size_t len = (y1 - y0) * w * bps;
            uint8_t *s = malloc(len > 0 ? len : 1);
            size_t k = 0;
            size_t y;
            for (y = y0; y < y1; y++) {
                size_t x;
                for (x = 0; x < w; x++) {
                    memcpy(s + k, px + ((y * w + x) * spp + p) * bps, bps);
                    k += bps;
                }
            }
            tg_add_chunk(d, s, len);
            free(s);
        }
    }
}

#endif
