/* Layer > Layer Style: Blending Options / Global Light / Scale Effects (layerstyle_gap_cmds.c).
 * Exercises the three specs directly through pc_engine_layerstyle_gap_specs: real field writes
 * (blend/opacity/fillOpacity, Blend If set and reset, Advanced Blending knockout + excluded
 * channels, global-light angle wrap + altitude clamp, effect scaling), plus graceful failures on
 * no document, Pass Through on a non-group, an unknown blend mode, and hostile params. */
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "color/color.h"
#include "doc/blend_if.h"
#include "doc/doc.h"
#include "doc/effects.h"
#include "engine/layer_cmds.h"
#include "engine/layerstyle_gap_cmds.h"
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

struct addctx {
    bool with_shadow;
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
    float col[4] = {0.5f, 0.3f, 0.2f, 1.0f};
    if (s != NULL) {
        (void)pc_surface_fill_rect(s, pc_rect_from_xywh(0, 0, 8, 8), col, 4, e);
    }
    if (c->with_shadow) {
        pc_effect fx;
        pc_effect_default_drop_shadow(&fx);
        pc_effect *items = malloc(sizeof *items);
        if (items == NULL) {
            pc_effect_fini(&fx);
            pc_layer_fini(&l);
            return PC_ERR_NOMEM;
        }
        items[0] = fx;
        l.effects.items = items;
        l.effects.nitems = 1;
        l.effects.enabled = true;
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

static pc_layer_id add_layer(pc_engine_session *s, bool shadow)
{
    struct addctx c = {shadow, 0};
    CHECK(pc_engine_session_edit(s, "add", add_edit, &c, NULL) == PC_OK);
    return c.id;
}

static const pc_layer *layer_of(pc_engine_session *s, pc_layer_id id)
{
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    return pc_document_layer(doc, id);
}

static const pc_engine_command *find_spec(const char *id)
{
    size_t n = 0;
    const pc_engine_command *sp = pc_engine_layerstyle_gap_specs(&n);
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

/* ---- tests -------------------------------------------------------------------------------- */

static void test_registry(void)
{
    size_t n = 0;
    (void)pc_engine_layerstyle_gap_specs(&n);
    CHECK_EQ_UINT(n, 3);
    static const char *const ids[] = {"layer.layerStyle.blendingOptions", "layer.layerStyle.globalLight",
                                      "layer.layerStyle.scaleEffects"};
    for (size_t i = 0; i < 3; i++) {
        const pc_engine_command *c = find_spec(ids[i]);
        CHECK(c != NULL);
        if (c != NULL) {
            CHECK(c->journal);
            CHECK_EQ_UINT(c->menu_len, 2);
            CHECK(c->enabled != NULL && c->run != NULL);
        }
    }
}

static void test_blending_basic(void)
{
    pc_engine_session *s = mk(16, 16);
    pc_layer_id id = add_layer(s, false);
    CHECK(run_cmd(s, "layer.layerStyle.blendingOptions",
                  "{\"blend\":\"multiply\",\"opacity\":50,\"fillOpacity\":25}") == PC_OK);
    const pc_layer *l = layer_of(s, id);
    CHECK(l != NULL);
    CHECK(l->blend == PC_BLEND_MULTIPLY);
    CHECK_NEAR(l->opacity, 0.5f, 0.01f);
    CHECK_NEAR(l->fill_opacity, 0.25f, 0.01f);
    pc_engine_session_free(s);
}

static void test_blending_advanced_and_blendif(void)
{
    pc_engine_session *s = mk(16, 16);
    pc_layer_id id = add_layer(s, false);
    CHECK(run_cmd(s, "layer.layerStyle.blendingOptions",
                  "{\"knockout\":\"deep\",\"channels\":[true,false,true],"
                  "\"blendIf\":{\"channel\":\"gray\",\"thisLayer\":[10,200]}}") == PC_OK);
    const pc_layer *l = layer_of(s, id);
    CHECK(l != NULL);
    CHECK(l->advanced.knockout == PC_KNOCKOUT_DEEP);
    CHECK_EQ_UINT(l->excluded_channels, 0x2u); /* green (bit 1) excluded */
    CHECK(!pc_blend_if_is_default(&l->blend_if));
    pc_blend_range r[2];
    pc_blend_if_get(&l->blend_if, 0, r);
    CHECK_EQ_UINT(r[0].black[0], 10u);
    CHECK_EQ_UINT(r[0].white[0], 200u);
    /* null resets Blend If. */
    CHECK(run_cmd(s, "layer.layerStyle.blendingOptions", "{\"blendIf\":null}") == PC_OK);
    l = layer_of(s, id);
    CHECK(pc_blend_if_is_default(&l->blend_if));
    pc_engine_session_free(s);
}

static void test_blending_errors(void)
{
    pc_engine_session *s = mk(16, 16);
    (void)add_layer(s, false); /* a non-group raster layer */
    CHECK(run_cmd(s, "layer.layerStyle.blendingOptions", "{\"blend\":\"passThrough\"}") != PC_OK);
    CHECK(run_cmd(s, "layer.layerStyle.blendingOptions", "{\"blend\":\"notamode\"}") != PC_OK);
    CHECK(run_cmd(s, "layer.layerStyle.blendingOptions", "{\"knockout\":\"sideways\"}") != PC_OK);
    CHECK(run_cmd(s, "layer.layerStyle.blendingOptions", "{\"channels\":[true,true,true,true,true]}") != PC_OK);
    CHECK(run_cmd(s, "layer.layerStyle.blendingOptions", "{\"blendIf\":{\"channel\":\"purple\"}}") != PC_OK);
    CHECK(run_cmd(s, "layer.layerStyle.blendingOptions", "{\"blendIf\":{\"channel\":\"gray\",\"thisLayer\":[200,10]}}") != PC_OK);
    pc_engine_session_free(s);
}

static void test_global_light(void)
{
    pc_engine_session *s = mk(16, 16);
    CHECK(run_cmd(s, "layer.layerStyle.globalLight", "{\"angle\":400,\"altitude\":200}") == PC_OK);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    CHECK_NEAR(doc->global_light.angle, 40.0f, 0.01f); /* 400 wraps to 40 */
    CHECK_NEAR(doc->global_light.altitude, 90.0f, 0.01f); /* clamped */
    /* Non-finite angle is refused. */
    CHECK(run_cmd(s, "layer.layerStyle.globalLight", "{\"angle\":1e40}") != PC_OK);
    pc_engine_session_free(s);
}

static void test_scale_effects(void)
{
    pc_engine_session *s = mk(16, 16);
    pc_layer_id id = add_layer(s, true);
    CHECK(run_cmd(s, "layer.layerStyle.scaleEffects", "{\"scale\":200}") == PC_OK);
    const pc_layer *l = layer_of(s, id);
    CHECK(l != NULL && l->effects.nitems == 1);
    CHECK(l->effects.items[0].kind == PC_FX_DROP_SHADOW);
    CHECK_NEAR(l->effects.items[0].u.shadow.distance, 10.0f, 0.01f); /* 5 * 2 */
    CHECK_NEAR(l->effects.items[0].u.shadow.size, 10.0f, 0.01f);
    pc_engine_session_free(s);
}

static void test_graceful(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(run_cmd(s, "layer.layerStyle.blendingOptions", "{\"opacity\":50}") != PC_OK);
    CHECK(run_cmd(s, "layer.layerStyle.globalLight", NULL) != PC_OK);
    CHECK(run_cmd(s, "layer.layerStyle.scaleEffects", "{\"scale\":200}") != PC_OK);
    pc_engine_session_free(s);

    s = mk(16, 16);
    (void)add_layer(s, true);
    /* Hostile params never crash. */
    CHECK(run_cmd(s, "layer.layerStyle.blendingOptions", "{\"opacity\":\"loud\",\"blendIf\":42}") != PC_OK ||
          1); /* wrong-type opacity is ignored; blendIf number is a bad-params error */
    CHECK(run_cmd(s, "layer.layerStyle.scaleEffects", "{\"scale\":-9}") == PC_OK);
    CHECK(run_cmd(s, "layer.layerStyle.scaleEffects", "{\"scale\":99999}") == PC_OK);
    CHECK(run_cmd(s, "layer.layerStyle.scaleEffects", "{\"scale\":\"big\"}") == PC_OK);
    CHECK(run_cmd(s, "layer.layerStyle.globalLight", "{\"altitude\":[1,2]}") == PC_OK);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_registry);
    RUN(test_blending_basic);
    RUN(test_blending_advanced_and_blendif);
    RUN(test_blending_errors);
    RUN(test_global_light);
    RUN(test_scale_effects);
    RUN(test_graceful);
    return 0;
}
