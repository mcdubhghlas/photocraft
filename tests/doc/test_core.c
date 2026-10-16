/* Tests for src/doc core: document, layers, masks, ids, text runs, comps, video, patterns (ports
 * of the #[test]s in crates/doc/src/{lib,comps,video,pattern,text}.rs, minus serde-only checks),
 * plus never-crash cases: very deep trees, zeroed values, clone independence. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../harness.h"
#include "doc/doc.h"

#define RGBA8 PC_PIXEL_FORMAT_RGBA8

static pc_document doc_bg(uint32_t w, uint32_t h)
{
    pc_document d;
    pc_status s = pc_document_with_background(&d, "t", pc_size_make(w, h), PC_COLOR_MODE_RGB,
                                              PC_SAMPLE_TYPE_U8, PC_COLOR_WHITE, NULL);
    CHECK(s == PC_OK);
    return d;
}

static pc_document doc(void)
{
    return doc_bg(100, 50);
}

static pc_layer raster(const char *name)
{
    pc_layer l;
    CHECK(pc_layer_raster(&l, name, RGBA8, NULL) == PC_OK);
    return l;
}

/* A group owning copies of the n layers in ls (taken over). */
static pc_layer group(const char *name, pc_layer *ls, size_t n)
{
    pc_layer g;
    pc_layer *arr = NULL;
    if (n > 0) {
        arr = malloc(n * sizeof *arr);
        CHECK(arr != NULL);
        memcpy(arr, ls, n * sizeof *arr);
    }
    CHECK(pc_layer_group(&g, name, arr, n, NULL) == PC_OK);
    return g;
}

static pc_layer_id insert(pc_document *d, const pc_layer_id *above, pc_layer l)
{
    pc_layer_id id = 0;
    CHECK(pc_document_insert_above(d, above, &l, &id, NULL) == PC_OK);
    return id;
}

/* Appends at the top of the root (Rust: d.layers.push). */
static void push(pc_document *d, pc_layer l)
{
    (void)insert(d, NULL, l);
}

/* ---- lib.rs ---- */

static void test_layer_color_ids_stay_stable(void)
{
    static const char *const names[] = {"None", "Red", "Orange", "Yellow", "Green", "Blue", "Violet", "Gray"};
    static const char *const bad[] = {"", "Red", "0", "unknown", "\xf0\x9f\x94\xb4"};
    size_t i;
    pc_label_color c;
    for (i = 0; i < 8; i++) {
        CHECK_EQ_STR(pc_label_color_name((pc_label_color)i), names[i]);
    }
    for (i = 0; i < PC_LABEL_COLOR_COUNT; i++) {
        const char *id = pc_label_color_id(PC_LABEL_COLOR_ALL[i]);
        CHECK(pc_label_color_from_id(id, strlen(id), &c));
        CHECK_EQ_INT(c, PC_LABEL_COLOR_ALL[i]);
    }
    for (i = 0; i < 5; i++) {
        CHECK(!pc_label_color_from_id(bad[i], strlen(bad[i]), &c));
    }
    CHECK(!pc_label_color_from_id(NULL, 3, &c));
    CHECK_EQ_STR(pc_label_color_label(PC_LABEL_NONE), "No Color");
    CHECK_EQ_STR(pc_label_color_id(PC_LABEL_SEAFOAM), "seafoam");
    CHECK_EQ_INT(PC_LABEL_COLOR_ALL[5], PC_LABEL_SEAFOAM);
    CHECK_EQ_INT(PC_LABEL_COLOR_ALL[11], PC_LABEL_GRAY);
    CHECK_EQ_STR(pc_label_color_id((pc_label_color)99), "none"); /* out of range: no crash */
}

static void with_link_groups(pc_document *d, const uint64_t *groups, size_t n)
{
    size_t i;
    while (d->nlayers > 1) {
        pc_layer gone;
        CHECK(pc_document_remove(d, d->layers[d->nlayers - 1].id, &gone, NULL) == PC_OK);
        pc_layer_fini(&gone);
    }
    for (i = 0; i < n; i++) {
        pc_layer l = raster("l");
        l.has_link_group = true;
        l.link_group = groups[i];
        push(d, l);
    }
}

static void check_link(pc_document *d, const uint64_t *groups, size_t n, uint64_t want)
{
    bool found = false;
    uint64_t got = 0;
    with_link_groups(d, groups, n);
    CHECK(pc_document_unused_link_group(d, &found, &got, NULL) == PC_OK);
    CHECK(found);
    CHECK_EQ_UINT(got, want);
}

static void test_unused_link_group_never_wraps(void)
{
    pc_document d = doc();
    static const uint64_t a[] = {3, 7};
    static const uint64_t b[] = {UINT64_MAX - 1};
    static const uint64_t c[] = {UINT64_MAX};
    static const uint64_t e[] = {0, 1, 2, 4, UINT64_MAX};
    check_link(&d, NULL, 0, 1);
    check_link(&d, a, 2, 8);
    check_link(&d, b, 1, UINT64_MAX);
    check_link(&d, c, 1, 1); /* #961 */
    check_link(&d, e, 5, 3);
    pc_document_fini(&d);
}

static void test_new_document_has_locked_background(void)
{
    pc_document d = doc();
    const pc_surface *s;
    float px[4];
    CHECK_EQ_UINT(d.nlayers, 1);
    CHECK(pc_str_eq_c(&d.layers[0].name, "Background"));
    CHECK(d.layers[0].locks.transparency);
    CHECK(d.layers[0].locks.position);
    s = pc_layer_surface(&d.layers[0]);
    CHECK(s != NULL);
    pc_surface_read_pixel(s, 0, 0, px);
    CHECK(px[0] == 1.0f && px[1] == 1.0f && px[2] == 1.0f && px[3] == 1.0f);
    pc_surface_read_pixel(s, 99, 49, px);
    CHECK(px[0] == 1.0f && px[3] == 1.0f);
    pc_surface_read_pixel(s, 100, 0, px);
    CHECK(px[0] == 0.0f && px[1] == 0.0f && px[2] == 0.0f && px[3] == 0.0f);
    CHECK(d.resolution_dpi == 72.0f);
    CHECK(d.id < d.layers[0].id); /* the document id is minted first */
    pc_document_fini(&d);
}

static void test_max_group_depth_counts_nesting_without_recursing(void)
{
    pc_document d = doc();
    pc_layer_id bg = d.layers[0].id;
    pc_layer chain = raster("L");
    size_t depth = 99;
    int i;
    for (i = 0; i < 3; i++) {
        chain = group("G", &chain, 1);
    }
    (void)insert(&d, &bg, chain);
    CHECK(pc_document_max_group_depth(&d, &depth, NULL) == PC_OK);
    CHECK_EQ_UINT(depth, 3);
    (void)insert(&d, &bg, group("empty", NULL, 0));
    CHECK(pc_document_max_group_depth(&d, &depth, NULL) == PC_OK);
    CHECK_EQ_UINT(depth, 3);
    pc_document_fini(&d);
}

static void test_pixel_format_follows_mode_and_depth(void)
{
    pc_document d;
    pc_pixel_format f;
    CHECK(pc_document_init(&d, "c", pc_size_make(1, 1), PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U16, NULL) ==
          PC_OK);
    f = pc_document_pixel_format(&d);
    CHECK(f.mode == PC_COLOR_MODE_CMYK && f.sample == PC_SAMPLE_TYPE_U16 && f.alpha);
    pc_document_fini(&d);
    CHECK(pc_document_init(&d, "b", pc_size_make(1, 1), PC_COLOR_MODE_BITMAP, PC_SAMPLE_TYPE_U8, NULL) ==
          PC_OK);
    CHECK(pc_document_pixel_format(&d).mode == PC_COLOR_MODE_GRAYSCALE);
    d.mode = PC_COLOR_MODE_DUOTONE;
    CHECK(pc_document_pixel_format(&d).mode == PC_COLOR_MODE_GRAYSCALE);
    d.mode = PC_COLOR_MODE_INDEXED;
    CHECK(pc_document_pixel_format(&d).mode == PC_COLOR_MODE_RGB);
    d.mode = PC_COLOR_MODE_MULTICHANNEL;
    CHECK(pc_document_pixel_format(&d).mode == PC_COLOR_MODE_RGB);
    d.mode = PC_COLOR_MODE_LAB;
    CHECK(pc_document_pixel_format(&d).mode == PC_COLOR_MODE_LAB);
    pc_document_fini(&d);
}

static void test_text_runs_with_maximal_lengths_normalize_to_text_length(void)
{
    pc_text_layer t;
    pc_text_run *chars = NULL;
    pc_paragraph_run *paras = NULL;
    size_t n = 0;
    pc_text_layer_default(&t);
    CHECK(pc_str_set_c(&t.text, "ab", NULL) == PC_OK);
    t.runs = calloc(2, sizeof *t.runs);
    t.nruns = 2;
    pc_char_style_default(&t.runs[0].style);
    t.runs[0].len = 1;
    pc_char_style_default(&t.runs[1].style);
    t.runs[1].style.size_pt = 24.0f;
    t.runs[1].len = SIZE_MAX;
    t.paragraphs = calloc(2, sizeof *t.paragraphs);
    t.nparagraphs = 2;
    t.paragraphs[0].len = 1;
    t.paragraphs[0].style = pc_paragraph_style_default();
    t.paragraphs[1].len = SIZE_MAX;
    t.paragraphs[1].style = pc_paragraph_style_default();
    t.paragraphs[1].style.align = PC_TEXT_ALIGN_CENTER;

    CHECK(pc_text_layer_char_runs(&t, &chars, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 2);
    CHECK_EQ_UINT(chars[0].len + chars[1].len, 2);
    CHECK_EQ_UINT(chars[1].len, 1);
    CHECK(chars[1].style.size_pt == 24.0f);
    pc_text_runs_free(chars, n);

    CHECK(pc_text_layer_paragraph_runs(&t, &paras, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 2);
    CHECK_EQ_UINT(paras[0].len + paras[1].len, 2);
    CHECK_EQ_UINT(paras[1].len, 1);
    CHECK_EQ_INT(paras[1].style.align, PC_TEXT_ALIGN_CENTER);
    free(paras);
    pc_text_layer_fini(&t);
}

static void layer_names(const pc_document *d, char *out, size_t cap)
{
    size_t i;
    out[0] = '\0';
    for (i = 0; i < d->nlayers; i++) {
        if (i > 0) {
            strncat(out, ",", cap - strlen(out) - 1);
        }
        strncat(out, pc_str_c(&d->layers[i].name), cap - strlen(out) - 1);
    }
}

static void test_insert_remove_shift(void)
{
    pc_document d = doc();
    pc_layer_id bg = d.layers[0].id;
    pc_layer_id a = insert(&d, &bg, raster("A"));
    pc_layer_id b = insert(&d, &bg, raster("B"));
    pc_layer removed;
    char names[64];
    layer_names(&d, names, sizeof names);
    CHECK_EQ_STR(names, "Background,B,A");
    CHECK(pc_document_shift(&d, b, 1));
    CHECK_EQ_UINT(d.layers[2].id, b);
    CHECK(!pc_document_shift(&d, b, 1));
    CHECK(!pc_document_shift(&d, b, INT32_MIN));
    CHECK(!pc_document_shift(&d, b, INT32_MAX));
    CHECK(!pc_document_shift(&d, UINT64_MAX, -1));
    CHECK(pc_document_shift(&d, b, -2));
    CHECK_EQ_UINT(d.layers[0].id, b);
    CHECK(pc_document_shift(&d, b, 2));
    CHECK(pc_document_remove(&d, a, &removed, NULL) == PC_OK);
    CHECK(pc_str_eq_c(&removed.name, "A"));
    pc_layer_fini(&removed);
    CHECK(pc_document_layer(&d, a) == NULL);
    {
        pc_error e;
        memset(&removed, 0x5a, sizeof removed);
        CHECK(pc_document_remove(&d, a, &removed, &e) == PC_ERR_NOT_FOUND);
        CHECK_CONTAINS(e.msg, "no layer with id");
    }
    pc_document_fini(&d);
}

static void test_groups_walk_and_paths(void)
{
    pc_document d = doc();
    pc_layer inner = raster("inner");
    pc_layer_id inner_id = inner.id;
    pc_layer_id gid = insert(&d, NULL, group("G", &inner, 1));
    pc_walk w;
    pc_layer_path p;
    pc_layer_id n;
    size_t count = 0;
    CHECK(pc_document_walk(&d, &w, NULL) == PC_OK);
    CHECK_EQ_UINT(w.len, 3);
    CHECK(pc_str_eq_c(&w.items[0].layer->name, "Background"));
    CHECK_EQ_UINT(w.items[0].depth, 0);
    CHECK_EQ_UINT(w.paths[w.items[0].path_start], 0);
    CHECK(pc_str_eq_c(&w.items[1].layer->name, "G"));
    CHECK_EQ_UINT(w.items[1].depth, 0);
    CHECK_EQ_UINT(w.paths[w.items[1].path_start], 1);
    CHECK(pc_str_eq_c(&w.items[2].layer->name, "inner"));
    CHECK_EQ_UINT(w.items[2].depth, 1);
    CHECK_EQ_UINT(w.paths[w.items[2].path_start], 1);
    CHECK_EQ_UINT(w.paths[w.items[2].path_start + 1], 0);
    pc_walk_fini(&w);
    CHECK(pc_document_path_of(&d, inner_id, &p, NULL) == PC_OK);
    CHECK_EQ_UINT(p.len, 2);
    CHECK_EQ_UINT(p.idx[0], 1);
    CHECK_EQ_UINT(p.idx[1], 0);
    pc_layer_path_fini(&p);
    CHECK(pc_document_layer(&d, gid)->blend == PC_BLEND_PASS_THROUGH);
    n = insert(&d, &inner_id, raster("n"));
    CHECK(pc_document_path_of(&d, n, &p, NULL) == PC_OK);
    CHECK_EQ_UINT(p.len, 2);
    CHECK_EQ_UINT(p.idx[0], 1);
    CHECK_EQ_UINT(p.idx[1], 1);
    CHECK(pc_document_layer_at(&d, p.idx, p.len) == pc_document_layer(&d, n));
    CHECK(pc_document_layer_at_mut(&d, p.idx, p.len) == pc_document_layer_mut(&d, n));
    pc_layer_path_fini(&p);
    CHECK(pc_document_layer_count(&d, &count, NULL) == PC_OK);
    CHECK_EQ_UINT(count, 4);
    {
        static const size_t bad1[] = {7};
        static const size_t bad2[] = {0, 0};
        CHECK(pc_document_layer_at(&d, bad1, 1) == NULL);
        CHECK(pc_document_layer_at(&d, bad2, 2) == NULL); /* the background has no children */
        CHECK(pc_document_layer_at(&d, bad1, 0) == NULL);
        CHECK(pc_document_path_of(&d, 12345678901ull, &p, NULL) == PC_ERR_NOT_FOUND);
    }
    pc_document_fini(&d);
}

static void test_a_locked_group_locks_its_contents(void)
{
    pc_document d = doc();
    pc_layer inner = raster("inner");
    pc_layer_id inner_id = inner.id;
    pc_layer h = group("H", &inner, 1);
    pc_layer g = group("G", &h, 1);
    pc_layer_id gid;
    pc_locks k;
    pc_locks none = {false, false, false, false, false};
    g.locks.position = true;
    gid = insert(&d, NULL, g);
    pc_document_layer_mut(&d, inner_id)->locks.pixels = true;
    k = pc_document_effective_locks(&d, inner_id);
    CHECK(k.position && k.pixels && !k.all);
    CHECK(!pc_document_effective_locks(&d, gid).pixels);
    CHECK(pc_locks_eq(pc_document_effective_locks(&d, UINT64_MAX), none));
    {
        static const size_t path[] = {1, 0, 0};
        static const size_t past[] = {1, 0, 0, 3};
        CHECK(pc_locks_eq(pc_document_locks_at(&d, path, 3), k));
        CHECK(pc_locks_eq(pc_document_locks_at(&d, past, 4), k)); /* stops at the missing level */
    }
    pc_document_fini(&d);
}

static void test_duplicate_assigns_fresh_ids_recursively(void)
{
    pc_layer x = raster("x");
    pc_layer g = group("G", &x, 1);
    pc_layer dup;
    size_t n1;
    size_t n2;
    const pc_layer *c1;
    const pc_layer *c2;
    CHECK(pc_layer_duplicate(&g, &dup, NULL) == PC_OK);
    CHECK(g.id != dup.id);
    c1 = pc_layer_children(&g, &n1);
    c2 = pc_layer_children(&dup, &n2);
    CHECK(c1 != NULL && c2 != NULL && n1 == 1 && n2 == 1);
    CHECK(c1[0].id != c2[0].id);
    CHECK(c2[0].id > dup.id); /* pre-order, like reassign_ids */
    CHECK(pc_str_eq_c(&c2[0].name, "x"));
    pc_layer_fini(&dup);
    pc_layer_fini(&g);
}

static void test_duplicate_clears_psd_id_but_keeps_blocks(void)
{
    pc_layer l = raster("x");
    pc_layer d;
    uint8_t one = 1;
    l.has_psd_id = true;
    l.psd_id = 7;
    l.psd_blocks = calloc(1, sizeof *l.psd_blocks);
    l.npsd_blocks = 1;
    memcpy(l.psd_blocks[0].key, "vmsk", 4);
    CHECK(pc_blob_new(&one, 1, &l.psd_blocks[0].data, NULL) == PC_OK);
    CHECK(pc_layer_duplicate(&l, &d, NULL) == PC_OK);
    CHECK(!d.has_psd_id);
    CHECK_EQ_UINT(d.npsd_blocks, 1);
    CHECK_EQ_MEM(d.psd_blocks[0].key, "vmsk", 4);
    CHECK(pc_blob_eq(d.psd_blocks[0].data, l.psd_blocks[0].data));
    pc_layer_fini(&d);
    pc_layer_fini(&l);
}

static void test_layer_names_increment(void)
{
    pc_document d = doc();
    pc_str s = PC_STR_INIT;
    CHECK(pc_document_next_layer_name(&d, "Layer", &s, NULL) == PC_OK);
    CHECK(pc_str_eq_c(&s, "Layer 1"));
    pc_str_fini(&s);
    push(&d, raster("Layer 1"));
    CHECK(pc_document_next_layer_name(&d, "Layer", &s, NULL) == PC_OK);
    CHECK(pc_str_eq_c(&s, "Layer 2"));
    pc_str_fini(&s);
    pc_document_fini(&d);
}

static void check_copy_name(const pc_document *d, const char *name, const char *want)
{
    pc_str s = PC_STR_INIT;
    CHECK(pc_document_copy_name(d, name, strlen(name), &s, NULL) == PC_OK);
    CHECK_EQ_STR(pc_str_c(&s), want);
    pc_str_fini(&s);
}

static void test_copy_names(void)
{
    pc_document d = doc();
    check_copy_name(&d, "Layer 1", "Layer 1 copy");
    push(&d, raster("Layer 1 copy"));
    check_copy_name(&d, "Layer 1", "Layer 1 copy 2");
    check_copy_name(&d, "Layer 1 copy", "Layer 1 copy 2");
    push(&d, raster("Layer 1 copy 2"));
    check_copy_name(&d, "Layer 1 copy 2", "Layer 1 copy 3");
    check_copy_name(&d, "copy", "copy copy");
    check_copy_name(&d, " copy", " copy");
    check_copy_name(&d, "A copy x", "A copy x copy");
    check_copy_name(&d, "A copy ", "A copy  copy");
    check_copy_name(&d, "", " copy");
    pc_document_fini(&d);
}

static void test_mask_density(void)
{
    pc_layer_mask m;
    pc_layer_mask_hide_all(&m);
    CHECK(pc_layer_mask_value(&m, 5, 5) == 0.0f);
    m.density = 0.5f;
    CHECK_NEAR(pc_layer_mask_value(&m, 5, 5), 0.5, 1e-6);
    m.enabled = false;
    CHECK(pc_layer_mask_value(&m, 5, 5) == 1.0f);
    pc_layer_mask_fini(&m);
    pc_layer_mask_reveal_all(&m);
    CHECK(pc_layer_mask_value(&m, -7, 1000) == 1.0f);
    pc_layer_mask_fini(&m);
}

static void test_mask_values_into(void)
{
    pc_layer_mask m;
    float out[6];
    float px[2] = {0.25f, 1.0f};
    pc_rect r = pc_rect_make(0, 0, 3, 2);
    size_t i;
    pc_layer_mask_hide_all(&m);
    m.density = 0.5f;
    CHECK(pc_layer_mask_values_into(&m, r, out, 6, NULL) == PC_OK);
    for (i = 0; i < 6; i++) {
        CHECK(out[i] == 0.5f);
    }
    CHECK(pc_layer_mask_values_into(&m, r, out, 5, NULL) == PC_ERR_INVALID);
    m.enabled = false;
    CHECK(pc_layer_mask_values_into(&m, r, out, 6, NULL) == PC_OK);
    CHECK(out[3] == 1.0f);
    pc_layer_mask_fini(&m);
    /* A two-channel mask surface: channel 0 of each pixel. */
    memset(&m, 0, sizeof m);
    pc_surface_init(&m.surface, PC_PIXEL_FORMAT_GRAYA8);
    m.enabled = true;
    m.density = 1.0f;
    CHECK(pc_surface_write_pixel(&m.surface, 1, 0, px, 2, NULL) == PC_OK);
    CHECK(pc_layer_mask_values_into(&m, r, out, 6, NULL) == PC_OK);
    CHECK(out[0] == 0.0f);
    CHECK_NEAR(out[1], 0.25, 0.003);
    CHECK(out[2] == 0.0f);
    pc_layer_mask_fini(&m);
    /* Empty region. */
    pc_layer_mask_reveal_all(&m);
    CHECK(pc_layer_mask_values_into(&m, PC_RECT_EMPTY, NULL, 0, NULL) == PC_OK);
    pc_layer_mask_fini(&m);
}

static void test_snapshot_clone_is_independent(void)
{
    pc_document d = doc();
    pc_document snap;
    pc_layer_id bg = d.layers[0].id;
    float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float px[4];
    CHECK(pc_document_clone(&d, &snap, NULL) == PC_OK);
    CHECK(pc_document_eq(&d, &snap));
    CHECK(pc_surface_write_pixel(pc_layer_surface_mut(pc_document_layer_mut(&d, bg)), 0, 0, black, 4, NULL) ==
          PC_OK);
    pc_surface_read_pixel(pc_layer_surface(&snap.layers[0]), 0, 0, px);
    CHECK(px[0] == 1.0f && px[1] == 1.0f && px[2] == 1.0f && px[3] == 1.0f);
    CHECK(!pc_document_eq(&d, &snap));
    pc_document_fini(&snap);
    pc_document_fini(&d);
}

static void test_layer_content_article_and_kind_name(void)
{
    pc_layer_content c;
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_ADJUSTMENT;
    c.u.adjustment.kind = PC_ADJ_INVERT;
    CHECK_EQ_STR(pc_layer_content_article(&c), "an");
    CHECK_EQ_STR(pc_layer_content_kind_name(&c), "Adjustment");
    pc_layer_content_fini(&c);
    c.kind = PC_LAYER_RASTER;
    pc_surface_init(&c.u.raster, RGBA8);
    CHECK_EQ_STR(pc_layer_content_article(&c), "a");
    CHECK_EQ_STR(pc_layer_content_kind_name(&c), "Pixel");
    pc_layer_content_fini(&c);
    c.kind = PC_LAYER_GROUP;
    c.u.group.expanded = true;
    CHECK_EQ_STR(pc_layer_content_article(&c), "a");
    c.kind = PC_LAYER_TEXT;
    CHECK_EQ_STR(pc_layer_content_kind_name(&c), "Type");
    c.kind = PC_LAYER_SMART;
    CHECK_EQ_STR(pc_layer_content_kind_name(&c), "Smart Object");
}

static void test_ensure_ids_above_advances_counter(void)
{
    uint64_t far = pc_layer_id_fresh() + 1000;
    pc_ensure_ids_above(far);
    CHECK(pc_layer_id_fresh() > far);
    pc_ensure_ids_above(3);
    CHECK(pc_doc_id_fresh() > far);
}

/* ---- comps.rs ---- */

static pc_document comps_doc(void)
{
    pc_document d = doc_bg(64, 64);
    pc_layer l = raster("A");
    float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    CHECK(pc_surface_fill_rect(pc_layer_surface_mut(&l), pc_rect_make(10, 12, 20, 22), red, 4, NULL) ==
          PC_OK);
    push(&d, l);
    return d;
}

static void test_capture_records_every_layer(void)
{
    pc_document d = comps_doc();
    pc_comp_layer_state *s = NULL;
    size_t n = 0;
    CHECK(pc_comp_capture_states(&d, &s, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 2);
    CHECK(s[1].has_position && s[1].position[0] == 10 && s[1].position[1] == 12);
    CHECK(s[1].has_visible && s[1].visible);
    CHECK(s[1].has_appearance);
    CHECK(s[1].appearance.effects.enabled);
    pc_comp_layer_states_free(s, n);
    pc_document_fini(&d);
}

static void test_captured_info_bits(void)
{
    pc_layer_comp c;
    memset(&c, 0, sizeof c);
    c.id = 1;
    pc_layer_comp_set_captured_info(&c, 5);
    CHECK(c.apply_visibility && !c.apply_position && c.apply_appearance);
    CHECK_EQ_INT(pc_layer_comp_captured_info(&c), 5);
    pc_layer_comp_set_captured_info(&c, -1);
    CHECK_EQ_INT(pc_layer_comp_captured_info(&c), 7);
    pc_layer_comp_fini(&c);
}

static void test_missing_layers_and_ids(void)
{
    pc_document d = comps_doc();
    pc_layer_comp c;
    pc_layer_id gone;
    pc_layer_id *miss = NULL;
    size_t n = 0;
    pc_layer removed;
    memset(&c, 0, sizeof c);
    CHECK(pc_comp_capture_states(&d, &c.states, &c.nstates, NULL) == PC_OK);
    c.id = pc_next_comp_id(&d);
    CHECK(pc_str_set_c(&c.name, "c", NULL) == PC_OK);
    c.apply_visibility = c.apply_position = c.apply_appearance = true;
    d.layer_comps = malloc(sizeof *d.layer_comps);
    d.layer_comps[0] = c;
    d.nlayer_comps = 1;
    CHECK_EQ_UINT(d.layer_comps[0].id, 1);
    CHECK_EQ_UINT(pc_next_comp_id(&d), 2);
    CHECK(pc_document_comp(&d, 1) == &d.layer_comps[0]);
    CHECK(pc_document_comp(&d, 2) == NULL);
    CHECK(pc_layer_comp_state(&d.layer_comps[0], d.layers[0].id) == &d.layer_comps[0].states[0]);
    gone = d.layers[1].id;
    CHECK(pc_document_remove(&d, gone, &removed, NULL) == PC_OK);
    pc_layer_fini(&removed);
    CHECK(pc_layer_comp_missing_layers(&d.layer_comps[0], &d, &miss, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 1);
    CHECK(miss != NULL && miss[0] == gone);
    free(miss);
    /* Wrapping ids: a comp at u32::MAX makes the next id 1, skipping taken ones. */
    d.layer_comps[0].id = UINT32_MAX;
    CHECK_EQ_UINT(pc_next_comp_id(&d), 1);
    {
        pc_layer_comp *two = realloc(d.layer_comps, 2 * sizeof *two);
        CHECK(two != NULL);
        d.layer_comps = two;
        memset(&d.layer_comps[1], 0, sizeof d.layer_comps[1]);
        d.layer_comps[1].id = 1;
        d.nlayer_comps = 2;
        CHECK_EQ_UINT(pc_next_comp_id(&d), 2);
    }
    pc_document_fini(&d);
}

static void test_artboards_listed_and_found(void)
{
    pc_document d = comps_doc();
    pc_layer child = raster("in");
    pc_layer_id cid = child.id;
    pc_layer g = group("Artboard 1", &child, 1);
    pc_layer_id gid = g.id;
    pc_artboard_ref *refs = NULL;
    size_t n = 0;
    pc_layer_id found = 0;
    int32_t pos[2] = {-1, -1};
    float rgba[4];
    pc_artboard_background bg;
    g.content.u.group.has_artboard = true;
    pc_artboard_init(&g.content.u.group.artboard, pc_rect_make(0, 0, 32, 32));
    push(&d, g);
    CHECK(pc_document_has_artboards(&d));
    CHECK(pc_document_artboards(&d, &refs, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 1);
    CHECK(refs[0].layer == gid && pc_str_eq_c(refs[0].name, "Artboard 1"));
    free(refs);
    CHECK(pc_document_artboard_of(&d, cid, &found));
    CHECK_EQ_UINT(found, gid);
    CHECK(!pc_document_artboard_of(&d, d.layers[0].id, &found));
    CHECK(pc_comp_layer_position(pc_document_layer(&d, gid), pos));
    CHECK(pos[0] == 0 && pos[1] == 0);
    CHECK(pc_layer_artboard_mut(pc_document_layer_mut(&d, gid)) != NULL);
    bg.kind = PC_ARTBOARD_BLACK;
    CHECK(pc_artboard_background_rgba(&bg, rgba));
    CHECK(rgba[0] == 0.0f && rgba[1] == 0.0f && rgba[2] == 0.0f && rgba[3] == 1.0f);
    bg.kind = PC_ARTBOARD_TRANSPARENT;
    CHECK(!pc_artboard_background_rgba(&bg, rgba));
    CHECK_EQ_INT(pc_artboard_background_psd_type(&bg), 3);
    CHECK_EQ_STR(pc_artboard_background_name(&bg), "transparent");
    bg.kind = PC_ARTBOARD_CUSTOM;
    bg.custom = pc_color_rgba(0.5f, 0.25f, 1.0f, 0.0f);
    CHECK(!pc_artboard_background_rgba(&bg, rgba));
    bg.custom.alpha = 0.5f;
    CHECK(pc_artboard_background_rgba(&bg, rgba));
    CHECK(rgba[3] == 0.5f);
    pc_document_fini(&d);
}

/* ---- video.rs ---- */

static void test_timeline_new_and_clamp(void)
{
    pc_timeline t = pc_timeline_new(0, 0.0f);
    CHECK_EQ_UINT(t.duration, 1);
    CHECK(t.fps == 30.0f);
    t = pc_timeline_new(24, 24.0f);
    t.current = 100;
    t.work_end = 999;
    pc_timeline_clamp(&t);
    CHECK_EQ_UINT(t.current, 23);
    CHECK_EQ_UINT(t.work_end, 24);
    CHECK_NEAR(pc_timeline_time(&t), 23.0 / 24.0, 1e-6);
    t.work_start = 30;
    t.work_end = 0;
    pc_timeline_clamp(&t);
    CHECK_EQ_UINT(t.work_start, 23);
    CHECK_EQ_UINT(t.work_end, 24);
}

static void test_step_saturates_at_both_ends(void)
{
    pc_timeline t = pc_timeline_new(3, 24.0f);
    size_t durations[2] = {0, SIZE_MAX};
    size_t i;
    pc_timeline_step(&t, -1);
    CHECK_EQ_UINT(t.current, 0);
    pc_timeline_step(&t, 1);
    pc_timeline_step(&t, INT64_MAX);
    CHECK_EQ_UINT(t.current, 2);
    pc_timeline_step(&t, INT64_MIN);
    CHECK_EQ_UINT(t.current, 0);
    for (i = 0; i < 2; i++) {
        pc_timeline u;
        u.fps = INFINITY;
        u.duration = durations[i];
        u.current = SIZE_MAX;
        u.work_start = 0;
        u.work_end = 0;
        pc_timeline_step(&u, 1);
        CHECK_EQ_UINT(u.current, durations[i] > 0 ? durations[i] - 1 : 0);
        pc_timeline_clamp(&u);
        CHECK(u.fps == 30.0f);
        CHECK(u.current < u.duration);
    }
}

static void test_video_data_by_shape(void)
{
    pc_video_data a;
    pc_video_data b;
    pc_surface *frames = calloc(2, sizeof *frames);
    pc_surface_init(&frames[0], RGBA8);
    pc_surface_init(&frames[1], RGBA8);
    pc_video_data_init(&a, frames, 2, 12.0f);
    CHECK(a.show_altered && !a.source.is_file);
    CHECK(pc_video_data_clone(&a, &b, NULL) == PC_OK);
    CHECK(pc_video_data_eq(&a, &b));
    {
        float px[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        CHECK(pc_surface_write_pixel(&b.frames[0], 0, 0, px, 4, NULL) == PC_OK);
    }
    CHECK(pc_video_data_eq(&a, &b)); /* pixels don't count */
    b.source.is_file = true;
    CHECK(!pc_video_data_eq(&a, &b));
    pc_video_data_fini(&b);
    pc_video_data_fini(&a);
    pc_video_data_init(&a, NULL, 0, 0.0f);
    CHECK(a.frames == NULL && a.nframes == 0);
    pc_video_data_fini(&a);
}

/* ---- pattern.rs ---- */

static void test_pattern_ids_are_deterministic_and_lookup_falls_back_to_names(void)
{
    static const char water[] = "$$$/Patterns/Defaults/Water=Water";
    pc_surface s;
    pc_surface s2;
    pc_surface s3;
    pc_pattern a;
    pc_pattern b;
    pc_pattern o;
    float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    size_t dl;
    const char *dn;
    pc_surface_init(&s, RGBA8);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(0, 0, 4, 4), red, 4, NULL) == PC_OK);
    CHECK(pc_surface_clone(&s, &s2, NULL) == PC_OK);
    CHECK(pc_surface_clone(&s, &s3, NULL) == PC_OK);
    CHECK(pc_pattern_new(water, strlen(water), &s, 4, 4, &a, NULL) == PC_OK);
    CHECK(pc_pattern_new(water, strlen(water), &s2, 4, 4, &b, NULL) == PC_OK);
    CHECK(pc_str_eq(&a.id, &b.id));
    CHECK_EQ_UINT(a.id.len, 36);
    CHECK(pc_pattern_new("Other", 5, &s3, 4, 4, &o, NULL) == PC_OK);
    CHECK(!pc_str_eq(&o.id, &a.id));
    dn = pc_pattern_display_name(a.name.ptr, a.name.len, &dl);
    CHECK(dl == 5 && memcmp(dn, "Water", 5) == 0);
    CHECK(pc_pattern_find(&a, 1, a.id.ptr, a.id.len, "", 0) == &a);
    CHECK(pc_pattern_find(&a, 1, "nope", 4, "water", 5) == &a);
    CHECK(pc_pattern_find(&a, 1, "nope", 4, "", 0) == NULL);
    CHECK(pc_pattern_find(NULL, 0, "nope", 4, "water", 5) == NULL);
    dn = pc_pattern_display_name("Caf\xc3\xa9\0\0", 7, &dl);
    CHECK_EQ_UINT(dl, 5);
    dn = pc_pattern_display_name("$$$/NoEquals", 12, &dl);
    CHECK_EQ_UINT(dl, 12);
    CHECK(dn != NULL);
    CHECK(pc_pattern_eq(&a, &a));
    {
        pc_pattern c;
        CHECK(pc_pattern_clone(&a, &c, NULL) == PC_OK);
        CHECK(pc_pattern_eq(&a, &c));
        c.width = 5;
        CHECK(!pc_pattern_eq(&a, &c));
        CHECK(!pc_pattern_is_empty(&c));
        pc_pattern_fini(&c);
    }
    pc_pattern_fini(&a);
    pc_pattern_fini(&b);
    pc_pattern_fini(&o);
    /* Sizes past i32 wrap to an empty region instead of crashing. */
    pc_surface_init(&s, RGBA8);
    CHECK(pc_pattern_new("big", 3, &s, UINT32_MAX, 0x80000000u, &a, NULL) == PC_OK);
    CHECK_EQ_UINT(a.id.len, 36);
    pc_pattern_fini(&a);
}

/* ---- text.rs ---- */

static pc_text_run run(size_t len, float size)
{
    pc_text_run r;
    pc_char_style_default(&r.style);
    r.style.size_pt = size;
    r.len = len;
    return r;
}

static void set_runs(pc_text_layer *t, const pc_text_run *rs, size_t n)
{
    pc_text_runs_free(t->runs, t->nruns);
    t->runs = calloc(n, sizeof *t->runs);
    memcpy(t->runs, rs, n * sizeof *rs);
    t->nruns = n;
}

static void test_runs_are_normalized_to_the_text(void)
{
    pc_text_layer t;
    pc_text_run *r = NULL;
    pc_paragraph_run *p = NULL;
    size_t n = 0;
    pc_text_layer_default(&t);
    CHECK(pc_str_set_c(&t.text, "h\xc3\xa9llo", NULL) == PC_OK);
    t.size_pt = 9.0f;
    CHECK(pc_text_layer_char_runs(&t, &r, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 1);
    CHECK(r[0].len == 6 && r[0].style.size_pt == 9.0f);
    pc_text_runs_free(r, n);
    {
        pc_text_run rs[2];
        rs[0] = run(2, 10.0f);
        rs[1] = run(1, 20.0f);
        set_runs(&t, rs, 2);
    }
    CHECK(pc_text_layer_char_runs(&t, &r, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 2);
    CHECK(r[0].len == 3 && r[1].len == 3);
    pc_text_runs_free(r, n);
    {
        pc_text_run rs[3];
        rs[0] = run(0, 1.0f);
        rs[1] = run(100, 10.0f);
        rs[2] = run(5, 20.0f);
        set_runs(&t, rs, 3);
    }
    CHECK(pc_text_layer_char_runs(&t, &r, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 1);
    CHECK(r[0].len == 6 && r[0].style.size_pt == 10.0f);
    pc_text_runs_free(r, n);
    pc_str_fini(&t.text);
    {
        pc_text_run rs[1];
        rs[0] = run(0, 33.0f);
        set_runs(&t, rs, 1);
    }
    CHECK(pc_text_layer_char_runs(&t, &r, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 1);
    CHECK(r[0].style.size_pt == 33.0f);
    pc_text_runs_free(r, n);
    CHECK(pc_text_layer_paragraph_runs(&t, &p, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 1);
    CHECK(p[0].style.auto_leading == 1.2f);
    free(p);
    pc_text_layer_fini(&t);
}

static void test_summary_follows_first_run(void)
{
    pc_text_layer t;
    pc_text_run rs[1];
    pc_text_layer_default(&t);
    CHECK(pc_str_set_c(&t.text, "ab", NULL) == PC_OK);
    rs[0] = run(2, 7.0f);
    CHECK(pc_str_set_c(&rs[0].style.font_family, "X", NULL) == PC_OK);
    set_runs(&t, rs, 1);
    CHECK(pc_text_layer_sync_summary(&t, NULL) == PC_OK);
    CHECK(pc_str_eq_c(&t.font_family, "X") && t.size_pt == 7.0f);
    pc_text_layer_fini(&t);
    pc_text_layer_default(&t);
    CHECK(pc_text_layer_sync_summary(&t, NULL) == PC_OK); /* no runs: nothing changes */
    CHECK(t.size_pt == 12.0f);
}

static void test_text_layer_clone_eq(void)
{
    pc_text_layer t;
    pc_text_layer c;
    pc_text_run rs[1];
    pc_text_layer_default(&t);
    CHECK(pc_str_set_c(&t.text, "abc", NULL) == PC_OK);
    rs[0] = run(3, 7.0f);
    rs[0].style.features = calloc(1, sizeof *rs[0].style.features);
    rs[0].style.nfeatures = 1;
    CHECK(pc_str_set_c(&rs[0].style.features[0].tag, "ss01", NULL) == PC_OK);
    rs[0].style.features[0].value = 1;
    set_runs(&t, rs, 1);
    t.has_warp = true;
    CHECK(pc_str_set_c(&t.warp.style, "warpArc", NULL) == PC_OK);
    t.shape.kind = PC_TEXT_SHAPE_BOX;
    t.shape.width = 10.0f;
    CHECK(pc_text_layer_clone(&t, &c, NULL) == PC_OK);
    CHECK(pc_text_layer_eq(&t, &c));
    c.runs[0].style.features[0].value = 2;
    CHECK(!pc_text_layer_eq(&t, &c));
    c.runs[0].style.features[0].value = 1;
    c.shape.kind = PC_TEXT_SHAPE_POINT;
    CHECK(!pc_text_layer_eq(&t, &c));
    t.shape.kind = PC_TEXT_SHAPE_POINT;
    CHECK(pc_text_layer_eq(&t, &c)); /* point text ignores the box fields */
    pc_text_layer_fini(&c);
    pc_text_layer_fini(&t);
    CHECK(pc_text_align_is_justified(PC_TEXT_ALIGN_JUSTIFY_ALL));
    CHECK(!pc_text_align_is_justified(PC_TEXT_ALIGN_CENTER));

    /* Type on a path: the Path shape clones its subpath deeply, compares by start/flip/path, and
     * frees the subpath (ASan-checked). */
    {
        double pp[2][2] = {{0.0, 0.0}, {50.0, 0.0}};
        pc_text_layer p2, pc2;
        pc_subpath sp;
        pc_text_layer_default(&p2);
        CHECK(pc_str_set_c(&p2.text, "abc", NULL) == PC_OK);
        CHECK(pc_subpath_polyline((const double(*)[2])pp, 2, &sp, NULL) == PC_OK);
        p2.shape.kind = PC_TEXT_SHAPE_PATH;
        p2.shape.path = sp;
        p2.shape.start = 3.5f;
        p2.shape.flip = true;
        CHECK(pc_text_layer_clone(&p2, &pc2, NULL) == PC_OK);
        CHECK(pc_text_layer_eq(&p2, &pc2));
        pc2.shape.start = 9.0f;
        CHECK(!pc_text_layer_eq(&p2, &pc2)); /* start differs */
        pc2.shape.start = 3.5f;
        pc2.shape.flip = false;
        CHECK(!pc_text_layer_eq(&p2, &pc2)); /* flip differs */
        pc2.shape.flip = true;
        pc2.shape.path.knots[1].anchor.x = 999.0;
        CHECK(!pc_text_layer_eq(&p2, &pc2)); /* subpath differs (deep copy, not shared) */
        pc_text_layer_fini(&pc2);
        pc_text_layer_fini(&p2);
    }
}

static void test_invalid_utf8_runs_terminate(void)
{
    /* Continuation bytes only: snapping runs forward still ends at the text length. */
    pc_text_layer t;
    pc_text_run rs[2];
    pc_text_run *r = NULL;
    size_t n = 0;
    pc_text_layer_default(&t);
    CHECK(pc_str_set(&t.text, "\x80\x80\x80", 3, NULL) == PC_OK);
    rs[0] = run(1, 1.0f);
    rs[1] = run(1, 2.0f);
    set_runs(&t, rs, 2);
    CHECK(pc_text_layer_char_runs(&t, &r, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 1);
    CHECK_EQ_UINT(r[0].len, 3);
    pc_text_runs_free(r, n);
    pc_text_layer_fini(&t);
}

/* ---- never crash ---- */

#define DEEP 10000

static void test_deep_nesting(void)
{
    pc_document d = doc();
    pc_document c;
    pc_layer chain = raster("leaf");
    pc_layer_id leaf = chain.id;
    pc_layer_id top;
    pc_layer dup;
    pc_walk w;
    pc_layer_path p;
    size_t depth = 0;
    size_t count = 0;
    size_t i;
    bool found = false;
    uint64_t lg = 0;
    pc_comp_layer_state *states = NULL;
    size_t nstates = 0;
    for (i = 0; i < DEEP; i++) {
        chain = group("G", &chain, 1);
    }
    top = insert(&d, NULL, chain);
    CHECK(pc_document_max_group_depth(&d, &depth, NULL) == PC_OK);
    CHECK_EQ_UINT(depth, DEEP);
    CHECK(pc_document_layer_count(&d, &count, NULL) == PC_OK);
    CHECK_EQ_UINT(count, DEEP + 2);
    CHECK(pc_document_walk(&d, &w, NULL) == PC_OK);
    CHECK_EQ_UINT(w.len, DEEP + 2);
    CHECK(w.items[DEEP + 1].layer->id == leaf);
    CHECK_EQ_UINT(w.items[DEEP + 1].depth, DEEP);
    CHECK_EQ_UINT(w.paths[w.items[DEEP + 1].path_start], 1);
    CHECK_EQ_UINT(w.paths[w.items[DEEP + 1].path_start + DEEP], 0);
    pc_walk_fini(&w);
    CHECK(pc_document_layer(&d, leaf) != NULL);
    CHECK(pc_document_layer_mut(&d, leaf) != NULL);
    CHECK(pc_document_path_of(&d, leaf, &p, NULL) == PC_OK);
    CHECK_EQ_UINT(p.len, DEEP + 1);
    CHECK(pc_document_layer_at(&d, p.idx, p.len) == pc_document_layer(&d, leaf));
    pc_layer_path_fini(&p);
    pc_document_layer_mut(&d, top)->locks.all = true;
    CHECK(pc_document_effective_locks(&d, leaf).all);
    CHECK(pc_document_unused_link_group(&d, &found, &lg, NULL) == PC_OK);
    CHECK(found && lg == 1);
    CHECK(pc_comp_capture_states(&d, &states, &nstates, NULL) == PC_OK);
    CHECK_EQ_UINT(nstates, DEEP + 2);
    pc_comp_layer_states_free(states, nstates);

    CHECK(pc_document_clone(&d, &c, NULL) == PC_OK);
    CHECK(pc_document_eq(&d, &c));
    pc_document_layer_mut(&c, leaf)->opacity = 0.5f;
    CHECK(!pc_document_eq(&d, &c));
    CHECK(pc_document_layer(&d, leaf)->opacity == 1.0f);
    pc_document_fini(&c);

    CHECK(pc_layer_duplicate(pc_document_layer(&d, top), &dup, NULL) == PC_OK);
    CHECK(dup.id != top);
    CHECK(!pc_layer_eq(&dup, pc_document_layer(&d, top)));
    {
        pc_layer cl;
        CHECK(pc_layer_clone(&dup, &cl, NULL) == PC_OK);
        CHECK(pc_layer_eq(&cl, &dup));
        pc_layer_fini(&cl);
    }
    {
        pc_layer_content cc;
        CHECK(pc_layer_content_clone(&dup.content, &cc, NULL) == PC_OK);
        CHECK(pc_layer_content_eq(&cc, &dup.content));
        pc_layer_content_fini(&cc);
    }
    pc_layer_fini(&dup);
    {
        pc_str s = PC_STR_INIT;
        CHECK(pc_document_copy_name(&d, "G", 1, &s, NULL) == PC_OK);
        CHECK(pc_str_eq_c(&s, "G copy"));
        pc_str_fini(&s);
    }
    pc_document_fini(&d);
}

static void test_wide_and_deep_tree_clone(void)
{
    /* Siblings at every level, so the walk's path sharing isn't the only path taken. */
    pc_layer chain = raster("leaf");
    pc_layer c;
    size_t i;
    for (i = 0; i < 300; i++) {
        pc_layer kids[2];
        kids[0] = raster("side");
        kids[1] = chain;
        chain = group("G", kids, 2);
    }
    CHECK(pc_layer_clone(&chain, &c, NULL) == PC_OK);
    CHECK(pc_layer_eq(&chain, &c));
    {
        pc_document d;
        pc_walk w;
        CHECK(pc_document_init(&d, "w", pc_size_make(1, 1), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, NULL) ==
              PC_OK);
        push(&d, c);
        CHECK(pc_document_walk(&d, &w, NULL) == PC_OK);
        CHECK_EQ_UINT(w.len, 601);
        for (i = 0; i < w.len; i++) {
            const pc_layer *l =
                pc_document_layer_at(&d, &w.paths[w.items[i].path_start], w.items[i].depth + 1);
            CHECK(l == w.items[i].layer);
        }
        pc_walk_fini(&w);
        pc_document_fini(&d);
    }
    pc_layer_fini(&chain);
}

static void test_zeroed_and_null_fini(void)
{
    pc_document d;
    pc_layer l;
    pc_layer_content c;
    pc_text_layer t;
    pc_layer_comp comp;
    pc_pattern p;
    pc_video_data v;
    pc_smart_object so;
    pc_metadata m;
    pc_alpha_channel a;
    pc_effects e;
    memset(&d, 0, sizeof d);
    memset(&l, 0, sizeof l);
    memset(&c, 0, sizeof c);
    memset(&t, 0, sizeof t);
    memset(&comp, 0, sizeof comp);
    memset(&p, 0, sizeof p);
    memset(&v, 0, sizeof v);
    memset(&so, 0, sizeof so);
    memset(&m, 0, sizeof m);
    memset(&a, 0, sizeof a);
    memset(&e, 0, sizeof e);
    pc_document_fini(&d);
    pc_document_fini(&d); /* twice: fini leaves a zeroed value */
    pc_layer_fini(&l);
    pc_layer_content_fini(&c);
    pc_text_layer_fini(&t);
    pc_layer_comp_fini(&comp);
    pc_pattern_fini(&p);
    pc_video_data_fini(&v);
    pc_smart_object_fini(&so);
    pc_metadata_fini(&m);
    pc_alpha_channel_fini(&a);
    pc_effects_fini(&e);
    pc_document_fini(NULL);
    pc_layer_fini(NULL);
    pc_layer_content_fini(NULL);
    pc_text_layer_fini(NULL);
    pc_char_style_fini(NULL);
    pc_text_warp_fini(NULL);
    pc_layer_comp_fini(NULL);
    pc_comp_layer_state_fini(NULL);
    pc_artboard_fini(NULL);
    pc_pattern_fini(NULL);
    pc_video_data_fini(NULL);
    pc_video_source_fini(NULL);
    pc_smart_object_fini(NULL);
    pc_smart_filter_fini(NULL);
    pc_metadata_fini(NULL);
    pc_alpha_channel_fini(NULL);
    pc_guides_fini(NULL);
    pc_layer_mask_fini(NULL);
    pc_effects_fini(NULL);
    pc_walk_fini(NULL);
    pc_layer_path_fini(NULL);
    pc_text_runs_free(NULL, 0);
    pc_comp_layer_states_free(NULL, 0);
    /* Queries on an empty document. */
    {
        pc_layer_id top;
        size_t n = 7;
        CHECK(pc_document_init(&d, NULL, pc_size_make(0, 0), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, NULL) ==
              PC_OK);
        CHECK(!pc_document_top_layer(&d, &top));
        CHECK(pc_document_layer_count(&d, &n, NULL) == PC_OK && n == 0);
        CHECK(pc_document_max_group_depth(&d, &n, NULL) == PC_OK && n == 0);
        CHECK(!pc_document_has_artboards(&d));
        CHECK(pc_document_layer(&d, 1) == NULL);
        CHECK(!pc_document_shift(&d, 1, 1));
        CHECK_EQ_UINT(pc_next_comp_id(&d), 1);
        pc_document_fini(&d);
    }
}

static void test_layer_clone_independence(void)
{
    pc_layer l = raster("L");
    pc_layer c;
    float px[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    float got[4];
    l.has_mask = true;
    pc_layer_mask_hide_all(&l.mask);
    l.effects.items = calloc(1, sizeof *l.effects.items);
    l.effects.nitems = 1;
    pc_effect_default_drop_shadow(&l.effects.items[0]);
    l.has_video = true;
    pc_video_data_init(&l.video, NULL, 0, 24.0f);
    CHECK(pc_str_set_c(&l.video.source.path, "clip.mov", NULL) == PC_OK);
    l.video.source.is_file = true;
    CHECK(pc_layer_clone(&l, &c, NULL) == PC_OK);
    CHECK(pc_layer_eq(&l, &c));
    CHECK(c.id == l.id);
    CHECK(pc_surface_write_pixel(pc_layer_surface_mut(&c), 3, 3, px, 4, NULL) == PC_OK);
    CHECK(pc_surface_write_pixel(&c.mask.surface, 3, 3, px, 1, NULL) == PC_OK);
    pc_surface_read_pixel(pc_layer_surface(&l), 3, 3, got);
    CHECK(got[3] == 0.0f);
    CHECK(pc_layer_mask_value(&l.mask, 3, 3) == 0.0f);
    CHECK(!pc_layer_eq(&l, &c));
    pc_layer_fini(&c);
    CHECK(pc_layer_clone(&l, &c, NULL) == PC_OK);
    CHECK(pc_str_set_c(&c.video.source.path, "other.mov", NULL) == PC_OK);
    CHECK(!pc_layer_eq(&l, &c));
    CHECK(pc_str_eq_c(&l.video.source.path, "clip.mov"));
    pc_layer_fini(&c);
    pc_layer_fini(&l);
}

static void test_smart_object_and_metadata(void)
{
    pc_smart_source src;
    pc_smart_object a;
    pc_smart_object b;
    pc_metadata m;
    pc_metadata mc;
    uint8_t bytes[3] = {1, 2, 3};
    double bounds[4] = {0.0, 0.0, 10.0, 10.0};
    memset(&src, 0, sizeof src);
    CHECK(pc_str_set_c(&src.file_name, "a.png", NULL) == PC_OK);
    CHECK(pc_blob_new(bytes, 3, &src.bytes, NULL) == PC_OK);
    pc_smart_object_init(&a, &src, PC_AFFINE_IDENTITY, NULL);
    CHECK(src.bytes == NULL);
    CHECK(a.filters_enabled && !a.has_cache);
    a.has_warp = true;
    CHECK(pc_mesh_identity(bounds, 2, 2, &a.warp.mesh, NULL) == PC_OK);
    a.warp.has_mesh = true;
    a.warp.style = PC_WARP_CUSTOM;
    a.smart_filters = calloc(1, sizeof *a.smart_filters);
    a.nsmart_filters = 1;
    CHECK(pc_str_set_c(&a.smart_filters[0].command, "filter.gaussianBlur", NULL) == PC_OK);
    a.smart_filters[0].opacity = 1.0f;
    a.has_perspective = true;
    a.perspective[8] = 1.0;
    CHECK(pc_smart_object_clone(&a, &b, NULL) == PC_OK);
    CHECK(pc_smart_object_eq(&a, &b));
    b.warp.mesh.points[0].x += 1.0;
    CHECK(!pc_smart_object_eq(&a, &b));
    b.warp.mesh.points[0].x -= 1.0;
    CHECK(pc_smart_object_eq(&a, &b));
    b.perspective[8] = NAN;
    CHECK(!pc_smart_object_eq(&a, &b));
    pc_smart_object_fini(&b);
    pc_smart_object_fini(&a);

    memset(&m, 0, sizeof m);
    m.has_xmp = true;
    CHECK(pc_str_set_c(&m.xmp, "<x/>", NULL) == PC_OK);
    m.psd_resources = calloc(1, sizeof *m.psd_resources);
    m.npsd_resources = 1;
    m.psd_resources[0].id = 1036;
    CHECK(pc_blob_new(bytes, 2, &m.psd_resources[0].data, NULL) == PC_OK);
    CHECK(pc_metadata_clone(&m, &mc, NULL) == PC_OK);
    CHECK(pc_metadata_eq(&m, &mc));
    mc.psd_resources[0].id = 1;
    CHECK(!pc_metadata_eq(&m, &mc));
    pc_metadata_fini(&mc);
    pc_metadata_fini(&m);
}

static void test_alpha_channel_and_guides(void)
{
    pc_surface s;
    pc_alpha_channel a;
    pc_alpha_channel b;
    pc_guides g;
    pc_guides gc;
    pc_surface_init(&s, PC_PIXEL_FORMAT_GRAY8);
    CHECK(pc_alpha_channel_init(&a, "Alpha 1", &s, NULL) == PC_OK);
    CHECK(a.opacity == 0.5f && a.color.c[0] == 1.0f && a.indicates == PC_INDICATES_MASKED_AREAS);
    CHECK(pc_alpha_channel_overlay_coverage(&a, 0.25f) == 0.75f);
    a.indicates = PC_INDICATES_SELECTED_AREAS;
    CHECK(pc_alpha_channel_overlay_coverage(&a, 0.25f) == 0.25f);
    CHECK(pc_alpha_channel_clone(&a, &b, NULL) == PC_OK);
    CHECK(pc_alpha_channel_eq(&a, &b));
    b.has_spot = true;
    CHECK(!pc_alpha_channel_eq(&a, &b));
    pc_alpha_channel_fini(&b);
    pc_alpha_channel_fini(&a);
    memset(&g, 0, sizeof g);
    g.horizontal = malloc(2 * sizeof *g.horizontal);
    g.horizontal[0] = 1.0;
    g.horizontal[1] = 2.0;
    g.nhorizontal = 2;
    CHECK(pc_guides_clone(&g, &gc, NULL) == PC_OK);
    CHECK(pc_guides_eq(&g, &gc));
    gc.horizontal[1] = NAN;
    CHECK(!pc_guides_eq(&g, &gc));
    pc_guides_fini(&gc);
    pc_guides_fini(&g);
}

static void test_warp_clone_eq(void)
{
    double bounds[4] = {0.0, 0.0, 4.0, 2.0};
    pc_warp a;
    pc_warp b;
    pc_mesh m;
    pc_warp_preset(&a, PC_WARP_ARC, 50.0, bounds);
    CHECK(pc_warp_clone(&a, &b, NULL) == PC_OK);
    CHECK(pc_warp_eq(&a, &b));
    b.bend = 51.0;
    CHECK(!pc_warp_eq(&a, &b));
    pc_warp_fini(&b);
    pc_warp_fini(&a);
    CHECK(pc_mesh_identity(bounds, 1, 1, &m, NULL) == PC_OK);
    pc_warp_custom(&a, &m, bounds);
    CHECK(pc_warp_clone(&a, &b, NULL) == PC_OK);
    CHECK(pc_warp_eq(&a, &b));
    CHECK(b.mesh.points != a.mesh.points);
    b.mesh.vs[1] = 0.5;
    CHECK(!pc_warp_eq(&a, &b));
    pc_warp_fini(&b);
    pc_warp_fini(&a);
}

static void test_insert_above_missing_goes_to_top(void)
{
    pc_document d = doc();
    pc_layer_id missing = 424242424242ull;
    pc_layer_id id = insert(&d, &missing, raster("X"));
    pc_layer_id top = 0;
    CHECK(pc_document_top_layer(&d, &top));
    CHECK_EQ_UINT(top, id);
    {
        pc_layer_id idg = insert(&d, NULL, group("G", NULL, 0));
        pc_layer_id inside;
        size_t n;
        /* Inserting above an empty group puts the layer next to it, not inside. */
        inside = insert(&d, &idg, raster("Y"));
        CHECK(pc_layer_children(pc_document_layer(&d, idg), &n) == NULL && n == 0);
        CHECK(pc_document_top_layer(&d, &top) && top == inside);
    }
    pc_document_fini(&d);
}

int main(int argc, char **argv)
{
    RUN(test_layer_color_ids_stay_stable);
    RUN(test_unused_link_group_never_wraps);
    RUN(test_new_document_has_locked_background);
    RUN(test_max_group_depth_counts_nesting_without_recursing);
    RUN(test_pixel_format_follows_mode_and_depth);
    RUN(test_text_runs_with_maximal_lengths_normalize_to_text_length);
    RUN(test_insert_remove_shift);
    RUN(test_groups_walk_and_paths);
    RUN(test_a_locked_group_locks_its_contents);
    RUN(test_duplicate_assigns_fresh_ids_recursively);
    RUN(test_duplicate_clears_psd_id_but_keeps_blocks);
    RUN(test_layer_names_increment);
    RUN(test_copy_names);
    RUN(test_mask_density);
    RUN(test_mask_values_into);
    RUN(test_snapshot_clone_is_independent);
    RUN(test_layer_content_article_and_kind_name);
    RUN(test_ensure_ids_above_advances_counter);
    RUN(test_capture_records_every_layer);
    RUN(test_captured_info_bits);
    RUN(test_missing_layers_and_ids);
    RUN(test_artboards_listed_and_found);
    RUN(test_timeline_new_and_clamp);
    RUN(test_step_saturates_at_both_ends);
    RUN(test_video_data_by_shape);
    RUN(test_pattern_ids_are_deterministic_and_lookup_falls_back_to_names);
    RUN(test_runs_are_normalized_to_the_text);
    RUN(test_summary_follows_first_run);
    RUN(test_text_layer_clone_eq);
    RUN(test_invalid_utf8_runs_terminate);
    RUN(test_deep_nesting);
    RUN(test_wide_and_deep_tree_clone);
    RUN(test_zeroed_and_null_fini);
    RUN(test_layer_clone_independence);
    RUN(test_smart_object_and_metadata);
    RUN(test_alpha_channel_and_guides);
    RUN(test_warp_clone_eq);
    RUN(test_insert_above_missing_goes_to_top);
    return pc_t_finish();
}
