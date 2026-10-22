/* Editing fill layers and querying content options (fill_layer_set_cmds.c):
 *   layer.fill.solidColor.set, layer.fill.pattern.set, layer.layerContentOptions.
 * Behavioural (the edit changes the composite and undo restores it) and graceful-failure
 * (adversarial params and wrong layer types are errors, never a crash, rule 9). */
#include <string.h>

#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine/engine.h"
#include "engine/engine_internal.h"
#include "engine/fill_layer_set_cmds.h"
#include "harness.h"

static pc_engine_session *mk(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(16, 16), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

static pc_status run(pc_engine_session *s, const char *id, const char *params, pc_json_value *out)
{
    pc_error e = {0};
    return pc_engine_session_execute_json(s, id, params, out, &e);
}

static pc_status run_void(pc_engine_session *s, const char *id, const char *params)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status rc = run(s, id, params, &out);
    pc_json_value_fini(&out);
    return rc;
}

static uint64_t new_layer(pc_engine_session *s, const char *id, const char *params)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(run(s, id, params, &out) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    uint64_t lid = 0;
    if (root != NULL && yyjson_mut_is_obj(root)) {
        yyjson_mut_val *l = yyjson_mut_obj_get(root, "layer");
        if (l != NULL) {
            lid = yyjson_mut_get_uint(l);
        }
    }
    pc_json_value_fini(&out);
    return lid;
}

/* Composite pixel at (x, y) into out[3] (RGB). */
static void comp_px(pc_engine_session *s, int x, int y, float out[3])
{
    char buf[64];
    (void)snprintf(buf, sizeof buf, "{\"x\":%d,\"y\":%d}", x, y);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(run(s, "document.pixel", buf, &o) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&o);
    for (int i = 0; i < 3; i++) {
        out[i] = 0.0f;
        if (root != NULL) {
            yyjson_mut_val *e = yyjson_mut_arr_get(root, (size_t)i);
            if (e != NULL) {
                out[i] = (float)yyjson_mut_get_real(e);
            }
        }
    }
    pc_json_value_fini(&o);
}

static void test_registry(void)
{
    size_t n = 0;
    (void)pc_engine_fill_layer_set_specs(&n);
    CHECK_EQ_UINT(n, 3);
    CHECK(pc_engine_command_find("layer.fill.solidColor.set") != NULL);
    CHECK(pc_engine_command_find("layer.fill.pattern.set") != NULL);
    CHECK(pc_engine_command_find("layer.layerContentOptions") != NULL);
}

static void test_solid_set_and_undo(void)
{
    pc_engine_session *s = mk();
    uint64_t id = new_layer(s, "layer.newFillLayer.solidColor", "{\"color\":\"#112233\"}");
    CHECK(id != 0);
    float px[3];
    comp_px(s, 8, 8, px);
    CHECK(px[0] > 0.05f && px[0] < 0.10f); /* ~0x11/255 */

    /* Change the colour: the composite follows it. */
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(run(s, "layer.fill.solidColor.set", "{\"color\":\"#cc8844\"}", &out) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    CHECK(root != NULL && yyjson_mut_is_obj(root));
    yyjson_mut_val *col = root != NULL ? yyjson_mut_obj_get(root, "color") : NULL;
    CHECK(col != NULL && yyjson_mut_is_obj(col));
    pc_json_value_fini(&out);
    comp_px(s, 8, 8, px);
    CHECK(px[0] > 0.75f); /* ~0xcc/255 */

    /* Undo restores the original colour. */
    CHECK(pc_engine_session_undo(s));
    comp_px(s, 8, 8, px);
    CHECK(px[0] > 0.05f && px[0] < 0.10f);
    CHECK(pc_engine_session_redo(s));
    comp_px(s, 8, 8, px);
    CHECK(px[0] > 0.75f);

    /* Re-applying the same colour is a graceful no-op. */
    CHECK(run_void(s, "layer.fill.solidColor.set", "{\"color\":\"#cc8844\"}") == PC_OK);
    comp_px(s, 8, 8, px);
    CHECK(px[0] > 0.75f);

    pc_engine_session_free(s);
}

static void test_solid_rgb_array_and_stored_color(void)
{
    pc_engine_session *s = mk();
    (void)new_layer(s, "layer.newFillLayer.solidColor", "{}");
    CHECK(run_void(s, "layer.fill.solidColor.set", "{\"color\":[0.25,0.5,0.75]}") == PC_OK);
    float px[3];
    comp_px(s, 8, 8, px);
    CHECK(px[0] > 0.2f && px[0] < 0.3f);
    CHECK(px[2] > 0.7f && px[2] < 0.8f);
    /* A stored Color object keeps its model. */
    CHECK(run_void(s, "layer.fill.solidColor.set",
                   "{\"color\":{\"mode\":\"Rgb\",\"c\":[0.1,0.2,0.3,0.0],\"alpha\":1.0}}") == PC_OK);
    pc_engine_session_free(s);
}

static void test_solid_graceful(void)
{
    pc_engine_session *empty = NULL;
    CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
    CHECK(run_void(empty, "layer.fill.solidColor.set", "{\"color\":\"#ffffff\"}") != PC_OK);
    pc_engine_session_free(empty);

    pc_engine_session *s = mk();
    (void)new_layer(s, "layer.newFillLayer.solidColor", "{}");
    const char *bad[] = {
        "{}",
        "null",
        "{\"color\":true}",
        "{\"color\":[1,\"bad\",0]}",
        "{\"color\":[1,0,0,2]}",
        "{\"color\":[5.0,0,0]}",
        "{\"color\":[0,0]}",
        "{\"color\":\"#zzz\"}",
        "{\"color\":\"#ff\"}",
        "{\"layer\":\"bad\",\"color\":\"#ffffff\"}",
        "{\"layer\":999999,\"color\":\"#ffffff\"}",
        "{\"document\":999999,\"color\":\"#ffffff\"}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(run_void(s, "layer.fill.solidColor.set", bad[i]) != PC_OK);
    }
    /* A pixel layer is not a Solid Color fill. */
    (void)new_layer(s, "layer.new.layer", "{}");
    CHECK(run_void(s, "layer.fill.solidColor.set", "{\"color\":\"#ffffff\"}") != PC_OK);
    pc_engine_session_free(s);
}

static void test_pattern_set_and_undo(void)
{
    pc_engine_session *s = mk();
    uint64_t id = new_layer(s, "layer.newFillLayer.pattern", "{\"pattern\":\"Checkerboard\"}");
    CHECK(id != 0);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(run(s, "layer.fill.pattern.set", "{\"pattern\":\"Bricks\",\"scale\":250,\"angle\":-45,"
                                           "\"link\":false,\"phase\":[3,-4]}",
              &out) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    CHECK(root != NULL && yyjson_mut_is_obj(root));
    yyjson_mut_val *sc = root != NULL ? yyjson_mut_obj_get(root, "scale") : NULL;
    CHECK(sc != NULL && yyjson_mut_get_real(sc) > 249.0 && yyjson_mut_get_real(sc) < 251.0);
    yyjson_mut_val *an = root != NULL ? yyjson_mut_obj_get(root, "angle") : NULL;
    CHECK(an != NULL && yyjson_mut_get_real(an) < -44.0 && yyjson_mut_get_real(an) > -46.0);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_undo(s)); /* back to Checkerboard */
    /* Snap to Origin: omitted values keep the layer's. */
    CHECK(run_void(s, "layer.fill.pattern.set", "{\"phase\":[0,0]}") == PC_OK);
    pc_engine_session_free(s);
}

static void test_pattern_graceful(void)
{
    pc_engine_session *s = mk();
    (void)new_layer(s, "layer.newFillLayer.pattern", "{\"pattern\":\"Checkerboard\"}");
    const char *bad[] = {
        "null", "[1,2]", "{\"layer\":-1}", "{\"layer\":\"x\"}", "{\"layer\":999999}",
        "{\"pattern\":\"nope\"}", "{\"pattern\":\"\"}", "{\"pattern\":5}", "{\"scale\":0}",
        "{\"scale\":1001}", "{\"scale\":\"big\"}", "{\"angle\":181}", "{\"link\":\"yes\"}",
        "{\"phase\":[1]}", "{\"phase\":[1,\"x\"]}", "{\"phase\":[1e30,0]}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(run_void(s, "layer.fill.pattern.set", bad[i]) != PC_OK);
    }
    /* A solid fill is not a pattern fill. */
    (void)new_layer(s, "layer.newFillLayer.solidColor", "{}");
    CHECK(run_void(s, "layer.fill.pattern.set", "{\"scale\":50}") != PC_OK);
    pc_engine_session_free(s);
}

static void test_content_options(void)
{
    pc_engine_session *s = mk();
    (void)new_layer(s, "layer.newFillLayer.solidColor", "{}");
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(run(s, "layer.layerContentOptions", "{}", &out) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    yyjson_mut_val *ed = root != NULL ? yyjson_mut_obj_get(root, "editor") : NULL;
    CHECK(ed != NULL && strcmp(yyjson_mut_get_str(ed), "fill") == 0);
    pc_json_value_fini(&out);

    /* A pixel layer has no content options. */
    (void)new_layer(s, "layer.new.layer", "{}");
    CHECK(run_void(s, "layer.layerContentOptions", "{}") != PC_OK);
    CHECK(!pc_engine_session_is_enabled(s, "layer.layerContentOptions"));
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_registry);
    RUN(test_solid_set_and_undo);
    RUN(test_solid_rgb_array_and_stored_color);
    RUN(test_solid_graceful);
    RUN(test_pattern_set_and_undo);
    RUN(test_pattern_graceful);
    RUN(test_content_options);
    return pc_t_finish();
}
