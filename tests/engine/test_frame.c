/* Layer > New > Frame (tool) and Frame from Layers. Port of crates/engine/src/frame_cmds.rs
 * tests (E12). */
#include "doc/doc.h"
#include "engine/engine_internal.h"
#include "harness.h"

static pc_status ex(pc_engine_session *s, const char *id, const char *p)
{
    pc_json_value o = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, id, p, &o, NULL);
    pc_json_value_fini(&o);
    return rc;
}

static pc_engine_session *session(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(ex(s, "file.new", "{\"width\":80,\"height\":60}") == PC_OK);
    return s;
}

static void test_new_frame_clips_group(void)
{
    pc_engine_session *s = session();
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    size_t before = pc_doc_arc_get(d->doc)->nlayers;
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(
              s, "layer.new.frame", "{\"x\":10,\"y\":10,\"width\":40,\"height\":30}", &out, NULL) ==
          PC_OK);
    uint64_t gid = yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "layer"));
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(pc_json_value_root(&out), "shape")), "rectangle");
    pc_json_value_fini(&out);
    d = pc_engine_session_active(s);
    const pc_layer *g = pc_document_layer(pc_doc_arc_get(d->doc), gid);
    CHECK(g != NULL && g->content.kind == PC_LAYER_GROUP);
    CHECK(g->content.u.group.nchildren == 0); /* empty frame */
    CHECK(g->has_mask);
    CHECK_NEAR(pc_layer_mask_value(&g->mask, 10, 10), 1.0, 1e-6);
    CHECK_NEAR(pc_layer_mask_value(&g->mask, 49, 39), 1.0, 1e-6);
    CHECK_NEAR(pc_layer_mask_value(&g->mask, 9, 10), 0.0, 1e-6);
    CHECK_NEAR(pc_layer_mask_value(&g->mask, 50, 39), 0.0, 1e-6);
    CHECK(d->has_active_layer && d->active_layer == gid);
    CHECK(pc_engine_session_undo(s));
    d = pc_engine_session_active(s);
    CHECK_EQ_UINT(pc_doc_arc_get(d->doc)->nlayers, before);

    /* ellipse frame over a pixel layer */
    CHECK(ex(s, "layer.new.layer", "{}") == PC_OK);
    d = pc_engine_session_active(s);
    pc_layer_id a = d->active_layer;
    char buf[160];
    snprintf(buf, sizeof buf,
             "{\"x\":10,\"y\":10,\"width\":40,\"height\":30,\"shape\":\"ellipse\",\"layer\":%llu}",
             (unsigned long long)a);
    CHECK(pc_engine_session_execute_json(s, "layer.new.frame", buf, &out, NULL) == PC_OK);
    gid = yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "layer"));
    pc_json_value_fini(&out);
    d = pc_engine_session_active(s);
    g = pc_document_layer(pc_doc_arc_get(d->doc), gid);
    CHECK(g != NULL && g->content.kind == PC_LAYER_GROUP && g->content.u.group.nchildren == 1);
    CHECK_NEAR(pc_layer_mask_value(&g->mask, 30, 25), 1.0, 1e-6); /* centre shown */
    CHECK_NEAR(pc_layer_mask_value(&g->mask, 10, 10), 0.0, 1e-6); /* corner hidden */
    pc_engine_session_free(s);
}

static void test_bad_frame_params(void)
{
    pc_engine_session *s = session();
    const char *bad[] = {"{\"x\":0,\"y\":0,\"width\":0,\"height\":5}",
                         "{\"x\":0,\"y\":0,\"width\":5,\"height\":5,\"shape\":\"star\"}",
                         "{\"x\":0}",
                         "[]",
                         "null",
                         "{\"x\":2147483647,\"y\":0,\"width\":5,\"height\":5}"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(ex(s, "layer.new.frame", bad[i]) != PC_OK);
    }
    pc_engine_session_free(s);
}

static void test_frame_from_layers(void)
{
    pc_engine_session *s = session();
    CHECK(ex(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(ex(s, "layer.new.layer", "{}") == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "layer.new.frameFromLayers", "{\"name\":\"Frame 1\"}", &out,
                                         NULL) == PC_OK);
    uint64_t gid = yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "layer"));
    pc_json_value_fini(&out);
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_layer *g = pc_document_layer(pc_doc_arc_get(d->doc), gid);
    CHECK(g != NULL && g->content.kind == PC_LAYER_GROUP && g->has_mask);
    pc_engine_session_free(s);
}

static void test_needs_selected_layer(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(ex(s, "layer.new.frameFromLayers", "{}") != PC_OK);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_new_frame_clips_group);
    RUN(test_bad_frame_params);
    RUN(test_frame_from_layers);
    RUN(test_needs_selected_layer);
    return pc_t_finish();
}
