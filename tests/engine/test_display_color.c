#include <math.h>
#include "harness.h"
#include "engine/display_color.h"
#include "cms/builtin.h"
#include "color/convert.h"
static pc_engine_session *session(int depth)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char params[100];
    (void)snprintf(params, sizeof params, "{\"width\":16,\"height\":12,\"depth\":%d}", depth);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.new", params, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    return s;
}
static void execute(pc_engine_session *s, const char *id, const char *params)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, id, params, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
}
static void test_identity_and_hdr(void)
{
    pc_engine_session *s = session(32);
    pc_engine_display d = {0};
    float rgb[3] = {.25f, .5f, .75f}, shown[3];
    CHECK(pc_engine_display_build(s, 5, &d, NULL) == PC_OK);
    pc_engine_display_apply(&d, rgb, shown);
    for (size_t i = 0; i < 3; i++) {
        CHECK_NEAR(shown[i], rgb[i], .001);
    }
    uint64_t key = pc_engine_display_key(s);
    execute(s, "view.thirtyTwoBitPreviewOptions", "{\"exposure\":1,\"gamma\":1}");
    CHECK(pc_engine_display_key(s) != key);
    CHECK(pc_engine_display_build(s, 5, &d, NULL) == PC_OK);
    pc_engine_display_apply(&d, rgb, shown);
    CHECK_NEAR(shown[0], .354, .02);
    execute(s, "view.thirtyTwoBitPreviewOptions", "{\"method\":\"highlightCompression\"}");
    CHECK(pc_engine_display_build(s, 5, &d, NULL) == PC_OK);
    pc_engine_display_apply(&d, rgb, shown);
    CHECK_NEAR(shown[0], .25, .001);
    pc_engine_session *other = session(32);
    pc_engine_display other_display = {0};
    CHECK(pc_engine_display_build(other, 5, &other_display, NULL) == PC_OK);
    CHECK(other_display.hdr.exposure == 0 && other_display.hdr.gamma == 1);
    pc_engine_display_fini(&other_display);
    pc_engine_session_free(other);
    float *old = d.lut.data;
    CHECK(pc_engine_display_build(s, 999, &d, NULL) == PC_ERR_LIMIT);
    CHECK(d.lut.data == old);
    pc_engine_display_fini(&d);
    pc_engine_session_free(s);
}
static void test_dichromat_and_simulations(void)
{
    float neutral[3] = {.2f, .2f, .2f}, white[3] = {1, 1, 1}, blue[3] = {0, 0, 1}, out[3];
    for (unsigned protan = 0; protan < 2; protan++) {
        pc_engine_display_dichromat(neutral, protan != 0, out);
        for (size_t i = 0; i < 3; i++) {
            CHECK_NEAR(out[i], .2, .02);
        }
        pc_engine_display_dichromat(white, protan != 0, out);
        for (size_t i = 0; i < 3; i++) {
            CHECK_NEAR(out[i], 1, .02);
        }
        pc_engine_display_dichromat(blue, protan != 0, out);
        CHECK(out[2] > .8f);
    }
    const char *commands[] = {
        "view.proofSetup.workingCyanPlate",         "view.proofSetup.workingMagentaPlate",
        "view.proofSetup.workingYellowPlate",       "view.proofSetup.workingBlackPlate",
        "view.proofSetup.workingCmyPlate",          "view.proofSetup.legacyMacintoshRgb",
        "view.proofSetup.colorBlindnessProtanopia", "view.proofSetup.colorBlindnessDeuteranopia"};
    pc_engine_session *s = session(8);
    pc_engine_display d = {0};
    for (size_t i = 0; i < sizeof commands / sizeof commands[0]; i++) {
        execute(s, commands[i], "{}");
        CHECK(pc_engine_display_build(s, 5, &d, NULL) == PC_OK);
        CHECK(d.lut.size == 5);
        for (size_t k = 0; k < 5 * 5 * 5 * 4; k++) {
            CHECK(isfinite(d.lut.data[k]));
        }
    }
    execute(s, "view.proofSetup.legacyMacintoshRgb", "{}");
    CHECK(pc_engine_display_build(s, 5, &d, NULL) == PC_OK);
    float rgb[3] = {.5f, .5f, .5f};
    pc_engine_display_apply(&d, rgb, out);
    CHECK(out[0] > .52f && out[0] < .62f);
    pc_engine_display_fini(&d);
    pc_engine_session_free(s);
}
static void test_linear_source(void)
{
    pc_engine_session *s = session(32);
    execute(s, "edit.assignProfile", "{\"profile\":\"linear-srgb\"}");
    pc_engine_display d = {0};
    CHECK(pc_engine_display_build(s, 17, &d, NULL) == PC_OK);
    CHECK(d.encode_linear);
    float rgb[3] = {.25f, .5f, .75f}, shown[3];
    pc_engine_display_apply(&d, rgb, shown);
    CHECK_NEAR(shown[0], pc_linear_to_srgb(.25f), .002);
    execute(s, "view.thirtyTwoBitPreviewOptions", "{\"exposure\":-1}");
    CHECK(pc_engine_display_build(s, 17, &d, NULL) == PC_OK);
    rgb[0] = rgb[1] = rgb[2] = 2;
    pc_engine_display_apply(&d, rgb, shown);
    CHECK_NEAR(shown[0], 1, .002);
    pc_engine_display_fini(&d);
    pc_engine_session_free(s);
}
static void test_gamut_preferences_and_key(void)
{
    pc_engine_session *s = session(8);
    pc_engine_display d = {0};
    execute(s, "view.gamutWarning", "{\"on\":true}");
    execute(s, "prefs.set", "{\"path\":\"transparencyAndGamut.gamutWarningColor\",\"value\":\"#ff00ff\"}");
    uint64_t key = pc_engine_display_key(s);
    execute(s, "prefs.set", "{\"path\":\"transparencyAndGamut.gamutWarningOpacity\",\"value\":75.5}");
    CHECK(pc_engine_display_key(s) != key);
    CHECK(pc_engine_display_build(s, 5, &d, NULL) == PC_OK && d.gamut_warning);
    CHECK_NEAR(d.warning_opacity, .755, .00001);
    bool tested = false;
    for (size_t z = 0; z < 5 && !tested; z++) {
        for (size_t y = 0; y < 5 && !tested; y++) {
            for (size_t x = 0; x < 5 && !tested; x++) {
                size_t offset = 4 * (x + y * 5 + z * 25);
                if (d.lut.data[offset + 3] > .5f) {
                    float rgb[3] = {(float)x / 4, (float)y / 4, (float)z / 4}, shown[3];
                    pc_engine_display_apply(&d, rgb, shown);
                    CHECK_NEAR(shown[0], d.lut.data[offset] * .245f + .755f, .00001);
                    CHECK_NEAR(shown[1], d.lut.data[offset + 1] * .245f, .00001);
                    CHECK_NEAR(shown[2], d.lut.data[offset + 2] * .245f + .755f, .00001);
                    tested = true;
                }
            }
        }
    }
    CHECK(tested);
    pc_engine_display_fini(&d);
    pc_engine_session_free(s);
}

static void test_hdr_survives_other_sessions(void)
{
    pc_engine_session *first = session(32);
    execute(first, "view.thirtyTwoBitPreviewOptions", "{\"exposure\":2}");
    for (unsigned i = 0; i < 260; i++) {
        pc_engine_session *other = session(32);
        execute(other, "view.thirtyTwoBitPreviewOptions", "{}");
        pc_engine_session_free(other);
    }
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(first, "view.thirtyTwoBitPreviewOptions", "{}", &out, NULL) ==
          PC_OK);
    CHECK_NEAR(yyjson_mut_get_num(yyjson_mut_obj_get(pc_json_value_root(&out), "exposure")), 2, 0);
    pc_json_value_fini(&out);
    pc_engine_session_free(first);
}

int main(int argc, char **argv)
{
    RUN(test_identity_and_hdr);
    RUN(test_dichromat_and_simulations);
    RUN(test_linear_source);
    RUN(test_gamut_preferences_and_key);
    RUN(test_hdr_survives_other_sessions);
    return pc_t_finish();
}
