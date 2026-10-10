/* Font database, OpenType readers, CJK fallback and craft fonts (ports of the #[test]s in
 * crates/text/src/fonts.rs, craft_fonts.rs, glyphs.rs (charmap/categories) and cjk.rs, plus
 * never-crash tests on damaged font data). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/file.h"
#include "base/utf8.h"
#include "harness.h"
#include "text/text.h"

static pc_blob *bundled(const char *name)
{
    size_t n, i;
    const pc_bundled_font *b = pc_text_bundled_fonts(&n);
    for (i = 0; i < n; i++) {
        if (strcmp(b[i].file, name) == 0) {
            pc_blob *o = NULL;
            CHECK(pc_blob_new(b[i].data, b[i].len, &o, NULL) == PC_OK);
            return o;
        }
    }
    return NULL;
}

static bool contains_cp(const uint32_t *v, size_t n, uint32_t c)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (v[i] == c) {
            return true;
        }
    }
    return false;
}

/* ---- fonts.rs ---- */

static void test_hidden_families_sort_last_and_are_not_listed(void)
{
    CHECK(pc_text_is_hidden_family(".Hiragino Kaku Gothic Interface", 31));
    CHECK(!pc_text_is_hidden_family("Hiragino Sans", 13));
    CHECK(!pc_text_is_hidden_family("", 0));
}

static void test_scans_flatpak_host_fonts(void)
{
#if defined(__linux__)
    pc_strlist d = {NULL, 0};
    size_t i;
    bool host = false, user = false;
    CHECK(pc_text_system_font_dirs(&d, NULL) == PC_OK);
    for (i = 0; i < d.n; i++) {
        host = host || strcmp(d.v[i], "/run/host/fonts") == 0;
        user = user || strcmp(d.v[i], "/run/host/user-fonts") == 0;
    }
    CHECK(host && user);
    pc_strlist_fini(&d);
#endif
}

/* Every character of letters_utf8 is mapped by every bundled font. */
static void check_cover(const char *letters_utf8)
{
    size_t n, i;
    const pc_bundled_font *b = pc_text_bundled_fonts(&n);
    for (i = 0; i < n; i++) {
        pc_sfnt sf;
        pc_sfnt_cmap cm;
        size_t k = 0, len = strlen(letters_utf8);
        CHECK(pc_sfnt_open(b[i].data, b[i].len, 0, &sf));
        pc_sfnt_cmap_skrifa(&sf, &cm);
        while (k < len) {
            uint32_t cp;
            if (!pc_utf8_next((const uint8_t *)letters_utf8, len, &k, &cp)) {
                PC_T_FAIL("bad UTF-8 in the test");
                break;
            }
            if (pc_sfnt_cmap_map(&cm, cp) == 0) {
                PC_T_FAIL("%s lacks U+%04X", b[i].file, (unsigned)cp);
            }
        }
    }
}

/* The bundled UI fonts cover Czech (the `cs` UI language) on their own. */
static void test_bundled_fonts_cover_czech(void)
{
    check_cover("aábcčdďeéěfghiíjklmnňoópqrřsštťuúůvwxyýzž„“"
                "AÁBCČDĎEÉĚFGHIÍJKLMNŇOÓPQRŘSŠTŤUÚŮVWXYÝZŽ");
}

/* French: accented letters, ligatures, guillemets and the no-break space. */
static void test_bundled_fonts_cover_french(void)
{
    check_cover("àâæçéèêëîïôœùûüÿÀÂÆÇÉÈÊËÎÏÔŒÙÛÜŸ«»\xc2\xa0’");
}

static void test_missing_font_dirs_are_skipped(void)
{
    pc_strlist files = {NULL, 0};
    CHECK(pc_text_collect_font_files("/nonexistent/photocraft/fonts", &files, NULL) == PC_OK);
    CHECK_EQ_UINT(files.n, 0);
    pc_strlist_fini(&files);
}

/* ---- glyphs.rs ---- */

static void test_bundled_charmap_and_categories(void)
{
    pc_fontdb *db = NULL;
    uint32_t *cm = NULL;
    size_t n = 0, i, latin = 0;
    CHECK(pc_fontdb_new(&db, NULL) == PC_OK);
    CHECK(pc_fontdb_charmap(db, PC_TEXT_DEFAULT_FAMILY, 5, 400, false, &cm, &n, NULL) == PC_OK);
    CHECK(n > 500);
    CHECK(contains_cp(cm, n, 'A') && contains_cp(cm, n, 0x20AC) && contains_cp(cm, n, ' '));
    CHECK(!contains_cp(cm, n, '\n'));
    for (i = 0; i < n; i++) {
        latin += pc_glyph_in_category(cm[i], "Basic Latin") ? 1 : 0;
    }
    CHECK_EQ_UINT(latin, 95);
    free(cm);
    cm = NULL;
    CHECK(pc_fontdb_charmap(db, "No Such Family", 14, 400, false, &cm, &n, NULL) == PC_OK);
    CHECK(cm == NULL && n == 0);
    CHECK(pc_glyph_in_category(0x20AC, "Currency") && pc_glyph_in_category(0x2014, "Punctuation") &&
          pc_glyph_in_category(0x2192, "Arrows"));
    CHECK(!pc_glyph_in_category('A', "Punctuation") && pc_glyph_in_category('A', "Entire Font"));
    CHECK(pc_glyph_in_category('A', NULL)); /* unknown/absent category matches everything */
    pc_fontdb_free(db);
}

/* ---- craft_fonts.rs ---- */

static void test_mincho_first_reorders_only_craft_families(void)
{
    /* The reference skips without craft fonts; the reorder itself only needs the list. */
    const char *craft[2] = {"BIZ UDPGothic", "Shippori Mincho"};
    const char *fb[4] = {"Noto Sans", "BIZ UDPGothic", "Shippori Mincho", "Hiragino Sans"};
    pc_craft_mincho_first(fb, 4, craft, 2);
    CHECK_EQ_STR(fb[0], "Noto Sans");
    CHECK_EQ_STR(fb[3], "Hiragino Sans");
    CHECK_EQ_STR(fb[1], "Shippori Mincho");
    CHECK_EQ_STR(fb[2], "BIZ UDPGothic");
}

static void test_mincho_first_without_craft_fonts_is_identity(void)
{
    const char *fb[2] = {"Noto Sans", "Hiragino Sans"};
    pc_craft_mincho_first(fb, 2, NULL, 0);
    CHECK_EQ_STR(fb[0], "Noto Sans");
    CHECK_EQ_STR(fb[1], "Hiragino Sans");
}

static void test_serif_names(void)
{
    static const char *const yes[] = {"Shippori Mincho", "Times New Roman", "Noto Serif JP",
                                      "Georgia",         "Songti SC",       "Adobe Garamond Pro"};
    static const char *const no[] = {"Inter", "Noto Sans Serif", "BIZ UDPGothic", "Helvetica",
                                     "",      "Noto Sans CJK JP"};
    size_t i;
    for (i = 0; i < 6; i++) {
        CHECK(pc_craft_is_serif_family(yes[i], strlen(yes[i])));
        CHECK(!pc_craft_is_serif_family(no[i], strlen(no[i])));
    }
}

/* A fake craft-fonts checkout (bundled fonts standing in for the Japanese ones). */
static bool write_file(const char *path, const void *data, size_t len)
{
    return pc_file_write_atomic(path, data, len, NULL) == PC_OK;
}

static void test_ui_order_prefers_biz_udpgothic_regular(void)
{
    char dir[] = "/tmp/pc-craft-XXXXXX";
    char path[256];
    pc_craft_font *f = NULL;
    const pc_craft_font **ui = NULL;
    const char **fams = NULL;
    size_t n = 0, nui = 0, nf = 0, i;
    pc_blob *inter = bundled("Inter-Regular.ttf");
    pc_error e;
    const char *manifest = "# craft fonts\n"
                           "\n"
                           "Shippori Mincho | Regular | fonts/a.ttf | Jpan\n"
                           "BIZ UDPGothic | Bold | fonts/a.ttf | Jpan, Latn\n"
                           "Other Sans | Regular | fonts/a.ttf | Latn\n"
                           "BIZ UDPGothic | Regular | fonts/a.ttf | Jpan | extra\r\n";
    if (mkdtemp(dir) == NULL) {
        PC_T_FAIL("mkdtemp");
        return;
    }
    snprintf(path, sizeof path, "%s/fonts", dir);
    CHECK(pc_mkdir_p(path, NULL) == PC_OK);
    snprintf(path, sizeof path, "%s/fonts/a.ttf", dir);
    CHECK(write_file(path, pc_blob_data(inter), pc_blob_len(inter)));
    snprintf(path, sizeof path, "%s/fonts/manifest.txt", dir);
    CHECK(write_file(path, manifest, strlen(manifest)));
    CHECK(pc_craft_fonts_load_dir(dir, &f, &n, &e) == PC_OK);
    CHECK_EQ_UINT(n, 4);
    if (n == 4) {
        CHECK_EQ_STR(f[3].family, "BIZ UDPGothic");
        CHECK_EQ_STR(f[3].style, "Regular");
        CHECK_EQ_UINT(f[1].nscripts, 2);
        CHECK_EQ_STR(f[1].scripts[1], "Latn");
        CHECK(pc_craft_font_is_mincho(&f[0]) && !pc_craft_font_is_japanese(&f[2]));
        CHECK(pc_craft_japanese_for_ui(f, n, &ui, &nui, NULL) == PC_OK);
        CHECK_EQ_UINT(nui, 3);
        CHECK(nui > 0 && strcmp(ui[0]->family, PC_CRAFT_UI_JAPANESE_FAMILY) == 0 &&
              strcmp(ui[0]->style, "Regular") == 0);
        for (i = 0; i < nui; i++) {
            CHECK(pc_craft_font_is_japanese(ui[i]) && pc_blob_len(ui[i]->bytes) > 0);
        }
        CHECK(pc_craft_japanese_families(f, n, &fams, &nf, NULL) == PC_OK);
        CHECK_EQ_UINT(nf, 2);
        CHECK(nf == 2 && strcmp(fams[0], "BIZ UDPGothic") == 0 && strcmp(fams[1], "Shippori Mincho") == 0);
    }
    free(ui);
    free(fams);
    pc_craft_fonts_free(f, n);
    /* Broken checkouts: missing manifest, malformed line, missing font file. */
    {
        char d2[] = "/tmp/pc-craft-XXXXXX";
        CHECK(mkdtemp(d2) != NULL);
        f = NULL;
        CHECK(pc_craft_fonts_load_dir(d2, &f, &n, &e) != PC_OK);
        CHECK_CONTAINS(e.msg, "manifest.txt");
        snprintf(path, sizeof path, "%s/fonts", d2);
        pc_mkdir_p(path, NULL);
        snprintf(path, sizeof path, "%s/fonts/manifest.txt", d2);
        CHECK(write_file(path, "Only | Two\n", 11));
        CHECK(pc_craft_fonts_load_dir(d2, &f, &n, &e) == PC_ERR_FORMAT);
        CHECK_CONTAINS(e.msg, "malformed manifest line: Only | Two");
        CHECK(write_file(path, "A | B | fonts/missing.ttf | Jpan\n", 33));
        CHECK(pc_craft_fonts_load_dir(d2, &f, &n, &e) != PC_OK);
        CHECK_CONTAINS(e.msg, "fonts/missing.ttf");
        unlink(path);
        snprintf(path, sizeof path, "%s/fonts", d2);
        rmdir(path);
        rmdir(d2);
    }
    snprintf(path, sizeof path, "%s/fonts/a.ttf", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/fonts/manifest.txt", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/fonts", dir);
    rmdir(path);
    rmdir(dir);
    pc_blob_release(inter);
    /* The process-wide list works whether or not a checkout is configured. */
    (void)pc_craft_fonts(&n);
    CHECK(pc_craft_fonts_error() != NULL);
}

/* ---- cjk.rs ---- */

static void test_locale_orders(void)
{
    pc_cjk_script o[4];
    static const char *const non_cjk[] = {NULL, "", "en_US.UTF-8", "C", "de", "!!@@"};
    static const char *const any[] = {"ja", "ko", "zh-TW", "zh", "en"};
    size_t i, k;
    pc_cjk_script_order("ja_JP.UTF-8", o);
    CHECK(o[0] == PC_CJK_JAPANESE);
    pc_cjk_script_order("ja", o);
    CHECK(o[0] == PC_CJK_JAPANESE);
    pc_cjk_script_order("zh_CN.UTF-8", o);
    CHECK(o[0] == PC_CJK_SIMPLIFIED_CHINESE);
    pc_cjk_script_order("zh-Hans-CN", o);
    CHECK(o[0] == PC_CJK_SIMPLIFIED_CHINESE);
    pc_cjk_script_order("zh-Hant-TW", o);
    CHECK(o[0] == PC_CJK_TRADITIONAL_CHINESE);
    pc_cjk_script_order("zh_TW", o);
    CHECK(o[0] == PC_CJK_TRADITIONAL_CHINESE);
    pc_cjk_script_order("zh-HK", o);
    CHECK(o[0] == PC_CJK_TRADITIONAL_CHINESE);
    pc_cjk_script_order("zh-Hans-HK", o);
    CHECK(o[0] == PC_CJK_SIMPLIFIED_CHINESE);
    pc_cjk_script_order("ko-KR", o);
    CHECK(o[0] == PC_CJK_KOREAN);
    for (i = 0; i < 6; i++) {
        pc_cjk_script_order(non_cjk[i], o);
        CHECK(o[0] == PC_CJK_SIMPLIFIED_CHINESE);
    }
    for (i = 0; i < 5; i++) {
        unsigned seen = 0;
        pc_cjk_script_order(any[i], o);
        for (k = 0; k < 4; k++) {
            seen |= 1u << o[k];
        }
        CHECK_EQ_UINT(seen, 15);
    }
}

static long pos_of(const char **v, size_t n, const char *f)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (strcmp(v[i], f) == 0) {
            return (long)i;
        }
    }
    PC_T_FAIL("%s not in the fallback list", f);
    return -1;
}

static void test_type_tool_fallback_follows_locale(void)
{
    static const char *const locs[4] = {"ja_JP", "zh_CN", "zh-Hant", "ko"};
    const char **v[4];
    size_t n[4], i;
    for (i = 0; i < 4; i++) {
        pc_cjk_script o[4];
        pc_cjk_script_order(locs[i], o);
        CHECK(pc_text_fallback_candidates(o, &v[i], &n[i], NULL) == PC_OK);
    }
    CHECK(pos_of(v[0], n[0], "Hiragino Sans") < pos_of(v[0], n[0], "PingFang SC"));
    CHECK(pos_of(v[0], n[0], "Noto Sans CJK JP") < pos_of(v[0], n[0], "Noto Sans CJK SC"));
    CHECK(pos_of(v[1], n[1], "PingFang SC") < pos_of(v[1], n[1], "Hiragino Sans"));
    CHECK(pos_of(v[1], n[1], "Microsoft YaHei") < pos_of(v[1], n[1], "Microsoft JhengHei"));
    CHECK(pos_of(v[2], n[2], "PingFang TC") < pos_of(v[2], n[2], "PingFang SC"));
    CHECK(pos_of(v[3], n[3], "Apple SD Gothic Neo") < pos_of(v[3], n[3], "PingFang SC"));
    CHECK(pos_of(v[3], n[3], "Malgun Gothic") < pos_of(v[3], n[3], "Yu Gothic"));
    for (i = 0; i < 4; i++) {
        CHECK(pos_of(v[i], n[i], "Arial Unicode MS") > pos_of(v[i], n[i], "Noto Sans CJK KR"));
        CHECK(pos_of(v[i], n[i], "Noto Sans") < pos_of(v[i], n[i], "Hiragino Sans"));
        free(v[i]);
    }
}

static void test_classify_chars(void)
{
    pc_cjk_script o[4];
    pc_cjk_script_order("en", o);
    CHECK(pc_cjk_classify('a') == PC_CJK_NONE);
    CHECK(pc_cjk_classify(0xE9) == PC_CJK_NONE);
    CHECK(pc_cjk_classify(0x56FE) == PC_CJK_HAN);
    CHECK(pc_cjk_classify(0x5716) == PC_CJK_HAN);
    CHECK(pc_cjk_classify(0x30EC) == PC_CJK_KANA);
    CHECK(pc_cjk_classify(0x30FC) == PC_CJK_KANA);
    CHECK(pc_cjk_classify(0xCE74) == PC_CJK_HANGUL);
    CHECK(pc_cjk_classify(0x3105) == PC_CJK_BOPOMOFO);
    CHECK(pc_cjk_preferred(PC_CJK_KANA, o) == PC_CJK_JAPANESE);
    CHECK(pc_cjk_preferred(PC_CJK_HANGUL, o) == PC_CJK_KOREAN);
    CHECK(pc_cjk_preferred(PC_CJK_HAN, o) == PC_CJK_SIMPLIFIED_CHINESE);
    pc_cjk_script_order("ja", o);
    CHECK(pc_cjk_preferred(PC_CJK_HAN, o) == PC_CJK_JAPANESE);
}

/* plutil -convert binary1 of a dict with AKLastLocale, AppleLanguages [zh-Hant-TW, en-US] and
 * AppleLocale (the reference's SAMPLE). */
static const uint8_t SAMPLE[] = {
    0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30, 0xd3, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x5b,
    0x41, 0x70, 0x70, 0x6c, 0x65, 0x4c, 0x6f, 0x63, 0x61, 0x6c, 0x65, 0x5c, 0x41, 0x4b, 0x4c, 0x61,
    0x73, 0x74, 0x4c, 0x6f, 0x63, 0x61, 0x6c, 0x65, 0x5e, 0x41, 0x70, 0x70, 0x6c, 0x65, 0x4c, 0x61,
    0x6e, 0x67, 0x75, 0x61, 0x67, 0x65, 0x73, 0x55, 0x7a, 0x68, 0x5f, 0x54, 0x57, 0x55, 0x65, 0x6e,
    0x5f, 0x55, 0x53, 0xa2, 0x07, 0x08, 0x5a, 0x7a, 0x68, 0x2d, 0x48, 0x61, 0x6e, 0x74, 0x2d, 0x54,
    0x57, 0x55, 0x65, 0x6e, 0x2d, 0x55, 0x53, 0x08, 0x0f, 0x1b, 0x28, 0x37, 0x3d, 0x43, 0x46, 0x51,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x57,
};

static void test_reads_apple_languages_and_rejects_garbage(void)
{
    pc_buf b = PC_BUF_INIT;
    uint8_t ff[64], m[sizeof SAMPLE];
    pc_cjk_script o[4];
    size_t i, k;
    static const uint8_t VALS[] = {0x00, 0x0F, 0x5F, 0xAF, 0xDF, 0xFF};
    CHECK(pc_cjk_bplist_first_language(SAMPLE, sizeof SAMPLE, &b));
    CHECK_EQ_STR(pc_buf_cstr(&b), "zh-Hant-TW");
    pc_cjk_script_order(pc_buf_cstr(&b), o);
    CHECK(o[0] == PC_CJK_TRADITIONAL_CHINESE);
    pc_buf_truncate(&b, 0);
    CHECK(!pc_cjk_bplist_first_language((const uint8_t *)"", 0, &b));
    CHECK(!pc_cjk_bplist_first_language((const uint8_t *)"bplist00", 8, &b));
    memset(ff, 0xFF, sizeof ff);
    CHECK(!pc_cjk_bplist_first_language(ff, sizeof ff, &b));
    /* Every truncation and every single-byte corruption fails gracefully. */
    for (i = 0; i < sizeof SAMPLE; i++) {
        (void)pc_cjk_bplist_first_language(SAMPLE, i, &b);
    }
    for (i = 0; i < sizeof SAMPLE; i++) {
        for (k = 0; k < sizeof VALS; k++) {
            memcpy(m, SAMPLE, sizeof SAMPLE);
            m[i] = VALS[k];
            (void)pc_cjk_bplist_first_language(m, sizeof m, &b);
        }
    }
    pc_buf_fini(&b);
}

static void test_face_index_handles_bad_data(void)
{
    pc_blob *inter = bundled("Inter-Regular.ttf");
    CHECK_EQ_UINT(pc_cjk_face_index_for_family((const uint8_t *)"", 0, "PingFang SC"), 0);
    CHECK_EQ_UINT(pc_cjk_face_index_for_family((const uint8_t *)"ttcf\0\0\0\0garbage", 15, "PingFang SC"), 0);
    CHECK_EQ_UINT(pc_cjk_face_index_for_family(pc_blob_data(inter), pc_blob_len(inter), "Inter"), 0);
    CHECK_EQ_UINT(pc_cjk_face_index_for_family(pc_blob_data(inter), pc_blob_len(inter), ""), 0);
    CHECK_EQ_UINT(pc_cjk_face_index_for_family(NULL, 0, NULL), 0);
    pc_blob_release(inter);
}

static void test_cjk_discovery_includes_host_fonts(void)
{
#if defined(__linux__)
    pc_cjk_script s;
    for (s = PC_CJK_JAPANESE; s <= PC_CJK_KOREAN; s++) {
        pc_cjk_font_file *f = NULL;
        size_t n = 0, i;
        CHECK(pc_cjk_font_files(s, &f, &n, NULL) == PC_OK);
        CHECK(n >= 2);
        if (n >= 2) {
            CHECK_EQ_STR(f[0].path, "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
            CHECK_EQ_STR(f[1].path, "/run/host/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
            CHECK(strncmp(f[1].family, "Noto Sans CJK", 13) == 0);
        }
        for (i = 0; i + 1 < n; i++) {
            if (strncmp(f[i].path, "/usr/share/fonts/", 17) == 0) {
                char want[512];
                snprintf(want, sizeof want, "/run/host/fonts/%s", f[i].path + 17);
                CHECK_EQ_STR(f[i + 1].path, want);
                CHECK_EQ_STR(f[i + 1].family, f[i].family);
            }
        }
        pc_cjk_font_files_free(f, n);
    }
    {
        pc_cjk_font_file *f = NULL;
        size_t n = 0, i;
        bool found = false;
        CHECK(pc_cjk_last_resort_files(&f, &n, NULL) == PC_OK);
        for (i = 0; i < n; i++) {
            found = found ||
                    (strcmp(f[i].path, "/run/host/fonts/truetype/droid/DroidSansFallbackFull.ttf") == 0 &&
                     f[i].family[0] == '\0');
        }
        CHECK(found);
        pc_cjk_font_files_free(f, n);
    }
#endif
}

/* ---- matching and the database ---- */

static void test_matching_and_synthesis(void)
{
    pc_fontdb *db = NULL;
    pc_font_match m;
    pc_font_query q = {1.0f, PC_SLOPE_NORMAL, false, 0.0f, 400.0f};
    CHECK(pc_fontdb_new(&db, NULL) == PC_OK);
    CHECK(pc_fontdb_match(db, "inter", 5, &q, true, &m, NULL) == PC_OK);
    CHECK_EQ_STR(m.family, "Inter");
    CHECK(m.weight == 400.0f && !m.embolden && m.skew_deg == 0.0f);
    pc_font_release(m.font);
    q.weight = 550.0f; /* above 500: ascending from 550 -> 600 */
    CHECK(pc_fontdb_match(db, "Inter", 5, &q, true, &m, NULL) == PC_OK);
    CHECK(m.weight == 600.0f && !m.embolden);
    pc_font_release(m.font);
    q.weight = 450.0f; /* 400..500: ascending up to 500 */
    CHECK(pc_fontdb_match(db, "Inter", 5, &q, true, &m, NULL) == PC_OK);
    CHECK(m.weight == 500.0f);
    pc_font_release(m.font);
    q.weight = 900.0f;
    q.slope = PC_SLOPE_ITALIC;
    CHECK(pc_fontdb_match(db, "Inter", 5, &q, true, &m, NULL) == PC_OK);
    CHECK(m.weight == 600.0f && m.embolden && m.skew_deg == 14.0f);
    pc_font_release(m.font);
    q.weight = 100.0f;
    q.slope = PC_SLOPE_OBLIQUE;
    q.has_angle = true;
    q.angle = 20.0f;
    CHECK(pc_fontdb_match(db, "Inter", 5, &q, true, &m, NULL) == PC_OK);
    CHECK(m.weight == 400.0f && !m.embolden && m.skew_deg == 20.0f);
    pc_font_release(m.font);
    CHECK(pc_fontdb_match(db, "No Such", 7, &q, true, &m, NULL) == PC_ERR_NOT_FOUND);
    CHECK(pc_fontdb_match(db, "", 0, &q, true, &m, NULL) == PC_ERR_NOT_FOUND);
    CHECK_EQ_STR(pc_fontdb_generic(db, "monospace"), PC_TEXT_MONO_FAMILY);
    CHECK_EQ_STR(pc_fontdb_generic(db, "serif"), PC_TEXT_DEFAULT_FAMILY);
    CHECK(pc_fontdb_generic(db, "cursive") == NULL);
    CHECK_EQ_STR(pc_fontdb_fallback(db, 0), PC_TEXT_DEFAULT_FAMILY);
    CHECK(pc_fontdb_fallback(db, pc_fontdb_fallback_count(db)) == NULL);
    CHECK(pc_fontdb_selected_face_has_feature(db, "Inter", 5, 400, false, PC_SFNT_TAG('l', 'i', 'g', 'a')) ||
          !pc_fontdb_selected_face_has_feature(db, "Inter", 5, 400, false, PC_SFNT_TAG('x', 'x', 'x', 'x')));
    CHECK(!pc_fontdb_selected_face_has_feature(db, "", 0, 400, false, PC_SFNT_TAG('x', 'x', 'x', 'x')));
    pc_fontdb_free(db);
}

static void test_collection_faces(void)
{
    /* A two-face collection: each face registers into its own family and draws like the TTF. */
    pc_blob *a = bundled("Inter-Regular.ttf"), *b = bundled("JetBrainsMono-Regular.ttf");
    size_t head = 20, oa = 20, ob = (oa + pc_blob_len(a) + 3) & ~(size_t)3, total = ob + pc_blob_len(b), k;
    uint8_t *ttc = calloc(1, total);
    pc_fontdb *db = NULL;
    pc_strlist names = {NULL, 0};
    pc_blob *tb = NULL;
    pc_font *f1 = NULL, *mono = NULL;
    (void)head;
    memcpy(ttc, "ttcf\0\1\0\0\0\0\0\2", 12);
    ttc[12 + 0] = (uint8_t)(oa >> 24);
    ttc[13] = (uint8_t)(oa >> 16);
    ttc[14] = (uint8_t)(oa >> 8);
    ttc[15] = (uint8_t)oa;
    ttc[16] = (uint8_t)(ob >> 24);
    ttc[17] = (uint8_t)(ob >> 16);
    ttc[18] = (uint8_t)(ob >> 8);
    ttc[19] = (uint8_t)ob;
    memcpy(ttc + oa, pc_blob_data(a), pc_blob_len(a));
    memcpy(ttc + ob, pc_blob_data(b), pc_blob_len(b));
    for (k = 0; k < 2; k++) {
        size_t base = k == 0 ? oa : ob, t, nt = (size_t)((ttc[base + 4] << 8) | ttc[base + 5]);
        for (t = 0; t < nt; t++) {
            uint8_t *p = ttc + base + 12 + 16 * t + 8;
            uint32_t o =
                ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]) + (uint32_t)base;
            p[0] = (uint8_t)(o >> 24);
            p[1] = (uint8_t)(o >> 16);
            p[2] = (uint8_t)(o >> 8);
            p[3] = (uint8_t)o;
        }
    }
    CHECK_EQ_UINT(pc_text_face_count(ttc, total), 2);
    CHECK(pc_fontdb_new(&db, NULL) == PC_OK);
    CHECK(pc_fontdb_register_font_data(db, ttc, total, &names, NULL) == PC_OK);
    CHECK_EQ_UINT(names.n, 2);
    CHECK_EQ_UINT(pc_cjk_face_index_for_family(ttc, total, "JetBrains Mono"), 1);
    CHECK(pc_blob_new(ttc, total, &tb, NULL) == PC_OK);
    CHECK(pc_font_new(tb, 1, &f1, NULL) == PC_OK);
    CHECK(pc_font_new(b, 0, &mono, NULL) == PC_OK);
    CHECK(pc_font_new(tb, 2, &mono, NULL) == PC_ERR_FORMAT); /* no third face; mono unchanged */
    if (f1 != NULL && mono != NULL) {
        /* Same outline through the collection and the plain file. */
        pc_text_layout la, lb;
        pc_char_style st;
        pc_placed_glyph g = {0, 70, 0, 0, 0, PC_GLYPH_HORIZONTAL};
        pc_glyph_outline *oa1 = NULL, *ob1 = NULL;
        size_t na = 0, nb = 0;
        pc_affine id = PC_AFFINE_IDENTITY;
        pc_char_style_default(&st);
        pc_text_layout_init(&la);
        pc_text_layout_init(&lb);
        pc_text_layout_add_face(&la, f1, NULL, 0, 33.0f, false, 0.0f, NULL, NULL);
        pc_text_layout_add_face(&lb, mono, NULL, 0, 33.0f, false, 0.0f, NULL, NULL);
        la.glyphs = lb.glyphs = &g;
        la.nglyphs = lb.nglyphs = 1;
        la.styles = lb.styles = &st;
        la.nstyles = lb.nstyles = 1;
        CHECK(pc_text_outlines(&la, &id, NULL, &oa1, &na, NULL) == PC_OK);
        CHECK(pc_text_outlines(&lb, &id, NULL, &ob1, &nb, NULL) == PC_OK);
        CHECK(na == 1 && nb == 1 && oa1[0].n == ob1[0].n && oa1[0].n > 0 &&
              memcmp(oa1[0].els, ob1[0].els, oa1[0].n * sizeof *oa1[0].els) == 0);
        pc_glyph_outlines_free(oa1, na);
        pc_glyph_outlines_free(ob1, nb);
        la.glyphs = lb.glyphs = NULL;
        la.nglyphs = lb.nglyphs = 0;
        la.styles = lb.styles = NULL;
        la.nstyles = lb.nstyles = 0;
        pc_text_layout_fini(&la);
        pc_text_layout_fini(&lb);
    }
    pc_font_release(f1);
    pc_font_release(mono);
    pc_strlist_fini(&names);
    pc_fontdb_free(db);
    pc_blob_release(tb);
    pc_blob_release(a);
    pc_blob_release(b);
    free(ttc);
}

/* ---- never crash ---- */

static uint64_t rng = 0x243F6A8885A308D3ull;
static uint32_t next_rand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 32);
}

/* Everything that reads font data, on bytes that may be garbage. */
static void exercise(const uint8_t *d, size_t n)
{
    pc_fontdb *db = NULL;
    pc_strlist names = {NULL, 0};
    uint32_t faces = pc_text_face_count(d, n), i;
    pc_blob *b = NULL;
    if (pc_fontdb_new(&db, NULL) != PC_OK) {
        return;
    }
    (void)pc_fontdb_register_font_data(db, d, n, &names, NULL);
    for (i = 0; i < names.n; i++) {
        uint32_t *cm = NULL;
        size_t ncm = 0;
        pc_face_info *fi = NULL;
        size_t nfi = 0;
        pc_font_match m;
        pc_font_query q = {1.0f, PC_SLOPE_ITALIC, false, 0.0f, 700.0f};
        (void)pc_fontdb_charmap(db, names.v[i], strlen(names.v[i]), 400, false, &cm, &ncm, NULL);
        free(cm);
        (void)pc_fontdb_faces(db, names.v[i], strlen(names.v[i]), &fi, &nfi, NULL);
        pc_face_infos_free(fi, nfi);
        if (pc_fontdb_match(db, names.v[i], strlen(names.v[i]), &q, true, &m, NULL) == PC_OK) {
            pc_font_release(m.font);
        }
        (void)pc_fontdb_selected_face_has_feature(db, names.v[i], strlen(names.v[i]), 400, true,
                                                  PC_SFNT_TAG('s', 'm', 'c', 'p'));
    }
    {
        pc_resolved_font r;
        if (pc_fontdb_resolve_postscript(db, "Inter-Regular", 13, &r, NULL) == PC_OK) {
            pc_resolved_font_fini(&r);
        }
    }
    if (pc_blob_new(d, n, &b, NULL) == PC_OK) {
        for (i = 0; i < faces && i < 3; i++) {
            pc_font *f = NULL;
            if (pc_font_new(b, i, &f, NULL) == PC_OK) {
                uint32_t g;
                float adv;
                for (g = 0; g < 40; g++) {
                    pc_bounds bb = {false, {0, 0, 0, 0}};
                    pc_line_sink s = pc_bounds_sink(&bb);
                    pc_flatten_pen p;
                    pc_outline_pen op;
                    float sz = 12.0f;
                    pc_flatten_pen_init(&p, &s, PC_XFORM_IDENTITY);
                    op = pc_flatten_pen_outline(&p);
                    (void)pc_font_draw(f, g, &sz, NULL, 0, &op, NULL);
                    (void)pc_font_draw(f, g, NULL, NULL, 0, &op, NULL);
                    (void)pc_font_advance(f, g, NULL, 0, &adv);
                }
                pc_font_release(f);
            }
        }
        pc_blob_release(b);
    }
    {
        pc_sfnt sf;
        pc_font_attrs at;
        pc_font_axis ax[4];
        pc_buf nm = PC_BUF_INIT;
        if (pc_sfnt_open(d, n, 0, &sf)) {
            pc_sfnt_cmap cm;
            pc_sfnt_attrs(&sf, &at);
            (void)pc_sfnt_axes(&sf, ax, 4);
            (void)pc_sfnt_name(&sf, 1, PC_SFNT_NAME_SKRIFA, &nm, NULL);
            (void)pc_sfnt_name(&sf, 6, PC_SFNT_NAME_FONTIQUE, &nm, NULL);
            pc_sfnt_cmap_skrifa(&sf, &cm);
            (void)pc_sfnt_cmap_map(&cm, 'A');
            if (pc_sfnt_cmap_fontique(&sf, &cm)) {
                (void)pc_sfnt_cmap_map_any(&cm, 0x41);
                (void)pc_sfnt_cmap_map_any(&cm, 0xE9);
            }
            (void)pc_sfnt_gsub_has_feature(&sf, PC_SFNT_TAG('l', 'i', 'g', 'a'));
        }
        pc_buf_fini(&nm);
    }
    pc_strlist_fini(&names);
    pc_fontdb_free(db);
}

static void test_damaged_fonts_never_crash(void)
{
    pc_blob *inter = bundled("Inter-Regular.ttf");
    const uint8_t *d = pc_blob_data(inter);
    size_t n = pc_blob_len(inter), cut, i;
    uint8_t *m = malloc(n);
    /* Truncations: the header and table directory byte by byte, then coarse steps. */
    for (cut = 0; cut < 400; cut += 7) {
        exercise(d, cut);
    }
    for (cut = 400; cut < n; cut += n / 23) {
        exercise(d, cut);
    }
    /* Byte flips: the table directory and random spots (seeded). */
    for (i = 0; i < 60; i++) {
        size_t at = i < 30 ? 12 + (size_t)next_rand() % 400 : (size_t)next_rand() % n;
        memcpy(m, d, n);
        m[at] = (uint8_t)next_rand();
        m[(at + 1) % n] = (uint8_t)next_rand();
        exercise(m, n);
    }
    /* Hostile directory: huge table count, tables pointing past the end, a TTC with absurd
     * face counts and offsets. */
    {
        uint8_t h[64] = {0, 1, 0, 0, 0xFF, 0xFF};
        uint8_t t[32] = {'t', 't', 'c', 'f', 0, 1, 0, 0, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0};
        uint8_t t2[24] = {'t', 't', 'c', 'f', 0, 1, 0, 0, 0, 0, 0, 2, 0, 0, 0, 20, 0xFF, 0xFF, 0xFF, 0x00};
        uint8_t dir[12 + 16] = {0,   1,   0, 0, 0, 1, 0,    0,    0,    0,    0,    0,    'n',  'a',
                                'm', 'e', 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xF0, 0xFF, 0xFF, 0xFF, 0xFF};
        exercise(h, sizeof h);
        exercise(t, sizeof t);
        exercise(t2, sizeof t2);
        exercise(dir, sizeof dir);
        CHECK_EQ_UINT(pc_text_face_count(t, sizeof t), 0);
        CHECK_EQ_UINT(pc_text_face_count(t2, sizeof t2), 2);
    }
    exercise(NULL, 0);
    free(m);
    pc_blob_release(inter);
}

static void test_missing_and_empty_inputs(void)
{
    pc_fontdb *db = NULL;
    pc_resolved_font r;
    pc_face_info *fi = (pc_face_info *)1;
    size_t n = 7;
    uint32_t *cm = (uint32_t *)1;
    pc_font *f = NULL;
    CHECK(pc_fontdb_new(&db, NULL) == PC_OK);
    CHECK(pc_fontdb_add_font_file(db, "/nonexistent/photocraft/font.ttf", NULL) == PC_OK);
    CHECK(pc_fontdb_add_font_file(db, "/", NULL) == PC_OK); /* a directory reads as an error: skipped */
    CHECK(pc_fontdb_faces(db, "", 0, &fi, &n, NULL) == PC_OK && fi == NULL && n == 0);
    CHECK(pc_fontdb_charmap(db, "", 0, 400, false, &cm, &n, NULL) == PC_OK && cm == NULL);
    CHECK(pc_fontdb_resolve_postscript(db, "", 0, &r, NULL) == PC_OK);
    pc_resolved_font_fini(&r);
    CHECK(pc_text_guess_from_postscript(NULL, 0, &r, NULL) == PC_OK);
    CHECK_EQ_STR(r.family, "");
    pc_resolved_font_fini(&r);
    CHECK(pc_fontdb_resolve_postscript(db, NULL, 3, &r, NULL) == PC_ERR_INVALID);
    CHECK(pc_fontdb_register_font_data(db, NULL, 5, NULL, NULL) == PC_ERR_INVALID);
    CHECK(pc_font_new(NULL, 0, &f, NULL) == PC_ERR_INVALID);
    CHECK(pc_fontdb_face_font(db, "Inter", 5, 99, NULL) == NULL);
    CHECK(pc_fontdb_face_font(db, "Inter", 5, 0, NULL) != NULL);
    pc_fontdb_free(db);
    pc_fontdb_free(NULL);
    pc_font_release(NULL);
}

static void test_system_fonts_load(void)
{
    /* Whatever is installed: scanning works, the bundled families stay, a second load is a
     * no-op. */
    pc_fontdb *db = NULL;
    pc_strlist f = {NULL, 0};
    size_t before;
    CHECK(pc_fontdb_with_system_fonts(&db, NULL) == PC_OK);
    CHECK(pc_fontdb_families(db, &f, NULL) == PC_OK);
    before = f.n;
    CHECK(pc_fontdb_has_family(db, "Inter", 5) && pc_fontdb_has_family(db, "jetbrains MONO", 14));
    pc_strlist_fini(&f);
    CHECK(pc_fontdb_load_system_fonts(db, NULL) == PC_OK);
    CHECK(pc_fontdb_families(db, &f, NULL) == PC_OK);
    CHECK_EQ_UINT(f.n, before);
    pc_strlist_fini(&f);
    pc_fontdb_free(db);
}

int main(int argc, char **argv)
{
    RUN(test_hidden_families_sort_last_and_are_not_listed);
    RUN(test_scans_flatpak_host_fonts);
    RUN(test_bundled_fonts_cover_czech);
    RUN(test_bundled_fonts_cover_french);
    RUN(test_missing_font_dirs_are_skipped);
    RUN(test_bundled_charmap_and_categories);
    RUN(test_mincho_first_reorders_only_craft_families);
    RUN(test_mincho_first_without_craft_fonts_is_identity);
    RUN(test_serif_names);
    RUN(test_ui_order_prefers_biz_udpgothic_regular);
    RUN(test_locale_orders);
    RUN(test_type_tool_fallback_follows_locale);
    RUN(test_classify_chars);
    RUN(test_reads_apple_languages_and_rejects_garbage);
    RUN(test_face_index_handles_bad_data);
    RUN(test_cjk_discovery_includes_host_fonts);
    RUN(test_matching_and_synthesis);
    RUN(test_collection_faces);
    RUN(test_damaged_fonts_never_crash);
    RUN(test_missing_and_empty_inputs);
    RUN(test_system_fonts_load);
    return pc_t_finish();
}
