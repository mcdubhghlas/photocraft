/* Edit > Color Settings > "Blend Text Colors Using Gamma" drives the compositor's application-wide
 * text gamma, and prefs.set/reset on colorSettings move it too (#987). Port of
 * crates/engine/tests/text_gamma.rs. Each C test is its own process, so the global gamma this test
 * changes never leaks into another test. */
#include "compose/psblend.h"
#include "engine/engine.h"
#include "harness.h"

#define EPS 1e-6

/* Run edit.colorSettings with json; on success return result["settings"]["blendTextGamma"] in
 * *gamma. Returns the pc_status. */
static pc_status color_settings(pc_engine_session *s, const char *json, double *gamma)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status rc = pc_engine_session_execute_json(s, "edit.colorSettings", json, &out, &err);
    if (rc == PC_OK) {
        if (gamma != NULL) {
            yyjson_mut_val *root = pc_json_value_root(&out);
            yyjson_mut_val *settings = root ? yyjson_mut_obj_get(root, "settings") : NULL;
            yyjson_mut_val *g = settings ? yyjson_mut_obj_get(settings, "blendTextGamma") : NULL;
            *gamma = g ? yyjson_mut_get_real(g) : 0.0;
        }
        pc_json_value_fini(&out);
    }
    return rc;
}

static void run_ok(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    if (pc_engine_session_execute_json(s, id, json, &out, &err) != PC_OK) {
        PC_T_FAIL("%s %s: %s", id, json, err.msg);
    } else {
        pc_json_value_fini(&out);
    }
}

static void test_blend_text_gamma_setting(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);

    double g = 0.0;
    CHECK(color_settings(s, "{}", &g) == PC_OK);
    CHECK_NEAR(g, 1.45, EPS); /* the default */

    /* Out of range is rejected. */
    CHECK(color_settings(s, "{\"blendTextGamma\":3.0}", NULL) != PC_OK);

    CHECK(color_settings(s, "{\"blendTextGamma\":1.45}", NULL) == PC_OK);
    CHECK_NEAR(pc_psblend_text_gamma(), 1.45, EPS);

    CHECK(color_settings(s, "{\"blendTextGamma\":false}", &g) == PC_OK);
    CHECK_NEAR(g, 1.0, EPS);
    CHECK_NEAR(pc_psblend_text_gamma(), 1.0, EPS);

    CHECK(color_settings(s, "{\"blendTextGamma\":1.8}", NULL) == PC_OK);
    CHECK_NEAR(pc_psblend_text_gamma(), 1.8, EPS);

    CHECK(color_settings(s, "{\"blendTextGamma\":true}", &g) == PC_OK);
    CHECK_NEAR(g, 1.45, EPS);
    CHECK_NEAR(pc_psblend_text_gamma(), 1.45, EPS);

    /* prefs.set / prefs.reset on colorSettings move the compositor's gamma too. */
    run_ok(s, "edit.colorSettings", "{\"blendTextGamma\":1.8}");
    run_ok(s, "prefs.set", "{\"path\":\"colorSettings.blendTextGamma\",\"value\":1.0}");
    CHECK_NEAR(pc_psblend_text_gamma(), 1.0, EPS);
    run_ok(s, "prefs.reset", "{\"path\":\"colorSettings.blendTextGamma\"}");
    CHECK_NEAR(pc_psblend_text_gamma(), 1.45, EPS);

    static const char *const resets[] = {"{\"path\":\"colorSettings\"}", "{}"};
    for (size_t i = 0; i < 2; i++) {
        run_ok(s, "edit.colorSettings", "{\"blendTextGamma\":1.8}");
        run_ok(s, "prefs.reset", resets[i]);
        CHECK_NEAR(pc_psblend_text_gamma(), 1.45, EPS);
    }
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_blend_text_gamma_setting);
    return pc_t_finish();
}
