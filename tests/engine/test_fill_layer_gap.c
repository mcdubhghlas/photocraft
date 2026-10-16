/* Layer > New Fill Layer: Solid Color / Gradient (fill_layer_gap_cmds.c). Exercises the two specs
 * directly through pc_engine_fill_layer_gap_specs: a solid-colour fill layer carries the chosen
 * colour, a gradient fill layer carries two stops, an active selection becomes the new layer's
 * mask, and the commands refuse gracefully with no document. */
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "doc/fill.h"
#include "engine/fill_layer_gap_cmds.h"
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

static const pc_engine_command *find_spec(const char *id)
{
    size_t n = 0;
    const pc_engine_command *sp = pc_engine_fill_layer_gap_specs(&n);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(sp[i].id, id) == 0) {
            return &sp[i];
        }
    }
    return NULL;
}

static pc_status run_cmd(pc_engine_session *s, const char *id, const char *json, pc_layer_id *out_id)
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
    if (rc == PC_OK && out_id != NULL) {
        /* The new fill layer becomes the active layer. */
        const pc_engine_doc_state *d = pc_engine_session_active(s);
        *out_id = (d != NULL && d->has_active_layer) ? d->active_layer : 0;
    }
    return rc;
}

static const pc_layer *layer_of(pc_engine_session *s, pc_layer_id id)
{
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    return pc_document_layer(doc, id);
}

static void test_registry(void)
{
    size_t n = 0;
    (void)pc_engine_fill_layer_gap_specs(&n);
    CHECK_EQ_UINT(n, 2);
    CHECK(find_spec("layer.newFillLayer.solidColor") != NULL);
    CHECK(find_spec("layer.newFillLayer.gradient") != NULL);
}

static void test_solid(void)
{
    pc_engine_session *s = mk(16, 16);
    pc_layer_id id = 0;
    CHECK(run_cmd(s, "layer.newFillLayer.solidColor", "{\"color\":\"#ff0000\"}", &id) == PC_OK);
    CHECK(id != 0);
    const pc_layer *l = layer_of(s, id);
    CHECK(l != NULL && l->content.kind == PC_LAYER_FILL);
    CHECK(l->content.u.fill.kind == PC_FILL_SOLID);
    /* Red at full strength in RGB. */
    pc_color c = l->content.u.fill.u.solid;
    CHECK_NEAR(c.c[0], 1.0f, 0.02f);
    CHECK_NEAR(c.c[1], 0.0f, 0.02f);
    pc_engine_session_free(s);
}

static void test_gradient(void)
{
    pc_engine_session *s = mk(16, 16);
    pc_layer_id id = 0;
    CHECK(run_cmd(s, "layer.newFillLayer.gradient",
                  "{\"from\":\"#000000\",\"to\":\"#ffffff\",\"angle\":45,\"style\":\"radial\"}", &id) ==
          PC_OK);
    CHECK(id != 0);
    const pc_layer *l = layer_of(s, id);
    CHECK(l != NULL && l->content.kind == PC_LAYER_FILL);
    CHECK(l->content.u.fill.kind == PC_FILL_GRADIENT);
    CHECK_EQ_UINT(l->content.u.fill.u.gradient.nstops, 2);
    pc_engine_session_free(s);
}

static void test_selection_becomes_mask(void)
{
    pc_engine_session *s = mk(16, 16);
    /* Make a rectangular selection, then a fill layer: it should carry a layer mask. */
    const pc_engine_command *sel = pc_engine_command_find("select.rect");
    CHECK(sel != NULL);
    if (sel != NULL) {
        yyjson_doc *pd = NULL;
        const char *j = "{\"x\":2,\"y\":2,\"width\":6,\"height\":6}";
        yyjson_val *params = NULL;
        if (pc_json_parse(j, strlen(j), &pd, NULL) == PC_OK) {
            params = yyjson_doc_get_root(pd);
        }
        pc_json_value o = PC_JSON_VALUE_INIT;
        pc_error e = {0};
        CHECK(sel->run(s, params, &o, &e) == PC_OK);
        pc_json_value_fini(&o);
        if (pd != NULL) {
            yyjson_doc_free(pd);
        }
    }
    pc_layer_id id = 0;
    CHECK(run_cmd(s, "layer.newFillLayer.solidColor", "{\"color\":\"#00ff00\"}", &id) == PC_OK);
    const pc_layer *l = layer_of(s, id);
    CHECK(l != NULL && l->has_mask);
    pc_engine_session_free(s);
}

static void test_graceful(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(run_cmd(s, "layer.newFillLayer.solidColor", "{\"color\":\"#ff0000\"}", NULL) != PC_OK);
    CHECK(run_cmd(s, "layer.newFillLayer.gradient", NULL, NULL) != PC_OK);
    pc_engine_session_free(s);

    s = mk(16, 16);
    /* Hostile params never crash: bad colour falls back to the tool colour, bad angle to default. */
    CHECK(run_cmd(s, "layer.newFillLayer.solidColor", "{\"color\":42}", NULL) == PC_OK);
    CHECK(run_cmd(s, "layer.newFillLayer.gradient", "{\"angle\":\"spin\",\"style\":\"bogus\"}", NULL) == PC_OK);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_registry);
    RUN(test_solid);
    RUN(test_gradient);
    RUN(test_selection_becomes_mask);
    RUN(test_graceful);
    return 0;
}
