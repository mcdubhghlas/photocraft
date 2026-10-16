/* UI localisation (port of crates/ui-egui/src/i18n/ tests). */
#include "harness.h"
#include "ui/i18n.h"

#include <string.h>

static const char *code(const pc_i18n_lang *l) { return l ? pc_i18n_lang_code(l) : "(null)"; }

static void test_dir_for_code(void)
{
    CHECK(pc_i18n_dir_for_code("ar") == PC_I18N_RTL);
    CHECK(pc_i18n_dir_for_code("fa-ir") == PC_I18N_RTL);
    CHECK(pc_i18n_dir_for_code("HE") == PC_I18N_RTL);
    CHECK(pc_i18n_dir_for_code("ur_PK") == PC_I18N_RTL);
    CHECK(pc_i18n_dir_for_code("en") == PC_I18N_LTR);
    CHECK(pc_i18n_dir_for_code("ja") == PC_I18N_LTR);
    CHECK(pc_i18n_lang_dir(pc_i18n_en()) == PC_I18N_LTR);
}

static void test_candidates(void)
{
    char c[16][32];
    size_t n = pc_i18n_candidates("pt_BR.UTF-8", c, 16);
    CHECK(n == 2 && strcmp(c[0], "pt-br") == 0 && strcmp(c[1], "pt") == 0);
    n = pc_i18n_candidates("zh_TW", c, 16);
    CHECK(n == 3 && strcmp(c[0], "zh-tw") == 0 && strcmp(c[1], "zh-hant") == 0 && strcmp(c[2], "zh") == 0);
    n = pc_i18n_candidates("zh-CN", c, 16);
    CHECK(n == 3 && strcmp(c[0], "zh-cn") == 0 && strcmp(c[1], "zh-hans") == 0 && strcmp(c[2], "zh") == 0);
    n = pc_i18n_candidates("zh-Hant-HK", c, 16);
    CHECK(n == 3 && strcmp(c[0], "zh-hant-hk") == 0 && strcmp(c[1], "zh-hant") == 0 && strcmp(c[2], "zh") == 0);
    CHECK(pc_i18n_candidates("", c, 16) == 0);
    CHECK(pc_i18n_candidates("_", c, 16) == 0);
}

static void test_tags_map(void)
{
    CHECK_EQ_STR(code(pc_i18n_from_tag("ja_JP.UTF-8")), "ja");
    CHECK_EQ_STR(code(pc_i18n_from_tag("ja-JP")), "ja");
    CHECK_EQ_STR(code(pc_i18n_from_tag("en_US.UTF-8")), "en");
    CHECK_EQ_STR(code(pc_i18n_from_tag("C")), "en");
    CHECK_EQ_STR(code(pc_i18n_from_tag("POSIX")), "en");
    CHECK_EQ_STR(code(pc_i18n_from_tag("cs_CZ.UTF-8")), "cs");
    CHECK_EQ_STR(code(pc_i18n_from_tag("fr_FR")), "fr");
    CHECK_EQ_STR(code(pc_i18n_from_tag("de-AT")), "de");
    CHECK_EQ_STR(code(pc_i18n_from_tag("it-CH")), "it");
    CHECK_EQ_STR(code(pc_i18n_from_tag("nl-BE")), "nl");
    CHECK_EQ_STR(code(pc_i18n_from_tag("tr")), "tr");
    CHECK_EQ_STR(code(pc_i18n_from_tag("zh_TW.UTF-8")), "zh-hant");
    CHECK_EQ_STR(code(pc_i18n_from_tag("zh-HK")), "zh-hant");
    CHECK_EQ_STR(code(pc_i18n_from_tag("zh-Hant-TW")), "zh-hant");
    const char *hans[] = {"zh-CN", "zh_CN.UTF-8", "zh_SG", "zh-Hans", "zh-Hans-CN", "zh"};
    for (size_t i = 0; i < 6; i++) {
        CHECK_EQ_STR(code(pc_i18n_from_tag(hans[i])), "zh-hans");
    }
    CHECK(pc_i18n_from_tag("") == NULL);
    CHECK(pc_i18n_from_tag("_") == NULL);
    /* Indonesian and Vietnamese. */
    for (const char **p = (const char *[]){"id", "id-ID", "id_ID", "id_ID.UTF-8", NULL}; *p; p++) {
        CHECK_EQ_STR(code(pc_i18n_from_tag(*p)), "id");
    }
    for (const char **p = (const char *[]){"vi", "vi-VN", "vi_VN", "vi_VN.UTF-8", NULL}; *p; p++) {
        CHECK_EQ_STR(code(pc_i18n_from_tag(*p)), "vi");
    }
}

static void test_prefs(void)
{
    /* Under test, force the system locale to English (override with an empty tag list). */
    pc_i18n_set_system_tags_for_test((const char *const[]){NULL}, 0);
    CHECK_EQ_STR(code(pc_i18n_from_pref("ja")), "ja");
    CHECK_EQ_STR(code(pc_i18n_from_pref("JA")), "ja");
    CHECK_EQ_STR(code(pc_i18n_from_pref("zh-hant")), "zh-hant");
    CHECK_EQ_STR(code(pc_i18n_from_pref("ZH-Hant")), "zh-hant");
    CHECK_EQ_STR(code(pc_i18n_from_pref("en")), "en");
    CHECK_EQ_STR(code(pc_i18n_from_pref("auto")), "en");
    CHECK_EQ_STR(code(pc_i18n_from_pref("xx-unknown")), "en");
    pc_i18n_set_system_tags_for_test(NULL, 0); /* clear override */
}

static void test_system_negotiation(void)
{
    struct {
        const char *tags[4];
        size_t n;
        const char *expect;
    } cases[] = {
        {{"ja-JP", "en-US"}, 2, "ja"},
        {{"fr-CA", "en-US"}, 2, "fr"},
        {{"uk-UA", "en-US"}, 2, "uk"},
        {{"sv-SE", "uk_UA.UTF-8", "ru-RU"}, 3, "uk"},
        {{"sv-SE", "ko-KR", "fr-FR"}, 3, "ko"},
        {{"zh-Hant-HK", "zh-CN"}, 2, "zh-hant"},
        {{"zh-Hans-CN", "zh-TW"}, 2, "zh-hans"},
        {{"en-US", "ru-RU"}, 2, "en"},
        {{"sv-SE", "ar-SA"}, 2, "en"},
        {{"de-AT", "en-US"}, 2, "de"},
        {{"pt-PT"}, 1, "pt-br"},
        {{"sr-Latn-RS", "en-US"}, 2, "sr"},
        {{"sv-SE", "sr_RS@latin", "ru-RU"}, 3, "sr"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pc_i18n_set_system_tags_for_test(cases[i].tags, cases[i].n);
        CHECK_EQ_STR(code(pc_i18n_system_lang()), cases[i].expect);
    }
    pc_i18n_set_system_tags_for_test((const char *const[]){NULL}, 0);
    CHECK_EQ_STR(code(pc_i18n_system_lang()), "en");
    pc_i18n_set_system_tags_for_test(NULL, 0);
}

static void test_plural_rules(void)
{
    const pc_i18n_lang *ru = pc_i18n_from_code("ru");
    CHECK_EQ_UINT(pc_i18n_lang_plural(ru, 1), 0);
    CHECK_EQ_UINT(pc_i18n_lang_plural(ru, 2), 1);
    CHECK_EQ_UINT(pc_i18n_lang_plural(ru, 5), 2);
    CHECK_EQ_UINT(pc_i18n_lang_plural(ru, 11), 2);
    CHECK_EQ_UINT(pc_i18n_lang_plural(ru, 21), 0);
    CHECK_EQ_UINT(pc_i18n_lang_plural(ru, 22), 1);
    const pc_i18n_lang *cs = pc_i18n_from_code("cs");
    CHECK_EQ_UINT(pc_i18n_lang_plural(cs, 1), 0);
    CHECK_EQ_UINT(pc_i18n_lang_plural(cs, 3), 1);
    CHECK_EQ_UINT(pc_i18n_lang_plural(cs, 5), 2);
    const pc_i18n_lang *fr = pc_i18n_from_code("fr");
    CHECK_EQ_UINT(pc_i18n_lang_plural(fr, 0), 0);
    CHECK_EQ_UINT(pc_i18n_lang_plural(fr, 1), 0);
    CHECK_EQ_UINT(pc_i18n_lang_plural(fr, 2), 1);
    const pc_i18n_lang *pl = pc_i18n_from_code("pl");
    CHECK_EQ_UINT(pc_i18n_lang_plural(pl, 1), 0);
    CHECK_EQ_UINT(pc_i18n_lang_plural(pl, 3), 1);
    CHECK_EQ_UINT(pc_i18n_lang_plural(pl, 13), 2);
    CHECK_EQ_UINT(pc_i18n_lang_plural(pl, 22), 1);
    const pc_i18n_lang *ja = pc_i18n_from_code("ja");
    CHECK_EQ_UINT(pc_i18n_lang_plural(ja, 0), 0);
    CHECK_EQ_UINT(pc_i18n_lang_plural(ja, 9), 0);
}

static void test_fmt(void)
{
    pc_buf out = PC_BUF_INIT;
    const char *keys[] = {"name", "url"};
    const char *vals[] = {"Alice", "http://x"};
    CHECK(pc_i18n_fmt("Hello {name} at {url}!", keys, vals, 2, &out, NULL) == PC_OK);
    CHECK_EQ_STR(pc_buf_cstr(&out), "Hello Alice at http://x!");
    /* unknown placeholder stays. */
    CHECK(pc_i18n_fmt("{name} {missing}", keys, vals, 2, &out, NULL) == PC_OK);
    CHECK_EQ_STR(pc_buf_cstr(&out), "Alice {missing}");
    CHECK(pc_i18n_fmt("no placeholders", keys, vals, 0, &out, NULL) == PC_OK);
    CHECK_EQ_STR(pc_buf_cstr(&out), "no placeholders");
    pc_buf_fini(&out);
}

static void test_translations(void)
{
    const pc_i18n_lang *zh = pc_i18n_from_code("zh-hans");
    CHECK(pc_i18n_lang_complete_menus(zh));
    CHECK_EQ_STR(pc_i18n_tr(zh, "Pixel Layer"), "\xe5\x83\x8f\xe7\xb4\xa0\xe5\x9b\xbe\xe5\xb1\x82");
    CHECK_EQ_STR(pc_i18n_tr(zh, "System Info"), "\xe7\xb3\xbb\xe7\xbb\x9f\xe4\xbf\xa1\xe6\x81\xaf");
    CHECK_EQ_STR(pc_i18n_tr(zh, "no such label"), "no such label"); /* fallback */
    CHECK(!pc_i18n_has(zh, "no such label"));
    CHECK(pc_i18n_has(zh, "Pixel Layer"));
    pc_buf out = PC_BUF_INIT;
    for (uint64_t n = 0; n <= 3; n++) {
        CHECK(pc_i18n_trn(zh, n, "{n} layer", "{n} layers", &out, NULL) == PC_OK);
        char expect[32];
        (void)snprintf(expect, sizeof expect, "%llu \xe4\xb8\xaa\xe5\x9b\xbe\xe5\xb1\x82", (unsigned long long)n);
        CHECK_EQ_STR(pc_buf_cstr(&out), expect);
    }
    pc_buf_fini(&out);
    const pc_i18n_lang *ko = pc_i18n_from_code("ko");
    CHECK_EQ_STR(pc_i18n_tr(ko, "Shape"), "\xeb\xaa\xa8\xec\x96\x91");
    CHECK_EQ_STR(pc_i18n_tr(ko, "Stroke"), "\xed\x9a\x8d");
    /* English is the source: never translates. */
    CHECK_EQ_STR(pc_i18n_tr(pc_i18n_en(), "Pixel Layer"), "Pixel Layer");
    CHECK(!pc_i18n_has(pc_i18n_en(), "Pixel Layer"));
}

static void test_current_thread_local(void)
{
    const pc_i18n_lang *ja = pc_i18n_from_code("ja");
    pc_i18n_set_current(ja);
    CHECK(pc_i18n_current() == ja);
    /* t() uses current. */
    CHECK(strcmp(pc_i18n_t("Pixel Layer"), "Pixel Layer") != 0);
    pc_i18n_set_current(pc_i18n_en());
    CHECK_EQ_STR(pc_i18n_t("Pixel Layer"), "Pixel Layer");
}

static void test_registry_and_names(void)
{
    CHECK_EQ_UINT(pc_i18n_count(), 21);
    CHECK_EQ_STR(pc_i18n_lang_name(pc_i18n_from_code("id")), "Bahasa Indonesia");
    CHECK_EQ_STR(pc_i18n_lang_name(pc_i18n_from_code("de")), "Deutsch");
    CHECK(pc_i18n_from_code("en") == pc_i18n_en());
    CHECK(!pc_i18n_lang_complete_menus(pc_i18n_en()));
    /* Every registered language name is non-empty. */
    for (size_t i = 0; i < pc_i18n_count(); i++) {
        CHECK(pc_i18n_lang_name(pc_i18n_at(i))[0] != '\0');
    }
}

static void test_catalog_parser(void)
{
    /* Synthetic catalog covering the four kinds. */
    const char *text = "# comment\n"
                       "\tHello\tBonjour\n"            /* plain */
                       "@id\tfile.open\tOuvrir\n"      /* id */
                       "menu\tOpen\tOuvrir (menu)\n"   /* contextual */
                       "@plural\t{n} layer|{n} layers\t{n} calque|{n} calques\n"
                       "\n"                            /* blank skipped */
                       "bad line no tabs\n"            /* malformed */
                       "\tonly two\n";                 /* malformed: only ctx+src */
    char first[128] = {0};
    size_t errs = pc_i18n_catalog_errors(text, strlen(text), first, sizeof first);
    CHECK_EQ_UINT(errs, 2);
    CHECK_CONTAINS(first, "context<TAB>source<TAB>translation");

    /* placeholders() extracts {name} in order. */
    char ph[8][64];
    size_t n = pc_i18n_placeholders("use {name} and {url} and {name}", ph, 8);
    CHECK(n == 3 && strcmp(ph[0], "name") == 0 && strcmp(ph[1], "url") == 0 && strcmp(ph[2], "name") == 0);
    CHECK(pc_i18n_placeholders("no braces", ph, 8) == 0);
}

static void test_bundled_catalogs_are_clean(void)
{
    /* Every bundled catalog parses with no malformed lines (live_tests invariant). */
    for (size_t i = 0; i < pc_i18n_count(); i++) {
        const pc_i18n_lang *l = pc_i18n_at(i);
        size_t len = 0;
        const unsigned char *src = pc_i18n_lang_source(l, &len);
        if (!src) {
            continue; /* English */
        }
        char first[160] = {0};
        size_t errs = pc_i18n_catalog_errors((const char *)src, len, first, sizeof first);
        if (errs != 0) {
            PC_T_FAIL("%s catalog has %zu malformed lines: %s", pc_i18n_lang_code(l), errs, first);
        }
        pc_t_checks++;
    }
}

int main(int argc, char **argv)
{
    RUN(test_dir_for_code);
    RUN(test_candidates);
    RUN(test_tags_map);
    RUN(test_prefs);
    RUN(test_system_negotiation);
    RUN(test_plural_rules);
    RUN(test_fmt);
    RUN(test_translations);
    RUN(test_current_thread_local);
    RUN(test_registry_and_names);
    RUN(test_catalog_parser);
    RUN(test_bundled_catalogs_are_clean);
    return pc_t_finish();
}
