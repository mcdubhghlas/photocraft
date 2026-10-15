/* Web slices. Port of crates/engine/src/slice_cmds/tests.rs (E12). */
#include "doc/doc.h"
#include "doc/slices.h"
#include "engine/engine_internal.h"
#include "harness.h"

static pc_status ex(pc_engine_session *s, const char *id, const char *p)
{
    pc_json_value o = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, id, p, &o, NULL);
    pc_json_value_fini(&o);
    return rc;
}

static uint64_t exu(pc_engine_session *s, const char *id, const char *p, const char *key)
{
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, id, p, &o, NULL) == PC_OK);
    uint64_t v = yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&o), key));
    pc_json_value_fini(&o);
    return v;
}

static const pc_document *doc_of(pc_engine_session *s)
{
    return pc_doc_arc_get(pc_engine_session_active(s)->doc);
}

static pc_engine_session *session(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(ex(s, "file.new", "{\"width\":120,\"height\":90,\"name\":\"site\"}") == PC_OK);
    return s;
}

static void test_user_slices_and_options(void)
{
    pc_engine_session *s = session();
    pc_json_value r = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(
              s, "slice.new",
              "{\"rect\":[20,10,40,20],\"name\":\"logo\",\"url\":\"https://example.org\",\"alt\":"
              "\"Logo\"}",
              &r, NULL) == PC_OK);
    uint32_t id = (uint32_t)yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&r), "slice"));
    CHECK_EQ_UINT((uint64_t)yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&r), "number")), 3u);
    pc_json_value_fini(&r);

    pc_json_value l = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "slice.list", "{}", &l, NULL) == PC_OK);
    yyjson_mut_val *all = yyjson_mut_obj_get(pc_json_value_root(&l), "slices");
    CHECK_EQ_UINT((uint64_t)yyjson_mut_arr_size(all), 5u);
    size_t autos = 0;
    for (size_t k = 0; k < yyjson_mut_arr_size(all); k++) {
        yyjson_mut_val *it = yyjson_mut_arr_get(all, k);
        yyjson_mut_val *origin = yyjson_mut_obj_get(it, "origin");
        const char *os = origin != NULL ? yyjson_mut_get_str(origin) : NULL;
        if (os != NULL && strcmp(os, "auto") == 0) {
            autos++;
        }
    }
    CHECK_EQ_UINT(autos, 4u);
    pc_json_value_fini(&l);

    /* Slice Options by number, then undo */
    CHECK(ex(s, "slice.set",
             "{\"number\":3,\"kind\":\"noImage\",\"cellText\":\"hi\",\"background\":\"#ff0000\"}") ==
          PC_OK);
    const pc_slice *sl = pc_slices_get(&doc_of(s)->slices, id);
    CHECK(sl != NULL && sl->kind == PC_SLICE_NO_IMAGE);
    CHECK(sl->has_background && sl->background[1] == 255 && sl->background[2] == 0);
    CHECK(pc_engine_session_undo(s));
    CHECK(pc_slices_get(&doc_of(s)->slices, id)->kind == PC_SLICE_IMAGE);
    char buf[64];
    snprintf(buf, sizeof buf, "{\"slice\":%u,\"kind\":\"bogus\"}", id);
    CHECK(ex(s, "slice.set", buf) != PC_OK);
    CHECK(ex(s, "slice.new", "{\"rect\":[500,500,10,10]}") != PC_OK); /* outside canvas */
    pc_engine_session_free(s);
}

static void test_rect_overflow_rejected(void)
{
    pc_engine_session *s = session();
    CHECK(ex(s, "slice.new", "{\"rect\":[2147483647,0,1,1]}") != PC_OK);
    uint64_t id = exu(s, "slice.new", "{\"rect\":[1,1,8,8]}", "slice");
    char buf[80];
    snprintf(buf, sizeof buf, "{\"slice\":%llu,\"rect\":[0,2147483647,1,1]}", (unsigned long long)id);
    CHECK(ex(s, "slice.set", buf) != PC_OK);
    pc_engine_session_free(s);
}

static void test_promote_and_divide(void)
{
    pc_engine_session *s = session();
    CHECK(ex(s, "slice.new", "{\"rect\":[0,0,60,45]}") == PC_OK);
    pc_json_value r = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "slice.set", "{\"number\":2,\"name\":\"right\"}", &r,
                                         NULL) == PC_OK);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(pc_json_value_root(&r), "origin")), "user");
    pc_json_value_fini(&r);
    CHECK_EQ_UINT(doc_of(s)->slices.len, 2u);
    uint32_t id = doc_of(s)->slices.list[0].id;
    char buf[80];
    snprintf(buf, sizeof buf, "{\"slice\":%u,\"horizontal\":3,\"vertical\":2}", id);
    CHECK_EQ_UINT(exu(s, "slice.divide", buf, "count"), 6u);
    CHECK_EQ_UINT(doc_of(s)->slices.len, 7u);
    uint64_t total = 0;
    for (size_t i = 0; i < 6; i++) {
        total += (uint64_t)pc_rect_width(doc_of(s)->slices.list[i].rect) *
                 pc_rect_height(doc_of(s)->slices.list[i].rect);
    }
    CHECK_EQ_UINT(total, 60u * 45u);
    snprintf(buf, sizeof buf, "{\"slice\":%u}", id);
    CHECK(ex(s, "slice.divide", buf) != PC_OK); /* needs >1 */
    pc_engine_session_free(s);
}

static void test_lock_clear_delete(void)
{
    pc_engine_session *s = session();
    CHECK(ex(s, "slice.new", "{\"rect\":[0,0,10,10]}") == PC_OK);
    uint64_t id = doc_of(s)->slices.list[0].id;
    char buf[48];
    snprintf(buf, sizeof buf, "{\"slice\":%llu}", (unsigned long long)id);
    CHECK_EQ_UINT(exu(s, "slice.delete", buf, "deleted"), 1u);
    CHECK_EQ_UINT(doc_of(s)->slices.len, 0u);
    /* lock then slice.new is refused */
    CHECK(ex(s, "slice.new", "{\"rect\":[0,0,10,10]}") == PC_OK);
    CHECK(ex(s, "view.lockSlices", "{\"on\":true}") == PC_OK);
    CHECK(s->slices_locked);
    CHECK(ex(s, "slice.new", "{\"rect\":[1,1,5,5]}") != PC_OK);
    CHECK(ex(s, "view.lockSlices", "{\"on\":false}") == PC_OK);
    CHECK_EQ_UINT(exu(s, "view.clearSlices", "{}", "cleared"), 1u);
    CHECK_EQ_UINT(doc_of(s)->slices.len, 0u);
    pc_engine_session_free(s);
}

static void test_layer_based_slice(void)
{
    pc_engine_session *s = session();
    CHECK(ex(s, "layer.new.layer", "{}") == PC_OK);
    /* give the layer content via a rectangular selection + fill */
    CHECK(ex(s, "select.rect", "{\"x\":10,\"y\":12,\"width\":30,\"height\":20}") == PC_OK);
    CHECK(ex(s, "edit.fill", "{\"color\":\"#ff0000\"}") == PC_OK);
    CHECK(ex(s, "select.deselect", "{}") == PC_OK || true);
    pc_json_value r = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "layer.newLayerBasedSlice", "{\"name\":\"L\"}", &r, NULL) ==
          PC_OK);
    uint32_t id = (uint32_t)yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&r), "slice"));
    pc_json_value_fini(&r);
    const pc_slice *sl = pc_slices_get(&doc_of(s)->slices, id);
    CHECK(sl != NULL && sl->origin == PC_SLICE_LAYER && sl->has_layer);
    /* a second layer-based slice on the same layer is refused */
    CHECK(ex(s, "layer.newLayerBasedSlice", "{}") != PC_OK);
    pc_engine_session_free(s);
}

static void test_bad_and_adversarial(void)
{
    pc_engine_session *s = session();
    CHECK(ex(s, "slice.new", "{}") != PC_OK);
    CHECK(ex(s, "slice.set", "{}") != PC_OK);
    CHECK(ex(s, "slice.delete", "{\"slice\":999}") != PC_OK);
    (void)ex(s, "slice.new", "{\"rect\":[0,0,20,20]}");
    const char *ids[] = {"slice.new",    "slice.fromGuides", "slice.set",    "slice.promote",
                         "slice.delete", "slice.divide",     "slice.list",   "view.lockSlices",
                         "view.clearSlices", "layer.newLayerBasedSlice"};
    const char *ps[] = {"{}", "[]", "null", "123", "{\"slice\":-1}", "{\"slice\":1.5}",
                        "{\"rect\":[1,2,3]}", "{\"number\":0}", "{\"slices\":[-1]}",
                        "{\"outsets\":[1,2,3,4,5]}", "{\"horizontal\":99999,\"vertical\":99999}"};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        for (size_t j = 0; j < sizeof ps / sizeof ps[0]; j++) {
            (void)ex(s, ids[i], ps[j]); /* must not crash */
        }
    }
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_user_slices_and_options);
    RUN(test_rect_overflow_rejected);
    RUN(test_promote_and_divide);
    RUN(test_lock_clear_delete);
    RUN(test_layer_based_slice);
    RUN(test_bad_and_adversarial);
    return pc_t_finish();
}
