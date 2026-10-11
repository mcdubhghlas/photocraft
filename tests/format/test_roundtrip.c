/* doc -> .pcraft -> doc equality, incremental saves, ids, previews
 * (crates/format/tests/roundtrip.rs, with analysis.rs and layer_labels.rs). */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codecs/codecs.h"
#include "common.h"
#include "format/bridge.h"
#include "format/manifest.h"
#include "harness.h"
#include "rich_doc.h"

static const pc_color_mode MODES[4] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_GRAYSCALE, PC_COLOR_MODE_CMYK,
                                       PC_COLOR_MODE_LAB};
static const pc_sample_type DEPTHS[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};

static void save(const pc_document *d, pc_buf *out)
{
    pc_error e;
    if (pc_format_save_to_bytes(d, NULL, out, &e) != PC_OK) {
        PC_T_FAIL("save: %s", e.msg);
    }
}

static bool loads_equal(const pc_buf *b, const pc_document *want)
{
    pc_document back;
    pc_error e;
    bool eq;
    if (pc_format_load_from_bytes(b->data, b->len, &back, &e) != PC_OK) {
        PC_T_FAIL("load: %s", e.msg);
        return false;
    }
    eq = pc_document_eq(&back, want);
    pc_document_fini(&back);
    return eq;
}

static void manifest_text(const pc_buf *bundle, pc_buf *out)
{
    pc_zip_reader r;
    pc_buf_truncate(out, 0);
    CHECK(pc_zip_reader_open(&r, bundle->data, bundle->len, NULL) == PC_OK);
    CHECK(pc_zip_read_by_name(&r, PC_FORMAT_MANIFEST, (size_t)1 << 30, out, NULL) == PC_OK);
    pc_zip_reader_close(&r);
}

/* The bundle with its manifest replaced. */
static void rezip(const pc_buf *bundle, const uint8_t *m, size_t mlen, pc_buf *out)
{
    pc_zip_reader r;
    pc_zip_writer w;
    pc_zip_writer_init(&w);
    CHECK(pc_zip_reader_open(&r, bundle->data, bundle->len, NULL) == PC_OK);
    CHECK(pc_zip_writer_add(&w, PC_FORMAT_MANIFEST, strlen(PC_FORMAT_MANIFEST), m, mlen, NULL) == PC_OK);
    for (size_t i = 0; i < r.count; i++) {
        pc_buf d = PC_BUF_INIT;
        if (pc_str_eq_c(&r.entries[i].name, PC_FORMAT_MANIFEST)) {
            continue;
        }
        CHECK(pc_zip_read(&r, &r.entries[i], (size_t)1 << 30, &d, NULL) == PC_OK);
        CHECK(pc_zip_writer_add(&w, pc_str_c(&r.entries[i].name), r.entries[i].name.len, d.data, d.len,
                                NULL) == PC_OK);
        pc_buf_fini(&d);
    }
    pc_zip_reader_close(&r);
    CHECK(pc_zip_writer_finish(&w, out, NULL) == PC_OK);
    pc_zip_writer_fini(&w);
}

/* ---- zip and directory round trips ---- */

static void test_zip_all_modes_and_depths(void)
{
    for (size_t m = 0; m < 4; m++) {
        for (size_t k = 0; k < 3; k++) {
            pc_document d;
            pc_buf b = PC_BUF_INIT;
            t_rich_doc(MODES[m], DEPTHS[k], &d);
            save(&d, &b);
            CHECK(pc_format_is_pcraft(b.data, b.len));
            if (!loads_equal(&b, &d)) {
                PC_T_FAIL("mode %d depth %d", (int)MODES[m], (int)DEPTHS[k]);
            }
            pc_document_fini(&d);
            pc_buf_fini(&b);
        }
    }
}

static void test_directory_bundle_roundtrip_all_modes(void)
{
    char tmp[512];
    CHECK(t_mkdtemp("dir", tmp, sizeof tmp) == 0);
    for (size_t m = 0; m < 4; m++) {
        for (size_t k = 0; k < 3; k++) {
            pc_document d;
            pc_document back;
            pc_pcraft_writer *w = NULL;
            char dir[700];
            char mf[800];
            struct stat st;
            t_rich_doc(MODES[m], DEPTHS[k], &d);
            (void)snprintf(dir, sizeof dir, "%s/b%zu%zu", tmp, m, k);
            CHECK(pc_pcraft_writer_new(&w, NULL) == PC_OK);
            CHECK(pc_pcraft_writer_save_dir(w, &d, dir, NULL, NULL, NULL) == PC_OK);
            pc_pcraft_writer_free(w);
            (void)snprintf(mf, sizeof mf, "%s/manifest.json", dir);
            CHECK(stat(mf, &st) == 0 && S_ISREG(st.st_mode));
            CHECK(pc_format_load_path(dir, &back, NULL) == PC_OK);
            CHECK(pc_document_eq(&back, &d));
            pc_document_fini(&back);
            pc_document_fini(&d);
            t_rm_rf(dir);
        }
    }
    t_rm_rf(tmp);
}

static void test_save_path_zip_file(void)
{
    char tmp[512];
    char path[700];
    pc_document d;
    pc_document back;
    pc_pcraft_writer *w = NULL;
    struct stat st;
    CHECK(t_mkdtemp("file", tmp, sizeof tmp) == 0);
    (void)snprintf(path, sizeof path, "%s/a.pcraft", tmp);
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, &d);
    CHECK(pc_pcraft_writer_new(&w, NULL) == PC_OK);
    CHECK(pc_pcraft_writer_save_path(w, &d, path, NULL, NULL, NULL) == PC_OK);
    pc_pcraft_writer_free(w);
    CHECK(stat(path, &st) == 0 && S_ISREG(st.st_mode));
    CHECK(pc_format_load_path(path, &back, NULL) == PC_OK);
    CHECK(pc_document_eq(&back, &d));
    pc_document_fini(&back);
    pc_document_fini(&d);
    t_rm_rf(tmp);
}

/* ---- nesting ---- */

/* serde_json's default limit (128 levels) used to make bundles with 40+ nested groups
 * unreadable. Runs on a 1 MiB stack, the smallest main-thread stack the reference ships on. */
static void *deep_run(void *arg)
{
    static const size_t levels[3] = {40, PC_MAX_GROUP_DEPTH, PC_MAX_GROUP_DEPTH};
    static const pc_sample_type depth[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    int *bad = arg;
    for (size_t i = 0; i < 3; i++) {
        pc_document d;
        pc_document back;
        pc_buf b = PC_BUF_INIT;
        t_nested_doc(levels[i], depth[i], &d);
        if (t_group_depth(d.layers, d.nlayers) != levels[i] ||
            pc_format_save_to_bytes(&d, NULL, &b, NULL) != PC_OK ||
            pc_format_load_from_bytes(b.data, b.len, &back, NULL) != PC_OK) {
            (*bad)++;
        } else {
            *bad += pc_document_eq(&back, &d) ? 0 : 1;
            pc_document_fini(&back);
        }
        pc_document_fini(&d);
        pc_buf_fini(&b);
    }
    return NULL;
}

static void test_deeply_nested_groups_roundtrip(void)
{
    pthread_attr_t attr;
    pthread_t t;
    int bad = 0;
    int created;
    int joined = -1;
    /* No CHECK while the thread runs: the harness counters are not synchronised (the thread's
     * builders use them). */
    CHECK(pthread_attr_init(&attr) == 0);
    CHECK(pthread_attr_setstacksize(&attr, (size_t)1 << 20) == 0);
    created = pthread_create(&t, &attr, deep_run, &bad);
    if (created == 0) {
        joined = pthread_join(t, NULL);
    }
    (void)pthread_attr_destroy(&attr);
    CHECK_EQ_INT(created, 0);
    CHECK_EQ_INT(joined, 0);
    CHECK_EQ_INT(bad, 0);
}

/* A save never writes a bundle the loader would reject for its nesting. */
static void test_nesting_beyond_the_load_limit_is_refused_at_save(void)
{
    pc_document d;
    pc_buf b = PC_BUF_INIT;
    pc_error e;
    pc_status st;
    t_nested_doc(PC_MAX_GROUP_DEPTH + 1, PC_SAMPLE_TYPE_U8, &d);
    st = pc_format_save_to_bytes(&d, NULL, &b, &e);
    CHECK(st == PC_ERR_LIMIT);
    CHECK(pc_format_error_kind(st, &e) == PC_FORMAT_ERR_LIMIT);
    CHECK_EQ_STR(e.msg, "limit exceeded: layer groups nested deeper than 100");
    pc_document_fini(&d);
    pc_buf_fini(&b);
}

static void test_empty_document_roundtrips(void)
{
    pc_document d;
    pc_size one = {1, 1};
    pc_buf b = PC_BUF_INIT;
    CHECK(pc_document_init(&d, "empty", one, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, NULL) == PC_OK);
    save(&d, &b);
    CHECK(loads_equal(&b, &d));
    pc_document_fini(&d);
    pc_buf_fini(&b);
}

static void test_exhausted_document_ids_and_maximal_text_runs_roundtrip(void)
{
    pc_document d;
    pc_document back;
    pc_size size = {2, 1};
    pc_layer_content c;
    pc_layer l;
    pc_buf b = PC_BUF_INIT;
    uint32_t id;
    CHECK(pc_document_init(&d, "exhausted", size, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, NULL) == PC_OK);
    d.slices.list = calloc(1, sizeof *d.slices.list);
    if (d.slices.list != NULL) {
        pc_slice_default(&d.slices.list[0]);
        d.slices.list[0].id = UINT32_MAX;
        d.slices.len = 1;
    }
    pc_text_styles_fini(&d.text_styles);
    T_JSON(pc_text_styles,
           "{\"character\":[{\"id\":4294967295,\"name\":\"\",\"attrs\":{}}],\"paragraph\":[{\"id\":"
           "4294967295}]}",
           &d.text_styles);
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_TEXT;
    pc_text_layer_default(&c.u.text);
    t_str(&c.u.text.text, "ab");
    T_JSON_ARRAY(pc_text_run,
                 "[{\"len\":1,\"style\":{}},{\"len\":18446744073709551615,\"style\":{\"size_pt\":24.0}}]",
                 &c.u.text.runs, &c.u.text.nruns);
    T_JSON_ARRAY(pc_paragraph_run, "[{\"len\":1,\"style\":{}},{\"len\":18446744073709551615,\"style\":{}}]",
                 &c.u.text.paragraphs, &c.u.text.nparagraphs);
    t_layer(&l, "Text", &c);
    t_push_layer(&d, &l);
    save(&d, &b);
    if (pc_format_load_from_bytes(b.data, b.len, &back, NULL) == PC_OK) {
        const pc_text_layer *t = &back.layers[back.nlayers - 1].content.u.text;
        pc_text_run *runs = NULL;
        pc_paragraph_run *paras = NULL;
        size_t nr = 0;
        size_t np = 0;
        size_t sum = 0;
        CHECK(!pc_slices_next_id(&back.slices, &id));
        CHECK(!pc_text_styles_next_char_id(&back.text_styles, &id));
        CHECK(!pc_text_styles_next_para_id(&back.text_styles, &id));
        CHECK(back.layers[back.nlayers - 1].content.kind == PC_LAYER_TEXT);
        CHECK(pc_text_layer_char_runs(t, &runs, &nr, NULL) == PC_OK);
        for (size_t i = 0; i < nr; i++) {
            sum += runs[i].len;
        }
        CHECK_EQ_UINT(sum, t->text.len);
        CHECK(nr >= 2 && runs[1].len == 1 && runs[1].style.size_pt == 24.0f);
        CHECK(pc_text_layer_paragraph_runs(t, &paras, &np, NULL) == PC_OK);
        sum = 0;
        for (size_t i = 0; i < np; i++) {
            sum += paras[i].len;
        }
        CHECK_EQ_UINT(sum, t->text.len);
        for (size_t i = 0; i < nr; i++) {
            pc_char_style_fini(&runs[i].style);
        }
        free(runs);
        free(paras);
        pc_document_fini(&back);
    } else {
        PC_T_FAIL("load failed");
    }
    pc_document_fini(&d);
    pc_buf_fini(&b);
}

/* ---- incremental saves ---- */

static void test_zip_incremental_only_new_tiles(void)
{
    pc_document d;
    pc_pcraft_writer *w = NULL;
    pc_buf b = PC_BUF_INIT;
    pc_format_save_stats s1, s2, s3;
    float px[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, &d);
    CHECK(pc_pcraft_writer_new(&w, NULL) == PC_OK);
    CHECK(pc_pcraft_writer_save_zip(w, &d, NULL, &b, &s1, NULL) == PC_OK);
    CHECK(s1.tiles_total > 5);
    CHECK_EQ_UINT(s1.tiles_written, s1.tiles_total);
    CHECK(pc_pcraft_writer_save_zip(w, &d, NULL, &b, &s2, NULL) == PC_OK);
    CHECK_EQ_UINT(s2.tiles_written, 0);
    CHECK_EQ_UINT(s2.tiles_reused, s2.tiles_total);
    CHECK_EQ_UINT(s2.blobs_written, 0);
    /* Touch one pixel of one layer: exactly one tile is new. */
    CHECK(pc_surface_write_pixel(pc_layer_surface_mut(&d.layers[1]), 3, 3, px, 4, NULL) == PC_OK);
    CHECK(pc_pcraft_writer_save_zip(w, &d, NULL, &b, &s3, NULL) == PC_OK);
    CHECK_EQ_UINT(s3.tiles_written, 1);
    CHECK(loads_equal(&b, &d));
    pc_pcraft_writer_free(w);
    pc_document_fini(&d);
    pc_buf_fini(&b);
}

static size_t count_files(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    size_t n = 0;
    while (d != NULL && (e = readdir(d)) != NULL) {
        n += e->d_name[0] != '.';
    }
    if (d != NULL) {
        (void)closedir(d);
    }
    return n;
}

static void test_directory_incremental_and_gc(void)
{
    pc_document d;
    pc_document back;
    pc_layer removed;
    pc_pcraft_writer *w = NULL;
    pc_pcraft_writer *fresh = NULL;
    pc_format_save_stats s;
    char tmp[512];
    char tiles[700];
    CHECK(t_mkdtemp("gc", tmp, sizeof tmp) == 0);
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, &d);
    CHECK(pc_pcraft_writer_new(&w, NULL) == PC_OK);
    CHECK(pc_pcraft_writer_save_dir(w, &d, tmp, NULL, &s, NULL) == PC_OK);
    CHECK_EQ_UINT(s.tiles_written, s.tiles_total);
    CHECK(pc_pcraft_writer_save_dir(w, &d, tmp, NULL, &s, NULL) == PC_OK);
    CHECK_EQ_UINT(s.tiles_written, 0);
    CHECK_EQ_UINT(s.tiles_reused, s.tiles_total);
    /* A fresh writer still skips files already on disk. */
    CHECK(pc_pcraft_writer_new(&fresh, NULL) == PC_OK);
    CHECK(pc_pcraft_writer_save_dir(fresh, &d, tmp, NULL, &s, NULL) == PC_OK);
    CHECK_EQ_UINT(s.tiles_written, 0);
    CHECK_EQ_UINT(s.objects_removed, 0);
    pc_pcraft_writer_free(fresh);
    /* Delete a layer: its unique tiles get garbage-collected. */
    CHECK(pc_document_remove(&d, d.layers[1].id, &removed, NULL) == PC_OK);
    pc_layer_fini(&removed);
    CHECK(pc_pcraft_writer_save_dir(w, &d, tmp, NULL, &s, NULL) == PC_OK);
    CHECK(s.objects_removed > 0);
    CHECK(pc_format_load_path(tmp, &back, NULL) == PC_OK);
    CHECK(pc_document_eq(&back, &d));
    pc_document_fini(&back);
    (void)snprintf(tiles, sizeof tiles, "%s/tiles", tmp);
    CHECK_EQ_UINT(count_files(tiles), s.tiles_total);
    pc_pcraft_writer_free(w);
    pc_document_fini(&d);
    t_rm_rf(tmp);
}

static void test_identical_tiles_are_stored_once(void)
{
    pc_document d;
    pc_size size = {10, 10};
    pc_layer a;
    pc_layer b;
    pc_buf out = PC_BUF_INIT;
    pc_format_save_stats s;
    pc_pcraft_writer *w = NULL;
    CHECK(pc_document_init(&d, "d", size, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, NULL) == PC_OK);
    CHECK(pc_layer_raster(&a, "a", pc_document_pixel_format(&d), NULL) == PC_OK);
    t_scribble(pc_layer_surface_mut(&a), 5, false);
    CHECK(pc_layer_duplicate(&a, &b, NULL) == PC_OK);
    t_push_layer(&d, &a);
    t_push_layer(&d, &b);
    CHECK(pc_pcraft_writer_new(&w, NULL) == PC_OK);
    CHECK(pc_pcraft_writer_save_zip(w, &d, NULL, &out, &s, NULL) == PC_OK);
    CHECK_EQ_UINT(s.tiles_total, pc_surface_tile_count(pc_layer_surface(&d.layers[0])));
    pc_pcraft_writer_free(w);
    pc_document_fini(&d);
    pc_buf_fini(&out);
}

/* ---- ids ---- */

static void test_ids_preserved_and_counter_advanced(void)
{
    pc_document d;
    pc_document back;
    pc_buf b = PC_BUF_INIT;
    pc_buf m = PC_BUF_INIT;
    pc_buf text = PC_BUF_INIT;
    pc_buf z = PC_BUF_INIT;
    yyjson_doc *j = NULL;
    yyjson_mut_doc *mj;
    uint64_t big;
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, &d);
    save(&d, &b);
    /* Simulate a file written with much larger ids. */
    manifest_text(&b, &m);
    CHECK(pc_json_parse((const char *)m.data, m.len, &j, NULL) == PC_OK);
    mj = yyjson_doc_mut_copy(j, NULL);
    big = pc_layer_id_fresh() + 5000;
    {
        yyjson_mut_val *l0 = yyjson_mut_arr_get(
            yyjson_mut_obj_get(yyjson_mut_obj_get(yyjson_mut_doc_get_root(mj), "document"), "layers"), 0);
        CHECK(yyjson_mut_obj_put(l0, yyjson_mut_str(mj, "id"), yyjson_mut_uint(mj, big)));
    }
    CHECK(pc_json_write(yyjson_mut_doc_get_root(mj), 0, &text, NULL) == PC_OK);
    rezip(&b, text.data, text.len, &z);
    CHECK(pc_format_load_from_bytes(z.data, z.len, &back, NULL) == PC_OK);
    CHECK(back.nlayers > 0 && back.layers[0].id == big);
    CHECK(pc_layer_id_fresh() > big); /* fresh ids must not collide with loaded ids */
    pc_document_fini(&back);
    yyjson_mut_doc_free(mj);
    yyjson_doc_free(j);
    pc_document_fini(&d);
    pc_buf_fini(&b);
    pc_buf_fini(&m);
    pc_buf_fini(&text);
    pc_buf_fini(&z);
}

static void test_fresh_ids_option(void)
{
    pc_document d;
    pc_document back;
    pc_buf b = PC_BUF_INIT;
    pc_format_load_opts o = pc_format_load_opts_default();
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, &d);
    save(&d, &b);
    o.preserve_ids = false;
    CHECK(pc_format_load_from_bytes_with(b.data, b.len, &o, &back, NULL) == PC_OK);
    CHECK(back.id != d.id);
    CHECK(back.layers[0].id != d.layers[0].id);
    CHECK_EQ_UINT(back.nlayers, d.nlayers);
    CHECK(pc_surface_eq(pc_layer_surface(&back.layers[1]), pc_layer_surface(&d.layers[1])));
    pc_document_fini(&back);
    pc_document_fini(&d);
    pc_buf_fini(&b);
}

/* ---- previews and the manifest text ---- */

static void test_previews_embedded_and_readable(void)
{
    pc_document d;
    pc_rgba8_image thumb;
    pc_format_save_opts opts;
    pc_buf b = PC_BUF_INIT;
    pc_buf png = PC_BUF_INIT;
    pc_cimage img;
    pc_format_manifest *m = NULL;
    bool present = false;
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, &d);
    CHECK(pc_rgba8_image_init(&thumb, 4, 3, NULL) == PC_OK);
    for (size_t i = 0; i < 48; i++) {
        thumb.pixels[i] = (uint8_t)i;
    }
    opts.thumbnail = &thumb;
    opts.composite = &thumb;
    CHECK(pc_format_save_to_bytes(&d, &opts, &b, NULL) == PC_OK);
    CHECK(pc_format_read_thumbnail(b.data, b.len, &present, &png, NULL) == PC_OK);
    CHECK(present);
    if (pc_cdecode(png.data, png.len, &img, NULL) == PC_OK) {
        uint8_t *px = NULL;
        size_t n = 0;
        CHECK(img.width == 4 && img.height == 3);
        CHECK(pc_cimage_to_rgba8(&img, &px, &n, NULL) == PC_OK);
        CHECK(n == 48 && memcmp(px, thumb.pixels, 48) == 0);
        free(px);
        pc_cimage_fini(&img);
    } else {
        PC_T_FAIL("thumbnail does not decode");
    }
    CHECK(pc_format_read_manifest(b.data, b.len, &m, NULL) == PC_OK);
    CHECK(m != NULL && pc_format_manifest_composite(m) != NULL &&
          strcmp(pc_format_manifest_composite(m), "composite/preview.png") == 0);
    CHECK(m != NULL && pc_format_manifest_format_version(m) == PC_FORMAT_VERSION);
    pc_format_manifest_free(m);
    save(&d, &b);
    CHECK(pc_format_read_thumbnail(b.data, b.len, &present, &png, NULL) == PC_OK);
    CHECK(!present);
    pc_rgba8_image_fini(&thumb);
    pc_document_fini(&d);
    pc_buf_fini(&b);
    pc_buf_fini(&png);
}

static void test_manifest_is_human_readable_json(void)
{
    pc_document d;
    pc_buf b = PC_BUF_INIT;
    pc_buf m = PC_BUF_INIT;
    pc_zip_reader r;
    bool tiles = false;
    bool blobs = false;
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, &d);
    save(&d, &b);
    CHECK(pc_zip_reader_open(&r, b.data, b.len, NULL) == PC_OK);
    CHECK(r.count > 0 && pc_str_eq_c(&r.entries[0].name, "manifest.json"));
    for (size_t i = 0; i < r.count; i++) {
        const char *n = pc_str_c(&r.entries[i].name);
        size_t len = r.entries[i].name.len;
        tiles |= strncmp(n, "tiles/", 6) == 0 && len > 4 && strcmp(n + len - 4, ".zst") == 0;
        blobs |= strncmp(n, "blobs/", 6) == 0;
    }
    pc_zip_reader_close(&r);
    CHECK(tiles && blobs);
    manifest_text(&b, &m);
    CHECK(strstr(pc_buf_cstr(&m), "\"format_version\": 1") != NULL);
    CHECK(strstr(pc_buf_cstr(&m), "\"Hue/Sat\"") != NULL);
    CHECK(strstr(pc_buf_cstr(&m), "filter.blur.gaussian") != NULL);
    pc_document_fini(&d);
    pc_buf_fini(&b);
    pc_buf_fini(&m);
}

static void test_save_is_deterministic(void)
{
    pc_document d;
    pc_buf a = PC_BUF_INIT;
    pc_buf b = PC_BUF_INIT;
    t_rich_doc(PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U16, &d);
    save(&d, &a);
    save(&d, &b);
    CHECK(a.len == b.len && memcmp(a.data, b.data, a.len) == 0);
    pc_document_fini(&d);
    pc_buf_fini(&a);
    pc_buf_fini(&b);
}

static void test_not_pcraft(void)
{
    pc_zip_writer w;
    pc_buf z = PC_BUF_INIT;
    CHECK(!pc_format_is_pcraft((const uint8_t *)"", 0));
    CHECK(!pc_format_is_pcraft((const uint8_t *)"8BPS....", 8));
    pc_zip_writer_init(&w);
    CHECK(pc_zip_writer_add(&w, "other.txt", 9, (const uint8_t *)"x", 1, NULL) == PC_OK);
    CHECK(pc_zip_writer_finish(&w, &z, NULL) == PC_OK);
    CHECK(!pc_format_is_pcraft(z.data, z.len));
    pc_zip_writer_fini(&w);
    pc_buf_fini(&z);
}

/* ---- older manifests: fields missing load as their defaults ---- */

/* Decode a manifest whose document was edited by f (a mutable copy of the saved one). */
static pc_format_manifest *decode_edited(const pc_buf *bundle, void (*f)(yyjson_mut_doc *, yyjson_mut_val *))
{
    pc_buf m = PC_BUF_INIT;
    pc_buf text = PC_BUF_INIT;
    yyjson_doc *j = NULL;
    yyjson_mut_doc *mj = NULL;
    pc_format_manifest *out = NULL;
    pc_error e;
    manifest_text(bundle, &m);
    CHECK(pc_json_parse((const char *)m.data, m.len, &j, NULL) == PC_OK);
    mj = j != NULL ? yyjson_doc_mut_copy(j, NULL) : NULL;
    if (mj != NULL) {
        f(mj, yyjson_mut_obj_get(yyjson_mut_doc_get_root(mj), "document"));
        CHECK(pc_json_write(yyjson_mut_doc_get_root(mj), 0, &text, NULL) == PC_OK);
        if (pc_format_manifest_parse(text.data, text.len, &out, &e) != PC_OK) {
            PC_T_FAIL("edited manifest: %s", e.msg);
        }
    }
    yyjson_mut_doc_free(mj);
    yyjson_doc_free(j);
    pc_buf_fini(&m);
    pc_buf_fini(&text);
    return out;
}

static void strip_vector_fields(yyjson_mut_doc *d, yyjson_mut_val *doc);

static void strip_layer_vectors(yyjson_mut_val *layers)
{
    yyjson_mut_val *l;
    yyjson_mut_arr_iter it;
    (void)yyjson_mut_arr_iter_init(layers, &it);
    while ((l = yyjson_mut_arr_iter_next(&it)) != NULL) {
        yyjson_mut_val *c = yyjson_mut_obj_get(l, "content");
        (void)yyjson_mut_obj_remove_key(l, "vector_mask");
        (void)yyjson_mut_obj_remove_key(c, "path");
        (void)yyjson_mut_obj_remove_key(c, "stroke");
        (void)yyjson_mut_obj_remove_key(c, "live");
        if (yyjson_mut_obj_get(c, "children") != NULL) {
            strip_layer_vectors(yyjson_mut_obj_get(c, "children"));
        }
    }
}

static void strip_vector_fields(yyjson_mut_doc *d, yyjson_mut_val *doc)
{
    (void)d;
    (void)yyjson_mut_obj_remove_key(doc, "paths");
    (void)yyjson_mut_obj_remove_key(doc, "work_path");
    (void)yyjson_mut_obj_remove_key(doc, "clipping_path");
    strip_layer_vectors(yyjson_mut_obj_get(doc, "layers"));
}

static const pc_layer *find_vector_mask(const pc_layer *layers, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const pc_layer *hit;
        if (layers[i].has_vector_mask) {
            return &layers[i];
        }
        if (layers[i].content.kind == PC_LAYER_GROUP &&
            (hit = find_vector_mask(layers[i].content.u.group.children,
                                    layers[i].content.u.group.nchildren)) != NULL) {
            return hit;
        }
    }
    return NULL;
}

static void test_vector_fields_roundtrip_and_default_when_absent(void)
{
    pc_document d;
    pc_document back;
    pc_buf b = PC_BUF_INIT;
    pc_format_manifest *old;
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, &d);
    save(&d, &b);
    CHECK(pc_format_load_from_bytes(b.data, b.len, &back, NULL) == PC_OK);
    CHECK_EQ_UINT(back.npaths, d.npaths);
    for (size_t i = 0; i < d.npaths && i < back.npaths; i++) {
        CHECK(pc_named_path_eq(&back.paths[i], &d.paths[i]));
    }
    CHECK(back.has_work_path && pc_path_eq(&back.work_path, &d.work_path));
    CHECK(back.has_clipping_path && pc_clipping_path_eq(&back.clipping_path, &d.clipping_path));
    {
        const pc_layer *a = find_vector_mask(back.layers, back.nlayers);
        const pc_layer *w = find_vector_mask(d.layers, d.nlayers);
        CHECK(a != NULL && w != NULL && pc_vector_mask_eq(&a->vector_mask, &w->vector_mask));
    }
    pc_document_fini(&back);
    /* Manifests written before the vector fields existed still load (serde defaults). */
    old = decode_edited(&b, strip_vector_fields);
    if (old != NULL) {
        CHECK_EQ_UINT(old->document.npaths, 0);
        CHECK(!old->document.has_work_path);
        for (size_t i = 0; i < old->document.layers.n; i++) {
            CHECK(!old->document.layers.v[i].has_vector_mask);
        }
    }
    pc_format_manifest_free(old);
    pc_document_fini(&d);
    pc_buf_fini(&b);
}

static void strip_kern(yyjson_mut_val *v)
{
    if (yyjson_mut_is_obj(v)) {
        yyjson_mut_obj_iter it;
        yyjson_mut_val *k;
        (void)yyjson_mut_obj_remove_key(v, "kern");
        (void)yyjson_mut_obj_iter_init(v, &it);
        while ((k = yyjson_mut_obj_iter_next(&it)) != NULL) {
            strip_kern(yyjson_mut_obj_iter_get_val(k));
        }
    } else if (yyjson_mut_is_arr(v)) {
        yyjson_mut_arr_iter it;
        yyjson_mut_val *e;
        (void)yyjson_mut_arr_iter_init(v, &it);
        while ((e = yyjson_mut_arr_iter_next(&it)) != NULL) {
            strip_kern(e);
        }
    }
}

static void strip_all_kern(yyjson_mut_doc *d, yyjson_mut_val *doc)
{
    pc_buf text = PC_BUF_INIT;
    (void)d;
    strip_kern(doc);
    CHECK(pc_json_write(doc, 0, &text, NULL) == PC_OK);
    CHECK(strstr(pc_buf_cstr(&text), "\"kern\"") == NULL);
    pc_buf_fini(&text);
}

static const pc_text_layer *find_text(const pc_layer *layers, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (layers[i].content.kind == PC_LAYER_TEXT) {
            return &layers[i].content.u.text;
        }
    }
    return NULL;
}

/* Type kerning (#206): the auto mode and manual kerning survive .pcraft; manifests written
 * before `kern` existed load with no manual kerning. */
static void test_text_kerning_roundtrips_and_defaults(void)
{
    pc_document d;
    pc_document back;
    pc_buf b = PC_BUF_INIT;
    pc_format_manifest *old;
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, &d);
    save(&d, &b);
    CHECK(pc_format_load_from_bytes(b.data, b.len, &back, NULL) == PC_OK);
    {
        const pc_text_layer *t = find_text(back.layers, back.nlayers);
        CHECK(t != NULL && t->nruns == 3);
        if (t != NULL && t->nruns == 3) {
            CHECK(t->runs[0].len == 1 && t->runs[0].style.kerning == PC_KERNING_OFF &&
                  t->runs[0].style.kern == 120.0f);
            CHECK(t->runs[1].len == 5 && t->runs[1].style.kerning == PC_KERNING_METRICS &&
                  t->runs[1].style.kern == 0.0f);
            CHECK(t->runs[2].len == 9 && t->runs[2].style.kerning == PC_KERNING_OPTICAL &&
                  t->runs[2].style.kern == 0.0f);
        }
    }
    pc_document_fini(&back);
    old = decode_edited(&b, strip_all_kern);
    if (old != NULL) {
        bool seen = false;
        for (size_t i = 0; i < old->document.layers.n; i++) {
            const pc_mf_content *c = &old->document.layers.v[i].content;
            if (c->kind == PC_LAYER_TEXT) {
                for (size_t k = 0; k < c->nruns; k++) {
                    CHECK(c->runs[k].style.kern == 0.0f); /* kern defaults to 0 */
                    seen = true;
                }
            }
        }
        CHECK(seen);
    }
    pc_format_manifest_free(old);
    pc_document_fini(&d);
    pc_buf_fini(&b);
}

static void test_channel_restrictions_and_bevel_elements_roundtrip(void)
{
    pc_document d;
    pc_document back;
    pc_buf b = PC_BUF_INIT;
    pc_effect *g;
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, &d);
    d.layers[0].excluded_channels = 5;
    g = realloc(d.layers[0].effects.items, (d.layers[0].effects.nitems + 1) * sizeof *g);
    if (g != NULL) {
        d.layers[0].effects.items = g;
        T_JSON(
            pc_effect,
            "{\"BevelEmboss\":{\"enabled\":true,\"style\":\"Emboss\",\"technique\":\"ChiselHard\",\"depth\":"
            "1.2,"
            "\"up\":false,\"size\":9.0,\"soften\":1.0,\"angle\":30.0,\"altitude\":40.0,"
            "\"use_global_light\":false,\"gloss_contour\":\"Linear\",\"highlight\":{\"enabled\":true,"
            "\"blend\":\"Screen\",\"opacity\":0.7},\"highlight_color\":" T_COLOR(
                "[1.0,1.0,1.0,0.0]") ","
                                     "\"shadow\":{\"enabled\":true,\"blend\":\"Multiply\",\"opacity\":0.6},"
                                     "\"shadow_color\":" T_COLOR(
                                         "[0.0,0.0,0.0,0.0]") ","
                                                              "\"contour\":{\"contour\":\"Linear\",\"range\":"
                                                              "0.3,\"anti_alias\":true},"
                                                              "\"texture\":{\"name\":\"p\",\"id\":\"i\","
                                                              "\"scale\":0.5,\"depth\":-1.5,\"invert\":true,"
                                                              "\"link\":false,\"phase\":[1.0,2.0]}}}",
            &g[d.layers[0].effects.nitems]);
        d.layers[0].effects.nitems++;
    }
    save(&d, &b);
    CHECK(pc_format_load_from_bytes(b.data, b.len, &back, NULL) == PC_OK);
    CHECK(pc_document_eq(&back, &d));
    CHECK_EQ_UINT(back.layers[0].excluded_channels, 5);
    pc_document_fini(&back);
    pc_document_fini(&d);
    pc_buf_fini(&b);
}

static void test_blend_if_roundtrips(void)
{
    pc_document d;
    pc_document back;
    pc_buf b = PC_BUF_INIT;
    pc_blend_range r0[2] = {{{20, 60}, {255, 255}}, PC_BLEND_RANGE_FULL};
    pc_blend_range r3[2] = {PC_BLEND_RANGE_FULL, {{0, 0}, {180, 220}}};
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, &d);
    CHECK(pc_blend_if_set(&d.layers[0].blend_if, 0, r0, NULL) == PC_OK);
    CHECK(pc_blend_if_set(&d.layers[0].blend_if, 3, r3, NULL) == PC_OK);
    save(&d, &b);
    CHECK(pc_format_load_from_bytes(b.data, b.len, &back, NULL) == PC_OK);
    CHECK(pc_document_eq(&back, &d));
    CHECK(pc_blend_if_eq(&back.layers[0].blend_if, &d.layers[0].blend_if));
    /* Layers without Blend If keep the default (the field is omitted from the manifest). */
    for (size_t i = 1; i < back.nlayers; i++) {
        CHECK(pc_blend_if_is_default(&back.layers[i].blend_if));
    }
    pc_document_fini(&back);
    pc_document_fini(&d);
    pc_buf_fini(&b);
}

static void strip_advanced(yyjson_mut_doc *d, yyjson_mut_val *doc)
{
    yyjson_mut_val *l;
    yyjson_mut_arr_iter it;
    (void)d;
    (void)yyjson_mut_arr_iter_init(yyjson_mut_obj_get(doc, "layers"), &it);
    while ((l = yyjson_mut_arr_iter_next(&it)) != NULL) {
        (void)yyjson_mut_obj_remove_key(l, "advanced");
    }
}

/* Advanced Blending survives .pcraft at every depth; manifests without the field load with
 * Photoshop's defaults, and default layers don't write it. */
static void test_advanced_blending_roundtrips_and_defaults_when_absent(void)
{
    for (size_t k = 0; k < 3; k++) {
        pc_document d;
        pc_document back;
        pc_buf b = PC_BUF_INIT;
        pc_buf m = PC_BUF_INIT;
        yyjson_doc *j = NULL;
        pc_format_manifest *old;
        size_t with = 0;
        pc_advanced_blending a = {PC_KNOCKOUT_DEEP, true, false, false, true, true};
        t_rich_doc(PC_COLOR_MODE_RGB, DEPTHS[k], &d);
        d.layers[0].advanced = a;
        if (d.nlayers > 1) {
            d.layers[1].advanced.knockout = PC_KNOCKOUT_SHALLOW;
        }
        save(&d, &b);
        CHECK(pc_format_load_from_bytes(b.data, b.len, &back, NULL) == PC_OK);
        CHECK(pc_document_eq(&back, &d));
        CHECK(back.layers[0].advanced.knockout == a.knockout &&
              back.layers[0].advanced.blend_interior == a.blend_interior &&
              back.layers[0].advanced.blend_clipped == a.blend_clipped &&
              back.layers[0].advanced.transparency_shapes == a.transparency_shapes &&
              back.layers[0].advanced.layer_mask_hides_effects == a.layer_mask_hides_effects &&
              back.layers[0].advanced.vector_mask_hides_effects == a.vector_mask_hides_effects);
        pc_document_fini(&back);
        manifest_text(&b, &m);
        if (pc_json_parse((const char *)m.data, m.len, &j, NULL) == PC_OK) {
            yyjson_val *layers = yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(j), "document"), "layers");
            for (size_t i = 0; i < yyjson_arr_size(layers); i++) {
                with += yyjson_obj_get(yyjson_arr_get(layers, i), "advanced") != NULL;
            }
        }
        CHECK_EQ_UINT(with, d.nlayers > 1 ? 2 : 1);
        yyjson_doc_free(j);
        old = decode_edited(&b, strip_advanced);
        for (size_t i = 0; old != NULL && i < old->document.layers.n; i++) {
            CHECK(pc_advanced_blending_is_default(&old->document.layers.v[i].advanced));
        }
        pc_format_manifest_free(old);
        pc_document_fini(&d);
        pc_buf_fini(&b);
        pc_buf_fini(&m);
    }
}

static void test_video_layer_frames_survive_roundtrip(void)
{
    pc_document d;
    pc_document back;
    pc_size size = {8, 6};
    pc_pixel_format fmt;
    pc_layer l;
    pc_surface *frames;
    pc_buf b = PC_BUF_INIT;
    CHECK(pc_document_init(&d, "Vid", size, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, NULL) == PC_OK);
    d.has_timeline = true;
    d.timeline = pc_timeline_new(3, 24.0f);
    fmt = pc_document_pixel_format(&d);
    CHECK(pc_layer_raster(&l, "Clip", fmt, NULL) == PC_OK);
    frames = calloc(3, sizeof *frames);
    for (uint32_t i = 0; frames != NULL && i < 3; i++) {
        float v = (float)i / 2.0f;
        float rgba[4] = {v, 0.0f, 1.0f - v, 1.0f};
        float px[PC_RASTER_MAX_CHANNELS];
        size_t n = pc_from_rgba(fmt, rgba, px);
        pc_rect r = {0, 0, 8, 6};
        pc_surface_init(&frames[i], fmt);
        CHECK(pc_surface_fill_rect(&frames[i], r, px, n, NULL) == PC_OK);
    }
    pc_video_data_init(&l.video, frames, frames != NULL ? 3 : 0, 24.0f);
    l.has_video = true;
    l.video.source.is_file = true;
    t_str(&l.video.source.path, "/tmp/seq");
    l.video.show_altered = false;
    t_push_layer(&d, &l);
    save(&d, &b);
    CHECK(pc_format_load_from_bytes(b.data, b.len, &back, NULL) == PC_OK);
    if (back.nlayers > 0 && back.layers[back.nlayers - 1].has_video) {
        const pc_video_data *v = &back.layers[back.nlayers - 1].video;
        float px[1][4];
        pc_rect r = {1, 1, 2, 2};
        CHECK_EQ_UINT(v->nframes, 3);
        CHECK(v->fps == 24.0f);
        CHECK(!v->show_altered);
        CHECK(v->source.is_file && pc_str_eq_c(&v->source.path, "/tmp/seq"));
        CHECK(back.has_timeline && back.timeline.duration == 3);
        /* A middle frame's pixel survived. */
        CHECK(v->nframes == 3 && pc_surface_read_rgba_into(&v->frames[2], r, px, 1, NULL) == PC_OK);
        CHECK(px[0][0] > 0.9f && px[0][2] < 0.1f);
    } else {
        PC_T_FAIL("video lost");
    }
    pc_document_fini(&back);
    pc_document_fini(&d);
    pc_buf_fini(&b);
}

/* Issue #1101: a non-finite float is refused at save (naming the value) rather than writing a
 * bundle that never opens again; finite documents are unaffected. */
static void test_a_non_finite_float_is_refused_at_save(void)
{
    static const char *const FIELD[3] = {"exposure", "gamma", "resolution_dpi"};
    for (int k = 0; k < 3; k++) {
        pc_document d;
        pc_buf b = PC_BUF_INIT;
        pc_error e;
        pc_status st;
        pc_pcraft_writer *w = NULL;
        t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, &d);
        if (k < 2) {
            pc_layer_content c;
            pc_layer l;
            memset(&c, 0, sizeof c);
            c.kind = PC_LAYER_ADJUSTMENT;
            T_JSON(pc_adjustment, "{\"Exposure\":{\"exposure\":0.0,\"offset\":0.0,\"gamma\":1.0}}",
                   &c.u.adjustment);
            if (k == 0) {
                c.u.adjustment.u.exposure.exposure = NAN;
            } else {
                c.u.adjustment.u.exposure.gamma = -INFINITY;
            }
            t_layer(&l, "adj", &c);
            t_push_layer(&d, &l);
        } else {
            d.resolution_dpi = INFINITY;
        }
        st = pc_format_save_to_bytes(&d, NULL, &b, &e);
        CHECK(pc_format_error_kind(st, &e) == PC_FORMAT_ERR_NON_FINITE);
        CHECK(strstr(e.msg, FIELD[k]) != NULL && strstr(e.msg, "NaN or infinite") != NULL);
        /* The incremental writer shares the check, for autosave and directory bundles. */
        CHECK(pc_pcraft_writer_new(&w, NULL) == PC_OK);
        st = pc_pcraft_writer_save_zip(w, &d, NULL, &b, NULL, &e);
        CHECK(pc_format_error_kind(st, &e) == PC_FORMAT_ERR_NON_FINITE);
        pc_pcraft_writer_free(w);
        pc_document_fini(&d);
        pc_buf_fini(&b);
    }
    {
        pc_document d;
        pc_buf b = PC_BUF_INIT;
        pc_layer_content c;
        pc_layer l;
        t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, &d);
        memset(&c, 0, sizeof c);
        c.kind = PC_LAYER_ADJUSTMENT;
        T_JSON(pc_adjustment, "{\"Exposure\":{\"exposure\":0.5,\"offset\":0.0,\"gamma\":1.0}}",
               &c.u.adjustment);
        t_layer(&l, "adj", &c);
        t_push_layer(&d, &l);
        save(&d, &b);
        CHECK(loads_equal(&b, &d));
        pc_document_fini(&d);
        pc_buf_fini(&b);
    }
}

/* ---- analysis.rs ---- */

static void strip_analysis(yyjson_mut_doc *d, yyjson_mut_val *doc)
{
    (void)d;
    (void)yyjson_mut_obj_remove_key(doc, "measurement");
    (void)yyjson_mut_obj_remove_key(doc, "notes");
}

/* Image > Analysis data and notes survive a .pcraft round trip; old manifests default them. */
static void test_measurement_and_notes_roundtrip_and_default_when_absent(void)
{
    pc_document d;
    pc_document back;
    pc_size size = {40, 30};
    pc_buf b = PC_BUF_INIT;
    pc_format_manifest *old;
    CHECK(pc_document_init(&d, "a", size, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, NULL) == PC_OK);
    pc_measurement_fini(&d.measurement);
    T_JSON(pc_measurement,
           "{\"scale\":{\"pixel_length\":72.0,\"logical_length\":1.0,\"units\":\"inches\"},\"count_groups\":"
           "[{\"name\":\"Cells\",\"color\":" T_COLOR(
               "[0.0,0.5,1.0,0.0]") ",\"marker_size\":3,\"label_size\":12,"
                                    "\"visible\":true,\"points\":[[1.0,2.0],[5.5,6.5]]}],\"active_count_"
                                    "group\":0,"
                                    "\"ruler\":{\"start\":[1.0,1.0],\"end\":[30.0,20.0],\"protractor\":[1.0,"
                                    "25.0]}}",
           &d.measurement);
    d.notes = calloc(1, sizeof *d.notes);
    if (d.notes != NULL) {
        pc_note_default(&d.notes[0]);
        t_str(&d.notes[0].author, "A");
        t_str(&d.notes[0].text, "Fix the sky\nplease");
        d.notes[0].position[0] = 10.0;
        d.notes[0].position[1] = 12.0;
        d.notes[0].popup[0] = 10.0;
        d.notes[0].popup[1] = 30.0;
        d.notes[0].popup[2] = 200.0;
        d.notes[0].popup[3] = 150.0;
        d.notes[0].open = true;
        d.nnotes = 1;
    }
    save(&d, &b);
    CHECK(pc_format_load_from_bytes(b.data, b.len, &back, NULL) == PC_OK);
    CHECK(pc_measurement_eq(&back.measurement, &d.measurement));
    CHECK(back.nnotes == 1 && pc_note_eq(&back.notes[0], &d.notes[0]));
    pc_document_fini(&back);
    old = decode_edited(&b, strip_analysis);
    CHECK(old != NULL && pc_measurement_is_empty(&old->document.measurement) && old->document.nnotes == 0);
    pc_format_manifest_free(old);
    pc_document_fini(&d);
    pc_buf_fini(&b);
}

/* ---- layer_labels.rs ---- */

static void test_layer_labels_pcraft_roundtrip_all_colors_and_depths(void)
{
    for (size_t k = 0; k < 3; k++) {
        pc_document d;
        pc_document back;
        pc_size size = {2, 2};
        pc_buf b = PC_BUF_INIT;
        CHECK(pc_document_init(&d, "Labels", size, PC_COLOR_MODE_RGB, DEPTHS[k], NULL) == PC_OK);
        for (size_t i = 0; i < PC_LABEL_COLOR_COUNT; i++) {
            pc_label_color color = PC_LABEL_COLOR_ALL[i];
            pc_layer *kids = calloc(1, sizeof *kids);
            pc_layer g;
            if (kids == NULL) {
                break;
            }
            CHECK(pc_layer_raster(&kids[0], "Child", pc_document_pixel_format(&d), NULL) == PC_OK);
            kids[0].label = color;
            CHECK(pc_layer_group(&g, pc_label_color_label(color), kids, 1, NULL) == PC_OK);
            g.label = color;
            t_push_layer(&d, &g);
        }
        save(&d, &b);
        CHECK(pc_format_load_from_bytes(b.data, b.len, &back, NULL) == PC_OK);
        CHECK(pc_document_eq(&back, &d));
        pc_document_fini(&back);
        CHECK(pc_format_load_from_bytes(b.data, b.len / 2, &back, NULL) != PC_OK);
        pc_document_fini(&d);
        pc_buf_fini(&b);
    }
}

int main(int argc, char **argv)
{
    RUN(test_zip_all_modes_and_depths);
    RUN(test_directory_bundle_roundtrip_all_modes);
    RUN(test_save_path_zip_file);
    RUN(test_deeply_nested_groups_roundtrip);
    RUN(test_nesting_beyond_the_load_limit_is_refused_at_save);
    RUN(test_empty_document_roundtrips);
    RUN(test_exhausted_document_ids_and_maximal_text_runs_roundtrip);
    RUN(test_zip_incremental_only_new_tiles);
    RUN(test_directory_incremental_and_gc);
    RUN(test_identical_tiles_are_stored_once);
    RUN(test_ids_preserved_and_counter_advanced);
    RUN(test_fresh_ids_option);
    RUN(test_previews_embedded_and_readable);
    RUN(test_manifest_is_human_readable_json);
    RUN(test_save_is_deterministic);
    RUN(test_not_pcraft);
    RUN(test_vector_fields_roundtrip_and_default_when_absent);
    RUN(test_text_kerning_roundtrips_and_defaults);
    RUN(test_channel_restrictions_and_bevel_elements_roundtrip);
    RUN(test_blend_if_roundtrips);
    RUN(test_advanced_blending_roundtrips_and_defaults_when_absent);
    RUN(test_video_layer_frames_survive_roundtrip);
    RUN(test_a_non_finite_float_is_refused_at_save);
    RUN(test_measurement_and_notes_roundtrip_and_default_when_absent);
    RUN(test_layer_labels_pcraft_roundtrip_all_colors_and_depths);
    return pc_t_finish();
}
