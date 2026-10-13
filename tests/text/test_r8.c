/* Tests upstream added to crates/text after L13-L15 (resync R8): tabs, navigation at wrap
 * boundaries, box lines with hanging descenders, faux bold under glyph scaling, the font
 * generation, Southeast Asian fallbacks, missing glyph protection, numeric named faces
 * (font_names_tests.rs), localized family names, Fontconfig paths, CJK Fontconfig coverage,
 * scripts.rs, the non-blocking shared engine. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "base/bytes.h"
#include "base/file.h"
#include "base/utf8.h"
#include "harness.h"
#include "text/scripts.h"
#include "text/text.h"

static const pc_pixel_format RGBA8 = {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true};

static pc_char_style style(float size)
{
    pc_char_style s;
    pc_char_style_default(&s);
    s.size_pt = size;
    return s;
}

static void styled(pc_text_layer *t, const char *text, const pc_char_style *s)
{
    pc_text_layer_default(t);
    (void)pc_str_set_c(&t->text, text, NULL);
    t->runs = calloc(1, sizeof *t->runs);
    t->nruns = 1;
    t->runs[0].len = strlen(text);
    (void)pc_char_style_clone(s, &t->runs[0].style, NULL);
}

static void point(pc_text_layer *t, const char *text, float size)
{
    pc_text_layer_default(t);
    (void)pc_str_set_c(&t->text, text, NULL);
    (void)pc_str_set_c(&t->font_family, "Inter", NULL);
    t->size_pt = size;
}

static void with_para(pc_text_layer *t, pc_paragraph_style p)
{
    free(t->paragraphs);
    t->paragraphs = calloc(1, sizeof *t->paragraphs);
    t->nparagraphs = 1;
    t->paragraphs[0].len = t->text.len;
    t->paragraphs[0].style = p;
}

static void set_box(pc_text_layer *t, float x, float y, float w, float h)
{
    t->shape = (pc_text_shape){true, x, y, w, h};
}

static pc_text_layout lay(pc_text_engine *e, const pc_text_layer *t, float dpi)
{
    pc_text_layout l;
    pc_error err;
    if (pc_text_engine_layout(e, t, dpi, &l, &err) != PC_OK) {
        PC_T_FAIL("layout failed: %s", err.msg);
        pc_text_layout_init(&l);
    }
    return l;
}

static float width(const pc_text_layout *l)
{
    float w = 0.0f;
    size_t i;
    for (i = 0; i < l->nlines; i++) {
        float d = l->lines[i].x1 - l->lines[i].x0;
        w = d > w ? d : w;
    }
    return w;
}

/* Pen position of the cluster starting at byte `at` (NAN when none). */
static float cluster_x(const pc_text_layout *l, size_t at)
{
    size_t i;
    for (i = 0; i < l->nclusters; i++) {
        if (l->clusters[i].start == at) {
            return l->clusters[i].x;
        }
    }
    return NAN;
}

static const pc_cluster_info *cluster_at(const pc_text_layout *l, size_t at)
{
    size_t i;
    for (i = 0; i < l->nclusters; i++) {
        if (l->clusters[i].start == at) {
            return &l->clusters[i];
        }
    }
    return NULL;
}

static bool near(float a, float b)
{
    return fabsf(a - b) < 0.05f;
}

static bool no_notdef(const pc_text_layout *l)
{
    size_t i;
    for (i = 0; i < l->nglyphs; i++) {
        if (l->glyphs[i].id == 0) {
            return false;
        }
    }
    return true;
}

/* ---- tests.rs ---- */

static void test_registering_fonts_moves_the_generation(void)
{
    pc_text_engine *e = NULL;
    size_t n;
    const pc_bundled_font *b = pc_text_bundled_fonts(&n);
    uint64_t g;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    g = pc_text_fonts_generation();
    CHECK(pc_fontdb_register_font_data(pc_text_engine_fonts(e), b[0].data, b[0].len, NULL, NULL) == PC_OK);
    CHECK(pc_text_fonts_generation() > g);
    pc_text_engine_free(e);
}

static void test_literal_psd_tabs_shape_as_whitespace_without_shifting_text_offsets(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t, p;
    pc_text_layout l, plain;
    float a, b, top, bottom;
    size_t i;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    point(&t, "A\tB", 20.0f);
    point(&p, "AB", 20.0f);
    l = lay(e, &t, 72.0f);
    plain = lay(e, &p, 72.0f);
    CHECK_EQ_UINT(l.nlines, 1);
    CHECK(no_notdef(&l));
    CHECK(width(&l) > width(&plain));
    for (i = 0; i < l.nclusters; i++) {
        CHECK(l.clusters[i].end <= t.text.len);
    }
    pc_text_layout_caret(&l, 2, &b, &top, &bottom);
    pc_text_layout_caret(&l, 1, &a, &top, &bottom);
    CHECK(b > a);
    pc_text_layout_fini(&l);
    pc_text_layout_fini(&plain);
    pc_text_layer_fini(&t);
    pc_text_layer_fini(&p);
    pc_text_engine_free(e);
}

/* #1881: a tab advances to the next 36 pt stop, from the anchor or the box's left edge. */
static void test_tabs_advance_to_the_next_default_tab_stop(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t;
    pc_text_layout l;
    float cx, top, bottom;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    point(&t, "a\tb", 20.0f);
    l = lay(e, &t, 72.0f);
    CHECK(no_notdef(&l));
    CHECK(near(cluster_x(&l, 2), 36.0f));
    pc_text_layout_caret(&l, 2, &cx, &top, &bottom);
    CHECK(near(cx, 36.0f));
    pc_text_layout_fini(&l);
    l = lay(e, &t, 144.0f);
    CHECK(near(cluster_x(&l, 2), 72.0f));
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    point(&t, "a\t\tb", 20.0f);
    l = lay(e, &t, 72.0f);
    CHECK(near(cluster_x(&l, 3), 72.0f));
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    point(&t, "MMM\tb", 20.0f);
    l = lay(e, &t, 72.0f);
    CHECK(cluster_x(&l, 3) > 36.0f);
    CHECK(near(cluster_x(&l, 4), 72.0f));
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    point(&t, "\tb", 20.0f);
    l = lay(e, &t, 72.0f);
    CHECK(near(cluster_x(&l, 1), 36.0f));
    CHECK(l.nlines > 0 && l.lines[0].x1 > 36.0f);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    point(&t, "a\tb", 20.0f);
    set_box(&t, 10.0f, 0.0f, 300.0f, 100.0f);
    l = lay(e, &t, 72.0f);
    CHECK(near(cluster_x(&l, 2), 46.0f));
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

/* Expanded tabs never push paragraph text past the box's right edge. */
static void test_tabs_wrap_inside_the_box(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t;
    pc_text_layout l;
    const pc_cluster_info *c4, *c6;
    size_t i;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    point(&t, "a\tb\tc\td\te\tf\tg", 20.0f);
    set_box(&t, 0.0f, 0.0f, 100.0f, 1000.0f);
    l = lay(e, &t, 72.0f);
    CHECK(l.nlines > 1);
    c4 = cluster_at(&l, 4);
    c6 = cluster_at(&l, 6);
    CHECK(c4 != NULL && c6 != NULL && c4->line == 0 && c6->line == 1);
    CHECK(fabsf(cluster_x(&l, 6)) < 0.05f);
    for (i = 0; i < l.nclusters; i++) {
        const pc_cluster_info *c = &l.clusters[i];
        if (!(c->end == c->start + 1 && t.text.ptr[c->start] == '\t')) {
            CHECK(c->x + c->advance <= 100.0f + 1e-3f);
        }
    }
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

/* Right-aligned and centred text keep their alignment around the expanded tab. */
static void test_tabs_keep_point_text_alignment(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t;
    pc_text_layout r;
    pc_paragraph_style ps = pc_paragraph_style_default();
    const pc_cluster_info *b;
    int k;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    ps.align = PC_TEXT_ALIGN_RIGHT;
    point(&t, "a\tb", 20.0f);
    with_para(&t, ps);
    r = lay(e, &t, 72.0f);
    CHECK(r.nlines == 1 && fabsf(r.lines[0].x1) < 0.05f);
    b = cluster_at(&r, 2);
    CHECK(b != NULL && fabsf(b->x + b->advance) < 0.05f);
    CHECK(r.nlines == 1 && r.lines[0].x0 < -36.0f);
    pc_text_layout_fini(&r);
    pc_text_layer_fini(&t);
    for (k = 0; k < 2; k++) {
        point(&t, k == 0 ? "a\tb" : "a\tb\t", 20.0f);
        set_box(&t, 0.0f, 0.0f, 200.0f, 100.0f);
        with_para(&t, ps);
        r = lay(e, &t, 72.0f);
        b = cluster_at(&r, 2);
        CHECK(b != NULL && fabsf(b->x + b->advance - 200.0f) < 0.5f);
        CHECK(r.nlines > 0 && fabsf(r.lines[0].x1 - 200.0f) < 0.5f);
        pc_text_layout_fini(&r);
        pc_text_layer_fini(&t);
    }
    pc_text_engine_free(e);
}

static pc_rect ink_rect(pc_text_engine *e, const pc_char_style *s)
{
    pc_text_layer t;
    pc_rendered r;
    pc_rect rect = {0, 0, 0, 0};
    styled(&t, "Hi", s);
    t.transform = pc_affine_translate(5.0, 50.0);
    if (pc_text_engine_render(e, &t, 72.0f, RGBA8, NULL, &r, NULL) == PC_OK) {
        rect = r.rect;
        pc_rendered_fini(&r);
    }
    pc_text_layer_fini(&t);
    return rect;
}

/* #3705: faux bold dilation scales with horizontal and vertical glyph scaling. */
static void test_faux_bold_scales_with_glyph_scaling(void)
{
    pc_text_engine *e = NULL;
    pc_char_style base = style(40.0f), bold, h, hb, v, vb;
    pc_rect p, pb, ph, phb, pv, pvb;
    int32_t ew, eh, hw, vh;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    bold = base;
    bold.faux_bold = true;
    p = ink_rect(e, &base);
    pb = ink_rect(e, &bold);
    ew = (pb.x1 - pb.x0) - (p.x1 - p.x0);
    eh = (pb.y1 - pb.y0) - (p.y1 - p.y0);
    h = style(40.0f);
    h.horizontal_scale = 2.0f;
    hb = h;
    hb.faux_bold = true;
    ph = ink_rect(e, &h);
    phb = ink_rect(e, &hb);
    hw = (phb.x1 - phb.x0) - (ph.x1 - ph.x0);
    CHECK(hw >= ew * 2 - 1 && hw <= ew * 2 + 1);
    v = style(40.0f);
    v.vertical_scale = 2.0f;
    vb = v;
    vb.faux_bold = true;
    pv = ink_rect(e, &v);
    pvb = ink_rect(e, &vb);
    vh = (pvb.y1 - pvb.y0) - (pv.y1 - pv.y0);
    CHECK(vh >= eh * 2 - 1 && vh <= eh * 2 + 1);
    pc_text_engine_free(e);
}

static size_t chars_before(const pc_text_layer *t, size_t byte)
{
    return pc_text_char_index(t->text.ptr, t->text.len, byte);
}

static void test_wrapped_and_forced_line_starts_use_the_following_line(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t;
    pc_text_layout l;
    size_t boundary, index, ci = 0, li = 9;
    float x, y;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    point(&t, "ab cd", 12.0f);
    set_box(&t, 0.0f, 0.0f, 20.0f, 70.0f);
    l = lay(e, &t, 72.0f);
    CHECK(l.nlines >= 2);
    if (l.nlines >= 2) {
        boundary = l.lines[1].start;
        CHECK_EQ_UINT(l.lines[0].end, boundary);
        index = chars_before(&t, boundary);
        CHECK_EQ_UINT(pc_text_line_index(&l, pc_text_byte_index(t.text.ptr, t.text.len, index)), 1);
        CHECK_EQ_UINT(pc_text_line_edge(&l, t.text.ptr, t.text.len, index, false), index);
        CHECK_EQ_UINT(pc_text_line_edge(&l, t.text.ptr, t.text.len, index, true),
                      chars_before(&t, l.lines[1].end));
        pc_text_layout_to_text(&l, l.lines[0].x1, l.lines[0].baseline, &x, &y);
        pc_text_hit_char(&l, t.text.ptr, t.text.len, x, y, &ci, &li);
        CHECK_EQ_UINT(li, 0);
    }
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    point(&t,
          "a\x03"
          "b",
          12.0f);
    l = lay(e, &t, 72.0f);
    CHECK_EQ_UINT(l.nlines, 2);
    if (l.nlines == 2) {
        size_t start = l.lines[1].start;
        index = chars_before(&t, start);
        CHECK_EQ_UINT(pc_text_line_index(&l, start), 1);
        CHECK_EQ_UINT(pc_text_line_edge(&l, t.text.ptr, t.text.len, index, false), index);
        CHECK_EQ_UINT(pc_text_line_edge(&l, t.text.ptr, t.text.len, index, true), 3);
    }
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    point(&t, "a\nb", 12.0f);
    l = lay(e, &t, 72.0f);
    CHECK(l.nlines == 2);
    if (l.nlines == 2) {
        CHECK_EQ_UINT(pc_text_line_index(&l, l.lines[0].end), 0);
        CHECK_EQ_UINT(pc_text_line_index(&l, l.lines[1].start), 1);
        CHECK_EQ_UINT(pc_text_line_index(&l, t.text.len), 1);
    }
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

static void test_line_navigation_reaches_adjacent_line_with_large_leading(void)
{
    pc_text_engine *e = NULL;
    pc_char_style s = style(12.0f);
    pc_text_layer t;
    pc_text_layout l;
    float x, top, bottom;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    (void)pc_str_set_c(&s.font_family, "Inter", NULL);
    s.has_leading = true;
    s.leading_pt = 40.0f;
    styled(&t, "ab\ncd", &s);
    l = lay(e, &t, 72.0f);
    CHECK_EQ_UINT(l.nlines, 2);
    if (l.nlines == 2) {
        CHECK(fabsf(l.lines[1].baseline - l.lines[0].baseline - 40.0f) < 0.01f);
    }
    pc_text_layout_caret(&l, 1, &x, &top, &bottom);
    CHECK_EQ_UINT(pc_text_line_step(&l, t.text.ptr, t.text.len, 1, x, 1), 4);
    pc_text_layout_caret(&l, 4, &x, &top, &bottom);
    CHECK_EQ_UINT(pc_text_line_step(&l, t.text.ptr, t.text.len, 4, x, -1), 1);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    pc_char_style_fini(&s);
    pc_text_engine_free(e);
}

/* #1909, #218: every platform has Thai, Khmer, Lao and Myanmar families ahead of the last
 * resorts. */
static void test_southeast_asian_fallback_candidates_cover_every_platform(void)
{
    static const char *const WANT[] = {
        "Leelawadee UI",    "Tahoma", "Thonburi",      "Noto Sans Thai", "Khmer UI",     "Khmer Sangam MN",
        "Noto Sans Khmer",  "Lao UI", "Lao Sangam MN", "Noto Sans Lao",  "Myanmar Text", "Myanmar Sangam MN",
        "Noto Sans Myanmar"};
    int o;
    for (o = 0; o < 2; o++) {
        pc_cjk_script order[4];
        const char **fb = NULL;
        size_t n = 0, i, w, last = SIZE_MAX;
        pc_cjk_script_order(o == 0 ? NULL : "ja", order);
        CHECK(pc_text_fallback_candidates(order, &fb, &n, NULL) == PC_OK);
        for (i = 0; i < n && last == SIZE_MAX; i++) {
            if (strcmp(fb[i], "Arial Unicode MS") == 0) {
                last = i;
            }
        }
        for (w = 0; w < sizeof WANT / sizeof WANT[0]; w++) {
            size_t at = SIZE_MAX;
            for (i = 0; i < n && at == SIZE_MAX; i++) {
                if (strcmp(fb[i], WANT[w]) == 0) {
                    at = i;
                }
            }
            if (!(at != SIZE_MAX && at < last)) {
                PC_T_FAIL("%s missing or after the last resorts", WANT[w]);
            }
        }
        free(fb);
    }
}

/* With system fonts: Thai / Khmer / Lao / Myanmar in a Latin font fall back to an installed
 * family (skipped per script when none is installed). */
static void test_southeast_asian_scripts_fall_back_to_installed_fonts(void)
{
    static const char *const SAMPLES[4] = {
        "\xE0\xB8\xA0\xE0\xB8\xB2\xE0\xB8\xA9\xE0\xB8\xB2\xE0\xB9\x84\xE0\xB8\x97\xE0\xB8\xA2",
        "\xE1\x9E\x97\xE1\x9E\xB6\xE1\x9E\x9F\xE1\x9E\xB6\xE1\x9E\x81\xE1\x9F\x92\xE1\x9E\x98\xE1\x9F\x82\xE1"
        "\x9E\x9A",
        "\xE0\xBA\x9E\xE0\xBA\xB2\xE0\xBA\xAA\xE0\xBA\xB2\xE0\xBA\xA5\xE0\xBA\xB2\xE0\xBA\xA7",
        "\xE1\x80\x99\xE1\x80\xBC\xE1\x80\x94\xE1\x80\xBA\xE1\x80\x99\xE1\x80\xAC"};
    const char *const *lists[4] = {PC_TEXT_THAI_FAMILIES, PC_TEXT_KHMER_FAMILIES, PC_TEXT_LAO_FAMILIES,
                                   PC_TEXT_MYANMAR_FAMILIES};
    pc_text_engine *e = NULL;
    int s;
    if (getenv("PHOTOCRAFT_SLOW_TESTS") == NULL && getenv("PC_TEST_SYSTEM_FONTS") == NULL) {
        (void)printf("(skipped: system font scan; set PC_TEST_SYSTEM_FONTS=1)\n");
        return;
    }
    CHECK(pc_text_engine_with_system_fonts(&e, NULL) == PC_OK);
    for (s = 0; e != NULL && s < 4; s++) {
        const char *const *f;
        const char *hit = NULL;
        pc_text_layer t;
        pc_text_layout l;
        char buf[128];
        for (f = lists[s]; *f != NULL && hit == NULL; f++) {
            if (pc_fontdb_has_family(pc_text_engine_fonts(e), *f, strlen(*f))) {
                hit = *f;
            }
        }
        if (hit == NULL) {
            (void)printf("(skipped: no font for script %d)\n", s);
            continue;
        }
        (void)snprintf(buf, sizeof buf, "Latin %s", SAMPLES[s]);
        point(&t, buf, 24.0f);
        l = lay(e, &t, 72.0f);
        CHECK(no_notdef(&l));
        CHECK(l.nglyphs > 1 && l.glyphs[0].face != l.glyphs[l.nglyphs - 1].face);
        pc_text_layout_fini(&l);
        pc_text_layer_fini(&t);
    }
    pc_text_engine_free(e);
}

/* #1317: a box line whose baseline is inside keeps its hanging descent. */
static void test_box_lines_whose_descent_hangs_below_the_box_are_kept(void)
{
    pc_text_engine *e = NULL;
    pc_char_style s = style(25.0f);
    pc_text_layer t;
    pc_text_layout l;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    s.has_leading = true;
    s.leading_pt = 35.0f;
    styled(&t, "one\ntwo\nthree", &s);
    set_box(&t, 0.0f, 0.0f, 300.0f, 91.27f);
    l = lay(e, &t, 72.0f);
    CHECK_EQ_UINT(l.nlines, 3);
    CHECK(l.nlines == 3 && l.lines[2].baseline <= 91.27f + 0.5f);
    pc_text_layout_fini(&l);
    set_box(&t, 0.0f, 0.0f, 300.0f, 80.0f);
    l = lay(e, &t, 72.0f);
    CHECK_EQ_UINT(l.nlines, 2);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

/* Missing glyph protection off: only the default family is in the fallback stack. */
static void test_missing_glyph_protection_can_turn_the_fallback_stack_off(void)
{
    pc_fontdb *db = NULL;
    size_t ncf, i;
    const pc_craft_font *cf = pc_craft_fonts(&ncf);
    pc_buf dv = PC_BUF_INIT;
    CHECK(pc_fontdb_new(&db, NULL) == PC_OK);
    /* Any coverage family will do: craft-fonts' Cairo (as upstream) or an installed DejaVu Sans. */
    for (i = 0; i < ncf; i++) {
        if (strcmp(cf[i].family, "Cairo") == 0) {
            CHECK(pc_fontdb_register_font_data(db, pc_blob_data(cf[i].bytes), pc_blob_len(cf[i].bytes), NULL,
                                               NULL) == PC_OK);
        }
    }
    if (pc_file_read("/usr/share/fonts/dejavu/DejaVuSans.ttf", (size_t)1 << 26, &dv, NULL) == PC_OK) {
        CHECK(pc_fontdb_register_font_data(db, dv.data, dv.len, NULL, NULL) == PC_OK);
    }
    pc_buf_fini(&dv);
    CHECK(pc_fontdb_missing_glyph_protection(db));
    if (pc_fontdb_fallback_count(db) < 2) {
        (void)printf("(skipped: no coverage font)\n");
        pc_fontdb_free(db);
        return;
    }
    pc_fontdb_set_missing_glyph_protection(db, false);
    CHECK_EQ_UINT(pc_fontdb_fallback_count(db), 1);
    CHECK_EQ_STR(pc_fontdb_fallback(db, 0), PC_TEXT_DEFAULT_FAMILY);
    CHECK(pc_fontdb_fallback(db, 1) == NULL);
    pc_fontdb_set_missing_glyph_protection(db, true);
    CHECK(pc_fontdb_fallback_count(db) >= 2);
    pc_fontdb_free(db);
}

/* ---- font_names_tests.rs: numeric subfamilies without redistributing proprietary fonts ---- */

/* A copy of `src` whose OS/2 weight is 400 and whose name table (appended) names `names`
 * (name id, language id, value). */
static pc_buf with_names(const pc_bundled_font *src, const uint16_t (*ids)[2], const char *const *values,
                         size_t n)
{
    pc_buf b = PC_BUF_INIT, table = PC_BUF_INIT, strings = PC_BUF_INIT;
    size_t count, i, name_record = 0;
    uint8_t tmp[4];
    pc_buf_append(&b, src->data, src->len, NULL);
    count = pc_load_be16(b.data + 4);
    for (i = 0; i < count; i++) {
        size_t r = 12 + i * 16;
        uint32_t off = pc_load_be32(b.data + r + 8);
        if (memcmp(b.data + r, "name", 4) == 0) {
            name_record = r;
        } else if (memcmp(b.data + r, "OS/2", 4) == 0) {
            pc_store_be16(b.data + off + 4, 400);
        }
    }
    pc_store_be16(tmp, 0);
    pc_buf_append(&table, tmp, 2, NULL);
    pc_store_be16(tmp, (uint16_t)n);
    pc_buf_append(&table, tmp, 2, NULL);
    pc_store_be16(tmp, (uint16_t)(6 + n * 12));
    pc_buf_append(&table, tmp, 2, NULL);
    for (i = 0; i < n; i++) {
        size_t at = 0, start = strings.len;
        const char *v = values[i];
        uint16_t rec[6];
        int k;
        while (at < strlen(v)) {
            uint32_t cp;
            (void)pc_utf8_next((const uint8_t *)v, strlen(v), &at, &cp);
            pc_store_be16(tmp, (uint16_t)cp);
            pc_buf_append(&strings, tmp, 2, NULL);
        }
        rec[0] = 3;
        rec[1] = 1;
        rec[2] = ids[i][1];
        rec[3] = ids[i][0];
        rec[4] = (uint16_t)(strings.len - start);
        rec[5] = (uint16_t)start;
        for (k = 0; k < 6; k++) {
            pc_store_be16(tmp, rec[k]);
            pc_buf_append(&table, tmp, 2, NULL);
        }
    }
    pc_buf_append(&table, strings.data, strings.len, NULL);
    pc_store_be32(b.data + name_record + 8, (uint32_t)b.len);
    pc_store_be32(b.data + name_record + 12, (uint32_t)table.len);
    pc_buf_append(&b, table.data, table.len, NULL);
    pc_buf_fini(&table);
    pc_buf_fini(&strings);
    return b;
}

static pc_buf numeric_font(size_t bundled, const char *style, const char *ps)
{
    static const uint16_t IDS[5][2] = {{1, 0x409}, {2, 0x409}, {6, 0x409}, {16, 0x409}, {17, 0x409}};
    size_t n;
    const pc_bundled_font *b = pc_text_bundled_fonts(&n);
    char legacy[64];
    const char *values[5];
    (void)snprintf(legacy, sizeof legacy, "Numeric Test %s", style);
    values[0] = legacy;
    values[1] = "Regular";
    values[2] = ps;
    values[3] = "Numeric Test";
    values[4] = style;
    return with_names(&b[bundled], IDS, values, 5);
}

static pc_face_info *faces_of(pc_fontdb *db, const char *fam, size_t *n)
{
    pc_face_info *f = NULL;
    CHECK(pc_fontdb_faces(db, fam, strlen(fam), &f, n, NULL) == PC_OK);
    return f;
}

static void test_numeric_subfamilies_with_identical_weights_select_distinct_faces(void)
{
    pc_fontdb *db = NULL;
    pc_buf a = numeric_font(0, "20", "UnrelatedPS20"), b = numeric_font(2, "30", "UnrelatedPS30");
    pc_face_info *f;
    size_t n = 0;
    pc_resolved_font r;
    pc_char_style sa, sb;
    pc_strlist fams = {NULL, 0};
    char alias[256];
    size_t i;
    CHECK(pc_fontdb_new(&db, NULL) == PC_OK);
    CHECK(pc_fontdb_register_font_data(db, a.data, a.len, NULL, NULL) == PC_OK);
    CHECK(pc_fontdb_register_font_data(db, b.data, b.len, NULL, NULL) == PC_OK);
    f = faces_of(db, "Numeric Test", &n);
    CHECK(n == 2 && strcmp(f[0].style, "20") == 0 && strcmp(f[1].style, "30") == 0);
    CHECK(n == 2 && f[0].weight == 400.0f && f[1].weight == 400.0f);
    pc_face_infos_free(f, n);
    memset(&r, 0, sizeof r);
    CHECK(pc_fontdb_resolve_postscript(db, "UnrelatedPS30", 13, &r, NULL) == PC_OK);
    CHECK(r.family != NULL && strcmp(r.family, "Numeric Test") == 0 && r.exact);
    pc_resolved_font_fini(&r);
    pc_char_style_default(&sa);
    (void)pc_str_set_c(&sa.font_family, "Numeric Test", NULL);
    (void)pc_str_set_c(&sa.font_style, "20", NULL);
    (void)pc_char_style_clone(&sa, &sb, NULL);
    (void)pc_str_set_c(&sb.font_style, "30", NULL);
    CHECK(pc_fontdb_select_named_face(db, &sa, NULL) == PC_OK);
    CHECK(pc_fontdb_select_named_face(db, &sb, NULL) == PC_OK);
    CHECK(strcmp(pc_str_c(&sa.font_family), pc_str_c(&sb.font_family)) != 0);
    f = faces_of(db, pc_str_c(&sa.font_family), &n);
    CHECK(n >= 1 && f[0].postscript_name != NULL && strcmp(f[0].postscript_name, "UnrelatedPS20") == 0);
    pc_face_infos_free(f, n);
    f = faces_of(db, pc_str_c(&sb.font_family), &n);
    CHECK(n >= 1 && f[0].postscript_name != NULL && strcmp(f[0].postscript_name, "UnrelatedPS30") == 0);
    pc_face_infos_free(f, n);
    CHECK(pc_fontdb_families(db, &fams, NULL) == PC_OK);
    for (i = 0; i < fams.n; i++) {
        CHECK(strncmp(fams.v[i], ".PhotoCraft-face-", 17) != 0);
    }
    pc_strlist_fini(&fams);
    (void)snprintf(alias, sizeof alias, "%s", pc_str_c(&sb.font_family));
    (void)pc_str_set_c(&sb.font_family, "Numeric Test", NULL);
    CHECK(pc_fontdb_select_named_face(db, &sb, NULL) == PC_OK);
    CHECK_EQ_STR(pc_str_c(&sb.font_family), alias);
    pc_char_style_fini(&sa);
    pc_char_style_fini(&sb);
    pc_buf_fini(&a);
    pc_buf_fini(&b);
    pc_fontdb_free(db);
}

static void test_numeric_styles_shape_different_outlines_and_keep_psd_identity(void)
{
    pc_text_engine *e = NULL;
    pc_buf a = numeric_font(0, "20", "UnrelatedPS20"), b = numeric_font(2, "30", "UnrelatedPS30");
    pc_text_layer la, lb, back;
    pc_text_layout ya, yb, yc;
    pc_psd_bytes bytes = {NULL, 0};
    const char *styles[2] = {"20", "30"};
    pc_text_layer *ls[2] = {&la, &lb};
    int k;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    CHECK(pc_fontdb_register_font_data(pc_text_engine_fonts(e), a.data, a.len, NULL, NULL) == PC_OK);
    CHECK(pc_fontdb_register_font_data(pc_text_engine_fonts(e), b.data, b.len, NULL, NULL) == PC_OK);
    for (k = 0; k < 2; k++) {
        pc_char_style s = style(32.0f);
        char ps[32];
        (void)pc_str_set_c(&s.font_family, "Numeric Test", NULL);
        (void)pc_str_set_c(&s.font_style, styles[k], NULL);
        (void)snprintf(ps, sizeof ps, "UnrelatedPS%s", styles[k]);
        s.has_postscript_name = true;
        (void)pc_str_set_c(&s.postscript_name, ps, NULL);
        styled(ls[k], "Hamburgefonstiv", &s);
        ls[k]->runs[0].len = 14;
        pc_char_style_fini(&s);
    }
    ya = lay(e, &la, 72.0f);
    yb = lay(e, &lb, 72.0f);
    CHECK(ya.nlines > 0 && yb.nlines > 0 && ya.lines[0].x1 != yb.lines[0].x1);
    CHECK(pc_text_build_tysh(&lb, 72.0f, NULL, &bytes, NULL) == PC_OK);
    CHECK(pc_text_layer_from_tysh(bytes.data, bytes.len, 72.0f, &back, NULL) == PC_OK);
    CHECK(back.nruns > 0 && back.runs[0].style.has_postscript_name &&
          pc_str_eq_c(&back.runs[0].style.postscript_name, "UnrelatedPS30"));
    yc = lay(e, &back, 72.0f);
    CHECK(yb.nlines > 0 && yc.nlines > 0 && fabsf(yb.lines[0].x1 - yc.lines[0].x1) < 0.01f);
    pc_text_layout_fini(&ya);
    pc_text_layout_fini(&yb);
    pc_text_layout_fini(&yc);
    pc_text_layer_fini(&la);
    pc_text_layer_fini(&lb);
    pc_text_layer_fini(&back);
    pc_psd_bytes_fini(&bytes);
    pc_buf_fini(&a);
    pc_buf_fini(&b);
    pc_text_engine_free(e);
}

static void test_explicit_weight_is_not_overridden_by_a_stale_style_name(void)
{
    pc_fontdb *db = NULL;
    pc_char_style s;
    pc_face_info *f = NULL;
    bool found = false;
    pc_blob *zero = NULL, *iso = NULL;
    uint8_t z[16] = {0};
    bool ok = true;
    CHECK(pc_fontdb_new(&db, NULL) == PC_OK);
    pc_char_style_default(&s);
    (void)pc_str_set_c(&s.font_family, "Inter", NULL);
    (void)pc_str_set_c(&s.font_style, "Regular", NULL);
    s.weight = 600;
    CHECK(pc_fontdb_select_named_face(db, &s, NULL) == PC_OK);
    CHECK_EQ_STR(pc_str_c(&s.font_family), "Inter");
    CHECK_EQ_UINT(s.weight, 600);
    CHECK(pc_fontdb_named_face(db, "Inter", "Medium", &f, &found, NULL) == PC_OK && found);
    pc_face_infos_free(f, found ? 1 : 0);
    CHECK(pc_fontdb_named_face(db, "missing", "20", &f, &found, NULL) == PC_OK && !found);
    CHECK(pc_blob_new(z, sizeof z, &zero, NULL) == PC_OK);
    CHECK(pc_text_isolated_face(zero, 0, &iso, &ok, NULL) == PC_OK && !ok);
    pc_blob_release(zero);
    pc_char_style_fini(&s);
    pc_fontdb_free(db);
}

static void test_collection_isolation_preserves_requested_face(void)
{
    pc_buf a = numeric_font(0, "20", "Collection20"), b = numeric_font(2, "30", "Collection30");
    pc_buf ttc = PC_BUF_INIT;
    uint8_t head[20] = "ttcf\0\x01\0\0\0\0\0\x02";
    pc_buf *src[2] = {&a, &b};
    size_t k, i;
    pc_blob *blob = NULL, *iso = NULL;
    bool ok = false;
    pc_fontdb *db = NULL;
    pc_char_style s;
    pc_face_info *f;
    size_t n = 0;
    pc_sfnt sf;
    pc_buf name = PC_BUF_INIT;
    pc_store_be32(head + 12, 20);
    pc_store_be32(head + 16, (uint32_t)(20 + a.len));
    pc_buf_append(&ttc, head, 20, NULL);
    for (k = 0; k < 2; k++) {
        size_t start = ttc.len, count;
        pc_buf_append(&ttc, src[k]->data, src[k]->len, NULL);
        count = pc_load_be16(ttc.data + start + 4);
        for (i = 0; i < count; i++) {
            uint8_t *r = ttc.data + start + 12 + i * 16 + 8;
            pc_store_be32(r, pc_load_be32(r) + (uint32_t)start);
        }
    }
    CHECK(pc_blob_new(ttc.data, ttc.len, &blob, NULL) == PC_OK);
    CHECK(pc_text_isolated_face(blob, 2, &iso, &ok, NULL) == PC_OK && !ok);
    CHECK(pc_fontdb_new(&db, NULL) == PC_OK);
    CHECK(pc_fontdb_register_font_data(db, ttc.data, ttc.len, NULL, NULL) == PC_OK);
    pc_char_style_default(&s);
    (void)pc_str_set_c(&s.font_family, "Numeric Test", NULL);
    (void)pc_str_set_c(&s.font_style, "30", NULL);
    CHECK(pc_fontdb_select_named_face(db, &s, NULL) == PC_OK);
    f = faces_of(db, pc_str_c(&s.font_family), &n);
    CHECK(n >= 1 && f[0].postscript_name != NULL && strcmp(f[0].postscript_name, "Collection30") == 0);
    pc_face_infos_free(f, n);
    CHECK(pc_text_isolated_face(blob, 1, &iso, &ok, NULL) == PC_OK && ok);
    if (ok) {
        CHECK(pc_sfnt_open(pc_blob_data(iso), pc_blob_len(iso), 0, &sf));
        CHECK(pc_sfnt_name(&sf, PC_SFNT_NAME_POSTSCRIPT, PC_SFNT_NAME_SKRIFA, &name, NULL));
        CHECK(name.len == 12 && memcmp(name.data, "Collection30", 12) == 0);
        pc_blob_release(iso);
    }
    pc_buf_fini(&name);
    pc_char_style_fini(&s);
    pc_fontdb_free(db);
    pc_blob_release(blob);
    pc_buf_fini(&ttc);
    pc_buf_fini(&a);
    pc_buf_fini(&b);
}

/* Show Font Names in English off: a font naming its family in Japanese is listed by that name. */
static void test_localized_family_names_map_english_families_to_native_names(void)
{
    static const uint16_t IDS[6][2] = {{1, 0x409},  {2, 0x409}, {6, 0x409},
                                       {16, 0x409}, {1, 0x411}, {16, 0x411}};
    static const char *const VALUES[6] = {
        "Gothic Test",
        "Regular",
        "GothicTest",
        "Gothic Test",
        "\xE3\x82\xB4\xE3\x82\xB7\xE3\x83\x83\xE3\x82\xAF \xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88",
        "\xE3\x82\xB4\xE3\x82\xB7\xE3\x83\x83\xE3\x82\xAF \xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88"};
    size_t n, i;
    const pc_bundled_font *bf = pc_text_bundled_fonts(&n);
    pc_buf f = with_names(&bf[0], IDS, VALUES, 6);
    pc_fontdb *db = NULL;
    pc_strlist names = {NULL, 0}, local = {NULL, 0};
    bool found = false;
    CHECK(pc_fontdb_new(&db, NULL) == PC_OK);
    CHECK(pc_fontdb_register_font_data(db, f.data, f.len, &names, NULL) == PC_OK);
    CHECK(names.n == 1 && strcmp(names.v[0], "Gothic Test") == 0);
    CHECK(pc_fontdb_localized_family_names(db, &local, NULL) == PC_OK);
    for (i = 0; i + 1 < local.n; i += 2) {
        CHECK(strcmp(local.v[i], PC_TEXT_DEFAULT_FAMILY) != 0 && local.v[i + 1][0] != '\0');
        if (strcmp(local.v[i], "Gothic Test") == 0) {
            found = strcmp(local.v[i + 1], VALUES[4]) == 0;
        }
    }
    CHECK(found);
    pc_strlist_fini(&names);
    pc_strlist_fini(&local);
    pc_fontdb_free(db);
    pc_buf_fini(&f);
}

/* ---- fonts.rs fontconfig_tests, cjk.rs, scripts.rs ---- */

static void test_parses_unique_absolute_font_paths_from_fontconfig(void)
{
    static const char OUT[] = "/usr/share/fonts/example.ttf\nrelative.otf\n/usr/share/fonts/README.txt\n"
                              "/usr/local/fonts/collection.OTC\n/usr/share/fonts/example.ttf\n\n";
    pc_strlist l = {NULL, 0};
    CHECK(pc_text_fontconfig_paths_from_output((const uint8_t *)OUT, sizeof OUT - 1, &l, NULL) == PC_OK);
    CHECK_EQ_UINT(l.n, 2);
    if (l.n == 2) {
        CHECK_EQ_STR(l.v[0], "/usr/share/fonts/example.ttf");
        CHECK_EQ_STR(l.v[1], "/usr/local/fonts/collection.OTC");
    }
    pc_strlist_fini(&l);
}

static void test_fontconfig_candidates_must_cover_the_requested_script(void)
{
    size_t n;
    const pc_bundled_font *b = pc_text_bundled_fonts(&n);
    int s;
    for (s = 0; s < 4; s++) {
        CHECK(!pc_cjk_font_covers_sample(b[0].data, b[0].len, "Inter", (pc_cjk_script)s));
    }
}

static void test_fontconfig_matches_cover_the_requested_script(void)
{
    int s;
    for (s = 0; s < 4; s++) {
        pc_cjk_font_file f;
        bool found = false;
        pc_buf bytes = PC_BUF_INIT;
        CHECK(pc_cjk_fontconfig_match((pc_cjk_script)s, &f, &found, NULL) == PC_OK);
        if (!found) {
            continue;
        }
        CHECK(pc_file_read(f.path, (size_t)1 << 28, &bytes, NULL) == PC_OK);
        CHECK(pc_cjk_font_covers_sample(bytes.data, bytes.len, f.family, (pc_cjk_script)s));
        pc_buf_fini(&bytes);
        free(f.path);
        free(f.family);
    }
    /* The background scan finishes and is not pending afterwards. */
    pc_cjk_start_fontconfig_scan();
    {
        time_t t0 = time(NULL);
        while (pc_cjk_fontconfig_scan_pending() && time(NULL) - t0 < 10) {
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, NULL);
        }
    }
    CHECK(!pc_cjk_fontconfig_scan_pending());
}

static void test_classifies_scripts_and_leaves_latin_and_cjk_alone(void)
{
    static const struct {
        uint32_t c;
        pc_text_script s;
    } YES[] = {{0x0E2A, PC_SCRIPT_THAI},   {0x0E21, PC_SCRIPT_THAI},   {0x0631, PC_SCRIPT_ARABIC},
               {0xFEFB, PC_SCRIPT_ARABIC}, {0x05E9, PC_SCRIPT_HEBREW}, {0x0928, PC_SCRIPT_DEVANAGARI},
               {0x0BA4, PC_SCRIPT_TAMIL},  {0x1780, PC_SCRIPT_KHMER},  {0x10D0, PC_SCRIPT_GEORGIAN}};
    static const uint32_t NO[] = {'A', 0xE9, 0x416, 0x3A9, 0x2013, 0x65E5, 0x30AB, 0xD55C, ' '};
    size_t i;
    int s;
    for (i = 0; i < sizeof YES / sizeof YES[0]; i++) {
        pc_text_script got = PC_SCRIPT_COUNT;
        CHECK(pc_text_script_classify(YES[i].c, &got) && got == YES[i].s);
    }
    for (i = 0; i < sizeof NO / sizeof NO[0]; i++) {
        pc_text_script got;
        CHECK(!pc_text_script_classify(NO[i], &got));
    }
    for (s = 0; s < PC_SCRIPT_COUNT; s++) {
        CHECK(pc_text_script_file_names((pc_text_script)s)[0] != NULL);
    }
}

static void test_candidates_match_file_names_regular_first_across_layouts(void)
{
    static const char *const FILES[] = {"/usr/share/fonts/google-noto/NotoSansThai-Bold.ttf",
                                        "/usr/share/fonts/google-noto-vf/NotoSansThai[wght].ttf",
                                        "/usr/share/fonts/truetype/noto/NotoSansThai-Regular.ttf",
                                        "/usr/share/fonts/truetype/noto/NotoSansThaiLooped-Regular.ttf",
                                        "/usr/share/fonts/truetype/noto/NotoSansThaana-Regular.ttf",
                                        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                                        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                                        "/usr/share/fonts/truetype/lohit-devanagari/Lohit-Devanagari.ttf",
                                        "/usr/share/fonts/Lohit-Tamil.ttf",
                                        "/mnt/c/Windows/Fonts/LeelawUI.ttf",
                                        "/mnt/c/Windows/Fonts/Nirmala.ttc",
                                        "/System/Library/Fonts/Supplemental/Thonburi.ttc",
                                        "/System/Library/Fonts/Supplemental/Kohinoor.ttc",
                                        "/usr/share/fonts/README.txt"};
    static const char *const THAI[] = {
        "NotoSansThai-Regular.ttf",       "NotoSansThai[wght].ttf", "NotoSansThai-Bold.ttf",
        "NotoSansThaiLooped-Regular.ttf", "LeelawUI.ttf",           "Thonburi.ttc"};
    static const char *const DEVA[] = {"Nirmala.ttc", "Kohinoor.ttc", "Lohit-Devanagari.ttf"};
    pc_strlist l = {NULL, 0};
    size_t i;
    CHECK(pc_text_script_candidates(PC_SCRIPT_THAI, FILES, 15, &l, NULL) == PC_OK);
    CHECK_EQ_UINT(l.n, 6);
    for (i = 0; i < l.n && i < 6; i++) {
        CHECK_EQ_STR(strrchr(l.v[i], '/') + 1, THAI[i]);
    }
    pc_strlist_fini(&l);
    CHECK(pc_text_script_candidates(PC_SCRIPT_DEVANAGARI, FILES, 15, &l, NULL) == PC_OK);
    CHECK_EQ_UINT(l.n, 3);
    for (i = 0; i < l.n && i < 3; i++) {
        CHECK_EQ_STR(strrchr(l.v[i], '/') + 1, DEVA[i]);
    }
    pc_strlist_fini(&l);
    CHECK(pc_text_script_candidates(PC_SCRIPT_ETHIOPIC, FILES, 15, &l, NULL) == PC_OK && l.n == 0);
    pc_strlist_fini(&l);
    CHECK(pc_text_script_candidates(PC_SCRIPT_THAI, NULL, 0, &l, NULL) == PC_OK && l.n == 0);
    pc_strlist_fini(&l);
    CHECK(pc_text_system_font_files() != NULL);
}

static void test_covering_face_checks_the_character_map_and_rejects_garbage(void)
{
    static const char *const BAD[4] = {"", "ttcf", "ttcf\0\x01\0\0\0\0\0\x02\xff\xff\xff\xff\0\0\0\x10",
                                       "\0\x01\0\0\0\xff"};
    static const size_t LEN[4] = {0, 4, 20, 6};
    size_t n, i;
    const pc_bundled_font *b = pc_text_bundled_fonts(&n);
    uint32_t face = 9;
    CHECK(pc_text_covering_face(b[0].data, b[0].len, 'A', &face) && face == 0);
    CHECK(!pc_text_covering_face(b[0].data, b[0].len, 0x0E2A, &face));
    CHECK(!pc_text_covering_face(b[0].data, b[0].len, 0x0631, &face));
    for (i = 0; i < 4; i++) {
        CHECK(!pc_text_covering_face((const uint8_t *)BAD[i], LEN[i], 'A', &face));
    }
}

/* lib.rs warm: try_lock doesn't block while the background build runs and gets there. */
static void test_shared_engine_becomes_ready_without_blocking(void)
{
    pc_text_engine *e = NULL;
    bool ready = false;
    time_t t0 = time(NULL);
    CHECK(pc_text_engine_shared_try_lock(&e, &ready, NULL) == PC_OK);
    while (!ready && time(NULL) - t0 < 60) {
        struct timespec ts = {0, 2000000};
        nanosleep(&ts, NULL);
        CHECK(pc_text_engine_shared_try_lock(&e, &ready, NULL) == PC_OK);
    }
    CHECK(ready);
    if (ready) {
        CHECK(e != NULL);
        pc_text_engine_shared_unlock();
    }
    pc_text_engine_warm_shared();
    CHECK(pc_text_engine_shared_try_lock(NULL, &ready, NULL) == PC_ERR_INVALID);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_registering_fonts_moves_the_generation);
    RUN(test_literal_psd_tabs_shape_as_whitespace_without_shifting_text_offsets);
    RUN(test_tabs_advance_to_the_next_default_tab_stop);
    RUN(test_tabs_wrap_inside_the_box);
    RUN(test_tabs_keep_point_text_alignment);
    RUN(test_faux_bold_scales_with_glyph_scaling);
    RUN(test_wrapped_and_forced_line_starts_use_the_following_line);
    RUN(test_line_navigation_reaches_adjacent_line_with_large_leading);
    RUN(test_southeast_asian_fallback_candidates_cover_every_platform);
    RUN(test_southeast_asian_scripts_fall_back_to_installed_fonts);
    RUN(test_box_lines_whose_descent_hangs_below_the_box_are_kept);
    RUN(test_missing_glyph_protection_can_turn_the_fallback_stack_off);
    RUN(test_numeric_subfamilies_with_identical_weights_select_distinct_faces);
    RUN(test_numeric_styles_shape_different_outlines_and_keep_psd_identity);
    RUN(test_explicit_weight_is_not_overridden_by_a_stale_style_name);
    RUN(test_collection_isolation_preserves_requested_face);
    RUN(test_localized_family_names_map_english_families_to_native_names);
    RUN(test_parses_unique_absolute_font_paths_from_fontconfig);
    RUN(test_fontconfig_candidates_must_cover_the_requested_script);
    RUN(test_fontconfig_matches_cover_the_requested_script);
    RUN(test_classifies_scripts_and_leaves_latin_and_cjk_alone);
    RUN(test_candidates_match_file_names_regular_first_across_layouts);
    RUN(test_covering_face_checks_the_character_map_and_rejects_garbage);
    RUN(test_shared_engine_becomes_ready_without_blocking);
    return pc_t_finish();
}
