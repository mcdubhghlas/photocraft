/* Move tool Auto-Select (port of a subset of pick_cmds tests, E10). The layer-management setup
 * commands (layer.new.layer, edit.fill) are not yet ported, so this builds layers through direct
 * edits. */
#include <stdlib.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "raster/surface.h"
#include "harness.h"

static pc_engine_session *mk(uint32_t w, uint32_t h)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "U", pc_size_make(w, h), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

/* Add an empty raster layer with a red square at (0..10, 0..10); *out gets its id. */
struct addctx {
    pc_layer_id out;
};

static pc_status add_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *v, pc_error *e)
{
    (void)ha;
    struct addctx *c = v;
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
    pc_layer *added = pc_document_layer_mut(doc, id);
    pc_surface *surf = pc_layer_surface_mut(added);
    float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    pc_status rc = pc_surface_fill_rect(surf, pc_rect_from_xywh(0, 0, 10, 10), red, 4, e);
    c->out = id;
    *a = id;
    return rc;
}

static pc_status pick(pc_engine_session *s, const char *json, pc_json_value *out, pc_error *err)
{
    return pc_engine_session_execute_json(s, "layer.pickAt", json, out, err);
}

static int64_t layer_of(pc_json_value *out, bool *is_null)
{
    yyjson_mut_val *root = pc_json_value_root(out);
    yyjson_mut_val *l = yyjson_mut_obj_get(root, "layer");
    *is_null = (l == NULL) || yyjson_mut_is_null(l);
    return *is_null ? 0 : (int64_t)yyjson_mut_get_uint(l);
}

static void test_pick_single_layer(void)
{
    pc_engine_session *s = mk(40, 40);
    pc_layer_id bg = pc_engine_session_active(s)->active_layer;
    pc_json_value out = PC_JSON_VALUE_INIT;
    bool isnull;
    /* In-canvas on the opaque background -> picks it. */
    CHECK(pick(s, "{\"x\":20,\"y\":20,\"select\":false}", &out, NULL) == PC_OK);
    CHECK((pc_layer_id)layer_of(&out, &isnull) == bg && !isnull);
    pc_json_value_fini(&out);
    /* Off-canvas -> null. */
    CHECK(pick(s, "{\"x\":-5,\"y\":-5,\"select\":false}", &out, NULL) == PC_OK);
    (void)layer_of(&out, &isnull);
    CHECK(isnull);
    pc_json_value_fini(&out);
    /* list -> one layer. */
    CHECK(pick(s, "{\"x\":20,\"y\":20,\"list\":true}", &out, NULL) == PC_OK);
    yyjson_mut_val *arr = yyjson_mut_obj_get(pc_json_value_root(&out), "layers");
    CHECK_EQ_UINT(yyjson_mut_arr_size(arr), 1);
    pc_json_value_fini(&out);
    /* bad params. */
    CHECK(pick(s, "{}", &out, NULL) != PC_OK);
    pc_json_value_fini(&out);
    CHECK(pick(s, "{\"x\":\"5\",\"y\":5}", &out, NULL) != PC_OK);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_pick_topmost_and_select(void)
{
    pc_engine_session *s = mk(40, 40);
    pc_layer_id bg = pc_engine_session_active(s)->active_layer;
    struct addctx c = {0};
    CHECK(pc_engine_session_edit(s, "add", add_edit, &c, NULL) == PC_OK);
    pc_layer_id a = c.out;
    pc_json_value out = PC_JSON_VALUE_INIT;
    bool isnull;
    /* Inside the red square -> the top layer; and it becomes active (default select). */
    CHECK(pick(s, "{\"x\":5,\"y\":5}", &out, NULL) == PC_OK);
    CHECK((pc_layer_id)layer_of(&out, &isnull) == a);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_active(s)->active_layer == a);
    /* Outside the square -> background. */
    CHECK(pick(s, "{\"x\":30,\"y\":30,\"select\":false}", &out, NULL) == PC_OK);
    CHECK((pc_layer_id)layer_of(&out, &isnull) == bg);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_pick_single_layer);
    RUN(test_pick_topmost_and_select);
    return pc_t_finish();
}
