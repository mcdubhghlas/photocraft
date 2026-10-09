/* Ports crates/psd/tests/{roundtrip,builder,tree,malformed,proptests,layer_block_order}.rs
 * (proptests become seeded random loops). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "base/bytes.h"
#include "harness.h"
#include "psd/builder.h"
#include "psd/misc.h"
#include "psd/pixels.h"
#include "testgen.h"

static uint64_t rng_state = 0x243F6A8885A308D3u;
static uint64_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* to_bytes → parse → equal model → to_bytes again equal bytes. */
static bool stable(const pc_psd_file *f, const char *name)
{
    pc_psd_bytes b = {NULL, 0};
    pc_psd_bytes again = {NULL, 0};
    pc_psd_file p;
    pc_error e;
    bool ok = false;
    if (pc_psd_file_to_bytes(f, &b, &e) != PC_OK) {
        pc_t_name = name;
        PC_T_FAIL("write failed: %s", e.msg);
        return false;
    }
    if (pc_psd_file_parse(b.data, b.len, &p, &e) != PC_OK) {
        pc_t_name = name;
        PC_T_FAIL("parse failed: %s", e.msg);
        pc_psd_bytes_fini(&b);
        return false;
    }
    ok = pc_psd_file_eq(&p, f) && pc_psd_file_to_bytes(&p, &again, NULL) == PC_OK && pc_psd_bytes_eq(&again, &b);
    if (!ok) {
        pc_t_name = name;
        PC_T_FAIL("%s", "not stable");
    }
    pc_psd_file_fini(&p);
    pc_psd_bytes_fini(&b);
    pc_psd_bytes_fini(&again);
    return ok;
}

static pc_psd_bytes bytes_of(const pc_psd_file *f)
{
    pc_psd_bytes b = {NULL, 0};
    CHECK_EQ_INT(pc_psd_file_to_bytes(f, &b, NULL), PC_OK);
    return b;
}

/* ---- roundtrip.rs ---- */

static void test_all_generated_cases(void)
{
    size_t n;
    size_t i;
    tg_case *c = tg_all_cases(&n);
    int unstable = 0;
    CHECK(n > 100);
    for (i = 0; i < n; i++) {
        const pc_psd_file *f = &c[i].file;
        size_t nl;
        size_t li;
        size_t k;
        const pc_psd_layer *layers = pc_psd_file_layers(f, &nl);
        pc_psd_bytes m = {NULL, 0};
        unstable += !stable(f, c[i].name);
        CHECK_EQ_INT(pc_psd_file_decode_merged(f, &m, NULL), PC_OK);
        CHECK_EQ_UINT(m.len, pc_psd_header_row_bytes(&f->header) * f->header.height * f->header.channels);
        pc_psd_bytes_fini(&m);
        for (li = 0; li < nl; li++) {
            for (k = 0; k < layers[li].nchannels; k++) {
                pc_psd_bytes d = {NULL, 0};
                pc_status st = pc_psd_layer_decode_channel(&layers[li], layers[li].channels[k].id, f->header.depth,
                                                           f->header.version, &d, NULL);
                CHECK_EQ_INT(st, PC_OK);
                pc_psd_bytes_fini(&d);
            }
        }
    }
    CHECK_EQ_INT(unstable, 0);
    tg_free_cases(c, n);
}

static void test_merged_only_decodes_to_pattern(void)
{
    int m;
    for (m = 0; m < TG_MODES; m++) {
        uint16_t d[3];
        size_t nd = tg_mode_depths(TG_MODE_LIST[m], d);
        size_t j;
        uint16_t c;
        for (j = 0; j < nd; j++) {
            for (c = 0; c < 4; c++) {
                pc_psd_file f = tg_merged_only(PC_PSD_V1, TG_MODE_LIST[m], d[j], c, 9, 5);
                pc_psd_bytes dec = {NULL, 0};
                pc_buf want = PC_BUF_INIT;
                uint16_t ch;
                for (ch = 0; ch < tg_mode_channels(TG_MODE_LIST[m]); ch++) {
                    pc_psd_bytes p = tg_pattern_plane(9, 5, d[j], ch);
                    pc_buf_append(&want, p.data, p.len, NULL);
                    pc_psd_bytes_fini(&p);
                }
                CHECK_EQ_INT(pc_psd_file_decode_merged(&f, &dec, NULL), PC_OK);
                CHECK(dec.len == want.len && memcmp(dec.data, want.data, want.len) == 0);
                pc_psd_bytes_fini(&dec);
                pc_buf_fini(&want);
                pc_psd_file_fini(&f);
            }
        }
    }
}

static void test_layered_and_merged_cases(void)
{
    static const struct {
        int v;
        uint16_t mode;
        uint16_t depth;
        uint16_t comp;
    } L[] = {{1, 3, 8, 0},  {1, 3, 8, 1},  {1, 3, 8, 2},  {1, 3, 8, 3},  {1, 3, 16, 0}, {1, 3, 16, 1}, {1, 3, 16, 2},
             {1, 3, 16, 3}, {1, 3, 32, 0}, {1, 3, 32, 1}, {1, 3, 32, 2}, {1, 3, 32, 3}, {2, 3, 8, 0},  {2, 3, 8, 1},
             {2, 3, 8, 2},  {2, 3, 8, 3},  {2, 3, 16, 1}, {2, 3, 32, 3}, {1, 1, 8, 1},  {1, 1, 16, 3}, {1, 1, 32, 2},
             {1, 4, 8, 1},  {1, 4, 16, 0}, {2, 4, 8, 2},  {1, 9, 8, 1},  {2, 9, 16, 3}};
    size_t i;
    int m;
    for (i = 0; i < sizeof L / sizeof L[0]; i++) {
        pc_psd_file f = tg_layered((pc_psd_version)L[i].v, L[i].mode, L[i].depth, L[i].comp);
        size_t nl;
        (void)pc_psd_file_layers(&f, &nl);
        CHECK(stable(&f, "layered"));
        CHECK(nl > 30);
        CHECK((f.placement.where == PC_PSD_LI_GLOBAL_BLOCK) == (L[i].depth != 8));
        pc_psd_file_fini(&f);
    }
    for (m = 0; m < TG_MODES; m++) {
        uint16_t d[3];
        size_t nd = tg_mode_depths(TG_MODE_LIST[m], d);
        size_t j;
        int v;
        uint16_t c;
        for (j = 0; j < nd; j++) {
            for (v = 1; v <= 2; v++) {
                for (c = 0; c < 4; c++) {
                    pc_psd_file f = tg_merged_only((pc_psd_version)v, TG_MODE_LIST[m], d[j], c, 31, 3);
                    CHECK(stable(&f, "merged"));
                    CHECK(!f.has_layer_info);
                    pc_psd_file_fini(&f);
                }
            }
        }
    }
}

static void test_psb_lengths(void)
{
    pc_psd_file f = tg_small(PC_PSD_V1, PC_PSD_RAW);
    pc_psd_file g = tg_small(PC_PSD_V2, PC_PSD_RAW);
    pc_psd_bytes a = bytes_of(&f);
    pc_psd_bytes b = bytes_of(&g);
    size_t nl;
    size_t i;
    size_t nch = 0;
    const pc_psd_layer *layers = pc_psd_file_layers(&f, &nl);
    pc_psd_file m1;
    pc_psd_file m2;
    for (i = 0; i < nl; i++) {
        nch += layers[i].nchannels;
    }
    CHECK_EQ_UINT(b.len, a.len + 8 + nch * 4);
    CHECK_EQ_UINT(b.data[5], 2);
    pc_psd_bytes_fini(&a);
    pc_psd_bytes_fini(&b);
    pc_psd_file_fini(&f);
    pc_psd_file_fini(&g);
    m1 = tg_merged_only(PC_PSD_V1, PC_PSD_MODE_GRAYSCALE, 8, PC_PSD_RLE, 5, 4);
    m2 = tg_merged_only(PC_PSD_V2, PC_PSD_MODE_GRAYSCALE, 8, PC_PSD_RLE, 5, 4);
    CHECK_EQ_UINT(m2.image_data.len, m1.image_data.len + 4 * 2);
    pc_psd_file_fini(&m1);
    pc_psd_file_fini(&m2);
}

static void push_global(pc_psd_file *f, const char key[4], const void *d, size_t n)
{
    tg_global(f, key, d, n);
}

static void test_psb_long_key_global_block(void)
{
    static const uint8_t a[4] = {1, 2, 3, 4};
    static const uint8_t b2[2] = {5, 6};
    pc_psd_file f = tg_small(PC_PSD_V2, PC_PSD_RLE);
    pc_psd_file p;
    pc_psd_bytes b;
    push_global(&f, "FMsk", a, 4);
    push_global(&f, "Alph", b2, 2);
    b = bytes_of(&f);
    CHECK_EQ_INT(pc_psd_file_parse(b.data, b.len, &p, NULL), PC_OK);
    CHECK(pc_psd_file_eq(&p, &f));
    pc_psd_file_fini(&p);
    b.data[5] = 1; /* reading PSB bytes as PSD must not silently give the same structure */
    if (pc_psd_file_parse(b.data, b.len, &p, NULL) == PC_OK) {
        CHECK(!pc_psd_file_eq(&p, &f));
        pc_psd_file_fini(&p);
    }
    pc_psd_bytes_fini(&b);
    pc_psd_file_fini(&f);
}

static void test_edits_and_passthrough(void)
{
    pc_psd_file f = tg_small(PC_PSD_V1, PC_PSD_RLE);
    pc_psd_file p;
    pc_psd_bytes b;
    pc_psd_bytes d = {NULL, 0};
    pc_psd_layer *l = &f.layer_info.layers[0];
    size_t w;
    size_t h;
    uint8_t *nw;
    size_t k;
    char *name = NULL;
    uint8_t q3[256];
    static const uint8_t ab[7] = {0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab};
    static const uint8_t r5[5] = {1, 2, 3, 4, 5};
    pc_psd_resource res;
    pc_psd_block blk;
    /* edits are re-encoded */
    CHECK_EQ_INT(pc_psd_rect_size(l->rect, &w, &h, NULL), PC_OK);
    nw = malloc(w * h);
    memset(nw, 7, w * h);
    for (k = 0; k < l->nchannels; k++) {
        if (l->channels[k].id == 0) {
            pc_psd_channel c;
            CHECK_EQ_INT(pc_psd_channel_encode(0, PC_PSD_ZIP_PREDICT, nw, w * h, w, h, 8, PC_PSD_V1, &c, NULL), PC_OK);
            pc_psd_bytes_fini(&l->channels[k].data);
            l->channels[k] = c;
        }
    }
    b = bytes_of(&f);
    CHECK_EQ_INT(pc_psd_file_parse(b.data, b.len, &p, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_layer_decode_channel(&p.layer_info.layers[0], 0, 8, PC_PSD_V1, &d, NULL), PC_OK);
    CHECK(d.len == w * h && memcmp(d.data, nw, w * h) == 0);
    CHECK(pc_psd_layer_channel(&p.layer_info.layers[0], 0)->compression == PC_PSD_ZIP_PREDICT);
    pc_psd_bytes_fini(&d);
    pc_psd_file_fini(&p);
    pc_psd_bytes_fini(&b);
    free(nw);
    /* renaming changes luni */
    {
        pc_psd_block *luni = pc_psd_layer_block_mut(l, "luni");
        CHECK(luni != NULL);
        CHECK_EQ_INT(pc_psd_block_make_unicode_name("Renamed \xE2\x98\xBA", &blk, NULL), PC_OK);
        if (luni != NULL) {
            pc_psd_block_fini(luni);
            *luni = blk;
        }
    }
    b = bytes_of(&f);
    CHECK_EQ_INT(pc_psd_file_parse(b.data, b.len, &p, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_layer_name(&p.layer_info.layers[0], &name, NULL), PC_OK);
    CHECK_EQ_STR(name, "Renamed \xE2\x98\xBA");
    free(name);
    pc_psd_file_fini(&p);
    pc_psd_bytes_fini(&b);
    /* unknown blocks, resources and blend keys pass through */
    for (k = 0; k < 256; k++) {
        q3[k] = (uint8_t)k;
    }
    pc_psd_resource_init(&res, 9999, r5, 5, NULL);
    tg_push_res(&f, &res);
    push_global(&f, "Q3Dx", q3, 256);
    pc_psd_block_init(&blk, "Vid!", ab, 7, NULL);
    pc_psd_layer_push_block(&f.layer_info.layers[0], &blk, NULL);
    memcpy(f.layer_info.layers[0].blend, "new!", 4);
    CHECK(stable(&f, "unknown passthrough"));
    b = bytes_of(&f);
    CHECK_EQ_INT(pc_psd_file_parse(b.data, b.len, &p, NULL), PC_OK);
    CHECK(memcmp(p.layer_info.layers[0].blend, "new!", 4) == 0);
    CHECK(pc_psd_file_global_block(&p, "Q3Dx") != NULL && pc_psd_file_global_block(&p, "Q3Dx")->data.len == 256);
    pc_psd_file_fini(&p);
    pc_psd_bytes_fini(&b);
    pc_psd_file_fini(&f);
}

static void test_unknown_mode_compression_and_trailing(void)
{
    pc_psd_file f = tg_merged_only(PC_PSD_V1, PC_PSD_MODE_GRAYSCALE, 8, PC_PSD_RAW, 2, 2);
    pc_psd_file g = tg_small(PC_PSD_V1, PC_PSD_RAW);
    pc_psd_bytes d = {NULL, 0};
    static const uint8_t z6[6];
    f.header.color_mode = 5;
    CHECK(stable(&f, "unknown mode"));
    pc_psd_file_fini(&f);
    g.layer_info.layers[0].channels[0].compression = 9;
    CHECK(stable(&g, "unknown channel compression"));
    CHECK(pc_psd_layer_decode_channel(&g.layer_info.layers[0], g.layer_info.layers[0].channels[0].id, 8, PC_PSD_V1, &d,
                                      NULL) != PC_OK);
    pc_psd_file_fini(&g);
    g = tg_small(PC_PSD_V1, PC_PSD_RAW);
    pc_psd_bytes_set(&g.layer_mask_trailing, z6, 6, NULL);
    CHECK(stable(&g, "layer mask trailing"));
    pc_psd_bytes_set(&g.layer_info.layers[1].extra_trailing, z6, 5, NULL);
    CHECK(stable(&g, "record trailing"));
    g.layer_info.padding.has = true;
    pc_psd_bytes_set(&g.layer_info.padding.v, z6, 4, NULL);
    pc_psd_bytes_fini(&g.layer_info.layers[1].extra_trailing);
    CHECK(stable(&g, "layer info padding"));
    pc_psd_file_fini(&g);
}

static void test_accessors_and_validation(void)
{
    pc_psd_file f = tg_layered(PC_PSD_V1, PC_PSD_MODE_RGB, 8, PC_PSD_RAW);
    pc_psd_file g;
    pc_psd_resolution r;
    uint16_t v16;
    uint8_t kind;
    size_t icc_len = 0;
    bool real = false;
    size_t nl;
    size_t i;
    const pc_psd_layer *layers;
    const pc_psd_block *lfx = NULL;
    pc_psd_desc d;
    pc_psd_desc want = tg_sample_descriptor();
    CHECK(pc_psd_global_mask_be16(&f, 0, &v16) && v16 == 0);
    CHECK(pc_psd_global_mask_be16(&f, 2, &v16) && v16 == 0xffff);
    CHECK(pc_psd_global_mask_be16(&f, 10, &v16) && v16 == 50);
    CHECK(pc_psd_global_mask_kind(&f, &kind) && kind == 128);
    pc_psd_file_fini(&f);
    f = tg_layered(PC_PSD_V1, PC_PSD_MODE_RGB, 8, PC_PSD_RLE);
    CHECK(pc_psd_file_resolution(&f, &r) && pc_psd_resolution_h(&r) == 72.0);
    CHECK(pc_psd_file_icc(&f, &icc_len) != NULL && icc_len == 13);
    CHECK(pc_psd_file_has_real_merged_data(&f, &real) && real);
    CHECK(pc_psd_file_merged_has_alpha(&f));
    CHECK_EQ_INT(pc_psd_file_validate(&f, NULL), PC_OK);
    layers = pc_psd_file_layers(&f, &nl);
    for (i = 0; i < nl && lfx == NULL; i++) {
        lfx = pc_psd_layer_block(&layers[i], "lfx2");
    }
    CHECK(lfx != NULL);
    if (lfx != NULL) {
        CHECK_EQ_INT(pc_psd_vdesc_parse(lfx->data.data + 4, lfx->data.len - 4, &d, NULL), PC_OK);
        CHECK(pc_psd_desc_eq(&d, &want));
        pc_psd_desc_fini(&d);
    }
    pc_psd_desc_fini(&want);
    pc_psd_file_fini(&f);
    f = tg_merged_only(PC_PSD_V1, PC_PSD_MODE_INDEXED, 8, PC_PSD_RAW, 2, 2);
    CHECK_EQ_INT(pc_psd_file_validate(&f, NULL), PC_OK);
    f.color_mode_data.len--;
    CHECK(pc_psd_file_validate(&f, NULL) != PC_OK);
    pc_psd_file_fini(&f);
    g = tg_merged_only(PC_PSD_V1, PC_PSD_MODE_RGB, 8, PC_PSD_RAW, 2, 2);
    g.header.width = 40000;
    CHECK(pc_psd_file_validate(&g, NULL) != PC_OK);
    g.header.version = PC_PSD_V2;
    CHECK_EQ_INT(pc_psd_file_validate(&g, NULL), PC_OK);
    pc_psd_file_fini(&g);
    {
        pc_psd_layout l = {1, 70000, 1, 8, PC_PSD_V1};
        uint8_t *row = malloc(70000);
        pc_psd_bytes out = {NULL, 0};
        for (i = 0; i < 70000; i++) {
            row[i] = (uint8_t)(i % 251);
        }
        CHECK(pc_psd_encode_planes(PC_PSD_RLE, row, 70000, &l, &out, NULL) != PC_OK);
        l.version = PC_PSD_V2;
        CHECK_EQ_INT(pc_psd_encode_planes(PC_PSD_RLE, row, 70000, &l, &out, NULL), PC_OK);
        pc_psd_bytes_fini(&out);
        free(row);
    }
}

static void test_save_and_open(void)
{
    pc_psd_file f = tg_small(PC_PSD_V1, PC_PSD_RLE);
    pc_psd_file g;
    char path[256];
    (void)snprintf(path, sizeof path, "/tmp/photocraft-psd-test-%ld.psd", (long)getpid());
    CHECK_EQ_INT(pc_psd_file_save(&f, path, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_file_open(path, &g, NULL), PC_OK);
    CHECK(pc_psd_file_eq(&g, &f));
    pc_psd_file_fini(&g);
    (void)unlink(path);
    pc_psd_file_fini(&f);
}

static void test_spec_order_density_only_mask_round_trips(void)
{
    pc_psd_file f = tg_small(PC_PSD_V1, PC_PSD_RAW);
    pc_psd_layer *l = &f.layer_info.layers[0];
    pc_psd_file g;
    pc_psd_bytes b;
    pc_psd_bytes again = {NULL, 0};
    pc_psd_bytes d = {NULL, 0};
    static const uint8_t px[1] = {64};
    size_t k;
    pc_psd_layer_mask_fini(&l->mask.mask);
    memset(&l->mask.mask, 0, sizeof l->mask.mask);
    l->mask.mask.rect = pc_psd_rect_from_xywh(0, 0, 1, 1);
    l->mask.mask.default_color = 255;
    l->mask.mask.flags = PC_PSD_MF_PARAMETERS;
    l->mask.mask.has_params = true;
    l->mask.mask.params.flags = 1;
    l->mask.mask.params.has_user_density = true;
    l->mask.mask.params.user_density = 64;
    l->mask.mask.has_real = true;
    l->mask.mask.real.rect = pc_psd_rect_from_xywh(0, 0, 1, 1);
    l->mask.mask.real_first = false;
    for (k = 0; k < l->nchannels; k++) {
        if (l->channels[k].id == -2) {
            pc_psd_channel c;
            CHECK_EQ_INT(pc_psd_channel_encode(-2, PC_PSD_RAW, px, 1, 1, 1, 8, PC_PSD_V1, &c, NULL), PC_OK);
            pc_psd_bytes_fini(&l->channels[k].data);
            l->channels[k] = c;
        }
    }
    b = bytes_of(&f);
    CHECK_EQ_INT(pc_psd_file_parse(b.data, b.len, &g, NULL), PC_OK);
    CHECK(pc_psd_file_eq(&g, &f));
    CHECK_EQ_INT(pc_psd_layer_decode_channel(&g.layer_info.layers[0], -2, 8, PC_PSD_V1, &d, NULL), PC_OK);
    CHECK(d.len == 1 && d.data[0] == 64);
    CHECK_EQ_INT(pc_psd_file_to_bytes(&g, &again, NULL), PC_OK);
    CHECK(pc_psd_bytes_eq(&again, &b));
    pc_psd_bytes_fini(&d);
    pc_psd_bytes_fini(&again);
    pc_psd_bytes_fini(&b);
    pc_psd_file_fini(&g);
    pc_psd_file_fini(&f);
}

/* layer_block_order.rs */
static void test_global_layer_block_roundtrips_with_neighbours(void)
{
    static const uint8_t a[3] = {7, 8, 9};
    static const uint8_t b2[2] = {10, 11};
    static const uint8_t pad[3] = {31, 32, 33};
    static const char *const sigs[2] = {"8BIM", "8B64"};
    int v;
    int depth;
    size_t idx;
    int s;
    int bad = 0;
    for (v = 1; v <= 2; v++) {
        for (depth = 16; depth <= 32; depth += 16) {
            for (idx = 0; idx <= 2; idx++) {
                for (s = 0; s < 2; s++) {
                    pc_psd_file f = tg_layered((pc_psd_version)v, PC_PSD_MODE_RGB, (uint16_t)depth, PC_PSD_RAW);
                    pc_psd_blocks_free(f.global_blocks, f.nglobal);
                    f.global_blocks = NULL;
                    f.nglobal = 0;
                    push_global(&f, "zz01", a, 3);
                    push_global(&f, "zz02", b2, 2);
                    f.placement.index = idx;
                    memcpy(f.placement.signature, sigs[s], 4);
                    memcpy(f.placement.key, depth == 16 ? "Lr16" : "Lr32", 4);
                    f.placement.padding.has = true;
                    pc_psd_bytes_set(&f.placement.padding.v, pad, 3, NULL);
                    bad += !stable(&f, "global block order");
                    pc_psd_file_fini(&f);
                }
            }
        }
    }
    CHECK_EQ_INT(bad, 0);
}

/* ---- builder.rs ---- */

static void rgba_pattern(size_t w, size_t h, uint8_t seed, uint8_t *v)
{
    size_t x;
    size_t y;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint8_t *p = v + (y * w + x) * 4;
            p[0] = (uint8_t)((uint8_t)(x * 10) ^ seed);
            p[1] = (uint8_t)(y * 20);
            p[2] = (uint8_t)((uint8_t)(x + y) ^ seed);
            p[3] = (x + y) % 3 == 0 ? 0 : (uint8_t)(200 + x % 50);
        }
    }
}

/* Builds, checks byte and model stability, returns the parsed file. */
static pc_psd_file built(const pc_psd_builder *b)
{
    pc_psd_bytes bytes = {NULL, 0};
    pc_psd_bytes again = {NULL, 0};
    pc_psd_file f;
    pc_psd_file m;
    pc_psd_file_init(&f);
    CHECK_EQ_INT(pc_psd_builder_to_bytes(b, &bytes, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_file_parse(bytes.data, bytes.len, &f, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_file_to_bytes(&f, &again, NULL), PC_OK);
    CHECK(pc_psd_bytes_eq(&again, &bytes));
    CHECK_EQ_INT(pc_psd_builder_build(b, &m, NULL), PC_OK);
    CHECK(pc_psd_file_eq(&f, &m));
    pc_psd_file_fini(&m);
    pc_psd_bytes_fini(&bytes);
    pc_psd_bytes_fini(&again);
    return f;
}

static pc_psd_pixels px8(pc_psd_px_kind k, const uint8_t *d, size_t n)
{
    pc_psd_pixels p;
    p.kind = k;
    p.data = d;
    p.len = n;
    return p;
}

static void test_builder_rgba8_cases(void)
{
    uint8_t px[7 * 5 * 4];
    int v;
    uint16_t c;
    rgba_pattern(7, 5, 3, px);
    for (v = 1; v <= 2; v++) {
        for (c = 0; c < 4; c++) {
            pc_psd_builder *b = pc_psd_builder_new(10, 8);
            pc_psd_layer_spec s = pc_psd_layer_spec_make("L", -2, 1, 7, 5, px8(PC_PSD_PX_RGBA8, px, sizeof px));
            pc_psd_file f;
            pc_psd_image8 img;
            pc_psd_builder_version(b, (pc_psd_version)v);
            pc_psd_builder_compression(b, c);
            CHECK_EQ_INT(pc_psd_builder_push_layer(b, &s, NULL), PC_OK);
            f = built(b);
            CHECK_EQ_INT(pc_psd_layer_rgba8(&f, 0, &img, NULL), PC_OK);
            CHECK(img.left == -2 && img.top == 1 && img.width == 7 && img.height == 5);
            CHECK(img.data.len == sizeof px && memcmp(img.data.data, px, sizeof px) == 0);
            pc_psd_image8_fini(&img);
            pc_psd_file_fini(&f);
            pc_psd_builder_free(b);
        }
    }
}

static void test_builder_depths_and_modes(void)
{
    uint16_t px16[48];
    uint16_t c;
    size_t i;
    for (i = 0; i < 48; i++) {
        px16[i] = (uint16_t)(i * 5003u);
    }
    for (c = 0; c < 4; c++) {
        pc_psd_builder *b = pc_psd_builder_new(4, 3);
        pc_psd_pixels p = {PC_PSD_PX_RGBA16, px16, 48};
        pc_psd_layer_spec s = pc_psd_layer_spec_make("x", 0, 0, 4, 3, p);
        pc_psd_file f;
        pc_psd_image8 img;
        pc_psd_bytes ch = {NULL, 0};
        int bad = 0;
        pc_psd_builder_depth(b, 16);
        pc_psd_builder_compression(b, c);
        pc_psd_builder_push_layer(b, &s, NULL);
        f = built(b);
        CHECK(f.placement.where == PC_PSD_LI_GLOBAL_BLOCK && memcmp(f.placement.key, "Lr16", 4) == 0);
        CHECK_EQ_INT(pc_psd_layer_rgba8(&f, 0, &img, NULL), PC_OK);
        for (i = 0; i < 48; i++) {
            bad += img.data.data[i] != (uint8_t)(((uint32_t)px16[i] * 255u + 32767u) / 65535u);
        }
        CHECK_EQ_INT(pc_psd_layer_decode_channel(&f.layer_info.layers[0], 0, 16, PC_PSD_V1, &ch, NULL), PC_OK);
        for (i = 0; i < 12; i++) {
            bad += pc_load_be16(ch.data + 2 * i) != px16[i * 4];
        }
        CHECK_EQ_INT(bad, 0);
        pc_psd_bytes_fini(&ch);
        pc_psd_image8_fini(&img);
        pc_psd_file_fini(&f);
        pc_psd_builder_free(b);
    }
    /* grayscale */
    {
        uint8_t g[12];
        pc_psd_builder *b = pc_psd_builder_new(3, 2);
        pc_psd_layer_spec s;
        pc_psd_file f;
        pc_psd_image8 img;
        pc_psd_image8 comp;
        for (i = 0; i < 6; i++) {
            g[2 * i] = (uint8_t)(i * 40);
            g[2 * i + 1] = (uint8_t)(255 - i);
        }
        pc_psd_builder_color_mode(b, PC_PSD_MODE_GRAYSCALE);
        s = pc_psd_layer_spec_make("g", 0, 0, 3, 2, px8(PC_PSD_PX_GRAYA8, g, 12));
        pc_psd_builder_push_layer(b, &s, NULL);
        pc_psd_builder_composite(b, px8(PC_PSD_PX_GRAYA8, g, 12), NULL);
        f = built(b);
        CHECK(f.header.color_mode == PC_PSD_MODE_GRAYSCALE && f.header.channels == 2);
        CHECK_EQ_INT(pc_psd_layer_rgba8(&f, 0, &img, NULL), PC_OK);
        for (i = 0; i < 6; i++) {
            CHECK(img.data.data[i * 4] == g[2 * i] && img.data.data[i * 4 + 2] == g[2 * i] &&
                  img.data.data[i * 4 + 3] == g[2 * i + 1]);
        }
        CHECK_EQ_INT(pc_psd_composite_rgba8(&f, &comp, NULL), PC_OK);
        CHECK(pc_psd_bytes_eq(&comp.data, &img.data));
        pc_psd_image8_fini(&img);
        pc_psd_image8_fini(&comp);
        pc_psd_file_fini(&f);
        pc_psd_builder_free(b);
    }
    /* grayscale 16, CMYK 8 and 16 */
    {
        static const uint16_t g16[4] = {0, 65535, 32768, 65535};
        static const uint8_t wg[8] = {0, 0, 0, 255, 128, 128, 128, 255};
        static const uint8_t cm[10] = {0, 0, 0, 0, 255, 0, 0, 0, 255, 255};
        static const uint8_t wc[8] = {255, 255, 255, 255, 0, 0, 0, 255};
        static const uint16_t c16[5] = {0, 0, 0, 0, 65535};
        static const uint8_t w16[4] = {255, 255, 255, 255};
        pc_psd_builder *b = pc_psd_builder_new(2, 1);
        pc_psd_pixels p = {PC_PSD_PX_GRAYA16, g16, 4};
        pc_psd_layer_spec s = pc_psd_layer_spec_make("g", 0, 0, 2, 1, p);
        pc_psd_file f;
        pc_psd_image8 img;
        pc_psd_bytes ch = {NULL, 0};
        pc_psd_builder_color_mode(b, PC_PSD_MODE_GRAYSCALE);
        pc_psd_builder_depth(b, 16);
        pc_psd_builder_push_layer(b, &s, NULL);
        f = built(b);
        CHECK_EQ_INT(pc_psd_layer_rgba8(&f, 0, &img, NULL), PC_OK);
        CHECK(img.data.len == 8 && memcmp(img.data.data, wg, 8) == 0);
        pc_psd_image8_fini(&img);
        pc_psd_file_fini(&f);
        pc_psd_builder_free(b);
        b = pc_psd_builder_new(2, 1);
        pc_psd_builder_color_mode(b, PC_PSD_MODE_CMYK);
        s = pc_psd_layer_spec_make("c", 0, 0, 2, 1, px8(PC_PSD_PX_CMYKA8, cm, 10));
        pc_psd_builder_push_layer(b, &s, NULL);
        pc_psd_builder_composite(b, px8(PC_PSD_PX_CMYKA8, cm, 10), NULL);
        f = built(b);
        CHECK_EQ_UINT(f.header.channels, 4);
        CHECK_EQ_INT(pc_psd_layer_decode_channel(&f.layer_info.layers[0], 0, 8, PC_PSD_V1, &ch, NULL), PC_OK);
        CHECK(ch.len == 2 && ch.data[0] == 255 && ch.data[1] == 255); /* stored inverted */
        CHECK_EQ_INT(pc_psd_layer_decode_channel(&f.layer_info.layers[0], 3, 8, PC_PSD_V1, &ch, NULL), PC_OK);
        CHECK(ch.len == 2 && ch.data[0] == 255 && ch.data[1] == 0);
        CHECK_EQ_INT(pc_psd_layer_rgba8(&f, 0, &img, NULL), PC_OK);
        CHECK(img.data.len == 8 && memcmp(img.data.data, wc, 8) == 0);
        pc_psd_image8_fini(&img);
        CHECK_EQ_INT(pc_psd_composite_rgba8(&f, &img, NULL), PC_OK);
        CHECK(img.data.len == 8 && memcmp(img.data.data, wc, 8) == 0);
        pc_psd_image8_fini(&img);
        pc_psd_bytes_fini(&ch);
        pc_psd_file_fini(&f);
        pc_psd_builder_free(b);
        b = pc_psd_builder_new(1, 1);
        pc_psd_builder_color_mode(b, PC_PSD_MODE_CMYK);
        pc_psd_builder_depth(b, 16);
        p.kind = PC_PSD_PX_CMYKA16;
        p.data = c16;
        p.len = 5;
        s = pc_psd_layer_spec_make("c", 0, 0, 1, 1, p);
        pc_psd_builder_push_layer(b, &s, NULL);
        f = built(b);
        CHECK_EQ_INT(pc_psd_layer_rgba8(&f, 0, &img, NULL), PC_OK);
        CHECK(img.data.len == 4 && memcmp(img.data.data, w16, 4) == 0);
        pc_psd_image8_fini(&img);
        pc_psd_file_fini(&f);
        pc_psd_builder_free(b);
    }
}

static void test_builder_layer_properties(void)
{
    uint8_t nine[16];
    pc_psd_builder *b = pc_psd_builder_new(4, 4);
    pc_psd_layer_spec s;
    pc_psd_block lclr;
    pc_psd_file f;
    const pc_psd_layer *r;
    char *name = NULL;
    uint32_t id;
    uint16_t color;
    int i;
    memset(nine, 9, 16);
    s = pc_psd_layer_spec_make("Props \xF0\x9F\x8E\xA8", 1, 1, 2, 2, px8(PC_PSD_PX_RGBA8, nine, 16));
    memcpy(s.blend, "sLit", 4);
    s.opacity = 128;
    s.has_fill_opacity = true;
    s.fill_opacity = 64;
    s.visible = false;
    s.clipping = true;
    pc_psd_block_make_sheet_color(4, &lclr, NULL);
    s.extra_blocks = &lclr;
    s.nextra = 1;
    pc_psd_builder_push_layer(b, &s, NULL);
    pc_psd_block_fini(&lclr);
    f = built(b);
    r = &f.layer_info.layers[0];
    CHECK_EQ_INT(pc_psd_layer_name(r, &name, NULL), PC_OK);
    CHECK_EQ_STR(name, "Props \xF0\x9F\x8E\xA8");
    free(name);
    CHECK(r->name.len == 7 && memcmp(r->name.data, "Props ?", 7) == 0);
    CHECK(memcmp(r->blend, "sLit", 4) == 0 && r->opacity == 128 && pc_psd_layer_fill_opacity(r) == 64);
    CHECK(!pc_psd_layer_is_visible(r) && r->clipping == 1);
    CHECK(pc_psd_layer_id(r, &id) && id == 1);
    CHECK(pc_psd_block_u16(pc_psd_layer_block(r, "lclr"), &color, NULL) == PC_OK && color == 4);
    pc_psd_file_fini(&f);
    pc_psd_builder_free(b);
    /* all blend modes */
    b = pc_psd_builder_new(1, 1);
    for (i = 0; i < PC_PSD_BLEND_COUNT; i++) {
        static const uint8_t p4[4] = {1, 2, 3, 4};
        s = pc_psd_layer_spec_make(TG_BLEND_NAMES[i], 0, 0, 1, 1, px8(PC_PSD_PX_RGBA8, p4, 4));
        memcpy(s.blend, PC_PSD_BLEND_KEYS[i], 4);
        pc_psd_builder_push_layer(b, &s, NULL);
    }
    f = built(b);
    CHECK_EQ_UINT(f.layer_info.n, PC_PSD_BLEND_COUNT);
    for (i = 0; i < PC_PSD_BLEND_COUNT; i++) {
        CHECK(memcmp(f.layer_info.layers[i].blend, PC_PSD_BLEND_KEYS[i], 4) == 0);
    }
    pc_psd_file_fini(&f);
    pc_psd_builder_free(b);
}

static void test_builder_masks(void)
{
    uint8_t mask[12];
    uint8_t px[36];
    uint16_t px16[36];
    int depth;
    size_t i;
    for (i = 0; i < 12; i++) {
        mask[i] = (uint8_t)(i * 20);
    }
    memset(px, 100, sizeof px);
    for (i = 0; i < 36; i++) {
        px16[i] = 25700;
    }
    for (depth = 8; depth <= 16; depth += 8) {
        pc_psd_builder *b = pc_psd_builder_new(3, 3);
        pc_psd_pixels p = depth == 8 ? px8(PC_PSD_PX_RGBA8, px, 36) : (pc_psd_pixels){PC_PSD_PX_RGBA16, px16, 36};
        pc_psd_layer_spec s = pc_psd_layer_spec_make("m", 0, 0, 3, 3, p);
        pc_psd_mask_spec ms = {pc_psd_rect_from_xywh(-1, 0, 4, 3), mask, 12, 255, true};
        pc_psd_file f;
        pc_psd_image8 m;
        pc_psd_image8 img;
        int bad = 0;
        s.mask = &ms;
        pc_psd_builder_depth(b, (uint16_t)depth);
        pc_psd_builder_push_layer(b, &s, NULL);
        f = built(b);
        CHECK_EQ_INT(pc_psd_layer_user_mask(&f, 0, &m, NULL), PC_OK);
        CHECK(m.left == -1 && m.top == 0 && m.width == 4 && m.height == 3);
        CHECK(m.data.len == 12 && memcmp(m.data.data, mask, 12) == 0 && m.default_value == 255);
        CHECK(pc_psd_layer_get_mask(&f.layer_info.layers[0])->flags & PC_PSD_MF_DISABLED);
        CHECK_EQ_INT(pc_psd_layer_rgba8(&f, 0, &img, NULL), PC_OK);
        for (i = 0; i < img.data.len; i++) {
            bad += img.data.data[i] != 100; /* mask not applied */
        }
        CHECK_EQ_INT(bad, 0);
        pc_psd_image8_fini(&m);
        pc_psd_image8_fini(&img);
        pc_psd_file_fini(&f);
        pc_psd_builder_free(b);
    }
}

static void test_builder_misc(void)
{
    static const uint8_t z4[4];
    pc_psd_builder *b = pc_psd_builder_new(1, 1);
    pc_psd_layer_spec s = pc_psd_layer_spec_make("x", 0, 0, 1, 1, px8(PC_PSD_PX_RGBA8, z4, 4));
    pc_psd_file f;
    pc_psd_image8 img;
    pc_error e;
    uint8_t px[24];
    bool real;
    size_t i;
    pc_psd_resolution r;
    size_t icc_len;
    const uint8_t *icc;
    static const uint8_t icc3[3] = {1, 2, 3};
    pc_psd_builder_push_layer(b, &s, NULL);
    f = built(b);
    CHECK_EQ_INT(pc_psd_layer_user_mask(&f, 0, &img, &e), PC_ERR_NOT_FOUND);
    pc_psd_file_fini(&f);
    pc_psd_builder_free(b);
    /* empty layer */
    b = pc_psd_builder_new(2, 2);
    s = pc_psd_layer_spec_make("empty", 0, 0, 0, 0, px8(PC_PSD_PX_RGBA8, NULL, 0));
    pc_psd_builder_push_layer(b, &s, NULL);
    f = built(b);
    CHECK_EQ_INT(pc_psd_layer_rgba8(&f, 0, &img, NULL), PC_OK);
    CHECK(img.width == 0 && img.height == 0 && img.data.len == 0);
    pc_psd_image8_fini(&img);
    pc_psd_file_fini(&f);
    pc_psd_builder_free(b);
    /* composite with alpha */
    rgba_pattern(3, 2, 0, px);
    b = pc_psd_builder_new(3, 2);
    s = pc_psd_layer_spec_make("x", 0, 0, 3, 2, px8(PC_PSD_PX_RGBA8, px, 24));
    pc_psd_builder_push_layer(b, &s, NULL);
    pc_psd_builder_composite(b, px8(PC_PSD_PX_RGBA8, px, 24), NULL);
    f = built(b);
    CHECK(f.header.channels == 4 && f.layer_info.merged_alpha && pc_psd_file_merged_has_alpha(&f));
    CHECK(pc_psd_file_has_real_merged_data(&f, &real) && real);
    CHECK_EQ_INT(pc_psd_composite_rgba8(&f, &img, NULL), PC_OK);
    CHECK(img.data.len == 24 && memcmp(img.data.data, px, 24) == 0);
    pc_psd_image8_fini(&img);
    pc_psd_file_fini(&f);
    pc_psd_builder_free(b);
    /* opaque composite drops alpha */
    for (i = 0; i < 6; i++) {
        px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = (uint8_t)i;
        px[i * 4 + 3] = 255;
    }
    b = pc_psd_builder_new(3, 2);
    pc_psd_builder_composite(b, px8(PC_PSD_PX_RGBA8, px, 24), NULL);
    f = built(b);
    CHECK(f.header.channels == 3 && !f.has_layer_info);
    CHECK_EQ_INT(pc_psd_composite_rgba8(&f, &img, NULL), PC_OK);
    CHECK(img.data.len == 24 && memcmp(img.data.data, px, 24) == 0);
    pc_psd_image8_fini(&img);
    pc_psd_file_fini(&f);
    pc_psd_builder_free(b);
    /* 16-bit composite */
    {
        static const uint16_t c16[8] = {65535, 0, 0, 65535, 0, 65535, 0, 32768};
        static const uint8_t w[8] = {255, 0, 0, 255, 0, 255, 0, 128};
        pc_psd_pixels p = {PC_PSD_PX_RGBA16, c16, 8};
        b = pc_psd_builder_new(2, 1);
        pc_psd_builder_depth(b, 16);
        pc_psd_builder_composite(b, p, NULL);
        f = built(b);
        CHECK_EQ_INT(pc_psd_composite_rgba8(&f, &img, NULL), PC_OK);
        CHECK(img.data.len == 8 && memcmp(img.data.data, w, 8) == 0);
        pc_psd_image8_fini(&img);
        pc_psd_file_fini(&f);
        pc_psd_builder_free(b);
    }
    /* placeholder composite is flagged and white */
    b = pc_psd_builder_new(2, 2);
    s = pc_psd_layer_spec_make("x", 0, 0, 1, 1, px8(PC_PSD_PX_RGBA8, z4, 4));
    pc_psd_builder_push_layer(b, &s, NULL);
    f = built(b);
    CHECK(pc_psd_file_has_real_merged_data(&f, &real) && !real);
    CHECK_EQ_INT(pc_psd_composite_rgba8(&f, &img, NULL), PC_OK);
    for (i = 0; i < img.data.len; i++) {
        CHECK(img.data.data[i] == 255);
    }
    pc_psd_image8_fini(&img);
    pc_psd_file_fini(&f);
    pc_psd_builder_free(b);
    /* resources */
    b = pc_psd_builder_new(1, 1);
    pc_psd_builder_resolution(b, 300.0, NULL);
    pc_psd_builder_icc(b, icc3, 3, NULL);
    f = built(b);
    CHECK(pc_psd_file_resolution(&f, &r) && pc_psd_resolution_v(&r) == 300.0);
    icc = pc_psd_file_icc(&f, &icc_len);
    CHECK(icc != NULL && icc_len == 3 && memcmp(icc, icc3, 3) == 0);
    pc_psd_file_fini(&f);
    pc_psd_builder_free(b);
    /* large PSB canvas */
    b = pc_psd_builder_new(40000, 1);
    pc_psd_builder_version(b, PC_PSD_V2);
    f = built(b);
    CHECK(f.header.width == 40000 && pc_psd_file_validate(&f, NULL) == PC_OK);
    pc_psd_file_fini(&f);
    pc_psd_builder_free(b);
}

static void test_builder_errors(void)
{
    static const uint8_t z4[4];
    pc_psd_builder *b = pc_psd_builder_new(2, 2);
    pc_psd_layer_spec s = pc_psd_layer_spec_make("bad", 0, 0, 2, 2, px8(PC_PSD_PX_RGBA8, z4, 3));
    pc_psd_file f;
    pc_psd_group_spec g = pc_psd_group_spec_make("open");
    pc_psd_mask_spec ms = {pc_psd_rect_from_xywh(0, 0, 2, 2), z4, 3, 0, false};
    pc_error e;
    pc_psd_builder_push_layer(b, &s, NULL);
    CHECK_EQ_INT(pc_psd_builder_build(b, &f, &e), PC_ERR_INVALID);
    CHECK_EQ_STR(e.msg, "invalid data: pixel buffer has 3 samples, expected 16");
    pc_psd_builder_free(b);
    b = pc_psd_builder_new(2, 2);
    s = pc_psd_layer_spec_make("wrong fmt", 0, 0, 1, 1, px8(PC_PSD_PX_GRAYA8, z4, 2));
    pc_psd_builder_push_layer(b, &s, NULL);
    CHECK_EQ_INT(pc_psd_builder_build(b, &f, &e), PC_ERR_INVALID);
    CHECK_EQ_STR(e.msg, "invalid data: pixel format (Grayscale, 8) does not match document Rgb/8");
    pc_psd_builder_free(b);
    b = pc_psd_builder_new(2, 2);
    pc_psd_builder_begin_group(b, &g, NULL);
    CHECK(pc_psd_builder_build(b, &f, NULL) != PC_OK);
    pc_psd_builder_free(b);
    b = pc_psd_builder_new(2, 2);
    CHECK(pc_psd_builder_end_group(b, NULL) != PC_OK);
    pc_psd_builder_depth(b, 32);
    CHECK_EQ_INT(pc_psd_builder_build(b, &f, &e), PC_ERR_UNSUPPORTED);
    pc_psd_builder_depth(b, 8);
    pc_psd_builder_color_mode(b, PC_PSD_MODE_LAB);
    CHECK_EQ_INT(pc_psd_builder_build(b, &f, &e), PC_ERR_UNSUPPORTED);
    CHECK_EQ_STR(e.msg, "unsupported: builder color mode Lab");
    pc_psd_builder_free(b);
    b = pc_psd_builder_new(2, 2);
    pc_psd_builder_composite(b, px8(PC_PSD_PX_RGBA8, z4, 4), NULL);
    CHECK(pc_psd_builder_build(b, &f, NULL) != PC_OK);
    pc_psd_builder_free(b);
    b = pc_psd_builder_new(2, 2);
    s = pc_psd_layer_spec_make("m", 0, 0, 1, 1, px8(PC_PSD_PX_RGBA8, z4, 4));
    s.mask = &ms;
    pc_psd_builder_push_layer(b, &s, NULL);
    CHECK_EQ_INT(pc_psd_builder_build(b, &f, &e), PC_ERR_INVALID);
    CHECK_EQ_STR(e.msg, "invalid data: mask buffer size mismatch");
    pc_psd_builder_free(b);
}

static void test_builder_groups(void)
{
    static const uint8_t z4[4];
    pc_psd_builder *b = pc_psd_builder_new(1, 1);
    pc_psd_group_spec g = pc_psd_group_spec_make("G");
    pc_psd_layer_spec s = pc_psd_layer_spec_make("x", 0, 0, 1, 1, px8(PC_PSD_PX_RGBA8, z4, 4));
    pc_psd_file f;
    const pc_psd_layer *gl;
    pc_psd_section_divider sd;
    char *name = NULL;
    size_t i;
    memcpy(g.blend, "mul ", 4);
    g.opacity = 77;
    g.visible = false;
    pc_psd_builder_begin_group(b, &g, NULL);
    pc_psd_builder_push_layer(b, &s, NULL);
    CHECK_EQ_INT(pc_psd_builder_end_group(b, NULL), PC_OK);
    f = built(b);
    gl = &f.layer_info.layers[2];
    CHECK_EQ_INT(pc_psd_layer_name(gl, &name, NULL), PC_OK);
    CHECK_EQ_STR(name, "G");
    free(name);
    CHECK(gl->opacity == 77 && !pc_psd_layer_is_visible(gl));
    CHECK(pc_psd_layer_section_divider(gl, &sd) && sd.kind == PC_PSD_SECTION_OPEN && sd.has_blend &&
          memcmp(sd.blend, "mul ", 4) == 0);
    CHECK_EQ_UINT(pc_psd_layer_section_type(&f.layer_info.layers[0]), PC_PSD_SECTION_DIVIDER);
    pc_psd_file_fini(&f);
    pc_psd_builder_free(b);
    /* indices and names */
    b = pc_psd_builder_new(1, 1);
    for (i = 0; i < 3; i++) {
        char nm[8];
        (void)snprintf(nm, sizeof nm, "l%zu", i);
        s = pc_psd_layer_spec_make(nm, 0, 0, 1, 1, px8(PC_PSD_PX_RGBA8, z4, 4));
        pc_psd_builder_push_layer(b, &s, NULL);
    }
    f = built(b);
    for (i = 0; i < 3; i++) {
        char nm[8];
        (void)snprintf(nm, sizeof nm, "l%zu", i);
        CHECK_EQ_INT(pc_psd_layer_name(&f.layer_info.layers[i], &name, NULL), PC_OK);
        CHECK_EQ_STR(name, nm);
        free(name);
    }
    {
        pc_psd_image8 img;
        CHECK_EQ_INT(pc_psd_layer_rgba8(&f, 3, &img, NULL), PC_ERR_NOT_FOUND);
    }
    pc_psd_file_fini(&f);
    pc_psd_builder_free(b);
}

/* ---- tree.rs ---- */

static pc_psd_layer trec(const char *name, int kind)
{
    pc_psd_layer l;
    pc_psd_block b;
    pc_psd_layer_init(&l);
    pc_psd_bytes_set(&l.name, name, strlen(name), NULL);
    if (kind >= 0) {
        pc_psd_block_make_section((uint32_t)kind, (const uint8_t *)"pass", NULL, &b, NULL);
        pc_psd_layer_push_block(&l, &b, NULL);
    }
    return l;
}

static pc_psd_file file_with(pc_psd_layer *recs, size_t n)
{
    pc_psd_file f = tg_merged_only(PC_PSD_V1, PC_PSD_MODE_RGB, 8, PC_PSD_RAW, 1, 1);
    size_t i;
    f.has_layer_info = true;
    for (i = 0; i < n; i++) {
        pc_psd_layer_info_push(&f.layer_info, &recs[i], NULL);
    }
    return f;
}

static void names(const pc_psd_file *f, const pc_psd_node *n, size_t count, pc_buf *o)
{
    size_t i;
    for (i = 0; i < count; i++) {
        char *nm = NULL;
        if (i > 0) {
            pc_buf_append_byte(o, ',', NULL);
        }
        pc_psd_layer_name(&f->layer_info.layers[n[i].index], &nm, NULL);
        pc_buf_append_str(o, nm, NULL);
        free(nm);
        if (n[i].is_group) {
            pc_buf_append_byte(o, '[', NULL);
            names(f, n[i].children, n[i].n, o);
            pc_buf_append_byte(o, ']', NULL);
        }
    }
}

static void check_tree(pc_psd_layer *recs, size_t n, const char *want)
{
    pc_psd_file f = file_with(recs, n);
    pc_psd_node *t = NULL;
    size_t nt = 0;
    pc_buf o = PC_BUF_INIT;
    CHECK_EQ_INT(pc_psd_layer_tree(&f, &t, &nt, NULL), PC_OK);
    names(&f, t, nt, &o);
    CHECK_EQ_STR(pc_buf_cstr(&o), want);
    pc_buf_fini(&o);
    pc_psd_nodes_free(t, nt);
    pc_psd_file_fini(&f);
}

#define E PC_PSD_SECTION_DIVIDER
#define O PC_PSD_SECTION_OPEN
#define C PC_PSD_SECTION_CLOSED

static void test_layer_trees(void)
{
    pc_psd_layer r[8];
    pc_psd_file f;
    pc_psd_node *t = NULL;
    size_t nt = 0;
    pc_psd_layer big[101];
    int i;
    r[0] = trec("a", -1);
    r[1] = trec("b", -1);
    r[2] = trec("c", -1);
    check_tree(r, 3, "a,b,c");
    f = tg_merged_only(PC_PSD_V1, PC_PSD_MODE_RGB, 8, PC_PSD_RAW, 1, 1);
    CHECK(pc_psd_layer_tree(&f, &t, &nt, NULL) == PC_OK && nt == 0);
    pc_psd_file_fini(&f);
    r[0] = trec("bg", -1);
    r[1] = trec("</Layer group>", E);
    r[2] = trec("x", -1);
    r[3] = trec("y", -1);
    r[4] = trec("G", O);
    f = file_with(r, 5);
    CHECK_EQ_INT(pc_psd_layer_tree(&f, &t, &nt, NULL), PC_OK);
    CHECK(nt == 2 && t[1].is_group && t[1].index == 4 && t[1].has_divider && t[1].divider == 1 && t[1].n == 2);
    pc_psd_nodes_free(t, nt);
    pc_psd_file_fini(&f);
    r[0] = trec("</Layer group>", E);
    r[1] = trec("x", -1);
    r[2] = trec("G", C);
    check_tree(r, 3, "G[x]");
    r[0] = trec("</Layer group>", E);
    r[1] = trec("a", -1);
    r[2] = trec("</Layer group>", E);
    r[3] = trec("b", -1);
    r[4] = trec("Inner", C);
    r[5] = trec("c", -1);
    r[6] = trec("Outer", O);
    r[7] = trec("top", -1);
    f = file_with(r, 8);
    CHECK_EQ_INT(pc_psd_layer_tree(&f, &t, &nt, NULL), PC_OK);
    CHECK(nt == 2 && pc_psd_node_count(&t[0]) == 5);
    pc_psd_nodes_free(t, nt);
    pc_psd_file_fini(&f);
    r[0] = trec("</Layer group>", E);
    r[1] = trec("a", -1);
    r[2] = trec("</Layer group>", E);
    r[3] = trec("b", -1);
    r[4] = trec("Inner", C);
    r[5] = trec("c", -1);
    r[6] = trec("Outer", O);
    r[7] = trec("top", -1);
    check_tree(r, 8, "Outer[a,Inner[b],c],top");
    r[0] = trec("</Layer group>", E);
    r[1] = trec("G", O);
    check_tree(r, 2, "G[]");
    r[0] = trec("</Layer group>", E);
    r[1] = trec("a", -1);
    r[2] = trec("G1", O);
    r[3] = trec("</Layer group>", E);
    r[4] = trec("b", -1);
    r[5] = trec("G2", O);
    check_tree(r, 6, "G1[a],G2[b]");
    for (i = 0; i < 50; i++) {
        big[i] = trec("</Layer group>", E);
    }
    big[50] = trec("leaf", -1);
    for (i = 0; i < 50; i++) {
        char nm[16];
        (void)snprintf(nm, sizeof nm, "g%d", i);
        big[51 + i] = trec(nm, O);
    }
    f = file_with(big, 101);
    CHECK_EQ_INT(pc_psd_layer_tree(&f, &t, &nt, NULL), PC_OK);
    CHECK(nt == 1 && pc_psd_node_count(&t[0]) == 51);
    {
        const pc_psd_node *n = &t[0];
        int depth = 1;
        while (n->n > 0) {
            n = &n->children[0];
            depth++;
        }
        CHECK_EQ_INT(depth, 51);
    }
    pc_psd_nodes_free(t, nt);
    pc_psd_file_fini(&f);
    r[0] = trec("a", -1);
    r[1] = trec("G", O);
    r[2] = trec("b", -1);
    check_tree(r, 3, "a,G[],b");
    r[0] = trec("a", -1);
    r[1] = trec("</Layer group>", E);
    r[2] = trec("b", -1);
    r[3] = trec("c", -1);
    check_tree(r, 4, "a,b,c");
    {
        pc_psd_block b;
        r[0] = trec("</Layer group>", -1);
        pc_psd_block_make_section(E, NULL, NULL, &b, NULL);
        memcpy(b.key, "lsdk", 4);
        pc_psd_layer_push_block(&r[0], &b, NULL);
        r[1] = trec("x", -1);
        r[2] = trec("G", O);
        check_tree(r, 3, "G[x]");
    }
    r[0] = trec("a", PC_PSD_SECTION_OTHER);
    r[1] = trec("b", 9);
    check_tree(r, 2, "a,b");
}

static void test_generated_and_builder_trees(void)
{
    pc_psd_file f = tg_layered(PC_PSD_V1, PC_PSD_MODE_RGB, 8, PC_PSD_RLE);
    pc_psd_node *t = NULL;
    size_t nt = 0;
    size_t i;
    const pc_psd_node *outer = NULL;
    char *nm = NULL;
    static const uint8_t white[16] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    pc_psd_builder *b;
    pc_psd_layer_spec s;
    pc_psd_group_spec g;
    pc_psd_bytes bytes = {NULL, 0};
    pc_buf o = PC_BUF_INIT;
    CHECK_EQ_INT(pc_psd_layer_tree(&f, &t, &nt, NULL), PC_OK);
    for (i = 0; i < nt && outer == NULL; i++) {
        if (t[i].is_group) {
            outer = &t[i];
        }
    }
    CHECK(outer != NULL);
    if (outer != NULL) {
        pc_psd_layer_name(&f.layer_info.layers[outer->index], &nm, NULL);
        CHECK_EQ_STR(nm, "Outer");
        free(nm);
        CHECK(outer->n == 2 && outer->children[1].is_group);
        pc_psd_layer_name(&f.layer_info.layers[outer->children[1].index], &nm, NULL);
        CHECK_EQ_STR(nm, "Inner");
        free(nm);
        CHECK_EQ_UINT(pc_psd_layer_section_type(&f.layer_info.layers[outer->children[1].index]), PC_PSD_SECTION_CLOSED);
        pc_psd_layer_name(&f.layer_info.layers[outer->children[1].children[0].index], &nm, NULL);
        CHECK_EQ_STR(nm, "Gr\xC3\xBC\xC3\x9F" "e");
        free(nm);
    }
    pc_psd_nodes_free(t, nt);
    pc_psd_file_fini(&f);
    b = pc_psd_builder_new(2, 2);
    s = pc_psd_layer_spec_make("bg", 0, 0, 2, 2, px8(PC_PSD_PX_RGBA8, white, 16));
    pc_psd_builder_push_layer(b, &s, NULL);
    g = pc_psd_group_spec_make("A");
    pc_psd_builder_begin_group(b, &g, NULL);
    s = pc_psd_layer_spec_make("a1", 0, 0, 1, 1, px8(PC_PSD_PX_RGBA8, white, 4));
    pc_psd_builder_push_layer(b, &s, NULL);
    g = pc_psd_group_spec_make("B");
    g.open = false;
    pc_psd_builder_begin_group(b, &g, NULL);
    s = pc_psd_layer_spec_make("b1", 0, 0, 1, 1, px8(PC_PSD_PX_RGBA8, white, 4));
    pc_psd_builder_push_layer(b, &s, NULL);
    pc_psd_builder_end_group(b, NULL);
    pc_psd_builder_end_group(b, NULL);
    CHECK_EQ_INT(pc_psd_builder_to_bytes(b, &bytes, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_file_parse(bytes.data, bytes.len, &f, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_layer_tree(&f, &t, &nt, NULL), PC_OK);
    names(&f, t, nt, &o);
    CHECK_EQ_STR(pc_buf_cstr(&o), "bg,A[a1,B[b1]]");
    pc_buf_fini(&o);
    pc_psd_nodes_free(t, nt);
    pc_psd_file_fini(&f);
    pc_psd_bytes_fini(&bytes);
    pc_psd_builder_free(b);
}

/* ---- malformed.rs ---- */

static int truncation_sweep(const pc_psd_bytes *b)
{
    size_t cut;
    int bad = 0;
    pc_psd_file f;
    for (cut = 0; cut < b->len; cut++) {
        if (pc_psd_file_parse(b->data, cut, &f, NULL) == PC_OK) {
            bad++;
            pc_psd_file_fini(&f);
        }
    }
    if (pc_psd_file_parse(b->data, b->len, &f, NULL) == PC_OK) {
        pc_psd_file_fini(&f);
    } else {
        bad++;
    }
    return bad;
}

static void test_truncations(void)
{
    int v;
    uint16_t c;
    int m;
    int bad = 0;
    for (v = 1; v <= 2; v++) {
        for (c = 0; c < 4; c++) {
            pc_psd_file f = tg_small((pc_psd_version)v, c);
            pc_psd_bytes b = bytes_of(&f);
            bad += truncation_sweep(&b);
            pc_psd_bytes_fini(&b);
            pc_psd_file_fini(&f);
        }
    }
    for (m = 0; m < TG_MODES; m++) {
        uint16_t d[3];
        size_t nd = tg_mode_depths(TG_MODE_LIST[m], d);
        size_t j;
        for (j = 0; j < nd; j++) {
            pc_psd_file f = tg_merged_only(PC_PSD_V1, TG_MODE_LIST[m], d[j], PC_PSD_RLE, 5, 3);
            pc_psd_bytes b = bytes_of(&f);
            bad += truncation_sweep(&b);
            pc_psd_bytes_fini(&b);
            pc_psd_file_fini(&f);
        }
    }
    {
        uint16_t px[24];
        pc_psd_builder *bl = pc_psd_builder_new(3, 2);
        pc_psd_pixels p = {PC_PSD_PX_RGBA16, px, 24};
        pc_psd_layer_spec s = pc_psd_layer_spec_make("x", 0, 0, 3, 2, p);
        pc_psd_bytes b = {NULL, 0};
        int i;
        for (i = 0; i < 24; i++) {
            px[i] = 1000;
        }
        pc_psd_builder_depth(bl, 16);
        pc_psd_builder_push_layer(bl, &s, NULL);
        CHECK_EQ_INT(pc_psd_builder_to_bytes(bl, &b, NULL), PC_OK);
        bad += truncation_sweep(&b);
        pc_psd_bytes_fini(&b);
        pc_psd_builder_free(bl);
    }
    CHECK_EQ_INT(bad, 0);
}

static void test_bad_headers(void)
{
    pc_psd_file f = tg_small(PC_PSD_V1, PC_PSD_RAW);
    pc_psd_bytes good = bytes_of(&f);
    pc_psd_file p;
    pc_error e;
    uint8_t *b = malloc(good.len);
    static const uint16_t vers[3] = {0, 3, 0xffff};
    int i;
    CHECK_EQ_INT(pc_psd_file_parse(NULL, 0, &p, &e), PC_ERR_FORMAT);
    CHECK_CONTAINS(e.msg, "unexpected end of data");
    memcpy(b, good.data, good.len);
    memcpy(b, "8BPX", 4);
    CHECK_EQ_INT(pc_psd_file_parse(b, good.len, &p, &e), PC_ERR_FORMAT);
    CHECK_CONTAINS(e.msg, "invalid signature");
    for (i = 0; i < 3; i++) {
        char want[64];
        memcpy(b, good.data, good.len);
        pc_store_be16(b + 4, vers[i]);
        CHECK_EQ_INT(pc_psd_file_parse(b, good.len, &p, &e), PC_ERR_UNSUPPORTED);
        (void)snprintf(want, sizeof want, "unsupported file version %u", (unsigned)vers[i]);
        CHECK_EQ_STR(e.msg, want);
    }
    memcpy(b, good.data, good.len);
    pc_store_be16(b + 22, 7);
    CHECK(pc_psd_file_parse(b, good.len, &p, NULL) != PC_OK);
    memcpy(b, good.data, good.len);
    pc_store_be16(b + 12, 0);
    CHECK(pc_psd_file_parse(b, good.len, &p, NULL) != PC_OK);
    pc_store_be16(b + 12, 57);
    CHECK(pc_psd_file_parse(b, good.len, &p, NULL) != PC_OK);
    for (i = 0; i < 3; i++) {
        memcpy(b, good.data, good.len);
        memset(b + (i == 1 ? 18 : 14), 0, i == 2 ? 8 : 4);
        CHECK(pc_psd_file_parse(b, good.len, &p, &e) != PC_OK);
        CHECK_CONTAINS(e.msg, "at least 1x1");
    }
    f.header.width = 0;
    CHECK(pc_psd_file_validate(&f, NULL) != PC_OK);
    /* resource signature, layer blend signature, oversized section lengths */
    memcpy(b, good.data, good.len);
    memcpy(b + 34, "XXXX", 4);
    CHECK_EQ_INT(pc_psd_file_parse(b, good.len, &p, &e), PC_ERR_FORMAT);
    CHECK_CONTAINS(e.msg, "invalid signature");
    memcpy(b, good.data, good.len);
    {
        size_t at;
        for (at = 0; at + 8 <= good.len; at++) {
            if (memcmp(b + at, "8BIMnorm", 8) == 0) {
                break;
            }
        }
        CHECK(at + 8 <= good.len);
        b[at] = 'X';
        CHECK(pc_psd_file_parse(b, good.len, &p, NULL) != PC_OK);
    }
    memcpy(b, good.data, good.len);
    pc_store_be32(b + 26, 0xFFFFFFFFu);
    CHECK(pc_psd_file_parse(b, good.len, &p, NULL) != PC_OK);
    memcpy(b, good.data, good.len);
    pc_store_be32(b + 30, 0xFFFFFFFFu);
    CHECK(pc_psd_file_parse(b, good.len, &p, NULL) != PC_OK);
    free(b);
    pc_psd_bytes_fini(&good);
    pc_psd_file_fini(&f);
    /* oversized dimensions */
    f = tg_small(PC_PSD_V2, PC_PSD_RAW);
    good = bytes_of(&f);
    pc_store_be32(good.data + 14, 300001);
    CHECK_EQ_INT(pc_psd_file_parse(good.data, good.len, &p, &e), PC_ERR_LIMIT);
    pc_psd_bytes_fini(&good);
    pc_psd_file_fini(&f);
}

static void test_bombs_and_lazy_errors(void)
{
    pc_psd_file f = tg_merged_only(PC_PSD_V2, PC_PSD_MODE_GRAYSCALE, 8, PC_PSD_RLE, 1, 1);
    pc_psd_file p;
    pc_psd_bytes b;
    pc_psd_bytes d = {NULL, 0};
    pc_psd_image8 img;
    static const uint8_t zero100[100];
    size_t i;
    int16_t id;
    f.header.width = 300000;
    f.header.height = 300000;
    b = bytes_of(&f);
    CHECK(pc_psd_file_parse(b.data, b.len, &p, NULL) != PC_OK);
    pc_psd_bytes_fini(&b);
    f.image_compression = PC_PSD_ZIP;
    pc_psd_zip_compress(zero100, 100, &f.image_data, NULL);
    b = bytes_of(&f);
    CHECK(pc_psd_file_parse(b.data, b.len, &p, NULL) != PC_OK);
    pc_psd_bytes_fini(&b);
    pc_psd_file_fini(&f);
    /* corrupt zip merged */
    f = tg_small(PC_PSD_V1, PC_PSD_ZIP);
    f.image_data.data[f.image_data.len / 2] ^= 0xff;
    f.image_data.data[f.image_data.len / 2 + 1] ^= 0x55;
    b = bytes_of(&f);
    CHECK(pc_psd_file_parse(b.data, b.len, &p, NULL) != PC_OK);
    pc_psd_bytes_fini(&b);
    pc_psd_file_fini(&f);
    /* corrupt layer channel: lazy error */
    f = tg_small(PC_PSD_V1, PC_PSD_RLE);
    id = f.layer_info.layers[0].channels[1].id;
    for (i = 0; i < f.layer_info.layers[0].channels[1].data.len; i++) {
        f.layer_info.layers[0].channels[1].data.data[i] = 0x7f;
    }
    b = bytes_of(&f);
    CHECK_EQ_INT(pc_psd_file_parse(b.data, b.len, &p, NULL), PC_OK);
    CHECK(pc_psd_layer_decode_channel(&p.layer_info.layers[0], id, 8, PC_PSD_V1, &d, NULL) != PC_OK);
    CHECK(pc_psd_layer_rgba8(&p, 0, &img, NULL) != PC_OK);
    pc_psd_file_fini(&p);
    pc_psd_bytes_fini(&b);
    pc_psd_file_fini(&f);
    /* negative layer rect */
    f = tg_small(PC_PSD_V1, PC_PSD_RAW);
    f.layer_info.layers[0].rect = (pc_psd_rect){10, 0, 0, 4};
    b = bytes_of(&f);
    CHECK_EQ_INT(pc_psd_file_parse(b.data, b.len, &p, NULL), PC_OK);
    CHECK(pc_psd_layer_decode_channel(&p.layer_info.layers[0], 0, 8, PC_PSD_V1, &d, NULL) != PC_OK);
    CHECK(pc_psd_layer_rgba8(&p, 0, &img, NULL) != PC_OK);
    pc_psd_file_fini(&p);
    pc_psd_bytes_fini(&b);
    pc_psd_file_fini(&f);
}

static void test_too_many_layer_channels(void)
{
    pc_psd_file f = tg_merged_only(PC_PSD_V1, PC_PSD_MODE_RGB, 8, PC_PSD_RAW, 1, 1);
    pc_psd_bytes b;
    pc_buf out = PC_BUF_INIT;
    uint8_t rec[20];
    uint8_t len4[4];
    size_t lm;
    uint32_t old;
    pc_psd_file p;
    pc_error e;
    f.has_layer_info = true;
    b = bytes_of(&f);
    lm = 34 + pc_load_be32(b.data + 30);
    memset(rec, 0, sizeof rec);
    rec[1] = 1;
    pc_store_be16(rec + 18, 0xffff);
    pc_buf_append(&out, b.data, lm, NULL);
    pc_store_be32(len4, 4 + 20);
    pc_buf_append(&out, len4, 4, NULL);
    pc_store_be32(len4, 20);
    pc_buf_append(&out, len4, 4, NULL);
    pc_buf_append(&out, rec, 20, NULL);
    old = pc_load_be32(b.data + lm);
    pc_buf_append(&out, b.data + lm + 4 + old, b.len - lm - 4 - old, NULL);
    CHECK(pc_psd_file_parse(out.data, out.len, &p, &e) != PC_OK);
    CHECK(e.code == PC_ERR_LIMIT || e.code == PC_ERR_FORMAT);
    pc_buf_fini(&out);
    pc_psd_bytes_fini(&b);
    pc_psd_file_fini(&f);
}

/* Exercises everything that a parsed file offers (must never crash). */
static void poke(const pc_psd_file *f)
{
    pc_psd_bytes b = {NULL, 0};
    pc_psd_image8 img;
    pc_psd_node *t = NULL;
    size_t nt;
    size_t nl;
    size_t i;
    size_t k;
    const pc_psd_layer *layers = pc_psd_file_layers(f, &nl);
    if (pc_psd_file_to_bytes(f, &b, NULL) == PC_OK) {
        pc_psd_bytes_fini(&b);
    }
    if (pc_psd_file_decode_merged(f, &b, NULL) == PC_OK) {
        pc_psd_bytes_fini(&b);
    }
    if (pc_psd_composite_rgba8(f, &img, NULL) == PC_OK) {
        pc_psd_image8_fini(&img);
    }
    if (pc_psd_layer_tree(f, &t, &nt, NULL) == PC_OK) {
        pc_psd_nodes_free(t, nt);
    }
    for (i = 0; i < nl; i++) {
        char *nm = NULL;
        if (pc_psd_layer_rgba8(f, i, &img, NULL) == PC_OK) {
            pc_psd_image8_fini(&img);
        }
        if (pc_psd_layer_user_mask(f, i, &img, NULL) == PC_OK) {
            pc_psd_image8_fini(&img);
        }
        if (pc_psd_layer_name(&layers[i], &nm, NULL) == PC_OK) {
            free(nm);
        }
        for (k = 0; k < layers[i].nblocks; k++) {
            (void)pc_psd_block_check_typed(&layers[i].blocks[k], NULL);
            (void)pc_psd_block_check_structure(&layers[i].blocks[k], NULL);
        }
    }
    for (i = 0; i < f->nresources; i++) {
        pc_psd_resolution r;
        pc_psd_version_info vi;
        int32_t v;
        uint16_t *ids = NULL;
        size_t n;
        (void)pc_psd_resolution_parse(f->resources[i].data.data, f->resources[i].data.len, &r, NULL);
        if (pc_psd_version_info_parse(f->resources[i].data.data, f->resources[i].data.len, &vi, NULL) == PC_OK) {
            pc_psd_version_info_fini(&vi);
        }
        (void)pc_psd_res_i32(&f->resources[i], &v, NULL);
        if (pc_psd_res_group_ids(&f->resources[i], &ids, &n, NULL) == PC_OK) {
            free(ids);
        }
    }
}

static void test_fuzz(void)
{
    int k;
    int unstable = 0;
    pc_psd_bytes sm[2][4];
    pc_psd_bytes head;
    int v;
    uint16_t c;
    for (v = 0; v < 2; v++) {
        for (c = 0; c < 4; c++) {
            pc_psd_file f = tg_small(v == 0 ? PC_PSD_V1 : PC_PSD_V2, c);
            sm[v][c] = bytes_of(&f);
            pc_psd_file_fini(&f);
        }
    }
    head = sm[0][0];
    for (k = 0; k < 600; k++) {
        uint8_t data[512 + 26];
        size_t n = rnd() % 512;
        size_t i;
        pc_psd_file f;
        for (i = 0; i < n; i++) {
            data[26 + i] = (uint8_t)rnd();
        }
        if (pc_psd_file_parse(data + 26, n, &f, NULL) == PC_OK) {
            poke(&f);
            pc_psd_file_fini(&f);
        }
        memcpy(data, head.data, 26);
        if (pc_psd_file_parse(data, n + 26, &f, NULL) == PC_OK) {
            poke(&f);
            pc_psd_file_fini(&f);
        }
    }
    for (k = 0; k < 1500; k++) {
        const pc_psd_bytes *src = &sm[rnd() % 2][rnd() % 4];
        uint8_t *m = malloc(src->len);
        int j;
        pc_psd_file f;
        memcpy(m, src->data, src->len);
        for (j = 0; j < 1 + (int)(rnd() % 7); j++) {
            m[rnd() % src->len] = (uint8_t)rnd();
        }
        if (pc_psd_file_parse(m, src->len, &f, NULL) == PC_OK) {
            poke(&f);
            pc_psd_file_fini(&f);
        }
        free(m);
    }
    /* parsed single-byte mutants of an RLE file are model-stable */
    for (k = 0; k < 1500; k++) {
        const pc_psd_bytes *src = &sm[0][1];
        uint8_t *m = malloc(src->len);
        pc_psd_file f;
        memcpy(m, src->data, src->len);
        m[rnd() % src->len] = (uint8_t)rnd();
        if (pc_psd_file_parse(m, src->len, &f, NULL) == PC_OK) {
            pc_psd_bytes out = {NULL, 0};
            pc_psd_file g;
            if (pc_psd_file_to_bytes(&f, &out, NULL) != PC_OK ||
                pc_psd_file_parse(out.data, out.len, &g, NULL) != PC_OK) {
                unstable++;
            } else {
                unstable += !pc_psd_file_eq(&g, &f);
                pc_psd_file_fini(&g);
            }
            pc_psd_bytes_fini(&out);
            pc_psd_file_fini(&f);
        }
        free(m);
    }
    CHECK_EQ_INT(unstable, 0);
    for (v = 0; v < 2; v++) {
        for (c = 0; c < 4; c++) {
            pc_psd_bytes_fini(&sm[v][c]);
        }
    }
}

/* ---- proptests.rs (file-level) ---- */

static void test_random_files_roundtrip(void)
{
    int k;
    int bad = 0;
    for (k = 0; k < 120; k++) {
        uint16_t mode = TG_MODE_LIST[rnd() % TG_MODES];
        uint16_t d[3];
        size_t nd = tg_mode_depths(mode, d);
        pc_psd_file f = tg_merged_only(rnd() % 2 ? PC_PSD_V1 : PC_PSD_V2, mode, d[rnd() % nd], (uint16_t)(rnd() % 4),
                                       (uint32_t)(1 + rnd() % 29), (uint32_t)(1 + rnd() % 9));
        bad += !stable(&f, "random merged");
        pc_psd_file_fini(&f);
    }
    for (k = 0; k < 60; k++) {
        bool sixteen = rnd() % 2 != 0;
        pc_psd_builder *b = pc_psd_builder_new(16, 16);
        size_t nlayers = rnd() % 6;
        size_t i;
        uint8_t *px8s[6];
        uint16_t *px16s[6];
        char names_[6][64];
        uint8_t ops[6];
        int bms[6];
        bool vis[6];
        size_t sizes[6];
        pc_psd_bytes bytes = {NULL, 0};
        pc_psd_file f;
        static const char *const pool[6] = {"a", "Zb 9", "\xC3\xA4\xC3\xB6\xC3\xBC", "\xF0\x9F\x98\x80x", "", "Ab Cd"};
        pc_psd_builder_version(b, rnd() % 2 ? PC_PSD_V1 : PC_PSD_V2);
        pc_psd_builder_compression(b, (uint16_t)(rnd() % 4));
        pc_psd_builder_depth(b, sixteen ? 16 : 8);
        for (i = 0; i < nlayers; i++) {
            int32_t x = (int32_t)(rnd() % 40) - 20;
            int32_t y = (int32_t)(rnd() % 40) - 20;
            uint32_t w = (uint32_t)(rnd() % 9);
            uint32_t h = (uint32_t)(rnd() % 9);
            size_t n = (size_t)w * h;
            size_t q;
            pc_psd_pixels p;
            pc_psd_layer_spec s;
            px8s[i] = malloc(n * 4 + 1);
            px16s[i] = malloc(n * 8 + 2);
            for (q = 0; q < n * 4; q++) {
                px8s[i][q] = (uint8_t)(q * 31 + i * 7);
                px16s[i][q] = (uint16_t)(px8s[i][q] * 257u);
            }
            sizes[i] = n * 4;
            (void)snprintf(names_[i], sizeof names_[i], "%s", pool[rnd() % 6]);
            ops[i] = (uint8_t)rnd();
            bms[i] = (int)(rnd() % PC_PSD_BLEND_COUNT);
            vis[i] = rnd() % 2 != 0;
            p.kind = sixteen ? PC_PSD_PX_RGBA16 : PC_PSD_PX_RGBA8;
            p.data = sixteen ? (const void *)px16s[i] : (const void *)px8s[i];
            p.len = n * 4;
            s = pc_psd_layer_spec_make(names_[i], x, y, w, h, p);
            s.opacity = ops[i];
            memcpy(s.blend, PC_PSD_BLEND_KEYS[bms[i]], 4);
            s.visible = vis[i];
            pc_psd_builder_push_layer(b, &s, NULL);
        }
        if (pc_psd_builder_to_bytes(b, &bytes, NULL) != PC_OK ||
            pc_psd_file_parse(bytes.data, bytes.len, &f, NULL) != PC_OK) {
            bad++;
        } else {
            pc_psd_bytes again = {NULL, 0};
            size_t nl;
            (void)pc_psd_file_layers(&f, &nl);
            bad += pc_psd_file_to_bytes(&f, &again, NULL) != PC_OK || !pc_psd_bytes_eq(&again, &bytes);
            bad += nl != nlayers;
            for (i = 0; i < nl && i < nlayers; i++) {
                pc_psd_image8 img;
                char *nm = NULL;
                const pc_psd_layer *l = &f.layer_info.layers[i];
                pc_psd_layer_name(l, &nm, NULL);
                bad += strcmp(nm, names_[i]) != 0;
                free(nm);
                if (pc_psd_layer_rgba8(&f, i, &img, NULL) == PC_OK) {
                    bad += img.data.len != sizes[i] || (sizes[i] > 0 && memcmp(img.data.data, px8s[i], sizes[i]) != 0);
                    pc_psd_image8_fini(&img);
                } else {
                    bad++;
                }
                bad += l->opacity != ops[i] || memcmp(l->blend, PC_PSD_BLEND_KEYS[bms[i]], 4) != 0 ||
                       pc_psd_layer_is_visible(l) != vis[i];
            }
            pc_psd_bytes_fini(&again);
            pc_psd_file_fini(&f);
        }
        pc_psd_bytes_fini(&bytes);
        for (i = 0; i < nlayers; i++) {
            free(px8s[i]);
            free(px16s[i]);
        }
        pc_psd_builder_free(b);
    }
    for (k = 0; k < 80; k++) {
        pc_psd_file f = tg_small(rnd() % 2 ? PC_PSD_V1 : PC_PSD_V2, PC_PSD_RAW);
        size_t nb = rnd() % 8;
        size_t i;
        for (i = 0; i < nb; i++) {
            uint8_t key[4];
            uint8_t data[40];
            size_t dl = rnd() % 40;
            size_t pad = rnd() % 4;
            size_t q;
            pc_psd_block *bl;
            for (q = 0; q < 4; q++) {
                key[q] = (uint8_t)rnd();
            }
            for (q = 0; q < dl; q++) {
                data[q] = (uint8_t)rnd();
            }
            tg_global(&f, (const char *)key, data, dl);
            bl = &f.global_blocks[f.nglobal - 1];
            if (rnd() % 2) {
                memcpy(bl->signature, "8B64", 4);
            }
            /* Only padding the detector can recover: zeros up to 3 bytes. */
            if (pad > 0 && pad - 1 != dl % 2) {
                bl->padding.has = true;
                if (pad > 1) {
                    bl->padding.v.data = calloc(pad - 1, 1);
                    bl->padding.v.len = pad - 1;
                }
            }
        }
        bad += !stable(&f, "random global blocks");
        pc_psd_file_fini(&f);
    }
    CHECK_EQ_INT(bad, 0);
}

int main(int argc, char **argv)
{
    RUN(test_all_generated_cases);
    RUN(test_merged_only_decodes_to_pattern);
    RUN(test_layered_and_merged_cases);
    RUN(test_psb_lengths);
    RUN(test_psb_long_key_global_block);
    RUN(test_edits_and_passthrough);
    RUN(test_unknown_mode_compression_and_trailing);
    RUN(test_accessors_and_validation);
    RUN(test_save_and_open);
    RUN(test_spec_order_density_only_mask_round_trips);
    RUN(test_global_layer_block_roundtrips_with_neighbours);
    RUN(test_builder_rgba8_cases);
    RUN(test_builder_depths_and_modes);
    RUN(test_builder_layer_properties);
    RUN(test_builder_masks);
    RUN(test_builder_misc);
    RUN(test_builder_errors);
    RUN(test_builder_groups);
    RUN(test_layer_trees);
    RUN(test_generated_and_builder_trees);
    RUN(test_truncations);
    RUN(test_bad_headers);
    RUN(test_bombs_and_lazy_errors);
    RUN(test_too_many_layer_channels);
    RUN(test_fuzz);
    RUN(test_random_files_roundtrip);
    return pc_t_finish();
}
