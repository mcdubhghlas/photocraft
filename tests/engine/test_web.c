/* File > Export > Save for Web / Export Preferences / Quick Export, File > Generate > Image
 * Assets. These are registered as unsupported stubs in this chunk (REWRITE_PLAN.md §6/§7); the
 * test checks the ids are present, never crash, and report unsupported. */
#include "engine/engine_internal.h"
#include "harness.h"

static pc_status ex(pc_engine_session *s, const char *id, const char *p)
{
    pc_json_value o = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, id, p, &o, NULL);
    pc_json_value_fini(&o);
    return rc;
}

static void test_registered_and_unsupported(void)
{
    const char *ids[] = {"file.export.saveForWebLegacy", "file.export.exportPreferences",
                         "file.export.quickExport", "file.generate.imageAssets"};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        CHECK(pc_engine_command_find(ids[i]) != NULL); /* id present in the registry */
    }
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(ex(s, "file.new", "{\"width\":8,\"height\":8}") == PC_OK);
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        CHECK(ex(s, ids[i], "{}") != PC_OK); /* unsupported, not a crash */
    }
    pc_engine_session_free(s);
}

static void test_adversarial(void)
{
    const char *ids[] = {"file.export.saveForWebLegacy", "file.export.exportPreferences",
                         "file.export.quickExport", "file.generate.imageAssets"};
    const char *ps[] = {"{}", "[]", "null", "123", "{\"path\":5}", "{\"on\":\"x\"}"};
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(ex(s, "file.new", "{\"width\":8,\"height\":8}") == PC_OK);
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        for (size_t j = 0; j < sizeof ps / sizeof ps[0]; j++) {
            (void)ex(s, ids[i], ps[j]);
        }
    }
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_registered_and_unsupported);
    RUN(test_adversarial);
    return pc_t_finish();
}
