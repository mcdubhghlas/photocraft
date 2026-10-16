/* Layer > Layer Style > Copy / Paste Layer Style (layerstyle_copy_cmds.c). Copies the active
 * layer's effects + blend + fill opacity + advanced blending into the session clipboard, then
 * pastes them onto another layer; covers the empty-clipboard refusal and no-document grace. */
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "doc/effects.h"
#include "engine/engine.h"
#include "engine/layer_cmds.h"
#include "harness.h"

const pc_engine_command *pc_engine_layerstyle_copy_specs(size_t *n);

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
    pc_blend_mode blend;
    pc_layer_id id;
};

static pc_status add_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *v, pc_error *e)
{
    struct addctx *c = v;
    pc_layer l;
    if (pc_layer_raster(&l, "L", PC_PIXEL_FORMAT_RGBA8, e) != PC_OK) {
        return e != NULL ? e->code : PC_ERR_NOMEM;
    }
    l.blend = c->blend;
    l.fill_opacity = 0.5f;
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

static pc_layer_id add_layer(pc_engine_session *s, bool shadow, pc_blend_mode blend)
{
    struct addctx c = {shadow, blend, 0};
    CHECK(pc_engine_session_edit(s, "add", add_edit, &c, NULL) == PC_OK);
    return c.id;
}

static const pc_engine_command *find_spec(const char *id)
{
    size_t n = 0;
    const pc_engine_command *sp = pc_engine_layerstyle_copy_specs(&n);
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
    pc_error e = {0};
    return c != NULL ? c->enabled(s, &e) : PC_ERR_INVALID;
}

static const pc_layer *layer_of(pc_engine_session *s, pc_layer_id id)
{
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    return pc_document_layer(doc, id);
}

static void test_registry(void)
{
    size_t n = 0;
    (void)pc_engine_layerstyle_copy_specs(&n);
    CHECK_EQ_UINT(n, 2);
    CHECK(find_spec("layer.layerStyle.copyLayerStyle") != NULL);
    CHECK(find_spec("layer.layerStyle.pasteLayerStyle") != NULL);
}

static void test_copy_paste(void)
{
    pc_engine_session *s = mk(16, 16);
    pc_layer_id src = add_layer(s, true, PC_BLEND_MULTIPLY);  /* top, active */
    pc_layer_id dst = add_layer(s, false, PC_BLEND_NORMAL);   /* now top, active */
    /* Paste is disabled until something is copied. */
    CHECK(enabled_of(s, "layer.layerStyle.pasteLayerStyle") != PC_OK);
    /* Copy from src explicitly. */
    char j[64];
    snprintf(j, sizeof j, "{\"layer\":%llu}", (unsigned long long)src);
    CHECK(run_cmd(s, "layer.layerStyle.copyLayerStyle", j) == PC_OK);
    CHECK(enabled_of(s, "layer.layerStyle.pasteLayerStyle") == PC_OK);
    /* Paste onto dst. */
    snprintf(j, sizeof j, "{\"layer\":%llu}", (unsigned long long)dst);
    CHECK(run_cmd(s, "layer.layerStyle.pasteLayerStyle", j) == PC_OK);
    const pc_layer *l = layer_of(s, dst);
    CHECK(l != NULL);
    CHECK(l->blend == PC_BLEND_MULTIPLY);
    CHECK_NEAR(l->fill_opacity, 0.5f, 0.01f);
    CHECK(l->effects.nitems == 1 && l->effects.items[0].kind == PC_FX_DROP_SHADOW);
    pc_engine_session_free(s);
}

static void test_graceful(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(run_cmd(s, "layer.layerStyle.copyLayerStyle", NULL) != PC_OK);
    CHECK(run_cmd(s, "layer.layerStyle.pasteLayerStyle", NULL) != PC_OK);
    pc_engine_session_free(s);

    s = mk(16, 16);
    (void)add_layer(s, false, PC_BLEND_NORMAL);
    /* Paste with nothing copied refuses; bad layer id never crashes. */
    CHECK(run_cmd(s, "layer.layerStyle.pasteLayerStyle", NULL) != PC_OK);
    CHECK(run_cmd(s, "layer.layerStyle.copyLayerStyle", "{\"layer\":999999}") != PC_OK);
    CHECK(run_cmd(s, "layer.layerStyle.copyLayerStyle", "{\"layer\":\"x\"}") != PC_OK);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_registry);
    RUN(test_copy_paste);
    RUN(test_graceful);
    return 0;
}
