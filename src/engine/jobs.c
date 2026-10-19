/* Background jobs and command dispatch (port of crates/engine/src/jobs.rs).
 *
 * A job-capable command runs its heavy part on a worker thread through pc_engine_run_job; the
 * result is applied on the calling thread as one undo step, through the same path a synchronous
 * command takes. dispatch() is the single entry every command goes through.
 *
 * Never-crash note (REWRITE_PLAN.md section 7): Rust catches a *panic* in the worker or command
 * and reports it as an error, keeping the document. C cannot catch a crash, so the contract is the
 * stronger "never crash": a work/run callback returns a status, never aborts. The reference's
 * two panic tests become C tests that return an error status and check the same invariants (the
 * document and its history are unchanged, the session still works). */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "base/mem.h"
#include "engine_internal.h"
#include "actions_cmds.h" /* E12: action recording seam (pause/restore recording) */

/* ---- JobCtx ------------------------------------------------------------------------------- */

static pc_engine_job_shared *shared_new(void)
{
    pc_engine_job_shared *sh = pc_calloc_array(1, sizeof *sh);
    if (sh == NULL) {
        return NULL;
    }
    atomic_init(&sh->refs, 1);
    atomic_init(&sh->cancel, false);
    atomic_init(&sh->progress, 0u);
    (void)pthread_mutex_init(&sh->msg_lock, NULL);
    sh->message[0] = '\0';
    return sh;
}

static void shared_retain(pc_engine_job_shared *sh)
{
    if (sh != NULL) {
        atomic_fetch_add_explicit(&sh->refs, 1, memory_order_relaxed);
    }
}

static void shared_release(pc_engine_job_shared *sh)
{
    if (sh != NULL && atomic_fetch_sub_explicit(&sh->refs, 1, memory_order_acq_rel) == 1) {
        (void)pthread_mutex_destroy(&sh->msg_lock);
        free(sh);
    }
}

/* Initialise a JobCtx with a fresh shared state (false on OOM). */
static bool ctx_init(pc_engine_job_ctx *c)
{
    c->shared = shared_new();
    return c->shared != NULL;
}

static void ctx_clone_into(pc_engine_job_ctx *dst, const pc_engine_job_ctx *src)
{
    dst->shared = src->shared;
    shared_retain(dst->shared);
}

static void ctx_fini(pc_engine_job_ctx *c)
{
    if (c != NULL) {
        shared_release(c->shared);
        c->shared = NULL;
    }
}

void pc_engine_job_ctx_progress(const pc_engine_job_ctx *ctx, float fraction, const char *message)
{
    if (ctx == NULL || ctx->shared == NULL) {
        return;
    }
    if (isfinite(fraction)) {
        float clamped = fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);
        unsigned bits;
        memcpy(&bits, &clamped, sizeof bits);
        /* Non-negative floats order like their bits; max keeps progress monotonic. */
        unsigned cur = atomic_load_explicit(&ctx->shared->progress, memory_order_relaxed);
        while (bits > cur &&
               !atomic_compare_exchange_weak_explicit(&ctx->shared->progress, &cur, bits,
                                                      memory_order_relaxed, memory_order_relaxed)) {
            /* retry with the reloaded cur */
        }
    }
    if (message != NULL && message[0] != '\0') {
        (void)pthread_mutex_lock(&ctx->shared->msg_lock);
        if (strcmp(ctx->shared->message, message) != 0) {
            size_t n = strlen(message);
            if (n >= sizeof ctx->shared->message) {
                n = sizeof ctx->shared->message - 1;
            }
            memcpy(ctx->shared->message, message, n);
            ctx->shared->message[n] = '\0';
        }
        (void)pthread_mutex_unlock(&ctx->shared->msg_lock);
    }
}

bool pc_engine_job_ctx_cancelled(const pc_engine_job_ctx *ctx)
{
    return ctx != NULL && ctx->shared != NULL &&
           atomic_load_explicit(&ctx->shared->cancel, memory_order_relaxed);
}

void pc_engine_job_ctx_cancel(const pc_engine_job_ctx *ctx)
{
    if (ctx != NULL && ctx->shared != NULL) {
        atomic_store_explicit(&ctx->shared->cancel, true, memory_order_relaxed);
    }
}

pc_status pc_engine_job_ctx_check(const pc_engine_job_ctx *ctx, pc_error *err)
{
    return pc_engine_job_ctx_cancelled(ctx) ? pc_engine_err_cancelled(err) : PC_OK;
}

float pc_engine_job_ctx_fraction(const pc_engine_job_ctx *ctx)
{
    if (ctx == NULL || ctx->shared == NULL) {
        return 0.0f;
    }
    unsigned bits = atomic_load_explicit(&ctx->shared->progress, memory_order_relaxed);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static void ctx_message(const pc_engine_job_ctx *ctx, char out[256])
{
    out[0] = '\0';
    if (ctx == NULL || ctx->shared == NULL) {
        return;
    }
    (void)pthread_mutex_lock(&ctx->shared->msg_lock);
    size_t n = strlen(ctx->shared->message);
    if (n > 255) {
        n = 255;
    }
    memcpy(out, ctx->shared->message, n);
    out[n] = '\0';
    (void)pthread_mutex_unlock(&ctx->shared->msg_lock);
}

pc_engine_job_ctx *pc_engine_job_ctx_new(void)
{
    pc_engine_job_ctx *c = pc_malloc(sizeof *c);
    if (c == NULL) {
        return NULL;
    }
    if (!ctx_init(c)) {
        free(c);
        return NULL;
    }
    return c;
}

void pc_engine_job_ctx_free(pc_engine_job_ctx *ctx)
{
    if (ctx != NULL) {
        ctx_fini(ctx);
        free(ctx);
    }
}

/* ---- worker mailbox ----------------------------------------------------------------------- */

typedef struct job_mailbox {
    atomic_int refs;
    pc_engine_job_ctx ctx; /* retained */
    pc_engine_job_work_fn work;
    void *ud;
    void (*free_ud)(void *);
    void (*free_payload)(void *);
    pthread_mutex_t lock;
    bool done;
    pc_status rc;
    void *payload;
    pc_status errcode;
    char errmsg[PC_ERROR_MSG_MAX];
} job_mailbox;

static void mailbox_release(job_mailbox *mb)
{
    if (mb == NULL || atomic_fetch_sub_explicit(&mb->refs, 1, memory_order_acq_rel) != 1) {
        return;
    }
    /* Last reference: free a payload that was produced but never applied. */
    if (mb->payload != NULL && mb->free_payload != NULL) {
        mb->free_payload(mb->payload);
    }
    if (mb->free_ud != NULL) {
        mb->free_ud(mb->ud);
    }
    ctx_fini(&mb->ctx);
    (void)pthread_mutex_destroy(&mb->lock);
    free(mb);
}

static void *worker_main(void *arg)
{
    job_mailbox *mb = arg;
    void *payload = NULL;
    pc_error e = {0};
    pc_status rc = mb->work(&mb->ctx, mb->ud, &payload, &e);
    (void)pthread_mutex_lock(&mb->lock);
    mb->done = true;
    mb->rc = rc;
    if (rc == PC_OK) {
        mb->payload = payload;
    } else {
        mb->errcode = e.code;
        memcpy(mb->errmsg, e.msg, sizeof mb->errmsg);
        if (payload != NULL && mb->free_payload != NULL) {
            mb->free_payload(payload);
        }
    }
    (void)pthread_mutex_unlock(&mb->lock);
    mailbox_release(mb);
    return NULL;
}

/* ---- Running / recent rows ---------------------------------------------------------------- */

typedef struct pc_engine_running {
    uint64_t id;
    char *command; /* owned */
    char *label;   /* owned */
    pc_json_value params;
    bool journal;
    bool has_doc;
    pc_doc_id doc;
    pc_doc_arc *snapshot; /* retained, or NULL */
    pc_engine_job_ctx ctx;
    double started_ms;
    char *coalesce; /* owned or NULL */
    size_t color_restrict;
    job_mailbox *mb; /* NULL for an inline run that never spawned */
    pthread_t thread;
    bool has_thread;
    pc_engine_job_apply_fn apply;
    void (*free_payload)(void *);
} pc_engine_running;

struct pc_engine_job_info_row {
    uint64_t id;
    char *command;
    char *label;
    bool has_doc;
    pc_doc_id doc;
    const char *state; /* "done" | "failed" | "cancelled" static */
    float progress;
    char message[256];
    double elapsed_ms;
    bool has_result;
    pc_json_value result;
    char *error; /* owned, or NULL */
};

enum { RECENT = 16 };

static double now_ms(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static char *dup_str(const char *s)
{
    return s == NULL ? NULL : pc_strndup(s, strlen(s));
}

static void running_free(pc_engine_running *r)
{
    if (r == NULL) {
        return;
    }
    free(r->command);
    free(r->label);
    pc_json_value_fini(&r->params);
    pc_doc_arc_release(r->snapshot);
    ctx_fini(&r->ctx);
    free(r->coalesce);
    if (r->mb != NULL) {
        mailbox_release(r->mb);
    }
    free(r);
}

void pc_engine_jobs_init(pc_engine_jobs *j)
{
    memset(j, 0, sizeof *j);
}

void pc_engine_jobs_fini(pc_engine_jobs *j)
{
    /* Stop workers (they own snapshots only) and join those we can. */
    for (size_t i = 0; i < j->nrunning; i++) {
        pc_engine_job_ctx_cancel(&j->running[i]->ctx);
    }
    if (j->pending != NULL) {
        pc_engine_job_ctx_cancel(&j->pending->ctx);
    }
    for (size_t i = 0; i < j->nrunning; i++) {
        if (j->running[i]->has_thread) {
            (void)pthread_join(j->running[i]->thread, NULL);
        }
        running_free(j->running[i]);
    }
    free(j->running);
    if (j->pending != NULL) {
        if (j->pending->has_thread) {
            (void)pthread_join(j->pending->thread, NULL);
        }
        running_free(j->pending);
    }
    for (size_t i = 0; i < j->ndraining; i++) {
        if (!j->draining_joined[i]) {
            (void)pthread_join(j->draining[i], NULL);
        }
    }
    free(j->draining);
    free(j->draining_joined);
    for (size_t i = 0; i < j->nevents; i++) {
        free(j->events[i].command);
        free(j->events[i].label);
        pc_json_value_fini(&j->events[i].result);
        free(j->events[i].error);
    }
    free(j->events);
    for (size_t i = 0; i < j->nrecent; i++) {
        free(j->recent[i].command);
        free(j->recent[i].label);
        pc_json_value_fini(&j->recent[i].result);
        free(j->recent[i].error);
    }
    free(j->recent);
    if (j->has_inline) {
        ctx_fini(&j->inline_ctx);
    }
    memset(j, 0, sizeof *j);
}

/* ---- commands allowed while a job runs on the active document ----------------------------- */

static bool independent_of_document(const char *id)
{
    static const char *const PREFIXES[] = {"jobs.",          "tools.",           "view.", "window.", "help.",
                                           "brush.presets.", "gradient.presets."};
    static const char *const IDS[] = {"file.new",         "file.close",         "file.closeAll",
                                      "edit.preferences", "edit.colorSettings", "edit.keyboardShortcuts",
                                      "edit.menus"};
    for (size_t i = 0; i < sizeof PREFIXES / sizeof PREFIXES[0]; i++) {
        if (strncmp(id, PREFIXES[i], strlen(PREFIXES[i])) == 0) {
            return true;
        }
    }
    for (size_t i = 0; i < sizeof IDS / sizeof IDS[0]; i++) {
        if (strcmp(id, IDS[i]) == 0) {
            return true;
        }
    }
    return false;
}

static pc_engine_running *running_by_id(pc_engine_jobs *j, uint64_t id, size_t *idx)
{
    for (size_t i = 0; i < j->nrunning; i++) {
        if (j->running[i]->id == id) {
            if (idx != NULL) {
                *idx = i;
            }
            return j->running[i];
        }
    }
    return NULL;
}

bool pc_engine_job_on(const pc_engine_session *s, pc_doc_id doc)
{
    for (size_t i = 0; i < s->jobs.nrunning; i++) {
        if (s->jobs.running[i]->has_doc && s->jobs.running[i]->doc == doc) {
            return true;
        }
    }
    return false;
}

bool pc_engine_active_job(const pc_engine_session *s)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    return st != NULL && pc_engine_job_on(s, pc_doc_arc_get(st->doc)->id);
}

bool pc_engine_job_conflict(const pc_engine_session *s, const char *id, bool journal, pc_error *reason)
{
    if (!journal || independent_of_document(id)) {
        return false;
    }
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return false;
    }
    pc_doc_id d = pc_doc_arc_get(st->doc)->id;
    for (size_t i = 0; i < s->jobs.nrunning; i++) {
        if (s->jobs.running[i]->has_doc && s->jobs.running[i]->doc == d) {
            (void)pc_fail(reason, PC_ERR_INVALID,
                          "\xe2\x80\x9c%s\xe2\x80\x9d is still running on this document; wait for it to "
                          "finish or cancel it (Esc)",
                          s->jobs.running[i]->label);
            return true;
        }
    }
    return false;
}

void pc_engine_cancel_jobs_on(pc_engine_session *s, pc_doc_id doc)
{
    for (;;) {
        bool any = false;
        for (size_t i = 0; i < s->jobs.nrunning; i++) {
            if (s->jobs.running[i]->has_doc && s->jobs.running[i]->doc == doc) {
                (void)pc_engine_session_cancel_job(s, s->jobs.running[i]->id);
                any = true;
                break;
            }
        }
        if (!any) {
            break;
        }
    }
}

/* ---- run_job ------------------------------------------------------------------------------ */

pc_status pc_engine_run_job(pc_engine_session *s, const char *label, bool lock_document,
                            pc_engine_job_work_fn work, void *ud, void (*free_ud)(void *),
                            void (*free_payload)(void *), pc_engine_job_apply_fn apply, pc_json_value *out,
                            pc_error *err)
{
    if (!s->jobs.spawn) {
        /* Inline: run work then apply on this thread, as before jobs existed. */
        pc_engine_job_ctx local;
        const pc_engine_job_ctx *ctx;
        bool owns_local = false;
        if (s->jobs.has_inline) {
            ctx = &s->jobs.inline_ctx;
        } else {
            if (!ctx_init(&local)) {
                if (free_ud != NULL) {
                    free_ud(ud);
                }
                return pc_fail(err, PC_ERR_NOMEM, "job context");
            }
            owns_local = true;
            ctx = &local;
        }
        void *payload = NULL;
        pc_status rc = work(ctx, ud, &payload, err);
        if (free_ud != NULL) {
            free_ud(ud);
        }
        if (owns_local) {
            ctx_fini(&local);
        }
        if (rc != PC_OK) {
            if (payload != NULL && free_payload != NULL) {
                free_payload(payload);
            }
            return rc;
        }
        rc = apply(s, payload, out, err);
        if (free_payload != NULL) {
            free_payload(payload);
        }
        return rc;
    }
    if (s->jobs.pending != NULL) {
        if (free_ud != NULL) {
            free_ud(ud);
        }
        return pc_engine_err_other(err, "`%s` started a second background job in one command", label);
    }
    bool has_doc = false;
    pc_doc_id doc_id = 0;
    pc_doc_arc *snapshot = NULL;
    if (lock_document) {
        const pc_engine_doc_state *st = pc_engine_session_active(s);
        if (st == NULL) {
            if (free_ud != NULL) {
                free_ud(ud);
            }
            return pc_engine_err_no_document(err);
        }
        has_doc = true;
        doc_id = pc_doc_arc_get(st->doc)->id;
        snapshot = pc_doc_arc_retain(st->doc);
    }
    pc_engine_running *r = pc_calloc_array(1, sizeof *r);
    job_mailbox *mb = pc_calloc_array(1, sizeof *mb);
    if (r == NULL || mb == NULL) {
        free(r);
        free(mb);
        pc_doc_arc_release(snapshot);
        if (free_ud != NULL) {
            free_ud(ud);
        }
        return pc_fail(err, PC_ERR_NOMEM, "job");
    }
    if (!ctx_init(&r->ctx)) {
        free(r);
        free(mb);
        pc_doc_arc_release(snapshot);
        if (free_ud != NULL) {
            free_ud(ud);
        }
        return pc_fail(err, PC_ERR_NOMEM, "job context");
    }
    atomic_init(&mb->refs, 2); /* session + worker */
    ctx_clone_into(&mb->ctx, &r->ctx);
    mb->work = work;
    mb->ud = ud;
    mb->free_ud = free_ud;
    mb->free_payload = free_payload;
    (void)pthread_mutex_init(&mb->lock, NULL);
    s->jobs.next++;
    r->id = s->jobs.next;
    r->command = dup_str("");
    r->label = dup_str(label);
    r->journal = true;
    r->has_doc = has_doc;
    r->doc = doc_id;
    r->snapshot = snapshot;
    r->started_ms = now_ms();
    r->coalesce = dup_str(s->coalesce_request);
    r->color_restrict = s->color_restrict;
    r->mb = mb;
    r->apply = apply;
    r->free_payload = free_payload;
    if (pthread_create(&r->thread, NULL, worker_main, mb) != 0) {
        /* Spawn failed: the worker never ran, so drop its mailbox reference here. */
        mailbox_release(mb); /* worker's ref */
        r->mb = NULL;        /* running_free will drop the session ref below */
        running_free(r);
        return pc_engine_err_other(err, "could not start a background job");
    }
    r->has_thread = true;
    s->jobs.pending = r;
    /* The command's result is {"job": id}. */
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "job", r->id);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ---- recent rows + events ----------------------------------------------------------------- */

static void end_job(pc_engine_session *s, pc_engine_running *r, pc_engine_job_outcome outcome,
                    const pc_json_value *result, const char *error)
{
    const char *state = outcome == PC_ENGINE_JOB_DONE     ? "done"
                        : outcome == PC_ENGINE_JOB_FAILED ? "failed"
                                                          : "cancelled";
    /* recent row */
    pc_engine_job_info_row *gr =
        pc_grow_array(s->jobs.recent, &s->jobs.cap_recent, s->jobs.nrecent + 1, sizeof *s->jobs.recent);
    if (gr != NULL) {
        s->jobs.recent = gr;
        pc_engine_job_info_row *row = &s->jobs.recent[s->jobs.nrecent];
        memset(row, 0, sizeof *row);
        row->id = r->id;
        row->command = dup_str(r->command);
        row->label = dup_str(r->label);
        row->has_doc = r->has_doc;
        row->doc = r->doc;
        row->state = state;
        row->progress = outcome == PC_ENGINE_JOB_DONE ? 1.0f : pc_engine_job_ctx_fraction(&r->ctx);
        ctx_message(&r->ctx, row->message);
        row->elapsed_ms = now_ms() - r->started_ms;
        if (outcome == PC_ENGINE_JOB_DONE && result != NULL) {
            if (pc_json_value_clone(result, &row->result, NULL) == PC_OK) {
                row->has_result = true;
            }
        }
        if (outcome == PC_ENGINE_JOB_FAILED && error != NULL) {
            row->error = dup_str(error);
        }
        s->jobs.nrecent++;
        if (s->jobs.nrecent > RECENT) {
            size_t extra = s->jobs.nrecent - RECENT;
            for (size_t i = 0; i < extra; i++) {
                free(s->jobs.recent[i].command);
                free(s->jobs.recent[i].label);
                pc_json_value_fini(&s->jobs.recent[i].result);
                free(s->jobs.recent[i].error);
            }
            memmove(&s->jobs.recent[0], &s->jobs.recent[extra],
                    (s->jobs.nrecent - extra) * sizeof *s->jobs.recent);
            s->jobs.nrecent -= extra;
        }
    }
    /* event */
    pc_engine_job_event *ge =
        pc_grow_array(s->jobs.events, &s->jobs.cap_events, s->jobs.nevents + 1, sizeof *s->jobs.events);
    if (ge != NULL) {
        s->jobs.events = ge;
        pc_engine_job_event *ev = &s->jobs.events[s->jobs.nevents];
        memset(ev, 0, sizeof *ev);
        ev->id = r->id;
        ev->command = dup_str(r->command);
        ev->label = dup_str(r->label);
        ev->has_document = r->has_doc;
        ev->document = r->doc;
        ev->outcome = outcome;
        if (outcome == PC_ENGINE_JOB_DONE && result != NULL) {
            (void)pc_json_value_clone(result, &ev->result, NULL);
        }
        if (outcome == PC_ENGINE_JOB_FAILED && error != NULL) {
            ev->error = dup_str(error);
        }
        s->jobs.nevents++;
    }
}

/* Bookkeeping after a command (or a job's apply) succeeded (after_command).
 *
 * `nested_from` is the journal length when the command started. A replayable command stands for
 * the commands it ran itself (e.g. Straighten runs Image Rotation and Crop): their entries are
 * dropped so a recorded action replays the command once, not its steps as well (#3067). */
static void after_command(pc_engine_session *s, const char *id, yyjson_val *params, bool journal,
                          size_t nested_from)
{
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st != NULL) {
        pc_engine_hook_mask_view_fix(st);
    }
    pc_engine_hook_edit_menu_after_command(s, id);
    pc_engine_hook_automate_after_command(s, id);
    pc_engine_hook_sync_preset_store(s);
    if (!journal) {
        return;
    }
    if (pc_engine_hook_replayable(id) && nested_from <= s->njournal) {
        for (size_t i = nested_from; i < s->njournal; i++) {
            free(s->journal[i].id);
            pc_json_value_fini(&s->journal[i].params);
        }
        s->njournal = nested_from;
    }
    if (!pc_engine_hook_coalesce_journal(s, id, params)) {
        (void)pc_engine_journal_push(s, id, params, NULL);
    }
}

/* Apply a job whose worker delivered a payload. */
static pc_status apply_job(pc_engine_session *s, pc_engine_running *r, void *payload, pc_json_value *out,
                           pc_error *err)
{
    yyjson_mut_val *stored_params = pc_json_value_root(&r->params);
    yyjson_doc *params = stored_params ? yyjson_mut_val_imut_copy(stored_params, NULL) : NULL;
    if (stored_params && !params)
        return pc_fail(err, PC_ERR_NOMEM, "job parameters");
    size_t target_index = SIZE_MAX;
    bool had_prev = s->has_active;
    size_t prev_active = s->active;
    bool restored_target = false;
    if (r->has_doc) {
        size_t i;
        bool found = false;
        for (i = 0; i < s->ndocs; i++) {
            if (pc_doc_arc_get(s->docs[i].doc)->id == r->doc) {
                found = true;
                break;
            }
        }
        if (!found) {
            yyjson_doc_free(params);
            return pc_engine_err_other(err, "`%s`: the document was closed", r->label);
        }
        bool unchanged = r->snapshot != NULL && s->docs[i].doc == r->snapshot;
        if (!unchanged) {
            yyjson_doc_free(params);
            return pc_engine_err_other(
                err, "`%s`: the document changed while it ran; the result was discarded", r->label);
        }
        s->has_active = true;
        s->active = i;
        target_index = i;
        restored_target = true;
    }
    free(s->coalesce_request);
    s->coalesce_request = dup_str(r->coalesce);
    s->color_restrict = r->color_restrict;
    size_t job_nested_from = s->njournal;
    pc_status rc = r->apply(s, payload, out, err);
    free(s->coalesce_request);
    s->coalesce_request = NULL;
    s->color_restrict = SIZE_MAX;
    if (rc == PC_OK) {
        after_command(s, r->command, params ? yyjson_doc_get_root(params) : NULL, r->journal,
                      job_nested_from);
    }
    /* Keep the user's active document unless the job opened a new one. */
    if (restored_target && s->has_active && s->active == target_index) {
        if (had_prev && prev_active < s->ndocs) {
            s->active = prev_active;
        }
    }
    yyjson_doc_free(params);
    return rc;
}

static void finish_job(pc_engine_session *s, pc_engine_running *r, bool have_result, pc_status work_rc,
                       void *payload, pc_status errcode, const char *errmsg)
{
    (void)have_result;
    (void)errcode;
    pc_engine_job_outcome outcome;
    pc_json_value result;
    memset(&result, 0, sizeof result);
    char errbuf[PC_ERROR_MSG_MAX];
    errbuf[0] = '\0';
    bool have_err = false;
    if (work_rc == PC_ERR_CANCELLED) {
        outcome = PC_ENGINE_JOB_CANCELLED;
    } else if (work_rc != PC_OK) {
        outcome = PC_ENGINE_JOB_FAILED;
        strncpy(errbuf, errmsg != NULL ? errmsg : "the job failed", sizeof errbuf - 1);
        errbuf[sizeof errbuf - 1] = '\0';
        have_err = true;
    } else if (pc_engine_job_ctx_cancelled(&r->ctx)) {
        outcome = PC_ENGINE_JOB_CANCELLED;
        if (payload != NULL && r->free_payload != NULL) {
            r->free_payload(payload);
        }
    } else {
        pc_error e = {0};
        pc_status rc = apply_job(s, r, payload, &result, &e);
        if (payload != NULL && r->free_payload != NULL) {
            r->free_payload(payload);
        }
        if (rc == PC_OK) {
            outcome = PC_ENGINE_JOB_DONE;
        } else if (rc == PC_ERR_CANCELLED) {
            outcome = PC_ENGINE_JOB_CANCELLED;
            pc_json_value_fini(&result);
            memset(&result, 0, sizeof result);
        } else {
            outcome = PC_ENGINE_JOB_FAILED;
            strncpy(errbuf, e.msg, sizeof errbuf - 1);
            errbuf[sizeof errbuf - 1] = '\0';
            have_err = true;
            pc_json_value_fini(&result);
            memset(&result, 0, sizeof result);
        }
    }
    end_job(s, r, outcome, outcome == PC_ENGINE_JOB_DONE ? &result : NULL, have_err ? errbuf : NULL);
    pc_json_value_fini(&result);
}

/* Collect one running job's worker result if it is ready (try_recv). Returns true if collected
 * (the job was removed and finished). */
static bool try_collect(pc_engine_session *s, size_t i)
{
    pc_engine_running *r = s->jobs.running[i];
    if (r->mb == NULL) {
        return false;
    }
    (void)pthread_mutex_lock(&r->mb->lock);
    bool done = r->mb->done;
    pc_status rc = r->mb->rc;
    void *payload = r->mb->payload;
    pc_status errcode = r->mb->errcode;
    char errmsg[PC_ERROR_MSG_MAX];
    memcpy(errmsg, r->mb->errmsg, sizeof errmsg);
    if (done) {
        r->mb->payload = NULL; /* ownership transfers to finish_job */
    }
    (void)pthread_mutex_unlock(&r->mb->lock);
    if (!done) {
        return false;
    }
    if (r->has_thread) {
        (void)pthread_join(r->thread, NULL);
        r->has_thread = false;
    }
    /* remove from running */
    memmove(&s->jobs.running[i], &s->jobs.running[i + 1],
            (s->jobs.nrunning - i - 1) * sizeof *s->jobs.running);
    s->jobs.nrunning--;
    finish_job(s, r, true, rc, payload, errcode, errmsg);
    running_free(r);
    return true;
}

pc_status pc_engine_session_poll_jobs(pc_engine_session *s, pc_engine_job_event **out, size_t *n,
                                      pc_error *err)
{
    size_t i = 0;
    while (i < s->jobs.nrunning) {
        if (!try_collect(s, i)) {
            i++;
        }
    }
    /* Drop finished drainers. */
    size_t keep = 0;
    for (size_t k = 0; k < s->jobs.ndraining; k++) {
        if (!s->jobs.draining_joined[k]) {
            s->jobs.draining[keep] = s->jobs.draining[k];
            s->jobs.draining_joined[keep] = false;
            keep++;
        }
    }
    s->jobs.ndraining = keep;
    (void)err;
    *out = s->jobs.events;
    *n = s->jobs.nevents;
    s->jobs.events = NULL;
    s->jobs.nevents = 0;
    s->jobs.cap_events = 0;
    return PC_OK;
}

void pc_engine_job_events_free(pc_engine_job_event *ev, size_t n)
{
    if (ev == NULL) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        free(ev[i].command);
        free(ev[i].label);
        pc_json_value_fini(&ev[i].result);
        free(ev[i].error);
    }
    free(ev);
}

pc_status pc_engine_session_wait_job(pc_engine_session *s, uint64_t id, pc_json_value *out, pc_error *err)
{
    size_t idx;
    pc_engine_running *r = running_by_id(&s->jobs, id, &idx);
    if (r != NULL) {
        if (r->has_thread) {
            (void)pthread_join(r->thread, NULL);
            r->has_thread = false;
        }
        pc_status rc = PC_OK;
        void *payload = NULL;
        pc_status errcode = PC_OK;
        char errmsg[PC_ERROR_MSG_MAX];
        errmsg[0] = '\0';
        if (r->mb != NULL) {
            (void)pthread_mutex_lock(&r->mb->lock);
            rc = r->mb->rc;
            payload = r->mb->payload;
            errcode = r->mb->errcode;
            memcpy(errmsg, r->mb->errmsg, sizeof errmsg);
            r->mb->payload = NULL;
            (void)pthread_mutex_unlock(&r->mb->lock);
        }
        memmove(&s->jobs.running[idx], &s->jobs.running[idx + 1],
                (s->jobs.nrunning - idx - 1) * sizeof *s->jobs.running);
        s->jobs.nrunning--;
        finish_job(s, r, true, rc, payload, errcode, errmsg);
        running_free(r);
    }
    /* Look up the recorded outcome (newest first). */
    for (size_t k = s->jobs.nrecent; k-- > 0;) {
        if (s->jobs.recent[k].id == id) {
            const pc_engine_job_info_row *row = &s->jobs.recent[k];
            if (strcmp(row->state, "done") == 0) {
                if (row->has_result) {
                    return pc_json_value_clone(&row->result, out, err);
                }
                return pc_engine_result_null(out, err);
            }
            if (strcmp(row->state, "cancelled") == 0) {
                return pc_engine_err_cancelled(err);
            }
            return pc_engine_err_other(err, "%s", row->error != NULL ? row->error : "the job failed");
        }
    }
    return pc_engine_err_other(err, "no job %llu", (unsigned long long)id);
}

bool pc_engine_session_cancel_job(pc_engine_session *s, uint64_t id)
{
    size_t idx;
    pc_engine_running *r = running_by_id(&s->jobs, id, &idx);
    if (r == NULL) {
        return false;
    }
    if (r->command != NULL &&
        (strcmp(r->command, "file.print") == 0 || strcmp(r->command, "file.printOneCopy") == 0)) {
        return false; /* handed to the OS print queue; removal belongs there */
    }
    memmove(&s->jobs.running[idx], &s->jobs.running[idx + 1],
            (s->jobs.nrunning - idx - 1) * sizeof *s->jobs.running);
    s->jobs.nrunning--;
    pc_engine_job_ctx_cancel(&r->ctx);
    end_job(s, r, PC_ENGINE_JOB_CANCELLED, NULL, NULL);
    /* Let the worker keep unwinding; join it later (draining). */
    if (r->has_thread) {
        pthread_t *gd = pc_grow_array(s->jobs.draining, &s->jobs.cap_draining, s->jobs.ndraining + 1,
                                      sizeof *s->jobs.draining);
        bool *gj = NULL;
        if (gd != NULL) {
            s->jobs.draining = gd;
            gj = pc_grow_array(s->jobs.draining_joined, &s->jobs.cap_draining, s->jobs.ndraining + 1,
                               sizeof *s->jobs.draining_joined);
        }
        if (gd != NULL && gj != NULL) {
            s->jobs.draining_joined = gj;
            s->jobs.draining[s->jobs.ndraining] = r->thread;
            s->jobs.draining_joined[s->jobs.ndraining] = false;
            s->jobs.ndraining++;
            r->has_thread = false;
        } else {
            (void)pthread_join(r->thread, NULL); /* fall back to a blocking join on OOM */
            r->has_thread = false;
        }
    }
    running_free(r);
    return true;
}

void pc_engine_session_cancel_all_jobs(pc_engine_session *s)
{
    while (s->jobs.nrunning > 0) {
        uint64_t id = s->jobs.running[0]->id;
        if (!pc_engine_session_cancel_job(s, id)) {
            /* A non-cancellable job (print): skip it to avoid an infinite loop. */
            bool moved = false;
            for (size_t i = 0; i < s->jobs.nrunning; i++) {
                if (s->jobs.running[i]->id != id) {
                    moved = true;
                    break;
                }
            }
            if (!moved) {
                break;
            }
        }
    }
}

void pc_engine_session_join_cancelled_jobs(pc_engine_session *s)
{
    for (size_t i = 0; i < s->jobs.ndraining; i++) {
        if (!s->jobs.draining_joined[i]) {
            (void)pthread_join(s->jobs.draining[i], NULL);
            s->jobs.draining_joined[i] = true;
        }
    }
    s->jobs.ndraining = 0;
}

bool pc_engine_session_has_jobs(const pc_engine_session *s)
{
    return s->jobs.nrunning > 0;
}

void pc_engine_session_set_inline_job_ctx(pc_engine_session *s, const pc_engine_job_ctx *ctx)
{
    if (s->jobs.has_inline) {
        ctx_fini(&s->jobs.inline_ctx);
        s->jobs.has_inline = false;
    }
    if (ctx != NULL) {
        ctx_clone_into(&s->jobs.inline_ctx, ctx);
        s->jobs.has_inline = true;
    }
}

/* ---- dispatch ----------------------------------------------------------------------------- */

pc_status pc_engine_dispatch(pc_engine_session *s, const char *id, yyjson_val *params, bool background,
                             bool *out_is_job, uint64_t *out_job_id, pc_json_value *out, pc_error *err)
{
    *out_is_job = false;
    const pc_engine_command *spec = pc_engine_command_find(id);
    if (spec == NULL) {
        return pc_engine_err_unknown_command(err, id);
    }
    if (params != NULL && !yyjson_is_obj(params) && !yyjson_is_null(params)) {
        return pc_engine_err_bad_params(err, id, "params must be a JSON object");
    }
    if (s->authorize != NULL) {
        pc_status rc = s->authorize(id, params, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    if (strcmp(id, "file.new") == 0) {
        pc_status rc = pc_engine_hook_validate_new_params(s, params, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    pc_status rc = pc_engine_hook_param_check(spec, params, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = pc_engine_hook_filter_validate(id, params, err);
    if (rc != PC_OK) {
        return rc;
    }
    bool handled = false;
    rc = pc_engine_hook_float_before_command(s, id, &handled, out, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (handled) {
        return PC_OK;
    }
    /* Build the run params: inject_kind (+__kind for adjustments) then the channel target. */
    yyjson_mut_doc *m = yyjson_mut_doc_new(NULL);
    if (m == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "params");
    }
    yyjson_mut_val *v = pc_engine_inject_kind(id, m, params);
    v = pc_engine_hook_inject_target(s, id, m, v);
    yyjson_mut_doc_set_root(m, v);
    yyjson_doc *imut = yyjson_mut_doc_imut_copy(m, NULL);
    yyjson_mut_doc_free(m);
    if (imut == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "params");
    }
    yyjson_val *run_params = yyjson_doc_get_root(imut);

    /* precondition (explicit-layer and mask-target variants arrive with E5/E7). */
    pc_error reason = {0};
    if (spec->enabled(s, &reason) != PC_OK) {
        yyjson_doc_free(imut);
        return pc_engine_err_disabled(err, id, reason.msg);
    }
    pc_error jc = {0};
    if (pc_engine_job_conflict(s, id, spec->journal, &jc)) {
        yyjson_doc_free(imut);
        return pc_engine_err_disabled(err, id, jc.msg);
    }
    const char *hidden = pc_engine_hidden_target_refusal(s, id, params);
    if (hidden != NULL) {
        yyjson_doc_free(imut);
        return pc_engine_err_other(err, "%s", hidden);
    }

    free(s->coalesce_request);
    s->coalesce_request = NULL;
    yyjson_val *coa = pc_json_get(params, "coalesce");
    if (coa != NULL) {
        size_t clen = 0;
        const char *cs = pc_json_as_str(coa, &clen);
        if (cs != NULL) {
            s->coalesce_request = pc_strndup(cs, clen);
        }
    }
    s->color_restrict = pc_engine_hook_color_restriction(s, id, run_params);
    s->jobs.spawn = background;
    /* Commands this one runs on its own behalf journal after this mark (#3067). A replayable
     * command is recorded as one step, so recording pauses while it runs. */
    size_t nested_from = s->njournal;
    size_t rec_from = 0, rec_idx = 0;
    bool paused = spec->journal && pc_engine_hook_replayable(id) &&
                  pc_engine_actions_take_recording(s, &rec_from, &rec_idx);

    rc = spec->run(s, run_params, out, err);

    if (paused) {
        pc_engine_actions_restore_recording(s, true, rec_from, rec_idx);
    }
    s->jobs.spawn = false;
    free(s->coalesce_request);
    s->coalesce_request = NULL;
    s->color_restrict = SIZE_MAX;
    pc_engine_running *pending = s->jobs.pending;
    s->jobs.pending = NULL;

    if (rc != PC_OK) {
        if (pending != NULL) {
            pc_engine_job_ctx_cancel(&pending->ctx);
            if (pending->has_thread) {
                (void)pthread_join(pending->thread, NULL);
                pending->has_thread = false;
            }
            running_free(pending);
        }
        yyjson_doc_free(imut);
        return rc;
    }
    if (pending != NULL) {
        char *command_copy = pc_strndup(id, strlen(id));
        pc_json_value params_copy = PC_JSON_VALUE_INIT;
        pc_status copied = command_copy ? PC_OK : pc_fail(err, PC_ERR_NOMEM, "job command");
        if (copied == PC_OK && params)
            copied = pc_json_value_from_val(params, &params_copy, err);
        if (copied != PC_OK) {
            free(command_copy);
            pc_json_value_fini(&params_copy);
            pc_engine_job_ctx_cancel(&pending->ctx);
            if (pending->has_thread) {
                (void)pthread_join(pending->thread, NULL);
                pending->has_thread = false;
            }
            running_free(pending);
            yyjson_doc_free(imut);
            return copied;
        }
        free(pending->command);
        pending->command = command_copy;
        pc_json_value_fini(&pending->params);
        pending->params = params_copy;
        pending->journal = spec->journal;
        uint64_t jid = pending->id;
        pc_engine_running **gr = pc_grow_array(s->jobs.running, &s->jobs.cap_running, s->jobs.nrunning + 1,
                                               sizeof *s->jobs.running);
        if (gr == NULL) {
            pc_engine_job_ctx_cancel(&pending->ctx);
            if (pending->has_thread) {
                (void)pthread_join(pending->thread, NULL);
            }
            running_free(pending);
            yyjson_doc_free(imut);
            return pc_fail(err, PC_ERR_NOMEM, "job table");
        }
        s->jobs.running = gr;
        s->jobs.running[s->jobs.nrunning++] = pending;
        *out_is_job = true;
        *out_job_id = jid;
        yyjson_doc_free(imut);
        return PC_OK;
    }
    /* Ran inline: record the journal and run post-command hooks. */
    after_command(s, id, params, spec->journal, nested_from);
    yyjson_doc_free(imut);
    return PC_OK;
}

/* ---- public entry points + job commands --------------------------------------------------- */

pc_status pc_engine_session_execute(pc_engine_session *s, const char *id, yyjson_val *params,
                                    pc_json_value *out, pc_error *err)
{
    bool is_job = false;
    uint64_t jid = 0;
    pc_status rc = pc_engine_dispatch(s, id, params, false, &is_job, &jid, out, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (is_job) {
        pc_json_value_fini(out);
        return pc_engine_session_wait_job(s, jid, out, err);
    }
    return PC_OK;
}

pc_status pc_engine_session_execute_json(pc_engine_session *s, const char *id, const char *params_json,
                                         pc_json_value *out, pc_error *err)
{
    yyjson_doc *doc = NULL;
    yyjson_val *root = NULL;
    if (params_json != NULL) {
        if (pc_json_parse(params_json, strlen(params_json), &doc, err) != PC_OK) {
            return PC_ERR_INVALID;
        }
        root = yyjson_doc_get_root(doc);
    }
    pc_status rc = pc_engine_session_execute(s, id, root, out, err);
    yyjson_doc_free(doc);
    return rc;
}

pc_status pc_engine_session_start(pc_engine_session *s, const char *id, yyjson_val *params, bool *out_is_job,
                                  uint64_t *out_job_id, pc_json_value *out, pc_error *err)
{
    return pc_engine_dispatch(s, id, params, true, out_is_job, out_job_id, out, err);
}

/* A standalone (non-command) job. Carries the user's work/apply through an owned closure box. */
typedef struct standalone_box {
    pc_engine_job_work_fn work;
    void *ud;
    void (*free_ud)(void *);
    void (*free_payload)(void *);
    pc_engine_job_apply_fn apply;
} standalone_box;

pc_status pc_engine_session_start_job(pc_engine_session *s, const char *command, yyjson_val *params,
                                      const char *label, bool lock_document, pc_engine_job_work_fn work,
                                      void *ud, void (*free_ud)(void *), void (*free_payload)(void *),
                                      pc_engine_job_apply_fn apply, bool *out_is_job, uint64_t *out_job_id,
                                      pc_json_value *out, pc_error *err)
{
    s->jobs.spawn = true;
    pc_status rc =
        pc_engine_run_job(s, label, lock_document, work, ud, free_ud, free_payload, apply, out, err);
    s->jobs.spawn = false;
    pc_engine_running *pending = s->jobs.pending;
    s->jobs.pending = NULL;
    *out_is_job = false;
    if (rc != PC_OK) {
        if (pending != NULL) {
            pc_engine_job_ctx_cancel(&pending->ctx);
            if (pending->has_thread) {
                (void)pthread_join(pending->thread, NULL);
            }
            running_free(pending);
        }
        return rc;
    }
    if (pending != NULL) {
        free(pending->command);
        pending->command = pc_strndup(command, strlen(command));
        pc_json_value_fini(&pending->params);
        if (params != NULL) {
            (void)pc_json_value_from_val(params, &pending->params, NULL);
        }
        pending->journal = false;
        uint64_t jid = pending->id;
        pc_engine_running **gr = pc_grow_array(s->jobs.running, &s->jobs.cap_running, s->jobs.nrunning + 1,
                                               sizeof *s->jobs.running);
        if (gr == NULL) {
            pc_engine_job_ctx_cancel(&pending->ctx);
            if (pending->has_thread) {
                (void)pthread_join(pending->thread, NULL);
            }
            running_free(pending);
            return pc_fail(err, PC_ERR_NOMEM, "job table");
        }
        s->jobs.running = gr;
        s->jobs.running[s->jobs.nrunning++] = pending;
        *out_is_job = true;
        *out_job_id = jid;
    }
    return PC_OK;
}

bool pc_engine_job_record_command(pc_engine_session *s, uint64_t id)
{
    pc_engine_running *job = running_by_id(&s->jobs, id, NULL);
    const pc_engine_command *command = job && job->command ? pc_engine_command_find(job->command) : NULL;
    if (!job || !command)
        return false;
    job->journal = command->journal;
    return true;
}

/* One JobInfo row as JSON into arr (running job r, or a recent row). */
static void info_running_json(yyjson_mut_doc *d, yyjson_mut_val *arr, const pc_engine_running *r)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, o, "id", r->id);
    yyjson_mut_obj_add_strcpy(d, o, "command", r->command != NULL ? r->command : "");
    yyjson_mut_obj_add_strcpy(d, o, "label", r->label != NULL ? r->label : "");
    if (r->has_doc) {
        yyjson_mut_obj_add_uint(d, o, "document", r->doc);
    } else {
        yyjson_mut_obj_add_null(d, o, "document");
    }
    yyjson_mut_obj_add_strcpy(d, o, "state", "running");
    yyjson_mut_obj_add_real(d, o, "progress", (double)pc_engine_job_ctx_fraction(&r->ctx));
    char msg[256];
    ctx_message(&r->ctx, msg);
    yyjson_mut_obj_add_strcpy(d, o, "message", msg);
    yyjson_mut_obj_add_real(d, o, "elapsedMs", now_ms() - r->started_ms);
    yyjson_mut_arr_append(arr, o);
}

static void info_recent_json(yyjson_mut_doc *d, yyjson_mut_val *arr, const pc_engine_job_info_row *row)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, o, "id", row->id);
    yyjson_mut_obj_add_strcpy(d, o, "command", row->command != NULL ? row->command : "");
    yyjson_mut_obj_add_strcpy(d, o, "label", row->label != NULL ? row->label : "");
    if (row->has_doc) {
        yyjson_mut_obj_add_uint(d, o, "document", row->doc);
    } else {
        yyjson_mut_obj_add_null(d, o, "document");
    }
    yyjson_mut_obj_add_strcpy(d, o, "state", row->state);
    yyjson_mut_obj_add_real(d, o, "progress", (double)row->progress);
    yyjson_mut_obj_add_strcpy(d, o, "message", row->message);
    yyjson_mut_obj_add_real(d, o, "elapsedMs", row->elapsed_ms);
    if (row->has_result) {
        yyjson_mut_val *copy = yyjson_mut_val_mut_copy(d, pc_json_value_root(&row->result));
        if (copy != NULL) {
            yyjson_mut_obj_add_val(d, o, "result", copy);
        }
    }
    if (row->error != NULL) {
        yyjson_mut_obj_add_strcpy(d, o, "error", row->error);
    }
    yyjson_mut_arr_append(arr, o);
}

static pc_status run_jobs_list(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (size_t i = 0; i < s->jobs.nrunning; i++) {
        info_running_json(d, arr, s->jobs.running[i]);
    }
    for (size_t i = 0; i < s->jobs.nrecent; i++) {
        info_recent_json(d, arr, &s->jobs.recent[i]);
    }
    yyjson_mut_obj_add_val(d, root, "jobs", arr);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status run_jobs_cancel(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, "job");
    if (v == NULL || yyjson_is_null(v)) {
        size_t n = s->jobs.nrunning;
        pc_engine_session_cancel_all_jobs(s);
        size_t cancelled = n > s->jobs.nrunning ? n - s->jobs.nrunning : 0;
        yyjson_mut_doc *d = NULL;
        if (pc_engine_result_doc(out, &d, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(d);
        yyjson_mut_obj_add_uint(d, root, "cancelled", cancelled);
        yyjson_mut_doc_set_root(d, root);
        return PC_OK;
    }
    uint64_t id;
    if (!pc_json_as_u64(v, &id)) {
        return pc_engine_err_bad_params(err, "jobs.cancel", "`job` must be a job id");
    }
    if (pc_engine_session_cancel_job(s, id)) {
        yyjson_mut_doc *d = NULL;
        if (pc_engine_result_doc(out, &d, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(d);
        yyjson_mut_obj_add_uint(d, root, "cancelled", 1);
        yyjson_mut_doc_set_root(d, root);
        return PC_OK;
    }
    if (running_by_id(&s->jobs, id, NULL) != NULL) {
        return pc_engine_err_other(err, "printing cannot be cancelled here; use the system print queue");
    }
    return pc_engine_err_other(err, "no running job %llu", (unsigned long long)id);
}

static const pc_engine_command JOBS[] = {
    {"jobs.list", "List Background Jobs", NULL, 0, NULL, "{}", pc_engine_always, run_jobs_list, false},
    {"jobs.cancel", "Cancel Background Job", NULL, 0, NULL, "{\"job\":u64? (default: every running job)}",
     pc_engine_always, run_jobs_cancel, false},
};

const pc_engine_command *pc_engine_jobs_specs(size_t *n)
{
    *n = sizeof JOBS / sizeof JOBS[0];
    return JOBS;
}
