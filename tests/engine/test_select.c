/* Selection tools, magnetic lasso, sky/isolate, smart select (port of a subset of the E7 selection
 * tests). */
#include <stdlib.h>
#include <string.h>

#include "algo/selection.h"
#include "color/color.h"
#include "engine/engine_internal.h"
#include "engine/selection_cmds.h"
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

struct fillctx {
    pc_rect r;
    float col[4];
};

static pc_status fill_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *v, pc_error *e)
{
    (void)ha;
    (void)a;
    struct fillctx *c = v;
    if (doc->nlayers == 0) {
        return PC_OK;
    }
    pc_surface *s = pc_layer_surface_mut(&doc->layers[0]);
    if (s == NULL) {
        return PC_OK;
    }
    return pc_surface_fill_rect(s, c->r, c->col, 4, e);
}

static void fill(pc_engine_session *s, int x, int y, int w, int h, float r, float g, float b)
{
    struct fillctx c = {pc_rect_from_xywh(x, y, (uint32_t)w, (uint32_t)h), {r, g, b, 1.0f}};
    CHECK(pc_engine_session_edit(s, "paint", fill_edit, &c, NULL) == PC_OK);
}

static const pc_document *doc_of(pc_engine_session *s)
{
    return pc_doc_arc_get(pc_engine_session_active(s)->doc);
}

static float cov(pc_engine_session *s, int x, int y)
{
    const pc_document *doc = doc_of(s);
    return doc->has_selection ? pc_surface_sample_channel(&doc->selection, x, y, 0) : 0.0f;
}

static pc_status rn(pc_engine_session *s, const char *id, const char *json, pc_json_value *out)
{
    return pc_engine_session_execute_json(s, id, json, out, NULL);
}

static void ok(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out;
    pc_error err = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, json, &out, &err);
    CHECK_EQ_STR(rc == PC_OK ? "ok" : err.msg, "ok");
    pc_json_value_fini(&out);
}

/* Select a rectangle via a direct edit (select.rect is not an E7 command). */
static pc_status rectsel_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *v, pc_error *e)
{
    (void)ha;
    (void)a;
    struct fillctx *c = v;
    pc_rect area = pc_document_bounds(doc);
    uint32_t w = pc_rect_width(area), h = pc_rect_height(area);
    size_t npx = (size_t)w * h;
    float *m = calloc(npx ? npx : 1, sizeof(float));
    if (m == NULL) {
        return pc_fail(e, PC_ERR_NOMEM, "m");
    }
    for (int y = c->r.y0; y < c->r.y1; y++) {
        for (int x = c->r.x0; x < c->r.x1; x++) {
            if (x >= 0 && y >= 0 && (uint32_t)x < w && (uint32_t)y < h) {
                m[(size_t)y * w + (size_t)x] = 1.0f;
            }
        }
    }
    pc_surface out;
    bool some = false;
    pc_status rc = pc_algo_selection_combine(NULL, m, npx, area, PC_ALGO_SELECTION_REPLACE, &out, &some, e);
    free(m);
    if (rc != PC_OK) {
        return rc;
    }
    if (doc->has_selection) {
        pc_surface_fini(&doc->selection);
        doc->has_selection = false;
    }
    if (some) {
        doc->selection = out;
        doc->has_selection = true;
    } else {
        pc_surface_fini(&out);
    }
    return PC_OK;
}

static void sel_rect(pc_engine_session *s, int x, int y, int w, int h)
{
    struct fillctx c = {{x, y, x + w, y + h}, {0, 0, 0, 0}};
    CHECK(pc_engine_session_edit(s, "sel", rectsel_edit, &c, NULL) == PC_OK);
}

/* ---- tests ------------------------------------------------------------------------------- */

static void test_magic_wand(void)
{
    pc_engine_session *s = mk(40, 30);
    fill(s, 5, 5, 10, 10, 1, 0, 0); /* red square x[5,15) y[5,15) */
    ok(s, "select.magicWand", "{\"x\": 7, \"y\": 7, \"tolerance\": 0, \"antiAlias\": false}");
    CHECK_NEAR(cov(s, 7, 7), 1.0f, 1e-3f);
    CHECK_NEAR(cov(s, 2, 2), 0.0f, 1e-3f); /* white background outside the red */
    CHECK_NEAR(cov(s, 30, 10), 0.0f, 1e-3f);
    /* Add the white background too. */
    ok(s, "select.magicWand", "{\"x\": 0, \"y\": 0, \"tolerance\": 0, \"antiAlias\": false, \"mode\": \"add\"}");
    CHECK_NEAR(cov(s, 0, 0), 1.0f, 1e-3f);
    pc_engine_session_free(s);
}

static void test_modify(void)
{
    pc_engine_session *s = mk(40, 30);
    sel_rect(s, 10, 10, 10, 10); /* x[10,20) y[10,20) */
    ok(s, "select.modify.expand", "{\"radius\": 2}");
    CHECK_NEAR(cov(s, 8, 15), 1.0f, 1e-3f);
    ok(s, "select.modify.contract", "{\"radius\": 4}");
    CHECK_NEAR(cov(s, 15, 15), 1.0f, 1e-3f);
    CHECK_NEAR(cov(s, 9, 15), 0.0f, 1e-3f);
    sel_rect(s, 10, 10, 10, 10);
    ok(s, "select.modify.feather", "{\"radius\": 4}");
    float e = cov(s, 10, 15);
    CHECK(e > 0.1f && e < 0.9f);
    /* Out-of-range radius refused, selection untouched. */
    pc_json_value out;
    CHECK(rn(s, "select.modify.expand", "{\"radius\": -5}", &out) != PC_OK);
    CHECK(rn(s, "select.modify.border", "{\"radius\": 5000}", &out) != PC_OK);
    CHECK(doc_of(s)->has_selection);
    pc_engine_session_free(s);
}

static void test_grow_similar(void)
{
    pc_engine_session *s = mk(40, 30);
    fill(s, 5, 5, 15, 15, 1, 0, 0);  /* big red block */
    fill(s, 25, 5, 10, 10, 1, 0, 0); /* separate red block */
    sel_rect(s, 6, 6, 2, 2);
    ok(s, "select.grow", "{\"tolerance\": 5}");
    CHECK_NEAR(cov(s, 14, 14), 1.0f, 1e-3f);
    CHECK_NEAR(cov(s, 30, 10), 0.0f, 1e-3f); /* grow is contiguous */
    ok(s, "select.similar", "{\"tolerance\": 5}");
    CHECK_NEAR(cov(s, 30, 10), 1.0f, 1e-3f); /* similar reaches the other block */
    pc_engine_session_free(s);
}

static void test_lasso(void)
{
    pc_engine_session *s = mk(40, 30);
    ok(s, "select.lasso", "{\"points\": [[0,0],[20,0],[0,20]]}");
    CHECK_NEAR(cov(s, 2, 2), 1.0f, 1e-3f);
    CHECK_NEAR(cov(s, 18, 18), 0.0f, 1e-3f);
    pc_json_value out;
    CHECK(rn(s, "select.lasso", "{\"points\": [[0,0],[1,1]]}", &out) != PC_OK);
    pc_engine_session_free(s);
}

static void test_magnetic_outline(void)
{
    pc_engine_session *s = mk(40, 30);
    /* trace=false commits the outline directly. */
    ok(s, "select.magneticLasso", "{\"points\": [[0,0],[20,0],[0,20]], \"trace\": false}");
    CHECK_NEAR(cov(s, 2, 2), 1.0f, 1e-3f);
    CHECK_NEAR(cov(s, 18, 18), 0.0f, 1e-3f);
    pc_json_value out;
    CHECK(rn(s, "select.magneticLasso", "{\"points\": [[0,0],[1,1]], \"trace\": false}", &out) != PC_OK);
    /* trace=true runs the tracer (no crash). */
    fill(s, 5, 5, 20, 20, 0.1f, 0.2f, 0.9f);
    pc_error err = {0};
    pc_status rc = pc_engine_session_execute_json(
        s, "select.magneticLasso", "{\"points\": [[5,5],[25,5],[25,25],[5,25]]}", &out, &err);
    if (rc == PC_OK) {
        pc_json_value_fini(&out);
    }
    pc_engine_session_free(s);
}

static pc_status add_layer_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *v, pc_error *e)
{
    (void)v;
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    pc_layer l;
    pc_status rc = pc_layer_raster(&l, "Layer 1", fmt, e);
    if (rc != PC_OK) {
        return rc;
    }
    pc_layer_id nid;
    rc = pc_document_insert_above(doc, NULL, &l, &nid, e);
    if (rc != PC_OK) {
        pc_layer_fini(&l);
        return rc;
    }
    *ha = true;
    *active = nid;
    return PC_OK;
}

static void test_isolate(void)
{
    pc_engine_session *s = mk(20, 20);
    CHECK(pc_engine_session_edit(s, "new layer", add_layer_edit, NULL, NULL) == PC_OK);
    pc_json_value out;
    CHECK(rn(s, "select.isolateLayers", "{}", &out) == PC_OK);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&out), "isolated")));
    pc_json_value_fini(&out);
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    CHECK(d->nisolated >= 1);
    pc_layer_id bg = doc_of(s)->layers[0].id;
    pc_layer_id top = doc_of(s)->layers[doc_of(s)->nlayers - 1].id;
    CHECK(pc_engine_isolation_shows(doc_of(s), d->isolated_layers, d->nisolated, top));
    CHECK(!pc_engine_isolation_shows(doc_of(s), d->isolated_layers, d->nisolated, bg));
    /* inspect reports it. */
    CHECK(rn(s, "document.inspect", "{}", &out) == PC_OK);
    yyjson_mut_val *iso = yyjson_mut_obj_get(pc_json_value_root(&out), "isolatedLayers");
    CHECK(iso != NULL && yyjson_mut_arr_size(iso) >= 1);
    pc_json_value_fini(&out);
    /* Toggle off. */
    CHECK(rn(s, "select.isolateLayers", "{}", &out) == PC_OK);
    CHECK(!yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&out), "isolated")));
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_active(s)->nisolated == 0);
    pc_engine_session_free(s);
}

static void test_quick_and_smart(void)
{
    pc_engine_session *s = mk(140, 100);
    fill(s, 0, 0, 60, 100, 0.8f, 0.5f, 0.3f);   /* warm left */
    fill(s, 60, 0, 80, 100, 0.2f, 0.35f, 0.6f); /* cool right */
    pc_json_value out;
    pc_error err = {0};
    pc_status rc = pc_engine_session_execute_json(
        s, "select.quick", "{\"points\": [[20,50],[35,50]], \"size\": 8}", &out, &err);
    CHECK_EQ_STR(rc == PC_OK ? "ok" : err.msg, "ok");
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&out), "selected")));
    pc_json_value_fini(&out);
    CHECK(cov(s, 10, 50) >= 0.5f);

    /* Focus area / subject just need to run without crashing. */
    rc = pc_engine_session_execute_json(s, "select.focusArea", "{}", &out, NULL);
    if (rc == PC_OK) {
        pc_json_value_fini(&out);
    }
    rc = pc_engine_session_execute_json(s, "select.subject", "{}", &out, NULL);
    if (rc == PC_OK) {
        pc_json_value_fini(&out);
    }
    pc_engine_session_free(s);
}

static void test_refine_edge(void)
{
    pc_engine_session *s = mk(80, 40);
    fill(s, 0, 0, 40, 40, 0.9f, 0.8f, 0.2f);
    fill(s, 40, 0, 40, 40, 0.1f, 0.2f, 0.6f);
    sel_rect(s, 0, 0, 40, 40);
    pc_json_value out;
    pc_error err = {0};
    pc_status rc = pc_engine_session_execute_json(s, "select.refineEdge", "{\"radius\": 8}", &out, &err);
    CHECK_EQ_STR(rc == PC_OK ? "ok" : err.msg, "ok");
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(pc_json_value_root(&out), "output")), "selection");
    pc_json_value_fini(&out);
    CHECK(doc_of(s)->has_selection);

    /* Layer-mask output: the active layer gains a mask, selection cleared. */
    sel_rect(s, 0, 0, 40, 40);
    rc = pc_engine_session_execute_json(s, "select.refineEdge", "{\"radius\": 8, \"output\": \"layerMask\"}",
                                        &out, &err);
    CHECK_EQ_STR(rc == PC_OK ? "ok" : err.msg, "ok");
    pc_json_value_fini(&out);
    CHECK(doc_of(s)->layers[0].has_mask);
    CHECK(!doc_of(s)->has_selection);

    /* New-layer output adds a layer. */
    sel_rect(s, 0, 0, 40, 40);
    size_t before = doc_of(s)->nlayers;
    rc = pc_engine_session_execute_json(s, "select.refineEdge", "{\"radius\": 8, \"output\": \"newLayer\"}",
                                        &out, &err);
    CHECK_EQ_STR(rc == PC_OK ? "ok" : err.msg, "ok");
    pc_json_value_fini(&out);
    CHECK(doc_of(s)->nlayers == before + 1);

    /* Bad output is an error. */
    CHECK(rn(s, "select.refineEdge", "{\"output\": \"bogus\"}", &out) != PC_OK);
    pc_engine_session_free(s);
}

static void test_adversarial(void)
{
    pc_engine_session *s = mk(24, 16);
    const char *cases[][2] = {
        {"select.magicWand", "{\"x\": 1e18, \"y\": -1e18}"},
        {"select.modify.feather", "{\"radius\": \"x\"}"},
        {"select.grow", "{}"},
        {"select.lasso", "{\"points\": \"nope\"}"},
        {"select.magneticLasso", "{\"points\": [[1,2]]}"},
        {"select.sky", "{}"},
        {"select.quick", "{\"points\": []}"},
        {"select.refineEdge", "{\"refineStrokes\": 7}"},
        {"select.refineEdge", "{\"refineStrokes\": [{\"points\": [[1,2]], \"size\": 0}]}"},
        {"select.isolateLayers", "{\"on\": 3}"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pc_json_value out;
        pc_error err = {0};
        pc_status rc = pc_engine_session_execute_json(s, cases[i][0], cases[i][1], &out, &err);
        if (rc == PC_OK) {
            pc_json_value_fini(&out);
        }
    }
    CHECK(doc_of(s) != NULL);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_magic_wand);
    RUN(test_modify);
    RUN(test_grow_similar);
    RUN(test_lasso);
    RUN(test_magnetic_outline);
    RUN(test_isolate);
    RUN(test_quick_and_smart);
    RUN(test_refine_edge);
    RUN(test_adversarial);
    return pc_t_finish();
}
