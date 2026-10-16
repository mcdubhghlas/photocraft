/* Headless control dispatch: round-trips, batch, and graceful failure on bad input (port of
 * rpc.rs tests plus adversarial cases). */
#include "harness.h"

#include <string.h>

#include "automation/headless.h"
#include "automation/security.h"
#include "base/json.h"

/* Run one method with JSON params text (or NULL). Returns the status; *out owned on PC_OK. */
static pc_status call(pc_headless *h, const char *method, const char *pjson, pc_json_value *out,
                      pc_error *err)
{
    yyjson_doc *pd = NULL;
    if (pjson) {
        if (pc_json_parse(pjson, strlen(pjson), &pd, err) != PC_OK) {
            return PC_ERR_INVALID;
        }
    }
    pc_status rc = pc_headless_handle(h, method, pd ? yyjson_doc_get_root(pd) : NULL, out, err);
    yyjson_doc_free(pd);
    return rc;
}

static void test_round_trip(void)
{
    pc_headless *h;
    CHECK(pc_headless_trusted_local(&h, NULL) == PC_OK);
    pc_json_value r = PC_JSON_VALUE_INIT;
    CHECK(call(h, "doc.new", "{\"width\":16,\"height\":12,\"background\":\"white\"}", &r, NULL) == PC_OK);
    pc_json_value_fini(&r);
    CHECK(call(h, "engine.execute", "{\"command\":\"layer.new.layer\",\"params\":{\"name\":\"Ink\"}}", &r, NULL) == PC_OK);
    pc_json_value_fini(&r);
    CHECK(call(h, "doc.inspect", NULL, &r, NULL) == PC_OK);
    pc_json_value_fini(&r);
    CHECK(call(h, "methods", NULL, &r, NULL) == PC_OK);
    pc_json_value_fini(&r);
    pc_error e;
    pc_error_clear(&e);
    CHECK(call(h, "nope", NULL, &r, &e) != PC_OK);
    CHECK_CONTAINS(e.msg, "unknown method");
    pc_headless_free(h);
}

static void test_batch(void)
{
    pc_headless *h;
    CHECK(pc_headless_new(&h, NULL) == PC_OK);
    pc_json_value r = PC_JSON_VALUE_INIT;
    CHECK(call(h, "doc.new", "{\"width\":16,\"height\":16}", &r, NULL) == PC_OK);
    pc_json_value_fini(&r);
    const char *steps =
        "{\"steps\":[{\"command\":\"layer.new.layer\",\"params\":{\"name\":\"A\"}},"
        "{\"command\":\"no.such.command\"},"
        "{\"command\":\"layer.new.layer\",\"params\":{\"name\":\"B\"}},"
        "{\"method\":\"doc.inspect\"}]}";
    CHECK(call(h, "batch", steps, &r, NULL) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&r);
    uint64_t completed = 0, failed = 0;
    completed = yyjson_mut_get_uint(yyjson_mut_obj_get(root, "completed"));
    failed = yyjson_mut_get_uint(yyjson_mut_obj_get(root, "failed"));
    CHECK_EQ_UINT(completed, 1u);
    CHECK_EQ_UINT(failed, 1u);
    CHECK_EQ_UINT(yyjson_mut_arr_size(yyjson_mut_obj_get(root, "results")), 2u);
    pc_json_value_fini(&r);
    /* stopOnError false runs them all (completed 3). */
    char with_cont[512];
    snprintf(with_cont, sizeof with_cont, "%.*s, \"stopOnError\": false}", (int)(strlen(steps) - 1), steps);
    CHECK(call(h, "batch", with_cont, &r, NULL) == PC_OK);
    completed = yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&r), "completed"));
    CHECK_EQ_UINT(completed, 3u);
    pc_json_value_fini(&r);
    pc_headless_free(h);
}

static void test_too_many_steps(void)
{
    pc_headless *h;
    CHECK(pc_headless_new(&h, NULL) == PC_OK);
    pc_buf b = PC_BUF_INIT;
    (void)pc_buf_append_str(&b, "{\"steps\":[", NULL);
    for (int i = 0; i < PC_AUTO_MAX_BATCH_STEPS + 1; i++) {
        (void)pc_buf_append_str(&b, i ? ",{\"command\":\"command.list\"}" : "{\"command\":\"command.list\"}", NULL);
    }
    (void)pc_buf_append_str(&b, "]}", NULL);
    pc_json_value r = PC_JSON_VALUE_INIT;
    pc_error e;
    pc_error_clear(&e);
    CHECK(call(h, "batch", pc_buf_cstr(&b), &r, &e) != PC_OK);
    CHECK_CONTAINS(e.msg, "maximum is 256");
    pc_buf_fini(&b);
    pc_headless_free(h);
}

static void test_bad_index(void)
{
    pc_headless *h;
    CHECK(pc_headless_trusted_local(&h, NULL) == PC_OK);
    pc_json_value r = PC_JSON_VALUE_INIT;
    (void)call(h, "doc.new", "{\"width\":8,\"height\":8}", &r, NULL);
    pc_json_value_fini(&r);
    const char *methods[] = {"doc.close", "doc.inspect", "doc.select"};
    const char *bad[] = {"{\"index\":\"0\"}", "{\"index\":-1}", "{\"index\":0.5}", "{\"index\":true}"};
    for (size_t mi = 0; mi < 3; mi++) {
        for (size_t bi = 0; bi < 4; bi++) {
            pc_error e;
            pc_error_clear(&e);
            CHECK(call(h, methods[mi], bad[bi], &r, &e) != PC_OK);
            CHECK_CONTAINS(e.msg, "`index` must be a non-negative integer");
        }
    }
    /* The document is still there. */
    CHECK(call(h, "session.list", NULL, &r, NULL) == PC_OK);
    CHECK_EQ_UINT(yyjson_mut_arr_size(yyjson_mut_obj_get(pc_json_value_root(&r), "documents")), 1u);
    pc_json_value_fini(&r);
    pc_headless_free(h);
}

static void test_bad_filter_params_do_not_crash(void)
{
    pc_headless *h;
    CHECK(pc_headless_trusted_local(&h, NULL) == PC_OK);
    pc_json_value r = PC_JSON_VALUE_INIT;
    (void)call(h, "doc.new", "{\"width\":8,\"height\":8}", &r, NULL);
    pc_json_value_fini(&r);
    const char *params[] = {"{\"radius\":\"big\"}", "{\"radus\":3}", "{\"radius\":5000}", "{\"radius\":-4}"};
    for (size_t i = 0; i < 4; i++) {
        for (int wait = 0; wait < 2; wait++) {
            char cmd[160];
            snprintf(cmd, sizeof cmd, "{\"command\":\"filter.blur.gaussianBlur\",\"params\":%s,\"wait\":%s}",
                     params[i], wait ? "true" : "false");
            pc_error e;
            pc_error_clear(&e);
            CHECK(call(h, "engine.execute", cmd, &r, &e) != PC_OK);
            CHECK_CONTAINS(e.msg, "filter.blur.gaussianBlur");
        }
    }
    pc_headless_free(h);
}

int main(int argc, char **argv)
{
    RUN(test_round_trip);
    RUN(test_batch);
    RUN(test_too_many_steps);
    RUN(test_bad_index);
    RUN(test_bad_filter_params_do_not_crash);
    return pc_t_finish();
}
