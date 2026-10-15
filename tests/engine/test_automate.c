/* File > Scripts (Browse, Script Events Manager). Port of the scripting tests of
 * crates/engine/src/automate_cmds.rs (E12). The Automate pipelines (Contact Sheet / Droplet /
 * Statistics) are deferred and not registered here. */
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

static size_t nlayers(pc_engine_session *s)
{
    return pc_doc_arc_get(pc_engine_session_active(s)->doc)->nlayers;
}

static void test_browse_json_and_lines(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(ex(s, "file.new", "{\"width\":8,\"height\":8}") == PC_OK);
    size_t base = nlayers(s);
    /* JSON steps */
    pc_json_value r = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(
              s, "file.scripts.browse",
              "{\"steps\":[[\"layer.new.layer\",{}],[\"layer.new.layer\",{}]]}", &r, NULL) == PC_OK);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&r), "ok")));
    CHECK_EQ_UINT((uint64_t)yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&r), "steps")), 2u);
    pc_json_value_fini(&r);
    CHECK_EQ_UINT(nlayers(s), base + 2);

    /* line-based script (one command.id per line, # comment) */
    CHECK(pc_engine_session_execute_json(
              s, "file.scripts.browse",
              "{\"script\":\"# make one\\nlayer.new.layer {}\\n\"}", &r, NULL) == PC_OK);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&r), "ok")));
    pc_json_value_fini(&r);
    CHECK_EQ_UINT(nlayers(s), base + 3);

    /* unknown command rejected before running */
    CHECK(ex(s, "file.scripts.browse", "{\"steps\":[[\"no.such.command\",{}]]}") != PC_OK);
    /* a step that errors stops the script with ok=false */
    CHECK(pc_engine_session_execute_json(
              s, "file.scripts.browse", "{\"steps\":[[\"image.trap\",{}]]}", &r, NULL) == PC_OK);
    CHECK(!yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&r), "ok")));
    pc_json_value_fini(&r);
    CHECK(ex(s, "file.scripts.browse", "{}") != PC_OK);
    pc_engine_session_free(s);
}

static void test_script_events_manager(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value r = PC_JSON_VALUE_INIT;
    /* initial read: disabled, no bindings, 8 events */
    CHECK(pc_engine_session_execute_json(s, "file.scripts.scriptEventsManager", "{}", &r, NULL) ==
          PC_OK);
    CHECK(!yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&r), "enabled")));
    CHECK_EQ_UINT((uint64_t)yyjson_mut_arr_size(yyjson_mut_obj_get(pc_json_value_root(&r), "events")),
                  8u);
    pc_json_value_fini(&r);
    /* add a binding (inline steps) for newDocument */
    CHECK(pc_engine_session_execute_json(
              s, "file.scripts.scriptEventsManager",
              "{\"add\":{\"event\":\"newDocument\",\"steps\":[[\"layer.new.layer\",{}]],\"name\":\"N\"}}",
              &r, NULL) == PC_OK);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&r), "enabled")));
    CHECK_EQ_UINT(
        (uint64_t)yyjson_mut_arr_size(yyjson_mut_obj_get(pc_json_value_root(&r), "bindings")), 1u);
    pc_json_value_fini(&r);
    /* unknown event rejected */
    CHECK(ex(s, "file.scripts.scriptEventsManager", "{\"add\":{\"event\":\"nope\",\"steps\":[]}}") !=
          PC_OK);
    /* remove the binding */
    CHECK(ex(s, "file.scripts.scriptEventsManager", "{\"removeAll\":true}") == PC_OK);
    pc_engine_session_free(s);
}

static void test_adversarial(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(ex(s, "file.new", "{\"width\":8,\"height\":8}") == PC_OK);
    const char *ids[] = {"file.scripts.browse", "file.scripts.scriptEventsManager"};
    const char *ps[] = {"{}", "[]", "null", "123", "{\"steps\":5}", "{\"steps\":[42]}",
                        "{\"script\":99}", "{\"add\":5}", "{\"remove\":-1}", "{\"path\":\"/no/x\"}"};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        for (size_t j = 0; j < sizeof ps / sizeof ps[0]; j++) {
            (void)ex(s, ids[i], ps[j]);
        }
    }
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_browse_json_and_lines);
    RUN(test_script_events_manager);
    RUN(test_adversarial);
    return pc_t_finish();
}
