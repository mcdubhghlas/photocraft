#include "harness.h"
#include "engine/engine.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Root, bottom to top: Background, A, Group(C,D), B. */
static pc_engine_session *fixture(pc_layer_id ids[6])
{
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "move", pc_size_make(16, 16), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgb(1, 1, 1), NULL) == PC_OK);
    ids[0] = doc.layers[0].id;
    pc_layer a, group, b;
    CHECK(pc_layer_raster(&a, "A", PC_PIXEL_FORMAT_RGBA8, NULL) == PC_OK);
    CHECK(pc_layer_raster(&b, "B", PC_PIXEL_FORMAT_RGBA8, NULL) == PC_OK);
    pc_layer *children = calloc(2, sizeof *children);
    CHECK(children != NULL);
    CHECK(pc_layer_raster(&children[0], "C", PC_PIXEL_FORMAT_RGBA8, NULL) == PC_OK);
    CHECK(pc_layer_raster(&children[1], "D", PC_PIXEL_FORMAT_RGBA8, NULL) == PC_OK);
    ids[3] = children[0].id;
    ids[4] = children[1].id;
    CHECK(pc_layer_group(&group, "Group", children, 2, NULL) == PC_OK);
    CHECK(pc_document_insert_above(&doc, NULL, &a, &ids[1], NULL) == PC_OK);
    CHECK(pc_document_insert_above(&doc, NULL, &group, &ids[2], NULL) == PC_OK);
    CHECK(pc_document_insert_above(&doc, NULL, &b, &ids[5], NULL) == PC_OK);
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}
static pc_status execute(pc_engine_session *s, const char *params, pc_json_value *out)
{
    pc_error error;
    pc_status status = pc_engine_session_execute_json(s, "layer.moveTo", params, out, &error);
    if (status != PC_OK)
        PC_T_FAIL("moveTo: %s", error.msg);
    return status;
}
static void test_batch_document_order_and_single_undo(void)
{
    pc_layer_id ids[6];
    pc_engine_session *s = fixture(ids);
    char p[256];
    (void)snprintf(p, sizeof p, "{\"layers\":[%llu,%llu],\"target\":%llu,\"position\":\"into\"}",
                   (unsigned long long)ids[5], (unsigned long long)ids[1], (unsigned long long)ids[2]);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(execute(s, p, &out) == PC_OK);
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "moved")) == 2);
    const pc_engine_doc_state *state = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(state->doc);
    CHECK(doc->nlayers == 2 && doc->layers[1].id == ids[2]);
    const pc_group *group = &doc->layers[1].content.u.group;
    CHECK(group->nchildren == 4 && group->children[2].id == ids[1] && group->children[3].id == ids[5]);
    CHECK(state->active_layer == ids[5]);
    CHECK(pc_engine_session_journal_len(s) == 1);
    CHECK(pc_engine_session_undo(s));
    doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    CHECK(doc->nlayers == 4 && doc->layers[1].id == ids[1] && doc->layers[3].id == ids[5]);
    CHECK(!pc_engine_session_undo(s));
    CHECK(pc_engine_session_redo(s));
    CHECK(pc_engine_session_active(s)->active_layer == ids[5]);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}
static void test_selected_descendants_stay_in_their_group(void)
{
    pc_layer_id ids[6];
    pc_engine_session *s = fixture(ids);
    char p[256];
    (void)snprintf(p, sizeof p, "{\"layers\":[%llu,%llu],\"target\":%llu,\"position\":\"above\"}",
                   (unsigned long long)ids[3], (unsigned long long)ids[2], (unsigned long long)ids[5]);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(execute(s, p, &out) == PC_OK);
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "moved")) == 1);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    CHECK(doc->nlayers == 4 && doc->layers[3].id == ids[2]);
    CHECK(doc->layers[3].content.u.group.children[0].id == ids[3]);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}
static void test_self_copy_background_and_fresh_subtree_ids(void)
{
    pc_layer_id ids[6];
    pc_engine_session *s = fixture(ids);
    char p[256];
    (void)snprintf(p, sizeof p, "{\"layer\":%llu,\"target\":%llu,\"copy\":true}", (unsigned long long)ids[2],
                   (unsigned long long)ids[2]);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(execute(s, p, &out) == PC_OK);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    CHECK(doc->nlayers == 5);
    const pc_layer *copy = &doc->layers[3];
    CHECK(copy->id != ids[2] && copy->content.u.group.nchildren == 2);
    CHECK(copy->content.u.group.children[0].id != ids[3]);
    CHECK_EQ_STR(pc_str_c(&copy->name), "Group copy");
    pc_json_value_fini(&out);
    (void)snprintf(p, sizeof p, "{\"layer\":%llu,\"target\":%llu,\"copy\":true}", (unsigned long long)ids[0],
                   (unsigned long long)ids[0]);
    CHECK(execute(s, p, &out) == PC_OK);
    doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    CHECK(doc->layers[0].id == ids[0]);
    CHECK(doc->layers[1].id != ids[0] && !doc->layers[1].locks.all);
    CHECK(!pc_document_is_background(doc, doc->layers[1].id));
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}
static void test_invalid_moves_are_atomic(void)
{
    pc_layer_id ids[6];
    pc_engine_session *s = fixture(ids);
    const char *bad[] = {"{}",
                         "{\"target\":null}",
                         "{\"target\":1,\"layers\":[]}",
                         "{\"target\":1,\"layers\":[1,1]}",
                         "{\"target\":1,\"layers\":[-1]}",
                         "{\"target\":1,\"layers\":{},\"layer\":2}",
                         "{\"target\":1,\"position\":\"bad\"}",
                         "{\"target\":1,\"copy\":\"yes\"}"};
    pc_json_value out = PC_JSON_VALUE_INIT;
    const pc_engine_doc_state *state = pc_engine_session_active(s);
    const pc_doc_arc *before = state->doc;
    uint64_t revision = state->revision;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(pc_engine_session_execute_json(s, "layer.moveTo", bad[i], &out, NULL) != PC_OK);
        CHECK(pc_engine_session_active(s)->doc == before &&
              pc_engine_session_active(s)->revision == revision);
        pc_json_value_fini(&out);
    }
    const pc_layer_id sources[] = {ids[0], ids[1], ids[2], ids[1]};
    const pc_layer_id targets[] = {ids[1], ids[0], ids[3], ids[5]};
    const char *positions[] = {"above", "below", "into", "into"};
    for (size_t i = 0; i < 4; i++) {
        char p[256];
        (void)snprintf(p, sizeof p, "{\"layer\":%llu,\"target\":%llu,\"position\":\"%s\"}",
                       (unsigned long long)sources[i], (unsigned long long)targets[i], positions[i]);
        CHECK(pc_engine_session_execute_json(s, "layer.moveTo", p, &out, NULL) != PC_OK);
        CHECK(pc_engine_session_active(s)->doc == before &&
              pc_engine_session_active(s)->revision == revision);
        pc_json_value_fini(&out);
    }
    CHECK(pc_engine_session_journal_len(s) == 0 && !pc_engine_session_undo(s));
    pc_engine_session_free(s);
}
int main(int argc, char **argv)
{
    RUN(test_batch_document_order_and_single_undo);
    RUN(test_selected_descendants_stay_in_their_group);
    RUN(test_self_copy_background_and_fresh_subtree_ids);
    RUN(test_invalid_moves_are_atomic);
    return pc_t_finish();
}
