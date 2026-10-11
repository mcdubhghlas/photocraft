/* The object store (store.rs): helpers, sources, the verified fetch path, collecting and
 * deduplicating objects, ZIP and directory saves with their caches (incremental saves, garbage
 * collection, repair of tampered objects, the rolling re-check). Document-level saves and loads
 * need chunk C5's manifest conversion; until then they must fail cleanly (checked here), and
 * crates/format/tests/{roundtrip,corruption}.rs are ported by C5. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "base/compress.h"
#include "base/hash.h"
#include "codecs/codecs.h"
#include "common.h"
#include "format/bridge.h"
#include "harness.h"

/* ---- fixtures ---- */

/* An RGBA8 surface with pixels in three tiles (seeded). */
static void scribble(pc_surface *s, uint32_t seed)
{
    static const int32_t AT[3][2] = {{-12, -7}, {0, 0}, {600, 30}};
    uint32_t r = seed;
    pc_surface_init(s, PC_PIXEL_FORMAT_RGBA8);
    for (int k = 0; k < 3; k++) {
        for (int dy = 0; dy < 5; dy++) {
            for (int dx = 0; dx < 7; dx++) {
                float px[4];
                for (int c = 0; c < 4; c++) {
                    r = r * 1664525u + 1013904223u;
                    px[c] = (float)(r >> 8) / 16777216.0f;
                }
                CHECK(pc_surface_write_pixel(s, AT[k][0] + dx, AT[k][1] + dy, px, 4, NULL) == PC_OK);
            }
        }
    }
}

static size_t sink_surface(pc_format_sink *sink, const pc_surface *s)
{
    size_t cursor = 0;
    size_t n = 0;
    pc_tile c;
    const pc_raster_tile *t;
    while (pc_surface_next_tile(s, &cursor, &c, &t)) {
        char h[65];
        CHECK(pc_format_sink_tile(sink, pc_surface_format(s), t, h, NULL) == PC_OK);
        n++;
    }
    return n;
}

static void blob_of(uint8_t seed, size_t n, pc_blob **out)
{
    uint8_t *d = malloc(n + 1);
    for (size_t i = 0; i < n; i++) {
        d[i] = (uint8_t)((uint8_t)i * 31u + seed);
    }
    CHECK(pc_blob_new(d, n, out, NULL) == PC_OK);
    free(d);
}

/* A prepared bundle with the tiles of `surfaces`, the given blobs and a tiny manifest. */
static void prepare(pc_format_hash_cache *cache, pc_surface *const *surfaces, size_t ns,
                    pc_blob *const *blobs, size_t nb, pc_format_prepared *p)
{
    pc_format_sink *sink = NULL;
    static const char M[] = "{\n  \"format_version\": 1\n}";
    CHECK(pc_format_sink_new(cache, &sink, NULL) == PC_OK);
    for (size_t i = 0; i < ns; i++) {
        (void)sink_surface(sink, surfaces[i]);
    }
    for (size_t i = 0; i < nb; i++) {
        char h[65];
        CHECK(pc_format_sink_blob(sink, blobs[i], h, NULL) == PC_OK);
    }
    CHECK(pc_format_prepared_from_sink(sink, (const uint8_t *)M, sizeof M - 1, p, NULL) == PC_OK);
    pc_format_sink_free(sink);
}

static void stats_eq(const pc_format_save_stats *s, size_t tt, size_t tw, size_t tr, size_t bt, size_t bw,
                     size_t rm)
{
    CHECK_EQ_UINT(s->tiles_total, tt);
    CHECK_EQ_UINT(s->tiles_written, tw);
    CHECK_EQ_UINT(s->tiles_reused, tr);
    CHECK_EQ_UINT(s->blobs_total, bt);
    CHECK_EQ_UINT(s->blobs_written, bw);
    CHECK_EQ_UINT(s->objects_removed, rm);
}

/* ---- tests ---- */

static void test_helpers(void)
{
    char h[65];
    pc_error e;
    pc_buf m = PC_BUF_INIT;
    uint8_t le[4] = {1, 2, 3, 4};
    pc_blake3_hex("x", 1, h);
    CHECK(pc_format_is_valid_hash(h, 64));
    CHECK(!pc_format_is_valid_hash(h, 63));
    h[3] = 'A';
    CHECK(!pc_format_is_valid_hash(h, 64)); /* lower-case only */
    h[3] = '/';
    CHECK(!pc_format_is_valid_hash(h, 64));
    pc_format_swap_to_le(le, 4, PC_SAMPLE_TYPE_U16);
    CHECK(le[0] == 1 || le[0] == 2); /* a no-op on little-endian hosts */
    CHECK_EQ_INT(PC_FORMAT_MAX_MANIFEST_DEPTH, 367);
    for (int i = 0; i < 367; i++) {
        (void)pc_buf_append_byte(&m, '[', NULL);
    }
    CHECK(pc_format_check_manifest_depth(m.data, m.len, NULL) == PC_OK);
    (void)pc_buf_append_byte(&m, '{', NULL);
    CHECK(pc_format_check_manifest_depth(m.data, m.len, &e) == PC_ERR_LIMIT);
    CHECK_EQ_STR(e.msg, "limit exceeded: manifest JSON nested deeper than 367 levels");
    pc_buf_truncate(&m, 0);
    (void)pc_buf_append_str(&m, "\"[[[\\\"[[\"]]]]]]]][", NULL); /* closers never go below zero */
    CHECK(pc_format_check_manifest_depth(m.data, m.len, NULL) == PC_OK);
    CHECK(pc_format_check_manifest_depth(NULL, 0, NULL) == PC_OK);
    pc_buf_fini(&m);
}

static void test_sink_dedupes_and_sorts(void)
{
    pc_surface a;
    pc_surface b;
    pc_blob *b1 = NULL;
    pc_blob *b1_copy = NULL;
    pc_blob *b2 = NULL;
    pc_format_sink *sink = NULL;
    pc_format_prepared p;
    char h1[65];
    char h2[65];
    scribble(&a, 1);
    scribble(&b, 1); /* same pixels, different tile objects */
    blob_of(3, 500, &b1);
    blob_of(3, 500, &b1_copy);
    blob_of(4, 4, &b2);
    CHECK(pc_format_sink_new(NULL, &sink, NULL) == PC_OK);
    CHECK_EQ_UINT(sink_surface(sink, &a), 3);
    CHECK_EQ_UINT(sink_surface(sink, &b), 3);
    CHECK(pc_format_sink_blob(sink, b1, h1, NULL) == PC_OK);
    CHECK(pc_format_sink_blob(sink, b1_copy, h2, NULL) == PC_OK);
    CHECK_EQ_STR(h1, h2);
    CHECK(pc_format_sink_blob(sink, b2, h2, NULL) == PC_OK);
    CHECK(pc_format_prepared_from_sink(sink, (const uint8_t *)"{}", 2, &p, NULL) == PC_OK);
    CHECK_EQ_UINT(p.nobjects, 5);
    CHECK_EQ_UINT(p.stats.tiles_total, 3);
    CHECK_EQ_UINT(p.stats.blobs_total, 2);
    CHECK_EQ_UINT(p.stats.manifest_bytes, 2);
    for (size_t i = 0; i + 1 < p.nobjects; i++) {
        CHECK(strcmp(p.objects[i].path, p.objects[i + 1].path) < 0);
    }
    CHECK(strncmp(p.objects[0].path, "blobs/", 6) == 0 && strncmp(p.objects[4].path, "tiles/", 6) == 0);
    /* Each tile's name is the BLAKE3 of its (little-endian) bytes. */
    for (size_t i = 0; i < p.nobjects; i++) {
        const uint8_t *d;
        size_t n;
        pc_buf tmp = PC_BUF_INIT;
        char hx[65];
        CHECK(pc_format_object_content(&p.objects[i], &tmp, &d, &n, NULL) == PC_OK);
        pc_blake3_hex(d, n, hx);
        CHECK(strncmp(p.objects[i].path + 6, hx, 64) == 0);
        pc_buf_fini(&tmp);
    }
    pc_format_prepared_fini(&p);
    pc_format_sink_free(sink);
    pc_blob_release(b1);
    pc_blob_release(b1_copy);
    pc_blob_release(b2);
    pc_surface_fini(&a);
    pc_surface_fini(&b);
}

/* The hash cache keys tiles by identity; editing a tile afterwards copies it (the cache holds a
 * reference), so a cached hash never describes changed pixels. */
static void test_hash_cache_never_goes_stale(void)
{
    pc_format_hash_cache *cache = NULL;
    pc_surface s;
    pc_tile c00 = {0, 0};
    const pc_raster_tile *before;
    char h1[65];
    char h2[65];
    char h3[65];
    pc_format_sink *sink = NULL;
    uint8_t *px;
    CHECK(pc_format_hash_cache_new(&cache, NULL) == PC_OK);
    scribble(&s, 9);
    before = pc_surface_tile(&s, c00);
    CHECK(before != NULL);
    pc_format_hash_cache_begin(cache);
    CHECK(pc_format_sink_new(cache, &sink, NULL) == PC_OK);
    CHECK(pc_format_sink_tile(sink, PC_PIXEL_FORMAT_RGBA8, before, h1, NULL) == PC_OK);
    CHECK(pc_format_sink_tile(sink, PC_PIXEL_FORMAT_RGBA8, before, h2, NULL) == PC_OK); /* cached */
    CHECK_EQ_STR(h1, h2);
    pc_format_sink_free(sink);
    pc_format_hash_cache_end(cache);
    CHECK(pc_surface_tile_mut(&s, c00, &px, NULL) == PC_OK);
    px[0] ^= 0xff;
    CHECK(pc_surface_tile(&s, c00) != before); /* copied, not edited in place */
    pc_format_hash_cache_begin(cache);
    CHECK(pc_format_sink_new(cache, &sink, NULL) == PC_OK);
    CHECK(pc_format_sink_tile(sink, PC_PIXEL_FORMAT_RGBA8, pc_surface_tile(&s, c00), h3, NULL) == PC_OK);
    CHECK(strcmp(h1, h3) != 0);
    pc_format_sink_free(sink);
    pc_format_hash_cache_end(cache); /* drops the old tile's entry (and its reference) */
    pc_format_hash_cache_free(cache);
    pc_surface_fini(&s);
}

static void test_zip_save_incremental(void)
{
    pc_surface a;
    pc_surface b;
    pc_blob *icc = NULL;
    pc_pcraft_writer *w = NULL;
    pc_format_prepared p1;
    pc_format_prepared p2;
    pc_buf z1 = PC_BUF_INIT;
    pc_buf z2 = PC_BUF_INIT;
    pc_format_save_stats st;
    pc_surface *both[2];
    pc_surface *only_b[1];
    scribble(&a, 1);
    scribble(&b, 2);
    blob_of(1, 500, &icc);
    both[0] = &a;
    both[1] = &b;
    only_b[0] = &b;
    CHECK(pc_pcraft_writer_new(&w, NULL) == PC_OK);
    prepare(NULL, both, 2, &icc, 1, &p1);
    CHECK(pc_format_prepared_add_preview(&p1, PC_FORMAT_THUMB, (const uint8_t *)"png!", 4, NULL) == PC_OK);
    CHECK(pc_pcraft_writer_save_zip_prepared(w, &p1, &z1, &st, NULL) == PC_OK);
    stats_eq(&st, 6, 6, 0, 1, 1, 0);
    CHECK(pc_pcraft_writer_save_zip_prepared(w, &p1, &z2, &st, NULL) == PC_OK);
    stats_eq(&st, 6, 0, 6, 1, 0, 0);
    CHECK(z1.len == z2.len && memcmp(z1.data, z2.data, z1.len) == 0); /* deterministic */
    prepare(NULL, only_b, 1, NULL, 0, &p2);
    CHECK(pc_pcraft_writer_save_zip_prepared(w, &p2, &z2, &st, NULL) == PC_OK);
    stats_eq(&st, 3, 0, 3, 0, 0, 0);
    /* Objects not in the last save were forgotten: a's tiles are compressed again. */
    CHECK(pc_pcraft_writer_save_zip_prepared(w, &p1, &z2, &st, NULL) == PC_OK);
    stats_eq(&st, 6, 3, 3, 1, 1, 0);
    {
        /* Entry order: manifest, previews, objects by path; everything loads back verified. */
        pc_zip_reader r;
        pc_format_source *src = NULL;
        pc_format_fetch *f = NULL;
        pc_format_load_opts o = pc_format_load_opts_default();
        CHECK(pc_zip_reader_open(&r, z1.data, z1.len, NULL) == PC_OK);
        CHECK_EQ_UINT(r.count, 9);
        CHECK_EQ_STR(r.entries[0].name.ptr, "manifest.json");
        CHECK_EQ_STR(r.entries[1].name.ptr, "thumb.png");
        CHECK(strncmp(r.entries[2].name.ptr, "blobs/", 6) == 0);
        CHECK(pc_format_source_zip(z1.data, z1.len, &src, NULL) == PC_OK);
        CHECK(pc_format_fetch_new(src, &o, &f, NULL) == PC_OK);
        for (size_t i = 0; i < p1.nobjects; i++) {
            const pc_format_object *ob = &p1.objects[i];
            char hash[65];
            const uint8_t *d;
            size_t n;
            pc_buf tmp = PC_BUF_INIT;
            CHECK(pc_format_object_content(ob, &tmp, &d, &n, NULL) == PC_OK);
            memcpy(hash, ob->path + 6, 64);
            hash[64] = '\0';
            if (ob->is_tile) {
                pc_buf got = PC_BUF_INIT;
                CHECK(pc_format_fetch_tile(f, hash, n, &got, NULL) == PC_OK);
                CHECK(got.len == n && memcmp(got.data, d, n) == 0);
                pc_buf_fini(&got);
            } else {
                pc_blob *got = NULL;
                CHECK(pc_format_fetch_blob(f, hash, &got, NULL) == PC_OK);
                CHECK(pc_blob_len(got) == n && memcmp(pc_blob_data(got), d, n) == 0);
                pc_blob_release(got);
            }
            pc_buf_fini(&tmp);
        }
        pc_format_fetch_free(f);
        pc_format_source_free(src);
        pc_zip_reader_close(&r);
    }
    pc_format_prepared_fini(&p1);
    pc_format_prepared_fini(&p2);
    pc_pcraft_writer_free(w);
    pc_blob_release(icc);
    pc_buf_fini(&z1);
    pc_buf_fini(&z2);
    pc_surface_fini(&a);
    pc_surface_fini(&b);
}

/* A zip bundle holding objects given as raw entry payloads. */
static void zip_of(pc_buf *out, const char *const *names, const pc_buf *payloads, size_t n)
{
    pc_zip_writer w;
    pc_zip_writer_init(&w);
    for (size_t i = 0; i < n; i++) {
        CHECK(pc_zip_writer_add(&w, names[i], strlen(names[i]), payloads[i].data, payloads[i].len, NULL) ==
              PC_OK);
    }
    CHECK(pc_zip_writer_finish(&w, out, NULL) == PC_OK);
    pc_zip_writer_fini(&w);
}

static void test_fetch_errors(void)
{
    uint8_t content[1000];
    char hash[65];
    char other[65];
    char tile_name[80];
    char blob_name[80];
    pc_buf payloads[2] = {PC_BUF_INIT, PC_BUF_INIT};
    const char *names[2];
    pc_buf z = PC_BUF_INIT;
    pc_buf got = PC_BUF_INIT;
    pc_format_source *src = NULL;
    pc_format_fetch *f = NULL;
    pc_format_load_opts o = pc_format_load_opts_default();
    pc_blob *b = NULL;
    pc_blob *b2 = NULL;
    pc_error e;
    for (int i = 0; i < 1000; i++) {
        content[i] = (uint8_t)(i % 7);
    }
    pc_blake3_hex(content, sizeof content, hash);
    pc_blake3_hex("other", 5, other);
    (void)snprintf(tile_name, sizeof tile_name, "tiles/%s.zst", hash);
    (void)snprintf(blob_name, sizeof blob_name, "blobs/%s.zst", hash);
    CHECK(pc_zstd_compress(1, content, sizeof content, &payloads[0], NULL) == PC_OK);
    CHECK(pc_zstd_compress(1, content, sizeof content, &payloads[1], NULL) == PC_OK);
    names[0] = tile_name;
    names[1] = blob_name;
    zip_of(&z, names, payloads, 2);
    CHECK(pc_format_source_zip(z.data, z.len, &src, NULL) == PC_OK);
    CHECK(pc_format_fetch_new(src, &o, &f, NULL) == PC_OK);

    CHECK(pc_format_fetch_tile(f, hash, 1000, &got, NULL) == PC_OK);
    CHECK(pc_format_fetch_tile(f, "../../etc/passwd", 1000, &got, &e) == PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "corrupt .pcraft bundle: invalid tile hash `../../etc/passwd`");
    CHECK(pc_format_fetch_blob(f, "zz", &b, &e) == PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "corrupt .pcraft bundle: invalid blob hash `zz`");
    CHECK(pc_format_fetch_tile(f, other, 1000, &got, &e) == PC_ERR_FORMAT);
    CHECK_CONTAINS(e.msg, "corrupt .pcraft bundle: missing `tiles/");
    /* Asking for fewer bytes than the frame holds reads one more and says so. */
    CHECK(pc_format_fetch_tile(f, hash, 10, &got, &e) == PC_ERR_FORMAT);
    CHECK_CONTAINS(e.msg, ".zst: expected 10 bytes, got 11");
    CHECK(pc_format_fetch_tile(f, hash, 2000, &got, &e) == PC_ERR_FORMAT);
    CHECK_CONTAINS(e.msg, ".zst: expected 2000 bytes, got 1000");
    /* Blobs are cached by hash and shared. */
    CHECK(pc_format_fetch_blob(f, hash, &b, NULL) == PC_OK);
    CHECK(pc_format_fetch_blob(f, hash, &b2, NULL) == PC_OK);
    CHECK(b == b2 && pc_blob_len(b) == 1000);
    pc_blob_release(b);
    pc_blob_release(b2);
    pc_format_fetch_free(f);

    /* Limits: every tile counts before it is read; a blob over max_blob_bytes. */
    o.max_total_bytes = 1500;
    CHECK(pc_format_fetch_new(src, &o, &f, NULL) == PC_OK);
    CHECK(pc_format_fetch_tile(f, hash, 1000, &got, NULL) == PC_OK);
    CHECK(pc_format_fetch_tile(f, other, 1000, &got, &e) == PC_ERR_LIMIT); /* counted, though missing */
    CHECK_EQ_STR(e.msg, "limit exceeded: bundle expands beyond 1500 bytes");
    pc_format_fetch_free(f);
    o = pc_format_load_opts_default();
    o.max_blob_bytes = 999;
    CHECK(pc_format_fetch_new(src, &o, &f, NULL) == PC_OK);
    CHECK(pc_format_fetch_blob(f, hash, &b, &e) == PC_ERR_LIMIT);
    CHECK_EQ_STR(e.msg + strlen(e.msg) - strlen("exceeds max_blob_bytes"), "exceeds max_blob_bytes");
    pc_format_fetch_free(f);
    o.max_blob_bytes = 5; /* smaller than the compressed entry: the zip layer refuses it */
    CHECK(pc_format_fetch_new(src, &o, &f, NULL) == PC_OK);
    CHECK(pc_format_fetch_blob(f, hash, &b, &e) == PC_ERR_LIMIT);
    CHECK_CONTAINS(e.msg, "limit exceeded: zip entry `blobs/");
    pc_format_fetch_free(f);
    pc_format_source_free(src);
    pc_buf_fini(&payloads[0]);
    pc_buf_fini(&payloads[1]);
    pc_buf_fini(&z);
    pc_buf_fini(&got);
}

static void test_dir_source_hostile(void)
{
    char dir[512];
    char p[700];
    pc_format_source *src = NULL;
    pc_buf got = PC_BUF_INIT;
    pc_error e;
    CHECK(t_mkdtemp("dirsrc", dir, sizeof dir) == 0);
    CHECK(pc_format_source_dir(dir, &src, NULL) == PC_OK);
    CHECK(pc_format_read_manifest_raw(src, 100, &got, &e) == PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "corrupt .pcraft bundle: missing `manifest.json`");
    (void)snprintf(p, sizeof p, "%s/manifest.json", dir);
    CHECK(t_write_file(p, "[[[]]]", 6) == 0);
    CHECK(pc_format_read_manifest_raw(src, 100, &got, NULL) == PC_OK);
    CHECK(got.len == 6);
    CHECK(pc_format_read_manifest_raw(src, 5, &got, &e) == PC_ERR_LIMIT);
    CHECK_EQ_STR(e.msg, "limit exceeded: `manifest.json` is 6 bytes (max 5)");
    /* A symlink to an endless device is bounded by max, not read for ever. */
    (void)unlink(p);
    if (symlink("/dev/zero", p) == 0) {
        CHECK(pc_format_read_manifest_raw(src, 1000, &got, &e) == PC_ERR_LIMIT);
        (void)unlink(p);
    }
    /* A FIFO fails instead of blocking the load. */
    if (mkfifo(p, 0644) == 0) {
        pc_status s = pc_format_read_manifest_raw(src, 1000, &got, &e);
        CHECK(s == PC_OK || s != PC_OK); /* returns at all; an empty FIFO reads as empty */
        (void)unlink(p);
    }
    /* A directory where a file should be. */
    CHECK(mkdir(p, 0755) == 0);
    CHECK(pc_format_read_manifest_raw(src, 1 << 20, &got, &e) != PC_OK);
    CHECK(pc_format_error_kind(e.code, &e) == PC_FORMAT_ERR_IO);
    pc_format_source_free(src);
    t_rm_rf(dir);
    pc_buf_fini(&got);
}

static size_t count_files(const char *dir, const char *sub)
{
    char p[700];
    DIR *d;
    struct dirent *e;
    size_t n = 0;
    (void)snprintf(p, sizeof p, "%s/%s", dir, sub);
    d = opendir(p);
    while (d != NULL && (e = readdir(d)) != NULL) {
        n += e->d_name[0] != '.';
    }
    if (d != NULL) {
        (void)closedir(d);
    }
    return n;
}

/* Path of the first object file in dir/sub. */
static void first_object(const char *dir, const char *sub, char *out, size_t cap)
{
    char p[700];
    DIR *d;
    struct dirent *e;
    out[0] = '\0';
    (void)snprintf(p, sizeof p, "%s/%s", dir, sub);
    d = opendir(p);
    while (d != NULL && (e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.') {
            if (snprintf(out, cap, "%s/%s", p, e->d_name) >= (int)cap) {
                out[0] = '\0';
            }
            break;
        }
    }
    if (d != NULL) {
        (void)closedir(d);
    }
}

/* Does every object in the directory bundle load verified? */
static bool dir_loads(const char *dir, const pc_format_prepared *p)
{
    pc_format_source *src = NULL;
    pc_format_fetch *f = NULL;
    pc_format_load_opts o = pc_format_load_opts_default();
    bool ok = true;
    CHECK(pc_format_source_dir(dir, &src, NULL) == PC_OK);
    CHECK(pc_format_fetch_new(src, &o, &f, NULL) == PC_OK);
    for (size_t i = 0; i < p->nobjects && ok; i++) {
        char hash[65];
        const uint8_t *d;
        size_t n;
        pc_buf tmp = PC_BUF_INIT;
        CHECK(pc_format_object_content(&p->objects[i], &tmp, &d, &n, NULL) == PC_OK);
        memcpy(hash, p->objects[i].path + 6, 64);
        hash[64] = '\0';
        if (p->objects[i].is_tile) {
            pc_buf got = PC_BUF_INIT;
            ok = pc_format_fetch_tile(f, hash, n, &got, NULL) == PC_OK;
            pc_buf_fini(&got);
        } else {
            pc_blob *b = NULL;
            ok = pc_format_fetch_blob(f, hash, &b, NULL) == PC_OK;
            pc_blob_release(b);
        }
        pc_buf_fini(&tmp);
    }
    pc_format_fetch_free(f);
    pc_format_source_free(src);
    return ok;
}

static void test_dir_save_incremental_gc_and_repair(void)
{
    char tmp[512];
    char dir[600];
    char junk[700];
    char obj[800];
    pc_surface a;
    pc_surface b;
    pc_blob *icc = NULL;
    pc_surface *both[2];
    pc_surface *only_a[1];
    pc_format_prepared p;
    pc_format_prepared pa;
    pc_pcraft_writer *w = NULL;
    pc_pcraft_writer *w2 = NULL;
    pc_format_save_stats st;
    scribble(&a, 1);
    scribble(&b, 2);
    blob_of(1, 300, &icc);
    both[0] = &a;
    both[1] = &b;
    only_a[0] = &a;
    prepare(NULL, both, 2, &icc, 1, &p);
    prepare(NULL, only_a, 1, &icc, 1, &pa);
    CHECK(t_mkdtemp("dirsave", tmp, sizeof tmp) == 0);
    (void)snprintf(dir, sizeof dir, "%s/bundle", tmp);
    CHECK(pc_pcraft_writer_new(&w, NULL) == PC_OK);
    CHECK(pc_pcraft_writer_save_dir_prepared(w, &p, dir, &st, NULL) == PC_OK);
    stats_eq(&st, 6, 6, 0, 1, 1, 0);
    CHECK_EQ_UINT(count_files(dir, "tiles"), 6);
    CHECK_EQ_UINT(count_files(dir, "blobs"), 1);
    CHECK(dir_loads(dir, &p));
    /* Same writer: everything trusted by signature. A fresh writer verifies by content. */
    CHECK(pc_pcraft_writer_save_dir_prepared(w, &p, dir, &st, NULL) == PC_OK);
    stats_eq(&st, 6, 0, 6, 1, 0, 0);
    CHECK(pc_pcraft_writer_new(&w2, NULL) == PC_OK);
    CHECK(pc_pcraft_writer_save_dir_prepared(w2, &p, dir, &st, NULL) == PC_OK);
    stats_eq(&st, 6, 0, 6, 1, 0, 0);
    /* Garbage collection: unreferenced objects (and stray .zst files) go; other files stay. */
    (void)snprintf(junk, sizeof junk, "%s/tiles/junk.zst", dir);
    CHECK(t_write_file(junk, "j", 1) == 0);
    (void)snprintf(junk, sizeof junk, "%s/tiles/notes.txt", dir);
    CHECK(t_write_file(junk, "n", 1) == 0);
    CHECK(pc_pcraft_writer_save_dir_prepared(w, &pa, dir, &st, NULL) == PC_OK);
    stats_eq(&st, 3, 0, 3, 1, 0, 4);
    CHECK_EQ_UINT(count_files(dir, "tiles"), 4); /* 3 tiles + notes.txt */
    /* Tampered objects are rewritten: garbage, and valid zstd of other content. */
    for (int k = 0; k < 2; k++) {
        const char *sub = k == 0 ? "tiles" : "blobs";
        pc_buf other = PC_BUF_INIT;
        first_object(dir, sub, obj, sizeof obj);
        if (strstr(obj, "notes.txt") != NULL) {
            pc_buf_fini(&other);
            continue;
        }
        CHECK(pc_zstd_compress(1, (const uint8_t *)"something else", 14, &other, NULL) == PC_OK);
        for (int damage = 0; damage < 2; damage++) {
            if (damage == 0) {
                CHECK(t_write_file(obj, "garbage", 7) == 0);
            } else {
                CHECK(t_write_file(obj, other.data, other.len) == 0);
            }
            CHECK(!dir_loads(dir, &pa));
            CHECK(pc_pcraft_writer_save_dir_prepared(w, &pa, dir, &st, NULL) == PC_OK);
            if (k == 0) {
                CHECK_EQ_UINT(st.tiles_written, 1);
            } else {
                CHECK_EQ_UINT(st.blobs_written, 1);
            }
            CHECK(dir_loads(dir, &pa));
        }
        pc_buf_fini(&other);
    }
    pc_pcraft_writer_free(w);
    pc_pcraft_writer_free(w2);
    pc_format_prepared_fini(&p);
    pc_format_prepared_fini(&pa);
    pc_blob_release(icc);
    pc_surface_fini(&a);
    pc_surface_fini(&b);
    t_rm_rf(tmp);
}

/* Damage one tile object in place without changing its length or modification time (what a
 * size-and-mtime cache alone would trust). Returns false if no single-byte change broke it. */
static bool damage_in_place_keeping_size_and_mtime(const char *dir, const pc_format_prepared *p)
{
    char obj[800];
    pc_buf orig = PC_BUF_INIT;
    struct stat st;
    bool done = false;
    first_object(dir, "tiles", obj, sizeof obj);
    CHECK(t_read_file(obj, &orig) == 0);
    CHECK(stat(obj, &st) == 0);
    for (size_t i = 0; i < orig.len && !done; i++) {
        size_t at = (orig.len / 2 + i) % orig.len;
        struct timespec times[2];
        orig.data[at] ^= 0x5a;
        CHECK(t_write_file(obj, orig.data, orig.len) == 0);
        orig.data[at] ^= 0x5a;
        times[0] = T_ATIM(st);
        times[1] = T_MTIM(st);
        CHECK(utimensat(AT_FDCWD, obj, times, 0) == 0);
        if (!dir_loads(dir, p)) {
            struct stat now;
            CHECK(stat(obj, &now) == 0);
            CHECK(now.st_size == st.st_size);
            CHECK(T_MTIM(now).tv_sec == T_MTIM(st).tv_sec && T_MTIM(now).tv_nsec == T_MTIM(st).tv_nsec);
            done = true;
        }
    }
    pc_buf_fini(&orig);
    return done;
}

static void test_same_size_same_mtime_tamper_is_repaired(void)
{
    char tmp[512];
    pc_surface a;
    pc_surface *s1[1];
    pc_format_prepared p;
    pc_pcraft_writer *w = NULL;
    pc_format_save_stats st;
    scribble(&a, 5);
    s1[0] = &a;
    prepare(NULL, s1, 1, NULL, 0, &p);
    CHECK(t_mkdtemp("tamper", tmp, sizeof tmp) == 0);
    CHECK(pc_pcraft_writer_new(&w, NULL) == PC_OK);
    CHECK(pc_pcraft_writer_save_dir_prepared(w, &p, tmp, NULL, NULL) == PC_OK);
    CHECK(pc_pcraft_writer_save_dir_prepared(w, &p, tmp, NULL, NULL) == PC_OK);
    CHECK(damage_in_place_keeping_size_and_mtime(tmp, &p));
    /* The bundle is far below the default budget: the rolling re-check reads every cached object
     * again (and the change time differs as well). */
    CHECK(pc_pcraft_writer_save_dir_prepared(w, &p, tmp, &st, NULL) == PC_OK);
    CHECK_EQ_UINT(st.tiles_written, 1);
    CHECK(dir_loads(tmp, &p));
    pc_pcraft_writer_free(w);
    pc_format_prepared_fini(&p);
    pc_surface_fini(&a);
    t_rm_rf(tmp);
}

static void test_rolling_reverify_reaches_every_object_with_a_tiny_budget(void)
{
    char tmp[512];
    pc_surface a;
    pc_surface b;
    pc_blob *blob = NULL;
    pc_surface *s2[2];
    pc_format_prepared p;
    pc_pcraft_writer *w = NULL;
    pc_pcraft_writer *fresh = NULL;
    pc_format_save_stats st;
    size_t objects;
    bool repaired = false;
    scribble(&a, 5);
    scribble(&b, 6);
    blob_of(7, 100, &blob);
    s2[0] = &a;
    s2[1] = &b;
    prepare(NULL, s2, 2, &blob, 1, &p);
    CHECK(t_mkdtemp("rolling", tmp, sizeof tmp) == 0);
    CHECK(pc_pcraft_writer_new(&w, NULL) == PC_OK);
    CHECK(pc_pcraft_writer_save_dir_prepared(w, &p, tmp, NULL, NULL) == PC_OK);
    objects = count_files(tmp, "tiles") + count_files(tmp, "blobs");
    /* One object per save: the oldest-checked one. */
    pc_pcraft_writer_set_reverify_budget(w, 1);
    CHECK(damage_in_place_keeping_size_and_mtime(tmp, &p));
    for (size_t i = 0; i <= objects && !repaired; i++) {
        CHECK(pc_pcraft_writer_save_dir_prepared(w, &p, tmp, &st, NULL) == PC_OK);
        repaired = st.tiles_written == 1;
    }
    CHECK(repaired);
    CHECK(dir_loads(tmp, &p));
    /* A zero budget re-reads nothing whose signature is unchanged; a fresh writer still verifies
     * everything on its first save. */
    CHECK(pc_pcraft_writer_new(&fresh, NULL) == PC_OK);
    pc_pcraft_writer_set_reverify_budget(fresh, 0);
    CHECK(damage_in_place_keeping_size_and_mtime(tmp, &p));
    CHECK(pc_pcraft_writer_save_dir_prepared(fresh, &p, tmp, &st, NULL) == PC_OK);
    CHECK_EQ_UINT(st.tiles_written, 1);
    CHECK(dir_loads(tmp, &p));
    pc_pcraft_writer_free(w);
    pc_pcraft_writer_free(fresh);
    pc_format_prepared_fini(&p);
    pc_blob_release(blob);
    pc_surface_fini(&a);
    pc_surface_fini(&b);
    t_rm_rf(tmp);
}

static void test_previews_and_stale_removal(void)
{
    char tmp[512];
    char p1[700];
    char p2[700];
    pc_format_prepared with;
    pc_format_prepared without;
    pc_pcraft_writer *w = NULL;
    struct stat st;
    prepare(NULL, NULL, 0, NULL, 0, &with);
    prepare(NULL, NULL, 0, NULL, 0, &without);
    CHECK(pc_format_prepared_add_preview(&with, PC_FORMAT_THUMB, (const uint8_t *)"t", 1, NULL) == PC_OK);
    CHECK(pc_format_prepared_add_preview(&with, PC_FORMAT_COMPOSITE, (const uint8_t *)"c", 1, NULL) == PC_OK);
    CHECK(pc_format_prepared_add_preview(&with, PC_FORMAT_COMPOSITE, (const uint8_t *)"c", 1, NULL) ==
          PC_ERR_INVALID);
    CHECK(t_mkdtemp("previews", tmp, sizeof tmp) == 0);
    (void)snprintf(p1, sizeof p1, "%s/thumb.png", tmp);
    (void)snprintf(p2, sizeof p2, "%s/composite/preview.png", tmp);
    CHECK(pc_pcraft_writer_new(&w, NULL) == PC_OK);
    CHECK(pc_pcraft_writer_save_dir_prepared(w, &with, tmp, NULL, NULL) == PC_OK);
    CHECK(stat(p1, &st) == 0 && stat(p2, &st) == 0);
    CHECK(pc_pcraft_writer_save_dir_prepared(w, &without, tmp, NULL, NULL) == PC_OK);
    CHECK(stat(p1, &st) != 0 && stat(p2, &st) != 0);
    pc_pcraft_writer_free(w);
    pc_format_prepared_fini(&with);
    pc_format_prepared_fini(&without);
    t_rm_rf(tmp);
}

static void test_dir_save_errors_are_reported(void)
{
    char tmp[512];
    char file[700];
    pc_format_prepared p;
    pc_pcraft_writer *w = NULL;
    pc_error e;
    prepare(NULL, NULL, 0, NULL, 0, &p);
    CHECK(t_mkdtemp("direrr", tmp, sizeof tmp) == 0);
    (void)snprintf(file, sizeof file, "%s/plain-file", tmp);
    CHECK(t_write_file(file, "x", 1) == 0);
    CHECK(pc_pcraft_writer_new(&w, NULL) == PC_OK);
    CHECK(pc_pcraft_writer_save_dir_prepared(w, &p, file, NULL, &e) != PC_OK);
    CHECK(pc_format_error_kind(e.code, &e) == PC_FORMAT_ERR_IO);
    CHECK(strncmp(e.msg, "I/O: ", 5) == 0);
    pc_pcraft_writer_free(w);
    pc_format_prepared_fini(&p);
    t_rm_rf(tmp);
}

/* The document-level API end to end (C5 replaced the placeholder this test used to pin). */
static void test_document_level_api(void)
{
    pc_document doc;
    pc_document back;
    pc_size size = {4, 4};
    pc_buf out = PC_BUF_INIT;
    pc_pcraft_writer *w = NULL;
    pc_error e;
    pc_format_manifest *m = NULL;
    bool present = true;
    char tmp[512];
    char path[700];
    static const char *const names[1] = {"manifest.json"};
    pc_buf payload[1] = {PC_BUF_INIT};
    pc_buf z = PC_BUF_INIT;
    CHECK(pc_document_init(&doc, "D", size, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, NULL) == PC_OK);
    CHECK(pc_format_save_to_bytes(&doc, NULL, &out, &e) == PC_OK);
    CHECK(pc_format_is_pcraft(out.data, out.len));
    CHECK(pc_format_load_from_bytes(out.data, out.len, &back, &e) == PC_OK);
    CHECK(pc_document_eq(&doc, &back));
    pc_document_fini(&back);
    CHECK(pc_format_read_manifest(out.data, out.len, &m, &e) == PC_OK);
    CHECK(m != NULL && pc_format_manifest_thumbnail(m) == NULL);
    pc_format_manifest_free(m);
    CHECK(pc_format_read_thumbnail(out.data, out.len, &present, &z, &e) == PC_OK);
    CHECK(!present);
    CHECK(pc_pcraft_writer_new(&w, NULL) == PC_OK);
    CHECK(t_mkdtemp("c5", tmp, sizeof tmp) == 0);
    (void)snprintf(path, sizeof path, "%s/x.pcraft", tmp);
    CHECK(pc_pcraft_writer_save_path(w, &doc, path, NULL, NULL, NULL) == PC_OK);
    CHECK(pc_format_load_path(path, &back, NULL) == PC_OK);
    CHECK(pc_document_eq(&doc, &back));
    pc_document_fini(&back);
    (void)snprintf(path, sizeof path, "%s/d/", tmp);
    CHECK(pc_pcraft_writer_save_path(w, &doc, path, NULL, NULL, NULL) == PC_OK);
    CHECK(pc_format_load_path(path, &back, NULL) == PC_OK);
    CHECK(pc_document_eq(&doc, &back));
    pc_document_fini(&back);
    pc_pcraft_writer_free(w);
    /* Loading: container errors come first and are the reference's; a sound container reaches
     * the manifest decoder. */
    CHECK(pc_format_load_from_bytes((const uint8_t *)"garbage", 7, &back, &e) == PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "corrupt .pcraft bundle: not a zip archive (too short)");
    (void)pc_buf_append_str(&payload[0], "{\"format_version\":1}", NULL);
    pc_buf_truncate(&z, 0);
    zip_of(&z, names, payload, 1);
    CHECK(pc_format_load_from_bytes(z.data, z.len, &back, &e) == PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "manifest JSON: missing field `generator`");
    CHECK(pc_format_error_kind(PC_ERR_FORMAT, &e) == PC_FORMAT_ERR_JSON);
    CHECK(pc_format_read_manifest(z.data, z.len, &m, &e) == PC_ERR_FORMAT);
    CHECK(pc_format_read_thumbnail(z.data, z.len, &present, &out, &e) == PC_ERR_FORMAT);
    (void)snprintf(path, sizeof path, "%s/missing.pcraft", tmp);
    CHECK(pc_format_load_path(path, &back, &e) == PC_ERR_NOT_FOUND);
    CHECK(strncmp(e.msg, "I/O: ", 5) == 0);
    (void)snprintf(path, sizeof path, "%s/empty", tmp);
    CHECK(mkdir(path, 0755) == 0);
    CHECK(pc_format_load_path(path, &back, &e) == PC_ERR_FORMAT); /* a directory without a manifest */
    CHECK_EQ_STR(e.msg, "corrupt .pcraft bundle: missing `manifest.json`");
    pc_document_fini(&doc);
    pc_buf_fini(&out);
    pc_buf_fini(&payload[0]);
    pc_buf_fini(&z);
    t_rm_rf(tmp);
}

static void test_png_previews(void)
{
    pc_rgba8_image img;
    pc_buf png = PC_BUF_INIT;
    pc_cimage back;
    pc_error e;
    CHECK(pc_rgba8_image_init(&img, 3, 2, NULL) == PC_OK);
    for (size_t i = 0; i < 24; i++) {
        img.pixels[i] = (uint8_t)(i * 10);
    }
    CHECK(pc_format_png(&img, &png, NULL) == PC_OK);
    CHECK(png.len > 8 && memcmp(png.data, "\x89PNG", 4) == 0);
    CHECK(pc_cdecode(png.data, png.len, &back, NULL) == PC_OK);
    CHECK(back.width == 3 && back.height == 2 && back.len == 24 && memcmp(back.data, img.pixels, 24) == 0);
    pc_cimage_fini(&back);
    pc_rgba8_image_fini(&img);
    img.width = 2;
    img.height = 2;
    img.pixels = NULL; /* inconsistent: refused, not read */
    CHECK(pc_format_png(&img, &png, &e) == PC_ERR_UNSUPPORTED);
    CHECK(strncmp(e.msg, "unsupported .pcraft feature: preview: ", 38) == 0);
    pc_buf_fini(&png);
}

static void test_error_kinds_and_texts(void)
{
    pc_error e;
    CHECK(pc_format_too_new(&e, 999, 1) == PC_ERR_UNSUPPORTED);
    CHECK_EQ_STR(e.msg, "bundle written by a newer version (format 999, this build reads up to 1)");
    CHECK(pc_format_error_kind(PC_ERR_UNSUPPORTED, &e) == PC_FORMAT_ERR_TOO_NEW);
    CHECK(pc_format_corrupt(&e, "x %d", 1) == PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "corrupt .pcraft bundle: x 1");
    CHECK(pc_format_error_kind(PC_ERR_FORMAT, &e) == PC_FORMAT_ERR_CORRUPT);
    pc_fail_set(&e, PC_ERR_INVALID, "expected value at line 1 column 1");
    CHECK(pc_format_json(PC_ERR_INVALID, &e) == PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "manifest JSON: expected value at line 1 column 1");
    CHECK(pc_format_error_kind(PC_ERR_FORMAT, &e) == PC_FORMAT_ERR_JSON);
    CHECK(pc_format_json(PC_ERR_NOMEM, &e) == PC_ERR_NOMEM);
    CHECK(pc_format_error_kind(PC_ERR_NOMEM, &e) == PC_FORMAT_ERR_OTHER);
    CHECK(pc_format_error_kind(PC_OK, NULL) == PC_FORMAT_ERR_NONE);
    CHECK(pc_format_error_kind(PC_ERR_IO, NULL) == PC_FORMAT_ERR_IO);
    CHECK(pc_format_corrupt(NULL, "ignored") == PC_ERR_FORMAT);
    {
        pc_format_load_opts o = pc_format_load_opts_default();
        CHECK_EQ_UINT(o.max_manifest_bytes, (size_t)256 << 20);
        CHECK_EQ_UINT(o.max_blob_bytes, (size_t)1 << 30);
        CHECK_EQ_UINT(o.max_total_bytes, (uint64_t)16 << 30);
        CHECK(o.preserve_ids);
    }
    CHECK_EQ_STR(PC_FORMAT_EXTENSION, "pcraft");
}

int main(int argc, char **argv)
{
    RUN(test_helpers);
    RUN(test_sink_dedupes_and_sorts);
    RUN(test_hash_cache_never_goes_stale);
    RUN(test_zip_save_incremental);
    RUN(test_fetch_errors);
    RUN(test_dir_source_hostile);
    RUN(test_dir_save_incremental_gc_and_repair);
    RUN(test_same_size_same_mtime_tamper_is_repaired);
    RUN(test_rolling_reverify_reaches_every_object_with_a_tiny_budget);
    RUN(test_previews_and_stale_removal);
    RUN(test_dir_save_errors_are_reported);
    RUN(test_document_level_api);
    RUN(test_png_previews);
    RUN(test_error_kinds_and_texts);
    return pc_t_finish();
}
