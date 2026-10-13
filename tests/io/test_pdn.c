/* Paint.NET (.pdn, "PDN3") import runtime tests (port of crates/io/src/pdn/tests.rs, R10c): a
 * synthetic PDN3 document (NRBF object graph + gzip or raw BGRA pixel chunks) imports as a
 * read-only-source RGB8 document with one layer per BitmapLayer, the fourteen Paint.NET blend
 * modes, per-layer opacity/visibility and the swapped-to-RGBA pixels; truncated, mutated,
 * unknown-mode and corrupt-chunk files fail without a crash. The expected values are those
 * pdn/tests.rs asserts on the Rust side, so this is parity with the reference. The fixture writer
 * here is a byte-for-byte port of the Rust test's `Writer`/`fixture`. */
#include "harness.h"

#include "common.h"

#include "base/compress.h"
#include "format/format.h" /* .pcraft save/load round-trip */
#include "format/zip.h" /* pc_zip_crc32 (gzip CRC-32) */
#include "io/pdn.h"

/* The fourteen Paint.NET modes, in the order pdn.c's MODES / OLD_MODES tables use. */
static const pc_blend_mode MODES[14] = {
    PC_BLEND_NORMAL,     PC_BLEND_MULTIPLY,       PC_BLEND_LINEAR_DODGE,
    PC_BLEND_PAINT_NET_COLOR_BURN, PC_BLEND_PAINT_NET_COLOR_DODGE, PC_BLEND_REFLECT,
    PC_BLEND_GLOW,       PC_BLEND_OVERLAY,        PC_BLEND_DIFFERENCE,
    PC_BLEND_NEGATION,   PC_BLEND_LIGHTEN,        PC_BLEND_DARKEN,
    PC_BLEND_SCREEN,     PC_BLEND_XOR};
static const char *const OLD_MODES[14] = {"Normal",   "Multiply", "Additive", "ColorBurn", "ColorDodge",
                                          "Reflect",  "Glow",     "Overlay",  "Difference", "Negation",
                                          "Lighten",  "Darken",   "Screen",   "Xor"};

/* ---- NRBF writer (a byte-for-byte port of the Rust test's Writer) ---- */

typedef enum fkind { F_INT, F_LONG, F_BYTE, F_BOOL, F_REF } fkind;
typedef struct field {
    const char *name;
    fkind kind;
    int64_t value;
} field;

static void w_u8(pc_buf *b, uint8_t v) { must(pc_buf_append(b, &v, 1, NULL)); }

static void w_int(pc_buf *b, int32_t n)
{
    uint8_t le[4] = {(uint8_t)n, (uint8_t)((uint32_t)n >> 8), (uint8_t)((uint32_t)n >> 16),
                     (uint8_t)((uint32_t)n >> 24)};
    must(pc_buf_append(b, le, 4, NULL));
}

static void w_long(pc_buf *b, int64_t n)
{
    uint8_t le[8];
    int i;
    for (i = 0; i < 8; i++) {
        le[i] = (uint8_t)((uint64_t)n >> (8 * i));
    }
    must(pc_buf_append(b, le, 8, NULL));
}

static void w_be32(pc_buf *b, uint32_t n)
{
    uint8_t be[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
    must(pc_buf_append(b, be, 4, NULL));
}

static void w_string(pc_buf *b, const char *s)
{
    size_t len = strlen(s);
    size_t v = len;
    while (v >= 128) {
        w_u8(b, (uint8_t)((v & 127) | 128));
        v >>= 7;
    }
    w_u8(b, (uint8_t)v);
    must(pc_buf_append(b, s, len, NULL));
}

static void w_object(pc_buf *b, int32_t id, const char *cls, const field *fields, size_t nf)
{
    size_t i;
    w_u8(b, 5);
    w_int(b, id);
    w_string(b, cls);
    w_int(b, (int32_t)nf);
    for (i = 0; i < nf; i++) {
        w_string(b, fields[i].name);
    }
    for (i = 0; i < nf; i++) {
        w_u8(b, fields[i].kind == F_REF ? 2 : 0);
    }
    for (i = 0; i < nf; i++) {
        switch (fields[i].kind) {
        case F_INT: w_u8(b, 8); break;
        case F_LONG: w_u8(b, 9); break;
        case F_BYTE: w_u8(b, 2); break;
        case F_BOOL: w_u8(b, 1); break;
        case F_REF: break;
        }
    }
    w_int(b, 1);
    for (i = 0; i < nf; i++) {
        switch (fields[i].kind) {
        case F_INT: w_int(b, (int32_t)fields[i].value); break;
        case F_LONG: w_long(b, fields[i].value); break;
        case F_BYTE: w_u8(b, (uint8_t)fields[i].value); break;
        case F_BOOL: w_u8(b, (uint8_t)(fields[i].value ? 1 : 0)); break;
        case F_REF:
            w_u8(b, 9);
            w_int(b, (int32_t)fields[i].value);
            break;
        }
    }
}

/* gzip(raw): header + raw deflate + CRC-32 LE + ISIZE LE. The pdn decoder reads PC_Z_GZIP. */
static void w_gzip(pc_buf *b, const uint8_t *raw, size_t n)
{
    static const uint8_t head[10] = {0x1f, 0x8b, 0x08, 0, 0, 0, 0, 0, 0, 0xff};
    pc_buf def = PC_BUF_INIT;
    uint32_t crc = pc_zip_crc32(raw, n);
    must(pc_buf_append(b, head, sizeof head, NULL));
    must(pc_deflate(PC_Z_RAW, PC_Z_DEFAULT, raw, n, &def, NULL));
    must(pc_buf_append(b, def.data, def.len, NULL));
    w_u8(b, (uint8_t)crc);
    w_u8(b, (uint8_t)(crc >> 8));
    w_u8(b, (uint8_t)(crc >> 16));
    w_u8(b, (uint8_t)(crc >> 24));
    w_u8(b, (uint8_t)n);
    w_u8(b, (uint8_t)(n >> 8));
    w_u8(b, (uint8_t)(n >> 16));
    w_u8(b, (uint8_t)(n >> 24));
    pc_buf_fini(&def);
}

/* A synthetic PDN3 file (port of the Rust test's `fixture`). modes[i] is the blend index of layer
 * i; `legacy` writes the old blendOp class instead of a blendMode ref; `compressed` gzips the
 * pixel chunks. */
static pc_buf fixture(const int32_t *modes, size_t nmodes, bool legacy, bool compressed)
{
    pc_buf b = PC_BUF_INIT;
    size_t i;
    must(pc_buf_append(&b, "PDN3\0\0\0\0\x01", 9, NULL));
    w_u8(&b, 0);
    w_int(&b, 1);
    w_int(&b, -1);
    w_int(&b, 1);
    w_int(&b, 0);
    w_u8(&b, 12);
    w_int(&b, 1);
    w_string(&b, "PaintDotNet.Data");
    {
        field doc_f[3] = {{"width", F_INT, 2}, {"height", F_INT, 2}, {"layers", F_REF, 2}};
        w_object(&b, 1, "PaintDotNet.Document", doc_f, 3);
    }
    {
        field list_f[3] = {{"parent", F_REF, 1},
                           {"ArrayList+_size", F_INT, (int64_t)nmodes},
                           {"ArrayList+_items", F_REF, 3}};
        w_object(&b, 2, "PaintDotNet.LayerList", list_f, 3);
    }
    w_u8(&b, 16);
    w_int(&b, 3);
    w_int(&b, (int32_t)nmodes + 2);
    for (i = 0; i < nmodes; i++) {
        w_u8(&b, 9);
        w_int(&b, 16 + (int32_t)i * 10);
    }
    w_u8(&b, 13);
    w_u8(&b, 2);
    for (i = 0; i < nmodes; i++) {
        int32_t id = 16 + (int32_t)i * 10;
        char name[32];
        char cls[64];
        int32_t mode = modes[i];
        {
            field lf[5] = {{"Layer+width", F_INT, 2},    {"Layer+height", F_INT, 2},
                           {"surface", F_REF, id + 1},   {"Layer+properties", F_REF, id + 2},
                           {"properties", F_REF, id + 3}};
            w_object(&b, id, "PaintDotNet.BitmapLayer", lf, 5);
        }
        {
            field sf[4] = {{"width", F_INT, 2}, {"height", F_INT, 2}, {"stride", F_INT, 12},
                           {"scan0", F_REF, id + 7}};
            w_object(&b, id + 1, "PaintDotNet.Surface", sf, 4);
        }
        {
            field pf[5] = {{"name", F_REF, id + 4},
                           {"opacity", F_BYTE, i == 0 ? 255 : 128},
                           {"visible", F_BOOL, i != 1},
                           {"isBackground", F_BOOL, i == 0},
                           {"blendMode", F_REF, id + 5}};
            w_object(&b, id + 2, "PaintDotNet.Layer+LayerProperties", pf, legacy ? 4 : 5);
        }
        {
            field bf[1] = {{"blendOp", F_REF, id + 6}};
            w_object(&b, id + 3, "PaintDotNet.BitmapLayer+BitmapLayerProperties", bf, 1);
        }
        w_u8(&b, 6);
        w_int(&b, id + 4);
        (void)snprintf(name, sizeof name, "L\xc3\xa4yer %zu", i); /* "Läyer i" */
        w_string(&b, name);
        {
            field vf[1] = {{"value__", F_INT, mode}};
            w_object(&b, id + 5, "PaintDotNet.LayerBlendMode", vf, 1);
        }
        (void)snprintf(cls, sizeof cls, "PaintDotNet.UserBlendOps+%sBlendOp",
                       mode >= 0 && mode < 14 ? OLD_MODES[mode] : "Unknown");
        w_object(&b, id + 6, cls, NULL, 0);
        {
            field mf[3] = {{"length64", F_LONG, 24}, {"hasParent", F_BOOL, 0}, {"deferred", F_BOOL, 1}};
            w_object(&b, id + 7, "PaintDotNet.MemoryBlock", mf, 3);
        }
    }
    w_u8(&b, 11);
    for (i = 0; i < nmodes; i++) {
        int c;
        w_u8(&b, compressed ? 0 : 1);
        w_be32(&b, 12); /* chunk size */
        for (c = 0; c < 2; c++) {
            uint32_t index = c == 0 ? 1u : 0u;
            static const uint8_t row0[12] = {30, 20, 10, 255, 60, 50, 40, 128, 99, 99, 99, 99};
            static const uint8_t row1[12] = {90, 80, 70, 64, 120, 110, 100, 0, 99, 99, 99, 99};
            const uint8_t *raw = index == 0 ? row0 : row1;
            w_be32(&b, index);
            if (compressed) {
                pc_buf gz = PC_BUF_INIT;
                w_gzip(&gz, raw, 12);
                w_be32(&b, (uint32_t)gz.len);
                must(pc_buf_append(&b, gz.data, gz.len, NULL));
                pc_buf_fini(&gz);
            } else {
                w_be32(&b, 12);
                must(pc_buf_append(&b, raw, 12, NULL));
            }
        }
    }
    return b;
}

/* ---- tests ---- */

static pc_status try_import(const char *name, const uint8_t *b, size_t n, pc_io_import_result *out)
{
    pc_error e;
    pc_status st = pc_io_import(name, b, n, out, &e);
    if (st != PC_OK) {
        CHECK(e.msg[0] != '\0');
    }
    return st;
}

static void test_all_modes_and_layer_properties(void)
{
    int32_t modes[14];
    int i;
    int legacy;
    int compressed;
    for (i = 0; i < 14; i++) {
        modes[i] = i;
    }
    for (legacy = 0; legacy < 2; legacy++) {
        for (compressed = 0; compressed < 2; compressed++) {
            pc_buf bytes = fixture(modes, 14, legacy != 0, compressed != 0);
            pc_io_import_result r;
            /* Detection works without an extension too. */
            must(pc_io_import("sample", bytes.data, bytes.len, &r, NULL));
            CHECK(r.source_read_only);
            CHECK(!r.preview_only);
            CHECK_EQ_UINT(r.document.nlayers, 14);
            CHECK_EQ_UINT(r.document.size.width, 2);
            CHECK_EQ_UINT(r.document.size.height, 2);
            CHECK_EQ_INT(r.document.mode, PC_COLOR_MODE_RGB);
            for (i = 0; i < 14; i++) {
                const pc_layer *l = &r.document.layers[i];
                const pc_surface *s = pc_layer_surface(l);
                char name[32];
                float rgba[4];
                static const int px[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
                static const int expect[4][4] = {
                    {10, 20, 30, 255}, {40, 50, 60, 128}, {70, 80, 90, 64}, {100, 110, 120, 0}};
                int p;
                (void)snprintf(name, sizeof name, "L\xc3\xa4yer %d", i);
                CHECK(pc_str_eq_c(&l->name, name));
                CHECK_EQ_INT(l->blend, MODES[i]);
                CHECK_EQ_INT(l->visible, i != 1);
                CHECK_NEAR(l->opacity, i == 0 ? 1.0f : 128.0f / 255.0f, 1e-6f);
                CHECK(!l->locks.all && !l->locks.pixels && !l->locks.position);
                CHECK(s != NULL);
                for (p = 0; p < 4; p++) {
                    int ch;
                    pc_surface_rgba(s, px[p][0], px[p][1], rgba);
                    for (ch = 0; ch < 4; ch++) {
                        CHECK_EQ_INT((int)(rgba[ch] * 255.0f + 0.5f), expect[p][ch]);
                    }
                }
            }
            pc_io_import_result_fini(&r);
            pc_buf_fini(&bytes);
        }
    }
}

static void test_truncation_unknown_modes_and_corrupt_chunks(void)
{
    int32_t one[1] = {0};
    int32_t unknown[1] = {999};
    pc_buf raw = fixture(one, 1, false, false);
    pc_buf unk = fixture(unknown, 1, false, false);
    pc_buf corrupt = PC_BUF_INIT;
    pc_buf dup = PC_BUF_INIT;
    pc_buf zipped = fixture(one, 1, false, true);
    size_t len;
    size_t block;
    pc_io_import_result r;
    /* Every truncated prefix fails. */
    for (len = 0; len < raw.len; len++) {
        CHECK(try_import("truncated.pdn", raw.data, len, &r) != PC_OK);
    }
    /* An unknown blend index fails. */
    CHECK(try_import("unknown.pdn", unk.data, unk.len, &r) != PC_OK);
    /* Zero pixel chunk size (the byte at block+1..block+5 of the last-45 region). */
    block = raw.len - 45;
    must(pc_buf_append(&corrupt, raw.data, raw.len, NULL));
    memset(corrupt.data + block + 1, 0, 4);
    CHECK(try_import("corrupt.pdn", corrupt.data, corrupt.len, &r) != PC_OK);
    /* Both row chunks claim index 1. */
    must(pc_buf_append(&dup, raw.data, raw.len, NULL));
    dup.data[block + 25] = 0;
    dup.data[block + 26] = 0;
    dup.data[block + 27] = 0;
    dup.data[block + 28] = 1;
    CHECK(try_import("duplicate.pdn", dup.data, dup.len, &r) != PC_OK);
    /* A flipped gzip checksum byte fails. */
    zipped.data[zipped.len - 1] ^= 1u;
    CHECK(try_import("checksum.pdn", zipped.data, zipped.len, &r) != PC_OK);
    pc_buf_fini(&raw);
    pc_buf_fini(&unk);
    pc_buf_fini(&corrupt);
    pc_buf_fini(&dup);
    pc_buf_fini(&zipped);
}

static void test_malformed_pdn_reports_a_format_error(void)
{
    static const struct {
        const uint8_t *b;
        size_t n;
        bool is_pdn;
    } C[] = {
        {(const uint8_t *)"", 0, false},
        {(const uint8_t *)"PDN3", 4, true},
        {(const uint8_t *)"PDN3\xff\xff\xff", 7, true},
        {(const uint8_t *)"PDN4\0\0\0", 7, false},
    };
    size_t i;
    for (i = 0; i < sizeof C / sizeof C[0]; i++) {
        pc_io_import_result r;
        pc_error e;
        pc_status st = pc_io_import("bad.pdn", C[i].b, C[i].n, &r, &e);
        CHECK(st != PC_OK);
        if (C[i].is_pdn) {
            CHECK_EQ_INT(pc_io_error_kind(st, &e), PC_IO_ERR_PDN);
        }
        if (st == PC_OK) {
            pc_io_import_result_fini(&r);
        }
    }
}

/* nrbf bounds: oversized counts, deep nesting, duplicate ids, null member runs, a missing class
 * and a missing root all fail (not crash) through the public importer. */
static void test_serialized_counts_depth_and_references_bounded(void)
{
    static const char preamble[9] = {'P', 'D', 'N', '3', 0, 0, 0, 0, 1};
    size_t k;
    for (k = 0; k < 6; k++) {
        pc_buf b = PC_BUF_INIT;
        pc_io_import_result r;
        must(pc_buf_append(&b, preamble, sizeof preamble, NULL));
        w_u8(&b, 0); /* the header() writer starts with a 0 byte */
        w_int(&b, 1);
        w_int(&b, -1);
        w_int(&b, 1);
        w_int(&b, 0);
        switch (k) {
        case 0: /* oversized member count */
            w_u8(&b, 16);
            w_int(&b, 1);
            w_int(&b, 1000001);
            break;
        case 1: { /* 66 levels of nested arrays */
            int32_t id;
            for (id = 1; id <= 66; id++) {
                w_u8(&b, 16);
                w_int(&b, id);
                w_int(&b, 1);
            }
            w_u8(&b, 10);
            w_u8(&b, 11);
            break;
        }
        case 2: /* duplicate object id */
            w_u8(&b, 6);
            w_int(&b, 1);
            w_string(&b, "duplicate");
            w_u8(&b, 6);
            w_int(&b, 1);
            w_string(&b, "duplicate");
            w_u8(&b, 11);
            break;
        case 3: /* a null member run */
            w_u8(&b, 16);
            w_int(&b, 1);
            w_int(&b, 1);
            w_u8(&b, 13);
            w_u8(&b, 2);
            w_u8(&b, 11);
            break;
        case 4: /* a reference to a missing class/object */
            w_u8(&b, 1);
            w_int(&b, 1);
            w_int(&b, 999);
            break;
        default: /* a missing root */
            w_u8(&b, 11);
            break;
        }
        CHECK(try_import("bounds.pdn", b.data, b.len, &r) != PC_OK);
        pc_buf_fini(&b);
    }
}

static void test_arbitrary_and_mutated_never_crash(void)
{
    int32_t three[3] = {0, 5, 13};
    pc_buf base = fixture(three, 3, false, true);
    uint32_t state = 0xabcdef01u;
    int iter;
    /* Arbitrary bytes after a PDN3 signature. */
    for (iter = 0; iter < 400; iter++) {
        uint8_t buf[80];
        pc_io_import_result r;
        size_t n;
        size_t i;
        memcpy(buf, "PDN3\0\0\0\0\x01", 9);
        n = 9 + (size_t)(iter % 64);
        for (i = 9; i < n; i++) {
            state = state * 1664525u + 1013904223u;
            buf[i] = (uint8_t)(state >> 23);
        }
        if (try_import("fuzz.pdn", buf, n, &r) == PC_OK) {
            pc_io_import_result_fini(&r);
        }
    }
    /* Mutated copies of a valid layered document. */
    for (iter = 0; iter < 400; iter++) {
        pc_buf b = PC_BUF_INIT;
        pc_io_import_result r;
        int e;
        must(pc_buf_append(&b, base.data, base.len, NULL));
        for (e = 0; e < 4; e++) {
            size_t idx;
            state = state * 1664525u + 1013904223u;
            idx = (size_t)state % b.len;
            b.data[idx] = (uint8_t)(state >> 16);
        }
        if (try_import("mutated.pdn", b.data, b.len, &r) == PC_OK) {
            pc_io_import_result_fini(&r);
        }
        pc_buf_fini(&b);
    }
    pc_buf_fini(&base);
}

/* A PDN import uses the six Paint.NET-only blend modes; saving to .pcraft and reloading must
 * round-trip them. The manifest JSON serialised those modes as null before the blend enum table
 * listed all 34 modes, so the reload failed with "invalid type: null, expected string or map". */
static void test_pcraft_roundtrip_preserves_paintnet_blends(void)
{
    int32_t modes[14];
    pc_buf bytes;
    pc_io_import_result r;
    pc_buf saved = PC_BUF_INIT;
    pc_document back;
    pc_error e;
    size_t i;
    for (i = 0; i < 14; i++) {
        modes[i] = (int32_t)i;
    }
    bytes = fixture(modes, 14, false, false);
    must(pc_io_import("sample.pdn", bytes.data, bytes.len, &r, NULL));
    CHECK_EQ_UINT(r.document.nlayers, 14);
    must(pc_format_save_to_bytes(&r.document, NULL, &saved, &e));
    must(pc_format_load_from_bytes(saved.data, saved.len, &back, &e));
    CHECK_EQ_UINT(back.nlayers, r.document.nlayers);
    for (i = 0; i < back.nlayers && i < r.document.nlayers; i++) {
        CHECK_EQ_INT(back.layers[i].blend, r.document.layers[i].blend);
    }
    pc_document_fini(&back);
    pc_io_import_result_fini(&r);
    pc_buf_fini(&saved);
    pc_buf_fini(&bytes);
}

int main(int argc, char **argv)
{
    RUN(test_all_modes_and_layer_properties);
    RUN(test_pcraft_roundtrip_preserves_paintnet_blends);
    RUN(test_truncation_unknown_modes_and_corrupt_chunks);
    RUN(test_malformed_pdn_reports_a_format_error);
    RUN(test_serialized_counts_depth_and_references_bounded);
    RUN(test_arbitrary_and_mutated_never_crash);
    return pc_t_finish();
}
