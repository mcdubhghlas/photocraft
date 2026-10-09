/* Bit-exact parity with crates/raster: replays `rustref raster-vectors` (tools/rustref) and
 * compares FNV-1a hashes of every output. Expected values come from rustc 1.95. */
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "harness.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#define FNV0 0xcbf29ce484222325u
#define NFMT 9

static const uint64_t WANT[NFMT][4] = {
    {0x76592058991e7101u, 0x92fe83a8a3f54ba0u, 0xc9348ea36ad1b706u, 0x382a070707169576u},
    {0x28ec17fba9d7c465u, 0xf6877842e9d719d4u, 0x9f34520b2708b627u, 0x9f1f557581e7fa59u},
    {0xff3990dece4fdbdau, 0x3cf40bd33a5bc61cu, 0xcc259f4cbbda8cd2u, 0x04d3b16a9bb78092u},
    {0x6cc922203a6ce28eu, 0x2cdc77acc10edd40u, 0x53b3578acb26c3a2u, 0x9e32e4b01145469eu},
    {0x4ef17b2402c1e04fu, 0xfc3690847387faecu, 0x56d9d71859235395u, 0x2aeaf2e12fbb5e13u},
    {0x35fe848f49787579u, 0x72f4ef2e3238723cu, 0xbade7c5073f9e001u, 0x00cb796f94cf8f93u},
    {0x3787641d914baf8au, 0xd6df2dceafbfd76cu, 0x9369bd701c1f665du, 0xf8a5c7bfeb30e773u},
    {0x0d64e2d30954f95cu, 0x922ea118a06d0690u, 0x7362b133b4ed8229u, 0x5e6c4dc46bd0ccceu},
    {0xa9e14e269fe484eau, 0x96f275a9ca6b1214u, 0x3dc1e200e45d8ec1u, 0x48cb4e94a167a833u},
};

static void fnv(uint64_t *h, const void *data, size_t n)
{
    const uint8_t *b = data;
    size_t i;
    for (i = 0; i < n; i++) {
        *h ^= b[i];
        *h *= 0x100000001b3u;
    }
}

static void h_u64(uint64_t *h, uint64_t v)
{
    uint8_t b[8];
    int i;
    for (i = 0; i < 8; i++) {
        b[i] = (uint8_t)(v >> (8 * i));
    }
    fnv(h, b, 8);
}

/* FNV over the 4 little-endian bytes of each float's bits. */
static void h_f32le(uint64_t *h, const float *v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint32_t u;
        uint8_t b[4];
        memcpy(&u, &v[i], 4);
        b[0] = (uint8_t)u;
        b[1] = (uint8_t)(u >> 8);
        b[2] = (uint8_t)(u >> 16);
        b[3] = (uint8_t)(u >> 24);
        fnv(h, b, 4);
    }
}

static void h_rect(uint64_t *h, pc_rect r)
{
    int32_t v[4];
    int i;
    v[0] = r.x0;
    v[1] = r.y0;
    v[2] = r.x1;
    v[3] = r.y1;
    for (i = 0; i < 4; i++) {
        uint32_t u = (uint32_t)v[i];
        uint8_t b[4];
        b[0] = (uint8_t)u;
        b[1] = (uint8_t)(u >> 8);
        b[2] = (uint8_t)(u >> 16);
        b[3] = (uint8_t)(u >> 24);
        fnv(h, b, 4);
    }
}

typedef struct lcg {
    uint64_t s;
} lcg;

static float next(lcg *r)
{
    r->s = r->s * 6364136223846793005u + 1442695040888963407u;
    return (float)(r->s >> 40) / 16777216.0f;
}

static pc_pixel_format FMTS[NFMT];

static void init_formats(void)
{
    FMTS[0] = PC_PIXEL_FORMAT_RGBA8;
    FMTS[1] = PC_PIXEL_FORMAT_RGBA16;
    FMTS[2] = PC_PIXEL_FORMAT_RGBA32F;
    FMTS[3] = PC_PIXEL_FORMAT_GRAY8;
    FMTS[4] = PC_PIXEL_FORMAT_GRAYA8;
    FMTS[5] = PC_PIXEL_FORMAT_CMYKA8;
    FMTS[6] = (pc_pixel_format){PC_COLOR_MODE_LAB, PC_SAMPLE_TYPE_F32, true};
    FMTS[7] = (pc_pixel_format){PC_COLOR_MODE_LAB, PC_SAMPLE_TYPE_U16, false};
    FMTS[8] = (pc_pixel_format){PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U16, false};
}

static void hash_region(uint64_t *h, const pc_surface *s, pc_rect r)
{
    float *f = NULL;
    size_t n = 0;
    CHECK_EQ_INT(pc_surface_read_region(s, r, &f, &n, NULL), PC_OK);
    h_f32le(h, f, n);
    free(f);
}

static void hash_interleaved(uint64_t *h, const pc_surface *s, pc_rect r)
{
    uint8_t *b = NULL;
    size_t n = 0;
    CHECK_EQ_INT(pc_surface_to_interleaved(s, r, &b, &n, NULL), PC_OK);
    fnv(h, b, n);
    free(b);
}

static void run_format(size_t fi)
{
    static const int32_t pts[6][2] = {{-1, -1}, {0, 0}, {255, 255}, {256, 256}, {-257, 5}, {100000, -100000}};
    static const int32_t strided[7][3] = {{15, -520, 7}, {-1, -1, 1}, {30, 3, 64}, {500, 0, 9},
                                          {5, INT32_MAX - 1000, 7}, {39, -301, 300}, {0, 0, -5}};
    static const int32_t probes[6][2] = {{-1, -1}, {299, 39}, {300, 40}, {600, 600}, {100000, -100000},
                                         {INT32_MIN, INT32_MAX}};
    static const int32_t moves[6][2] = {{0, 0}, {256, -512}, {1, 0}, {-3, 7}, {300, -257}, {-1000, 999}};
    pc_pixel_format fmt = FMTS[fi];
    size_t n = pc_pixel_format_channels(fmt);
    uint64_t h = FNV0;
    uint64_t ht = FNV0;
    uint64_t hc = FNV0;
    uint64_t hp = FNV0;
    lcg r;
    float quarter[PC_RASTER_MAX_CHANNELS] = {0.25f, 0.25f, 0.25f, 0.25f, 0.25f};
    float px[PC_RASTER_MAX_CHANNELS];
    pc_surface s;
    pc_surface q;
    pc_rect wr = pc_rect_make(-300, -20, 300, 40);
    pc_rect big = pc_rect_make(-310, -30, 910, 710);
    pc_rect rr = pc_rect_make(-300, -20, 600, 300);
    pc_rect content;
    size_t len = (size_t)600 * 60 * n;
    size_t npx = (size_t)900 * 320;
    float *data = malloc(len * sizeof *data);
    float(*rgba)[4] = malloc(npx * sizeof *rgba);
    uint8_t(*rgba8)[4] = malloc(npx * sizeof *rgba8);
    pc_tile_entry *tiles = NULL;
    size_t ntiles = 0;
    size_t i;
    size_t k;
    r.s = 100 + fi;
    if (data == NULL || rgba == NULL || rgba8 == NULL) {
        PC_T_FAIL("%s", "out of memory");
        goto out;
    }
    pc_surface_init_default(&s, fmt, quarter, n);
    for (i = 0; i < len; i++) {
        data[i] = next(&r) * 1.2f - 0.1f;
    }
    CHECK_EQ_INT(pc_surface_write_region(&s, wr, data, len, NULL), PC_OK);
    for (i = 0; i < n; i++) {
        px[i] = next(&r);
    }
    CHECK_EQ_INT(pc_surface_fill_rect(&s, pc_rect_make(500, 100, 900, 700), px, n, NULL), PC_OK);
    for (i = 0; i < 6; i++) {
        float p[PC_RASTER_MAX_CHANNELS];
        for (k = 0; k < n; k++) {
            p[k] = next(&r) * 1.4f - 0.2f;
        }
        CHECK_EQ_INT(pc_surface_write_pixel(&s, pts[i][0], pts[i][1], p, n, NULL), PC_OK);
    }
    hash_region(&h, &s, big);
    hash_interleaved(&h, &s, big);
    h_rect(&h, pc_surface_content_bounds(&s));
    h_rect(&h, pc_surface_tile_bounds(&s));
    h_u64(&h, pc_surface_tile_count(&s));
    CHECK_EQ_INT(pc_surface_read_rgba_into(&s, rr, rgba, npx, NULL), PC_OK);
    h_f32le(&h, &rgba[0][0], npx * 4);
    CHECK_EQ_INT(pc_surface_read_rgba8_into(&s, rr, rgba8, npx, NULL), PC_OK);
    fnv(&h, rgba8, npx * 4);
    for (i = 0; i < 7; i++) {
        float out[300];
        pc_surface_sample_row_strided(&s, strided[i][0], strided[i][1], strided[i][2], n - 1, out, 300);
        h_f32le(&h, out, 300);
    }
    for (i = 0; i < 6; i++) {
        float p[PC_RASTER_MAX_CHANNELS];
        float c4[4];
        float c0;
        pc_surface_read_pixel(&s, probes[i][0], probes[i][1], p);
        h_f32le(&h, p, n);
        pc_surface_rgba(&s, probes[i][0], probes[i][1], c4);
        h_f32le(&h, c4, 4);
        c0 = pc_surface_sample_channel(&s, probes[i][0], probes[i][1], 0);
        h_f32le(&h, &c0, 1);
    }
    CHECK_EQ_UINT(h, WANT[fi][0]);

    content = pc_surface_content_bounds(&s);
    for (i = 0; i < 6; i++) {
        pc_surface t;
        if (pc_surface_translated(&s, moves[i][0], moves[i][1], content, &t, NULL) != PC_OK) {
            PC_T_FAIL("translated %zu", i);
            continue;
        }
        hash_interleaved(&ht, &t, pc_rect_translate(wr, moves[i][0], moves[i][1]));
        h_u64(&ht, pc_surface_tile_count(&t));
        h_rect(&ht, pc_surface_content_bounds(&t));
        pc_surface_fini(&t);
    }
    CHECK_EQ_UINT(ht, WANT[fi][1]);

    for (i = 0; i < NFMT; i++) {
        pc_surface c;
        float dp[PC_RASTER_MAX_CHANNELS];
        float cp[PC_RASTER_MAX_CHANNELS];
        float c4[4];
        size_t m;
        if (pc_surface_convert(&s, FMTS[i], &c, NULL) != PC_OK) {
            PC_T_FAIL("convert %zu", i);
            continue;
        }
        hash_region(&hc, &c, pc_rect_make(-20, -20, 300, 60));
        pc_surface_default_pixel(&c, dp);
        h_f32le(&hc, dp, pc_surface_channels(&c));
        h_u64(&hc, pc_surface_tile_count(&c));
        m = pc_convert_pixel(fmt, FMTS[i], px, n, cp);
        h_f32le(&hc, cp, m);
        pc_to_rgba(fmt, px, n, c4);
        h_f32le(&hc, c4, 4);
        pc_surface_fini(&c);
    }
    CHECK_EQ_UINT(hc, WANT[fi][2]);

    if (pc_surface_clone(&s, &q, NULL) == PC_OK) {
        CHECK_EQ_INT(pc_surface_take_tiles(&q, pc_rect_make(-10, -10, 600, 300), &tiles, &ntiles, NULL), PC_OK);
        h_u64(&hp, ntiles);
        for (i = 0; i < ntiles; i++) {
            uint8_t *b = pc_raster_tile_make_mut(&tiles[i].tile);
            size_t blen = pc_raster_tile_len(tiles[i].tile);
            uint32_t ax = tiles[i].coord.tx < 0 ? 0u - (uint32_t)tiles[i].coord.tx : (uint32_t)tiles[i].coord.tx;
            uint32_t ay = tiles[i].coord.ty < 0 ? 0u - (uint32_t)tiles[i].coord.ty : (uint32_t)tiles[i].coord.ty;
            size_t at = (size_t)(ax * 7u + ay * 13u) % blen;
            if (b == NULL) {
                PC_T_FAIL("%s", "make_mut");
                continue;
            }
            b[at] = (uint8_t)(b[at] + 1);
        }
        CHECK_EQ_INT(pc_surface_put_tiles(&q, tiles, ntiles, NULL), PC_OK);
        free(tiles);
        hash_interleaved(&hp, &q, pc_rect_make(-260, -20, 600, 300));
        CHECK_EQ_INT(pc_surface_fill_rect(&q, wr, quarter, n, NULL), PC_OK);
        pc_surface_prune(&q);
        h_u64(&hp, pc_surface_tile_count(&q));
        h_rect(&hp, pc_surface_content_bounds(&q));
        pc_surface_fini(&q);
    }
    CHECK_EQ_UINT(hp, WANT[fi][3]);
    pc_surface_fini(&s);
out:
    free(data);
    free(rgba);
    free(rgba8);
}

static void test_all_formats(void)
{
    size_t fi;
    init_formats();
    for (fi = 0; fi < NFMT; fi++) {
        run_format(fi);
    }
}

int main(int argc, char **argv)
{
    RUN(test_all_formats);
    return pc_t_finish();
}
