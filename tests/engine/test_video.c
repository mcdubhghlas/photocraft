/* Layer > Video Layers (frame stack). Port of the frame-stack tests of
 * crates/engine/src/video_cmds/tests.rs (E12). The file-footage loaders and Render Video are
 * deferred (owner: io/codecs); their tests are not ported here. */
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
    CHECK(ex(s, "file.new", "{\"width\":16,\"height\":12}") == PC_OK);
    return s;
}

static size_t frames_of(pc_engine_session *s, pc_layer_id id)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(d->doc), id);
    CHECK(l != NULL && l->has_video);
    return l->video.nframes;
}

static pc_layer_id new_blank(pc_engine_session *s)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "layer.videoLayers.newBlankVideoLayer", "{}", &out, NULL) ==
          PC_OK);
    uint64_t id = yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "layer"));
    pc_json_value_fini(&out);
    return (pc_layer_id)id;
}

static void test_new_blank_insert_duplicate_delete(void)
{
    pc_engine_session *s = session();
    pc_layer_id id = new_blank(s);
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    CHECK(pc_document_layer(doc, id)->has_video);
    CHECK(doc->has_timeline);
    size_t start = frames_of(s, id);
    CHECK(ex(s, "layer.videoLayers.insertBlankFrame", "{}") == PC_OK);
    CHECK_EQ_UINT(frames_of(s, id), start + 1);
    CHECK(ex(s, "layer.videoLayers.duplicateFrame", "{}") == PC_OK);
    CHECK_EQ_UINT(frames_of(s, id), start + 2);
    CHECK(ex(s, "layer.videoLayers.deleteFrame", "{}") == PC_OK);
    CHECK_EQ_UINT(frames_of(s, id), start + 1);
    d = pc_engine_session_active(s);
    CHECK(pc_doc_arc_get(d->doc)->timeline.duration >= frames_of(s, id));
    pc_engine_session_free(s);
}

static void test_rasterize_drops_stack(void)
{
    pc_engine_session *s = session();
    (void)new_blank(s);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "layer.videoLayers.rasterize", "{}", &out, NULL) == PC_OK);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&out), "rasterized")));
    pc_json_value_fini(&out);
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    CHECK(d->has_active_layer);
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(d->doc), d->active_layer);
    CHECK(l != NULL && !l->has_video && l->content.kind == PC_LAYER_RASTER);
    pc_engine_session_free(s);
}

static void test_frame_ops_need_video_layer(void)
{
    pc_engine_session *s = session();
    CHECK(ex(s, "layer.videoLayers.insertBlankFrame", "{}") != PC_OK);
    CHECK(ex(s, "layer.videoLayers.deleteFrame", "{}") != PC_OK);
    CHECK(ex(s, "layer.videoLayers.rasterize", "{}") != PC_OK);
    pc_engine_session_free(s);
}

static void test_scrub_keeps_raster(void)
{
    pc_engine_session *s = session();
    pc_layer_id id = new_blank(s);
    CHECK(ex(s, "layer.videoLayers.insertBlankFrame", "{}") == PC_OK);
    CHECK(ex(s, "timeline.setFrame", "{\"frame\":1}") == PC_OK);
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(d->doc), id);
    CHECK(l != NULL && l->content.kind == PC_LAYER_RASTER);
    pc_engine_session_free(s);
}

/* No command may crash on hostile params. */
static void test_adversarial(void)
{
    pc_engine_session *s = session();
    (void)new_blank(s);
    const char *ids[] = {"layer.videoLayers.newBlankVideoLayer",
                         "layer.videoLayers.insertBlankFrame",
                         "layer.videoLayers.duplicateFrame",
                         "layer.videoLayers.deleteFrame",
                         "layer.videoLayers.restoreFrame",
                         "layer.videoLayers.restoreAllFrames",
                         "layer.videoLayers.showAlteredVideo",
                         "layer.videoLayers.rasterize",
                         "layer.rasterize.video",
                         "layer.videoLayers.interpretFootage"};
    const char *ps[] = {"{}", "[]", "null", "{\"fps\":\"x\"}", "{\"fps\":-1e999}", "123"};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        for (size_t j = 0; j < sizeof ps / sizeof ps[0]; j++) {
            (void)ex(s, ids[i], ps[j]); /* must not crash */
        }
    }
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_new_blank_insert_duplicate_delete);
    RUN(test_rasterize_drops_stack);
    RUN(test_frame_ops_need_video_layer);
    RUN(test_scrub_keeps_raster);
    RUN(test_adversarial);
    return pc_t_finish();
}
