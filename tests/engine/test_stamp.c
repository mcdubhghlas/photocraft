/* Stamping: layer.stampVisible / layer.stampDown (E4). The layer-adding commands are not ported
 * yet, so extra layers are built through direct edits (as test_pick does). */
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "doc/doc.h"
#include "engine/engine_internal.h"
#include "raster/surface.h"
#include "harness.h"

static pc_status exec(pc_engine_session *s, const char *id, const char *json, pc_json_value *out)
{
    return pc_engine_session_execute_json(s, id, json, out, NULL);
}

static pc_engine_session *mk(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(exec(s, "file.new", "{\"width\":40,\"height\":40,\"background\":\"#ffffff\"}", &o) == PC_OK);
    pc_json_value_fini(&o);
    return s;
}

/* Add a raster layer with a blue square at (0..20, 0..20); *out gets its id. */
static pc_status add_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *v, pc_error *e)
{
    (void)ha;
    pc_layer_id *out = v;
    pc_layer layer;
    if (pc_layer_raster(&layer, "A", PC_PIXEL_FORMAT_RGBA8, e) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    pc_layer_id top = doc->layers[doc->nlayers - 1].id;
    pc_layer_id id;
    if (pc_document_insert_above(doc, &top, &layer, &id, e) != PC_OK) {
        pc_layer_fini(&layer);
        return PC_ERR_NOMEM;
    }
    pc_surface *surf = pc_layer_surface_mut(pc_document_layer_mut(doc, id));
    float blue[4] = {0, 0, 1, 1};
    pc_status rc = pc_surface_fill_rect(surf, pc_rect_from_xywh(0, 0, 20, 20), blue, 4, e);
    *out = id;
    *a = id;
    return rc;
}

static pc_layer_id add_blue(pc_engine_session *s)
{
    pc_layer_id id = 0;
    CHECK(pc_engine_session_edit(s, "add", add_edit, &id, NULL) == PC_OK);
    return id;
}

static void rgba(pc_engine_session *s, pc_layer_id id, int32_t x, int32_t y, float out[4])
{
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    pc_surface_rgba(pc_layer_surface(pc_document_layer(doc, id)), x, y, out);
}

static size_t nlayers(pc_engine_session *s)
{
    return pc_doc_arc_get(pc_engine_session_active(s)->doc)->nlayers;
}

static void test_stamp_visible(void)
{
    pc_engine_session *s = mk();
    (void)add_blue(s);
    size_t before = nlayers(s);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(exec(s, "layer.stampVisible", "{}", &o) == PC_OK);
    pc_layer_id id = (pc_layer_id)yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&o), "layer"));
    pc_json_value_fini(&o);
    CHECK(nlayers(s) == before + 1);
    /* The stamp is the active, selected, visible raster layer; composite = blue square on white. */
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    CHECK(st->has_active_layer && st->active_layer == id && st->nselected == 1);
    float got[4];
    rgba(s, id, 5, 5, got);
    CHECK(got[2] == 1.0f && got[0] == 0.0f); /* blue */
    rgba(s, id, 30, 30, got);
    CHECK(got[0] == 1.0f && got[1] == 1.0f && got[2] == 1.0f); /* white */
    /* One undo step restores the layer list. */
    CHECK(exec(s, "edit.undo", "{}", &o) == PC_OK);
    pc_json_value_fini(&o);
    CHECK(nlayers(s) == before);
    pc_engine_session_free(s);
}

static void test_stamp_down(void)
{
    pc_engine_session *s = mk();
    pc_layer_id bg = pc_engine_session_active(s)->active_layer;
    pc_layer_id a = add_blue(s);
    size_t before = nlayers(s);
    /* Active layer A above the white Background: stamp a copy of A into the Background. */
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(exec(s, "layer.stampDown", "{}", &o) == PC_OK);
    pc_json_value_fini(&o);
    CHECK(nlayers(s) == before); /* no layer added or removed */
    float got[4];
    rgba(s, bg, 5, 5, got);
    CHECK(got[2] == 1.0f && got[0] == 0.0f); /* the blue square stamped into the Background */
    rgba(s, bg, 30, 30, got);
    CHECK(got[0] == 1.0f); /* white elsewhere */
    rgba(s, a, 5, 5, got);
    CHECK(got[2] == 1.0f); /* A stays */
    pc_engine_session_free(s);
}

static void test_adversarial(void)
{
    pc_engine_session *s = mk();
    const char *bad[] = {"[1,2]", "\"x\"", "3"};
    pc_json_value o = PC_JSON_VALUE_INIT;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(exec(s, "layer.stampVisible", bad[i], &o) != PC_OK);
        pc_json_value_fini(&o);
        CHECK(exec(s, "layer.stampDown", bad[i], &o) != PC_OK);
        pc_json_value_fini(&o);
    }
    /* The bottom layer has nothing below: stamp down is an error. */
    pc_layer_id bg = pc_engine_session_active(s)->active_layer;
    (void)bg;
    CHECK(exec(s, "layer.stampDown", "{}", &o) != PC_OK); /* background alone, nothing below */
    pc_json_value_fini(&o);
    pc_engine_session *empty = NULL;
    CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
    CHECK(exec(empty, "layer.stampVisible", "{}", &o) != PC_OK);
    pc_json_value_fini(&o);
    CHECK(exec(empty, "layer.stampDown", "{}", &o) != PC_OK);
    pc_json_value_fini(&o);
    pc_engine_session_free(empty);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_stamp_visible);
    RUN(test_stamp_down);
    RUN(test_adversarial);
    return pc_t_finish();
}
