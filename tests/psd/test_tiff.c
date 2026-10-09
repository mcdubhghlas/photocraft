/* Ports the tests of crates/psd/src/tiff.rs and replays `rustref tiff-vectors`
 * (tests/golden/tiff-vectors.txt) over the testgen cases that involve no zlib stream. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/file.h"
#include "harness.h"
#include "psd/filter_effects.h"
#include "psd/misc.h"
#include "psd/patterns.h"
#include "psd/tiff.h"
#include "testgen.h"

#define FNV0 0xcbf29ce484222325u

static void fnv(uint64_t *h, const void *data, size_t n)
{
    const uint8_t *b = data;
    size_t i;
    for (i = 0; i < n; i++) {
        *h ^= b[i];
        *h *= 0x100000001b3u;
    }
}

static void fnv_warn(uint64_t *h, const pc_psd_warnings *w)
{
    size_t i;
    for (i = 0; i < w->n; i++) {
        fnv(h, w->v[i], strlen(w->v[i]));
    }
}

static bool uses_zlib(const pc_psd_file *f)
{
    size_t nl;
    size_t i;
    size_t k;
    const pc_psd_layer *layers = pc_psd_file_layers(f, &nl);
    for (i = 0; i < nl; i++) {
        for (k = 0; k < layers[i].nchannels; k++) {
            uint16_t c = layers[i].channels[k].compression;
            if (layers[i].channels[k].has_compression && (c == PC_PSD_ZIP || c == PC_PSD_ZIP_PREDICT)) {
                return true;
            }
        }
    }
    return false;
}

static pc_buf golden;

static const char *expect(const char *name)
{
    char key[160];
    const char *p = (const char *)golden.data;
    const char *end = p + golden.len;
    size_t kl = (size_t)snprintf(key, sizeof key, "tiff\t%s\t", name);
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl != NULL ? (size_t)(nl - p) : (size_t)(end - p);
        if (len > kl && memcmp(p, key, kl) == 0) {
            return p + kl;
        }
        p += len + 1;
    }
    return NULL;
}

static void test_parity(void)
{
    size_t n;
    size_t i;
    int compared = 0;
    tg_case *cases = tg_all_cases(&n);
    for (i = 0; i < n; i++) {
        pc_psd_isd d;
        uint64_t h = FNV0;
        int o;
        char got[20];
        const char *want = expect(cases[i].name);
        pc_psd_bytes res = {NULL, 0};
        pc_psd_bytes lres = {NULL, 0};
        pc_error e;
        if (uses_zlib(&cases[i].file)) {
            continue;
        }
        pc_t_name = cases[i].name;
        CHECK_EQ_INT(pc_psd_isd_from_psd(&cases[i].file, &d, NULL), PC_OK);
        for (o = 0; o < 2; o++) {
            pc_psd_bytes b = {NULL, 0};
            pc_psd_warnings w;
            if (pc_psd_isd_to_bytes(&d, o == 0 ? PC_PSD_BIG : PC_PSD_LITTLE, &b, &w, &e) == PC_OK) {
                pc_psd_isd back;
                pc_psd_warnings w2;
                fnv(&h, b.data, b.len);
                fnv_warn(&h, &w);
                pc_psd_warnings_fini(&w);
                if (pc_psd_isd_parse(b.data, b.len, &back, &w2, &e) == PC_OK) {
                    pc_psd_bytes x = {NULL, 0};
                    fnv_warn(&h, &w2);
                    pc_psd_warnings_fini(&w2);
                    if (pc_psd_isd_to_bytes(&back, PC_PSD_BIG, &x, NULL, NULL) == PC_OK) {
                        fnv(&h, x.data, x.len);
                    }
                    pc_psd_bytes_fini(&x);
                    if (pc_psd_isd_to_bytes(&back, PC_PSD_LITTLE, &x, NULL, NULL) == PC_OK) {
                        fnv(&h, x.data, x.len);
                    }
                    pc_psd_bytes_fini(&x);
                    pc_psd_isd_fini(&back);
                } else {
                    fnv(&h, e.msg, strlen(e.msg));
                }
            } else {
                fnv(&h, e.msg, strlen(e.msg));
            }
            pc_psd_bytes_fini(&b);
        }
        CHECK_EQ_INT(pc_psd_tiff_resources_to_bytes(cases[i].file.resources, cases[i].file.nresources, &res, NULL),
                     PC_OK);
        fnv(&h, res.data, res.len);
        if (pc_psd_transcode_resources(res.data, res.len, PC_PSD_BIG, PC_PSD_LITTLE, &lres, NULL, &e) == PC_OK) {
            fnv(&h, lres.data, lres.len);
        } else {
            fnv(&h, e.msg, strlen(e.msg));
        }
        (void)snprintf(got, sizeof got, "%016llx", (unsigned long long)h);
        CHECK(want != NULL && strncmp(want, got, 16) == 0);
        compared++;
        pc_psd_bytes_fini(&res);
        pc_psd_bytes_fini(&lres);
        pc_psd_isd_fini(&d);
    }
    (void)printf("(%d cases compared) ", compared);
    CHECK(compared >= 80);
    tg_free_cases(cases, n);
}

/* ---- tiff.rs tests ---- */

static bool unknown_key(const uint8_t k[4])
{
    return memcmp(k, "zOdd", 4) == 0 || memcmp(k, "zNop", 4) == 0 || memcmp(k, "Txt2", 4) == 0;
}

static void drop_unknown(pc_psd_block **b, size_t *n)
{
    size_t i = 0;
    while (i < *n) {
        if (unknown_key((*b)[i].key)) {
            pc_psd_block_fini(&(*b)[i]);
            memmove(&(*b)[i], &(*b)[i + 1], (*n - i - 1) * sizeof **b);
            (*n)--;
        } else {
            i++;
        }
    }
}

/* d without what a little-endian round trip drops, and without the layer-info padding. */
static pc_psd_isd known(const pc_psd_isd *src)
{
    pc_psd_isd d;
    size_t i;
    CHECK_EQ_INT(pc_psd_isd_clone(src, &d, NULL), PC_OK);
    d.byte_order = PC_PSD_BIG;
    drop_unknown(&d.global_blocks, &d.nglobal);
    if (!d.has_layer_info) {
        memcpy(d.layer_info_key, "Layr", 4);
    } else {
        pc_psd_opt_bytes_fini(&d.layer_info.padding);
        for (i = 0; i < d.layer_info.n; i++) {
            drop_unknown(&d.layer_info.layers[i].blocks, &d.layer_info.layers[i].nblocks);
        }
    }
    return d;
}

static bool known_eq(const pc_psd_isd *a, const pc_psd_isd *b)
{
    pc_psd_isd x = known(a);
    pc_psd_isd y = known(b);
    bool eq = pc_psd_isd_eq(&x, &y);
    pc_psd_isd_fini(&x);
    pc_psd_isd_fini(&y);
    return eq;
}

static bool known_layers_eq(const pc_psd_isd *a, const pc_psd_isd *b)
{
    pc_psd_isd x = known(a);
    pc_psd_isd y = known(b);
    bool eq = x.has_layer_info == y.has_layer_info &&
              (!x.has_layer_info || pc_psd_layer_info_eq(&x.layer_info, &y.layer_info));
    pc_psd_isd_fini(&x);
    pc_psd_isd_fini(&y);
    return eq;
}

static bool warns_only_unknown(const pc_psd_warnings *w)
{
    size_t i;
    for (i = 0; i < w->n; i++) {
        if (strncmp(w->v[i], "zOdd", 4) != 0 && strncmp(w->v[i], "zNop", 4) != 0 && strncmp(w->v[i], "Txt2", 4) != 0) {
            return false;
        }
    }
    return true;
}

static pc_psd_bytes to_bytes(const pc_psd_isd *d, pc_psd_byte_order o, pc_psd_warnings *w)
{
    pc_psd_bytes b = {NULL, 0};
    pc_error e;
    if (pc_psd_isd_to_bytes(d, o, &b, w, &e) != PC_OK) {
        PC_T_FAIL("to_bytes: %s", e.msg);
    }
    return b;
}

static pc_psd_isd parse(const pc_psd_bytes *b, pc_psd_warnings *w)
{
    pc_psd_isd d;
    pc_error e;
    pc_psd_isd_init(&d);
    if (pc_psd_isd_parse(b->data, b->len, &d, w, &e) != PC_OK) {
        PC_T_FAIL("parse: %s", e.msg);
        pc_psd_isd_init(&d);
    }
    return d;
}

/* tiff.rs roundtrip(): returns the parsed big-endian copy. */
static pc_psd_isd roundtrip(const pc_psd_isd *d)
{
    pc_psd_warnings w;
    pc_psd_bytes big = to_bytes(d, PC_PSD_BIG, &w);
    pc_psd_bytes little;
    pc_psd_bytes l2;
    pc_psd_bytes l3;
    pc_psd_isd bb;
    pc_psd_isd bl;
    pc_psd_isd t;
    CHECK_EQ_UINT(w.n, 0);
    pc_psd_warnings_fini(&w);
    little = to_bytes(d, PC_PSD_LITTLE, &w);
    CHECK(warns_only_unknown(&w));
    if (w.n == 0) {
        CHECK_EQ_UINT(big.len, little.len);
    }
    pc_psd_warnings_fini(&w);
    if (big.len > 36) {
        CHECK(!pc_psd_bytes_eq(&big, &little));
        CHECK(memcmp(little.data + 36, "MIB8", 4) == 0);
    }
    bb = parse(&big, &w);
    CHECK_EQ_UINT(w.n, 0);
    pc_psd_warnings_fini(&w);
    bl = parse(&little, &w);
    CHECK_EQ_UINT(w.n, 0);
    pc_psd_warnings_fini(&w);
    if (little.len > 36) {
        CHECK(bl.byte_order == PC_PSD_LITTLE);
    }
    CHECK(known_eq(&bl, &bb));
    l2 = to_bytes(&bl, PC_PSD_LITTLE, NULL);
    t = parse(&l2, NULL);
    l3 = to_bytes(&t, PC_PSD_LITTLE, NULL);
    CHECK(pc_psd_bytes_eq(&l3, &l2));
    pc_psd_isd_fini(&t);
    pc_psd_isd_fini(&bl);
    pc_psd_bytes_fini(&big);
    pc_psd_bytes_fini(&little);
    pc_psd_bytes_fini(&l2);
    pc_psd_bytes_fini(&l3);
    return bb;
}

static void test_generated_files_survive_both_orders(void)
{
    size_t n;
    size_t i;
    tg_case *cases = tg_all_cases(&n);
    for (i = 0; i < n; i++) {
        pc_psd_isd d;
        pc_psd_isd back;
        pc_psd_bytes little;
        pc_psd_isd b2;
        pc_t_name = cases[i].name;
        pc_psd_isd_from_psd(&cases[i].file, &d, NULL);
        back = roundtrip(&d);
        CHECK(back.version == d.version);
        CHECK(known_eq(&back, &d));
        little = to_bytes(&d, PC_PSD_LITTLE, NULL);
        b2 = parse(&little, NULL);
        CHECK(known_eq(&b2, &d));
        pc_psd_isd_fini(&b2);
        pc_psd_bytes_fini(&little);
        pc_psd_isd_fini(&back);
        pc_psd_isd_fini(&d);
    }
    tg_free_cases(cases, n);
}

static void test_same_order_is_identity(void)
{
    size_t n;
    size_t i;
    int bad = 0;
    tg_case *cases = tg_all_cases(&n);
    for (i = 0; i < n; i++) {
        pc_psd_isd d;
        pc_psd_bytes big;
        pc_psd_bytes same = {NULL, 0};
        pc_psd_warnings w;
        pc_psd_isd_from_psd(&cases[i].file, &d, NULL);
        big = to_bytes(&d, PC_PSD_BIG, NULL);
        bad += pc_psd_transcode_blocks(big.data + 36, big.len - 36, d.version, PC_PSD_BIG, PC_PSD_BIG, &same, &w,
                                       NULL) != PC_OK;
        bad += w.n != 0 || same.len != big.len - 36 || (same.len > 0 && memcmp(same.data, big.data + 36, same.len) != 0);
        pc_psd_warnings_fini(&w);
        pc_psd_bytes_fini(&same);
        pc_psd_bytes_fini(&big);
        pc_psd_isd_fini(&d);
    }
    CHECK_EQ_INT(bad, 0);
    tg_free_cases(cases, n);
}

static pc_psd_isd sample_layers(void)
{
    pc_psd_file f = tg_layered(PC_PSD_V1, PC_PSD_MODE_RGB, 8, PC_PSD_RLE);
    pc_psd_isd d;
    pc_psd_isd_from_psd(&f, &d, NULL);
    pc_psd_file_fini(&f);
    return d;
}

static void test_signature_and_keys(void)
{
    pc_psd_isd d = sample_layers();
    pc_psd_bytes b = to_bytes(&d, PC_PSD_BIG, NULL);
    pc_psd_isd back;
    uint8_t k[4];
    static const struct {
        uint16_t depth;
        uint16_t comp;
    } cases[3] = {{16, PC_PSD_ZIP_PREDICT}, {32, PC_PSD_ZIP_PREDICT}, {16, PC_PSD_RLE}};
    int i;
    CHECK(memcmp(b.data, PC_PSD_ISD_SIGNATURE, 36) == 0);
    pc_psd_bytes_fini(&b);
    d.version = PC_PSD_V2;
    b = to_bytes(&d, PC_PSD_BIG, NULL);
    CHECK(memcmp(b.data, PC_PSD_ISD_SIGNATURE_PSB, 36) == 0);
    back = parse(&b, NULL);
    CHECK(back.version == PC_PSD_V2);
    pc_psd_isd_fini(&back);
    pc_psd_bytes_fini(&b);
    back = roundtrip(&d);
    pc_psd_isd_fini(&back);
    pc_psd_isd_fini(&d);
    pc_psd_layer_info_key(8, k);
    CHECK(memcmp(k, "Layr", 4) == 0);
    pc_psd_layer_info_key(16, k);
    CHECK(memcmp(k, "Lr16", 4) == 0);
    pc_psd_layer_info_key(32, k);
    CHECK(memcmp(k, "Lr32", 4) == 0);
    for (i = 0; i < 3; i++) {
        pc_psd_file f = tg_layered(PC_PSD_V1, PC_PSD_MODE_RGB, cases[i].depth, cases[i].comp);
        pc_psd_file psd;
        pc_psd_file again;
        pc_psd_isd from_again;
        pc_psd_resource *res = NULL;
        pc_psd_bytes img = {NULL, 0};
        pc_psd_bytes pb = {NULL, 0};
        pc_psd_bytes little;
        pc_psd_isd bl;
        size_t r;
        pc_psd_isd_from_psd(&f, &d, NULL);
        pc_psd_layer_info_key(cases[i].depth, k);
        CHECK(memcmp(d.layer_info_key, k, 4) == 0);
        back = roundtrip(&d);
        CHECK(memcmp(back.layer_info_key, d.layer_info_key, 4) == 0);
        res = calloc(f.nresources, sizeof *res);
        for (r = 0; r < f.nresources; r++) {
            pc_psd_resource_clone(&f.resources[r], &res[r], NULL);
        }
        pc_psd_bytes_clone(&f.image_data, &img, NULL);
        CHECK_EQ_INT(pc_psd_isd_into_psd(&back, f.header, &res, f.nresources, f.image_compression, &img, &psd, NULL),
                     PC_OK);
        CHECK(psd.placement.where == PC_PSD_LI_GLOBAL_BLOCK);
        CHECK_EQ_INT(pc_psd_file_to_bytes(&psd, &pb, NULL), PC_OK);
        CHECK_EQ_INT(pc_psd_file_parse(pb.data, pb.len, &again, NULL), PC_OK);
        pc_psd_isd_from_psd(&again, &from_again, NULL);
        CHECK(known_layers_eq(&from_again, &d));
        little = to_bytes(&d, PC_PSD_LITTLE, NULL);
        bl = parse(&little, NULL);
        CHECK(known_layers_eq(&bl, &d));
        pc_psd_isd_fini(&bl);
        pc_psd_bytes_fini(&little);
        pc_psd_isd_fini(&from_again);
        pc_psd_file_fini(&again);
        pc_psd_bytes_fini(&pb);
        pc_psd_file_fini(&psd);
        pc_psd_isd_fini(&d);
        pc_psd_file_fini(&f);
    }
}

static void test_blocks_are_padded_to_four(void)
{
    pc_psd_isd d = sample_layers();
    pc_psd_bytes b = to_bytes(&d, PC_PSD_BIG, NULL);
    size_t at = 36;
    while (at + 12 <= b.len) {
        CHECK(memcmp(b.data + at, "8BIM", 4) == 0);
        at += 12 + pc_load_be32(b.data + at + 8);
        at += (4 - at % 4) % 4;
    }
    CHECK_EQ_UINT(at, b.len);
    pc_psd_bytes_fini(&b);
    pc_psd_isd_fini(&d);
}

/* block_rt: a lone global block both ways; returns big and little bytes. */
static void block_rt(const char key4[4], const uint8_t *data, size_t n, pc_psd_bytes *big, pc_psd_bytes *little)
{
    pc_psd_isd d;
    pc_psd_isd back;
    pc_psd_warnings w;
    pc_psd_bytes again;
    pc_psd_isd_init(&d);
    d.global_blocks = calloc(1, sizeof *d.global_blocks);
    d.nglobal = 1;
    pc_psd_block_init(&d.global_blocks[0], key4, data, n, NULL);
    *big = to_bytes(&d, PC_PSD_BIG, NULL);
    *little = to_bytes(&d, PC_PSD_LITTLE, &w);
    CHECK_EQ_UINT(w.n, 0);
    if (w.n > 0) {
        PC_T_FAIL("%.4s: %s", key4, w.v[0]);
    }
    pc_psd_warnings_fini(&w);
    back = parse(little, &w);
    CHECK_EQ_UINT(w.n, 0);
    pc_psd_warnings_fini(&w);
    CHECK(back.nglobal == 1 && back.global_blocks[0].data.len == n &&
          (n == 0 || memcmp(back.global_blocks[0].data.data, data, n) == 0));
    again = to_bytes(&back, PC_PSD_BIG, NULL);
    CHECK(pc_psd_bytes_eq(&again, big));
    pc_psd_bytes_fini(&again);
    pc_psd_isd_fini(&back);
    pc_psd_isd_fini(&d);
}

static bool contains(const pc_psd_bytes *b, const void *needle, size_t n)
{
    size_t i;
    for (i = 0; i + n <= b->len; i++) {
        if (memcmp(b->data + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

static void put16(pc_buf *b, uint16_t v)
{
    uint8_t x[2];
    pc_store_be16(x, v);
    pc_buf_append(b, x, 2, NULL);
}

static void put32(pc_buf *b, uint32_t v)
{
    uint8_t x[4];
    pc_store_be32(x, v);
    pc_buf_append(b, x, 4, NULL);
}

static void put64(pc_buf *b, uint64_t v)
{
    uint8_t x[8];
    pc_store_be64(x, v);
    pc_buf_append(b, x, 8, NULL);
}

static void putf64(pc_buf *b, double v)
{
    put64(b, pc_f64_to_bits(v));
}

static void puts_(pc_buf *b, const char *s)
{
    pc_buf_append(b, s, strlen(s), NULL);
}

static void put_vdesc(pc_buf *b, pc_psd_desc *d)
{
    pc_psd_bytes x = {NULL, 0};
    pc_psd_vdesc_to_bytes(d, &x, NULL);
    pc_buf_append(b, x.data, x.len, NULL);
    pc_psd_bytes_fini(&x);
    pc_psd_desc_fini(d);
}

static void test_descriptor_and_type_blocks(void)
{
    pc_psd_desc desc;
    pc_psd_desc inner;
    pc_psd_value v;
    pc_psd_value list;
    pc_buf data = PC_BUF_INIT;
    pc_psd_bytes big;
    pc_psd_bytes little;
    static const uint8_t zero4[4] = {0, 0, 0, 0};
    static const uint8_t v16le[4] = {16, 0, 0, 0};
    static const uint8_t v16be[4] = {0, 0, 0, 16};
    uint8_t one_le[8];
    pc_psd_desc_init(&desc, "null", NULL);
    memset(&v, 0, sizeof v);
    v.kind = PC_PSD_V_UNIT_FLOAT;
    memcpy(v.unit, "#Prc", 4);
    v.d = 75.5;
    pc_psd_desc_add(&desc, "Opct", &v, NULL);
    pc_psd_value_text("Drop Shadow", true, &v, NULL);
    pc_psd_desc_add(&desc, "Nm  ", &v, NULL);
    v = pc_psd_value_bool(true);
    pc_psd_desc_add(&desc, "enab", &v, NULL);
    pc_psd_value_enum("BlnM", "Mltp", &v, NULL);
    pc_psd_desc_add(&desc, "Md  ", &v, NULL);
    memset(&list, 0, sizeof list);
    list.kind = PC_PSD_V_LIST;
    v = pc_psd_value_int(-3);
    pc_psd_value_list_push(&list, &v, NULL);
    v = pc_psd_value_double(1.5);
    pc_psd_value_list_push(&list, &v, NULL);
    memset(&v, 0, sizeof v);
    v.kind = PC_PSD_V_LARGE_INTEGER;
    v.l = 7;
    pc_psd_value_list_push(&list, &v, NULL);
    pc_psd_desc_add(&desc, "list", &list, NULL);
    pc_psd_desc_init(&inner, "innr", NULL);
    pc_psd_value_object(&inner, &v, NULL);
    pc_psd_desc_add(&desc, "longKeyName", &v, NULL);
    put32(&data, 0);
    put_vdesc(&data, &desc);
    block_rt("lfx2", data.data, data.len, &big, &little);
    CHECK(memcmp(little.data + 36, "MIB82xfl", 8) == 0);
    CHECK(memcmp(little.data + 48, zero4, 4) == 0);
    CHECK(memcmp(little.data + 52, v16le, 4) == 0);
    CHECK(memcmp(big.data + 52, v16be, 4) == 0);
    CHECK(contains(&little, "crP#", 4));
    CHECK(contains(&little, "longKeyName", 11));
    pc_psd_bytes_fini(&big);
    pc_psd_bytes_fini(&little);
    /* TySh */
    pc_buf_truncate(&data, 0);
    put16(&data, 1);
    putf64(&data, 1.0);
    putf64(&data, 0.0);
    putf64(&data, 0.0);
    putf64(&data, 1.0);
    putf64(&data, 12.5);
    putf64(&data, -3.0);
    put16(&data, 50);
    pc_psd_desc_init(&desc, "TxLr", NULL);
    put_vdesc(&data, &desc);
    put16(&data, 1);
    pc_psd_desc_init(&desc, "warp", NULL);
    put_vdesc(&data, &desc);
    putf64(&data, 0.0);
    putf64(&data, 0.0);
    putf64(&data, 100.0);
    putf64(&data, 20.0);
    block_rt("TySh", data.data, data.len, &big, &little);
    CHECK(little.data[48] == 1 && little.data[49] == 0);
    pc_store_le64(one_le, pc_f64_to_bits(1.0));
    CHECK(memcmp(little.data + 50, one_le, 8) == 0);
    pc_psd_bytes_fini(&big);
    pc_psd_bytes_fini(&little);
    pc_buf_fini(&data);
}

static void test_legacy_effects_and_adjustments(void)
{
    pc_buf fx = PC_BUF_INIT;
    pc_buf sh = PC_BUF_INIT;
    pc_buf so = PC_BUF_INIT;
    pc_buf b = PC_BUF_INIT;
    pc_psd_bytes big;
    pc_psd_bytes little;
    static const uint8_t sw[2] = {0x34, 0x12};
    int i;
    put16(&fx, 0);
    put16(&fx, 3);
    puts_(&fx, "8BIMcmnS");
    put32(&fx, 7);
    put32(&fx, 0);
    pc_buf_append_byte(&fx, 1, NULL);
    put16(&fx, 0);
    puts_(&fx, "8BIMdsdw");
    put32(&sh, 2);
    put32(&sh, 5u << 16);
    put32(&sh, 75u << 16);
    put32(&sh, 120u << 16);
    put32(&sh, 7u << 16);
    for (i = 0; i < 5; i++) {
        put16(&sh, 0);
    }
    puts_(&sh, "8BIMmul ");
    pc_buf_append_byte(&sh, 1, NULL);
    pc_buf_append_byte(&sh, 1, NULL);
    pc_buf_append_byte(&sh, 191, NULL);
    put16(&sh, 0);
    put16(&sh, 0x1234);
    put16(&sh, 0);
    put16(&sh, 0);
    put16(&sh, 0);
    put32(&fx, (uint32_t)sh.len);
    pc_buf_append(&fx, sh.data, sh.len, NULL);
    puts_(&fx, "8BIMsofi");
    put32(&so, 2);
    puts_(&so, "8BIMnorm");
    put16(&so, 0);
    put16(&so, 0xffff);
    put16(&so, 0);
    put16(&so, 0);
    put16(&so, 0);
    pc_buf_append_byte(&so, 255, NULL);
    pc_buf_append_byte(&so, 1, NULL);
    put16(&so, 0);
    put16(&so, 0xffff);
    put16(&so, 0);
    put16(&so, 0);
    put16(&so, 0);
    put32(&fx, (uint32_t)so.len);
    pc_buf_append(&fx, so.data, so.len, NULL);
    block_rt("lrFX", fx.data, fx.len, &big, &little);
    CHECK(contains(&little, "MIB8wdsd", 8));
    CHECK(contains(&little, sw, 2));
    pc_psd_bytes_fini(&big);
    pc_psd_bytes_fini(&little);
    /* levl */
    put16(&b, 2);
    for (i = 0; i < 29 * 5; i++) {
        put16(&b, (uint16_t)i);
    }
    puts_(&b, "Lvls");
    put16(&b, 3);
    put16(&b, 33);
    for (i = 0; i < 20; i++) {
        put16(&b, (uint16_t)(1000 + i));
    }
    block_rt("levl", b.data, b.len, &big, &little);
    CHECK(contains(&little, "slvL", 4));
    pc_psd_bytes_fini(&big);
    pc_psd_bytes_fini(&little);
    /* curv */
    pc_buf_truncate(&b, 0);
    pc_buf_append_byte(&b, 0, NULL);
    put16(&b, 1);
    put32(&b, 5);
    put16(&b, 2);
    put16(&b, 0);
    put16(&b, 0);
    put16(&b, 255);
    put16(&b, 255);
    put16(&b, 1);
    put16(&b, 128);
    put16(&b, 100);
    puts_(&b, "Crv ");
    put16(&b, 4);
    put32(&b, 1);
    put16(&b, 2);
    put16(&b, 1);
    put16(&b, 7);
    put16(&b, 9);
    block_rt("curv", b.data, b.len, &big, &little);
    pc_psd_bytes_fini(&big);
    pc_psd_bytes_fini(&little);
    {
        static const struct {
            const char *key;
            uint8_t d[24];
            size_t n;
        } C[] = {{"hue2", {0, 2, 0, 0, 0, 10, 0, 20, 0, 30, 0, 40, 0, 50, 0, 60}, 16},
                 {"brit", {0, 10, 0, 20, 0, 30, 1, 0}, 8},
                 {"thrs", {0, 128, 0, 0}, 4},
                 {"post", {0, 4, 0, 0}, 4},
                 {"nvrt", {0}, 0},
                 {"expA", {0, 1, 0x3f, 0, 0, 0, 0, 0, 0, 0, 0x3f, 0x80, 0, 0}, 14},
                 {"blnc", {0}, 20},
                 {"mixr", {0, 1, 0, 0, 0, 100, 0, 0, 0, 0, 0, 0, 0, 0}, 14},
                 {"selc", {0, 1, 0, 0, 0, 5, 0, 6, 0, 7, 0, 8}, 12},
                 {"phfl", {0, 3, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 25, 1, 0}, 19},
                 {"phfl", {0, 2, 0, 0, 1, 0, 2, 0, 3, 0, 4, 0, 0, 0, 25, 1, 0}, 17},
                 {"lclr", {0, 1, 0, 0, 0, 0, 0, 0}, 8},
                 {"fxrp", {0x3f, 0xf8, 0, 0, 0, 0, 0, 0, 0x40, 0x04, 0, 0, 0, 0, 0, 0}, 16},
                 {"brst", {0, 0, 0, 1, 0, 0, 0, 2}, 8},
                 {"FMsk", {0, 0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 50}, 12},
                 {"lsct", {0, 0, 0, 3, '8', 'B', 'I', 'M', 'p', 'a', 's', 's'}, 12},
                 {"lspf", {0, 0, 0, 5}, 4},
                 {"lnsr", {'l', 'a', 'y', 'r'}, 4},
                 {"knko", {1, 0, 0, 0}, 4},
                 {"knko", {2, 0, 0, 0}, 4},
                 {"infx", {1, 0, 0, 0}, 4},
                 {"clbl", {0, 0, 0, 0}, 4},
                 {"tsly", {0, 0, 0, 0}, 4},
                 {"lmgm", {1, 0, 0, 0}, 4},
                 {"vmgm", {1, 0, 0, 0}, 4},
                 {"iOpa", {128, 0, 0, 0}, 4}};
        size_t k;
        for (k = 0; k < sizeof C / sizeof C[0]; k++) {
            pc_t_name = C[k].key;
            block_rt(C[k].key, C[k].d, C[k].n, &big, &little);
            if (C[k].n >= 2 && strcmp(C[k].key, "nvrt") != 0) {
                CHECK(!pc_psd_bytes_eq(&big, &little));
            }
            pc_psd_bytes_fini(&big);
            pc_psd_bytes_fini(&little);
        }
    }
    pc_buf_fini(&fx);
    pc_buf_fini(&sh);
    pc_buf_fini(&so);
    pc_buf_fini(&b);
}

static void test_global_mask_gradient_vector(void)
{
    static const uint8_t gm[14] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 50, 128, 0};
    pc_psd_isd d;
    pc_psd_isd back;
    pc_psd_bytes little;
    pc_psd_bytes big;
    pc_buf g = PC_BUF_INIT;
    uint8_t beef[4];
    static const uint8_t fixed[4] = {0, 0, 0x10, 0};
    int i;
    pc_psd_isd_init(&d);
    d.has_global_mask = true;
    pc_psd_bytes_set(&d.global_mask, gm, 14, NULL);
    little = to_bytes(&d, PC_PSD_LITTLE, NULL);
    CHECK(memcmp(little.data + 36, "MIB8ksML", 8) == 0);
    back = parse(&little, NULL);
    CHECK(back.has_global_mask && pc_psd_bytes_eq(&back.global_mask, &d.global_mask));
    pc_psd_isd_fini(&back);
    pc_psd_bytes_fini(&little);
    pc_psd_isd_fini(&d);
    /* grdm */
    put16(&g, 3);
    pc_buf_append_byte(&g, 0, NULL);
    pc_buf_append_byte(&g, 1, NULL);
    puts_(&g, "Lnr ");
    put32(&g, 2);
    put16(&g, 'a');
    put16(&g, 0);
    put16(&g, 2);
    for (i = 0; i < 2; i++) {
        put32(&g, i == 0 ? 0 : 4096);
        put32(&g, 50);
        put16(&g, 0);
        put16(&g, 0x1111);
        put16(&g, 0x2222);
        put16(&g, 0x3333);
        put16(&g, 0);
        put16(&g, 0);
    }
    put16(&g, 2);
    for (i = 0; i < 2; i++) {
        put32(&g, i == 0 ? 0 : 4096);
        put32(&g, 50);
        put16(&g, 100);
    }
    put16(&g, 2);
    put16(&g, 1);
    put16(&g, 32);
    put16(&g, 0);
    put32(&g, 0xdeadbeefu);
    for (i = 0; i < 12; i++) {
        put16(&g, 0);
    }
    block_rt("grdm", g.data, g.len, &big, &little);
    pc_store_le32(beef, 0xdeadbeefu);
    CHECK(contains(&little, beef, 4));
    CHECK(contains(&little, " rnL", 4));
    pc_psd_bytes_fini(&big);
    pc_psd_bytes_fini(&little);
    /* vmsk */
    pc_buf_truncate(&g, 0);
    put32(&g, 3);
    put32(&g, 0);
    put16(&g, 0);
    put16(&g, 2);
    put16(&g, 1);
    for (i = 0; i < 20; i++) {
        pc_buf_append_byte(&g, 0, NULL);
    }
    for (i = 0; i < 2; i++) {
        int j;
        put16(&g, 1);
        for (j = 0; j < 6; j++) {
            put32(&g, (uint32_t)(j + 1) * 0x00100000u);
        }
    }
    block_rt("vmsk", g.data, g.len, &big, &little);
    CHECK(contains(&little, fixed, 4));
    pc_psd_bytes_fini(&big);
    pc_psd_bytes_fini(&little);
    pc_buf_fini(&g);
}

static void test_pattern_block(void)
{
    uint8_t plane[16];
    pc_psd_pattern p;
    pc_psd_bytes data = {NULL, 0};
    pc_psd_bytes big;
    pc_psd_bytes little;
    pc_psd_isd back;
    pc_psd_pattern *got = NULL;
    size_t ngot = 0;
    int i;
    for (i = 0; i < 16; i++) {
        plane[i] = (uint8_t)i;
    }
    memset(&p, 0, sizeof p);
    p.mode = 3;
    p.width = 4;
    p.height = 4;
    pc_psd_str_set(&p.name, "dots", 4, NULL);
    pc_psd_str_set(&p.id, "abc", 3, NULL);
    p.depth = 8;
    p.channels = calloc(3, sizeof *p.channels);
    p.nchannels = 3;
    for (i = 0; i < 3; i++) {
        pc_psd_bytes_set(&p.channels[i], plane, 16, NULL);
    }
    p.alpha.has = true;
    pc_psd_bytes_set(&p.alpha.v, plane, 16, NULL);
    CHECK_EQ_INT(pc_psd_write_pattern_block(&p, 1, &data, NULL), PC_OK);
    block_rt("Patt", data.data, data.len, &big, &little);
    back = parse(&little, NULL);
    CHECK_EQ_INT(pc_psd_parse_pattern_block(back.global_blocks[0].data.data, back.global_blocks[0].data.len, &got,
                                            &ngot, NULL),
                 PC_OK);
    CHECK(ngot == 1 && pc_psd_pattern_eq(&got[0], &p));
    pc_psd_patterns_free(got, ngot);
    pc_psd_isd_fini(&back);
    pc_psd_bytes_fini(&big);
    pc_psd_bytes_fini(&little);
    pc_psd_bytes_fini(&data);
    pc_psd_pattern_fini(&p);
}

static void test_filter_effects_and_metadata(void)
{
    static const uint8_t px[6] = {1, 2, 3, 4, 5, 6};
    static const uint8_t nine[6] = {9, 9, 9, 9, 9, 9};
    pc_psd_fx_item it;
    pc_psd_filter_effects fx;
    pc_psd_filter_effects backfx;
    pc_psd_bytes data = {NULL, 0};
    pc_psd_bytes big;
    pc_psd_bytes little;
    pc_psd_isd d;
    pc_psd_isd back;
    pc_psd_warnings w;
    pc_psd_meta_item items[3];
    pc_psd_meta_item *got = NULL;
    size_t ngot = 0;
    pc_psd_desc desc;
    pc_psd_bytes vd = {NULL, 0};
    static const uint8_t eight[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    static const uint8_t one[4] = {0, 0, 0, 1};
    memset(&it, 0, sizeof it);
    it.id = strdup("x");
    it.version = 1;
    it.rect.bottom = 2;
    it.rect.right = 3;
    it.depth = 8;
    it.max_channels = 1;
    it.nslots = 3;
    it.slots = calloc(3, sizeof *it.slots);
    it.slot_written = calloc(3, sizeof *it.slot_written);
    pc_psd_fx_plane_encode(PC_PSD_RLE, px, 6, 3, 2, 8, &it.slots[0], NULL);
    pc_psd_fx_plane_encode(PC_PSD_RLE, px, 6, 3, 2, 8, &it.slots[2], NULL);
    it.slot_written[0] = it.slot_written[2] = true;
    it.has_mask = true;
    it.mask_rect = it.rect;
    pc_psd_fx_plane_encode(PC_PSD_RAW, nine, 6, 3, 2, 8, &it.mask, NULL);
    fx.version = 1;
    fx.items = &it;
    fx.n = 1;
    pc_psd_filter_effects_to_bytes(&fx, &data, NULL);
    block_rt("FEid", data.data, data.len, &big, &little);
    back = parse(&little, NULL);
    CHECK_EQ_INT(pc_psd_filter_effects_parse(back.global_blocks[0].data.data, back.global_blocks[0].data.len, &backfx,
                                             NULL),
                 PC_OK);
    CHECK(backfx.n == 1 && pc_psd_fx_item_eq(&backfx.items[0], &it));
    pc_psd_filter_effects_fini(&backfx);
    pc_psd_isd_fini(&back);
    pc_psd_bytes_fini(&big);
    pc_psd_bytes_fini(&little);
    pc_psd_bytes_fini(&data);
    pc_psd_fx_item_fini(&it);
    /* shmd */
    pc_psd_desc_init(&desc, "null", NULL);
    pc_psd_vdesc_to_bytes(&desc, &vd, NULL);
    pc_psd_desc_fini(&desc);
    pc_psd_meta_item_make("cmls", vd.data, vd.len, &items[0], NULL);
    pc_psd_write_shmd(items, 1, &data, NULL);
    block_rt("shmd", data.data, data.len, &big, &little);
    pc_psd_bytes_fini(&big);
    pc_psd_bytes_fini(&little);
    pc_psd_bytes_fini(&data);
    pc_psd_meta_items_free(NULL, 0);
    pc_psd_bytes_fini(&items[0].data);
    pc_psd_meta_item_make("zzzz", eight, 8, &items[0], NULL);
    pc_psd_meta_item_make("mdyn", one, 4, &items[1], NULL);
    pc_psd_meta_item_make("cmls", vd.data, vd.len, &items[2], NULL);
    pc_psd_write_shmd(items, 3, &data, NULL);
    pc_psd_isd_init(&d);
    d.global_blocks = calloc(1, sizeof *d.global_blocks);
    d.nglobal = 1;
    pc_psd_block_init(&d.global_blocks[0], "shmd", data.data, data.len, NULL);
    little = to_bytes(&d, PC_PSD_LITTLE, &w);
    CHECK(w.n == 1 && strncmp(w.v[0], "shmd item zzzz dropped", 22) == 0);
    pc_psd_warnings_fini(&w);
    back = parse(&little, &w);
    CHECK_EQ_UINT(w.n, 0);
    CHECK_EQ_INT(pc_psd_parse_shmd(back.global_blocks[0].data.data, back.global_blocks[0].data.len, &got, &ngot, NULL),
                 PC_OK);
    CHECK(ngot == 2 && pc_psd_meta_item_eq(&got[0], &items[1]) && pc_psd_meta_item_eq(&got[1], &items[2]));
    pc_psd_meta_items_free(got, ngot);
    pc_psd_isd_fini(&back);
    pc_psd_isd_fini(&d);
    pc_psd_bytes_fini(&little);
    pc_psd_bytes_fini(&data);
    pc_psd_bytes_fini(&vd);
    pc_psd_bytes_fini(&items[0].data);
    pc_psd_bytes_fini(&items[1].data);
    pc_psd_bytes_fini(&items[2].data);
}

static void test_unknown_descriptor_shaped_linked_annotations(void)
{
    pc_psd_desc desc;
    pc_psd_value v;
    pc_buf b = PC_BUF_INIT;
    pc_buf item = PC_BUF_INIT;
    pc_buf dd = PC_BUF_INIT;
    pc_psd_bytes big;
    pc_psd_bytes little;
    static const uint8_t png[7] = {0x89, 'P', 'N', 'G', 1, 2, 3};
    static const uint8_t text[6] = {0xfe, 0xff, 0, 'h', 0, 'i'};
    static const char *const strs[3] = {"me", "note", "2026"};
    static const uint16_t aname[6] = {'a', '.', 'p', 'n', 'g', 0};
    int i;
    pc_psd_desc_init(&desc, "null", NULL);
    pc_psd_value_text("x", true, &v, NULL);
    pc_psd_desc_add(&desc, "Nm  ", &v, NULL);
    {
        pc_psd_desc copy;
        pc_psd_desc_clone(&desc, &copy, NULL);
        put_vdesc(&b, &copy);
    }
    block_rt("Qqqq", b.data, b.len, &big, &little);
    pc_psd_bytes_fini(&big);
    pc_psd_bytes_fini(&little);
    pc_buf_truncate(&b, 0);
    put32(&b, 3);
    put_vdesc(&b, &desc);
    block_rt("Qqqq", b.data, b.len, &big, &little);
    pc_psd_bytes_fini(&big);
    pc_psd_bytes_fini(&little);
    block_rt("Mt32", NULL, 0, &big, &little);
    pc_psd_bytes_fini(&big);
    pc_psd_bytes_fini(&little);
    /* lnk2 */
    puts_(&item, "liFD");
    put32(&item, 7);
    pc_buf_append_byte(&item, 4, NULL);
    puts_(&item, "uuid");
    put32(&item, 6);
    for (i = 0; i < 6; i++) {
        put16(&item, aname[i]);
    }
    puts_(&item, "png ");
    puts_(&item, "8BIM");
    put64(&item, 7);
    pc_buf_append_byte(&item, 0, NULL);
    pc_buf_append(&item, png, 7, NULL);
    put32(&item, 0);
    putf64(&item, 0.0);
    pc_buf_append_byte(&item, 0, NULL);
    pc_buf_truncate(&b, 0);
    put64(&b, item.len);
    pc_buf_append(&b, item.data, item.len, NULL);
    while (b.len % 4 != 0) {
        pc_buf_append_byte(&b, 0, NULL);
    }
    block_rt("lnk2", b.data, b.len, &big, &little);
    CHECK(contains(&little, "DFil", 4));
    CHECK(contains(&little, png, 7));
    pc_psd_bytes_fini(&big);
    pc_psd_bytes_fini(&little);
    /* Anno */
    pc_buf_truncate(&b, 0);
    pc_buf_truncate(&item, 0);
    put16(&b, 2);
    put16(&b, 1);
    put32(&b, 1);
    puts_(&item, "txtA");
    pc_buf_append_byte(&item, 0, NULL);
    pc_buf_append_byte(&item, 1, NULL);
    put16(&item, 0);
    for (i = 1; i <= 8; i++) {
        put32(&item, (uint32_t)(i * 10));
    }
    put16(&item, 0);
    put16(&item, 0xffff);
    put16(&item, 0);
    put16(&item, 0);
    put16(&item, 0);
    for (i = 0; i < 3; i++) {
        size_t n = strlen(strs[i]);
        pc_buf_append_byte(&item, (uint8_t)n, NULL);
        puts_(&item, strs[i]);
        if ((1 + n) % 2 == 1) {
            pc_buf_append_byte(&item, 0, NULL);
        }
    }
    puts_(&dd, "txtC");
    put32(&dd, 6);
    pc_buf_append(&dd, text, 6, NULL);
    put32(&item, (uint32_t)dd.len + 4);
    pc_buf_append(&item, dd.data, dd.len, NULL);
    put32(&b, (uint32_t)item.len + 4);
    pc_buf_append(&b, item.data, item.len, NULL);
    block_rt("Anno", b.data, b.len, &big, &little);
    CHECK(contains(&little, "Atxt", 4));
    CHECK(contains(&little, text, 6));
    pc_psd_bytes_fini(&big);
    pc_psd_bytes_fini(&little);
    pc_buf_fini(&b);
    pc_buf_fini(&item);
    pc_buf_fini(&dd);
}

static void test_unknown_blocks(void)
{
    static const uint8_t five[5] = {1, 2, 3, 4, 5};
    pc_psd_isd d = sample_layers();
    pc_psd_isd back;
    pc_psd_bytes big;
    pc_psd_bytes little;
    pc_psd_warnings w;
    pc_buf raw = PC_BUF_INIT;
    uint8_t le[4];
    size_t i;
    int dropped = 0;
    d.global_blocks = realloc(d.global_blocks, (d.nglobal + 1) * sizeof *d.global_blocks);
    pc_psd_block_init(&d.global_blocks[d.nglobal++], "Zzzz", five, 5, NULL);
    big = to_bytes(&d, PC_PSD_BIG, &w);
    CHECK_EQ_UINT(w.n, 0);
    pc_psd_warnings_fini(&w);
    back = parse(&big, &w);
    CHECK(w.n == 0 && pc_psd_blocks_eq(back.global_blocks, back.nglobal, d.global_blocks, d.nglobal));
    pc_psd_warnings_fini(&w);
    pc_psd_isd_fini(&back);
    little = to_bytes(&d, PC_PSD_LITTLE, &w);
    for (i = 0; i < w.n; i++) {
        dropped += strncmp(w.v[i], "Zzzz block (5 bytes) dropped", 28) == 0;
    }
    CHECK_EQ_INT(dropped, 1);
    pc_psd_warnings_fini(&w);
    back = parse(&little, &w);
    CHECK(w.n == 0 && back.nglobal == 2 && memcmp(back.global_blocks[0].key, "Patt", 4) == 0 &&
          memcmp(back.global_blocks[1].key, "FMsk", 4) == 0);
    CHECK(known_layers_eq(&back, &d));
    pc_psd_warnings_fini(&w);
    pc_psd_isd_fini(&back);
    pc_psd_bytes_fini(&big);
    pc_psd_bytes_fini(&little);
    pc_psd_isd_fini(&d);
    /* unknown blocks from a little-endian file are kept with their byte order */
    d = sample_layers();
    little = to_bytes(&d, PC_PSD_LITTLE, NULL);
    pc_buf_append(&raw, little.data, little.len, NULL);
    puts_(&raw, "MIB8zzzZ");
    pc_store_le32(le, 6);
    pc_buf_append(&raw, le, 4, NULL);
    {
        static const uint8_t pl[8] = {9, 8, 7, 6, 5, 4, 0, 0};
        pc_buf_append(&raw, pl, 8, NULL);
    }
    puts_(&raw, "MIB8diyl");
    pc_store_le32(le, 4);
    pc_buf_append(&raw, le, 4, NULL);
    pc_store_le32(le, 7);
    pc_buf_append(&raw, le, 4, NULL);
    {
        pc_psd_bytes rb = {raw.data, raw.len};
        const pc_psd_block *kept = NULL;
        const pc_psd_block *lyid = NULL;
        pc_psd_bytes again;
        pc_psd_bytes big2;
        pc_psd_isd re;
        static const uint8_t want[6] = {9, 8, 7, 6, 5, 4};
        back = parse(&rb, &w);
        CHECK_EQ_UINT(w.n, 0);
        pc_psd_warnings_fini(&w);
        CHECK(known_layers_eq(&back, &d));
        for (i = 0; i < back.nglobal; i++) {
            if (memcmp(back.global_blocks[i].key, "Zzzz", 4) == 0) {
                kept = &back.global_blocks[i];
            }
            if (memcmp(back.global_blocks[i].key, "lyid", 4) == 0) {
                lyid = &back.global_blocks[i];
            }
        }
        CHECK(kept != NULL && memcmp(kept->signature, "MIB8", 4) == 0 && kept->data.len == 6 &&
              memcmp(kept->data.data, want, 6) == 0);
        CHECK(lyid != NULL && lyid->data.len == 4 && pc_load_be32(lyid->data.data) == 7);
        again = to_bytes(&back, PC_PSD_LITTLE, &w);
        CHECK_EQ_UINT(w.n, 0);
        pc_psd_warnings_fini(&w);
        CHECK(contains(&again, raw.data + raw.len - 34, 34));
        re = parse(&again, NULL);
        CHECK(pc_psd_blocks_eq(re.global_blocks, re.nglobal, back.global_blocks, back.nglobal));
        pc_psd_isd_fini(&re);
        big2 = to_bytes(&back, PC_PSD_BIG, NULL);
        CHECK(contains(&big2, "MIB8Zzzz", 8));
        re = parse(&big2, NULL);
        CHECK(pc_psd_blocks_eq(re.global_blocks, re.nglobal, back.global_blocks, back.nglobal));
        pc_psd_isd_fini(&re);
        pc_psd_bytes_fini(&big2);
        pc_psd_bytes_fini(&again);
        pc_psd_isd_fini(&back);
    }
    pc_buf_fini(&raw);
    pc_psd_bytes_fini(&little);
    pc_psd_isd_fini(&d);
}

static void test_rle_counts_and_blending_ranges(void)
{
    pc_psd_file f = tg_layered(PC_PSD_V1, PC_PSD_MODE_RGB, 16, PC_PSD_RLE);
    pc_psd_isd d;
    pc_psd_isd back;
    pc_psd_isd kd;
    pc_psd_bytes big;
    pc_psd_bytes little;
    size_t i;
    size_t k;
    bool found = false;
    pc_psd_isd_from_psd(&f, &d, NULL);
    big = to_bytes(&d, PC_PSD_BIG, NULL);
    little = to_bytes(&d, PC_PSD_LITTLE, NULL);
    back = parse(&little, NULL);
    kd = known(&d);
    for (i = 0; i < kd.layer_info.n && i < back.layer_info.n; i++) {
        const pc_psd_layer *la = &kd.layer_info.layers[i];
        for (k = 0; k < la->nchannels && k < back.layer_info.layers[i].nchannels; k++) {
            const pc_psd_channel *ca = &la->channels[k];
            CHECK(ca->has_compression == back.layer_info.layers[i].channels[k].has_compression &&
                  pc_psd_bytes_eq(&ca->data, &back.layer_info.layers[i].channels[k].data));
            if (!found && ca->id >= -1 && ca->has_compression && ca->compression == PC_PSD_RLE &&
                pc_psd_rect_height(la->rect) > 1 &&
                ca->data.len > (size_t)pc_psd_rect_height(la->rect) * 2 + 8) {
                size_t table = (size_t)pc_psd_rect_height(la->rect) * 2;
                pc_psd_bytes samples = {ca->data.data + table, ca->data.len - table};
                uint8_t *sw = malloc(table + samples.len);
                size_t q;
                for (q = 0; q < table; q += 2) {
                    sw[q] = ca->data.data[q + 1];
                    sw[q + 1] = ca->data.data[q];
                }
                memcpy(sw + table, samples.data, samples.len);
                CHECK(contains(&big, samples.data, samples.len));
                CHECK(contains(&little, sw, table + samples.len));
                free(sw);
                found = true;
            }
        }
    }
    CHECK(found);
    pc_psd_isd_fini(&kd);
    pc_psd_isd_fini(&back);
    pc_psd_bytes_fini(&big);
    pc_psd_bytes_fini(&little);
    pc_psd_isd_fini(&d);
    pc_psd_file_fini(&f);
    /* blending ranges are bytes in either order */
    {
        uint8_t ranges[40];
        uint8_t marked[40];
        pc_psd_file g = tg_layered(PC_PSD_V1, PC_PSD_MODE_RGB, 8, PC_PSD_RLE);
        for (i = 0; i < 40; i++) {
            static const uint8_t pat[4] = {0, 0, 255, 255};
            ranges[i] = pat[i % 4];
            marked[i] = (uint8_t)(i + 1);
        }
        for (i = 0; i < g.layer_info.n; i++) {
            pc_psd_bytes_set(&g.layer_info.layers[i].blending_ranges, i == 0 ? marked : ranges, 40, NULL);
        }
        pc_psd_isd_from_psd(&g, &d, NULL);
        little = to_bytes(&d, PC_PSD_LITTLE, NULL);
        CHECK(contains(&little, marked, 40));
        back = parse(&little, NULL);
        for (i = 0; i < d.layer_info.n; i++) {
            CHECK(pc_psd_bytes_eq(&d.layer_info.layers[i].blending_ranges, &back.layer_info.layers[i].blending_ranges));
        }
        pc_psd_isd_fini(&back);
        pc_psd_bytes_fini(&little);
        pc_psd_isd_fini(&d);
        pc_psd_file_fini(&g);
    }
    /* PSB-sized data */
    f = tg_layered(PC_PSD_V2, PC_PSD_MODE_CMYK, 8, PC_PSD_RLE);
    pc_psd_isd_from_psd(&f, &d, NULL);
    CHECK(d.version == PC_PSD_V2);
    back = roundtrip(&d);
    CHECK(pc_psd_layer_info_eq(&back.layer_info, &d.layer_info));
    pc_psd_isd_fini(&back);
    little = to_bytes(&d, PC_PSD_LITTLE, NULL);
    CHECK(memcmp(little.data + 36, "MIB8", 4) == 0);
    back = parse(&little, NULL);
    CHECK(known_layers_eq(&back, &d));
    pc_psd_isd_fini(&back);
    pc_psd_bytes_fini(&little);
    pc_psd_isd_fini(&d);
    pc_psd_file_fini(&f);
}

static void test_resources_both_orders(void)
{
    static const uint8_t r1005[16] = {0, 72, 0, 0, 0, 1, 0, 1, 0, 72, 0, 0, 0, 1, 0, 1};
    static const uint8_t r1037[4] = {0, 0, 0, 120};
    static const uint8_t icc[3] = {1, 2, 3};
    static const uint8_t grp[6] = {0, 1, 0, 1, 0, 0};
    pc_psd_resource res[4];
    pc_psd_resource *back = NULL;
    size_t nb = 0;
    pc_psd_bytes big = {NULL, 0};
    pc_psd_bytes little = {NULL, 0};
    pc_psd_warnings w;
    size_t i;
    pc_psd_resource_init(&res[0], 1005, r1005, 16, NULL);
    pc_psd_resource_init(&res[1], 1037, r1037, 4, NULL);
    pc_psd_resource_init(&res[2], 1039, icc, 3, NULL);
    pc_psd_resource_init(&res[3], 1026, grp, 6, NULL);
    pc_psd_bytes_set(&res[3].name, "grp", 3, NULL);
    CHECK_EQ_INT(pc_psd_tiff_resources_to_bytes(res, 4, &big, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_tiff_resources_parse(big.data, big.len, &back, &nb, &w, NULL), PC_OK);
    CHECK(w.n == 0 && nb == 4);
    for (i = 0; i < 4 && i < nb; i++) {
        CHECK(pc_psd_resource_eq(&back[i], &res[i]));
    }
    pc_psd_resources_free(back, nb);
    CHECK_EQ_INT(pc_psd_transcode_resources(big.data, big.len, PC_PSD_BIG, PC_PSD_LITTLE, &little, &w, NULL), PC_OK);
    CHECK(w.n == 0 && memcmp(little.data, "MIB8", 4) == 0 && little.len == big.len);
    CHECK(little.data[4] == 0xed && little.data[5] == 3);
    CHECK_EQ_INT(pc_psd_tiff_resources_parse(little.data, little.len, &back, &nb, &w, NULL), PC_OK);
    CHECK(nb == 4);
    for (i = 0; i < 4 && i < nb; i++) {
        CHECK(pc_psd_resource_eq(&back[i], &res[i]));
    }
    pc_psd_resources_free(back, nb);
    CHECK(contains(&little, icc, 3));
    big.data = realloc(big.data, big.len + 1);
    big.data[big.len++] = 0;
    CHECK_EQ_INT(pc_psd_tiff_resources_parse(big.data, big.len, &back, &nb, NULL, NULL), PC_OK);
    CHECK_EQ_UINT(nb, 4);
    pc_psd_resources_free(back, nb);
    pc_psd_bytes_fini(&big);
    pc_psd_bytes_fini(&little);
    for (i = 0; i < 4; i++) {
        pc_psd_resource_fini(&res[i]);
    }
}

static void test_malformed_input(void)
{
    pc_psd_isd d;
    uint8_t bad[44];
    pc_psd_bytes b;
    pc_psd_resource *r = NULL;
    size_t nr;
    int o;
    size_t cut;
    size_t i;
    uint8_t huge[48];
    CHECK(pc_psd_isd_parse((const uint8_t *)"", 0, &d, NULL, NULL) != PC_OK);
    CHECK(pc_psd_isd_parse((const uint8_t *)"Adobe Photoshop Document Data Block", 35, &d, NULL, NULL) != PC_OK);
    memcpy(bad, PC_PSD_ISD_SIGNATURE, 36);
    memcpy(bad + 36, "XXXXLayr", 8);
    CHECK(pc_psd_isd_parse(bad, 44, &d, NULL, NULL) != PC_OK);
    d = sample_layers();
    for (o = 0; o < 2; o++) {
        pc_psd_isd x;
        pc_psd_warnings w;
        b = to_bytes(&d, o == 0 ? PC_PSD_BIG : PC_PSD_LITTLE, NULL);
        for (cut = 0; cut < b.len; cut += cut + 8 >= b.len ? 1 : 7) {
            if (pc_psd_isd_parse(b.data, cut, &x, &w, NULL) == PC_OK) {
                pc_psd_isd_fini(&x);
                pc_psd_warnings_fini(&w);
            }
        }
        for (i = 36; i < b.len; i += 13) {
            b.data[i] ^= 0x5a;
            if (pc_psd_isd_parse(b.data, b.len, &x, &w, NULL) == PC_OK) {
                pc_psd_isd_fini(&x);
                pc_psd_warnings_fini(&w);
            }
            b.data[i] ^= 0x5a;
        }
        pc_psd_bytes_fini(&b);
    }
    pc_psd_isd_fini(&d);
    memcpy(huge, PC_PSD_ISD_SIGNATURE, 36);
    memcpy(huge + 36, "8BIMluni", 8);
    pc_store_be32(huge + 44, 0xFFFFFFFFu);
    CHECK(pc_psd_isd_parse(huge, 48, &d, NULL, NULL) != PC_OK);
    CHECK(pc_psd_tiff_resources_parse((const uint8_t *)"8BIM\x03\xed\x00", 7, &r, &nr, NULL, NULL) != PC_OK);
}

static void test_into_psd_roundtrips_through_the_psd_writer(void)
{
    pc_psd_file f = tg_layered(PC_PSD_V1, PC_PSD_MODE_GRAYSCALE, 8, PC_PSD_ZIP);
    pc_psd_isd d;
    pc_psd_isd back;
    pc_psd_isd exp;
    pc_psd_isd ag;
    pc_psd_bytes little;
    pc_psd_bytes pb = {NULL, 0};
    pc_psd_file psd;
    pc_psd_file again;
    pc_psd_resource *res;
    pc_psd_bytes img = {NULL, 0};
    pc_psd_node *t1 = NULL;
    pc_psd_node *t2 = NULL;
    size_t n1 = 0;
    size_t n2 = 0;
    size_t i;
    pc_psd_isd_from_psd(&f, &d, NULL);
    little = to_bytes(&d, PC_PSD_LITTLE, NULL);
    back = parse(&little, NULL);
    res = calloc(f.nresources, sizeof *res);
    for (i = 0; i < f.nresources; i++) {
        pc_psd_resource_clone(&f.resources[i], &res[i], NULL);
    }
    pc_psd_bytes_clone(&f.image_data, &img, NULL);
    pc_psd_isd_into_psd(&back, f.header, &res, f.nresources, f.image_compression, &img, &psd, NULL);
    CHECK_EQ_INT(pc_psd_file_to_bytes(&psd, &pb, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_file_parse(pb.data, pb.len, &again, NULL), PC_OK);
    pc_psd_isd_from_psd(&f, &exp, NULL);
    pc_psd_isd_from_psd(&again, &ag, NULL);
    CHECK(known_layers_eq(&ag, &exp));
    {
        pc_psd_isd ke = known(&exp);
        CHECK(pc_psd_blocks_eq(again.global_blocks, again.nglobal, ke.global_blocks, ke.nglobal));
        pc_psd_isd_fini(&ke);
    }
    pc_psd_layer_tree(&again, &t1, &n1, NULL);
    pc_psd_layer_tree(&f, &t2, &n2, NULL);
    CHECK_EQ_UINT(n1, n2);
    pc_psd_nodes_free(t1, n1);
    pc_psd_nodes_free(t2, n2);
    pc_psd_isd_fini(&ag);
    pc_psd_isd_fini(&exp);
    pc_psd_file_fini(&again);
    pc_psd_file_fini(&psd);
    pc_psd_bytes_fini(&pb);
    pc_psd_bytes_fini(&little);
    pc_psd_isd_fini(&back);
    pc_psd_isd_fini(&d);
    pc_psd_file_fini(&f);
}

int main(int argc, char **argv)
{
    if (pc_file_read("tests/golden/tiff-vectors.txt", (size_t)1 << 20, &golden, NULL) != PC_OK) {
        (void)printf("tests/golden/tiff-vectors.txt missing\n");
        return 1;
    }
    RUN(test_parity);
    RUN(test_generated_files_survive_both_orders);
    RUN(test_same_order_is_identity);
    RUN(test_signature_and_keys);
    RUN(test_blocks_are_padded_to_four);
    RUN(test_descriptor_and_type_blocks);
    RUN(test_legacy_effects_and_adjustments);
    RUN(test_global_mask_gradient_vector);
    RUN(test_pattern_block);
    RUN(test_filter_effects_and_metadata);
    RUN(test_unknown_descriptor_shaped_linked_annotations);
    RUN(test_unknown_blocks);
    RUN(test_rle_counts_and_blending_ranges);
    RUN(test_resources_both_orders);
    RUN(test_malformed_input);
    RUN(test_into_psd_roundtrips_through_the_psd_writer);
    pc_buf_fini(&golden);
    return pc_t_finish();
}
