/* Background-job machinery parity (crates/engine/src/jobs_tests.rs). The reference drives real
 * filter/transform commands (E3+); E1 exercises the same machinery with the start_job + edit
 * pattern (its gated_job helper) and a JobCtx, which needs no downstream commands.
 *
 * Never-crash note: the reference's "panic in the worker/command becomes an error" tests cannot
 * crash in C (REWRITE_PLAN.md section 7); the worker here returns an error status instead and the
 * same invariants are checked (the document is unchanged, the session still works). */
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#include "color/color.h"
#include "engine/engine.h"
#include "harness.h"

static pc_engine_session *white(uint32_t w, uint32_t h)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(w, h), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1.0f, 1.0f, 1.0f, 1.0f), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

static void nap(void)
{
    struct timespec ts = {0, 1000000}; /* 1 ms */
    (void)nanosleep(&ts, NULL);
}

/* Poll until the event for `id` arrives (or ~30 s pass); returns its outcome. */
static pc_engine_job_outcome wait_event(pc_engine_session *s, uint64_t id)
{
    for (int i = 0; i < 30000; i++) {
        pc_engine_job_event *ev = NULL;
        size_t n = 0;
        CHECK(pc_engine_session_poll_jobs(s, &ev, &n, NULL) == PC_OK);
        pc_engine_job_outcome out = PC_ENGINE_JOB_FAILED;
        bool found = false;
        for (size_t k = 0; k < n; k++) {
            if (ev[k].id == id) {
                out = ev[k].outcome;
                found = true;
            }
        }
        pc_engine_job_events_free(ev, n);
        if (found) {
            return out;
        }
        nap();
    }
    CHECK(false); /* timed out */
    return PC_ENGINE_JOB_FAILED;
}

/* ---- JobCtx ------------------------------------------------------------------------------- */

static void test_progress_is_monotonic_and_reaches_one(void)
{
    pc_engine_job_ctx *ctx = pc_engine_job_ctx_new();
    CHECK(ctx != NULL);
    pc_engine_job_ctx_progress(ctx, 0.5f, "half");
    pc_engine_job_ctx_progress(ctx, 0.3f, "");
    CHECK(pc_engine_job_ctx_fraction(ctx) == 0.5f); /* never goes backwards */
    pc_engine_job_ctx_progress(ctx, NAN, "");
    pc_engine_job_ctx_progress(ctx, 7.0f, "");
    CHECK(pc_engine_job_ctx_fraction(ctx) == 1.0f);
    CHECK(!pc_engine_job_ctx_cancelled(ctx));
    pc_engine_job_ctx_cancel(ctx);
    CHECK(pc_engine_job_ctx_cancelled(ctx));
    CHECK(pc_engine_job_ctx_check(ctx, NULL) == PC_ERR_CANCELLED);
    pc_engine_job_ctx_free(ctx);
}

/* ---- a gated job (jobs_tests::gated_job) -------------------------------------------------- */

static pc_status gated_work(const pc_engine_job_ctx *ctx, void *ud, void **out_payload, pc_error *err)
{
    atomic_bool *gate = ud;
    while (!atomic_load_explicit(gate, memory_order_relaxed)) {
        if (pc_engine_job_ctx_check(ctx, err) != PC_OK) {
            return PC_ERR_CANCELLED;
        }
        pc_engine_job_ctx_progress(ctx, 0.5f, "waiting");
        nap();
    }
    *out_payload = NULL;
    return PC_OK;
}

static pc_status noop_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *ctx, pc_error *err)
{
    (void)doc;
    (void)ha;
    (void)a;
    (void)ctx;
    (void)err;
    return PC_OK;
}

static void result_null(pc_json_value *out)
{
    memset(out, 0, sizeof *out);
    out->doc = yyjson_mut_doc_new(NULL);
    if (out->doc != NULL) {
        yyjson_mut_doc_set_root(out->doc, yyjson_mut_null(out->doc));
    }
}

static pc_status gated_apply(pc_engine_session *s, void *payload, pc_json_value *out, pc_error *err)
{
    (void)payload;
    if (pc_engine_session_edit(s, "Gated Job", noop_edit, NULL, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    result_null(out);
    return PC_OK;
}

static uint64_t start_gated(pc_engine_session *s, atomic_bool *gate)
{
    bool is_job = false;
    uint64_t id = 0;
    pc_json_value out;
    CHECK(pc_engine_session_start_job(s, "test.gated", NULL, "Gated Job", true, gated_work, gate, NULL,
                                      NULL, gated_apply, &is_job, &id, &out, NULL) == PC_OK);
    CHECK(is_job);
    pc_json_value_fini(&out);
    return id;
}

static void test_job_locks_its_document(void)
{
    pc_engine_session *s = white(32, 32);
    atomic_bool gate;
    atomic_init(&gate, false);
    size_t past0 = pc_history_past_len(pc_engine_session_active(s)->history);
    uint64_t id = start_gated(s, &gate);
    /* Edits of the busy document are disabled with a message naming the job. */
    pc_error why = {0};
    CHECK(pc_engine_session_disabled_reason(s, "select.all", &why));
    CHECK(strstr(why.msg, "Gated Job") != NULL);
    CHECK(!pc_engine_session_is_enabled(s, "select.all"));
    CHECK(pc_engine_session_disabled_reason(s, "edit.undo", NULL)); /* undo waits */
    CHECK(!pc_engine_session_undo(s));
    /* Queries still run. */
    pc_json_value out;
    CHECK(pc_engine_session_execute(s, "jobs.list", NULL, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    /* Open the gate: the job finishes and records one undo step. */
    atomic_store_explicit(&gate, true, memory_order_relaxed);
    CHECK(wait_event(s, id) == PC_ENGINE_JOB_DONE);
    CHECK(pc_history_past_len(pc_engine_session_active(s)->history) == past0 + 1);
    pc_engine_session_free(s);
}

static void test_cancel_leaves_document_unchanged(void)
{
    pc_engine_session *s = white(64, 64);
    const pc_document *before = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    uint64_t rev = pc_engine_session_active(s)->revision;
    size_t past = pc_history_past_len(pc_engine_session_active(s)->history);
    atomic_bool gate;
    atomic_init(&gate, false);
    uint64_t id = start_gated(s, &gate);
    CHECK(pc_engine_session_cancel_job(s, id));
    CHECK(wait_event(s, id) == PC_ENGINE_JOB_CANCELLED);
    pc_engine_session_join_cancelled_jobs(s);
    /* The snapshot was not replaced; revision and history are unchanged. */
    CHECK(pc_doc_arc_get(pc_engine_session_active(s)->doc) == before);
    CHECK(pc_engine_session_active(s)->revision == rev);
    CHECK(pc_history_past_len(pc_engine_session_active(s)->history) == past);
    /* wait_job reports the cancellation. */
    pc_json_value out;
    CHECK(pc_engine_session_wait_job(s, id, &out, NULL) == PC_ERR_CANCELLED);
    pc_engine_session_free(s);
}

static pc_status failing_work(const pc_engine_job_ctx *ctx, void *ud, void **out_payload, pc_error *err)
{
    (void)ctx;
    (void)ud;
    (void)out_payload;
    return pc_fail(err, PC_ERR_INVALID, "`Failing job` failed with an internal error");
}

static void test_worker_error_becomes_failure(void)
{
    pc_engine_session *s = white(32, 32);
    const pc_document *before = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    bool is_job = false;
    uint64_t id = 0;
    pc_json_value out;
    CHECK(pc_engine_session_start_job(s, "test.fails", NULL, "Failing job", true, failing_work, NULL, NULL,
                                      NULL, gated_apply, &is_job, &id, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(is_job);
    CHECK(wait_event(s, id) == PC_ENGINE_JOB_FAILED);
    CHECK(pc_doc_arc_get(pc_engine_session_active(s)->doc) == before); /* unchanged */
    /* The session still works. */
    CHECK(pc_engine_session_execute(s, "select.all", NULL, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_wait_job_blocks_until_applied(void)
{
    pc_engine_session *s = white(32, 32);
    atomic_bool gate;
    atomic_init(&gate, true); /* finishes immediately */
    uint64_t id = start_gated(s, &gate);
    pc_json_value out;
    CHECK(pc_engine_session_wait_job(s, id, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_history_past_len(pc_engine_session_active(s)->history) == 1);
    /* Waiting again reports the recorded result. */
    CHECK(pc_engine_session_wait_job(s, id, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_wait_job(s, 999999, &out, NULL) != PC_OK);
    pc_engine_session_free(s);
}

static void test_jobs_cancel_command_and_bad_params(void)
{
    pc_engine_session *s = white(64, 64);
    atomic_bool gate;
    atomic_init(&gate, false);
    uint64_t id = start_gated(s, &gate);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "jobs.cancel", "{\"job\":\"x\"}", &out, NULL) != PC_OK);
    CHECK(pc_engine_session_execute_json(s, "jobs.cancel", "{\"job\":999}", &out, NULL) != PC_OK);
    char p[48];
    (void)snprintf(p, sizeof p, "{\"job\":%llu}", (unsigned long long)id);
    CHECK(pc_engine_session_execute_json(s, "jobs.cancel", p, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(wait_event(s, id) == PC_ENGINE_JOB_CANCELLED);
    pc_engine_session_join_cancelled_jobs(s);
    CHECK(pc_engine_session_execute(s, "jobs.cancel", NULL, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_non_job_command_runs_inline_with_start(void)
{
    pc_engine_session *s = white(16, 16);
    bool is_job = true;
    uint64_t id = 0;
    pc_json_value out;
    yyjson_doc *jd = NULL;
    CHECK(pc_json_parse("{}", 2, &jd, NULL) == PC_OK);
    CHECK(pc_engine_session_start(s, "select.all", yyjson_doc_get_root(jd), &is_job, &id, &out, NULL) == PC_OK);
    CHECK(!is_job);
    pc_json_value_fini(&out);
    yyjson_doc_free(jd);
    /* Unknown commands still error. */
    bool ij = false;
    CHECK(pc_engine_session_start(s, "no.such", NULL, &ij, &id, &out, NULL) == PC_ERR_NOT_FOUND);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_progress_is_monotonic_and_reaches_one);
    RUN(test_job_locks_its_document);
    RUN(test_cancel_leaves_document_unchanged);
    RUN(test_worker_error_becomes_failure);
    RUN(test_wait_job_blocks_until_applied);
    RUN(test_jobs_cancel_command_and_bad_params);
    RUN(test_non_job_command_runs_inline_with_start);
    return pc_t_finish();
}
