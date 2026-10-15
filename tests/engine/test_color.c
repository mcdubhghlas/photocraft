/* Colour management: Image > Mode conversions, Assign/Convert to Profile, Color Settings, proofing
 * and gamut warning, plus the exported conversion primitives (ported from the reachable parts of
 * crates/engine/src/color_cmds.rs tests; image.mode.* is E3, so conversions run through
 * edit.convertToProfile and pc_engine_color_convert_mode). */
#include <math.h>
#include <string.h>

#include "cms/builtin.h"
#include "cms/profile.h"
#include "color/color.h"
#include "doc/doc.h"
#include "raster/surface.h"
#include "engine/engine_internal.h"
#include "engine/prefs.h"
#include "harness.h"

static const pc_document *doc_of(pc_engine_session *s)
{
    return pc_doc_arc_get(pc_engine_session_active(s)->doc);
}

/* Fill the background layer: white, with red/blue/gray bands in the top-left. */
static pc_status paint_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *ctx, pc_error *err)
{
    (void)ha;
    (void)active;
    (void)ctx;
    pc_layer *l = &doc->layers[0];
    pc_surface *surf = pc_layer_surface_mut(l);
    CHECK(surf != NULL);
    size_t ch = pc_surface_channels(surf);
    float white[5] = {1, 1, 1, 1, 1};
    float red[5] = {1, 0, 0, 1, 1};
    float blue[5] = {0, 0, 1, 1, 1};
    float gray[5] = {0.5f, 0.5f, 0.5f, 1, 1};
    CHECK(pc_surface_fill_rect(surf, pc_document_bounds(doc), white, ch, err) == PC_OK);
    CHECK(pc_surface_fill_rect(surf, pc_rect_make(0, 0, 16, 16), red, ch, err) == PC_OK);
    CHECK(pc_surface_fill_rect(surf, pc_rect_make(16, 0, 32, 16), blue, ch, err) == PC_OK);
    CHECK(pc_surface_fill_rect(surf, pc_rect_make(32, 0, 48, 16), gray, ch, err) == PC_OK);
    return PC_OK;
}

static pc_engine_session *rgb_session(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(
              s, "file.new", "{\"width\":64,\"height\":32,\"depth\":8,\"mode\":\"rgb\"}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_edit(s, "paint", paint_edit, NULL, NULL) == PC_OK);
    return s;
}

static const char *str_at(pc_json_value *r, const char *key)
{
    return yyjson_mut_get_str(yyjson_mut_obj_get(pc_json_value_root(r), key));
}

/* Image > Mode conversions through the exported primitive (what E3 image.mode.* calls). */
static void test_convert_mode_primitive(void)
{
    int depths[] = {8, 16, 32};
    for (size_t di = 0; di < 3; di++) {
        pc_engine_session *s = NULL;
        CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
        char buf[128];
        snprintf(buf, sizeof buf, "{\"width\":64,\"height\":32,\"depth\":%d,\"mode\":\"rgb\"}", depths[di]);
        pc_json_value o;
        CHECK(pc_engine_session_execute_json(s, "file.new", buf, &o, NULL) == PC_OK);
        pc_json_value_fini(&o);
        CHECK(pc_engine_session_edit(s, "paint", paint_edit, NULL, NULL) == PC_OK);

        pc_json_value r;
        pc_error e;
        CHECK(pc_engine_color_convert_mode(s, PC_COLOR_MODE_CMYK, NULL, &r, &e) == PC_OK);
        pc_json_value_fini(&r);
        const pc_document *d = doc_of(s);
        CHECK(d->mode == PC_COLOR_MODE_CMYK);
        CHECK(d->icc_profile != NULL);
        CHECK(pc_surface_format(pc_layer_surface(&d->layers[0])).mode == PC_COLOR_MODE_CMYK);
        /* Red -> heavy magenta + yellow, little cyan. */
        float cmyk[5];
        pc_surface_read_pixel(pc_layer_surface(&d->layers[0]), 8, 8, cmyk);
        CHECK(cmyk[1] > 0.7f && cmyk[2] > 0.7f && cmyk[0] < 0.2f);
        /* White background -> no ink. */
        float bg[5];
        pc_surface_read_pixel(pc_layer_surface(&d->layers[0]), 60, 30, bg);
        CHECK(bg[0] < 0.01f && bg[1] < 0.01f && bg[2] < 0.01f && bg[3] < 0.01f);

        CHECK(pc_engine_color_convert_mode(s, PC_COLOR_MODE_LAB, NULL, &r, &e) == PC_OK);
        pc_json_value_fini(&r);
        CHECK(doc_of(s)->mode == PC_COLOR_MODE_LAB);
        CHECK(pc_engine_color_convert_mode(s, PC_COLOR_MODE_GRAYSCALE, NULL, &r, &e) == PC_OK);
        pc_json_value_fini(&r);
        CHECK(doc_of(s)->mode == PC_COLOR_MODE_GRAYSCALE);
        CHECK(pc_engine_color_convert_mode(s, PC_COLOR_MODE_RGB, NULL, &r, &e) == PC_OK);
        pc_json_value_fini(&r);
        CHECK(doc_of(s)->mode == PC_COLOR_MODE_RGB);

        /* Undo all four conversions returns to RGB. */
        for (int i = 0; i < 4; i++) {
            CHECK(pc_engine_session_undo(s));
        }
        CHECK(doc_of(s)->mode == PC_COLOR_MODE_RGB);
        pc_engine_session_free(s);
    }
}

static void test_assign_and_convert_profile(void)
{
    pc_engine_session *s = rgb_session();
    pc_json_value r;
    CHECK(pc_engine_session_execute_json(s, "edit.assignProfile", "{\"profile\":\"display-p3\"}", &r, NULL) ==
          PC_OK);
    pc_json_value_fini(&r);
    pc_profile dp;
    CHECK(pc_engine_document_profile(doc_of(s), &dp, NULL) == PC_OK);
    CHECK_CONTAINS(dp.description, "Display P3");
    pc_profile_fini(&dp);
    /* Assign does not change pixels. */
    float px[5];
    pc_surface_read_pixel(pc_layer_surface(&doc_of(s)->layers[0]), 8, 8, px);
    CHECK_NEAR(px[0], 1.0f, 1e-6f);
    CHECK_NEAR(px[1], 0.0f, 1e-6f);
    /* Wrong colour space is rejected. */
    CHECK(pc_engine_session_execute_json(s, "edit.assignProfile", "{\"profile\":\"coated-cmyk\"}", &r,
                                         NULL) != PC_OK);
    /* Convert to sRGB: P3 red clips to sRGB red. */
    CHECK(pc_engine_session_execute_json(s, "edit.convertToProfile",
                                         "{\"profile\":\"srgb\",\"intent\":\"relative\"}", &r, NULL) == PC_OK);
    pc_json_value_fini(&r);
    pc_surface_read_pixel(pc_layer_surface(&doc_of(s)->layers[0]), 8, 8, px);
    CHECK(px[0] > 0.99f && px[1] < 0.01f);
    /* Convert to CMYK changes the mode. */
    CHECK(pc_engine_session_execute_json(s, "edit.convertToProfile",
                                         "{\"profile\":\"coated-cmyk\",\"intent\":\"perceptual\"}", &r,
                                         NULL) == PC_OK);
    CHECK_EQ_STR(str_at(&r, "mode"), "Cmyk");
    pc_json_value_fini(&r);
    CHECK(doc_of(s)->mode == PC_COLOR_MODE_CMYK);
    /* profileInfo reports the current space. */
    CHECK(pc_engine_session_execute_json(s, "edit.profileInfo", "{}", &r, NULL) == PC_OK);
    yyjson_mut_val *pinfo = yyjson_mut_obj_get(pc_json_value_root(&r), "profile");
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(pinfo, "colorSpace")), "Cmyk");
    yyjson_mut_val *bi = yyjson_mut_obj_get(pc_json_value_root(&r), "builtins");
    CHECK(yyjson_mut_arr_size(bi) >= 10);
    pc_json_value_fini(&r);
    /* Unknown profile is rejected. */
    CHECK(pc_engine_session_execute_json(s, "edit.convertToProfile", "{\"profile\":\"nope\"}", &r, NULL) !=
          PC_OK);
    /* Missing profile is a BadParams error. */
    CHECK(pc_engine_session_execute_json(s, "edit.assignProfile", "{}", &r, NULL) != PC_OK);
    /* Assign none clears the profile. */
    CHECK(pc_engine_session_execute_json(s, "edit.assignProfile", "{\"profile\":\"none\"}", &r, NULL) ==
          PC_OK);
    pc_json_value_fini(&r);
    CHECK(doc_of(s)->icc_profile == NULL);
    pc_engine_session_free(s);
}

static void test_color_settings(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value r;
    CHECK(pc_engine_session_execute_json(s, "edit.colorSettings", "{}", &r, NULL) == PC_OK);
    yyjson_mut_val *set = yyjson_mut_obj_get(pc_json_value_root(&r), "settings");
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(set, "workingRgb")), "srgb");
    pc_json_value_fini(&r);
    /* An RGB working space must be RGB; a bad policy/intent is rejected. */
    CHECK(pc_engine_session_execute_json(s, "edit.colorSettings", "{\"workingRgb\":\"coated-cmyk\"}", &r,
                                         NULL) != PC_OK);
    CHECK(pc_engine_session_execute_json(s, "edit.colorSettings", "{\"policyRgb\":\"sometimes\"}", &r,
                                         NULL) != PC_OK);
    CHECK(pc_engine_session_execute_json(s, "edit.colorSettings", "{\"intent\":\"vivid\"}", &r, NULL) !=
          PC_OK);
    CHECK(pc_engine_session_execute_json(
              s, "edit.colorSettings",
              "{\"workingRgb\":\"adobe-rgb-compat\",\"intent\":\"perceptual\",\"bpc\":false}", &r, NULL) ==
          PC_OK);
    yyjson_mut_val *wk = yyjson_mut_obj_get(pc_json_value_root(&r), "working");
    CHECK_CONTAINS(yyjson_mut_get_str(yyjson_mut_obj_get(wk, "rgb")), "Adobe RGB");
    pc_json_value_fini(&r);
    CHECK(pc_color_settings_intent(&s->color.settings) == PC_INTENT_PERCEPTUAL);
    /* The reply reports a headless monitor (sRGB fallback, no displays). */
    CHECK(pc_engine_session_execute_json(s, "edit.colorSettings", "{}", &r, NULL) == PC_OK);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&r), "monitorDetected")) == false);
    CHECK_CONTAINS(str_at(&r, "monitor"), "sRGB");
    pc_json_value_fini(&r);
    pc_engine_session_free(s);
}

static void test_mode_conversion_uses_working_spaces(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value r;
    CHECK(pc_engine_session_execute_json(s, "edit.colorSettings", "{\"workingRgb\":\"rec2020\"}", &r,
                                         NULL) == PC_OK);
    pc_json_value_fini(&r);
    int depths[] = {8, 16, 32};
    for (size_t i = 0; i < 3; i++) {
        char buf[128];
        snprintf(buf, sizeof buf, "{\"width\":8,\"height\":8,\"depth\":%d,\"mode\":\"gray\"}", depths[i]);
        CHECK(pc_engine_session_execute_json(s, "file.new", buf, &r, NULL) == PC_OK);
        pc_json_value_fini(&r);
        pc_error e;
        CHECK(pc_engine_color_convert_mode(s, PC_COLOR_MODE_RGB, NULL, &r, &e) == PC_OK);
        pc_json_value_fini(&r);
        const pc_document *d = doc_of(s);
        CHECK(d->mode == PC_COLOR_MODE_RGB);
        pc_profile dp;
        CHECK(pc_engine_document_profile(d, &dp, NULL) == PC_OK);
        CHECK_CONTAINS(dp.description, "2020");
        pc_profile_fini(&dp);
    }
    pc_engine_session_free(s);
}

static void test_proofing_and_gamut(void)
{
    pc_engine_session *s = rgb_session();
    pc_doc_id id = doc_of(s)->id;
    pc_json_value r;
    CHECK(pc_engine_session_execute_json(
              s, "view.proofSetup",
              "{\"profile\":\"coated-cmyk\",\"intent\":\"perceptual\",\"simulatePaper\":true}", &r, NULL) ==
          PC_OK);
    pc_json_value_fini(&r);
    /* The proof state was recorded. */
    bool found = false;
    for (size_t i = 0; i < s->color.nproofs; i++) {
        if (s->color.proofs[i].doc == id) {
            found = true;
            CHECK(s->color.proofs[i].view.setup.intent == PC_INTENT_PERCEPTUAL);
            CHECK(s->color.proofs[i].view.setup.simulate_paper == true);
        }
    }
    CHECK(found);
    /* Proof Colors toggles on. */
    CHECK(pc_engine_session_execute_json(s, "view.proofColors", "{}", &r, NULL) == PC_OK);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&r), "on")) == true);
    pc_json_value_fini(&r);
    /* Gamut Warning: the saturated blue band is outside coated CMYK, white/gray are inside, so some
     * but not all pixels are out of gamut. */
    CHECK(pc_engine_session_execute_json(s, "view.gamutWarning", "{\"on\":true}", &r, NULL) == PC_OK);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&r), "on")) == true);
    uint64_t oog = yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&r), "outOfGamut"));
    CHECK(oog > 0);
    CHECK(oog < 64u * 32u);
    pc_json_value_fini(&r);
    pc_engine_session_free(s);
}

static void test_composite_profiles(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value o;
    CHECK(pc_engine_session_execute_json(s, "file.new", "{\"width\":8,\"height\":8,\"mode\":\"gray\"}", &o,
                                         NULL) == PC_OK);
    pc_json_value_fini(&o);
    pc_profile cp;
    CHECK(pc_engine_composite_profile(doc_of(s), &cp, NULL) == PC_OK);
    CHECK(cp.space == PC_CS_RGB); /* gray composites through its gray-as-RGB view */
    pc_profile_fini(&cp);
    CHECK(pc_engine_session_execute_json(s, "file.new", "{\"width\":8,\"height\":8,\"mode\":\"cmyk\"}", &o,
                                         NULL) == PC_OK);
    pc_json_value_fini(&o);
    CHECK(pc_engine_composite_profile(doc_of(s), &cp, NULL) == PC_OK);
    CHECK_CONTAINS(cp.description, "sRGB");
    pc_profile_fini(&cp);
    pc_engine_session_free(s);
}

/* Adversarial params never crash, always a graceful error (Rule 9). */
static void test_adversarial(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value r;
    /* No document: document-gated commands are disabled. */
    CHECK(!pc_engine_session_is_enabled(s, "edit.assignProfile"));
    CHECK(pc_engine_session_execute_json(s, "edit.assignProfile", "{\"profile\":\"srgb\"}", &r, NULL) !=
          PC_OK);
    /* colorSettings works with no document (always enabled). */
    CHECK(pc_engine_session_is_enabled(s, "edit.colorSettings"));
    CHECK(pc_engine_session_execute_json(s, "edit.colorSettings", "{}", &r, NULL) == PC_OK);
    pc_json_value_fini(&r);
    pc_json_value o;
    CHECK(pc_engine_session_execute_json(s, "file.new", "{\"width\":4,\"height\":4}", &o, NULL) == PC_OK);
    pc_json_value_fini(&o);
    const char *bad[] = {
        "{}",
        "{\"profile\":123}",
        "{\"profile\":[]}",
        "null",
        "{\"profile\":\"\"}",
        "{\"profile\":\"/no/such/file.icc\"}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        pc_error e;
        pc_json_value rr;
        /* Any outcome is fine as long as it does not crash; most are errors. */
        if (pc_engine_session_execute_json(s, "edit.convertToProfile", bad[i], &rr, &e) == PC_OK) {
            pc_json_value_fini(&rr);
        }
        if (pc_engine_session_execute_json(s, "edit.assignProfile", bad[i], &rr, &e) == PC_OK) {
            pc_json_value_fini(&rr);
        }
        if (pc_engine_session_execute_json(s, "edit.colorSettings", bad[i], &rr, &e) == PC_OK) {
            pc_json_value_fini(&rr);
        }
        if (pc_engine_session_execute_json(s, "view.gamutWarning", bad[i], &rr, &e) == PC_OK) {
            pc_json_value_fini(&rr);
        }
    }
    /* Unknown proof-mismatch action is a graceful error. */
    CHECK(pc_engine_session_execute_json(s, "color.profileMismatch", "{\"action\":\"explode\"}", &r, NULL) !=
          PC_OK);
    pc_engine_session_free(s);
}

/* colorSettings routes through the preferences: persisted in prefs JSON, and reachable via
 * prefs.get/set/reset (the E2a seam E8 wired). */
static void test_prefs_color_routing(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value r;
    /* prefs.set a colour setting. */
    CHECK(pc_engine_session_execute_json(
              s, "prefs.set", "{\"path\":\"colorSettings.workingRgb\",\"value\":\"adobe-rgb-compat\"}", &r,
              NULL) == PC_OK);
    pc_json_value_fini(&r);
    CHECK_EQ_STR(s->color.settings.working_rgb, "adobe-rgb-compat");
    /* prefs.get reads it back. */
    CHECK(pc_engine_session_execute_json(s, "prefs.get", "{\"path\":\"colorSettings.workingRgb\"}", &r,
                                         NULL) == PC_OK);
    CHECK_EQ_STR(yyjson_mut_get_str(pc_json_value_root(&r)), "adobe-rgb-compat");
    pc_json_value_fini(&r);
    /* An invalid colour setting via prefs.set is rejected (RGB space must be RGB). */
    CHECK(pc_engine_session_execute_json(
              s, "prefs.set", "{\"path\":\"colorSettings.workingRgb\",\"value\":\"coated-cmyk\"}", &r,
              NULL) != PC_OK);
    /* Settings travel with the preferences file. */
    char *json = pc_engine_session_prefs_to_json(s);
    CHECK(json != NULL);
    CHECK_CONTAINS(json, "colorSettings");
    CHECK_CONTAINS(json, "adobe-rgb-compat");
    pc_engine_session *t = NULL;
    CHECK(pc_engine_session_new(&t, NULL) == PC_OK);
    CHECK(pc_engine_session_load_prefs_json(t, json, strlen(json), NULL) == PC_OK);
    CHECK_EQ_STR(t->color.settings.working_rgb, "adobe-rgb-compat");
    free(json);
    /* prefs.reset restores the default. */
    CHECK(pc_engine_session_execute_json(s, "prefs.reset", "{\"path\":\"colorSettings.workingRgb\"}", &r,
                                         NULL) == PC_OK);
    pc_json_value_fini(&r);
    CHECK_EQ_STR(s->color.settings.working_rgb, "srgb");
    pc_engine_session_free(t);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_convert_mode_primitive);
    RUN(test_prefs_color_routing);
    RUN(test_assign_and_convert_profile);
    RUN(test_color_settings);
    RUN(test_mode_conversion_uses_working_spaces);
    RUN(test_proofing_and_gamut);
    RUN(test_composite_profiles);
    RUN(test_adversarial);
    return pc_t_finish();
}
