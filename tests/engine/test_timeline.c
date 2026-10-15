/* Window > Timeline. Port of crates/engine/src/timeline_cmds.rs tests (E12). */
#include "doc/doc.h"
#include "doc/video.h"
#include "engine/engine_internal.h"
#include "harness.h"

#define MAX_DURATION ((uint64_t)1000000)
#define MAX_FPS 1000.0

static pc_status ex(pc_engine_session *s, const char *id, const char *p)
{
    pc_json_value o = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, id, p, &o, NULL);
    pc_json_value_fini(&o);
    return rc;
}

static pc_engine_session *session(uint32_t w, uint32_t h)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char buf[64];
    snprintf(buf, sizeof buf, "{\"width\":%u,\"height\":%u}", w, h);
    CHECK(ex(s, "file.new", buf) == PC_OK);
    return s;
}

/* timeline.info -> current frame, -1 if null timeline. */
static int64_t current(pc_engine_session *s)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "timeline.info", "{}", &out, NULL) == PC_OK);
    yyjson_mut_val *tl = yyjson_mut_obj_get(pc_json_value_root(&out), "timeline");
    int64_t r = -1;
    if (tl != NULL && yyjson_mut_is_obj(tl)) {
        r = (int64_t)yyjson_mut_get_uint(yyjson_mut_obj_get(tl, "current"));
    }
    pc_json_value_fini(&out);
    return r;
}

static bool has_tl(pc_engine_session *s)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "timeline.info", "{}", &out, NULL) == PC_OK);
    yyjson_mut_val *tl = yyjson_mut_obj_get(pc_json_value_root(&out), "timeline");
    bool r = tl != NULL && yyjson_mut_is_obj(tl);
    pc_json_value_fini(&out);
    return r;
}

static void test_create_navigate_delete(void)
{
    pc_engine_session *s = session(16, 16);
    CHECK(!has_tl(s));
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "timeline.create", "{\"duration\":24,\"fps\":24.0}", &out,
                                         NULL) == PC_OK);
    yyjson_mut_val *tl = yyjson_mut_obj_get(pc_json_value_root(&out), "timeline");
    CHECK_EQ_UINT((uint64_t)yyjson_mut_get_uint(yyjson_mut_obj_get(tl, "duration")), 24u);
    CHECK_EQ_UINT((uint64_t)yyjson_mut_get_uint(yyjson_mut_obj_get(tl, "current")), 0u);
    pc_json_value_fini(&out);

    CHECK(ex(s, "timeline.setFrame", "{\"frame\":10}") == PC_OK);
    CHECK(ex(s, "timeline.nextFrame", "{}") == PC_OK);
    CHECK_EQ_INT(current(s), 11);
    CHECK(ex(s, "timeline.previousFrame", "{}") == PC_OK);
    CHECK_EQ_INT(current(s), 10);
    /* Clamp beyond the end. */
    CHECK(ex(s, "timeline.setFrame", "{\"frame\":999}") == PC_OK);
    CHECK_EQ_INT(current(s), 23);
    /* Props. */
    CHECK(ex(s, "timeline.setProps", "{\"fps\":30.0}") == PC_OK);
    CHECK(ex(s, "timeline.delete", "{}") == PC_OK);
    CHECK(!has_tl(s));
    pc_engine_session_free(s);
}

static void test_create_rejects_out_of_range(void)
{
    pc_engine_session *s = session(8, 8);
    CHECK(ex(s, "timeline.create", "{\"duration\":24,\"fps\":24.0}") == PC_OK);
    CHECK(ex(s, "timeline.setFrame", "{\"frame\":5}") == PC_OK);
    const char *bad[] = {"{\"duration\":0}",       "{\"duration\":18446744073709551615}",
                         "{\"duration\":1000001}", "{\"duration\":-1}",
                         "{\"duration\":2.5}",     "{\"duration\":\"24\"}",
                         "{\"fps\":0.0}",          "{\"fps\":-24.0}",
                         "{\"fps\":1001.0}",       "{\"fps\":1e300}",
                         "{\"fps\":\"fast\"}"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(ex(s, "timeline.create", bad[i]) != PC_OK);
        CHECK_EQ_INT(current(s), 5); /* nothing changed */
    }
    /* Stepping still works. */
    CHECK(ex(s, "timeline.nextFrame", "{}") == PC_OK);
    CHECK_EQ_INT(current(s), 6);
    CHECK(ex(s, "timeline.previousFrame", "{}") == PC_OK);
    CHECK_EQ_INT(current(s), 5);
    /* Largest accepted values. */
    CHECK(ex(s, "timeline.create", "{\"duration\":1000000,\"fps\":1000.0}") == PC_OK);
    CHECK(ex(s, "timeline.previousFrame", "{}") == PC_OK);
    CHECK_EQ_INT(current(s), 0);
    CHECK(ex(s, "timeline.setFrame", "{\"frame\":1000000}") == PC_OK);
    CHECK(ex(s, "timeline.nextFrame", "{}") == PC_OK);
    CHECK_EQ_INT(current(s), 999999);
    /* null -> defaults. */
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "timeline.create", "{\"duration\":null,\"fps\":null}", &out,
                                         NULL) == PC_OK);
    yyjson_mut_val *tl = yyjson_mut_obj_get(pc_json_value_root(&out), "timeline");
    CHECK_EQ_UINT((uint64_t)yyjson_mut_get_uint(yyjson_mut_obj_get(tl, "duration")), 30u);
    CHECK_NEAR(yyjson_mut_get_real(yyjson_mut_obj_get(tl, "fps")), 30.0, 1e-6);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_set_props_rejects(void)
{
    pc_engine_session *s = session(8, 8);
    CHECK(ex(s, "timeline.create", "{\"duration\":24,\"fps\":24.0}") == PC_OK);
    const char *bad[] = {"{\"duration\":0}", "{\"duration\":18446744073709551615}", "{\"fps\":0.0}",
                         "{\"fps\":-1.0}", "{\"fps\":30.0,\"duration\":0}"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(ex(s, "timeline.setProps", bad[i]) != PC_OK);
    }
    CHECK(ex(s, "timeline.nextFrame", "{}") == PC_OK);
    CHECK_EQ_INT(current(s), 1);
    pc_engine_session_free(s);
}

static void test_navigation_without_timeline_errors(void)
{
    pc_engine_session *s = session(8, 8);
    CHECK(ex(s, "timeline.setFrame", "{\"frame\":1}") != PC_OK);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_create_navigate_delete);
    RUN(test_create_rejects_out_of_range);
    RUN(test_set_props_rejects);
    RUN(test_navigation_without_timeline_errors);
    return pc_t_finish();
}
