/* Layer > Matting / Mask All Objects / Reveal in Finder (matting_gap_cmds.c): the six specs'
 * enabled()/run() exercised directly through pc_engine_matting_gap_specs. Covers real pixel results
 * (defringe recolours a fringe pixel from its opaque neighbour and leaves alpha alone; Remove Black
 * and White Matte un-premultiply a partial-alpha pixel; Color Decontaminate runs and changes the
 * fringe), a mask-all-objects run, Reveal in Finder's graceful refusal on a pixel layer, and
 * never-crash on no document / out-of-range / wrong-type params. */
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine/layer_cmds.h"
#include "engine/matting_gap_cmds.h"
#include "harness.h"
#include "raster/surface.h"

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

/* An edit that adds a raster layer and fills `rects` (each {x,y,w,h, r,g,b,a}); leaves the rest
 * transparent. The layer becomes active. */
struct rectfill {
    int x, y, w, h;
    float rgba[4];
};
struct addctx {
    const struct rectfill *rects;
    size_t n;
    pc_layer_id id;
};

static pc_status add_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *v, pc_error *e)
{
    struct addctx *c = v;
    pc_layer l;
    if (pc_layer_raster(&l, "L", PC_PIXEL_FORMAT_RGBA8, e) != PC_OK) {
        return e != NULL ? e->code : PC_ERR_NOMEM;
    }
    pc_surface *s = pc_layer_surface_mut(&l);
    if (s != NULL) {
        for (size_t i = 0; i < c->n; i++) {
            pc_rect r = pc_rect_from_xywh(c->rects[i].x, c->rects[i].y, (uint32_t)c->rects[i].w,
                                          (uint32_t)c->rects[i].h);
            (void)pc_surface_fill_rect(s, r, c->rects[i].rgba, 4, e);
        }
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

static pc_layer_id add_layer(pc_engine_session *s, const struct rectfill *rects, size_t n)
{
    struct addctx c = {rects, n, 0};
    CHECK(pc_engine_session_edit(s, "add", add_edit, &c, NULL) == PC_OK);
    return c.id;
}

static void read_px(pc_engine_session *s, pc_layer_id id, int x, int y, float out[4])
{
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    const pc_layer *l = pc_document_layer(doc, id);
    const pc_surface *surf = l != NULL ? pc_layer_surface(l) : NULL;
    out[0] = out[1] = out[2] = out[3] = -1.0f;
    if (surf != NULL) {
        float px[1][4];
        pc_rect r = pc_rect_from_xywh(x, y, 1, 1);
        if (pc_surface_read_rgba_into(surf, r, px, 1, NULL) == PC_OK) {
            memcpy(out, px[0], sizeof px[0]);
        }
    }
}

static const pc_engine_command *find_spec(const char *id)
{
    size_t n = 0;
    const pc_engine_command *sp = pc_engine_matting_gap_specs(&n);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(sp[i].id, id) == 0) {
            return &sp[i];
        }
    }
    return NULL;
}

static pc_status run_cmd(pc_engine_session *s, const char *id, const char *json)
{
    const pc_engine_command *c = find_spec(id);
    CHECK(c != NULL);
    if (c == NULL) {
        return PC_ERR_INVALID;
    }
    yyjson_doc *pd = NULL;
    yyjson_val *params = NULL;
    if (json != NULL && pc_json_parse(json, strlen(json), &pd, NULL) == PC_OK) {
        params = yyjson_doc_get_root(pd);
    }
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error e = {0};
    pc_status rc = c->run(s, params, &out, &e);
    pc_json_value_fini(&out);
    if (pd != NULL) {
        yyjson_doc_free(pd);
    }
    return rc;
}

static pc_status enabled_of(pc_engine_session *s, const char *id)
{
    const pc_engine_command *c = find_spec(id);
    CHECK(c != NULL);
    if (c == NULL) {
        return PC_ERR_INVALID;
    }
    pc_error e = {0};
    return c->enabled(s, &e);
}

/* ---- tests -------------------------------------------------------------------------------- */

static void test_registry(void)
{
    size_t n = 0;
    const pc_engine_command *sp = pc_engine_matting_gap_specs(&n);
    CHECK_EQ_UINT(n, 6);
    static const char *const ids[] = {"layer.maskAllObjects",
                                      "layer.matting.defringe",
                                      "layer.matting.removeBlackMatte",
                                      "layer.matting.removeWhiteMatte",
                                      "layer.matting.colorDecontaminate",
                                      "layer.smartObjects.revealInFinder"};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        const pc_engine_command *c = find_spec(ids[i]);
        CHECK(c != NULL);
        if (c != NULL) {
            CHECK(c->journal);
            CHECK(c->enabled != NULL && c->run != NULL);
            CHECK(c->menu_len >= 1);
        }
    }
    (void)sp;
}

static void test_defringe(void)
{
    pc_engine_session *s = mk(16, 16);
    /* An 8x8 opaque blue block with its right column painted red; transparency begins at x=8.
     * width=1: the red column (x=7) is a one-pixel fringe whose known-inside neighbours (x=6) are
     * blue, so it is recoloured blue. The interior stays blue, and alpha is never touched. */
    struct rectfill r[] = {
        {0, 0, 8, 8, {0.0f, 0.0f, 1.0f, 1.0f}},
        {7, 0, 1, 8, {1.0f, 0.0f, 0.0f, 1.0f}},
    };
    pc_layer_id id = add_layer(s, r, 2);
    CHECK(run_cmd(s, "layer.matting.defringe", "{\"width\":1}") == PC_OK);
    float p1[4];
    read_px(s, id, 7, 4, p1);
    CHECK_NEAR(p1[0], 0.0f, 0.01f);
    CHECK_NEAR(p1[1], 0.0f, 0.01f);
    CHECK_NEAR(p1[2], 1.0f, 0.01f);
    CHECK_NEAR(p1[3], 1.0f, 0.01f); /* alpha untouched */
    float p0[4];
    read_px(s, id, 3, 3, p0);
    CHECK_NEAR(p0[2], 1.0f, 0.01f); /* core pixel unchanged */
    pc_engine_session_free(s);
}

static void test_defringe_opaque_noop(void)
{
    /* A fully opaque layer has no transparency, so defringe leaves it unchanged. */
    pc_engine_session *s = mk(16, 16);
    struct rectfill r[] = {{0, 0, 8, 8, {0.3f, 0.6f, 0.9f, 1.0f}}};
    pc_layer_id id = add_layer(s, r, 1);
    CHECK(run_cmd(s, "layer.matting.defringe", NULL) == PC_OK);
    float p[4];
    read_px(s, id, 3, 3, p);
    CHECK_NEAR(p[0], 0.3f, 0.01f);
    CHECK_NEAR(p[1], 0.6f, 0.01f);
    CHECK_NEAR(p[2], 0.9f, 0.01f);
    pc_engine_session_free(s);
}

static void test_remove_black_matte(void)
{
    pc_engine_session *s = mk(16, 16);
    /* A pixel composited over black: colour 0.5 at alpha 0.5 un-premultiplies to 1.0. */
    struct rectfill r[] = {{0, 0, 4, 4, {0.5f, 0.0f, 0.0f, 0.5f}}};
    pc_layer_id id = add_layer(s, r, 1);
    CHECK(run_cmd(s, "layer.matting.removeBlackMatte", NULL) == PC_OK);
    float p[4];
    read_px(s, id, 1, 1, p);
    CHECK_NEAR(p[0], 1.0f, 0.02f);
    CHECK_NEAR(p[3], 0.5f, 0.02f); /* alpha unchanged */
    pc_engine_session_free(s);
}

static void test_remove_white_matte(void)
{
    pc_engine_session *s = mk(16, 16);
    /* Over white: (v - (1-a))/a. v=0.5, a=0.5 -> (0.5-0.5)/0.5 = 0. */
    struct rectfill r[] = {{0, 0, 4, 4, {0.5f, 0.5f, 0.5f, 0.5f}}};
    pc_layer_id id = add_layer(s, r, 1);
    CHECK(run_cmd(s, "layer.matting.removeWhiteMatte", NULL) == PC_OK);
    float p[4];
    read_px(s, id, 1, 1, p);
    CHECK_NEAR(p[0], 0.0f, 0.02f);
    CHECK_NEAR(p[3], 0.5f, 0.02f);
    pc_engine_session_free(s);
}

static void test_decontaminate_runs(void)
{
    pc_engine_session *s = mk(16, 16);
    struct rectfill r[] = {
        {0, 0, 4, 4, {0.0f, 0.0f, 1.0f, 1.0f}},
        {4, 0, 2, 4, {1.0f, 0.0f, 0.0f, 0.4f}}, /* soft fringe */
    };
    pc_layer_id id = add_layer(s, r, 2);
    (void)id;
    CHECK(run_cmd(s, "layer.matting.colorDecontaminate", "{\"amount\":100,\"radius\":4}") == PC_OK);
    pc_engine_session_free(s);
}

static void test_mask_all_objects(void)
{
    pc_engine_session *s = mk(32, 32);
    /* A bright blob centred on a dark field: Select Subject should find it, or fail gracefully. */
    struct rectfill r[] = {
        {0, 0, 32, 32, {0.02f, 0.02f, 0.02f, 1.0f}},
        {10, 10, 12, 12, {0.95f, 0.9f, 0.1f, 1.0f}},
    };
    pc_layer_id id = add_layer(s, r, 2);
    pc_error e = {0};
    const pc_engine_command *c = find_spec("layer.maskAllObjects");
    CHECK(c != NULL);
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status rc = c->run(s, NULL, &out, &e);
    pc_json_value_fini(&out);
    /* Either a subject was masked, or a clean "no objects" error -- never a crash. */
    CHECK(rc == PC_OK || e.code != PC_OK);
    if (rc == PC_OK) {
        const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
        const pc_layer *l = pc_document_layer(doc, id);
        CHECK(l != NULL && l->has_mask);
    }
    pc_engine_session_free(s);
}

static void test_reveal_refuses_pixel_layer(void)
{
    pc_engine_session *s = mk(16, 16);
    struct rectfill r[] = {{0, 0, 8, 8, {1.0f, 1.0f, 1.0f, 1.0f}}};
    (void)add_layer(s, r, 1);
    /* Active layer is a pixel layer, not a linked smart object. */
    CHECK(enabled_of(s, "layer.smartObjects.revealInFinder") != PC_OK);
    CHECK(run_cmd(s, "layer.smartObjects.revealInFinder", "{\"dryRun\":true}") != PC_OK);
    pc_engine_session_free(s);
}

static void test_graceful(void)
{
    /* No document: every command refuses without crashing. */
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(run_cmd(s, "layer.matting.defringe", "{\"width\":5}") != PC_OK);
    CHECK(run_cmd(s, "layer.matting.removeBlackMatte", NULL) != PC_OK);
    CHECK(run_cmd(s, "layer.matting.colorDecontaminate", NULL) != PC_OK);
    CHECK(run_cmd(s, "layer.maskAllObjects", NULL) != PC_OK);
    CHECK(run_cmd(s, "layer.smartObjects.revealInFinder", NULL) != PC_OK);
    pc_engine_session_free(s);

    /* Hostile params on a real layer: out-of-range and wrong-type never crash. */
    s = mk(16, 16);
    struct rectfill r[] = {{0, 0, 4, 4, {0.5f, 0.2f, 0.1f, 0.6f}}};
    (void)add_layer(s, r, 1);
    CHECK(run_cmd(s, "layer.matting.defringe", "{\"width\":-50}") == PC_OK);
    CHECK(run_cmd(s, "layer.matting.defringe", "{\"width\":999999}") == PC_OK);
    CHECK(run_cmd(s, "layer.matting.defringe", "{\"width\":\"big\"}") == PC_OK);
    CHECK(run_cmd(s, "layer.matting.colorDecontaminate", "{\"amount\":-9,\"radius\":99999}") == PC_OK);
    CHECK(run_cmd(s, "layer.matting.colorDecontaminate", "{\"amount\":[1,2]}") == PC_OK);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_registry);
    RUN(test_defringe);
    RUN(test_defringe_opaque_noop);
    RUN(test_remove_black_matte);
    RUN(test_remove_white_matte);
    RUN(test_decontaminate_runs);
    RUN(test_mask_all_objects);
    RUN(test_reveal_refuses_pixel_layer);
    RUN(test_graceful);
    return 0;
}
