/* Layer > Rasterize > Layer Style: layer.rasterize.layerStyle (rasterize_style_cmds.c).
 *
 * A layer's visible effects are baked into its pixels in one undoable step; the layer stays a
 * raster layer with no effects; a layer with nothing to bake, no document and bad params are
 * graceful errors (never a crash, REWRITE_PLAN.md rule 9). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "color/color.h"
#include "compose/compose.h"
#include "compose/effects.h"
#include "doc/doc.h"
#include "engine/engine.h"
#include "engine/engine_internal.h"
#include "engine/rasterize_style_cmds.h"
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
    pc_layer_id id = d->layers[d->nlayers - 1].id;
    *layer_out = id;
    CHECK(run_json(s, "select.rect", "{\"x\":4,\"y\":4,\"width\":6,\"height\":6}") == PC_OK);
    CHECK(run_json(s, "edit.fill", "{\"contents\":\"color\",\"color\":\"#00ff00\"}") == PC_OK);
    CHECK(run_json(s, "select.deselect", "{}") == PC_OK);
    return s;
}

static void test_registry(void)
{
    size_t n = 0;
    (void)pc_engine_rasterize_style_specs(&n);
    CHECK_EQ_UINT(n, 1);
    CHECK(pc_engine_command_find("layer.rasterize.layerStyle") != NULL);
}

static void test_bakes_effect(void)
{
    pc_layer_id id = 0;
    pc_engine_session *s = mk(&id);
    /* A red Color Overlay turns the opaque (green) pixels red. */
    CHECK(run_json(s, "layer.layerStyle.colorOverlay",
                   "{\"color\":\"#ff0000\",\"opacity\":100,\"blend\":\"Normal\"}") == PC_OK);
    const pc_layer *l = pc_document_layer(doc_of(s), id);
    CHECK(l != NULL && pc_compose_has_effects(l));
    CHECK(pc_engine_session_is_enabled(s, "layer.rasterize.layerStyle"));
    CHECK(run_json(s, "layer.rasterize.layerStyle", "{}") == PC_OK);
    l = pc_document_layer(doc_of(s), id);
    CHECK(l != NULL);
    CHECK(l->content.kind == PC_LAYER_RASTER);
    CHECK(!pc_compose_has_effects(l)); /* the effect is in the pixels now */
    const pc_surface *su = pc_layer_surface(l);
    CHECK(su != NULL);
    CHECK(pc_surface_sample_channel(su, 6, 6, 0) > 0.9f); /* baked overlay: red */
    CHECK(pc_surface_sample_channel(su, 6, 6, 1) < 0.1f); /* no green left */
    /* Undo restores the effect. */
    CHECK(pc_engine_session_undo(s));
    l = pc_document_layer(doc_of(s), id);
    CHECK(l != NULL && pc_compose_has_effects(l));
    pc_engine_session_free(s);
}

static void test_graceful(void)
{
    /* No document. */
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(!pc_engine_session_is_enabled(s, "layer.rasterize.layerStyle"));
    CHECK(run_json(s, "layer.rasterize.layerStyle", "{}") != PC_OK);
    pc_engine_session_free(s);

    /* A layer with no effects: not enabled, and running errors. */
    pc_layer_id id = 0;
    s = mk(&id);
    CHECK(!pc_engine_session_is_enabled(s, "layer.rasterize.layerStyle"));
    CHECK(run_json(s, "layer.rasterize.layerStyle", "{}") != PC_OK);
    /* Bad params. */
    CHECK(run_json(s, "layer.rasterize.layerStyle", "{\"layer\":\"L\"}") != PC_OK);
    CHECK(run_json(s, "layer.rasterize.layerStyle", "{\"layer\":99999}") != PC_OK);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_registry);
    RUN(test_bakes_effect);
    RUN(test_graceful);
    return pc_t_finish();
}
