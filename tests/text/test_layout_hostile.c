/* Never-crash tests for the layouter and the text engine (AGENTS.md "Never crash"): huge and
 * malformed text, absurd numbers, runs that don't fit the text, damaged fonts, deep bidi
 * nesting, and the shared engine used from several threads. Every call must return a status and
 * leave nothing leaked (run under ASan/UBSan and TSan). */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "harness.h"
#include "raster/surface.h"
#include "text/text.h"

static const pc_pixel_format RGBA8 = {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true};

static void one_run(pc_text_layer *t, const char *text, size_t len, const pc_char_style *s)
{
    pc_text_layer_default(t);
    (void)pc_str_set(&t->text, text, len, NULL);
    t->runs = calloc(1, sizeof *t->runs);
    t->nruns = 1;
    t->runs[0].len = len;
    (void)pc_char_style_clone(s, &t->runs[0].style, NULL);
}

/* Lays out, renders, and walks every navigation function; only checks that nothing fails
 * unexpectedly (a status is fine) and that results stay in range. */
static void exercise(pc_text_engine *e, const pc_text_layer *t, float dpi)
{
    pc_text_layout l;
    pc_rendered r;
    pc_status st = pc_text_engine_layout(e, t, dpi, &l, NULL);
    const char *text = pc_str_c(&t->text);
    size_t len = t->text.len, i;
    if (st == PC_OK) {
        float seg[4], b[4], x, top, bot;
        size_t ci, li;
        (void)pc_text_layout_bounds(&l, b);
        for (i = 0; i <= len + 2 && i < 64; i++) {
            pc_text_layout_caret(&l, i, &x, &top, &bot);
            pc_text_layout_caret_segment(&l, i, seg);
            (void)pc_text_line_index(&l, i);
            (void)pc_text_line_step(&l, text, len, i, x, 1);
            (void)pc_text_line_step(&l, text, len, i, NAN, -1);
            (void)pc_text_line_edge(&l, text, len, i, true);
            (void)pc_text_word_boundary(text, len, i, i % 2 == 0);
        }
        CHECK(pc_text_layout_hit_test(&l, NAN, INFINITY) <= len);
        pc_text_hit_char(&l, text, len, -1e30f, 1e30f, &ci, &li);
        (void)pc_text_point_inside(&l, 0.0f, 0.0f, NAN);
        for (i = 0; i < l.nlines; i++) {
            CHECK(l.lines[i].start <= l.lines[i].end && l.lines[i].end <= len);
        }
        for (i = 0; i < l.nclusters; i++) {
            CHECK(l.clusters[i].start <= l.clusters[i].end && l.clusters[i].end <= len);
        }
        for (i = 0; i < l.nglyphs; i++) {
            CHECK(l.glyphs[i].face < l.nfaces && l.glyphs[i].style < l.nstyles);
        }
        pc_text_layout_fini(&l);
    }
    if (pc_text_engine_render(e, t, dpi, RGBA8, NULL, &r, NULL) == PC_OK) {
        pc_rendered_fini(&r);
    }
    for (i = 0; i < len && i < 8; i++) {
        bool has;
        float k;
        (void)pc_text_engine_pair_kerning(e, t, dpi, i, &has, &k, NULL);
    }
}

static void test_invalid_utf8_is_an_error(void)
{
    pc_text_engine *e = NULL;
    pc_char_style s;
    pc_text_layer t;
    pc_text_layout l;
    pc_rendered r;
    pc_error err;
    static const char bad[] = "ab\xff\xc3(\xed\xa0\x80z";
    pc_char_style_default(&s);
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    one_run(&t, bad, sizeof bad - 1, &s);
    CHECK(pc_text_engine_layout(e, &t, 72.0f, &l, &err) == PC_ERR_INVALID);
    CHECK(pc_text_engine_render(e, &t, 72.0f, RGBA8, NULL, &r, NULL) == PC_ERR_INVALID);
    exercise(e, &t, 72.0f);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

static void test_absurd_numbers(void)
{
    static const float sizes[] = {0.0f, -10.0f, NAN, INFINITY, -INFINITY, 1e-30f, 1e9f, 3.4e38f};
    static const float boxes[][4] = {{0, 0, 0, 0},         {NAN, NAN, NAN, NAN},
                                     {0, 0, -100, -100},   {1e30f, -1e30f, 1e30f, 1e30f},
                                     {0, 0, INFINITY, 50}, {0, 0, 1, 1e9f}};
    static const float dpis[] = {72.0f, 0.0f, -1.0f, NAN, INFINITY, 1e-9f};
    pc_text_engine *e = NULL;
    size_t i, j;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        for (j = 0; j < sizeof boxes / sizeof boxes[0] + 1; j++) {
            pc_char_style s;
            pc_text_layer t;
            pc_paragraph_style p = pc_paragraph_style_default();
            pc_char_style_default(&s);
            s.size_pt = sizes[i];
            s.tracking = j % 2 == 0 ? 1e30f : -NAN;
            s.horizontal_scale = j % 3 == 0 ? 1e30f : NAN;
            s.vertical_scale = -1e30f;
            s.baseline_shift_pt = j % 2 == 0 ? INFINITY : -1e30f;
            s.kern = j % 2 == 0 ? NAN : 1e30f;
            s.kerning = j % 3 == 0 ? PC_KERNING_OPTICAL : PC_KERNING_METRICS;
            s.has_leading = j % 2 == 1;
            s.leading_pt = sizes[(i + 1) % 8];
            s.caps = j % 2 == 0 ? PC_CAPS_SMALL_CAPS : PC_CAPS_ALL_CAPS;
            s.underline = s.strikethrough = true;
            one_run(&t, "Absurd\nnumbers here ﬁ", strlen("Absurd\nnumbers here ﬁ"), &s);
            p.first_line_indent_pt = sizes[(i + 2) % 8];
            p.start_indent_pt = sizes[(i + 3) % 8];
            p.end_indent_pt = sizes[(i + 4) % 8];
            p.space_before_pt = sizes[(i + 5) % 8];
            p.space_after_pt = sizes[(i + 6) % 8];
            p.auto_leading = sizes[(i + 7) % 8];
            p.align = (pc_text_align)(j % 7);
            t.paragraphs = calloc(1, sizeof *t.paragraphs);
            t.nparagraphs = 1;
            t.paragraphs[0].len = t.text.len;
            t.paragraphs[0].style = p;
            if (j < sizeof boxes / sizeof boxes[0]) {
                t.shape = (pc_text_shape){true, boxes[j][0], boxes[j][1], boxes[j][2], boxes[j][3]};
            }
            t.orientation = j % 2 == 0 ? PC_ORIENT_VERTICAL : PC_ORIENT_HORIZONTAL;
            exercise(e, &t, dpis[(i + j) % 6]);
            pc_text_layer_fini(&t);
        }
    }
    pc_text_engine_free(e);
}

static void test_runs_that_dont_fit_the_text(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t;
    size_t k;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    for (k = 0; k < 5; k++) {
        size_t i;
        pc_text_layer_default(&t);
        (void)pc_str_set_c(&t.text, "héllo wörld\n日本\r\nend", NULL);
        t.nruns = 4;
        t.runs = calloc(4, sizeof *t.runs);
        for (i = 0; i < 4; i++) {
            pc_char_style_default(&t.runs[i].style);
            t.runs[i].style.size_pt = 8.0f + 4.0f * (float)i;
            t.runs[i].len = k == 0 ? 0 : k == 1 ? SIZE_MAX / 2 : k == 2 ? 1 : k == 3 ? 3 * i + 1 : SIZE_MAX;
        }
        t.nparagraphs = 2;
        t.paragraphs = calloc(2, sizeof *t.paragraphs);
        t.paragraphs[0].len = k == 4 ? SIZE_MAX : 1;
        t.paragraphs[0].style = pc_paragraph_style_default();
        t.paragraphs[1].len = 0;
        t.paragraphs[1].style = pc_paragraph_style_default();
        exercise(e, &t, 72.0f);
        pc_text_layer_fini(&t);
    }
    pc_text_engine_free(e);
}

static void test_huge_text(void)
{
    pc_text_engine *e = NULL;
    pc_char_style s;
    pc_text_layer t;
    pc_buf b = PC_BUF_INIT;
    size_t i;
    pc_text_layout l;
    pc_char_style_default(&s);
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    for (i = 0; i < 20000; i++) {
        (void)pc_buf_append_str(&b, i % 50 == 49 ? "word\n" : i % 7 == 0 ? "שלום " : "word ", NULL);
    }
    one_run(&t, (const char *)b.data, b.len, &s);
    t.shape = (pc_text_shape){true, 0.0f, 0.0f, 300.0f, 1e9f};
    CHECK(pc_text_engine_layout(e, &t, 72.0f, &l, NULL) == PC_OK);
    CHECK(l.nlines > 400);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    /* One enormous word in a narrow box, and the same as point text. */
    pc_buf_truncate(&b, 0);
    for (i = 0; i < 100000; i++) {
        (void)pc_buf_append_byte(&b, (uint8_t)('a' + i % 26), NULL);
    }
    one_run(&t, (const char *)b.data, b.len, &s);
    t.shape = (pc_text_shape){true, 0.0f, 0.0f, 10.0f, 100.0f};
    CHECK(pc_text_engine_layout(e, &t, 72.0f, &l, NULL) == PC_OK);
    pc_text_layout_fini(&l);
    t.shape.is_box = false;
    CHECK(pc_text_engine_layout(e, &t, 72.0f, &l, NULL) == PC_OK);
    CHECK_EQ_UINT(l.nglyphs, 100000);
    pc_text_layout_fini(&l);
    pc_text_layer_fini(&t);
    pc_buf_fini(&b);
    pc_text_engine_free(e);
}

static void test_text_over_the_limit(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t;
    pc_text_layout l;
    char *big = malloc(PC_TEXT_MAX_LAYOUT_BYTES + 1);
    if (big == NULL) {
        return;
    }
    memset(big, 'a', PC_TEXT_MAX_LAYOUT_BYTES + 1);
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    pc_text_layer_default(&t);
    t.text = pc_str_adopt(big, PC_TEXT_MAX_LAYOUT_BYTES + 1);
    CHECK(pc_text_engine_layout(e, &t, 72.0f, &l, NULL) == PC_ERR_LIMIT);
    pc_text_layer_fini(&t);
    pc_text_engine_free(e);
}

static void test_many_runs_and_deep_bidi(void)
{
    pc_text_engine *e = NULL;
    pc_text_layer t;
    pc_buf b = PC_BUF_INIT;
    size_t i, n;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    /* Every character its own style. */
    for (i = 0; i < 3000; i++) {
        (void)pc_buf_append_str(&b, i % 3 == 0 ? "é" : i % 3 == 1 ? "x" : "א", NULL);
    }
    pc_text_layer_default(&t);
    (void)pc_str_set(&t.text, (const char *)b.data, b.len, NULL);
    n = 0;
    t.runs = calloc(3000, sizeof *t.runs);
    for (i = 0; i < 3000; i++) {
        pc_char_style_default(&t.runs[i].style);
        t.runs[i].style.size_pt = 6.0f + (float)(i % 17);
        t.runs[i].style.tracking = (float)(i % 5) * 10.0f;
        t.runs[i].style.kerning = (pc_kerning)(i % 3);
        t.runs[i].len = i % 3 == 1 ? 1 : 2;
        n++;
    }
    t.nruns = n;
    exercise(e, &t, 72.0f);
    pc_text_layer_fini(&t);
    /* 300 nested embeddings, overrides and isolates, never closed. */
    pc_buf_truncate(&b, 0);
    for (i = 0; i < 300; i++) {
        static const char *const marks[] = {"\u202B", "\u202A", "\u202E", "\u202D",
                                            "\u2067", "\u2066", "\u2068"};
        (void)pc_buf_append_str(&b, marks[i % 7], NULL);
        (void)pc_buf_append_str(&b, i % 2 == 0 ? "a" : "ש", NULL);
    }
    for (i = 0; i < 100; i++) {
        (void)pc_buf_append_str(&b, i % 2 == 0 ? "\u202C" : "\u2069", NULL);
    }
    {
        pc_char_style s;
        pc_char_style_default(&s);
        one_run(&t, (const char *)b.data, b.len, &s);
        exercise(e, &t, 72.0f);
        t.orientation = PC_ORIENT_VERTICAL;
        exercise(e, &t, 72.0f);
        pc_text_layer_fini(&t);
    }
    pc_buf_fini(&b);
    pc_text_engine_free(e);
}

static void test_odd_characters_and_fonts(void)
{
    static const char *const texts[] = {
        "\0\0\0",
        "\xef\xbb\xbf\xe2\x80\x8b\xe2\x80\x8c\xe2\x80\x8d",
        "\xcc\x81\xcc\x81\xcc\x81 leading marks",
        "e\xcc\x81\xcc\x82\xcc\x83\xcc\x84\xcc\x85\xcc\x86\xcc\x87\xcc\x88\xcc\x89\xcc\x8a\xcc\x8b\xcc\x8c"
        "\xcc\x8d"
        "\xcc\x8e\xcc\x8f\xcc\x90\xcc\x91\xcc\x92\xcc\x93\xcc\x94\xcc\x95\xcc\x96\xcc\x97\xcc\x98\xcc\x99\xcc"
        "\x9a"
        "\xcc\x9b\xcc\x9c\xcc\x9d\xcc\x9e\xcc\x9f\xcc\xa0\xcc\xa1\xcc\xa2\xcc\xa3",
        "\xf0\x9f\x87\xaf\xf0\x9f\x87\xb5\xf0\x9f\x8f\xb4\xf3\xa0\x81\xa7\xf3\xa0\x81\xa2\xf3\xa0\x81\xbf",
        "\xf4\x8f\xbf\xbf\xee\x80\x80\xef\xbf\xbe",
        "\t\t\v\f\r\r\n\xc2\x85\xe2\x80\xa8\xe2\x80\xa9",
        "\x03\x03\x03",
        "ｶﾀｶﾅ ﾊﾝｶｸ",
        "ﷺ ﷽",
        "a\u200e\u200f\u200e\u200fb"};
    static const char *const families[] = {"",           "Inter",  "JetBrains Mono", "\"\\Bad\" \"Family",
                                           "sans-serif", "Damaged"};
    pc_text_engine *e = NULL;
    size_t i, f, n;
    const pc_bundled_font *bf = pc_text_bundled_fonts(&n);
    pc_buf dmg = PC_BUF_INIT;
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    /* A damaged copy of a bundled font: tables cut off half way, glyph data garbled. */
    if (n > 0) {
        size_t k;
        (void)pc_buf_append(&dmg, bf[0].data, bf[0].len / 2, NULL);
        for (k = dmg.len / 3; k < dmg.len; k += 97) {
            dmg.data[k] ^= 0x5A;
        }
        (void)pc_fontdb_register_font_data(pc_text_engine_fonts(e), dmg.data, dmg.len, NULL, NULL);
    }
    for (i = 0; i < sizeof texts / sizeof texts[0]; i++) {
        for (f = 0; f < sizeof families / sizeof families[0]; f++) {
            pc_char_style s;
            pc_text_layer t;
            pc_font_feature feat;
            pc_font_variation var;
            pc_char_style_default(&s);
            (void)pc_str_set_c(&s.font_family, families[f], NULL);
            s.size_pt = 14.0f;
            s.weight = (uint16_t)(f * 300);
            s.italic = f % 2 == 1;
            memset(&feat, 0, sizeof feat);
            (void)pc_str_set(&feat.tag, "\x01\"x\xff", 4, NULL);
            feat.value = 65535;
            memset(&var, 0, sizeof var);
            (void)pc_str_set_c(&var.axis, "wght", NULL);
            var.value = NAN;
            s.features = &feat;
            s.nfeatures = 1;
            s.variations = &var;
            s.nvariations = 1;
            s.has_language = true;
            (void)pc_str_set_c(&s.language, f % 2 == 0 ? "x-private-only-y" : "zh-Hant-TW-u-co-pinyin", NULL);
            one_run(&t, texts[i], i == 0 ? 3 : strlen(texts[i]), &s);
            s.features = NULL;
            s.nfeatures = 0;
            s.variations = NULL;
            s.nvariations = 0;
            pc_str_fini(&feat.tag);
            pc_str_fini(&var.axis);
            pc_char_style_fini(&s);
            t.orientation = (f % 3 == 0) ? PC_ORIENT_VERTICAL : PC_ORIENT_HORIZONTAL;
            exercise(e, &t, 72.0f);
            pc_text_layer_fini(&t);
        }
    }
    pc_buf_fini(&dmg);
    pc_text_engine_free(e);
}

static void test_nav_on_empty_and_bad_inputs(void)
{
    pc_text_layout l;
    float x, top, bot, seg[4];
    size_t ci, li;
    pc_text_layout_init(&l);
    CHECK_EQ_UINT(pc_text_layout_hit_test(&l, 1.0f, 1.0f), 0);
    pc_text_layout_caret(&l, 5, &x, &top, &bot);
    CHECK(x == 0.0f && top == 0.0f && bot == 0.0f);
    pc_text_layout_caret_segment(&l, 0, seg);
    CHECK_EQ_UINT(pc_text_line_index(&l, 3), 0);
    pc_text_hit_char(&l, "abc", 3, 0.0f, 0.0f, &ci, &li);
    CHECK(ci == 0 && li == 0);
    CHECK(!pc_text_point_inside(&l, 0.0f, 0.0f, 1.0f));
    CHECK_EQ_UINT(pc_text_line_step(&l, "abc", 3, 9, 0.0f, 1), 3);
    CHECK_EQ_UINT(pc_text_line_edge(&l, "abc", 3, 9, true), 3);
    CHECK_EQ_UINT(pc_text_char_index("\xc3\xa9\xc3", 3, 2), 1);
    CHECK_EQ_UINT(pc_text_char_index("\xc3\xa9", 2, 1), 0);
    CHECK_EQ_UINT(pc_text_char_index("\xff\xfe", 2, 99), 2);
    CHECK_EQ_UINT(pc_text_byte_index("ab", 2, 99), 2);
    CHECK_EQ_UINT(pc_text_word_boundary("\xff a", 3, 0, true), 3);
    pc_text_layout_fini(&l);
    pc_text_layout_fini(&l);
}

/* ---- threads ---- */

static void *worker(void *arg)
{
    int id = *(const int *)arg, k;
    for (k = 0; k < 6; k++) {
        pc_text_engine *e = NULL;
        pc_text_layer t;
        pc_char_style s;
        pc_rendered r;
        pc_char_style_default(&s);
        s.size_pt = 10.0f + (float)(id * 3 + k);
        s.kerning = PC_KERNING_OPTICAL;
        one_run(&t, "Shared engine AVATAR שלום", strlen("Shared engine AVATAR שלום"), &s);
        if (k % 2 == 0) {
            if (pc_text_engine_shared_lock(&e, NULL) == PC_OK) {
                if (pc_text_engine_render(e, &t, 72.0f, RGBA8, NULL, &r, NULL) == PC_OK) {
                    pc_rendered_fini(&r);
                }
                pc_text_engine_shared_unlock();
            }
        } else if (pc_text_engine_new(&e, NULL) == PC_OK) {
            /* A private engine: fonts of different databases render concurrently. */
            if (pc_text_engine_render(e, &t, 72.0f, RGBA8, NULL, &r, NULL) == PC_OK) {
                pc_rendered_fini(&r);
            }
            pc_text_engine_free(e);
        }
        pc_text_layer_fini(&t);
        (void)pc_spell_check(pc_spell_english(), "recieve", 7, NULL);
    }
    return NULL;
}

static void test_threads(void)
{
    pthread_t th[4];
    int ids[4] = {0, 1, 2, 3}, i;
    for (i = 0; i < 4; i++) {
        CHECK(pthread_create(&th[i], NULL, worker, &ids[i]) == 0);
    }
    for (i = 0; i < 4; i++) {
        (void)pthread_join(th[i], NULL);
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    (void)setenv("PHOTOCRAFT_NO_SYSTEM_FONTS", "1", 1);
    RUN(test_invalid_utf8_is_an_error);
    RUN(test_absurd_numbers);
    RUN(test_runs_that_dont_fit_the_text);
    RUN(test_huge_text);
    RUN(test_text_over_the_limit);
    RUN(test_many_runs_and_deep_bidi);
    RUN(test_odd_characters_and_fonts);
    RUN(test_nav_on_empty_and_bad_inputs);
    RUN(test_threads);
    return pc_t_finish();
}
