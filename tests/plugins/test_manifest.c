/* The plug-in manifest (port of the tests in crates/plugins/src/manifest.rs), plus never-crash
 * checks on hostile manifests and parameters. Byte-level parity with serde's messages is
 * tests/plugins/test_parity.c. */
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "harness.h"
#include "plugins/plugins.h"

static const char FULL[] =
    "{\"id\":\"org.example.tone\",\"name\":\"Tone\",\"version\":\"1.2\",\"kind\":\"filter\",\n"
    "        \"params\":{\"amount\":{\"type\":\"number\",\"min\":0,\"max\":100,\"default\":50},\n"
    "                  \"mode\":{\"type\":\"choice\",\"options\":[\"soft\",\"hard\"],"
    "\"default\":\"hard\"},\n"
    "                  \"mono\":{\"type\":\"bool\"},\"seed\":{\"type\":\"int\",\"min\":0,"
    "\"max\":9}},\n"
    "        \"overlap\":2,\"area\":\"canvas\"}";

static pc_status parse(const char *s, pc_plugin_manifest *m, pc_error *err)
{
    return pc_plugin_manifest_parse((const uint8_t *)s, strlen(s), m, err);
}

/* resolve_params(json) written as serde_json::Value text, or NULL on error. */
static char *resolve(const pc_plugin_manifest *m, const char *params, pc_error *err)
{
    yyjson_doc *doc = NULL;
    pc_json_value v = PC_JSON_VALUE_INIT;
    pc_buf b = PC_BUF_INIT;
    char *out = NULL;
    if (params != NULL && pc_json_parse(params, strlen(params), &doc, NULL) != PC_OK) {
        return NULL;
    }
    if (pc_plugin_manifest_resolve_params(m, params != NULL ? yyjson_doc_get_root(doc) : NULL, &v, err) ==
            PC_OK &&
        pc_json_write(pc_json_value_root(&v), PC_JSON_SORT_KEYS, &b, NULL) == PC_OK) {
        out = (char *)pc_buf_take(&b, NULL);
    }
    pc_buf_fini(&b);
    pc_json_value_fini(&v);
    yyjson_doc_free(doc);
    return out;
}

static void check_resolve(const pc_plugin_manifest *m, const char *params, const char *want)
{
    char *got = resolve(m, params, NULL);
    CHECK(got != NULL);
    if (got != NULL) {
        CHECK_EQ_STR(got, want);
    }
    free(got);
}

static void test_parses_and_keeps_param_order(void)
{
    pc_plugin_manifest m;
    pc_buf n = PC_BUF_INIT;
    CHECK_EQ_INT(parse(FULL, &m, NULL), PC_OK);
    CHECK_EQ_STR(pc_str_c(&m.id), "org.example.tone");
    CHECK_EQ_UINT(m.n_params, 4);
    if (m.n_params == 4) {
        CHECK_EQ_STR(pc_str_c(&m.params[0].name), "amount");
        CHECK_EQ_STR(pc_str_c(&m.params[1].name), "mode");
        CHECK_EQ_STR(pc_str_c(&m.params[2].name), "mono");
        CHECK_EQ_STR(pc_str_c(&m.params[3].name), "seed");
    }
    CHECK_EQ_UINT(m.overlap, 2);
    CHECK(m.area == PC_PLUGIN_AREA_CANVAS);
    CHECK_EQ_INT(pc_plugin_manifest_params_notation(&m, &n, NULL), PC_OK);
    CHECK_EQ_STR(pc_buf_cstr(&n),
                 "{\"amount\":0..100=50,\"mode\":\"hard|soft\",\"mono\":bool=false,\"seed\":int=0}");
    pc_buf_fini(&n);
    pc_plugin_manifest_fini(&m);
}

static void test_resolves_defaults_clamps_and_rejects_wrong_types(void)
{
    pc_plugin_manifest m;
    pc_error e;
    char *r;
    CHECK_EQ_INT(parse(FULL, &m, NULL), PC_OK);
    check_resolve(&m, "{\"amount\": 500, \"seed\": 3.6, \"junk\": 1}",
                  "{\"amount\":100.0,\"mode\":\"hard\",\"mono\":false,\"seed\":4}");
    r = resolve(&m, "{\"amount\": \"lots\"}", &e);
    CHECK(r == NULL);
    CHECK_EQ_INT(pc_plugin_error_kind_of(&e), PC_PLUGIN_E_PARAMS);
    CHECK_EQ_STR(e.msg, "plug-in parameters: `amount` must be a number");
    CHECK(resolve(&m, "{\"mode\": \"medium\"}", NULL) == NULL);
    CHECK(resolve(&m, "{\"mono\": 1}", NULL) == NULL);
    r = resolve(&m, "[1, 2]", &e);
    CHECK(r == NULL);
    CHECK_EQ_STR(e.msg, "plug-in parameters: plug-in parameters must be a JSON object");
    r = resolve(&m, "null", NULL);
    CHECK(r != NULL);
    free(r);
    r = resolve(&m, NULL, NULL);
    CHECK(r != NULL);
    free(r);
    pc_plugin_manifest_fini(&m);
}

static void test_out_of_range_defaults_are_clamped(void)
{
    /* Issue #1012: a missing value fell back to a default outside the declared range unclamped. */
    static const char T[] =
        "{\"id\":\"org.example.tone\",\"name\":\"Tone\",\"kind\":\"filter\",\n"
        "\"params\":{\"amount\":{\"type\":\"number\",\"min\":0,\"max\":100,\"default\":1000},\n"
        "\"steps\":{\"type\":\"int\",\"min\":1,\"max\":16,\"default\":1000},\n"
        "\"low\":{\"type\":\"number\",\"min\":-1,\"max\":1,\"default\":-5},\n"
        "\"count\":{\"type\":\"int\",\"min\":3,\"max\":9}}}";
    static const char RESOLVED[] = "{\"amount\":100.0,\"count\":3,\"low\":-1.0,\"steps\":16}";
    pc_plugin_manifest m, full;
    CHECK_EQ_INT(parse(T, &m, NULL), PC_OK);
    check_resolve(&m, "{}", RESOLVED);
    check_resolve(&m, "null", RESOLVED);
    check_resolve(&m, "{\"amount\": null, \"steps\": null}", RESOLVED);
    check_resolve(&m, "{\"amount\": 500, \"steps\": 500, \"low\": 0.5, \"count\": 5}",
                  "{\"amount\":100.0,\"count\":5,\"low\":0.5,\"steps\":16}");
    /* In-range defaults are unchanged. */
    CHECK_EQ_INT(parse(FULL, &full, NULL), PC_OK);
    check_resolve(&full, "{}", "{\"amount\":50.0,\"mode\":\"hard\",\"mono\":false,\"seed\":0}");
    pc_plugin_manifest_fini(&full);
    pc_plugin_manifest_fini(&m);
}

static const char BAD_RANGE[] = "{\"id\":\"a\",\"name\":\"x\",\"kind\":\"filter\",\"params\":{\"r\":"
                                "{\"type\":\"number\",\"min\":5,\"max\":1,\"default\":1}}}";
static const char BAD_CHOICE[] = "{\"id\":\"a\",\"name\":\"x\",\"kind\":\"filter\",\"params\":{\"c\":"
                                 "{\"type\":\"choice\",\"options\":[]}}}";

static void test_rejects_bad_manifests(void)
{
    static const char *const BAD[] = {
        "",
        "[]",
        "not json",
        "{\"id\":\"\",\"name\":\"x\",\"kind\":\"filter\"}",
        "{\"id\":\"a b\",\"name\":\"x\",\"kind\":\"filter\"}",
        "{\"id\":\"a\",\"name\":\"\",\"kind\":\"filter\"}",
        "{\"id\":\"a\",\"name\":\"x\",\"kind\":\"panel\"}",
        "{\"id\":\"a\",\"name\":\"x\"}",
        "{\"id\":\"a\",\"name\":\"x\",\"kind\":\"filter\",\"overlap\":100000}",
        BAD_RANGE,
        BAD_CHOICE,
        "{\"id\":\"a\",\"name\":\"x\",\"kind\":\"filter\",\"params\":{\"__x\":{\"type\":\"bool\"}}}",
        "{\"id\":\"a\",\"name\":\"x\",\"kind\":\"filter\",\"params\":[1]}",
    };
    static const uint8_t NOT_UTF8[2] = {0xff, 0xfe};
    pc_plugin_manifest m;
    pc_error e;
    uint8_t *spaces;
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        pc_error_clear(&e);
        CHECK_EQ_INT(parse(BAD[i], &m, &e), PC_ERR_FORMAT);
        CHECK_EQ_INT(pc_plugin_error_kind_of(&e), PC_PLUGIN_E_MANIFEST);
    }
    CHECK_EQ_INT(pc_plugin_manifest_parse(NOT_UTF8, 2, &m, &e), PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "plug-in manifest: manifest is not UTF-8");
    spaces = malloc(PC_PLUGIN_MAX_MANIFEST_BYTES + 1);
    CHECK(spaces != NULL);
    if (spaces != NULL) {
        memset(spaces, ' ', PC_PLUGIN_MAX_MANIFEST_BYTES + 1);
        CHECK_EQ_INT(pc_plugin_manifest_parse(spaces, PC_PLUGIN_MAX_MANIFEST_BYTES + 1, &m, &e),
                     PC_ERR_FORMAT);
        CHECK_EQ_STR(e.msg, "plug-in manifest: manifest is 65537 bytes (limit 65536)");
        /* Exactly the limit is read (the reference: "EOF while parsing a value at line 1 column
         * 65536"). */
        CHECK_EQ_INT(pc_plugin_manifest_parse(spaces, PC_PLUGIN_MAX_MANIFEST_BYTES, &m, &e), PC_ERR_FORMAT);
        CHECK_EQ_STR(e.msg, "plug-in manifest: manifest: EOF while parsing a value at line 1 column 65536");
        free(spaces);
    }
}

static pc_status with_name(const char *name, pc_plugin_manifest *m)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *o = yyjson_mut_obj(d);
    pc_buf b = PC_BUF_INIT;
    pc_status st;
    yyjson_mut_obj_add_str(d, o, "id", "org.example.tone");
    yyjson_mut_obj_add_str(d, o, "name", name);
    yyjson_mut_obj_add_str(d, o, "kind", "filter");
    st = pc_json_write(o, 0, &b, NULL);
    if (st == PC_OK) {
        st = pc_plugin_manifest_parse(b.data, b.len, m, NULL);
    }
    pc_buf_fini(&b);
    yyjson_mut_doc_free(d);
    return st;
}

static void test_name_must_be_printable_unicode(void)
{
    static const char *const BAD[] = {
        "Tone\xE2\x80\xAE\x65vil",
        "\xE2\x81\xA7Tone\xE2\x81\xA9",
        "Tone\xE2\x80\x8F",
        "\xE2\x80\x8B\xE2\x80\x8B\xE2\x80\x8B",
        "Tone\xC2\xAD",
        "\xEF\xBB\xBFTone",
        "\xE2\x80\x8D",
        "\xE3\x85\xA4\xEF\xB8\x8F",
        " \xE3\x80\x80 ",
        "Tone\x07",
    };
    /* Unicode names still install, with joiners where the script or emoji needs them. */
    static const char *const GOOD[] = {
        "Tone\xE2\x80\xA6",
        "Kontrast \xC2\xB1",
        "\xD9\x85\xDB\x8C\xE2\x80\x8C\xD8\xAE\xD9\x88\xD8\xA7\xD9\x87\xD9\x85",
        "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB Code",
        "\xE2\x9D\xA4\xEF\xB8\x8F Love",
        "\xE8\x89\xB2\xE8\xB0\x83",
    };
    pc_plugin_manifest m;
    char e64[129], e65[131];
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        CHECK(with_name(BAD[i], &m) != PC_OK);
    }
    for (size_t i = 0; i < sizeof GOOD / sizeof GOOD[0]; i++) {
        CHECK_EQ_INT(with_name(GOOD[i], &m), PC_OK);
        CHECK_EQ_STR(pc_str_c(&m.name), GOOD[i]);
        pc_plugin_manifest_fini(&m);
    }
    /* The documented limit counts characters, not bytes. */
    for (int i = 0; i < 65; i++) {
        e65[2 * i] = (char)0xC3;
        e65[2 * i + 1] = (char)0xA9;
    }
    memcpy(e64, e65, 128);
    e64[128] = '\0';
    e65[130] = '\0';
    CHECK_EQ_INT(with_name(e64, &m), PC_OK);
    pc_plugin_manifest_fini(&m);
    CHECK(with_name(e65, &m) != PC_OK);
}

static void test_serialises_like_serde(void)
{
    static const char WANT[] =
        "{\"id\":\"org.example.tone\",\"name\":\"Tone\",\"version\":\"1.2\",\"kind\":\"filter\","
        "\"description\":\"\",\"author\":\"\",\"params\":{\"amount\":{\"type\":\"number\",\"min\":0.0,"
        "\"max\":100.0,\"default\":50.0},\"mode\":{\"type\":\"choice\",\"options\":[\"soft\",\"hard\"],"
        "\"default\":\"hard\"},\"mono\":{\"type\":\"bool\",\"default\":false},\"seed\":{\"type\":\"int\","
        "\"min\":0,\"max\":9,\"default\":0}},\"overlap\":2,\"area\":\"canvas\"}";
    pc_plugin_manifest m, c;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    pc_buf b = PC_BUF_INIT;
    CHECK_EQ_INT(parse(FULL, &m, NULL), PC_OK);
    CHECK_EQ_INT(pc_plugin_manifest_clone(&m, &c, NULL), PC_OK);
    CHECK_EQ_INT(pc_json_write(pc_plugin_manifest_to_json(&c, d), 0, &b, NULL), PC_OK);
    CHECK_EQ_STR(pc_buf_cstr(&b), WANT);
    pc_buf_fini(&b);
    yyjson_mut_doc_free(d);
    pc_plugin_manifest_fini(&c);
    pc_plugin_manifest_fini(&m);
}

static void test_hostile_manifests_never_crash(void)
{
    /* Deep nesting, long inputs, every truncation of a valid manifest. */
    char *deep = malloc(70000);
    pc_plugin_manifest m;
    pc_error e;
    size_t n = strlen(FULL);
    if (deep != NULL) {
        size_t k = 0;
        k += (size_t)snprintf(deep, 70000,
                              "{\"id\":\"a\",\"name\":\"x\",\"kind\":\"filter\",\"params\":{\"a\":"
                              "{\"type\":\"bool\",\"x\":");
        for (int i = 0; i < 20000; i++) {
            deep[k++] = '[';
        }
        deep[k] = '\0';
        CHECK(pc_plugin_manifest_parse((const uint8_t *)deep, k, &m, &e) != PC_OK);
        CHECK_CONTAINS(e.msg, "recursion limit exceeded");
        free(deep);
    }
    for (size_t cut = 0; cut < n; cut++) {
        pc_error_clear(&e);
        CHECK(pc_plugin_manifest_parse((const uint8_t *)FULL, cut, &m, &e) != PC_OK);
        CHECK(e.code == PC_ERR_FORMAT);
    }
    /* Random byte flips. */
    {
        char *t = malloc(n);
        uint32_t s = 12345;
        for (int round = 0; t != NULL && round < 3000; round++) {
            memcpy(t, FULL, n);
            for (int f = 0; f < 3; f++) {
                s = s * 1103515245u + 12345u;
                t[(s >> 8) % n] = (char)(s >> 16);
            }
            if (pc_plugin_manifest_parse((const uint8_t *)t, n, &m, NULL) == PC_OK) {
                pc_plugin_manifest_fini(&m);
            }
        }
        free(t);
    }
}

int main(int argc, char **argv)
{
    RUN(test_parses_and_keeps_param_order);
    RUN(test_resolves_defaults_clamps_and_rejects_wrong_types);
    RUN(test_out_of_range_defaults_are_clamped);
    RUN(test_rejects_bad_manifests);
    RUN(test_name_must_be_printable_unicode);
    RUN(test_serialises_like_serde);
    RUN(test_hostile_manifests_never_crash);
    return pc_t_finish();
}
