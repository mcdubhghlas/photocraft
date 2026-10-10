/* Unit tests on synthetic inputs (port of crates/affinity/src/tests.rs) and the public-document
 * tests (crates/affinity/tests/real_files.rs, which skip without corpus/affinity). Proptests are
 * seeded loops with the same case counts. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "affinity/affinity.h"
#include "base/buf.h"
#include "base/compress.h"
#include "base/file.h"
#include "harness.h"
#include "synth.h"

typedef struct rng {
    uint64_t s;
} rng;

static uint64_t rnext(rng *r)
{
    r->s = r->s * 6364136223846793005ull + 1442695040888963407ull;
    return r->s >> 33;
}

static void be32(pc_buf *b, uint32_t v)
{
    uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    sy_put(b, x, 4);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* ---- preview ---- */

static pc_buf fixture(void)
{
    pc_buf png = PC_BUF_INIT;
    pc_buf b = PC_BUF_INIT;
    static const uint8_t IHDR_TAIL[5] = {8, 6, 0, 0, 0};
    uint8_t head[72];
    sy_put(&png, "\x89PNG\r\n\x1a\n", 8);
    be32(&png, 13);
    sy_put(&png, "IHDR", 4);
    be32(&png, 2);
    be32(&png, 1);
    sy_put(&png, IHDR_TAIL, 5);
    be32(&png, pc_crc32(0, png.data + 12, png.len - 12));
    be32(&png, 0);
    sy_put(&png, "IDAT", 4);
    be32(&png, pc_crc32(0, (const uint8_t *)"IDAT", 4));
    be32(&png, 0);
    sy_put(&png, "IEND", 4);
    be32(&png, pc_crc32(0, (const uint8_t *)"IEND", 4));
    memset(head, 0, sizeof head);
    memcpy(head, PC_AF_MAGIC, 4);
    head[4] = 12;
    memcpy(head + 8, "nsrP", 4);
    memcpy(head + 12, "#Inf", 4);
    head[24] = 72;
    memcpy(head + 64, "Prot", 4);
    sy_put(&b, head, 72);
    sy_put(&b, "\xff\xff\xff\xffThmb", 8);
    sy_le(&b, 1, 4);
    sy_le(&b, png.len + 13, 4);
    sy_le(&b, 29, 4);
    sy_le(&b, 0, 4);
    sy_le(&b, png.len, 4);
    sy_byte(&b, 1);
    sy_put(&b, png.data, png.len);
    pc_buf_fini(&png);
    return b;
}

static bool preview_ok(const uint8_t *b, size_t n)
{
    pc_af_preview p;
    return pc_af_preview_read(b, n, &p, NULL) == PC_OK;
}

static void test_indexed_preview_and_header(void)
{
    pc_buf b = fixture();
    pc_af_header h;
    pc_af_preview p;
    CHECK_EQ_INT(pc_af_header_read(b.data, b.len, &h, NULL), PC_OK);
    CHECK_EQ_UINT(h.version, 12);
    CHECK_EQ_INT(pc_af_preview_read(b.data, b.len, &p, NULL), PC_OK);
    CHECK(p.width == 2 && p.height == 1);
    CHECK(p.png == b.data + 101 && p.len == b.len - 101);
    pc_buf_fini(&b);
}

static void test_every_truncation_is_rejected(void)
{
    pc_buf b = fixture();
    size_t end;
    for (end = 0; end < b.len; end++) {
        uint8_t *c = malloc(end > 0 ? end : 1);
        memcpy(c, b.data, end);
        if (preview_ok(c, end)) {
            PC_T_FAIL("truncation at %zu accepted", end);
        }
        free(c);
    }
    pc_buf_fini(&b);
}

static void test_rejects_assets_versions_offsets_lengths_and_bombs(void)
{
    pc_buf b = fixture();
    static const struct {
        size_t at;
        uint8_t v[8];
        size_t n;
    } P[9] = {{4, {13, 0, 0, 0, 0, 0, 0, 0}, 8},
              {8, {'u', 'r', 'B', 'R'}, 4},
              {24, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, 8},
              {24, {1, 0, 0, 0, 0, 0, 0, 0}, 8},
              {24, {0, 0, 0, 0, 0, 0, 0, 0}, 8},
              {84, {0xff, 0xff, 0xff, 0xff}, 4},
              {88, {30, 0, 0, 0}, 4},
              {96, {0xff, 0xff, 0xff, 0xff}, 4},
              {117, {0, 0, 0x13, 0x88}, 4}};
    size_t i;
    uint16_t version;
    pc_error e;
    pc_af_preview p;
    for (i = 0; i < 9; i++) {
        uint8_t *c = malloc(b.len);
        memcpy(c, b.data, b.len);
        memcpy(c + P[i].at, P[i].v, P[i].n);
        if (preview_ok(c, b.len)) {
            PC_T_FAIL("offset %zu accepted", P[i].at);
        }
        free(c);
    }
    /* Versions 8 to 11 store the same record. */
    for (version = 8; version < 12; version++) {
        uint8_t *c = malloc(b.len);
        memcpy(c, b.data, b.len);
        c[4] = (uint8_t)version;
        c[5] = 0;
        CHECK(preview_ok(c, b.len));
        free(c);
    }
    /* Flags 4 and 8 occur in real files; the low two bits mark other container variants. */
    {
        static const uint16_t F[4] = {4, 8, 1, 2};
        static const bool OK[4] = {true, true, false, false};
        for (i = 0; i < 4; i++) {
            uint8_t *c = malloc(b.len);
            memcpy(c, b.data, b.len);
            c[6] = (uint8_t)F[i];
            c[7] = 0;
            CHECK(preview_ok(c, b.len) == OK[i]);
            free(c);
        }
    }
    {
        uint8_t *c = malloc(b.len);
        memcpy(c, b.data, b.len);
        c[4] = 7;
        c[5] = 0;
        CHECK(pc_af_preview_read(c, b.len, &p, &e) == PC_ERR_UNSUPPORTED);
        free(c);
    }
    pc_buf_fini(&b);
}

static void test_an_unindexed_png_or_resource_is_never_used(void)
{
    pc_buf b = fixture();
    memset(b.data + 24, 0, 8);
    CHECK(!preview_ok(b.data, b.len));
    b.data[24] = 72;
    memcpy(b.data + 76, "Meta", 4);
    CHECK(!preview_ok(b.data, b.len));
    pc_buf_fini(&b);
}

/* Inserts a zero-length chunk of kind before the IEND chunk and fixes the record lengths. */
static pc_buf with_extra(const pc_buf *orig, const char *kind)
{
    pc_buf b = PC_BUF_INIT;
    size_t at = orig->len - 12;
    uint32_t length;
    sy_put(&b, orig->data, at);
    be32(&b, 0);
    sy_put(&b, kind, 4);
    be32(&b, pc_crc32(0, (const uint8_t *)kind, 4));
    sy_put(&b, orig->data + at, 12);
    length = (uint32_t)(b.len - 101);
    put_le32(b.data + 84, length + 13);
    put_le32(b.data + 96, length);
    return b;
}

static void test_every_chunk_checksum_including_the_end_is_checked(void)
{
    pc_buf b = fixture();
    size_t ats[2];
    size_t i;
    pc_buf w;
    pc_error e;
    pc_af_preview p;
    ats[0] = 130;
    ats[1] = b.len - 1;
    for (i = 0; i < 2; i++) {
        uint8_t *c = malloc(b.len);
        memcpy(c, b.data, b.len);
        c[ats[i]] ^= 1;
        CHECK(pc_af_preview_read(c, b.len, &p, &e) == PC_ERR_FORMAT);
        CHECK_EQ_STR(e.msg, "damaged Affinity file: PNG chunk checksum");
        free(c);
    }
    w = with_extra(&b, "tEXt");
    CHECK(preview_ok(w.data, w.len));
    w.data[b.len - 12 + 8] ^= 1;
    CHECK(pc_af_preview_read(w.data, w.len, &p, &e) == PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "damaged Affinity file: PNG chunk checksum");
    pc_buf_fini(&w);
    pc_buf_fini(&b);
}

static void test_compressed_metadata_and_animation_are_not_preview_support(void)
{
    static const char *const K[6] = {"zTXt", "iTXt", "iCCP", "acTL", "fcTL", "fdAT"};
    pc_buf b = fixture();
    size_t i;
    for (i = 0; i < 6; i++) {
        pc_buf w = with_extra(&b, K[i]);
        pc_af_preview p;
        CHECK(pc_af_preview_read(w.data, w.len, &p, NULL) == PC_ERR_UNSUPPORTED);
        pc_buf_fini(&w);
    }
    pc_buf_fini(&b);
}

static pc_buf with_chunks(const char *const *kinds, size_t n)
{
    pc_buf orig = fixture();
    pc_buf b = PC_BUF_INIT;
    size_t i;
    uint32_t length;
    sy_put(&b, orig.data, 134);
    for (i = 0; i < n; i++) {
        be32(&b, 0);
        sy_put(&b, kinds[i], 4);
        be32(&b, pc_crc32(0, (const uint8_t *)kinds[i], 4));
    }
    length = (uint32_t)(b.len - 101);
    put_le32(b.data + 84, length + 13);
    put_le32(b.data + 96, length);
    pc_buf_fini(&orig);
    return b;
}

static void test_critical_chunks_follow_a_single_complete_png_layout(void)
{
    static const char *const OK1[3] = {"IDAT", "IDAT", "IEND"};
    static const char *const OK2[3] = {"abct", "IDAT", "IEND"};
    static const char *const BAD[7][4] = {{"IEND"},
                                          {"IDAT", "IHDR", "IEND"},
                                          {"PLTE", "PLTE", "IDAT", "IEND"},
                                          {"IDAT", "PLTE", "IEND"},
                                          {"IDAT", "tEXt", "IDAT", "IEND"},
                                          {"ABCD", "IDAT", "IEND"},
                                          {"IDAT", "IEND", "tEXt"}};
    static const size_t NB[7] = {1, 3, 4, 3, 4, 3, 3};
    pc_buf b = with_chunks(OK1, 3);
    size_t i;
    CHECK(preview_ok(b.data, b.len));
    pc_buf_fini(&b);
    /* An unknown ancillary chunk with the reserved bit set is still valid. */
    b = with_chunks(OK2, 3);
    CHECK(preview_ok(b.data, b.len));
    pc_buf_fini(&b);
    for (i = 0; i < 7; i++) {
        b = with_chunks(BAD[i], NB[i]);
        if (preview_ok(b.data, b.len)) {
            PC_T_FAIL("chunk layout %zu accepted", i);
        }
        pc_buf_fini(&b);
    }
}

static void test_arbitrary_input_and_mutations_never_panic(void)
{
    rng r = {256};
    pc_buf fx = fixture();
    int it;
    for (it = 0; it < 256; it++) {
        size_t n = (size_t)(rnext(&r) % 2048);
        uint8_t *b = malloc(n > 0 ? n : 1);
        size_t i;
        pc_af_header h;
        for (i = 0; i < n; i++) {
            b[i] = (uint8_t)rnext(&r);
        }
        (void)pc_af_header_read(b, n, &h, NULL);
        (void)preview_ok(b, n);
        free(b);
    }
    for (it = 0; it < 256; it++) {
        uint8_t *b = malloc(fx.len);
        size_t edits = (size_t)(rnext(&r) % 32);
        size_t k;
        memcpy(b, fx.data, fx.len);
        for (k = 0; k < edits; k++) {
            size_t i = (size_t)(rnext(&r) % 200);
            uint8_t v = (uint8_t)rnext(&r);
            if (i < fx.len) {
                b[i] = v;
            }
        }
        (void)preview_ok(b, fx.len);
        free(b);
    }
    pc_buf_fini(&fx);
}

/* ---- archive and stream ---- */

static pc_buf doc(void)
{
    pc_buf d = PC_BUF_INIT;
    static const double XF[6] = {1.0, 0.0, 5.0, 0.0, 1.0, -2.0};
    static const char *const RGBA[1] = {"RGBA"};
    static const char *const GRUP[2] = {"Grup", "Node"};
    static const uint8_t STAT[3] = {4, 0, 2};
    uint8_t zero16[16];
    uint8_t recs[4 * 18];
    memset(zero16, 0, sizeof zero16);
    memset(recs, 0, sizeof recs);
    sy_stream_begin(&d);
    sy_str(&d, "Desc", "Document");
    sy_bool(&d, "Visi", true);
    sy_f64s(&d, "Xfrm", XF, 6);
    sy_def(&d, "Colr", 7, RGBA, 1);
    sy_struct(&d, "_col", zero16, 16);
    sy_end(&d);
    sy_ref(&d, "Same", 7);
    sy_null(&d, "Null");
    sy_shared(&d, "Kids", 2);
    sy_def_payload(&d, 8, GRUP, 2);
    sy_str(&d, "Desc", "g");
    sy_end(&d);
    sy_ref_payload(&d, 8);
    sy_obj(&d, "Crvs", "PCvD");
    sy_pos(&d, "Data");
    sy_u8(&d, NULL, 0);
    sy_u32(&d, NULL, 1);
    sy_bool(&d, NULL, true);
    sy_records(&d, NULL, 18, recs, 4);
    sy_end(&d);
    sy_end(&d);
    sy_enum(&d, "Enum", 2, 1);
    sy_entry(&d, "Tile", "d/3");
    sy_u8s(&d, "Stat", STAT, 3);
    sy_end(&d);
    return d;
}

static pc_buf container1(const pc_buf *d, sy_method m)
{
    sy_entry_t e[1];
    e[0].name = "doc.dat";
    e[0].data = d->data;
    e[0].len = d->len;
    e[0].method = m;
    return sy_container(e, 1, NULL, 0);
}

static void test_every_method_round_trips_with_checksums(void)
{
    pc_buf d = doc();
    int m;
    for (m = 0; m < 3; m++) {
        sy_entry_t e[2];
        pc_buf file;
        pc_af_archive a;
        uint8_t *out;
        size_t n;
        e[0].name = "doc.dat";
        e[0].data = d.data;
        e[0].len = d.len;
        e[0].method = (sy_method)m;
        e[1].name = "d/3";
        e[1].data = (const uint8_t *)"tile";
        e[1].len = 4;
        e[1].method = (sy_method)m;
        file = sy_container(e, 2, NULL, 0);
        CHECK_EQ_INT(pc_af_archive_open(file.data, file.len, pc_af_limits_default(), &a, NULL), PC_OK);
        CHECK_EQ_UINT(a.header.version, 12);
        CHECK_EQ_INT(pc_af_archive_read(&a, "doc.dat", &out, &n, NULL), PC_OK);
        CHECK(n == d.len && memcmp(out, d.data, n) == 0);
        free(out);
        CHECK_EQ_INT(pc_af_archive_read(&a, "d/3", &out, &n, NULL), PC_OK);
        CHECK(n == 4 && memcmp(out, "tile", 4) == 0);
        free(out);
        CHECK_EQ_UINT(a.extracted, d.len + 4);
        CHECK(pc_af_archive_read(&a, "missing", &out, &n, NULL) != PC_OK);
        pc_af_archive_fini(&a);
        pc_buf_fini(&file);
    }
    pc_buf_fini(&d);
}

static const pc_af_value *get(const pc_af_stream *s, size_t id, const char *t)
{
    return pc_af_object_get(&s->objects[id], PC_AF_TAG(t));
}

static void test_the_stream_parses_into_shared_objects_and_links(void)
{
    pc_buf d = doc();
    pc_af_stream s;
    const pc_af_value *v;
    const pc_af_value *a;
    const pc_af_value *b;
    CHECK_EQ_INT(pc_af_stream_parse(d.data, d.len, &s, NULL), PC_OK);
    CHECK(s.objects[s.root].cls == PC_AF_TAG("Pers"));
    v = get(&s, s.root, "Desc");
    CHECK(v != NULL && v->kind == PC_AF_STR && strcmp(v->as.str.s, "Document") == 0);
    v = get(&s, s.root, "Xfrm");
    CHECK(v != NULL && v->kind == PC_AF_FLOATS && v->as.floats.n == 6 && v->as.floats.v[2] == 5.0 &&
          v->as.floats.v[5] == -2.0);
    a = get(&s, s.root, "Colr");
    b = get(&s, s.root, "Same");
    CHECK(a != NULL && b != NULL && a->kind == PC_AF_OBJ && b->kind == PC_AF_OBJ && a->as.obj.some &&
          b->as.obj.some && a->as.obj.id == b->as.obj.id);
    if (a != NULL && a->kind == PC_AF_OBJ && a->as.obj.some) {
        CHECK(s.objects[a->as.obj.id].cls == PC_AF_TAG("RGBA"));
        CHECK(s.objects[a->as.obj.id].kind == PC_AF_SHARED);
    }
    v = get(&s, s.root, "Kids");
    CHECK(v != NULL && v->kind == PC_AF_ARRAY && v->as.arr.n == 2);
    if (v != NULL && v->kind == PC_AF_ARRAY && v->as.arr.n == 2) {
        const pc_af_object *g;
        CHECK(v->as.arr.v[0].as.obj.id == v->as.arr.v[1].as.obj.id);
        g = &s.objects[v->as.arr.v[0].as.obj.id];
        CHECK(g->nchain == 2 && g->chain[0] == PC_AF_TAG("Grup") && g->chain[1] == PC_AF_TAG("Node"));
    }
    v = get(&s, s.root, "Enum");
    CHECK(v != NULL && v->kind == PC_AF_ENUM && v->as.e.id == 2 && v->as.e.version == 1);
    v = get(&s, s.root, "Tile");
    CHECK(v != NULL && v->kind == PC_AF_ENTRY && strcmp(v->as.str.s, "d/3") == 0);
    v = get(&s, s.root, "Null");
    CHECK(v != NULL && v->kind == PC_AF_OBJ && !v->as.obj.some);
    pc_af_stream_fini(&s);
    pc_buf_fini(&d);
}

static void test_checksums_sizes_and_budgets_are_enforced(void)
{
    pc_buf d = doc();
    pc_buf file = container1(&d, SY_ZSTD);
    pc_af_archive a;
    pc_af_limits l;
    uint8_t *out;
    size_t n;
    pc_error e;
    /* Flip a byte inside the compressed payload, past the "#Fil" tag at offset 72. */
    file.data[90] ^= 0x55;
    CHECK_EQ_INT(pc_af_archive_open(file.data, file.len, pc_af_limits_default(), &a, NULL), PC_OK);
    CHECK(pc_af_archive_read(&a, "doc.dat", &out, &n, NULL) != PC_OK);
    pc_af_archive_fini(&a);
    file.data[90] ^= 0x55;
    l.max_entry = d.len - 1;
    l.max_total = SIZE_MAX;
    CHECK_EQ_INT(pc_af_archive_open(file.data, file.len, l, &a, NULL), PC_OK);
    CHECK(pc_af_archive_read(&a, "doc.dat", &out, &n, &e) == PC_ERR_LIMIT);
    pc_af_archive_fini(&a);
    l.max_entry = SIZE_MAX;
    l.max_total = d.len + 1;
    CHECK_EQ_INT(pc_af_archive_open(file.data, file.len, l, &a, NULL), PC_OK);
    CHECK_EQ_INT(pc_af_archive_read(&a, "doc.dat", &out, &n, NULL), PC_OK);
    free(out);
    CHECK(pc_af_archive_read(&a, "doc.dat", &out, &n, &e) == PC_ERR_LIMIT);
    pc_af_archive_fini(&a);
    pc_buf_fini(&file);
    pc_buf_fini(&d);
}

static void test_a_declared_size_smaller_than_the_data_is_rejected_not_truncated(void)
{
    size_t big_n = (size_t)1 << 20;
    uint8_t *big = malloc(big_n);
    sy_entry_t e[1];
    pc_buf file;
    size_t fat = 0;
    size_t at;
    int i;
    pc_af_archive a;
    uint8_t *out;
    size_t n;
    pc_error err;
    memset(big, 7, big_n);
    e[0].name = "doc.dat";
    e[0].data = big;
    e[0].len = big_n;
    e[0].method = SY_ZLIB;
    file = sy_container(e, 1, NULL, 0);
    for (i = 7; i >= 0; i--) {
        fat = fat << 8 | file.data[16 + i];
    }
    at = fat + 59 + 5 + 8;
    memset(file.data + at, 0, 8);
    file.data[at + 1] = 4; /* 1024 */
    CHECK_EQ_INT(pc_af_archive_open(file.data, file.len, pc_af_limits_default(), &a, NULL), PC_OK);
    CHECK(pc_af_archive_read(&a, "doc.dat", &out, &n, &err) == PC_ERR_FORMAT);
    CHECK_EQ_STR(err.msg, "damaged Affinity file: archive entry size mismatch");
    pc_af_archive_fini(&a);
    pc_buf_fini(&file);
    free(big);
}

static void test_libraries_and_cyclic_tables_are_rejected(void)
{
    pc_buf d = doc();
    pc_buf file = container1(&d, SY_STORED);
    pc_af_archive a;
    pc_error e;
    uint8_t *lib = malloc(file.len);
    size_t fat = 0;
    int i;
    memcpy(lib, file.data, file.len);
    memcpy(lib + 8, "rAsA", 4);
    CHECK(pc_af_archive_open(lib, file.len, pc_af_limits_default(), &a, &e) == PC_ERR_UNSUPPORTED);
    free(lib);
    for (i = 7; i >= 0; i--) {
        fat = fat << 8 | file.data[16 + i];
    }
    memcpy(file.data + fat + 4, file.data + 16, 8);
    CHECK(pc_af_archive_open(file.data, file.len, pc_af_limits_default(), &a, &e) == PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "damaged Affinity file: allocation tables form a cycle");
    pc_buf_fini(&file);
    pc_buf_fini(&d);
}

static bool parse_ok(const pc_buf *b)
{
    pc_af_stream s;
    if (pc_af_stream_parse(b->data, b->len, &s, NULL) == PC_OK) {
        pc_af_stream_fini(&s);
        return true;
    }
    return false;
}

static void test_hostile_streams_fail_cleanly(void)
{
    pc_buf b = PC_BUF_INIT;
    pc_af_stream s;
    pc_error e;
    int i;
    size_t end;
    pc_buf d;
    /* 2000 nested inline groups. */
    sy_stream_begin(&b);
    for (i = 0; i < 2000; i++) {
        sy_obj(&b, "Chld", "Grup");
    }
    for (i = 0; i < 2000; i++) {
        sy_end(&b);
    }
    sy_end(&b);
    CHECK(pc_af_stream_parse(b.data, b.len, &s, &e) == PC_ERR_LIMIT);
    CHECK_EQ_STR(e.msg, "Affinity file exceeds a safety limit: objects nested too deeply");
    pc_buf_fini(&b);
    /* An array claiming four billion elements in a few bytes. */
    {
        static const uint8_t RAW[10] = {0x81, '_', 'g', 'i', 'B', 0xff, 0xff, 0xff, 0xff, 0};
        sy_stream_begin(&b);
        sy_put(&b, RAW, 10);
        sy_end(&b);
        CHECK(pc_af_stream_parse(b.data, b.len, &s, &e) == PC_ERR_FORMAT);
        CHECK_EQ_STR(e.msg, "damaged Affinity file: array longer than its data");
        pc_buf_fini(&b);
    }
    sy_stream_begin(&b);
    sy_ref(&b, "Link", 99);
    sy_end(&b);
    CHECK(!parse_ok(&b));
    pc_buf_fini(&b);
    {
        static const char *const RGBA[1] = {"RGBA"};
        sy_stream_begin(&b);
        sy_def(&b, "A___", 1, RGBA, 1);
        sy_end(&b);
        sy_def(&b, "B___", 1, RGBA, 1);
        sy_end(&b);
        sy_end(&b);
        CHECK(!parse_ok(&b));
        pc_buf_fini(&b);
    }
    {
        static const uint8_t RAW[5] = {0x0b, 1, 2, 3, 4};
        sy_stream_begin(&b);
        sy_put(&b, RAW, 5);
        sy_end(&b);
        CHECK(pc_af_stream_parse(b.data, b.len, &s, &e) == PC_ERR_UNSUPPORTED);
        pc_buf_fini(&b);
    }
    d = doc();
    for (end = 0; end < d.len; end++) {
        uint8_t *c = malloc(end > 0 ? end : 1);
        memcpy(c, d.data, end);
        if (pc_af_stream_parse(c, end, &s, NULL) == PC_OK) {
            PC_T_FAIL("truncation at %zu parsed", end);
            pc_af_stream_fini(&s);
        }
        free(c);
    }
    pc_buf_fini(&d);
}

static void test_a_small_compressed_entry_cannot_make_the_stream_allocate_gigabytes(void)
{
    uint32_t counts[2];
    uint8_t codes[2] = {0x29, 0x01};
    size_t datas[2];
    int k;
    counts[0] = 1u << 27;
    datas[0] = (size_t)(1u << 27) / 8;
    counts[1] = (uint32_t)PC_AF_MAX_VALUES + 1;
    datas[1] = (size_t)counts[1];
    for (k = 0; k < 2; k++) {
        pc_buf d = PC_BUF_INIT;
        pc_buf file;
        pc_af_archive a;
        uint8_t *entry;
        size_t n;
        pc_af_stream s;
        pc_error e;
        uint8_t *zeros = calloc(datas[k], 1);
        sy_stream_begin(&d);
        sy_byte(&d, (uint8_t)(0x80 | codes[k]));
        sy_tag(&d, "Big_");
        sy_le(&d, counts[k], 4);
        sy_put(&d, zeros, datas[k]);
        free(zeros);
        sy_end(&d);
        file = container1(&d, SY_ZSTD);
        CHECK(file.len < d.len / 100);
        CHECK_EQ_INT(pc_af_archive_open(file.data, file.len, pc_af_limits_default(), &a, NULL), PC_OK);
        CHECK_EQ_INT(pc_af_archive_read(&a, "doc.dat", &entry, &n, NULL), PC_OK);
        CHECK(pc_af_stream_parse(entry, n, &s, &e) == PC_ERR_LIMIT);
        CHECK_EQ_STR(e.msg, "Affinity file exceeds a safety limit: too many values in the document stream");
        free(entry);
        pc_af_archive_fini(&a);
        pc_buf_fini(&file);
        pc_buf_fini(&d);
    }
}

static void test_mutated_containers_and_streams_never_panic(void)
{
    rng r = {512};
    pc_buf d = doc();
    int it;
    pc_buf files[3];
    int m;
    for (m = 0; m < 3; m++) {
        files[m] = container1(&d, (sy_method)m);
    }
    for (it = 0; it < 512; it++) {
        size_t nedits = 1 + (size_t)(rnext(&r) % 23);
        size_t ei[24];
        uint8_t ev[24];
        size_t k;
        uint8_t *sbuf;
        pc_af_stream s;
        for (k = 0; k < nedits; k++) {
            ei[k] = (size_t)(rnext(&r) % 4096);
            ev[k] = (uint8_t)rnext(&r);
        }
        for (m = 0; m < 3; m++) {
            uint8_t *f = malloc(files[m].len);
            pc_af_archive a;
            memcpy(f, files[m].data, files[m].len);
            for (k = 0; k < nedits; k++) {
                f[ei[k] % files[m].len] = ev[k];
            }
            if (pc_af_archive_open(f, files[m].len, pc_af_limits_default(), &a, NULL) == PC_OK) {
                uint8_t *out;
                size_t n;
                if (pc_af_archive_read(&a, "doc.dat", &out, &n, NULL) == PC_OK) {
                    if (pc_af_stream_parse(out, n, &s, NULL) == PC_OK) {
                        pc_af_stream_fini(&s);
                    }
                    free(out);
                }
                pc_af_archive_fini(&a);
            }
            free(f);
        }
        sbuf = malloc(d.len);
        memcpy(sbuf, d.data, d.len);
        for (k = 0; k < nedits; k++) {
            sbuf[ei[k] % d.len] = ev[k];
        }
        if (pc_af_stream_parse(sbuf, d.len, &s, NULL) == PC_OK) {
            pc_af_stream_fini(&s);
        }
        free(sbuf);
    }
    for (m = 0; m < 3; m++) {
        pc_buf_fini(&files[m]);
    }
    pc_buf_fini(&d);
}

/* ---- public documents (real_files.rs) ---- */

static char **docs;
static size_t ndocs;

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void walk(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (d == NULL) {
        return;
    }
    while ((e = readdir(d)) != NULL) {
        size_t len;
        char *full;
        struct stat st;
        const char *dot;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        len = strlen(dir) + strlen(e->d_name) + 2;
        full = malloc(len);
        if (full == NULL) {
            break;
        }
        (void)snprintf(full, len, "%s/%s", dir, e->d_name);
        dot = strrchr(e->d_name, '.');
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            walk(full);
            free(full);
        } else if (dot != NULL && (strcmp(dot, ".af") == 0 || strcmp(dot, ".afdesign") == 0 ||
                                   strcmp(dot, ".afphoto") == 0 || strcmp(dot, ".afpub") == 0 ||
                                   strcmp(dot, ".aftemplate") == 0)) {
            char **g = realloc(docs, (ndocs + 1) * sizeof *docs);
            if (g == NULL) {
                free(full);
                continue;
            }
            docs = g;
            docs[ndocs++] = full;
        } else {
            free(full);
        }
    }
    (void)closedir(d);
}

static size_t count_nodes(const pc_af_node *n, size_t k)
{
    size_t i;
    size_t c = 0;
    for (i = 0; i < k; i++) {
        c += 1 + count_nodes(n[i].children, n[i].nchildren);
    }
    return c;
}

static void test_public_documents_parse_and_map_to_the_model(void)
{
    struct stat st;
    size_t i;
    size_t parsed = 0;
    size_t nodes = 0;
    if (stat("corpus/affinity/SOURCES.md", &st) != 0) {
        (void)fprintf(stderr, "(corpus/affinity absent; skipped) ");
        return;
    }
    walk("corpus/affinity");
    qsort(docs, ndocs, sizeof *docs, cmp_str);
    CHECK(ndocs > 0);
    for (i = 0; i < ndocs; i++) {
        pc_buf b = PC_BUF_INIT;
        pc_af_archive a;
        pc_error e;
        if (pc_file_read(docs[i], SIZE_MAX, &b, &e) != PC_OK || !pc_af_is_affinity(b.data, b.len)) {
            pc_buf_fini(&b);
            continue;
        }
        if (pc_af_archive_open(b.data, b.len, pc_af_limits_default(), &a, &e) == PC_OK) {
            uint8_t *out;
            size_t n;
            if (pc_af_archive_read(&a, "doc.dat", &out, &n, &e) == PC_OK) {
                pc_af_stream s;
                if (pc_af_stream_parse(out, n, &s, &e) == PC_OK) {
                    CHECK(s.nobjects > 0);
                    parsed++;
                    pc_af_stream_fini(&s);
                } else {
                    PC_T_FAIL("%s: %s", docs[i], e.msg);
                }
                free(out);
            } else {
                PC_T_FAIL("%s: %s", docs[i], e.msg);
            }
            pc_af_archive_fini(&a);
        } else {
            PC_T_FAIL("%s: %s", docs[i], e.msg);
        }
        {
            pc_af_document d;
            if (pc_af_read(b.data, b.len, pc_af_limits_default(), &d, &e) == PC_OK) {
                size_t k;
                for (k = 0; k < d.nspreads; k++) {
                    nodes += count_nodes(d.spreads[k].nodes, d.spreads[k].nnodes);
                }
                pc_af_document_fini(&d);
            } else {
                PC_T_FAIL("%s: %s", docs[i], e.msg);
            }
        }
        pc_buf_fini(&b);
    }
    (void)fprintf(stderr, "(%zu Affinity documents parsed, %zu nodes) ", parsed, nodes);
    for (i = 0; i < ndocs; i++) {
        free(docs[i]);
    }
    free(docs);
}

int main(int argc, char **argv)
{
    RUN(test_indexed_preview_and_header);
    RUN(test_every_truncation_is_rejected);
    RUN(test_rejects_assets_versions_offsets_lengths_and_bombs);
    RUN(test_an_unindexed_png_or_resource_is_never_used);
    RUN(test_every_chunk_checksum_including_the_end_is_checked);
    RUN(test_compressed_metadata_and_animation_are_not_preview_support);
    RUN(test_critical_chunks_follow_a_single_complete_png_layout);
    RUN(test_arbitrary_input_and_mutations_never_panic);
    RUN(test_every_method_round_trips_with_checksums);
    RUN(test_the_stream_parses_into_shared_objects_and_links);
    RUN(test_checksums_sizes_and_budgets_are_enforced);
    RUN(test_a_declared_size_smaller_than_the_data_is_rejected_not_truncated);
    RUN(test_libraries_and_cyclic_tables_are_rejected);
    RUN(test_hostile_streams_fail_cleanly);
    RUN(test_a_small_compressed_entry_cannot_make_the_stream_allocate_gigabytes);
    RUN(test_mutated_containers_and_streams_never_panic);
    RUN(test_public_documents_parse_and_map_to_the_model);
    return pc_t_finish();
}
