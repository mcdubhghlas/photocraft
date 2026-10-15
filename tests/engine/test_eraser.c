/* Magic Eraser and Background Eraser (port of a subset of eraser_cmds tests, E10). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "raster/pixel.h"
#include "raster/surface.h"
#include "harness.h"

static pc_engine_session *mk(uint32_t w, uint32_t h, pc_color bg)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "U", pc_size_make(w, h), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, bg, NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

static pc_status run(pc_engine_session *s, const char *id, const char *json, pc_json_value *out)
{
    pc_error e = {0};
    pc_json_value local = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, id, json, out ? out : &local, &e);
    if (out == NULL) {
        pc_json_value_fini(&local);
    }
    return rc;
}

static void layer_px(pc_engine_session *s, int x, int y, float out[4])
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_surface_rgba(pc_layer_surface(pc_document_layer(doc, d->active_layer)), x, y, out);
}

/* Fill the active layer solid, then paint a differently-coloured block. */
struct fillctx {
    float a[4], b[4];
    pc_rect block;
};
static pc_status fill_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *v, pc_error *e)
{
    (void)ha;
    struct fillctx *c = v;
    pc_surface *surf = pc_layer_surface_mut(pc_document_layer_mut(doc, *a));
    CHECK(surf != NULL);
    pc_rect bb = pc_document_bounds(doc);
    if (pc_surface_fill_rect(surf, bb, c->a, 4, e) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    return pc_surface_fill_rect(surf, c->block, c->b, 4, e);
}

static void test_magic_eraser(void)
{
    pc_engine_session *s = mk(20, 20, pc_color_rgba(1, 1, 1, 1));
    /* white field with a solid red block at the top-left 10x20 */
    struct fillctx c = {{1, 1, 1, 1}, {1, 0, 0, 1}, pc_rect_make(0, 0, 10, 20)};
    CHECK(pc_engine_session_edit(s, "setup", fill_edit, &c, NULL) == PC_OK);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(run(s, "paint.magicEraser", "{\"x\":2,\"y\":2,\"tolerance\":0,\"antiAlias\":false}", &o) == PC_OK);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&o), "erased")));
    pc_json_value_fini(&o);
    float erased[4], kept[4];
    layer_px(s, 2, 2, erased);
    layer_px(s, 15, 2, kept);
    CHECK(erased[3] < 0.02f);       /* red block erased to transparency */
    CHECK(kept[3] > 0.98f);         /* white field kept */
    CHECK(pc_engine_session_undo(s));
    layer_px(s, 2, 2, erased);
    CHECK(erased[3] > 0.98f);       /* restored */
    /* an off-canvas click erases nothing */
    pc_json_value o2 = PC_JSON_VALUE_INIT;
    CHECK(run(s, "paint.magicEraser", "{\"x\":-5,\"y\":2}", &o2) == PC_OK);
    CHECK(!yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&o2), "erased")));
    pc_json_value_fini(&o2);
    pc_engine_session_free(s);
}

static void test_background_eraser(void)
{
    pc_engine_session *s = mk(20, 20, pc_color_rgba(1, 1, 1, 1));
    struct fillctx c = {{1, 1, 1, 1}, {1, 0, 0, 1}, pc_rect_make(0, 0, 20, 20)};
    /* whole layer red */
    c.a[0] = 1; c.a[1] = 0; c.a[2] = 0; c.a[3] = 1;
    CHECK(pc_engine_session_edit(s, "setup", fill_edit, &c, NULL) == PC_OK);
    /* a stroke across the middle samples red (continuous) and erases it */
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(run(s, "paint.backgroundEraser",
              "{\"points\":[[2,10],[10,10],[18,10]],\"size\":6,\"tolerance\":50}", &o) == PC_OK);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&o), "erased")));
    pc_json_value_fini(&o);
    float mid[4];
    layer_px(s, 10, 10, mid);
    CHECK(mid[3] < 0.5f); /* erased under the stroke */
    pc_engine_session_free(s);
}

static void test_errors(void)
{
    pc_engine_session *s = mk(16, 16, pc_color_rgba(1, 1, 1, 1));
    /* magic eraser needs x/y */
    CHECK(run(s, "paint.magicEraser", "{}", NULL) != PC_OK);
    CHECK(run(s, "paint.magicEraser", "{\"x\":null,\"y\":1}", NULL) != PC_OK);
    CHECK(run(s, "paint.magicEraser", "{\"x\":2,\"y\":2,\"opacity\":\"x\"}", NULL) != PC_OK);
    /* background eraser needs points */
    CHECK(run(s, "paint.backgroundEraser", "{}", NULL) != PC_OK);
    CHECK(run(s, "paint.backgroundEraser", "{\"points\":[]}", NULL) != PC_OK);
    CHECK(run(s, "paint.backgroundEraser", "{\"points\":[[2,2]],\"sampling\":\"bogus\"}", NULL) != PC_OK);
    pc_engine_session_free(s);
}

static void test_adversarial(void)
{
    pc_engine_session *s = mk(16, 16, pc_color_rgba(1, 1, 1, 1));
    const char *cmds[] = {"paint.magicEraser", "paint.backgroundEraser"};
    const char *params[] = {"{}", "[]", "null", "\"x\"", "{\"x\":1e308,\"y\":0}",
                            "{\"points\":[[1e308,0]]}", "{\"tolerance\":-9e99}",
                            "{\"x\":2,\"y\":2,\"opacity\":1e308}", "{\"limits\":5}"};
    for (size_t c = 0; c < 2; c++) {
        for (size_t pi = 0; pi < sizeof params / sizeof params[0]; pi++) {
            pc_json_value out = PC_JSON_VALUE_INIT;
            pc_error e = {0};
            (void)pc_engine_session_execute_json(s, cmds[c], params[pi], &out, &e);
            pc_json_value_fini(&out);
        }
    }
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_magic_eraser);
    RUN(test_background_eraser);
    RUN(test_errors);
    RUN(test_adversarial);
    return pc_t_finish();
}
