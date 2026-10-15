/* Window > Actions. Port of crates/engine/src/actions_cmds.rs tests (E12). The transform-again
 * tests need edit.transform (E4, not yet registered); the script-recording test needs
 * file.scripts.browse (automate); both are covered by their owning chunks. */
#include <string.h>

#include "doc/doc.h"
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

static size_t nlayers(pc_engine_session *s)
{
    return pc_doc_arc_get(pc_engine_session_active(s)->doc)->nlayers;
}

/* record N layer.new.layer steps interleaved with queries; stop returns step count. */
static void test_record_play_and_queries_skipped(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(ex(s, "file.new", "{\"width\":8,\"height\":8}") == PC_OK);
    size_t base = nlayers(s);
    CHECK(exu(s, "actions.record", "{\"name\":\"Red\"}", "index") == 0u);
    CHECK(ex(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(ex(s, "actions.list", "{}") == PC_OK);  /* query: not recorded */
    CHECK(ex(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(ex(s, "actions.get", "{\"action\":0}") == PC_OK); /* query */
    CHECK(exu(s, "actions.stop", "{}", "steps") == 2u);

    /* play on a fresh document adds two layers (one history/step each) */
    pc_engine_session *s2 = NULL;
    CHECK(pc_engine_session_new(&s2, NULL) == PC_OK);
    CHECK(ex(s2, "file.new", "{\"width\":8,\"height\":8}") == PC_OK);
    /* move the action list across by re-recording the same steps in s2 */
    CHECK(ex(s2, "actions.record", "{\"name\":\"Red\"}") == PC_OK);
    CHECK(ex(s2, "layer.new.layer", "{}") == PC_OK);
    CHECK(ex(s2, "layer.new.layer", "{}") == PC_OK);
    CHECK(ex(s2, "actions.stop", "{}") == PC_OK);
    size_t b2 = nlayers(s2);
    pc_json_value r = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s2, "actions.play", "{\"action\":\"Red\"}", &r, NULL) == PC_OK);
    CHECK_EQ_UINT((uint64_t)yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&r), "ran")), 2u);
    CHECK(yyjson_mut_obj_get(pc_json_value_root(&r), "failed") == NULL);
    pc_json_value_fini(&r);
    CHECK_EQ_UINT(nlayers(s2), b2 + 2);

    (void)base;
    pc_engine_session_free(s);
    pc_engine_session_free(s2);
}

/* toggle skips steps; move/delete remap the disabled set. */
static void test_toggle_move_delete(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(ex(s, "file.new", "{\"width\":8,\"height\":8}") == PC_OK);
    CHECK(ex(s, "actions.record", "{\"name\":\"Three\"}") == PC_OK);
    CHECK(ex(s, "layer.new.layer", "{\"name\":\"A\"}") == PC_OK);
    CHECK(ex(s, "layer.new.layer", "{\"name\":\"B\"}") == PC_OK);
    CHECK(ex(s, "layer.new.layer", "{\"name\":\"C\"}") == PC_OK);
    CHECK(ex(s, "actions.stop", "{}") == PC_OK);

    /* disable step 1 */
    pc_json_value r = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "actions.toggle", "{\"action\":\"Three\",\"step\":1}", &r,
                                         NULL) == PC_OK);
    yyjson_mut_val *dis = yyjson_mut_obj_get(pc_json_value_root(&r), "disabled");
    CHECK(yyjson_mut_arr_size(dis) == 1 && yyjson_mut_get_uint(yyjson_mut_arr_get(dis, 0)) == 1);
    pc_json_value_fini(&r);
    CHECK_EQ_UINT(exu(s, "actions.play", "{\"action\":0}", "ran"), 2u); /* skip the disabled step */

    /* move step 2 to 0: [C,A,B], disabled follows B -> index 2 */
    CHECK(ex(s, "actions.move", "{\"action\":0,\"step\":2,\"to\":0}") == PC_OK);
    CHECK(pc_engine_session_execute_json(s, "actions.get", "{\"action\":0}", &r, NULL) == PC_OK);
    dis = yyjson_mut_obj_get(pc_json_value_root(&r), "disabled");
    CHECK(yyjson_mut_arr_size(dis) == 1 && yyjson_mut_get_uint(yyjson_mut_arr_get(dis, 0)) == 2);
    pc_json_value_fini(&r);

    /* delete step 0 -> [A,B], disabled shifts to 1 */
    CHECK(ex(s, "actions.delete", "{\"action\":0,\"step\":0}") == PC_OK);
    CHECK(pc_engine_session_execute_json(s, "actions.get", "{\"action\":0}", &r, NULL) == PC_OK);
    CHECK(yyjson_mut_arr_size(yyjson_mut_obj_get(pc_json_value_root(&r), "steps")) == 2);
    dis = yyjson_mut_obj_get(pc_json_value_root(&r), "disabled");
    CHECK(yyjson_mut_arr_size(dis) == 1 && yyjson_mut_get_uint(yyjson_mut_arr_get(dis, 0)) == 1);
    pc_json_value_fini(&r);

    /* the action's own check: all on unless all already are */
    CHECK(ex(s, "actions.toggle", "{\"action\":0}") == PC_OK);
    CHECK(pc_engine_session_execute_json(s, "actions.get", "{\"action\":0}", &r, NULL) == PC_OK);
    CHECK(yyjson_mut_arr_size(yyjson_mut_obj_get(pc_json_value_root(&r), "disabled")) == 0);
    pc_json_value_fini(&r);
    CHECK(ex(s, "actions.toggle", "{\"action\":0}") == PC_OK);
    CHECK(pc_engine_session_execute_json(s, "actions.get", "{\"action\":0}", &r, NULL) == PC_OK);
    CHECK(yyjson_mut_arr_size(yyjson_mut_obj_get(pc_json_value_root(&r), "disabled")) == 2);
    pc_json_value_fini(&r);

    const char *bad[] = {"{\"action\":0,\"step\":2}", "{\"action\":0,\"step\":-1}",
                         "{\"action\":0,\"step\":\"x\"}", "{\"action\":0,\"step\":0,\"enabled\":\"yes\"}",
                         "{\"action\":7,\"step\":0}", "{}"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(ex(s, "actions.toggle", bad[i]) != PC_OK);
    }
    pc_engine_session_free(s);
}

static void test_rename(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(ex(s, "file.new", "{\"width\":8,\"height\":8}") == PC_OK);
    CHECK(ex(s, "actions.record", "{\"name\":\"Action 1\"}") == PC_OK);
    CHECK(ex(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(ex(s, "actions.stop", "{}") == PC_OK);
    /* Caller action referencing "Action 1" */
    CHECK(ex(s, "actions.record", "{\"name\":\"Caller\"}") == PC_OK);
    CHECK(ex(s, "actions.play", "{\"action\":\"Action 1\"}") == PC_OK);
    CHECK(ex(s, "actions.stop", "{}") == PC_OK);

    pc_json_value r = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "actions.rename", "{\"action\":0,\"name\":\"Sepia\"}", &r,
                                         NULL) == PC_OK);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(pc_json_value_root(&r), "action")), "Sepia");
    pc_json_value_fini(&r);
    /* the Caller's nested play step follows the rename */
    CHECK(pc_engine_session_execute_json(s, "actions.get", "{\"action\":\"Caller\"}", &r, NULL) ==
          PC_OK);
    yyjson_mut_val *steps = yyjson_mut_obj_get(pc_json_value_root(&r), "steps");
    yyjson_mut_val *step0 = yyjson_mut_arr_get(steps, 0);
    yyjson_mut_val *params = yyjson_mut_arr_get(step0, 1);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(params, "action")), "Sepia");
    pc_json_value_fini(&r);

    const char *bad[] = {"{\"action\":0,\"name\":\"\"}", "{\"action\":0,\"name\":\"  \"}",
                         "{\"action\":0,\"name\":\"Caller\"}", "{\"action\":0,\"name\":7}",
                         "{\"action\":9,\"name\":\"X\"}", "{\"action\":0}"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(ex(s, "actions.rename", bad[i]) != PC_OK);
    }
    pc_engine_session_free(s);
}

static void test_adversarial(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(ex(s, "file.new", "{\"width\":8,\"height\":8}") == PC_OK);
    (void)ex(s, "actions.record", "{\"name\":\"X\"}");
    (void)ex(s, "layer.new.layer", "{}");
    (void)ex(s, "actions.stop", "{}");
    const char *ids[] = {"actions.move",  "actions.list",   "actions.get",    "actions.record",
                         "actions.stop",  "actions.play",   "actions.delete", "actions.toggle",
                         "actions.rename"};
    const char *ps[] = {"{}",          "[]",          "null",         "123",
                        "{\"action\":-1}", "{\"action\":1.5}", "{\"action\":[]}", "{\"action\":0,\"to\":99}",
                        "{\"action\":0,\"from\":-1}", "{\"action\":\"\"}"};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        for (size_t j = 0; j < sizeof ps / sizeof ps[0]; j++) {
            (void)ex(s, ids[i], ps[j]); /* must not crash */
        }
    }
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_record_play_and_queries_skipped);
    RUN(test_toggle_move_delete);
    RUN(test_rename);
    RUN(test_adversarial);
    return pc_t_finish();
}
