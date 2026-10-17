/* Image > Apply Image and Image > Calculations (channel_cmds.c).
 *
 * Apply Image blends a source onto the active pixel layer; Calculations blends two gray sources
 * into a new channel / document / selection. Unsupported blending, a bad result mode and
 * no-document are graceful errors (never a crash, REWRITE_PLAN.md rule 9). */
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine/engine.h"
#include "harness.h"

static pc_engine_session *mk(uint32_t w, uint32_t h)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(w, h), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(0.5f, 0.5f, 0.5f, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

struct addctx {
    pc_layer_id id;
};
static pc_status add_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *v, pc_error *e)
{
    struct addctx *c = v;
    pc_layer l;
    if (pc_layer_raster(&l, "L", PC_PIXEL_FORMAT_RGBA8, e) != PC_OK) {
        return e != NULL ? e->code : PC_ERR_NOMEM;
    }
    pc_surface *su = pc_layer_surface_mut(&l);
    float col[4] = {0.6f, 0.3f, 0.2f, 1.0f};
    if (su != NULL) {
        (void)pc_surface_fill_rect(su, pc_rect_from_xywh(0, 0, 16, 16), col, 4, e);
    }
    pc_layer_id id = 0;
    pc_status rc = pc_document_insert_above(doc, NULL, &l, &id, e);
    if (rc != PC_OK) {
        pc_layer_fini(&l);
        return rc;
    }
    c->id = id;
    *ha = true;
    *active = id;
    return PC_OK;
}

static pc_layer_id add_layer(pc_engine_session *s)
{
    struct addctx c = {0};
    CHECK(pc_engine_session_edit(s, "add", add_edit, &c, NULL) == PC_OK);
    return c.id;
}

static pc_status run_json(pc_engine_session *s, const char *id, const char *params, pc_json_value *out)
{
    pc_error e = {0};
    return pc_engine_session_execute_json(s, id, params, out, &e);
}

static void test_apply_image(void)
{
    pc_engine_session *s = mk(16, 16);
    (void)add_layer(s);
    pc_json_value out = PC_JSON_VALUE_INIT;
    /* Multiply the merged source onto the layer. */
    CHECK(run_json(s, "image.applyImage",
                   "{\"source\":{\"channel\":\"composite\"},\"blending\":\"multiply\",\"opacity\":80}",
                   &out) == PC_OK);
    pc_json_value_fini(&out);
    /* Add/Subtract blending modes with scale+offset. */
    CHECK(run_json(s, "image.applyImage", "{\"blending\":\"add\",\"scale\":2,\"offset\":10}", &out) ==
          PC_OK);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_calculations(void)
{
    pc_engine_session *s = mk(16, 16);
    (void)add_layer(s);
    pc_json_value out = PC_JSON_VALUE_INIT;
    /* New channel result. */
    CHECK(run_json(s, "image.calculations",
                   "{\"source1\":{\"channel\":\"red\"},\"source2\":{\"channel\":\"green\"},"
                   "\"blending\":\"multiply\",\"result\":\"newChannel\"}",
                   &out) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    CHECK(root != NULL && yyjson_mut_is_obj(root));
    CHECK(yyjson_mut_obj_get(root, "channel") != NULL);
    pc_json_value_fini(&out);
    /* Selection result. */
    CHECK(run_json(s, "image.calculations",
                   "{\"blending\":\"multiply\",\"result\":\"selection\"}", &out) == PC_OK);
    root = pc_json_value_root(&out);
    CHECK(root != NULL && yyjson_mut_obj_get(root, "selected") != NULL);
    pc_json_value_fini(&out);
    /* New document result. */
    size_t ndocs_before = 0;
    (void)pc_engine_session_documents(s, &ndocs_before);
    CHECK(run_json(s, "image.calculations",
                   "{\"blending\":\"screen\",\"result\":\"newDocument\",\"name\":\"Calc\"}", &out) ==
          PC_OK);
    size_t ndocs_after = 0;
    (void)pc_engine_session_documents(s, &ndocs_after);
    CHECK(ndocs_after == ndocs_before + 1);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_graceful(void)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    /* No document. */
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(run_json(s, "image.applyImage", "{}", &out) != PC_OK);
    pc_json_value_fini(&out);
    CHECK(run_json(s, "image.calculations", "{}", &out) != PC_OK);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);

    /* Adversarial params on a real doc are errors, never crashes. */
    s = mk(16, 16);
    (void)add_layer(s);
    CHECK(run_json(s, "image.applyImage", "{\"blending\":\"notamode\"}", &out) != PC_OK);
    pc_json_value_fini(&out);
    CHECK(run_json(s, "image.applyImage", "{\"blending\":\"dissolve\"}", &out) != PC_OK); /* not separable */
    pc_json_value_fini(&out);
    CHECK(run_json(s, "image.calculations", "{\"result\":\"bogus\"}", &out) != PC_OK);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_apply_image);
    RUN(test_calculations);
    RUN(test_graceful);
    return pc_t_finish();
}
