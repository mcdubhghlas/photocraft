/* TIFF: ports of crates/codecs/tests/{tiff_pages,tiff_photoshop,tiff_regressions,camera_raw}.rs.
 * Every file is hand-built by tiffgen.h. */
#include "common.h"
#include "codecs/orientation.h"
#include "tiffgen.h"

static const bool ORDERS[4][2] = {{true, false}, {false, false}, {true, true}, {false, true}};

typedef struct truth {
    uint8_t *u8s;
    uint16_t *u16s;
    float *f32s;
    size_t n;
} truth;

static truth make_truth(size_t n, uint64_t seed)
{
    truth t;
    rng r = {seed};
    size_t i;
    t.n = n;
    t.u8s = malloc(n + 1);
    t.u16s = malloc(2 * n + 2);
    t.f32s = malloc(4 * n + 4);
    for (i = 0; i < n; i++) {
        t.u16s[i] = (uint16_t)rng_next(&r);
        t.u8s[i] = (uint8_t)(t.u16s[i] >> 8);
        t.f32s[i] = (float)t.u16s[i] / 65535.0f;
    }
    return t;
}

static void truth_free(truth *t)
{
    free(t->u8s);
    free(t->u16s);
    free(t->f32s);
}

/* (bits, sample format, file bytes in this order, expected native bytes, sample type) */
typedef struct depth {
    uint16_t bits;
    uint16_t format;
    uint8_t *file;
    const uint8_t *want;
    size_t len;
    pc_csample sample;
} depth;

static void depths(const truth *t, bool little, depth out[3])
{
    out[0].bits = 8;
    out[0].format = 1;
    out[0].file = malloc(t->n + 1);
    memcpy(out[0].file, t->u8s, t->n);
    out[0].want = t->u8s;
    out[0].len = t->n;
    out[0].sample = PC_CS_U8;
    out[1].bits = 16;
    out[1].format = 1;
    out[1].file = tg_u16s(t->u16s, t->n, little);
    out[1].want = (const uint8_t *)t->u16s;
    out[1].len = 2 * t->n;
    out[1].sample = PC_CS_U16;
    out[2].bits = 32;
    out[2].format = 3;
    out[2].file = tg_f32s(t->f32s, t->n, little);
    out[2].want = (const uint8_t *)t->f32s;
    out[2].len = 4 * t->n;
    out[2].sample = PC_CS_F32;
}

static void depths_free(depth d[3])
{
    int i;
    for (i = 0; i < 3; i++) {
        free(d[i].file);
    }
}

static const size_t CHAIN0[1] = {0};

static tg_built build1(bool little, bool big, tg_dir *d)
{
    tg_built b = tg_build(little, big, d, 1, CHAIN0, 1);
    tg_dir_free(d);
    return b;
}

static pc_cimage decode_ok(const tg_built *b) { return dec(b->bytes, b->len); }

static void test_bigtiff_and_classic_in_both_byte_orders_at_every_depth(void)
{
    static const uint16_t SPP[3] = {1, 3, 4};
    int si;
    int o;
    int k;
    for (si = 0; si < 3; si++) {
        uint16_t spp = SPP[si];
        truth t = make_truth(7 * 5 * spp, spp);
        for (o = 0; o < 4; o++) {
            bool little = ORDERS[o][0];
            bool big = ORDERS[o][1];
            depth ds[3];
            depths(&t, little, ds);
            for (k = 0; k < 3; k++) {
                tg_dir d = tg_strips(7, 5, ds[k].bits, spp, spp == 1 ? 1 : 2, 2, ds[k].file, ds[k].len);
                uint64_t fmt[4] = {ds[k].format, ds[k].format, ds[k].format, ds[k].format};
                tg_built b;
                pc_cimage img;
                pc_tiff_info info;
                tg_tag(&d, 339, TG_SHORT, fmt, spp);
                if (spp == 4) {
                    tg_tag1(&d, 338, TG_SHORT, 2);
                }
                b = build1(little, big, &d);
                img = decode_ok(&b);
                CHECK(img.width == 7 && img.height == 5 && img.sample == ds[k].sample);
                CHECK_EQ_UINT(pc_clayout_channels(img.layout), spp);
                CHECK(img.len == ds[k].len && memcmp(img.data, ds[k].want, ds[k].len) == 0);
                CHECK_EQ_UINT(img.nwarnings, 0);
                CHECK_EQ_INT(pc_ctiff_info(b.bytes, b.len, &info, NULL), PC_OK);
                CHECK(info.big_tiff == big && info.little_endian == little && info.npages == 1 && info.complete);
                pc_tiff_info_fini(&info);
                pc_cimage_fini(&img);
                tg_built_free(&b);
            }
            depths_free(ds);
        }
        truth_free(&t);
    }
}

static void test_tiles_with_padding_in_both_planar_configurations(void)
{
    static const size_t SPP[3] = {1, 3, 4};
    int si;
    int o;
    int k;
    int planar;
    for (si = 0; si < 3; si++) {
        size_t spp = SPP[si];
        truth t = make_truth(37 * 29 * spp, 9 + spp);
        for (o = 0; o < 4; o++) {
            bool little = ORDERS[o][0];
            bool big = ORDERS[o][1];
            depth ds[3];
            depths(&t, little, ds);
            for (k = 0; k < 3; k++) {
                for (planar = 0; planar < 2; planar++) {
                    tg_dir d = tg_base(37, 29, ds[k].bits, (uint16_t)spp, spp == 1 ? 1 : 2);
                    tg_built b;
                    pc_cimage img;
                    pc_tiff_info info;
                    d.tiled = true;
                    tg_tile_split(&d, ds[k].file, 37, 29, spp, ds[k].bits / 8, 16, 16, planar != 0);
                    tg_tag1(&d, 322, TG_SHORT, 16);
                    tg_tag1(&d, 323, TG_SHORT, 16);
                    tg_tag1(&d, 339, TG_SHORT, ds[k].format);
                    if (planar) {
                        tg_tag1(&d, 284, TG_SHORT, 2);
                    }
                    if (spp == 4) {
                        tg_tag1(&d, 338, TG_SHORT, 2);
                    }
                    b = build1(little, big, &d);
                    img = decode_ok(&b);
                    if (!(img.len == ds[k].len && memcmp(img.data, ds[k].want, ds[k].len) == 0)) {
                        PC_T_FAIL("tiles: little %d big %d bits %u spp %zu planar %d", little, big, ds[k].bits, spp,
                                  planar);
                    }
                    CHECK_EQ_INT(pc_ctiff_info(b.bytes, b.len, &info, NULL), PC_OK);
                    CHECK(info.npages > 0 && info.pages[0].tiled && info.pages[0].planar == (planar != 0));
                    pc_tiff_info_fini(&info);
                    pc_cimage_fini(&img);
                    tg_built_free(&b);
                }
            }
            depths_free(ds);
        }
        truth_free(&t);
    }
}

static void test_planar_strips_at_every_depth(void)
{
    static const size_t SPP[2] = {3, 4};
    static const size_t ROWS[3] = {1, 4, 9};
    int si;
    int o;
    int k;
    int ri;
    for (si = 0; si < 2; si++) {
        size_t spp = SPP[si];
        truth t = make_truth(11 * 9 * spp, 3);
        for (o = 0; o < 4; o++) {
            bool little = ORDERS[o][0];
            bool big = ORDERS[o][1];
            depth ds[3];
            depths(&t, little, ds);
            for (k = 0; k < 3; k++) {
                for (ri = 0; ri < 3; ri++) {
                    tg_dir d = tg_base(11, 9, ds[k].bits, (uint16_t)spp, 2);
                    tg_built b;
                    pc_cimage img;
                    tg_planar_strips(&d, ds[k].file, 11, 9, spp, ds[k].bits / 8, ROWS[ri]);
                    tg_tag1(&d, 278, TG_LONG, ROWS[ri]);
                    tg_tag1(&d, 284, TG_SHORT, 2);
                    tg_tag1(&d, 339, TG_SHORT, ds[k].format);
                    if (spp == 4) {
                        tg_tag1(&d, 338, TG_SHORT, 2);
                    }
                    b = build1(little, big, &d);
                    img = decode_ok(&b);
                    if (!(img.len == ds[k].len && memcmp(img.data, ds[k].want, ds[k].len) == 0)) {
                        PC_T_FAIL("planar strips: little %d big %d bits %u spp %zu rows %zu", little, big, ds[k].bits,
                                  spp, ROWS[ri]);
                    }
                    pc_cimage_fini(&img);
                    tg_built_free(&b);
                }
            }
            depths_free(ds);
        }
        truth_free(&t);
    }
}

static uint8_t *keep_samples(const uint8_t *px, size_t npx, size_t spp, size_t keep)
{
    uint8_t *o = malloc(npx * keep + 1);
    size_t i;
    for (i = 0; i < npx; i++) {
        memcpy(o + i * keep, px + i * spp, keep);
    }
    return o;
}

static void test_extra_samples_that_are_not_alpha_are_dropped(void)
{
    truth t = make_truth(5 * 3 * 5, 4);
    int planar;
    for (planar = 0; planar < 2; planar++) {
        tg_dir d = tg_base(5, 3, 8, 4, 2);
        tg_built b;
        pc_cimage img;
        uint8_t *want;
        if (planar) {
            tg_planar_strips(&d, t.u8s, 5, 3, 4, 1, 2);
        } else {
            size_t i;
            for (i = 0; i < 5 * 3 * 4; i += 5 * 4 * 2) {
                tg_add_chunk(&d, t.u8s + i, 5 * 3 * 4 - i < 40 ? 5 * 3 * 4 - i : 40);
            }
        }
        tg_tag1(&d, 278, TG_LONG, 2);
        tg_tag1(&d, 338, TG_SHORT, 0);
        if (planar) {
            tg_tag1(&d, 284, TG_SHORT, 2);
        }
        b = build1(true, false, &d);
        img = decode_ok(&b);
        want = keep_samples(t.u8s, 15, 4, 3);
        CHECK(img.layout == PC_CL_RGB);
        CHECK(img.len == 45 && memcmp(img.data, want, 45) == 0);
        free(want);
        pc_cimage_fini(&img);
        tg_built_free(&b);
        /* RGB + alpha + an unspecified extra sample: RGBA */
        d = tg_base(5, 3, 8, 5, 2);
        if (planar) {
            tg_planar_strips(&d, t.u8s, 5, 3, 5, 1, 2);
        } else {
            size_t i;
            for (i = 0; i < 5 * 3 * 5; i += 5 * 5 * 2) {
                tg_add_chunk(&d, t.u8s + i, 75 - i < 50 ? 75 - i : 50);
            }
        }
        tg_tag1(&d, 278, TG_LONG, 2);
        {
            uint64_t ex[2] = {2, 0};
            tg_tag(&d, 338, TG_SHORT, ex, 2);
        }
        if (planar) {
            tg_tag1(&d, 284, TG_SHORT, 2);
        }
        b = build1(false, true, &d);
        img = decode_ok(&b);
        want = keep_samples(t.u8s, 15, 5, 4);
        CHECK(img.layout == PC_CL_RGBA);
        CHECK(img.len == 60 && memcmp(img.data, want, 60) == 0);
        free(want);
        pc_cimage_fini(&img);
        tg_built_free(&b);
    }
    {
        /* gray + alpha + extra: gray and alpha; gray + an unspecified extra: gray */
        tg_dir d = tg_strips(5, 3, 8, 3, 1, 3, t.u8s, 45);
        uint64_t ex[2] = {2, 0};
        tg_built b;
        pc_cimage img;
        uint8_t *want = keep_samples(t.u8s, 15, 3, 2);
        tg_tag(&d, 338, TG_SHORT, ex, 2);
        b = build1(true, false, &d);
        img = decode_ok(&b);
        CHECK(img.layout == PC_CL_GRAYA && img.len == 30 && memcmp(img.data, want, 30) == 0);
        free(want);
        pc_cimage_fini(&img);
        tg_built_free(&b);
        d = tg_strips(5, 3, 8, 2, 1, 3, t.u8s, 30);
        tg_tag1(&d, 338, TG_SHORT, 0);
        b = build1(true, false, &d);
        img = decode_ok(&b);
        want = keep_samples(t.u8s, 15, 2, 1);
        CHECK(img.layout == PC_CL_GRAY && img.len == 15 && memcmp(img.data, want, 15) == 0);
        free(want);
        pc_cimage_fini(&img);
        tg_built_free(&b);
    }
    truth_free(&t);
}

static void test_alpha_after_an_unspecified_extra_sample_is_kept(void)
{
    static const struct {
        uint16_t photometric;
        size_t color;
        pc_clayout layout;
    } C[3] = {{2, 3, PC_CL_RGBA}, {5, 4, PC_CL_CMYKA}, {1, 1, PC_CL_GRAYA}};
    int ci;
    int o;
    int k;
    int planar;
    for (ci = 0; ci < 3; ci++) {
        size_t spp = C[ci].color + 2;
        truth t = make_truth(5 * 3 * spp, 13 + spp);
        for (o = 0; o < 4; o++) {
            bool little = ORDERS[o][0];
            bool big = ORDERS[o][1];
            depth ds[3];
            depths(&t, little, ds);
            for (k = 0; k < 3; k++) {
                size_t bps = ds[k].bits / 8;
                size_t npx = 15;
                size_t keep = (C[ci].color + 1) * bps;
                uint8_t *want = malloc(npx * keep + 1);
                size_t p;
                for (p = 0; p < npx; p++) {
                    const uint8_t *src = ds[k].want + p * spp * bps;
                    memcpy(want + p * keep, src, C[ci].color * bps);
                    memcpy(want + p * keep + C[ci].color * bps, src + (C[ci].color + 1) * bps, bps);
                }
                for (planar = 0; planar < 2; planar++) {
                    tg_dir d = tg_base(5, 3, ds[k].bits, (uint16_t)spp, C[ci].photometric);
                    uint64_t fmt[8];
                    uint64_t ex[2] = {0, 2};
                    tg_built b;
                    pc_cimage img;
                    size_t q;
                    if (planar) {
                        tg_planar_strips(&d, ds[k].file, 5, 3, spp, bps, 2);
                    } else {
                        size_t rowb = 5 * spp * bps * 2;
                        for (q = 0; q < ds[k].len; q += rowb) {
                            tg_add_chunk(&d, ds[k].file + q, ds[k].len - q < rowb ? ds[k].len - q : rowb);
                        }
                    }
                    for (q = 0; q < spp; q++) {
                        fmt[q] = ds[k].format;
                    }
                    tg_tag1(&d, 278, TG_LONG, 2);
                    tg_tag(&d, 339, TG_SHORT, fmt, spp);
                    tg_tag(&d, 338, TG_SHORT, ex, 2);
                    if (planar) {
                        tg_tag1(&d, 284, TG_SHORT, 2);
                    }
                    b = build1(little, big, &d);
                    img = decode_ok(&b);
                    if (!(img.layout == C[ci].layout && img.sample == ds[k].sample && img.len == npx * keep &&
                          memcmp(img.data, want, npx * keep) == 0 && img.nwarnings == 0)) {
                        PC_T_FAIL("photometric %u little %d big %d bits %u planar %d", C[ci].photometric, little, big,
                                  ds[k].bits, planar);
                    }
                    pc_cimage_fini(&img);
                    tg_built_free(&b);
                }
                free(want);
            }
            depths_free(ds);
        }
        truth_free(&t);
    }
    {
        /* The issue's file: 2x2 RGB 200 100 50, then 17 (unspecified), then alpha 128. */
        uint8_t px[20];
        uint8_t want[16];
        uint64_t ex[2] = {0, 2};
        tg_dir d;
        tg_built b;
        pc_cimage img;
        int i;
        for (i = 0; i < 4; i++) {
            static const uint8_t p5[5] = {200, 100, 50, 17, 128};
            static const uint8_t p4[4] = {200, 100, 50, 128};
            memcpy(px + 5 * i, p5, 5);
            memcpy(want + 4 * i, p4, 4);
        }
        d = tg_strips(2, 2, 8, 5, 2, 2, px, 20);
        tg_tag(&d, 338, TG_SHORT, ex, 2);
        b = build1(true, false, &d);
        img = decode_ok(&b);
        CHECK(img.len == 16 && memcmp(img.data, want, 16) == 0);
        pc_cimage_fini(&img);
        tg_built_free(&b);
        /* Associated alpha in the same position is made straight. */
        for (i = 0; i < 4; i++) {
            static const uint8_t p5[5] = {100, 50, 25, 17, 128};
            static const uint8_t p4[4] = {199, 100, 50, 128};
            memcpy(px + 5 * i, p5, 5);
            memcpy(want + 4 * i, p4, 4);
        }
        d = tg_strips(2, 2, 8, 5, 2, 2, px, 20);
        ex[1] = 1;
        tg_tag(&d, 338, TG_SHORT, ex, 2);
        b = build1(true, false, &d);
        img = decode_ok(&b);
        CHECK(img.len == 16 && memcmp(img.data, want, 16) == 0);
        pc_cimage_fini(&img);
        tg_built_free(&b);
    }
}

static void test_associated_alpha_is_made_straight(void)
{
    static const uint8_t px[6] = {64, 128, 200, 255, 0, 0};
    static const uint8_t want[6] = {128, 128, 200, 255, 0, 0};
    static const uint16_t v16[2] = {16384, 32768};
    tg_dir d = tg_strips(3, 1, 8, 2, 1, 1, px, 6);
    tg_built b;
    pc_cimage img;
    uint8_t *f16;
    tg_tag1(&d, 338, TG_SHORT, 1);
    b = build1(true, false, &d);
    img = decode_ok(&b);
    CHECK(img.layout == PC_CL_GRAYA && img.len == 6 && memcmp(img.data, want, 6) == 0);
    pc_cimage_fini(&img);
    tg_built_free(&b);
    f16 = tg_u16s(v16, 2, false);
    d = tg_strips(1, 1, 16, 2, 1, 1, f16, 4);
    free(f16);
    tg_tag1(&d, 338, TG_SHORT, 1);
    b = build1(false, false, &d);
    img = decode_ok(&b);
    CHECK(img.len == 4 && ne16(img.data, 0) == 32768 && ne16(img.data, 1) == 32768);
    pc_cimage_fini(&img);
    tg_built_free(&b);
}

static void test_white_is_zero_opens_with_the_right_tones(void)
{
    static const uint8_t px[3] = {0x00, 0xff, 0x00};
    static const uint8_t white[3] = {255, 0, 255};
    tg_dir d = tg_strips(3, 1, 8, 1, 0, 1, px, 3);
    tg_built b = build1(true, false, &d);
    pc_cimage img = decode_ok(&b);
    int o;
    CHECK(img.len == 3 && memcmp(img.data, white, 3) == 0);
    pc_cimage_fini(&img);
    tg_built_free(&b);
    d = tg_strips(3, 1, 8, 1, 1, 1, px, 3);
    b = build1(true, false, &d);
    img = decode_ok(&b);
    CHECK(img.len == 3 && memcmp(img.data, px, 3) == 0);
    pc_cimage_fini(&img);
    tg_built_free(&b);
    for (o = 0; o < 4; o++) {
        static const uint16_t v[2] = {0, 65535};
        uint8_t *f = tg_u16s(v, 2, ORDERS[o][0]);
        d = tg_strips(2, 1, 16, 1, 0, 1, f, 4);
        free(f);
        b = build1(ORDERS[o][0], ORDERS[o][1], &d);
        img = decode_ok(&b);
        CHECK(img.len == 4 && ne16(img.data, 0) == 65535 && ne16(img.data, 1) == 0);
        pc_cimage_fini(&img);
        tg_built_free(&b);
    }
    {
        static const uint8_t bits[1] = {0xA0};
        static const uint8_t want[8] = {0, 255, 0, 255, 255, 255, 255, 255};
        d = tg_strips(8, 1, 1, 1, 0, 1, bits, 1);
        b = build1(true, false, &d);
        img = decode_ok(&b);
        CHECK(img.len == 8 && memcmp(img.data, want, 8) == 0);
        pc_cimage_fini(&img);
        tg_built_free(&b);
    }
    {
        static const uint8_t ga[4] = {0, 255, 255, 10};
        static const uint8_t want[4] = {255, 255, 0, 10};
        d = tg_strips(2, 1, 8, 2, 0, 1, ga, 4);
        tg_tag1(&d, 338, TG_SHORT, 2);
        b = build1(true, false, &d);
        img = decode_ok(&b);
        CHECK(img.layout == PC_CL_GRAYA && img.len == 4 && memcmp(img.data, want, 4) == 0);
        pc_cimage_fini(&img);
        tg_built_free(&b);
    }
    {
        static const float fv[2] = {0.0f, 0.25f};
        uint8_t *f = tg_f32s(fv, 2, true);
        d = tg_strips(2, 1, 32, 1, 0, 1, f, 8);
        free(f);
        tg_tag1(&d, 339, TG_SHORT, 3);
        b = build1(true, false, &d);
        img = decode_ok(&b);
        CHECK(img.len == 8 && nef32(img.data, 0) == 1.0f && nef32(img.data, 1) == 0.75f);
        pc_cimage_fini(&img);
        tg_built_free(&b);
    }
}

static void test_bigtiff_orientation_is_applied_like_classic_tiff(void)
{
    int o;
    for (o = 0; o < 4; o++) {
        static const uint8_t px[2] = {10, 200};
        tg_dir d = tg_strips(2, 1, 8, 1, 1, 1, px, 2);
        tg_built b;
        pc_cimage img;
        pc_cdecode_opts keep = pc_cdecode_opts_default();
        tg_tag1(&d, 274, TG_SHORT, 6);
        b = build1(ORDERS[o][0], ORDERS[o][1], &d);
        CHECK_EQ_UINT(pc_exif_orientation(b.bytes, b.len), 6);
        CHECK_EQ_UINT(pc_ctiff_orientation(b.bytes, b.len), 6);
        img = decode_ok(&b);
        CHECK(img.width == 1 && img.height == 2 && img.len == 2 && memcmp(img.data, px, 2) == 0);
        pc_cimage_fini(&img);
        keep.keep_orientation = true;
        img = dec_with(b.bytes, b.len, &keep);
        CHECK(img.width == 2 && img.height == 1);
        pc_cimage_fini(&img);
        tg_built_free(&b);
    }
}

static void test_palette_images_open_as_rgb(void)
{
    uint64_t map[768];
    int o;
    memset(map, 0, sizeof map);
    map[0] = 0xffff;
    map[1] = 16 * 257;
    map[256 + 1] = 32 * 257;
    map[512 + 1] = 48 * 257;
    for (o = 0; o < 4; o++) {
        static const uint8_t px[3] = {0, 1, 0};
        static const uint8_t want[9] = {255, 0, 0, 16, 32, 48, 255, 0, 0};
        tg_dir d = tg_strips(3, 1, 8, 1, 3, 1, px, 3);
        tg_built b;
        pc_cimage img;
        tg_tag(&d, 320, TG_SHORT, map, 768);
        b = build1(ORDERS[o][0], ORDERS[o][1], &d);
        img = decode_ok(&b);
        CHECK(img.layout == PC_CL_RGB && img.sample == PC_CS_U8 && img.len == 9 && memcmp(img.data, want, 9) == 0);
        pc_cimage_fini(&img);
        tg_built_free(&b);
    }
    {
        uint64_t m4[48];
        static const uint8_t px[1] = {0xf0};
        static const uint8_t want[6] = {255, 128, 0, 0, 0, 0};
        tg_dir d;
        tg_built b;
        pc_cimage img;
        memset(m4, 0, sizeof m4);
        m4[15] = 0xffff;
        m4[16 + 15] = 0x8000;
        d = tg_strips(2, 1, 4, 1, 3, 1, px, 1);
        tg_tag(&d, 320, TG_SHORT, m4, 48);
        b = build1(true, false, &d);
        img = decode_ok(&b);
        CHECK(img.len == 6 && memcmp(img.data, want, 6) == 0);
        pc_cimage_fini(&img);
        tg_built_free(&b);
    }
    {
        static const uint8_t px[2] = {0, 1};
        uint64_t three[3] = {1, 2, 3};
        tg_dir d = tg_strips(2, 1, 8, 1, 3, 1, px, 2);
        tg_built b = build1(true, false, &d);
        pc_cimage img;
        CHECK(pc_cdecode(b.bytes, b.len, &img, NULL) != PC_OK);
        tg_built_free(&b);
        d = tg_strips(2, 1, 8, 1, 3, 1, px, 2);
        tg_tag(&d, 320, TG_SHORT, three, 3);
        b = build1(true, false, &d);
        CHECK(pc_cdecode(b.bytes, b.len, &img, NULL) != PC_OK);
        tg_built_free(&b);
    }
}

static void test_wide_and_odd_sample_formats(void)
{
    int o;
    for (o = 0; o < 4; o++) {
        bool little = ORDERS[o][0];
        bool big = ORDERS[o][1];
        uint8_t px[16];
        tg_dir d;
        tg_built b;
        pc_cimage img;
        static const uint32_t u32v[2] = {0x12345678u, 0xffff0000u};
        static const double f64v[2] = {0.5, 2.0};
        int i;
        int k;
        for (i = 0; i < 2; i++) {
            for (k = 0; k < 4; k++) {
                px[4 * i + k] = (uint8_t)(u32v[i] >> (8 * (little ? k : (3 - k))));
            }
        }
        d = tg_strips(2, 1, 32, 1, 1, 1, px, 8);
        b = build1(little, big, &d);
        img = decode_ok(&b);
        CHECK(img.len == 4 && ne16(img.data, 0) == 0x1234 && ne16(img.data, 1) == 0xffff);
        pc_cimage_fini(&img);
        tg_built_free(&b);
        for (i = 0; i < 2; i++) {
            uint64_t u;
            memcpy(&u, &f64v[i], 8);
            for (k = 0; k < 8; k++) {
                px[8 * i + k] = (uint8_t)(u >> (8 * (little ? k : (7 - k))));
            }
        }
        d = tg_strips(2, 1, 64, 1, 1, 1, px, 16);
        tg_tag1(&d, 339, TG_SHORT, 3);
        b = build1(little, big, &d);
        img = decode_ok(&b);
        CHECK(img.sample == PC_CS_F32 && img.len == 8 && nef32(img.data, 0) == 0.5f && nef32(img.data, 1) == 2.0f);
        pc_cimage_fini(&img);
        tg_built_free(&b);
        {
            uint16_t hv[2] = {pc_f16_from_f32(0.25f), 0x3C00};
            uint8_t *f = tg_u16s(hv, 2, little);
            d = tg_strips(2, 1, 16, 1, 1, 1, f, 4);
            free(f);
            tg_tag1(&d, 339, TG_SHORT, 3);
            b = build1(little, big, &d);
            img = decode_ok(&b);
            CHECK(img.sample == PC_CS_F16 && img.len == 4 && pc_cimage_sample_normalized(&img, 0) == 0.25f &&
                  pc_cimage_sample_normalized(&img, 1) == 1.0f);
            pc_cimage_fini(&img);
            tg_built_free(&b);
        }
    }
    {
        static const uint8_t two[2] = {0x1B, 0xE4};
        static const uint8_t want2[6] = {0, 85, 170, 255, 170, 85};
        static const uint8_t four[4] = {0x0f, 0x80, 0x12, 0x30};
        static const uint8_t want4[6] = {0, 255, 136, 17, 34, 51};
        static const uint8_t zeros[4] = {0, 0, 0, 0};
        tg_dir d = tg_strips(3, 2, 2, 1, 1, 1, two, 2);
        tg_built b = build1(true, false, &d);
        pc_cimage img = decode_ok(&b);
        pc_error e;
        CHECK(img.len == 6 && memcmp(img.data, want2, 6) == 0);
        pc_cimage_fini(&img);
        tg_built_free(&b);
        d = tg_strips(3, 2, 4, 1, 1, 1, four, 4);
        b = build1(false, true, &d);
        img = decode_ok(&b);
        CHECK(img.len == 6 && memcmp(img.data, want4, 6) == 0);
        pc_cimage_fini(&img);
        tg_built_free(&b);
        d = tg_strips(2, 1, 16, 1, 1, 1, zeros, 4);
        tg_tag1(&d, 339, TG_SHORT, 2);
        b = build1(true, false, &d);
        CHECK(pc_cdecode(b.bytes, b.len, &img, &e) == PC_ERR_UNSUPPORTED);
        tg_built_free(&b);
    }
}

static tg_dir gray_page(uint32_t w, uint32_t h, uint8_t level, int64_t kind)
{
    uint8_t *px = malloc((size_t)w * h);
    tg_dir d;
    memset(px, level, (size_t)w * h);
    d = tg_strips(w, h, 8, 1, 1, h, px, (size_t)w * h);
    free(px);
    if (kind >= 0) {
        tg_tag1(&d, 254, TG_LONG, (uint64_t)kind);
    }
    return d;
}

static void test_every_ifd_and_subifd_is_listed_and_decodable(void)
{
    int o;
    for (o = 0; o < 4; o++) {
        bool little = ORDERS[o][0];
        bool big = ORDERS[o][1];
        tg_dir dirs[6];
        static const size_t chain[4] = {0, 1, 2, 3};
        static const size_t subs[2] = {4, 5};
        tg_built b;
        pc_tiff_info info;
        pc_cimage img;
        size_t dp;
        int i;
        static const struct {
            uint32_t w;
            bool has_parent;
            size_t parent;
            pc_tiff_page_kind kind;
        } want[6] = {{4, false, 0, PC_TIFF_REDUCED}, {9, false, 0, PC_TIFF_PAGE},   {3, true, 1, PC_TIFF_REDUCED},
                     {2, true, 1, PC_TIFF_REDUCED}, {9, false, 0, PC_TIFF_MASK},   {5, false, 0, PC_TIFF_PAGE}};
        static const struct {
            uint32_t w;
            uint8_t level;
        } pages[6] = {{4, 1}, {9, 2}, {3, 5}, {2, 6}, {9, 3}, {5, 4}};
        dirs[0] = gray_page(4, 3, 1, 1);
        dirs[1] = gray_page(9, 7, 2, -1);
        tg_sub_ifds(&dirs[1], big ? TG_IFD8 : TG_IFD, subs, 2);
        dirs[2] = gray_page(9, 7, 3, 4);
        dirs[3] = gray_page(5, 5, 4, 2);
        dirs[4] = gray_page(3, 2, 5, 1);
        dirs[5] = gray_page(2, 1, 6, 1);
        b = tg_build(little, big, dirs, 6, chain, 4);
        for (i = 0; i < 6; i++) {
            tg_dir_free(&dirs[i]);
        }
        CHECK_EQ_INT(pc_ctiff_info(b.bytes, b.len, &info, NULL), PC_OK);
        CHECK(info.complete && info.npages == 6);
        for (i = 0; i < 6 && (size_t)i < info.npages; i++) {
            CHECK(info.pages[i].width == want[i].w && info.pages[i].has_parent == want[i].has_parent &&
                  (!want[i].has_parent || info.pages[i].parent == want[i].parent) &&
                  info.pages[i].kind == want[i].kind);
        }
        CHECK(info.npages > 1 && info.pages[1].ifd_offset == b.ifd_at[1]);
        CHECK(pc_tiff_info_default_page(&info, &dp) && dp == 1 && pc_tiff_info_page_count(&info) == 2);
        pc_tiff_info_fini(&info);
        img = dec(b.bytes, b.len);
        CHECK(img.width == 9 && img.height == 7 && img.len > 0 && img.data[0] == 2);
        CHECK(img.nwarnings == 1 && img.warnings[0].kind == PC_CW_MORE_PAGES && img.warnings[0].has_total &&
              img.warnings[0].total == 2);
        pc_cimage_fini(&img);
        for (i = 0; i < 6; i++) {
            pc_cdecode_opts op = pc_cdecode_opts_default();
            CHECK_EQ_INT(pc_cdecode_tiff_page(b.bytes, b.len, (size_t)i, &op, &img, NULL), PC_OK);
            CHECK(img.width == pages[i].w && img.len > 0 && img.data[0] == pages[i].level);
            pc_cimage_fini(&img);
        }
        {
            pc_cdecode_opts op = pc_cdecode_opts_default();
            CHECK(pc_cdecode_tiff_page(b.bytes, b.len, 6, &op, &img, NULL) == PC_ERR_INVALID);
        }
        tg_built_free(&b);
    }
}

static void test_a_file_of_only_thumbnails_still_opens(void)
{
    tg_dir d = gray_page(4, 3, 7, 1);
    tg_built b = build1(true, false, &d);
    pc_tiff_info info;
    size_t dp;
    pc_cimage img;
    CHECK_EQ_INT(pc_ctiff_info(b.bytes, b.len, &info, NULL), PC_OK);
    CHECK(pc_tiff_info_default_page(&info, &dp) && dp == 0);
    pc_tiff_info_fini(&info);
    img = decode_ok(&b);
    CHECK(img.len > 0 && img.data[0] == 7);
    pc_cimage_fini(&img);
    tg_built_free(&b);
}

static void test_cyclic_chains_and_subifds_terminate(void)
{
    int o;
    for (o = 0; o < 4; o++) {
        bool little = ORDERS[o][0];
        bool big = ORDERS[o][1];
        tg_dir dirs[2];
        static const size_t chain[2] = {0, 1};
        tg_built b;
        pc_tiff_info info;
        pc_cimage img;
        int k;
        dirs[0] = gray_page(2, 2, 1, -1);
        dirs[1] = gray_page(2, 2, 2, -1);
        b = tg_build(little, big, dirs, 2, chain, 2);
        tg_dir_free(&dirs[0]);
        tg_dir_free(&dirs[1]);
        tg_set_next(&b, little, big, 1, b.ifd_at[0]);
        CHECK_EQ_INT(pc_ctiff_info(b.bytes, b.len, &info, NULL), PC_OK);
        CHECK(info.npages == 2 && !info.complete);
        pc_tiff_info_fini(&info);
        img = decode_ok(&b);
        CHECK(img.nwarnings == 1 && img.warnings[0].kind == PC_CW_MORE_PAGES && !img.warnings[0].has_total);
        pc_cimage_fini(&img);
        tg_built_free(&b);
        /* a directory pointing at itself */
        dirs[0] = gray_page(2, 2, 1, -1);
        b = build1(little, big, &dirs[0]);
        tg_set_next(&b, little, big, 0, b.ifd_at[0]);
        CHECK_EQ_INT(pc_ctiff_info(b.bytes, b.len, &info, NULL), PC_OK);
        CHECK_EQ_UINT(info.npages, 1);
        pc_tiff_info_fini(&info);
        img = decode_ok(&b);
        CHECK_EQ_UINT(img.nwarnings, 0);
        pc_cimage_fini(&img);
        tg_built_free(&b);
        /* a SubIFD listing its own parent, and one listing itself */
        {
            static const size_t s1[1] = {1};
            static const size_t s01[2] = {0, 1};
            dirs[0] = gray_page(2, 2, 1, -1);
            tg_sub_ifds(&dirs[0], big ? TG_IFD8 : TG_IFD, s1, 1);
            dirs[1] = gray_page(1, 1, 2, 1);
            tg_sub_ifds(&dirs[1], big ? TG_IFD8 : TG_IFD, s01, 2);
            b = tg_build(little, big, dirs, 2, CHAIN0, 1);
            tg_dir_free(&dirs[0]);
            tg_dir_free(&dirs[1]);
            CHECK_EQ_INT(pc_ctiff_info(b.bytes, b.len, &info, NULL), PC_OK);
            CHECK(info.npages == 2 && !info.complete);
            pc_tiff_info_fini(&info);
            img = decode_ok(&b);
            pc_cimage_fini(&img);
            tg_built_free(&b);
        }
        /* a next pointer past the end, or into the header */
        for (k = 0; k < 3; k++) {
            uint64_t to;
            dirs[0] = gray_page(2, 2, 1, -1);
            dirs[1] = gray_page(2, 2, 2, -1);
            b = tg_build(little, big, dirs, 2, chain, 2);
            tg_dir_free(&dirs[0]);
            tg_dir_free(&dirs[1]);
            to = k == 0 ? b.len + 100 : k == 1 ? 3 : UINT64_MAX;
            tg_set_next(&b, little, big, 1, to);
            CHECK_EQ_INT(pc_ctiff_info(b.bytes, b.len, &info, NULL), PC_OK);
            CHECK(info.npages == 2 && !info.complete);
            pc_tiff_info_fini(&info);
            img = decode_ok(&b);
            pc_cimage_fini(&img);
            tg_built_free(&b);
        }
    }
}

static void test_the_chain_walk_is_capped(void)
{
    size_t n = 10001;
    tg_dir *dirs = malloc(n * sizeof *dirs);
    size_t *chain = malloc(n * sizeof *chain);
    tg_built b;
    pc_tiff_info info;
    pc_cimage img;
    size_t i;
    for (i = 0; i < n; i++) {
        dirs[i] = gray_page(1, 1, 9, -1);
        chain[i] = i;
    }
    b = tg_build(true, false, dirs, n, chain, n);
    for (i = 0; i < n; i++) {
        tg_dir_free(&dirs[i]);
    }
    free(dirs);
    free(chain);
    CHECK_EQ_INT(pc_ctiff_info(b.bytes, b.len, &info, NULL), PC_OK);
    CHECK(info.npages == 10000 && !info.complete);
    pc_tiff_info_fini(&info);
    img = decode_ok(&b);
    CHECK(img.nwarnings == 1 && img.warnings[0].kind == PC_CW_MORE_PAGES && !img.warnings[0].has_total);
    pc_cimage_fini(&img);
    tg_built_free(&b);
}

static void test_image_data_cut_off_decodes_with_a_warning(void)
{
    uint8_t px[32];
    int o;
    int i;
    for (i = 0; i < 32; i++) {
        px[i] = (uint8_t)i;
    }
    for (o = 0; o < 4; o++) {
        bool little = ORDERS[o][0];
        bool big = ORDERS[o][1];
        tg_dir d = tg_strips(4, 8, 8, 1, 1, 2, px, 32);
        tg_built b = build1(little, big, &d);
        pc_cimage img;
        static const uint8_t z[8] = {0};
        tg_patch_value(&b, little, big, 0, 273, 3, b.len + 1000);
        img = decode_ok(&b);
        CHECK(img.nwarnings == 1 && img.warnings[0].kind == PC_CW_TRUNCATED);
        CHECK(img.len == 32 && memcmp(img.data, px, 24) == 0 && memcmp(img.data + 24, z, 8) == 0);
        pc_cimage_fini(&img);
        tg_built_free(&b);
        d = tg_strips(4, 8, 8, 1, 1, 2, px, 32);
        b = build1(little, big, &d);
        tg_patch_value(&b, little, big, 0, 273, 1, b.len - 3);
        img = decode_ok(&b);
        CHECK(img.nwarnings == 1 && img.warnings[0].kind == PC_CW_TRUNCATED);
        pc_cimage_fini(&img);
        tg_built_free(&b);
        d = tg_strips(4, 8, 8, 1, 1, 2, px, 32);
        b = build1(little, big, &d);
        for (i = 0; i < 4; i++) {
            tg_patch_value(&b, little, big, 0, 273, (size_t)i, b.len + 7);
        }
        CHECK(pc_cdecode(b.bytes, b.len, &img, NULL) != PC_OK);
        tg_built_free(&b);
    }
}

static void test_hostile_sizes_and_counts_are_refused_without_allocating(void)
{
    pc_cdecode_opts tight = pc_cdecode_opts_default();
    pc_cdecode_opts none = pc_cdecode_opts_default();
    int o;
    tight.limits.max_alloc = 64u << 20;
    none.limits = pc_climits_none();
    for (o = 0; o < 4; o++) {
        bool little = ORDERS[o][0];
        bool big = ORDERS[o][1];
        static const uint8_t px[16] = {7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};
        static const uint64_t dims[5][2] = {
            {1ull << 31, 4}, {4, 1ull << 31}, {0xFFFFFFFFu, 0xFFFFFFFFu}, {200000, 200000}, {30000, 30000}};
        int k;
        pc_cimage img;
        pc_error e;
        for (k = 0; k < 5; k++) {
            tg_dir d = tg_strips(4, 4, 8, 1, 1, 1, px, 16);
            tg_built b = build1(little, big, &d);
            tg_patch_value(&b, little, big, 0, 256, 0, dims[k][0]);
            tg_patch_value(&b, little, big, 0, 257, 0, dims[k][1]);
            CHECK(pc_cdecode(b.bytes, b.len, &img, NULL) != PC_OK);
            CHECK(pc_cdecode_with(b.bytes, b.len, &none, &img, NULL) != PC_OK);
            tg_built_free(&b);
        }
        {
            tg_dir d = tg_strips(4, 4, 8, 1, 1, 1, px, 16);
            tg_built b = build1(little, big, &d);
            tg_patch_count(&b, little, big, 0, 273, 1u << 30);
            CHECK(pc_cdecode(b.bytes, b.len, &img, &e) == PC_ERR_FORMAT);
            tg_built_free(&b);
            d = tg_strips(4, 4, 8, 1, 1, 1, px, 16);
            b = build1(little, big, &d);
            tg_patch_count(&b, little, big, 0, 258, 0xFFFFFFFFu);
            CHECK(pc_cdecode(b.bytes, b.len, &img, NULL) != PC_OK);
            tg_built_free(&b);
        }
        {
            static const struct {
                uint16_t tag;
                uint64_t v;
            } P[3] = {{278, 0}, {277, 0}, {277, 60000}};
            for (k = 0; k < 3; k++) {
                tg_dir d = tg_strips(4, 4, 8, 1, 1, 1, px, 16);
                tg_built b = build1(little, big, &d);
                tg_patch_value(&b, little, big, 0, P[k].tag, 0, P[k].v);
                CHECK(pc_cdecode(b.bytes, b.len, &img, NULL) != PC_OK);
                tg_built_free(&b);
            }
        }
        {
            int planar;
            static const uint64_t T[3][2] = {{1ull << 30, 16}, {16, 1ull << 30}, {0xFFFFFFFFu, 0xFFFFFFFFu}};
            for (planar = 0; planar < 2; planar++) {
                for (k = 0; k < 3; k++) {
                    uint16_t spp = planar ? 3 : 1;
                    tg_dir d = tg_base(4, 4, 8, spp, planar ? 2 : 1);
                    uint8_t ones[64];
                    tg_built b;
                    int c;
                    memset(ones, 1, sizeof ones);
                    d.tiled = true;
                    for (c = 0; c < spp; c++) {
                        tg_add_chunk(&d, ones, 64);
                    }
                    tg_tag1(&d, 322, TG_LONG, T[k][0]);
                    tg_tag1(&d, 323, TG_LONG, T[k][1]);
                    if (planar) {
                        tg_tag1(&d, 284, TG_SHORT, 2);
                    }
                    b = build1(little, big, &d);
                    if (pc_cdecode_with(b.bytes, b.len, &tight, &img, NULL) == PC_OK) {
                        pc_cimage_fini(&img);
                    }
                    if (pc_cdecode(b.bytes, b.len, &img, NULL) == PC_OK) {
                        pc_cimage_fini(&img);
                    }
                    tg_built_free(&b);
                }
            }
        }
    }
    {
        /* a BigTIFF directory claiming 2^60 entries */
        tg_dir d = gray_page(2, 2, 1, -1);
        tg_built b = build1(true, true, &d);
        pc_tiff_info info;
        pc_cimage img;
        uint64_t v = 1ull << 60;
        size_t i;
        for (i = 0; i < 8; i++) {
            b.bytes[b.ifd_at[0] + i] = (uint8_t)(v >> (8 * i));
        }
        CHECK(pc_ctiff_info(b.bytes, b.len, &info, NULL) != PC_OK);
        CHECK(pc_cdecode(b.bytes, b.len, &img, NULL) != PC_OK);
        tg_built_free(&b);
    }
}

static void test_banded_decode_matches_the_encoder_on_larger_images(void)
{
    static const struct {
        pc_clayout l;
        pc_csample s;
    } C[4] = {{PC_CL_RGB, PC_CS_U8}, {PC_CL_RGBA, PC_CS_U16}, {PC_CL_GRAY, PC_CS_F32}, {PC_CL_CMYK, PC_CS_U8}};
    int i;
    int c;
    for (i = 0; i < 4; i++) {
        pc_cimage img = synth(613, 411, C[i].l, C[i].s, 3, 0.2f);
        for (c = 0; c < 4; c++) {
            pc_cencode_opts o = pc_cencode_opts_default();
            pc_buf b;
            pc_cimage back;
            o.tiff_compression = (pc_tiff_compression)c;
            b = enc(&img, PC_CF_TIFF, &o);
            back = dec(b.data, b.len);
            CHECK(back.len == img.len && memcmp(back.data, img.data, img.len) == 0);
            pc_cimage_fini(&back);
            pc_buf_fini(&b);
        }
        pc_cimage_fini(&img);
    }
    {
        truth t = make_truth(300 * 260 * 3, 77);
        uint8_t *f = tg_u16s(t.u16s, t.n, false);
        tg_dir d = tg_base(300, 260, 16, 3, 2);
        tg_built b;
        pc_cimage img;
        d.tiled = true;
        tg_tile_split(&d, f, 300, 260, 3, 2, 64, 32, false);
        tg_tag1(&d, 322, TG_SHORT, 64);
        tg_tag1(&d, 323, TG_SHORT, 32);
        free(f);
        b = build1(false, true, &d);
        img = decode_ok(&b);
        CHECK(img.len == 2 * t.n && memcmp(img.data, t.u16s, 2 * t.n) == 0);
        pc_cimage_fini(&img);
        tg_built_free(&b);
        truth_free(&t);
    }
}

static void test_bigtiff_is_written_on_request_and_reads_back(void)
{
    pc_cimage img = synth(23, 17, PC_CL_RGBA, PC_CS_U16, 2, 0.3f);
    static const uint8_t layers[] = "Adobe Photoshop Document Data Block\0MIB8ryaL";
    static const uint8_t res[] = "8BIM\x04\x04\0\0\0\0\0\0";
    int c;
    set_blob(&img.meta.photoshop_layers, layers, sizeof layers - 1);
    set_blob(&img.meta.photoshop_resources, res, sizeof res - 1);
    for (c = 0; c < 3; c++) {
        static const pc_tiff_compression CS[3] = {PC_TIFF_NONE, PC_TIFF_DEFLATE, PC_TIFF_LZW};
        pc_cencode_opts o = pc_cencode_opts_default();
        pc_buf classic;
        pc_buf big;
        pc_tiff_info info;
        pc_cimage back;
        const uint8_t *r;
        const uint8_t *l;
        size_t rl;
        size_t ll;
        o.tiff_compression = CS[c];
        classic = enc(&img, PC_CF_TIFF, &o);
        o.tiff_bigtiff = true;
        big = enc(&img, PC_CF_TIFF, &o);
        CHECK(classic.len > 4 && big.len > 4);
        CHECK((classic.data[0] == 'I' ? classic.data[2] : classic.data[3]) == 42);
        CHECK((big.data[0] == 'I' ? big.data[2] : big.data[3]) == 43);
        CHECK(pc_ctiff_info(big.data, big.len, &info, NULL) == PC_OK && info.big_tiff);
        pc_tiff_info_fini(&info);
        back = dec(big.data, big.len);
        CHECK(back.len == img.len && memcmp(back.data, img.data, img.len) == 0);
        CHECK(pc_meta_blob_eq(&back.meta.photoshop_layers, &img.meta.photoshop_layers));
        CHECK(pc_meta_blob_eq(&back.meta.photoshop_resources, &img.meta.photoshop_resources));
        pc_ctiff_photoshop_tags(big.data, big.len, &r, &rl, &l, &ll);
        CHECK(r != NULL && rl == sizeof res - 1 && memcmp(r, res, rl) == 0);
        CHECK(l != NULL && ll == sizeof layers - 1 && memcmp(l, layers, ll) == 0);
        pc_cimage_fini(&back);
        pc_buf_fini(&classic);
        pc_buf_fini(&big);
    }
    pc_cimage_fini(&img);
}

/* Small files covering every structure: multi-IFD, SubIFDs, BigTIFF both orders, tiles, planar. */
static size_t zoo(tg_built *out)
{
    truth t = make_truth(6 * 5 * 4, 5);
    size_t n = 0;
    int o;
    for (o = 0; o < 4; o++) {
        bool little = ORDERS[o][0];
        bool big = ORDERS[o][1];
        tg_dir dirs[4];
        static const size_t chain[3] = {0, 1, 2};
        static const size_t sub3[1] = {3};
        uint8_t *u16f = tg_u16s(t.u16s, t.n, little);
        static const uint8_t a5[3] = {0xa5, 0xa5, 0xa5};
        int i;
        dirs[0] = gray_page(3, 2, 1, 1);
        dirs[1] = tg_base(6, 5, 8, 3, 2);
        dirs[1].tiled = true;
        tg_tile_split(&dirs[1], t.u8s, 6, 5, 3, 1, 16, 16, true);
        tg_tag1(&dirs[1], 322, TG_SHORT, 16);
        tg_tag1(&dirs[1], 323, TG_SHORT, 16);
        tg_tag1(&dirs[1], 284, TG_SHORT, 2);
        tg_sub_ifds(&dirs[1], big ? TG_IFD8 : TG_IFD, sub3, 1);
        dirs[2] = tg_base(6, 5, 16, 4, 2);
        tg_planar_strips(&dirs[2], u16f, 6, 5, 4, 2, 2);
        tg_tag1(&dirs[2], 278, TG_SHORT, 2);
        tg_tag1(&dirs[2], 284, TG_SHORT, 2);
        tg_tag1(&dirs[2], 338, TG_SHORT, 1);
        dirs[3] = gray_page(2, 2, 4, 1);
        tg_tag1(&dirs[3], 274, TG_SHORT, 6);
        out[n++] = tg_build(little, big, dirs, 4, chain, 3);
        for (i = 0; i < 4; i++) {
            tg_dir_free(&dirs[i]);
        }
        free(u16f);
        dirs[0] = tg_strips(5, 3, 1, 1, 0, 2, a5, 3);
        out[n++] = build1(little, big, &dirs[0]);
    }
    truth_free(&t);
    return n;
}

static void poke(const uint8_t *b, size_t n)
{
    pc_cimage img;
    pc_cdecode_opts small = pc_cdecode_opts_default();
    pc_tiff_info info;
    small.limits.max_alloc = 16u << 20;
    if (pc_cdecode(b, n, &img, NULL) == PC_OK) {
        pc_cimage_fini(&img);
    }
    if (pc_cdecode_with(b, n, &small, &img, NULL) == PC_OK) {
        pc_cimage_fini(&img);
    }
    (void)pc_exif_orientation(b, n);
    (void)pc_ctiff_orientation(b, n);
    if (pc_ctiff_info(b, n, &info, NULL) == PC_OK) {
        size_t i;
        pc_cdecode_opts o = pc_cdecode_opts_default();
        for (i = 0; i < info.npages && i < 8; i++) {
            if (pc_cdecode_tiff_page(b, n, i, &o, &img, NULL) == PC_OK) {
                pc_cimage_fini(&img);
            }
        }
        pc_tiff_info_fini(&info);
    }
}

static void test_zoo_decodes_and_never_panics_when_cut_or_corrupted(void)
{
    tg_built z[8];
    size_t nz = zoo(z);
    size_t k;
    for (k = 0; k < nz; k++) {
        const tg_built *f = &z[k];
        pc_tiff_info info;
        uint8_t *g = malloc(f->len);
        rng r = {f->len};
        size_t i;
        int it;
        CHECK_EQ_INT(pc_ctiff_info(f->bytes, f->len, &info, NULL), PC_OK);
        for (i = 0; i < info.npages; i++) {
            pc_cdecode_opts o = pc_cdecode_opts_default();
            pc_cimage img;
            CHECK_EQ_INT(pc_cdecode_tiff_page(f->bytes, f->len, i, &o, &img, NULL), PC_OK);
            pc_cimage_fini(&img);
        }
        pc_tiff_info_fini(&info);
        for (i = 0; i < f->len; i++) {
            poke(f->bytes, i);
        }
        for (i = 0; i < f->len; i++) {
            static const uint8_t FL[3] = {0xff, 0x80, 0x01};
            int q;
            for (q = 0; q < 3; q++) {
                memcpy(g, f->bytes, f->len);
                g[i] ^= FL[q];
                poke(g, f->len);
            }
        }
        for (it = 0; it < 300; it++) {
            int q;
            memcpy(g, f->bytes, f->len);
            for (q = 0; q < 4; q++) {
                g[rng_next(&r) % f->len] = (uint8_t)rng_next(&r);
            }
            poke(g, f->len);
        }
        free(g);
    }
    for (k = 0; k < nz; k++) {
        tg_built_free(&z[k]);
    }
}

/* ---- Photoshop's private tags (tiff_photoshop.rs) ---- */

static pc_cimage with_tags(pc_clayout l, pc_csample s)
{
    static const uint8_t res[] = "8BIM\x03\xed\x00\x00\x00\x00\x00\x10\x00\x48\x00\x00\x00\x01\x00\x01\x00\x48\x00"
                                 "\x00\x00\x01\x00\x01";
    uint8_t layers[36 + 8 + 8];
    pc_cimage img = synth(9, 7, l, s, 5, 0.3f);
    set_blob(&img.meta.photoshop_resources, res, sizeof res - 1);
    memcpy(layers, "Adobe Photoshop Document Data Block\0", 36);
    memcpy(layers + 36, "MIB8ryaL", 8);
    memcpy(layers + 44, "\x04\0\0\0\0\0\0\0", 8);
    set_blob(&img.meta.photoshop_layers, layers, sizeof layers);
    return img;
}

static void test_tags_round_trip_through_the_encoder(void)
{
    static const struct {
        pc_clayout l;
        pc_csample s;
    } C[4] = {{PC_CL_RGB, PC_CS_U8}, {PC_CL_RGBA, PC_CS_U16}, {PC_CL_GRAY, PC_CS_F32}, {PC_CL_CMYKA, PC_CS_U8}};
    int i;
    int c;
    for (i = 0; i < 4; i++) {
        for (c = 0; c < 4; c++) {
            pc_cimage img = with_tags(C[i].l, C[i].s);
            pc_cencode_opts o = pc_cencode_opts_default();
            pc_buf b;
            pc_cimage back;
            const uint8_t *r;
            const uint8_t *l;
            size_t rl;
            size_t ll;
            o.tiff_compression = (pc_tiff_compression)c;
            b = enc(&img, PC_CF_TIFF, &o);
            CHECK((b.data[0] == 'I') == pc_ctiff_writes_little_endian());
            pc_ctiff_photoshop_tags(b.data, b.len, &r, &rl, &l, &ll);
            CHECK(r != NULL && rl == img.meta.photoshop_resources.len &&
                  memcmp(r, img.meta.photoshop_resources.data, rl) == 0);
            CHECK(l != NULL && ll == img.meta.photoshop_layers.len && memcmp(l, img.meta.photoshop_layers.data, ll) == 0);
            back = dec(b.data, b.len);
            CHECK(pc_meta_blob_eq(&back.meta.photoshop_resources, &img.meta.photoshop_resources));
            CHECK(pc_meta_blob_eq(&back.meta.photoshop_layers, &img.meta.photoshop_layers));
            CHECK(back.len == img.len && memcmp(back.data, img.data, img.len) == 0);
            pc_cimage_fini(&back);
            pc_buf_fini(&b);
            pc_cimage_fini(&img);
        }
    }
}

static void test_absent_and_empty_tags_are_not_written(void)
{
    pc_cimage img = synth(4, 4, PC_CL_RGB, PC_CS_U8, 1, 0.0f);
    pc_buf b = enc(&img, PC_CF_TIFF, NULL);
    const uint8_t *r;
    const uint8_t *l;
    size_t rl;
    size_t ll;
    pc_cimage back;
    pc_ctiff_photoshop_tags(b.data, b.len, &r, &rl, &l, &ll);
    CHECK(r == NULL && l == NULL);
    back = dec(b.data, b.len);
    CHECK(!back.meta.photoshop_layers.has);
    pc_cimage_fini(&back);
    pc_buf_fini(&b);
    set_blob(&img.meta.photoshop_layers, "", 0);
    set_blob(&img.meta.photoshop_resources, "", 0);
    b = enc(&img, PC_CF_TIFF, NULL);
    pc_ctiff_photoshop_tags(b.data, b.len, &r, &rl, &l, &ll);
    CHECK(r == NULL && l == NULL);
    pc_buf_fini(&b);
    pc_cimage_fini(&img);
}

/* A minimal 2x1 gray TIFF with a 37724 tag (inline when it fits, else after the IFD). */
static pc_buf hand_made(bool little, bool big, const uint8_t *payload, size_t plen)
{
    pc_buf f = PC_BUF_INIT;
    size_t header = big ? 16 : 8;
    size_t pixels_at = header;
    size_t ifd_at = pixels_at + 2;
    size_t entry_len = big ? 20 : 12;
    size_t inl = big ? 8 : 4;
    size_t count_len = big ? 8 : 2;
    size_t next_len = big ? 8 : 4;
    size_t data_at;
    size_t i;
    static const uint64_t tags[10][4] = {{256, 3, 1, 2},   {257, 3, 1, 1}, {258, 3, 1, 8}, {259, 3, 1, 1},
                                         {262, 3, 1, 1},   {273, 4, 1, 0}, {277, 3, 1, 1}, {278, 3, 1, 1},
                                         {279, 4, 1, 2},   {37724, 7, 0, 0}};
#define W(v, k)                                                                                    \
    do {                                                                                           \
        uint8_t t_[8];                                                                             \
        size_t q_;                                                                                 \
        for (q_ = 0; q_ < (k); q_++) {                                                             \
            t_[q_] = (uint8_t)((uint64_t)(v) >> (8 * (little ? q_ : ((size_t)(k)-1 - q_)))); \
        }                                                                                          \
        (void)pc_buf_append(&f, t_, (k), NULL);                                                    \
    } while (0)
    (void)pc_buf_append(&f, little ? "II" : "MM", 2, NULL);
    W(big ? 43 : 42, 2);
    if (big) {
        W(8, 2);
        W(0, 2);
        W(ifd_at, 8);
    } else {
        W(ifd_at, 4);
    }
    (void)pc_buf_append(&f, "\x40\xc0", 2, NULL);
    data_at = ifd_at + count_len + 10 * entry_len + next_len;
    W(10, count_len);
    for (i = 0; i < 10; i++) {
        uint64_t tag = tags[i][0];
        uint64_t ty = tags[i][1];
        uint64_t count = tag == 37724 ? plen : tags[i][2];
        uint64_t value = tag == 273 ? pixels_at : tags[i][3];
        uint8_t field[8] = {0};
        W(tag, 2);
        W(ty, 2);
        W(count, big ? 8 : 4);
        if (tag == 37724) {
            if (plen <= inl) {
                memcpy(field, payload, plen);
                (void)pc_buf_append(&f, field, inl, NULL);
            } else {
                W(data_at, inl);
            }
        } else if (ty == 3) {
            W(value, 2);
            (void)pc_buf_append(&f, field, inl - 2, NULL);
        } else {
            W(value, inl);
        }
    }
    W(0, next_len);
    if (plen > inl) {
        (void)pc_buf_append(&f, payload, plen, NULL);
    }
#undef W
    return f;
}

static void test_hand_made_files_in_every_layout(void)
{
    static const char long_[] = "Adobe Photoshop Document Data Block\0MIB8";
    const struct {
        const uint8_t *p;
        size_t n;
    } P[4] = {{(const uint8_t *)"ab", 2},
              {(const uint8_t *)"abcd", 4},
              {(const uint8_t *)"abcdefgh", 8},
              {(const uint8_t *)long_, sizeof long_ - 1}};
    int little;
    int big;
    int i;
    for (little = 0; little < 2; little++) {
        for (big = 0; big < 2; big++) {
            for (i = 0; i < 4; i++) {
                pc_buf f = hand_made(little != 0, big != 0, P[i].p, P[i].n);
                const uint8_t *r;
                const uint8_t *l;
                size_t rl;
                size_t ll;
                pc_cimage img;
                pc_ctiff_photoshop_tags(f.data, f.len, &r, &rl, &l, &ll);
                CHECK(r == NULL);
                CHECK(l != NULL && ll == P[i].n && memcmp(l, P[i].p, ll) == 0);
                img = dec(f.data, f.len);
                if (img.width != 2) {
                    PC_T_FAIL("little %d big %d len %zu", little, big, P[i].n);
                }
                CHECK(img.width == 2 && img.height == 1);
                CHECK(img.meta.photoshop_layers.has && img.meta.photoshop_layers.len == P[i].n &&
                      memcmp(img.meta.photoshop_layers.data, P[i].p, P[i].n) == 0);
                pc_cimage_fini(&img);
                pc_buf_fini(&f);
            }
        }
    }
}

static void test_malformed_tags_read_as_absent_without_panicking(void)
{
    uint8_t lg[40];
    int little;
    int big;
    const uint8_t *r;
    const uint8_t *l;
    size_t rl;
    size_t ll;
    memset(lg, 7, sizeof lg);
    for (little = 0; little < 2; little++) {
        for (big = 0; big < 2; big++) {
            pc_buf f = hand_made(little != 0, big != 0, lg, 40);
            size_t cut;
            size_t i;
            pc_cimage img;
            uint8_t *g = malloc(f.len);
            for (cut = 0; cut < f.len; cut++) {
                pc_ctiff_photoshop_tags(f.data, cut, &r, &rl, &l, &ll);
                CHECK(l == NULL || ll == 40);
                if (pc_cdecode(f.data, cut, &img, NULL) == PC_OK) {
                    pc_cimage_fini(&img);
                }
            }
            for (i = 0; i + 40 < f.len; i++) {
                memcpy(g, f.data, f.len);
                g[i] ^= 0xff;
                pc_ctiff_photoshop_tags(g, f.len, &r, &rl, &l, &ll);
                if (pc_cdecode(g, f.len, &img, NULL) == PC_OK) {
                    pc_cimage_fini(&img);
                }
            }
            free(g);
            pc_buf_fini(&f);
        }
    }
    pc_ctiff_photoshop_tags((const uint8_t *)"", 0, &r, &rl, &l, &ll);
    CHECK(r == NULL && l == NULL);
    pc_ctiff_photoshop_tags((const uint8_t *)"II*\0", 4, &r, &rl, &l, &ll);
    CHECK(r == NULL && l == NULL);
    pc_ctiff_photoshop_tags((const uint8_t *)"MM\0*\xff\xff\xff\xff", 8, &r, &rl, &l, &ll);
    CHECK(r == NULL && l == NULL);
    {
        pc_buf f = hand_made(true, false, lg, 40);
        size_t at;
        for (at = 0; at + 1 < f.len; at++) {
            if (f.data[at] == (37724 & 0xff) && f.data[at + 1] == (37724 >> 8)) {
                break;
            }
        }
        f.data[at + 4] = f.data[at + 5] = f.data[at + 6] = f.data[at + 7] = 0xff;
        pc_ctiff_photoshop_tags(f.data, f.len, &r, &rl, &l, &ll);
        CHECK(l == NULL);
        pc_buf_fini(&f);
    }
}

/* ---- regressions (tiff_regressions.rs) ---- */

static void test_white_is_zero_is_normalized_once(void)
{
    static const uint8_t pixels[4] = {0, 64, 192, 255};
    static const uint8_t want[4] = {255, 191, 63, 0};
    static const uint32_t E[9][3] = {{256, 3, 4}, {257, 3, 1}, {258, 3, 8}, {259, 3, 1}, {262, 3, 0},
                                     {273, 4, 122}, {277, 3, 1}, {278, 3, 1}, {279, 4, 4}};
    uint8_t f[126];
    size_t at = 10;
    int i;
    pc_cimage img;
    memcpy(f, "II*\0\x08\0\0\0\x09\0", 10);
    for (i = 0; i < 9; i++) {
        f[at] = (uint8_t)E[i][0];
        f[at + 1] = (uint8_t)(E[i][0] >> 8);
        f[at + 2] = (uint8_t)E[i][1];
        f[at + 3] = 0;
        f[at + 4] = 1;
        f[at + 5] = f[at + 6] = f[at + 7] = 0;
        f[at + 8] = (uint8_t)E[i][2];
        f[at + 9] = (uint8_t)(E[i][2] >> 8);
        f[at + 10] = f[at + 11] = 0;
        at += 12;
    }
    memset(f + at, 0, 4);
    at += 4;
    memcpy(f + at, pixels, 4);
    img = dec(f, at + 4);
    CHECK(img.width == 4 && img.height == 1 && img.len == 4 && memcmp(img.data, want, 4) == 0);
    pc_cimage_fini(&img);
}

static void test_big_tiff_orientation_6_and_8_are_applied(void)
{
    static const uint8_t upright[6] = {1, 2, 3, 4, 5, 6};
    static const uint8_t s6[6] = {3, 6, 2, 5, 1, 4};
    static const uint8_t s8[6] = {4, 1, 5, 2, 6, 3};
    int k;
    for (k = 0; k < 2; k++) {
        uint16_t o = k == 0 ? 6 : 8;
        tg_dir d = tg_strips(2, 3, 8, 1, 1, 3, k == 0 ? s6 : s8, 6);
        tg_built b;
        pc_cimage img;
        uint8_t *up = NULL;
        size_t un = 0;
        tg_tag1(&d, 274, TG_SHORT, o);
        b = build1(true, true, &d);
        CHECK_EQ_UINT(pc_exif_orientation(b.bytes, b.len), o);
        CHECK_EQ_INT(pc_upright_exif(b.bytes, b.len, &up, &un, NULL), PC_OK);
        CHECK(up != NULL && pc_exif_orientation(up, un) == 1);
        free(up);
        img = decode_ok(&b);
        CHECK(img.width == 3 && img.height == 2 && img.len == 6 && memcmp(img.data, upright, 6) == 0);
        pc_cimage_fini(&img);
        tg_built_free(&b);
    }
}

/* ---- camera raw (camera_raw.rs) ---- */

static pc_buf raw_tiff(uint32_t (*const *ifds)[3], const size_t *counts, size_t nifd, const uint8_t *header8)
{
    pc_buf b = PC_BUF_INIT;
    uint32_t first = header8 != NULL ? 16 : 8;
    uint32_t sizes[4];
    uint32_t strip;
    uint32_t at = first;
    size_t i;
    (void)pc_buf_append(&b, "II*\0", 4, NULL);
    (void)pc_buf_append(&b, &first, 4, NULL);
    if (header8 != NULL) {
        uint32_t z = 0;
        (void)pc_buf_append(&b, header8, 4, NULL);
        (void)pc_buf_append(&b, &z, 4, NULL);
    }
    strip = first;
    for (i = 0; i < nifd; i++) {
        sizes[i] = (uint32_t)(2 + 12 * (counts[i] + 1) + 4);
        strip += sizes[i];
    }
    for (i = 0; i < nifd; i++) {
        uint32_t e[16][3];
        size_t n = counts[i];
        size_t k;
        uint16_t cnt16 = (uint16_t)(n + 1);
        uint32_t next;
        memcpy(e, ifds[i], n * sizeof e[0]);
        e[n][0] = 273;
        e[n][1] = 4;
        e[n][2] = strip;
        n++;
        /* sort by tag */
        for (k = 1; k < n; k++) {
            size_t j = k;
            while (j > 0 && e[j - 1][0] > e[j][0]) {
                uint32_t t[3];
                memcpy(t, e[j], sizeof t);
                memcpy(e[j], e[j - 1], sizeof t);
                memcpy(e[j - 1], t, sizeof t);
                j--;
            }
        }
        (void)pc_buf_append(&b, &cnt16, 2, NULL);
        for (k = 0; k < n; k++) {
            uint16_t tag = (uint16_t)e[k][0];
            uint16_t ty = (uint16_t)e[k][1];
            uint32_t count = ty == 1 ? 4 : 1;
            (void)pc_buf_append(&b, &tag, 2, NULL);
            (void)pc_buf_append(&b, &ty, 2, NULL);
            (void)pc_buf_append(&b, &count, 4, NULL);
            if (ty == 3) {
                uint16_t v = (uint16_t)e[k][2];
                uint16_t z = 0;
                (void)pc_buf_append(&b, &v, 2, NULL);
                (void)pc_buf_append(&b, &z, 2, NULL);
            } else {
                (void)pc_buf_append(&b, &e[k][2], 4, NULL);
            }
        }
        at += sizes[i];
        next = i + 1 < nifd ? at : 0;
        (void)pc_buf_append(&b, &next, 4, NULL);
    }
    {
        uint8_t px[12];
        memset(px, 128, sizeof px);
        (void)pc_buf_append(&b, px, 12, NULL);
    }
    return b;
}

static void assert_camera_raw(const pc_buf *b)
{
    pc_cformat f;
    pc_cimage img;
    pc_error e;
    CHECK(pc_cformat_detect(b->data, b->len, &f) && f == PC_CF_TIFF);
    if (pc_cdecode(b->data, b->len, &img, &e) == PC_OK) {
        PC_T_FAIL("decoded a camera raw file as %ux%u", (unsigned)img.width, (unsigned)img.height);
        pc_cimage_fini(&img);
        return;
    }
    CHECK(e.code == PC_ERR_UNSUPPORTED);
    CHECK_CONTAINS(e.msg, "camera raw");
}

#define IMG_IFD(name, photometric, samples)                                                        \
    uint32_t name[9][3] = {{256, 3, 2}, {257, 3, 2}, {258, 3, 8}, {259, 3, 1}, {262, 3, photometric}, \
                           {277, 3, samples}, {278, 3, 2}, {279, 4, 4 * (samples)}, {0, 0, 0}}

static void test_camera_raw(void)
{
    uint32_t(*ifds[2])[3];
    size_t counts[2];
    pc_buf b;
    {
        IMG_IFD(cfa, 32803, 1);
        ifds[0] = cfa;
        counts[0] = 8;
        b = raw_tiff(ifds, counts, 1, NULL);
        assert_camera_raw(&b);
        pc_buf_fini(&b);
    }
    {
        IMG_IFD(lin, 34892, 3);
        ifds[0] = lin;
        counts[0] = 8;
        b = raw_tiff(ifds, counts, 1, NULL);
        assert_camera_raw(&b);
        pc_buf_fini(&b);
    }
    {
        /* raw behind a preview: IFD0 an RGB preview, the CFA a SubIFD */
        IMG_IFD(ifd0, 2, 3);
        IMG_IFD(cfa, 32803, 1);
        uint32_t size0 = 2 + 12 * (8 + 2) + 4;
        size_t next_at = 8 + size0 - 4;
        ifd0[8][0] = 330;
        ifd0[8][1] = 4;
        ifd0[8][2] = 8 + size0;
        ifds[0] = ifd0;
        ifds[1] = cfa;
        counts[0] = 9;
        counts[1] = 8;
        b = raw_tiff(ifds, counts, 2, NULL);
        memset(b.data + next_at, 0, 4);
        assert_camera_raw(&b);
        pc_buf_fini(&b);
    }
    {
        IMG_IFD(ifd0, 2, 3);
        ifd0[8][0] = 50706;
        ifd0[8][1] = 1;
        ifd0[8][2] = 0x00000401;
        ifds[0] = ifd0;
        counts[0] = 9;
        b = raw_tiff(ifds, counts, 1, NULL);
        assert_camera_raw(&b);
        pc_buf_fini(&b);
    }
    {
        IMG_IFD(ifd0, 6, 3);
        ifds[0] = ifd0;
        counts[0] = 8;
        b = raw_tiff(ifds, counts, 1, (const uint8_t *)"CR\x02\0");
        assert_camera_raw(&b);
        {
            size_t n;
            for (n = 0; n < b.len; n++) {
                pc_cimage img;
                if (pc_cdecode(b.data, n, &img, NULL) == PC_OK) {
                    pc_cimage_fini(&img);
                }
            }
        }
        pc_buf_fini(&b);
    }
    {
        IMG_IFD(rgb, 2, 3);
        IMG_IFD(gray, 1, 1);
        pc_cimage img;
        ifds[0] = rgb;
        counts[0] = 8;
        b = raw_tiff(ifds, counts, 1, NULL);
        img = dec(b.data, b.len);
        CHECK(img.width == 2 && img.height == 2);
        pc_cimage_fini(&img);
        pc_buf_fini(&b);
        ifds[0] = gray;
        b = raw_tiff(ifds, counts, 1, NULL);
        img = dec(b.data, b.len);
        CHECK(img.width == 2 && img.height == 2);
        pc_cimage_fini(&img);
        pc_buf_fini(&b);
    }
}

int main(int argc, char **argv)
{
    RUN(test_bigtiff_and_classic_in_both_byte_orders_at_every_depth);
    RUN(test_tiles_with_padding_in_both_planar_configurations);
    RUN(test_planar_strips_at_every_depth);
    RUN(test_extra_samples_that_are_not_alpha_are_dropped);
    RUN(test_alpha_after_an_unspecified_extra_sample_is_kept);
    RUN(test_associated_alpha_is_made_straight);
    RUN(test_white_is_zero_opens_with_the_right_tones);
    RUN(test_bigtiff_orientation_is_applied_like_classic_tiff);
    RUN(test_palette_images_open_as_rgb);
    RUN(test_wide_and_odd_sample_formats);
    RUN(test_every_ifd_and_subifd_is_listed_and_decodable);
    RUN(test_a_file_of_only_thumbnails_still_opens);
    RUN(test_cyclic_chains_and_subifds_terminate);
    RUN(test_the_chain_walk_is_capped);
    RUN(test_image_data_cut_off_decodes_with_a_warning);
    RUN(test_hostile_sizes_and_counts_are_refused_without_allocating);
    RUN(test_banded_decode_matches_the_encoder_on_larger_images);
    RUN(test_bigtiff_is_written_on_request_and_reads_back);
    RUN(test_zoo_decodes_and_never_panics_when_cut_or_corrupted);
    RUN(test_tags_round_trip_through_the_encoder);
    RUN(test_absent_and_empty_tags_are_not_written);
    RUN(test_hand_made_files_in_every_layout);
    RUN(test_malformed_tags_read_as_absent_without_panicking);
    RUN(test_white_is_zero_is_normalized_once);
    RUN(test_big_tiff_orientation_6_and_8_are_applied);
    RUN(test_camera_raw);
    return pc_t_finish();
}
