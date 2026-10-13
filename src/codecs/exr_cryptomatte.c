/* Cryptomatte (specification 1.2; port of crates/codecs/src/codecs/exr_cryptomatte.rs): per-pixel
 * object/material IDs with coverage in EXR channels, as written by Arnold, V-Ray, Redshift,
 * Mantra/Karma and Cycles. Layers are announced by cryptomatte/<key>/name|hash|conversion|manifest
 * header attributes; the samples live in <name>NN.red (ID) / <name>NN.green (coverage) pairs.
 *
 * IDs are float values: MurmurHash3 x86-32 (seed 0) of the object name with the exponent moved
 * away from 0/255 (no denormal, infinite or NaN ID), read as an f32 bit pattern. IDs compare by
 * value, never by bit pattern. The layer listing reads the headers alone (no OpenEXRCore needed);
 * decoding the samples needs it. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "base/mem.h"
#include "codecs/deep_exr.h"
#include "codecs/internal.h"

#define F PC_CF_EXR
#define MANIFEST_CAP ((size_t)16 << 20)

static uint32_t rotl(uint32_t x, unsigned r)
{
    return (x << r) | (x >> (32 - r));
}

/* MurmurHash3, x86-32 variant, seed 0 (public-domain algorithm, Austin Appleby). */
static uint32_t murmur3_32(const uint8_t *d, size_t n)
{
    const uint32_t c1 = 0xcc9e2d51u;
    const uint32_t c2 = 0x1b873593u;
    uint32_t h = 0;
    size_t i;
    size_t blocks = n / 4;
    for (i = 0; i < blocks; i++) {
        uint32_t k = cx_le32(d + 4 * i);
        k = rotl(k * c1, 15) * c2;
        h ^= k;
        h = rotl(h, 13) * 5 + 0xe6546b64u;
    }
    if (n % 4 != 0) {
        uint8_t t[4] = {0, 0, 0, 0};
        uint32_t k;
        memcpy(t, d + 4 * blocks, n % 4);
        k = cx_le32(t);
        k = rotl(k * c1, 15) * c2;
        h ^= k;
    }
    h ^= (uint32_t)n;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    return h ^ (h >> 16);
}

static float from_bits(uint32_t b)
{
    float f;
    memcpy(&f, &b, 4);
    return f;
}

static uint32_t to_bits(float f)
{
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}

float pc_cryptomatte_id(const char *name, size_t len)
{
    uint32_t bits = murmur3_32((const uint8_t *)name, len);
    uint32_t exp = (bits >> 23) & 0xff;
    if (exp == 0 || exp == 255) {
        bits ^= 1u << 23;
    }
    return from_bits(bits);
}

void pc_cryptomatte_key(const char *name, size_t len, char out[8])
{
    char full[9];
    (void)snprintf(full, sizeof full, "%08x", (unsigned)to_bits(pc_cryptomatte_id(name, len)));
    memcpy(out, full, 7);
    out[7] = '\0';
}

void pc_cryptomatte_preview_color(float id, float out[3])
{
    uint32_t bits = to_bits(id);
    out[0] = 0.0f;
    out[1] = from_bits(bits << 8);
    out[2] = from_bits(bits << 16);
}

/* A stream channel name "<name>NN.red" (or .r: the ID) / ".green" (.g: the coverage): its stream
 * part ("<name>NN") length, the index NN and which of the two it is. */
static bool split_stream(const char *ch, size_t *stream_len, uint32_t *index, bool *coverage)
{
    const char *dot = strrchr(ch, '.');
    size_t sl;
    size_t digits = 0;
    uint64_t v = 0;
    size_t i;
    if (dot == NULL) {
        return false;
    }
    if (strcmp(dot + 1, "red") == 0 || strcmp(dot + 1, "r") == 0) {
        *coverage = false;
    } else if (strcmp(dot + 1, "green") == 0 || strcmp(dot + 1, "g") == 0) {
        *coverage = true;
    } else {
        return false;
    }
    sl = (size_t)(dot - ch);
    while (digits < sl && ch[sl - 1 - digits] >= '0' && ch[sl - 1 - digits] <= '9') {
        digits++;
    }
    if (digits == 0 || digits == sl) {
        return false;
    }
    for (i = sl - digits; i < sl; i++) {
        v = v * 10 + (uint64_t)(ch[i] - '0');
        if (v > UINT32_MAX) {
            return false;
        }
    }
    *stream_len = sl;
    *index = (uint32_t)v;
    return true;
}

/* Lower-case ASCII, without '_' and '-' (renderers spell "CryptoAsset" or "crypto_asset"). */
static size_t normalize(const char *s, size_t n, char *out)
{
    size_t i;
    size_t o = 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c == '_' || c == '-') {
            continue;
        }
        out[o++] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
    }
    return o;
}

typedef struct stream_ref {
    uint32_t index;
    char *name; /* the stream ("crypto_asset00"), owned */
} stream_ref;

static int stream_cmp(const void *pa, const void *pb)
{
    const stream_ref *a = pa;
    const stream_ref *b = pb;
    if (a->index != b->index) {
        return a->index < b->index ? -1 : 1;
    }
    return strcmp(a->name, b->name);
}

/* The stream channel bases of a layer (sorted by index then name, without duplicates), matched
 * leniently against the layer name. *out is new; *n may be 0. */
static pc_status layer_streams(const exr_hdr *h, const char *name, char ***out, size_t *n)
{
    size_t nl = strlen(name);
    char *want = malloc(nl + 1);
    size_t wl;
    stream_ref *v = NULL;
    size_t nv = 0;
    size_t i;
    char **res;
    *out = NULL;
    *n = 0;
    if (want == NULL) {
        return PC_ERR_NOMEM;
    }
    wl = normalize(name, nl, want);
    v = calloc(h->nch > 0 ? h->nch : 1, sizeof *v);
    if (v == NULL) {
        free(want);
        return PC_ERR_NOMEM;
    }
    for (i = 0; i < h->nch; i++) {
        char *u = cx_latin1_to_utf8((const uint8_t *)h->ch[i].name, strlen(h->ch[i].name));
        size_t sl;
        uint32_t idx;
        bool cov;
        size_t base;
        char *norm;
        size_t nn;
        if (u == NULL) {
            goto oom;
        }
        if (!split_stream(u, &sl, &idx, &cov) || cov) {
            free(u);
            continue;
        }
        base = sl;
        while (base > 0 && u[base - 1] >= '0' && u[base - 1] <= '9') {
            base--;
        }
        norm = malloc(base + 1);
        if (norm == NULL) {
            free(u);
            goto oom;
        }
        nn = normalize(u, base, norm);
        if (nn == wl && memcmp(norm, want, wl) == 0) {
            u[sl] = '\0';
            v[nv].index = idx;
            v[nv].name = u;
            nv++;
        } else {
            free(u);
        }
        free(norm);
    }
    free(want);
    want = NULL;
    qsort(v, nv, sizeof *v, stream_cmp);
    res = malloc((nv > 0 ? nv : 1) * sizeof *res);
    if (res == NULL) {
        goto oom;
    }
    for (i = 0; i < nv; i++) {
        if (*n > 0 && v[i].index == v[i - 1].index && strcmp(v[i].name, v[i - 1].name) == 0) {
            free(v[i].name);
            continue;
        }
        res[(*n)++] = v[i].name;
    }
    free(v);
    *out = res;
    return PC_OK;
oom:
    free(want);
    for (i = 0; i < nv; i++) {
        free(v[i].name);
    }
    free(v);
    return PC_ERR_NOMEM;
}

/* The cryptomatte/<key>/<field> string attribute (UTF-8 copy), or NULL; *found says whether it
 * exists (a NULL with *found set is out of memory). */
static char *attribute(const exr_hdr *h, const char *key, const char *field, bool *oom)
{
    char full[512];
    size_t i;
    int k = snprintf(full, sizeof full, "cryptomatte/%s/%s", key, field);
    if (k < 0 || (size_t)k >= sizeof full) {
        return NULL;
    }
    for (i = 0; i < h->nattrs; i++) {
        if (h->attrs[i].name_len == (size_t)k && memcmp(h->attrs[i].name, full, (size_t)k) == 0) {
            char *u = cx_latin1_to_utf8(h->attrs[i].val, h->attrs[i].len);
            if (u == NULL) {
                *oom = true;
            }
            return u;
        }
    }
    return NULL;
}

/* u32::from_str_radix(s, 16): an optional '+', then hex digits, no overflow. */
static bool parse_hex_u32(const char *s, size_t n, uint32_t *out)
{
    size_t i = 0;
    uint64_t v = 0;
    if (n > 0 && s[0] == '+') {
        i = 1;
    }
    if (i == n) {
        return false;
    }
    for (; i < n; i++) {
        char c = s[i];
        unsigned d;
        if (c >= '0' && c <= '9') {
            d = (unsigned)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (unsigned)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = (unsigned)(c - 'A' + 10);
        } else {
            return false;
        }
        v = v * 16 + d;
        if (v > UINT32_MAX) {
            return false;
        }
    }
    *out = (uint32_t)v;
    return true;
}

typedef struct mentry {
    const char *key;
    size_t kl;
    size_t seq;
    yyjson_val *val;
} mentry;

static int mentry_cmp(const void *pa, const void *pb)
{
    const mentry *a = pa;
    const mentry *b = pb;
    size_t k = a->kl < b->kl ? a->kl : b->kl;
    int c = memcmp(a->key, b->key, k);
    if (c != 0) {
        return c;
    }
    if (a->kl != b->kl) {
        return a->kl < b->kl ? -1 : 1;
    }
    return a->seq < b->seq ? -1 : a->seq > b->seq;
}

/* A manifest JSON ({"object name": "<hex ID bits>"}) as (ID, name) pairs sorted by name (a
 * serde_json map), the last duplicate winning. Entries that are not hex strings are skipped; a
 * broken document yields none. */
static pc_status parse_manifest(const char *json, pc_crypto_entry **out, size_t *n)
{
    yyjson_doc *doc = NULL;
    yyjson_val *root;
    yyjson_obj_iter it;
    yyjson_val *k;
    mentry *e;
    size_t ne = 0;
    size_t i;
    pc_crypto_entry *res;
    size_t nr = 0;
    *out = NULL;
    *n = 0;
    if (pc_json_parse(json, strlen(json), &doc, NULL) != PC_OK) {
        return PC_OK;
    }
    root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return PC_OK;
    }
    e = malloc((yyjson_obj_size(root) > 0 ? yyjson_obj_size(root) : 1) * sizeof *e);
    if (e == NULL) {
        yyjson_doc_free(doc);
        return PC_ERR_NOMEM;
    }
    yyjson_obj_iter_init(root, &it);
    while ((k = yyjson_obj_iter_next(&it)) != NULL) {
        e[ne].key = yyjson_get_str(k);
        e[ne].kl = yyjson_get_len(k);
        e[ne].seq = ne;
        e[ne].val = yyjson_obj_iter_get_val(k);
        ne++;
    }
    qsort(e, ne, sizeof *e, mentry_cmp);
    res = calloc(ne > 0 ? ne : 1, sizeof *res);
    if (res == NULL) {
        free(e);
        yyjson_doc_free(doc);
        return PC_ERR_NOMEM;
    }
    for (i = 0; i < ne; i++) {
        const char *hex;
        size_t hl;
        uint32_t bits;
        /* the last of equal keys wins */
        if (i + 1 < ne && e[i + 1].kl == e[i].kl && memcmp(e[i + 1].key, e[i].key, e[i].kl) == 0) {
            continue;
        }
        if (!yyjson_is_str(e[i].val)) {
            continue;
        }
        hex = yyjson_get_str(e[i].val);
        hl = yyjson_get_len(e[i].val);
        while (hl >= 2 && hex[0] == '0' && hex[1] == 'x') {
            hex += 2;
            hl -= 2;
        }
        if (!parse_hex_u32(hex, hl, &bits)) {
            continue;
        }
        res[nr].name = malloc(e[i].kl + 1);
        if (res[nr].name == NULL) {
            size_t j;
            for (j = 0; j < nr; j++) {
                free(res[j].name);
            }
            free(res);
            free(e);
            yyjson_doc_free(doc);
            return PC_ERR_NOMEM;
        }
        memcpy(res[nr].name, e[i].key, e[i].kl);
        res[nr].name[e[i].kl] = '\0';
        res[nr].id = from_bits(bits);
        nr++;
    }
    free(e);
    yyjson_doc_free(doc);
    *out = res;
    *n = nr;
    return PC_OK;
}

typedef struct sortable {
    pc_crypto_layer layer;
    size_t seq;
} sortable;

static int layer_cmp(const void *pa, const void *pb)
{
    const sortable *a = pa;
    const sortable *b = pb;
    int c;
    if (a->layer.part != b->layer.part) {
        return a->layer.part < b->layer.part ? -1 : 1;
    }
    c = strcmp(a->layer.name, b->layer.name);
    if (c != 0) {
        return c;
    }
    return a->seq < b->seq ? -1 : a->seq > b->seq;
}

/* Every distinct cryptomatte/<key>/ key of a header, in file order (key into keys[i]). */
static size_t header_keys(const exr_hdr *h, char (*keys)[256], size_t cap)
{
    size_t i;
    size_t n = 0;
    for (i = 0; i < h->nattrs && n < cap; i++) {
        const char *rest = h->attrs[i].name + 12;
        size_t rl = h->attrs[i].name_len - 12;
        const char *slash = memchr(rest, '/', rl);
        size_t kl;
        size_t j;
        bool dup = false;
        if (slash == NULL || slash == rest || (size_t)(slash - rest) >= 256) {
            continue;
        }
        kl = (size_t)(slash - rest);
        for (j = 0; j < n; j++) {
            dup = dup || (strlen(keys[j]) == kl && memcmp(keys[j], rest, kl) == 0);
        }
        if (!dup) {
            memcpy(keys[n], rest, kl);
            keys[n][kl] = '\0';
            n++;
        }
    }
    return n;
}

static pc_status list_layers(const exr_meta *m, const pc_climits *l, pc_crypto_layer **layers,
                             size_t *nlayers, pc_error *err)
{
    sortable *v = NULL;
    size_t nv = 0;
    size_t cap = 0;
    size_t part;
    size_t i;
    pc_status st = PC_OK;
    char (*keys)[256] = NULL;
    for (part = 0; part < m->n && st == PC_OK; part++) {
        const exr_hdr *h = &m->parts[part];
        uint32_t w = h->w > UINT32_MAX ? UINT32_MAX : (uint32_t)h->w;
        uint32_t hh = h->h > UINT32_MAX ? UINT32_MAX : (uint32_t)h->h;
        size_t nk;
        size_t k;
        if ((st = cx_check_bytes(l, w, hh, 8, err)) != PC_OK) {
            break;
        }
        free(keys);
        keys = malloc((h->nattrs > 0 ? h->nattrs : 1) * sizeof *keys);
        if (keys == NULL) {
            st = cx_nomem(err);
            break;
        }
        nk = header_keys(h, keys, h->nattrs);
        for (k = 0; k < nk && st == PC_OK; k++) {
            bool oom = false;
            pc_crypto_layer lay;
            char *manifest;
            sortable *g;
            memset(&lay, 0, sizeof lay);
            lay.part = part;
            lay.name = attribute(h, keys[k], "name", &oom);
            if (lay.name == NULL) {
                if (oom) {
                    st = cx_nomem(err);
                }
                continue;
            }
            if (layer_streams(h, lay.name, &lay.channels, &lay.nchannels) != PC_OK) {
                pc_crypto_layer_fini(&lay);
                st = cx_nomem(err);
                break;
            }
            if (lay.nchannels == 0) {
                pc_crypto_layer_fini(&lay);
                continue;
            }
            lay.key = malloc(strlen(keys[k]) + 1);
            manifest = attribute(h, keys[k], "manifest", &oom);
            lay.conversion = attribute(h, keys[k], "conversion", &oom);
            lay.sidecar_manifest = attribute(h, keys[k], "manif_file", &oom);
            if (lay.key == NULL || oom) {
                free(manifest);
                pc_crypto_layer_fini(&lay);
                st = cx_nomem(err);
                break;
            }
            strcpy(lay.key, keys[k]);
            /* Manifests can be large: bound the embedded text before parsing it. */
            if (manifest != NULL && strlen(manifest) <= MANIFEST_CAP &&
                parse_manifest(manifest, &lay.manifest, &lay.nmanifest) != PC_OK) {
                free(manifest);
                pc_crypto_layer_fini(&lay);
                st = cx_nomem(err);
                break;
            }
            free(manifest);
            g = pc_grow_array(v, &cap, nv + 1, sizeof *v);
            if (g == NULL) {
                pc_crypto_layer_fini(&lay);
                st = cx_nomem(err);
                break;
            }
            v = g;
            v[nv].layer = lay;
            v[nv].seq = nv;
            nv++;
        }
    }
    free(keys);
    if (st != PC_OK) {
        for (i = 0; i < nv; i++) {
            pc_crypto_layer_fini(&v[i].layer);
        }
        free(v);
        return st;
    }
    if (nv > 1) {
        qsort(v, nv, sizeof *v, layer_cmp);
    }
    *layers = calloc(nv > 0 ? nv : 1, sizeof **layers);
    if (*layers == NULL) {
        for (i = 0; i < nv; i++) {
            pc_crypto_layer_fini(&v[i].layer);
        }
        free(v);
        return cx_nomem(err);
    }
    for (i = 0; i < nv; i++) {
        (*layers)[i] = v[i].layer;
    }
    *nlayers = nv;
    free(v);
    return PC_OK;
}

pc_status pc_ccryptomatte_layers(const uint8_t *b, size_t n, const pc_climits *l, pc_crypto_layer **layers,
                                 size_t *nlayers, pc_error *err)
{
    exr_meta m;
    pc_status st = cx_exr_meta_parse(b, n, &m, err);
    *layers = NULL;
    *nlayers = 0;
    if (st != PC_OK) {
        return st;
    }
    st = list_layers(&m, l, layers, nlayers, err);
    cx_exr_meta_fini(&m);
    return st;
}

static bool ieq(const char *a, const char *b)
{
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a - 'A' + 'a') : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b - 'A' + 'a') : *b;
        if (x != y) {
            return false;
        }
    }
    return *a == *b;
}

pc_status pc_cdecode_cryptomatte(const uint8_t *b, size_t n, const char *layer_name, const pc_climits *l,
                                 pc_crypto_buffer *out, pc_error *err)
{
    exr_meta m;
    pc_crypto_layer *found = NULL;
    size_t nfound = 0;
    const pc_crypto_layer *lay = NULL;
    uint32_t w;
    uint32_t h;
    uint64_t per_pixel;
    size_t npx;
    float **chans = NULL;
    char **names = NULL;
    size_t nch = 0;
    const float **ids = NULL;
    const float **covs = NULL;
    pc_crypto_buffer buf;
    size_t cap = 0;
    size_t i;
    size_t p;
    pc_status st = cx_exr_meta_parse(b, n, &m, err);
    memset(&buf, 0, sizeof buf);
    if (st != PC_OK) {
        return st;
    }
    if ((st = list_layers(&m, l, &found, &nfound, err)) != PC_OK) {
        cx_exr_meta_fini(&m);
        return st;
    }
    for (i = 0; i < nfound && lay == NULL; i++) {
        if (ieq(found[i].name, layer_name)) {
            lay = &found[i];
        }
    }
    if (lay == NULL) {
        st = cx_unsupported(err, F, "no Cryptomatte layer named %s", layer_name);
        goto done;
    }
    if (m.has_deep) {
        st = cx_unsupported(err, F, "this file has deep parts; Cryptomatte decoding stays with flat files");
        goto done;
    }
    if (m.parts[lay->part].w > UINT32_MAX || m.parts[lay->part].h > UINT32_MAX) {
        st = cx_malformed(err, F, "the layer is too large");
        goto done;
    }
    w = (uint32_t)m.parts[lay->part].w;
    h = (uint32_t)m.parts[lay->part].h;
    /* Each pixel also gets a pair list, so count its header with the channel samples. */
    per_pixel = (uint64_t)(lay->nchannels > 0 ? lay->nchannels : 1) * 8 + 3 * sizeof(size_t);
    if ((st = cx_check_bytes(l, w, h, per_pixel, err)) != PC_OK) {
        goto done;
    }
    npx = (size_t)w * h;
    {
        uint32_t rw;
        uint32_t rh;
        if ((st = cx_exr_read_part_f32(b, n, lay->part, &rw, &rh, &chans, &names, &nch, err)) != PC_OK) {
            goto done;
        }
        if (rw != w || rh != h) {
            st = cx_malformed(err, F, "a Cryptomatte channel is smaller than its layer");
            goto done;
        }
    }
    ids = calloc(lay->nchannels, sizeof *ids);
    covs = calloc(lay->nchannels, sizeof *covs);
    buf.start = calloc(npx + 1, sizeof *buf.start);
    if (ids == NULL || covs == NULL || buf.start == NULL) {
        st = cx_nomem(err);
        goto done;
    }
    for (i = 0; i < nch; i++) {
        size_t sl;
        uint32_t idx;
        bool cov;
        size_t slot;
        if (!split_stream(names[i], &sl, &idx, &cov)) {
            continue;
        }
        for (slot = 0; slot < lay->nchannels; slot++) {
            if (strlen(lay->channels[slot]) == sl && memcmp(lay->channels[slot], names[i], sl) == 0) {
                break;
            }
        }
        if (slot == lay->nchannels) {
            continue;
        }
        if (chans[i] == NULL) {
            st = cx_malformed(err, F, "a Cryptomatte channel is smaller than its layer");
            goto done;
        }
        if (cov) {
            covs[slot] = chans[i];
        } else {
            ids[slot] = chans[i];
        }
    }
    buf.width = w;
    buf.height = h;
    for (p = 0; p < npx; p++) {
        size_t s;
        size_t first = buf.npairs;
        buf.start[p] = first;
        for (s = 0; s < lay->nchannels; s++) {
            float id = ids[s] != NULL ? ids[s][p] : 0.0f;
            float c = covs[s] != NULL ? covs[s][p] : 0.0f;
            if (c > 0.0f && isfinite(id)) {
                size_t k;
                if (buf.npairs == cap) {
                    pc_crypto_pair *g = pc_grow_array(buf.pairs, &cap, buf.npairs + 1, sizeof *g);
                    if (g == NULL) {
                        st = cx_nomem(err);
                        goto done;
                    }
                    buf.pairs = g;
                }
                /* insertion keeps the sort stable: descending coverage */
                k = buf.npairs;
                while (k > first && buf.pairs[k - 1].coverage < c) {
                    buf.pairs[k] = buf.pairs[k - 1];
                    k--;
                }
                buf.pairs[k].id = id;
                buf.pairs[k].coverage = c;
                buf.npairs++;
            }
        }
    }
    buf.start[npx] = buf.npairs;
    *out = buf;
    memset(&buf, 0, sizeof buf);
done:
    pc_crypto_buffer_fini(&buf);
    for (i = 0; i < nch; i++) {
        free(chans[i]);
        free(names[i]);
    }
    free(chans);
    free(names);
    free(ids);
    free(covs);
    pc_crypto_layers_free(found, nfound);
    cx_exr_meta_fini(&m);
    return st;
}
