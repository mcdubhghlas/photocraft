/* Document -> PSD -> Document (crates/io/tests): roundtrip.rs (all 30 tests), psd32_export.rs
 * (32-bit layout; the corpus test runs with corpus/), psd_empty_channels.rs and
 * psd_block_layout.rs. The generators are checked against the reference first: t_gen_doc_all
 * must export to the bytes rustref io-export-vectors recorded for gen_doc(.., Features::ALL). */
#define GOLDEN "tests/golden/io-export-vectors.txt"
#include "harness.h"

#include <dirent.h>

#include "golden.h"
#include "psd_rt.h"
#include "../psd/testgen.h"
#include "io/effects_map.h"
#include "io/vector_map.h"
#include "psd/misc.h"

static const pc_color_mode MODES[4] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_GRAYSCALE, PC_COLOR_MODE_CMYK,
                                       PC_COLOR_MODE_LAB};
static const pc_sample_type DEPTHS[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};

static const char *mode_name(pc_color_mode m)
{
    return m == PC_COLOR_MODE_RGB         ? "Rgb"
           : m == PC_COLOR_MODE_GRAYSCALE ? "Grayscale"
                                          : pc_color_mode_name(m);
}

/* The generators build what the reference builds: the default PSD export of each has the
 * reference's raw-planes hash (the bundle-loaded documents of the parity test do too). */
static void test_generators_match_the_reference(void)
{
    if (load_golden() != 0) {
        PC_T_FAIL("cannot read %s", GOLDEN);
        return;
    }
    for (size_t m = 0; m < 4; m++) {
        for (size_t k = 0; k < 3; k++) {
            char name[64];
            const gline *l;
            pc_document d = t_gen_doc_all(MODES[m], DEPTHS[k]);
            pc_psd_file f;
            pc_psd_bytes b = {NULL, 0};
            char nh[64];
            (void)snprintf(name, sizeof name, "gen-%s-%s", mode_name(MODES[m]),
                           pc_sample_type_name(DEPTHS[k]));
            must(pc_io_document_to_psd(&d, &f, NULL));
            must(pc_psd_file_to_bytes(&f, &b, NULL));
            for (size_t i = 0; i < g_nlines; i++) {
                l = &g_lines[i];
                if (is_kind(l, "psd") && l->n == 5 && strcmp(l->f[1], name) == 0 &&
                    strcmp(l->f[2], "d") == 0) {
                    char h[17];
                    /* f[3] = "ok LEN HASH ZIP NHASH ...": byte-identical files need no zip */
                    (void)sscanf(l->f[3], "ok %*s %63s", nh);
                    if (DEPTHS[k] != PC_SAMPLE_TYPE_F32) {
                        h16(b.data, b.len, h);
                        if (strcmp(h, nh) != 0) {
                            PC_T_FAIL("%s: generator differs from the reference (%s vs %s)", name, h, nh);
                        }
                        pc_t_checks++;
                    }
                }
            }
            pc_psd_bytes_fini(&b);
            pc_psd_file_fini(&f);
            pc_document_fini(&d);
        }
    }
}

/* rt_case!: gen_doc -> PSD -> doc keeps everything assert_docs_eq looks at. */
static void rt_case(pc_color_mode mode, pc_sample_type depth, bool all)
{
    pc_document d = all ? t_gen_doc_all(mode, depth) : t_gen_doc(mode, depth);
    pc_document back = rt_roundtrip(&d);
    rt_docs_eq(&d, &back);
    pc_document_fini(&back);
    pc_document_fini(&d);
}

static void test_rt_cases(void)
{
    for (size_t m = 0; m < 4; m++) {
        for (size_t k = 0; k < 3; k++) {
            if (MODES[m] == PC_COLOR_MODE_CMYK && DEPTHS[k] == PC_SAMPLE_TYPE_F32) {
                continue; /* the reference has no 32-bit CMYK/Lab cases */
            }
            if (MODES[m] == PC_COLOR_MODE_LAB && DEPTHS[k] == PC_SAMPLE_TYPE_F32) {
                continue;
            }
            rt_case(MODES[m], DEPTHS[k], true);
        }
    }
    rt_case(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false);
    rt_case(PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U16, false);
    rt_case(PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, false);
}

static void test_psb_roundtrip_and_option(void)
{
    pc_document d = t_gen_doc_all(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_buf b = rt_export(&d, "x.psb", NULL);
    pc_io_export_opts o = pc_io_export_opts_default();
    pc_document back;
    CHECK(b.data[5] == 2);
    back = rt_import("x.psb", &b);
    rt_docs_eq(&d, &back);
    pc_document_fini(&back);
    pc_buf_fini(&b);
    o.force_psb = true;
    b = rt_export(&d, "x.psd", &o);
    CHECK(b.data[5] == 2);
    /* import_sets_name */
    back = rt_import("hello.psd", &b);
    CHECK(pc_str_eq_c(&back.name, "hello.psd"));
    pc_document_fini(&back);
    pc_buf_fini(&b);
    pc_document_fini(&d);
}

static void test_empty_and_zero_sized_documents(void)
{
    pc_document e = t_doc("e", 4, 3, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_document back = rt_roundtrip(&e);
    pc_document d = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_document z = t_doc("e", 0, 3, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_buf good = rt_export(&d, "x.psd", NULL);
    static const size_t RANGES[3][2] = {{14, 18}, {18, 22}, {14, 22}};
    pc_io_export_result r;
    /* A flattened, fully transparent file comes back as one background layer at most. */
    CHECK(back.nlayers <= 1);
    CHECK(back.size.width == 4 && back.size.height == 3);
    pc_document_fini(&back);
    /* A header declaring a zero width or height is corrupt; a zero-sized document is never
     * written. */
    for (size_t i = 0; i < 3; i++) {
        pc_buf b = PC_BUF_INIT;
        pc_io_import_result ir;
        pc_error err;
        must(pc_buf_append(&b, good.data, good.len, NULL));
        memset(b.data + RANGES[i][0], 0, RANGES[i][1] - RANGES[i][0]);
        CHECK(pc_io_import("z.psd", b.data, b.len, &ir, &err) != PC_OK);
        CHECK_CONTAINS(err.msg, "at least 1x1");
        pc_buf_fini(&b);
    }
    CHECK(pc_io_export(&z, "x.psd", NULL, &r, NULL) != PC_OK);
    pc_buf_fini(&good);
    pc_document_fini(&z);
    pc_document_fini(&d);
    pc_document_fini(&e);
}

/* The first export renders fill layers itself; after import they carry those pixels as their
 * fill cache, so from the second generation on the bytes are identical. */
static void test_round_trip_reaches_a_fixed_point(void)
{
    pc_document d = t_gen_doc_all(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16);
    pc_buf a = rt_export(&d, "a.psd", NULL);
    pc_document da = rt_import("a.psd", &a);
    pc_buf b = rt_export(&da, "a.psd", NULL);
    pc_document db = rt_import("b.psd", &b);
    pc_buf c = rt_export(&db, "a.psd", NULL);
    pc_psd_file fa;
    pc_psd_file fb;
    CHECK(b.len == c.len && memcmp(b.data, c.data, b.len) == 0);
    must(pc_psd_file_parse(a.data, a.len, &fa, NULL));
    must(pc_psd_file_parse(b.data, b.len, &fb, NULL));
    CHECK(pc_psd_layer_info_eq(&fa.layer_info, &fb.layer_info));
    pc_psd_file_fini(&fa);
    pc_psd_file_fini(&fb);
    pc_buf_fini(&a);
    pc_buf_fini(&b);
    pc_buf_fini(&c);
    pc_document_fini(&da);
    pc_document_fini(&db);
    pc_document_fini(&d);
}

static void set_block(pc_layer *l, size_t i, const char *key, const void *data, size_t len)
{
    memcpy(l->psd_blocks[i].key, key, 4);
    must(pc_blob_new(data, len, &l->psd_blocks[i].data, NULL));
}

static pc_layer_content smart_content(pc_surface *cache, const uint8_t *raw, size_t len)
{
    pc_layer_content c;
    pc_smart_source src;
    memset(&src, 0, sizeof src);
    src.linked = true;
    c.kind = PC_LAYER_SMART;
    pc_smart_object_init(&c.u.smart, &src, (pc_affine){{1, 0, 0, 1, 0, 0}}, cache);
    must(pc_blob_new(raw, len, &c.u.smart.psd_raw, NULL));
    return c;
}

static void test_text_shape_smart_raw_blocks_survive(void)
{
    static const uint8_t PLACED[4] = {1, 2, 3, 4};
    static const uint8_t V5[8] = {5, 5, 5, 5, 5, 5, 5, 5};
    static const uint8_t V7[8] = {7, 7, 7, 7, 7, 7, 7, 7};
    static const uint8_t Z8[8] = {0};
    pc_document d = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_pixel_format fmt = pc_document_pixel_format(&d);
    pc_surface cache = t_pattern(fmt, pc_rect_make(1, 1, 6, 4), 42, true);
    pc_layer_content c = smart_content(&cache, PLACED, 4);
    pc_layer smart;
    pc_layer shape;
    pc_document back;
    bool found = false;
    uint32_t flags = 0;
    must(pc_layer_init(&smart, "Smart", &c, NULL));
    smart.psd_blocks = xmalloc(3 * sizeof *smart.psd_blocks);
    set_block(&smart, 0, "PlLd", Z8, 4);
    set_block(&smart, 1, "vmsk", V5, 8);
    set_block(&smart, 2, "luni", Z8, 8);
    smart.npsd_blocks = 3;
    /* The typed vector mask (as import produces it) keeps the raw block while unchanged. */
    must(pc_io_vector_mask_from_block(V5, 8, d.size.width, d.size.height, &found, &smart.vector_mask, NULL));
    smart.has_vector_mask = found;
    t_push(&d, &smart);
    cache = t_pattern(fmt, pc_rect_make(1, 1, 6, 4), 42, true);
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_SHAPE;
    c.u.shape.has_cache = true;
    c.u.shape.cache = cache;
    must(pc_io_path_from_vmsk(V7, 8, d.size.width, d.size.height, &found, &c.u.shape.path, &flags, NULL));
    must(pc_layer_init(&shape, "Shape", &c, NULL));
    shape.psd_blocks = xmalloc(2 * sizeof *shape.psd_blocks);
    set_block(&shape, 0, "vscg", PLACED, 4);
    set_block(&shape, 1, "vmsk", V7, 8);
    shape.npsd_blocks = 2;
    t_push(&d, &shape);
    back = rt_roundtrip(&d);
    {
        size_t n = back.nlayers;
        const pc_layer *bs = &back.layers[n - 2];
        const pc_layer *bsh = &back.layers[n - 1];
        bool has_placed = false;
        bool has_vmsk = false;
        bool has_luni = false;
        CHECK(bs->content.kind == PC_LAYER_SMART);
        CHECK(bsh->content.kind == PC_LAYER_SHAPE);
        for (size_t i = 0; i < bs->npsd_blocks; i++) {
            const pc_doc_psd_block *b = &bs->psd_blocks[i];
            has_placed = has_placed || (memcmp(b->key, "PlLd", 4) == 0 && pc_blob_len(b->data) == 4 &&
                                        memcmp(pc_blob_data(b->data), PLACED, 4) == 0);
            has_vmsk = has_vmsk || memcmp(b->key, "vmsk", 4) == 0;
            has_luni = has_luni || memcmp(b->key, "luni", 4) == 0;
        }
        /* psd_raw overrides the PlLd entry; the stale luni is regenerated, not kept. */
        CHECK(has_placed && has_vmsk && !has_luni);
        if (bs->content.kind == PC_LAYER_SMART) {
            CHECK(pc_blob_len(bs->content.u.smart.psd_raw) == 4 &&
                  memcmp(pc_blob_data(bs->content.u.smart.psd_raw), PLACED, 4) == 0);
        }
        if (bsh->content.kind == PC_LAYER_SHAPE) {
            CHECK(pc_blob_len(bsh->content.u.shape.psd_raw) == 8 &&
                  memcmp(pc_blob_data(bsh->content.u.shape.psd_raw), V7, 8) == 0);
        }
        CHECK_EQ_UINT(bsh->npsd_blocks, 2);
        for (size_t i = 0; i < bsh->npsd_blocks && i < 2; i++) {
            CHECK(rt_has_block(bsh, &d.layers[n - 1].psd_blocks[i]));
        }
    }
    pc_document_fini(&back);
    pc_document_fini(&d);
}

static void test_raster_layer_blocks_preserved(void)
{
    static const uint8_t Z12[12] = {0};
    static const uint8_t ONE = 1;
    pc_document d = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_layer *l = &d.layers[1];
    pc_document back;
    pc_document back2;
    bool found = false;
    l->psd_blocks = xmalloc(3 * sizeof *l->psd_blocks);
    set_block(l, 0, "vmsk", Z12, 12);
    set_block(l, 1, "clbl", Z12, 4);
    set_block(l, 2, "Zzzz", &ONE, 1);
    l->npsd_blocks = 3;
    /* clbl is written from the field (in place), so the field agrees with the preserved block. */
    l->advanced = pc_advanced_blending_default();
    l->advanced.blend_clipped = false;
    must(pc_io_vector_mask_from_block(Z12, 12, d.size.width, d.size.height, &found, &l->vector_mask, NULL));
    l->has_vector_mask = found;
    back = rt_roundtrip(&d);
    /* An odd-length layer block comes back with its pad byte inside the length. */
    {
        const pc_layer *b = &back.layers[1];
        CHECK_EQ_UINT(b->npsd_blocks, 3);
        if (b->npsd_blocks == 3) {
            CHECK(memcmp(b->psd_blocks[0].key, "vmsk", 4) == 0 &&
                  pc_blob_eq(b->psd_blocks[0].data, l->psd_blocks[0].data));
            CHECK(memcmp(b->psd_blocks[1].key, "clbl", 4) == 0 &&
                  pc_blob_eq(b->psd_blocks[1].data, l->psd_blocks[1].data));
            CHECK(memcmp(b->psd_blocks[2].key, "Zzzz", 4) == 0 && pc_blob_len(b->psd_blocks[2].data) == 2 &&
                  pc_blob_data(b->psd_blocks[2].data)[0] == 1 && pc_blob_data(b->psd_blocks[2].data)[1] == 0);
        }
        CHECK(b->has_vector_mask == l->has_vector_mask &&
              (!b->has_vector_mask || pc_vector_mask_eq(&b->vector_mask, &l->vector_mask)));
        back2 = rt_roundtrip(&back);
        CHECK_EQ_UINT(back2.layers[1].npsd_blocks, 3);
        for (size_t i = 0; i < back2.layers[1].npsd_blocks && i < 3; i++) {
            CHECK(rt_has_block(&back2.layers[1], &b->psd_blocks[i]));
        }
        pc_document_fini(&back2);
    }
    pc_document_fini(&back);
    pc_document_fini(&d);
}

static void test_psd_ids_are_kept_and_deduplicated(void)
{
    pc_document d = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_document back;
    pc_walk w;
    d.layers[0].has_psd_id = true;
    d.layers[0].psd_id = 500;
    d.layers[1].has_psd_id = true;
    d.layers[1].psd_id = 500; /* duplicate: the second gets a fresh id */
    d.layers[2].has_psd_id = true;
    d.layers[2].psd_id = 3;
    back = rt_roundtrip(&d);
    CHECK(back.layers[0].has_psd_id && back.layers[0].psd_id == 500);
    CHECK(!back.layers[1].has_psd_id || back.layers[1].psd_id != 500);
    CHECK(back.layers[2].has_psd_id && back.layers[2].psd_id == 3);
    must(pc_document_walk(&back, &w, NULL));
    for (size_t i = 0; i < w.len; i++) {
        for (size_t j = i + 1; j < w.len; j++) {
            const pc_layer *a = w.items[i].layer;
            const pc_layer *b = w.items[j].layer;
            if (a->has_psd_id && b->has_psd_id && a->psd_id == b->psd_id) {
                PC_T_FAIL("ids not unique: %u", a->psd_id);
            }
        }
    }
    pc_walk_fini(&w);
    pc_document_fini(&back);
    pc_document_fini(&d);
}

static pc_layer adjustment_layer(const char *name, pc_adjustment *a)
{
    pc_layer_content c;
    pc_layer l;
    c.kind = PC_LAYER_ADJUSTMENT;
    c.u.adjustment = *a;
    must(pc_layer_init(&l, name, &c, NULL));
    return l;
}

static void test_edited_adjustment_regenerates_block_unedited_keeps_raw(void)
{
    pc_document d = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_adjustment base;
    pc_layer l;
    pc_psd_file f;
    const pc_psd_block *b;
    pc_buf raw = PC_BUF_INIT;
    pc_adjustment_identity_levels(&base);
    l = adjustment_layer("lv", &base);
    {
        pc_document t;
        pc_layer copy;
        must(pc_document_clone(&d, &t, NULL));
        must(pc_layer_clone(&l, &copy, NULL));
        t_push(&t, &copy);
        must(pc_io_document_to_psd(&t, &f, NULL));
        b = pc_psd_layer_block(rt_record(&f, "lv"), "levl");
        CHECK(b != NULL);
        must(pc_buf_append(&raw, b->data.data, b->data.len, NULL));
        pc_psd_file_fini(&f);
        pc_document_fini(&t);
    }
    raw.data[2 + 5 * 10 + 1] = 7; /* record 5 in_black = 7 (not modelled) */
    l.psd_blocks = xmalloc(sizeof *l.psd_blocks);
    set_block(&l, 0, "levl", raw.data, raw.len);
    l.npsd_blocks = 1;
    t_push(&d, &l);
    must(pc_io_document_to_psd(&d, &f, NULL));
    CHECK(rt_block_is(pc_psd_layer_block(rt_record(&f, "lv"), "levl"), raw.data, raw.len)); /* unchanged */
    pc_psd_file_fini(&f);
    d.layers[d.nlayers - 1].content.u.adjustment.u.levels.master.in_black = 10.0f / 255.0f;
    must(pc_io_document_to_psd(&d, &f, NULL));
    b = pc_psd_layer_block(rt_record(&f, "lv"), "levl");
    CHECK(b != NULL && !rt_block_is(b, raw.data, raw.len)); /* edited: regenerated */
    CHECK(b != NULL && b->data.len > 3 && b->data.data[3] == 10);
    pc_psd_file_fini(&f);
    pc_buf_fini(&raw);
    pc_document_fini(&d);
}

static void test_text_layer_roundtrip_with_tysh(void)
{
    pc_buf tysh = PC_BUF_INIT;
    uint8_t t8[8];
    static const double M[6] = {1.0, 0.0, 0.0, 1.0, 3.0, 4.0};
    pc_psd_desc text;
    pc_psd_desc warp;
    pc_psd_value v;
    pc_psd_bytes vb = {NULL, 0};
    pc_document d = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_layer_content c;
    pc_layer l;
    pc_document back;
    must(pc_buf_append(&tysh, "\0\1", 2, NULL));
    for (int i = 0; i < 6; i++) {
        pc_store_be64(t8, pc_f64_to_bits(M[i]));
        must(pc_buf_append(&tysh, t8, 8, NULL));
    }
    must(pc_buf_append(&tysh, "\0\x32", 2, NULL));
    must(pc_psd_desc_init(&text, "TxLr", NULL));
    must(pc_psd_value_text("Hello", true, &v, NULL));
    must(pc_psd_desc_add(&text, "Txt ", &v, NULL));
    must(pc_psd_vdesc_to_bytes(&text, &vb, NULL));
    must(pc_buf_append(&tysh, vb.data, vb.len, NULL));
    pc_psd_bytes_fini(&vb);
    must(pc_buf_append(&tysh, "\0\1", 2, NULL));
    must(pc_psd_desc_init(&warp, "warp", NULL));
    must(pc_psd_vdesc_to_bytes(&warp, &vb, NULL));
    must(pc_buf_append(&tysh, vb.data, vb.len, NULL));
    pc_psd_bytes_fini(&vb);
    {
        static const uint8_t Z16[16] = {0};
        must(pc_buf_append(&tysh, Z16, 16, NULL));
    }
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_TEXT;
    pc_text_layer_default(&c.u.text);
    must(pc_str_set_c(&c.u.text.text, "Hello", NULL));
    c.u.text.size_pt = 0.0f;
    memcpy(c.u.text.transform.m, M, sizeof M);
    c.u.text.has_cache = true;
    c.u.text.cache = t_pattern(pc_document_pixel_format(&d), pc_rect_make(3, 4, 9, 8), 5, true);
    must(pc_blob_new(tysh.data, tysh.len, &c.u.text.psd_raw, NULL));
    must(pc_layer_init(&l, "Hello", &c, NULL));
    t_push(&d, &l);
    back = rt_roundtrip(&d);
    {
        const pc_layer *t = &back.layers[back.nlayers - 1];
        CHECK(t->content.kind == PC_LAYER_TEXT);
        if (t->content.kind == PC_LAYER_TEXT) {
            CHECK(pc_str_eq_c(&t->content.u.text.text, "Hello"));
            CHECK(memcmp(t->content.u.text.transform.m, M, sizeof M) == 0);
        }
        rt_layers_eq(&d.layers[d.nlayers - 1], t, "", 0);
    }
    pc_document_fini(&back);
    pc_psd_desc_fini(&text);
    pc_psd_desc_fini(&warp);
    pc_buf_fini(&tysh);
    pc_document_fini(&d);
}

static void test_effects_raw_roundtrip(void)
{
    pc_document d = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_psd_desc s = tg_sample_descriptor();
    pc_psd_bytes vb = {NULL, 0};
    pc_buf lfx = PC_BUF_INIT;
    bool found = false;
    bool master = false;
    pc_effect *items = NULL;
    size_t n = 0;
    pc_document back;
    must(pc_buf_append(&lfx, "\0\0\0\0", 4, NULL));
    must(pc_psd_vdesc_to_bytes(&s, &vb, NULL));
    must(pc_buf_append(&lfx, vb.data, vb.len, NULL));
    /* Typed items and the raw block must agree; the raw bytes are then kept. */
    must(pc_io_parse_lfx2(lfx.data, lfx.len, &found, &master, &items, &n, NULL));
    CHECK(found);
    d.layers[1].effects.enabled = master;
    d.layers[1].effects.items = items;
    d.layers[1].effects.nitems = n;
    must(pc_blob_new(lfx.data, lfx.len, &d.layers[1].effects.psd_raw, NULL));
    back = rt_roundtrip(&d);
    rt_docs_eq(&d, &back);
    pc_document_fini(&back);
    pc_buf_fini(&lfx);
    pc_psd_bytes_fini(&vb);
    pc_psd_desc_fini(&s);
    pc_document_fini(&d);
}

static void test_gradient_and_pattern_fills_roundtrip(void)
{
    pc_document d = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_layer_content c;
    pc_layer l;
    pc_document back;
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_FILL;
    c.u.fill.kind = PC_FILL_GRADIENT;
    c.u.fill.u.gradient.stops = xmalloc(2 * sizeof *c.u.fill.u.gradient.stops);
    c.u.fill.u.gradient.stops[0].pos = 0.0f;
    c.u.fill.u.gradient.stops[0].color = pc_color_rgb(1, 0, 0);
    c.u.fill.u.gradient.stops[1].pos = 1.0f;
    c.u.fill.u.gradient.stops[1].color = pc_color_rgb(0, 0, 1);
    c.u.fill.u.gradient.nstops = 2;
    c.u.fill.u.gradient.angle = 30.0f;
    c.u.fill.u.gradient.scale = 1.0f;
    c.u.fill.u.gradient.style = PC_GRADIENT_DIAMOND;
    c.u.fill.u.gradient.reverse = true;
    c.u.fill.u.gradient.align = true;
    must(pc_layer_init(&l, "grad", &c, NULL));
    t_push(&d, &l);
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_FILL;
    c.u.fill.kind = PC_FILL_PATTERN;
    must(pc_str_set_c(&c.u.fill.u.pattern.name, "Dots", NULL));
    c.u.fill.u.pattern.scale = 0.5f;
    c.u.fill.u.pattern.link = true;
    must(pc_layer_init(&l, "pat", &c, NULL));
    t_push(&d, &l);
    back = rt_roundtrip(&d);
    rt_docs_eq(&d, &back);
    pc_document_fini(&back);
    pc_document_fini(&d);
}

static void test_psd_raw_resources_and_global_blocks_preserved(void)
{
    pc_document d = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_document back;
    static const uint8_t SEV[6] = {7, 7, 7, 7, 7, 7};
    d.metadata.psd_resources = xmalloc(sizeof *d.metadata.psd_resources);
    memset(d.metadata.psd_resources, 0, sizeof *d.metadata.psd_resources);
    d.metadata.psd_resources[0].id = 4001;
    must(pc_str_set_c(&d.metadata.psd_resources[0].name, "named", NULL));
    must(pc_blob_new(SEV, 1, &d.metadata.psd_resources[0].data, NULL));
    d.metadata.npsd_resources = 1;
    d.metadata.psd_global_blocks = xmalloc(2 * sizeof *d.metadata.psd_global_blocks);
    memcpy(d.metadata.psd_global_blocks[0].signature, "8BIM", 4);
    memcpy(d.metadata.psd_global_blocks[0].key, "Patt", 4);
    must(pc_blob_new(SEV, 6, &d.metadata.psd_global_blocks[0].data, NULL));
    memcpy(d.metadata.psd_global_blocks[1].signature, "8B64", 4);
    memcpy(d.metadata.psd_global_blocks[1].key, "lnk2", 4);
    must(pc_blob_new((const uint8_t[2]){1, 2}, 2, &d.metadata.psd_global_blocks[1].data, NULL));
    d.metadata.npsd_global_blocks = 2;
    d.metadata.psd_resources[0].data = d.metadata.psd_resources[0].data; /* [1] in the reference */
    back = rt_roundtrip(&d);
    /* A PSD holds no 8B64 blocks (Photoshop refuses the file): a kept PSB block is written as
     * 8BIM. Compare against the document with that one signature changed. */
    memcpy(d.metadata.psd_global_blocks[1].signature, "8BIM", 4);
    CHECK(pc_metadata_eq(&back.metadata, &d.metadata));
    pc_document_fini(&back);
    pc_document_fini(&d);
}

static void test_document_to_psd_direct_api_and_big_psb(void)
{
    pc_document d = t_gen_doc_all(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_document big = t_doc("big", 30001, 1, PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8);
    pc_psd_file f;
    pc_document d2;
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    must(pc_io_document_to_psd(&d, &f, NULL));
    CHECK_EQ_UINT(f.header.width, 24);
    must(pc_io_psd_to_document(&f, &d2, &w, NULL));
    rt_docs_eq(&d, &d2);
    pc_document_fini(&d2);
    pc_psd_file_fini(&f);
    pc_io_warnings_fini(&w);
    must(pc_io_document_to_psd_with(&big, NULL, &f, &w, NULL));
    CHECK(f.header.version == PC_PSD_V2);
    CHECK(pc_io_warnings_any(&w, "PSB"));
    pc_psd_file_fini(&f);
    pc_io_warnings_fini(&w);
    pc_document_fini(&big);
    pc_document_fini(&d);
}

static pc_effect stroke(float opacity, float size, pc_stroke_position pos, pc_color c)
{
    pc_effect e;
    memset(&e, 0, sizeof e);
    e.kind = PC_FX_STROKE;
    e.u.stroke.common = pc_fx_common_new(PC_BLEND_NORMAL, opacity);
    e.u.stroke.size = size;
    e.u.stroke.position = pos;
    e.u.stroke.paint.kind = PC_FX_PAINT_COLOR;
    e.u.stroke.paint.u.color = c;
    return e;
}

static pc_effect color_overlay(pc_color c)
{
    pc_effect e;
    memset(&e, 0, sizeof e);
    e.kind = PC_FX_COLOR_OVERLAY;
    e.u.color_overlay.common = pc_fx_common_new(PC_BLEND_NORMAL, 1.0f);
    e.u.color_overlay.color = c;
    return e;
}

static void test_effects_roundtrip_typed_and_raw(void)
{
    pc_document d = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_document back;
    pc_document edited;
    pc_psd_file f;
    pc_layer *l = &d.layers[1];
    const pc_blob *raw;
    const pc_psd_block *b;
    char *name;
    d.global_light.angle = 45.0f;
    d.global_light.altitude = 25.0f;
    l->effects.items = xmalloc(3 * sizeof *l->effects.items);
    pc_effect_default_drop_shadow(&l->effects.items[0]);
    l->effects.items[1] = stroke(1.0f, 3.0f, PC_STROKE_CENTER, pc_color_rgb(0, 1, 0));
    l->effects.items[2] = stroke(0.5f, 6.0f, PC_STROKE_OUTSIDE, pc_color_rgb(0, 0, 1));
    l->effects.nitems = 3;
    back = rt_roundtrip(&d);
    CHECK(back.global_light.angle == 45.0f && back.global_light.altitude == 25.0f);
    CHECK_EQ_UINT(back.layers[1].effects.nitems, 3);
    raw = back.layers[1].effects.psd_raw;
    CHECK(raw != NULL);
    name = pc_strndup(pc_str_c(&back.layers[1].name), back.layers[1].name.len);
    /* Unchanged: the same lfx2 bytes are written again. */
    must(pc_io_document_to_psd(&back, &f, NULL));
    b = pc_psd_layer_block(rt_record(&f, name), "lfx2");
    CHECK(rt_block_is(b, pc_blob_data(raw), pc_blob_len(raw)));
    pc_psd_file_fini(&f);
    /* Edited: regenerated. */
    must(pc_document_clone(&back, &edited, NULL));
    CHECK(edited.layers[1].effects.items[0].kind == PC_FX_DROP_SHADOW);
    edited.layers[1].effects.items[0].u.shadow.distance = 20.0f;
    must(pc_io_document_to_psd(&edited, &f, NULL));
    b = pc_psd_layer_block(rt_record(&f, name), "lfx2");
    CHECK(b != NULL && !rt_block_is(b, pc_blob_data(raw), pc_blob_len(raw)));
    if (b != NULL) {
        bool found = false;
        bool master = false;
        pc_effect *items = NULL;
        size_t n = 0;
        must(pc_io_parse_lfx2(b->data.data, b->data.len, &found, &master, &items, &n, NULL));
        CHECK(found && n >= 1 && items[0].kind == PC_FX_DROP_SHADOW && items[0].u.shadow.distance == 20.0f);
        for (size_t i = 0; i < n; i++) {
            pc_effect_fini(&items[i]);
        }
        free(items);
    }
    pc_psd_file_fini(&f);
    free(name);
    pc_document_fini(&edited);
    pc_document_fini(&back);
    pc_document_fini(&d);
}

static void test_group_effects_use_lfxs_and_clearing_drops_blocks(void)
{
    pc_document d = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    size_t n = 0;
    pc_psd_file f;
    pc_document back;
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    while (n < d.nlayers && !pc_layer_is_group(&d.layers[n])) {
        n++;
    }
    d.layers[n].effects.items = xmalloc(sizeof *d.layers[n].effects.items);
    d.layers[n].effects.items[0] = color_overlay(pc_color_rgb(1, 0, 0));
    d.layers[n].effects.nitems = 1;
    must(pc_io_document_to_psd(&d, &f, NULL));
    {
        bool ok = false;
        for (size_t i = 0; i < f.layer_info.n; i++) {
            const pc_psd_layer *r = &f.layer_info.layers[i];
            uint32_t st = pc_psd_layer_section_type(r);
            if ((st == PC_PSD_SECTION_OPEN || st == PC_PSD_SECTION_CLOSED) && r == rt_record(&f, "Outer")) {
                ok = pc_psd_layer_block(r, "lfxs") != NULL && pc_psd_layer_block(r, "lfx2") == NULL;
            }
        }
        CHECK(ok);
    }
    must(pc_io_psd_to_document(&f, &back, &w, NULL));
    CHECK_EQ_UINT(back.layers[n].effects.nitems, 1);
    pc_document_fini(&back);
    pc_psd_file_fini(&f);
    pc_io_warnings_fini(&w);
    /* clearing_effects_drops_stale_blocks */
    pc_effects_fini(&d.layers[n].effects);
    d.layers[1].effects.items = xmalloc(sizeof *d.layers[1].effects.items);
    pc_effect_default_drop_shadow(&d.layers[1].effects.items[0]);
    d.layers[1].effects.nitems = 1;
    back = rt_roundtrip(&d);
    pc_effects_fini(&back.layers[1].effects);
    back.layers[1].effects.enabled = true;
    must(pc_io_document_to_psd(&back, &f, NULL));
    {
        char *name = pc_strndup(pc_str_c(&back.layers[1].name), back.layers[1].name.len);
        CHECK(pc_psd_layer_block(rt_record(&f, name), "lfx2") == NULL);
        free(name);
    }
    pc_psd_file_fini(&f);
    pc_document_fini(&back);
    pc_document_fini(&d);
}

static pc_layer red_raster(const char *name, pc_pixel_format fmt)
{
    static const float RED[4] = {1, 0, 0, 1};
    pc_layer l;
    must(pc_layer_raster(&l, name, fmt, NULL));
    must(pc_surface_fill_rect(&l.content.u.raster, pc_rect_make(0, 0, 4, 4), RED, 4, NULL));
    return l;
}

static pc_layer group_of(const char *name, pc_layer child)
{
    pc_layer *kids = xmalloc(sizeof *kids);
    pc_layer g;
    kids[0] = child;
    must(pc_layer_group(&g, name, kids, 1, NULL));
    return g;
}

static void res1026(const pc_document *d, pc_buf *out)
{
    pc_psd_file f;
    const pc_psd_resource *r;
    pc_buf_truncate(out, 0);
    must(pc_io_document_to_psd(d, &f, NULL));
    r = pc_psd_file_resource(&f, 1026);
    if (r != NULL) {
        must(pc_buf_append(out, r->data.data, r->data.len, NULL));
    }
    pc_psd_file_fini(&f);
}

static void test_link_groups_and_effects_reference_round_trip(void)
{
    pc_document d = t_doc("l", 8, 8, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_pixel_format fmt = pc_document_pixel_format(&d);
    pc_layer a = red_raster("a", fmt);
    pc_layer b;
    pc_layer inner;
    pc_layer g;
    pc_document back;
    pc_buf r0 = PC_BUF_INIT;
    pc_buf r1 = PC_BUF_INIT;
    static const uint8_t WANT0[10] = {0, 7, 0, 7, 0, 0, 0, 3, 0, 0};
    static const uint8_t WANTB[10] = {0, 1, 0, 1, 0, 0, 0, 2, 0, 0};
    a.has_link_group = true;
    a.link_group = 7;
    a.effects.has_reference = true;
    a.effects.reference[0] = -201.0;
    a.effects.reference[1] = 47.5;
    must(pc_layer_raster(&b, "b", fmt, NULL));
    b.has_link_group = true;
    b.link_group = 7;
    must(pc_layer_raster(&inner, "inner", fmt, NULL));
    inner.has_link_group = true;
    inner.link_group = 3;
    g = group_of("g", inner);
    t_push(&d, &a);
    t_push(&d, &b);
    t_push(&d, &g);
    back = rt_roundtrip(&d);
    CHECK(back.layers[0].has_link_group && back.layers[0].link_group == 7);
    CHECK(back.layers[1].has_link_group && back.layers[1].link_group == 7);
    CHECK(!back.layers[2].has_link_group);
    CHECK(back.layers[2].content.u.group.nchildren == 1 &&
          back.layers[2].content.u.group.children[0].has_link_group &&
          back.layers[2].content.u.group.children[0].link_group == 3);
    CHECK(back.layers[0].effects.has_reference && back.layers[0].effects.reference[0] == -201.0 &&
          back.layers[0].effects.reference[1] == 47.5);
    CHECK(!back.layers[1].effects.has_reference);
    /* Unchanged documents write the same resource bytes again. */
    res1026(&back, &r1);
    res1026(&d, &r0);
    CHECK(r0.len == r1.len && memcmp(r0.data, r1.data, r0.len) == 0);
    /* One u16 per record: a, b, divider, inner, group (bottom first). */
    CHECK(r0.len == 10 && memcmp(r0.data, WANT0, 10) == 0);
    /* Large engine ids are renumbered by first appearance. */
    d.layers[0].link_group = (uint64_t)1 << 40;
    d.layers[1].link_group = (uint64_t)1 << 40;
    res1026(&d, &r0);
    CHECK(r0.len == 10 && memcmp(r0.data, WANTB, 10) == 0);
    /* Unlinked documents write no 1026. */
    d.layers[0].has_link_group = false;
    d.layers[1].has_link_group = false;
    d.layers[2].content.u.group.children[0].has_link_group = false;
    res1026(&d, &r0);
    CHECK_EQ_UINT(r0.len, 0);
    pc_buf_fini(&r0);
    pc_buf_fini(&r1);
    pc_document_fini(&back);
    pc_document_fini(&d);
}

static void test_channel_restrictions_round_trip_as_brst(void)
{
    static const uint8_t WANT[8] = {0, 0, 0, 1, 0, 0, 0, 2};
    pc_document d = t_doc("c", 8, 8, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_layer a = red_raster("a", pc_document_pixel_format(&d));
    pc_layer b;
    pc_psd_file f;
    pc_document back;
    a.excluded_channels = 6;
    must(pc_layer_raster(&b, "b", pc_document_pixel_format(&d), NULL));
    t_push(&d, &a);
    t_push(&d, &b);
    must(pc_io_document_to_psd(&d, &f, NULL));
    CHECK(rt_block_is(pc_psd_layer_block(rt_record(&f, "a"), "brst"), WANT, 8));
    CHECK(pc_psd_layer_block(rt_record(&f, "b"), "brst") == NULL);
    pc_psd_file_fini(&f);
    back = rt_roundtrip(&d);
    CHECK(back.layers[0].excluded_channels == 6 && back.layers[1].excluded_channels == 0);
    /* Cleared restrictions drop the preserved block. */
    back.layers[0].excluded_channels = 0;
    must(pc_io_document_to_psd(&back, &f, NULL));
    CHECK(pc_psd_layer_block(rt_record(&f, "a"), "brst") == NULL);
    pc_psd_file_fini(&f);
    pc_document_fini(&back);
    pc_document_fini(&d);
}

static void test_advanced_blending_round_trips_as_tagged_blocks(void)
{
    static const char *const KEYS[6] = {"knko", "infx", "clbl", "tsly", "lmgm", "vmgm"};
    static const uint8_t VALS[6] = {2, 1, 0, 0, 1, 1};
    for (size_t k = 0; k < 3; k++) {
        pc_document d = t_doc("a", 8, 8, PC_COLOR_MODE_RGB, DEPTHS[k]);
        pc_pixel_format fmt = pc_document_pixel_format(&d);
        pc_layer a = red_raster("a", fmt);
        pc_layer b;
        pc_layer c;
        pc_layer g;
        pc_advanced_blending adv = {PC_KNOCKOUT_DEEP, true, false, false, true, true};
        pc_psd_file f;
        pc_document back;
        pc_document again;
        a.advanced = adv;
        must(pc_layer_raster(&c, "c", fmt, NULL));
        g = group_of("g", c);
        g.advanced.knockout = PC_KNOCKOUT_SHALLOW;
        g.fill_opacity = 128.0f / 255.0f;
        must(pc_layer_raster(&b, "b", fmt, NULL));
        t_push(&d, &a);
        t_push(&d, &b);
        t_push(&d, &g);
        must(pc_io_document_to_psd(&d, &f, NULL));
        for (size_t i = 0; i < 6; i++) {
            const uint8_t want[4] = {VALS[i], 0, 0, 0};
            CHECK(rt_block_is(pc_psd_layer_block(rt_record(&f, "a"), KEYS[i]), want, 4));
            CHECK(pc_psd_layer_block(rt_record(&f, "b"), KEYS[i]) == NULL);
        }
        CHECK(rt_block_is(pc_psd_layer_block(rt_record(&f, "g"), "knko"), (const uint8_t[4]){1, 0, 0, 0}, 4));
        pc_psd_file_fini(&f);
        back = rt_roundtrip(&d);
        CHECK(pc_advanced_blending_eq(&back.layers[0].advanced, &adv));
        CHECK(pc_advanced_blending_is_default(&back.layers[1].advanced));
        CHECK(back.layers[2].advanced.knockout == PC_KNOCKOUT_SHALLOW);
        CHECK(back.layers[2].fill_opacity == 128.0f / 255.0f);
        /* Reset to the defaults: the imported blocks stay (in place) with default values. */
        back.layers[0].advanced = pc_advanced_blending_default();
        must(pc_io_document_to_psd(&back, &f, NULL));
        CHECK(rt_block_is(pc_psd_layer_block(rt_record(&f, "a"), "knko"), (const uint8_t[4]){0, 0, 0, 0}, 4));
        CHECK(rt_block_is(pc_psd_layer_block(rt_record(&f, "a"), "clbl"), (const uint8_t[4]){1, 0, 0, 0}, 4));
        pc_psd_file_fini(&f);
        again = rt_roundtrip(&back);
        CHECK(pc_advanced_blending_is_default(&again.layers[0].advanced));
        pc_document_fini(&again);
        pc_document_fini(&back);
        pc_document_fini(&d);
    }
}

static void test_blend_if_round_trips_as_blending_ranges(void)
{
    pc_document d = t_doc("b", 8, 8, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_pixel_format fmt = pc_document_pixel_format(&d);
    pc_layer a = red_raster("a", fmt);
    pc_layer b;
    pc_layer c;
    pc_layer g;
    const pc_blend_range p0[2] = {{{0, 0}, {200, 230}}, PC_BLEND_RANGE_FULL};
    const pc_blend_range p2[2] = {PC_BLEND_RANGE_FULL, {{64, 64}, {255, 255}}};
    const pc_blend_range pg[2] = {{{1, 2}, {3, 4}}, PC_BLEND_RANGE_FULL};
    static const uint8_t WANT[32] = {0, 0, 200, 230, 0,  0,  255, 255, 0, 0, 255, 255, 0, 0, 255, 255,
                                     0, 0, 255, 255, 64, 64, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255};
    pc_psd_file f;
    pc_psd_bytes full = {NULL, 0};
    pc_document back;
    pc_blend_range got[2];
    must(pc_blend_if_set(&a.blend_if, 0, p0, NULL));
    must(pc_blend_if_set(&a.blend_if, 2, p2, NULL));
    must(pc_layer_raster(&c, "c", fmt, NULL));
    g = group_of("g", c);
    must(pc_blend_if_set(&g.blend_if, 1, pg, NULL));
    must(pc_layer_raster(&b, "b", fmt, NULL));
    t_push(&d, &a);
    t_push(&d, &b);
    t_push(&d, &g);
    must(pc_io_document_to_psd(&d, &f, NULL));
    {
        const pc_psd_layer *r = rt_record(&f, "a");
        CHECK(r != NULL && r->blending_ranges.len == 32 && memcmp(r->blending_ranges.data, WANT, 32) == 0);
        /* Layers without Blend If keep full ranges for gray + R, G, B. */
        must(pc_psd_blending_ranges_full(3, &full, NULL));
        r = rt_record(&f, "b");
        CHECK(r != NULL && pc_psd_bytes_eq(&r->blending_ranges, &full));
    }
    pc_psd_bytes_fini(&full);
    pc_psd_file_fini(&f);
    back = rt_roundtrip(&d);
    CHECK(pc_blend_if_eq(&back.layers[0].blend_if, &d.layers[0].blend_if));
    CHECK(pc_blend_if_is_default(&back.layers[1].blend_if));
    pc_blend_if_get(&back.layers[2].blend_if, 1, got);
    CHECK(got[0].black[0] == 1 && got[0].black[1] == 2 && got[0].white[0] == 3 && got[0].white[1] == 4);
    pc_document_fini(&back);
    pc_document_fini(&d);
}

/* ---- psd32_export.rs ---- */

/* Walks the raw file: the classic layer info length and the keys of the global blocks. */
static bool raw_layer_section(const uint8_t *b, size_t len, uint32_t *li_len, char keys[][5], size_t *nkeys)
{
    size_t p = 26;
    size_t lm;
    size_t end;
    p += 4 + pc_load_be32(b + p);
    p += 4 + pc_load_be32(b + p);
    lm = pc_load_be32(b + p);
    p += 4;
    if (lm == 0 || p + lm > len) {
        return false;
    }
    end = p + lm;
    *li_len = pc_load_be32(b + p);
    p += 4 + *li_len;
    p += 4 + pc_load_be32(b + p);
    *nkeys = 0;
    while (p + 12 <= end && *nkeys < 16) {
        size_t n = pc_load_be32(b + p + 8);
        CHECK(memcmp(b + p, "8BIM", 4) == 0 || memcmp(b + p, "8B64", 4) == 0);
        memcpy(keys[*nkeys], b + p + 4, 4);
        keys[(*nkeys)++][4] = '\0';
        p += 12 + (n + 3) / 4 * 4; /* global blocks are padded to 4 */
    }
    return true;
}

static void assert_32bit_layout(const char *name, const pc_buf *bytes)
{
    pc_psd_file f;
    pc_psd_bytes toning = {NULL, 0};
    bool real = false;
    uint32_t li_len = 1;
    char keys[16][5];
    size_t nkeys = 0;
    size_t n;
    must(pc_psd_file_parse(bytes->data, bytes->len, &f, NULL));
    CHECK_EQ_UINT(f.header.depth, 32);
    must(pc_psd_color_mode_data_for_depth(32, &toning, NULL));
    CHECK(pc_psd_bytes_eq(&f.color_mode_data, &toning));
    CHECK(pc_psd_file_resource(&f, 1005) != NULL && pc_psd_file_resource(&f, 1057) != NULL);
    CHECK(pc_psd_file_has_real_merged_data(&f, &real) && real);
    CHECK(f.image_compression == PC_PSD_RAW);
    n = (size_t)f.header.width * f.header.height * f.header.channels;
    CHECK_EQ_UINT(f.image_data.len, n * 4);
    for (size_t i = 0; i + 4 <= f.image_data.len; i += 4) {
        if (!isfinite(pc_f32_from_bits(pc_load_be32(f.image_data.data + i)))) {
            PC_T_FAIL("%s: non-finite merged sample", name);
            break;
        }
    }
    if (f.has_layer_info && f.layer_info.n > 0) {
        CHECK(f.placement.where == PC_PSD_LI_GLOBAL_BLOCK && memcmp(f.placement.key, "Lr32", 4) == 0 &&
              memcmp(f.placement.signature, "8BIM", 4) == 0);
        CHECK(raw_layer_section(bytes->data, bytes->len, &li_len, keys, &nkeys));
        CHECK_EQ_UINT(li_len, 0);
        CHECK(nkeys > 0 && strcmp(keys[0], "Lr32") == 0);
        for (size_t i = 0; i < f.layer_info.n; i++) {
            const pc_psd_layer *r = &f.layer_info.layers[i];
            for (size_t c = 0; c < r->nchannels; c++) {
                uint16_t comp = r->channels[c].has_compression ? r->channels[c].compression : PC_PSD_RAW;
                pc_psd_bytes dec = {NULL, 0};
                CHECK(comp == PC_PSD_RAW || comp == PC_PSD_ZIP || comp == PC_PSD_ZIP_PREDICT);
                CHECK(pc_psd_layer_decode_channel(r, r->channels[c].id, 32, f.header.version, &dec, NULL) ==
                      PC_OK);
                pc_psd_bytes_fini(&dec);
            }
        }
        CHECK_EQ_UINT(rt_strict_block_errors(bytes->data, bytes->len), 0);
    }
    pc_psd_bytes_fini(&toning);
    pc_psd_file_fini(&f);
}

static void test_32bit_export_layout(void)
{
    for (int m = 0; m < 2; m++) {
        pc_color_mode mode = m == 0 ? PC_COLOR_MODE_RGB : PC_COLOR_MODE_GRAYSCALE;
        pc_document flat = t_doc("flat", 9, 7, mode, PC_SAMPLE_TYPE_F32);
        pc_buf b = rt_export(&flat, "t.psd", NULL);
        assert_32bit_layout("flat", &b);
        pc_buf_fini(&b);
        pc_document_fini(&flat);
        for (int all = 0; all < 2; all++) {
            pc_document d =
                all ? t_gen_doc_all(mode, PC_SAMPLE_TYPE_F32) : t_gen_doc(mode, PC_SAMPLE_TYPE_F32);
            pc_document back;
            pc_compose_buffer x;
            pc_compose_buffer y;
            b = rt_export(&d, "t.psd", NULL);
            assert_32bit_layout(all ? "all" : "pixels", &b);
            if (all) {
                /* layered_32bit_export_reimports_render_equal */
                back = rt_import("t.psd", &b);
                must(pc_compose_flatten(&d, &x, NULL));
                must(pc_compose_flatten(&back, &y, NULL));
                CHECK_EQ_UINT(x.len, y.len);
                CHECK(rt_max_diff(&x, &y) <= 1e-4f);
                pc_compose_buffer_fini(&x);
                pc_compose_buffer_fini(&y);
                pc_document_fini(&back);
            }
            pc_buf_fini(&b);
            pc_document_fini(&d);
        }
    }
    /* only_32bit_exports_carry_toning_data */
    for (size_t k = 0; k < 2; k++) {
        pc_document d = t_gen_doc(PC_COLOR_MODE_RGB, DEPTHS[k]);
        pc_buf b = rt_export(&d, "t.psd", NULL);
        pc_psd_file f;
        must(pc_psd_file_parse(b.data, b.len, &f, NULL));
        CHECK_EQ_UINT(f.color_mode_data.len, 0);
        pc_psd_file_fini(&f);
        pc_buf_fini(&b);
        pc_document_fini(&d);
    }
}

/* photoshop_32bit_corpus_round_trip (skipped without corpus/). */
static void test_photoshop_32bit_corpus_round_trip(void)
{
    static const char *const DIRS[2] = {"corpus/photoshop/adjustments/rgb32",
                                        "corpus/photoshop/adjustments/gray32"};
    size_t files = 0;
    for (size_t k = 0; k < 2; k++) {
        DIR *dir = opendir(DIRS[k]);
        struct dirent *e;
        if (dir == NULL) {
            (void)printf("%s absent: skipped\n", DIRS[k]);
            continue;
        }
        while ((e = readdir(dir)) != NULL) {
            char path[1024];
            pc_buf bytes = PC_BUF_INIT;
            pc_psd_file ps;
            pc_psd_bytes toning = {NULL, 0};
            pc_document doc;
            pc_document back;
            pc_buf out;
            pc_compose_buffer x;
            pc_compose_buffer y;
            size_t n1 = 0;
            size_t n2 = 0;
            size_t len = strlen(e->d_name);
            if (len < 4 || strcmp(e->d_name + len - 4, ".psd") != 0) {
                continue;
            }
            (void)snprintf(path, sizeof path, "%s/%s", DIRS[k], e->d_name);
            must(pc_file_read(path, (size_t)1 << 30, &bytes, NULL));
            must(pc_psd_file_parse(bytes.data, bytes.len, &ps, NULL));
            must(pc_psd_color_mode_data_for_depth(32, &toning, NULL));
            CHECK(ps.header.depth == 32 && pc_psd_bytes_eq(&ps.color_mode_data, &toning));
            doc = rt_import(e->d_name, &bytes);
            out = rt_export(&doc, "t.psd", NULL);
            assert_32bit_layout(path, &out);
            back = rt_import(e->d_name, &out);
            must(pc_document_layer_count(&doc, &n1, NULL));
            must(pc_document_layer_count(&back, &n2, NULL));
            CHECK_EQ_UINT(n1, n2);
            must(pc_compose_flatten(&doc, &x, NULL));
            must(pc_compose_flatten(&back, &y, NULL));
            if (rt_max_diff(&x, &y) > 1e-4f) {
                PC_T_FAIL("%s: render differs", path);
            }
            pc_compose_buffer_fini(&x);
            pc_compose_buffer_fini(&y);
            pc_document_fini(&back);
            pc_buf_fini(&out);
            pc_document_fini(&doc);
            pc_psd_bytes_fini(&toning);
            pc_psd_file_fini(&ps);
            pc_buf_fini(&bytes);
            files++;
        }
        (void)closedir(dir);
    }
    CHECK(files == 0 || files >= 20);
}

/* ---- psd_empty_channels.rs ---- */

static void test_empty_layer_and_mask_channels_use_raw_without_a_payload(void)
{
    for (int m = 0; m < 2; m++) {
        for (size_t k = 0; k < 3; k++) {
            for (int psb = 0; psb < 2; psb++) {
                pc_color_mode mode = m == 0 ? PC_COLOR_MODE_RGB : PC_COLOR_MODE_GRAYSCALE;
                pc_document doc = t_gen_doc_all(mode, DEPTHS[k]);
                pc_layer masked = t_raster("Uniform mask", pc_document_pixel_format(&doc),
                                           pc_rect_make(0, 0, 3, 3), 1, true);
                pc_pixel_format mf = {PC_COLOR_MODE_GRAYSCALE, DEPTHS[k], false};
                pc_buf b;
                pc_psd_file f;
                size_t empty = 0;
                size_t empty_masks = 0;
                masked.has_mask = true;
                pc_surface_init(&masked.mask.surface, mf);
                masked.mask.enabled = true;
                masked.mask.linked = true;
                masked.mask.density = 1.0f;
                t_push(&doc, &masked);
                b = rt_export(&doc, psb ? "out.psb" : "out.psd", NULL);
                must(pc_psd_file_parse(b.data, b.len, &f, NULL));
                for (size_t i = 0; i < f.layer_info.n; i++) {
                    const pc_psd_layer *l = &f.layer_info.layers[i];
                    for (size_t c = 0; c < l->nchannels; c++) {
                        pc_psd_rect r = pc_psd_layer_channel_rect(l, l->channels[c].id);
                        if (pc_psd_rect_width(r) == 0 || pc_psd_rect_height(r) == 0) {
                            empty++;
                            empty_masks += l->channels[c].id == -2 ? 1 : 0;
                            CHECK(l->channels[c].has_compression && l->channels[c].compression == PC_PSD_RAW);
                            CHECK_EQ_UINT(l->channels[c].data.len, 0);
                        }
                    }
                }
                CHECK(empty > 0 && empty_masks > 0);
                pc_psd_file_fini(&f);
                pc_buf_fini(&b);
                pc_document_fini(&doc);
            }
        }
    }
}

/* ---- psd_block_layout.rs ---- */

static pc_buf plld_data(const char *uuid)
{
    static const double Q[8] = {0.0, 0.0, 4.0, 0.0, 4.0, 3.0, 0.0, 3.0};
    pc_buf d = PC_BUF_INIT;
    uint8_t t[8];
    pc_psd_desc warp;
    pc_psd_bytes vb = {NULL, 0};
    must(pc_buf_append(&d, "plcL\0\0\0\3", 8, NULL));
    must(pc_buf_append_byte(&d, (uint8_t)strlen(uuid), NULL));
    must(pc_buf_append(&d, uuid, strlen(uuid), NULL));
    for (uint32_t v = 0; v < 4; v++) {
        static const uint32_t F[4] = {1, 1, 16, 2};
        pc_store_be32(t, F[v]);
        must(pc_buf_append(&d, t, 4, NULL));
    }
    for (int i = 0; i < 8; i++) {
        pc_store_be64(t, pc_f64_to_bits(Q[i]));
        must(pc_buf_append(&d, t, 8, NULL));
    }
    must(pc_buf_append(&d, "\0\0\0\0", 4, NULL));
    must(pc_psd_desc_init(&warp, "warp", NULL));
    must(pc_psd_vdesc_to_bytes(&warp, &vb, NULL));
    must(pc_buf_append(&d, vb.data, vb.len, NULL));
    while (d.len % 4 != 0) {
        must(pc_buf_append_byte(&d, 0, NULL));
    }
    pc_psd_bytes_fini(&vb);
    pc_psd_desc_fini(&warp);
    return d;
}

static pc_buf sold_data(const char *uuid)
{
    static const double Q[8] = {0.0, 0.0, 4.0, 0.0, 4.0, 4.0, 0.0, 4.0};
    pc_buf d = PC_BUF_INIT;
    pc_psd_desc desc;
    pc_psd_value v;
    pc_psd_value list;
    pc_psd_bytes vb = {NULL, 0};
    must(pc_buf_append(&d, "soLD\0\0\0\4", 8, NULL));
    must(pc_psd_desc_init(&desc, "null", NULL));
    must(pc_psd_value_text(uuid, true, &v, NULL));
    must(pc_psd_desc_add(&desc, "Idnt", &v, NULL));
    memset(&list, 0, sizeof list);
    list.kind = PC_PSD_V_LIST;
    for (int i = 0; i < 8; i++) {
        v = pc_psd_value_double(Q[i]);
        must(pc_psd_value_list_push(&list, &v, NULL));
    }
    must(pc_psd_desc_add(&desc, "Trnf", &list, NULL));
    must(pc_psd_vdesc_to_bytes(&desc, &vb, NULL));
    must(pc_buf_append(&d, vb.data, vb.len, NULL));
    while (d.len % 4 != 0) {
        must(pc_buf_append_byte(&d, 0, NULL));
    }
    pc_psd_bytes_fini(&vb);
    pc_psd_desc_fini(&desc);
    return d;
}

static pc_buf reexport(const pc_psd_file *f)
{
    pc_psd_bytes bytes = {NULL, 0};
    pc_buf in = PC_BUF_INIT;
    pc_document d;
    pc_buf out;
    must(pc_psd_file_to_bytes(f, &bytes, NULL));
    must(pc_buf_append(&in, bytes.data, bytes.len, NULL));
    d = rt_import("in.psd", &in);
    out = rt_export(&d, "out.psd", NULL);
    pc_document_fini(&d);
    pc_buf_fini(&in);
    pc_psd_bytes_fini(&bytes);
    return out;
}

static void push_raw_block(pc_psd_layer *l, const char *key, const void *data, size_t len)
{
    pc_psd_block b;
    must(pc_psd_block_init(&b, key, data, len, NULL));
    must(pc_psd_layer_push_block(l, &b, NULL));
    pc_psd_block_fini(&b);
}

static void test_placed_layer_blocks_keep_their_keys(void)
{
    const char *uuid = "5b0c0b42-1b9c-11e0-a5b7-d0b1b2c3d4e5";
    pc_psd_file f = tg_small(PC_PSD_V1, PC_PSD_RAW);
    pc_buf pl = plld_data(uuid);
    pc_buf so = sold_data(uuid);
    pc_psd_layer rec;
    pc_psd_block lu;
    pc_buf out;
    pc_psd_file back;
    pc_psd_layer_init(&rec);
    must(pc_psd_bytes_set(&rec.name, "smart", 5, NULL));
    must(pc_psd_block_make_unicode_name("smart", &lu, NULL));
    must(pc_psd_layer_push_block(&rec, &lu, NULL));
    pc_psd_block_fini(&lu);
    push_raw_block(&rec, "PlLd", pl.data, pl.len);
    push_raw_block(&rec, "SoLd", so.data, so.len);
    for (size_t i = 0; i < rec.nblocks; i++) {
        CHECK(pc_psd_block_check_structure(&rec.blocks[i], NULL) == PC_OK);
    }
    must(pc_psd_layer_info_push(&f.layer_info, &rec, NULL));
    out = reexport(&f);
    CHECK_EQ_UINT(rt_strict_block_errors(out.data, out.len), 0);
    must(pc_psd_file_parse(out.data, out.len, &back, NULL));
    {
        const pc_psd_layer *r = NULL;
        for (size_t i = 0; i < back.layer_info.n && r == NULL; i++) {
            if (pc_psd_layer_block(&back.layer_info.layers[i], "SoLd") != NULL) {
                r = &back.layer_info.layers[i];
            }
        }
        CHECK(r != NULL);
        if (r != NULL) {
            /* Before the fix the SoLd data overwrote the PlLd block. */
            CHECK(rt_block_is(pc_psd_layer_block(r, "PlLd"), pl.data, pl.len));
            CHECK(rt_block_is(pc_psd_layer_block(r, "SoLd"), so.data, so.len));
        }
    }
    pc_psd_file_fini(&back);
    pc_buf_fini(&out);
    pc_buf_fini(&pl);
    pc_buf_fini(&so);
    pc_psd_file_fini(&f);
}

static void test_check_structure_rejects_swapped_placed_data(void)
{
    pc_buf pl = plld_data("u");
    pc_buf so = sold_data("u");
    pc_psd_block b;
    uint8_t lnk[12];
    static const char *const KEYS[6] = {"PlLd", "SoLd", "SoLE", "lnk2", "lfx2", "zzzz"};
    must(pc_psd_block_init(&b, "PlLd", so.data, so.len, NULL));
    CHECK(pc_psd_block_check_structure(&b, NULL) != PC_OK);
    pc_psd_block_fini(&b);
    must(pc_psd_block_init(&b, "SoLd", pl.data, pl.len, NULL));
    CHECK(pc_psd_block_check_structure(&b, NULL) != PC_OK);
    pc_psd_block_fini(&b);
    must(pc_psd_block_init(&b, "PlLd", pl.data, pl.len - 3, NULL));
    CHECK(pc_psd_block_check_structure(&b, NULL) != PC_OK);
    pc_psd_block_fini(&b);
    must(pc_buf_append(&so, "\1\2\3\4\5", 5, NULL));
    must(pc_psd_block_init(&b, "SoLd", so.data, so.len, NULL));
    CHECK(pc_psd_block_check_structure(&b, NULL) != PC_OK);
    pc_psd_block_fini(&b);
    /* Bad linked-file chain: a length past the end of the block. */
    pc_store_be64(lnk, 100);
    memcpy(lnk + 8, "liFD", 4);
    must(pc_psd_block_init(&b, "lnk2", lnk, 12, NULL));
    CHECK(pc_psd_block_check_structure(&b, NULL) != PC_OK);
    pc_psd_block_fini(&b);
    for (size_t i = 0; i < 6; i++) {
        must(pc_psd_block_init(&b, KEYS[i], NULL, 0, NULL));
        (void)pc_psd_block_check_structure(&b, NULL);
        pc_psd_block_fini(&b);
    }
    pc_buf_fini(&pl);
    pc_buf_fini(&so);
}

static void push_global(pc_psd_file *f, pc_psd_block *b)
{
    pc_psd_block *g = realloc(f->global_blocks, (f->nglobal + 1) * sizeof *g);
    if (g == NULL) {
        abort();
    }
    f->global_blocks = g;
    g[f->nglobal++] = *b;
}

static void test_global_blocks_are_padded_to_four(void)
{
    pc_psd_file f = tg_small(PC_PSD_V1, PC_PSD_RAW);
    uint8_t cai[77];
    pc_psd_block b;
    pc_buf out;
    pc_psd_file back;
    const pc_psd_block *g;
    memset(cai, 7, sizeof cai);
    must(pc_psd_block_init(&b, "CAI ", cai, 77, NULL));
    b.padding.has = true;
    must(pc_psd_bytes_set(&b.padding.v, "\0\0\0", 3, NULL));
    push_global(&f, &b);
    must(pc_psd_block_init(&b, "zEnd", "\1\2\3\4", 4, NULL));
    push_global(&f, &b);
    out = reexport(&f);
    CHECK_EQ_UINT(rt_strict_block_errors(out.data, out.len), 0);
    must(pc_psd_file_parse(out.data, out.len, &back, NULL));
    g = pc_psd_file_global_block(&back, "CAI ");
    CHECK(rt_block_is(g, cai, 77));
    CHECK(pc_psd_file_global_block(&back, "zEnd") != NULL);
    pc_psd_file_fini(&back);
    pc_buf_fini(&out);
    pc_psd_file_fini(&f);
}

static void test_deep_documents_pad_the_layer_info_block(void)
{
    for (size_t k = 1; k < 3; k++) {
        pc_document d = t_doc("d", 5, 3, PC_COLOR_MODE_RGB, DEPTHS[k]);
        pc_pixel_format fmt = pc_document_pixel_format(&d);
        pc_layer a = t_raster("a", fmt, pc_rect_make(0, 0, 5, 3), 1, true);
        pc_layer b = t_raster("b", fmt, pc_rect_make(1, 1, 3, 1), 2, true);
        pc_buf out;
        t_push(&d, &a);
        t_push(&d, &b);
        out = rt_export(&d, "out.psd", NULL);
        CHECK_EQ_UINT(rt_strict_block_errors(out.data, out.len), 0);
        pc_buf_fini(&out);
        pc_document_fini(&d);
    }
}

static void test_odd_layer_blocks_carry_their_pad_inside_the_length(void)
{
    pc_psd_file f = tg_small(PC_PSD_V1, PC_PSD_RAW);
    pc_psd_block b;
    pc_buf out;
    must(pc_psd_block_init(&b, "zOdd", "\1\2\3", 3, NULL));
    b.padding.has = true;
    must(pc_psd_bytes_set(&b.padding.v, "\0", 1, NULL));
    must(pc_psd_layer_push_block(&f.layer_info.layers[0], &b, NULL));
    pc_psd_block_fini(&b);
    out = reexport(&f);
    CHECK_EQ_UINT(rt_strict_block_errors(out.data, out.len), 0);
    pc_buf_fini(&out);
    pc_psd_file_fini(&f);
}

int main(int argc, char **argv)
{
    RUN(test_generators_match_the_reference);
    RUN(test_rt_cases);
    RUN(test_psb_roundtrip_and_option);
    RUN(test_empty_and_zero_sized_documents);
    RUN(test_round_trip_reaches_a_fixed_point);
    RUN(test_text_shape_smart_raw_blocks_survive);
    RUN(test_raster_layer_blocks_preserved);
    RUN(test_psd_ids_are_kept_and_deduplicated);
    RUN(test_edited_adjustment_regenerates_block_unedited_keeps_raw);
    RUN(test_text_layer_roundtrip_with_tysh);
    RUN(test_effects_raw_roundtrip);
    RUN(test_gradient_and_pattern_fills_roundtrip);
    RUN(test_psd_raw_resources_and_global_blocks_preserved);
    RUN(test_document_to_psd_direct_api_and_big_psb);
    RUN(test_effects_roundtrip_typed_and_raw);
    RUN(test_group_effects_use_lfxs_and_clearing_drops_blocks);
    RUN(test_link_groups_and_effects_reference_round_trip);
    RUN(test_channel_restrictions_round_trip_as_brst);
    RUN(test_advanced_blending_round_trips_as_tagged_blocks);
    RUN(test_blend_if_round_trips_as_blending_ranges);
    RUN(test_32bit_export_layout);
    RUN(test_photoshop_32bit_corpus_round_trip);
    RUN(test_empty_layer_and_mask_channels_use_raw_without_a_payload);
    RUN(test_placed_layer_blocks_keep_their_keys);
    RUN(test_check_structure_rejects_swapped_placed_data);
    RUN(test_global_blocks_are_padded_to_four);
    RUN(test_deep_documents_pad_the_layer_info_block);
    RUN(test_odd_layer_blocks_carry_their_pad_inside_the_length);
    return pc_t_finish();
}
