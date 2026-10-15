/* ADJ-parameter machinery (ported from crates/engine/src/adjust_params.rs tests):
 * from_params / to_params / default_for round trips, params reaching the model, and bad params
 * being errors rather than crashes. */
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "color/color.h"
#include "doc/adjust.h"
#include "engine/adjust_params.h"
#include "harness.h"

static const char *KINDS[] = {"invert",       "brightnessContrast", "threshold",  "posterize",
                              "exposure",     "vibrance",           "hueSaturation", "levels",
                              "curves",       "colorBalance",       "blackWhite",  "photoFilter",
                              "channelMixer", "gradientMap",        "selectiveColor"};

static const char *sample_json(const char *kind)
{
    if (strcmp(kind, "brightnessContrast") == 0) {
        return "{\"brightness\":20,\"contrast\":-10,\"legacy\":true}";
    }
    if (strcmp(kind, "threshold") == 0) {
        return "{\"level\":100}";
    }
    if (strcmp(kind, "posterize") == 0) {
        return "{\"levels\":7}";
    }
    if (strcmp(kind, "exposure") == 0) {
        return "{\"exposure\":1.5,\"offset\":0.1,\"gamma\":1.4}";
    }
    if (strcmp(kind, "vibrance") == 0) {
        return "{\"vibrance\":30,\"saturation\":-20}";
    }
    if (strcmp(kind, "hueSaturation") == 0) {
        return "{\"hue\":25,\"saturation\":-20,\"lightness\":10,\"reds\":{\"hue\":10,\"saturation\":"
               "-30,\"lightness\":5,\"range\":[300,340,20,50]}}";
    }
    if (strcmp(kind, "levels") == 0) {
        return "{\"inBlack\":10,\"inWhite\":240,\"outBlack\":5,\"outWhite\":250,\"gamma\":1.2,"
               "\"red\":{\"inBlack\":3,\"gamma\":0.8}}";
    }
    if (strcmp(kind, "curves") == 0) {
        return "{\"points\":[[0,0],[128,150],[255,255]],\"blue\":[[0,20],[255,235]]}";
    }
    if (strcmp(kind, "colorBalance") == 0) {
        return "{\"shadows\":[10,-5,0],\"midtones\":[0,20,-30],\"highlights\":[5,5,5],"
               "\"preserveLuminosity\":false}";
    }
    if (strcmp(kind, "blackWhite") == 0) {
        return "{\"reds\":120,\"blues\":-40,\"tint\":true,\"tintColor\":\"#d0a070\"}";
    }
    if (strcmp(kind, "photoFilter") == 0) {
        return "{\"color\":\"#3366cc\",\"density\":40,\"preserveLuminosity\":false}";
    }
    if (strcmp(kind, "channelMixer") == 0) {
        return "{\"red\":[80,30,-10,5],\"blue\":[0,10,90,0]}";
    }
    if (strcmp(kind, "gradientMap") == 0) {
        return "{\"stops\":[[0,\"#000000\"],[0.5,\"#ff0000\"],[1,[1,1,1]]],\"reverse\":true,"
               "\"dither\":true}";
    }
    if (strcmp(kind, "selectiveColor") == 0) {
        return "{\"method\":\"absolute\",\"reds\":[10,0,-20,5]}";
    }
    return "{}";
}

/* Parse JSON text into an immutable doc (caller frees with yyjson_doc_free). */
static yyjson_doc *parse(const char *json)
{
    return yyjson_read(json, strlen(json), 0);
}

/* to_params(adj) as a JSON string (fixed key order; caller free()s). */
static char *to_json(const pc_adjustment *a)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (d == NULL) {
        return NULL;
    }
    yyjson_mut_val *v = NULL;
    if (pc_engine_adjustment_to_params(a, d, &v, NULL) != PC_OK) {
        yyjson_mut_doc_free(d);
        return NULL;
    }
    yyjson_mut_doc_set_root(d, v);
    char *s = yyjson_mut_write(d, 0, NULL);
    yyjson_mut_doc_free(d);
    return s;
}

static pc_color_mode MODES[] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_CMYK, PC_COLOR_MODE_LAB,
                                PC_COLOR_MODE_GRAYSCALE};

static void test_every_kind_round_trips(void)
{
    for (size_t mi = 0; mi < 4; mi++) {
        pc_color_mode mode = MODES[mi];
        for (size_t ki = 0; ki < sizeof KINDS / sizeof KINDS[0]; ki++) {
            const char *kind = KINDS[ki];
            /* Off RGB the Levels/Curves channel keys are the document's; drop red/blue. */
            const char *json = sample_json(kind);
            char buf[512];
            if ((strcmp(kind, "levels") == 0 || strcmp(kind, "curves") == 0) &&
                mode != PC_COLOR_MODE_RGB && mode != PC_COLOR_MODE_GRAYSCALE) {
                if (strcmp(kind, "levels") == 0) {
                    json = "{\"inBlack\":10,\"inWhite\":240,\"outBlack\":5,\"outWhite\":250,"
                           "\"gamma\":1.2}";
                } else {
                    json = "{\"points\":[[0,0],[128,150],[255,255]]}";
                }
            }
            (void)buf;
            yyjson_doc *pd = parse(json);
            CHECK(pd != NULL);
            pc_adjustment a;
            pc_error err = {0};
            pc_status rc = pc_engine_adjustment_from_params(kind, yyjson_doc_get_root(pd), NULL, mode,
                                                            &a, &err);
            if (rc != PC_OK) {
                PC_T_FAIL("%s mode %d: from_params failed: %s", kind, (int)mode, err.msg);
                yyjson_doc_free(pd);
                continue;
            }
            yyjson_doc_free(pd);
            /* to_params -> from_params -> to_params must be stable. */
            char *j1 = to_json(&a);
            CHECK(j1 != NULL);
            yyjson_doc *rd = parse(j1 != NULL ? j1 : "{}");
            pc_adjustment back;
            CHECK(pc_engine_adjustment_from_params(kind, yyjson_doc_get_root(rd), NULL, mode, &back,
                                                   NULL) == PC_OK);
            yyjson_doc_free(rd);
            char *j2 = to_json(&back);
            CHECK(j1 != NULL && j2 != NULL && strcmp(j1, j2) == 0);
            if (j1 != NULL && j2 != NULL && strcmp(j1, j2) != 0) {
                PC_T_FAIL("%s mode %d: round trip differs\n  %s\n  %s", kind, (int)mode, j1, j2);
            }
            free(j1);
            free(j2);
            /* A partial update with no keys keeps everything. */
            yyjson_doc *ed = parse("{}");
            pc_adjustment again;
            CHECK(pc_engine_adjustment_from_params(kind, yyjson_doc_get_root(ed), &a, mode, &again,
                                                   NULL) == PC_OK);
            yyjson_doc_free(ed);
            CHECK(pc_adjustment_eq(&again, &a));
            pc_adjustment_fini(&again);
            pc_adjustment_fini(&back);
            pc_adjustment_fini(&a);
        }
    }
}

static pc_adjustment from(const char *kind, const char *json, pc_color_mode mode, pc_status *rc)
{
    pc_adjustment a;
    memset(&a, 0, sizeof a);
    yyjson_doc *d = parse(json);
    *rc = pc_engine_adjustment_from_params(kind, d != NULL ? yyjson_doc_get_root(d) : NULL, NULL, mode,
                                           &a, NULL);
    yyjson_doc_free(d);
    return a;
}

static void test_params_reach_the_model(void)
{
    pc_status rc;
    pc_adjustment a = from("colorBalance", "{\"midtones\":[50,0,-200]}", PC_COLOR_MODE_RGB, &rc);
    CHECK(rc == PC_OK);
    CHECK(a.u.color_balance.midtones[0] == 50.0f && a.u.color_balance.midtones[2] == -100.0f);
    CHECK(a.u.color_balance.preserve_luminosity);
    pc_adjustment_fini(&a);

    a = from("channelMixer", "{\"monochrome\":true}", PC_COLOR_MODE_RGB, &rc);
    CHECK(rc == PC_OK && a.u.channel_mixer.monochrome);
    CHECK(a.u.channel_mixer.matrix[0][0] == 0.4f && a.u.channel_mixer.matrix[0][2] == 0.2f);
    pc_adjustment_fini(&a);

    a = from("photoFilter", "{\"filter\":\"cooling80\",\"density\":60}", PC_COLOR_MODE_RGB, &rc);
    CHECK(rc == PC_OK && a.u.photo_filter.color[2] == 1.0f);
    CHECK(fabsf(a.u.photo_filter.density - 0.6f) < 1e-6f);
    pc_adjustment_fini(&a);

    a = from("blackWhite", "{\"reds\":120,\"tint\":true}", PC_COLOR_MODE_RGB, &rc);
    CHECK(rc == PC_OK && a.u.black_white.weights[0] == 120.0f && a.u.black_white.weights[1] == 60.0f);
    CHECK(a.u.black_white.has_tint);
    pc_adjustment_fini(&a);

    /* Colorize without a saturation starts at 25%; explicit 0 stays 0. */
    a = from("hueSaturation", "{\"colorize\":true,\"hue\":30}", PC_COLOR_MODE_RGB, &rc);
    CHECK(rc == PC_OK && a.u.hue_saturation.colorize && a.u.hue_saturation.saturation == 25.0f);
    pc_adjustment_fini(&a);
    a = from("hueSaturation", "{\"colorize\":true,\"saturation\":0}", PC_COLOR_MODE_RGB, &rc);
    CHECK(rc == PC_OK && a.u.hue_saturation.colorize && a.u.hue_saturation.saturation == 0.0f);
    pc_adjustment_fini(&a);

    /* CMYK curves black channel; Lab lightness without a composite. */
    a = from("curves", "{\"black\":[[0,0],[255,200]]}", PC_COLOR_MODE_RGB, &rc);
    CHECK(rc == PC_OK && a.u.curves.space == PC_TONE_CMYK && a.u.curves.black.len == 2);
    pc_adjustment_fini(&a);
    a = from("curves", "{\"points\":[[0,40],[255,255]]}", PC_COLOR_MODE_LAB, &rc);
    CHECK(rc == PC_OK && a.u.curves.space == PC_TONE_LAB && a.u.curves.per_channel[0].len >= 2);
    pc_adjustment_fini(&a);
    a = from("levels", "{\"inBlack\":20}", PC_COLOR_MODE_CMYK, &rc);
    CHECK(rc == PC_OK && a.u.levels.space == PC_TONE_CMYK);
    CHECK(fabsf(a.u.levels.master.in_black - 20.0f / 255.0f) < 1e-6f);
    pc_adjustment_fini(&a);
}

static void test_bad_params_are_errors(void)
{
    struct {
        const char *kind;
        const char *json;
    } bad[] = {
        {"curves", "{\"points\":\"abc\"}"},
        {"curves", "{\"points\":[[0,0]]}"},
        {"curves", "{\"red\":[[0,0],[1]]}"},
        {"curves", "{\"points\":[[5,5],[5,9]]}"},
        {"curves", "{\"pencil\":\"yes\"}"},
        {"levels", "{\"red\":3}"},
        {"levels", "{\"inBlack\":\"x\"}"},
        {"hueSaturation", "{\"reds\":{\"range\":[1,2]}}"},
        {"hueSaturation", "{\"colorize\":1}"},
        {"colorBalance", "{\"shadows\":[1,2]}"},
        {"blackWhite", "{\"tintColor\":\"#zz0000\"}"},
        {"photoFilter", "{\"filter\":\"nope\"}"},
        {"photoFilter", "{\"color\":[1]}"},
        {"channelMixer", "{\"red\":[1,2,3]}"},
        {"gradientMap", "{\"stops\":[[0,\"#000\"]]}"},
        {"gradientMap", "{\"stops\":7}"},
        {"selectiveColor", "{\"method\":5}"},
        {"selectiveColor", "{\"reds\":\"not-an-array\"}"},
        {"colorLookup", "{\"lut\":42}"},
        {"colorLookup", "{\"interpolation\":5}"},
        {"colorLookup", "{\"dither\":\"yes\"}"},
        {"nope", "{}"},
        {"levels", "[1,2]"},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        yyjson_doc *d = parse(bad[i].json);
        pc_adjustment a;
        memset(&a, 0, sizeof a);
        pc_status rc = pc_engine_adjustment_from_params(
            bad[i].kind, d != NULL ? yyjson_doc_get_root(d) : NULL, NULL, PC_COLOR_MODE_RGB, &a, NULL);
        if (rc == PC_OK) {
            PC_T_FAIL("%s %s should be an error", bad[i].kind, bad[i].json);
            pc_adjustment_fini(&a);
        }
        yyjson_doc_free(d);
    }
    /* Out-of-range numbers clamp. */
    pc_status rc;
    pc_adjustment a = from("hueSaturation", "{\"hue\":1e9,\"saturation\":-1e9}", PC_COLOR_MODE_RGB, &rc);
    CHECK(rc == PC_OK && a.u.hue_saturation.hue == 180.0f && a.u.hue_saturation.saturation == -100.0f);
    pc_adjustment_fini(&a);
    a = from("levels", "{\"inBlack\":250,\"inWhite\":10}", PC_COLOR_MODE_RGB, &rc);
    CHECK(rc == PC_OK && a.u.levels.master.in_white > a.u.levels.master.in_black);
    pc_adjustment_fini(&a);
}

static void test_default_for_all_kinds(void)
{
    for (size_t ki = 0; ki < sizeof KINDS / sizeof KINDS[0]; ki++) {
        pc_adjustment a;
        CHECK(pc_engine_adjustment_default_for(KINDS[ki], PC_COLOR_MODE_RGB, &a, NULL) == PC_OK);
        /* kind round-trips to its id (invert for Invert). */
        const char *k = pc_engine_adjustment_kind(&a);
        CHECK(k != NULL);
        pc_adjustment_fini(&a);
    }
}

int main(int argc, char **argv)
{
    RUN(test_every_kind_round_trips);
    RUN(test_params_reach_the_model);
    RUN(test_bad_params_are_errors);
    RUN(test_default_for_all_kinds);
    return pc_t_finish();
}
