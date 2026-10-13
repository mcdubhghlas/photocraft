/* The object store's building blocks (store.rs): sources, the verified loader side (Fetch), the
 * collecting saver side (Sink) with its tile-hash cache, prepared bundles and helpers. The writer
 * is in store_save.c, the load driver in lib.c. */
#include "format/bridge.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/compress.h"
#include "base/file.h"
#include "base/hash.h"
#include "base/mem.h"
#include "base/num.h"
#include "codecs/codecs.h"
#include "format/pathmap.h"

/* ---- helpers ---- */

bool pc_format_is_valid_hash(const char *h, size_t len)
{
    if (len != PC_FORMAT_HASH_HEX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = h[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool host_is_big_endian(void)
{
    const uint16_t one = 1;
    uint8_t first;
    memcpy(&first, &one, 1);
    return first == 0;
}

void pc_format_swap_to_le(uint8_t *bytes, size_t len, pc_sample_type s)
{
    size_t w = pc_sample_type_bytes(s);
    if (!host_is_big_endian() || w < 2) {
        return;
    }
    for (size_t i = 0; i + w <= len; i += w) {
        for (size_t a = 0, b = w - 1; a < b; a++, b--) {
            uint8_t t = bytes[i + a];
            bytes[i + a] = bytes[i + b];
            bytes[i + b] = t;
        }
    }
}

pc_status pc_format_check_manifest_depth(const uint8_t *json, size_t len, pc_error *err)
{
    size_t depth = 0;
    bool in_str = false;
    bool escaped = false;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = json[i];
        if (in_str) {
            if (escaped) {
                escaped = false;
            } else if (b == '\\') {
                escaped = true;
            } else if (b == '"') {
                in_str = false;
            }
            continue;
        }
        if (b == '"') {
            in_str = true;
        } else if (b == '[' || b == '{') {
            if (++depth > PC_FORMAT_MAX_MANIFEST_DEPTH) {
                return pc_format_limit(err, "manifest JSON nested deeper than %d levels",
                                       PC_FORMAT_MAX_MANIFEST_DEPTH);
            }
        } else if ((b == ']' || b == '}') && depth > 0) {
            depth--;
        }
    }
    return PC_OK;
}

pc_status pc_format_write_atomic(const char *path, const void *data, size_t len, pc_error *err)
{
    pc_status s = pc_atomic_write(path, data, len, err);
    return s == PC_OK ? s : pc_format_io(s, err);
}

pc_status pc_format_png(const pc_rgba8_image *img, pc_buf *out, pc_error *err)
{
    pc_cimage image;
    pc_cencode_opts o = pc_cencode_opts_default();
    pc_buf png = PC_BUF_INIT;
    pc_error inner = {0};
    size_t n = 0;
    pc_status s;

    if (!pc_size_mul3(img->width, img->height, 4, &n)) {
        n = SIZE_MAX;
    }
    s = pc_cimage_from_copy(img->width, img->height, PC_CL_RGBA, PC_CS_U8, img->pixels,
                            img->pixels != NULL ? n : 0, &image, &inner);
    if (s == PC_OK) {
        s = pc_cencode(&image, PC_CF_PNG, &o, &png, &inner);
        pc_cimage_fini(&image);
    }
    if (s == PC_ERR_NOMEM) {
        return pc_fail(err, PC_ERR_NOMEM, "%s", inner.msg);
    }
    if (s != PC_OK) {
        return pc_format_unsupported(err, "preview: %s", inner.msg);
    }
    pc_buf_fini(out);
    *out = png;
    return PC_OK;
}

/* ---- sources ---- */

struct pc_format_source {
    bool is_zip;
    pc_zip_reader zip;
    char *root;
    pc_format_byte_source_get get; /* a caller's byte source (root and zip unused) */
    void *ctx;
};

pc_status pc_format_source_zip(const uint8_t *bytes, size_t len, pc_format_source **out, pc_error *err)
{
    pc_format_source *src = calloc(1, sizeof *src);
    pc_status s;
    if (src == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    s = pc_zip_reader_open(&src->zip, bytes, len, err);
    if (s != PC_OK) {
        free(src);
        return s;
    }
    src->is_zip = true;
    *out = src;
    return PC_OK;
}

pc_status pc_format_source_dir(const char *root, pc_format_source **out, pc_error *err)
{
    pc_format_source *src = calloc(1, sizeof *src);
    if (src == NULL || (src->root = pc_strndup(root, strlen(root))) == NULL) {
        free(src);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    *out = src;
    return PC_OK;
}

pc_status pc_format_source_custom(pc_format_byte_source_get get, void *ctx, pc_format_source **out,
                                  pc_error *err)
{
    pc_format_source *src = calloc(1, sizeof *src);
    if (src == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    src->get = get;
    src->ctx = ctx;
    *out = src;
    return PC_OK;
}

void pc_format_source_free(pc_format_source *src)
{
    if (src != NULL) {
        if (src->is_zip) {
            pc_zip_reader_close(&src->zip);
        }
        free(src->root);
        free(src);
    }
}

pc_status pc_format_read_entry(int fd, uint64_t len_hint, size_t max, const char *path, pc_buf *out,
                               pc_error *err)
{
    /* The stat length is a buffer hint only (files can change while open, a FIFO reports 0): the
     * bound is enforced on the read itself. */
    pc_status s;
    (void)len_hint;
    s = pc_fd_read_all(fd, max, out, err);
    if (s == PC_ERR_LIMIT) {
        return pc_format_limit(err, "`%s` exceeds %zu bytes", path, max);
    }
    return s == PC_OK || s == PC_ERR_NOMEM ? s : pc_format_io(s, err);
}

static pc_status dir_get(const char *root, const char *path, size_t max, pc_buf *out, pc_error *err)
{
    pc_buf full = PC_BUF_INIT;
    struct stat st;
    pc_status s;
    int fd;
    size_t rl = strlen(root);

    s = pc_buf_appendf(&full, err, "%s%s%s", root, rl > 0 && root[rl - 1] == '/' ? "" : "/", path);
    if (s != PC_OK) {
        return s;
    }
    if (stat(pc_buf_cstr(&full), &st) != 0) {
        pc_buf_fini(&full);
        return pc_format_corrupt(err, "missing `%s`", path);
    }
    if (!S_ISREG(st.st_mode)) {
        pc_buf_fini(&full);
        return pc_format_corrupt(err, "`%s` is not a regular file", path);
    }
    /* O_NONBLOCK: a FIFO swapped in after the check fails instead of hanging the load. */
    do {
        fd = open(pc_buf_cstr(&full), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    pc_buf_fini(&full);
    if (fd < 0) {
        return pc_format_corrupt(err, "missing `%s`", path);
    }
    s = pc_format_read_entry(fd, st.st_size > 0 ? (uint64_t)st.st_size : 0, max, path, out, err);
    (void)close(fd);
    return s;
}

pc_status pc_format_source_get(const pc_format_source *src, const char *path, size_t max, pc_buf *out,
                               pc_error *err)
{
    if (src->is_zip) {
        return pc_zip_read_by_name(&src->zip, path, max, out, err);
    }
    if (src->get != NULL) {
        return src->get(src->ctx, path, max, out, err);
    }
    return dir_get(src->root, path, max, out, err);
}

pc_status pc_format_read_manifest_raw(const pc_format_source *src, size_t max, pc_buf *out, pc_error *err)
{
    pc_buf raw = PC_BUF_INIT;
    pc_status s = pc_format_source_get(src, PC_FORMAT_MANIFEST, max, &raw, err);
    if (s == PC_OK) {
        /* serde_json stops at 128 levels (about 40 nested groups); the bounded check replaces
         * that limit. */
        s = pc_format_check_manifest_depth(raw.data, raw.len, err);
    }
    if (s != PC_OK) {
        pc_buf_fini(&raw);
        return s;
    }
    pc_buf_fini(out);
    *out = raw;
    return PC_OK;
}

/* ---- loading objects ---- */

struct pc_format_fetch {
    const pc_format_source *src;
    pc_format_load_opts opts;
    uint64_t total;
    pc_pathmap blobs; /* hash -> pc_blob (ptr) */
};

pc_status pc_format_fetch_new(const pc_format_source *src, const pc_format_load_opts *opts,
                              pc_format_fetch **out, pc_error *err)
{
    pc_format_fetch *f = calloc(1, sizeof *f);
    if (f == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    f->src = src;
    f->opts = *opts;
    *out = f;
    return PC_OK;
}

void pc_format_fetch_free(pc_format_fetch *f)
{
    if (f != NULL) {
        for (size_t i = 0; i < f->blobs.cap; i++) {
            if (f->blobs.slots[i].used) {
                pc_blob_release(f->blobs.slots[i].ptr);
            }
        }
        pc_pathmap_fini(&f->blobs);
        free(f);
    }
}

static pc_status account(pc_format_fetch *f, size_t n, pc_error *err)
{
    f->total = (uint64_t)n > UINT64_MAX - f->total ? UINT64_MAX : f->total + n;
    if (f->total > f->opts.max_total_bytes) {
        return pc_format_limit(err, "bundle expands beyond %llu bytes",
                               (unsigned long long)f->opts.max_total_bytes);
    }
    return PC_OK;
}

/* zstd-decode one frame into at most max_out bytes; errors are corrupt "<path>: zstd: ...". */
static pc_status unzstd(const pc_buf *z, size_t max_out, const char *path, pc_buf *out, pc_error *err)
{
    pc_status s = pc_zstd_decompress(z->data, z->len, max_out, 0, out, NULL, err);
    if (s == PC_OK || s == PC_ERR_NOMEM) {
        return s;
    }
    {
        char inner[PC_ERROR_MSG_MAX];
        (void)snprintf(inner, sizeof inner, "%s", err != NULL ? err->msg : "zstd: corrupt frame");
        return pc_format_corrupt(err, "%s: %s", path, inner);
    }
}

static bool hash_matches(const pc_buf *data, const char *hash)
{
    char hex[PC_BLAKE3_HEX_LEN + 1];
    pc_blake3_hex(data->data, data->len, hex);
    return memcmp(hex, hash, PC_BLAKE3_HEX_LEN) == 0;
}

pc_status pc_format_fetch_tile(pc_format_fetch *f, const char *hash, size_t len, pc_buf *out, pc_error *err)
{
    char path[PC_FORMAT_OBJECT_PATH_MAX];
    pc_buf z = PC_BUF_INIT;
    pc_buf data = PC_BUF_INIT;
    size_t max;
    pc_status s;

    if (!pc_format_is_valid_hash(hash, strlen(hash))) {
        return pc_format_corrupt(err, "invalid tile hash `%s`", hash);
    }
    if ((s = account(f, len, err)) != PC_OK) {
        return s;
    }
    (void)snprintf(path, sizeof path, "tiles/%s.zst", hash);
    /* Compressed tiles never exceed their size plus framing; bound damaged ones by it. */
    if (!pc_size_add(len, len / 8, &max) || !pc_size_add(max, 4096, &max)) {
        max = SIZE_MAX;
    }
    s = pc_format_source_get(f->src, path, max, &z, err);
    if (s == PC_OK) {
        s = unzstd(&z, len == SIZE_MAX ? len : len + 1, path, &data, err);
    }
    pc_buf_fini(&z);
    if (s == PC_OK && data.len != len) {
        s = pc_format_corrupt(err, "%s: expected %zu bytes, got %zu", path, len, data.len);
    }
    if (s == PC_OK && !hash_matches(&data, hash)) {
        s = pc_format_corrupt(err, "%s: content does not match its hash", path);
    }
    if (s != PC_OK) {
        pc_buf_fini(&data);
        return s;
    }
    pc_buf_fini(out);
    *out = data;
    return PC_OK;
}

pc_status pc_format_fetch_blob(pc_format_fetch *f, const char *hash, pc_blob **out, pc_error *err)
{
    char path[PC_FORMAT_OBJECT_PATH_MAX];
    pc_buf z = PC_BUF_INIT;
    pc_buf data = PC_BUF_INIT;
    pc_blob *b = NULL;
    pc_pathmap_ent *e;
    size_t max = f->opts.max_blob_bytes;
    bool inserted;
    pc_status s;

    if (strlen(hash) < PC_PATHMAP_KEY_MAX && (e = pc_pathmap_get(&f->blobs, hash)) != NULL) {
        *out = pc_blob_retain(e->ptr);
        return PC_OK;
    }
    if (!pc_format_is_valid_hash(hash, strlen(hash))) {
        return pc_format_corrupt(err, "invalid blob hash `%s`", hash);
    }
    (void)snprintf(path, sizeof path, "blobs/%s.zst", hash);
    s = pc_format_source_get(f->src, path, max, &z, err);
    if (s == PC_OK) {
        /* A blob's length is not in the manifest; the frame bounds it and we cap at max. */
        s = unzstd(&z, max == SIZE_MAX ? max : max + 1, path, &data, err);
    }
    pc_buf_fini(&z);
    if (s == PC_OK && data.len > max) {
        s = pc_format_limit(err, "%s exceeds %zu bytes", path, max);
    }
    if (s == PC_OK) {
        s = account(f, data.len, err);
    }
    if (s == PC_OK && !hash_matches(&data, hash)) {
        s = pc_format_corrupt(err, "%s: content does not match its hash", path);
    }
    if (s == PC_OK) {
        s = pc_blob_new(data.data, data.len, &b, err);
    }
    pc_buf_fini(&data);
    if (s == PC_OK) {
        s = pc_pathmap_put(&f->blobs, hash, &e, &inserted, err);
        if (s == PC_OK) {
            e->ptr = pc_blob_retain(b);
        }
    }
    if (s != PC_OK) {
        pc_blob_release(b);
        return s;
    }
    *out = b;
    return PC_OK;
}

/* ---- the tile-hash cache (by tile identity) ----
 *
 * The reference keys it by Arc pointer with a Weak reference. C has no weak references, so
 * entries hold a strong one, and every prepare drops the entries its document no longer
 * references (the reference drops them once the tile itself is gone). Holding a reference makes a
 * later in-place edit of that tile copy it first (copy-on-write), which is exactly what keeps a
 * cached hash from going stale. Caching is best effort: out of memory just skips it. */

typedef struct cache_ent {
    pc_raster_tile *tile; /* NULL = empty slot */
    uint64_t gen;
    char hex[PC_FORMAT_HASH_HEX + 1];
} cache_ent;

struct pc_format_hash_cache {
    cache_ent *slots;
    size_t cap; /* 0 or a power of two */
    size_t count;
    uint64_t gen;
};

static size_t ptr_hash(const void *p)
{
    uint64_t v = (uint64_t)(uintptr_t)p;
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdull;
    v ^= v >> 33;
    return (size_t)v;
}

pc_status pc_format_hash_cache_new(pc_format_hash_cache **out, pc_error *err)
{
    pc_format_hash_cache *c = calloc(1, sizeof *c);
    if (c == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    *out = c;
    return PC_OK;
}

void pc_format_hash_cache_free(pc_format_hash_cache *c)
{
    if (c != NULL) {
        for (size_t i = 0; i < c->cap; i++) {
            pc_raster_tile_release(c->slots[i].tile);
        }
        free(c->slots);
        free(c);
    }
}

static cache_ent *cache_find(const pc_format_hash_cache *c, const pc_raster_tile *t)
{
    if (c->cap == 0) {
        return NULL;
    }
    for (size_t i = ptr_hash(t) & (c->cap - 1); c->slots[i].tile != NULL; i = (i + 1) & (c->cap - 1)) {
        if (c->slots[i].tile == t) {
            return &c->slots[i];
        }
    }
    return NULL;
}

/* Rebuilds the table at `cap` slots keeping entries of generation >= keep_gen (releasing the
 * rest). False on out of memory (the table is then unchanged). */
static bool cache_rebuild(pc_format_hash_cache *c, size_t cap, uint64_t keep_gen)
{
    cache_ent *slots = cap > 0 ? pc_calloc_array(cap, sizeof *slots) : NULL;
    size_t count = 0;
    if (cap > 0 && slots == NULL) {
        return false;
    }
    for (size_t j = 0; j < c->cap; j++) {
        cache_ent *e = &c->slots[j];
        if (e->tile == NULL) {
            continue;
        }
        if (e->gen < keep_gen || cap == 0) {
            pc_raster_tile_release(e->tile);
            continue;
        }
        {
            size_t i = ptr_hash(e->tile) & (cap - 1);
            while (slots[i].tile != NULL) {
                i = (i + 1) & (cap - 1);
            }
            slots[i] = *e;
            count++;
        }
    }
    free(c->slots);
    c->slots = slots;
    c->cap = cap;
    c->count = count;
    return true;
}

static void cache_insert(pc_format_hash_cache *c, pc_raster_tile *t, const char *hex)
{
    size_t i;
    if ((c->count + 1) * 4 > c->cap * 3) {
        size_t cap = c->cap == 0 ? 64 : c->cap * 2;
        if (cap > SIZE_MAX / 2 / sizeof(cache_ent) || !cache_rebuild(c, cap, 0)) {
            return;
        }
    }
    for (i = ptr_hash(t) & (c->cap - 1); c->slots[i].tile != NULL; i = (i + 1) & (c->cap - 1)) {
    }
    c->slots[i].tile = pc_raster_tile_retain(t);
    c->slots[i].gen = c->gen;
    memcpy(c->slots[i].hex, hex, sizeof c->slots[i].hex);
    c->count++;
}

void pc_format_hash_cache_begin(pc_format_hash_cache *c)
{
    c->gen++;
}

void pc_format_hash_cache_end(pc_format_hash_cache *c)
{
    size_t live = 0;
    size_t cap = 0;
    for (size_t i = 0; i < c->cap; i++) {
        if (c->slots[i].tile != NULL && c->slots[i].gen == c->gen) {
            live++;
        }
    }
    if (live > 0) {
        cap = 64;
        while (cap * 3 < live * 4 + 4) {
            cap *= 2;
        }
    }
    if (!cache_rebuild(c, cap, c->gen)) {
        (void)cache_rebuild(c, 0, c->gen); /* out of memory: drop the cache */
    }
}

/* ---- collecting objects ---- */

struct pc_format_sink {
    pc_format_hash_cache *cache;
    pc_format_object *objects;
    size_t count;
    size_t cap;
    pc_pathmap index; /* path -> objects[index] */
};

pc_status pc_format_sink_new(pc_format_hash_cache *cache, pc_format_sink **out, pc_error *err)
{
    pc_format_sink *s = calloc(1, sizeof *s);
    if (s == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    s->cache = cache;
    *out = s;
    return PC_OK;
}

static void objects_free(pc_format_object *o, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        pc_raster_tile_release(o[i].tile);
        pc_blob_release(o[i].bytes);
    }
    free(o);
}

void pc_format_sink_free(pc_format_sink *s)
{
    if (s != NULL) {
        objects_free(s->objects, s->count);
        pc_pathmap_fini(&s->index);
        free(s);
    }
}

/* Adds the object at path unless present (the first one recorded wins). Takes over the
 * references in `o` only when it returns PC_OK with *added. */
static pc_status sink_add(pc_format_sink *s, const pc_format_object *o, bool *added, pc_error *err)
{
    pc_pathmap_ent *e;
    bool inserted;
    pc_status st;
    if (s->count == s->cap) {
        pc_format_object *grown = pc_grow_array(s->objects, &s->cap, s->count + 1, sizeof *grown);
        if (grown == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        s->objects = grown;
    }
    st = pc_pathmap_put(&s->index, o->path, &e, &inserted, err);
    if (st != PC_OK) {
        return st;
    }
    *added = inserted;
    if (inserted) {
        e->index = s->count;
        s->objects[s->count++] = *o;
    }
    return PC_OK;
}

pc_status pc_format_sink_tile(pc_format_sink *s, pc_pixel_format f, const pc_raster_tile *ctile,
                              char hash[PC_FORMAT_HASH_HEX + 1], pc_error *err)
{
    pc_format_object o;
    /* Taking a reference doesn't change the tile; the sink only reads it. */
    pc_raster_tile *tile = (pc_raster_tile *)(uintptr_t)ctile;
    cache_ent *ce = s->cache != NULL ? cache_find(s->cache, tile) : NULL;
    bool added = false;
    pc_status st;

    if (ce != NULL) {
        ce->gen = s->cache->gen;
        memcpy(hash, ce->hex, PC_FORMAT_HASH_HEX + 1);
    } else {
        const uint8_t *bytes = pc_raster_tile_bytes(tile);
        size_t n = pc_raster_tile_len(tile);
        if (host_is_big_endian() && pc_sample_type_bytes(f.sample) > 1) {
            uint8_t *le = pc_malloc(n);
            if (le == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "out of memory");
            }
            memcpy(le, bytes, n);
            pc_format_swap_to_le(le, n, f.sample);
            pc_blake3_hex(le, n, hash);
            free(le);
        } else {
            pc_blake3_hex(bytes, n, hash);
        }
        if (s->cache != NULL) {
            cache_insert(s->cache, tile, hash);
        }
    }
    memset(&o, 0, sizeof o);
    (void)snprintf(o.path, sizeof o.path, "tiles/%s.zst", hash);
    o.is_tile = true;
    o.tile = tile;
    o.sample = f.sample;
    st = sink_add(s, &o, &added, err);
    if (st == PC_OK && added) {
        (void)pc_raster_tile_retain(tile);
    }
    return st;
}

pc_status pc_format_sink_blob(pc_format_sink *s, pc_blob *data, char hash[PC_FORMAT_HASH_HEX + 1],
                              pc_error *err)
{
    pc_format_object o;
    bool added = false;
    pc_status st;
    pc_blake3_hex(pc_blob_data(data), pc_blob_len(data), hash);
    memset(&o, 0, sizeof o);
    (void)snprintf(o.path, sizeof o.path, "blobs/%s.zst", hash);
    o.bytes = data;
    st = sink_add(s, &o, &added, err);
    if (st == PC_OK && added) {
        (void)pc_blob_retain(data);
    }
    return st;
}

pc_status pc_format_sink_bytes(pc_format_sink *s, bool is_tile, const uint8_t *bytes, size_t len,
                               char hash[PC_FORMAT_HASH_HEX + 1], pc_error *err)
{
    pc_format_object o;
    pc_blob *b = NULL;
    bool added = false;
    pc_status st = pc_blob_new(bytes, len, &b, err);
    if (st != PC_OK) {
        return st;
    }
    pc_blake3_hex(bytes, len, hash);
    memset(&o, 0, sizeof o);
    (void)snprintf(o.path, sizeof o.path, "%s/%s.zst", is_tile ? "tiles" : "blobs", hash);
    o.is_tile = is_tile;
    o.sample = PC_SAMPLE_TYPE_U8; /* already little-endian */
    o.bytes = b;
    st = sink_add(s, &o, &added, err);
    if (st != PC_OK || !added) {
        pc_blob_release(b);
    }
    return st;
}

/* ---- prepared bundles ---- */

static int object_cmp(const void *a, const void *b)
{
    return strcmp(((const pc_format_object *)a)->path, ((const pc_format_object *)b)->path);
}

pc_status pc_format_prepared_from_sink(pc_format_sink *sink, const uint8_t *manifest, size_t len,
                                       pc_format_prepared *out, pc_error *err)
{
    pc_format_prepared p;
    pc_status s;
    memset(&p, 0, sizeof p);
    s = pc_buf_append(&p.manifest, manifest, len, err);
    if (s != PC_OK) {
        return s;
    }
    p.objects = sink->objects;
    p.nobjects = sink->count;
    sink->objects = NULL;
    sink->count = 0;
    sink->cap = 0;
    pc_pathmap_fini(&sink->index);
    if (p.nobjects > 1) {
        qsort(p.objects, p.nobjects, sizeof *p.objects, object_cmp);
    }
    for (size_t i = 0; i < p.nobjects; i++) {
        if (p.objects[i].is_tile) {
            p.stats.tiles_total++;
        } else {
            p.stats.blobs_total++;
        }
    }
    p.stats.manifest_bytes = len;
    *out = p;
    return PC_OK;
}

pc_status pc_format_prepared_add_preview(pc_format_prepared *p, const char *name, const uint8_t *png,
                                         size_t len, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    pc_status s;
    if (p->npreviews >= 2) {
        return pc_fail(err, PC_ERR_INVALID, "at most two previews");
    }
    s = pc_buf_append(&b, png, len, err);
    if (s != PC_OK) {
        return s;
    }
    p->previews[p->npreviews].name = name;
    p->previews[p->npreviews].png = b;
    p->npreviews++;
    return PC_OK;
}

void pc_format_prepared_fini(pc_format_prepared *p)
{
    if (p != NULL) {
        pc_buf_fini(&p->manifest);
        objects_free(p->objects, p->nobjects);
        for (size_t i = 0; i < p->npreviews; i++) {
            pc_buf_fini(&p->previews[i].png);
        }
        memset(p, 0, sizeof *p);
    }
}

pc_status pc_format_object_content(const pc_format_object *o, pc_buf *tmp, const uint8_t **data, size_t *len,
                                   pc_error *err)
{
    if (o->tile == NULL) {
        *data = pc_blob_data(o->bytes);
        *len = pc_blob_len(o->bytes);
        return PC_OK;
    }
    if (host_is_big_endian() && pc_sample_type_bytes(o->sample) > 1) {
        pc_status s;
        pc_buf_truncate(tmp, 0);
        s = pc_buf_append(tmp, pc_raster_tile_bytes(o->tile), pc_raster_tile_len(o->tile), err);
        if (s != PC_OK) {
            return s;
        }
        pc_format_swap_to_le(tmp->data, tmp->len, o->sample);
        *data = tmp->data;
        *len = tmp->len;
        return PC_OK;
    }
    *data = pc_raster_tile_bytes(o->tile);
    *len = pc_raster_tile_len(o->tile);
    return PC_OK;
}
