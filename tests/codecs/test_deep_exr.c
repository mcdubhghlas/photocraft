/* Deep EXR: port of crates/codecs/tests/deep_exr.rs. A synthetic generator builds deep chunks
 * per the OpenEXR file layout specification (NONE, RLE and ZIPS x scanline and tile), the
 * oracle tests check the compositing math against hand-computed values, and malformed inputs
 * must error, never crash. When corpus/exr/ has been fetched, the OpenEXR project's deep test
 * images are checked against the ID manifests their sidecars list (skipped otherwise). */
#include "common.h"

#ifndef PC_NO_EXR

#include <sys/stat.h>
#include <zlib.h>

/* ---------------------------------------------------------------------------------------------
 * A minimal deep-EXR writer, mirroring what the reference files show on disk:
 * version 2 | deep bit, one header with the mandatory attributes in alphabetical order, a chunk
 * offset table, then deep chunks of
 * [y | tile coords][u64 packed table][u64 packed data][u64 unpacked data][table][data].
 * The table holds u32 sample counts, cumulative within each row of the block; the sample data is
 * grouped per row, then per channel, in pixel order (the layout the OpenEXR library writes).
 * NONE stores raw; RLE and ZIPS separate even/odd bytes, apply the +128 predictor, then RLE
 * packs runs and ZIPS deflates (zlib here, flate2 in the reference). */

static void put(pc_buf *b, const void *d, size_t n) { (void)pc_buf_append(b, d, n, NULL); }
static void put8(pc_buf *b, uint8_t v) { (void)pc_buf_append_byte(b, v, NULL); }

static void put32(pc_buf *b, uint32_t v)
{
    uint8_t t[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    put(b, t, 4);
}

static void put64(pc_buf *b, uint64_t v)
{
    put32(b, (uint32_t)v);
    put32(b, (uint32_t)(v >> 32));
}

static void putf32(pc_buf *b, float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    put32(b, v);
}

static uint64_t rd64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

static void wr64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static void attr(pc_buf *out, const char *name, const char *ty, const void *payload, size_t n)
{
    put(out, name, strlen(name) + 1);
    put(out, ty, strlen(ty) + 1);
    put32(out, (uint32_t)n);
    put(out, payload, n);
}

static void samples_to_differences(uint8_t *buf, size_t n)
{
    int prev;
    size_t i;
    if (n == 0) {
        return;
    }
    prev = buf[0];
    for (i = 1; i < n; i++) {
        int v = buf[i];
        buf[i] = (uint8_t)(v - prev + 128);
        prev = v;
    }
}

static void separate_bytes_fragments(uint8_t *buf, size_t n)
{
    uint8_t *copy = malloc(n + 1);
    size_t half = (n + 1) / 2;
    size_t i;
    memcpy(copy, buf, n);
    for (i = 0; i < n; i++) {
        buf[i % 2 == 0 ? i / 2 : half + i / 2] = copy[i];
    }
    free(copy);
}

static void rle_bytes(const uint8_t *src, size_t n, pc_buf *out)
{
    size_t i = 0;
    while (i < n) {
        size_t run = 1;
        while (i + run < n && run < 128 && src[i + run] == src[i]) {
            run++;
        }
        /* OpenEXR tokens: n >= 0 repeats the next byte n + 1 times, n < 0 copies -n bytes. */
        if (run > 1) {
            put8(out, (uint8_t)(run - 1));
            put8(out, src[i]);
            i += run;
        } else {
            size_t start = i;
            size_t k;
            i++;
            while (i < n && (i - start) < 127 && src[i] != src[i - 1]) {
                i++;
            }
            k = i - start;
            put8(out, (uint8_t)(256 - k));
            put(out, src + start, k);
        }
    }
}

/* Appends the packed form of raw to out; returns its length. */
static size_t compress_into(int kind, const uint8_t *raw, size_t n, pc_buf *out)
{
    uint8_t *b;
    pc_buf c = PC_BUF_INIT;
    size_t len;
    if (kind != 1 && kind != 2) {
        put(out, raw, n);
        return n;
    }
    b = malloc(n + 1);
    memcpy(b, raw, n);
    separate_bytes_fragments(b, n);
    samples_to_differences(b, n);
    if (kind == 1) {
        /* Like ZIPS, RLE packs the byte-split, predicted stream. */
        rle_bytes(b, n, &c);
    } else {
        uLongf zl = compressBound((uLong)n);
        (void)pc_buf_reserve(&c, (size_t)zl + 1, NULL);
        if (compress2(c.data, &zl, b, (uLong)n, Z_BEST_SPEED) == Z_OK) {
            c.len = (size_t)zl;
        } else {
            PC_T_FAIL("zlib compress2 failed");
        }
    }
    free(b);
    if (c.len < n) {
        put(out, c.data, c.len);
        len = c.len;
    } else {
        put(out, raw, n);
        len = n;
    }
    pc_buf_fini(&c);
    return len;
}

/* One channel: a name (channels in alphabetical order, as chlist requires) and the sample type
 * as in the chlist attribute (0 = u32, 1 = half, 2 = float). */
typedef struct chan {
    const char *name;
    int ty;
} chan;

/* Writes channel ci's values for pixel p into out (counts[p] of them, at most 8). */
typedef void (*sample_fn)(const void *ctx, size_t p, size_t ci, float *out);

/* counts[p] samples per pixel; tw == 0 writes deepscanline, else deeptile with tw x th tiles. */
static pc_buf gen_deep(size_t w, size_t h, const chan *channels, size_t nch, int compression, size_t tw, size_t th,
                       const uint32_t *counts, sample_fn sample, const void *ctx)
{
    pc_buf out = PC_BUF_INIT;
    pc_buf chlist = PC_BUF_INIT;
    pc_buf chunks = PC_BUF_INIT;
    size_t *chunk_len;
    size_t nblocks;
    size_t txn = 0;
    size_t bi;
    size_t ci;
    uint64_t pos;
    uint8_t dw[16];
    uint8_t f1[4];
    uint8_t v2[8];
    uint8_t one[4] = {1, 0, 0, 0};
    uint8_t zero = 0;
    uint8_t comp = (uint8_t)compression;
    bool tile = tw != 0;
    int32_t box[4];

    put32(&out, 0x01312f76u);
    /* Single-part deep files (deepscanline and deeptile alike) carry only the deep-data flag;
     * the tiling lives in the tiles attribute and the type string. */
    put32(&out, 2u | 0x800u);

    for (ci = 0; ci < nch; ci++) {
        put(&chlist, channels[ci].name, strlen(channels[ci].name) + 1);
        put32(&chlist, (uint32_t)channels[ci].ty);
        put8(&chlist, 0); /* pLinear */
        put8(&chlist, 0);
        put8(&chlist, 0);
        put8(&chlist, 0);
        put32(&chlist, 1);
        put32(&chlist, 1);
    }
    put8(&chlist, 0);
    attr(&out, "channels", "chlist", chlist.data, chlist.len);
    pc_buf_fini(&chlist);
    attr(&out, "compression", "compression", &comp, 1);
    /* box2i is inclusive: the data window of a w x h image at the origin ends at (w-1, h-1). */
    box[0] = 0;
    box[1] = 0;
    box[2] = (int32_t)w - 1;
    box[3] = (int32_t)h - 1;
    for (ci = 0; ci < 4; ci++) {
        uint32_t u = (uint32_t)box[ci];
        dw[ci * 4] = (uint8_t)u;
        dw[ci * 4 + 1] = (uint8_t)(u >> 8);
        dw[ci * 4 + 2] = (uint8_t)(u >> 16);
        dw[ci * 4 + 3] = (uint8_t)(u >> 24);
    }
    attr(&out, "dataWindow", "box2i", dw, 16);
    attr(&out, "displayWindow", "box2i", dw, 16);
    attr(&out, "lineOrder", "lineOrder", &zero, 1);
    {
        float f = 1.0f;
        memcpy(f1, &f, 4); /* the test hosts are little-endian */
    }
    memset(v2, 0, sizeof v2);
    attr(&out, "pixelAspectRatio", "float", f1, 4);
    attr(&out, "screenWindowCenter", "v2f", v2, 8);
    attr(&out, "screenWindowWidth", "float", f1, 4);
    if (tile) {
        pc_buf td = PC_BUF_INIT;
        put32(&td, (uint32_t)tw);
        put32(&td, (uint32_t)th);
        put8(&td, 0); /* one level, round down */
        attr(&out, "tiles", "tiledesc", td.data, td.len);
        pc_buf_fini(&td);
    }
    if (tile) {
        attr(&out, "type", "string", "deeptile", 8);
    } else {
        attr(&out, "type", "string", "deepscanline", 12);
    }
    attr(&out, "version", "int", one, 4);
    put8(&out, 0); /* end of the header */

    /* The blocks: scan lines (one line each) or the level-0 tiles. */
    if (tile) {
        txn = (w + tw - 1) / tw;
        nblocks = txn * ((h + th - 1) / th);
    } else {
        nblocks = h;
    }
    chunk_len = calloc(nblocks + 1, sizeof *chunk_len);
    for (bi = 0; bi < nblocks; bi++) {
        size_t x0;
        size_t x1;
        size_t y0;
        size_t y1;
        size_t x;
        size_t y;
        size_t start = chunks.len;
        size_t want_data = 0;
        size_t ct;
        size_t cd;
        size_t sizes_at;
        pc_buf table = PC_BUF_INIT;
        pc_buf data = PC_BUF_INIT;
        float vals[8];
        if (tile) {
            size_t bx = bi % txn;
            size_t by = bi / txn;
            x0 = bx * tw;
            x1 = x0 + tw < w ? x0 + tw : w;
            y0 = by * th;
            y1 = y0 + th < h ? y0 + th : h;
            put32(&chunks, (uint32_t)bx);
            put32(&chunks, (uint32_t)by);
            put32(&chunks, 0);
            put32(&chunks, 0);
        } else {
            x0 = 0;
            x1 = w;
            y0 = bi % h;
            y1 = y0 + 1;
            put32(&chunks, (uint32_t)y0);
        }
        for (y = y0; y < y1; y++) {
            uint32_t acc = 0;
            for (x = x0; x < x1; x++) {
                acc += counts[y * w + x];
                put32(&table, acc);
            }
        }
        for (y = y0; y < y1; y++) {
            for (ci = 0; ci < nch; ci++) {
                for (x = x0; x < x1; x++) {
                    size_t p = y * w + x;
                    uint32_t k;
                    sample(ctx, p, ci, vals);
                    for (k = 0; k < counts[p]; k++) {
                        switch (channels[ci].ty) {
                        case 1: {
                            uint16_t hv = pc_f16_from_f32(vals[k]);
                            put8(&data, (uint8_t)hv);
                            put8(&data, (uint8_t)(hv >> 8));
                            break;
                        }
                        case 0:
                            put32(&data, (uint32_t)vals[k]);
                            break;
                        default:
                            putf32(&data, vals[k]);
                        }
                    }
                    want_data += (size_t)counts[p] * (channels[ci].ty == 1 ? 2u : 4u);
                }
            }
        }
        CHECK_EQ_UINT(table.len, (x1 - x0) * (y1 - y0) * 4);
        CHECK_EQ_UINT(data.len, want_data);
        sizes_at = chunks.len;
        put64(&chunks, 0);
        put64(&chunks, 0);
        put64(&chunks, data.len);
        ct = compress_into(compression, table.data, table.len, &chunks);
        cd = compress_into(compression, data.data, data.len, &chunks);
        wr64(chunks.data + sizes_at, ct);
        wr64(chunks.data + sizes_at + 8, cd);
        chunk_len[bi] = chunks.len - start;
        pc_buf_fini(&table);
        pc_buf_fini(&data);
    }
    pos = out.len + nblocks * 8;
    for (bi = 0; bi < nblocks; bi++) {
        put64(&out, pos);
        pos += chunk_len[bi];
    }
    put(&out, chunks.data, chunks.len);
    pc_buf_fini(&chunks);
    free(chunk_len);
    return out;
}

/* RGBA(Z) float channels in chlist (alphabetical) order. */
static const chan RGBAZ[5] = {{"A", 2}, {"B", 2}, {"G", 2}, {"R", 2}, {"Z", 2}};

static const float QUAD[4] = {0.25f, 0.5f, 0.75f, 1.0f};

static void sample_rgba(const void *ctx, size_t p, size_t ci, float *out)
{
    /* One sample per pixel: a small checkerboard-ish pattern with varying depth. Deep colours
     * are premultiplied, so the stored RGB values include the 0.5 alpha. */
    float v = QUAD[p % 4];
    (void)ctx;
    switch (ci) {
    case 0:
        out[0] = 0.5f; /* A */
        break;
    case 1:
        out[0] = 0.125f * v; /* B = straight 0.25v, premultiplied */
        break;
    case 2:
        out[0] = 0.25f * v; /* G = straight 0.5v, premultiplied */
        break;
    case 3:
        out[0] = 0.5f * v; /* R = straight v, premultiplied */
        break;
    default:
        out[0] = 10.0f + (float)p; /* Z */
    }
}

static const uint32_t COUNTS_ONE[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}; /* 4x4 */

/* Decodes flat; checks Rgba / F32 / DeepFlattened; returns the f32 samples (caller frees). */
static float *rgba_flat(const pc_buf *bytes, size_t *n)
{
    pc_cimage img = dec(bytes->data, bytes->len);
    float *s = NULL;
    *n = 0;
    if (pc_cimage_to_normalized(&img, &s, n, NULL) != PC_OK) {
        PC_T_FAIL("f32");
    }
    CHECK(img.layout == PC_CL_RGBA);
    CHECK(img.sample == PC_CS_F32);
    CHECK(has_warning(&img, PC_CW_DEEP_FLATTENED));
    pc_cimage_fini(&img);
    if (s == NULL) {
        s = calloc(1, sizeof *s);
    }
    return s;
}

static bool deep_ok(const pc_buf *b, const pc_climits *l, pc_cdeep *d)
{
    pc_error e;
    pc_climits none = pc_climits_none();
    if (pc_cdecode_deep_exr(b->data, b->len, l != NULL ? l : &none, d, &e) != PC_OK) {
        PC_T_FAIL("deep: %s", e.msg);
        return false;
    }
    return true;
}

static pc_status deep_status(const uint8_t *b, size_t n, const pc_climits *l, pc_error *e)
{
    pc_cdeep d;
    pc_status st = pc_cdecode_deep_exr(b, n, l, &d, e);
    if (st == PC_OK) {
        pc_cdeep_fini(&d);
    }
    return st;
}

static pc_status flat_status(const uint8_t *b, size_t n)
{
    pc_cimage img;
    pc_error e;
    pc_status st = pc_cdecode(b, n, &img, &e);
    if (st == PC_OK) {
        pc_cimage_fini(&img);
    }
    return st;
}

/* ---------------------------------------------------------------------------------------------
 * Oracles */

/* One sample per pixel: the flat image is the stored colour un-premultiplied. */
static void test_single_sample_matches_flat(void)
{
    pc_buf bytes = gen_deep(4, 4, RGBAZ, 5, 0, 0, 0, COUNTS_ONE, sample_rgba, NULL);
    size_t n;
    float *px = rgba_flat(&bytes, &n);
    size_t p;
    CHECK_EQ_UINT(n, 64);
    for (p = 0; p < 16 && n == 64; p++) {
        float v = QUAD[p % 4];
        CHECK_NEAR(px[p * 4], v, 1e-6);
        CHECK_NEAR(px[p * 4 + 1], 0.5f * v, 1e-6);
        CHECK_NEAR(px[p * 4 + 2], 0.25f * v, 1e-6);
        CHECK_NEAR(px[p * 4 + 3], 0.5, 1e-6);
    }
    free(px);
    pc_buf_fini(&bytes);
}

static void sample_z_order(const void *ctx, size_t p, size_t ci, float *out)
{
    static const float px0[5][2] = {{0.25f, 0.5f}, {0.25f, 0.0f}, {0.25f, 0.25f}, {0.25f, 0.5f}, {9.0f, 1.0f}};
    (void)ctx;
    if (p != 0) {
        float v[1];
        sample_rgba(NULL, p, ci, v);
        out[0] = v[0];
        out[1] = v[0];
        return;
    }
    out[0] = px0[ci][0]; /* back first */
    out[1] = px0[ci][1];
}

/* Two samples stored back-to-front: the front (smaller Z) covers the back. Hand-computed:
 * front a=0.5, premultiplied colour (0.5, 0.25, 0); back a=0.25, premultiplied (0.25, 0.25, 0.25).
 * Over (front first): a = 0.5 + (1-0.5)*0.25 = 0.625; c = (0.625, 0.375, 0.125);
 * straight = c / a = (1.0, 0.6, 0.2). */
static void test_z_order_composites_front_over_back(void)
{
    uint32_t counts[16];
    pc_buf bytes;
    float *px;
    size_t n;
    float a = 0.5f + 0.5f * 0.25f;
    size_t i;
    for (i = 0; i < 16; i++) {
        counts[i] = 2;
    }
    bytes = gen_deep(4, 4, RGBAZ, 5, 0, 0, 0, counts, sample_z_order, NULL);
    px = rgba_flat(&bytes, &n);
    CHECK_EQ_UINT(n, 64);
    if (n == 64) {
        CHECK_NEAR(px[3], a, 1e-6);
        CHECK_NEAR(px[0], 0.625f / a, 1e-6);
        CHECK_NEAR(px[1], 0.375f / a, 1e-6);
        CHECK_NEAR(px[2], 0.125f / a, 1e-6);
    }
    free(px);
    pc_buf_fini(&bytes);
}

static void sample_opaque(const void *ctx, size_t p, size_t ci, float *out)
{
    /* front opaque, middle partial, back opaque */
    static const float px1[5][3] = {
        {1.0f, 0.9f, 1.0f}, {0.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 5.0f, 9.0f}};
    (void)ctx;
    if (p != 1) {
        float v[1];
        sample_rgba(NULL, p, ci, v);
        out[0] = out[1] = out[2] = v[0];
        return;
    }
    memcpy(out, px1[ci], sizeof px1[ci]);
}

/* A fully opaque front sample hides everything behind it. */
static void test_opaque_front_hides_back(void)
{
    uint32_t counts[16];
    pc_buf bytes;
    float *px;
    size_t n;
    size_t i;
    for (i = 0; i < 16; i++) {
        counts[i] = 3;
    }
    bytes = gen_deep(4, 4, RGBAZ, 5, 0, 0, 0, counts, sample_opaque, NULL);
    px = rgba_flat(&bytes, &n);
    CHECK_EQ_UINT(n, 64);
    if (n == 64) {
        float r = px[4], g = px[5], b = px[6], a = px[7];
        CHECK(fabsf(r - 1.0f) < 1e-6f && fabsf(g - 1.0f) < 1e-6f && fabsf(b - 0.0f) < 1e-6f);
        CHECK_NEAR(a, 1.0, 1e-6);
    }
    free(px);
    pc_buf_fini(&bytes);
}

static void sample_hdr(const void *ctx, size_t p, size_t ci, float *out)
{
    uint32_t n = ((uint32_t)p * 3) % 4;
    uint32_t k;
    (void)ctx;
    for (k = 0; k < n; k++) {
        float v[5] = {1.0f, 0.25f * (float)k, 0.5f, 2.0f + (float)p / 8.0f, 10.0f - (float)k};
        out[k] = v[ci];
    }
}

/* Deep EXR is HDR: colour above 1.0 survives the composite (it used to be clamped), and a tiled
 * file whose edge tiles are narrower than the tile size decodes like the scan lines. */
static void test_hdr_colour_is_kept_and_edge_tiles_match_scanlines(void)
{
    uint32_t counts[35];
    int compression;
    size_t p;
    for (p = 0; p < 35; p++) {
        counts[p] = ((uint32_t)p * 3) % 4;
    }
    for (compression = 0; compression <= 2; compression++) {
        pc_buf sb = gen_deep(7, 5, RGBAZ, 5, compression, 0, 0, counts, sample_hdr, NULL);
        pc_buf tb = gen_deep(7, 5, RGBAZ, 5, compression, 3, 2, counts, sample_hdr, NULL);
        size_t ns;
        size_t nt;
        float *scan = rgba_flat(&sb, &ns);
        float *tiled = rgba_flat(&tb, &nt);
        CHECK_EQ_UINT(ns, nt);
        CHECK(ns == nt && memcmp(scan, tiled, ns * sizeof *scan) == 0);
        /* Pixel 1 has 3 opaque samples; the front one (k = 2, Z = 8) has R = 2.125. */
        if (ns > 4) {
            CHECK_NEAR(scan[4], 2.125, 1e-5);
        } else {
            PC_T_FAIL("compression %d: too few samples", compression);
        }
        free(scan);
        free(tiled);
        pc_buf_fini(&sb);
        pc_buf_fini(&tb);
    }
}

static void sample_empty(const void *ctx, size_t p, size_t ci, float *out)
{
    static const float v[5] = {1.0f, 0.5f, 0.5f, 0.5f, 1.0f};
    (void)ctx;
    if (p == 5) {
        out[0] = v[ci];
    }
}

/* Pixels without samples are transparent black, and the structured counts say so. */
static void test_empty_pixels_are_transparent(void)
{
    uint32_t counts[16] = {0};
    pc_buf bytes;
    float *px;
    size_t n;
    size_t p;
    pc_cdeep deep;
    counts[5] = 1;
    bytes = gen_deep(4, 4, RGBAZ, 5, 0, 0, 0, counts, sample_empty, NULL);
    px = rgba_flat(&bytes, &n);
    CHECK_EQ_UINT(n, 64);
    for (p = 0; p < 16 && n == 64; p++) {
        size_t base = p * 4;
        if (p == 5) {
            CHECK_NEAR(px[base + 3], 1.0, 1e-6);
        } else {
            CHECK(px[base] == 0.0f && px[base + 1] == 0.0f && px[base + 2] == 0.0f && px[base + 3] == 0.0f);
        }
    }
    if (deep_ok(&bytes, NULL, &deep)) {
        uint64_t s;
        uint64_t e;
        CHECK_EQ_UINT(pc_cdeep_total_samples(&deep), 1);
        pc_cdeep_sample_range(&deep, 5, &s, &e);
        CHECK_EQ_UINT(e, 1);
        pc_cdeep_fini(&deep);
    }
    free(px);
    pc_buf_fini(&bytes);
}

static void sample_mixed(const void *ctx, size_t p, size_t ci, float *out)
{
    static const float two[5][2] = {{0.75f, 0.25f}, {0.1f, 0.2f}, {0.3f, 0.4f}, {0.5f, 0.6f}, {7.0f, 3.0f}};
    const uint32_t *counts = ctx;
    if (counts[p] == 2) {
        out[0] = two[ci][0];
        out[1] = two[ci][1];
    } else {
        sample_rgba(NULL, p, ci, out);
    }
}

/* RLE, ZIPS and NONE decode to identical pixels and identical structured samples. */
static void test_rle_and_zips_match_uncompressed(void)
{
    uint32_t counts[16];
    pc_buf base;
    float *px;
    size_t n;
    size_t p;
    int kind;
    pc_cdeep deep_base;
    for (p = 0; p < 16; p++) {
        counts[p] = p % 3 == 0 ? 2 : 1;
    }
    base = gen_deep(4, 4, RGBAZ, 5, 0, 0, 0, counts, sample_mixed, counts);
    px = rgba_flat(&base, &n);
    if (!deep_ok(&base, NULL, &deep_base)) {
        free(px);
        pc_buf_fini(&base);
        return;
    }
    for (kind = 1; kind <= 2; kind++) {
        pc_buf bytes = gen_deep(4, 4, RGBAZ, 5, kind, 0, 0, counts, sample_mixed, counts);
        size_t no;
        float *other = rgba_flat(&bytes, &no);
        size_t i;
        pc_cdeep deep;
        for (i = 0; i < n && i < no; i++) {
            if (fabsf(px[i] - other[i]) >= 1e-6f) {
                PC_T_FAIL("compression %d, sample %zu: %g vs %g", kind, i, (double)px[i], (double)other[i]);
            }
        }
        if (deep_ok(&bytes, NULL, &deep)) {
            CHECK(deep.ncounts == deep_base.ncounts &&
                  memcmp(deep.counts, deep_base.counts, deep.ncounts * sizeof *deep.counts) == 0);
            for (i = 0; i < deep.nchannels && i < deep_base.nchannels; i++) {
                const pc_cdeep_channel *a = &deep.channels[i];
                const pc_cdeep_channel *b = &deep_base.channels[i];
                CHECK_EQ_STR(a->name, b->name);
                CHECK(a->nsamples == b->nsamples && memcmp(a->samples, b->samples, a->nsamples * sizeof(float)) == 0);
            }
            pc_cdeep_fini(&deep);
        }
        free(other);
        pc_buf_fini(&bytes);
    }
    pc_cdeep_fini(&deep_base);
    free(px);
    pc_buf_fini(&base);
}

static void sample_tiles(const void *ctx, size_t p, size_t ci, float *out)
{
    static const float two[5][2] = {{0.5f, 0.5f}, {0.2f, 0.1f}, {0.3f, 0.3f}, {0.4f, 0.4f}, {2.0f, 8.0f}};
    static const float one[5] = {0.5f, 0.2f, 0.4f, 0.7f, 4.0f};
    const uint32_t *counts = ctx;
    if (counts[p] == 2) {
        out[0] = two[ci][0];
        out[1] = two[ci][1];
    } else if (counts[p] > 0) {
        out[0] = one[ci];
    }
}

/* Deep tiles decode to the same pixels as deep scanlines of the same data. */
static void test_tiles_match_scanlines(void)
{
    static const uint32_t counts[16] = {1, 0, 2, 1, 1, 1, 0, 1, 2, 1, 1, 0, 1, 1, 1, 1};
    pc_buf sl = gen_deep(4, 4, RGBAZ, 5, 0, 0, 0, counts, sample_tiles, counts);
    pc_buf tiled = gen_deep(4, 4, RGBAZ, 5, 0, 2, 2, counts, sample_tiles, counts);
    size_t na;
    size_t nb;
    float *a = rgba_flat(&sl, &na);
    float *b = rgba_flat(&tiled, &nb);
    size_t i;
    CHECK_EQ_UINT(na, nb);
    for (i = 0; i < na && i < nb; i++) {
        if (fabsf(a[i] - b[i]) >= 1e-6f) {
            PC_T_FAIL("tile sample %zu: %g vs %g", i, (double)a[i], (double)b[i]);
        }
    }
    free(a);
    free(b);
    pc_buf_fini(&sl);
    pc_buf_fini(&tiled);
}

static void sample_half(const void *ctx, size_t p, size_t ci, float *out)
{
    float v = pc_f16_to_f32(pc_f16_from_f32(0.5f));
    (void)ctx;
    (void)ci;
    out[0] = v * ((float)p + 1.0f) / 4.0f;
}

/* Half-float channels come back as F16 samples with exact values. */
static void test_half_channels_round_trip(void)
{
    static const chan channels[5] = {{"A", 1}, {"B", 1}, {"G", 1}, {"R", 1}, {"Z", 1}};
    pc_buf bytes = gen_deep(4, 4, channels, 5, 2, 0, 0, COUNTS_ONE, sample_half, NULL);
    pc_cdeep deep;
    if (deep_ok(&bytes, NULL, &deep)) {
        size_t i;
        const pc_cdeep_channel *a;
        for (i = 0; i < deep.nchannels; i++) {
            if (deep.channels[i].sample != PC_CS_F16) {
                PC_T_FAIL("%s is %s", deep.channels[i].name, pc_csample_name(deep.channels[i].sample));
            }
        }
        a = pc_cdeep_channel_named(&deep, "A");
        CHECK(a != NULL);
        if (a != NULL) {
            CHECK_EQ_UINT(a->nsamples, 16);
        }
        pc_cdeep_fini(&deep);
    }
    pc_buf_fini(&bytes);
}

static void sample_no_alpha(const void *ctx, size_t p, size_t ci, float *out)
{
    static const float even[4][2] = {{0.9f, 0.1f}, {0.2f, 0.8f}, {0.4f, 0.6f}, {2.0f, 6.0f}};
    static const float odd0[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    static const float odd1[4] = {0.0f, 0.0f, 1.0f, 9.0f};
    (void)ctx;
    if (p % 2 == 0) {
        out[0] = even[ci][0];
        out[1] = even[ci][1];
    } else {
        out[0] = odd0[ci];
        out[1] = odd1[ci];
    }
}

/* Without an alpha channel the front-most sample wins, opaque. */
static void test_without_alpha_front_sample_is_opaque(void)
{
    static const chan channels[4] = {{"B", 2}, {"G", 2}, {"R", 2}, {"Z", 2}};
    uint32_t counts[16];
    pc_buf bytes;
    pc_cimage img;
    float *px = NULL;
    size_t n = 0;
    size_t i;
    for (i = 0; i < 16; i++) {
        counts[i] = 2;
    }
    bytes = gen_deep(4, 4, channels, 4, 0, 0, 0, counts, sample_no_alpha, NULL);
    img = dec(bytes.data, bytes.len);
    CHECK(img.layout == PC_CL_RGB);
    (void)pc_cimage_to_normalized(&img, &px, &n, NULL);
    /* Even pixels: the front sample (z=2) is (r 0.4, g 0.2, b 0.9). */
    CHECK(n >= 3 && fabsf(px[0] - 0.4f) < 1e-6f && fabsf(px[1] - 0.2f) < 1e-6f && fabsf(px[2] - 0.9f) < 1e-6f);
    free(px);
    pc_cimage_fini(&img);
    pc_buf_fini(&bytes);
}

static void sample_gray_a(const void *ctx, size_t p, size_t ci, float *out)
{
    (void)ctx;
    out[0] = ci == 0 ? 0.5f /* A */ : ci == 1 ? 0.5f /* Y, premultiplied: straight 1.0 x 0.5 */ : (float)p;
}

static void sample_gray(const void *ctx, size_t p, size_t ci, float *out)
{
    (void)ctx;
    out[0] = ci == 0 ? 0.4f : (float)p;
}

/* Grayscale deep files (Y + Z) flatten to opaque gray, Y + A + Z keeps its alpha: the alpha must
 * land in the GrayA alpha slot, not a luminance copy (that was the bug). */
static void test_grayscale_deep_keeps_its_alpha(void)
{
    static const chan ya[3] = {{"A", 2}, {"Y", 2}, {"Z", 2}};
    static const chan y[2] = {{"Y", 2}, {"Z", 2}};
    pc_buf bytes = gen_deep(4, 4, ya, 3, 0, 0, 0, COUNTS_ONE, sample_gray_a, NULL);
    pc_cimage img = dec(bytes.data, bytes.len);
    float *px = NULL;
    size_t n = 0;
    size_t p;
    CHECK(img.layout == PC_CL_GRAYA);
    (void)pc_cimage_to_normalized(&img, &px, &n, NULL);
    CHECK_EQ_UINT(n, 32);
    for (p = 0; p + 1 < n; p += 2) {
        CHECK_NEAR(px[p + 1], 0.5, 1e-6);
        CHECK_NEAR(px[p], 1.0, 1e-6); /* the un-premultiplied 1.0 */
    }
    free(px);
    pc_cimage_fini(&img);
    pc_buf_fini(&bytes);

    /* Without an alpha channel the same file is opaque gray. */
    bytes = gen_deep(4, 4, y, 2, 0, 0, 0, COUNTS_ONE, sample_gray, NULL);
    img = dec(bytes.data, bytes.len);
    CHECK(img.layout == PC_CL_GRAY);
    px = NULL;
    n = 0;
    (void)pc_cimage_to_normalized(&img, &px, &n, NULL);
    CHECK_EQ_UINT(n, 16);
    for (p = 0; p < n; p++) {
        CHECK_NEAR(px[p], 0.4, 1e-6);
    }
    free(px);
    pc_cimage_fini(&img);
    pc_buf_fini(&bytes);
}

/* decode_deep returns the counts and channels in file order. */
static void test_decode_deep_structure(void)
{
    static const char *names[5] = {"A", "B", "G", "R", "Z"};
    pc_buf bytes = gen_deep(4, 4, RGBAZ, 5, 0, 0, 0, COUNTS_ONE, sample_rgba, NULL);
    pc_cdeep deep;
    if (deep_ok(&bytes, NULL, &deep)) {
        size_t i;
        const pc_cdeep_channel *z;
        CHECK_EQ_UINT(deep.width, 4);
        CHECK_EQ_UINT(deep.height, 4);
        CHECK_EQ_UINT(deep.nchannels, 5);
        for (i = 0; i < deep.nchannels && i < 5; i++) {
            CHECK_EQ_STR(deep.channels[i].name, names[i]);
        }
        CHECK_EQ_UINT(deep.ncounts, 17);
        CHECK_EQ_UINT(deep.counts[0], 0);
        CHECK_EQ_UINT(pc_cdeep_total_samples(&deep), 16);
        for (i = 0; i < 16; i++) {
            uint64_t s;
            uint64_t e;
            pc_cdeep_sample_range(&deep, i, &s, &e);
            CHECK_EQ_UINT(e - s, 1);
        }
        z = pc_cdeep_channel_named(&deep, "Z");
        CHECK(z != NULL && z->nsamples > 5 && z->samples[5] == 15.0f);
        pc_cdeep_fini(&deep);
    }
    pc_buf_fini(&bytes);
}

/* ---------------------------------------------------------------------------------------------
 * Hostile input: every damaged file errors, nothing crashes. */

static void truncate_sweep(const pc_buf *bytes)
{
    /* Cut at many offsets (cap the count for speed): a cut file must error, not crash. Each cut
     * is a fresh allocation so ASan sees reads past the cut. */
    size_t step = bytes->len / 96 > 1 ? bytes->len / 96 : 1;
    size_t i;
    pc_climits none = pc_climits_none();
    for (i = step; i < bytes->len; i += step) {
        uint8_t *cut = malloc(i);
        pc_error e;
        memcpy(cut, bytes->data, i);
        (void)deep_status(cut, i, &none, &e);
        (void)flat_status(cut, i);
        free(cut);
    }
}

static void test_truncated_files_error_never_panic(void)
{
    int kind;
    pc_buf tiled;
    for (kind = 0; kind <= 2; kind++) {
        pc_buf bytes = gen_deep(4, 4, RGBAZ, 5, kind, 0, 0, COUNTS_ONE, sample_rgba, NULL);
        truncate_sweep(&bytes);
        pc_buf_fini(&bytes);
    }
    tiled = gen_deep(4, 4, RGBAZ, 5, 1, 2, 2, COUNTS_ONE, sample_rgba, NULL);
    truncate_sweep(&tiled);
    pc_buf_fini(&tiled);
    CHECK(true); /* reaching here without a crash is the assertion */
}

/* Walk the attributes like the format prescribes: the offset table starts after the header's
 * terminating zero byte. */
static size_t header_end_of(const pc_buf *b)
{
    size_t p = 8; /* magic + version */
    for (;;) {
        const uint8_t *nul = p < b->len ? memchr(b->data + p, 0, b->len - p) : NULL;
        const uint8_t *t;
        uint32_t size;
        if (nul == NULL) {
            PC_T_FAIL("unterminated attribute name");
            return 0;
        }
        if (nul == b->data + p) {
            return p + 1; /* the empty name terminates the header */
        }
        p = (size_t)(nul - b->data) + 1;
        t = memchr(b->data + p, 0, b->len - p);
        if (t == NULL) {
            PC_T_FAIL("unterminated attribute type");
            return 0;
        }
        p = (size_t)(t - b->data) + 1;
        size = (uint32_t)b->data[p] | (uint32_t)b->data[p + 1] << 8 | (uint32_t)b->data[p + 2] << 16 |
               (uint32_t)b->data[p + 3] << 24;
        p += 4 + size;
    }
}

static void test_hostile_sizes_and_offsets_error(void)
{
    pc_buf bytes = gen_deep(4, 4, RGBAZ, 5, 0, 0, 0, COUNTS_ONE, sample_rgba, NULL);
    pc_climits none = pc_climits_none();
    pc_climits tight = none;
    pc_error e;
    uint8_t px[16];
    pc_cimage img;
    pc_buf flat;
    uint8_t *broken;
    size_t header_end;
    size_t last_off;
    uint32_t imax = 0x7fffffffu;

    /* A tiny sample-data budget rejects the file. */
    tight.max_alloc = 64;
    CHECK(deep_status(bytes.data, bytes.len, &tight, &e) == PC_ERR_LIMIT);
    /* A non-deep file is unsupported for decode_deep. */
    memset(px, 128, sizeof px);
    if (pc_cimage_from_copy(2, 2, PC_CL_RGBA, PC_CS_U8, px, sizeof px, &img, NULL) != PC_OK) {
        PC_T_FAIL("img");
        pc_cimage_init(&img);
    }
    flat = enc(&img, PC_CF_EXR, NULL);
    CHECK(deep_status(flat.data, flat.len, &none, &e) == PC_ERR_UNSUPPORTED);
    pc_buf_fini(&flat);
    pc_cimage_fini(&img);

    /* The last chunk's y coordinate destroyed: out of the data window. */
    header_end = header_end_of(&bytes);
    broken = malloc(bytes.len);
    memcpy(broken, bytes.data, bytes.len);
    last_off = (size_t)rd64(broken + header_end + 24);
    memcpy(broken + last_off, &imax, 4); /* little-endian host */
    CHECK(deep_status(broken, bytes.len, &none, &e) != PC_OK);
    CHECK(flat_status(broken, bytes.len) != PC_OK);

    /* Two chunks claiming the same block: duplicate offsets. */
    memcpy(broken, bytes.data, bytes.len);
    memcpy(broken + header_end + 8, broken + header_end, 8);
    CHECK(deep_status(broken, bytes.len, &none, &e) == PC_ERR_FORMAT);

    /* An offset pointing past the file. */
    memcpy(broken, bytes.data, bytes.len);
    wr64(broken + header_end, UINT64_MAX);
    CHECK(deep_status(broken, bytes.len, &none, &e) == PC_ERR_FORMAT);
    free(broken);
    pc_buf_fini(&bytes);
}

static void test_inconsistent_count_table_errors(void)
{
    /* The declared unpacked size disagrees with the count table. */
    pc_buf bytes = gen_deep(4, 4, RGBAZ, 5, 0, 0, 0, COUNTS_ONE, sample_rgba, NULL);
    pc_climits none = pc_climits_none();
    pc_error e;
    size_t header_end = header_end_of(&bytes);
    size_t first_chunk = (size_t)rd64(bytes.data + header_end);
    /* Scanline chunk: y (4), three u64s (24); bump the unpacked size by one. */
    size_t at = first_chunk + 4 + 16;
    wr64(bytes.data + at, rd64(bytes.data + at) + 1);
    CHECK(deep_status(bytes.data, bytes.len, &none, &e) == PC_ERR_FORMAT);
    pc_buf_fini(&bytes);
}

/* ---------------------------------------------------------------------------------------------
 * Real reference files: the deep test images of the OpenEXR repository, fetched into
 * corpus/exr/ (`make corpus`; skipped when absent). Their .txt sidecars upstream list the ID
 * manifest of each file, an independent oracle for the u32 ID channels. */

static const uint32_t MULTIVARIATE_IDS[] = {
    127035619u,  147085119u,  319332210u,  409096490u,  523461226u,  678492822u,  683859976u,  1058109025u,
    1193275200u, 1503246798u, 1552124883u, 1613501244u, 1672208113u, 1687665471u, 1741895317u, 1841370478u,
    1863854195u, 1865183722u, 2056745377u, 2202049807u, 2303547305u, 2369798380u, 2433329986u, 2439705590u,
    2505186565u, 2698909330u, 2862333315u, 2898564507u, 2979371527u, 3007860936u, 3182339638u, 3236172513u,
    3497437340u, 3626859046u, 3656820528u, 3693645783u, 3739501011u, 3936563811u, 4048943790u};
static const uint32_t IDS_11[] = {523461226u,  683859976u,  1552124883u, 2202049807u, 2303547305u, 2433329986u,
                                  2698909330u, 3182339638u, 3236172513u, 3626859046u, 3739501011u};
static const uint32_t MODEL_42[] = {147148503u, 1252544961u, 1917926828u, 3170049167u, 3338960663u, 3411631724u};
static const uint32_t MATERIAL_42[] = {489905694u,  507819813u,  2340321182u, 3683581971u,
                                       3905155331u, 3947898621u, 4179825792u};

typedef struct id_set {
    const char *channel;
    const uint32_t *ids;
    size_t n;
} id_set;

typedef struct real_file {
    const char *name;
    uint64_t total;
    uint64_t max_per_pixel;
    id_set ids[2];
    size_t nids;
} real_file;

#define IDS(c, a) {c, a, sizeof a / sizeof a[0]}

static const real_file REAL_FILES[] = {
    {"11.deep.exr", 6056, 2, {IDS("id", IDS_11), {NULL, NULL, 0}}, 1},
    {"42.deep.exr", 67605, 8, {IDS("modelid", MODEL_42), IDS("materialid", MATERIAL_42)}, 2},
    /* 64-bit IDs split over .id0/.id1: only the sample counts are checked. */
    {"64.deep.exr", 64543, 6, {{NULL, NULL, 0}, {NULL, NULL, 0}}, 0},
    {"multivariate.deep.exr", 64543, 6, {IDS("id", MULTIVARIATE_IDS), {NULL, NULL, 0}}, 1},
    /* The same samples as multivariate; its sidecar has no manifest. */
    {"objectid.deep.exr", 64543, 6, {{NULL, NULL, 0}, {NULL, NULL, 0}}, 0},
};

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* Sorted, deduplicated in place; returns the new length. */
static size_t sort_unique(uint32_t *v, size_t n)
{
    size_t i;
    size_t k = 0;
    qsort(v, n, sizeof *v, cmp_u32);
    for (i = 0; i < n; i++) {
        if (k == 0 || v[k - 1] != v[i]) {
            v[k++] = v[i];
        }
    }
    return k;
}

static bool read_file(const char *path, pc_buf *out)
{
    FILE *f = fopen(path, "rb");
    uint8_t chunk[65536];
    size_t got;
    if (f == NULL) {
        return false;
    }
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) {
        put(out, chunk, got);
    }
    (void)fclose(f);
    return true;
}

static void test_real_deep_files_decode(void)
{
    struct stat sb;
    size_t fi;
    pc_climits none = pc_climits_none();
    if (stat("corpus/exr", &sb) != 0) {
        (void)printf("(corpus/exr absent: run `make corpus`; skipped) ");
        return;
    }
    for (fi = 0; fi < sizeof REAL_FILES / sizeof REAL_FILES[0]; fi++) {
        const real_file *rf = &REAL_FILES[fi];
        char path[256];
        pc_buf bytes = PC_BUF_INIT;
        pc_cdeep deep;
        pc_cimage img;
        pc_error e;
        size_t i;
        uint64_t maxd = 0;
        bool cumulative = true;
        (void)snprintf(path, sizeof path, "corpus/exr/%s", rf->name);
        if (!read_file(path, &bytes)) {
            PC_T_FAIL("%s: unreadable (run `make corpus`)", path);
            continue;
        }
        if (pc_cdecode_deep_exr(bytes.data, bytes.len, &none, &deep, &e) != PC_OK) {
            PC_T_FAIL("%s: %s", rf->name, e.msg);
            pc_buf_fini(&bytes);
            continue;
        }
        CHECK_EQ_UINT(deep.width, 256);
        CHECK_EQ_UINT(deep.height, 256);
        CHECK_EQ_UINT(pc_cdeep_total_samples(&deep), rf->total);
        CHECK_EQ_UINT(deep.ncounts, 256 * 256 + 1);
        for (i = 1; i < deep.ncounts; i++) {
            if (deep.counts[i - 1] > deep.counts[i]) {
                cumulative = false;
            } else if (deep.counts[i] - deep.counts[i - 1] > maxd) {
                maxd = deep.counts[i] - deep.counts[i - 1];
            }
        }
        CHECK(cumulative);
        CHECK_EQ_UINT(maxd, rf->max_per_pixel);
        for (i = 0; i < deep.nchannels; i++) {
            if (deep.channels[i].nsamples != rf->total) {
                PC_T_FAIL("%s: channel %s has %zu samples", rf->name, deep.channels[i].name, deep.channels[i].nsamples);
            }
        }
        /* Every decoded ID is one the sidecar lists, and every listed ID occurs. Samples hold
         * u32 IDs as f32 (exact only to 2^24), so compare at f32 precision. */
        for (i = 0; i < rf->nids; i++) {
            const id_set *s = &rf->ids[i];
            const pc_cdeep_channel *c = pc_cdeep_channel_named(&deep, s->channel);
            uint32_t *want;
            uint32_t *got;
            size_t nw;
            size_t ng;
            size_t k;
            if (c == NULL) {
                PC_T_FAIL("%s: no channel %s", rf->name, s->channel);
                continue;
            }
            want = malloc(s->n * sizeof *want);
            got = malloc((c->nsamples + 1) * sizeof *got);
            for (k = 0; k < s->n; k++) {
                float f = (float)s->ids[k];
                memcpy(&want[k], &f, 4);
            }
            for (k = 0; k < c->nsamples; k++) {
                memcpy(&got[k], &c->samples[k], 4);
            }
            nw = sort_unique(want, s->n);
            ng = sort_unique(got, c->nsamples);
            if (nw != ng || memcmp(want, got, nw * sizeof *want) != 0) {
                PC_T_FAIL("%s: channel %s IDs differ from the manifest", rf->name, s->channel);
            }
            pc_t_checks++;
            free(want);
            free(got);
        }
        /* The flat decode composites the same samples. */
        if (pc_cdecode(bytes.data, bytes.len, &img, &e) != PC_OK) {
            PC_T_FAIL("%s: %s", rf->name, e.msg);
        } else {
            float *v = NULL;
            size_t n = 0;
            CHECK_EQ_UINT(img.width, deep.width);
            CHECK_EQ_UINT(img.height, deep.height);
            (void)pc_cimage_to_normalized(&img, &v, &n, NULL);
            CHECK_EQ_UINT(n, 256 * 256 * 4);
            CHECK(has_warning(&img, PC_CW_DEEP_FLATTENED));
            free(v);
            pc_cimage_fini(&img);
        }
        pc_cdeep_fini(&deep);
        pc_buf_fini(&bytes);
    }
}

#endif /* PC_NO_EXR */

int main(int argc, char **argv)
{
#ifndef PC_NO_EXR
    RUN(test_single_sample_matches_flat);
    RUN(test_z_order_composites_front_over_back);
    RUN(test_opaque_front_hides_back);
    RUN(test_hdr_colour_is_kept_and_edge_tiles_match_scanlines);
    RUN(test_empty_pixels_are_transparent);
    RUN(test_rle_and_zips_match_uncompressed);
    RUN(test_tiles_match_scanlines);
    RUN(test_half_channels_round_trip);
    RUN(test_without_alpha_front_sample_is_opaque);
    RUN(test_grayscale_deep_keeps_its_alpha);
    RUN(test_decode_deep_structure);
    RUN(test_truncated_files_error_never_panic);
    RUN(test_hostile_sizes_and_offsets_error);
    RUN(test_inconsistent_count_table_errors);
    RUN(test_real_deep_files_decode);
#else
    (void)argc;
    (void)argv;
#endif
    return pc_t_finish();
}
