/* Delete Layer never leaves a document without layers (#1107); a refused delete changes nothing
 * and adds no history step (port of crates/engine/tests/layer_delete_last.rs). */
#include "engine/engine.h"
#include "harness.h"

static pc_engine_session *one_layer_doc(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.new",
                                         "{\"width\":10,\"height\":5,\"background\":\"transparent\"}", &out,
                                         NULL) == PC_OK);
    pc_json_value_fini(&out);
    return s;
}

static void test_deleting_the_only_layer_is_refused(void)
{
    pc_engine_session *s = one_layer_doc();
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(st->doc);
    CHECK(doc->nlayers == 1 && st->has_active_layer);
    pc_layer_id id = doc->layers[0].id;
    bool undo_before = pc_history_can_undo(st->history);

    char p[48];
    (void)snprintf(p, sizeof p, "{\"layer\":%llu}", (unsigned long long)id);
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    CHECK(pc_engine_session_execute_json(s, "layer.delete", p, &out, &err) != PC_OK);

    st = pc_engine_session_active(s);
    doc = pc_doc_arc_get(st->doc);
    CHECK_EQ_UINT(doc->nlayers, 1);
    CHECK(st->has_active_layer && st->active_layer == id);
    CHECK(pc_history_can_undo(st->history) == undo_before);
    pc_engine_session_free(s);
}

static void test_deleting_without_param_is_refused(void)
{
    pc_engine_session *s = one_layer_doc();
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    CHECK(pc_engine_session_execute_json(s, "layer.delete", "{}", &out, &err) != PC_OK);
    CHECK_EQ_UINT(pc_doc_arc_get(pc_engine_session_active(s)->doc)->nlayers, 1);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_deleting_the_only_layer_is_refused);
    RUN(test_deleting_without_param_is_refused);
    return pc_t_finish();
}
