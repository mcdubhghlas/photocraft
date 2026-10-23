/* Layer > Video Layers file footage + File > Import/Export video (video_cmds.c additions):
 * newVideoLayerFromFile, replaceFootage, reloadFrame, videoFramesToLayers, renderVideo.
 *
 * Loads frames from an image/GIF, adds them as video or layers, and renders a sequence/GIF; no
 * path, no document and a non-video layer are graceful (never a crash, REWRITE_PLAN.md rule 9). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine/engine.h"
#include "engine/engine_internal.h"
#include "harness.h"

static pc_status run_json(pc_engine_session *s, const char *id, const char *params)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error e = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, params, &out, &e);
    pc_json_value_fini(&out);
    return rc;
}

static int64_t run_int(pc_engine_session *s, const char *id, const char *params, const char *field)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error e = {0};
    int64_t n = -1;
    if (pc_engine_session_execute_json(s, id, params, &out, &e) == PC_OK) {
        yyjson_mut_val *root = pc_json_value_root(&out);
        yyjson_mut_val *v = root ? yyjson_mut_obj_get(root, field) : NULL;
        n = v ? yyjson_mut_get_sint(v) : -1;
    }
    pc_json_value_fini(&out);
    return n;
}

static const pc_document *doc_of(pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    CHECK(d != NULL);
    return pc_doc_arc_get(d->doc);
}

static pc_engine_session *mk(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "clip", pc_size_make(8, 8), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(0.2f, 0.5f, 0.8f, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

/* Write an 8x8 PNG to `path`. */
static void write_png(const char *path)
{
    pc_engine_session *s = mk();
    char params[4160];
    (void)snprintf(params, sizeof params, "{\"path\":\"%s\"}", path);
    CHECK(run_json(s, "file.saveACopy", params) == PC_OK);
    pc_engine_session_free(s);
}

static void test_registry(void)
{
    CHECK(pc_engine_command_find("layer.videoLayers.newVideoLayerFromFile") != NULL);
    CHECK(pc_engine_command_find("layer.videoLayers.replaceFootage") != NULL);
    CHECK(pc_engine_command_find("layer.videoLayers.reloadFrame") != NULL);
    CHECK(pc_engine_command_find("file.import.videoFramesToLayers") != NULL);
    CHECK(pc_engine_command_find("file.export.renderVideo") != NULL);
}

static void test_new_from_file_and_frames_to_layers(void)
{
    char png[128];
    (void)snprintf(png, sizeof png, "/dev/shm/pc-gap3/pc-vid-%ld.png", (long)getpid());
    write_png(png);

    pc_engine_session *s = mk();
    char params[4200];
    (void)snprintf(params, sizeof params, "{\"path\":\"%s\"}", png);
    CHECK(run_int(s, "layer.videoLayers.newVideoLayerFromFile", params, "frames") == 1);
    /* The active layer is a video layer with a file source. */
    const pc_document *d = doc_of(s);
    const pc_layer *l = pc_document_layer(d, pc_engine_session_active(s)->active_layer);
    CHECK(l != NULL && l->has_video && l->video.source.is_file);
    /* replaceFootage + reloadFrame both run. */
    CHECK(run_json(s, "layer.videoLayers.replaceFootage", params) == PC_OK);
    CHECK(run_json(s, "layer.videoLayers.reloadFrame", "{}") == PC_OK);
    /* Video Frames to Layers adds one layer. */
    size_t before = doc_of(s)->nlayers;
    CHECK(run_int(s, "file.import.videoFramesToLayers", params, "layers") == 1);
    CHECK(doc_of(s)->nlayers == before + 1);
    pc_engine_session_free(s);
    (void)remove(png);
}

static void test_render_sequence_and_gif(void)
{
    pc_engine_session *s = mk();
    /* A 2-frame video layer. */
    CHECK(run_json(s, "layer.videoLayers.newBlankVideoLayer", "{}") == PC_OK);
    CHECK(run_json(s, "layer.videoLayers.insertBlankFrame", "{}") == PC_OK);
    char dir[128];
    (void)snprintf(dir, sizeof dir, "/dev/shm/pc-gap3/vid-%ld", (long)getpid());
    (void)mkdir(dir, 0700);
    char params[200];
    (void)snprintf(params, sizeof params, "{\"dir\":\"%s\",\"format\":\"png\"}", dir);
    CHECK(run_int(s, "file.export.renderVideo", params, "frames") >= 1);
    /* A GIF render, then load it back as a video layer. */
    (void)snprintf(params, sizeof params, "{\"dir\":\"%s\",\"format\":\"gif\"}", dir);
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error e = {0};
    CHECK(pc_engine_session_execute_json(s, "file.export.renderVideo", params, &out, &e) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    const char *gifpath = yyjson_mut_get_str(yyjson_mut_obj_get(root, "file"));
    char gif[256] = {0};
    if (gifpath) {
        (void)snprintf(gif, sizeof gif, "%s", gifpath);
    }
    pc_json_value_fini(&out);
    CHECK(gif[0] != '\0');
    if (gif[0]) {
        char p2[320];
        (void)snprintf(p2, sizeof p2, "{\"path\":\"%s\"}", gif);
        CHECK(run_int(s, "layer.videoLayers.newVideoLayerFromFile", p2, "frames") >= 1);
        (void)remove(gif);
    }
    pc_engine_session_free(s);
}

static void test_graceful(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(run_json(s, "layer.videoLayers.newVideoLayerFromFile", "{\"path\":\"/x.png\"}") != PC_OK);
    CHECK(run_json(s, "file.export.renderVideo", "{\"dir\":\"/dev/shm/pc-gap3\"}") != PC_OK);
    pc_engine_session_free(s);

    s = mk();
    /* Missing path / dir. */
    CHECK(run_json(s, "layer.videoLayers.newVideoLayerFromFile", "{}") != PC_OK);
    CHECK(run_json(s, "file.import.videoFramesToLayers", "{}") != PC_OK);
    CHECK(run_json(s, "file.export.renderVideo", "{}") != PC_OK);
    /* reloadFrame on a non-video layer. */
    CHECK(run_json(s, "layer.videoLayers.reloadFrame", "{}") != PC_OK);
    /* A nonexistent file. */
    CHECK(run_json(s, "layer.videoLayers.newVideoLayerFromFile",
                   "{\"path\":\"/dev/shm/pc-gap3/nope.png\"}") != PC_OK);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_registry);
    RUN(test_new_from_file_and_frames_to_layers);
    RUN(test_render_sequence_and_gif);
    RUN(test_graceful);
    return pc_t_finish();
}
