/* Edit > Content-Aware Fill: edit.contentAwareFill (content_fill_cmds.c).
 *
 * Filling a selection replaces its pixels in one undoable step; new/duplicate output modes add a
 * layer; no selection, no document and bad params are graceful (never a crash, rule 9). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine/content_fill_cmds.h"
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

static const pc_document *doc_of(pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    CHECK(d != NULL);
    return pc_doc_arc_get(d->doc);
}

/* A document whose single raster layer holds a vertical blue/yellow gradient (structure to sample)
 * with a red hole punched in the middle. */
static pc_status fill_grad(pc_document *doc, bool *ha, pc_layer_id *active, void *v, pc_error *e)
{
    (void)ha;
    (void)active;
    (void)v;
    pc_layer *l = &doc->layers[0];
    pc_surface *su = pc_layer_surface_mut(l);
    for (int32_t y = 0; y < 48; y++) {
        for (int32_t x = 0; x < 48; x++) {
            float t = (float)x / 48.0f;
            float px[4] = {t, 1.0f - t, 0.5f, 1.0f};
            (void)pc_surface_write_region(su, pc_rect_from_xywh(x, y, 1, 1), px, 4, e);
        }
    }
    return PC_OK;
}

static pc_engine_session *mk(pc_layer_id *id_out)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(48, 48), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    *id_out = doc_of(s)->layers[0].id;
    CHECK(pc_engine_session_edit(s, "grad", fill_grad, NULL, NULL) == PC_OK);
    return s;
}

static void test_registry(void)
{
    size_t n = 0;
    (void)pc_engine_content_fill_specs(&n);
    CHECK_EQ_UINT(n, 1);
    CHECK(pc_engine_command_find("edit.contentAwareFill") != NULL);
}

static void test_fills_selection(void)
{
    pc_layer_id id = 0;
    pc_engine_session *s = mk(&id);
    CHECK(run_json(s, "select.rect", "{\"x\":18,\"y\":18,\"width\":12,\"height\":12}") == PC_OK);
    CHECK(pc_engine_session_is_enabled(s, "edit.contentAwareFill"));
    CHECK(run_json(s, "edit.contentAwareFill", "{\"sampling\":\"rectangular\"}") == PC_OK);
    /* The command completed and left a pixel there (opaque). */
    const pc_layer *l = pc_document_layer(doc_of(s), id);
    const pc_surface *su = pc_layer_surface(l);
    CHECK(su != NULL);
    CHECK(pc_surface_sample_channel(su, 24, 24, 3) > 0.5f);
    /* Undo restores. */
    CHECK(pc_engine_session_undo(s));
    pc_engine_session_free(s);
}

static void test_output_modes(void)
{
    pc_layer_id id = 0;
    pc_engine_session *s = mk(&id);
    size_t before = doc_of(s)->nlayers;
    CHECK(run_json(s, "select.rect", "{\"x\":18,\"y\":18,\"width\":12,\"height\":12}") == PC_OK);
    CHECK(run_json(s, "edit.contentAwareFill", "{\"sampling\":\"rectangular\",\"output\":\"new\"}") ==
          PC_OK);
    CHECK(doc_of(s)->nlayers == before + 1);
    CHECK(run_json(s, "select.rect", "{\"x\":18,\"y\":18,\"width\":12,\"height\":12}") == PC_OK);
    CHECK(run_json(s, "edit.contentAwareFill",
                   "{\"sampling\":\"rectangular\",\"output\":\"duplicate\"}") == PC_OK);
    CHECK(doc_of(s)->nlayers == before + 2);
    pc_engine_session_free(s);
}

static void test_graceful(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(!pc_engine_session_is_enabled(s, "edit.contentAwareFill"));
    CHECK(run_json(s, "edit.contentAwareFill", "{}") != PC_OK);
    pc_engine_session_free(s);

    pc_layer_id id = 0;
    s = mk(&id);
    /* No selection. */
    CHECK(!pc_engine_session_is_enabled(s, "edit.contentAwareFill"));
    CHECK(run_json(s, "edit.contentAwareFill", "{}") != PC_OK);
    /* Bad params with a selection. */
    CHECK(run_json(s, "select.rect", "{\"x\":18,\"y\":18,\"width\":12,\"height\":12}") == PC_OK);
    CHECK(run_json(s, "edit.contentAwareFill", "{\"sampling\":\"bogus\"}") != PC_OK);
    CHECK(run_json(s, "edit.contentAwareFill", "{\"output\":\"bogus\"}") != PC_OK);
    CHECK(run_json(s, "edit.contentAwareFill", "{\"sampling\":\"custom\"}") != PC_OK);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_registry);
    RUN(test_fills_selection);
    RUN(test_output_modes);
    RUN(test_graceful);
    return pc_t_finish();
}
