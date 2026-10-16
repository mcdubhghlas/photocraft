/* File > Scripts > Flatten All Layer Effects / Flatten All Masks (flatten_gap_cmds.c). A layer
 * with a drop shadow is baked to a raster that keeps its mask; a masked raster layer is baked with
 * the mask folded in and the mask removed; a masked non-raster layer is skipped. No document is a
 * graceful error. */
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "doc/effects.h"
#include "engine/engine.h"
#include "engine/flatten_gap_cmds.h"
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
    bool shadow, mask;
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
    float col[4] = {0.2f, 0.4f, 0.8f, 1.0f};
    if (s != NULL) {
        (void)pc_surface_fill_rect(s, pc_rect_from_xywh(2, 2, 8, 8), col, 4, e);
    }
    if (c->shadow) {
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
    if (c->mask) {
        pc_layer_mask m;
        pc_layer_mask_reveal_all(&m);
        float half[1] = {0.5f};
        (void)pc_surface_fill_rect(&m.surface, pc_rect_from_xywh(0, 0, 16, 16), half, 1, e);
        l.mask = m;
        l.has_mask = true;
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

static pc_layer_id add_layer(pc_engine_session *s, bool shadow, bool mask)
{
    struct addctx c = {shadow, mask, 0};
    CHECK(pc_engine_session_edit(s, "add", add_edit, &c, NULL) == PC_OK);
    return c.id;
}

static const pc_engine_command *find_spec(const char *id)
{
    size_t n = 0;
    const pc_engine_command *sp = pc_engine_flatten_gap_specs(&n);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(sp[i].id, id) == 0) {
            return &sp[i];
        }
    }
    return NULL;
}

static pc_status run_cmd(pc_engine_session *s, const char *id)
{
    const pc_engine_command *c = find_spec(id);
    CHECK(c != NULL);
    if (c == NULL) {
        return PC_ERR_INVALID;
    }
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error e = {0};
    pc_status rc = c->run(s, NULL, &out, &e);
    pc_json_value_fini(&out);
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
    (void)pc_engine_flatten_gap_specs(&n);
    CHECK_EQ_UINT(n, 2);
    CHECK(find_spec("file.scripts.flattenAllLayerEffects") != NULL);
    CHECK(find_spec("file.scripts.flattenAllMasks") != NULL);
}

static void test_flatten_effects(void)
{
    pc_engine_session *s = mk(16, 16);
    pc_layer_id id = add_layer(s, true, false);
    CHECK(run_cmd(s, "file.scripts.flattenAllLayerEffects") == PC_OK);
    const pc_layer *l = layer_of(s, id);
    CHECK(l != NULL);
    CHECK(l->content.kind == PC_LAYER_RASTER);
    CHECK(l->effects.nitems == 0); /* effects baked away */
    pc_engine_session_free(s);
}

static void test_flatten_masks(void)
{
    pc_engine_session *s = mk(16, 16);
    pc_layer_id id = add_layer(s, false, true);
    CHECK(layer_of(s, id)->has_mask);
    CHECK(run_cmd(s, "file.scripts.flattenAllMasks") == PC_OK);
    const pc_layer *l = layer_of(s, id);
    CHECK(l != NULL);
    CHECK(l->content.kind == PC_LAYER_RASTER);
    CHECK(!l->has_mask); /* mask folded in and removed */
    pc_engine_session_free(s);
}

static void test_no_crash_no_masks(void)
{
    /* A document with nothing to flatten returns 0 flattened, gracefully. */
    pc_engine_session *s = mk(16, 16);
    (void)add_layer(s, false, false);
    CHECK(run_cmd(s, "file.scripts.flattenAllLayerEffects") == PC_OK);
    CHECK(run_cmd(s, "file.scripts.flattenAllMasks") == PC_OK);
    pc_engine_session_free(s);
}

static void test_graceful(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(run_cmd(s, "file.scripts.flattenAllLayerEffects") != PC_OK);
    CHECK(run_cmd(s, "file.scripts.flattenAllMasks") != PC_OK);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_registry);
    RUN(test_flatten_effects);
    RUN(test_flatten_masks);
    RUN(test_no_crash_no_masks);
    RUN(test_graceful);
    return 0;
}
