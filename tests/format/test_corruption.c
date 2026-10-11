/* Damaged or hostile bundles must fail cleanly, never crash (crates/format/tests/corruption.rs).
 * The reference's proptest cases are deterministic pseudo-random sweeps here (same counts). */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "base/bytes.h"
#include "base/compress.h"
#include "base/utf8.h"
#include "common.h"
#include "format/bridge.h"
#include "harness.h"
#include "rich_doc.h"

static pc_document g_doc;
static pc_buf g_sample;

static const pc_buf *sample(void)
{
    if (g_sample.len == 0) {
        t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, &g_doc);
        CHECK(pc_format_save_to_bytes(&g_doc, NULL, &g_sample, NULL) == PC_OK);
    }
    return &g_sample;
}

/* The rustref/reference `rebuild`: f decides each entry's new payload (return false to drop). */
typedef bool (*rebuild_fn)(void *ctx, const char *name, pc_buf *data);

static void rebuild(const pc_buf *bytes, rebuild_fn f, void *ctx, pc_buf *out)
{
    pc_zip_reader r;
    pc_zip_writer w;
    pc_zip_writer_init(&w);
    CHECK(pc_zip_reader_open(&r, bytes->data, bytes->len, NULL) == PC_OK);
    for (size_t i = 0; i < r.count; i++) {
        pc_buf d = PC_BUF_INIT;
        CHECK(pc_zip_read(&r, &r.entries[i], (size_t)1 << 30, &d, NULL) == PC_OK);
        if (f(ctx, pc_str_c(&r.entries[i].name), &d)) {
            CHECK(pc_zip_writer_add(&w, pc_str_c(&r.entries[i].name), r.entries[i].name.len, d.data, d.len,
                                    NULL) == PC_OK);
        }
        pc_buf_fini(&d);
    }
    pc_zip_reader_close(&r);
    CHECK(pc_zip_writer_finish(&w, out, NULL) == PC_OK);
    pc_zip_writer_fini(&w);
}

static pc_format_err_kind load_kind(const pc_buf *b, pc_error *e)
{
    pc_document d;
    pc_status st = pc_format_load_from_bytes(b->data, b->len, &d, e);
    if (st == PC_OK) {
        pc_document_fini(&d);
    }
    return pc_format_error_kind(st, e);
}

static void test_truncation_never_panics_and_fails(void)
{
    const pc_buf *b = sample();
    size_t step = b->len / 200 > 0 ? b->len / 200 : 1;
    for (size_t cut = 0; cut < b->len; cut += step) {
        pc_document d;
        if (pc_format_load_from_bytes(b->data, cut, &d, NULL) == PC_OK) {
            PC_T_FAIL("cut at %zu loaded", cut);
            pc_document_fini(&d);
        }
    }
}

static void test_byte_flips_detected(void)
{
    const pc_buf *b = sample();
    t_rng rng = {9};
    pc_buf c = PC_BUF_INIT;
    int failures = 0;
    for (int k = 0; k < 300; k++) {
        size_t i = (size_t)(t_rng_next(&rng) % b->len);
        pc_document d;
        pc_buf_truncate(&c, 0);
        CHECK(pc_buf_append(&c, b->data, b->len, NULL) == PC_OK);
        c.data[i] ^= 0x20;
        if (pc_format_load_from_bytes(c.data, c.len, &d, NULL) != PC_OK) {
            failures++;
        } else {
            pc_document_fini(&d);
        }
    }
    /* Some flips hit unused header fields; most must be caught (CRC + hashes). */
    CHECK(failures > 200);
    pc_buf_fini(&c);
}

static bool drop_tiles(void *ctx, const char *name, pc_buf *d)
{
    (void)ctx;
    (void)d;
    return strncmp(name, "tiles/", 6) != 0;
}

static bool drop_manifest(void *ctx, const char *name, pc_buf *d)
{
    (void)ctx;
    (void)d;
    return strcmp(name, PC_FORMAT_MANIFEST) != 0;
}

static void test_missing_tile_and_manifest(void)
{
    pc_buf b = PC_BUF_INIT;
    pc_error e;
    rebuild(sample(), drop_tiles, NULL, &b);
    CHECK(load_kind(&b, &e) == PC_FORMAT_ERR_CORRUPT);
    pc_buf_truncate(&b, 0);
    rebuild(sample(), drop_manifest, NULL, &b);
    CHECK(load_kind(&b, &e) != PC_FORMAT_ERR_NONE);
    pc_buf_fini(&b);
}

typedef struct replace_ctx {
    const char *target;
    const pc_buf *with;
} replace_ctx;

static bool replace(void *ctx, const char *name, pc_buf *d)
{
    const replace_ctx *c = ctx;
    if (strcmp(name, c->target) == 0) {
        pc_buf_truncate(d, 0);
        CHECK(pc_buf_append(d, c->with->data, c->with->len, NULL) == PC_OK);
    }
    return true;
}

/* The first two tile entry names. */
static void two_tiles(const pc_buf *b, char first[96], char second[96])
{
    pc_zip_reader r;
    size_t k = 0;
    first[0] = second[0] = '\0';
    CHECK(pc_zip_reader_open(&r, b->data, b->len, NULL) == PC_OK);
    for (size_t i = 0; i < r.count && k < 2; i++) {
        if (strncmp(pc_str_c(&r.entries[i].name), "tiles/", 6) == 0) {
            (void)snprintf(k == 0 ? first : second, 96, "%s", pc_str_c(&r.entries[i].name));
            k++;
        }
    }
    pc_zip_reader_close(&r);
}

static void test_swapped_tile_content_fails_hash_check(void)
{
    const pc_buf *b = sample();
    char t0[96];
    char t1[96];
    pc_buf other = PC_BUF_INIT;
    pc_buf b2 = PC_BUF_INIT;
    pc_zip_reader r;
    pc_error e;
    replace_ctx c;
    two_tiles(b, t0, t1);
    CHECK(pc_zip_reader_open(&r, b->data, b->len, NULL) == PC_OK);
    CHECK(pc_zip_read_by_name(&r, t1, (size_t)1 << 30, &other, NULL) == PC_OK);
    pc_zip_reader_close(&r);
    c.target = t0;
    c.with = &other;
    rebuild(b, replace, &c, &b2);
    CHECK(load_kind(&b2, &e) != PC_FORMAT_ERR_NONE);
    CHECK(strstr(e.msg, "hash") != NULL);
    pc_buf_fini(&other);
    pc_buf_fini(&b2);
}

static void with_manifest(const pc_buf *m, pc_buf *out)
{
    replace_ctx c = {PC_FORMAT_MANIFEST, m};
    pc_buf_truncate(out, 0);
    rebuild(sample(), replace, &c, out);
}

static void test_bad_manifest_json(void)
{
    pc_buf m = PC_BUF_INIT;
    pc_buf b = PC_BUF_INIT;
    pc_error e;
    CHECK(pc_buf_append_str(&m, "{not json", NULL) == PC_OK);
    with_manifest(&m, &b);
    CHECK(load_kind(&b, &e) == PC_FORMAT_ERR_JSON);
    pc_buf_fini(&m);
    pc_buf_fini(&b);
}

/* Hostile nesting fails with an error instead of overflowing the stack; nesting as deep as the
 * deepest groups a bundle may hold parses, and is then rejected for lacking a format version. */
static void test_over_deep_manifest_fails_cleanly(void)
{
    pc_buf m = PC_BUF_INIT;
    pc_buf b = PC_BUF_INIT;
    pc_error e;
    size_t n = 100000;
    for (int pass = 0; pass < 2; pass++) {
        pc_buf_truncate(&m, 0);
        for (size_t i = 0; i < n; i++) {
            (void)pc_buf_append_byte(&m, '[', NULL);
        }
        for (size_t i = 0; i < n; i++) {
            (void)pc_buf_append_byte(&m, ']', NULL);
        }
        with_manifest(&m, &b);
        CHECK(load_kind(&b, &e) == (pass == 0 ? PC_FORMAT_ERR_LIMIT : PC_FORMAT_ERR_CORRUPT));
        n = 3 * PC_MAX_GROUP_DEPTH;
    }
    /* Brackets inside strings (escaped quotes included) don't count. */
    pc_buf_truncate(&m, 0);
    (void)pc_buf_append_str(&m, "{\"x\":\"", NULL);
    for (int i = 0; i < 100000; i++) {
        (void)pc_buf_append_byte(&m, '[', NULL);
    }
    (void)pc_buf_append_str(&m, "\\\"", NULL);
    for (int i = 0; i < 10; i++) {
        (void)pc_buf_append_byte(&m, '{', NULL);
    }
    (void)pc_buf_append_str(&m, "\"}", NULL);
    with_manifest(&m, &b);
    CHECK(load_kind(&b, &e) == PC_FORMAT_ERR_CORRUPT);
    pc_buf_fini(&m);
    pc_buf_fini(&b);
}

/* The sample's manifest with `from` (first occurrence) replaced by `to`. */
static void edited_manifest(const char *from, const char *to, pc_buf *out)
{
    pc_zip_reader r;
    pc_buf m = PC_BUF_INIT;
    const char *at;
    CHECK(pc_zip_reader_open(&r, sample()->data, sample()->len, NULL) == PC_OK);
    CHECK(pc_zip_read_by_name(&r, PC_FORMAT_MANIFEST, (size_t)1 << 30, &m, NULL) == PC_OK);
    pc_zip_reader_close(&r);
    at = strstr(pc_buf_cstr(&m), from);
    pc_buf_truncate(out, 0);
    if (at == NULL) {
        PC_T_FAIL("`%s` not in the manifest", from);
    } else {
        size_t off = (size_t)(at - pc_buf_cstr(&m));
        (void)pc_buf_append(out, m.data, off, NULL);
        (void)pc_buf_append_str(out, to, NULL);
        (void)pc_buf_append(out, m.data + off + strlen(from), m.len - off - strlen(from), NULL);
    }
    pc_buf_fini(&m);
}

static void test_too_new_version_rejected(void)
{
    pc_buf m = PC_BUF_INIT;
    pc_buf b = PC_BUF_INIT;
    pc_error e;
    edited_manifest("\"format_version\": 1", "\"format_version\": 999", &m);
    with_manifest(&m, &b);
    CHECK(load_kind(&b, &e) == PC_FORMAT_ERR_TOO_NEW);
    CHECK_EQ_STR(e.msg, "bundle written by a newer version (format 999, this build reads up to 1)");
    pc_buf_fini(&m);
    pc_buf_fini(&b);
}

static void test_invalid_hash_strings_rejected(void)
{
    pc_zip_reader r;
    pc_buf m = PC_BUF_INIT;
    pc_buf b = PC_BUF_INIT;
    pc_error e;
    const char *at;
    CHECK(pc_zip_reader_open(&r, sample()->data, sample()->len, NULL) == PC_OK);
    CHECK(pc_zip_read_by_name(&r, PC_FORMAT_MANIFEST, (size_t)1 << 30, &m, NULL) == PC_OK);
    pc_zip_reader_close(&r);
    at = strstr(pc_buf_cstr(&m), "\"hash\": \"");
    CHECK(at != NULL);
    if (at != NULL) {
        m.data[(size_t)(at - pc_buf_cstr(&m)) + 9] = '/';
    }
    with_manifest(&m, &b);
    CHECK(load_kind(&b, &e) != PC_FORMAT_ERR_NONE);
    pc_buf_fini(&m);
    pc_buf_fini(&b);
}

static void test_size_limits(void)
{
    const pc_buf *b = sample();
    pc_format_load_opts o = pc_format_load_opts_default();
    pc_document d;
    pc_error e;
    pc_status st;
    o.max_manifest_bytes = 100;
    st = pc_format_load_from_bytes_with(b->data, b->len, &o, &d, &e);
    CHECK(pc_format_error_kind(st, &e) == PC_FORMAT_ERR_LIMIT);
    o = pc_format_load_opts_default();
    o.max_total_bytes = 1000;
    st = pc_format_load_from_bytes_with(b->data, b->len, &o, &d, &e);
    CHECK(pc_format_error_kind(st, &e) == PC_FORMAT_ERR_LIMIT);
}

/* ---- directory bundles repair damaged objects ---- */

static bool first_file(const char *dir, char *out, size_t cap)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    bool found = false;
    while (d != NULL && !found && (e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.') {
            found = snprintf(out, cap, "%s/%s", dir, e->d_name) < (int)cap;
        }
    }
    if (d != NULL) {
        (void)closedir(d);
    }
    return found;
}

static bool load_dir_equal(const char *dir, const pc_document *want)
{
    pc_document back;
    bool eq;
    if (pc_format_load_path(dir, &back, NULL) != PC_OK) {
        return false;
    }
    eq = pc_document_eq(&back, want);
    pc_document_fini(&back);
    return eq;
}

static void test_directory_bundle_resave_repairs_tampered_objects(void)
{
    char tmp[512];
    pc_pcraft_writer *w = NULL;
    (void)sample();
    CHECK(t_mkdtemp("tamper", tmp, sizeof tmp) == 0);
    CHECK(pc_pcraft_writer_new(&w, NULL) == PC_OK);
    CHECK(pc_pcraft_writer_save_dir(w, &g_doc, tmp, NULL, NULL, NULL) == PC_OK);
    for (int kind = 0; kind < 2; kind++) {
        char sub[700];
        char object[1000];
        pc_buf original = PC_BUF_INIT;
        pc_buf decoded = PC_BUF_INIT;
        pc_buf mismatched = PC_BUF_INIT;
        (void)snprintf(sub, sizeof sub, "%s/%s", tmp, kind == 0 ? "tiles" : "blobs");
        CHECK(first_file(sub, object, sizeof object));
        CHECK(t_read_file(object, &original) == 0);
        CHECK(pc_zstd_decompress(original.data, original.len, (size_t)1 << 30, 0, &decoded, NULL, NULL) ==
              PC_OK);
        if (decoded.len > 0) {
            decoded.data[0] ^= 1;
        } else {
            (void)pc_buf_append_byte(&decoded, 1, NULL);
        }
        CHECK(pc_zstd_compress(1, decoded.data, decoded.len, &mismatched, NULL) == PC_OK);
        for (int dmg = 0; dmg < 2; dmg++) {
            pc_format_save_stats st;
            pc_document back;
            if (dmg == 0) {
                CHECK(t_write_file(object, mismatched.data, mismatched.len) == 0);
            } else {
                CHECK(t_write_file(object, "garbage", 7) == 0);
            }
            CHECK(pc_format_load_path(tmp, &back, NULL) != PC_OK);
            CHECK(pc_pcraft_writer_save_dir(w, &g_doc, tmp, NULL, &st, NULL) == PC_OK);
            CHECK_EQ_UINT(kind == 0 ? st.tiles_written : st.blobs_written, 1);
            CHECK(load_dir_equal(tmp, &g_doc));
        }
        pc_buf_fini(&original);
        pc_buf_fini(&decoded);
        pc_buf_fini(&mismatched);
    }
    pc_pcraft_writer_free(w);
    t_rm_rf(tmp);
}

/* Damages one tile in place without changing its length or modification time, the case a
 * size-and-mtime cache alone would trust. */
static void damage_in_place_keeping_size_and_mtime(const char *dir)
{
    char tiles[700];
    char object[1000];
    pc_buf original = PC_BUF_INIT;
    struct stat st0;
    (void)snprintf(tiles, sizeof tiles, "%s/tiles", dir);
    CHECK(first_file(tiles, object, sizeof object));
    CHECK(t_read_file(object, &original) == 0);
    CHECK(stat(object, &st0) == 0);
    for (size_t k = 0; k < original.len; k++) {
        size_t at = (original.len / 2 + k) % original.len;
        pc_buf damaged = PC_BUF_INIT;
        struct timespec times[2];
        struct stat st1;
        pc_document back;
        (void)pc_buf_append(&damaged, original.data, original.len, NULL);
        damaged.data[at] ^= 0x5a;
        CHECK(t_write_file(object, damaged.data, damaged.len) == 0);
        times[0] = T_ATIM(st0);
        times[1] = T_MTIM(st0);
        CHECK(utimensat(AT_FDCWD, object, times, 0) == 0);
        pc_buf_fini(&damaged);
        if (pc_format_load_path(dir, &back, NULL) != PC_OK) {
            CHECK(stat(object, &st1) == 0 && (size_t)st1.st_size == original.len);
            CHECK(T_MTIM(st1).tv_sec == T_MTIM(st0).tv_sec && T_MTIM(st1).tv_nsec == T_MTIM(st0).tv_nsec);
            pc_buf_fini(&original);
            return;
        }
        pc_document_fini(&back);
    }
    PC_T_FAIL("no single-byte change made the bundle fail to load");
    pc_buf_fini(&original);
}

static void test_same_size_same_mtime_tamper_is_repaired_on_the_next_save(void)
{
    char tmp[512];
    pc_pcraft_writer *w = NULL;
    pc_format_save_stats st;
    (void)sample();
    CHECK(t_mkdtemp("tamper-same", tmp, sizeof tmp) == 0);
    CHECK(pc_pcraft_writer_new(&w, NULL) == PC_OK);
    CHECK(pc_pcraft_writer_save_dir(w, &g_doc, tmp, NULL, NULL, NULL) == PC_OK);
    /* A second save trusts every object it verified in the first one. */
    CHECK(pc_pcraft_writer_save_dir(w, &g_doc, tmp, NULL, NULL, NULL) == PC_OK);
    damage_in_place_keeping_size_and_mtime(tmp);
    /* The sample is far below the default re-verify budget, so the rolling re-check reads every
     * cached object again (and the change time differs as well). */
    CHECK(pc_pcraft_writer_save_dir(w, &g_doc, tmp, NULL, &st, NULL) == PC_OK);
    CHECK_EQ_UINT(st.tiles_written, 1);
    CHECK(load_dir_equal(tmp, &g_doc));
    pc_pcraft_writer_free(w);
    t_rm_rf(tmp);
}

static void test_rolling_reverify_reaches_every_object_with_a_tiny_budget(void)
{
    char tmp[512];
    char sub[700];
    pc_pcraft_writer *w = NULL;
    pc_pcraft_writer *fresh = NULL;
    pc_format_save_stats st;
    size_t objects = 0;
    bool repaired = false;
    (void)sample();
    CHECK(t_mkdtemp("tamper-rolling", tmp, sizeof tmp) == 0);
    CHECK(pc_pcraft_writer_new(&w, NULL) == PC_OK);
    CHECK(pc_pcraft_writer_save_dir(w, &g_doc, tmp, NULL, NULL, NULL) == PC_OK);
    for (int k = 0; k < 2; k++) {
        DIR *d;
        struct dirent *e;
        (void)snprintf(sub, sizeof sub, "%s/%s", tmp, k == 0 ? "tiles" : "blobs");
        d = opendir(sub);
        while (d != NULL && (e = readdir(d)) != NULL) {
            objects += e->d_name[0] != '.';
        }
        if (d != NULL) {
            (void)closedir(d);
        }
    }
    /* One object per save: the oldest-checked one. */
    pc_pcraft_writer_set_reverify_budget(w, 1);
    damage_in_place_keeping_size_and_mtime(tmp);
    for (size_t i = 0; i <= objects && !repaired; i++) {
        CHECK(pc_pcraft_writer_save_dir(w, &g_doc, tmp, NULL, &st, NULL) == PC_OK);
        repaired = st.tiles_written == 1;
    }
    CHECK(repaired);
    CHECK(load_dir_equal(tmp, &g_doc));
    /* A zero budget re-reads nothing whose signature is unchanged; a fresh writer still verifies
     * everything on its first save. */
    CHECK(pc_pcraft_writer_new(&fresh, NULL) == PC_OK);
    pc_pcraft_writer_set_reverify_budget(fresh, 0);
    damage_in_place_keeping_size_and_mtime(tmp);
    CHECK(pc_pcraft_writer_save_dir(fresh, &g_doc, tmp, NULL, &st, NULL) == PC_OK);
    CHECK_EQ_UINT(st.tiles_written, 1);
    CHECK(load_dir_equal(tmp, &g_doc));
    pc_pcraft_writer_free(fresh);
    pc_pcraft_writer_free(w);
    t_rm_rf(tmp);
}

/* Re-zip with DEFLATE (as a user's zip tool might): it must still load. */
static void test_deflated_zip_entries_are_readable(void)
{
    const pc_buf *b = sample();
    pc_zip_reader r;
    pc_buf out = PC_BUF_INIT;
    pc_buf central = PC_BUF_INIT;
    pc_document a;
    pc_document back;
    uint8_t hdr[46];
    uint16_t n;
    CHECK(pc_zip_reader_open(&r, b->data, b->len, NULL) == PC_OK);
    for (size_t i = 0; i < r.count; i++) {
        pc_buf data = PC_BUF_INIT;
        pc_buf comp = PC_BUF_INIT;
        const char *name = pc_str_c(&r.entries[i].name);
        uint16_t nl = (uint16_t)r.entries[i].name.len;
        uint32_t crc;
        uint32_t off = (uint32_t)out.len;
        CHECK(pc_zip_read(&r, &r.entries[i], (size_t)1 << 30, &data, NULL) == PC_OK);
        CHECK(pc_deflate(PC_Z_RAW, PC_Z_DEFAULT, data.data, data.len, &comp, NULL) == PC_OK);
        crc = pc_zip_crc32(data.data, data.len);
        memset(hdr, 0, sizeof hdr);
        pc_store_le32(hdr, 0x04034b50u);
        hdr[4] = 20;
        hdr[8] = 8;
        hdr[12] = 0x21;
        pc_store_le32(hdr + 14, crc);
        pc_store_le32(hdr + 18, (uint32_t)comp.len);
        pc_store_le32(hdr + 22, (uint32_t)data.len);
        pc_store_le16(hdr + 26, nl);
        (void)pc_buf_append(&out, hdr, 30, NULL);
        (void)pc_buf_append(&out, name, nl, NULL);
        (void)pc_buf_append(&out, comp.data, comp.len, NULL);
        memset(hdr, 0, sizeof hdr);
        pc_store_le32(hdr, 0x02014b50u);
        hdr[4] = 20;
        hdr[6] = 20;
        hdr[10] = 8;
        hdr[14] = 0x21;
        pc_store_le32(hdr + 16, crc);
        pc_store_le32(hdr + 20, (uint32_t)comp.len);
        pc_store_le32(hdr + 24, (uint32_t)data.len);
        pc_store_le16(hdr + 28, nl);
        pc_store_le32(hdr + 42, off);
        (void)pc_buf_append(&central, hdr, 46, NULL);
        (void)pc_buf_append(&central, name, nl, NULL);
        pc_buf_fini(&data);
        pc_buf_fini(&comp);
    }
    n = (uint16_t)r.count;
    pc_zip_reader_close(&r);
    {
        uint32_t cd_off = (uint32_t)out.len;
        (void)pc_buf_append(&out, central.data, central.len, NULL);
        memset(hdr, 0, sizeof hdr);
        pc_store_le32(hdr, 0x06054b50u);
        pc_store_le16(hdr + 8, n);
        pc_store_le16(hdr + 10, n);
        pc_store_le32(hdr + 12, (uint32_t)central.len);
        pc_store_le32(hdr + 16, cd_off);
        (void)pc_buf_append(&out, hdr, 22, NULL);
    }
    CHECK(pc_format_load_from_bytes(b->data, b->len, &a, NULL) == PC_OK);
    CHECK(pc_format_load_from_bytes(out.data, out.len, &back, NULL) == PC_OK);
    CHECK(pc_document_eq(&a, &back));
    pc_document_fini(&a);
    pc_document_fini(&back);
    pc_buf_fini(&out);
    pc_buf_fini(&central);
}

/* ---- the reference's proptests, as deterministic sweeps (300 cases each) ---- */

static void test_random_bytes_never_panic(void)
{
    t_rng rng = {0x5eed};
    pc_buf data = PC_BUF_INIT;
    for (int k = 0; k < 300; k++) {
        size_t n = (size_t)(t_rng_next(&rng) % 4096);
        pc_document d;
        pc_format_manifest *m = NULL;
        pc_buf_truncate(&data, 0);
        for (size_t i = 0; i < n; i++) {
            (void)pc_buf_append_byte(&data, (uint8_t)t_rng_next(&rng), NULL);
        }
        /* A few cases start like a ZIP so the container code runs further. */
        if (k % 3 == 0 && n >= 4) {
            memcpy(data.data, "PK\x03\x04", 4);
        }
        if (pc_format_load_from_bytes(data.data, data.len, &d, NULL) == PC_OK) {
            pc_document_fini(&d);
        }
        if (pc_format_read_manifest(data.data, data.len, &m, NULL) == PC_OK) {
            pc_format_manifest_free(m);
        }
    }
    pc_buf_fini(&data);
}

static void test_random_manifest_never_panics(void)
{
    static const char ALPHABET[] = "{}[]\":,0123456789.-+eEtruefalsnl \\/abcxyz_";
    t_rng rng = {0xfeed};
    pc_buf json = PC_BUF_INIT;
    for (int k = 0; k < 300; k++) {
        size_t n = (size_t)(t_rng_next(&rng) % 301);
        pc_zip_writer w;
        pc_buf z = PC_BUF_INIT;
        pc_document d;
        pc_buf_truncate(&json, 0);
        for (size_t i = 0; i < n; i++) {
            uint64_t r = t_rng_next(&rng);
            if (r % 5 == 0) {
                /* any printable character, multi-byte UTF-8 included */
                uint32_t cp = (uint32_t)(0x20 + (r >> 8) % 0x2FFE0);
                if (cp >= 0xD800 && cp < 0xE000) {
                    cp = 0xFFFD;
                }
                (void)pc_utf8_put(&json, cp, NULL);
            } else {
                (void)pc_buf_append_byte(&json, (uint8_t)ALPHABET[(r >> 8) % (sizeof ALPHABET - 1)], NULL);
            }
        }
        pc_zip_writer_init(&w);
        CHECK(pc_zip_writer_add(&w, PC_FORMAT_MANIFEST, strlen(PC_FORMAT_MANIFEST), json.data, json.len,
                                NULL) == PC_OK);
        CHECK(pc_zip_writer_finish(&w, &z, NULL) == PC_OK);
        pc_zip_writer_fini(&w);
        if (pc_format_load_from_bytes(z.data, z.len, &d, NULL) == PC_OK) {
            pc_document_fini(&d);
        }
        pc_buf_fini(&z);
    }
    pc_buf_fini(&json);
}

static void test_random_tile_payload_never_panics(void)
{
    const pc_buf *b = sample();
    pc_zip_reader r;
    char tiles[64][96];
    size_t nt = 0;
    t_rng rng = {0xbeef};
    CHECK(pc_zip_reader_open(&r, b->data, b->len, NULL) == PC_OK);
    for (size_t i = 0; i < r.count && nt < 64; i++) {
        if (strncmp(pc_str_c(&r.entries[i].name), "tiles/", 6) == 0) {
            (void)snprintf(tiles[nt++], sizeof tiles[0], "%s", pc_str_c(&r.entries[i].name));
        }
    }
    pc_zip_reader_close(&r);
    CHECK(nt > 0);
    for (int k = 0; k < 300 && nt > 0; k++) {
        size_t n = (size_t)(t_rng_next(&rng) % 512);
        size_t which = (size_t)(t_rng_next(&rng) % 8);
        pc_buf data = PC_BUF_INIT;
        pc_buf b2 = PC_BUF_INIT;
        replace_ctx c;
        pc_document d;
        for (size_t i = 0; i < n; i++) {
            (void)pc_buf_append_byte(&data, (uint8_t)t_rng_next(&rng), NULL);
        }
        c.target = tiles[which % nt];
        c.with = &data;
        rebuild(b, replace, &c, &b2);
        if (pc_format_load_from_bytes(b2.data, b2.len, &d, NULL) == PC_OK) {
            PC_T_FAIL("a random tile payload loaded");
            pc_document_fini(&d);
        }
        pc_buf_fini(&data);
        pc_buf_fini(&b2);
    }
}

/* #938: a tile index whose rectangle overflows i32 must be refused at load. */
static void tile_at(int64_t tx, int64_t ty, pc_buf *out, size_t *layer)
{
    pc_zip_reader r;
    pc_buf m = PC_BUF_INIT;
    pc_buf text = PC_BUF_INIT;
    yyjson_doc *j = NULL;
    yyjson_mut_doc *mj = NULL;
    *layer = SIZE_MAX;
    CHECK(pc_zip_reader_open(&r, sample()->data, sample()->len, NULL) == PC_OK);
    CHECK(pc_zip_read_by_name(&r, PC_FORMAT_MANIFEST, (size_t)1 << 30, &m, NULL) == PC_OK);
    pc_zip_reader_close(&r);
    CHECK(pc_json_parse((const char *)m.data, m.len, &j, NULL) == PC_OK);
    mj = j != NULL ? yyjson_doc_mut_copy(j, NULL) : NULL;
    if (mj != NULL) {
        yyjson_mut_val *layers =
            yyjson_mut_obj_get(yyjson_mut_obj_get(yyjson_mut_doc_get_root(mj), "document"), "layers");
        for (size_t i = 0; i < yyjson_mut_arr_size(layers); i++) {
            yyjson_mut_val *c = yyjson_mut_obj_get(yyjson_mut_arr_get(layers, i), "content");
            yyjson_mut_val *t =
                yyjson_mut_arr_get(yyjson_mut_obj_get(yyjson_mut_obj_get(c, "surface"), "tiles"), 0);
            if (t != NULL) {
                CHECK(yyjson_mut_obj_put(t, yyjson_mut_str(mj, "tx"), yyjson_mut_sint(mj, tx)));
                CHECK(yyjson_mut_obj_put(t, yyjson_mut_str(mj, "ty"), yyjson_mut_sint(mj, ty)));
                *layer = i;
                break;
            }
        }
        CHECK(pc_json_write(yyjson_mut_doc_get_root(mj), 0, &text, NULL) == PC_OK);
    }
    with_manifest(&text, out);
    yyjson_mut_doc_free(mj);
    yyjson_doc_free(j);
    pc_buf_fini(&m);
    pc_buf_fini(&text);
}

static void test_tile_coordinates_outside_the_i32_pixel_range_are_rejected(void)
{
    const int64_t lo = INT32_MIN / 256;
    const int64_t hi = INT32_MAX / 256 - 1;
    const int64_t bad[6][2] = {{hi + 1, 0}, {0, hi + 1},    {lo - 1, 0},
                               {0, lo - 1}, {INT32_MAX, 0}, {0, INT32_MIN}};
    const int64_t good[3][2] = {{hi, hi}, {lo, lo}, {hi, lo}};
    for (int k = 0; k < 6; k++) {
        pc_buf b = PC_BUF_INIT;
        pc_error e;
        size_t layer;
        tile_at(bad[k][0], bad[k][1], &b, &layer);
        CHECK(load_kind(&b, &e) == PC_FORMAT_ERR_CORRUPT);
        CHECK(strstr(e.msg, "outside the coordinate range") != NULL);
        pc_buf_fini(&b);
    }
    /* The extreme valid indices still load, and the layer's bounds can be computed. */
    for (int k = 0; k < 3; k++) {
        pc_buf b = PC_BUF_INIT;
        pc_document d;
        size_t layer;
        tile_at(good[k][0], good[k][1], &b, &layer);
        if (pc_format_load_from_bytes(b.data, b.len, &d, NULL) == PC_OK && layer < d.nlayers) {
            const pc_surface *s = pc_layer_surface(&d.layers[layer]);
            pc_tile c = {(int32_t)good[k][0], (int32_t)good[k][1]};
            pc_rect r;
            CHECK(s != NULL && pc_surface_tile(s, c) != NULL);
            r = s != NULL ? pc_surface_content_bounds(s) : (pc_rect){0, 0, 0, 0};
            CHECK(r.x1 > r.x0 && r.y1 > r.y0);
            pc_document_fini(&d);
        } else {
            PC_T_FAIL("(%lld, %lld) did not load", (long long)good[k][0], (long long)good[k][1]);
        }
        pc_buf_fini(&b);
    }
}

int main(int argc, char **argv)
{
    RUN(test_truncation_never_panics_and_fails);
    RUN(test_byte_flips_detected);
    RUN(test_missing_tile_and_manifest);
    RUN(test_swapped_tile_content_fails_hash_check);
    RUN(test_bad_manifest_json);
    RUN(test_over_deep_manifest_fails_cleanly);
    RUN(test_too_new_version_rejected);
    RUN(test_invalid_hash_strings_rejected);
    RUN(test_size_limits);
    RUN(test_directory_bundle_resave_repairs_tampered_objects);
    RUN(test_same_size_same_mtime_tamper_is_repaired_on_the_next_save);
    RUN(test_rolling_reverify_reaches_every_object_with_a_tiny_budget);
    RUN(test_deflated_zip_entries_are_readable);
    RUN(test_random_bytes_never_panic);
    RUN(test_random_manifest_never_panics);
    RUN(test_random_tile_payload_never_panics);
    RUN(test_tile_coordinates_outside_the_i32_pixel_range_are_rejected);
    pc_document_fini(&g_doc);
    pc_buf_fini(&g_sample);
    return pc_t_finish();
}
