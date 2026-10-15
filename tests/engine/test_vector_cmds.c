/* E11 vector command specs (vector_specs.c): shape.*, path.*, layer.vectorMask.* behaviour plus
 * an adversarial-params sweep (no command crashes on hostile input, REWRITE_PLAN.md rule 9). */
#include <stdlib.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "harness.h"

static pc_engine_session *mk(uint32_t w, uint32_t h)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "U", pc_size_make(w, h), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8,
                                      pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

static pc_status ex(pc_engine_session *s, const char *id, const char *json, pc_json_value *out)
{
    return pc_engine_session_execute_json(s, id, json, out, NULL);
}

static const char *str_of(pc_json_value *out, const char *key)
{
    yyjson_mut_val *v = yyjson_mut_obj_get(pc_json_value_root(out), key);
    return v != NULL ? yyjson_mut_get_str(v) : NULL;
}

static pc_layer_id create_shape(pc_engine_session *s, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "shape.create", json, &out) == PC_OK);
    yyjson_mut_val *l = yyjson_mut_obj_get(pc_json_value_root(&out), "layer");
    pc_layer_id id = (pc_layer_id)yyjson_mut_get_uint(l);
    pc_json_value_fini(&out);
    return id;
}

static void test_create_kinds(void)
{
    pc_engine_session *s = mk(200, 200);
    struct {
        const char *json;
        const char *kind;
    } cases[] = {
        {"{\"kind\":\"rect\",\"rect\":[10,10,50,40]}", "rect"},
        {"{\"kind\":\"roundedRect\",\"rect\":[10,10,50,40],\"radii\":10}", "roundedRect"},
        {"{\"kind\":\"ellipse\",\"rect\":[20,20,100,60]}", "ellipse"},
        {"{\"kind\":\"polygon\",\"rect\":[0,0,100,100],\"sides\":6}", "polygon"},
        {"{\"kind\":\"star\",\"rect\":[0,0,100,100],\"sides\":5,\"starRatio\":0.5}", "star"},
        {"{\"kind\":\"line\",\"from\":[10,100],\"to\":[110,100],\"weight\":4}", "line"},
        {"{\"kind\":\"path\",\"path\":{\"subpaths\":[{\"closed\":true,\"knots\":[[0,0],[60,0],[0,60]]}]}}", "path"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pc_json_value out = PC_JSON_VALUE_INIT;
        CHECK(ex(s, "shape.create", cases[i].json, &out) == PC_OK);
        CHECK_EQ_STR(str_of(&out, "kind"), cases[i].kind);
        pc_json_value_fini(&out);
    }
    /* A kind with no geometry, and an unknown kind, are rejected (not crashes). */
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "shape.create", "{\"kind\":\"rect\"}", &out) != PC_OK);
    CHECK(ex(s, "shape.create", "{\"kind\":\"blob\",\"rect\":[0,0,1,1]}", &out) != PC_OK);
    pc_engine_session_free(s);
}

static void test_edit_live(void)
{
    pc_engine_session *s = mk(200, 200);
    pc_layer_id id = create_shape(s, "{\"kind\":\"rect\",\"rect\":[10,10,50,50]}");
    char buf[128];
    pc_json_value out = PC_JSON_VALUE_INIT;
    snprintf(buf, sizeof buf, "{\"layer\":%llu,\"radii\":[20,0,20,0]}", (unsigned long long)id);
    CHECK(ex(s, "shape.edit", buf, &out) == PC_OK);
    CHECK_EQ_STR(str_of(&out, "kind"), "roundedRect");
    pc_json_value_fini(&out);
    /* Move keeps the live shape; a rotation drops it. */
    snprintf(buf, sizeof buf, "{\"layer\":%llu,\"move\":[5,7]}", (unsigned long long)id);
    CHECK(ex(s, "shape.edit", buf, &out) == PC_OK);
    CHECK_EQ_STR(str_of(&out, "kind"), "roundedRect");
    pc_json_value_fini(&out);
    snprintf(buf, sizeof buf, "{\"layer\":%llu,\"transform\":[1,0.3,0.3,1,0,0]}", (unsigned long long)id);
    CHECK(ex(s, "shape.edit", buf, &out) == PC_OK);
    CHECK_EQ_STR(str_of(&out, "kind"), "path");
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_document_paths(void)
{
    pc_engine_session *s = mk(100, 100);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "path.set", "{\"name\":\"work\",\"path\":{\"subpaths\":[{\"knots\":[[0,0],[10,0],[10,10]]}]}}", &out) ==
          PC_OK);
    pc_json_value_fini(&out);
    CHECK(ex(s, "path.info", "{\"name\":\"work\"}", &out) == PC_OK);
    pc_json_value_fini(&out);
    /* Rename the work path -> saved. */
    CHECK(ex(s, "path.rename", "{\"name\":\"work\",\"to\":\"P1\"}", &out) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(ex(s, "path.list", "{}", &out) == PC_OK);
    CHECK(yyjson_mut_arr_size(yyjson_mut_obj_get(pc_json_value_root(&out), "paths")) == 1);
    pc_json_value_fini(&out);
    CHECK(ex(s, "path.delete", "{\"name\":\"P1\"}", &out) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(ex(s, "path.delete", "{\"name\":\"P1\"}", &out) != PC_OK); /* already gone */
    pc_engine_session_free(s);
}

static void test_vector_mask(void)
{
    pc_engine_session *s = mk(100, 100);
    pc_layer_id bg = pc_engine_session_active(s)->active_layer;
    char buf[256];
    pc_json_value out = PC_JSON_VALUE_INIT;
    snprintf(buf, sizeof buf,
             "{\"layer\":%llu,\"path\":{\"subpaths\":[{\"knots\":[[0,0],[50,0],[50,50]]}]}}",
             (unsigned long long)bg);
    CHECK(ex(s, "layer.vectorMask.add", buf, &out) == PC_OK);
    pc_json_value_fini(&out);
    snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)bg);
    CHECK(ex(s, "layer.vectorMask.info", buf, &out) == PC_OK);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&out), "enabled")));
    pc_json_value_fini(&out);
    /* Toggle enabled off. */
    CHECK(ex(s, "layer.vectorMask.enabled", buf, &out) == PC_OK);
    CHECK(!yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&out), "enabled")));
    pc_json_value_fini(&out);
    CHECK(ex(s, "layer.vectorMask.delete", buf, &out) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(ex(s, "layer.vectorMask.delete", buf, &out) != PC_OK); /* already gone */
    pc_engine_session_free(s);
}

static void test_clipboard_and_combine(void)
{
    pc_engine_session *s = mk(200, 200);
    create_shape(s, "{\"kind\":\"rect\",\"rect\":[10,10,50,40],\"fill\":\"#ff0000\","
                    "\"stroke\":{\"width\":3,\"color\":\"#0000ff\"}}");
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "path.style.copyFill", "{}", &out) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(ex(s, "path.style.copyStroke", "{}", &out) == PC_OK);
    pc_json_value_fini(&out);
    pc_layer_id b = create_shape(s, "{\"kind\":\"ellipse\",\"rect\":[60,60,40,40]}");
    char buf[64];
    snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)b);
    CHECK(ex(s, "path.style.pasteFill", buf, &out) == PC_OK);
    CHECK_EQ_STR(str_of(&out, "fill"), "#ff0000");
    pc_json_value_fini(&out);
    CHECK(ex(s, "path.style.pasteStroke", buf, &out) == PC_OK);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_transform_and_selection(void)
{
    pc_engine_session *s = mk(100, 100);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "path.set", "{\"name\":\"work\",\"path\":{\"subpaths\":[{\"knots\":[[0,0],[40,0],[40,40],[0,40]]}]}}",
             &out) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(ex(s, "path.transform", "{\"name\":\"work\",\"matrix\":[1,0,0,1,10,5]}", &out) == PC_OK);
    CHECK(yyjson_mut_obj_get(pc_json_value_root(&out), "matrix") != NULL);
    pc_json_value_fini(&out);
    /* work path -> selection -> work path round trip. */
    CHECK(ex(s, "path.toSelection", "{\"name\":\"work\"}", &out) == PC_OK);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&out), "selected")));
    pc_json_value_fini(&out);
    CHECK(ex(s, "select.toWorkPath", "{\"tolerance\":2}", &out) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(ex(s, "select.convertToShape", "{\"tolerance\":2}", &out) == PC_OK);
    CHECK_EQ_STR(str_of(&out, "kind"), "path");
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

/* Every registered id with a spread of hostile params must return a status (never crash). */
static void test_adversarial(void)
{
    static const char *const IDS[] = {
        "shape.create",
        "shape.edit",
        "shape.info",
        "shape.rasterize",
        "path.list",
        "path.info",
        "path.set",
        "path.delete",
        "path.transform",
        "path.clippingPath.set",
        "path.clippingPath.clear",
        "path.style.copyFill",
        "path.style.copyStroke",
        "path.style.pasteFill",
        "path.style.pasteStroke",
        "path.rename",
        "path.toSelection",
        "select.toWorkPath",
        "path.fill",
        "layer.vectorMask.add",
        "layer.vectorMask.fromPath",
        "layer.vectorMask.edit",
        "layer.vectorMask.delete",
        "layer.rasterize.vectorMask",
        "layer.vectorMask.revealAll",
        "layer.vectorMask.hideAll",
        "layer.vectorMask.currentPath",
        "layer.vectorMask.enabled",
        "layer.vectorMask.linked",
        "layer.combineShapes.unite",
        "layer.combineShapes.subtractFrontShape",
        "layer.combineShapes.intersectShapeAreas",
        "layer.combineShapes.excludeOverlappingShapes",
        "layer.combineShapes.mergeShapeComponents",
        "select.convertToShape",
        "layer.vectorMask.info",
    };
    static const char *const HOSTILE[] = {
        "{}",
        "null",
        "[]",
        "{\"layer\":\"x\",\"rect\":\"y\",\"path\":5,\"name\":7,\"radii\":\"q\"}",
        "{\"rect\":[1e400,-1e400,0,0],\"sides\":1e9,\"weight\":-5,\"starRatio\":9}",
        "{\"path\":{\"subpaths\":99},\"stroke\":{\"width\":-3,\"dashes\":[\"a\"]}}",
        "{\"layer\":999999,\"matrix\":[1,2,3],\"quad\":[[0,0]],\"flatness\":500}",
        "{\"to\":\"\",\"op\":\"nope\",\"feather\":1e9,\"mode\":42,\"fill\":{\"gradient\":{\"stops\":7}}}",
        "{\"layer\":0,\"density\":99999,\"feather\":-1,\"subpath\":4000000000}",
    };
    for (size_t i = 0; i < sizeof IDS / sizeof IDS[0]; i++) {
        /* Once on an empty session (no document), once with a document + a shape layer. */
        pc_engine_session *empty = NULL;
        CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
        pc_engine_session *doc = mk(80, 80);
        create_shape(doc, "{\"kind\":\"rect\",\"rect\":[5,5,30,30]}");
        for (size_t j = 0; j < sizeof HOSTILE / sizeof HOSTILE[0]; j++) {
            pc_json_value o1 = PC_JSON_VALUE_INIT, o2 = PC_JSON_VALUE_INIT;
            pc_status r1 = ex(empty, IDS[i], HOSTILE[j], &o1);
            CHECK(r1 == PC_OK || r1 != PC_OK); /* reached here = no crash */
            if (r1 == PC_OK) {
                pc_json_value_fini(&o1);
            }
            pc_status r2 = ex(doc, IDS[i], HOSTILE[j], &o2);
            CHECK(r2 == PC_OK || r2 != PC_OK);
            if (r2 == PC_OK) {
                pc_json_value_fini(&o2);
            }
        }
        pc_engine_session_free(empty);
        pc_engine_session_free(doc);
    }
}

int main(int argc, char **argv)
{
    RUN(test_create_kinds);
    RUN(test_edit_live);
    RUN(test_document_paths);
    RUN(test_vector_mask);
    RUN(test_clipboard_and_combine);
    RUN(test_transform_and_selection);
    RUN(test_adversarial);
    return pc_t_finish();
}
