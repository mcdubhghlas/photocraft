#include "harness.h"
#include "engine/engine.h"
#include <string.h>
static pc_status execute(pc_engine_session *s, const char *id, const char *params, pc_json_value *out)
{
    pc_json_value result = PC_JSON_VALUE_INIT;
    pc_error err;
    pc_status rc = pc_engine_session_execute_json(s, id, params, out ? out : &result, &err);
    if (rc != PC_OK) {
        PC_T_FAIL("%s: %s", id, err.msg);
    }
    pc_json_value_fini(&result);
    return rc;
}
static pc_status rejected(pc_engine_session *s, const char *id, const char *params)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, id, params, &out, NULL);
    pc_json_value_fini(&out);
    return rc;
}
static const pc_document *document(pc_engine_session *s)
{
    return pc_doc_arc_get(pc_engine_session_active(s)->doc);
}
static pc_engine_session *session(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(execute(s, "file.new", "{\"width\":20,\"height\":10}", NULL) == PC_OK);
    CHECK(execute(s, "layer.new.layer", "{}", NULL) == PC_OK);
    CHECK(execute(s, "edit.fill", "{\"color\":\"#ff0000\"}", NULL) == PC_OK);
    CHECK(execute(s, "edit.fill", "{\"color\":\"#00ff00\"}", NULL) == PC_OK);
    return s;
}
static void test_delete_and_copy_states(void)
{
    pc_engine_session *s = session();
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    CHECK(pc_history_entry_count(st->history) == 4);
    CHECK(rejected(s, "history.deleteState", "{\"state\":0}") != PC_OK);
    CHECK(rejected(s, "history.deleteState", "{\"state\":-1}") != PC_OK);
    CHECK(rejected(s, "history.newDocument", "{\"state\":99}") != PC_OK);
    pc_doc_id original = document(s)->id;
    pc_layer_id layer = document(s)->layers[0].id;
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(execute(s, "history.newDocument", "{\"state\":1,\"name\":\"Earlier\"}", &out) == PC_OK);
    CHECK(document(s)->id != original && document(s)->layers[0].id != layer);
    CHECK_EQ_STR(pc_str_c(&document(s)->name), "Earlier");
    CHECK(pc_history_entry_count(pc_engine_session_active(s)->history) == 1);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_set_active(s, 0));
    CHECK(execute(s, "history.deleteState", "{\"state\":2}", NULL) == PC_OK);
    st = pc_engine_session_active(s);
    CHECK(pc_history_entry_count(st->history) == 2 && !pc_history_can_redo(st->history));
    CHECK(execute(s, "history.undo", "{}", NULL) == PC_OK);
    CHECK(pc_history_can_redo(st->history));
    CHECK(execute(s, "history.redo", "{}", NULL) == PC_OK);
    CHECK(pc_history_entry_count(st->history) == 2);
    pc_engine_session_free(s);
}
static void test_snapshot_identity_selection_and_cow(void)
{
    pc_engine_session *s = session();
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    pc_doc_id id = document(s)->id;
    pc_layer_id active = st->active_layer;
    uint64_t revision = st->revision;
    size_t count = pc_history_entry_count(st->history);
    CHECK(execute(s, "history.newSnapshot", "{\"name\":\"  Green  \"}", NULL) == PC_OK);
    CHECK(st->revision == revision && pc_history_entry_count(st->history) == count && st->nsnapshots == 1);
    CHECK(execute(s, "edit.fill", "{\"color\":\"#0000ff\"}", NULL) == PC_OK);
    CHECK(execute(s, "layer.renameLayer", "{\"name\":\"Changed\"}", NULL) == PC_OK);
    CHECK(execute(s, "history.selectSnapshot", "{\"snapshot\":1}", NULL) == PC_OK);
    CHECK(document(s)->id == id && st->active_layer == active);
    float pixel[4];
    pc_surface_read_pixel(pc_layer_surface(&document(s)->layers[1]), 5, 5, pixel);
    CHECK_NEAR(pixel[1], 1, 0);
    CHECK_NEAR(pixel[2], 0, 0);
    CHECK_EQ_STR(pc_str_c(pc_history_entry(st->history, pc_history_entry_count(st->history) - 1)), "Green");
    CHECK(execute(s, "history.undo", "{}", NULL) == PC_OK);
    pc_surface_read_pixel(pc_layer_surface(&document(s)->layers[1]), 5, 5, pixel);
    CHECK_NEAR(pixel[2], 1, 0);
    CHECK(execute(s, "history.renameSnapshot", "{\"snapshot\":1,\"name\":\"Saved\"}", NULL) == PC_OK);
    pc_json_value inspect = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_inspect_document(st, &inspect, NULL) == PC_OK);
    yyjson_mut_val *row =
        yyjson_mut_arr_get(yyjson_mut_obj_get(pc_json_value_root(&inspect), "snapshots"), 0);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(row, "name")), "Saved");
    pc_json_value_fini(&inspect);
    CHECK(execute(s, "history.deleteSnapshot", "{\"snapshot\":1}", NULL) == PC_OK);
    CHECK(st->nsnapshots == 0);
    CHECK(execute(s, "history.newSnapshot", "{}", NULL) == PC_OK);
    CHECK(st->nsnapshots == 1 && st->next_snapshot == 3);
    CHECK(rejected(s, "history.selectSnapshot", "{\"snapshot\":1}") != PC_OK);
    pc_engine_session_free(s);
}
static void test_snapshot_sources_and_bad_input(void)
{
    pc_engine_session *s = session();
    CHECK(execute(s, "history.newSnapshot", "{\"from\":\"currentLayer\"}", NULL) == PC_OK);
    CHECK(execute(s, "history.newSnapshot", "{\"from\":\"mergedLayers\"}", NULL) == PC_OK);
    CHECK(execute(s, "history.selectSnapshot", "{\"snapshot\":1}", NULL) == PC_OK);
    CHECK(document(s)->nlayers == 1);
    CHECK(execute(s, "history.selectSnapshot", "{\"snapshot\":2}", NULL) == PC_OK);
    CHECK(document(s)->nlayers == 1);
    size_t count = pc_history_entry_count(pc_engine_session_active(s)->history);
    uint64_t revision = pc_engine_session_active(s)->revision;
    const char *bad[] = {"{\"name\":false}", "{\"name\":\"  \"}", "{\"from\":\"other\"}", "{\"from\":3}"};
    for (size_t i = 0; i < 4; i++) {
        CHECK(rejected(s, "history.newSnapshot", bad[i]) != PC_OK);
    }
    CHECK(rejected(s, "history.renameSnapshot", "{\"snapshot\":1,\"name\":null}") != PC_OK);
    CHECK(rejected(s, "history.deleteSnapshot", "{\"snapshot\":-1}") != PC_OK);
    CHECK(rejected(s, "history.selectSnapshot", "{\"snapshot\":1e30}") != PC_OK);
    CHECK(pc_engine_session_active(s)->revision == revision);
    CHECK(pc_history_entry_count(pc_engine_session_active(s)->history) == count);
    pc_engine_session_free(s);
}
int main(int argc, char **argv)
{
    RUN(test_delete_and_copy_states);
    RUN(test_snapshot_identity_selection_and_cow);
    RUN(test_snapshot_sources_and_bad_input);
    return pc_t_finish();
}
