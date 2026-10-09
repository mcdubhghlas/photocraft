/* Ports the unit tests of crates/psd/src/{tagged,layer,pixels,file}.rs. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "harness.h"
#include "psd/file.h"
#include "psd/internal.h"
#include "psd/pixels.h"
#include "testgen.h"

/* ---- tagged.rs ---- */

static pc_psd_bytes write_blocks(const pc_psd_block *b, size_t n, pc_psd_version v)
{
    psd_w w;
    pc_psd_bytes out = {NULL, 0};
    psd_w_init(&w, NULL);
    psd_write_blocks(&w, b, n, v);
    CHECK_EQ_INT(psd_w_finish(&w, &out), PC_OK);
    return out;
}

/* Writes, reads back, checks equality and no trailing bytes; returns the bytes. */
static pc_psd_bytes rt_blocks(const pc_psd_block *b, size_t n, pc_psd_version v)
{
    pc_psd_bytes out = write_blocks(b, n, v);
    psd_rd r = psd_rd_make(out.data, out.len);
    pc_psd_block *back = NULL;
    size_t nb = 0;
    pc_psd_bytes trailing = {NULL, 0};
    CHECK_EQ_INT(psd_read_blocks(&r, v, &back, &nb, &trailing, NULL), PC_OK);
    CHECK(pc_psd_blocks_eq(back, nb, b, n));
    CHECK_EQ_UINT(trailing.len, 0);
    pc_psd_blocks_free(back, nb);
    pc_psd_bytes_fini(&trailing);
    return out;
}

static void test_unicode_name_typed(void)
{
    pc_psd_block b;
    char *name = NULL;
    pc_psd_bytes out;
    CHECK_EQ_INT(pc_psd_block_make_unicode_name("Ebene \xC3\xA4\xF0\x9F\x98\x80", &b, NULL), PC_OK);
    CHECK_EQ_UINT(b.data.len % 4, 0);
    CHECK_EQ_INT(pc_psd_block_unicode_name(&b, &name, NULL), PC_OK);
    CHECK_EQ_STR(name, "Ebene \xC3\xA4\xF0\x9F\x98\x80");
    free(name);
    out = rt_blocks(&b, 1, PC_PSD_V1);
    pc_psd_bytes_fini(&out);
    pc_psd_block_fini(&b);
}

static void test_section_divider_variants(void)
{
    static const struct {
        uint32_t kind;
        const char *blend;
        bool sub;
        size_t len;
    } c[3] = {{PC_PSD_SECTION_OPEN, NULL, false, 4}, {PC_PSD_SECTION_CLOSED, "pass", false, 12},
              {PC_PSD_SECTION_DIVIDER, "mul ", true, 16}};
    uint32_t one = 1;
    int i;
    pc_psd_block b;
    pc_psd_section_divider s;
    for (i = 0; i < 3; i++) {
        CHECK_EQ_INT(pc_psd_block_make_section(c[i].kind, (const uint8_t *)c[i].blend, c[i].sub ? &one : NULL, &b, NULL),
                     PC_OK);
        CHECK_EQ_UINT(b.data.len, c[i].len);
        CHECK_EQ_INT(pc_psd_block_section(&b, &s, NULL), PC_OK);
        CHECK(s.kind == c[i].kind && s.has_blend == (c[i].blend != NULL) && s.has_sub_type == c[i].sub);
        CHECK(c[i].blend == NULL || memcmp(s.blend, c[i].blend, 4) == 0);
        CHECK(!c[i].sub || s.sub_type == 1);
        pc_psd_block_fini(&b);
    }
    CHECK_EQ_INT(pc_psd_block_make_section(PC_PSD_SECTION_OPEN, (const uint8_t *)"norm", NULL, &b, NULL), PC_OK);
    b.data.data[4] = 'X';
    CHECK(pc_psd_block_section(&b, &s, NULL) != PC_OK);
    CHECK(pc_psd_block_check_typed(&b, NULL) != PC_OK);
    pc_psd_block_fini(&b);
    CHECK_EQ_INT(pc_psd_block_make_section(PC_PSD_SECTION_OPEN, NULL, NULL, &b, NULL), PC_OK);
    memcpy(b.key, "lsdk", 4);
    CHECK(pc_psd_block_has_typed(&b) && pc_psd_block_section(&b, &s, NULL) == PC_OK);
    pc_psd_block_fini(&b);
}

static void test_simple_typed_blocks(void)
{
    static const uint8_t cont[4] = {'c', 'o', 'n', 't'};
    static const uint8_t one4[4] = {1, 0, 0, 0};
    pc_psd_block b;
    uint32_t u32;
    uint16_t u16;
    uint8_t u8;
    uint8_t a4[4];
    CHECK_EQ_INT(pc_psd_block_make_u32("lyid", 7, &b, NULL), PC_OK);
    CHECK(pc_psd_block_u32(&b, &u32, NULL) == PC_OK && u32 == 7);
    pc_psd_block_fini(&b);
    CHECK_EQ_INT(pc_psd_block_make_name_source(cont, &b, NULL), PC_OK);
    CHECK(pc_psd_block_name_source(&b, a4, NULL) == PC_OK && memcmp(a4, cont, 4) == 0);
    pc_psd_block_fini(&b);
    CHECK_EQ_INT(pc_psd_block_make_flag("vmgm", 1, &b, NULL), PC_OK);
    CHECK(b.data.len == 4 && memcmp(b.data.data, one4, 4) == 0);
    CHECK(pc_psd_block_u8(&b, &u8, NULL) == PC_OK && u8 == 1);
    pc_psd_block_fini(&b);
    CHECK_EQ_INT(pc_psd_block_init(&b, "tsly", NULL, 0, NULL), PC_OK);
    CHECK(pc_psd_block_check_typed(&b, NULL) != PC_OK); /* truncated flag: an error, not a crash */
    pc_psd_block_fini(&b);
    CHECK_EQ_INT(pc_psd_block_make_u32("lspf", 0x80000000u, &b, NULL), PC_OK);
    CHECK(pc_psd_block_u32(&b, &u32, NULL) == PC_OK && u32 == 0x80000000u);
    pc_psd_block_fini(&b);
    CHECK_EQ_INT(pc_psd_block_make_sheet_color(3, &b, NULL), PC_OK);
    CHECK(pc_psd_block_u16(&b, &u16, NULL) == PC_OK && u16 == 3 && b.data.len == 8);
    pc_psd_block_fini(&b);
    CHECK_EQ_INT(pc_psd_block_make_flag("iOpa", 128, &b, NULL), PC_OK);
    CHECK(pc_psd_block_u8(&b, &u8, NULL) == PC_OK && u8 == 128);
    pc_psd_block_fini(&b);
    CHECK_EQ_INT(pc_psd_block_init(&b, "lfx2", NULL, 0, NULL), PC_OK);
    CHECK(!pc_psd_block_has_typed(&b));
    pc_psd_block_fini(&b);
    CHECK_EQ_INT(pc_psd_block_init(&b, "lyid", one4, 1, NULL), PC_OK);
    CHECK(pc_psd_block_check_typed(&b, NULL) != PC_OK);
    pc_psd_block_fini(&b);
}

static void test_padding_rules(void)
{
    static const uint8_t d3[3] = {1, 2, 3};
    static const uint8_t d2[2] = {1, 2};
    static const uint8_t d1[1] = {1};
    static const uint8_t z[8];
    pc_psd_block b[3];
    pc_psd_bytes out;
    psd_rd r;
    pc_psd_block *back = NULL;
    size_t nb = 0;
    pc_psd_bytes trailing = {NULL, 0};
    CHECK_EQ_INT(pc_psd_block_init(&b[0], "abcd", d3, 3, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_block_clone(&b[0], &b[1], NULL), PC_OK);
    out = rt_blocks(b, 2, PC_PSD_V1);
    CHECK_EQ_UINT(out.len, 2 * (12 + 4));
    pc_psd_bytes_fini(&out);
    pc_psd_block_fini(&b[1]);
    b[0].padding.has = true;
    pc_psd_bytes_set(&b[0].padding.v, z, 3, NULL);
    pc_psd_block_init(&b[1], "efgh", d2, 2, NULL);
    b[1].padding.has = true;
    pc_psd_bytes_set(&b[1].padding.v, z, 2, NULL);
    pc_psd_block_init(&b[2], "ijkl", d1, 1, NULL);
    b[2].padding.has = true;
    out = rt_blocks(b, 3, PC_PSD_V1);
    pc_psd_bytes_fini(&out);
    pc_psd_block_fini(&b[0]);
    pc_psd_block_fini(&b[1]);
    pc_psd_block_fini(&b[2]);
    /* 8B64 kept */
    pc_psd_block_init(&b[0], "Lr16", z, 6, NULL);
    memcpy(b[0].signature, "8B64", 4);
    out = rt_blocks(b, 1, PC_PSD_V2);
    pc_psd_bytes_fini(&out);
    pc_psd_block_fini(&b[0]);
    /* trailing bytes: 8 zeros are not padding; 2 zeros at the end are */
    pc_psd_block_make_u32("lyid", 1, &b[0], NULL);
    out = write_blocks(b, 1, PC_PSD_V1);
    out.data = realloc(out.data, out.len + 8);
    memset(out.data + out.len, 0, 8);
    r = psd_rd_make(out.data, out.len + 8);
    CHECK_EQ_INT(psd_read_blocks(&r, PC_PSD_V1, &back, &nb, &trailing, NULL), PC_OK);
    CHECK(nb == 1 && trailing.len == 8);
    pc_psd_blocks_free(back, nb);
    pc_psd_bytes_fini(&trailing);
    r = psd_rd_make(out.data, out.len + 2);
    CHECK_EQ_INT(psd_read_blocks(&r, PC_PSD_V1, &back, &nb, &trailing, NULL), PC_OK);
    CHECK(nb == 1 && back[0].padding.has && back[0].padding.v.len == 2 && trailing.len == 0);
    pc_psd_blocks_free(back, nb);
    pc_psd_bytes_fini(&trailing);
    pc_psd_bytes_fini(&out);
    pc_psd_block_fini(&b[0]);
}

static void test_psb_long_keys_and_overlong(void)
{
    static const uint8_t seven[4] = {7, 7, 7, 7};
    int i;
    psd_w w;
    pc_psd_bytes out = {NULL, 0};
    psd_rd r;
    pc_psd_block *back = NULL;
    size_t nb = 0;
    pc_psd_bytes trailing = {NULL, 0};
    for (i = 0; i < PC_PSD_LONG_KEY_COUNT; i++) {
        pc_psd_block b;
        CHECK(pc_psd_uses_long_length(PC_PSD_V2, (const uint8_t *)PC_PSD_LONG_KEYS[i]));
        CHECK(!pc_psd_uses_long_length(PC_PSD_V1, (const uint8_t *)PC_PSD_LONG_KEYS[i]));
        pc_psd_block_init(&b, PC_PSD_LONG_KEYS[i], seven, 4, NULL);
        out = rt_blocks(&b, 1, PC_PSD_V2);
        CHECK(out.len == 8 + 8 + 4 && pc_psd_block_encoded_len(&b, PC_PSD_V2) == out.len);
        pc_psd_bytes_fini(&out);
        out = rt_blocks(&b, 1, PC_PSD_V1);
        CHECK_EQ_UINT(out.len, 8 + 4 + 4);
        pc_psd_bytes_fini(&out);
        pc_psd_block_fini(&b);
    }
    CHECK(!pc_psd_uses_long_length(PC_PSD_V2, (const uint8_t *)"luni"));
    psd_w_init(&w, NULL);
    psd_put(&w, "8BIMabcd", 8);
    psd_put_u32(&w, 100);
    psd_put_zeros(&w, 4);
    psd_w_finish(&w, &out);
    r = psd_rd_make(out.data, out.len);
    CHECK(psd_read_blocks(&r, PC_PSD_V1, &back, &nb, &trailing, NULL) != PC_OK);
    pc_psd_bytes_fini(&out);
}

static void test_check_structure(void)
{
    pc_psd_block b;
    pc_psd_desc d;
    pc_psd_bytes vd = {NULL, 0};
    pc_buf data = PC_BUF_INIT;
    pc_error e;
    static const uint8_t zero4[4];
    /* lfx2: version + a valid versioned descriptor (+ up to 3 zeros) passes; garbage does not */
    d = tg_sample_descriptor();
    pc_psd_vdesc_to_bytes(&d, &vd, NULL);
    pc_buf_append(&data, zero4, 4, NULL);
    pc_buf_append(&data, vd.data, vd.len, NULL);
    pc_buf_append(&data, zero4, 3, NULL);
    pc_psd_block_init(&b, "lfx2", data.data, data.len, NULL);
    CHECK_EQ_INT(pc_psd_block_check_structure(&b, NULL), PC_OK);
    pc_psd_block_fini(&b);
    pc_buf_append(&data, "x", 1, NULL);
    pc_psd_block_init(&b, "lfx2", data.data, data.len, NULL);
    CHECK_EQ_INT(pc_psd_block_check_structure(&b, &e), PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "invalid data: lfx2: 4 unexpected bytes after the block structure");
    pc_psd_block_fini(&b);
    /* SoLd: a descriptor without Idnt is refused with the key prefix */
    pc_buf_truncate(&data, 0);
    pc_buf_append(&data, "soLD", 4, NULL);
    pc_buf_append(&data, zero4, 4, NULL);
    pc_buf_append(&data, vd.data, vd.len, NULL);
    pc_psd_block_init(&b, "SoLd", data.data, data.len, NULL);
    CHECK_EQ_INT(pc_psd_block_check_structure(&b, &e), PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "invalid data: SoLd: invalid data: no Idnt (unique id)");
    pc_psd_block_fini(&b);
    /* PlLd with the wrong kind */
    pc_psd_block_init(&b, "PlLd", "xxxx", 4, NULL);
    CHECK_EQ_INT(pc_psd_block_check_structure(&b, &e), PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "invalid data: expected plcL structure, found \"xxxx\"");
    pc_psd_block_fini(&b);
    /* lnk2 with an unknown item type */
    pc_buf_truncate(&data, 0);
    {
        uint8_t len8[8];
        pc_store_be64(len8, 4);
        pc_buf_append(&data, len8, 8, NULL);
        pc_buf_append(&data, "liFX", 4, NULL);
    }
    pc_psd_block_init(&b, "lnk2", data.data, data.len, NULL);
    CHECK_EQ_INT(pc_psd_block_check_structure(&b, &e), PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "invalid data: lnk2: linked item of unknown type");
    pc_psd_block_fini(&b);
    /* unknown keys pass */
    pc_psd_block_init(&b, "zzzz", "garbage", 7, NULL);
    CHECK_EQ_INT(pc_psd_block_check_structure(&b, NULL), PC_OK);
    pc_psd_block_fini(&b);
    pc_buf_fini(&data);
    pc_psd_bytes_fini(&vd);
    pc_psd_desc_fini(&d);
}

/* ---- layer.rs ---- */

static pc_psd_layer sample_record(pc_psd_version v)
{
    pc_psd_layer l;
    uint8_t px[20];
    static const int16_t ids[4] = {-1, 0, 1, 2};
    static const uint16_t comps[4] = {PC_PSD_RLE, PC_PSD_RAW, PC_PSD_ZIP, PC_PSD_ZIP_PREDICT};
    static const uint8_t m4[4] = {1, 2, 3, 4};
    pc_psd_block b;
    int i;
    pc_psd_layer_init(&l);
    for (i = 0; i < 20; i++) {
        px[i] = (uint8_t)i;
    }
    l.rect = pc_psd_rect_from_xywh(-3, 2, 5, 4);
    for (i = 0; i < 4; i++) {
        pc_psd_channel c;
        CHECK_EQ_INT(pc_psd_channel_encode(ids[i], comps[i], px, 20, 5, 4, 8, v, &c, NULL), PC_OK);
        pc_psd_layer_push_channel(&l, &c, NULL);
    }
    {
        pc_psd_channel c;
        CHECK_EQ_INT(pc_psd_channel_encode(-2, PC_PSD_RLE, m4, 4, 2, 2, 8, v, &c, NULL), PC_OK);
        pc_psd_layer_push_channel(&l, &c, NULL);
    }
    pc_psd_layer_mask_make(pc_psd_rect_from_xywh(0, 0, 2, 2), 255, PC_PSD_MF_RELATIVE, &l.mask.mask, NULL);
    l.mask.kind = PC_PSD_MASK_PARSED;
    memcpy(l.blend, "scrn", 4);
    l.opacity = 200;
    l.clipping = 1;
    l.flags = PC_PSD_LF_HIDDEN;
    pc_psd_blending_ranges_full(3, &l.blending_ranges, NULL);
    pc_psd_bytes_set(&l.name, "abc", 3, NULL);
    pc_psd_block_make_unicode_name("\xC3\xA4" "bc", &b, NULL);
    pc_psd_layer_push_block(&l, &b, NULL);
    pc_psd_block_make_u32("lyid", 5, &b, NULL);
    pc_psd_layer_push_block(&l, &b, NULL);
    return l;
}

/* Writes, parses, compares and re-writes a layer info body; returns the bytes. */
static pc_psd_bytes rt_info(const pc_psd_layer_info *li, pc_psd_version v)
{
    pc_psd_bytes out = {NULL, 0};
    pc_psd_bytes again = {NULL, 0};
    pc_psd_layer_info back;
    CHECK_EQ_INT(pc_psd_layer_info_to_bytes(li, v, &out, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_layer_info_parse(out.data, out.len, v, &back, NULL), PC_OK);
    CHECK(pc_psd_layer_info_eq(&back, li));
    CHECK_EQ_INT(pc_psd_layer_info_to_bytes(&back, v, &again, NULL), PC_OK);
    CHECK(pc_psd_bytes_eq(&again, &out));
    pc_psd_bytes_fini(&again);
    pc_psd_layer_info_fini(&back);
    return out;
}

static void test_layer_info_roundtrips(void)
{
    pc_psd_layer_info li;
    pc_psd_bytes out;
    int v;
    static const uint8_t zero2[2];
    memset(&li, 0, sizeof li);
    out = rt_info(&li, PC_PSD_V1);
    CHECK(out.len == 2 && out.data[0] == 0 && out.data[1] == 0);
    pc_psd_bytes_fini(&out);
    for (v = 1; v <= 2; v++) {
        pc_psd_layer d;
        memset(&li, 0, sizeof li);
        li.merged_alpha = true;
        {
            pc_psd_layer s = sample_record((pc_psd_version)v);
            pc_psd_layer_info_push(&li, &s, NULL);
        }
        pc_psd_layer_init(&d);
        pc_psd_layer_info_push(&li, &d, NULL);
        out = rt_info(&li, (pc_psd_version)v);
        CHECK(out.len % 2 == 0 && out.data[0] == 0xff && out.data[1] == 0xfe);
        pc_psd_bytes_fini(&out);
        pc_psd_layer_info_fini(&li);
    }
    /* PSB channel lengths are 8 bytes */
    {
        pc_psd_layer l;
        pc_psd_channel c;
        size_t a;
        memset(&li, 0, sizeof li);
        pc_psd_layer_init(&l);
        memset(&c, 0, sizeof c);
        c.has_compression = true;
        pc_psd_layer_push_channel(&l, &c, NULL);
        pc_psd_layer_info_push(&li, &l, NULL);
        out = rt_info(&li, PC_PSD_V1);
        a = out.len;
        pc_psd_bytes_fini(&out);
        out = rt_info(&li, PC_PSD_V2);
        CHECK_EQ_UINT(out.len, a + 4);
        pc_psd_bytes_fini(&out);
        pc_psd_layer_info_fini(&li);
    }
    /* non-default padding kept */
    memset(&li, 0, sizeof li);
    li.padding.has = true;
    pc_psd_bytes_set(&li.padding.v, zero2, 2, NULL);
    out = rt_info(&li, PC_PSD_V1);
    CHECK_EQ_UINT(out.len, 4);
    pc_psd_bytes_fini(&out);
    pc_psd_layer_info_fini(&li);
}

static void test_record_accessors_and_channels(void)
{
    pc_psd_layer rec = sample_record(PC_PSD_V1);
    pc_psd_layer plain;
    pc_psd_bytes d = {NULL, 0};
    pc_psd_block b;
    char *name = NULL;
    uint32_t id;
    static const int16_t ids[4] = {-1, 0, 1, 2};
    int i;
    for (i = 0; i < 4; i++) {
        CHECK_EQ_INT(pc_psd_layer_decode_channel(&rec, ids[i], 8, PC_PSD_V1, &d, NULL), PC_OK);
        CHECK(d.len == 20 && d.data[19] == 19);
    }
    CHECK_EQ_INT(pc_psd_layer_decode_channel(&rec, -2, 8, PC_PSD_V1, &d, NULL), PC_OK);
    CHECK(d.len == 4 && d.data[3] == 4);
    CHECK(pc_psd_layer_decode_channel(&rec, 7, 8, PC_PSD_V1, &d, NULL) != PC_OK);
    pc_psd_bytes_fini(&d);
    CHECK_EQ_INT(pc_psd_layer_name(&rec, &name, NULL), PC_OK);
    CHECK_EQ_STR(name, "\xC3\xA4" "bc");
    free(name);
    CHECK(!pc_psd_layer_is_visible(&rec));
    CHECK(pc_psd_layer_id(&rec, &id) && id == 5);
    CHECK_EQ_UINT(pc_psd_layer_fill_opacity(&rec), 255);
    CHECK_EQ_UINT(pc_psd_layer_section_type(&rec), PC_PSD_SECTION_OTHER);
    CHECK(pc_psd_layer_get_mask(&rec) != NULL);
    CHECK(pc_psd_rect_eq(pc_psd_layer_channel_rect(&rec, -2), pc_psd_rect_from_xywh(0, 0, 2, 2)));
    CHECK(pc_psd_rect_eq(pc_psd_layer_channel_rect(&rec, 0), rec.rect));
    pc_psd_layer_init(&plain);
    pc_psd_bytes_set(&plain.name, "legacy", 6, NULL);
    CHECK_EQ_INT(pc_psd_layer_name(&plain, &name, NULL), PC_OK);
    CHECK_EQ_STR(name, "legacy");
    free(name);
    pc_psd_block_make_flag("iOpa", 10, &b, NULL);
    pc_psd_layer_push_block(&plain, &b, NULL);
    CHECK_EQ_UINT(pc_psd_layer_fill_opacity(&plain), 10);
    pc_psd_layer_block_mut(&plain, "iOpa")->data.data[0] = 11;
    CHECK_EQ_UINT(pc_psd_layer_fill_opacity(&plain), 11);
    pc_psd_layer_fini(&plain);
    pc_psd_layer_fini(&rec);
}

static pc_psd_layer_mask mask_base(void)
{
    pc_psd_layer_mask m;
    pc_psd_layer_mask_make(pc_psd_rect_from_xywh(1, 2, 3, 4), 0, 0, &m, NULL);
    return m;
}

static void check_mask_rt(const pc_psd_layer_mask *m)
{
    pc_psd_bytes b = {NULL, 0};
    pc_psd_layer_mask back;
    CHECK_EQ_INT(pc_psd_layer_mask_to_bytes(m, &b, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_layer_mask_parse(b.data, b.len, &back, NULL), PC_OK);
    CHECK(pc_psd_layer_mask_eq(&back, m));
    pc_psd_layer_mask_fini(&back);
    pc_psd_bytes_fini(&b);
}

static void test_mask_variants_roundtrip(void)
{
    pc_psd_layer_mask m = mask_base();
    check_mask_rt(&m);
    pc_psd_bytes_fini(&m.trailing);
    m.has_real = true;
    m.real.flags = 1;
    m.real.background = 255;
    m.real.rect = pc_psd_rect_from_xywh(0, 0, 9, 9);
    check_mask_rt(&m);
    m.flags = PC_PSD_MF_PARAMETERS;
    m.has_params = true;
    m.params.flags = 0xF;
    m.params.has_user_density = true;
    m.params.user_density = 128;
    m.params.has_user_feather = true;
    m.params.user_feather = 2.5;
    m.params.has_vector_density = true;
    m.params.vector_density = 64;
    m.params.has_vector_feather = true;
    m.params.vector_feather = 0.5;
    memset(&m.real, 0, sizeof m.real);
    pc_psd_bytes_set(&m.trailing, "\0\0", 2, NULL);
    check_mask_rt(&m);
    pc_psd_layer_mask_fini(&m);
    m = mask_base();
    pc_psd_bytes_fini(&m.trailing);
    m.flags = PC_PSD_MF_PARAMETERS;
    m.has_params = true;
    m.params.flags = 2;
    m.params.has_user_feather = true;
    m.params.user_feather = 1.0;
    check_mask_rt(&m);
}

static void put_rect(pc_buf *b, pc_psd_rect r)
{
    uint8_t x[16];
    pc_store_be32(x, (uint32_t)r.top);
    pc_store_be32(x + 4, (uint32_t)r.left);
    pc_store_be32(x + 8, (uint32_t)r.bottom);
    pc_store_be32(x + 12, (uint32_t)r.right);
    pc_buf_append(b, x, 16, NULL);
}

static void test_mask_field_orders(void)
{
    pc_buf d = PC_BUF_INIT;
    pc_psd_layer_mask m;
    pc_psd_bytes out = {NULL, 0};
    static const uint8_t ps_hdr[2] = {0, PC_PSD_MF_PARAMETERS | 8};
    static const uint8_t ps_real[2] = {0, 255};
    static const uint8_t ps_par[4] = {5, 64, 64, 0};
    static const int64_t cases[4][2] = {{64, 1}, {200, 1}, {64, 300}, {1, 7}};
    int i;
    /* Photoshop: real-mask fields before the parameters (psd-tools mask-density-layervectormask). */
    put_rect(&d, pc_psd_rect_from_xywh(15, -1, 18, 10));
    pc_buf_append(&d, ps_hdr, 2, NULL);
    pc_buf_append(&d, ps_real, 2, NULL);
    put_rect(&d, pc_psd_rect_from_xywh(0, 0, 32, 8));
    pc_buf_append(&d, ps_par, 4, NULL);
    CHECK_EQ_INT(pc_psd_layer_mask_parse(d.data, d.len, &m, NULL), PC_OK);
    CHECK(m.has_real && pc_psd_rect_eq(m.real.rect, pc_psd_rect_from_xywh(0, 0, 32, 8)) && m.real.background == 255);
    CHECK(m.params.has_user_density && m.params.user_density == 64 && m.params.vector_density == 64);
    CHECK(m.trailing.len == 1 && m.real_first);
    CHECK_EQ_INT(pc_psd_layer_mask_to_bytes(&m, &out, NULL), PC_OK);
    CHECK(out.len == d.len && memcmp(out.data, d.data, d.len) == 0);
    pc_psd_bytes_fini(&out);
    pc_psd_layer_mask_fini(&m);
    /* density-only spec layout stays in the spec order (#949) */
    for (i = 0; i < 4; i++) {
        uint8_t hdr[2] = {255, PC_PSD_MF_PARAMETERS};
        uint8_t par[2] = {1, (uint8_t)cases[i][0]};
        static const uint8_t zz[2];
        pc_buf_truncate(&d, 0);
        put_rect(&d, pc_psd_rect_from_xywh(0, 0, 1, 1));
        pc_buf_append(&d, hdr, 2, NULL);
        pc_buf_append(&d, par, 2, NULL);
        pc_buf_append(&d, zz, 2, NULL);
        put_rect(&d, pc_psd_rect_from_xywh(0, 0, (uint32_t)cases[i][1], 1));
        CHECK_EQ_INT(pc_psd_layer_mask_parse(d.data, d.len, &m, NULL), PC_OK);
        CHECK(!m.real_first && m.params.user_density == (uint8_t)cases[i][0]);
        CHECK(m.has_real && m.real.background == 0 && m.trailing.len == 0);
        CHECK_EQ_INT(pc_psd_layer_mask_to_bytes(&m, &out, NULL), PC_OK);
        CHECK(out.len == d.len && memcmp(out.data, d.data, d.len) == 0);
        pc_psd_bytes_fini(&out);
        pc_psd_layer_mask_fini(&m);
    }
    /* sizes */
    m = mask_base();
    CHECK_EQ_INT(pc_psd_layer_mask_to_bytes(&m, &out, NULL), PC_OK);
    CHECK_EQ_UINT(out.len, 20);
    pc_psd_bytes_fini(&out);
    pc_psd_bytes_fini(&m.trailing);
    m.has_real = true;
    CHECK_EQ_INT(pc_psd_layer_mask_to_bytes(&m, &out, NULL), PC_OK);
    CHECK_EQ_UINT(out.len, 36);
    pc_psd_bytes_fini(&out);
    pc_buf_fini(&d);
}

static void test_layer_misc(void)
{
    pc_psd_layer_info li;
    pc_psd_layer l;
    pc_psd_bytes out = {NULL, 0};
    pc_psd_layer_info back;
    pc_psd_block b;
    uint8_t flags = 0;
    pc_psd_rect bad = {5, 0, 0, 1};
    pc_psd_rect huge = {0, INT32_MIN, 1, INT32_MAX};
    size_t w;
    size_t h;
    pc_error e;
    size_t cut;
    int fails = 0;
    static const uint8_t thousand[2] = {0x03, 0xe8};
    /* short mask kept raw */
    memset(&li, 0, sizeof li);
    pc_psd_layer_init(&l);
    l.mask.kind = PC_PSD_MASK_RAW;
    pc_psd_bytes_set(&l.mask.raw, "\1\2\3", 3, NULL);
    pc_psd_layer_info_push(&li, &l, NULL);
    out = rt_info(&li, PC_PSD_V1);
    pc_psd_bytes_fini(&out);
    pc_psd_layer_info_fini(&li);
    /* flags and ranges */
    flags = pc_psd_flags_set_pixel_data_irrelevant(flags, true);
    CHECK(flags == (PC_PSD_LF_BIT4_USEFUL | PC_PSD_LF_PIXEL_DATA_IRRELEVANT));
    pc_psd_blending_ranges_full(3, &out, NULL);
    CHECK(out.len == 32 && out.data[2] == 255);
    pc_psd_bytes_fini(&out);
    /* rect helpers */
    CHECK(pc_psd_rect_size(pc_psd_rect_from_xywh(-5, -6, 10, 3), &w, &h, NULL) == PC_OK && w == 10 && h == 3);
    CHECK(pc_psd_rect_size(bad, &w, &h, NULL) != PC_OK && pc_psd_rect_is_empty(bad));
    CHECK_EQ_INT(pc_psd_rect_size(huge, &w, &h, &e), PC_ERR_LIMIT);
    /* truncations */
    memset(&li, 0, sizeof li);
    l = sample_record(PC_PSD_V1);
    pc_psd_layer_info_push(&li, &l, NULL);
    pc_psd_layer_info_to_bytes(&li, PC_PSD_V1, &out, NULL);
    for (cut = 0; cut + 1 < out.len; cut++) {
        if (pc_psd_layer_info_parse(out.data, cut, PC_PSD_V1, &back, NULL) == PC_OK) {
            fails++;
            pc_psd_layer_info_fini(&back);
        }
    }
    CHECK_EQ_INT(fails, 0);
    pc_psd_bytes_fini(&out);
    pc_psd_layer_info_fini(&li);
    /* bad blend signature */
    memset(&li, 0, sizeof li);
    pc_psd_layer_init(&l);
    pc_psd_layer_info_push(&li, &l, NULL);
    pc_psd_layer_info_to_bytes(&li, PC_PSD_V1, &out, NULL);
    out.data[20] = 'X';
    CHECK_EQ_INT(pc_psd_layer_info_parse(out.data, out.len, PC_PSD_V1, &back, &e), PC_ERR_FORMAT);
    CHECK_CONTAINS(e.msg, "expected 8BIM (layer blend mode)");
    pc_psd_bytes_fini(&out);
    pc_psd_layer_info_fini(&li);
    /* too many layers for the data */
    CHECK(pc_psd_layer_info_parse(thousand, 2, PC_PSD_V1, &back, NULL) != PC_OK);
    /* zero-length channel and unknown blend/blocks pass through */
    memset(&li, 0, sizeof li);
    pc_psd_layer_init(&l);
    {
        pc_psd_channel c;
        memset(&c, 0, sizeof c);
        pc_psd_layer_push_channel(&l, &c, NULL);
    }
    memcpy(l.blend, "wxyz", 4);
    pc_psd_block_init(&b, "Zzzz", "\1\2\3\4\5", 5, NULL);
    pc_psd_layer_push_block(&l, &b, NULL);
    pc_psd_layer_info_push(&li, &l, NULL);
    out = rt_info(&li, PC_PSD_V1);
    pc_psd_bytes_fini(&out);
    CHECK_EQ_INT(pc_psd_layer_decode_channel(&li.layers[0], 0, 8, PC_PSD_V1, &out, NULL), PC_OK);
    CHECK_EQ_UINT(out.len, 0);
    pc_psd_layer_info_fini(&li);
}

/* ---- pixels.rs ---- */

static void test_pixel_helpers(void)
{
    static const uint8_t bits[2] = {0xA0, 0x80};
    static const uint8_t want_bits[6] = {1, 0, 1, 1, 0, 0};
    static const uint8_t s16[6] = {0xff, 0xff, 0, 0, 0x80, 0x00};
    static const uint8_t w16[3] = {255, 0, 128};
    static const uint8_t b1[1] = {0x80};
    static const uint8_t w1[2] = {0, 255};
    static const uint8_t w32[5] = {0, 255, 255, 0, 0};
    uint8_t f32[20];
    static const float fv[5] = {0.0f, 1.0f, 2.0f, -1.0f, NAN};
    pc_psd_bytes o = {NULL, 0};
    int i;
    CHECK_EQ_INT(pc_psd_unpack_bits(bits, 2, 3, 2, &o, NULL), PC_OK);
    CHECK(o.len == 6 && memcmp(o.data, want_bits, 6) == 0);
    CHECK_EQ_INT(pc_psd_plane_to_u8(s16, 6, 16, 3, 1, &o, NULL), PC_OK);
    CHECK(o.len == 3 && memcmp(o.data, w16, 3) == 0);
    for (i = 0; i < 5; i++) {
        pc_store_be32(f32 + 4 * i, pc_f32_to_bits(fv[i]));
    }
    CHECK_EQ_INT(pc_psd_plane_to_u8(f32, 20, 32, 5, 1, &o, NULL), PC_OK);
    CHECK(o.len == 5 && memcmp(o.data, w32, 5) == 0);
    CHECK_EQ_INT(pc_psd_plane_to_u8(b1, 1, 1, 2, 1, &o, NULL), PC_OK);
    CHECK(o.len == 2 && memcmp(o.data, w1, 2) == 0);
    CHECK(pc_psd_plane_to_u8(NULL, 0, 12, 0, 0, &o, NULL) != PC_OK);
    pc_psd_bytes_fini(&o);
}

/* ---- file.rs write tests ---- */

static void test_streamed_layer_info_equals_materialized_block(void)
{
    static const char *const keys[3] = {"Lr16", "Lr32", "Layr"};
    static const size_t idx[4] = {0, 1, 2, SIZE_MAX};
    static const uint8_t pads[4][4] = {{0}, {0}, {42}, {1, 2, 3}};
    static const int padn[4] = {-1, 0, 1, 3};
    static const uint8_t eleven[1] = {11};
    static const uint8_t twelve[2] = {12, 13};
    int v;
    int k;
    int i;
    int p;
    int bad = 0;
    for (v = 1; v <= 2; v++) {
        for (k = 0; k < 3; k++) {
            for (i = 0; i < 4; i++) {
                for (p = 0; p < 4; p++) {
                    pc_psd_file f = tg_small((pc_psd_version)v, PC_PSD_RAW);
                    pc_psd_file m;
                    pc_psd_bytes actual = {NULL, 0};
                    pc_psd_bytes want = {NULL, 0};
                    pc_psd_block blk;
                    size_t at;
                    pc_psd_blocks_free(f.global_blocks, f.nglobal);
                    f.global_blocks = calloc(2, sizeof *f.global_blocks);
                    f.nglobal = 2;
                    pc_psd_block_init(&f.global_blocks[0], "zz01", eleven, 1, NULL);
                    pc_psd_block_init(&f.global_blocks[1], "zz02", twelve, 2, NULL);
                    f.placement.where = PC_PSD_LI_GLOBAL_BLOCK;
                    f.placement.index = idx[i];
                    memcpy(f.placement.signature, "8B64", 4);
                    memcpy(f.placement.key, keys[k], 4);
                    if (padn[p] >= 0) {
                        f.placement.padding.has = true;
                        pc_psd_bytes_set(&f.placement.padding.v, pads[p], (size_t)padn[p], NULL);
                    }
                    /* An odd body catches padding computed against the final file offset. */
                    f.layer_info.padding.has = true;
                    pc_psd_bytes_set(&f.layer_info.padding.v, "M", 1, NULL);
                    bad += pc_psd_file_to_bytes(&f, &actual, NULL) != PC_OK;
                    pc_psd_file_clone(&f, &m, NULL);
                    memset(&blk, 0, sizeof blk);
                    memcpy(blk.signature, "8B64", 4);
                    memcpy(blk.key, keys[k], 4);
                    pc_psd_layer_info_to_bytes(&m.layer_info, (pc_psd_version)v, &blk.data, NULL);
                    blk.padding = m.placement.padding;
                    memset(&m.placement.padding, 0, sizeof m.placement.padding);
                    pc_psd_layer_info_fini(&m.layer_info);
                    m.has_layer_info = false;
                    m.placement.where = PC_PSD_LI_SECTION;
                    at = idx[i] < m.nglobal ? idx[i] : m.nglobal;
                    m.global_blocks = realloc(m.global_blocks, (m.nglobal + 1) * sizeof *m.global_blocks);
                    memmove(&m.global_blocks[at + 1], &m.global_blocks[at], (m.nglobal - at) * sizeof *m.global_blocks);
                    m.global_blocks[at] = blk;
                    m.nglobal++;
                    bad += pc_psd_file_to_bytes(&m, &want, NULL) != PC_OK;
                    bad += !pc_psd_bytes_eq(&actual, &want);
                    pc_psd_bytes_fini(&actual);
                    pc_psd_bytes_fini(&want);
                    pc_psd_file_fini(&f);
                    pc_psd_file_fini(&m);
                }
            }
        }
    }
    CHECK_EQ_INT(bad, 0);
}

static void test_global_layer_info_propagates_limit_errors(void)
{
    pc_psd_file f = tg_small(PC_PSD_V1, PC_PSD_RAW);
    pc_psd_bytes out = {NULL, 0};
    pc_error e;
    size_t i;
    pc_psd_layer_info_fini(&f.layer_info);
    f.layer_info.layers = calloc(32768, sizeof *f.layer_info.layers);
    f.layer_info.n = 32768;
    for (i = 0; i < 32768; i++) {
        pc_psd_layer_init(&f.layer_info.layers[i]);
    }
    f.placement.where = PC_PSD_LI_GLOBAL_BLOCK;
    f.placement.index = SIZE_MAX;
    memcpy(f.placement.signature, "8BIM", 4);
    memcpy(f.placement.key, "Lr16", 4);
    CHECK_EQ_INT(pc_psd_file_to_bytes(&f, &out, &e), PC_ERR_LIMIT);
    CHECK_EQ_STR(e.msg, "limit exceeded: more than 32767 layers");
    pc_psd_file_fini(&f);
}

int main(int argc, char **argv)
{
    RUN(test_unicode_name_typed);
    RUN(test_section_divider_variants);
    RUN(test_simple_typed_blocks);
    RUN(test_padding_rules);
    RUN(test_psb_long_keys_and_overlong);
    RUN(test_check_structure);
    RUN(test_layer_info_roundtrips);
    RUN(test_record_accessors_and_channels);
    RUN(test_mask_variants_roundtrip);
    RUN(test_mask_field_orders);
    RUN(test_layer_misc);
    RUN(test_pixel_helpers);
    RUN(test_streamed_layer_info_equals_materialized_block);
    RUN(test_global_layer_info_propagates_limit_errors);
    return pc_t_finish();
}
