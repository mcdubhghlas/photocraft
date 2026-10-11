/* Spelling checker and Warp Text styles (port of the tests in crates/text/src/spell.rs and
 * warp.rs). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "text/text.h"

static bool chk(const pc_spell_dict *d, const char *w)
{
    return pc_spell_check(d, w, strlen(w), NULL);
}

static void test_bundled_dictionary_loads(void)
{
    const pc_spell_dict *d = pc_spell_english();
    static const char *const good[] = {"the",   "The",   "THE",   "color", "colors",         "running",
                                       "don't", "don’t", "Paris", "PARIS", "photographer's", "NASA"};
    static const char *const bad[] = {"teh", "recieve", "paris", "Photograpy", "zxqv"};
    size_t i;
    CHECK(pc_spell_len(d) > 70000);
    for (i = 0; i < sizeof good / sizeof good[0]; i++) {
        if (!chk(d, good[i])) {
            PC_T_FAIL("%s should be correct", good[i]);
        }
    }
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        if (chk(d, bad[i])) {
            PC_T_FAIL("%s should be flagged", bad[i]);
        }
    }
    CHECK(chk(d, "2026") && chk(d, "B2B"));
}

static bool suggest_has(const pc_spell_dict *d, const char *w, size_t max, size_t at, const char *want)
{
    pc_spell_list l = {NULL, 0};
    bool ok =
        pc_spell_suggest(d, w, strlen(w), max, &l, NULL) == PC_OK && at < l.n && strcmp(l.v[at], want) == 0;
    pc_spell_list_fini(&l);
    return ok;
}

static void test_suggestions_rank_by_distance_then_frequency(void)
{
    const pc_spell_dict *d = pc_spell_english();
    pc_spell_list l = {NULL, 0};
    size_t i;
    bool found = false, upper = true;
    CHECK(suggest_has(d, "teh", 3, 0, "the"));
    CHECK(pc_spell_suggest(d, "recieve", 7, 5, &l, NULL) == PC_OK);
    for (i = 0; i < l.n; i++) {
        found = found || strcmp(l.v[i], "receive") == 0;
    }
    CHECK(found);
    pc_spell_list_fini(&l);
    CHECK(pc_spell_suggest(d, "Wrold", 5, 1, &l, NULL) == PC_OK && l.n == 1 && strcmp(l.v[0], "World") == 0);
    pc_spell_list_fini(&l);
    CHECK(pc_spell_suggest(d, "brwon", 5, 1, &l, NULL) == PC_OK && l.n == 1 && strcmp(l.v[0], "brown") == 0);
    pc_spell_list_fini(&l);
    CHECK(pc_spell_suggest(d, "HELO", 4, 5, &l, NULL) == PC_OK && l.n > 0);
    for (i = 0; l.n > 0 && l.v[0][i] != '\0'; i++) {
        upper = upper && !(l.v[0][i] >= 'a' && l.v[0][i] <= 'z');
    }
    CHECK(upper);
    pc_spell_list_fini(&l);
}

static void test_misspellings_and_user_words(void)
{
    static const char list[] = "#10\nhello\nworld\nParis\n";
    static const char text[] = "Hello wrld, Paris paris photocraft 42x";
    static const char *const words[] = {"photocraft"};
    pc_spell_user user = {words, 1};
    pc_spell_dict *d = NULL;
    pc_spell_span *m = NULL;
    size_t n = 0;
    CHECK(pc_spell_from_list(list, sizeof list - 1, &d, NULL) == PC_OK);
    CHECK(pc_spell_misspellings(d, text, strlen(text), NULL, &m, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 3);
    if (n == 3) {
        CHECK(m[0].end - m[0].start == 4 && memcmp(text + m[0].start, "wrld", 4) == 0);
        CHECK(m[1].end - m[1].start == 5 && memcmp(text + m[1].start, "paris", 5) == 0);
        CHECK(m[2].end - m[2].start == 10 && memcmp(text + m[2].start, "photocraft", 10) == 0);
    }
    free(m);
    CHECK(pc_spell_misspellings(d, text, strlen(text), &user, &m, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 2);
    free(m);
    pc_spell_free(d);
}

static void test_tokenizer_and_distance(void)
{
    static const char t[] = "it's well-known 'quoted' café";
    static const char *const want[] = {"it's", "well", "known", "quoted", "café"};
    pc_spell_span *w = NULL;
    size_t n = 0, i;
    uint32_t teh[3] = {'t', 'e', 'h'}, the[3] = {'t', 'h', 'e'}, toe[3] = {'t', 'o', 'e'};
    uint32_t banana[6] = {'b', 'a', 'n', 'a', 'n', 'a'};
    CHECK(pc_spell_words(t, strlen(t), &w, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 5);
    for (i = 0; i < n && i < 5; i++) {
        CHECK(w[i].end - w[i].start == strlen(want[i]) &&
              memcmp(t + w[i].start, want[i], strlen(want[i])) == 0);
    }
    free(w);
    CHECK_EQ_UINT(pc_spell_distance(teh, 3, the, 3, 2), 1);
    CHECK_EQ_UINT(pc_spell_distance(teh, 3, toe, 3, 2), 2);
    CHECK(pc_spell_distance(teh, 3, banana, 6, 2) == SIZE_MAX);
}

static void test_spell_hostile_input(void)
{
    const pc_spell_dict *d = pc_spell_english();
    pc_spell_list l = {NULL, 0};
    pc_spell_span *w = NULL;
    size_t n = 0, i;
    char big[4096];
    static const char bad_utf8[] = "ab\xff\xfe\xc3 caf\xc3";
    for (i = 0; i < sizeof big - 1; i++) {
        big[i] = (char)('a' + (char)(i % 26));
    }
    big[sizeof big - 1] = '\0';
    CHECK(!pc_spell_check(d, big, strlen(big), NULL));
    CHECK(pc_spell_suggest(d, big, strlen(big), 5, &l, NULL) == PC_OK);
    pc_spell_list_fini(&l);
    (void)pc_spell_check(d, bad_utf8, sizeof bad_utf8 - 1, NULL);
    CHECK(pc_spell_suggest(d, bad_utf8, sizeof bad_utf8 - 1, 3, &l, NULL) == PC_OK);
    pc_spell_list_fini(&l);
    CHECK(pc_spell_words(bad_utf8, sizeof bad_utf8 - 1, &w, &n, NULL) == PC_OK);
    free(w);
    CHECK(pc_spell_check(d, NULL, 0, NULL));
    CHECK(pc_spell_suggest(d, "", 0, 0, &l, NULL) == PC_OK && l.n == 0);
    pc_spell_list_fini(&l);
    CHECK(pc_spell_suggest(NULL, "x", 1, 1, &l, NULL) == PC_ERR_INVALID);
    {
        pc_spell_dict *e = NULL;
        CHECK(pc_spell_from_list("", 0, &e, NULL) == PC_OK && pc_spell_len(e) == 0);
        CHECK(pc_spell_check(e, "word", 4, NULL) == false);
        CHECK(pc_spell_suggest(e, "word", 4, 3, &l, NULL) == PC_OK && l.n == 0);
        pc_spell_list_fini(&l);
        pc_spell_free(e);
        CHECK(pc_spell_from_list("#300\n#+5\nw\n\r\n#\n", 15, &e, NULL) == PC_OK && pc_spell_len(e) == 1);
        pc_spell_free(e);
    }
    pc_spell_free((pc_spell_dict *)NULL);
}

/* ---- warp.rs ---- */

static pc_style_warp warp_of(const char *style, float value)
{
    pc_text_warp w;
    pc_style_warp sw;
    float b[4] = {0.0f, -40.0f, 200.0f, 10.0f};
    const char *psd = pc_text_warp_psd_style(style, strlen(style));
    memset(&w, 0, sizeof w);
    memset(&sw, 0, sizeof sw);
    (void)pc_str_set_c(&w.style, psd != NULL ? psd : "", NULL);
    w.value = value;
    w.horizontal = true;
    CHECK(pc_text_warp_prepare(&w, b, &sw));
    pc_str_fini(&w.style);
    return sw;
}

static void test_styles_resolve_and_none_is_identity(void)
{
    pc_text_warp none, flat;
    float b[4] = {0.0f, 0.0f, 10.0f, 10.0f};
    pc_style_warp sw;
    CHECK(strcmp(pc_text_warp_psd_style("Arc", 3), "warpArc") == 0);
    CHECK(strcmp(pc_text_warp_psd_style("warpTwist", 9), "warpTwist") == 0);
    CHECK(strcmp(pc_text_warp_psd_style("none", 4), "warpNone") == 0);
    CHECK(pc_text_warp_psd_style("spiral", 6) == NULL);
    memset(&none, 0, sizeof none);
    (void)pc_str_set_c(&none.style, "warpNone", NULL);
    none.value = 50.0f;
    CHECK(!pc_text_warp_prepare(&none, b, &sw));
    memset(&flat, 0, sizeof flat);
    (void)pc_str_set_c(&flat.style, "warpArc", NULL);
    CHECK(!pc_text_warp_prepare(&flat, b, &sw));
    pc_str_fini(&none.style);
    pc_str_fini(&flat.style);
}

static void test_every_style_is_continuous_and_fixes_the_centre_column(void)
{
    size_t i;
    for (i = 0; i < 15; i++) {
        const char *id = PC_TEXT_WARP_STYLES[i][1];
        pc_style_warp w = warp_of(id, 50.0f), t = warp_of(id, 0.5f);
        pc_point p = pc_style_warp_apply(&w, 100.0, -15.0), q = pc_style_warp_apply(&t, 20.0, 0.0);
        CHECK(isfinite(p.x) && isfinite(p.y));
        if (!(fabs(q.x - 20.0) < 2.0 && fabs(q.y) < 2.0)) {
            PC_T_FAIL("%s: %g,%g", id, q.x, q.y);
        }
    }
}

static void test_arc_raises_the_middle_relative_to_the_ends(void)
{
    pc_style_warp w = warp_of("arc", 50.0f), s = warp_of("arc", -50.0f);
    pc_point mid = pc_style_warp_apply(&w, 100.0, -15.0), end = pc_style_warp_apply(&w, 0.0, -15.0);
    CHECK(fabs(mid.y - -15.0) < 1e-9);
    CHECK(end.y > mid.y + 5.0);
    CHECK(end.x > 0.0);
    CHECK(pc_style_warp_apply(&s, 0.0, -15.0).y < -20.0);
}

static void test_bulge_and_squeeze_change_height_in_the_middle(void)
{
    pc_style_warp b = warp_of("bulge", 50.0f), s = warp_of("squeeze", 50.0f);
    CHECK(pc_style_warp_apply(&b, 100.0, -40.0).y < -50.0);
    CHECK(pc_style_warp_apply(&s, 100.0, -40.0).y > -40.0);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_bundled_dictionary_loads);
    RUN(test_suggestions_rank_by_distance_then_frequency);
    RUN(test_misspellings_and_user_words);
    RUN(test_tokenizer_and_distance);
    RUN(test_spell_hostile_input);
    RUN(test_styles_resolve_and_none_is_identity);
    RUN(test_every_style_is_continuous_and_fixes_the_centre_column);
    RUN(test_arc_raises_the_middle_relative_to_the_ends);
    RUN(test_bulge_and_squeeze_change_height_in_the_middle);
    return pc_t_finish();
}
