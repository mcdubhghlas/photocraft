/* Deep OpenEXR (port of crates/codecs/src/codecs/deep_exr.rs; REWRITE_PLAN.md D27): the chunk
 * stream is walked directly from the bytes per the OpenEXR file layout: a deep chunk carries its
 * coordinates, the packed size of the sample-count table, the packed and unpacked sizes of the
 * sample data, the compressed count table (cumulative within each row of the block) and the
 * compressed sample data (row by row, each row channel by channel). NONE, RLE and ZIPS are read
 * (one scan line per block); ZIP is declined as unsupported.
 *
 * cx_exr_flatten composites the samples into a flat image: per pixel, samples sorted by Z
 * (stable; NaN last), front-to-back `over` with premultiplied colour, un-premultiplied into
 * straight alpha (HDR kept). */
#include "codecs/deep_exr.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/compress.h"
#include "base/half.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "codecs/internal.h"

#define F PC_CF_EXR

/* ---- header parsing ---- */

typedef struct rd {
    const uint8_t *b;
    size_t n;
    size_t pos;
} rd;

static bool rd_cstr(rd *r, size_t max, const char **s, size_t *len)
{
    size_t i = r->pos;
    while (i < r->n && r->b[i] != 0) {
        if (i - r->pos >= max) {
            return false;
        }
        i++;
    }
    if (i >= r->n) {
        return false;
    }
    *s = (const char *)r->b + r->pos;
    *len = i - r->pos;
    r->pos = i + 1;
    return true;
}

static void free_channels(exr_hdr *h)
{
    size_t i;
    for (i = 0; i < h->nch; i++) {
        free(h->ch[i].name);
    }
    free(h->ch);
    h->ch = NULL;
    h->nch = 0;
}

static void hdr_fini(exr_hdr *h)
{
    size_t i;
    free_channels(h);
    free(h->name);
    free(h->view);
    for (i = 0; i < h->nattrs; i++) {
        free(h->attrs[i].name);
        free(h->attrs[i].val);
    }
    free(h->attrs);
    memset(h, 0, sizeof *h);
}

static uint8_t *dup_bytes(const void *p, size_t n)
{
    uint8_t *d = malloc(n + 1);
    if (d != NULL) {
        memcpy(d, p, n);
        d[n] = 0;
    }
    return d;
}

/* Keeps a string attribute (replacing an earlier one of the same name). false when out of memory. */
static bool keep_attr(exr_hdr *h, const char *name, size_t nl, const uint8_t *v, size_t vl)
{
    size_t i;
    uint8_t *val = dup_bytes(v, vl);
    exr_sattr *g;
    if (val == NULL) {
        return false;
    }
    for (i = 0; i < h->nattrs; i++) {
        if (h->attrs[i].name_len == nl && memcmp(h->attrs[i].name, name, nl) == 0) {
            free(h->attrs[i].val);
            h->attrs[i].val = val;
            h->attrs[i].len = vl;
            return true;
        }
    }
    g = realloc(h->attrs, (h->nattrs + 1) * sizeof *g);
    if (g == NULL) {
        free(val);
        return false;
    }
    h->attrs = g;
    g[h->nattrs].name = (char *)dup_bytes(name, nl);
    if (g[h->nattrs].name == NULL) {
        free(val);
        return false;
    }
    g[h->nattrs].name_len = nl;
    g[h->nattrs].val = val;
    g[h->nattrs].len = vl;
    h->nattrs++;
    return true;
}

void cx_exr_meta_fini(exr_meta *m)
{
    size_t i;
    if (m == NULL) {
        return;
    }
    for (i = 0; i < m->n; i++) {
        hdr_fini(&m->parts[i]);
    }
    free(m->parts);
    memset(m, 0, sizeof *m);
}

static int32_t i32le(const uint8_t *p) { return (int32_t)cx_le32(p); }

/* chlist: (name\0, i32 type, u8 pLinear, 3 reserved, i32 xs, i32 ys)* \0. false when malformed. */
static bool parse_chlist(const uint8_t *v, size_t vn, size_t max, exr_hdr *h)
{
    rd r = {v, vn, 0};
    size_t cap = 0;
    for (;;) {
        const char *name;
        size_t len;
        exr_chan c;
        exr_chan *g;
        if (r.pos >= r.n) {
            return false;
        }
        if (r.b[r.pos] == 0) {
            return true;
        }
        if (!rd_cstr(&r, max, &name, &len) || r.n - r.pos < 16) {
            return false;
        }
        c.type = i32le(r.b + r.pos);
        c.xs = i32le(r.b + r.pos + 8);
        c.ys = i32le(r.b + r.pos + 12);
        r.pos += 16;
        if (c.type < 0 || c.type > 2) {
            return false;
        }
        c.name = malloc(len + 1);
        g = c.name != NULL ? pc_grow_array(h->ch, &cap, h->nch + 1, sizeof *g) : NULL;
        if (g == NULL) {
            free(c.name);
            return false;
        }
        memcpy(c.name, name, len);
        c.name[len] = '\0';
        h->ch = g;
        h->ch[h->nch++] = c;
    }
}

static uint64_t blocks(uint64_t size, uint64_t block) { return block == 0 ? 0 : (size + block - 1) / block; }

static size_t lines_per_block(int compression)
{
    switch (compression) {
    case 0:
    case 1:
    case 2:
        return 1;
    case 3:
    case 5:
        return 16;
    case 4:
    case 6:
    case 7:
    case 8:
        return 32;
    default:
        return 256;
    }
}

/* One header up to its terminating NUL. */
static pc_status read_header(rd *r, bool long_names, bool single_tiled, exr_hdr *h, pc_error *err)
{
    size_t max = long_names ? 255 : 31;
    bool has_comp = false;
    bool has_dw = false;
    bool has_disp = false;
    bool has_ch = false;
    bool has_tiles = false;
    int block_type = -1; /* 0 scanline, 1 tiled, 2 deepscanline, 3 deeptile */
    memset(h, 0, sizeof *h);
    for (;;) {
        const char *name;
        const char *type;
        size_t nl;
        size_t tl;
        int32_t size;
        const uint8_t *v;
        if (r->pos >= r->n) {
            hdr_fini(h);
            return cx_malformed(err, F, "reference to missing bytes");
        }
        if (r->b[r->pos] == 0) {
            r->pos++;
            break;
        }
        if (!rd_cstr(r, max, &name, &nl) || !rd_cstr(r, max, &type, &tl) || r->n - r->pos < 4) {
            hdr_fini(h);
            return cx_malformed(err, F, "invalid attribute name or type");
        }
        size = i32le(r->b + r->pos);
        r->pos += 4;
        if (size < 0 || (size_t)size > r->n - r->pos) {
            hdr_fini(h);
            return cx_malformed(err, F, "attribute size exceeds the file");
        }
        v = r->b + r->pos;
        r->pos += (size_t)size;
#define IS(s, l, lit) ((l) == sizeof(lit) - 1 && memcmp((s), (lit), sizeof(lit) - 1) == 0)
        if (IS(name, nl, "channels") && IS(type, tl, "chlist")) {
            exr_hdr t;
            memset(&t, 0, sizeof t);
            if (parse_chlist(v, (size_t)size, max, &t)) {
                free_channels(h); /* keep the newest */
                h->ch = t.ch;
                h->nch = t.nch;
                has_ch = true;
            } else {
                hdr_fini(&t);
            }
        } else if ((IS(name, nl, "name") || IS(name, nl, "view")) && IS(type, tl, "string")) {
            uint8_t **dst = IS(name, nl, "name") ? &h->name : &h->view;
            uint8_t *d = dup_bytes(v, (size_t)size);
            if (d == NULL) {
                hdr_fini(h);
                return cx_nomem(err);
            }
            free(*dst);
            *dst = d;
            if (dst == &h->name) {
                h->name_len = (size_t)size;
            } else {
                h->view_len = (size_t)size;
            }
        } else if (nl > 12 && memcmp(name, "cryptomatte/", 12) == 0 && IS(type, tl, "string")) {
            if (!keep_attr(h, name, nl, v, (size_t)size)) {
                hdr_fini(h);
                return cx_nomem(err);
            }
        } else if (IS(name, nl, "compression") && IS(type, tl, "compression") && size == 1 && v[0] <= 11) {
            h->compression = v[0];
            has_comp = true;
        } else if (IS(name, nl, "dataWindow") && IS(type, tl, "box2i") && size == 16) {
            int i;
            for (i = 0; i < 4; i++) {
                h->dw[i] = i32le(v + 4 * i);
            }
            has_dw = true;
        } else if (IS(name, nl, "displayWindow") && IS(type, tl, "box2i") && size == 16) {
            has_disp = true;
        } else if (IS(name, nl, "tiles") && IS(type, tl, "tiledesc") && size == 9) {
            h->tw = cx_le32(v);
            h->th = cx_le32(v + 4);
            h->level_mode = v[8] & 0x0F;
            has_tiles = true;
        } else if (IS(name, nl, "type") && IS(type, tl, "string")) {
            if (IS((const char *)v, (size_t)size, "scanlineimage")) {
                block_type = 0;
            } else if (IS((const char *)v, (size_t)size, "tiledimage")) {
                block_type = 1;
            } else if (IS((const char *)v, (size_t)size, "deepscanline")) {
                block_type = 2;
            } else if (IS((const char *)v, (size_t)size, "deeptile")) {
                block_type = 3;
            } else {
                hdr_fini(h);
                return cx_malformed(err, F, "invalid block type attribute");
            }
        }
#undef IS
    }
    if (!has_comp) {
        hdr_fini(h);
        return cx_malformed(err, F, "missing or invalid compression attribute");
    }
    if (!has_disp) {
        hdr_fini(h);
        return cx_malformed(err, F, "missing or invalid display window attribute");
    }
    if (!has_dw) {
        hdr_fini(h);
        return cx_malformed(err, F, "missing or invalid data window attribute");
    }
    if ((int64_t)h->dw[2] < (int64_t)h->dw[0] - 1 || (int64_t)h->dw[3] < (int64_t)h->dw[1] - 1) {
        hdr_fini(h);
        return cx_malformed(err, F, "invalid data window size");
    }
    h->w = (uint64_t)((int64_t)h->dw[2] - h->dw[0] + 1);
    h->h = (uint64_t)((int64_t)h->dw[3] - h->dw[1] + 1);
    if (has_tiles && (h->tw == 0 || h->th == 0 || h->level_mode > 2)) {
        hdr_fini(h);
        return cx_malformed(err, F, "invalid tile description");
    }
    if ((block_type < 0 && single_tiled) || block_type == 1 || block_type == 3) {
        if (!has_tiles) {
            hdr_fini(h);
            return cx_malformed(err, F, "missing or invalid tiles attribute");
        }
        h->tiled = true;
        h->chunk_count = blocks(h->w, h->tw) * blocks(h->h, h->th);
    } else {
        h->chunk_count = blocks(h->h, lines_per_block(h->compression));
    }
    if (!has_ch) {
        hdr_fini(h);
        return cx_malformed(err, F, "missing or invalid channels attribute");
    }
    h->deep = block_type == 2 || block_type == 3;
    return PC_OK;
}

pc_status cx_exr_meta_parse(const uint8_t *b, size_t n, exr_meta *out, pc_error *err)
{
    rd r = {b, n, 8};
    uint32_t version;
    bool tiled;
    bool long_names;
    exr_meta m;
    size_t cap = 0;
    pc_status st;
    if (n < 8 || memcmp(b, "\x76\x2F\x31\x01", 4) != 0) {
        return cx_malformed(err, F, "file identifier missing");
    }
    version = cx_le32(b + 4);
    if ((version & 0xFF) != 2) {
        return cx_unsupported(err, F, "not supported: file versions other than 2");
    }
    if ((version & ~(uint32_t)(0xFF | 0x200 | 0x400 | 0x800 | 0x1000)) != 0) {
        return cx_unsupported(err, F, "not supported: unknown file feature flags");
    }
    memset(&m, 0, sizeof m);
    tiled = (version & 0x200) != 0;
    long_names = (version & 0x400) != 0;
    m.has_deep = (version & 0x800) != 0;
    m.multipart = (version & 0x1000) != 0;
    do {
        exr_hdr h;
        exr_hdr *g;
        if (m.multipart && r.pos < n && b[r.pos] == 0) {
            r.pos++;
            break;
        }
        if ((st = read_header(&r, long_names, tiled && !m.multipart, &h, err)) != PC_OK) {
            cx_exr_meta_fini(&m);
            return st;
        }
        g = pc_grow_array(m.parts, &cap, m.n + 1, sizeof *g);
        if (g == NULL) {
            hdr_fini(&h);
            cx_exr_meta_fini(&m);
            return cx_nomem(err);
        }
        m.parts = g;
        m.parts[m.n++] = h;
        if (m.n > 1024) {
            cx_exr_meta_fini(&m);
            return cx_malformed(err, F, "too many parts");
        }
    } while (m.multipart);
    m.header_end = r.pos;
    *out = m;
    return PC_OK;
}

/* ---- decompression ---- */

static void differences_to_samples(uint8_t *b, size_t n)
{
    size_t i;
    if (n == 0) {
        return;
    }
    for (i = 1; i < n; i++) {
        b[i] = (uint8_t)(b[i - 1] + b[i] - 128);
    }
}

static pc_status interleave(uint8_t *b, size_t n, pc_error *err)
{
    size_t half = (n + 1) / 2;
    uint8_t *o = malloc(n > 0 ? n : 1);
    size_t i;
    size_t k = 0;
    if (o == NULL) {
        return cx_nomem(err);
    }
    for (i = 0; i < n / 2; i++) {
        o[k++] = b[i];
        o[k++] = b[half + i];
    }
    if (n % 2 == 1 && half > 0) {
        o[k] = b[half - 1];
    }
    memcpy(b, o, n);
    free(o);
    return PC_OK;
}

static pc_status rle_decode(const uint8_t *src, size_t n, size_t expected, const char *what, uint8_t *out,
                            pc_error *err)
{
    size_t i = 0;
    size_t o = 0;
    while (i < n) {
        int8_t count = (int8_t)src[i++];
        size_t k = count < 0 ? (size_t)(-(int)count) : (size_t)count + 1;
        if (k > expected - o) {
            return cx_malformed(err, F, "%s: RLE data expands past %zu bytes", what, expected);
        }
        if (count < 0) {
            if (n - i < k) {
                return cx_malformed(err, F, "%s: RLE run points past the compressed data", what);
            }
            memcpy(out + o, src + i, k);
            i += k;
        } else {
            if (i >= n) {
                return cx_malformed(err, F, "%s: RLE run points past the compressed data", what);
            }
            memset(out + o, src[i], k);
            i++;
        }
        o += k;
    }
    if (o != expected) {
        return cx_malformed(err, F, "%s: RLE data expands to %zu bytes, expected %zu", what, o, expected);
    }
    return PC_OK;
}

static const char *comp_name(int c)
{
    static const char *const N[] = {"Uncompressed", "RLE",  "ZIP1", "ZIP16", "PIZ",     "PXR24",
                                    "B44",          "B44A", "DWAA", "DWAB",  "HTJ2K256", "HTJ2K32"};
    return c >= 0 && c < 12 ? N[c] : "?";
}

/* Decompresses to exactly expected bytes into a new buffer. */
static pc_status decompress(int comp, const uint8_t *packed, size_t n, size_t expected, const char *what,
                            uint8_t **out, pc_error *err)
{
    uint8_t *o = malloc(expected > 0 ? expected : 1);
    pc_status st = PC_OK;
    if (o == NULL) {
        return cx_nomem(err);
    }
    if (n == expected) {
        if (n > 0) {
            memcpy(o, packed, n);
        }
        *out = o;
        return PC_OK;
    }
    if (comp == 0) {
        st = cx_malformed(err, F, "%s: expected %zu uncompressed bytes, found %zu", what, expected, n);
    } else if (comp == 1) {
        st = rle_decode(packed, n, expected, what, o, err);
        if (st == PC_OK) {
            differences_to_samples(o, expected);
            st = interleave(o, expected, err);
        }
    } else if (comp == 2) {
        pc_buf z = PC_BUF_INIT;
        if (pc_inflate(PC_Z_ZLIB, packed, n, expected + 1, &z, NULL, NULL) != PC_OK) {
            st = cx_malformed(err, F, "%s: zlib data is malformed", what);
        } else if (z.len != expected) {
            st = cx_malformed(err, F, "%s: zlib data expands to %zu bytes, expected %zu", what, z.len, expected);
        } else {
            if (expected > 0) {
                memcpy(o, z.data, expected);
            }
            differences_to_samples(o, expected);
            st = interleave(o, expected, err);
        }
        pc_buf_fini(&z);
    } else {
        st = cx_unsupported(err, F, "deep data with %s compression (NONE, RLE and ZIPS are supported)",
                            comp_name(comp));
    }
    if (st != PC_OK) {
        free(o);
        return st;
    }
    *out = o;
    return PC_OK;
}

/* ---- the deep chunk stream ---- */

typedef struct dchunk {
    size_t *pixels; /* global pixel indices, block order */
    size_t npixels;
    size_t row_w;
    uint32_t *prefix; /* npixels + 1 */
    size_t pos;
    uint64_t packed_data;
    uint64_t unpacked_data;
} dchunk;

static size_t sample_bytes(int t) { return t == 1 ? 2 : 4; }

static bool rd_i32(const uint8_t *b, size_t n, size_t *pos, int32_t *v)
{
    if (n - *pos < 4 || *pos > n) {
        return false;
    }
    *v = i32le(b + *pos);
    *pos += 4;
    return true;
}

static bool rd_u64(const uint8_t *b, size_t n, size_t *pos, uint64_t *v)
{
    if (*pos > n || n - *pos < 8) {
        return false;
    }
    *v = (uint64_t)cx_le32(b + *pos) | ((uint64_t)cx_le32(b + *pos + 4) << 32);
    *pos += 8;
    return true;
}

static void chunks_free(dchunk *c, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        free(c[i].pixels);
        free(c[i].prefix);
    }
    free(c);
}

pc_status cx_exr_decode_deep(const exr_meta *m, const uint8_t *b, size_t n, const pc_climits *l, pc_cdeep *out,
                             pc_error *err)
{
    size_t hi;
    const exr_hdr *h = NULL;
    size_t w;
    size_t hh;
    size_t npixels;
    size_t bps_total = 0;
    size_t table_pos;
    size_t pos;
    uint64_t *offsets = NULL;
    size_t noff;
    size_t tables_end;
    size_t tiles_x = 0;
    size_t tile_w;
    size_t tile_h;
    size_t slots;
    uint32_t *counts = NULL;
    bool *seen = NULL;
    dchunk *chunks = NULL;
    size_t nchunks = 0;
    pc_cdeep d;
    size_t i;
    pc_status st = PC_OK;
    memset(&d, 0, sizeof d);
    for (hi = 0; hi < m->n; hi++) {
        if (m->parts[hi].deep) {
            h = &m->parts[hi];
            break;
        }
    }
    if (h == NULL) {
        return cx_unsupported(err, F, "the file has no deep part");
    }
    if (!(h->compression == 0 || h->compression == 1 || h->compression == 2)) {
        return cx_unsupported(err, F, "deep data with %s compression (NONE, RLE and ZIPS are supported)",
                              comp_name(h->compression));
    }
    if (h->w == 0 || h->h == 0) {
        return cx_malformed(err, F, "the data window is empty");
    }
    if (h->w > UINT32_MAX || h->h > UINT32_MAX) {
        return cx_malformed(err, F, "the data window is too large");
    }
    w = (size_t)h->w;
    hh = (size_t)h->h;
    if (!pc_size_mul(w, hh, &npixels)) {
        return cx_malformed(err, F, "the data window is too large");
    }
    if ((st = cx_check_bytes(l, (uint32_t)w, (uint32_t)hh, (uint64_t)(h->nch > 0 ? h->nch : 1) * 4, err)) != PC_OK) {
        return st;
    }
    for (i = 0; i < h->nch; i++) {
        bps_total += sample_bytes(h->ch[i].type);
    }
    /* The offset tables follow the headers, one per part in order. */
    table_pos = m->header_end;
    for (i = 0; i < hi; i++) {
        uint64_t cc = m->parts[i].chunk_count < (1u << 24) ? m->parts[i].chunk_count : (1u << 24);
        table_pos = cc * 8 > SIZE_MAX - table_pos ? SIZE_MAX : table_pos + (size_t)cc * 8;
    }
    if (table_pos > n) {
        return cx_malformed(err, F, "chunk offset %zu points past the end of the file", table_pos);
    }
    if (h->chunk_count > (n - table_pos) / 8) {
        return cx_malformed(err, F, "truncated file while reading a u64");
    }
    noff = (size_t)h->chunk_count;
    offsets = malloc((noff > 0 ? noff : 1) * sizeof *offsets);
    if (offsets == NULL) {
        return cx_nomem(err);
    }
    pos = table_pos;
    for (i = 0; i < noff; i++) {
        if (!rd_u64(b, n, &pos, &offsets[i])) {
            free(offsets);
            return cx_malformed(err, F, "truncated file while reading a u64");
        }
    }
    tables_end = pos;
    if (!h->tiled) {
        tile_w = w;
        tile_h = 1;
        slots = hh;
    } else {
        size_t ty;
        if (h->level_mode != 0) {
            st = cx_unsupported(err, F, "deep mip-map or rip-map tiles (single-level tiles are supported)");
            goto fail;
        }
        tile_w = h->tw;
        tile_h = h->th;
        tiles_x = (w + tile_w - 1) / tile_w;
        ty = (hh + tile_h - 1) / tile_h;
        if (!pc_size_mul(tiles_x, ty, &slots)) {
            st = cx_malformed(err, F, "tile count overflows");
            goto fail;
        }
    }
    counts = calloc(npixels, sizeof *counts);
    seen = calloc(slots > 0 ? slots : 1, sizeof *seen);
    chunks = calloc(noff > 0 ? noff : 1, sizeof *chunks);
    if (counts == NULL || seen == NULL || chunks == NULL) {
        st = cx_nomem(err);
        goto fail;
    }
    for (i = 0; i < noff; i++) {
        uint64_t off = offsets[i];
        size_t p;
        size_t slot;
        size_t row_w;
        size_t *px = NULL;
        size_t npx = 0;
        uint64_t packed_table;
        uint64_t packed_data;
        uint64_t unpacked_data;
        uint64_t remaining;
        size_t expected_table;
        size_t full_table;
        uint8_t *raw = NULL;
        uint32_t *prefix;
        uint32_t row_base = 0;
        uint32_t prev = 0;
        size_t k;
        uint64_t total;
        if (off < tables_end || off >= n) {
            st = cx_malformed(err, F, "chunk offset %llu is outside the chunk area of the file", (unsigned long long)off);
            goto fail;
        }
        p = (size_t)off;
        if (m->multipart) {
            int32_t part;
            if (!rd_i32(b, n, &p, &part)) {
                st = cx_malformed(err, F, "truncated file while reading an i32");
                goto fail;
            }
            if (part < 0 || (size_t)part != hi) {
                st = cx_malformed(err, F, "chunk declares part %d, expected %zu", (int)part, hi);
                goto fail;
            }
        }
        if (!h->tiled) {
            int32_t y;
            int64_t row;
            if (!rd_i32(b, n, &p, &y)) {
                st = cx_malformed(err, F, "truncated file while reading an i32");
                goto fail;
            }
            row = (int64_t)y - h->dw[1];
            if (row < 0 || (uint64_t)row >= hh) {
                st = cx_malformed(err, F, "scan line block y=%d is outside the data window", (int)y);
                goto fail;
            }
            npx = w;
            px = malloc(npx * sizeof *px);
            if (px == NULL) {
                st = cx_nomem(err);
                goto fail;
            }
            for (k = 0; k < w; k++) {
                px[k] = (size_t)row * w + k;
            }
            slot = (size_t)row;
            row_w = w;
        } else {
            int32_t c[4];
            size_t bx;
            size_t by;
            size_t rows;
            size_t cols;
            size_t yy;
            for (k = 0; k < 4; k++) {
                if (!rd_i32(b, n, &p, &c[k])) {
                    st = cx_malformed(err, F, "truncated file while reading an i32");
                    goto fail;
                }
            }
            if (c[2] != 0 || c[3] != 0) {
                st = cx_malformed(err, F, "deep tile at level (%d,%d); only level 0 exists in a single-level file",
                                  (int)c[2], (int)c[3]);
                goto fail;
            }
            if (c[0] < 0 || c[1] < 0 || (size_t)c[0] >= tiles_x || (size_t)c[1] * tile_h >= hh) {
                st = cx_malformed(err, F, "tile (%d,%d) is outside the data window", (int)c[0], (int)c[1]);
                goto fail;
            }
            bx = (size_t)c[0] * tile_w;
            by = (size_t)c[1] * tile_h;
            rows = by + tile_h < hh ? by + tile_h : hh;
            cols = bx + tile_w < w ? bx + tile_w : w;
            npx = (rows - by) * (cols - bx);
            px = malloc((npx > 0 ? npx : 1) * sizeof *px);
            if (px == NULL) {
                st = cx_nomem(err);
                goto fail;
            }
            k = 0;
            for (yy = by; yy < rows; yy++) {
                size_t xx;
                for (xx = bx; xx < cols; xx++) {
                    px[k++] = yy * w + xx;
                }
            }
            slot = (size_t)c[1] * tiles_x + (size_t)c[0];
            row_w = cols - bx;
        }
        if (slot >= slots || seen[slot]) {
            free(px);
            st = cx_malformed(err, F, "two chunks cover block %zu", slot);
            goto fail;
        }
        seen[slot] = true;
        chunks[nchunks].pixels = px;
        chunks[nchunks].npixels = npx;
        chunks[nchunks].row_w = row_w;
        chunks[nchunks].pos = (size_t)off;
        nchunks++;
        if (!rd_u64(b, n, &p, &packed_table) || !rd_u64(b, n, &p, &packed_data) ||
            !rd_u64(b, n, &p, &unpacked_data)) {
            st = cx_malformed(err, F, "truncated file while reading a u64");
            goto fail;
        }
        remaining = (uint64_t)(n - p);
        if (packed_table > remaining || packed_data > remaining - packed_table) {
            st = cx_malformed(err, F, "deep chunk sizes point past the end of the file");
            goto fail;
        }
        if (!pc_size_mul(npx, 4, &expected_table)) {
            st = cx_malformed(err, F, "count table overflows");
            goto fail;
        }
        /* An edge tile's table may be stored at the full tile size. */
        full_table = tile_w * tile_h * 4 > expected_table ? tile_w * tile_h * 4 : expected_table;
        st = decompress(h->compression, b + p, (size_t)packed_table, full_table, "the sample-count table", &raw, NULL);
        if (st != PC_OK) {
            st = decompress(h->compression, b + p, (size_t)packed_table, expected_table, "the sample-count table", &raw,
                            err);
            if (st != PC_OK) {
                goto fail;
            }
        }
        prefix = malloc((npx + 1) * sizeof *prefix);
        if (prefix == NULL) {
            free(raw);
            st = cx_nomem(err);
            goto fail;
        }
        chunks[nchunks - 1].prefix = prefix;
        prefix[0] = 0;
        for (k = 0; k < npx; k++) {
            uint32_t v = cx_le32(raw + 4 * k);
            if (row_w > 0 && k % row_w == 0) {
                if (prev > UINT32_MAX - row_base) {
                    free(raw);
                    st = cx_malformed(err, F, "the block's sample count overflows");
                    goto fail;
                }
                row_base += prev;
                prev = 0;
            }
            if (v < prev) {
                free(raw);
                st = cx_malformed(err, F, "the sample-count table decreases (malformed cumulative counts)");
                goto fail;
            }
            counts[px[k]] = v - prev;
            prev = v;
            if (v > UINT32_MAX - row_base) {
                free(raw);
                st = cx_malformed(err, F, "the block's sample count overflows");
                goto fail;
            }
            prefix[k + 1] = row_base + v;
        }
        free(raw);
        total = prefix[npx];
        if (total * bps_total != unpacked_data) {
            st = cx_malformed(err, F, "the chunk declares %llu bytes of sample data, the count table implies %llu",
                              (unsigned long long)unpacked_data, (unsigned long long)(total * bps_total));
            goto fail;
        }
        chunks[nchunks - 1].packed_data = packed_data;
        chunks[nchunks - 1].unpacked_data = unpacked_data;
    }
    {
        size_t missing = 0;
        for (i = 0; i < slots; i++) {
            missing += !seen[i];
        }
        if (missing > 0) {
            st = cx_malformed(err, F, "%zu of %zu chunks are missing from the offset table", missing, slots);
            goto fail;
        }
    }
    /* global cumulative counts in raster order */
    d.ncounts = npixels + 1;
    d.counts = malloc(d.ncounts * sizeof *d.counts);
    if (d.counts == NULL) {
        st = cx_nomem(err);
        goto fail;
    }
    d.counts[0] = 0;
    for (i = 0; i < npixels; i++) {
        d.counts[i + 1] = d.counts[i] + counts[i];
    }
    {
        uint64_t tot = d.counts[npixels];
        uint64_t bytes = tot > UINT64_MAX / (h->nch * 4 + 1) ? UINT64_MAX : tot * (uint64_t)h->nch * 4;
        if (bytes > l->max_alloc) {
            st = cx_limit(err, "deep samples need %llu bytes, exceeding max_alloc %llu", (unsigned long long)bytes,
                          (unsigned long long)l->max_alloc);
            goto fail;
        }
        d.channels = calloc(h->nch > 0 ? h->nch : 1, sizeof *d.channels);
        if (d.channels == NULL) {
            st = cx_nomem(err);
            goto fail;
        }
        d.nchannels = h->nch;
        for (i = 0; i < h->nch; i++) {
            size_t ln = strlen(h->ch[i].name);
            d.channels[i].name = malloc(ln + 1);
            d.channels[i].samples = calloc(tot > 0 ? (size_t)tot : 1, sizeof(float));
            if (d.channels[i].name == NULL || d.channels[i].samples == NULL) {
                st = cx_nomem(err);
                goto fail;
            }
            memcpy(d.channels[i].name, h->ch[i].name, ln + 1);
            d.channels[i].sample = h->ch[i].type == 1 ? PC_CS_F16 : PC_CS_F32;
            d.channels[i].nsamples = (size_t)tot;
        }
    }
    /* Second pass: the sample data, scattered per pixel. */
    for (i = 0; i < nchunks; i++) {
        const dchunk *c = &chunks[i];
        size_t p = c->pos + (m->multipart ? 4 : 0) + (h->tiled ? 16 : 4);
        uint64_t packed_table;
        uint64_t tmp;
        uint8_t *raw = NULL;
        size_t in_off = 0;
        size_t r0;
        size_t row_w = c->row_w > 0 ? c->row_w : 1;
        if (!rd_u64(b, n, &p, &packed_table) || !rd_u64(b, n, &p, &tmp) || !rd_u64(b, n, &p, &tmp) ||
            packed_table > n - p || c->packed_data > n - p - packed_table) {
            st = cx_malformed(err, F, "truncated file while reading the deep sample data");
            goto fail;
        }
        p += (size_t)packed_table;
        if ((st = decompress(h->compression, b + p, (size_t)c->packed_data, (size_t)c->unpacked_data,
                             "the deep sample data", &raw, err)) != PC_OK) {
            goto fail;
        }
        /* Row by row, and within a row channel by channel. */
        for (r0 = 0; r0 < c->npixels; r0 += row_w) {
            size_t rl = c->npixels - r0 < row_w ? c->npixels - r0 : row_w;
            size_t row_from = c->prefix[r0];
            size_t row_to = c->prefix[r0 + rl];
            size_t row_total = row_to - row_from;
            size_t ci;
            for (ci = 0; ci < h->nch; ci++) {
                int t = h->ch[ci].type;
                size_t sb = sample_bytes(t);
                size_t chb = row_total * sb;
                size_t j;
                if (chb > c->unpacked_data - in_off) {
                    free(raw);
                    st = cx_malformed(err, F, "the sample data is short");
                    goto fail;
                }
                for (j = 0; j < rl; j++) {
                    size_t from = c->prefix[r0 + j] - row_from;
                    size_t cnt = c->prefix[r0 + j + 1] - c->prefix[r0 + j];
                    size_t to = (size_t)d.counts[c->pixels[r0 + j]];
                    size_t k;
                    for (k = 0; k < cnt; k++) {
                        const uint8_t *s = raw + in_off + (from + k) * sb;
                        float v;
                        if (t == 1) {
                            v = pc_f16_to_f32(cx_le16(s));
                        } else if (t == 2) {
                            uint32_t bits = cx_le32(s);
                            memcpy(&v, &bits, 4);
                        } else {
                            v = (float)cx_le32(s);
                        }
                        if (to + k >= d.channels[ci].nsamples) {
                            free(raw);
                            st = cx_malformed(err, F, "the sample data is short");
                            goto fail;
                        }
                        d.channels[ci].samples[to + k] = v;
                    }
                }
                in_off += chb;
            }
        }
        free(raw);
    }
    d.width = (uint32_t)w;
    d.height = (uint32_t)hh;
    *out = d;
    memset(&d, 0, sizeof d);
fail:
    pc_cdeep_fini(&d);
    chunks_free(chunks, nchunks);
    free(offsets);
    free(counts);
    free(seen);
    return st;
}

/* ---- flattening ---- */

typedef struct zkey {
    float z;
    uint32_t i;
} zkey;

/* f32::total_cmp ordering, then the original index (a stable sort). */
static int64_t total_key(float f)
{
    int32_t bits;
    memcpy(&bits, &f, 4);
    bits ^= (int32_t)(((uint32_t)(bits >> 31)) >> 1);
    return bits;
}

static int zcmp(const void *a, const void *b)
{
    const zkey *x = a;
    const zkey *y = b;
    int64_t kx = total_key(x->z);
    int64_t ky = total_key(y->z);
    if (kx != ky) {
        return kx < ky ? -1 : 1;
    }
    return x->i < y->i ? -1 : x->i > y->i ? 1 : 0;
}

pc_status cx_exr_flatten(const pc_cdeep *d, pc_cimage *out, pc_error *err)
{
    size_t npx = (size_t)d->width * d->height;
    const pc_cdeep_channel *R = pc_cdeep_channel_named(d, "R");
    const pc_cdeep_channel *G = pc_cdeep_channel_named(d, "G");
    const pc_cdeep_channel *B = pc_cdeep_channel_named(d, "B");
    const pc_cdeep_channel *A = pc_cdeep_channel_named(d, "A");
    const pc_cdeep_channel *AR = pc_cdeep_channel_named(d, "AR");
    const pc_cdeep_channel *AG = pc_cdeep_channel_named(d, "AG");
    const pc_cdeep_channel *AB = pc_cdeep_channel_named(d, "AB");
    const pc_cdeep_channel *Z = pc_cdeep_channel_named(d, "Z");
    const pc_cdeep_channel *Y = pc_cdeep_channel_named(d, "Y");
    const pc_cdeep_channel *lum;
    bool rgb = R != NULL && G != NULL && B != NULL;
    bool gray = !rgb && (Y != NULL || d->nchannels == 1);
    bool any_alpha = A != NULL || AR != NULL || AG != NULL || AB != NULL;
    pc_clayout layout;
    size_t nc;
    float *o;
    zkey *order = NULL;
    size_t cap = 0;
    uint32_t max_samples = 0;
    size_t p;
    pc_status st;
    if (!rgb && !gray) {
        return cx_unsupported(err, F, "no R/G/B or Y channels in the deep part");
    }
    layout = any_alpha ? (rgb ? PC_CL_RGBA : PC_CL_GRAYA) : (rgb ? PC_CL_RGB : PC_CL_GRAY);
    nc = pc_clayout_channels(layout);
    lum = Y != NULL ? Y : (d->nchannels > 0 ? &d->channels[0] : NULL);
    o = calloc(npx * nc > 0 ? npx * nc : 1, sizeof *o);
    if (o == NULL) {
        return cx_nomem(err);
    }
#define AT(ch, k) ((ch) != NULL && s0 + (k) < (ch)->nsamples ? (ch)->samples[s0 + (k)] : 0.0f)
    for (p = 0; p < npx; p++) {
        uint64_t s, e;
        size_t s0;
        size_t n;
        size_t k;
        float cacc[3] = {0.0f, 0.0f, 0.0f};
        float aacc[3] = {0.0f, 0.0f, 0.0f};
        float a_out;
        float a_cl;
        float *base = o + p * nc;
        pc_cdeep_sample_range(d, p, &s, &e);
        s0 = (size_t)s;
        n = (size_t)(e - s);
        if (n > max_samples) {
            max_samples = n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
        }
        if (n == 0) {
            continue; /* no samples: transparent black */
        }
        if (n > cap) {
            zkey *g = realloc(order, n * sizeof *g);
            if (g == NULL) {
                free(order);
                free(o);
                return cx_nomem(err);
            }
            order = g;
            cap = n;
        }
        for (k = 0; k < n; k++) {
            order[k].i = (uint32_t)k;
            order[k].z = Z != NULL ? (s0 + k < Z->nsamples ? Z->samples[s0 + k] : NAN) : 0.0f;
        }
        if (Z != NULL) {
            qsort(order, n, sizeof *order, zcmp);
        }
        for (k = 0; k < n; k++) {
            uint32_t i = order[k].i;
            float sa = AT(A, i);
            float ai[3];
            float src[3];
            bool covered = true;
            int q;
            ai[0] = A != NULL && AR == NULL ? sa : AR != NULL ? AT(AR, i) : 1.0f;
            ai[1] = A != NULL && AG == NULL ? sa : AG != NULL ? AT(AG, i) : 1.0f;
            ai[2] = A != NULL && AB == NULL ? sa : AB != NULL ? AT(AB, i) : 1.0f;
            if (rgb) {
                src[0] = AT(R, i);
                src[1] = AT(G, i);
                src[2] = AT(B, i);
            } else {
                src[0] = src[1] = src[2] = AT(lum, i);
            }
            for (q = 0; q < 3; q++) {
                float t = pc_rs_maxf_k(1.0f - aacc[q], 0.0f);
                cacc[q] += src[q] * t;
                aacc[q] += t * ai[q];
                covered = covered && aacc[q] >= 1.0f;
            }
            if (covered) {
                break; /* everything behind is hidden */
            }
        }
        a_out = !any_alpha ? 1.0f : (aacc[0] + aacc[1] + aacc[2]) / 3.0f;
        a_cl = pc_clampf(a_out, 0.0f, 1.0f);
#define STRAIGHT(c) (a_cl > 1e-6f ? (c) / a_cl : 0.0f)
        if (rgb) {
            base[0] = STRAIGHT(cacc[0]);
            base[1] = STRAIGHT(cacc[1]);
            base[2] = STRAIGHT(cacc[2]);
            if (nc == 4) {
                base[3] = a_cl;
            }
        } else {
            base[0] = STRAIGHT(cacc[0]);
            if (nc == 2) {
                base[1] = a_cl;
            }
        }
#undef STRAIGHT
    }
#undef AT
    free(order);
    st = pc_cimage_from_raw(d->width, d->height, layout, PC_CS_F32, (uint8_t *)o, npx * nc * sizeof(float), out, err);
    if (st == PC_OK) {
        pc_cwarning w;
        memset(&w, 0, sizeof w);
        w.kind = PC_CW_DEEP_FLATTENED;
        w.format = F;
        w.max_samples = max_samples;
        st = pc_cimage_add_warning(out, w, err);
        if (st != PC_OK) {
            pc_cimage_fini(out);
        }
    }
    return st;
}

pc_status pc_cdecode_deep_exr(const uint8_t *b, size_t n, const pc_climits *l, pc_cdeep *out, pc_error *err)
{
    exr_meta m;
    pc_status st = cx_exr_meta_parse(b, n, &m, err);
    if (st != PC_OK) {
        return st;
    }
    st = cx_exr_decode_deep(&m, b, n, l, out, err);
    cx_exr_meta_fini(&m);
    return st;
}
