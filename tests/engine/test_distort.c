/* Filter > Liquify, Edit > Puppet Warp and Edit > Perspective Warp (distort_cmds.c).
 *
 * A liquify stroke / a moved puppet pin / a shifted perspective plane changes the active pixel
 * layer; an empty / identity edit reports changed=false; adversarial params and no-document are
 * graceful errors (never a crash, REWRITE_PLAN.md rule 9); the smart-filter re-apply hook runs a
 * stored Liquify onto a plain surface. */
#include <stdlib.h>
#include <string.h>

#include "algo/liquify.h"
#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine/distort_cmds.h"
#include "engine/engine.h"
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

/* Add a raster layer with an opaque rectangle; make it active. */
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
    float col[4] = {0.2f, 0.4f, 0.8f, 1.0f};
    if (su != NULL) {
        (void)pc_surface_fill_rect(su, pc_rect_from_xywh(4, 4, 24, 24), col, 4, e);
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

/* Execute an id with JSON params; returns the status and the "changed" field (false when absent). */
static pc_status run_json(pc_engine_session *s, const char *id, const char *params, bool *changed)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error e = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, params, &out, &e);
    if (changed != NULL) {
        *changed = false;
        yyjson_mut_val *root = pc_json_value_root(&out);
        if (root != NULL && yyjson_mut_is_obj(root)) {
            yyjson_mut_val *c = yyjson_mut_obj_get(root, "changed");
            if (c != NULL) {
                *changed = yyjson_mut_get_bool(c);
            }
        }
    }
    pc_json_value_fini(&out);
    return rc;
}

static void test_registry(void)
{
    size_t n = 0;
    (void)pc_engine_distort_specs(&n);
    CHECK_EQ_UINT(n, 5);
    CHECK(pc_engine_command_find("filter.liquify") != NULL);
    CHECK(pc_engine_command_find("edit.puppetWarp") != NULL);
    CHECK(pc_engine_command_find("edit.perspectiveWarp") != NULL);
    CHECK(pc_engine_command_find("layer.smartObjects.puppetWarp") != NULL);
    CHECK(pc_engine_command_find("layer.smartObjects.perspectiveWarp") != NULL);
}

static void test_liquify(void)
{
    pc_engine_session *s = mk(32, 32);
    (void)add_layer(s);
    bool changed = false;
    /* A forward-warp stroke across the opaque region moves pixels. */
    CHECK(run_json(s, "filter.liquify",
                   "{\"strokes\":[{\"tool\":\"forwardWarp\",\"size\":20,"
                   "\"points\":[[10,16],[20,16]]}]}",
                   &changed) == PC_OK);
    CHECK(changed);
    /* No strokes = no change. */
    CHECK(run_json(s, "filter.liquify", "{\"strokes\":[]}", &changed) == PC_OK);
    CHECK(!changed);
    pc_engine_session_free(s);
}

static void test_puppet(void)
{
    pc_engine_session *s = mk(32, 32);
    (void)add_layer(s);
    bool changed = false;
    /* A pin that moves changes the layer. */
    CHECK(run_json(s, "edit.puppetWarp",
                   "{\"pins\":[{\"src\":[8,8],\"dst\":[12,8]},{\"src\":[24,24],\"dst\":[24,24]}]}",
                   &changed) == PC_OK);
    CHECK(changed);
    /* No pins = identity = no change on a raster layer. */
    CHECK(run_json(s, "edit.puppetWarp", "{\"pins\":[]}", &changed) == PC_OK);
    CHECK(!changed);
    pc_engine_session_free(s);
}

static void test_perspective(void)
{
    pc_engine_session *s = mk(32, 32);
    (void)add_layer(s);
    bool changed = false;
    /* src == dst = identity = no change, but planes are echoed. */
    CHECK(run_json(s, "edit.perspectiveWarp",
                   "{\"planes\":[{\"src\":[[0,0],[32,0],[32,32],[0,32]],"
                   "\"dst\":[[0,0],[32,0],[32,32],[0,32]]}]}",
                   &changed) == PC_OK);
    CHECK(!changed);
    /* A shifted top edge warps. */
    CHECK(run_json(s, "edit.perspectiveWarp",
                   "{\"planes\":[{\"src\":[[0,0],[32,0],[32,32],[0,32]],"
                   "\"dst\":[[4,2],[28,2],[32,32],[0,32]]}]}",
                   &changed) == PC_OK);
    CHECK(changed);
    pc_engine_session_free(s);
}

static void test_graceful(void)
{
    /* No document: every warp is a clean error, not a crash. */
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(run_json(s, "filter.liquify", "{\"strokes\":[]}", NULL) != PC_OK);
    CHECK(run_json(s, "edit.puppetWarp", "{\"pins\":[]}", NULL) != PC_OK);
    CHECK(run_json(s, "edit.perspectiveWarp", "{\"planes\":[]}", NULL) != PC_OK);
    pc_engine_session_free(s);

    /* Adversarial params on a real document are errors, never crashes. */
    s = mk(16, 16);
    (void)add_layer(s);
    CHECK(run_json(s, "filter.liquify", "{\"strokes\":\"notarray\"}", NULL) != PC_OK);
    CHECK(run_json(s, "filter.liquify", "{\"meshSize\":1000}", NULL) != PC_OK);
    CHECK(run_json(s, "edit.puppetWarp", "{\"pins\":[{\"src\":[0,0]}]}", NULL) != PC_OK);
    CHECK(run_json(s, "edit.puppetWarp", "{\"expansion\":99999}", NULL) != PC_OK);
    CHECK(run_json(s, "edit.perspectiveWarp", "{}", NULL) != PC_OK);        /* no planes */
    CHECK(run_json(s, "edit.perspectiveWarp", "{\"planes\":[]}", NULL) != PC_OK); /* empty */
    CHECK(run_json(s, "edit.perspectiveWarp", "{\"planes\":\"x\"}", NULL) != PC_OK);
    CHECK(run_json(s, "edit.perspectiveWarp",
                   "{\"planes\":[{\"src\":[[0,0],[1,0]],\"dst\":[[0,0]]}]}", NULL) != PC_OK);
    pc_engine_session_free(s);
}

/* The smart-filter re-apply hook runs a stored Liquify onto a plain surface. */
static void test_apply_hook(void)
{
    pc_surface surf;
    pc_surface_init(&surf, PC_PIXEL_FORMAT_RGBA8);
    float col[4] = {0.3f, 0.6f, 0.9f, 1.0f};
    pc_error e = {0};
    CHECK(pc_surface_fill_rect(&surf, pc_rect_from_xywh(0, 0, 32, 32), col, 4, &e) == PC_OK);
    pc_rect canvas = pc_rect_from_xywh(0, 0, 32, 32);

    static const char *pjson = "{\"strokes\":[{\"tool\":\"forwardWarp\",\"size\":20,"
                               "\"points\":[[8,16],[24,16]]}]}";
    yyjson_doc *pd = yyjson_read(pjson, strlen(pjson), 0);
    CHECK(pd != NULL);
    yyjson_val *params = pd != NULL ? yyjson_doc_get_root(pd) : NULL;
    pc_surface out;
    bool applied = false;
    pc_status rc = pc_engine_distort_apply_to_surface("filter.liquify", params, &surf, canvas, &out,
                                                      &applied, &e);
    CHECK(rc == PC_OK);
    CHECK(applied);
    if (applied) {
        pc_surface_fini(&out);
    }
    /* A non-distort id leaves applied=false. */
    applied = true;
    rc = pc_engine_distort_apply_to_surface("filter.blur.gaussianBlur", params, &surf, canvas, &out,
                                            &applied, &e);
    CHECK(rc == PC_OK);
    CHECK(!applied);
    if (pd != NULL) {
        yyjson_doc_free(pd);
    }
    pc_surface_fini(&surf);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_registry);
    RUN(test_liquify);
    RUN(test_puppet);
    RUN(test_perspective);
    RUN(test_graceful);
    RUN(test_apply_hook);
    return pc_t_finish();
}
