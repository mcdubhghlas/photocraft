/* Ports the unit tests of the R0-feature resync command modules: layer_list_cmds (layer.list),
 * face_cmds (image.detectFaces, unsupported here), and layer_xmp_cmds (layer.xmpMetadata). */
#include <string.h>

#include "base/json.h"
#include "engine/engine_internal.h"
#include "harness.h"

static pc_engine_session *new_session(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    return s;
}

static pc_status exec(pc_engine_session *s, const char *id, const char *params)
{
    pc_json_value out = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, params, &out, NULL);
    if (rc == PC_OK) {
        pc_json_value_fini(&out);
    }
    return rc;
}

static const pc_document *active_doc(pc_engine_session *s)
{
    return pc_doc_arc_get(pc_engine_session_active(s)->doc);
}

/* ---- layer.list ---------------------------------------------------------------------------- */

static void test_layer_list_tree_and_flat(void)
{
    pc_engine_session *s = new_session();
    CHECK(exec(s, "file.new", "{\"width\":64,\"height\":48}") == PC_OK);
    pc_layer_id bg = pc_engine_session_active(s)->active_layer;

    pc_json_value nl = {0};
    CHECK(pc_engine_session_execute_json(s, "layer.new.layer", "{}", &nl, NULL) == PC_OK);
    uint64_t inner = yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&nl), "layer"));
    pc_json_value_fini(&nl);
    CHECK(exec(s, "edit.fill", "{\"color\":\"#ff0000\"}") == PC_OK);
    CHECK(exec(s, "layer.groupLayers", "{}") == PC_OK);
    const pc_document *doc = active_doc(s);
    pc_layer_id group = doc->layers[doc->nlayers - 1].id; /* top row */

    char buf[64];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)inner);
    CHECK(exec(s, "layer.select", buf) == PC_OK);
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu,\"visible\":false}", (unsigned long long)bg);
    CHECK(exec(s, "layer.setProps", buf) == PC_OK);

    /* Tree. */
    pc_json_value out = {0};
    CHECK(pc_engine_session_execute_json(s, "layer.list", "{}", &out, NULL) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    yyjson_mut_val *layers = yyjson_mut_obj_get(root, "layers");
    CHECK_EQ_UINT(yyjson_mut_arr_size(layers), 2u);
    yyjson_mut_val *l0 = yyjson_mut_arr_get(layers, 0);
    CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(l0, "id")), group);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(l0, "kind")), "Group");
    yyjson_mut_val *kids = yyjson_mut_obj_get(l0, "children");
    CHECK(kids != NULL && yyjson_mut_arr_size(kids) == 1u);
    yyjson_mut_val *k0 = yyjson_mut_arr_get(kids, 0);
    CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(k0, "id")), (uint64_t)inner);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(k0, "active")));
    CHECK(yyjson_mut_is_arr(yyjson_mut_obj_get(k0, "bounds")));
    yyjson_mut_val *l1 = yyjson_mut_arr_get(layers, 1);
    CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(l1, "id")), bg);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(l1, "name")), "Background");
    CHECK(!yyjson_mut_get_bool(yyjson_mut_obj_get(l1, "visible")));
    CHECK(yyjson_mut_obj_get(l1, "children") == NULL);
    CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(root, "activeLayer")), (uint64_t)inner);
    pc_json_value_fini(&out);

    /* Flat: one row per layer with depth and parent. */
    pc_json_value flat = {0};
    CHECK(pc_engine_session_execute_json(s, "layer.list", "{\"flat\":true}", &flat, NULL) == PC_OK);
    yyjson_mut_val *rows = yyjson_mut_obj_get(pc_json_value_root(&flat), "layers");
    CHECK_EQ_UINT(yyjson_mut_arr_size(rows), 3u);
    yyjson_mut_val *r0 = yyjson_mut_arr_get(rows, 0);
    CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(r0, "id")), group);
    CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(r0, "depth")), 0u);
    CHECK(yyjson_mut_is_null(yyjson_mut_obj_get(r0, "parent")));
    yyjson_mut_val *r1 = yyjson_mut_arr_get(rows, 1);
    CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(r1, "id")), (uint64_t)inner);
    CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(r1, "depth")), 1u);
    CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(r1, "parent")), group);
    yyjson_mut_val *r2 = yyjson_mut_arr_get(rows, 2);
    CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(r2, "id")), bg);
    CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(r2, "depth")), 0u);
    pc_json_value_fini(&flat);

    pc_engine_session_free(s);
}

static void test_layer_list_is_a_query(void)
{
    pc_engine_session *s = new_session();
    CHECK(exec(s, "file.new", "{\"width\":64,\"height\":48}") == PC_OK);
    uint64_t rev = pc_engine_session_active(s)->revision;
    CHECK(exec(s, "layer.list", "{}") == PC_OK);
    CHECK_EQ_UINT(pc_engine_session_active(s)->revision, rev);
    pc_engine_session_free(s);
}

static void test_layer_list_bad_params(void)
{
    pc_engine_session *s = new_session();
    CHECK(exec(s, "layer.list", "{}") != PC_OK); /* no document */
    CHECK(exec(s, "file.new", "{\"width\":64,\"height\":48}") == PC_OK);
    CHECK(exec(s, "layer.list", "{\"flat\":\"yes\"}") != PC_OK);
    CHECK(exec(s, "layer.list", "{\"document\":-1}") != PC_OK);
    CHECK(exec(s, "layer.list", "{\"document\":99}") != PC_OK);
    CHECK(exec(s, "layer.list", "{\"document\":\"a\"}") != PC_OK);
    CHECK(exec(s, "layer.list", "{\"document\":0,\"flat\":false}") == PC_OK);
    pc_engine_session_free(s);
}

/* ---- image.detectFaces (unsupported) ------------------------------------------------------- */

static void test_detect_faces_unsupported(void)
{
    pc_engine_session *s = new_session();
    CHECK(exec(s, "image.detectFaces", "{}") != PC_OK); /* no doc / unsupported */
    CHECK(!pc_engine_session_is_enabled(s, "image.detectFaces"));
    CHECK(exec(s, "file.new", "{\"width\":64,\"height\":48,\"background\":\"white\"}") == PC_OK);
    CHECK(!pc_engine_session_is_enabled(s, "image.detectFaces"));
    pc_error err = {0};
    pc_json_value out = {0};
    pc_status rc = pc_engine_session_execute_json(s, "image.detectFaces", "{}", &out, &err);
    CHECK(rc != PC_OK); /* unsupported: no face detector in this build */
    if (rc == PC_OK) {
        pc_json_value_fini(&out);
    }
    /* Odd params still don't crash. */
    CHECK(exec(s, "image.detectFaces", "{\"minScore\":2}") != PC_OK);
    CHECK(exec(s, "image.detectFaces", "{\"sampleAllLayers\":1}") != PC_OK);
    pc_engine_session_free(s);
}

/* ---- layer.xmpMetadata --------------------------------------------------------------------- */

static const char *PACKET =
    "<?xpacket begin=\"\"?><x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
    "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"></rdf:RDF></x:xmpmeta>";

static void test_layer_xmp_roundtrip_and_validation(void)
{
    pc_engine_session *s = new_session();
    CHECK(exec(s, "file.new", "{\"width\":16,\"height\":16}") == PC_OK);

    /* Read: no packet yet. */
    pc_json_value out = {0};
    CHECK(pc_engine_session_execute_json(s, "layer.xmpMetadata", "{}", &out, NULL) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    CHECK(yyjson_mut_is_null(yyjson_mut_obj_get(root, "rawData")));
    CHECK(!yyjson_mut_get_bool(yyjson_mut_obj_get(root, "changed")));
    pc_json_value_fini(&out);

    /* Set a valid packet. */
    pc_buf param = PC_BUF_INIT;
    (void)pc_buf_append_str(&param, "{\"rawData\":", NULL);
    /* Encode PACKET as a JSON string via yyjson. */
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *sv = yyjson_mut_strcpy(d, PACKET);
    yyjson_mut_doc_set_root(d, sv);
    pc_buf js = PC_BUF_INIT;
    CHECK(pc_json_write(yyjson_mut_doc_get_root(d), 0, &js, NULL) == PC_OK);
    (void)pc_buf_append_str(&param, pc_buf_cstr(&js), NULL);
    (void)pc_buf_append_str(&param, "}", NULL);
    yyjson_mut_doc_free(d);
    pc_buf_fini(&js);

    pc_json_value set = {0};
    CHECK(pc_engine_session_execute_json(s, "layer.xmpMetadata", pc_buf_cstr(&param), &set, NULL) == PC_OK);
    root = pc_json_value_root(&set);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(root, "changed")));
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(root, "rawData")), PACKET);
    pc_json_value_fini(&set);

    /* Read back; changed false. */
    pc_json_value rd = {0};
    CHECK(pc_engine_session_execute_json(s, "layer.xmpMetadata", "{}", &rd, NULL) == PC_OK);
    root = pc_json_value_root(&rd);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(root, "rawData")), PACKET);
    CHECK(!yyjson_mut_get_bool(yyjson_mut_obj_get(root, "changed")));
    pc_json_value_fini(&rd);

    /* Set the same again: not changed. */
    pc_json_value again = {0};
    CHECK(pc_engine_session_execute_json(s, "layer.xmpMetadata", pc_buf_cstr(&param), &again, NULL) == PC_OK);
    CHECK(!yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&again), "changed")));
    pc_json_value_fini(&again);
    pc_buf_fini(&param);

    /* Remove with null. */
    pc_json_value del = {0};
    CHECK(pc_engine_session_execute_json(s, "layer.xmpMetadata", "{\"rawData\":null}", &del, NULL) == PC_OK);
    root = pc_json_value_root(&del);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(root, "changed")));
    CHECK(yyjson_mut_is_null(yyjson_mut_obj_get(root, "rawData")));
    pc_json_value_fini(&del);

    /* Invalid packets / params are errors, not crashes. */
    CHECK(exec(s, "layer.xmpMetadata", "{\"rawData\":\"<not-xmp/>\"}") != PC_OK);
    CHECK(exec(s, "layer.xmpMetadata", "{\"rawData\":\"plain text\"}") != PC_OK);
    CHECK(exec(s, "layer.xmpMetadata", "{\"rawData\":42}") != PC_OK);
    CHECK(exec(s, "layer.xmpMetadata", "{\"layer\":\"a\"}") != PC_OK);
    CHECK(exec(s, "layer.xmpMetadata", "{\"layer\":99999}") != PC_OK);

    pc_engine_session_free(s);
}

static void test_layer_xmp_no_document(void)
{
    pc_engine_session *s = new_session();
    CHECK(exec(s, "layer.xmpMetadata", "{}") != PC_OK);
    CHECK(!pc_engine_session_is_enabled(s, "layer.xmpMetadata"));
    pc_engine_session_free(s);
}

/* ---- history.stepForward / stepBackward (+ clear / setSource unsupported) ------------------ */

static void test_history_step(void)
{
    pc_engine_session *s = new_session();
    CHECK(exec(s, "file.new", "{\"width\":20,\"height\":10}") == PC_OK);
    CHECK(exec(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(exec(s, "edit.fill", "{\"color\":\"#ff0000\"}") == PC_OK);
    CHECK(exec(s, "edit.fill", "{\"color\":\"#00ff00\"}") == PC_OK);

    /* At the last state: nothing to step forward to, something to step back to. */
    CHECK(!pc_engine_session_is_enabled(s, "history.stepForward"));
    CHECK(pc_engine_session_is_enabled(s, "history.stepBackward"));
    CHECK(exec(s, "history.stepForward", "{}") != PC_OK);

    pc_json_value out = {0};
    CHECK(pc_engine_session_execute_json(s, "history.stepBackward", "{}", &out, NULL) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    uint64_t states = yyjson_mut_get_uint(yyjson_mut_obj_get(root, "states"));
    uint64_t current = yyjson_mut_get_uint(yyjson_mut_obj_get(root, "current"));
    CHECK(states >= 1u);
    CHECK(current + 1u <= states);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_is_enabled(s, "history.stepForward"));

    /* Step back to the oldest, then a step too far is a refused error, not a crash. */
    int guard = 0;
    while (pc_engine_session_is_enabled(s, "history.stepBackward") && guard++ < 100) {
        CHECK(exec(s, "history.stepBackward", "{}") == PC_OK);
    }
    CHECK(exec(s, "history.stepBackward", "{}") != PC_OK);

    /* Clear History and setSource are registered but unsupported in this build. */
    CHECK(exec(s, "history.clear", "{}") != PC_OK);
    CHECK(exec(s, "history.setSource", "{\"state\":0}") != PC_OK);
    pc_engine_session_free(s);

    /* No document: the step commands are disabled and error. */
    pc_engine_session *empty = new_session();
    CHECK(!pc_engine_session_is_enabled(empty, "history.stepForward"));
    CHECK(!pc_engine_session_is_enabled(empty, "history.stepBackward"));
    CHECK(exec(empty, "history.stepForward", "{}") != PC_OK);
    CHECK(exec(empty, "history.stepBackward", "{}") != PC_OK);
    CHECK(exec(empty, "history.clear", "{}") != PC_OK);
    pc_engine_session_free(empty);
}

int main(int argc, char **argv)
{
    RUN(test_layer_list_tree_and_flat);
    RUN(test_layer_list_is_a_query);
    RUN(test_layer_list_bad_params);
    RUN(test_detect_faces_unsupported);
    RUN(test_layer_xmp_roundtrip_and_validation);
    RUN(test_layer_xmp_no_document);
    RUN(test_history_step);
    return pc_t_finish();
}
