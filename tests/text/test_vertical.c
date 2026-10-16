/* Vertical type (port of crates/text/src/vertical_tests.rs): column flow, upright CJK, vertical
 * alternates, rotated Latin, box wrapping, carets and hit tests. Tests needing a CJK font use an
 * installed system font and skip (with a note) when none is found. The PSD round trip of that
 * file belongs to chunk L15. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/file.h"
#include "base/utf8.h"
#include "harness.h"
#include "raster/surface.h"
#include "text/text.h"

static const pc_pixel_format RGBA8 = {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true};

static void vertical(pc_text_layer *t, const char *text, const char *family, float size)
{
    pc_text_layer_default(t);
    (void)pc_str_set_c(&t->text, text, NULL);
    t->runs = calloc(1, sizeof *t->runs);
    t->nruns = 1;
    t->runs[0].len = strlen(text);
    pc_char_style_default(&t->runs[0].style);
    (void)pc_str_set_c(&t->runs[0].style.font_family, family, NULL);
    t->runs[0].style.size_pt = size;
    t->orientation = PC_ORIENT_VERTICAL;
}

static pc_text_layout lay(pc_text_engine *e, const pc_text_layer *t)
{
    pc_text_layout l;
    if (pc_text_engine_layout(e, t, 72.0f, &l, NULL) != PC_OK) {
        PC_T_FAIL("layout failed");
        pc_text_layout_init(&l);
    }
    return l;
}

/* An engine with an installed Japanese (or Chinese) font registered, and its family. */
static pc_text_engine *cjk_engine(char *family, size_t cap)
{
    static const pc_cjk_script scripts[2] = {PC_CJK_JAPANESE, PC_CJK_SIMPLIFIED_CHINESE};
    size_t s, i, k;
    for (s = 0; s < 2; s++) {
        pc_cjk_font_file *files = NULL;
        size_t n = 0;
        if (pc_cjk_font_files(scripts[s], &files, &n, NULL) != PC_OK) {
            continue;
        }
        for (i = 0; i < n; i++) {
            pc_buf b = PC_BUF_INIT;
            pc_text_engine *e = NULL;
            pc_strlist names = {NULL, 0};
            const char *pick = NULL;
            if (pc_file_read(files[i].path, (size_t)256 << 20, &b, NULL) != PC_OK) {
                continue;
            }
            if (pc_text_engine_new(&e, NULL) == PC_OK &&
                pc_fontdb_register_font_data(pc_text_engine_fonts(e), b.data, b.len, &names, NULL) == PC_OK) {
                for (k = 0; k < names.n && files[i].family[0] != '\0'; k++) {
                    if (strcmp(names.v[k], files[i].family) == 0) {
                        pick = names.v[k];
                    }
                }
                if (pick == NULL && names.n > 0) {
                    pick = names.v[0];
                }
            }
            pc_buf_fini(&b);
            if (pick != NULL) {
                (void)snprintf(family, cap, "%s", pick);
                pc_strlist_fini(&names);
                pc_cjk_font_files_free(files, n);
                return e;
            }
            pc_strlist_fini(&names);
            pc_text_engine_free(e);
        }
        pc_cjk_font_files_free(files, n);
    }
    /* Not one of cjk.c's candidates, but a Japanese font with vertical alternates that is often
     * installed on Linux: lets these tests run on more machines than the reference's did. */
    {
        pc_buf b = PC_BUF_INIT;
        pc_text_engine *e = NULL;
        pc_strlist names = {NULL, 0};
        if (pc_file_read("/usr/share/fonts/vlgothic/VL-Gothic-Regular.ttf", (size_t)64 << 20, &b, NULL) ==
                PC_OK &&
            pc_text_engine_new(&e, NULL) == PC_OK &&
            pc_fontdb_register_font_data(pc_text_engine_fonts(e), b.data, b.len, &names, NULL) == PC_OK &&
            names.n > 0) {
            (void)snprintf(family, cap, "%s", names.v[0]);
            pc_strlist_fini(&names);
            pc_buf_fini(&b);
            return e;
        }
        pc_strlist_fini(&names);
        pc_buf_fini(&b);
        pc_text_engine_free(e);
    }
    (void)fprintf(stderr, "no CJK system font found: skipping\n");
    return NULL;
}

static void test_classes_follow_vertical_orientation(void)
{
    static const uint32_t up[] = {0x7E26, 0x304B, 0x30AB, 0xD55C, 0x3001, 0x3002, 0xFF21, 0xFF11};
    static const uint32_t alt[] = {0x30FC, 0x300C, 0x300D, 0xFF08, 0xFF09, 0x301C, 0x2026, 0x2014};
    static const uint32_t rot[] = {'A', 'z', '1', ' ', '-', 0xFF76, 0xE9};
    size_t i;
    for (i = 0; i < 8; i++) {
        CHECK(pc_text_vertical_class(up[i]) == PC_VCLASS_UPRIGHT);
        CHECK(pc_text_vertical_class(alt[i]) == PC_VCLASS_ALTERNATE_OR_ROTATE);
    }
    for (i = 0; i < 7; i++) {
        CHECK(pc_text_vertical_class(rot[i]) == PC_VCLASS_ROTATE);
    }
}

static void test_latin_is_rotated_and_runs_down_columns_right_to_left(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t;
    pc_text_layout l;
    float b[4];
    size_t i;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    vertical(&t, "abc\nde", "Inter", 20.0f);
    l = lay(e, &t);
    CHECK(l.vertical);
    CHECK_EQ_UINT(l.nlines, 2);
    for (i = 0; i < l.nglyphs; i++) {
        CHECK(l.glyphs[i].orient == PC_GLYPH_ROTATED);
    }
    if (l.nglyphs >= 4) {
        pc_placed_glyph a = l.glyphs[0], bb = l.glyphs[1], c = l.glyphs[2], d = l.glyphs[3];
        CHECK(a.y < bb.y && bb.y < c.y);
        CHECK(fabsf(a.x - bb.x) < 1e-3f && fabsf(bb.x - c.x) < 1e-3f);
        CHECK(fabsf(a.x - d.x - 24.0f) < 1e-3f);
        CHECK(fabsf(d.y - a.y) < 1e-3f);
        CHECK(a.x < 0.0f && a.x > -10.0f);
    }
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    vertical(&t, "abcdef", "Inter", 20.0f);
    l = lay(e, &t);
    CHECK(pc_text_layout_bounds(&l, b));
    CHECK((b[3] - b[1]) > 2.0f * (b[2] - b[0]));
    CHECK(fabsf(b[0] + 10.0f) < 1e-3f && fabsf(b[2] - 10.0f) < 1e-3f && fabsf(b[1]) < 1e-3f);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

static void test_rotated_render_ink_is_taller_than_wide(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t;
    pc_rendered h, v;
    uint32_t diff;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    vertical(&t, "Vertical", "Inter", 30.0f);
    t.orientation = PC_ORIENT_HORIZONTAL;
    t.transform = pc_affine_translate(100.0, 50.0);
    CHECK(pc_text_engine_render(e, &t, 72.0f, RGBA8, NULL, &h, NULL) == PC_OK);
    t.orientation = PC_ORIENT_VERTICAL;
    CHECK(pc_text_engine_render(e, &t, 72.0f, RGBA8, NULL, &v, NULL) == PC_OK);
    CHECK(pc_rect_width(h.rect) > pc_rect_height(h.rect));
    CHECK(pc_rect_height(v.rect) > 2 * pc_rect_width(v.rect));
    diff = pc_rect_height(v.rect) > pc_rect_width(h.rect) ? pc_rect_height(v.rect) - pc_rect_width(h.rect)
                                                          : pc_rect_width(h.rect) - pc_rect_height(v.rect);
    CHECK(diff <= 3);
    CHECK(v.rect.y0 >= 49 && v.rect.x0 < 100 && v.rect.x1 > 100);
    pc_rendered_fini(&h);
    pc_rendered_fini(&v);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

static void test_caret_hit_test_and_arrow_geometry_follow_columns(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t;
    pc_text_layout l;
    float s1[4], s2[4], u, v, x, y;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    vertical(&t, "abc\ndef", "Inter", 20.0f);
    l = lay(e, &t);
    CHECK_EQ_UINT(pc_text_layout_hit_test(&l, 0.0f, -5.0f), 0);
    CHECK_EQ_UINT(pc_text_layout_hit_test(&l, 0.0f, 1000.0f), 3);
    CHECK_EQ_UINT(pc_text_layout_hit_test(&l, -24.0f, -5.0f), 4);
    pc_text_layout_caret_segment(&l, 1, s1);
    CHECK(fabsf(s1[1] - s1[3]) < 1e-4f && s1[0] > s1[2] && fabsf(s1[0] - s1[2] - 20.0f) < 1e-3f);
    pc_text_layout_caret_segment(&l, 2, s2);
    CHECK(s2[1] > s1[1]);
    pc_text_layout_to_line(&l, -7.0f, 33.0f, &u, &v);
    pc_text_layout_to_text(&l, u, v, &x, &y);
    CHECK(x == -7.0f && y == 33.0f);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

static void test_box_text_wraps_columns_within_the_box_height(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t;
    pc_text_layout l;
    float b[4];
    size_t i;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    vertical(&t, "ab cd ef gh ij kl mn op", "Inter", 20.0f);
    t.shape = (pc_text_shape){.kind = PC_TEXT_SHAPE_BOX, .x = 10.0f, .y = 20.0f, .width = 200.0f, .height = 60.0f};
    l = lay(e, &t);
    CHECK(l.nlines >= 3);
    CHECK(pc_text_layout_bounds(&l, b));
    CHECK(fabsf(b[2] - 210.0f) < 1e-3f);
    CHECK(b[0] >= 10.0f - 1e-3f && b[1] >= 20.0f - 1e-3f && b[3] <= 80.0f + 1e-3f);
    for (i = 0; i < l.nglyphs; i++) {
        CHECK(l.glyphs[i].y >= 20.0f - 1e-3f && l.glyphs[i].y <= 80.0f + 1e-3f);
    }
    pc_text_layout_fini(&l);
    t.shape = (pc_text_shape){.kind = PC_TEXT_SHAPE_BOX, .x = 0.0f, .y = 0.0f, .width = 30.0f, .height = 60.0f};
    l = lay(e, &t);
    CHECK_EQ_UINT(l.nlines, 1);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

static void test_horizontal_layout_is_unchanged_by_the_vertical_path(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t;
    pc_text_layout l;
    float a[4], b[4], x, y;
    size_t i;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    vertical(&t, "abc", "Inter", 20.0f);
    t.orientation = PC_ORIENT_HORIZONTAL;
    l = lay(e, &t);
    CHECK(!l.vertical);
    for (i = 0; i < l.nglyphs; i++) {
        CHECK(l.glyphs[i].orient == PC_GLYPH_HORIZONTAL);
    }
    CHECK(pc_text_layout_bounds(&l, a) && pc_text_layout_line_bounds(&l, b) && memcmp(a, b, sizeof a) == 0);
    pc_text_layout_to_text(&l, 3.0f, 4.0f, &x, &y);
    CHECK(x == 3.0f && y == 4.0f);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

static void test_cjk_is_upright_centred_with_vertical_alternates(void)
{
    char fam[256];
    pc_text_engine *e = cjk_engine(fam, sizeof fam);
    pc_text_layer t;
    pc_text_layout l, h, s;
    pc_rendered r;
    size_t i;
    const char *text = "縦書き、テスト。ーA";
    if (e == NULL) {
        return;
    }
    vertical(&t, text, fam, 40.0f);
    l = lay(e, &t);
    t.orientation = PC_ORIENT_HORIZONTAL;
    h = lay(e, &t);
    CHECK_EQ_UINT(l.nglyphs, 10);
    if (l.nglyphs == 10 && h.nglyphs == 10) {
        const pc_placed_glyph *g = l.glyphs;
        for (i = 0; i < 10; i++) {
            CHECK(g[i].id != 0);
        }
        for (i = 0; i < 8; i++) {
            CHECK(g[i].orient == PC_GLYPH_UPRIGHT);
            CHECK(fabsf(g[i].x + 20.0f) < 4.0f);
        }
        for (i = 1; i < 10; i++) {
            CHECK(g[i].y > g[i - 1].y);
        }
        CHECK(g[0].y > 20.0f && g[0].y < 40.0f);
        CHECK(g[3].id != h.glyphs[3].id);
        CHECK(g[7].id != h.glyphs[7].id);
        if (g[8].id != h.glyphs[8].id) {
            CHECK(g[8].orient == PC_GLYPH_UPRIGHT);
        } else {
            CHECK(g[8].orient == PC_GLYPH_ROTATED);
        }
        CHECK(g[9].orient == PC_GLYPH_ROTATED);
    }
    pc_text_layout_fini(&l);
    pc_text_layout_fini(&h);
    pc_text_layer_fini(&t);
    vertical(&t, "ト、「東", fam, 40.0f);
    s = lay(e, &t);
    if (s.nclusters >= 3 && s.nglyphs >= 3) {
        CHECK(fabsf(s.clusters[1].x - s.clusters[0].x - 40.0f) < 1.0f &&
              fabsf(s.clusters[2].x - s.clusters[1].x - 20.0f) < 1.0f);
        CHECK(fabsf(s.glyphs[2].y - s.glyphs[1].y - 20.0f) < 1.0f);
    } else {
        CHECK(s.nclusters >= 3);
    }
    pc_text_layout_fini(&s);
    pc_text_layer_fini(&t);
    vertical(&t, "縦書きテスト", fam, 40.0f);
    t.transform = pc_affine_translate(900.0, 30.0);
    CHECK(pc_text_engine_render(e, &t, 72.0f, RGBA8, NULL, &r, NULL) == PC_OK);
    CHECK(pc_rect_height(r.rect) > 4 * pc_rect_width(r.rect));
    CHECK(r.rect.x0 > 870 && r.rect.x1 < 930 && r.rect.y0 >= 28 && r.rect.y1 < 30 + 6 * 40 + 4);
    pc_rendered_fini(&r);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

static void test_nearest_face_is_not_synthetically_emboldened(void)
{
    CHECK(!pc_text_synthetic_bold(true, 400, 300.0f));
    CHECK(!pc_text_synthetic_bold(true, 500, 300.0f));
    CHECK(pc_text_synthetic_bold(true, 700, 400.0f));
    CHECK(!pc_text_synthetic_bold(true, 700, 600.0f));
    CHECK(!pc_text_synthetic_bold(false, 900, 100.0f));
}

static void test_punctuation_squeeze_removes_half_em_between_marks(void)
{
    const char *text = "ト、「東」。";
    pc_cluster_info cl[6], cl2[3], none[1], bad[3];
    pc_placed_glyph g[6];
    size_t i, at = 0;
    float x = 0.0f;
    static const float want[6] = {0.0f, 40.0f, 60.0f, 100.0f, 140.0f, 160.0f};
    for (i = 0; i < 6; i++) {
        cl[i] = (pc_cluster_info){at, at + 3, x, 40.0f, 0, false};
        g[i] = (pc_placed_glyph){0, (uint32_t)i + 1, x, 0.0f, 0, PC_GLYPH_HORIZONTAL};
        at += 3;
        x += 40.0f;
    }
    CHECK(pc_text_squeeze_punctuation(text, strlen(text), cl, 6, g, 6) == 40.0f);
    for (i = 0; i < 6; i++) {
        CHECK(cl[i].x == want[i] && g[i].x == want[i]);
    }
    text = "「「a";
    cl2[0] = (pc_cluster_info){0, 3, 0.0f, 40.0f, 0, false};
    cl2[1] = (pc_cluster_info){3, 6, 40.0f, 40.0f, 0, false};
    cl2[2] = (pc_cluster_info){6, 7, 80.0f, 40.0f, 0, false};
    CHECK(pc_text_squeeze_punctuation(text, strlen(text), cl2, 3, NULL, 0) == 20.0f);
    CHECK(cl2[0].x == 0.0f && cl2[1].x == 20.0f && cl2[2].x == 60.0f);
    none[0] = (pc_cluster_info){0, 3, 0.0f, 40.0f, 0, false};
    CHECK(pc_text_squeeze_punctuation("東京", strlen("東京"), none, 1, NULL, 0) == 0.0f);
    for (i = 0; i < 3; i++) {
        bad[i] = (pc_cluster_info){50, 60, 0.0f, 1.0f, 0, false};
    }
    CHECK(pc_text_squeeze_punctuation("", 0, bad, 3, NULL, 0) == 0.0f);
    CHECK(pc_text_squeeze_punctuation("", 0, NULL, 0, NULL, 0) == 0.0f);
}

static void test_hostile_vertical_input_does_not_panic(void)
{
    static const char *const texts[] = {"", "\n\n", "\xE2\x80\x8F\xD7\x90\xCC\x81",
                                        "「」ー。、\r\n\xEF\xBF\xBF", "a\0b"};
    static const size_t lens[] = {0, 2, 7, 20, 3};
    pc_text_engine *e = NULL;
    size_t i, s;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    for (i = 0; i < 5; i++) {
        for (s = 0; s < 3; s++) {
            pc_text_layer t;
            pc_text_layout l;
            pc_rendered r;
            float seg[4], b[4];
            vertical(&t, "", "Inter", 12.0f);
            (void)pc_str_set(&t.text, texts[i], lens[i], NULL);
            t.runs[0].len = lens[i];
            if (s == 1) {
                t.shape = (pc_text_shape){.kind = PC_TEXT_SHAPE_BOX, .x = 0.0f, .y = 0.0f, .width = 0.0f, .height = 0.0f};
            } else if (s == 2) {
                t.shape = (pc_text_shape){.kind = PC_TEXT_SHAPE_BOX, .x = NAN, .y = 1e30f, .width = -5.0f, .height = 1.0f};
            }
            CHECK(pc_text_engine_render(e, &t, 72.0f, RGBA8, &l, &r, NULL) == PC_OK);
            (void)pc_text_layout_bounds(&l, b);
            (void)pc_text_layout_hit_test(&l, 1.0f, 2.0f);
            pc_text_layout_caret_segment(&l, 1, seg);
            pc_text_layout_fini(&l);
            pc_rendered_fini(&r);
            pc_text_layer_fini(&t);
        }
    }
    pc_text_engine_free(e);
}

static void kerned(pc_text_layer *out, const pc_text_layer *t, float kern)
{
    size_t first = 0, len = t->text.len;
    uint32_t cp;
    const pc_char_style *base = &t->runs[0].style;
    (void)pc_utf8_next((const uint8_t *)t->text.ptr, len, &first, &cp);
    (void)pc_text_layer_clone(t, out, NULL);
    pc_text_runs_free(out->runs, out->nruns);
    out->runs = calloc(2, sizeof *out->runs);
    out->nruns = 2;
    out->runs[0].len = first;
    (void)pc_char_style_clone(base, &out->runs[0].style, NULL);
    out->runs[0].style.kerning = PC_KERNING_OFF;
    out->runs[0].style.kern = kern;
    out->runs[1].len = len - first;
    (void)pc_char_style_clone(base, &out->runs[1].style, NULL);
}

static void test_manual_kerning_runs_along_the_column(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t, k;
    pc_text_layout plain, kl, o;
    float b0[4], b1[4];
    size_t i;
    char fam[256];
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    vertical(&t, "HOH", "Inter", 40.0f);
    plain = lay(e, &t);
    kerned(&k, &t, 100.0f);
    kl = lay(e, &k);
    CHECK(kl.vertical);
    CHECK(kl.nglyphs == 3 && plain.nglyphs == 3);
    if (kl.nglyphs == 3 && plain.nglyphs == 3) {
        CHECK(kl.glyphs[0].y == plain.glyphs[0].y);
        for (i = 1; i < 3; i++) {
            CHECK(fabsf(kl.glyphs[i].y - plain.glyphs[i].y - 4.0f) < 1e-3f);
            CHECK(fabsf(kl.glyphs[i].x - plain.glyphs[i].x) < 1e-3f);
        }
    }
    CHECK(pc_text_layout_bounds(&plain, b0) && pc_text_layout_bounds(&kl, b1));
    CHECK(fabsf(b1[3] - b0[3] - 4.0f) < 1e-3f);
    pc_text_layout_fini(&plain);
    pc_text_layout_fini(&kl);
    pc_text_layer_fini(&t);
    pc_text_layer_fini(&k);
    vertical(&t, "AVATAR", "Inter", 40.0f);
    t.runs[0].style.kerning = PC_KERNING_OPTICAL;
    o = lay(e, &t);
    CHECK_EQ_UINT(o.nglyphs, 6);
    pc_text_layout_fini(&o);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
    e = cjk_engine(fam, sizeof fam);
    if (e == NULL) {
        return;
    }
    vertical(&t, "東京、「都」", fam, 40.0f);
    plain = lay(e, &t);
    kerned(&k, &t, -250.0f);
    kl = lay(e, &k);
    if (kl.nglyphs >= 4 && plain.nglyphs >= 4) {
        CHECK(fabsf(kl.glyphs[1].y - plain.glyphs[1].y + 10.0f) < 1e-3f);
        CHECK(fabsf((kl.glyphs[3].y - kl.glyphs[2].y) - (plain.glyphs[3].y - plain.glyphs[2].y)) < 1e-3f);
    } else {
        CHECK(kl.nglyphs >= 4);
    }
    pc_text_layout_fini(&plain);
    pc_text_layout_fini(&kl);
    pc_text_layer_fini(&t);
    pc_text_layer_fini(&k);
    pc_text_engine_free(e);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_classes_follow_vertical_orientation);
    RUN(test_latin_is_rotated_and_runs_down_columns_right_to_left);
    RUN(test_rotated_render_ink_is_taller_than_wide);
    RUN(test_caret_hit_test_and_arrow_geometry_follow_columns);
    RUN(test_box_text_wraps_columns_within_the_box_height);
    RUN(test_horizontal_layout_is_unchanged_by_the_vertical_path);
    RUN(test_cjk_is_upright_centred_with_vertical_alternates);
    RUN(test_nearest_face_is_not_synthetically_emboldened);
    RUN(test_punctuation_squeeze_removes_half_em_between_marks);
    RUN(test_hostile_vertical_input_does_not_panic);
    RUN(test_manual_kerning_runs_along_the_column);
    return pc_t_finish();
}
