/* Layer menu: Quick Export as PNG, Export As…, Create Layer (layer_export_cmds.c).
 *
 * Exporting a layer writes a trimmed one-layer image; Create Layer turns a layer's effects into
 * separate layers; no document, an empty layer and bad params are graceful (never a crash,
 * REWRITE_PLAN.md rule 9). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "base/buf.h"
#include "base/json.h"
#include "color/color.h"
#include "compose/compose.h"
#include "compose/effects.h"
#include "doc/doc.h"
#include "engine/engine.h"
#include "engine/engine_internal.h"
#include "engine/layer_export_cmds.h"
#include "harness.h"

static pc_status run_json(pc_engine_session *s, const char *id, const char *params)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error e = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, params, &out, &e);
    pc_json_value_fini(&out);
    return rc;
}

static const pc_document *doc_of(pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    CHECK(d != NULL);
    return pc_doc_arc_get(d->doc);
}

/* A document with a white Background and a layer "L" holding a green square at (4,4)-(10,10). */
static pc_engine_session *mk(pc_layer_id *layer_out)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(16, 16), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "layer.new.layer", "{\"name\":\"L\"}", &o, NULL) == PC_OK);
    pc_json_value_fini(&o);
    const pc_document *d = doc_of(s);
    *layer_out = d->layers[d->nlayers - 1].id;
    CHECK(run_json(s, "select.rect", "{\"x\":4,\"y\":4,\"width\":6,\"height\":6}") == PC_OK);
    CHECK(run_json(s, "edit.fill", "{\"contents\":\"color\",\"color\":\"#00ff00\"}") == PC_OK);
    CHECK(run_json(s, "select.deselect", "{}") == PC_OK);
    return s;
}

static void test_registry(void)
{
    size_t n = 0;
    (void)pc_engine_layer_export_specs(&n);
    CHECK_EQ_UINT(n, 3);
    CHECK(pc_engine_command_find("layer.quickExportAsPng") != NULL);
    CHECK(pc_engine_command_find("layer.exportAs") != NULL);
    CHECK(pc_engine_command_find("layer.layerStyle.createLayer") != NULL);
}

static void test_quick_export_png(void)
{
    pc_layer_id id = 0;
    pc_engine_session *s = mk(&id);
    char path[256];
    (void)snprintf(path, sizeof path, "/dev/shm/pc-gap3/pc-layer-export-%ld.png", (long)getpid());
    char params[384];
    (void)snprintf(params, sizeof params, "{\"path\":\"%s\"}", path);
    CHECK(run_json(s, "layer.quickExportAsPng", params) == PC_OK);
    /* The file exists and is a 6x6 PNG-ish (trimmed to the green square). */
    pc_buf bytes = PC_BUF_INIT;
    CHECK(pc_engine_read_file(path, &bytes, NULL) == PC_OK);
    CHECK(bytes.len > 8);
    CHECK(bytes.data[1] == 'P' && bytes.data[2] == 'N' && bytes.data[3] == 'G');
    pc_buf_fini(&bytes);
    (void)remove(path);
    /* png_only rejects a non-png path. */
    (void)snprintf(params, sizeof params, "{\"path\":\"/dev/shm/pc-gap3/x-%ld.jpg\"}", (long)getpid());
    CHECK(run_json(s, "layer.quickExportAsPng", params) != PC_OK);
    pc_engine_session_free(s);
}

static void test_export_as_scale(void)
{
    pc_layer_id id = 0;
    pc_engine_session *s = mk(&id);
    char path[256];
    (void)snprintf(path, sizeof path, "/dev/shm/pc-gap3/pc-layer-export-%ld.tif", (long)getpid());
    char params[384];
    (void)snprintf(params, sizeof params, "{\"path\":\"%s\",\"scale\":200}", path);
    CHECK(run_json(s, "layer.exportAs", params) == PC_OK);
    pc_buf bytes = PC_BUF_INIT;
    CHECK(pc_engine_read_file(path, &bytes, NULL) == PC_OK);
    CHECK(bytes.len > 0);
    pc_buf_fini(&bytes);
    (void)remove(path);
    pc_engine_session_free(s);
}

static void test_create_layer(void)
{
    pc_layer_id id = 0;
    pc_engine_session *s = mk(&id);
    size_t before = doc_of(s)->nlayers;
    /* A drop shadow goes below the layer; a color overlay (clipped) goes above it. */
    CHECK(run_json(s, "layer.layerStyle.dropShadow", "{}") == PC_OK);
    CHECK(run_json(s, "layer.layerStyle.colorOverlay",
                   "{\"color\":\"#ff0000\",\"opacity\":100,\"blend\":\"Normal\",\"add\":true}") == PC_OK);
    const pc_layer *l = pc_document_layer(doc_of(s), id);
    CHECK(l != NULL && pc_compose_has_effects(l));
    CHECK(pc_engine_session_is_enabled(s, "layer.layerStyle.createLayer"));
    CHECK(run_json(s, "layer.layerStyle.createLayer", "{}") == PC_OK);
    const pc_document *d = doc_of(s);
    /* Two new layers added, source keeps its pixels but loses its effects. */
    CHECK(d->nlayers == before + 2);
    l = pc_document_layer(d, id);
    CHECK(l != NULL && !pc_compose_has_effects(l) && l->content.kind == PC_LAYER_RASTER);
    /* Undo removes both and restores the effects. */
    CHECK(pc_engine_session_undo(s));
    d = doc_of(s);
    CHECK(d->nlayers == before);
    CHECK(pc_compose_has_effects(pc_document_layer(d, id)));
    pc_engine_session_free(s);
}

static void test_graceful(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(run_json(s, "layer.quickExportAsPng", "{\"path\":\"/dev/shm/pc-gap3/x.png\"}") != PC_OK);
    CHECK(run_json(s, "layer.layerStyle.createLayer", "{}") != PC_OK);
    CHECK(!pc_engine_session_is_enabled(s, "layer.layerStyle.createLayer"));
    pc_engine_session_free(s);

    pc_layer_id id = 0;
    s = mk(&id);
    /* Missing path. */
    CHECK(run_json(s, "layer.exportAs", "{}") != PC_OK);
    /* createLayer with no effects. */
    CHECK(run_json(s, "layer.layerStyle.createLayer", "{}") != PC_OK);
    /* Bad layer id. */
    CHECK(run_json(s, "layer.quickExportAsPng", "{\"layer\":99999,\"path\":\"/dev/shm/pc-gap3/x.png\"}") !=
          PC_OK);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_registry);
    RUN(test_quick_export_png);
    RUN(test_export_as_scale);
    RUN(test_create_layer);
    RUN(test_graceful);
    return pc_t_finish();
}
