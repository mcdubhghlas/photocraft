/* Embedded Profile Mismatch model (U10 B8): the should-open gate, parsing the report, and the option
 * tables. Pure logic, no window. */
#include "harness.h"
#include "ui/profile_mismatch.h"
#include "base/json.h"
#include <string.h>

static yyjson_mut_doc *mut_from(const char *text)
{
    yyjson_doc *doc = NULL;
    pc_error err;
    if (pc_json_parse(text, strlen(text), &doc, &err) != PC_OK) {
        PC_T_FAIL("parse: %s", err.msg);
        return NULL;
    }
    yyjson_mut_doc *m = yyjson_doc_mut_copy(doc, NULL);
    yyjson_doc_free(doc);
    return m;
}

static void test_should_open(void)
{
    yyjson_mut_doc *m;
    /* Mismatch + ask -> open. */
    m = mut_from("{\"mismatch\":true,\"ask\":true,\"embedded\":\"sRGB\",\"working\":\"Adobe RGB\"}");
    CHECK(pc_ui_profile_mismatch_should_open(yyjson_mut_doc_get_root(m)));
    yyjson_mut_doc_free(m);
    /* Mismatch but ask off -> no. */
    m = mut_from("{\"mismatch\":true,\"ask\":false}");
    CHECK(!pc_ui_profile_mismatch_should_open(yyjson_mut_doc_get_root(m)));
    yyjson_mut_doc_free(m);
    /* Missing + ask -> open. */
    m = mut_from("{\"missing\":true,\"ask\":true,\"working\":\"sRGB\"}");
    CHECK(pc_ui_profile_mismatch_should_open(yyjson_mut_doc_get_root(m)));
    yyjson_mut_doc_free(m);
    /* Ask but no mismatch and no missing -> no. */
    m = mut_from("{\"ask\":true,\"mismatch\":false}");
    CHECK(!pc_ui_profile_mismatch_should_open(yyjson_mut_doc_get_root(m)));
    yyjson_mut_doc_free(m);
    /* "kept" report with no ask field -> no. */
    m = mut_from("{\"action\":\"kept\"}");
    CHECK(!pc_ui_profile_mismatch_should_open(yyjson_mut_doc_get_root(m)));
    yyjson_mut_doc_free(m);
    CHECK(!pc_ui_profile_mismatch_should_open(NULL));
}

static void test_parse(void)
{
    yyjson_mut_doc *m;
    pc_ui_profile_mismatch model;

    m = mut_from("{\"mismatch\":true,\"ask\":true,\"embedded\":\"sRGB\",\"working\":\"Adobe RGB\","
                 "\"action\":\"converted\"}");
    CHECK(pc_ui_profile_mismatch_parse(yyjson_mut_doc_get_root(m), &model));
    CHECK(!model.missing);
    CHECK(!strcmp(model.embedded, "sRGB"));
    CHECK(!strcmp(model.working, "Adobe RGB"));
    CHECK(!strcmp(model.initial_action, "convert"));
    yyjson_mut_doc_free(m);

    m = mut_from("{\"action\":\"discarded\",\"embedded\":\"x\",\"working\":\"y\"}");
    CHECK(pc_ui_profile_mismatch_parse(yyjson_mut_doc_get_root(m), &model));
    CHECK(!strcmp(model.initial_action, "discard"));
    yyjson_mut_doc_free(m);

    m = mut_from("{\"missing\":true,\"working\":\"sRGB\",\"action\":\"assigned\"}");
    CHECK(pc_ui_profile_mismatch_parse(yyjson_mut_doc_get_root(m), &model));
    CHECK(model.missing);
    CHECK(!strcmp(model.embedded, ""));
    CHECK(!strcmp(model.working, "sRGB"));
    CHECK(!strcmp(model.initial_action, "preserve")); /* anything but converted/discarded */
    yyjson_mut_doc_free(m);

    /* Bad input: zeroed + safe defaults. */
    CHECK(!pc_ui_profile_mismatch_parse(NULL, &model));
    CHECK(!model.missing);
    CHECK(!strcmp(model.embedded, ""));
    CHECK(!strcmp(model.initial_action, "preserve"));
    CHECK(!pc_ui_profile_mismatch_parse(NULL, NULL));
}

static void test_options(void)
{
    const pc_ui_pm_option *opts = NULL;
    size_t n = pc_ui_profile_mismatch_options(false, &opts);
    CHECK_EQ_UINT(n, 3u);
    CHECK(opts && !strcmp(opts[0].action, "preserve"));
    CHECK(opts && !strcmp(opts[1].action, "convert"));
    CHECK(opts && !strcmp(opts[2].action, "discard"));
    n = pc_ui_profile_mismatch_options(true, &opts);
    CHECK_EQ_UINT(n, 2u);
    CHECK(opts && !strcmp(opts[0].action, "preserve"));
    CHECK(opts && !strcmp(opts[1].action, "assignWorking"));
    /* NULL out is tolerated. */
    CHECK_EQ_UINT(pc_ui_profile_mismatch_options(false, NULL), 3u);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_should_open);
    RUN(test_parse);
    RUN(test_options);
    return pc_t_finish();
}
