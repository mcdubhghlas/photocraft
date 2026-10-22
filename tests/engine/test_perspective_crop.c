/* The Perspective Crop tool's commit: image.perspectiveCrop (perspective_crop_cmds.c).
 *
 * An identity quad crop keeps the pixels and resizes to the requested canvas; a skewed quad rectifies
 * and resizes in one undoable step; bad quads/params are graceful errors (never a crash, rule 9). */
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine/engine.h"
#include "engine/engine_internal.h"
#include "engine/perspective_crop_cmds.h"
#include "harness.h"

static pc_engine_session *mk(uint32_t w, uint32_t h)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(w, h), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

static pc_status paint_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *v, pc_error *e)
{
    (void)ha;
    (void)active;
    (void)v;
    pc_layer *bg = pc_document_background_mut(doc);
    if (bg == NULL) {
        return pc_fail(e, PC_ERR_INVALID, "no background");
    }
    pc_surface *su = pc_layer_surface_mut(bg);
    if (su != NULL) {
        float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
        (void)pc_surface_fill_rect(su, pc_rect_from_xywh(20, 20, 10, 10), red, 4, e);
    }
    return PC_OK;
}

static void doc_size(pc_engine_session *s, uint32_t *w, uint32_t *h)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    CHECK(d != NULL);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    *w = doc->size.width;
    *h = doc->size.height;
}

static pc_status run_json(pc_engine_session *s, const char *id, const char *params)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error e = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, params, &out, &e);
    pc_json_value_fini(&out);
    return rc;
}

static void test_registry(void)
{
    size_t n = 0;
    (void)pc_engine_perspective_crop_specs(&n);
    CHECK_EQ_UINT(n, 1);
    CHECK(pc_engine_command_find("image.perspectiveCrop") != NULL);
}

/* An identity quad over the whole canvas, cropped to a smaller size, resizes the document. */
static void test_identity_quad_resizes(void)
{
    pc_engine_session *s = mk(160, 120);
    CHECK(pc_engine_session_edit(s, "paint", paint_edit, NULL, NULL) == PC_OK);
    CHECK(run_json(s, "image.perspectiveCrop",
                   "{\"corners\":[[0,0],[160,0],[160,120],[0,120]],\"width\":80,\"height\":60}") == PC_OK);
    uint32_t w = 0, h = 0;
    doc_size(s, &w, &h);
    CHECK_EQ_UINT(w, 80);
    CHECK_EQ_UINT(h, 60);
    CHECK(pc_engine_session_undo(s));
    doc_size(s, &w, &h);
    CHECK_EQ_UINT(w, 160);
    CHECK_EQ_UINT(h, 120);
    pc_engine_session_free(s);
}

/* No width/height: the quad's mean side lengths set the canvas. */
static void test_default_size_from_quad(void)
{
    pc_engine_session *s = mk(160, 120);
    CHECK(pc_engine_session_edit(s, "paint", paint_edit, NULL, NULL) == PC_OK);
    CHECK(run_json(s, "image.perspectiveCrop",
                   "{\"corners\":[[30,12],[140,25],[128,110],[14,95]]}") == PC_OK);
    uint32_t w = 0, h = 0;
    doc_size(s, &w, &h);
    CHECK(w >= 90 && w <= 140);  /* ~mean horizontal edge length */
    CHECK(h >= 70 && h <= 110);  /* ~mean vertical edge length */
    pc_engine_session_free(s);
}

static void test_graceful(void)
{
    /* No document. */
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(run_json(s, "image.perspectiveCrop",
                   "{\"corners\":[[0,0],[10,0],[10,10],[0,10]]}") != PC_OK);
    pc_engine_session_free(s);

    s = mk(160, 120);
    uint32_t w = 0, h = 0;
    /* Each of these is an error and leaves the document untouched. */
    CHECK(run_json(s, "image.perspectiveCrop", "{}") != PC_OK);
    CHECK(run_json(s, "image.perspectiveCrop", "{\"corners\":[[0,0],[10,0],[10,10]]}") != PC_OK);
    CHECK(run_json(s, "image.perspectiveCrop",
                   "{\"corners\":[[0,0],[50,0],[100,0],[0,50]]}") != PC_OK); /* collinear */
    CHECK(run_json(s, "image.perspectiveCrop",
                   "{\"corners\":[[0,0],[50,50],[50,0],[0,50]]}") != PC_OK); /* self-crossing */
    CHECK(run_json(s, "image.perspectiveCrop",
                   "{\"corners\":[[0,0],[1e300,0],[50,50],[0,50]]}") != PC_OK); /* huge */
    CHECK(run_json(s, "image.perspectiveCrop",
                   "{\"corners\":[[0,0],[\"a\",0],[50,50],[0,50]]}") != PC_OK); /* non-numeric */
    CHECK(run_json(s, "image.perspectiveCrop",
                   "{\"corners\":[[0,0],[50,0],[50,50],[0,50]],\"width\":0}") != PC_OK);
    CHECK(run_json(s, "image.perspectiveCrop",
                   "{\"corners\":[[0,0],[50,0],[50,50],[0,50]],\"resolution\":-3}") != PC_OK);
    CHECK(run_json(s, "image.perspectiveCrop",
                   "{\"corners\":[[0,0],[50,0],[50,50],[0,50]],\"interpolation\":3}") != PC_OK);
    doc_size(s, &w, &h);
    CHECK_EQ_UINT(w, 160);
    CHECK_EQ_UINT(h, 120);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_registry);
    RUN(test_identity_quad_resizes);
    RUN(test_default_size_from_quad);
    RUN(test_graceful);
    return pc_t_finish();
}
