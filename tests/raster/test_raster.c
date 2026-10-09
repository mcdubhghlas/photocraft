/* Ports the unit tests of crates/raster/src/{lib,interrupt}.rs (the two proptests become seeded
 * random loops), plus C-specific checks: the tile table under random insert/remove, limits and
 * length validation where the reference panics. */
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "harness.h"
#include "raster/interrupt.h"
#include "raster/pixel.h"
#include "raster/surface.h"

static void read_px(const pc_surface *s, int32_t x, int32_t y, float *out)
{
    pc_surface_read_pixel(s, x, y, out);
}

static bool px_eq(const float *a, const float *b, size_t n)
{
    return memcmp(a, b, n * sizeof *a) == 0;
}

static float *region(const pc_surface *s, pc_rect r, size_t *n)
{
    float *f = NULL;
    if (pc_surface_read_region(s, r, &f, n, NULL) != PC_OK) {
        PC_T_FAIL("%s", "read_region");
    }
    return f;
}

/* ---- lib.rs ---- */

static void test_from_rgba_into_matches_from_rgba(void)
{
    static const float in[4] = {0.2f, 0.5f, 0.7f, 0.4f};
    pc_pixel_format fmts[5];
    int i;
    fmts[0] = PC_PIXEL_FORMAT_RGBA8;
    fmts[1] = PC_PIXEL_FORMAT_GRAY8;
    fmts[2] = PC_PIXEL_FORMAT_GRAYA8;
    fmts[3] = PC_PIXEL_FORMAT_CMYKA8;
    fmts[4] = (pc_pixel_format){PC_COLOR_MODE_LAB, PC_SAMPLE_TYPE_F32, true};
    for (i = 0; i < 5; i++) {
        float out[PC_RASTER_MAX_CHANNELS];
        CHECK_EQ_UINT(pc_from_rgba(fmts[i], in, out), pc_pixel_format_channels(fmts[i]));
    }
}

static void test_zero_alloc_accessors_match_read_region(void)
{
    static const int32_t pts[5][2] = {{-299, -19}, {0, 0}, {255, 39}, {256, 10}, {299, 0}};
    pc_pixel_format fmts[6];
    int f;
    fmts[0] = PC_PIXEL_FORMAT_RGBA8;
    fmts[1] = PC_PIXEL_FORMAT_RGBA16;
    fmts[2] = PC_PIXEL_FORMAT_RGBA32F;
    fmts[3] = PC_PIXEL_FORMAT_GRAYA8;
    fmts[4] = PC_PIXEL_FORMAT_CMYKA8;
    fmts[5] = PC_PIXEL_FORMAT_GRAY8;
    for (f = 0; f < 6; f++) {
        pc_pixel_format fmt = fmts[f];
        size_t n = pc_pixel_format_channels(fmt);
        float quarter[PC_RASTER_MAX_CHANNELS] = {0.25f, 0.25f, 0.25f, 0.25f, 0.25f};
        pc_rect r = pc_rect_make(-300, -20, 300, 40);
        size_t npx = 600 * 60;
        pc_surface s;
        float *reg;
        float *into;
        float(*rgba)[4];
        size_t len = 0;
        size_t i;
        int bad = 0;
        pc_surface_init_default(&s, fmt, quarter, n);
        for (i = 0; i < 5; i++) {
            float px[PC_RASTER_MAX_CHANNELS];
            size_t c;
            for (c = 0; c < n; c++) {
                px[c] = (float)((i * 3 + c) % 5) / 4.0f;
            }
            CHECK_EQ_INT(pc_surface_write_pixel(&s, pts[i][0], pts[i][1], px, n, NULL), PC_OK);
        }
        reg = region(&s, r, &len);
        into = malloc(len * sizeof *into);
        rgba = malloc(npx * sizeof *rgba);
        if (reg == NULL || into == NULL || rgba == NULL) {
            PC_T_FAIL("%s", "out of memory");
            free(reg);
            free(into);
            free(rgba);
            pc_surface_fini(&s);
            continue;
        }
        CHECK_EQ_INT(pc_surface_read_region_into(&s, r, into, len, NULL), PC_OK);
        CHECK_EQ_MEM(reg, into, len * sizeof *reg);
        CHECK_EQ_INT(pc_surface_read_rgba_into(&s, r, rgba, npx, NULL), PC_OK);
        for (i = 0; i < npx; i++) {
            float want[4];
            int c;
            int32_t x = r.x0 + (int32_t)(i % 600);
            int32_t y = r.y0 + (int32_t)(i / 600);
            pc_to_rgba(fmt, reg + i * n, n, want);
            for (c = 0; c < 4; c++) {
                bad += fabsf(rgba[i][c] - want[c]) >= 1e-6f;
            }
            bad += pc_surface_sample_channel(&s, x, y, 0) != reg[i * n];
        }
        CHECK_EQ_INT(bad, 0);
        CHECK(pc_surface_has_tiles_in(&s, pc_rect_make(0, 0, 1, 1)));
        CHECK(!pc_surface_has_tiles_in(&s, pc_rect_make(1000, 1000, 1001, 1001)));
        free(reg);
        free(into);
        free(rgba);
        pc_surface_fini(&s);
    }
}

static void test_fill_rect_shares_covered_tiles_and_edits_stay_local(void)
{
    /* A 300000^2 background used to materialise ~360 GB of tiles. */
    static const float orange[4] = {1.0f, 0.5f, 0.0f, 1.0f};
    static const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    const float want[4] = {1.0f, 128.0f / 255.0f, 0.0f, 1.0f};
    static const float zero[4] = {0, 0, 0, 0};
    pc_surface s;
    float p[4];
    int32_t edge = 300000 / PC_TILE_SIZE;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    CHECK_EQ_INT(pc_surface_fill_rect(&s, pc_rect_make(0, 0, 300000, 300000), orange, 4, NULL), PC_OK);
    CHECK(pc_surface_tile(&s, (pc_tile){0, 0}) == pc_surface_tile(&s, (pc_tile){1000, 900}));
    /* 300000 is not a multiple of 256: the edge tile is partial and stays its own. */
    CHECK(pc_surface_tile(&s, (pc_tile){0, 0}) != pc_surface_tile(&s, (pc_tile){edge, 0}));
    read_px(&s, 299999, 299999, p);
    CHECK(px_eq(p, want, 4));
    read_px(&s, 300000, 0, p);
    CHECK(px_eq(p, zero, 4));
    CHECK_EQ_INT(pc_surface_write_pixel(&s, 5, 5, blue, 4, NULL), PC_OK);
    read_px(&s, 5, 5, p);
    CHECK(px_eq(p, blue, 4));
    read_px(&s, 300, 5, p);
    CHECK(px_eq(p, want, 4));
    CHECK(pc_surface_tile(&s, (pc_tile){0, 0}) != pc_surface_tile(&s, (pc_tile){1, 0}));
    pc_surface_fini(&s);
}

static void test_empty_surface_reads_default(void)
{
    static const float one[1] = {1.0f};
    pc_surface s;
    pc_surface m;
    float p[4];
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    read_px(&s, 10, -500, p);
    CHECK(p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f && p[3] == 0.0f);
    pc_surface_init_default(&m, PC_PIXEL_FORMAT_GRAY8, one, 1);
    read_px(&m, 123456, -9, p);
    CHECK(p[0] == 1.0f);
    CHECK_EQ_UINT(pc_surface_tile_count(&m), 0);
    pc_surface_fini(&s);
    pc_surface_fini(&m);
}

static void test_write_read_pixel_every_depth(void)
{
    static const float in[4] = {0.2f, 0.4f, 0.6f, 1.0f};
    unsigned st;
    for (st = 0; st < PC_SAMPLE_TYPE_COUNT; st++) {
        pc_pixel_format f = PC_PIXEL_FORMAT_RGBA8;
        pc_surface s;
        float p[4];
        int i;
        f.sample = (pc_sample_type)st;
        pc_surface_init(&s, f);
        CHECK_EQ_INT(pc_surface_write_pixel(&s, -3, 700, in, 4, NULL), PC_OK);
        read_px(&s, -3, 700, p);
        for (i = 0; i < 4; i++) {
            CHECK(fabsf(p[i] - in[i]) <= 1.0f / 255.0f);
        }
        CHECK_EQ_UINT(pc_surface_tile_count(&s), 1);
        CHECK(pc_rect_eq(pc_surface_tile_bounds(&s), pc_tile_rect(pc_tile_containing(-3, 700))));
        pc_surface_fini(&s);
    }
}

static void test_copy_on_write_shares_untouched_tiles(void)
{
    static const float red[4] = {1, 0, 0, 1};
    static const float green[4] = {0, 1, 0, 1};
    pc_surface a;
    pc_surface b;
    float p[4];
    size_t cur = 0;
    pc_tile c;
    const pc_raster_tile *t;
    int shared = 0;
    pc_surface_init(&a, PC_PIXEL_FORMAT_RGBA8);
    CHECK_EQ_INT(pc_surface_fill_rect(&a, pc_rect_make(0, 0, 512, 512), red, 4, NULL), PC_OK);
    CHECK_EQ_INT(pc_surface_clone(&a, &b, NULL), PC_OK);
    CHECK_EQ_INT(pc_surface_write_pixel(&b, 10, 10, green, 4, NULL), PC_OK);
    read_px(&a, 10, 10, p);
    CHECK(px_eq(p, red, 4)); /* a unchanged */
    read_px(&b, 10, 10, p);
    CHECK(px_eq(p, green, 4));
    while (pc_surface_next_tile(&a, &cur, &c, &t)) { /* only tile (0, 0) was copied */
        shared += t == pc_surface_tile(&b, c);
    }
    CHECK_EQ_INT(shared, 3);
    CHECK(!pc_surface_eq(&a, &b));
    pc_surface_fini(&a);
    pc_surface_fini(&b);
}

static void test_content_bounds_and_prune(void)
{
    static const float white[4] = {1, 1, 1, 1};
    static const float black[4] = {0, 0, 0, 1};
    static const float clear[4] = {0, 0, 0, 0};
    pc_surface s;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    CHECK_EQ_INT(pc_surface_fill_rect(&s, pc_rect_make(300, 10, 310, 20), white, 4, NULL), PC_OK);
    CHECK_EQ_INT(pc_surface_write_pixel(&s, -5, -5, black, 4, NULL), PC_OK);
    CHECK(pc_rect_eq(pc_surface_content_bounds(&s), pc_rect_make(-5, -5, 310, 20)));
    /* erase the single pixel: its tile becomes default and is pruned */
    CHECK_EQ_INT(pc_surface_write_pixel(&s, -5, -5, clear, 4, NULL), PC_OK);
    pc_surface_prune(&s);
    CHECK_EQ_UINT(pc_surface_tile_count(&s), 1);
    CHECK(pc_rect_eq(pc_surface_content_bounds(&s), pc_rect_make(300, 10, 310, 20)));
    pc_surface_fini(&s);
}

static void test_interleaved_roundtrip_crossing_tiles(void)
{
    pc_rect r = pc_rect_make(-20, 250, 280, 262);
    size_t bpp = pc_pixel_format_bytes_per_pixel(PC_PIXEL_FORMAT_RGBA16);
    size_t len = (size_t)300 * 12 * bpp;
    uint8_t *bytes = malloc(len);
    uint8_t *back = NULL;
    size_t blen = 0;
    pc_surface s;
    size_t i;
    if (bytes == NULL) {
        return;
    }
    for (i = 0; i < len; i++) {
        bytes[i] = (uint8_t)(i * 31 % 251);
    }
    CHECK_EQ_INT(pc_surface_from_interleaved(PC_PIXEL_FORMAT_RGBA16, r, bytes, len, &s, NULL), PC_OK);
    CHECK_EQ_INT(pc_surface_to_interleaved(&s, r, &back, &blen, NULL), PC_OK);
    CHECK(blen == len && back != NULL && memcmp(back, bytes, len) == 0);
    CHECK_EQ_UINT(pc_surface_tile_count(&s), 6); /* x tiles -1..=1, y tiles 0..=1 */
    /* The reference asserts on a length mismatch; C reports it. */
    CHECK_EQ_INT(pc_surface_write_interleaved(&s, r, bytes, len - 1, NULL), PC_ERR_INVALID);
    free(bytes);
    free(back);
    pc_surface_fini(&s);
}

static void test_region_outside_reads_default(void)
{
    static const float one[1] = {1.0f};
    pc_surface s;
    float *v;
    size_t n = 0;
    size_t i;
    pc_surface_init_default(&s, PC_PIXEL_FORMAT_GRAY8, one, 1);
    v = region(&s, pc_rect_make(0, 0, 3, 3), &n);
    CHECK_EQ_UINT(n, 9);
    for (i = 0; v != NULL && i < n; i++) {
        CHECK(v[i] == 1.0f);
    }
    free(v);
    pc_surface_fini(&s);
}

static void test_convert_depths_and_models(void)
{
    static const float white[4] = {1, 1, 1, 1};
    pc_surface s;
    pc_surface g;
    pc_surface f;
    pc_surface c;
    pc_surface back;
    float p[5];
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    CHECK_EQ_INT(pc_surface_write_pixel(&s, 1, 1, white, 4, NULL), PC_OK);
    CHECK_EQ_INT(pc_surface_convert(&s, PC_PIXEL_FORMAT_GRAYA8, &g, NULL), PC_OK);
    read_px(&g, 1, 1, p);
    CHECK(fabsf(p[0] - 1.0f) < 1e-2f);
    CHECK_EQ_INT(pc_surface_convert(&s, PC_PIXEL_FORMAT_RGBA32F, &f, NULL), PC_OK);
    read_px(&f, 1, 1, p);
    CHECK(px_eq(p, white, 4));
    CHECK_EQ_INT(pc_surface_convert(&s, PC_PIXEL_FORMAT_CMYKA8, &c, NULL), PC_OK);
    read_px(&c, 1, 1, p);
    CHECK(p[3] < 0.01f && fabsf(p[4] - 1.0f) < 1e-6f);
    CHECK_EQ_INT(pc_surface_convert(&c, PC_PIXEL_FORMAT_RGBA8, &back, NULL), PC_OK);
    read_px(&back, 1, 1, p);
    CHECK(px_eq(p, white, 4));
    pc_surface_fini(&s);
    pc_surface_fini(&g);
    pc_surface_fini(&f);
    pc_surface_fini(&c);
    pc_surface_fini(&back);
}

static void test_rgba_from_gray_and_cmyk(void)
{
    static const float ga[2] = {0.5f, 1.0f};
    pc_surface g;
    float v[4];
    pc_surface_init(&g, PC_PIXEL_FORMAT_GRAYA8);
    CHECK_EQ_INT(pc_surface_write_pixel(&g, 0, 0, ga, 2, NULL), PC_OK);
    pc_surface_rgba(&g, 0, 0, v);
    CHECK(fabsf(v[0] - v[2]) < 1e-6f && fabsf(v[3] - 1.0f) < 1e-6f);
    pc_surface_fini(&g);
}

static void test_translated_matches_reading_the_moved_pixels(void)
{
    static const int32_t moves[6][2] = {{0, 0}, {256, -512}, {1, 0}, {-3, 7}, {300, -257}, {-1000, 999}};
    pc_pixel_format fmts[4];
    int f;
    fmts[0] = PC_PIXEL_FORMAT_RGBA8;
    fmts[1] = PC_PIXEL_FORMAT_RGBA16;
    fmts[2] = PC_PIXEL_FORMAT_RGBA32F;
    fmts[3] = PC_PIXEL_FORMAT_GRAY8;
    for (f = 0; f < 4; f++) {
        pc_pixel_format fmt = fmts[f];
        size_t n = pc_pixel_format_channels(fmt);
        float quarter[PC_RASTER_MAX_CHANNELS] = {0.25f, 0.25f, 0.25f, 0.25f, 0.25f};
        pc_rect r = pc_rect_make(-37, 200, 530, 300);
        size_t len = (size_t)567 * 100 * n;
        float *v = malloc(len * sizeof *v);
        pc_surface s;
        pc_surface t;
        pc_surface padded;
        pc_surface p1;
        pc_surface p2;
        float dp[PC_RASTER_MAX_CHANNELS];
        float tdp[PC_RASTER_MAX_CHANNELS];
        size_t i;
        size_t cur = 0;
        size_t cur2 = 0;
        pc_tile ca;
        pc_tile cb;
        const pc_raster_tile *ta;
        const pc_raster_tile *tb;
        bool all_shared = true;
        if (v == NULL) {
            continue;
        }
        pc_surface_init_default(&s, fmt, quarter, n);
        for (i = 0; i < len; i++) {
            v[i] = (float)(i % 251) / 250.0f;
        }
        CHECK_EQ_INT(pc_surface_write_region(&s, r, v, len, NULL), PC_OK);
        for (i = 0; i < 6; i++) {
            int32_t dx = moves[i][0];
            int32_t dy = moves[i][1];
            pc_rect moved = pc_rect_inflate(pc_rect_translate(r, dx, dy), 3);
            float *a;
            float *b;
            size_t na = 0;
            size_t nb = 0;
            uint8_t *ia = NULL;
            uint8_t *ib = NULL;
            size_t la = 0;
            size_t lb = 0;
            if (pc_surface_translated(&s, dx, dy, pc_surface_content_bounds(&s), &t, NULL) != PC_OK) {
                PC_T_FAIL("%s", "translated");
                continue;
            }
            a = region(&t, moved, &na);
            b = region(&s, pc_rect_translate(moved, -dx, -dy), &nb);
            CHECK(na == nb && a != NULL && b != NULL && memcmp(a, b, na * sizeof *a) == 0);
            pc_surface_default_pixel(&t, tdp);
            pc_surface_default_pixel(&s, dp);
            CHECK(px_eq(tdp, dp, n));
            CHECK_EQ_INT(pc_surface_to_interleaved(&t, pc_rect_translate(r, dx, dy), &ia, &la, NULL), PC_OK);
            CHECK_EQ_INT(pc_surface_to_interleaved(&s, r, &ib, &lb, NULL), PC_OK);
            CHECK(la == lb && ia != NULL && ib != NULL && memcmp(ia, ib, la) == 0); /* exact copy */
            free(a);
            free(b);
            free(ia);
            free(ib);
            pc_surface_fini(&t);
        }
        /* Whole tiles: the tiles are shared, not copied. */
        if (pc_surface_translated(&s, 512, -256, pc_surface_content_bounds(&s), &t, NULL) == PC_OK) {
            CHECK_EQ_UINT(pc_surface_tile_count(&t), pc_surface_tile_count(&s));
            while (pc_surface_next_tile(&s, &cur, &ca, &ta)) {
                pc_tile moved_c = {ca.tx + 2, ca.ty - 1};
                all_shared = all_shared && pc_surface_tile(&t, moved_c) == ta;
            }
            CHECK(all_shared);
            (void)cur2;
            (void)cb;
            (void)tb;
            pc_surface_fini(&t);
        }
        /* Tiles without content (allocated, default pixels) are left behind. */
        CHECK_EQ_INT(pc_surface_clone(&s, &padded, NULL), PC_OK);
        CHECK_EQ_INT(pc_surface_fill_rect(&padded, pc_rect_make(2000, 2000, 2100, 2100), dp, n, NULL), PC_OK);
        CHECK_EQ_INT(pc_surface_translated(&padded, 5, 5, pc_surface_content_bounds(&s), &p1, NULL), PC_OK);
        CHECK_EQ_INT(pc_surface_translated(&s, 5, 5, pc_surface_content_bounds(&s), &p2, NULL), PC_OK);
        CHECK_EQ_UINT(pc_surface_tile_count(&p1), pc_surface_tile_count(&p2));
        pc_surface_fini(&p1);
        pc_surface_fini(&p2);
        pc_surface_fini(&padded);
        pc_surface_fini(&s);
        free(v);
    }
    /* Offsets at the limits don't crash. */
    {
        static const float one[1] = {1.0f};
        pc_surface s;
        pc_surface t;
        pc_rect all = pc_rect_make(INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX);
        pc_surface_init(&s, PC_PIXEL_FORMAT_GRAY8);
        CHECK_EQ_INT(pc_surface_write_pixel(&s, 5, 5, one, 1, NULL), PC_OK);
        if (pc_surface_translated(&s, INT32_MAX, INT32_MIN, all, &t, NULL) == PC_OK) {
            pc_surface_fini(&t);
        }
        if (pc_surface_translated(&s, INT32_MIN + 1, 3, all, &t, NULL) == PC_OK) {
            pc_surface_fini(&t);
        }
        pc_surface_fini(&s);
    }
}

static uint32_t mix(uint32_t i, uint32_t seed)
{
    return i * 2654435761u + seed;
}

/* proptest region_write_read_roundtrip, as 300 seeded cases. */
static void test_region_write_read_roundtrip(void)
{
    uint64_t st = 12345;
    int k;
    int bad = 0;
    for (k = 0; k < 300; k++) {
        int32_t x;
        int32_t y;
        uint32_t w;
        uint32_t h;
        uint32_t seed;
        pc_rect r;
        size_t len;
        float *data;
        float *back;
        size_t nb = 0;
        size_t i;
        pc_surface s;
        st = st * 6364136223846793005u + 1442695040888963407u;
        x = (int32_t)((st >> 33) % 1200) - 600;
        y = (int32_t)((st >> 13) % 1200) - 600;
        st = st * 6364136223846793005u + 1442695040888963407u;
        w = (uint32_t)((st >> 33) % 89) + 1;
        h = (uint32_t)((st >> 20) % 89) + 1;
        seed = (uint32_t)(st >> 7);
        r = pc_rect_from_xywh(x, y, w, h);
        len = (size_t)w * h * 4;
        data = malloc(len * sizeof *data);
        if (data == NULL) {
            continue;
        }
        for (i = 0; i < len; i++) {
            data[i] = (float)(mix((uint32_t)i, seed) % 256) / 255.0f;
        }
        pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
        CHECK_EQ_INT(pc_surface_write_region(&s, r, data, len, NULL), PC_OK);
        back = region(&s, r, &nb);
        for (i = 0; back != NULL && i < nb; i++) {
            bad += fabsf(back[i] - data[i]) >= 1e-6f;
        }
        CHECK(pc_rect_eq(pc_rect_union(pc_surface_content_bounds(&s), r), r));
        free(back);
        free(data);
        pc_surface_fini(&s);
    }
    CHECK_EQ_INT(bad, 0);
}

/* proptest clone_mutation_isolated, as 300 seeded cases. */
static void test_clone_mutation_isolated(void)
{
    static const float gray[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    static const float red[4] = {1, 0, 0, 1};
    pc_surface a;
    uint64_t st = 999;
    int k;
    pc_rect box = pc_rect_make(-256, -256, 256, 256);
    pc_surface_init(&a, PC_PIXEL_FORMAT_RGBA8);
    CHECK_EQ_INT(pc_surface_fill_rect(&a, box, gray, 4, NULL), PC_OK);
    for (k = 0; k < 300; k++) {
        pc_surface work;
        pc_surface snapshot;
        int32_t x;
        int32_t y;
        float got[4];
        int c;
        st = st * 6364136223846793005u + 1442695040888963407u;
        x = (int32_t)((st >> 33) % 600) - 300;
        y = (int32_t)((st >> 13) % 600) - 300;
        if (pc_surface_clone(&a, &work, NULL) != PC_OK || pc_surface_clone(&work, &snapshot, NULL) != PC_OK) {
            PC_T_FAIL("%s", "clone");
            break;
        }
        CHECK_EQ_INT(pc_surface_write_pixel(&work, x, y, red, 4, NULL), PC_OK);
        pc_surface_read_pixel(&snapshot, x, y, got);
        for (c = 0; c < 4; c++) {
            float e = pc_rect_contains(box, x, y) ? gray[c] : 0.0f;
            CHECK(fabsf(got[c] - e) <= 1.0f / 255.0f);
        }
        pc_surface_fini(&work);
        pc_surface_fini(&snapshot);
    }
    pc_surface_fini(&a);
}

static void test_strided_row_sampling_matches_sample_channel(void)
{
    static const float q[1] = {0.25f};
    static const float one[1] = {1.0f};
    static const float half[1] = {0.5f};
    static const int32_t cases[4][3] = {{15, -520, 7}, {15, -1, 1}, {30, 3, 64}, {500, 0, 9}};
    pc_surface s;
    int c;
    pc_surface_init_default(&s, PC_PIXEL_FORMAT_GRAY8, q, 1);
    CHECK_EQ_INT(pc_surface_fill_rect(&s, pc_rect_make(-300, 10, 200, 40), one, 1, NULL), PC_OK);
    CHECK_EQ_INT(pc_surface_fill_rect(&s, pc_rect_make(250, 10, 700, 20), half, 1, NULL), PC_OK);
    for (c = 0; c < 4; c++) {
        float out[120];
        int i;
        int bad = 0;
        pc_surface_sample_row_strided(&s, cases[c][0], cases[c][1], cases[c][2], 0, out, 120);
        for (i = 0; i < 120; i++) {
            bad += out[i] != pc_surface_sample_channel(&s, cases[c][1] + i * cases[c][2], cases[c][0], 0);
        }
        CHECK_EQ_INT(bad, 0);
    }
    pc_surface_fini(&s);
}

static void test_content_bounds_with_tile_skipping(void)
{
    static const float zero[1] = {0.0f};
    static const float one[1] = {1.0f};
    static const float half[1] = {0.5f};
    pc_surface s;
    /* Interior content only; ring tiles present but all default (unpruned). */
    pc_surface_init(&s, PC_PIXEL_FORMAT_GRAY8);
    CHECK_EQ_INT(pc_surface_fill_rect(&s, pc_rect_make(0, 0, 1024, 1024), zero, 1, NULL), PC_OK);
    CHECK_EQ_INT(pc_surface_fill_rect(&s, pc_rect_make(300, 310, 700, 720), one, 1, NULL), PC_OK);
    CHECK(pc_rect_eq(pc_surface_content_bounds(&s), pc_rect_make(300, 310, 700, 720)));
    pc_surface_fini(&s);
    /* Full-canvas content plus a far-away speck. */
    pc_surface_init(&s, PC_PIXEL_FORMAT_GRAY8);
    CHECK_EQ_INT(pc_surface_fill_rect(&s, pc_rect_make(0, 0, 2000, 1500), one, 1, NULL), PC_OK);
    CHECK_EQ_INT(pc_surface_fill_rect(&s, pc_rect_make(-700, 4000, -699, 4001), half, 1, NULL), PC_OK);
    CHECK(pc_rect_eq(pc_surface_content_bounds(&s), pc_rect_make(-700, 0, 2000, 4001)));
    pc_surface_fini(&s);
}

/* ---- interrupt.rs ---- */

typedef struct flags {
    atomic_bool cancel;
    atomic_uint last;
} flags;

static bool is_cancelled(void *ctx)
{
    return atomic_load(&((flags *)ctx)->cancel);
}

static void record(void *ctx, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    atomic_store(&((flags *)ctx)->last, u);
}

static float last_of(flags *f)
{
    uint32_t u = atomic_load(&f->last);
    float v;
    memcpy(&v, &u, 4);
    return v;
}

static void test_none_never_cancels(void)
{
    pc_interrupt none = PC_INTERRUPT_NONE;
    pc_error e;
    CHECK(!pc_interrupt_cancelled(&none));
    CHECK(!pc_interrupt_cancelled(NULL));
    pc_interrupt_progress(&none, 0.5f);
    pc_interrupt_progress(NULL, 0.5f);
    CHECK_EQ_INT(pc_interrupt_check(&none, &e), PC_OK);
}

static void test_reports_cancel_and_clamped_progress(void)
{
    flags f;
    pc_interrupt i;
    pc_error e;
    atomic_init(&f.cancel, false);
    atomic_init(&f.last, 0u);
    i.cancel = is_cancelled;
    i.progress = record;
    i.ctx = &f;
    pc_interrupt_progress(&i, 2.0f);
    CHECK(last_of(&f) == 1.0f);
    pc_interrupt_progress(&i, NAN);
    CHECK(last_of(&f) == 1.0f);
    pc_interrupt_progress(&i, -INFINITY);
    CHECK(last_of(&f) == 1.0f);
    pc_interrupt_progress(&i, -0.5f);
    CHECK(last_of(&f) == 0.0f);
    CHECK(!pc_interrupt_cancelled(&i));
    atomic_store(&f.cancel, true);
    CHECK(pc_interrupt_cancelled(&i));
    CHECK_EQ_INT(pc_interrupt_check(&i, &e), PC_ERR_CANCELLED);
    CHECK_EQ_STR(e.msg, "cancelled");
    i.progress = NULL; /* cancel only */
    pc_interrupt_progress(&i, 0.25f);
    CHECK(last_of(&f) == 0.0f);
}

/* ---- C-specific ---- */

/* Random inserts, overwrites and removals against a dense reference model, so the table's
 * probing and backward-shift deletion are exercised hard (including wrapped probe runs). */
static void test_tile_table_matches_a_model(void)
{
    enum { SIDE = 24 };
    static const float px1[1] = {1.0f};
    static const float px0[1] = {0.0f};
    uint8_t model[SIDE][SIDE];
    pc_surface s;
    uint64_t st = 42;
    int step;
    int bad = 0;
    memset(model, 0, sizeof model);
    pc_surface_init(&s, PC_PIXEL_FORMAT_GRAY8);
    for (step = 0; step < 3000; step++) {
        int tx;
        int ty;
        st = st * 6364136223846793005u + 1442695040888963407u;
        tx = (int)((st >> 33) % SIDE) - SIDE / 2;
        ty = (int)((st >> 45) % SIDE) - SIDE / 2;
        if ((st >> 20) % 3 != 0) {
            CHECK_EQ_INT(pc_surface_write_pixel(&s, tx * PC_TILE_SIZE + 7, ty * PC_TILE_SIZE + 9, px1, 1, NULL),
                         PC_OK);
            model[tx + SIDE / 2][ty + SIDE / 2] = 1;
        } else {
            /* Erase the pixel and prune: the tile disappears if it was the only content. */
            CHECK_EQ_INT(pc_surface_write_pixel(&s, tx * PC_TILE_SIZE + 7, ty * PC_TILE_SIZE + 9, px0, 1, NULL),
                         PC_OK);
            pc_surface_prune(&s);
            model[tx + SIDE / 2][ty + SIDE / 2] = 0;
        }
        if (step % 149 == 0) {
            size_t n = 0;
            int a;
            int b;
            pc_surface_prune(&s);
            for (a = 0; a < SIDE; a++) {
                for (b = 0; b < SIDE; b++) {
                    pc_tile c = {a - SIDE / 2, b - SIDE / 2};
                    bool has = pc_surface_tile(&s, c) != NULL;
                    bad += has != (model[a][b] != 0);
                    n += model[a][b];
                }
            }
            bad += pc_surface_tile_count(&s) != n;
        }
    }
    CHECK_EQ_INT(bad, 0);
    {
        pc_tile_entry *e = NULL;
        size_t n = 0;
        size_t i;
        CHECK_EQ_INT(pc_surface_tiles_sorted(&s, &e, &n, NULL), PC_OK);
        CHECK_EQ_UINT(n, pc_surface_tile_count(&s));
        for (i = 1; i < n; i++) {
            CHECK(pc_tile_cmp(e[i - 1].coord, e[i].coord) < 0);
        }
        pc_tile_entries_free(e, n);
    }
    pc_surface_fini(&s);
}

static void test_limits_and_bad_lengths(void)
{
    static const float px[4] = {1, 1, 1, 1};
    pc_surface s;
    pc_rect huge = pc_rect_make(INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX);
    float *f = NULL;
    size_t n = 0;
    uint8_t *b = NULL;
    pc_tile_entry *e = NULL;
    float buf[8];
    float rgba[2][4];
    uint8_t rgba8[2][4];
    pc_error err;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    CHECK_EQ_INT(pc_surface_fill_rect(&s, huge, px, 4, &err), PC_ERR_LIMIT);
    CHECK_EQ_UINT(pc_surface_tile_count(&s), 0);
    CHECK_EQ_INT(pc_surface_fill_rect(&s, pc_rect_make(0, 0, 1, 1), px, 3, &err), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_surface_read_region(&s, huge, &f, &n, &err), PC_ERR_LIMIT);
    CHECK_EQ_INT(pc_surface_to_interleaved(&s, huge, &b, &n, &err), PC_ERR_LIMIT);
    CHECK_EQ_INT(pc_surface_take_tiles(&s, huge, &e, &n, &err), PC_ERR_LIMIT);
    CHECK_EQ_INT(pc_surface_read_region_into(&s, pc_rect_make(0, 0, 2, 1), buf, 7, &err), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_surface_read_region_into(&s, pc_rect_make(0, 0, 2, 1), buf, 8, &err), PC_OK);
    CHECK_EQ_INT(pc_surface_read_rgba_into(&s, pc_rect_make(0, 0, 3, 1), rgba, 2, &err), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_surface_read_rgba8_into(&s, pc_rect_make(0, 0, 3, 1), rgba8, 2, &err), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_surface_write_region(&s, pc_rect_make(0, 0, 2, 1), buf, 7, &err), PC_ERR_INVALID);
    CHECK_EQ_STR(err.msg, "region data length mismatch");
    /* Huge but empty-area queries are fine and fast. */
    CHECK(!pc_surface_has_tiles_in(&s, huge));
    CHECK_EQ_INT(pc_surface_write_pixel(&s, -9, 9, px, 4, NULL), PC_OK);
    CHECK(pc_surface_has_tiles_in(&s, huge));
    CHECK(pc_surface_sample_channel(&s, -9, 9, 4) == 0.0f); /* channel out of range */
    CHECK_EQ_INT(pc_surface_read_region(&s, PC_RECT_EMPTY, &f, &n, NULL), PC_OK);
    CHECK(f == NULL && n == 0);
    pc_surface_fini(&s);
    pc_surface_fini(&s); /* twice is safe */
}

static void test_take_and_put_tiles(void)
{
    static const float px[1] = {1.0f};
    static const float q[1] = {0.5f};
    pc_surface s;
    pc_tile_entry *e = NULL;
    size_t n = 0;
    float p[1];
    size_t i;
    pc_surface_init_default(&s, PC_PIXEL_FORMAT_GRAY8, q, 1);
    CHECK_EQ_INT(pc_surface_write_pixel(&s, 3, 3, px, 1, NULL), PC_OK);
    CHECK_EQ_INT(pc_surface_take_tiles(&s, pc_rect_make(0, 0, 600, 10), &e, &n, NULL), PC_OK);
    CHECK_EQ_UINT(n, 3);
    CHECK_EQ_UINT(pc_surface_tile_count(&s), 0);
    CHECK(e != NULL && e[1].tile == e[2].tile); /* missing tiles share one blank */
    for (i = 0; e != NULL && i < n; i++) {
        uint8_t *b = pc_raster_tile_make_mut(&e[i].tile);
        CHECK(b != NULL);
        if (b != NULL) {
            b[0] = 255;
        }
    }
    CHECK(e != NULL && e[1].tile != e[2].tile); /* un-shared before writing */
    CHECK_EQ_INT(pc_surface_put_tiles(&s, e, n, NULL), PC_OK);
    free(e);
    CHECK_EQ_UINT(pc_surface_tile_count(&s), 3);
    pc_surface_read_pixel(&s, 3, 3, p);
    CHECK(p[0] == 1.0f);
    pc_surface_read_pixel(&s, 512, 0, p);
    CHECK(p[0] == 1.0f);
    pc_surface_read_pixel(&s, 513, 0, p);
    CHECK(p[0] == 128.0f / 255.0f);
    pc_surface_fini(&s);
}

static void test_rgba8_image(void)
{
    pc_rgba8_image img;
    pc_error e;
    CHECK_EQ_INT(pc_rgba8_image_init(&img, 3, 2, NULL), PC_OK);
    CHECK(img.pixels != NULL && img.pixels[23] == 0);
    pc_rgba8_image_fini(&img);
    CHECK_EQ_INT(pc_rgba8_image_init(&img, 0, 5, NULL), PC_OK);
    CHECK(img.pixels == NULL);
    pc_rgba8_image_fini(&img);
    CHECK_EQ_INT(pc_rgba8_image_init(&img, UINT32_MAX, UINT32_MAX, &e), PC_ERR_LIMIT);
    CHECK(pc_sample_is_hdr(PC_SAMPLE_TYPE_F32) && !pc_sample_is_hdr(PC_SAMPLE_TYPE_U16));
}

int main(int argc, char **argv)
{
    RUN(test_from_rgba_into_matches_from_rgba);
    RUN(test_zero_alloc_accessors_match_read_region);
    RUN(test_fill_rect_shares_covered_tiles_and_edits_stay_local);
    RUN(test_empty_surface_reads_default);
    RUN(test_write_read_pixel_every_depth);
    RUN(test_copy_on_write_shares_untouched_tiles);
    RUN(test_content_bounds_and_prune);
    RUN(test_interleaved_roundtrip_crossing_tiles);
    RUN(test_region_outside_reads_default);
    RUN(test_convert_depths_and_models);
    RUN(test_rgba_from_gray_and_cmyk);
    RUN(test_translated_matches_reading_the_moved_pixels);
    RUN(test_region_write_read_roundtrip);
    RUN(test_clone_mutation_isolated);
    RUN(test_strided_row_sampling_matches_sample_channel);
    RUN(test_content_bounds_with_tile_skipping);
    RUN(test_none_never_cancels);
    RUN(test_reports_cancel_and_clamped_progress);
    RUN(test_tile_table_matches_a_model);
    RUN(test_limits_and_bad_lengths);
    RUN(test_take_and_put_tiles);
    RUN(test_rgba8_image);
    return pc_t_finish();
}
