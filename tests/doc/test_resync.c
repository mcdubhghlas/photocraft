/* Tests for the doc model changes of the Phase 5R resync (R4): ports of the #[test]s upstream
 * added to crates/doc after efa62efa (background_is_structural_not_named,
 * layer_lookup_agrees_with_walk, the Curves `pencil` default), plus the new fields' clone/eq/fini
 * (Curves pencil, Contour corners, SmartObject contents_id, Guides colors, Metadata text, video
 * original frames, Document quick_mask_unselected / file_encoding) and the new helpers. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../harness.h"
#include "doc/doc.h"
#include "doc/json.h"

#define RGBA8 PC_PIXEL_FORMAT_RGBA8

static pc_document doc(void)
{
    pc_document d;
    CHECK(pc_document_with_background(&d, "t", pc_size_make(100, 50), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8,
                                      PC_COLOR_WHITE, NULL) == PC_OK);
    return d;
}

static pc_layer raster(const char *name)
{
    pc_layer l;
    CHECK(pc_layer_raster(&l, name, RGBA8, NULL) == PC_OK);
    return l;
}

static pc_layer group(const char *name, pc_layer *ls, size_t n)
{
    pc_layer g;
    pc_layer *arr = malloc((n > 0 ? n : 1) * sizeof *arr);
    CHECK(arr != NULL);
    memcpy(arr, ls, n * sizeof *arr);
    CHECK(pc_layer_group(&g, name, arr, n, NULL) == PC_OK);
    return g;
}

static void push(pc_document *d, pc_layer l)
{
    pc_layer_id id;
    CHECK(pc_document_insert_above(d, NULL, &l, &id, NULL) == PC_OK);
}

/* The Background is the locked bottom pixel layer, whatever it is called. */
static void test_background_is_structural_not_named(void)
{
    pc_document d = doc();
    pc_layer_id id = d.layers[0].id;
    pc_layer top;
    pc_layer_id top_id;
    static const char *const NAMES[] = {"Hintergrund", "\xe8\x83\x8c\xe6\x99\xaf", "Arri\xc3\xa8re-plan"};
    CHECK(pc_document_is_background(&d, id));
    for (size_t i = 0; i < 3; i++) {
        CHECK(pc_str_set_c(&d.layers[0].name, NAMES[i], NULL) == PC_OK);
        CHECK(pc_document_background(&d) != NULL && pc_document_background(&d)->id == id);
    }
    /* A layer called "Background" without the locks, or above the bottom, is not it. */
    top = raster("Background");
    top.locks.transparency = true;
    top.locks.position = true;
    top_id = top.id;
    push(&d, top);
    CHECK(!pc_document_is_background(&d, top_id));
    d.layers[0].locks.position = false;
    CHECK(pc_document_background(&d) == NULL);
    d.layers[0].locks.position = true;
    d.layers[0].clipped = true;
    CHECK(pc_document_background_mut(&d) == NULL);
    d.layers[0].clipped = false;
    CHECK(pc_document_background_mut(&d) == &d.layers[0]);
    pc_document_fini(&d);
    /* No layers, no Background. */
    CHECK(pc_document_init(&d, "e", pc_size_make(1, 1), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, NULL) == PC_OK);
    CHECK(pc_document_background(&d) == NULL && !pc_document_is_background(&d, 1));
    pc_document_fini(&d);
}

/* layer and path_of agree with walk at every depth, and miss cleanly. */
static void test_layer_lookup_agrees_with_walk(void)
{
    pc_document d = doc();
    pc_layer c[1] = {raster("c0")};
    pc_layer b[3];
    pc_layer a[3];
    pc_walk w;
    pc_layer missing = raster("elsewhere");
    pc_layer_path p;
    b[0] = raster("b0");
    b[1] = group("C", c, 1);
    b[2] = raster("b2");
    a[0] = raster("a0");
    a[1] = group("B", b, 3);
    a[2] = raster("a2");
    push(&d, group("A", a, 3));
    push(&d, raster("top"));
    CHECK(pc_document_walk(&d, &w, NULL) == PC_OK);
    CHECK_EQ_UINT(w.len, 10);
    for (size_t i = 0; i < w.len; i++) {
        pc_layer_id id = w.items[i].layer->id;
        CHECK(pc_document_path_of(&d, id, &p, NULL) == PC_OK);
        CHECK_EQ_UINT(p.len, w.items[i].depth + 1);
        CHECK(memcmp(p.idx, &w.paths[w.items[i].path_start], p.len * sizeof *p.idx) == 0);
        pc_layer_path_fini(&p);
        CHECK(pc_document_layer(&d, id) == w.items[i].layer);
        CHECK(pc_document_layer_mut(&d, id) == w.items[i].layer);
    }
    pc_walk_fini(&w);
    CHECK(pc_document_path_of(&d, missing.id, &p, NULL) == PC_ERR_NOT_FOUND);
    CHECK(pc_document_layer(&d, missing.id) == NULL && pc_document_layer_mut(&d, missing.id) == NULL);
    pc_layer_fini(&missing);
    pc_document_fini(&d);
}

static void test_curves_pencil_defaults_and_compares(void)
{
    pc_adjustment a;
    pc_adjustment b;
    yyjson_doc *jd = NULL;
    static const char J[] = "{\"Curves\":{\"master\":[],\"per_channel\":[[],[],[]]}}";
    CHECK(pc_json_parse(J, sizeof J - 1, &jd, NULL) == PC_OK);
    CHECK(pc_adjustment_from_json(yyjson_doc_get_root(jd), &a, NULL) == PC_OK);
    CHECK(a.kind == PC_ADJ_CURVES && a.u.curves.space == PC_TONE_RGB && !a.u.curves.pencil);
    yyjson_doc_free(jd);
    pc_adjustment_fini(&a);
    CHECK(pc_adjustment_identity_curves(&a, NULL) == PC_OK);
    CHECK(!a.u.curves.pencil);
    a.u.curves.pencil = true;
    CHECK(pc_adjustment_clone(&a, &b, NULL) == PC_OK);
    CHECK(b.u.curves.pencil && pc_adjustment_eq(&a, &b));
    b.u.curves.pencil = false;
    CHECK(!pc_adjustment_eq(&a, &b));
    pc_adjustment_fini(&a);
    pc_adjustment_fini(&b);
}

static void test_curve_maps(void)
{
    pc_curve c;
    float outs[PC_CURVE_MAP_LEN];
    float in[3] = {0.25f, NAN, 2.0f};
    pc_curve_point pts[3] = {{1.0f, 0.0f}, {0.0f, 1.0f}, {NAN, 0.5f}};
    CHECK(pc_curve_map(in, 3, &c, NULL) == PC_OK);
    CHECK_EQ_UINT(c.len, PC_CURVE_MAP_LEN);
    CHECK(c.pts[0].input == 0.0f && c.pts[0].output == 0.25f);
    CHECK(c.pts[1].output == 0.25f); /* NaN repeats the last */
    CHECK(c.pts[2].output == 1.0f);  /* clamped */
    CHECK(c.pts[255].input == 1.0f && c.pts[255].output == 1.0f);
    pc_curve_fini(&c);
    CHECK(pc_curve_map(NULL, 0, &c, NULL) == PC_OK);
    CHECK(c.pts[255].output == 0.0f);
    pc_curve_fini(&c);
    /* Sorted by input, NaN points dropped: a falling line. */
    CHECK(pc_curve_map_outputs(pts, 3, outs, NULL) == PC_OK);
    CHECK(outs[0] == 1.0f && outs[255] == 0.0f);
    CHECK(fabsf(outs[51] - 0.8f) < 1e-6f);
    /* Fewer than two points: identity. */
    CHECK(pc_curve_map_outputs(pts, 1, outs, NULL) == PC_OK);
    CHECK(outs[0] == 0.0f && outs[255] == 1.0f);
    CHECK(pc_curve_map_outputs(NULL, 0, outs, NULL) == PC_OK);
}

static void test_contour_corners(void)
{
    pc_contour a;
    pc_contour b;
    memset(&a, 0, sizeof a);
    a.custom = true;
    CHECK(pc_curve_identity(&a.points, NULL) == PC_OK);
    a.corners = calloc(2, sizeof *a.corners);
    a.ncorners = 2;
    a.corners[1] = true;
    CHECK(!pc_contour_is_corner(&a, 0) && pc_contour_is_corner(&a, 1) && !pc_contour_is_corner(&a, 2));
    CHECK(!pc_contour_is_corner(&a, SIZE_MAX));
    CHECK(pc_contour_clone(&a, &b, NULL) == PC_OK);
    CHECK(pc_contour_eq(&a, &b));
    b.corners[1] = false;
    CHECK(!pc_contour_eq(&a, &b));
    pc_contour_fini(&b);
    CHECK(b.corners == NULL && b.ncorners == 0);
    a.custom = false;
    CHECK(!pc_contour_is_corner(&a, 1));
    pc_contour_fini(&a);
}

static void test_smart_contents_id(void)
{
    pc_smart_source src;
    pc_layer l;
    pc_layer dup;
    pc_layer_content c;
    pc_smart_object b;
    memset(&src, 0, sizeof src);
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_SMART;
    pc_smart_object_init(&c.u.smart, &src, PC_AFFINE_IDENTITY, NULL);
    CHECK(c.u.smart.contents_id != 0);
    CHECK(pc_layer_init(&l, "s", &c, NULL) == PC_OK);
    /* Ordinary duplicates share their contents (fresh layer ids, same contents id). */
    CHECK(pc_layer_duplicate(&l, &dup, NULL) == PC_OK);
    CHECK(dup.id != l.id && dup.content.u.smart.contents_id == l.content.u.smart.contents_id);
    CHECK(pc_smart_object_clone(&l.content.u.smart, &b, NULL) == PC_OK);
    CHECK(pc_smart_object_eq(&l.content.u.smart, &b));
    b.contents_id = pc_smart_contents_id_fresh();
    CHECK(b.contents_id > l.content.u.smart.contents_id);
    CHECK(!pc_smart_object_eq(&l.content.u.smart, &b));
    pc_smart_object_fini(&b);
    pc_layer_fini(&dup);
    pc_layer_fini(&l);
}

static void test_guide_colors(void)
{
    pc_guides g;
    pc_guides c;
    uint8_t rgb[3];
    static const uint8_t RED[3] = {255, 0, 0};
    static const uint8_t BLUE[3] = {0, 0, 255};
    memset(&g, 0, sizeof g);
    g.vertical = malloc(2 * sizeof *g.vertical);
    g.vertical[0] = 118.11023622047244;
    g.vertical[1] = 3.0;
    g.nvertical = 2;
    CHECK(!pc_guides_color(&g, true, 3.0, rgb));
    CHECK(pc_guides_set_color(&g, true, 3.0, RED, NULL) == PC_OK);
    CHECK(pc_guides_set_color(&g, false, 3.0, BLUE, NULL) == PC_OK);
    CHECK(pc_guides_color(&g, true, 3.0, rgb) && memcmp(rgb, RED, 3) == 0);
    CHECK(pc_guides_color(&g, false, 3.0, rgb) && memcmp(rgb, BLUE, 3) == 0);
    CHECK(pc_guides_set_color(&g, true, 3.0, BLUE, NULL) == PC_OK); /* replaces, keeps one entry */
    CHECK_EQ_UINT(g.ncolors, 2);
    CHECK(pc_guides_color(&g, true, 3.0, rgb) && memcmp(rgb, BLUE, 3) == 0);
    CHECK(pc_guides_clone(&g, &c, NULL) == PC_OK);
    CHECK(pc_guides_eq(&g, &c));
    c.colors[0].rgb[1] = 1;
    CHECK(!pc_guides_eq(&g, &c));
    pc_guides_fini(&c);
    /* The horizontal 3.0 guide doesn't exist: pruned. */
    pc_guides_prune_colors(&g);
    CHECK_EQ_UINT(g.ncolors, 1);
    CHECK(g.colors[0].vertical && g.colors[0].position == 3.0);
    CHECK(pc_guides_set_color(&g, true, 3.0, NULL, NULL) == PC_OK);
    CHECK_EQ_UINT(g.ncolors, 0);
    pc_guides_fini(&g);
    CHECK(g.colors == NULL && g.ncolors == 0);
}

static void test_metadata_text_and_encoding(void)
{
    pc_metadata m;
    pc_metadata c;
    pc_file_encoding e = {false, 0, false, false, false, 0};
    pc_file_encoding f = e;
    memset(&m, 0, sizeof m);
    m.text = calloc(2, sizeof *m.text);
    m.ntext = 2;
    CHECK(pc_str_set_c(&m.text[0].key, "Comment", NULL) == PC_OK);
    CHECK(pc_str_set_c(&m.text[0].value, "first", NULL) == PC_OK);
    CHECK(pc_str_set_c(&m.text[1].key, "Comment", NULL) == PC_OK);
    CHECK(pc_str_set_c(&m.text[1].value, "second", NULL) == PC_OK);
    CHECK(pc_metadata_clone(&m, &c, NULL) == PC_OK);
    CHECK(pc_metadata_eq(&m, &c));
    CHECK(pc_str_set_c(&c.text[1].value, "other", NULL) == PC_OK);
    CHECK(!pc_metadata_eq(&m, &c));
    pc_metadata_fini(&c);
    pc_metadata_fini(&m);
    CHECK(pc_file_encoding_is_empty(&e) && pc_file_encoding_eq(&e, &f));
    f.jpeg_quality = 90; /* the payload of a None doesn't count */
    CHECK(pc_file_encoding_eq(&e, &f));
    f.has_jpeg_quality = true;
    CHECK(!pc_file_encoding_is_empty(&f) && !pc_file_encoding_eq(&e, &f));
}

static void test_document_new_fields_clone_and_compare(void)
{
    pc_document d = doc();
    pc_document c;
    CHECK(!d.quick_mask_unselected && pc_file_encoding_is_empty(&d.file_encoding));
    d.quick_mask_unselected = true;
    d.file_encoding.has_webp_lossless = true;
    d.file_encoding.webp_lossless = true;
    CHECK(pc_document_clone(&d, &c, NULL) == PC_OK);
    CHECK(pc_document_eq(&d, &c) && c.quick_mask_unselected && c.file_encoding.webp_lossless);
    c.quick_mask_unselected = false;
    CHECK(!pc_document_eq(&d, &c));
    c.quick_mask_unselected = true;
    c.file_encoding.webp_lossless = false;
    CHECK(!pc_document_eq(&d, &c));
    pc_document_fini(&c);
    pc_document_fini(&d);
}

static void test_video_original_frames(void)
{
    pc_surface *frames = calloc(2, sizeof *frames);
    pc_video_data v;
    pc_video_data c;
    pc_surface_init(&frames[0], RGBA8);
    pc_surface_init(&frames[1], RGBA8);
    pc_video_data_init(&v, frames, 2, 24.0f);
    CHECK(!v.has_original_frames);
    CHECK(pc_video_data_clone(&v, &c, NULL) == PC_OK);
    CHECK(pc_video_data_eq(&v, &c));
    /* Some(empty) differs from None; the count matters, not the pixels. */
    c.has_original_frames = true;
    CHECK(!pc_video_data_eq(&v, &c));
    v.has_original_frames = true;
    v.original_frames = calloc(2, sizeof *v.original_frames);
    v.noriginal_frames = 2;
    pc_surface_init(&v.original_frames[0], RGBA8);
    pc_surface_init(&v.original_frames[1], RGBA8);
    CHECK(!pc_video_data_eq(&v, &c));
    pc_video_data_fini(&c);
    CHECK(pc_video_data_clone(&v, &c, NULL) == PC_OK);
    CHECK(c.has_original_frames && c.noriginal_frames == 2 && pc_video_data_eq(&v, &c));
    pc_video_data_fini(&c);
    pc_video_data_fini(&v);
    CHECK(v.original_frames == NULL && !v.has_original_frames);
}

static void test_terminal_paragraph_kept(void)
{
    pc_text_layer t;
    pc_paragraph_run *pr = NULL;
    size_t n = 0;
    pc_text_layer_default(&t);
    CHECK(pc_str_set_c(&t.text, "ab\r", NULL) == PC_OK);
    t.paragraphs = calloc(2, sizeof *t.paragraphs);
    t.nparagraphs = 2;
    t.paragraphs[0].len = 3;
    t.paragraphs[0].style = pc_paragraph_style_default();
    t.paragraphs[1].len = 0;
    t.paragraphs[1].style = pc_paragraph_style_default();
    t.paragraphs[1].style.align = PC_TEXT_ALIGN_RIGHT;
    CHECK(pc_text_layer_paragraph_runs(&t, &pr, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 2);
    CHECK(pr[0].len == 3 && pr[1].len == 0 && pr[1].style.align == PC_TEXT_ALIGN_RIGHT);
    free(pr);
    /* Without the trailing break the last empty run is dropped as before. */
    CHECK(pc_str_set_c(&t.text, "abc", NULL) == PC_OK);
    CHECK(pc_text_layer_paragraph_runs(&t, &pr, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 1);
    free(pr);
    /* Lengths that overflow when summed never match. */
    CHECK(pc_str_set_c(&t.text, "ab\n", NULL) == PC_OK);
    t.paragraphs[0].len = SIZE_MAX;
    CHECK(pc_text_layer_paragraph_runs(&t, &pr, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 1);
    free(pr);
    pc_text_layer_fini(&t);
}

static void test_stroke_validate_and_pattern_limits(void)
{
    pc_shape_stroke s;
    pc_pattern p;
    pc_error e;
    pc_shape_stroke_default(&s);
    CHECK(pc_shape_stroke_validate(&s, &e) == PC_OK);
    s.miter_limit = 0.5f;
    CHECK(pc_shape_stroke_validate(&s, &e) == PC_ERR_INVALID);
    CHECK_EQ_STR(e.msg, "miter limit must be between 1 and 500");
    s.miter_limit = 4.0f;
    s.dash_offset = -INFINITY;
    CHECK(pc_shape_stroke_validate(&s, &e) == PC_ERR_INVALID);
    CHECK_EQ_STR(e.msg, "dash offset must be finite and within \xc2\xb1"
                        "1000000 widths");
    pc_shape_stroke_fini(&s);
    memset(&p, 0, sizeof p);
    p.width = UINT32_MAX;
    p.height = UINT32_MAX;
    CHECK(pc_pattern_pixel_count(&p) == (uint64_t)UINT32_MAX * UINT32_MAX && !pc_pattern_is_paintable(&p));
    p.width = 8192;
    p.height = 8192;
    CHECK(pc_pattern_is_paintable(&p));
}

static void test_effect_switch_and_modes(void)
{
    pc_effect fx;
    pc_fill f = pc_fill_solid(PC_COLOR_WHITE);
    pc_effect_default_drop_shadow(&fx);
    CHECK(pc_effect_enabled(&fx));
    pc_effect_set_enabled(&fx, false);
    CHECK(!pc_effect_enabled(&fx));
    pc_effect_in_mode(&fx, PC_COLOR_MODE_GRAYSCALE);
    CHECK(fx.u.shadow.color.mode == PC_COLOR_MODE_GRAYSCALE);
    pc_effect_fini(&fx);
    pc_fill_in_mode(&f, PC_COLOR_MODE_CMYK);
    CHECK(f.u.solid.mode == PC_COLOR_MODE_CMYK);
    pc_fill_in_mode(&f, PC_COLOR_MODE_CMYK); /* unchanged when already there */
    CHECK(f.u.solid.mode == PC_COLOR_MODE_CMYK);
    pc_fill_fini(&f);
}

int main(int argc, char **argv)
{
    RUN(test_background_is_structural_not_named);
    RUN(test_layer_lookup_agrees_with_walk);
    RUN(test_curves_pencil_defaults_and_compares);
    RUN(test_curve_maps);
    RUN(test_contour_corners);
    RUN(test_smart_contents_id);
    RUN(test_guide_colors);
    RUN(test_metadata_text_and_encoding);
    RUN(test_document_new_fields_clone_and_compare);
    RUN(test_video_original_frames);
    RUN(test_terminal_paragraph_kept);
    RUN(test_stroke_validate_and_pattern_limits);
    RUN(test_effect_switch_and_modes);
    return pc_t_finish();
}
