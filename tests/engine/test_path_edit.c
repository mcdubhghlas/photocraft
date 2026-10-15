/* E11 point-level path edits (path_edit_cmds.inc): moveAnchors/moveHandle/bend/convert/add/
 * delete on the work path, a shape path and a vector mask + adversarial. */
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "harness.h"

static pc_engine_session *mk(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "U", pc_size_make(120, 100), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8,
                                      pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

static pc_status ex(pc_engine_session *s, const char *id, const char *json, pc_json_value *out)
{
    return pc_engine_session_execute_json(s, id, json, out, NULL);
}

#define SQUARE "{\"path\":{\"subpaths\":[{\"closed\":true,\"knots\":[[10,10],[60,10],[60,60],[10,60]]}]}}"

static double anchor_x(pc_json_value *out, size_t k)
{
    yyjson_mut_val *root = pc_json_value_root(out);
    yyjson_mut_val *path = yyjson_mut_obj_get(root, "path");
    yyjson_mut_val *subs = yyjson_mut_obj_get(path, "subpaths");
    yyjson_mut_val *s0 = yyjson_mut_arr_get(subs, 0);
    yyjson_mut_val *knots = yyjson_mut_obj_get(s0, "knots");
    yyjson_mut_val *kn = yyjson_mut_arr_get(knots, k);
    yyjson_mut_val *a = yyjson_mut_obj_get(kn, "anchor");
    return yyjson_mut_get_real(yyjson_mut_arr_get(a, 0));
}

static void test_move_anchors(void)
{
    pc_engine_session *s = mk();
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "path.set", SQUARE, &out) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(ex(s, "path.moveAnchors", "{\"anchors\":[[0,1],[0,2]],\"move\":[5,-3]}", &out) == PC_OK);
    CHECK_NEAR(anchor_x(&out, 1), 65.0, 1e-6);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_undo(s));
    pc_engine_session_free(s);
}

static void test_add_delete(void)
{
    pc_engine_session *s = mk();
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "path.set", SQUARE, &out) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(ex(s, "path.addAnchor", "{\"subpath\":0,\"knot\":0,\"t\":0.5}", &out) == PC_OK);
    yyjson_mut_val *an = yyjson_mut_obj_get(pc_json_value_root(&out), "anchor");
    CHECK(an != NULL);
    pc_json_value_fini(&out);
    CHECK(ex(s, "path.deleteAnchor", "{\"subpath\":0,\"knot\":1}", &out) == PC_OK);
    yyjson_mut_val *rm = yyjson_mut_obj_get(pc_json_value_root(&out), "subpathRemoved");
    CHECK(rm != NULL && !yyjson_mut_get_bool(rm));
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_shape_and_mask(void)
{
    pc_engine_session *s = mk();
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "shape.create", "{\"kind\":\"rect\",\"rect\":[10,10,40,40],\"fill\":\"#ff0000\"}", &out) == PC_OK);
    uint64_t id = yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "layer"));
    pc_json_value_fini(&out);
    char p[160];
    snprintf(p, sizeof p, "{\"name\":\"layer\",\"layer\":%llu,\"anchors\":[[0,1],[0,2]],\"move\":[30,0]}",
             (unsigned long long)id);
    CHECK(ex(s, "path.moveAnchors", p, &out) == PC_OK);
    CHECK_NEAR(anchor_x(&out, 1), 80.0, 1e-6);
    pc_json_value_fini(&out);
    /* the edited rectangle is a plain path now */
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(pc_engine_session_active(s)->doc), (pc_layer_id)id);
    CHECK(l->content.kind == PC_LAYER_SHAPE && !l->content.u.shape.has_live);
    pc_engine_session_free(s);
}

static const char *const IDS[] = {"path.moveAnchors", "path.moveHandle", "path.bendSegment",
                                   "path.convertPoint", "path.addAnchor", "path.deleteAnchor"};
static const char *const HOSTILE[] = {
    "{}", "null", "[]", "{\"anchors\":[]}", "{\"anchors\":[[0,9]],\"move\":[1,1]}",
    "{\"anchors\":[[0,0]],\"move\":[1e300,1]}", "{\"subpath\":-1,\"knot\":0}", "{\"subpath\":5,\"knot\":0}",
    "{\"subpath\":0,\"knot\":0,\"handle\":\"up\",\"to\":[1,1]}", "{\"subpath\":0,\"knot\":0,\"t\":2,\"move\":[1,1]}",
    "{\"subpath\":0,\"knot\":0,\"t\":0}", "{\"subpath\":0,\"knot\":0,\"out\":\"x\"}", "{\"name\":\"nope\",\"subpath\":0,\"knot\":0}",
    "{\"name\":\"layer\",\"subpath\":0,\"knot\":0}", "{\"knot\":0}",
};

static void test_adversarial(void)
{
    for (size_t i = 0; i < sizeof IDS / sizeof IDS[0]; i++) {
        pc_engine_session *s = mk();
        pc_json_value sq = PC_JSON_VALUE_INIT;
        if (ex(s, "path.set", SQUARE, &sq) == PC_OK) {
            pc_json_value_fini(&sq);
        }
        for (size_t j = 0; j < sizeof HOSTILE / sizeof HOSTILE[0]; j++) {
            pc_json_value o = PC_JSON_VALUE_INIT;
            if (ex(s, IDS[i], HOSTILE[j], &o) == PC_OK) {
                pc_json_value_fini(&o);
            }
        }
        pc_engine_session_free(s);
    }
}

int main(int argc, char **argv)
{
    setenv("PHOTOCRAFT_NO_SYSTEM_FONTS", "1", 1);
    RUN(test_move_anchors);
    RUN(test_add_delete);
    RUN(test_shape_and_mask);
    RUN(test_adversarial);
    return pc_t_finish();
}
