/* param_check hook: reject undocumented params for STRICT commands (E4, #4336). */
#include <string.h>

#include "base/json.h"
#include "engine/engine_internal.h"
#include "harness.h"

static pc_status check(const char *id, const char *params_doc, const char *json)
{
    pc_engine_command spec = {0};
    spec.id = id;
    spec.params = params_doc;
    yyjson_doc *doc = NULL;
    CHECK(pc_json_parse(json, strlen(json), &doc, NULL) == PC_OK);
    pc_error err = {0};
    pc_status rc = pc_engine_hook_param_check(&spec, yyjson_doc_get_root(doc), &err);
    yyjson_doc_free(doc);
    return rc;
}

static void test_strict_rejects_unknown(void)
{
    const char *pd = "{\"layer\":id?} (no layer: every selected layer)";
    /* A known key is accepted; an unknown one is rejected naming it. */
    CHECK(check("layer.hideLayers", pd, "{\"layer\":4}") == PC_OK);
    CHECK(check("layer.hideLayers", pd, "{\"coalesce\":\"k\"}") == PC_OK); /* universal */
    CHECK(check("layer.hideLayers", pd, "{\"target\":\"mask\"}") == PC_OK); /* universal */
    CHECK(check("layer.hideLayers", pd, "{\"ids\":[4]}") != PC_OK);
    CHECK(check("layer.showLayers", pd, "{\"nope\":1}") != PC_OK);
}

static void test_non_strict_is_permissive(void)
{
    /* A command not in STRICT ignores unknown keys. */
    CHECK(check("edit.fill", "{\"color\":x}", "{\"whatever\":1,\"color\":\"#fff\"}") == PC_OK);
    CHECK(check("some.other", "{\"a\":x}", "{\"z\":1}") == PC_OK);
}

static void test_open_doc_and_multi_key(void)
{
    /* A top-level ellipsis means the doc is open-ended: never checked, even in STRICT. */
    CHECK(check("layer.hideLayers", "{\"layer\":id?, …more}", "{\"anything\":1}") == PC_OK);
    /* A ,"other": pair list names every key. */
    const char *pd = "{\"locks\":{\"all\",\"pixels\":bool}?,\"name\":str?}";
    CHECK(check("layer.hideLayers", pd, "{\"name\":\"x\"}") == PC_OK);
    CHECK(check("layer.hideLayers", pd, "{\"pixels\":true}") == PC_OK);
    CHECK(check("layer.hideLayers", pd, "{\"bogus\":1}") != PC_OK);
}

static void test_non_object_params(void)
{
    /* Non-object params are left to the command to reject; the hook passes them through. */
    CHECK(check("layer.hideLayers", "{\"layer\":id?}", "[1,2]") == PC_OK);
    CHECK(check("layer.hideLayers", "{\"layer\":id?}", "null") == PC_OK);
}

int main(int argc, char **argv)
{
    RUN(test_strict_rejects_unknown);
    RUN(test_non_strict_is_permissive);
    RUN(test_open_doc_and_multi_key);
    RUN(test_non_object_params);
    return pc_t_finish();
}
