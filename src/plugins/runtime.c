/* The plug-in host: loading, instantiating and calling a plug-in under resource limits (port of
 * crates/plugins/src/runtime.rs, on WAMR instead of wasmi; REWRITE_PLAN.md D13).
 *
 * How the reference's sandbox maps onto WAMR:
 *   - fuel (instructions per call, in 20M slices with a deadline check between slices) -> WAMR's
 *     instruction metering (a per-call count, capped at INT_MAX by WAMR's int counter);
 *   - the wall-clock deadline and sibling-band aborts -> a watchdog thread per run calling
 *     wasm_runtime_terminate (the thread manager's suspend flags stop even a tight loop). Like
 *     the reference, only calls with more than one fuel slice are interrupted: a shorter call
 *     always ends with its own result;
 *   - StoreLimits (memory cap, 4 tables of at most 100,000 elements) -> checked from the module's
 *     declarations before instantiation, plus WAMR's max_memory_pages for growth;
 *   - EnforcedLimits::strict -> scan.c; max_recursion_depth -> the interpreter stack size;
 *   - no imports -> refused after validation, as the reference does; WAMR never sees a start
 *     function or the exports it would run on its own (scan.c rewrites them).
 * Trap messages are translated to wasmi's wording for the traps both runtimes share. */
#include "plugins/internal.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "base/mem.h"
#include "base/num.h"
#include "plugins/scan.h"

#ifdef PC_HAVE_WAMR
#include <wasm_export.h>
#endif

/* Fuel handed out per slice in the reference; a call needing more can be interrupted. */
#define FUEL_SLICE ((uint64_t)20000000)
/* Interpreter stack per unit of max_recursion_depth (a small function's frame plus operands). */
#define STACK_BYTES_PER_LEVEL 512u
/* wasmi StoreLimits as the reference configures them. */
#define MAX_TABLES 4u
#define MAX_TABLE_ELEMENTS 100000u
#define PAGE_BYTES ((uint64_t)65536)

pc_plugin_limits pc_plugin_limits_default(void)
{
    pc_plugin_limits l;
    l.max_module_bytes = (size_t)32 << 20;
    l.max_memory_bytes = (size_t)512 << 20;
    l.band_bytes = (size_t)4 << 20;
    l.fuel_base = 50000000;
    l.fuel_per_sample = 4000;
    l.max_recursion_depth = 1024;
    l.wall_time_ms = 60000;
    return l;
}

bool pc_plugins_available(void)
{
#ifdef PC_HAVE_WAMR
    return true;
#else
    return false;
#endif
}

struct pc_plugin {
    atomic_size_t refs;
    pc_plugin_manifest manifest;
    pc_plugin_limits limits;
    size_t size;
    char *source;
#ifdef PC_HAVE_WAMR
    uint8_t *code; /* the (rewritten) module bytes; WAMR may write to them and keeps pointers */
    size_t code_len;
    wasm_module_t module;
    bool has_start;
    bool exports_memory; /* an export named "memory" of kind memory */
    /* Memory and table declarations, for the store limits at each instantiation. */
    uint32_t memory_count, table_count;
    uint64_t memory_min_pages, table_max_min;
#endif
};

pc_plugin *pc_plugin_retain(pc_plugin *p)
{
    if (p != NULL) {
        atomic_fetch_add_explicit(&p->refs, 1, memory_order_relaxed);
    }
    return p;
}

void pc_plugin_release(pc_plugin *p)
{
    if (p == NULL || atomic_fetch_sub_explicit(&p->refs, 1, memory_order_acq_rel) != 1) {
        return;
    }
#ifdef PC_HAVE_WAMR
    if (p->module != NULL) {
        wasm_runtime_unload(p->module);
    }
    free(p->code);
#endif
    pc_plugin_manifest_fini(&p->manifest);
    free(p->source);
    free(p);
}

const pc_plugin_manifest *pc_plugin_get_manifest(const pc_plugin *p)
{
    return &p->manifest;
}
const char *pc_plugin_id(const pc_plugin *p)
{
    return pc_str_c(&p->manifest.id);
}
const pc_plugin_limits *pc_plugin_get_limits(const pc_plugin *p)
{
    return &p->limits;
}
size_t pc_plugin_size(const pc_plugin *p)
{
    return p->size;
}
const char *pc_plugin_source(const pc_plugin *p)
{
    return p->source;
}

pc_status pc_plugin_set_source(pc_plugin *p, const char *source, pc_error *err)
{
    char *s = pc_strndup(source, strlen(source));
    if (s == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    free(p->source);
    p->source = s;
    return PC_OK;
}

/* ---- deadline: watchdog, abort and the running instances ---- */

typedef struct live_slot {
    void *inst;         /* wasm_module_inst_t */
    bool interruptible; /* the current call needs more than one fuel slice */
} live_slot;

struct pc_plugin_deadline {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint64_t at_ns; /* CLOCK_MONOTONIC */
    bool has_error, done, has_watchdog;
    pc_error error;
    live_slot *live;
    size_t n_live, cap_live;
    pthread_t watchdog;
};

static uint64_t mono_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* Stops every registered call that may be interrupted. Caller holds mu. */
static void terminate_all_locked(pc_plugin_deadline *d)
{
#ifdef PC_HAVE_WAMR
    for (size_t i = 0; i < d->n_live; i++) {
        if (d->live[i].interruptible) {
            wasm_runtime_terminate((wasm_module_inst_t)d->live[i].inst);
        }
    }
#else
    (void)d;
#endif
}

static void set_error_locked(pc_plugin_deadline *d, const pc_error *e)
{
    if (!d->has_error) {
        d->has_error = true;
        d->error = *e;
    }
    terminate_all_locked(d);
}

#ifdef PC_HAVE_WAMR
static void time_budget_locked(pc_plugin_deadline *d)
{
    pc_error e;
    pc_error_clear(&e);
    (void)PC_PLUGIN_FAIL_LIMIT(&e, "time budget");
    set_error_locked(d, &e);
}

/* The watchdog: at the deadline, publishes the time budget and stops the running calls. */
static void *watchdog_main(void *arg)
{
    pc_plugin_deadline *d = arg;
    (void)pthread_mutex_lock(&d->mu);
    while (!d->done) {
        uint64_t now = mono_ns();
        if (now >= d->at_ns) {
            if (!d->has_error) {
                time_budget_locked(d);
            } else {
                terminate_all_locked(d);
            }
            /* Calls registered later find the error and do not start; keep stopping any that
             * became interruptible since. */
            {
                struct timespec ts;
                (void)clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 20000000;
                if (ts.tv_nsec >= 1000000000) {
                    ts.tv_sec++;
                    ts.tv_nsec -= 1000000000;
                }
                (void)pthread_cond_timedwait(&d->cv, &d->mu, &ts);
            }
            continue;
        }
        {
            /* Wait on the realtime clock (portable), at most a second at a time, so a clock
             * step cannot stretch the monotonic deadline much. */
            uint64_t wait = d->at_ns - now;
            struct timespec ts;
            if (wait > 1000000000u) {
                wait = 1000000000u;
            }
            (void)clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += (time_t)(wait / 1000000000u);
            ts.tv_nsec += (long)(wait % 1000000000u);
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            (void)pthread_cond_timedwait(&d->cv, &d->mu, &ts);
        }
    }
    (void)pthread_mutex_unlock(&d->mu);
    return NULL;
}
#endif

pc_status pc_plugin_deadline_new(const pc_plugin_limits *limits, pc_plugin_deadline **out, pc_error *err)
{
    pc_plugin_deadline *d = calloc(1, sizeof *d);
    uint64_t budget;
    if (d == NULL) {
        return PC_PLUGIN_FAIL_NOMEM(err);
    }
    if (pthread_mutex_init(&d->mu, NULL) != 0) {
        free(d);
        return PC_PLUGIN_FAIL_NOMEM(err);
    }
    if (pthread_cond_init(&d->cv, NULL) != 0) {
        (void)pthread_mutex_destroy(&d->mu);
        free(d);
        return PC_PLUGIN_FAIL_NOMEM(err);
    }
    budget = limits->wall_time_ms > UINT64_MAX / 1000000u ? UINT64_MAX : limits->wall_time_ms * 1000000u;
    d->at_ns = mono_ns();
    d->at_ns = budget > UINT64_MAX - d->at_ns ? UINT64_MAX : d->at_ns + budget;
#ifdef PC_HAVE_WAMR
    if (pthread_create(&d->watchdog, NULL, watchdog_main, d) != 0) {
        (void)pthread_cond_destroy(&d->cv);
        (void)pthread_mutex_destroy(&d->mu);
        free(d);
        return PC_PLUGIN_FAIL_FAILED(err, "could not start the plug-in watchdog thread");
    }
    d->has_watchdog = true;
#endif
    *out = d;
    return PC_OK;
}

void pc_plugin_deadline_free(pc_plugin_deadline *d)
{
    if (d == NULL) {
        return;
    }
    if (d->has_watchdog) {
        (void)pthread_mutex_lock(&d->mu);
        d->done = true;
        (void)pthread_cond_signal(&d->cv);
        (void)pthread_mutex_unlock(&d->mu);
        (void)pthread_join(d->watchdog, NULL);
    }
    free(d->live);
    (void)pthread_cond_destroy(&d->cv);
    (void)pthread_mutex_destroy(&d->mu);
    free(d);
}

void pc_plugin_deadline_abort(pc_plugin_deadline *d, const pc_error *e)
{
    (void)pthread_mutex_lock(&d->mu);
    set_error_locked(d, e);
    (void)pthread_mutex_unlock(&d->mu);
}

#ifdef PC_HAVE_WAMR

/* ---- WAMR ---- */

/* Deadline::check: the published error, or the time budget once the deadline has passed. */
static pc_status deadline_check_locked(pc_plugin_deadline *d, pc_error *err)
{
    if (!d->has_error && mono_ns() >= d->at_ns) {
        time_budget_locked(d);
    }
    if (d->has_error) {
        if (err != NULL) {
            *err = d->error;
        }
        return d->error.code != PC_OK ? d->error.code : PC_ERR_CANCELLED;
    }
    return PC_OK;
}

static pc_status deadline_check(pc_plugin_deadline *d, pc_error *err)
{
    pc_status st;
    (void)pthread_mutex_lock(&d->mu);
    st = deadline_check_locked(d, err);
    (void)pthread_mutex_unlock(&d->mu);
    return st;
}

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static bool g_runtime_ok;

static void runtime_init(void)
{
    RuntimeInitArgs a;
    memset(&a, 0, sizeof a);
    a.mem_alloc_type = Alloc_With_System_Allocator;
    a.running_mode = Mode_Interp;
    wasm_runtime_set_log_level(WASM_LOG_LEVEL_FATAL);
    g_runtime_ok = wasm_runtime_full_init(&a);
    wasm_runtime_set_log_level(WASM_LOG_LEVEL_FATAL);
}

static pc_status runtime_ready(pc_error *err)
{
    if (pthread_once(&g_once, runtime_init) != 0 || !g_runtime_ok) {
        return PC_PLUGIN_FAIL_FAILED(err, "the WebAssembly runtime could not be started");
    }
    return PC_OK;
}

/* One live instance: its own memory and interpreter stack, registered with the deadline. */
typedef struct live {
    wasm_module_inst_t inst;
    wasm_exec_env_t env;
    wasm_memory_inst_t memory;
    pc_plugin_deadline *d;
    bool registered;
} live;

static void live_unregister(live *l)
{
    if (!l->registered) {
        return;
    }
    (void)pthread_mutex_lock(&l->d->mu);
    for (size_t i = 0; i < l->d->n_live; i++) {
        if (l->d->live[i].inst == (void *)l->inst) {
            l->d->live[i] = l->d->live[--l->d->n_live];
            break;
        }
    }
    (void)pthread_mutex_unlock(&l->d->mu);
    l->registered = false;
}

static void live_fini(live *l)
{
    live_unregister(l);
    if (l->env != NULL) {
        wasm_runtime_destroy_exec_env(l->env);
    }
    if (l->inst != NULL) {
        wasm_runtime_deinstantiate(l->inst);
    }
    memset(l, 0, sizeof *l);
}

/* Marks the instance's next call interruptible or not (under the deadline lock, so a terminate
 * never races the flag). Fails with the run's error when it has already ended. */
static pc_status live_arm(live *l, bool interruptible, pc_error *err)
{
    pc_status st;
    (void)pthread_mutex_lock(&l->d->mu);
    st = deadline_check_locked(l->d, err);
    if (st == PC_OK) {
        for (size_t i = 0; i < l->d->n_live; i++) {
            if (l->d->live[i].inst == (void *)l->inst) {
                l->d->live[i].interruptible = interruptible;
            }
        }
    }
    (void)pthread_mutex_unlock(&l->d->mu);
    return st;
}

static const char *strip_exception(const char *e)
{
    if (e == NULL) {
        return "";
    }
    return strncmp(e, "Exception: ", 11) == 0 ? e + 11 : e;
}

/* WAMR's exception as the reference's Error (wasm_err): wasmi's TrapCode wording where both
 * runtimes have the trap; the instruction budget; or the run's published error after a
 * terminate. */
static pc_status trap_error(live *l, const char *raw, pc_error *err)
{
    static const char *const SAME[] = {"out of bounds memory access", "integer divide by zero",
                                       "integer overflow", "invalid conversion to integer",
                                       "indirect call type mismatch"};
    const char *e = strip_exception(raw);
    if (strcmp(e, "instruction limit exceeded") == 0) {
        return PC_PLUGIN_FAIL_LIMIT(err, "instruction budget");
    }
    if (strcmp(e, "terminated by user") == 0) {
        pc_status st = deadline_check(l->d, err);
        if (st != PC_OK) {
            return st;
        }
        return PC_PLUGIN_FAIL_TRAP(err, "terminated");
    }
    if (strcmp(e, "unreachable") == 0) {
        return PC_PLUGIN_FAIL_TRAP(err, "wasm `unreachable` instruction executed");
    }
    for (size_t i = 0; i < sizeof SAME / sizeof SAME[0]; i++) {
        if (strcmp(e, SAME[i]) == 0) {
            return PC_PLUGIN_FAIL_TRAP(err, "%s", e);
        }
    }
    if (strcmp(e, "undefined element") == 0 || strcmp(e, "out of bounds table access") == 0) {
        return PC_PLUGIN_FAIL_TRAP(err, "undefined element: out of bounds table access");
    }
    if (strcmp(e, "uninitialized element") == 0) {
        return PC_PLUGIN_FAIL_TRAP(err, "uninitialized element 2");
    }
    if (strcmp(e, "wasm operand stack overflow") == 0 || strcmp(e, "native stack overflow") == 0 ||
        strcmp(e, "wasm auxiliary stack overflow") == 0) {
        return PC_PLUGIN_FAIL_TRAP(err, "call stack exhausted");
    }
    if (strcmp(e, "allocate memory failed") == 0) {
        return PC_PLUGIN_FAIL_NOMEM(err);
    }
    return PC_PLUGIN_FAIL_TRAP(err, "%s", e[0] != '\0' ? e : "unknown trap");
}

/* Calls f with at most `fuel` instructions. argv holds the i32/i64 cells in and the results out. */
static pc_status call(live *l, wasm_function_inst_t f, uint32_t argc, uint32_t *argv, uint64_t fuel,
                      pc_error *err)
{
    pc_status st = live_arm(l, fuel > FUEL_SLICE, err);
    if (st != PC_OK) {
        return st;
    }
    wasm_runtime_set_instruction_count_limit(l->env, fuel > (uint64_t)INT_MAX ? INT_MAX : (int)fuel);
    if (!wasm_runtime_call_wasm(l->env, f, argc, argv)) {
        return trap_error(l, wasm_runtime_get_exception(l->inst), err);
    }
    return PC_OK;
}

/* Instance::get_typed_func: the export must be a function of exactly this signature. */
static pc_status typed_func(live *l, const char *name, const wasm_valkind_t *params, uint32_t np,
                            const wasm_valkind_t *results, uint32_t nr, wasm_function_inst_t *out,
                            pc_error *err)
{
    wasm_function_inst_t f = wasm_runtime_lookup_function(l->inst, name);
    wasm_valkind_t kinds[16];
    uint32_t n;
    if (f == NULL) {
        return PC_PLUGIN_FAIL_ABI(err, "export `%s`: could not find exported function", name);
    }
    n = wasm_func_get_param_count(f, l->inst);
    if (n != np) {
        return PC_PLUGIN_FAIL_ABI(err, "export `%s`: encountered an incorrect number of parameters", name);
    }
    if (n > 0) {
        wasm_func_get_param_types(f, l->inst, kinds);
        if (memcmp(kinds, params, n * sizeof kinds[0]) != 0) {
            return PC_PLUGIN_FAIL_ABI(err, "export `%s`: encountered incorrect function parameter type",
                                      name);
        }
    }
    n = wasm_func_get_result_count(f, l->inst);
    if (n != nr) {
        return PC_PLUGIN_FAIL_ABI(err, "export `%s`: encountered an incorrect number of results", name);
    }
    if (n > 0) {
        wasm_func_get_result_types(f, l->inst, kinds);
        if (memcmp(kinds, results, n * sizeof kinds[0]) != 0) {
            return PC_PLUGIN_FAIL_ABI(err, "export `%s`: encountered incorrect function result type", name);
        }
    }
    *out = f;
    return PC_OK;
}

static const wasm_valkind_t I32x8[8] = {WASM_I32, WASM_I32, WASM_I32, WASM_I32,
                                        WASM_I32, WASM_I32, WASM_I32, WASM_I32};
static const wasm_valkind_t I64x1[1] = {WASM_I64};

static pc_status instantiate(const pc_plugin *p, pc_plugin_deadline *d, live *out, pc_error *err)
{
    live l;
    struct InstantiationArgs2 *args = NULL;
    char ebuf[128];
    uint64_t pages = p->limits.max_memory_bytes / PAGE_BYTES;
    uint32_t stack;
    size_t depth = p->limits.max_recursion_depth < 16 ? 16 : p->limits.max_recursion_depth;
    pc_status st = deadline_check(d, err);
    if (st != PC_OK) {
        return st;
    }
    if (p->table_count > MAX_TABLES) {
        return PC_PLUGIN_FAIL_TRAP(err, "tried to instantiate too many tables");
    }
    if (p->table_max_min > MAX_TABLE_ELEMENTS) {
        return PC_PLUGIN_FAIL_TRAP(err,
                                   "failed to instantiate table: a resource limiter denied to allocate or "
                                   "grow the table");
    }
    if (p->memory_count > 0 && (p->memory_min_pages > UINT64_MAX / PAGE_BYTES ||
                                p->memory_min_pages * PAGE_BYTES > (uint64_t)p->limits.max_memory_bytes)) {
        return PC_PLUGIN_FAIL_TRAP(err,
                                   "failed to instantiate memory: a resource limiter denied to allocate or "
                                   "grow the linear memory");
    }
    stack = depth > UINT32_MAX / STACK_BYTES_PER_LEVEL ? UINT32_MAX : (uint32_t)depth * STACK_BYTES_PER_LEVEL;
    if (pages > 65536) {
        pages = 65536;
    }
    if (pages == 0) {
        pages = 1; /* WAMR reads 0 as "no cap"; a module with memory had more than the cap anyway */
    }
    if (!wasm_runtime_instantiation_args_create(&args)) {
        return PC_PLUGIN_FAIL_NOMEM(err);
    }
    wasm_runtime_instantiation_args_set_default_stack_size(args, stack);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(args, 0);
    wasm_runtime_instantiation_args_set_max_memory_pages(args, (uint32_t)pages);
    memset(&l, 0, sizeof l);
    l.d = d;
    ebuf[0] = '\0';
    l.inst = wasm_runtime_instantiate_ex2(p->module, args, ebuf, sizeof ebuf);
    wasm_runtime_instantiation_args_destroy(args);
    if (l.inst == NULL) {
        if (strstr(ebuf, "allocate memory failed") != NULL) {
            return PC_PLUGIN_FAIL_NOMEM(err);
        }
        /* WAMR prefixes the reason; wasmi reports it alone. */
        return PC_PLUGIN_FAIL_TRAP(err, "%s",
                                   strncmp(ebuf, "WASM module instantiate failed: ", 32) == 0 ? ebuf + 32
                                   : ebuf[0] != '\0' ? ebuf
                                                     : "instantiation failed");
    }
    l.env = wasm_runtime_create_exec_env(l.inst, stack);
    if (l.env == NULL) {
        live_fini(&l);
        return PC_PLUGIN_FAIL_NOMEM(err);
    }
    /* Register so an abort or the deadline can stop this instance's calls. */
    (void)pthread_mutex_lock(&d->mu);
    st = deadline_check_locked(d, err);
    if (st == PC_OK) {
        live_slot *grown = pc_grow_array(d->live, &d->cap_live, d->n_live + 1, sizeof *grown);
        if (grown == NULL) {
            st = PC_PLUGIN_FAIL_NOMEM(err);
        } else {
            d->live = grown;
            d->live[d->n_live++] = (live_slot){l.inst, false};
            l.registered = true;
        }
    }
    (void)pthread_mutex_unlock(&d->mu);
    if (st != PC_OK) {
        live_fini(&l);
        return st;
    }
    if (p->has_start) {
        /* The start function runs on the call budget, as in instantiate_and_start. */
        wasm_function_inst_t f = wasm_runtime_lookup_function(l.inst, PC_WASM_START_EXPORT);
        uint32_t none[1] = {0};
        if (f == NULL) {
            live_fini(&l);
            return PC_PLUGIN_FAIL_MODULE(err, "start function not found");
        }
        st = call(&l, f, 0, none, p->limits.fuel_base, err);
        if (st != PC_OK) {
            live_fini(&l);
            return st;
        }
    }
    /* (WAMR's lookup by name returns memory 0 whatever its export name, hence the flag.) */
    l.memory = p->exports_memory ? wasm_runtime_get_default_memory(l.inst) : NULL;
    if (l.memory == NULL) {
        live_fini(&l);
        return PC_PLUGIN_FAIL_ABI(err, "the module must export its linear memory as `memory`");
    }
    *out = l;
    return PC_OK;
}

/* The instance's linear memory right now (it may have grown or moved). */
static uint8_t *memory_bytes(const live *l, size_t *len)
{
    uint64_t size =
        wasm_memory_get_cur_page_count(l->memory) * (uint64_t)wasm_memory_get_bytes_per_page(l->memory);
    *len = size > SIZE_MAX ? SIZE_MAX : (size_t)size;
    return wasm_memory_get_base_address(l->memory);
}

/* Whether the module exports its memory as "memory". */
static bool exports_memory(wasm_module_t m)
{
    int32_t n = wasm_runtime_get_export_count(m);
    for (int32_t i = 0; i < n; i++) {
        wasm_export_t e;
        wasm_runtime_get_export_type(m, i, &e);
        if (e.kind == WASM_IMPORT_EXPORT_KIND_MEMORY && e.name != NULL && strcmp(e.name, "memory") == 0) {
            return true;
        }
    }
    return false;
}

/* The start function, if any, must have type [] -> [] (wasmi validates this). */
static pc_status check_start_type(wasm_module_t m, pc_error *err)
{
    int32_t n = wasm_runtime_get_export_count(m);
    for (int32_t i = 0; i < n; i++) {
        wasm_export_t e;
        wasm_runtime_get_export_type(m, i, &e);
        if (e.kind == WASM_IMPORT_EXPORT_KIND_FUNC && e.name != NULL &&
            strcmp(e.name, PC_WASM_START_EXPORT) == 0) {
            if (wasm_func_type_get_param_count(e.u.func_type) != 0 ||
                wasm_func_type_get_result_count(e.u.func_type) != 0) {
                return PC_PLUGIN_FAIL_MODULE(err, "start function must have type [] -> []");
            }
            return PC_OK;
        }
    }
    return PC_PLUGIN_FAIL_MODULE(err, "start function not found");
}

static pc_status load_wamr(pc_plugin *p, const uint8_t *bytes, size_t len, pc_error *err)
{
    pc_wasm_scan s;
    char ebuf[160];
    pc_status st;
    pc_wasm_scan_module(bytes, len, &s);
    if (s.limit_msg[0] != '\0') {
        return PC_PLUGIN_FAIL_MODULE(err, "%s", s.limit_msg);
    }
    st = pc_wasm_rewrite(bytes, len, &s, &p->code, &p->code_len, err);
    if (st == PC_ERR_NOMEM) {
        return st;
    }
    if (st != PC_OK) {
        return PC_PLUGIN_FAIL_MODULE(err, "%s", err != NULL ? err->msg : "malformed module");
    }
    if (p->code_len > UINT32_MAX) {
        return PC_PLUGIN_FAIL_MODULE(err, "module too large");
    }
    ebuf[0] = '\0';
    p->module = wasm_runtime_load(p->code, (uint32_t)p->code_len, ebuf, sizeof ebuf);
    if (p->module == NULL) {
        if (strstr(ebuf, "allocate memory failed") != NULL) {
            return PC_PLUGIN_FAIL_NOMEM(err);
        }
        return PC_PLUGIN_FAIL_MODULE(err, "%s",
                                     strncmp(ebuf, "WASM module load failed: ", 25) == 0 ? ebuf + 25
                                     : ebuf[0] != '\0'                                   ? ebuf
                                                                                         : "invalid module");
    }
    if (!s.walked) {
        return PC_PLUGIN_FAIL_MODULE(err, "malformed module");
    }
    if (s.has_start) {
        st = check_start_type(p->module, err);
        if (st != PC_OK) {
            return st;
        }
    }
    if (s.has_import) {
        return PC_PLUGIN_FAIL_ABI(
            err,
            "the module imports `%.*s.%.*s`, but plug-ins get no host imports (no WASI, files or network)",
            (int)(s.imp_module_len > 100 ? 100 : s.imp_module_len), (const char *)bytes + s.imp_module_off,
            (int)(s.imp_name_len > 100 ? 100 : s.imp_name_len), (const char *)bytes + s.imp_name_off);
    }
    p->has_start = s.has_start;
    p->exports_memory = exports_memory(p->module);
    p->memory_count = s.memory_count;
    p->memory_min_pages = s.memory_min_pages;
    p->table_count = s.table_count;
    p->table_max_min = s.table_max_min;
    return PC_OK;
}

#endif /* PC_HAVE_WAMR */

pc_status pc_plugin_load(const uint8_t *bytes, size_t len, const pc_plugin_limits *limits, pc_plugin **out,
                         pc_error *err)
{
    pc_plugin_limits lim = limits != NULL ? *limits : pc_plugin_limits_default();
    if (!pc_plugins_available()) {
        return pc_fail(err, PC_ERR_UNSUPPORTED, "plug-ins unavailable in this build");
    }
    if (len > lim.max_module_bytes) {
        return PC_PLUGIN_FAIL_MODULE(err, "module is %zu bytes (limit %zu)", len, lim.max_module_bytes);
    }
    if (bytes == NULL || len < 4 || memcmp(bytes, "\0asm", 4) != 0) {
        return PC_PLUGIN_FAIL_MODULE(err, "not a WebAssembly binary (missing \\0asm header)");
    }
#ifdef PC_HAVE_WAMR
    {
        pc_plugin *p;
        pc_plugin_deadline *d = NULL;
        live l;
        wasm_function_inst_t f;
        uint32_t argv[2] = {0, 0};
        uint64_t packed;
        size_t ptr, mlen, memlen;
        const uint8_t *mem;
        pc_plugin_manifest man;
        pc_status st = runtime_ready(err);
        if (st != PC_OK) {
            return st;
        }
        p = calloc(1, sizeof *p);
        if (p == NULL) {
            return PC_PLUGIN_FAIL_NOMEM(err);
        }
        atomic_init(&p->refs, 1);
        p->limits = lim;
        p->size = len;
        st = load_wamr(p, bytes, len, err);
        if (st == PC_OK) {
            st = pc_plugin_deadline_new(&p->limits, &d, err);
        }
        if (st != PC_OK) {
            pc_plugin_release(p);
            return st;
        }
        memset(&l, 0, sizeof l);
        st = instantiate(p, d, &l, err);
        if (st == PC_OK) {
            st = typed_func(&l, "pc_abi_version", NULL, 0, I32x8, 1, &f, err);
        }
        if (st == PC_OK) {
            st = call(&l, f, 0, argv, p->limits.fuel_base, err);
        }
        if (st == PC_OK && (int32_t)argv[0] != PC_PLUGIN_ABI_VERSION) {
            st = PC_PLUGIN_FAIL_ABI(err, "pc_abi_version returned %d; this host implements version %d",
                                    (int)(int32_t)argv[0], PC_PLUGIN_ABI_VERSION);
        }
        if (st == PC_OK) {
            st = typed_func(&l, "pc_manifest", NULL, 0, I64x1, 1, &f, err);
        }
        if (st == PC_OK) {
            argv[0] = argv[1] = 0;
            st = call(&l, f, 0, argv, p->limits.fuel_base, err);
        }
        if (st == PC_OK) {
            memcpy(&packed, argv, sizeof packed);
            ptr = (size_t)(packed & 0xffffffffu);
            mlen = (size_t)(packed >> 32);
            if (mlen > PC_PLUGIN_MAX_MANIFEST_BYTES) {
                st = PC_PLUGIN_FAIL_MANIFEST(err, "manifest is %zu bytes (limit %zu)", mlen,
                                             PC_PLUGIN_MAX_MANIFEST_BYTES);
            } else {
                mem = memory_bytes(&l, &memlen);
                if (mem == NULL || ptr > memlen || mlen > memlen - ptr) {
                    st = PC_PLUGIN_FAIL_ABI(err, "pc_manifest points outside memory");
                } else {
                    st = pc_plugin_manifest_parse(mem + ptr, mlen, &man, err);
                    if (st == PC_OK) {
                        p->manifest = man;
                    }
                }
            }
        }
        if (st == PC_OK) {
            st = typed_func(&l, "pc_alloc", I32x8, 1, I32x8, 1, &f, err);
        }
        if (st == PC_OK) {
            st = typed_func(&l, "pc_filter", I32x8, 8, I32x8, 1, &f, err);
        }
        live_fini(&l);
        pc_plugin_deadline_free(d);
        if (st != PC_OK) {
            pc_plugin_release(p);
            return st;
        }
        *out = p;
        return PC_OK;
    }
#else
    (void)out;
    return pc_fail(err, PC_ERR_UNSUPPORTED, "plug-ins unavailable in this build");
#endif
}

pc_status pc_plugin_filter_buffer(const pc_plugin *p, float *data, size_t n, uint32_t w, uint32_t h,
                                  uint32_t channels, uint32_t format, const uint8_t *params,
                                  size_t params_len, pc_plugin_deadline *d, pc_error *err)
{
#ifdef PC_HAVE_WAMR
    live l;
    wasm_function_inst_t alloc_f, filter_f;
    uint32_t argv[8];
    size_t bytes, ptr = 0, pptr = 0, memlen;
    uint8_t *mem;
    uint64_t fuel;
    pc_status st;
    if (n > SIZE_MAX / 4 || n * 4 > (size_t)INT32_MAX || params_len > p->limits.max_memory_bytes ||
        n * 4 > p->limits.max_memory_bytes - params_len) {
        return PC_PLUGIN_FAIL_LIMIT(err, "memory budget (band too large)");
    }
    bytes = n * 4;
    memset(&l, 0, sizeof l);
    st = instantiate(p, d, &l, err);
    if (st != PC_OK) {
        return st;
    }
    st = typed_func(&l, "pc_alloc", I32x8, 1, I32x8, 1, &alloc_f, err);
    if (st == PC_OK) {
        st = typed_func(&l, "pc_filter", I32x8, 8, I32x8, 1, &filter_f, err);
    }
    if (st == PC_OK) {
        argv[0] = (uint32_t)bytes;
        st = call(&l, alloc_f, 1, argv, p->limits.fuel_base, err);
        ptr = argv[0];
    }
    if (st == PC_OK) {
        argv[0] = (uint32_t)(params_len > 0 ? params_len : 1);
        st = call(&l, alloc_f, 1, argv, p->limits.fuel_base, err);
        pptr = argv[0];
    }
    if (st == PC_OK && (ptr == 0 || pptr == 0)) {
        st = PC_PLUGIN_FAIL_FAILED(err, "pc_alloc returned 0 (out of memory)");
    }
    if (st == PC_OK) {
        mem = memory_bytes(&l, &memlen);
        if (mem == NULL || bytes > memlen || ptr > memlen - bytes || params_len > memlen ||
            pptr > memlen - params_len) {
            st = PC_PLUGIN_FAIL_ABI(err, "pc_alloc returned a block outside memory");
        } else {
            for (size_t i = 0; i < n; i++) {
                uint32_t u;
                memcpy(&u, &data[i], 4);
                mem[ptr + 4 * i] = (uint8_t)u;
                mem[ptr + 4 * i + 1] = (uint8_t)(u >> 8);
                mem[ptr + 4 * i + 2] = (uint8_t)(u >> 16);
                mem[ptr + 4 * i + 3] = (uint8_t)(u >> 24);
            }
            if (params_len > 0) {
                memcpy(mem + pptr, params, params_len);
            }
        }
    }
    if (st == PC_OK) {
        uint64_t per = p->limits.fuel_per_sample;
        uint64_t extra = per != 0 && (uint64_t)n > UINT64_MAX / per ? UINT64_MAX : (uint64_t)n * per;
        fuel = p->limits.fuel_base > UINT64_MAX - extra ? UINT64_MAX : p->limits.fuel_base + extra;
        argv[0] = (uint32_t)ptr;
        argv[1] = (uint32_t)bytes;
        argv[2] = w;
        argv[3] = h;
        argv[4] = channels;
        argv[5] = format;
        argv[6] = (uint32_t)pptr;
        argv[7] = (uint32_t)params_len;
        st = call(&l, filter_f, 8, argv, fuel, err);
    }
    if (st == PC_OK && argv[0] != 0) {
        st = PC_PLUGIN_FAIL_FAILED(err, "pc_filter returned error code %d", (int)(int32_t)argv[0]);
    }
    if (st == PC_OK) {
        mem = memory_bytes(&l, &memlen);
        if (mem == NULL || bytes > memlen || ptr > memlen - bytes) {
            st = PC_PLUGIN_FAIL_ABI(err, "pixel buffer no longer in memory");
        } else {
            for (size_t i = 0; i < n; i++) {
                const uint8_t *s = mem + ptr + 4 * i;
                uint32_t u =
                    (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16 | (uint32_t)s[3] << 24;
                memcpy(&data[i], &u, 4);
            }
        }
    }
    live_fini(&l);
    return st;
#else
    (void)p, (void)data, (void)n, (void)w, (void)h, (void)channels, (void)format, (void)params,
        (void)params_len;
    (void)d;
    return pc_fail(err, PC_ERR_UNSUPPORTED, "plug-ins unavailable in this build");
#endif
}
