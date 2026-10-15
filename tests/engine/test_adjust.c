/* Image > Adjustments and Layer > New Adjustment Layer commands: a representative apply of each,
 * the adjustment-layer / setAdjustment / destructive-only paths, and an adversarial-params sweep
 * over every registered E8 adjustment command (must never crash; run under ASan/UBSan). */
#include <string.h>

#include <yyjson.h>

#include "doc/doc.h"
#include "engine/engine.h"
#include "harness.h"

static pc_status paint_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *ctx, pc_error *err)
{
    (void)ha;
    (void)active;
    (void)ctx;
    pc_surface *surf = pc_layer_surface_mut(&doc->layers[0]);
    CHECK(surf != NULL);
    size_t ch = pc_surface_channels(surf);
    float white[5] = {1, 1, 1, 1, 1};
    float red[5] = {0.8f, 0.2f, 0.1f, 1, 1};
    float blue[5] = {0.1f, 0.2f, 0.8f, 1, 1};
    CHECK(pc_surface_fill_rect(surf, pc_document_bounds(doc), white, ch, err) == PC_OK);
    CHECK(pc_surface_fill_rect(surf, pc_rect_make(0, 0, 16, 16), red, ch, err) == PC_OK);
    CHECK(pc_surface_fill_rect(surf, pc_rect_make(16, 0, 32, 16), blue, ch, err) == PC_OK);
    return PC_OK;
}

static pc_engine_session *rgb_session(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(
              s, "file.new", "{\"width\":64,\"height\":32,\"depth\":8,\"mode\":\"rgb\"}", &out, NULL) ==
          PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_edit(s, "paint", paint_edit, NULL, NULL) == PC_OK);
    return s;
}

/* The composite pixel at (x, y) as RGBA. */
static void pixel_at(pc_engine_session *s, int x, int y, float out[4])
{
    char buf[64];
    snprintf(buf, sizeof buf, "{\"x\":%d,\"y\":%d}", x, y);
    pc_json_value r;
    CHECK(pc_engine_session_execute_json(s, "document.pixel", buf, &r, NULL) == PC_OK);
    yyjson_mut_val *arr = pc_json_value_root(&r);
    for (int i = 0; i < 4; i++) {
        yyjson_mut_val *v = yyjson_mut_arr_get(arr, (size_t)i);
        out[i] = v != NULL ? (float)yyjson_mut_get_real(v) : 0.0f;
    }
    pc_json_value_fini(&r);
}

static size_t layer_count(pc_engine_session *s)
{
    return pc_doc_arc_get(pc_engine_session_active(s)->doc)->nlayers;
}

static void test_invert_changes_pixels(void)
{
    pc_engine_session *s = rgb_session();
    float before[4];
    pixel_at(s, 0, 0, before);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "image.adjustments.invert", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    float after[4];
    pixel_at(s, 0, 0, after);
    /* Invert turns the red band (0.8,0.2,0.1) toward (0.2,0.8,0.9). */
    CHECK(after[0] < before[0] && after[2] > before[2]);
    pc_engine_session_free(s);
}

static void test_brightness_applies(void)
{
    pc_engine_session *s = rgb_session();
    float before[4];
    pixel_at(s, 20, 0, before); /* blue band */
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "image.adjustments.brightnessContrast",
                                         "{\"brightness\":80}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    float after[4];
    pixel_at(s, 20, 0, after);
    CHECK(after[0] >= before[0] && after[1] >= before[1]);
    pc_engine_session_free(s);
}

static void test_new_adjustment_layer(void)
{
    pc_engine_session *s = rgb_session();
    size_t before = layer_count(s);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "layer.newAdjustmentLayer.levels",
                                         "{\"inBlack\":20,\"gamma\":1.5}", &out, NULL) == PC_OK);
    yyjson_mut_val *layer = yyjson_mut_obj_get(pc_json_value_root(&out), "layer");
    CHECK(layer != NULL);
    pc_json_value_fini(&out);
    CHECK(layer_count(s) == before + 1);
    /* The new layer is an adjustment layer. */
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    CHECK(doc->layers[doc->nlayers - 1].content.kind == PC_LAYER_ADJUSTMENT);
    /* layer.setAdjustment edits it. */
    CHECK(pc_engine_session_execute_json(s, "layer.setAdjustment", "{\"gamma\":2.0}", &out, NULL) ==
          PC_OK);
    pc_json_value_fini(&out);
    /* No params resets to defaults. */
    CHECK(pc_engine_session_execute_json(s, "layer.setAdjustment", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_new_adjustment_layer_masks_selection(void)
{
    pc_engine_session *s = rgb_session();
    pc_json_value out;
    /* Select a rectangle, then a new adjustment layer takes it as its mask. */
    CHECK(pc_engine_session_execute_json(s, "select.rect",
                                         "{\"x\":0,\"y\":0,\"width\":10,\"height\":10}", &out,
                                         NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(s, "layer.newAdjustmentLayer.invert", "{}", &out, NULL) ==
          PC_OK);
    pc_json_value_fini(&out);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    CHECK(doc->layers[doc->nlayers - 1].has_mask);
    pc_engine_session_free(s);
}

static void test_destructive_only(void)
{
    const char *cases[] = {
        "image.adjustments.desaturate",        "{}",
        "image.adjustments.shadowsHighlights", "{\"shadowAmount\":50}",
        "image.adjustments.replaceColor",      "{\"color\":\"#cc3319\",\"hue\":40}",
        "image.adjustments.hdrToning",         "{\"gamma\":1.2}",
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i += 2) {
        pc_engine_session *s = rgb_session();
        pc_json_value out;
        pc_error err = {0};
        pc_status rc = pc_engine_session_execute_json(s, cases[i], cases[i + 1], &out, &err);
        if (rc != PC_OK) {
            PC_T_FAIL("%s failed: %s", cases[i], err.msg);
        } else {
            pc_json_value_fini(&out);
        }
        pc_engine_session_free(s);
    }
}

static void test_color_lookup_list(void)
{
    pc_engine_session *s = rgb_session();
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "image.adjustments.colorLookup.list", "{}", &out, NULL) ==
          PC_OK);
    yyjson_mut_val *arr = pc_json_value_root(&out);
    CHECK(yyjson_mut_arr_size(arr) == 8);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_match_color_source(void)
{
    pc_engine_session *s = rgb_session();
    /* Match the (only) document to itself: a no-op path that must still succeed. */
    pc_json_value out;
    pc_error err = {0};
    pc_status rc = pc_engine_session_execute_json(s, "image.adjustments.matchColor",
                                                  "{\"source\":0,\"luminance\":120}", &out, &err);
    if (rc != PC_OK) {
        PC_T_FAIL("matchColor failed: %s", err.msg);
    } else {
        pc_json_value_fini(&out);
    }
    pc_engine_session_free(s);
}

/* An adjustment id is one this chunk owns. */
static bool is_adjust_id(const char *id)
{
    return strncmp(id, "image.adjustments.", 18) == 0 ||
           strncmp(id, "layer.newAdjustmentLayer.", 25) == 0 ||
           strcmp(id, "layer.setAdjustment") == 0;
}

static void test_adversarial_params(void)
{
    static const char *const PARAMS[] = {
        "{}",
        "null",
        "[]",
        "123",
        "\"x\"",
        "{\"brightness\":\"x\",\"levels\":-5,\"level\":1e30}",
        "{\"points\":\"abc\",\"red\":[[0,0],[1]]}",
        "{\"stops\":7,\"color\":[1],\"filter\":\"nope\"}",
        "{\"inBlack\":1e30,\"inWhite\":-1e30,\"gamma\":0}",
        "{\"source\":-9,\"sourceLayer\":99,\"curve\":\"no\"}",
        "{\"layer\":999999}",
        "{\"method\":5,\"colors\":\"nope\"}",
        "{\"pencil\":true,\"points\":[1,2,3],\"smooth\":1e9}",
        "{\"target\":\"quickMask\"}",
        "{\"target\":\"mask\"}",
        "{\"eyedropper\":{\"point\":\"black\",\"at\":[1,1]}}",
    };
    size_t n = 0;
    const pc_engine_command *all = pc_engine_commands(&n);
    size_t fuzzed = 0;
    for (size_t i = 0; i < n; i++) {
        if (!is_adjust_id(all[i].id)) {
            continue;
        }
        /* A fresh session per id so one command's edit can't mask another. */
        for (size_t j = 0; j < sizeof PARAMS / sizeof PARAMS[0]; j++) {
            pc_engine_session *s = rgb_session();
            pc_json_value out;
            pc_error err = {0};
            pc_status rc = pc_engine_session_execute_json(s, all[i].id, PARAMS[j], &out, &err);
            if (rc == PC_OK) {
                pc_json_value_fini(&out);
            }
            fuzzed++;
            pc_engine_session_free(s);
        }
    }
    CHECK(fuzzed > 0);
}

int main(int argc, char **argv)
{
    RUN(test_invert_changes_pixels);
    RUN(test_brightness_applies);
    RUN(test_new_adjustment_layer);
    RUN(test_new_adjustment_layer_masks_selection);
    RUN(test_destructive_only);
    RUN(test_color_lookup_list);
    RUN(test_match_color_source);
    RUN(test_adversarial_params);
    return pc_t_finish();
}
