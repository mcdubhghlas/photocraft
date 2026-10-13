/* Manifest -> document (crates/format/src/convert.rs, the load half: Loader and
 * reserve_ids_through).
 *
 * Tiles and blobs are fetched through the store (verified against their hashes) in exactly the
 * reference's order, so the first damaged object reported, and the running byte total checked
 * against max_total_bytes, are the same. Values decoded from the manifest are moved into the
 * document (the manifest is only freed afterwards).
 *
 * Group nesting is checked against PC_MAX_GROUP_DEPTH before descending, as the reference does,
 * so the recursion over the layer tree is bounded. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "format/bridge.h"
#include "format/convert.h"
#include "format/manifest.h"
#include "raster/pixel.h"

/* ---- hex ---- */

static int hexdigit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool pc_format_unhex(const char *s, size_t len, uint8_t *out, size_t cap, size_t *n)
{
    size_t i;
    if (len % 2 != 0) {
        return false;
    }
    for (i = 0; i < len; i += 2) {
        int hi = hexdigit(s[i]);
        int lo = hexdigit(s[i + 1]);
        int v;
        /* Two ASCII hex digits only: from_str_radix's leading '+' is refused (#1818). */
        if (hi < 0 || lo < 0) {
            return false;
        }
        v = hi * 16 + lo;
        if (i / 2 < cap) {
            out[i / 2] = (uint8_t)v;
        }
    }
    *n = len / 2;
    return true;
}

static pc_status bad_hex(const pc_str *s, pc_error *err)
{
    return pc_format_corrupt(err, "bad hex `%s`", pc_str_c(s));
}

/* key4: four bytes from 8 hex digits. */
static pc_status key4(const pc_str *s, uint8_t out[4], pc_error *err)
{
    size_t n = 0;
    if (!pc_format_unhex(pc_str_c(s), s->len, out, 4, &n)) {
        return bad_hex(s, err);
    }
    if (n != 4) {
        return pc_format_corrupt(err, "bad 4-byte key `%s`", pc_str_c(s));
    }
    return PC_OK;
}

/* ---- stored id -> loaded id (open addressing; the last insert of an id wins) ---- */

typedef struct idmap {
    uint64_t *keys;
    uint64_t *vals;
    bool *used;
    size_t cap; /* 0 or a power of two */
    size_t count;
} idmap;

static size_t id_slot(uint64_t k, size_t cap)
{
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdull;
    k ^= k >> 33;
    return (size_t)k & (cap - 1);
}

static void idmap_fini(idmap *m)
{
    free(m->keys);
    free(m->vals);
    free(m->used);
    memset(m, 0, sizeof *m);
}

static bool idmap_get(const idmap *m, uint64_t k, uint64_t *v)
{
    if (m->cap == 0) {
        return false;
    }
    for (size_t i = id_slot(k, m->cap);; i = (i + 1) & (m->cap - 1)) {
        if (!m->used[i]) {
            return false;
        }
        if (m->keys[i] == k) {
            *v = m->vals[i];
            return true;
        }
    }
}

static void idmap_set_raw(idmap *m, uint64_t k, uint64_t v)
{
    for (size_t i = id_slot(k, m->cap);; i = (i + 1) & (m->cap - 1)) {
        if (!m->used[i] || m->keys[i] == k) {
            if (!m->used[i]) {
                m->used[i] = true;
                m->count++;
            }
            m->keys[i] = k;
            m->vals[i] = v;
            return;
        }
    }
}

static pc_status idmap_set(idmap *m, uint64_t k, uint64_t v, pc_error *err)
{
    if (m->count + 1 > m->cap / 2) {
        idmap g;
        size_t nc = m->cap == 0 ? 64 : m->cap * 2;
        if (nc < m->cap) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        memset(&g, 0, sizeof g);
        g.keys = pc_calloc_array(nc, sizeof *g.keys);
        g.vals = pc_calloc_array(nc, sizeof *g.vals);
        g.used = pc_calloc_array(nc, sizeof *g.used);
        g.cap = nc;
        if (g.keys == NULL || g.vals == NULL || g.used == NULL) {
            idmap_fini(&g);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        for (size_t i = 0; i < m->cap; i++) {
            if (m->used[i]) {
                idmap_set_raw(&g, m->keys[i], m->vals[i]);
            }
        }
        idmap_fini(m);
        *m = g;
    }
    idmap_set_raw(m, k, v);
    return PC_OK;
}

/* ---- byte-string keys -> a value or a tile (open addressing; first insert of a key wins) ---- */

typedef struct smap_ent {
    char *key; /* NULL = empty slot */
    size_t klen;
    uint64_t val;
    pc_raster_tile *tile; /* one reference, or NULL */
} smap_ent;

typedef struct smap {
    smap_ent *slots;
    size_t cap; /* 0 or a power of two */
    size_t count;
} smap;

static uint64_t smap_hash(const char *k, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull; /* FNV-1a */
    for (size_t i = 0; i < n; i++) {
        h ^= (uint8_t)k[i];
        h *= 0x100000001b3ull;
    }
    return h ^ (h >> 31);
}

static void smap_fini(smap *m)
{
    for (size_t i = 0; i < m->cap; i++) {
        free(m->slots[i].key);
        pc_raster_tile_release(m->slots[i].tile);
    }
    free(m->slots);
    memset(m, 0, sizeof *m);
}

static smap_ent *smap_get(const smap *m, const char *k, size_t n)
{
    if (m->cap == 0) {
        return NULL;
    }
    for (size_t i = (size_t)smap_hash(k, n) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        smap_ent *e = &m->slots[i];
        if (e->key == NULL) {
            return NULL;
        }
        if (e->klen == n && memcmp(e->key, k, n) == 0) {
            return e;
        }
    }
}

/* Inserts a new key (absent: the caller checked with smap_get); takes over *tile on success. */
static pc_status smap_put(smap *m, const char *k, size_t n, uint64_t val, pc_raster_tile *tile, pc_error *err)
{
    char *key;
    if (m->count + 1 > m->cap / 2) {
        size_t nc = m->cap == 0 ? 64 : m->cap * 2;
        smap_ent *v;
        if (nc < m->cap) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        v = pc_calloc_array(nc, sizeof *v);
        if (v == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        for (size_t i = 0; i < m->cap; i++) {
            if (m->slots[i].key != NULL) {
                size_t j = (size_t)smap_hash(m->slots[i].key, m->slots[i].klen) & (nc - 1);
                while (v[j].key != NULL) {
                    j = (j + 1) & (nc - 1);
                }
                v[j] = m->slots[i];
            }
        }
        free(m->slots);
        m->slots = v;
        m->cap = nc;
    }
    key = malloc(n > 0 ? n : 1);
    if (key == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (n > 0) {
        memcpy(key, k, n);
    }
    for (size_t i = (size_t)smap_hash(k, n) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        if (m->slots[i].key == NULL) {
            m->slots[i] = (smap_ent){key, n, val, tile};
            m->count++;
            return PC_OK;
        }
    }
}

/* ---- the loader ---- */

typedef struct loader {
    pc_format_fetch *fetch;
    bool preserve_ids;
    idmap ids;          /* stored layer id -> loaded id, for layer comp states and slices */
    idmap contents_ids; /* stored smart contents id -> loaded id */
    smap legacy_linked; /* linked path -> contents id, for bundles without contents ids */
    /* Decoded tiles by sample type and content hash ("<sample>:<hash>"): a load decodes and
     * charges the byte budget once per distinct tile and shares it copy-on-write (#3315). */
    smap tiles;
} loader;

static pc_status fetch_blob(loader *L, const pc_str *hash, pc_blob **out, pc_error *err)
{
    if (memchr(pc_str_c(hash), '\0', hash->len) != NULL) {
        return pc_format_corrupt(err, "invalid blob hash `%s`", pc_str_c(hash));
    }
    return pc_format_fetch_blob(L->fetch, pc_str_c(hash), out, err);
}

/* Option<Hash> -> Option<blob> (NULL = None). */
static pc_status opt_blob(loader *L, bool has, const pc_str *hash, pc_blob **out, pc_error *err)
{
    *out = NULL;
    return has ? fetch_blob(L, hash, out, err) : PC_OK;
}

/* Both edges of a tile's rectangle must fit in i32 (tile index * PC_TILE_SIZE): a damaged
 * manifest can name any index (#938). */
static bool tile_fits(int32_t i)
{
    int64_t lo = (int64_t)i * PC_TILE_SIZE;
    int64_t hi = ((int64_t)i + 1) * PC_TILE_SIZE;
    return lo >= INT32_MIN && lo <= INT32_MAX && hi >= INT32_MIN && hi <= INT32_MAX && i != INT32_MAX;
}

static pc_status load_surface(loader *L, const pc_mf_surface *m, pc_surface *out, pc_error *err)
{
    pc_pixel_format f = m->format;
    size_t bpp = pc_pixel_format_bytes_per_pixel(f);
    uint8_t dp[PC_RASTER_MAX_PIXEL_BYTES];
    float px[PC_RASTER_MAX_CHANNELS];
    size_t n = 0;
    size_t len;
    pc_surface s;
    pc_buf bytes = PC_BUF_INIT;
    pc_buf key = PC_BUF_INIT;
    smap_ent *cached;
    pc_status st = PC_OK;

    if (!pc_format_unhex(pc_str_c(&m->def), m->def.len, dp, sizeof dp, &n)) {
        return bad_hex(&m->def, err);
    }
    if (n != bpp || bpp > sizeof dp) {
        return pc_format_corrupt(err, "default pixel has wrong size for its format");
    }
    pc_format_swap_to_le(dp, bpp, f.sample);
    memset(px, 0, sizeof px);
    pc_decode_pixel(f, dp, px);
    pc_surface_init_default(&s, f, px, pc_pixel_format_channels(f));
    len = (size_t)PC_TILE_SIZE * PC_TILE_SIZE * bpp;
    for (size_t i = 0; i < m->ntiles && st == PC_OK; i++) {
        const pc_mf_tile *t = &m->tiles[i];
        pc_tile c;
        uint8_t *data = NULL;
        if (!tile_fits(t->tx) || !tile_fits(t->ty)) {
            st = pc_format_corrupt(err, "tile (%ld, %ld) is outside the coordinate range", (long)t->tx,
                                   (long)t->ty);
            break;
        }
        c.tx = t->tx;
        c.ty = t->ty;
        if (pc_surface_tile(&s, c) != NULL) {
            st = pc_format_corrupt(err, "duplicate tile (%ld, %ld)", (long)t->tx, (long)t->ty);
            break;
        }
        if (memchr(pc_str_c(&t->hash), '\0', t->hash.len) != NULL) {
            st = pc_format_corrupt(err, "invalid tile hash `%s`", pc_str_c(&t->hash));
            break;
        }
        key.len = 0;
        st = pc_buf_appendf(&key, err, "%u:%s", (unsigned)f.sample, pc_str_c(&t->hash));
        if (st != PC_OK) {
            break;
        }
        cached = smap_get(&L->tiles, (const char *)key.data, key.len);
        if (cached != NULL) {
            /* One hash names one byte string: a cached tile of another length means the hash
             * was referenced from formats of different pixel sizes. */
            pc_tile_entry e;
            if (pc_raster_tile_len(cached->tile) != len) {
                st = pc_format_corrupt(err, "tile %s is referenced with different sizes", pc_str_c(&t->hash));
                break;
            }
            e.coord = c;
            e.tile = pc_raster_tile_retain(cached->tile);
            st = pc_surface_put_tiles(&s, &e, 1, err);
            continue;
        }
        st = pc_format_fetch_tile(L->fetch, pc_str_c(&t->hash), len, &bytes, err);
        if (st == PC_OK) {
            pc_format_swap_to_le(bytes.data, bytes.len, f.sample);
            st = pc_surface_tile_mut(&s, c, &data, err);
        }
        if (st == PC_OK && bytes.len == len) {
            const pc_raster_tile *tile;
            memcpy(data, bytes.data, len);
            tile = pc_surface_tile(&s, c);
            if (tile != NULL) {
                pc_raster_tile *ref = pc_raster_tile_retain((pc_raster_tile *)(uintptr_t)tile);
                st = smap_put(&L->tiles, (const char *)key.data, key.len, 0, ref, err);
                if (st != PC_OK) {
                    pc_raster_tile_release(ref);
                }
            }
        }
    }
    pc_buf_fini(&key);
    pc_buf_fini(&bytes);
    if (st != PC_OK) {
        pc_surface_fini(&s);
        return st;
    }
    *out = s;
    return PC_OK;
}

static pc_status load_mask(loader *L, const pc_mf_mask *m, pc_layer_mask *out, pc_error *err)
{
    pc_status st = load_surface(L, &m->surface, &out->surface, err);
    if (st == PC_OK) {
        out->enabled = m->enabled;
        out->linked = m->linked;
        out->density = m->density;
        out->feather = m->feather;
    }
    return st;
}

/* EffectsM -> Effects; the items move. */
static pc_status load_effects(loader *L, pc_mf_effects *m, pc_effects *out, pc_error *err)
{
    pc_blob *raw = NULL;
    pc_status st = opt_blob(L, m->has_psd_raw, &m->psd_raw, &raw, err);
    if (st != PC_OK) {
        return st;
    }
    memset(out, 0, sizeof *out);
    out->enabled = m->enabled;
    out->items = m->items;
    out->nitems = m->nitems;
    m->items = NULL;
    m->nitems = 0;
    out->psd_raw = raw;
    out->has_reference = m->has_reference;
    out->reference[0] = m->reference[0];
    out->reference[1] = m->reference[1];
    return PC_OK;
}

static pc_status layer_id(loader *L, uint64_t raw, uint64_t *out, pc_error *err)
{
    uint64_t id = L->preserve_ids ? raw : pc_layer_id_fresh();
    *out = id;
    return idmap_set(&L->ids, raw, id, err);
}

/* Loader::contents_id: a stored id keeps its sharing group (remapped together); bundles without
 * one share only a non-empty linked path (equal embedded blobs may be independent objects). */
static pc_status contents_id(loader *L, const pc_mf_content *m, uint64_t *out, pc_error *err)
{
    smap_ent *e;
    uint64_t id;
    pc_status st;
    if (m->has_contents_id) {
        if (idmap_get(&L->contents_ids, m->contents_id, out)) {
            return PC_OK;
        }
        id = L->preserve_ids ? m->contents_id : pc_smart_contents_id_fresh();
        st = idmap_set(&L->contents_ids, m->contents_id, id, err);
        if (st == PC_OK) {
            *out = id;
        }
        return st;
    }
    if (m->source.kind == PC_MF_LINKED && m->source.path.len > 0) {
        e = smap_get(&L->legacy_linked, pc_str_c(&m->source.path), m->source.path.len);
        if (e != NULL) {
            *out = e->val;
            return PC_OK;
        }
        id = pc_smart_contents_id_fresh();
        st = smap_put(&L->legacy_linked, pc_str_c(&m->source.path), m->source.path.len, id, NULL, err);
        if (st == PC_OK) {
            *out = id;
        }
        return st;
    }
    *out = pc_smart_contents_id_fresh();
    return PC_OK;
}

static void str_move(pc_str *dst, pc_str *src)
{
    *dst = *src;
    src->ptr = NULL;
    src->len = 0;
}

static bool all_finite(const double *v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (!isfinite(v[i])) {
            return false;
        }
    }
    return true;
}

/* The content of one layer whose group children (if any) are already converted. */
static pc_status load_content(loader *L, pc_mf_content *m, pc_layer *kids, size_t nkids,
                              pc_layer_content *out, pc_error *err)
{
    pc_status st = PC_OK;
    memset(out, 0, sizeof *out);
    out->kind = m->kind;
    switch (m->kind) {
    case PC_LAYER_RASTER:
        st = load_surface(L, &m->surface, &out->u.raster, err);
        break;
    case PC_LAYER_GROUP: {
        pc_group *g = &out->u.group;
        g->has_artboard = m->has_artboard;
        g->artboard = m->artboard;
        memset(&m->artboard, 0, sizeof m->artboard);
        m->has_artboard = false;
        g->children = kids;
        g->nchildren = nkids;
        g->expanded = m->expanded;
        return PC_OK;
    }
    case PC_LAYER_ADJUSTMENT:
        out->u.adjustment = m->adjustment;
        memset(&m->adjustment, 0, sizeof m->adjustment);
        break;
    case PC_LAYER_FILL:
        out->u.fill = m->fill;
        memset(&m->fill, 0, sizeof m->fill);
        break;
    case PC_LAYER_TEXT: {
        pc_text_layer *t = &out->u.text;
        if (m->has_cache) {
            st = load_surface(L, &m->cache, &t->cache, err);
            t->has_cache = st == PC_OK;
        }
        if (st == PC_OK) {
            st = opt_blob(L, m->has_psd_raw, &m->psd_raw, &t->psd_raw, err);
        }
        if (st != PC_OK) {
            break;
        }
        str_move(&t->text, &m->text);
        str_move(&t->font_family, &m->font_family);
        t->size_pt = m->size_pt;
        t->color = m->color;
        t->transform = m->transform;
        t->runs = m->runs;
        t->nruns = m->nruns;
        m->runs = NULL;
        m->nruns = 0;
        t->paragraphs = m->paragraphs;
        t->nparagraphs = m->nparagraphs;
        m->paragraphs = NULL;
        m->nparagraphs = 0;
        t->shape = m->shape;
        t->orientation = m->orientation;
        t->antialias = m->antialias;
        t->has_warp = m->has_text_warp;
        t->warp = m->text_warp;
        memset(&m->text_warp, 0, sizeof m->text_warp);
        m->has_text_warp = false;
        break;
    }
    case PC_LAYER_SHAPE: {
        pc_shape_layer *s = &out->u.shape;
        if (m->has_cache) {
            st = load_surface(L, &m->cache, &s->cache, err);
            s->has_cache = st == PC_OK;
        }
        if (st == PC_OK) {
            st = opt_blob(L, m->has_psd_raw, &m->psd_raw, &s->psd_raw, err);
        }
        if (st != PC_OK) {
            break;
        }
        s->path = m->path;
        memset(&m->path, 0, sizeof m->path);
        s->has_fill = m->has_fill;
        s->fill = m->fill;
        memset(&m->fill, 0, sizeof m->fill);
        m->has_fill = false;
        s->has_stroke = m->has_stroke;
        s->stroke = m->stroke;
        memset(&m->stroke, 0, sizeof m->stroke);
        m->has_stroke = false;
        s->has_live = m->has_live;
        s->live = m->live;
        memset(&m->live, 0, sizeof m->live);
        m->has_live = false;
        break;
    }
    case PC_LAYER_SMART: {
        pc_smart_object *s = &out->u.smart;
        st = contents_id(L, m, &s->contents_id, err);
        if (st != PC_OK) {
            break;
        }
        if (m->source.kind == PC_MF_EMBEDDED) {
            st = fetch_blob(L, &m->source.blob, &s->source.bytes, err);
            if (st == PC_OK) {
                str_move(&s->source.file_name, &m->source.file_name);
            }
        } else {
            s->source.linked = true;
            str_move(&s->source.path, &m->source.path);
        }
        if (st == PC_OK && m->has_cache) {
            st = load_surface(L, &m->cache, &s->cache, err);
            s->has_cache = st == PC_OK;
        }
        if (st == PC_OK) {
            st = opt_blob(L, m->has_psd_raw, &m->psd_raw, &s->psd_raw, err);
        }
        if (st == PC_OK && m->has_filter_mask) {
            st = load_mask(L, &m->filter_mask, &s->filter_mask, err);
            s->has_filter_mask = st == PC_OK;
        }
        if (st != PC_OK) {
            break;
        }
        s->transform = m->transform;
        s->smart_filters = m->smart_filters;
        s->nsmart_filters = m->nsmart_filters;
        m->smart_filters = NULL;
        m->nsmart_filters = 0;
        s->filters_enabled = m->filters_enabled;
        /* A custom warp whose mesh doesn't fit together is dropped, not trusted. */
        if (m->has_warp && (!m->warp.has_mesh || pc_mesh_is_valid(&m->warp.mesh))) {
            s->has_warp = true;
            s->warp = m->warp;
            memset(&m->warp, 0, sizeof m->warp);
            m->has_warp = false;
        }
        s->has_stack_mode = m->has_stack_mode;
        s->stack_mode = m->stack_mode;
        if (m->has_perspective && all_finite(m->perspective, 9)) {
            s->has_perspective = true;
            memcpy(s->perspective, m->perspective, sizeof s->perspective);
        }
        break;
    }
    }
    if (st != PC_OK) {
        pc_layer_content_fini(out);
    }
    return st;
}

static pc_status load_layer(loader *L, pc_mf_layer *m, size_t depth, pc_layer *out, pc_error *err);

/* Loader::layer_with: one layer, given its converted group children (taken over). */
static pc_status load_layer_with(loader *L, pc_mf_layer *m, pc_layer *kids, size_t nkids, pc_layer *out,
                                 pc_error *err)
{
    pc_layer l;
    pc_status st;
    memset(&l, 0, sizeof l);
    st = load_content(L, &m->content, kids, nkids, &l.content, err);
    if (st != PC_OK) {
        for (size_t i = 0; i < nkids; i++) {
            pc_layer_fini(&kids[i]);
        }
        free(kids);
        return st;
    }
    if (m->has_mask) {
        st = load_mask(L, &m->mask, &l.mask, err);
        l.has_mask = st == PC_OK;
    }
    if (st == PC_OK && m->has_fill_cache) {
        st = load_surface(L, &m->fill_cache.surface, &l.fill_cache.surface, err);
        if (st == PC_OK) {
            l.has_fill_cache = true;
            l.fill_cache.fill = m->fill_cache.fill;
            memset(&m->fill_cache.fill, 0, sizeof m->fill_cache.fill);
        }
    }
    if (st == PC_OK && m->npsd_blocks > 0) {
        l.psd_blocks = pc_calloc_array(m->npsd_blocks, sizeof *l.psd_blocks);
        if (l.psd_blocks == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        for (size_t i = 0; st == PC_OK && i < m->npsd_blocks; i++) {
            st = key4(&m->psd_blocks[i].key, l.psd_blocks[i].key, err);
            if (st == PC_OK) {
                st = fetch_blob(L, &m->psd_blocks[i].hash, &l.psd_blocks[i].data, err);
            }
            if (st == PC_OK) {
                l.npsd_blocks = i + 1;
            }
        }
    }
    if (st == PC_OK) {
        st = layer_id(L, m->id, &l.id, err);
    }
    if (st == PC_OK) {
        st = load_effects(L, &m->effects, &l.effects, err);
    }
    if (st == PC_OK && m->has_video) {
        pc_mf_video *v = &m->video;
        if (v->nframes > 0) {
            l.video.frames = pc_calloc_array(v->nframes, sizeof *l.video.frames);
            if (l.video.frames == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
            }
        }
        l.has_video = true;
        for (size_t i = 0; st == PC_OK && i < v->nframes; i++) {
            st = load_surface(L, &v->frames[i], &l.video.frames[i], err);
            if (st == PC_OK) {
                l.video.nframes = i + 1;
            }
        }
        if (st == PC_OK && v->has_original_frames) {
            if (v->original_frames.n != v->nframes) {
                st = pc_format_corrupt(err, "video original frame count differs from edited frame count");
            } else if (v->nframes > 0) {
                l.video.original_frames = pc_calloc_array(v->nframes, sizeof *l.video.original_frames);
                if (l.video.original_frames == NULL) {
                    st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
                }
            }
            l.video.has_original_frames = st == PC_OK;
            for (size_t i = 0; st == PC_OK && i < v->original_frames.n; i++) {
                st = load_surface(L, &v->original_frames.v[i], &l.video.original_frames[i], err);
                if (st == PC_OK) {
                    l.video.noriginal_frames = i + 1;
                }
            }
        }
        l.video.source = v->source;
        memset(&v->source, 0, sizeof v->source);
        l.video.fps = v->fps;
        l.video.show_altered = v->show_altered;
    }
    if (st != PC_OK) {
        pc_layer_fini(&l);
        return st;
    }
    str_move(&l.name, &m->name);
    l.visible = m->visible;
    l.locks = m->locks;
    l.blend = m->blend;
    l.opacity = m->opacity;
    l.fill_opacity = m->fill_opacity;
    l.clipped = m->clipped;
    l.has_vector_mask = m->has_vector_mask;
    l.vector_mask = m->vector_mask;
    memset(&m->vector_mask, 0, sizeof m->vector_mask);
    m->has_vector_mask = false;
    l.label = m->label;
    l.has_psd_id = m->has_psd_id;
    l.psd_id = m->psd_id;
    l.has_link_group = m->has_link_group;
    l.link_group = m->link_group;
    l.excluded_channels = m->excluded_channels;
    l.blend_if = m->blend_if;
    memset(&m->blend_if, 0, sizeof m->blend_if);
    l.advanced = m->advanced;
    *out = l;
    return PC_OK;
}

/* Loader::layer: children first (bounded by the group depth limit), then the layer. */
static pc_status load_layer(loader *L, pc_mf_layer *m, size_t depth, pc_layer *out, pc_error *err)
{
    pc_layer *kids = NULL;
    size_t nkids = 0;
    if (depth > PC_MAX_GROUP_DEPTH) {
        return pc_format_limit(err, "layer groups nested deeper than %d", PC_MAX_GROUP_DEPTH);
    }
    if (m->content.kind == PC_LAYER_GROUP && m->content.children.n > 0) {
        pc_mf_layer_list *ch = &m->content.children;
        kids = pc_calloc_array(ch->n, sizeof *kids);
        if (kids == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        for (size_t i = 0; i < ch->n; i++) {
            pc_status st = load_layer(L, &ch->v[i], depth + 1, &kids[nkids], err);
            if (st != PC_OK) {
                while (nkids > 0) {
                    pc_layer_fini(&kids[--nkids]);
                }
                free(kids);
                return st;
            }
            nkids++;
        }
    }
    return load_layer_with(L, m, kids, nkids, out, err);
}

static pc_status load_channel(loader *L, pc_mf_channel *m, pc_alpha_channel *out, pc_error *err)
{
    pc_alpha_channel c;
    pc_status st;
    memset(&c, 0, sizeof c);
    st = load_surface(L, &m->surface, &c.surface, err);
    if (st != PC_OK) {
        return st;
    }
    str_move(&c.name, &m->name);
    c.has_spot = m->has_spot;
    c.spot_color = m->spot.color;
    c.spot_solidity = m->spot.solidity;
    c.color = m->color;
    c.opacity = m->opacity;
    c.indicates = m->indicates;
    *out = c;
    return PC_OK;
}

/* Loader::comp. States of layers deleted before the save keep their (now dangling) id, which
 * layerComp.updateWarnings reports; it must not collide with a live layer. */
static pc_status load_comp(loader *L, pc_mf_comp *m, pc_layer_comp *out, pc_error *err)
{
    pc_layer_comp c;
    pc_status st = PC_OK;
    memset(&c, 0, sizeof c);
    if (m->nstates > 0) {
        c.states = pc_calloc_array(m->nstates, sizeof *c.states);
        if (c.states == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
    }
    for (size_t i = 0; st == PC_OK && i < m->nstates; i++) {
        pc_mf_comp_state *s = &m->states[i];
        pc_comp_layer_state *o = &c.states[i];
        uint64_t id = 0;
        if (s->has_appearance) {
            st = load_effects(L, &s->appearance.effects, &o->appearance.effects, err);
            if (st != PC_OK) {
                break;
            }
            o->has_appearance = true;
            o->appearance.blend = s->appearance.blend;
            o->appearance.opacity = s->appearance.opacity;
            o->appearance.fill_opacity = s->appearance.fill_opacity;
        }
        if (!idmap_get(&L->ids, s->layer, &id)) {
            id = UINT64_MAX - (s->layer < UINT64_MAX / 2 ? s->layer : UINT64_MAX / 2);
        }
        o->layer = id;
        o->has_visible = s->has_visible;
        o->visible = s->visible;
        o->has_position = s->has_position;
        o->position[0] = s->position[0];
        o->position[1] = s->position[1];
        c.nstates = i + 1;
    }
    if (st != PC_OK) {
        pc_layer_comp_fini(&c);
        return st;
    }
    c.id = m->id;
    str_move(&c.name, &m->name);
    str_move(&c.comment, &m->comment);
    c.apply_visibility = m->apply_visibility;
    c.apply_position = m->apply_position;
    c.apply_appearance = m->apply_appearance;
    *out = c;
    return PC_OK;
}

/* Arrays of n zeroed elements (NULL when n is 0). */
static pc_status zalloc(void **p, size_t n, size_t size, pc_error *err)
{
    *p = NULL;
    if (n == 0) {
        return PC_OK;
    }
    *p = pc_calloc_array(n, size);
    return *p != NULL ? PC_OK : pc_fail(err, PC_ERR_NOMEM, "out of memory");
}

/* The largest stored layer and smart contents id (Loader::document's max_ids), bounded by the
 * group depth limit like the conversion itself. */
static pc_status max_ids(const pc_mf_layer_list *layers, size_t depth, uint64_t *max, pc_error *err)
{
    if (depth > PC_MAX_GROUP_DEPTH && layers->n > 0) {
        return pc_format_limit(err, "layer groups nested deeper than %d", PC_MAX_GROUP_DEPTH);
    }
    for (size_t i = 0; i < layers->n; i++) {
        const pc_mf_layer *l = &layers->v[i];
        if (l->id > *max) {
            *max = l->id;
        }
        if (l->content.kind == PC_LAYER_GROUP) {
            pc_status st = max_ids(&l->content.children, depth + 1, max, err);
            if (st != PC_OK) {
                return st;
            }
        } else if (l->content.kind == PC_LAYER_SMART && l->content.has_contents_id &&
                   l->content.contents_id > *max) {
            *max = l->content.contents_id;
        }
    }
    return PC_OK;
}

/* Loader::document, in the reference's order of fetches. */
static pc_status load_document(loader *L, pc_mf_doc *m, pc_document *out, pc_error *err)
{
    pc_document d;
    pc_status st;
    void *p = NULL;
    memset(&d, 0, sizeof d);

    /* Reserve every stored id before minting identities missing from older bundles; an
     * implausibly large one remaps the whole document (shared contents groups stay together). */
    if (L->preserve_ids) {
        uint64_t max = m->id;
        st = max_ids(&m->layers, 0, &max, err);
        if (st != PC_OK) {
            return st;
        }
        L->preserve_ids = pc_format_reserve_ids_through(max);
    }
    st = zalloc(&p, m->layers.n, sizeof *d.layers, err);
    d.layers = p;
    for (size_t i = 0; st == PC_OK && i < m->layers.n; i++) {
        st = load_layer(L, &m->layers.v[i], 0, &d.layers[i], err);
        if (st == PC_OK) {
            d.nlayers = i + 1;
        }
    }
    if (st == PC_OK) {
        st = zalloc(&p, m->nchannels, sizeof *d.channels, err);
        d.channels = p;
    }
    for (size_t i = 0; st == PC_OK && i < m->nchannels; i++) {
        st = load_channel(L, &m->channels[i], &d.channels[i], err);
        if (st == PC_OK) {
            d.nchannels = i + 1;
        }
    }
    if (st == PC_OK && m->has_quick_mask) {
        st = load_channel(L, &m->quick_mask, &d.quick_mask, err);
        d.has_quick_mask = st == PC_OK;
    }
    /* metadata */
    if (st == PC_OK) {
        st = opt_blob(L, m->metadata.has_exif, &m->metadata.exif, &d.metadata.exif, err);
    }
    if (st == PC_OK) {
        d.metadata.text = m->metadata.text;
        d.metadata.ntext = m->metadata.ntext;
        m->metadata.text = NULL;
        m->metadata.ntext = 0;
    }
    if (st == PC_OK) {
        st = zalloc(&p, m->metadata.npsd_resources, sizeof *d.metadata.psd_resources, err);
        d.metadata.psd_resources = p;
    }
    for (size_t i = 0; st == PC_OK && i < m->metadata.npsd_resources; i++) {
        pc_mf_resource *r = &m->metadata.psd_resources[i];
        st = fetch_blob(L, &r->hash, &d.metadata.psd_resources[i].data, err);
        if (st == PC_OK) {
            d.metadata.psd_resources[i].id = r->id;
            str_move(&d.metadata.psd_resources[i].name, &r->name);
            d.metadata.npsd_resources = i + 1;
        }
    }
    if (st == PC_OK) {
        st = zalloc(&p, m->metadata.npsd_global_blocks, sizeof *d.metadata.psd_global_blocks, err);
        d.metadata.psd_global_blocks = p;
    }
    for (size_t i = 0; st == PC_OK && i < m->metadata.npsd_global_blocks; i++) {
        pc_mf_global_block *g = &m->metadata.psd_global_blocks[i];
        pc_doc_psd_global_block *o = &d.metadata.psd_global_blocks[i];
        st = key4(&g->signature, o->signature, err);
        if (st == PC_OK) {
            st = key4(&g->key, o->key, err);
        }
        if (st == PC_OK) {
            st = fetch_blob(L, &g->hash, &o->data, err);
        }
        if (st == PC_OK) {
            d.metadata.npsd_global_blocks = i + 1;
        }
    }
    if (st == PC_OK) {
        d.metadata.has_xmp = m->metadata.has_xmp;
        str_move(&d.metadata.xmp, &m->metadata.xmp);
        st = zalloc(&p, m->npaths, sizeof *d.paths, err);
        d.paths = p;
    }
    for (size_t i = 0; st == PC_OK && i < m->npaths; i++) {
        pc_mf_named_path *np = &m->paths[i];
        st = opt_blob(L, np->has_psd_raw, &np->psd_raw, &d.paths[i].psd_raw, err);
        if (st == PC_OK) {
            str_move(&d.paths[i].name, &np->name);
            d.paths[i].path = np->path;
            memset(&np->path, 0, sizeof np->path);
            d.npaths = i + 1;
        }
    }
    if (st == PC_OK) {
        st = zalloc(&p, m->npatterns, sizeof *d.patterns, err);
        d.patterns = p;
    }
    for (size_t i = 0; st == PC_OK && i < m->npatterns; i++) {
        pc_mf_pattern *pt = &m->patterns[i];
        /* Sizes come from the file: a pattern without tiles can claim any size, and renderers
         * allocate per pixel. */
        if (pt->width == 0 || pt->height == 0) {
            st = pc_format_corrupt(err, "pattern `%s` is %lu\xc3\x97%lu px", pc_str_c(&pt->name),
                                   (unsigned long)pt->width, (unsigned long)pt->height);
            break;
        }
        if (pt->width > PC_PATTERN_MAX_EDGE || pt->height > PC_PATTERN_MAX_EDGE) {
            st = pc_format_limit(err, "pattern `%s` is %lu\xc3\x97%lu px (max %lu per side)",
                                 pc_str_c(&pt->name), (unsigned long)pt->width, (unsigned long)pt->height,
                                 (unsigned long)PC_PATTERN_MAX_EDGE);
            break;
        }
        st = load_surface(L, &pt->surface, &d.patterns[i].surface, err);
        if (st == PC_OK) {
            str_move(&d.patterns[i].id, &pt->id);
            str_move(&d.patterns[i].name, &pt->name);
            d.patterns[i].width = pt->width;
            d.patterns[i].height = pt->height;
            d.npatterns = i + 1;
        }
    }
    if (st == PC_OK) {
        st = zalloc(&p, m->nlayer_comps, sizeof *d.layer_comps, err);
        d.layer_comps = p;
    }
    for (size_t i = 0; st == PC_OK && i < m->nlayer_comps; i++) {
        st = load_comp(L, &m->layer_comps[i], &d.layer_comps[i], err);
        if (st == PC_OK) {
            d.nlayer_comps = i + 1;
        }
    }
    if (st == PC_OK && m->has_last_document_state) {
        st = load_comp(L, &m->last_document_state, &d.last_document_state, err);
        d.has_last_document_state = st == PC_OK;
    }
    if (st == PC_OK) {
        /* Layer-based slices follow their layer through id remapping; a dangling one becomes a
         * user slice (its rect stays where it was). */
        d.slices = m->slices;
        memset(&m->slices, 0, sizeof m->slices);
        for (size_t i = 0; i < d.slices.len; i++) {
            pc_slice *sl = &d.slices.list[i];
            if (sl->has_layer) {
                uint64_t id = 0;
                sl->has_layer = idmap_get(&L->ids, sl->layer, &id);
                sl->layer = sl->has_layer ? id : 0;
                if (!sl->has_layer && sl->origin == PC_SLICE_LAYER) {
                    sl->origin = PC_SLICE_USER;
                }
            }
        }
        d.id = L->preserve_ids ? m->id : pc_doc_id_fresh();
        st = opt_blob(L, m->has_icc_profile, &m->icc_profile, &d.icc_profile, err);
    }
    if (st == PC_OK && m->has_selection) {
        st = load_surface(L, &m->selection, &d.selection, err);
        d.has_selection = st == PC_OK;
    }
    if (st != PC_OK) {
        pc_document_fini(&d);
        return st;
    }
    str_move(&d.name, &m->name);
    d.size = m->size;
    d.resolution_dpi = m->resolution_dpi;
    d.mode = m->mode;
    d.depth = m->depth;
    d.guides = m->guides;
    memset(&m->guides, 0, sizeof m->guides);
    d.global_light = m->global_light;
    d.has_work_path = m->has_work_path;
    d.work_path = m->work_path;
    memset(&m->work_path, 0, sizeof m->work_path);
    m->has_work_path = false;
    d.has_clipping_path = m->has_clipping_path;
    d.clipping_path = m->clipping_path;
    memset(&m->clipping_path, 0, sizeof m->clipping_path);
    m->has_clipping_path = false;
    d.has_color_table = m->has_color_table;
    d.color_table = m->color_table;
    memset(&m->color_table, 0, sizeof m->color_table);
    m->has_color_table = false;
    d.has_duotone = m->has_duotone;
    d.duotone = m->duotone;
    memset(&m->duotone, 0, sizeof m->duotone);
    m->has_duotone = false;
    d.variables = m->variables;
    memset(&m->variables, 0, sizeof m->variables);
    d.has_timeline = m->has_timeline;
    d.timeline = m->timeline;
    d.quick_mask_unselected = m->quick_mask_unselected;
    d.file_encoding = m->file_encoding;
    d.has_last_applied_comp = m->has_last_applied_comp;
    d.last_applied_comp = m->last_applied_comp;
    d.measurement = m->measurement;
    memset(&m->measurement, 0, sizeof m->measurement);
    d.notes = m->notes;
    d.nnotes = m->nnotes;
    m->notes = NULL;
    m->nnotes = 0;
    d.text_styles = m->text_styles;
    memset(&m->text_styles, 0, sizeof m->text_styles);
    *out = d;
    return PC_OK;
}

pc_status pc_format_manifest_to_document(pc_format_manifest *m, pc_format_fetch *fetch, bool preserve_ids,
                                         pc_document *out, pc_error *err)
{
    loader L;
    pc_status st;
    memset(&L, 0, sizeof L);
    L.fetch = fetch;
    L.preserve_ids = preserve_ids;
    st = load_document(&L, &m->document, out, err);
    idmap_fini(&L.ids);
    idmap_fini(&L.contents_ids);
    smap_fini(&L.legacy_linked);
    smap_fini(&L.tiles);
    return st;
}

pc_size pc_format_manifest_canvas(const pc_format_manifest *m)
{
    return m->document.size;
}

bool pc_format_reserve_ids_through(uint64_t max)
{
    uint64_t probe = pc_layer_id_fresh();
    if (max < probe) {
        return true;
    }
    if (max - probe > 10000000u) {
        return false;
    }
    pc_ensure_ids_above(max);
    return true;
}
