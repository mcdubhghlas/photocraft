/* Ports of the crates/format tests upstream added after efa62efa (Phase 5R, R4): zip name index
 * and foreign entries, the progress-reporting atomic write, the orphan sweep and best-effort
 * cleanup of autosave, shared tiles on load, the canvas and pattern budgets, older bundles without
 * text metadata, the byte-source load and read_entry, directory-bundle symlink refusals, original
 * video frames, custom shape strokes, and smart-object contents identities (smart_contents.rs). */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/compress.h"
#include "base/json.h"
#include "common.h"
#include "format/bridge.h"
#include "harness.h"
#include "rich_doc.h"

/* ---- helpers ---- */

static void save(const pc_document *d, pc_buf *out)
{
    pc_buf_truncate(out, 0);
    CHECK(pc_format_save_to_bytes(d, NULL, out, NULL) == PC_OK);
}

/* The bundle with its manifest replaced by edit(manifest) (manifest first, as saves write it). */
typedef void (*edit_fn)(yyjson_mut_doc *m, void *ctx);

static void rewrite(const pc_buf *bundle, edit_fn edit, void *ctx, pc_buf *out)
{
    pc_zip_reader r;
    pc_zip_writer w;
    pc_buf m = PC_BUF_INIT;
    pc_buf j = PC_BUF_INIT;
    yyjson_doc *jd = NULL;
    yyjson_mut_doc *md;
    CHECK(pc_zip_reader_open(&r, bundle->data, bundle->len, NULL) == PC_OK);
    CHECK(pc_zip_read_by_name(&r, PC_FORMAT_MANIFEST, SIZE_MAX, &m, NULL) == PC_OK);
    CHECK(pc_json_parse_depth((const char *)m.data, m.len, 0, &jd, NULL) == PC_OK);
    md = yyjson_doc_mut_copy(jd, NULL);
    edit(md, ctx);
    CHECK(pc_json_write(yyjson_mut_doc_get_root(md), 0, &j, NULL) == PC_OK);
    pc_zip_writer_init(&w);
    CHECK(pc_zip_writer_add(&w, PC_FORMAT_MANIFEST, strlen(PC_FORMAT_MANIFEST), j.data, j.len, NULL) == PC_OK);
    for (size_t i = 0; i < r.count; i++) {
        pc_buf e = PC_BUF_INIT;
        if (pc_str_eq_c(&r.entries[i].name, PC_FORMAT_MANIFEST)) {
            continue;
        }
        CHECK(pc_zip_read(&r, &r.entries[i], SIZE_MAX, &e, NULL) == PC_OK);
        CHECK(pc_zip_writer_add(&w, pc_str_c(&r.entries[i].name), r.entries[i].name.len, e.data, e.len, NULL) ==
              PC_OK);
        pc_buf_fini(&e);
    }
    pc_buf_truncate(out, 0);
    CHECK(pc_zip_writer_finish(&w, out, NULL) == PC_OK);
    pc_zip_writer_fini(&w);
    yyjson_mut_doc_free(md);
    yyjson_doc_free(jd);
    pc_zip_reader_close(&r);
    pc_buf_fini(&m);
    pc_buf_fini(&j);
}

static yyjson_mut_val *at(yyjson_mut_doc *m, const char *ptr)
{
    yyjson_mut_val *v = yyjson_mut_doc_ptr_get(m, ptr);
    CHECK(v != NULL);
    return v;
}

static size_t count_tiles(const pc_surface *s, const pc_raster_tile **first, bool *all_same)
{
    size_t cursor = 0;
    size_t n = 0;
    pc_tile c;
    const pc_raster_tile *t;
    *all_same = true;
    *first = NULL;
    while (pc_surface_next_tile(s, &cursor, &c, &t)) {
        if (n++ == 0) {
            *first = t;
        } else if (t != *first) {
            *all_same = false;
        }
    }
    return n;
}

static pc_document solid_doc(uint32_t side, pc_sample_type depth)
{
    pc_document d;
    pc_layer l;
    float px[4] = {1.0f, 0.5f, 0.25f, 1.0f};
    pc_rect r = {0, 0, (int32_t)side, (int32_t)side};
    CHECK(pc_document_init(&d, "d", pc_size_make(side, side), PC_COLOR_MODE_RGB, depth, NULL) == PC_OK);
    CHECK(pc_layer_raster(&l, "solid", pc_document_pixel_format(&d), NULL) == PC_OK);
    CHECK(pc_surface_fill_rect(pc_layer_surface_mut(&l), r, px, 4, NULL) == PC_OK);
    t_push_layer(&d, &l);
    return d;
}

/* ---- zip.rs ---- */

static void test_zip_find_locates_every_entry_and_first_duplicate_wins(void)
{
    pc_zip_writer w;
    pc_buf b = PC_BUF_INIT;
    pc_buf got = PC_BUF_INIT;
    pc_zip_reader r;
    pc_zip_writer_init(&w);
    for (uint32_t i = 0; i < 3000; i++) {
        char name[32];
        uint8_t le[4] = {(uint8_t)i, (uint8_t)(i >> 8), 0, 0};
        int n = snprintf(name, sizeof name, "tiles/%05u.zst", (unsigned)i);
        CHECK(pc_zip_writer_add(&w, name, (size_t)n, le, 4, NULL) == PC_OK);
    }
    CHECK(pc_zip_writer_add(&w, "tiles/00007.zst", 15, (const uint8_t *)"dup!", 4, NULL) == PC_OK);
    CHECK(pc_zip_writer_finish(&w, &b, NULL) == PC_OK);
    CHECK(pc_zip_reader_open(&r, b.data, b.len, NULL) == PC_OK);
    CHECK_EQ_UINT(r.count, 3001);
    for (uint32_t i = 3000; i-- > 0;) {
        char name[32];
        int n = snprintf(name, sizeof name, "tiles/%05u.zst", (unsigned)i);
        const pc_zip_entry *e = pc_zip_find(&r, name, (size_t)n);
        CHECK(e != NULL && pc_str_eq_c(&e->name, name));
        CHECK(e != NULL && pc_zip_read(&r, e, 4, &got, NULL) == PC_OK && got.data[0] == (uint8_t)i);
    }
    CHECK(pc_zip_find(&r, "tiles/missing.zst", 17) == NULL);
    CHECK(pc_zip_find(&r, "", 0) == NULL);
    CHECK(pc_zip_find(&r, "tiles/00007.zst", 15) == &r.entries[7]);
    pc_zip_reader_close(&r);
    pc_zip_writer_fini(&w);
    pc_buf_fini(&b);
    pc_buf_fini(&got);
}

static void test_zip_entry_from_another_archive_is_an_error(void)
{
    pc_zip_writer w;
    pc_buf big = PC_BUF_INIT;
    pc_buf small = PC_BUF_INIT;
    pc_buf got = PC_BUF_INIT;
    uint8_t sevens[4096];
    pc_zip_reader rb;
    pc_zip_reader rs;
    pc_error e;
    memset(sevens, 7, sizeof sevens);
    pc_zip_writer_init(&w);
    CHECK(pc_zip_writer_add(&w, "a", 1, sevens, sizeof sevens, NULL) == PC_OK);
    CHECK(pc_zip_writer_finish(&w, &big, NULL) == PC_OK);
    CHECK(pc_zip_writer_add(&w, "a", 1, (const uint8_t *)"x", 1, NULL) == PC_OK);
    CHECK(pc_zip_writer_finish(&w, &small, NULL) == PC_OK);
    CHECK(pc_zip_reader_open(&rb, big.data, big.len, NULL) == PC_OK);
    CHECK(pc_zip_reader_open(&rs, small.data, small.len, NULL) == PC_OK);
    CHECK(pc_zip_read(&rs, &rb.entries[0], SIZE_MAX, &got, &e) == PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "corrupt .pcraft bundle: zip entry `a` out of range");
    pc_zip_reader_close(&rb);
    pc_zip_reader_close(&rs);
    pc_zip_writer_fini(&w);
    pc_buf_fini(&big);
    pc_buf_fini(&small);
    pc_buf_fini(&got);
}

/* A DEFLATE entry read with an unlimited budget (max + 1 must not wrap), and a forged 4 GiB size
 * rejected without reserving it. */
static void test_zip_deflated_entry_with_unbounded_limit(void)
{
    static const char CONTENT[] = "DEFLATE-compressed PhotoCraft project content, with a valid CRC.";
    size_t clen = sizeof CONTENT - 1;
    pc_buf z = PC_BUF_INIT;
    pc_buf b = PC_BUF_INIT;
    pc_buf got = PC_BUF_INIT;
    pc_zip_writer w;
    pc_zip_reader r;
    size_t eocd;
    size_t central;
    uint32_t crc = pc_zip_crc32((const uint8_t *)CONTENT, clen);
    CHECK(pc_deflate(PC_Z_RAW, 6, (const uint8_t *)CONTENT, clen, &z, NULL) == PC_OK);
    pc_zip_writer_init(&w);
    CHECK(pc_zip_writer_add(&w, "manifest.json", 13, z.data, z.len, NULL) == PC_OK);
    CHECK(pc_zip_writer_finish(&w, &b, NULL) == PC_OK);
    eocd = b.len - 22;
    central = (size_t)b.data[eocd + 16] | (size_t)b.data[eocd + 17] << 8 | (size_t)b.data[eocd + 18] << 16;
    for (int k = 0; k < 2; k++) {
        size_t base = k == 0 ? 0 : central;
        size_t mo = k == 0 ? 8 : 10;
        size_t co = k == 0 ? 14 : 16;
        size_t lo = k == 0 ? 22 : 24;
        b.data[base + mo] = 8;
        b.data[base + mo + 1] = 0;
        for (int i = 0; i < 4; i++) {
            b.data[base + co + (size_t)i] = (uint8_t)(crc >> (8 * i));
            b.data[base + lo + (size_t)i] = (uint8_t)(clen >> (8 * i));
        }
    }
    CHECK(pc_zip_reader_open(&r, b.data, b.len, NULL) == PC_OK);
    CHECK(pc_zip_read_by_name(&r, "manifest.json", SIZE_MAX, &got, NULL) == PC_OK);
    CHECK(got.len == clen && memcmp(got.data, CONTENT, clen) == 0);
    CHECK(pc_zip_read_by_name(&r, "manifest.json", clen, &got, NULL) == PC_OK);
    CHECK(pc_zip_read_by_name(&r, "manifest.json", clen - 1, &got, NULL) == PC_ERR_LIMIT);
    pc_zip_reader_close(&r);
    memset(b.data + 22, 0xff, 4);
    memset(b.data + central + 24, 0xff, 4);
    CHECK(pc_zip_reader_open(&r, b.data, b.len, NULL) == PC_OK);
    CHECK(pc_zip_read_by_name(&r, "manifest.json", SIZE_MAX, &got, NULL) == PC_ERR_FORMAT);
    pc_zip_reader_close(&r);
    pc_zip_writer_fini(&w);
    pc_buf_fini(&z);
    pc_buf_fini(&b);
    pc_buf_fini(&got);
}

/* ---- atomic.rs ---- */

typedef struct progress_log {
    float v[8];
    size_t n;
} progress_log;

static void on_progress(void *ctx, float f)
{
    progress_log *l = ctx;
    if (l->n < 8) {
        l->v[l->n] = f;
    }
    l->n++;
}

/* #3389: one report per 8 MiB chunk up to 1.0, and the same bytes as a plain write. */
static void test_atomic_write_reports_progress(void)
{
    char dir[512];
    char p[700];
    size_t len = ((size_t)8 << 20) * 2 + 123;
    uint8_t *bytes = malloc(len);
    pc_buf back = PC_BUF_INIT;
    progress_log log = {{0}, 0};
    CHECK(bytes != NULL && t_mkdtemp("progress", dir, sizeof dir) == 0);
    if (bytes == NULL) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        bytes[i] = (uint8_t)(i % 251);
    }
    (void)snprintf(p, sizeof p, "%s/big.psd", dir);
    CHECK(pc_atomic_write_progress(p, bytes, len, on_progress, &log, NULL) == PC_OK);
    CHECK_EQ_UINT(log.n, 3);
    CHECK(log.v[0] > 0.0f && log.v[0] < 0.6f && log.v[0] < log.v[1] && log.v[1] < log.v[2]);
    CHECK(log.v[2] == 1.0f);
    CHECK(t_read_file(p, &back) == 0 && back.len == len && memcmp(back.data, bytes, len) == 0);
    log.n = 0;
    CHECK(pc_atomic_write_progress(p, bytes, 0, on_progress, &log, NULL) == PC_OK);
    CHECK_EQ_UINT(log.n, 0);
    CHECK(pc_atomic_write_progress(p, bytes, 5, NULL, NULL, NULL) == PC_OK);
    t_rm_rf(dir);
    free(bytes);
    pc_buf_fini(&back);
}

/* ---- autosave.rs ---- */

static bool exists(const char *p)
{
    struct stat st;
    return lstat(p, &st) == 0;
}

static void test_sweep_removes_orphaned_bundles_and_foreign_temp_files_only(void)
{
    char dir[512];
    char p[1100];
    char own[1024];
    char foreign[2][1024];
    char orphan[1024];
    char other[1024];
    pc_autosaver *a = NULL;
    pc_document d;
    pc_recovery_entry *e = NULL;
    size_t n = 0;
    bool ok = false;
    CHECK(t_mkdtemp("sweep", dir, sizeof dir) == 0);
    CHECK(pc_document_init(&d, "Kept", pc_size_make(4, 4), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, NULL) == PC_OK);
    CHECK(pc_autosaver_new(dir, "kept", 4, &a, NULL) == PC_OK);
    CHECK(pc_autosaver_request(a, &d, 1, NULL, NULL, NULL) == PC_OK);
    CHECK(pc_autosaver_flush(a, &ok, NULL, NULL) && ok);
    pc_autosaver_free(a);
    /* A first autosave that crashed before its sidecar was written. */
    (void)snprintf(orphan, sizeof orphan, "%s/doc-crashed-1.pcraft", dir);
    (void)snprintf(p, sizeof p, "%s/tiles", orphan);
    CHECK(mkdir(orphan, 0755) == 0 && mkdir(p, 0755) == 0);
    (void)snprintf(p, sizeof p, "%s/tiles/abc.zst", orphan);
    CHECK(t_write_file(p, "partial", 7) == 0);
    /* Stale temp files: another process's (swept) and this process's (kept, maybe in flight). */
    (void)snprintf(foreign[0], sizeof foreign[0], "%s/.kept.json.4294967294-3.pcsave.tmp", dir);
    (void)snprintf(foreign[1], sizeof foreign[1], "%s/kept.pcraft/.manifest.json.4294967294-0.pcsave.tmp", dir);
    (void)snprintf(own, sizeof own, "%s/.kept.json.%ld-9.pcsave.tmp", dir, (long)getpid());
    (void)snprintf(other, sizeof other, "%s/notes.txt", dir);
    CHECK(t_write_file(foreign[0], "tmp", 3) == 0 && t_write_file(foreign[1], "tmp", 3) == 0);
    CHECK(t_write_file(own, "tmp", 3) == 0 && t_write_file(other, "user file", 9) == 0);
    /* Fresh data is never swept. */
    pc_recovery_sweep_orphans(dir, 3600);
    CHECK(exists(orphan) && exists(foreign[0]) && exists(foreign[1]));
    pc_recovery_sweep_orphans(dir, 0);
    CHECK(!exists(orphan));
    CHECK(!exists(foreign[0]) && !exists(foreign[1]));
    CHECK(exists(own) && exists(other));
    CHECK(pc_list_recovery(dir, &e, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 1);
    if (n == 1) {
        pc_document back;
        CHECK(pc_recover(&e[0], &back, NULL) == PC_OK);
        CHECK(pc_document_eq(&back, &d));
        pc_document_fini(&back);
    }
    pc_recovery_entries_free(e, n);
    pc_document_fini(&d);
    t_rm_rf(dir);
}

/* Once the manifest is committed the save has succeeded: an unremovable unreferenced object (a
 * folder squatting on a tile name) must not turn the directory save into a failure. */
static void test_failed_cleanup_after_commit_does_not_fail_the_save(void)
{
    char dir[512];
    char p[1024];
    pc_document d;
    pc_document back;
    pc_pcraft_writer *w = NULL;
    pc_format_save_stats st;
    CHECK(t_mkdtemp("cleanup", dir, sizeof dir) == 0);
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, &d);
    CHECK(pc_pcraft_writer_new(&w, NULL) == PC_OK);
    CHECK(pc_pcraft_writer_save_dir(w, &d, dir, NULL, NULL, NULL) == PC_OK);
    (void)snprintf(p, sizeof p, "%s/tiles/%064d.zst", dir, 0);
    CHECK(mkdir(p, 0755) == 0);
    (void)snprintf(p, sizeof p, "%s/tiles/%064d.zst/inner", dir, 0);
    CHECK(mkdir(p, 0755) == 0);
    CHECK(pc_pcraft_writer_save_dir(w, &d, dir, NULL, &st, NULL) == PC_OK);
    CHECK_EQ_UINT(st.objects_removed, 0);
    CHECK(pc_format_load_path(dir, &back, NULL) == PC_OK);
    CHECK(pc_document_eq(&back, &d));
    pc_document_fini(&back);
    pc_pcraft_writer_free(w);
    pc_document_fini(&d);
    t_rm_rf(dir);
}

/* ---- corruption.rs ---- */

static void drop_text(yyjson_mut_doc *m, void *ctx)
{
    (void)ctx;
    CHECK(yyjson_mut_obj_remove_key(at(m, "/document/metadata"), "text") != NULL);
}

static void test_older_bundles_without_text_metadata_still_load(void)
{
    pc_document d;
    pc_document back;
    pc_buf b = PC_BUF_INIT;
    pc_buf old = PC_BUF_INIT;
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, &d);
    d.metadata.text = calloc(1, sizeof *d.metadata.text);
    d.metadata.ntext = 1;
    t_str(&d.metadata.text[0].key, "Author");
    t_str(&d.metadata.text[0].value, "Synthetic \xc3\xa9");
    save(&d, &b);
    rewrite(&b, drop_text, NULL, &old);
    for (size_t i = 0; i < d.metadata.ntext; i++) {
        pc_str_fini(&d.metadata.text[i].key);
        pc_str_fini(&d.metadata.text[i].value);
    }
    free(d.metadata.text);
    d.metadata.text = NULL;
    d.metadata.ntext = 0;
    CHECK(pc_format_load_from_bytes(old.data, old.len, &back, NULL) == PC_OK);
    CHECK(pc_document_eq(&back, &d));
    pc_document_fini(&back);
    pc_document_fini(&d);
    pc_buf_fini(&b);
    pc_buf_fini(&old);
}

typedef struct many_ctx {
    int n;
} many_ctx;

static void many_refs(yyjson_mut_doc *m, void *ctx)
{
    yyjson_mut_val *tiles = at(m, "/document/layers/0/content/surface/tiles");
    yyjson_mut_val *hash = yyjson_mut_obj_get(yyjson_mut_arr_get(tiles, 0), "hash");
    const char *h = yyjson_mut_get_str(hash);
    many_ctx *c = ctx;
    yyjson_mut_arr_clear(tiles);
    for (int i = 0; i < c->n * c->n; i++) {
        yyjson_mut_val *t = yyjson_mut_obj(m);
        yyjson_mut_obj_add_int(m, t, "tx", i % c->n);
        yyjson_mut_obj_add_int(m, t, "ty", i / c->n);
        yyjson_mut_obj_add_strcpy(m, t, "hash", h);
        yyjson_mut_arr_append(tiles, t);
    }
}

/* #3315: one stored tile named at many coordinates loads once, shared, charged once. */
static void test_many_references_to_one_tile_load_within_the_budget(void)
{
    pc_document d = solid_doc(256, PC_SAMPLE_TYPE_U8);
    pc_document back;
    pc_buf b = PC_BUF_INIT;
    pc_buf hostile = PC_BUF_INIT;
    pc_format_load_opts o = pc_format_load_opts_default();
    many_ctx c = {64};
    const pc_raster_tile *first;
    bool same;
    save(&d, &b);
    rewrite(&b, many_refs, &c, &hostile);
    o.max_total_bytes = 256 * 256 * 4;
    CHECK(pc_format_load_from_bytes_with(hostile.data, hostile.len, &o, &back, NULL) == PC_OK);
    CHECK_EQ_UINT(count_tiles(pc_layer_surface(&back.layers[0]), &first, &same), 64 * 64);
    CHECK(same);
    pc_document_fini(&back);
    o.max_total_bytes -= 1;
    CHECK(pc_format_load_from_bytes_with(hostile.data, hostile.len, &o, &back, NULL) == PC_ERR_LIMIT);
    pc_document_fini(&d);
    pc_buf_fini(&b);
    pc_buf_fini(&hostile);
}

static void test_solid_fill_reopens_with_one_shared_tile(void)
{
    static const pc_sample_type DEPTHS[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    for (size_t k = 0; k < 3; k++) {
        pc_document d = solid_doc(2048, DEPTHS[k]);
        pc_document back;
        pc_buf b = PC_BUF_INIT;
        pc_format_load_opts o = pc_format_load_opts_default();
        const pc_raster_tile *first;
        bool same;
        save(&d, &b);
        o.max_total_bytes = (uint64_t)256 * 256 * pc_pixel_format_bytes_per_pixel(pc_document_pixel_format(&d));
        CHECK(pc_format_load_from_bytes_with(b.data, b.len, &o, &back, NULL) == PC_OK);
        CHECK(pc_document_eq(&back, &d));
        CHECK_EQ_UINT(count_tiles(pc_layer_surface(&back.layers[0]), &first, &same), 64);
        CHECK(same);
        pc_document_fini(&back);
        pc_document_fini(&d);
        pc_buf_fini(&b);
    }
}

static void test_an_unlimited_blob_budget_still_loads_blobs(void)
{
    pc_document d;
    pc_document back;
    pc_buf b = PC_BUF_INIT;
    pc_format_load_opts o = pc_format_load_opts_default();
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, &d);
    save(&d, &b);
    o.max_blob_bytes = SIZE_MAX;
    CHECK(pc_format_load_from_bytes_with(b.data, b.len, &o, &back, NULL) == PC_OK);
    CHECK(pc_document_eq(&back, &d));
    pc_document_fini(&back);
    pc_document_fini(&d);
    pc_buf_fini(&b);
}

typedef struct size_ctx {
    const char *ptr;
    uint64_t w;
    uint64_t h;
} size_ctx;

static void set_size(yyjson_mut_doc *m, void *ctx)
{
    size_ctx *c = ctx;
    yyjson_mut_val *o = at(m, c->ptr);
    yyjson_mut_obj_put(o, yyjson_mut_str(m, "width"), yyjson_mut_uint(m, c->w));
    yyjson_mut_obj_put(o, yyjson_mut_str(m, "height"), yyjson_mut_uint(m, c->h));
}

static pc_status load_resized(const pc_buf *b, const char *ptr, uint64_t w, uint64_t h,
                              const pc_format_load_opts *o, pc_document *out, pc_error *e)
{
    size_ctx c = {ptr, w, h};
    pc_buf x = PC_BUF_INIT;
    pc_status s;
    rewrite(b, set_size, &c, &x);
    s = pc_format_load_from_bytes_with(x.data, x.len, o, out, e);
    pc_buf_fini(&x);
    return s;
}

static void test_pattern_sizes_from_the_file_are_checked(void)
{
    pc_document d;
    pc_document back;
    pc_buf b = PC_BUF_INIT;
    pc_format_load_opts o = pc_format_load_opts_default();
    pc_error e;
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, &d);
    save(&d, &b);
    CHECK(load_resized(&b, "/document/patterns/0", 65535, 65535, &o, &back, &e) == PC_ERR_LIMIT);
    CHECK(load_resized(&b, "/document/patterns/0", 0, 24, &o, &back, &e) == PC_ERR_FORMAT);
    CHECK(load_resized(&b, "/document/patterns/0", 32, 24, &o, &back, &e) == PC_OK);
    pc_document_fini(&back);
    pc_document_fini(&d);
    pc_buf_fini(&b);
}

/* #2935: a declared canvas past the budget is refused before any work; the largest creatable
 * canvas loads without allocating for its area. */
static void test_declared_canvas_outside_the_budget_is_refused(void)
{
    static const uint64_t BAD[4][2] = {{300001, 1}, {1, 300001}, {300001, 300001}, {4294967295u, 4294967295u}};
    pc_document d;
    pc_document back;
    pc_buf b = PC_BUF_INIT;
    pc_format_load_opts o = pc_format_load_opts_default();
    pc_error e;
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, &d);
    save(&d, &b);
    for (size_t i = 0; i < 4; i++) {
        CHECK(load_resized(&b, "/document/size", BAD[i][0], BAD[i][1], &o, &back, &e) == PC_ERR_LIMIT);
    }
    CHECK(load_resized(&b, "/document/size", 0, 64, &o, &back, &e) != PC_OK);
    CHECK(load_resized(&b, "/document/size", 300000, 300000, &o, &back, &e) == PC_OK);
    CHECK(back.size.width == 300000 && back.size.height == 300000);
    CHECK_EQ_UINT(pc_surface_tile_count(pc_layer_surface(&back.layers[1])),
                  pc_surface_tile_count(pc_layer_surface(&d.layers[1])));
    pc_document_fini(&back);
    o.max_canvas_pixels = (uint64_t)1 << 20;
    CHECK(load_resized(&b, "/document/size", 300000, 300000, &o, &back, &e) == PC_ERR_LIMIT);
    o = pc_format_load_opts_default();
    o.max_canvas_side = 65536;
    CHECK(load_resized(&b, "/document/size", 65537, 1, &o, &back, &e) == PC_ERR_LIMIT);
    CHECK_EQ_STR(e.msg, "limit exceeded: canvas 65537x1 exceeds 65536 pixels per side");
    pc_document_fini(&d);
    pc_buf_fini(&b);
}

/* ---- roundtrip.rs ---- */

static void test_custom_shape_stroke_roundtrip_keeps_every_style_field(void)
{
    static const pc_color_mode MODES[4] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_GRAYSCALE, PC_COLOR_MODE_CMYK,
                                           PC_COLOR_MODE_LAB};
    static const pc_sample_type DEPTHS[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    static const float DASHES[6] = {0.0f, 2.0f, 4.0f, 1.0f, 1.5f, 3.0f};
    for (size_t m = 0; m < 4; m++) {
        for (size_t k = 0; k < 3; k++) {
            pc_document d;
            pc_document back;
            pc_layer_content c;
            pc_layer l;
            pc_buf b = PC_BUF_INIT;
            t_rich_doc(MODES[m], DEPTHS[k], &d);
            memset(&c, 0, sizeof c);
            c.kind = PC_LAYER_SHAPE;
            T_JSON(pc_path,
                   "{\"subpaths\":[{\"knots\":[{\"anchor\":{\"x\":10.0,\"y\":10.0},\"in_ctrl\":{\"x\":10.0,"
                   "\"y\":10.0},\"out_ctrl\":{\"x\":10.0,\"y\":10.0}},{\"anchor\":{\"x\":40.0,\"y\":10.0},"
                   "\"in_ctrl\":{\"x\":40.0,\"y\":10.0},\"out_ctrl\":{\"x\":40.0,\"y\":10.0}},{\"anchor\":{\"x\""
                   ":25.0,\"y\":40.0},\"in_ctrl\":{\"x\":25.0,\"y\":40.0},\"out_ctrl\":{\"x\":25.0,\"y\":40.0}}],"
                   "\"closed\":true}]}",
                   &c.u.shape.path);
            c.u.shape.has_stroke = true;
            pc_shape_stroke_default(&c.u.shape.stroke);
            c.u.shape.stroke.width = 5.5f;
            c.u.shape.stroke.opacity = 0.35f;
            c.u.shape.stroke.align = PC_STROKE_ALIGN_OUTSIDE;
            c.u.shape.stroke.cap = PC_LINE_CAP_SQUARE;
            c.u.shape.stroke.join = PC_LINE_JOIN_BEVEL;
            c.u.shape.stroke.miter_limit = 2.0f;
            c.u.shape.stroke.dashes = malloc(sizeof DASHES);
            memcpy(c.u.shape.stroke.dashes, DASHES, sizeof DASHES);
            c.u.shape.stroke.ndashes = 6;
            c.u.shape.stroke.dash_offset = -0.5f;
            t_layer(&l, "Custom stroke", &c);
            t_push_layer(&d, &l);
            save(&d, &b);
            CHECK(pc_format_load_from_bytes(b.data, b.len, &back, NULL) == PC_OK);
            CHECK(pc_document_eq(&back, &d));
            pc_document_fini(&back);
            pc_document_fini(&d);
            pc_buf_fini(&b);
        }
    }
}

/* A caller's byte source over a directory bundle (roundtrip.rs FsDir). */
static pc_status fs_get(void *ctx, const char *path, size_t max, pc_buf *out, pc_error *err)
{
    char p[1200];
    struct stat st;
    int fd;
    pc_status s;
    (void)snprintf(p, sizeof p, "%s/%s", (const char *)ctx, path);
    if (stat(p, &st) != 0) {
        return pc_format_corrupt(err, "missing `%s`", path);
    }
    if (!S_ISREG(st.st_mode)) {
        return pc_format_corrupt(err, "`%s` is not a regular file", path);
    }
    fd = open(p, O_RDONLY);
    if (fd < 0) {
        return pc_format_corrupt(err, "missing `%s`", path);
    }
    s = pc_format_read_entry(fd, (uint64_t)st.st_size, max, path, out, err);
    (void)close(fd);
    return s;
}

static void test_load_bytes_source_matches_the_path_load(void)
{
    char dir[512];
    char bundle[700];
    char missing[800];
    pc_document d;
    pc_document back;
    pc_pcraft_writer *w = NULL;
    pc_format_load_opts o = pc_format_load_opts_default();
    CHECK(t_mkdtemp("bytes-source", dir, sizeof dir) == 0);
    (void)snprintf(bundle, sizeof bundle, "%s/a.pcraft", dir);
    (void)snprintf(missing, sizeof missing, "%s/no-such-dir", bundle);
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, &d);
    CHECK(pc_pcraft_writer_new(&w, NULL) == PC_OK);
    CHECK(pc_pcraft_writer_save_dir(w, &d, bundle, NULL, NULL, NULL) == PC_OK);
    CHECK(pc_format_load_bytes_source(fs_get, bundle, &o, &back, NULL) == PC_OK);
    CHECK(pc_document_eq(&back, &d));
    pc_document_fini(&back);
    CHECK(pc_format_load_bytes_source(fs_get, missing, &o, &back, NULL) != PC_OK);
    pc_pcraft_writer_free(w);
    pc_document_fini(&d);
    t_rm_rf(dir);
}

/* read_entry bounds the read, not the stat hint: an endless stream fails instead of filling memory. */
static void test_read_entry_bounds_the_read_not_the_hint(void)
{
    pc_buf out = PC_BUF_INIT;
    pc_error e;
    int fd = open("/dev/zero", O_RDONLY);
    int p[2];
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(pc_format_read_entry(fd, 0, 100, "manifest.json", &out, &e) == PC_ERR_LIMIT);
        CHECK_CONTAINS(e.msg, "exceeds");
        (void)close(fd);
    }
    CHECK(pipe(p) == 0);
    CHECK(write(p[1], "hi", 2) == 2);
    (void)close(p[1]);
    CHECK(pc_format_read_entry(p[0], 2, 100, "manifest.json", &out, NULL) == PC_OK);
    CHECK(out.len == 2 && memcmp(out.data, "hi", 2) == 0);
    (void)close(p[0]);
    pc_buf_fini(&out);
}

static bool dir_has_manifest(const char *dir)
{
    char p[800];
    (void)snprintf(p, sizeof p, "%s/manifest.json", dir);
    return exists(p);
}

static void test_directory_save_refuses_symlinked_entries(void)
{
    char dir[512];
    char out[512];
    char p[800];
    char q[810];
    pc_buf b = PC_BUF_INIT;
    pc_document d;
    pc_document back;
    pc_pcraft_writer *w = NULL;
    pc_error e;
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, &d);
    CHECK(pc_pcraft_writer_new(&w, NULL) == PC_OK);

    /* A symlinked object folder: refused, the outside object untouched, no manifest. */
    CHECK(t_mkdtemp("symlink-object-dir", dir, sizeof dir) == 0);
    CHECK(t_mkdtemp("symlink-outside", out, sizeof out) == 0);
    (void)snprintf(q, sizeof q, "%s/unrelated.zst", out);
    CHECK(t_write_file(q, "keep this object", 16) == 0);
    (void)snprintf(p, sizeof p, "%s/tiles", dir);
    CHECK(symlink(out, p) == 0);
    CHECK(pc_pcraft_writer_save_dir(w, &d, dir, NULL, NULL, &e) == PC_ERR_IO);
    CHECK(pc_format_error_kind(PC_ERR_IO, &e) == PC_FORMAT_ERR_IO);
    CHECK_CONTAINS(e.msg, "refusing directory bundle save");
    CHECK(t_read_file(q, &b) == 0 && b.len == 16 && memcmp(b.data, "keep this object", 16) == 0);
    CHECK(!dir_has_manifest(dir));
    t_rm_rf(dir);

    /* A symlinked object file: refused, its target not overwritten, the link kept. */
    CHECK(t_mkdtemp("symlink-object", dir, sizeof dir) == 0);
    CHECK(pc_pcraft_writer_save_dir(w, &d, dir, NULL, NULL, NULL) == PC_OK);
    {
        pc_buf listing = PC_BUF_INIT;
        char first[200] = "";
        const char *c;
        (void)snprintf(p, sizeof p, "%s/tiles", dir);
        t_listing(p, &listing);
        c = strchr(pc_buf_cstr(&listing), ',');
        (void)snprintf(first, sizeof first, "%.*s", c != NULL ? (int)(c - pc_buf_cstr(&listing)) : 199,
                       pc_buf_cstr(&listing));
        (void)snprintf(p, sizeof p, "%s/tiles/%s", dir, first);
        pc_buf_fini(&listing);
    }
    CHECK(unlink(p) == 0);
    (void)snprintf(q, sizeof q, "%s/external.zst", out);
    CHECK(t_write_file(q, "do not overwrite", 16) == 0);
    CHECK(symlink(q, p) == 0);
    CHECK(pc_pcraft_writer_save_dir(w, &d, dir, NULL, NULL, NULL) != PC_OK);
    CHECK(t_read_file(q, &b) == 0 && b.len == 16 && memcmp(b.data, "do not overwrite", 16) == 0);
    {
        struct stat st;
        CHECK(lstat(p, &st) == 0 && S_ISLNK(st.st_mode));
    }
    t_rm_rf(dir);

    /* A symlinked manifest: refused before any object is written. */
    CHECK(t_mkdtemp("symlink-manifest", dir, sizeof dir) == 0);
    (void)snprintf(q, sizeof q, "%s/external.json", out);
    CHECK(t_write_file(q, "keep manifest", 13) == 0);
    (void)snprintf(p, sizeof p, "%s/manifest.json", dir);
    CHECK(symlink(q, p) == 0);
    CHECK(pc_pcraft_writer_save_dir(w, &d, dir, NULL, NULL, NULL) != PC_OK);
    CHECK(t_read_file(q, &b) == 0 && b.len == 13);
    {
        pc_buf listing = PC_BUF_INIT;
        (void)snprintf(p, sizeof p, "%s/tiles", dir);
        t_listing(p, &listing);
        CHECK_EQ_STR(pc_buf_cstr(&listing), "");
        pc_buf_fini(&listing);
    }
    t_rm_rf(dir);

    /* A chosen root may be a symlink (also with a trailing separator); a symlinked preview not. */
    CHECK(t_mkdtemp("symlink-root", dir, sizeof dir) == 0);
    (void)snprintf(p, sizeof p, "%s/linked.pcraft", dir);
    t_rm_rf(out);
    CHECK(mkdir(out, 0755) == 0);
    CHECK(symlink(out, p) == 0);
    CHECK(pc_pcraft_writer_save_dir(w, &d, p, NULL, NULL, NULL) == PC_OK);
    CHECK(pc_format_load_path(p, &back, NULL) == PC_OK && pc_document_eq(&back, &d));
    pc_document_fini(&back);
    (void)snprintf(q, sizeof q, "%s/", p);
    CHECK(pc_pcraft_writer_save_dir(w, &d, q, NULL, NULL, NULL) == PC_OK);
    CHECK(dir_has_manifest(out));
    t_rm_rf(dir);
    CHECK(t_mkdtemp("symlink-preview", dir, sizeof dir) == 0);
    (void)snprintf(q, sizeof q, "%s/preview.png", out);
    CHECK(t_write_file(q, "keep preview", 12) == 0);
    (void)snprintf(p, sizeof p, "%s/composite", dir);
    CHECK(mkdir(p, 0755) == 0);
    (void)snprintf(p, sizeof p, "%s/composite/preview.png", dir);
    CHECK(symlink(q, p) == 0);
    {
        pc_rgba8_image img = {2, 2, NULL};
        uint8_t px[16] = {0};
        pc_format_save_opts so = {NULL, &img};
        img.pixels = px;
        CHECK(pc_pcraft_writer_save_dir(w, &d, dir, &so, NULL, NULL) != PC_OK);
    }
    CHECK(t_read_file(q, &b) == 0 && b.len == 12 && memcmp(b.data, "keep preview", 12) == 0);
    CHECK(!dir_has_manifest(dir));
    t_rm_rf(dir);
    t_rm_rf(out);
    pc_pcraft_writer_free(w);
    pc_document_fini(&d);
    pc_buf_fini(&b);
}

static void test_video_original_frames_survive_roundtrip(void)
{
    pc_document d;
    pc_document back;
    pc_buf b = PC_BUF_INIT;
    pc_layer l;
    pc_pixel_format fmt;
    pc_surface *frames = calloc(3, sizeof *frames);
    pc_surface *orig = calloc(3, sizeof *orig);
    CHECK(pc_document_init(&d, "Vid", pc_size_make(8, 6), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, NULL) == PC_OK);
    fmt = pc_document_pixel_format(&d);
    CHECK(pc_layer_raster(&l, "Clip", fmt, NULL) == PC_OK);
    for (size_t i = 0; i < 3; i++) {
        float px[4] = {(float)i / 2.0f, 0.0f, 1.0f, 1.0f};
        pc_rect r = {0, 0, 8, 6};
        pc_surface_init(&frames[i], fmt);
        CHECK(pc_surface_fill_rect(&frames[i], r, px, 4, NULL) == PC_OK);
        CHECK(pc_surface_clone(&frames[i], &orig[i], NULL) == PC_OK);
    }
    pc_video_data_init(&l.video, frames, 3, 24.0f);
    l.video.has_original_frames = true;
    l.video.original_frames = orig;
    l.video.noriginal_frames = 3;
    l.has_video = true;
    t_push_layer(&d, &l);
    save(&d, &b);
    CHECK(pc_format_load_from_bytes(b.data, b.len, &back, NULL) == PC_OK);
    CHECK(pc_document_eq(&back, &d));
    {
        const pc_video_data *v = &back.layers[back.nlayers - 1].video;
        float a[4];
        float c[4];
        CHECK(v->has_original_frames && v->noriginal_frames == 3);
        pc_surface_rgba(&v->original_frames[2], 1, 1, a);
        pc_surface_rgba(&v->frames[2], 1, 1, c);
        CHECK(memcmp(a, c, sizeof a) == 0);
    }
    pc_document_fini(&back);
    pc_document_fini(&d);
    pc_buf_fini(&b);
}

/* ---- smart_contents.rs ---- */

static pc_layer smart_layer(const char *name, bool linked, const char *path, pc_blob *bytes)
{
    pc_layer_content c;
    pc_smart_source src;
    pc_layer l;
    memset(&src, 0, sizeof src);
    memset(&c, 0, sizeof c);
    src.linked = linked;
    if (linked) {
        t_str(&src.path, path);
    } else {
        t_str(&src.file_name, "source.png");
        src.bytes = pc_blob_retain(bytes);
    }
    c.kind = PC_LAYER_SMART;
    pc_smart_object_init(&c.u.smart, &src, PC_AFFINE_IDENTITY, NULL);
    t_layer(&l, name, &c);
    return l;
}

/* A group holding an embedded instance and its duplicate, then an independent equal one. */
static void contents_sample(pc_document *d)
{
    uint8_t raw[3] = {1, 2, 3};
    pc_blob *bytes = NULL;
    pc_layer kids[2];
    pc_layer g;
    pc_layer l;
    CHECK(pc_document_init(d, "contents identities", pc_size_make(1, 1), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8,
                           NULL) == PC_OK);
    CHECK(pc_blob_new(raw, 3, &bytes, NULL) == PC_OK);
    kids[0] = smart_layer("embedded", false, NULL, bytes);
    CHECK(pc_layer_duplicate(&kids[0], &kids[1], NULL) == PC_OK);
    CHECK(pc_layer_group(&g, "instances", NULL, 0, NULL) == PC_OK);
    g.content.u.group.children = malloc(2 * sizeof kids[0]);
    memcpy(g.content.u.group.children, kids, sizeof kids);
    g.content.u.group.nchildren = 2;
    t_push_layer(d, &g);
    l = smart_layer("embedded", false, NULL, bytes);
    t_push_layer(d, &l);
    pc_blob_release(bytes);
}

static size_t contents_ids(const pc_document *d, uint64_t *ids, size_t cap)
{
    pc_walk w;
    size_t n = 0;
    CHECK(pc_document_walk(d, &w, NULL) == PC_OK);
    for (size_t i = 0; i < w.len && n < cap; i++) {
        if (w.items[i].layer->content.kind == PC_LAYER_SMART) {
            ids[n++] = w.items[i].layer->content.u.smart.contents_id;
        }
    }
    pc_walk_fini(&w);
    return n;
}

static void remove_ids_in(yyjson_mut_val *layers)
{
    size_t i;
    size_t max;
    yyjson_mut_val *l;
    yyjson_mut_arr_foreach(layers, i, max, l)
    {
        yyjson_mut_val *c = yyjson_mut_obj_get(l, "content");
        yyjson_mut_val *kids = yyjson_mut_obj_get(c, "children");
        (void)yyjson_mut_obj_remove_key(c, "contents_id");
        if (kids != NULL) {
            remove_ids_in(kids);
        }
    }
}

static void drop_ids(yyjson_mut_doc *m, void *ctx)
{
    (void)ctx;
    remove_ids_in(at(m, "/document/layers"));
}

static void drop_ids_keep_one(yyjson_mut_doc *m, void *ctx)
{
    remove_ids_in(at(m, "/document/layers"));
    yyjson_mut_obj_put(at(m, "/document/layers/0/content/children/1/content"), yyjson_mut_str(m, "contents_id"),
                       yyjson_mut_uint(m, *(const uint64_t *)ctx));
}

static void extreme_ids(yyjson_mut_doc *m, void *ctx)
{
    (void)ctx;
    for (int i = 0; i < 2; i++) {
        char ptr[64];
        (void)snprintf(ptr, sizeof ptr, "/document/layers/0/content/children/%d/content", i);
        yyjson_mut_obj_put(at(m, ptr), yyjson_mut_str(m, "contents_id"), yyjson_mut_uint(m, UINT64_MAX));
    }
}

static void test_smart_contents_roundtrip_and_remapping(void)
{
    pc_document d;
    pc_document back;
    pc_buf b = PC_BUF_INIT;
    uint64_t ids[8];
    uint64_t orig[8];
    pc_format_load_opts o = pc_format_load_opts_default();
    contents_sample(&d);
    CHECK_EQ_UINT(contents_ids(&d, orig, 8), 3);
    CHECK(orig[0] == orig[1] && orig[0] != orig[2]);
    save(&d, &b);
    CHECK(pc_format_load_from_bytes(b.data, b.len, &back, NULL) == PC_OK);
    CHECK(pc_document_eq(&back, &d));
    pc_document_fini(&back);
    o.preserve_ids = false;
    CHECK(pc_format_load_from_bytes_with(b.data, b.len, &o, &back, NULL) == PC_OK);
    CHECK_EQ_UINT(contents_ids(&back, ids, 8), 3);
    CHECK(ids[0] == ids[1] && ids[0] != ids[2]);
    for (size_t i = 0; i < 3; i++) {
        CHECK(ids[i] != orig[0] && ids[i] != orig[2]);
    }
    CHECK(back.id != d.id && back.layers[0].id != d.layers[0].id);
    pc_document_fini(&back);
    pc_document_fini(&d);
    pc_buf_fini(&b);
}

static void test_legacy_smart_contents_share_only_nonempty_linked_paths(void)
{
    static const char *const PATHS[5] = {"/tmp/shared.psd", "/tmp/shared.psd", "/tmp/other.psd", "", ""};
    pc_document d;
    pc_document back;
    pc_document again;
    pc_buf b = PC_BUF_INIT;
    pc_buf legacy = PC_BUF_INIT;
    uint64_t ids[8];
    contents_sample(&d);
    for (size_t i = 0; i < 5; i++) {
        pc_layer l = smart_layer("linked", true, PATHS[i], NULL);
        t_push_layer(&d, &l);
    }
    save(&d, &b);
    rewrite(&b, drop_ids, NULL, &legacy);
    CHECK(pc_format_load_from_bytes(legacy.data, legacy.len, &back, NULL) == PC_OK);
    CHECK_EQ_UINT(contents_ids(&back, ids, 8), 8);
    CHECK(ids[0] != ids[1] && ids[0] != ids[2] && ids[1] != ids[2]);
    CHECK(ids[3] == ids[4] && ids[3] != ids[5]);
    CHECK(ids[6] != ids[7]);
    /* Minted identities become explicit on the next save. */
    save(&back, &b);
    CHECK(pc_format_load_from_bytes(b.data, b.len, &again, NULL) == PC_OK);
    CHECK(pc_document_eq(&again, &back));
    pc_document_fini(&again);
    pc_document_fini(&back);
    pc_document_fini(&d);
    pc_buf_fini(&b);
    pc_buf_fini(&legacy);
}

static void test_stored_contents_ids_are_reserved_before_minting(void)
{
    pc_document d;
    pc_document back;
    pc_buf b = PC_BUF_INIT;
    pc_buf mixed = PC_BUF_INIT;
    uint64_t ids[8];
    uint64_t stored;
    pc_layer l;
    contents_sample(&d);
    l = smart_layer("linked", true, "/tmp/source.psd", NULL);
    t_push_layer(&d, &l);
    save(&d, &b);
    stored = pc_layer_id_fresh() + 5000;
    rewrite(&b, drop_ids_keep_one, &stored, &mixed);
    CHECK(pc_format_load_from_bytes(mixed.data, mixed.len, &back, NULL) == PC_OK);
    CHECK_EQ_UINT(contents_ids(&back, ids, 8), 4);
    CHECK(ids[1] == stored);
    CHECK(ids[0] > stored && ids[2] > stored && ids[3] > stored);
    CHECK(pc_layer_id_fresh() > stored);
    pc_document_fini(&back);
    pc_document_fini(&d);
    pc_buf_fini(&b);
    pc_buf_fini(&mixed);
}

static void test_extreme_contents_ids_remap_without_poisoning_the_allocator(void)
{
    pc_document d;
    pc_document back;
    pc_buf b = PC_BUF_INIT;
    pc_buf damaged = PC_BUF_INIT;
    uint64_t ids[8];
    contents_sample(&d);
    save(&d, &b);
    rewrite(&b, extreme_ids, NULL, &damaged);
    CHECK(pc_format_load_from_bytes(damaged.data, damaged.len, &back, NULL) == PC_OK);
    CHECK_EQ_UINT(contents_ids(&back, ids, 8), 3);
    CHECK(ids[0] == ids[1] && ids[0] != ids[2]);
    CHECK(back.id != d.id && back.layers[0].id != d.layers[0].id);
    for (size_t i = 0; i < 3; i++) {
        CHECK(ids[i] < UINT64_MAX / 2);
    }
    CHECK(pc_smart_contents_id_fresh() < UINT64_MAX / 2 && pc_layer_id_fresh() < UINT64_MAX / 2);
    pc_document_fini(&back);
    pc_document_fini(&d);
    pc_buf_fini(&b);
    pc_buf_fini(&damaged);
}

int main(int argc, char **argv)
{
    RUN(test_zip_find_locates_every_entry_and_first_duplicate_wins);
    RUN(test_zip_entry_from_another_archive_is_an_error);
    RUN(test_zip_deflated_entry_with_unbounded_limit);
    RUN(test_atomic_write_reports_progress);
    RUN(test_sweep_removes_orphaned_bundles_and_foreign_temp_files_only);
    RUN(test_failed_cleanup_after_commit_does_not_fail_the_save);
    RUN(test_older_bundles_without_text_metadata_still_load);
    RUN(test_many_references_to_one_tile_load_within_the_budget);
    RUN(test_solid_fill_reopens_with_one_shared_tile);
    RUN(test_an_unlimited_blob_budget_still_loads_blobs);
    RUN(test_pattern_sizes_from_the_file_are_checked);
    RUN(test_declared_canvas_outside_the_budget_is_refused);
    RUN(test_custom_shape_stroke_roundtrip_keeps_every_style_field);
    RUN(test_load_bytes_source_matches_the_path_load);
    RUN(test_read_entry_bounds_the_read_not_the_hint);
    RUN(test_directory_save_refuses_symlinked_entries);
    RUN(test_video_original_frames_survive_roundtrip);
    RUN(test_smart_contents_roundtrip_and_remapping);
    RUN(test_legacy_smart_contents_share_only_nonempty_linked_paths);
    RUN(test_stored_contents_ids_are_reserved_before_minting);
    RUN(test_extreme_contents_ids_remap_without_poisoning_the_allocator);
    return pc_t_finish();
}
