/* Layout, rendering and engine tests (port of crates/text/src/tests.rs, layout.rs's inline test
 * and glyphs.rs preview_draws_ink). The PSD tests of tests.rs belong to chunk L15. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/file.h"
#include "base/num.h"
#include "base/utf8.h"
#include "harness.h"
#include "raster/surface.h"
#include "text/text.h"

/* ---- helpers ---- */

static pc_char_style style(float size)
{
    pc_char_style s;
    pc_char_style_default(&s);
    s.size_pt = size;
    return s;
}

/* A layer of `text` in one run of `s` (cloned). */
static void styled(pc_text_layer *t, const char *text, const pc_char_style *s)
{
    pc_text_layer_default(t);
    (void)pc_str_set_c(&t->text, text, NULL);
    t->runs = calloc(1, sizeof *t->runs);
    t->nruns = 1;
    t->runs[0].len = strlen(text);
    (void)pc_char_style_clone(s, &t->runs[0].style, NULL);
}

/* Point text in the summary fields only (no runs). */
static void point(pc_text_layer *t, const char *text, float size)
{
    pc_text_layer_default(t);
    (void)pc_str_set_c(&t->text, text, NULL);
    (void)pc_str_set_c(&t->font_family, "Inter", NULL);
    t->size_pt = size;
}

static void runs_of(pc_text_layer *t, const char *text, size_t n, const size_t *lens,
                    const pc_char_style *styles)
{
    size_t i;
    pc_text_layer_default(t);
    (void)pc_str_set_c(&t->text, text, NULL);
    t->runs = calloc(n, sizeof *t->runs);
    t->nruns = n;
    for (i = 0; i < n; i++) {
        t->runs[i].len = lens[i];
        (void)pc_char_style_clone(&styles[i], &t->runs[i].style, NULL);
    }
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

static pc_paragraph_style para_align(pc_text_align a)
{
    pc_paragraph_style p = pc_paragraph_style_default();
    p.align = a;
    return p;
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

static double alpha_sum(const pc_surface *s, pc_rect r)
{
    float *px = NULL;
    size_t n = 0, i, ch = pc_surface_channels(s);
    double sum = 0.0;
    if (r.x1 <= r.x0 || r.y1 <= r.y0) {
        return 0.0;
    }
    if (pc_surface_read_region(s, r, &px, &n, NULL) != PC_OK) {
        return 0.0;
    }
    for (i = ch - 1; i < n; i += ch) {
        sum += (double)px[i];
    }
    free(px);
    return sum;
}

static pc_rendered render(pc_text_engine *e, const pc_text_layer *t, float dpi, pc_pixel_format f,
                          pc_text_layout *l)
{
    pc_rendered r;
    pc_error err;
    if (pc_text_engine_render(e, t, dpi, f, l, &r, &err) != PC_OK) {
        PC_T_FAIL("render failed: %s", err.msg);
        memset(&r, 0, sizeof r);
        if (l != NULL) {
            pc_text_layout_init(l);
        }
    }
    return r;
}

static const pc_pixel_format RGBA8 = {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true};

static bool same_text(const pc_text_layer *t, size_t a, size_t z, const char *want)
{
    return z >= a && z <= t->text.len && z - a == strlen(want) && memcmp(t->text.ptr + a, want, z - a) == 0;
}

/* ---- tests ---- */

static void test_bundled_fonts_cover_latin(void)
{
    pc_text_engine *e = NULL;
    pc_strlist fams = {NULL, 0};
    pc_text_layer t;
    pc_text_layout l;
    pc_face_info *fi = NULL;
    size_t nfi = 0, i;
    bool inter = false, mono = false, nonzero = true;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    CHECK(pc_fontdb_families(pc_text_engine_fonts(e), &fams, NULL) == PC_OK);
    for (i = 0; i < fams.n; i++) {
        inter = inter || strcmp(fams.v[i], "Inter") == 0;
        mono = mono || strcmp(fams.v[i], "JetBrains Mono") == 0;
    }
    CHECK(inter && mono);
    point(&t, "Hello, Wörld! 0123", 12.0f);
    l = lay(e, &t, 72.0f);
    CHECK_EQ_UINT(l.nglyphs, 18);
    for (i = 0; i < l.nglyphs; i++) {
        nonzero = nonzero && l.glyphs[i].id != 0;
    }
    CHECK(nonzero);
    CHECK(pc_fontdb_faces(pc_text_engine_fonts(e), "Inter", 5, &fi, &nfi, NULL) == PC_OK);
    CHECK_EQ_UINT(nfi, 3);
    pc_face_infos_free(fi, nfi);
    pc_strlist_fini(&fams);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

static void test_metrics_are_stable_and_scale_with_dpi(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t;
    pc_text_layout a, again, b;
    float w;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    point(&t, "Hamburgefonstiv", 12.0f);
    a = lay(e, &t, 72.0f);
    w = width(&a);
    again = lay(e, &t, 72.0f);
    CHECK(width(&again) == w);
    CHECK(w >= 80.0f && w < 110.0f);
    b = lay(e, &t, 144.0f);
    CHECK(fabsf(width(&b) - 2.0f * w) < 0.05f);
    CHECK(a.nlines > 0 && a.lines[0].baseline == 0.0f);
    CHECK(a.nlines > 0 && a.lines[0].ascent > 8.0f && a.lines[0].ascent < 13.0f);
    pc_text_layout_fini(&a);
    pc_text_layout_fini(&again);
    pc_text_layout_fini(&b);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

static void test_small_caps_are_synthesized_without_smcp(void)
{
    pc_text_engine *e = NULL;
    pc_char_style s = style(40.0f), u = style(40.0f);
    pc_text_layer ts, tu;
    pc_text_layout small, up;
    (void)pc_str_set_c(&s.font_family, "Inter", NULL);
    (void)pc_str_set_c(&u.font_family, "Inter", NULL);
    s.caps = PC_CAPS_SMALL_CAPS;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    styled(&ts, "aA", &s);
    styled(&tu, "AA", &u);
    small = lay(e, &ts, 72.0f);
    up = lay(e, &tu, 72.0f);
    CHECK_EQ_UINT(small.nglyphs, 2);
    if (small.nglyphs == 2 && up.nglyphs == 2) {
        CHECK_EQ_UINT(small.glyphs[0].id, up.glyphs[0].id);
        CHECK_EQ_UINT(small.glyphs[1].id, up.glyphs[1].id);
        CHECK_NEAR(small.faces[small.glyphs[0].face].size_px, 28.0, 0.01);
        CHECK_NEAR(small.faces[small.glyphs[1].face].size_px, 40.0, 0.01);
    }
    pc_text_layout_fini(&small);
    pc_text_layout_fini(&up);
    pc_text_layer_fini(&ts);
    pc_text_layer_fini(&tu);
    pc_char_style_fini(&s);
    pc_char_style_fini(&u);
    pc_text_engine_free(e);
}

/* layout.rs uses_real_small_caps_only_when_the_selected_face_supports_smcp. */
static void test_real_small_caps_only_with_smcp(void)
{
    pc_char_style s = style(12.0f);
    pc_text_feature f[16];
    size_t n, i;
    bool has = false;
    s.caps = PC_CAPS_SMALL_CAPS;
    CHECK(pc_text_small_caps_mode(s.caps, true) == PC_SMALL_CAPS_OPENTYPE);
    n = pc_text_style_features(&s, PC_SMALL_CAPS_OPENTYPE, f, 16);
    for (i = 0; i < n; i++) {
        has = has || (f[i].tag == PC_SFNT_TAG('s', 'm', 'c', 'p') && f[i].value == 1);
    }
    CHECK(has);
    CHECK(pc_text_small_caps_mode(s.caps, false) == PC_SMALL_CAPS_SYNTHETIC);
    n = pc_text_style_features(&s, PC_SMALL_CAPS_SYNTHETIC, f, 16);
    for (i = 0, has = false; i < n; i++) {
        has = has || f[i].tag == PC_SFNT_TAG('s', 'm', 'c', 'p');
    }
    CHECK(!has);
}

static void test_auto_and_explicit_leading(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t;
    pc_text_layout l;
    pc_char_style s = style(10.0f);
    pc_paragraph_style p = pc_paragraph_style_default();
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    point(&t, "one\ntwo\rthree", 10.0f);
    l = lay(e, &t, 72.0f);
    CHECK_EQ_UINT(l.nlines, 3);
    if (l.nlines == 3) {
        CHECK(fabsf(l.lines[1].baseline - 12.0f) < 1e-3f);
        CHECK(fabsf(l.lines[2].baseline - 24.0f) < 1e-3f);
        CHECK(same_text(&t, l.lines[2].start, l.lines[2].end, "three"));
    }
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    s.has_leading = true;
    s.leading_pt = 30.0f;
    styled(&t, "a\nb", &s);
    l = lay(e, &t, 144.0f);
    CHECK(l.nlines == 2 && fabsf(l.lines[1].baseline - 60.0f) < 1e-3f);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    point(&t, "a\nb", 10.0f);
    p.space_after_pt = 5.0f;
    with_para(&t, p);
    l = lay(e, &t, 72.0f);
    CHECK(l.nlines == 2 && fabsf(l.lines[1].baseline - 17.0f) < 1e-3f);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

static void test_box_text_wraps_inside_width(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t;
    pc_text_layout l;
    size_t i;
    float drop;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    point(&t, "The quick brown fox jumps over the lazy dog again and again", 12.0f);
    set_box(&t, 10.0f, 20.0f, 100.0f, 1000.0f);
    l = lay(e, &t, 72.0f);
    CHECK(l.nlines >= 3);
    for (i = 0; i < l.nlines; i++) {
        CHECK(l.lines[i].x0 >= 10.0f - 1e-3f && l.lines[i].x1 <= 110.0f + 1e-3f);
    }
    if (l.nlines > 0) {
        drop = l.lines[0].baseline - 20.0f;
        CHECK(drop > 0.65f * 12.0f && drop < l.lines[0].ascent);
        CHECK_EQ_UINT(l.lines[0].start, 0);
    }
    for (i = 1; i < l.nlines; i++) {
        CHECK(l.lines[i].start >= l.lines[i - 1].end);
    }
    pc_text_layout_fini(&l);
    set_box(&t, 0.0f, 0.0f, 100.0f, 30.0f);
    l = lay(e, &t, 72.0f);
    CHECK_EQ_UINT(l.nlines, 2);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

static void test_alignment_point_and_box(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t;
    pc_text_layout l;
    size_t i;
    float last = -3.4e38f;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    point(&t, "Centered", 20.0f);
    with_para(&t, para_align(PC_TEXT_ALIGN_CENTER));
    l = lay(e, &t, 72.0f);
    CHECK(l.nlines == 1 && fabsf(l.lines[0].x0 + l.lines[0].x1) < 0.01f);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    point(&t, "Right", 20.0f);
    with_para(&t, para_align(PC_TEXT_ALIGN_RIGHT));
    l = lay(e, &t, 72.0f);
    CHECK(l.nlines == 1 && fabsf(l.lines[0].x1) < 0.01f && l.lines[0].x0 < -10.0f);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    point(&t, "aaa bbb ccc ddd eee fff ggg hhh iii jjj kkk", 12.0f);
    set_box(&t, 0.0f, 0.0f, 120.0f, 500.0f);
    with_para(&t, para_align(PC_TEXT_ALIGN_JUSTIFY_LEFT));
    l = lay(e, &t, 72.0f);
    CHECK(l.nlines > 1);
    for (i = 0; i < l.nglyphs && l.nlines > 0; i++) {
        if (fabsf(l.glyphs[i].y - l.lines[0].baseline) < 1e-3f && l.glyphs[i].x > last) {
            last = l.glyphs[i].x;
        }
    }
    CHECK(last > 110.0f);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    point(&t, "end", 12.0f);
    set_box(&t, 0.0f, 0.0f, 200.0f, 100.0f);
    with_para(&t, para_align(PC_TEXT_ALIGN_RIGHT));
    l = lay(e, &t, 72.0f);
    CHECK(l.nlines == 1 && fabsf(l.lines[0].x1 - 200.0f) < 0.5f);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

static bool all_hebrew(const pc_text_layer *t, const pc_cluster_info *c)
{
    size_t i = c->start;
    while (i < c->end) {
        uint32_t cp;
        if (!pc_utf8_next((const uint8_t *)t->text.ptr, c->end, &i, &cp) || cp < 0x590 || cp > 0x5FF) {
            return false;
        }
    }
    return c->end > c->start;
}

static void test_rtl_text_is_reordered(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t;
    pc_text_layout l;
    const pc_cluster_info *heb[16], *first = NULL, *a = NULL, *shin = NULL;
    size_t nh = 0, i;
    pc_paragraph_style p = pc_paragraph_style_default();
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    point(&t, "abc שלום def", 12.0f);
    l = lay(e, &t, 72.0f);
    for (i = 0; i < l.nclusters && nh < 16; i++) {
        if (all_hebrew(&t, &l.clusters[i])) {
            heb[nh++] = &l.clusters[i];
        }
    }
    CHECK_EQ_UINT(nh, 4);
    for (i = 0; i < nh; i++) {
        CHECK(heb[i]->rtl);
        if (first == NULL || heb[i]->start < first->start) {
            first = heb[i];
        }
    }
    for (i = 0; i < nh && first != NULL; i++) {
        CHECK(heb[i]->x <= first->x + 1e-3f);
    }
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    point(&t, "שלום abc", 12.0f);
    p.direction = PC_TEXT_DIR_RTL;
    with_para(&t, p);
    l = lay(e, &t, 72.0f);
    for (i = 0; i < l.nclusters; i++) {
        if (same_text(&t, l.clusters[i].start, l.clusters[i].end, "a")) {
            a = &l.clusters[i];
        }
        if (l.clusters[i].start == 0 && shin == NULL) {
            shin = &l.clusters[i];
        }
        CHECK(l.clusters[i].end <= t.text.len);
    }
    CHECK(a != NULL && shin != NULL && a->x < shin->x);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

/* A TrueType collection from standalone fonts (table offsets rebased). */
static pc_buf make_ttc(const pc_bundled_font *const *fonts, size_t n)
{
    pc_buf out = PC_BUF_INIT;
    size_t header = 12 + 4 * n, i, body_at = header, offs[4];
    uint8_t hb[12] = {'t', 't', 'c', 'f', 0, 1, 0, 0, 0, 0, 0, (uint8_t)n};
    (void)pc_buf_append(&out, hb, 12, NULL);
    for (i = 0; i < n; i++) {
        while (body_at % 4 != 0) {
            body_at++;
        }
        offs[i] = body_at;
        body_at += fonts[i]->len;
    }
    for (i = 0; i < n; i++) {
        uint8_t o[4] = {(uint8_t)(offs[i] >> 24), (uint8_t)(offs[i] >> 16), (uint8_t)(offs[i] >> 8),
                        (uint8_t)offs[i]};
        (void)pc_buf_append(&out, o, 4, NULL);
    }
    for (i = 0; i < n; i++) {
        uint8_t *copy = malloc(fonts[i]->len);
        size_t nt, k;
        while (out.len < offs[i]) {
            (void)pc_buf_append_byte(&out, 0, NULL);
        }
        memcpy(copy, fonts[i]->data, fonts[i]->len);
        nt = ((size_t)copy[4] << 8) | copy[5];
        for (k = 0; k < nt; k++) {
            size_t at = 12 + k * 16 + 8;
            uint32_t off = ((uint32_t)copy[at] << 24) | ((uint32_t)copy[at + 1] << 16) |
                           ((uint32_t)copy[at + 2] << 8) | copy[at + 3];
            off += (uint32_t)offs[i];
            copy[at] = (uint8_t)(off >> 24);
            copy[at + 1] = (uint8_t)(off >> 16);
            copy[at + 2] = (uint8_t)(off >> 8);
            copy[at + 3] = (uint8_t)off;
        }
        (void)pc_buf_append(&out, copy, fonts[i]->len, NULL);
        free(copy);
    }
    return out;
}

static void test_truetype_collections_load(void)
{
    size_t nb, i, before = 0, after = 0;
    const pc_bundled_font *b = pc_text_bundled_fonts(&nb);
    const pc_bundled_font *two[2];
    pc_buf ttc;
    pc_fontdb *db;
    pc_text_engine *e = NULL;
    pc_strlist names = {NULL, 0};
    pc_face_info *fi = NULL;
    pc_char_style s = style(12.0f);
    pc_text_layer t;
    pc_text_layout l;
    bool inter = false, mono = false, nonzero = true;
    CHECK(nb >= 4);
    if (nb < 4) {
        return;
    }
    two[0] = &b[0];
    two[1] = &b[3];
    ttc = make_ttc(two, 2);
    CHECK_EQ_UINT(pc_text_face_count(ttc.data, ttc.len), 2);
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    db = pc_text_engine_fonts(e);
    CHECK(pc_fontdb_faces(db, "JetBrains Mono", 14, &fi, &before, NULL) == PC_OK);
    pc_face_infos_free(fi, before);
    CHECK(pc_fontdb_register_font_data(db, ttc.data, ttc.len, &names, NULL) == PC_OK);
    for (i = 0; i < names.n; i++) {
        inter = inter || strcmp(names.v[i], "Inter") == 0;
        mono = mono || strcmp(names.v[i], "JetBrains Mono") == 0;
    }
    CHECK(inter && mono);
    CHECK(pc_fontdb_faces(db, "JetBrains Mono", 14, &fi, &after, NULL) == PC_OK);
    pc_face_infos_free(fi, after);
    CHECK_EQ_UINT(after, before + 1);
    (void)pc_str_set_c(&s.font_family, "JetBrains Mono", NULL);
    styled(&t, "mono", &s);
    l = lay(e, &t, 72.0f);
    for (i = 0; i < l.nglyphs; i++) {
        nonzero = nonzero && l.glyphs[i].id != 0;
    }
    CHECK(nonzero && l.nglyphs == 4);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    pc_char_style_fini(&s);
    pc_strlist_fini(&names);
    pc_buf_fini(&ttc);
    pc_text_engine_free(e);
}

static void ids_of(pc_text_engine *e, pc_font_feature *f, size_t nf, bool lig, uint32_t out[4], size_t *n)
{
    pc_char_style s = style(12.0f);
    pc_text_layer t;
    pc_text_layout l;
    size_t i;
    s.features = f;
    s.nfeatures = nf;
    s.ligatures = lig;
    styled(&t, "a0", &s);
    s.features = NULL;
    s.nfeatures = 0;
    l = lay(e, &t, 72.0f);
    *n = l.nglyphs < 4 ? l.nglyphs : 4;
    for (i = 0; i < *n; i++) {
        out[i] = l.glyphs[i].id;
    }
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
}

static void test_opentype_features_and_tracking(void)
{
    pc_text_engine *e = NULL;
    uint32_t plain[4], alt[4];
    size_t np, na;
    pc_font_feature zero;
    pc_char_style s0 = style(12.0f), s1 = style(12.0f);
    pc_text_layer t;
    pc_text_layout l;
    float a, b;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    ids_of(e, NULL, 0, true, plain, &np);
    memset(&zero, 0, sizeof zero);
    (void)pc_str_set_c(&zero.tag, "zero", NULL);
    zero.value = 1;
    ids_of(e, &zero, 1, true, alt, &na);
    CHECK(np == na && (np != 2 || memcmp(plain, alt, sizeof(uint32_t) * 2) != 0));
    pc_str_fini(&zero.tag);
    styled(&t, "tracking", &s0);
    l = lay(e, &t, 72.0f);
    a = width(&l);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    s1.tracking = 100.0f;
    styled(&t, "tracking", &s1);
    l = lay(e, &t, 72.0f);
    b = width(&l);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    CHECK(fabsf(b - a - 8.0f * 1.2f) < 1.3f);
    pc_text_engine_free(e);
}

#define GOLDEN_INK_12 44.4

static void test_raster_coverage_scales_and_depths_agree(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t12, t24;
    pc_rendered r12, r24, again;
    double s12, s24, ratio;
    pc_pixel_format fmts[4] = {{PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, true},
                               {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, true},
                               {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, true},
                               {PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, true}};
    size_t i;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    point(&t12, "Ink", 12.0f);
    t12.transform = pc_affine_translate(10.0, 50.0);
    point(&t24, "Ink", 24.0f);
    t24.transform = pc_affine_translate(10.0, 50.0);
    r12 = render(e, &t12, 72.0f, RGBA8, NULL);
    r24 = render(e, &t24, 72.0f, RGBA8, NULL);
    s12 = alpha_sum(&r12.surface, r12.rect);
    s24 = alpha_sum(&r24.surface, r24.rect);
    CHECK(s12 > 20.0);
    ratio = s24 / s12;
    CHECK(ratio > 3.6 && ratio < 4.4);
    CHECK(r12.rect.x0 >= 8 && r12.rect.x0 <= 11 && r12.rect.y1 <= 53 && r12.rect.y1 >= 50);
    for (i = 0; i < 4; i++) {
        pc_rendered r = render(e, &t12, 72.0f, fmts[i], NULL);
        double s = alpha_sum(&r.surface, r.rect);
        CHECK(r.rect.x0 == r12.rect.x0 && r.rect.y0 == r12.rect.y0 && r.rect.x1 == r12.rect.x1 &&
              r.rect.y1 == r12.rect.y1);
        CHECK(fabs(s - s12) / s12 < 0.01);
        pc_rendered_fini(&r);
    }
    again = render(e, &t12, 72.0f, RGBA8, NULL);
    CHECK(fabs(alpha_sum(&again.surface, again.rect) - s12) == 0.0);
    CHECK(fabs(s12 - GOLDEN_INK_12) / GOLDEN_INK_12 < 0.03);
    pc_rendered_fini(&again);
    pc_rendered_fini(&r12);
    pc_rendered_fini(&r24);
    pc_text_layer_fini(&t12);
    pc_text_layer_fini(&t24);
    pc_text_engine_free(e);
}

static double ink(pc_text_engine *e, const pc_char_style *s, pc_rect *rect)
{
    pc_text_layer t;
    pc_rendered r;
    double sum;
    styled(&t, "Hi", s);
    t.transform = pc_affine_translate(5.0, 40.0);
    r = render(e, &t, 72.0f, RGBA8, NULL);
    sum = alpha_sum(&r.surface, r.rect);
    *rect = r.rect;
    pc_rendered_fini(&r);
    pc_text_layer_fini(&t);
    return sum;
}

static void test_styles_change_pixels(void)
{
    pc_text_engine *e = NULL;
    pc_char_style base = style(30.0f), s;
    pc_rect pr, ur, ir, sr, hr, br;
    double plain, bold, under;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    plain = ink(e, &base, &pr);
    s = base;
    s.faux_bold = true;
    bold = ink(e, &s, &br);
    CHECK(bold > plain * 1.1);
    s = base;
    s.underline = true;
    under = ink(e, &s, &ur);
    CHECK(under > plain && ur.y1 > pr.y1);
    s = base;
    s.faux_italic = true;
    (void)ink(e, &s, &ir);
    CHECK(ir.x1 > pr.x1);
    s = base;
    s.baseline_shift_pt = 10.0f;
    (void)ink(e, &s, &sr);
    CHECK_EQ_INT(sr.y0, pr.y0 - 10);
    s = base;
    s.horizontal_scale = 2.0f;
    (void)ink(e, &s, &hr);
    CHECK((float)pc_rect_width(hr) > (float)pc_rect_width(pr) * 1.7f);
    pc_text_engine_free(e);
}

static void test_multicolor_runs(void)
{
    pc_text_engine *e = NULL;
    pc_char_style st[2] = {style(40.0f), style(40.0f)};
    size_t lens[2] = {1, 1}, n = 0, i;
    pc_text_layer t;
    pc_text_layout l;
    pc_rendered r, rc;
    float *px = NULL;
    bool red = false, blue = false, cmyk = false;
    pc_pixel_format cm = {PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, true};
    st[0].color = pc_color_rgb(1.0f, 0.0f, 0.0f);
    st[1].color = pc_color_rgb(0.0f, 0.0f, 1.0f);
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    runs_of(&t, "HH", 2, lens, st);
    t.transform = pc_affine_translate(0.0, 40.0);
    r = render(e, &t, 72.0f, RGBA8, &l);
    CHECK_EQ_UINT(l.nglyphs, 2);
    if (pc_surface_read_region(&r.surface, r.rect, &px, &n, NULL) == PC_OK) {
        for (i = 0; i + 3 < n; i += 4) {
            if (px[i + 3] > 0.99f) {
                red = red || (px[i] > 0.99f && px[i + 2] < 0.01f);
                blue = blue || (px[i + 2] > 0.99f && px[i] < 0.01f);
            }
        }
        free(px);
    }
    CHECK(red && blue);
    rc = render(e, &t, 72.0f, cm, NULL);
    px = NULL;
    if (pc_surface_read_region(&rc.surface, rc.rect, &px, &n, NULL) == PC_OK) {
        for (i = 0; i + 4 < n; i += 5) {
            cmyk = cmyk || (px[i + 4] > 0.99f && px[i + 1] > 0.9f && px[i + 2] > 0.9f);
        }
        free(px);
    }
    CHECK(cmyk);
    pc_rendered_fini(&r);
    pc_rendered_fini(&rc);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

static void test_hit_test_and_caret(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t;
    pc_text_layout l;
    float x, top, bottom, x2, a, b;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    point(&t, "abc\ndef", 20.0f);
    l = lay(e, &t, 72.0f);
    CHECK_EQ_UINT(pc_text_layout_hit_test(&l, -5.0f, -5.0f), 0);
    CHECK_EQ_UINT(pc_text_layout_hit_test(&l, 1000.0f, -5.0f), 3);
    CHECK_EQ_UINT(pc_text_layout_hit_test(&l, -5.0f, 24.0f), 4);
    pc_text_layout_caret(&l, 1, &x, &top, &bottom);
    CHECK(x > 0.0f && top < 0.0f && bottom > 0.0f);
    pc_text_layout_caret(&l, 2, &x2, &a, &b);
    CHECK(x2 > x);
    pc_text_layout_caret(&l, 3, &x, &a, &b);
    CHECK(l.nlines > 0 && x == l.lines[0].x1);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

static void test_empty_text_and_empty_lines(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t;
    pc_text_layout l;
    pc_rendered r;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    point(&t, "", 12.0f);
    l = lay(e, &t, 72.0f);
    CHECK_EQ_UINT(l.nlines, 1);
    CHECK_EQ_UINT(l.nglyphs, 0);
    pc_text_layout_fini(&l);
    r = render(e, &t, 72.0f, RGBA8, NULL);
    CHECK_EQ_UINT(pc_rect_width(r.rect), 0);
    pc_rendered_fini(&r);
    pc_text_layer_fini(&t);
    point(&t, "a\n\nb", 12.0f);
    l = lay(e, &t, 72.0f);
    CHECK_EQ_UINT(l.nlines, 3);
    CHECK(l.nlines == 3 && fabsf(l.lines[2].baseline - 2.0f * 14.4f) < 1e-3f);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

static void test_forced_line_break_starts_a_line_in_the_same_paragraph(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t;
    pc_text_layout l;
    pc_paragraph_style sp = pc_paragraph_style_default();
    const char *text = "one\x03two";
    size_t i;
    bool nonzero = true;
    pc_pixel_format fmts[3] = {
        RGBA8, {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, true}, {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, true}};
    sp.space_before_pt = 5.0f;
    sp.space_after_pt = 5.0f;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    point(&t, text, 10.0f);
    with_para(&t, sp);
    l = lay(e, &t, 72.0f);
    CHECK_EQ_UINT(l.nlines, 2);
    if (l.nlines == 2) {
        size_t z = l.lines[0].end;
        while (z > l.lines[0].start && text[z - 1] == 3) {
            z--;
        }
        CHECK(same_text(&t, l.lines[0].start, z, "one"));
        CHECK(same_text(&t, l.lines[1].start, l.lines[1].end, "two"));
        CHECK(l.lines[0].paragraph == 0 && l.lines[1].paragraph == 0);
        CHECK(fabsf(l.lines[1].baseline - 12.0f) < 1e-3f);
    }
    for (i = 0; i < l.nglyphs; i++) {
        nonzero = nonzero && l.glyphs[i].id != 0;
    }
    CHECK(nonzero);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    point(&t, "one\ntwo", 10.0f);
    with_para(&t, sp);
    l = lay(e, &t, 72.0f);
    CHECK(l.nlines == 2 && fabsf(l.lines[1].baseline - 22.0f) < 1e-3f);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    point(&t, "a\x03wide line", 20.0f);
    with_para(&t, para_align(PC_TEXT_ALIGN_CENTER));
    l = lay(e, &t, 72.0f);
    CHECK_EQ_UINT(l.nlines, 2);
    for (i = 0; i < l.nlines; i++) {
        CHECK(fabsf(l.lines[i].x0 + l.lines[i].x1) < 0.01f);
    }
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    point(&t, "short\x03next", 12.0f);
    set_box(&t, 0.0f, 0.0f, 500.0f, 500.0f);
    l = lay(e, &t, 72.0f);
    CHECK_EQ_UINT(l.nlines, 2);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    for (i = 0; i < 3; i++) {
        pc_rendered r;
        pc_rect row0, row1, after;
        point(&t, text, 20.0f);
        t.transform = pc_affine_translate(0.0, 40.0);
        r = render(e, &t, 72.0f, fmts[i], &l);
        if (l.nlines == 2) {
            row0 = pc_rect_make(-2, pc_f32_to_i32(l.lines[0].baseline - l.lines[0].ascent + 40.0f), 200,
                                pc_f32_to_i32(l.lines[0].baseline + 40.0f));
            row1 = pc_rect_make(-2, pc_f32_to_i32(l.lines[1].baseline - l.lines[1].ascent + 40.0f), 200,
                                pc_f32_to_i32(l.lines[1].baseline + 40.0f));
            CHECK(alpha_sum(&r.surface, row0) > 1.0 && alpha_sum(&r.surface, row1) > 1.0);
            after = pc_rect_make(pc_f32_to_i32(ceilf(l.lines[0].x1)) + 2, row0.y0, 200, row0.y1);
            CHECK(alpha_sum(&r.surface, after) < 1e-3);
        } else {
            CHECK(l.nlines == 2);
        }
        pc_rendered_fini(&r);
        pc_text_layout_fini(&l);
        pc_text_layer_fini(&t);
    }
    pc_text_engine_free(e);
}

static void test_postscript_names_fall_back_in_layout(void)
{
    pc_text_engine *e = NULL;
    pc_char_style s = style(12.0f);
    pc_text_layer t;
    pc_text_layout l;
    pc_resolved_font r;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    CHECK(pc_fontdb_resolve_postscript(pc_text_engine_fonts(e), "Inter-SemiBold", 14, &r, NULL) == PC_OK);
    CHECK(strcmp(r.family, "Inter") == 0 && r.weight == 600 && r.exact);
    pc_resolved_font_fini(&r);
    (void)pc_str_set_c(&s.font_family, "Nonexistent Sans", NULL);
    s.has_postscript_name = true;
    (void)pc_str_set_c(&s.postscript_name, "Nope-Bold", NULL);
    styled(&t, "x", &s);
    l = lay(e, &t, 72.0f);
    CHECK(l.nglyphs == 1 && l.glyphs[0].id != 0);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    pc_char_style_fini(&s);
    pc_text_engine_free(e);
}

static void test_depth_is_respected(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t;
    pc_rendered r;
    pc_pixel_format f;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    point(&t, "a", 20.0f);
    t.transform = pc_affine_translate(2.0, 20.0);
    r = render(e, &t, 72.0f, (pc_pixel_format){PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, true}, NULL);
    f = pc_surface_format(&r.surface);
    CHECK(f.sample == PC_SAMPLE_TYPE_U16 && f.alpha);
    pc_rendered_fini(&r);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

/* Variable-font axes reach the outlines (a system variable font, when one is present). */
static void test_variable_font_axes(void)
{
    static const char *const candidates[] = {
        "/System/Library/Fonts/SFNS.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-VariableFont_wdth,wght.ttf", NULL};
    pc_buf b = PC_BUF_INIT;
    pc_text_engine *e = NULL;
    pc_strlist names = {NULL, 0};
    size_t i;
    double light = 0.0, heavy = 0.0;
    for (i = 0; candidates[i] != NULL; i++) {
        if (pc_file_read(candidates[i], (size_t)64 << 20, &b, NULL) == PC_OK) {
            break;
        }
    }
    if (candidates[i] == NULL) {
        return;
    }
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    if (pc_fontdb_register_font_data(pc_text_engine_fonts(e), b.data, b.len, &names, NULL) == PC_OK &&
        names.n > 0) {
        int k;
        for (k = 0; k < 2; k++) {
            pc_char_style s = style(40.0f);
            pc_font_variation v;
            pc_text_layer t;
            pc_rendered r;
            (void)pc_str_set_c(&s.font_family, names.v[0], NULL);
            memset(&v, 0, sizeof v);
            (void)pc_str_set_c(&v.axis, "wght", NULL);
            v.value = k == 0 ? 200.0f : 800.0f;
            s.variations = &v;
            s.nvariations = 1;
            styled(&t, "Weight", &s);
            s.variations = NULL;
            s.nvariations = 0;
            t.transform = pc_affine_translate(0.0, 50.0);
            r = render(e, &t, 72.0f, RGBA8, NULL);
            *(k == 0 ? &light : &heavy) = alpha_sum(&r.surface, r.rect);
            pc_rendered_fini(&r);
            pc_text_layer_fini(&t);
            pc_str_fini(&v.axis);
            pc_char_style_fini(&s);
        }
        CHECK(heavy > light * 1.5);
    }
    pc_strlist_fini(&names);
    pc_buf_fini(&b);
    pc_text_engine_free(e);
}

static double low_y(const pc_glyph_outline *o)
{
    double m = -1.0e308;
    size_t i;
    for (i = 0; i < o->n; i++) {
        if ((o->els[i].kind == PC_PATH_MOVE || o->els[i].kind == PC_PATH_LINE) && o->els[i].p[0][1] > m) {
            m = o->els[i].p[0][1];
        }
    }
    return m;
}

static void test_warp_bends_rendered_text_and_outlines(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t;
    pc_rendered flat, arced;
    pc_text_layout l;
    pc_style_warp w;
    bool hw;
    pc_glyph_outline *outs = NULL, *fouts = NULL;
    size_t no = 0, nf = 0, i, j, drawn = 0;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    point(&t, "WARPED TEXT", 24.0f);
    t.transform = pc_affine_translate(20.0, 60.0);
    flat = render(e, &t, 72.0f, RGBA8, NULL);
    t.has_warp = true;
    (void)pc_str_set_c(&t.warp.style, "warpArc", NULL);
    t.warp.value = 60.0f;
    t.warp.horizontal = true;
    arced = render(e, &t, 72.0f, RGBA8, &l);
    CHECK(pc_rect_height(arced.rect) > pc_rect_height(flat.rect) + 10);
    CHECK(alpha_sum(&arced.surface, arced.rect) > 0.0);
    hw = pc_text_layout_warp(&l, &t.warp, &w);
    CHECK(hw);
    CHECK(pc_text_outlines(&l, &t.transform, hw ? &w : NULL, &outs, &no, NULL) == PC_OK);
    for (i = 0; i < l.nglyphs; i++) {
        drawn += l.glyphs[i].id != 0 ? 1 : 0;
    }
    CHECK_EQ_UINT(no, drawn - 1);
    CHECK(no > 2 && low_y(&outs[0]) > low_y(&outs[no / 2]) + 5.0);
    CHECK(pc_text_outlines(&l, &t.transform, NULL, &fouts, &nf, NULL) == PC_OK);
    for (i = 0; i < nf; i++) {
        for (j = 0; j < fouts[i].n; j++) {
            const pc_path_el *el = &fouts[i].els[j];
            if (el->kind == PC_PATH_MOVE || el->kind == PC_PATH_LINE) {
                CHECK(el->p[0][0] >= flat.rect.x0 && el->p[0][0] <= flat.rect.x1 &&
                      el->p[0][1] >= flat.rect.y0 && el->p[0][1] <= flat.rect.y1);
            }
        }
    }
    pc_glyph_outlines_free(outs, no);
    pc_glyph_outlines_free(fouts, nf);
    pc_text_layout_fini(&l);
    pc_rendered_fini(&flat);
    pc_rendered_fini(&arced);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

/* #123: carets and hit tests sit on the rendered glyphs. */
static void check_clusters_on_ink(pc_text_engine *e, pc_text_layer *t, const char *name, size_t min_lines)
{
    pc_text_layout l;
    pc_rendered r;
    size_t i;
    t->transform = pc_affine_translate(7.0, 60.0);
    r = render(e, t, 72.0f, RGBA8, &l);
    CHECK(l.nlines >= min_lines);
    for (i = 0; i < l.nclusters; i++) {
        const pc_cluster_info *c = &l.clusters[i];
        const pc_line_info *ln;
        size_t k;
        bool blank = true;
        float y, cx, top, bottom;
        pc_rect col;
        for (k = c->start; k < c->end; k++) {
            blank = blank && t->text.ptr[k] == ' ';
        }
        if (blank || c->line >= l.nlines) {
            continue;
        }
        ln = &l.lines[c->line];
        col = pc_rect_make(pc_f32_to_i32(floorf(c->x + c->advance * 0.3f + 7.0f)),
                           pc_f32_to_i32(floorf(ln->baseline - ln->ascent + 60.0f)),
                           pc_f32_to_i32(ceilf(c->x + c->advance * 0.55f + 7.0f)),
                           pc_f32_to_i32(ceilf(ln->baseline + ln->descent + 60.0f)));
        if (!(alpha_sum(&r.surface, col) > 1.0)) {
            PC_T_FAIL("%s: no ink under cluster %zu..%zu", name, c->start, c->end);
        }
        y = ln->baseline - ln->ascent * 0.4f;
        CHECK_EQ_UINT(pc_text_layout_hit_test(&l, c->x + c->advance * 0.2f, y), c->start);
        CHECK_EQ_UINT(pc_text_layout_hit_test(&l, c->x + c->advance * 0.8f, y), c->end);
        pc_text_layout_caret(&l, c->start, &cx, &top, &bottom);
        CHECK(fabsf(cx - c->x) < 1e-3f && top < y && bottom > y);
    }
    pc_text_layout_fini(&l);
    pc_rendered_fini(&r);
}

static void test_clusters_sit_on_rendered_glyphs(void)
{
    pc_text_engine *e = NULL;
    pc_char_style big = style(40.0f), small = style(18.0f), s;
    pc_char_style mix[2];
    size_t lens[2] = {3, 3};
    pc_text_layer t;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    styled(&t, "HOHOH", &big);
    check_clusters_on_ink(e, &t, "plain", 1);
    pc_text_layer_fini(&t);
    s = big;
    s.tracking = 300.0f;
    styled(&t, "HOHOH", &s);
    check_clusters_on_ink(e, &t, "tracking", 1);
    pc_text_layer_fini(&t);
    s = big;
    s.horizontal_scale = 1.6f;
    styled(&t, "HOHOH", &s);
    check_clusters_on_ink(e, &t, "hscale", 1);
    pc_text_layer_fini(&t);
    s = big;
    s.kern = 250.0f;
    s.kerning = PC_KERNING_OPTICAL;
    styled(&t, "HOHOH", &s);
    check_clusters_on_ink(e, &t, "kerned", 1);
    pc_text_layer_fini(&t);
    mix[0] = big;
    mix[1] = small;
    runs_of(&t, "HOHOHO", 2, lens, mix);
    check_clusters_on_ink(e, &t, "mixed", 1);
    pc_text_layer_fini(&t);
    styled(&t, "HOH HOH HOH HOH", &small);
    set_box(&t, 0.0f, 0.0f, 150.0f, 200.0f);
    with_para(&t, para_align(PC_TEXT_ALIGN_CENTER));
    check_clusters_on_ink(e, &t, "box", 2);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

static float gx(const pc_text_layout *l, size_t i)
{
    return i < l->nglyphs ? l->glyphs[i].x : NAN;
}

static void test_manual_kerning_moves_the_next_glyph(void)
{
    pc_text_engine *e = NULL;
    pc_char_style s = style(100.0f), k[2];
    size_t lens[2] = {1, 2}, lens2[2] = {2, 1};
    pc_text_layer t;
    pc_text_layout plain, kerned, tight, last, l;
    float a, b, metric, off, x, top, bot, x2;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    styled(&t, "HOH", &s);
    plain = lay(e, &t, 72.0f);
    pc_text_layer_fini(&t);
    k[0] = s;
    k[0].kern = 100.0f;
    k[1] = s;
    runs_of(&t, "HOH", 2, lens, k);
    kerned = lay(e, &t, 72.0f);
    pc_text_layer_fini(&t);
    CHECK(gx(&kerned, 0) == gx(&plain, 0));
    CHECK(fabsf(gx(&kerned, 1) - gx(&plain, 1) - 10.0f) < 1e-3f);
    CHECK(fabsf(gx(&kerned, 2) - gx(&plain, 2) - 10.0f) < 1e-3f);
    CHECK(fabsf(width(&kerned) - width(&plain) - 10.0f) < 1e-3f);
    pc_text_layout_caret(&kerned, 1, &x, &top, &bot);
    pc_text_layout_caret(&plain, 1, &x2, &top, &bot);
    CHECK(fabsf(x - x2 - 10.0f) < 1e-3f);
    k[0].kern = -50.0f;
    runs_of(&t, "HOH", 2, lens, k);
    tight = lay(e, &t, 72.0f);
    pc_text_layer_fini(&t);
    CHECK(fabsf(gx(&tight, 1) - gx(&plain, 1) + 5.0f) < 1e-3f);
    k[0] = s;
    k[1] = s;
    k[1].kern = 500.0f;
    runs_of(&t, "HOH", 2, lens2, k);
    last = lay(e, &t, 72.0f);
    pc_text_layer_fini(&t);
    CHECK(fabsf(width(&last) - width(&plain)) < 1e-3f);
    styled(&t, "AV", &s);
    l = lay(e, &t, 72.0f);
    metric = gx(&l, 1);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    k[0] = s;
    k[0].kerning = PC_KERNING_OFF;
    styled(&t, "AV", &k[0]);
    l = lay(e, &t, 72.0f);
    off = gx(&l, 1);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    CHECK(off > metric + 1.0f);
    k[0] = s;
    k[0].kern = 300.0f;
    k[1] = s;
    runs_of(&t, "HOH", 2, lens, k);
    with_para(&t, para_align(PC_TEXT_ALIGN_CENTER));
    l = lay(e, &t, 72.0f);
    CHECK(l.nlines == 1 && fabsf(l.lines[0].x0 + l.lines[0].x1) < 0.5f);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    a = 0.0f;
    b = 0.0f;
    (void)a;
    (void)b;
    pc_text_layout_fini(&plain);
    pc_text_layout_fini(&kerned);
    pc_text_layout_fini(&tight);
    pc_text_layout_fini(&last);
    pc_text_engine_free(e);
}

static float gap_of(pc_text_engine *e, const char *text, pc_kerning k, float kern)
{
    pc_char_style s = style(100.0f);
    pc_text_layer t;
    pc_text_layout l;
    float g;
    s.kerning = k;
    s.kern = kern;
    styled(&t, text, &s);
    l = lay(e, &t, 72.0f);
    g = l.nglyphs >= 2 ? l.glyphs[1].x - l.glyphs[0].x : NAN;
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    return g;
}

static void test_optical_kerning_tightens_open_pairs(void)
{
    pc_text_engine *e = NULL;
    static const char *const open[] = {"AV", "To", "LT", "Ty"};
    static const char *const neutral[] = {"HH", "nn", "oo", "HO"};
    size_t i;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    for (i = 0; i < 4; i++) {
        float off = gap_of(e, open[i], PC_KERNING_OFF, 0.0f),
              opt = gap_of(e, open[i], PC_KERNING_OPTICAL, 0.0f);
        if (!(opt < off - 3.0f)) {
            PC_T_FAIL("%s: optical %g vs unkerned %g", open[i], (double)opt, (double)off);
        }
    }
    for (i = 0; i < 4; i++) {
        float off = gap_of(e, neutral[i], PC_KERNING_OFF, 0.0f),
              opt = gap_of(e, neutral[i], PC_KERNING_OPTICAL, 0.0f);
        CHECK(fabsf(opt - off) < 6.0f);
    }
    CHECK(gap_of(e, "A V", PC_KERNING_OPTICAL, 0.0f) == gap_of(e, "A V", PC_KERNING_OFF, 0.0f));
    {
        float off = gap_of(e, "AV", PC_KERNING_OFF, 0.0f);
        CHECK(fabsf(gap_of(e, "AV", PC_KERNING_OPTICAL, 100.0f) - off - 10.0f) < 1e-3f);
        CHECK(fabsf(gap_of(e, "AV", PC_KERNING_METRICS, 100.0f) - off - 10.0f) < 1e-3f);
    }
    pc_text_engine_free(e);
}

static void test_kerning_modes_split_pairs(void)
{
    pc_text_engine *e = NULL;
    pc_char_style s = style(100.0f), off = style(100.0f), st[3];
    size_t lens[3] = {2, 1, 1};
    pc_text_layer t;
    pc_text_layout metric, plain, mixed;
    off.kerning = PC_KERNING_OFF;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    styled(&t, "AVAV", &s);
    metric = lay(e, &t, 72.0f);
    pc_text_layer_fini(&t);
    styled(&t, "AVAV", &off);
    plain = lay(e, &t, 72.0f);
    pc_text_layer_fini(&t);
    st[0] = s;
    st[1] = off;
    st[2] = s;
    runs_of(&t, "AVAV", 3, lens, st);
    mixed = lay(e, &t, 72.0f);
    pc_text_layer_fini(&t);
    CHECK(mixed.nglyphs == 4 && metric.nglyphs == 4 && plain.nglyphs == 4);
    if (mixed.nglyphs == 4 && metric.nglyphs == 4 && plain.nglyphs == 4) {
        CHECK(fabsf((gx(&mixed, 1) - gx(&mixed, 0)) - (gx(&metric, 1) - gx(&metric, 0))) < 1e-3f);
        CHECK(fabsf((gx(&mixed, 2) - gx(&mixed, 1)) - (gx(&plain, 2) - gx(&plain, 1))) < 1e-3f);
        CHECK(fabsf((gx(&mixed, 3) - gx(&mixed, 2)) - (gx(&plain, 3) - gx(&plain, 2))) < 1e-3f);
    }
    pc_text_layout_fini(&metric);
    pc_text_layout_fini(&plain);
    pc_text_layout_fini(&mixed);
    pc_text_engine_free(e);
}

static void test_japanese_box_text_preserves_wrap_boundaries(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t;
    pc_text_layout l;
    size_t i, end = 0;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    point(&t, "日本語の文章を折り返します。", 12.0f);
    set_box(&t, 0.0f, 0.0f, 40.0f, 1000.0f);
    l = lay(e, &t, 72.0f);
    CHECK(l.nlines > 1);
    for (i = 0; i < l.nlines; i++) {
        CHECK_EQ_UINT(l.lines[i].start, end);
        CHECK(
            !(l.lines[i].end - l.lines[i].start >= 3 && memcmp(t.text.ptr + l.lines[i].start, "。", 3) == 0));
        end = l.lines[i].end;
    }
    CHECK_EQ_UINT(end, t.text.len);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

/* Which craft-fonts family drew the glyphs (matched by font bytes). */
static size_t craft_families_of(const pc_text_layout *l, const char **out, size_t cap)
{
    size_t ncf, n = 0, i, j, k;
    const pc_craft_font *cf = pc_craft_fonts(&ncf);
    for (i = 0; i < l->nglyphs; i++) {
        size_t len;
        const uint8_t *d;
        if (l->glyphs[i].face >= l->nfaces) {
            continue;
        }
        d = pc_font_data(l->faces[l->glyphs[i].face].font, &len);
        for (j = 0; j < ncf; j++) {
            size_t bl = pc_blob_len(cf[j].bytes);
            const uint8_t *bd = pc_blob_data(cf[j].bytes);
            if (bd == d && bl == len) {
                bool seen = false;
                for (k = 0; k < n; k++) {
                    seen = seen || strcmp(out[k], cf[j].family) == 0;
                }
                if (!seen && n < cap) {
                    out[n++] = cf[j].family;
                }
            }
        }
    }
    return n;
}

static void test_craft_fonts_cover_japanese_without_system_fonts(void)
{
    size_t ncf, i, n;
    const pc_craft_font *cf = pc_craft_fonts(&ncf);
    bool jp = false, mincho = false, nonzero = true;
    pc_text_engine *e = NULL;
    pc_text_layer t;
    pc_text_layout l;
    const char *fams[8];
    for (i = 0; i < ncf; i++) {
        jp = jp || pc_craft_font_is_japanese(&cf[i]);
        mincho = mincho || pc_craft_font_is_mincho(&cf[i]);
    }
    if (!jp) {
        (void)fprintf(stderr, "skipping: no craft fonts (set CRAFT_FONTS_DIR to a craft-fonts checkout)\n");
        return;
    }
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    point(&t, "日本語の文字、カタカナ。", 24.0f);
    l = lay(e, &t, 72.0f);
    CHECK_EQ_UINT(l.nglyphs, 12);
    for (i = 0; i < l.nglyphs; i++) {
        nonzero = nonzero && l.glyphs[i].id != 0;
    }
    CHECK(nonzero);
    n = craft_families_of(&l, fams, 8);
    CHECK(n == 1 && strcmp(fams[0], PC_CRAFT_UI_JAPANESE_FAMILY) == 0);
    pc_text_layout_fini(&l);
    if (mincho) {
        (void)pc_str_set_c(&t.font_family, "Times New Roman", NULL);
        l = lay(e, &t, 72.0f);
        n = craft_families_of(&l, fams, 8);
        CHECK(n == 1 && strstr(fams[0], "Mincho") != NULL);
        pc_text_layout_fini(&l);
    }
    pc_text_layer_fini(&t);
    point(&t, "Layer 1", 24.0f);
    l = lay(e, &t, 72.0f);
    CHECK_EQ_UINT(craft_families_of(&l, fams, 8), 0);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

static void test_works_without_craft_fonts(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t;
    pc_text_layout l;
    size_t ncf, i;
    const pc_craft_font *cf = pc_craft_fonts(&ncf);
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    CHECK(pc_fontdb_has_family(pc_text_engine_fonts(e), "Inter", 5));
    point(&t, "日本語 Latin", 12.0f);
    l = lay(e, &t, 72.0f);
    CHECK_EQ_UINT(l.nglyphs, 9);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    for (i = 0; i < ncf; i++) {
        CHECK(pc_fontdb_has_family(pc_text_engine_fonts(e), cf[i].family, strlen(cf[i].family)));
    }
    if (ncf == 0) {
        const char **fam = NULL, **fb = NULL;
        size_t nf = 0, nfb = 0;
        pc_cjk_script order[4];
        CHECK(pc_craft_japanese_families(cf, ncf, &fam, &nf, NULL) == PC_OK && nf == 0);
        free(fam);
        pc_cjk_script_order("ja", order);
        CHECK(pc_text_fallback_candidates(order, &fb, &nfb, NULL) == PC_OK);
        for (i = 0; i < nfb; i++) {
            CHECK(strstr(fb[i], "BIZ UD") == NULL);
        }
        free(fb);
    }
    pc_text_engine_free(e);
}

static void test_word_and_line_navigation(void)
{
    pc_text_engine *e = NULL;
    const char *text = "AäB\ncd";
    size_t len = strlen(text), n = 6, i;
    int vertical;
    CHECK_EQ_UINT(pc_text_word_boundary("hello big world", 15, 0, true), 5);
    CHECK_EQ_UINT(pc_text_word_boundary("hello big world", 15, 7, false), 6);
    CHECK_EQ_UINT(pc_text_word_boundary("hello big world", 15, 15, false), 10);
    CHECK_EQ_UINT(pc_text_word_boundary("hello", 5, 0, false), 0);
    CHECK_EQ_UINT(pc_text_word_boundary("hello", 5, 5, true), 5);
    CHECK_EQ_UINT(pc_text_word_boundary("", 0, 4, true), 0);
    CHECK_EQ_UINT(pc_text_word_boundary("ab", 2, 100, true), 2);
    CHECK_EQ_UINT(pc_text_word_boundary("ab, cd", 6, 0, true), 2);
    CHECK_EQ_UINT(pc_text_word_boundary("ab, cd", 6, 2, true), 6);
    CHECK_EQ_UINT(pc_text_word_boundary("Größe", strlen("Größe"), 0, true), 5);
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    for (vertical = 0; vertical < 2; vertical++) {
        pc_text_layer t;
        pc_text_layout l;
        size_t end0, start1, next;
        float x, top, bot;
        point(&t, text, 20.0f);
        if (vertical) {
            t.orientation = PC_ORIENT_VERTICAL;
        }
        l = lay(e, &t, 72.0f);
        for (i = 0; i <= n; i++) {
            size_t b = pc_text_byte_index(text, len, i), idx, line;
            float seg[4];
            pc_text_layout_caret_segment(&l, b, seg);
            pc_text_hit_char(&l, text, len, (seg[0] + seg[2]) / 2.0f, (seg[1] + seg[3]) / 2.0f, &idx, &line);
            CHECK_EQ_UINT(idx, i);
            CHECK_EQ_UINT(line, pc_text_line_index(&l, b));
        }
        CHECK(l.nlines >= 2);
        if (l.nlines >= 2) {
            end0 = pc_text_char_index(text, len, l.lines[0].end);
            start1 = pc_text_char_index(text, len, l.lines[1].start);
            CHECK_EQ_UINT(pc_text_line_edge(&l, text, len, 1, true), end0);
            CHECK_EQ_UINT(pc_text_line_edge(&l, text, len, 1, false), 0);
            CHECK_EQ_UINT(pc_text_line_edge(&l, text, len, start1, false), start1);
            CHECK_EQ_UINT(pc_text_line_edge(&l, text, len, start1, true), n);
            pc_text_layout_caret(&l, pc_text_byte_index(text, len, 0), &x, &top, &bot);
            next = pc_text_line_step(&l, text, len, 0, x, 1);
            CHECK(next >= start1 && next <= n);
            CHECK_EQ_UINT(pc_text_line_step(&l, text, len, 0, x, -1), 0);
            CHECK_EQ_UINT(pc_text_line_step(&l, text, len, n, x, 1), n);
        }
        pc_text_layout_fini(&l);
        pc_text_layer_fini(&t);
    }
    {
        const char *t2 = "WWWWWW\nI";
        size_t l2 = strlen(t2), end0, kept, jumped;
        pc_text_layer t;
        pc_text_layout l;
        float x0, x1, a, b;
        point(&t, t2, 30.0f);
        l = lay(e, &t, 72.0f);
        end0 = pc_text_line_edge(&l, t2, l2, 0, true);
        pc_text_layout_caret(&l, 0, &x0, &a, &b);
        pc_text_layout_caret(&l, pc_text_byte_index(t2, l2, end0), &x1, &a, &b);
        kept = pc_text_line_step(&l, t2, l2, end0, x0, 1);
        jumped = pc_text_line_step(&l, t2, l2, end0, x1, 1);
        CHECK(l.nlines == 2);
        if (l.nlines == 2) {
            CHECK_EQ_UINT(kept, pc_text_char_index(t2, l2, l.lines[1].start));
            CHECK_EQ_UINT(jumped, pc_text_char_index(t2, l2, l.lines[1].end));
        }
        CHECK(jumped > kept);
        pc_text_layout_fini(&l);
        pc_text_layer_fini(&t);
    }
    pc_text_engine_free(e);
}

/* glyphs.rs preview_draws_ink, through the engine. */
static void test_preview_draws_ink(void)
{
    pc_text_engine *e = NULL;
    pc_char_style s = style(12.0f);
    uint32_t n = 0, inkv = 0;
    uint8_t *a = NULL;
    size_t i;
    bool blank = true;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    (void)pc_str_set_c(&s.font_family, PC_TEXT_DEFAULT_FAMILY, NULL);
    CHECK(pc_text_glyph_preview(e, &s, 'W', 32, &n, &a, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 32);
    for (i = 0; a != NULL && i < 32 * 32; i++) {
        inkv += a[i];
    }
    CHECK(inkv > 20 * 255);
    free(a);
    a = NULL;
    CHECK(pc_text_glyph_preview(e, &s, ' ', 32, &n, &a, NULL) == PC_OK);
    for (i = 0; a != NULL && i < 32 * 32; i++) {
        blank = blank && a[i] == 0;
    }
    CHECK(blank);
    free(a);
    pc_char_style_fini(&s);
    pc_text_engine_free(e);
}

static void test_render_layer_sets_the_cache(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t;
    pc_rect r;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    point(&t, "Cache", 20.0f);
    t.transform = pc_affine_translate(3.0, 30.0);
    CHECK(pc_text_engine_render_layer(e, &t, 72.0f, RGBA8, &r, NULL) == PC_OK);
    CHECK(t.has_cache && pc_rect_width(r) > 0);
    CHECK(alpha_sum(&t.cache, r) > 10.0);
    /* Again: the old cache is replaced, not leaked. */
    CHECK(pc_text_engine_render_layer(e, &t, 72.0f, RGBA8, &r, NULL) == PC_OK);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

static void test_pair_kerning(void)
{
    pc_text_engine *e = NULL;
    pc_char_style s = style(50.0f), k[2];
    size_t lens[2] = {1, 2};
    pc_text_layer t;
    bool has = false;
    float v = 0.0f;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    k[0] = s;
    k[0].kern = 120.0f;
    k[1] = s;
    runs_of(&t, "HOH", 2, lens, k);
    CHECK(pc_text_engine_pair_kerning(e, &t, 72.0f, 0, &has, &v, NULL) == PC_OK);
    CHECK(has && fabsf(v - 120.0f) < 0.01f);
    CHECK(pc_text_engine_pair_kerning(e, &t, 72.0f, 2, &has, &v, NULL) == PC_OK);
    CHECK(!has);
    CHECK(pc_text_engine_pair_kerning(e, &t, 72.0f, 99, &has, &v, NULL) == PC_OK);
    CHECK(!has);
    pc_text_layer_fini(&t);
    styled(&t, "AV", &s);
    CHECK(pc_text_engine_pair_kerning(e, &t, 72.0f, 0, &has, &v, NULL) == PC_OK);
    CHECK(has && v < -10.0f); /* Inter kerns AV */
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_bundled_fonts_cover_latin);
    RUN(test_metrics_are_stable_and_scale_with_dpi);
    RUN(test_small_caps_are_synthesized_without_smcp);
    RUN(test_real_small_caps_only_with_smcp);
    RUN(test_auto_and_explicit_leading);
    RUN(test_box_text_wraps_inside_width);
    RUN(test_alignment_point_and_box);
    RUN(test_rtl_text_is_reordered);
    RUN(test_truetype_collections_load);
    RUN(test_opentype_features_and_tracking);
    RUN(test_raster_coverage_scales_and_depths_agree);
    RUN(test_styles_change_pixels);
    RUN(test_multicolor_runs);
    RUN(test_hit_test_and_caret);
    RUN(test_empty_text_and_empty_lines);
    RUN(test_forced_line_break_starts_a_line_in_the_same_paragraph);
    RUN(test_postscript_names_fall_back_in_layout);
    RUN(test_depth_is_respected);
    RUN(test_variable_font_axes);
    RUN(test_warp_bends_rendered_text_and_outlines);
    RUN(test_clusters_sit_on_rendered_glyphs);
    RUN(test_manual_kerning_moves_the_next_glyph);
    RUN(test_optical_kerning_tightens_open_pairs);
    RUN(test_kerning_modes_split_pairs);
    RUN(test_japanese_box_text_preserves_wrap_boundaries);
    RUN(test_craft_fonts_cover_japanese_without_system_fonts);
    RUN(test_works_without_craft_fonts);
    RUN(test_word_and_line_navigation);
    RUN(test_preview_draws_ink);
    RUN(test_render_layer_sets_the_cache);
    RUN(test_pair_kerning);
    return pc_t_finish();
}
